#include "../include/types.h"
static void *receive_copy(void *, const void *, size_t);
#define memcpy receive_copy
#define NET_HOST_TEST
#define NET_HOST_TEST_IRQ
#include "../kernel/net.c"
#undef memcpy

extern int printf(const char *, ...);
extern void exit(int);
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned checks, irq_depth, ack_count;
static bool interrupts_enabled = true;
static void (*pending_preemption)(void), (*copy_preemption)(void);
static void *preempt_destination;
static uint8_t consumer[32];
static int consumed;
static uint32_t last_ack;
static uint16_t last_window;
static const uint8_t peer_ip[4] = {10, 0, 2, 2};

nic_ops_t nic_ops;
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int d) { (void)n; (void)d; }
void fb_puts(const char *s) { (void)s; }
void fb_putdec(uint64_t n) { (void)n; }
uint64_t idt_get_ticks(void) { return 100; }
bool sched_is_enabled(void) { return false; }
int sched_current_get(void) { return 1; }
void sched_unblock(int p) { (void)p; }

uint64_t net_test_irq_save(void)
{
    uint64_t old = interrupts_enabled ? 0x200 : 0;
    interrupts_enabled = false;
    irq_depth++;
    return old;
}

void net_test_irq_restore(uint64_t flags)
{
    CHECK(irq_depth > 0);
    irq_depth--;
    interrupts_enabled = (flags & 0x200) != 0;
    if (interrupts_enabled && pending_preemption) {
        void (*run)(void) = pending_preemption;
        pending_preemption = NULL;
        run();
    }
}

static void *receive_copy(void *destination, const void *source, size_t size)
{
    uint8_t *dst = destination;
    const uint8_t *src = source;
    for (size_t i = 0; i < size; i++) {
        /* Inject a task switch halfway through the real data copy. An IRQ
         * guard postpones it until publication/compaction has completed. */
        if (i == size / 2 && destination == preempt_destination && copy_preemption) {
            void (*run)(void) = copy_preemption;
            copy_preemption = NULL;
            if (interrupts_enabled) run();
            else {
                CHECK(!pending_preemption);
                pending_preemption = run;
            }
        }
        dst[i] = src[i];
    }
    return destination;
}

static int transmit(const void *packet, uint32_t length)
{
    CHECK(irq_depth == 0 && interrupts_enabled);
    CHECK(length >= 60);
    const eth_hdr_t *eth = packet;
    if (ntohs(eth->ethertype) == ETH_TYPE_ARP) return 0;
    const ipv4_hdr_t *ip = (const void *)((const uint8_t *)packet + ETH_HDR_LEN);
    const tcp_hdr_t *tcp = (const void *)((const uint8_t *)ip + sizeof(*ip));
    CHECK(ntohs(eth->ethertype) == ETH_TYPE_IP4 && ip->proto == IP_PROTO_TCP);
    CHECK(ip_checksum(ip, sizeof(*ip)) == 0);
    CHECK(tcp_checksum(ip->src, ip->dst, tcp, ntohs(ip->total_len) - sizeof(*ip)) == 0);
    last_ack = ntohl(tcp->ack);
    last_window = ntohs(tcp->window);
    ack_count++;
    return 0;
}

static tcp_conn_t *setup(void)
{
    const uint8_t ip[4] = {10, 0, 2, 15}, mac[6] = {2, 3, 4, 5, 6, 7};
    CHECK(!irq_depth && !pending_preemption && !copy_preemption);
    interrupts_enabled = true;
    memset(tcp_conns, 0, sizeof(tcp_conns));
    memset(tcp_listeners, 0, sizeof(tcp_listeners));
    memset(net_waiters, 0, sizeof(net_waiters));
    net_waiter_count = 0;
    nic_ops = (nic_ops_t){.send = transmit};
    net_init(ip);
    arp_update(peer_ip, mac);
    tcp_conn_t *c = &tcp_conns[0];
    c->claimed = 1; c->state = TCP_ESTABLISHED;
    memcpy(c->remote_ip, peer_ip, 4);
    c->local_port = 40000; c->remote_port = 443;
    c->snd_nxt = c->snd_una = 1234; c->snd_wnd = 32768;
    c->rcv_nxt = 900;
    ack_count = 0; consumed = -99;
    preempt_destination = NULL;
    memset(consumer, 0, sizeof(consumer));
    return c;
}

