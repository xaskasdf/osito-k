#define NET_HOST_TEST
#include "../kernel/net.c"

extern int printf(const char *, ...);
extern void exit(int);
static unsigned checks, syn_attempts, syn_sent, arp_requests, wakes;
static uint64_t ticks;
static bool fail_tx, allow_reset;
static uint32_t wire_seq, wire_ack;
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
void sched_unblock(int p) { CHECK(p == 1); wakes++; }
void ipv6_handle_packet(const uint8_t *p, uint32_t n, const uint8_t *mac)
{ (void)p; (void)n; (void)mac; }
static int recv_empty(void *p, uint32_t *n) { (void)p; (void)n; return -1; }
static bool link_up(void) { return true; }

static int transmit(const void *packet, uint32_t length)
{
    const eth_hdr_t *eth = packet;
    CHECK(length >= 60);
    if (ntohs(eth->ethertype) == ETH_TYPE_ARP) {
        arp_requests++;
        return 0;
    }
    CHECK(ntohs(eth->ethertype) == ETH_TYPE_IP4);
    const ipv4_hdr_t *ip = (const void *)((const uint8_t *)packet + ETH_HDR_LEN);
    CHECK(ip_checksum(ip, sizeof(*ip)) == 0 && ip->proto == IP_PROTO_TCP);
    const tcp_hdr_t *tcp = (const void *)((const uint8_t *)ip + sizeof(*ip));
    CHECK(tcp_checksum(ip->src, ip->dst, tcp, ntohs(ip->total_len) - sizeof(*ip)) == 0);
    if (allow_reset && tcp->flags == (TCP_RST | TCP_ACK)) return 0;
    CHECK(tcp->flags == (TCP_SYN | TCP_ACK));
    wire_seq = ntohl(tcp->seq); wire_ack = ntohl(tcp->ack);
    syn_attempts++;
    if (fail_tx) return -1;
    syn_sent++;
    return 0;
}

static int setup(bool warm, uint32_t isn)
{
    const uint8_t ip[4] = {10, 0, 2, 15};
    memset(tcp_conns, 0, sizeof(tcp_conns));
    memset(tcp_listeners, 0, sizeof(tcp_listeners));
    memset(net_waiters, 0, sizeof(net_waiters));
    net_waiter_count = 0;
    ticks = 100;
    fail_tx = false;
    allow_reset = false;
    syn_attempts = syn_sent = arp_requests = wakes = 0;
    nic_ops = (nic_ops_t){.recv = recv_empty, .send = transmit, .link_up = link_up};
    net_init(ip);
    tcp_isn_counter = isn;
    if (warm) arp_update(peer_ip, peer_mac);
    int listener = net_tcp_listen(22);
    CHECK(listener == 0);
    return listener;
}

static void packet(uint8_t flags, uint32_t seq, uint32_t ack)
{
    tcp_hdr_t p = {0};
    p.src_port = htons(40000); p.dst_port = htons(22);
    p.data_off = 5 << 4; p.flags = flags; p.window = htons(32768);
    p.seq = htonl(seq); p.ack = htonl(ack);
    p.checksum = tcp_checksum(peer_ip, our_ip, &p, sizeof(p));
    handle_tcp(peer_ip, our_ip, (const uint8_t *)&p, sizeof(p));
}

static void poll_at(uint64_t now)
{
    ticks = now;
    net_poll();
    CHECK(!in_net_poll);
}

