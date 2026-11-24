#define NET_HOST_TEST
#include "../kernel/net.c"

extern int printf(const char *, ...);
extern void exit(int);
static unsigned checks, sends, frames, ack_frames;
static uint64_t ticks;
static bool fail_tx, immediate_ack, nested_send, stream_mode;
static uint32_t peer_sequence, peer_bytes;
static uint32_t sequences[128], lengths[128];
static uint8_t payloads[128][TCP_MSS];
static uint8_t source[TCP_TX_BUF_SIZE + TCP_MSS], nested[137];
static const uint8_t peer_ip[4] = {10, 0, 2, 2};
static const uint8_t peer_mac[6] = {2, 3, 4, 5, 6, 7};
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

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

static void acknowledge(uint32_t ack, const void *data, unsigned size)
{
    uint8_t bytes[sizeof(tcp_hdr_t) + 32] = {0};
    CHECK(size <= 32);
    tcp_hdr_t *p = (void *)bytes;
    p->src_port = htons(443); p->dst_port = htons(40000);
    p->data_off = 5 << 4; p->flags = TCP_ACK; p->window = htons(32768);
    p->seq = htonl(tcp_conns[0].rcv_nxt); p->ack = htonl(ack);
    if (size) memcpy(bytes + sizeof(*p), data, size);
    p->checksum = tcp_checksum(peer_ip, our_ip, bytes, sizeof(*p) + size);
    handle_tcp(peer_ip, our_ip, bytes, sizeof(*p) + size);
}

static int transmit(const void *packet, uint32_t length)
{
    CHECK(length >= 60);
    const eth_hdr_t *eth = packet;
    if (ntohs(eth->ethertype) == ETH_TYPE_ARP) return 0;
    CHECK(ntohs(eth->ethertype) == ETH_TYPE_IP4);
    const ipv4_hdr_t *ip = (const void *)((const uint8_t *)packet + ETH_HDR_LEN);
    CHECK(ip_checksum(ip, sizeof(*ip)) == 0 && ip->proto == IP_PROTO_TCP);
    const tcp_hdr_t *tcp = (const void *)((const uint8_t *)ip + sizeof(*ip));
    uint32_t total = ntohs(ip->total_len) - sizeof(*ip);
    CHECK(tcp_checksum(ip->src, ip->dst, tcp, total) == 0);
    uint32_t header = (tcp->data_off >> 4) * 4;
    CHECK(header <= total);
    uint32_t n = total - header;
    if (!n) { ack_frames++; return 0; }
    sends++;
    if (fail_tx) return -1;
    if (stream_mode) {
        /* Independent in-order receiver: reject local submissions, lose
         * accepted packets and delay cumulative ACKs on different periods. */
        if (!(sends % 7)) return -1;
        if (!(sends % 11)) return 0;
        if (ntohl(tcp->seq) == peer_sequence) {
            const uint8_t *data = (const uint8_t *)tcp + header;
            for (unsigned i = 0; i < n; i++) {
                uint32_t offset = peer_bytes + i;
                CHECK(data[i] == (uint8_t)(offset * 31 + offset / 251));
            }
            peer_sequence += n;
            peer_bytes += n;
        }
        if (sends % 3) acknowledge(peer_sequence, NULL, 0);
        return 0;
    }
    CHECK(frames < 128 && n <= TCP_MSS);
    unsigned frame = frames++;
    sequences[frame] = ntohl(tcp->seq); lengths[frame] = n;
    memcpy(payloads[frame], (const uint8_t *)tcp + header, n);
    if (nested_send) {
        nested_send = false;
        CHECK(net_tcp_send(0, nested, sizeof(nested)) == sizeof(nested));
    }
    if (immediate_ack) acknowledge(sequences[frame] + n, NULL, 0);
    return 0;
}