/* Independent, byte-wise network-order oracle, including the pseudo-header. */
static uint16_t wire_tcp_checksum(const uint8_t *src, const uint8_t *dst,
                                   const uint8_t *bytes, unsigned length)
{
    uint32_t sum = IP_PROTO_TCP + length;
    for (unsigned i = 0; i < 4; i += 2)
        sum += ((unsigned)src[i] << 8) + src[i + 1] +
               ((unsigned)dst[i] << 8) + dst[i + 1];
    for (unsigned i = 0; i < length; i += 2)
        sum += ((unsigned)bytes[i] << 8) + (i + 1 < length ? bytes[i + 1] : 0);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return htons((uint16_t)~sum);
}

static unsigned wire_packet(uint8_t *bytes, uint32_t sequence, const void *data,
                             unsigned size, uint8_t flags, unsigned options)
{
    unsigned ihl = sizeof(ipv4_hdr_t) + options;
    unsigned thl = sizeof(tcp_hdr_t) + options;
    unsigned total = ihl + thl + size;
    memset(bytes, 0, total);
    ipv4_hdr_t *ip = (void *)bytes;
    ip->ver_ihl = 0x40 | (ihl / 4);
    ip->total_len = htons(total);
    ip->proto = IP_PROTO_TCP;
    memcpy(ip->src, peer_ip, 4);
    memcpy(ip->dst, our_ip, 4);
    tcp_hdr_t *p = (void *)(bytes + ihl);
    p->src_port = htons(443); p->dst_port = htons(40000);
    p->data_off = (thl / 4) << 4; p->flags = flags; p->window = htons(32768);
    p->seq = htonl(sequence); p->ack = htonl(1234);
    if (options) {
        CHECK(options == 4);
        bytes[20] = bytes[ihl + 20] = 1; /* NOP, then EOL. */
    }
    if (size) memcpy(bytes + ihl + thl, data, size);
    p->checksum = wire_tcp_checksum(ip->src, ip->dst, (const uint8_t *)p, thl + size);
    ip->checksum = ip_checksum(ip, ihl);
    return total;
}

static void feed_at_flags(uint32_t sequence, const void *data, unsigned size,
                          uint8_t flags)
{
    uint8_t bytes[sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t) + TCP_MSS] = {0};
    CHECK(size <= TCP_MSS);
    unsigned length = wire_packet(bytes, sequence, data, size, flags, 0);
    handle_ipv4(bytes, length);
}

static void feed_at(uint32_t sequence, const void *data, unsigned size)
{ feed_at_flags(sequence, data, size, TCP_ACK); }

static void feed(const void *data, unsigned size)
{ feed_at(tcp_conns[0].rcv_nxt, data, size); }

static void consume_three(void)
{ consumed = net_tcp_recv(0, consumer, 3); }

static void append_four(void)
{ feed("GHIJ", 4); }

static void interrupt_copy(void *destination, void (*run)(void))
{
    CHECK(!pending_preemption && !copy_preemption);
    preempt_destination = destination;
    copy_preemption = run;
}

