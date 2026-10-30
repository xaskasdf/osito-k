#define NET_HOST_TEST
#include "../kernel/net.c"

extern int printf(const char *, ...);
extern void abort(void);
static unsigned checks, wakes, sends;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); abort(); } } while (0)
nic_ops_t nic_ops;
static nic_link_state_t radio;
static bool busy;
static int read_link(nic_link_state_t *s) { if (busy) return -1; *s = radio; return 0; }
static bool link_up(void) { return radio.up; }
static void get_mac(uint8_t m[6]) { memcpy(m, radio.mac, 6); }
static int send_packet(const void *p, uint32_t n) { (void)p; (void)n; sends++; return 0; }
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int d) { (void)n; (void)d; }
void fb_puts(const char *s) { (void)s; }
void fb_putdec(uint64_t n) { (void)n; }
void sched_unblock(int p) { CHECK(p >= 10 && p < 14); wakes++; }
uint64_t idt_get_ticks(void) { return 100; }
static void listener(const uint8_t *ip, uint16_t port, const void *p, uint32_t n)
{ (void)ip; (void)port; (void)p; (void)n; }

static bool zero(const void *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++) if (((const uint8_t *)p)[i]) return false;
    return true;
}

int main(void)
{
    uint8_t ip[4] = {192,168,1,42}, mask[4] = {255,255,255,0};
    uint8_t gw[4] = {192,168,1,1}, dns[4] = {1,1,1,1}, empty[4] = {0};
    CHECK(nic_send(ip, 4) == -1 && nic_recv(ip, NULL) == -1 && !nic_link_up());
    nic_ops = (nic_ops_t){.read_link = read_link, .link_up = link_up, .get_mac = get_mac, .send = send_packet};
    net_init(empty);
    CHECK(!net_link_ready() && zero(our_ip, 4) && zero(gateway_ip, 4) && zero(dns_server, 4));
    uint8_t dns_copy[4] = {255, 255, 255, 255};
    CHECK(!net_dns_get_server(NULL));
    CHECK(!net_dns_get_server(dns_copy) && zero(dns_copy, 4));
    uint64_t epoch = net_link_epoch();
    CHECK(net_apply_ipv4(epoch, ip, mask, gw, dns) == -1);
    CHECK(net_udp_listen(7777, listener) == 0);
    unsigned listeners = udp_listener_count;
    tcp_listeners[0].active = true; tcp_listeners[0].port = 80;
    radio.generation = 1; radio.mac[0] = 2; radio.mac[5] = 10;
    CHECK(net_sync_link() && net_link_epoch() != epoch && !net_link_ready());
    radio.up = true; CHECK(net_sync_link() && net_link_ready());
    epoch = net_link_epoch();
    CHECK(net_apply_ipv4(epoch - 1, ip, mask, gw, dns) == -1);
    CHECK(!net_apply_ipv4(epoch, ip, mask, gw, dns));
    CHECK(net_dns_get_server(dns_copy) && !memcmp(dns_copy, dns, 4));
    dns_copy[0] = 2;
    CHECK(net_dns_get_server(dns_copy) && !memcmp(dns_copy, dns, 4));
    CHECK(!memcmp(our_ip, ip, 4) && !memcmp(our_mac, radio.mac, 6));
    CHECK(net_sync_link() && net_link_epoch() == epoch && !memcmp(our_ip, ip, 4));
    busy = true;
    CHECK(!net_sync_link() && net_link_epoch() == epoch && net_link_ready());
    CHECK(net_apply_ipv4(epoch, empty, empty, empty, empty) == -1 && !memcmp(our_ip, ip, 4));
    busy = false;
    in_net_poll = true; CHECK(net_apply_ipv4(epoch, ip, mask, gw, dns) == -1); in_net_poll = false;
    tcp_conns[0].claimed = 1; tcp_conns[0].state = TCP_ESTABLISHED;
    tcp_conns[0].tx_len = tcp_conns[0].rx_len = 123;
    arp_table[0].valid = true;
    for (unsigned i = 0; i < 4; i++) {
        dns_requests[i].state = DNS_REQ_BUILDING + i;
        dns_requests[i].id = i + 1;
        dns_requests[i].result_ip[0] = 8;
        net_waiters[i].type = NETWAIT_ARP + i;
        net_waiters[i].target = 0; net_waiters[i].proc_idx = 10 + i;
    }
    net_waiter_count = 4;
    radio.up = false;
    CHECK(net_sync_link() && !net_link_ready() && net_link_epoch() != epoch);
    CHECK(zero(our_ip, 4) && zero(gateway_ip, 4) && zero(netmask, 4) && zero(dns_server, 4));
    CHECK(!net_dns_get_server(dns_copy) && zero(dns_copy, 4));
    CHECK(!arp_table[0].valid && tcp_conns[0].claimed == 1 && tcp_conns[0].state == TCP_CLOSED);
    CHECK(!tcp_conns[0].tx_len && !tcp_conns[0].rx_len && wakes == 4 && !net_waiter_count);
    CHECK(!sends && udp_listener_count == (int)listeners && tcp_listeners[0].active);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(dns_requests[i].state == DNS_REQ_NEGATIVE && dns_requests[i].id == i + 1);
        CHECK(zero(dns_requests[i].result_ip, 4));
    }
    radio.up = true; radio.generation++;
    CHECK(net_apply_ipv4(epoch, ip, mask, gw, dns) == -1); /* Unpolled reconnect. */
    epoch = net_link_epoch();
    CHECK(!net_apply_ipv4(epoch, ip, mask, gw, dns));
    int op = async_alloc(); CHECK(op >= 0 && async_ops[op].link_epoch == epoch); async_release(op);
    radio.generation++; CHECK(net_sync_link() && zero(our_ip, 4)); /* Missed down/up edge. */
    nic_ops.read_link = NULL; net_init(ip);
    CHECK(net_link_ready() && !memcmp(our_ip, ip, 4) && net_link_epoch() == 1);
    CHECK(!net_apply_ipv4(1, ip, mask, gw, dns));
    printf("network link lifecycle: %u checks\n", checks);
    return 0;
}