static tcp_conn_t *setup(uint32_t sequence)
{
    const uint8_t ip[4] = {10, 0, 2, 15};
    memset(tcp_conns, 0, sizeof(tcp_conns));
    memset(tcp_listeners, 0, sizeof(tcp_listeners));
    memset(net_waiters, 0, sizeof(net_waiters));
    net_waiter_count = 0; in_net_poll = false;
    ticks = 100;
    fail_tx = immediate_ack = nested_send = stream_mode = false;
    peer_sequence = sequence; peer_bytes = 0;
    sends = frames = ack_frames = 0;
    nic_ops = (nic_ops_t){.recv = recv_empty, .send = transmit, .link_up = link_up};
    net_init(ip);
    arp_update(peer_ip, peer_mac);
    tcp_conn_t *c = &tcp_conns[0];
    c->claimed = 1; c->state = TCP_ESTABLISHED;
    memcpy(c->remote_ip, peer_ip, 4);
    c->local_port = 40000; c->remote_port = 443;
    c->snd_nxt = c->snd_una = sequence; c->snd_wnd = 32768;
    c->peer_mss = TCP_MSS;
    c->rcv_nxt = 900;
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i * 31 + i / 251);
    for (unsigned i = 0; i < sizeof(nested); i++) nested[i] = (uint8_t)(i + 197);
    return c;
}

static void queue_matches(tcp_conn_t *c, const void *data, unsigned size)
{
    CHECK(c->tx_len == size);
    CHECK(c->snd_nxt - c->snd_una == size);
    if (size) {
        CHECK(c->tx_seq == c->snd_una);
        CHECK(memcmp(c->tx_buf, data, size) == 0);
    }
}

static void poll_due(tcp_conn_t *c)
{
    CHECK(c->rto_tick >= ticks);
    ticks = c->rto_tick;
    net_poll();
    CHECK(!in_net_poll);
}

static void test_normal(void)
{
    tcp_conn_t *c = setup(1234);
    CHECK(net_tcp_send(0, source, sizeof(source)) == TCP_TX_BUF_SIZE);
    queue_matches(c, source, TCP_TX_BUF_SIZE);
    CHECK(!net_tcp_send_ready(0));
    CHECK(net_tcp_send(0, source, 1) == 0);
    acknowledge(1234 + 731, NULL, 0);
    queue_matches(c, source + 731, TCP_TX_BUF_SIZE - 731);
    CHECK(net_tcp_send_ready(0));
    poll_due(c);
    CHECK(sequences[frames - 1] == 1234 + 731);
    CHECK(memcmp(payloads[frames - 1], source + 731, lengths[frames - 1]) == 0);
    acknowledge(c->snd_nxt, NULL, 0);
    queue_matches(c, NULL, 0);
    CHECK(net_tcp_send(0, source, 83) == 83);
    queue_matches(c, source, 83);
}

static void test_early_ack(void)
{
    tcp_conn_t *c = setup(1234);
    immediate_ack = true;
    CHECK(net_tcp_send(0, source, 3 * TCP_MSS + 11) == 3 * TCP_MSS + 11);
    queue_matches(c, NULL, 0);
    CHECK(!c->rto_tick && !c->rto_count);
    unsigned before = sends;
    ticks += 1000; net_poll();
    CHECK(sends == before);
}

static void test_pressure(void)
{
    tcp_conn_t *c = setup(1234);
    fail_tx = true;
    CHECK(net_tcp_send(0, source, 731) == 731);
    queue_matches(c, source, 731);
    CHECK(c->state == TCP_ESTABLISHED && !net_tcp_error(0));
    CHECK(!frames && c->rto_tick > ticks);
    fail_tx = false; immediate_ack = true;
    poll_due(c);
    CHECK(frames == 1 && sequences[0] == 1234 && lengths[0] == 731);
    CHECK(memcmp(payloads[0], source, 731) == 0);
    queue_matches(c, NULL, 0);
    CHECK(!c->rto_tick && !c->rto_count);
}

static void test_wrap(void)
{
    tcp_conn_t *c = setup(0xffffff00u);
    CHECK(net_tcp_send(0, source, 1024) == 1024);
    acknowledge(0x100, NULL, 0);
    queue_matches(c, source + 512, 512);
    poll_due(c);
    CHECK(sequences[frames - 1] == 0x100);
    CHECK(memcmp(payloads[frames - 1], source + 512, 512) == 0);
    acknowledge(0x300, NULL, 0);
    queue_matches(c, NULL, 0);
    CHECK(net_tcp_send_ready(0));
}

static void test_invalid_ack(void)
{
    tcp_conn_t *c = setup(1234);
    CHECK(net_tcp_send(0, source, 1024) == 1024);
    acknowledge(c->snd_nxt + 1, source, 32);
    queue_matches(c, source, 1024);
    CHECK(!c->rx_len && c->rcv_nxt == 900);
    CHECK(ack_frames == 1);
    acknowledge(c->snd_una - 1, NULL, 0);
    queue_matches(c, source, 1024);
}