static void test_basic(void)
{
    tcp_conn_t *c = setup();
    uint8_t output[32] = {0};
    feed("abcdef", 6);
    CHECK(c->rx_len == 6 && c->rcv_nxt == 906 && last_ack == 906);
    CHECK(net_tcp_peek(0, output, 3) == 3 && memcmp(output, "abc", 3) == 0);
    CHECK(c->rx_len == 6);
    CHECK(net_tcp_recv(0, output, 2) == 2 && memcmp(output, "ab", 2) == 0);
    CHECK(net_tcp_recv(0, output, sizeof(output)) == 4 && memcmp(output, "cdef", 4) == 0);
    CHECK(net_tcp_recv(0, output, sizeof(output)) == 0);
    CHECK(net_tcp_peek(0, output, sizeof(output)) == 0);
    feed("tail", 4); c->state = TCP_CLOSE_WAIT;
    CHECK(net_tcp_peek(0, output, sizeof(output)) == 4);
    CHECK(net_tcp_recv(0, output, sizeof(output)) == 4 && memcmp(output, "tail", 4) == 0);
    CHECK(net_tcp_recv(0, output, sizeof(output)) == -1);
    CHECK(net_tcp_peek(0, output, sizeof(output)) == -1);
    CHECK(net_tcp_recv(-1, output, 1) == -1 && net_tcp_peek(TCP_MAX_CONNS, output, 1) == -1);
}

static void test_append_preempted(void)
{
    const int states[] = {TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        tcp_conn_t *c = setup();
        feed("OLD", 3);
        c->state = states[i];
        interrupt_copy(c->rx_buf + 3, consume_three);
        feed("NEW!", 4);
        CHECK(!copy_preemption && !pending_preemption);
        CHECK(consumed == 3 && memcmp(consumer, "OLD", 3) == 0);
        uint8_t output[8] = {0};
        CHECK(net_tcp_recv(0, output, sizeof(output)) == 4);
        CHECK(memcmp(output, "NEW!", 4) == 0 && c->rcv_nxt == 907);
    }
}

static void test_read_preempted(bool peek)
{
    setup();
    feed("ABCDEFGH", 8);
    uint8_t output[8] = {0};
    interrupt_copy(output, consume_three);
    int count = peek ? net_tcp_peek(0, output, 4) : net_tcp_recv(0, output, 4);
    CHECK(!copy_preemption && !pending_preemption);
    CHECK(count == 4 && memcmp(output, "ABCD", 4) == 0);
    CHECK(consumed == 3 && memcmp(consumer, peek ? "ABC" : "EFG", 3) == 0);
    CHECK(net_tcp_recv(0, output, sizeof(output)) == (peek ? 5 : 1));
    CHECK(memcmp(output, peek ? "DEFGH" : "H", peek ? 5 : 1) == 0);
}

static void test_read_append(void)
{
    setup();
    feed("ABCDEF", 6);
    uint8_t output[16] = {0};
    interrupt_copy(output, append_four);
    CHECK(net_tcp_recv(0, output, 3) == 3 && memcmp(output, "ABC", 3) == 0);
    CHECK(!copy_preemption && !pending_preemption);
    CHECK(net_tcp_recv(0, output, sizeof(output)) == 7);
    CHECK(memcmp(output, "DEFGHIJ", 7) == 0);
}

static void test_window(void)
{
    tcp_conn_t *c = setup();
    uint8_t bytes[TCP_MSS], output[2048];
    memset(bytes, 'x', sizeof(bytes));
    while (c->rx_len < TCP_RX_BUF_SIZE) {
        uint32_t size = TCP_RX_BUF_SIZE - c->rx_len;
        if (size > sizeof(bytes)) size = sizeof(bytes);
        feed(bytes, size);
    }
    CHECK(!last_window && c->rcv_nxt == 900 + TCP_RX_BUF_SIZE);
    uint32_t sequence = c->rcv_nxt;
    feed("!", 1);
    CHECK(c->rx_len == TCP_RX_BUF_SIZE && c->rcv_nxt == sequence && last_ack == sequence);
    unsigned before = ack_count;
    CHECK(net_tcp_recv(0, output, sizeof(output)) == sizeof(output));
    CHECK(ack_count == before + 1 && last_window == sizeof(output));
    for (unsigned i = 0; i < sizeof(output); i++) CHECK(output[i] == 'x');
    feed_at(sequence - 32, bytes, 32);
    CHECK(c->rx_len == TCP_RX_BUF_SIZE - sizeof(output) && c->rcv_nxt == sequence);
    setup()->rcv_nxt = 0xfffffff0u;
    feed(bytes, 32);
    CHECK(tcp_conns[0].rcv_nxt == 16 && last_ack == 16);
}

