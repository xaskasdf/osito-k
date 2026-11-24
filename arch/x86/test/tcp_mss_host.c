#define NET_HOST_TEST
#include "../kernel/net.c"

extern int printf(const char *, ...);
extern void exit(int);
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned checks, frames, wire_max, peer_limit, delivered;
static uint64_t ticks;
static uint32_t origin, peer_next;
static bool reject_data, ack_data;
static const uint8_t peer_ip[4] = {10, 0, 2, 2};
static const uint8_t peer_mac[6] = {2, 3, 4, 5, 6, 7};
static uint8_t source[12000];
nic_ops_t nic_ops;
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int d) { (void)n; (void)d; }
void fb_puts(const char *s) { (void)s; }
void fb_putdec(uint64_t n) { (void)n; }
uint64_t idt_get_ticks(void) { return ticks; }
bool sched_is_enabled(void) { return false; }
void sched_yield(void) { ticks++; }
int sched_current_get(void) { return 1; }
int sched_block_current(void) { return 1; }
void sched_unblock(int p) { (void)p; }
void ipv6_handle_packet(const uint8_t *p, uint32_t n, const uint8_t *mac)
{ (void)p; (void)n; (void)mac; }
static int recv_empty(void *p, uint32_t *n) { (void)p; (void)n; return -1; }
static bool link_up(void) { return true; }

static void incoming(unsigned flags, uint32_t seq, uint32_t ack,
                     int mss, bool timestamps)
{
    uint8_t bytes[60] = {0};
    tcp_hdr_t *tcp = (void *)bytes;
    unsigned size = 20;
    tcp->src_port = htons(443); tcp->dst_port = htons(40000);
    tcp->seq = htonl(seq); tcp->ack = htonl(ack);
    tcp->flags = flags; tcp->window = htons(32768);
    if (mss >= 0) {
        bytes[size++] = 2; bytes[size++] = 4;
        bytes[size++] = (unsigned)mss >> 8; bytes[size++] = mss;
    }
    if (timestamps) {
        bytes[size++] = 1; bytes[size++] = 1;
        bytes[size++] = 8; bytes[size++] = 10;
        uint32_t ts = htonl(100);
        memcpy(bytes + size, &ts, 4); size += 8;
    }
    tcp->data_off = (size / 4) << 4;
    tcp->checksum = tcp_checksum(peer_ip, our_ip, tcp, size);
    handle_tcp(peer_ip, our_ip, bytes, size);
}

static void acknowledge(uint32_t seq)
{
    incoming(TCP_ACK, tcp_conns[0].rcv_nxt, seq, -1, false);
}

static int transmit(const void *packet, uint32_t length)
{
    const eth_hdr_t *eth = packet;
    if (ntohs(eth->ethertype) == ETH_TYPE_ARP) return 0;
    CHECK(ntohs(eth->ethertype) == ETH_TYPE_IP4);
    const ipv4_hdr_t *ip = (const void *)((const uint8_t *)packet + ETH_HDR_LEN);
    CHECK(ip_checksum(ip, 20) == 0 && ip->proto == IP_PROTO_TCP);
    const tcp_hdr_t *tcp = (const void *)((const uint8_t *)ip + 20);
    unsigned total = ntohs(ip->total_len) - 20;
    unsigned header = (tcp->data_off >> 4) * 4;
    CHECK(header >= 20 && header <= 60 && total >= header);
    CHECK(tcp_checksum(ip->src, ip->dst, tcp, total) == 0);
    if (tcp->flags & TCP_SYN) {
        const uint8_t *option = (const uint8_t *)tcp + 20;
        CHECK(option[0] == 2 && option[1] == 4);
        CHECK(((unsigned)option[2] << 8 | option[3]) == 1460);
    }
    unsigned size = total - header;
    if (!size) return 0;
    frames++;
    if (length > wire_max) wire_max = length;
    if (length > 1514)
        printf("Oversized Ethernet frame: %u bytes (MTU 1500)\n", length);
    CHECK(length <= 1514 && ntohs(ip->total_len) <= 1500);
    CHECK(size + header - 20 <= peer_limit);
    unsigned offset = ntohl(tcp->seq) - origin;
    CHECK(offset <= sizeof(source) && size <= sizeof(source) - offset);
    CHECK(!memcmp((const uint8_t *)tcp + header, source + offset, size));
    if (reject_data) return -1;
    CHECK(ntohl(tcp->seq) == peer_next);
    peer_next += size; delivered += size;
    if (ack_data) acknowledge(peer_next);
    return 0;
}