int main(void)
{
    int listener = setup(false, 1234);
    packet(TCP_SYN, 77, 0);
    CHECK(tcp_conns[0].state == TCP_SYN_RCVD && !syn_sent && arp_requests == 1);
    CHECK(tcp_listener_take_ready(&tcp_listeners[listener]) == -1);
    arp_update(peer_ip, peer_mac);
    poll_at(199);
    CHECK(!syn_sent);
    poll_at(200);
    CHECK(syn_sent == 1); /* Cold SYN-ACK must retry without a duplicate SYN. */
    CHECK(wire_seq == 1234 && wire_ack == 78 && tcp_conns[0].snd_nxt == 1235);
    net_waiters[0] = (net_waiter_t){NETWAIT_TCP_ACCEPT, listener, 1, 1000};
    net_waiter_count = 1;
    packet(TCP_ACK, 78, 1235);
    CHECK(tcp_conns[0].state == TCP_ESTABLISHED && wakes == 1);
    CHECK(!tcp_conns[0].rto_tick && !tcp_conns[0].rto_count && tcp_conns[0].rto >= 300);
    CHECK(tcp_listener_take_ready(&tcp_listeners[listener]) == 0);
    poll_at(10000);
    CHECK(syn_sent == 1 && tcp_conns[0].claimed);

    listener = setup(true, 0xffffffffU);
    packet(TCP_SYN, 77, 0);
    CHECK(syn_sent == 1 && wire_seq == 0xffffffffU && tcp_conns[0].snd_nxt == 0);
    poll_at(200);
    CHECK(syn_sent == 2 && wire_seq == 0xffffffffU && tcp_conns[0].snd_nxt == 0);
    CHECK(tcp_conns[0].rto_tick == 400 && tcp_conns[0].rto_count == 1);
    ticks = 250;
    packet(TCP_SYN, 77, 0);
    CHECK(syn_sent == 2 && tcp_conns[0].rto_tick == 400 && tcp_conns[0].snd_nxt == 0);
    fail_tx = true;
    poll_at(400);
    CHECK(syn_attempts == 3 && syn_sent == 2 && tcp_conns[0].snd_nxt == 0);
    packet(TCP_ACK, 78, 0); /* ACK for the earlier successful SYN-ACK. */
    CHECK(tcp_conns[0].state == TCP_ESTABLISHED && !tcp_conns[0].rto_tick);
    CHECK(tcp_listener_take_ready(&tcp_listeners[listener]) == 0);

    setup(true, 1234);
    fail_tx = true;
    packet(TCP_SYN, 77, 0);
    CHECK(!syn_sent && syn_attempts == 1 && tcp_conns[0].snd_nxt == 1235);
    packet(TCP_ACK, 78, 1234); /* Does not acknowledge the SYN sequence byte. */
    CHECK(tcp_conns[0].state == TCP_SYN_RCVD);
    fail_tx = false;
    poll_at(200);
    CHECK(syn_sent == 1 && wire_seq == 1234);
    packet(TCP_ACK, 78, 1235);
    CHECK(tcp_conns[0].state == TCP_ESTABLISHED);

    listener = setup(true, 1234);
    packet(TCP_SYN, 77, 0);
    uint64_t prior = 100, interval = 100;
    for (unsigned retry = 0; retry < 7; retry++) {
        uint64_t due = tcp_conns[0].rto_tick;
        CHECK(due == prior + interval);
        poll_at(due - 1);
        CHECK(syn_sent == retry + 1);
        poll_at(due);
        CHECK(syn_sent == retry + 2 && tcp_conns[0].snd_nxt == 1235);
        prior = due;
        interval *= 2;
        if (interval > 6000) interval = 6000;
    }
    CHECK(tcp_conns[0].rto_tick - 100 >= 18000);
    poll_at(tcp_conns[0].rto_tick);
    CHECK(tcp_conns[0].state == TCP_CLOSED && !tcp_conns[0].claimed);
    CHECK(tcp_conns[0].close_reason == NET_TCP_ERROR_TIMED_OUT);
    CHECK(tcp_listeners[listener].pending_conns[0] == TCP_LISTENER_SLOT_FREE);
    CHECK(syn_sent == 8);
    packet(TCP_SYN, 99, 0);
    CHECK(tcp_conns[0].state == TCP_SYN_RCVD && tcp_conns[0].claimed);
    CHECK(wire_ack == 100 && syn_sent == 9);

    setup(false, 1234);
    packet(TCP_SYN, 77, 0);
    for (unsigned retry = 0; retry < 8; retry++) poll_at(tcp_conns[0].rto_tick);
    CHECK(tcp_conns[0].state == TCP_CLOSED && !tcp_conns[0].claimed && !syn_sent);
    CHECK(arp_requests == 8);

    listener = setup(true, 1234);
    packet(TCP_SYN, 77, 0);
    allow_reset = true;
    net_tcp_stop_listen(listener);
    poll_at(10000);
    CHECK(syn_sent == 1 && !tcp_conns[0].claimed);
    printf("TCP passive open: %u checks\n", checks);
    return 0;
}