static void test_irq_state(void)
{
    tcp_conn_t *c = setup();
    feed("data", 4);
    uint8_t output[8] = {0};
    interrupts_enabled = false;
    CHECK(net_tcp_recv(0, output, 0) == 0 && c->rx_len == 4);
    CHECK(net_tcp_peek(0, output, 0) == 0 && c->rx_len == 4);
    CHECK(net_tcp_peek(0, output, 4) == 4 && !interrupts_enabled);
    CHECK(net_tcp_recv(0, output, 4) == 4 && !interrupts_enabled);
    CHECK(net_tcp_recv(0, output, 4) == 0 && !interrupts_enabled);
    c->state = TCP_CLOSED;
    CHECK(net_tcp_recv(0, output, 4) == -1 && !interrupts_enabled);
    CHECK(net_tcp_peek(0, output, 4) == -1 && !interrupts_enabled);
    interrupts_enabled = true;
}

static void test_overlap(void)
{
    for (unsigned mode = 0; mode < 3; mode++) {
        tcp_conn_t *c = setup();
        uint8_t output[16] = {0};
        CHECK(!c->sack_ok);
        feed("ABCDEF", 6);
        if (mode == 1) {
            CHECK(net_tcp_recv(0, output, 6) == 6);
            CHECK(memcmp(output, "ABCDEF", 6) == 0);
        }
        if (mode == 2) interrupt_copy(c->rx_buf + 6, consume_three);
        feed_at(903, "DEFGHI", 6);
        CHECK(c->rcv_nxt == 909 && last_ack == 909);
        if (mode == 2) {
            CHECK(consumed == 3 && memcmp(consumer, "ABC", 3) == 0);
            CHECK(!pending_preemption && !copy_preemption);
        }
        unsigned skip = mode == 1 ? 6 : mode == 2 ? 3 : 0;
        CHECK(net_tcp_recv(0, output, sizeof(output)) == (int)(9 - skip));
        CHECK(memcmp(output, &"ABCDEFGHI"[skip], 9 - skip) == 0);
        unsigned before = ack_count;
        feed_at(900, "ABCD", 4);
        CHECK(c->rx_len == 0 && last_ack == 909 && ack_count == before + 1);
    }

    tcp_conn_t *c = setup();
    c->rcv_nxt = 0xfffffffcu;
    feed("ABCD", 4);
    feed_at(0xfffffffeu, "CDEFGH", 6);
    uint8_t output[TCP_MSS];
    CHECK(c->rcv_nxt == 4 && last_ack == 4);
    CHECK(net_tcp_recv(0, output, sizeof(output)) == 8);
    CHECK(memcmp(output, "ABCDEFGH", 8) == 0);

    c = setup();
    uint8_t bytes[TCP_MSS];
    memset(bytes, 'x', sizeof(bytes));
    while (c->rx_len < TCP_RX_BUF_SIZE - 2) {
        uint32_t amount = TCP_RX_BUF_SIZE - 2 - c->rx_len;
        if (amount > sizeof(bytes)) amount = sizeof(bytes);
        feed(bytes, amount);
    }
    uint32_t sequence = c->rcv_nxt;
    feed_at(sequence - 4, "xxxxABCDEF", 10);
    CHECK(c->rx_len == TCP_RX_BUF_SIZE - 2 && last_ack == sequence);
    CHECK(net_tcp_recv(0, output, sizeof(output)) == sizeof(output));
    CHECK(memcmp(output, bytes, sizeof(output)) == 0);
    feed_at(sequence - 4, "xxxxABCDEF", 10);
    CHECK(last_ack == sequence + 6 && c->rcv_nxt == sequence + 6);
    for (uint32_t left = TCP_RX_BUF_SIZE - 2 - sizeof(output); left;) {
        uint32_t amount = left > sizeof(output) ? sizeof(output) : left;
        CHECK(net_tcp_recv(0, output, amount) == (int)amount);
        CHECK(memcmp(output, bytes, amount) == 0);
        left -= amount;
    }
    CHECK(net_tcp_recv(0, output, sizeof(output)) == 6);
    CHECK(memcmp(output, "ABCDEF", 6) == 0);

    for (unsigned tail = 0; tail <= 3; tail += 3) {
        c = setup();
        feed("ABCDEF", 6);
        feed_at_flags(903, "DEFGHI", 3 + tail, TCP_ACK | TCP_FIN);
        CHECK(c->state == TCP_CLOSE_WAIT && last_ack == 907 + tail);
        CHECK(net_tcp_recv(0, output, sizeof(output)) == (int)(6 + tail));
        CHECK(memcmp(output, "ABCDEFGHI", 6 + tail) == 0);
        CHECK(net_tcp_recv(0, output, sizeof(output)) == -1);
    }
}