static void test_half_closed(void)
{
    tcp_conn_t *c = setup(1234);
    c->state = TCP_CLOSE_WAIT;
    CHECK(net_tcp_send(0, source, 1024) == 1024);
    acknowledge(1234 + 137, NULL, 0);
    queue_matches(c, source + 137, 1024 - 137);
    unsigned before = frames;
    poll_due(c);
    CHECK(frames == before + 1 && sequences[before] == 1234 + 137);
    CHECK(memcmp(payloads[before], source + 137, lengths[before]) == 0);
    acknowledge(c->snd_nxt, NULL, 0);
    queue_matches(c, NULL, 0);
    CHECK(c->state == TCP_CLOSE_WAIT);
}

static void test_nested_send(void)
{
    tcp_conn_t *c = setup(1234);
    nested_send = true;
    CHECK(net_tcp_send(0, source, 731) == 731);
    uint8_t expected[731 + sizeof(nested)];
    memcpy(expected, source, 731); memcpy(expected + 731, nested, sizeof(nested));
    queue_matches(c, expected, sizeof(expected));
    poll_due(c);
    CHECK(sequences[frames - 1] == 1234);
    CHECK(lengths[frames - 1] == sizeof(expected));
    CHECK(memcmp(payloads[frames - 1], expected, sizeof(expected)) == 0);
}

static void test_no_credit(void)
{
    tcp_conn_t *c = setup(1234);
    c->snd_wnd = 0;
    CHECK(!net_tcp_send(0, source, 731) && !net_tcp_send_ready(0));
    CHECK(!frames && !c->tx_len && c->snd_nxt == 1234);
    c->state = TCP_CLOSED;
    CHECK(net_tcp_send(0, source, 731) == -1);
    CHECK(!frames && !c->tx_len && c->snd_nxt == 1234);
}

static void test_loss_stream(void)
{
    uint32_t initial = 0xffff8000u;
    tcp_conn_t *c = setup(initial);
    stream_mode = true;
    uint32_t accepted = 0, total = 512 * 1024;
    unsigned rounds;
    for (rounds = 0; rounds < 30000 && (accepted < total || c->tx_len); rounds++) {
        uint8_t bytes[4096];
        unsigned n = total - accepted;
        if (n > sizeof(bytes)) n = sizeof(bytes);
        for (unsigned i = 0; i < n; i++) {
            uint32_t offset = accepted + i;
            bytes[i] = (uint8_t)(offset * 31 + offset / 251);
        }
        int result = net_tcp_send(0, bytes, n);
        CHECK(result >= 0 && (unsigned)result <= n);
        accepted += result;
        CHECK(c->state == TCP_ESTABLISHED);
        CHECK(c->tx_len <= TCP_TX_BUF_SIZE && c->snd_nxt - c->snd_una == c->tx_len);
        if (c->tx_len) CHECK(c->tx_seq == c->snd_una);
        for (unsigned i = 0; i < c->tx_len; i++) {
            uint32_t offset = c->tx_seq - initial + i;
            CHECK(c->tx_buf[i] == (uint8_t)(offset * 31 + offset / 251));
        }
        ticks += 20;
        net_poll();
    }
    CHECK(rounds < 30000 && accepted == total && peer_bytes == total);
    queue_matches(c, NULL, 0);
    printf("Loss stream: %u bytes, %u submissions, %u rounds\n", total, sends, rounds);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "normal")) test_normal();
    else if (!strcmp(argv[1], "early-ack")) test_early_ack();
    else if (!strcmp(argv[1], "pressure")) test_pressure();
    else if (!strcmp(argv[1], "wrap")) test_wrap();
    else if (!strcmp(argv[1], "invalid-ack")) test_invalid_ack();
    else if (!strcmp(argv[1], "half-closed")) test_half_closed();
    else if (!strcmp(argv[1], "nested-send")) test_nested_send();
    else if (!strcmp(argv[1], "no-credit")) test_no_credit();
    else if (!strcmp(argv[1], "loss-stream")) test_loss_stream();
    else CHECK(false);
    printf("TCP TX %s: %u checks\n", argv[1], checks);
    return 0;
}