static tcp_conn_t *setup(int mss, bool timestamps, bool passive)
{
    const uint8_t ip[4] = {10, 0, 2, 15};
    memset(tcp_conns, 0, sizeof(tcp_conns));
    memset(tcp_listeners, 0, sizeof(tcp_listeners));
    memset(net_waiters, 0, sizeof(net_waiters));
    net_waiter_count = 0; in_net_poll = 0;
    ticks = 100; frames = wire_max = delivered = 0;
    reject_data = false; ack_data = true;
    peer_limit = mss > 0 ? (unsigned)mss : 536;
    nic_ops = (nic_ops_t){.recv = recv_empty, .send = transmit, .link_up = link_up};
    net_init(ip); arp_update(peer_ip, peer_mac);
    if (passive) {
        CHECK(net_tcp_listen(40000) == 0);
        incoming(TCP_SYN, 899, 0, mss, timestamps);
        CHECK(tcp_conns[0].state == TCP_SYN_RCVD);
        acknowledge(tcp_conns[0].snd_nxt);
    } else {
        CHECK(net_tcp_connect_begin(peer_ip, 443, 40000) == 0);
        incoming(TCP_SYN | TCP_ACK, 899, tcp_conns[0].snd_nxt, mss, timestamps);
    }
    CHECK(tcp_conns[0].state == TCP_ESTABLISHED);
    CHECK(tcp_conns[0].tsopt_ok == timestamps);
    origin = peer_next = tcp_conns[0].snd_nxt;
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = i * 31 + i / 251;
    return &tcp_conns[0];
}

static void send_all(tcp_conn_t *c)
{
    CHECK(net_tcp_send(0, source, sizeof(source)) == sizeof(source));
    CHECK(delivered == sizeof(source) && !c->tx_len && c->snd_una == c->snd_nxt);
}

static void test_sizes(int mss, bool timestamps)
{
    for (unsigned passive = 0; passive < 2; passive++) {
        tcp_conn_t *c = setup(mss, timestamps, passive);
        send_all(c);
        unsigned expected = peer_limit < 1460 ? peer_limit : 1460;
        CHECK(wire_max == 54 + expected);
    }
}

static void test_options(void)
{
    for (unsigned ts = 0; ts < 2; ts++) {
        for (unsigned blocks = 0; blocks <= 4; blocks++) {
            tcp_conn_t *c = setup(1460, ts, false);
            /* SACK is not negotiated until RX reassembly exists; still bound
             * every option layout the serializer can emit. */
            c->sack_ok = 1; c->n_sack_blocks = blocks;
            for (unsigned i = 0; i < blocks; i++) {
                c->sack_blocks[i][0] = 2000 + i * 100;
                c->sack_blocks[i][1] = 2050 + i * 100;
            }
            send_all(c);
            CHECK(wire_max == 1514);
        }
    }
}

static void test_retransmit(bool fast)
{
    for (unsigned ts = 0; ts < 2; ts++) {
        tcp_conn_t *c = setup(1000, ts, false);
        reject_data = true;
        CHECK(net_tcp_send(0, source, 6000) == 6000);
        CHECK(c->tx_len == 6000 && !delivered);
        reject_data = false;
        for (unsigned n = 0; n < 20 && c->tx_len; n++) {
            if (fast) {
                uint32_t ack = c->snd_una;
                for (unsigned i = 0; i < 3; i++) acknowledge(ack);
            } else {
                ticks = c->rto_tick;
                net_poll();
            }
        }
        CHECK(delivered == 6000 && !c->tx_len && c->snd_una == c->snd_nxt);
    }
}

static void test_non_syn(void)
{
    tcp_conn_t *c = setup(600, true, false);
    incoming(TCP_ACK, c->rcv_nxt, c->snd_una, 1460, false);
    send_all(c);
    CHECK(wire_max == 654);
}

static void test_tiny(void)
{
    tcp_conn_t *c = setup(8, true, false);
    CHECK(net_tcp_send(0, source, 1) == 0 && !net_tcp_send_ready(0));
    CHECK(!c->tx_len && c->snd_nxt == origin && !frames);
    c = setup(32, true, false);
    CHECK(net_tcp_send(0, source, 100) == 100 && delivered == 100 && frames == 5);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "normal")) test_sizes(1460, false);
    else if (!strcmp(argv[1], "timestamps")) test_sizes(1460, true);
    else if (!strcmp(argv[1], "peer")) { test_sizes(1000, false); test_sizes(1000, true); }
    else if (!strcmp(argv[1], "default")) { test_sizes(-1, false); test_sizes(-1, true); test_sizes(0, true); }
    else if (!strcmp(argv[1], "large")) test_sizes(65535, true);
    else if (!strcmp(argv[1], "options")) test_options();
    else if (!strcmp(argv[1], "rto")) test_retransmit(false);
    else if (!strcmp(argv[1], "fast")) test_retransmit(true);
    else if (!strcmp(argv[1], "non-syn")) test_non_syn();
    else if (!strcmp(argv[1], "tiny")) test_tiny();
    else CHECK(false);
    printf("TCP MSS %s: %u checks\n", argv[1], checks);
    return 0;
}