static void test_checksum(void)
{
    static tcp_conn_t before;
    uint8_t packet[1600] __attribute__((aligned(8)));
    uint8_t data[128], output[128];
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 31 + 17);
    const unsigned payloads[] = {1, 2, 3, 31, 32, 127, 128};
    for (unsigned options = 0; options <= 4; options += 4) {
        for (unsigned n = 0; n < sizeof(payloads) / sizeof(payloads[0]); n++) {
            unsigned size = payloads[n];
            tcp_conn_t *c = setup();
            unsigned length = wire_packet(packet, 900, data, size, TCP_ACK, options);
            unsigned ihl = 20 + options, thl = 20 + options;
            CHECK(tcp_checksum(peer_ip, our_ip, packet + ihl, thl + size) == 0);
            memcpy(&before, c, sizeof(before));
            packet[length - 1] ^= 1;
            handle_ipv4(packet, length);
            CHECK(!memcmp(&before, c, sizeof(before)) && ack_count == 0);
            CHECK(net_tcp_recv(0, output, sizeof(output)) == 0);
            packet[length - 1] ^= 1;
            memset(packet + length, 0xa5, 16); /* Link padding is not TCP data. */
            handle_ipv4(packet, length + 16);
            CHECK(c->rcv_nxt == 900 + size && last_ack == 900 + size);
            CHECK(net_tcp_recv(0, output, sizeof(output)) == (int)size);
            CHECK(!memcmp(output, data, size));
        }
    }

    /* Reject damaged ACK/control/header/options before touching any state. */
    const unsigned states[] = {TCP_ESTABLISHED, TCP_SYN_SENT, TCP_FIN_WAIT_1,
                               TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_LAST_ACK};
    const unsigned changes[] = {4, 8, 12, 13, 14, 16, 20};
    for (unsigned s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
        for (unsigned i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
            tcp_conn_t *c = setup();
            c->state = states[s];
            c->tx_len = 16; c->tx_seq = c->snd_una; c->snd_nxt += 16;
            unsigned length = wire_packet(packet, 900, NULL, 0,
                                           TCP_ACK | TCP_FIN | TCP_RST, 4);
            packet[24 + changes[i]] ^= 1;
            memcpy(&before, c, sizeof(before));
            handle_ipv4(packet, length);
            CHECK(!memcmp(&before, c, sizeof(before)) && ack_count == 0);
        }
    }

    /* A valid IP header does not validate TCP's source/destination/length. */
    for (unsigned field = 0; field < 3; field++) {
        tcp_conn_t *c = setup();
        unsigned length = wire_packet(packet, 900, data, 7, TCP_ACK, 0);
        ipv4_hdr_t *ip = (void *)packet;
        tcp_hdr_t *tcp = (void *)(packet + 20);
        uint8_t src[4], dst[4];
        memcpy(src, ip->src, 4); memcpy(dst, ip->dst, 4);
        if (field == 0) src[3] ^= 1;
        if (field == 1) dst[3] ^= 1;
        packet[length] = 0;
        tcp->checksum = 0;
        tcp->checksum = wire_tcp_checksum(src, dst, (uint8_t *)tcp,
                                           length - 20 + (field == 2));
        memcpy(&before, c, sizeof(before));
        handle_ipv4(packet, length);
        CHECK(!memcmp(&before, c, sizeof(before)) && !ack_count);
    }

    tcp_conn_t *c = setup();
    c->state = TCP_CLOSED;
    c->claimed = 0;
    CHECK(net_tcp_listen(40000) >= 0);
    unsigned length = wire_packet(packet, 900, NULL, 0, TCP_SYN, 0);
    packet[36] ^= 1;
    memcpy(&before, c, sizeof(before));
    handle_ipv4(packet, length);
    CHECK(!memcmp(&before, c, sizeof(before)) && !ack_count);
    packet[36] ^= 1;
    handle_ipv4(packet, length);
    CHECK(c->state == TCP_SYN_RCVD && ack_count == 1);

    c = setup();
    for (unsigned short_length = 0; short_length < 20; short_length++) {
        wire_packet(packet, 900, NULL, 0, TCP_ACK, 0);
        ipv4_hdr_t *ip = (void *)packet;
        ip->total_len = htons(20 + short_length);
        ip->checksum = 0;
        ip->checksum = ip_checksum(ip, 20);
        memcpy(&before, c, sizeof(before));
        handle_ipv4(packet, 20 + short_length);
        CHECK(!memcmp(&before, c, sizeof(before)) && !ack_count);
    }
    length = wire_packet(packet, 900, data, 7, TCP_ACK, 0);
    tcp_hdr_t *missing = (void *)(packet + 20);
    CHECK(missing->checksum != 0);
    missing->checksum = 0;
    memcpy(&before, c, sizeof(before));
    handle_ipv4(packet, length);
    CHECK(!memcmp(&before, c, sizeof(before)) && !ack_count);

    /* Zero is a possible valid TCP checksum, not an opt-out as in IPv4 UDP. */
    c = setup();
    uint8_t zeros[2] = {0};
    length = wire_packet(packet, 900, zeros, 2, TCP_ACK, 0);
    tcp_hdr_t *tcp = (void *)(packet + 20);
    memcpy(packet + 40, &tcp->checksum, 2);
    tcp->checksum = 0;
    CHECK(wire_tcp_checksum(peer_ip, our_ip, packet + 20, length - 20) == 0);
    handle_ipv4(packet, length);
    CHECK(c->rx_len == 2 && last_ack == 902);

    /* The checksum helper is used by RX with arbitrary packet alignment. */
    uint8_t odd_src[5] __attribute__((aligned(8)));
    uint8_t odd_dst[5] __attribute__((aligned(8)));
    memcpy(odd_src + 1, peer_ip, 4); memcpy(odd_dst + 1, our_ip, 4);
    for (unsigned i = 0; i < 129; i++) {
        for (unsigned j = 0; j < i; j++) packet[1 + j] = (uint8_t)(j * 13);
        CHECK(tcp_checksum(odd_src + 1, odd_dst + 1, packet + 1, i) ==
               wire_tcp_checksum(peer_ip, our_ip, packet + 1, i));
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "basic")) test_basic();
    else if (!strcmp(argv[1], "append")) test_append_preempted();
    else if (!strcmp(argv[1], "read")) test_read_preempted(false);
    else if (!strcmp(argv[1], "peek")) test_read_preempted(true);
    else if (!strcmp(argv[1], "read-append")) test_read_append();
    else if (!strcmp(argv[1], "window")) test_window();
    else if (!strcmp(argv[1], "irq-state")) test_irq_state();
    else if (!strcmp(argv[1], "overlap")) test_overlap();
    else if (!strcmp(argv[1], "checksum")) test_checksum();
    else CHECK(false);
    CHECK(!irq_depth && interrupts_enabled && !pending_preemption && !copy_preemption);
    printf("TCP RX %s: %u checks\n", argv[1], checks);
    return 0;
}
