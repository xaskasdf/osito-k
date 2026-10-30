/* Real driver, WPA2, NIC dispatch and IPv4 client. Only hardware/the AP and
 * scheduler/console are simulated. This does not validate RF or firmware CCMP. */
#define IWX_NET_HOST_TEST
#define main transport_tests_main
#include "iwx_host.c"
#undef main
#define NET_HOST_TEST
#include "../kernel/nic.c"
#include "../kernel/net.c"
#include "../kernel/dhcp.c"

static unsigned ap_packets, ap_pn;
static bool ap_dhcp_unicast;
static unsigned bootstrap_received, fallback_received;
static const uint8_t ap_mac[6] = {2,8,9,10,11,12};
static const uint8_t ap_ip[4] = {192,168,7,1}, client_ip[4] = {192,168,7,42};
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int d) { (void)n; (void)d; }
void fb_puts(const char *s) { (void)s; }
void fb_putdec(uint64_t n) { (void)n; }
void boot_diag_mark(const char *s) { (void)s; }
void boot_diag_heartbeat(const char *s) { (void)s; }
uint64_t idt_get_ticks(void) { return hw.now / 10000000; }
int sched_current_get(void) { return 1; }
int sched_block_current(void) { return 1; }
void sched_unblock(int p) { (void)p; }
void ipv6_handle_packet(const uint8_t *p, uint32_t n, const uint8_t *mac)
{ (void)p; (void)n; (void)mac; }

void iwx_test_net_yield(void)
{
    hw.now += 10000000;
    CHECK(iwx_poll() >= 0);
}

static void ap_frame(uint16_t type, const void *body, unsigned n, bool group)
{
    uint8_t eth[IWX_ETH_MAX] = {0};
    CHECK(n <= sizeof(eth) - 14);
    memcpy(eth, runtime.link.local, 6); memcpy(eth + 6, ap_mac, 6);
    if (group) memset(eth, 255, 6);
    eth[12] = type >> 8; eth[13] = type;
    memcpy(eth + 14, body, n);
    queue_ethernet(eth, n + 14, runtime.link.status.rsn, ++ap_pn);
    ap_packets++;
}

static void ap_ipv4(const uint8_t *source, unsigned protocol, const void *body, unsigned n, bool broadcast)
{
    uint8_t p[1500] = {0};
    ipv4_hdr_t *ip = (void *)p;
    ip->ver_ihl = 0x45; ip->ttl = 64; ip->proto = protocol;
    memcpy(ip->src, source, 4);
    if (broadcast) memset(ip->dst, 255, 4); else memcpy(ip->dst, client_ip, 4);
    ip->total_len = htons(20 + n); ip->checksum = ip_checksum(ip, 20);
    memcpy(p + 20, body, n);
    ap_frame(0x800, p, 20 + n, broadcast);
}

static void ap_udp(const uint8_t *source, unsigned from, unsigned to, const void *body, unsigned n, bool broadcast)
{
    uint8_t p[1480] = {0};
    udp_hdr_t *udp = (void *)p;
    udp->src_port = htons(from); udp->dst_port = htons(to); udp->length = htons(n + 8);
    memcpy(p + 8, body, n);
    ap_ipv4(source, 17, p, n + 8, broadcast);
}

void iwx_test_net_tx(const uint8_t *page)
{
    CHECK(transport.tx_data && runtime.link.status.port_open);
    const uint8_t *p = page + 72;
    unsigned type = (page[70] << 8) | page[71];
    if (type == 0x806) {
        const arp_pkt_t *request = (const void *)p;
        CHECK(ntohs(request->oper) == ARP_OP_REQUEST);
        arp_pkt_t arp = *request;
        arp.oper = htons(ARP_OP_REPLY);
        memcpy(arp.tha, request->sha, 6); memcpy(arp.tpa, request->spa, 4);
        memcpy(arp.sha, ap_mac, 6); memcpy(arp.spa, request->tpa, 4);
        ap_frame(0x806, &arp, sizeof(arp), false);
        return;
    }
    CHECK(type == 0x800);
    const ipv4_hdr_t *ip = (const void *)p;
    CHECK(ip->ver_ihl == 0x45 && ip_checksum(ip, 20) == 0);
    unsigned n = ntohs(ip->total_len) - 20;
    p += 20;
    if (ip->proto == 1) {
        uint8_t icmp[64]; CHECK(n <= sizeof(icmp)); memcpy(icmp, p, n);
        CHECK(icmp[0] == ICMP_ECHO_REQUEST && ip_checksum(icmp, n) == 0);
        icmp[0] = ICMP_ECHO_REPLY; icmp[2] = icmp[3] = 0;
        uint16_t sum = ip_checksum(icmp, n); memcpy(icmp + 2, &sum, 2);
        ap_ipv4(ip->dst, 1, icmp, n, false);
        return;
    }
    CHECK(ip->proto == 17 && n >= 8);
    const udp_hdr_t *udp = (const void *)p;
    p += 8; n -= 8;
    if (ntohs(udp->dst_port) == 67) {
        const dhcp_packet_t *request = (const void *)p;
        CHECK(n == sizeof(*request) && request->op == 1 && !memcmp(request->chaddr, runtime.link.local, 6));
        dhcp_packet_t response = {0};
        response.op = 2; response.htype = 1; response.hlen = 6;
        response.xid = request->xid; response.cookie = request->cookie;
        memcpy(response.chaddr, request->chaddr, 16); memcpy(response.yiaddr, client_ip, 4);
        const uint8_t options[] = {53,1,2,54,4,192,168,7,1,51,4,0,0,14,16,
            1,4,255,255,255,0,3,4,192,168,7,1,6,4,192,168,7,1,255};
        memcpy(response.options, options, sizeof(options));
        response.options[2] = request->options[2] == DHCP_DISCOVER ? DHCP_OFFER : DHCP_ACK;
        ap_udp(ap_ip, 67, 68, &response, 240 + sizeof(options), !ap_dhcp_unicast);
    } else if (ntohs(udp->dst_port) == 53) {
        CHECK(n >= 17 && n < 256);
        uint8_t dns[300] = {0}; memcpy(dns, p, n);
        dns[2] = 0x81; dns[3] = 0x80; dns[6] = 0; dns[7] = 1;
        const uint8_t answer[] = {0xc0,0x0c,0,1,0,1,0,0,0,60,0,4,203,0,113,7};
        memcpy(dns + n, answer, sizeof(answer));
        ap_udp(ap_ip, 53, ntohs(udp->src_port), dns, n + sizeof(answer), false);
    } else CHECK(false);
}

static void cancelled(int result, void *ctx)
{
    CHECK(result == -1);
    (*(unsigned *)ctx)++;
}

static bool boot_carrier;
static bool boot_link_up(void) { return boot_carrier; }

static void test_boot_selection(void)
{
    CHECK(!nic_boot_prefer_wlan(false) && nic_boot_prefer_wlan(true));
    nic_ops.link_up = boot_link_up;
    for (unsigned link = 0; link < 2; link++) {
        boot_carrier = link != 0;
        CHECK(!nic_boot_prefer_wlan(false));
        CHECK(nic_boot_prefer_wlan(true) == !boot_carrier);
        CHECK(nic_ops.link_up == boot_link_up && !nic_is_iwx());
    }
    nic_ops = (nic_ops_t){0};
}

static void bootstrap_probe(const uint8_t *ip, uint16_t port, const void *p, uint32_t n)
{
    CHECK(!memcmp(ip, ap_ip, 4) && port == 67 && p && n == 4);
    bootstrap_received++;
}

static void fallback_probe(const uint8_t *ip, uint16_t port, const void *p, uint32_t n)
{
    (void)ip; (void)port; (void)p; (void)n;
    fallback_received++;
}

static void test_bootstrap_demux(void)
{
    bootstrap_received = fallback_received = 0;
    CHECK(!net_udp_listen(0, fallback_probe));
    const uint8_t body[4] = {0};
    ap_udp(ap_ip, 67, 68, body, sizeof(body), false);
    for (unsigned i = 0; i < 4; i++) net_poll_wait();
    CHECK(!fallback_received); /* No explicit DHCP listener yet. */
    CHECK(!net_udp_listen(68, bootstrap_probe));
    for (unsigned fault = 0; fault < 16; fault++) {
        uint8_t p[40] = {0};
        unsigned ihl = fault == 14 ? 24 : 20, n = ihl + 12;
        ipv4_hdr_t *ip = (void *)p;
        ip->ver_ihl = 0x40 | (ihl / 4); ip->ttl = 64; ip->proto = 17;
        ip->total_len = htons(n);
        memcpy(ip->src, ap_ip, 4); memcpy(ip->dst, client_ip, 4);
        udp_hdr_t *udp = (void *)(p + ihl);
        udp->src_port = htons(67); udp->dst_port = htons(68); udp->length = htons(12);
        switch (fault) {
        case 1: ip->ver_ihl = 0x65; break;
        case 2: ip->ver_ihl = 0x40; break;
        case 3: ip->ver_ihl = 0x4f; break;
        case 4: ip->total_len = htons(19); break;
        case 5: ip->total_len = htons(n + 1); break;
        case 6: ip->frag = htons(1); break;
        case 7: ip->frag = htons(0x2000); break;
        case 9: udp->length = htons(7); break;
        case 10: udp->length = htons(13); break;
        case 11: udp->src_port = htons(66); break;
        case 12: udp->dst_port = htons(69); break;
        case 13: ip->proto = 6; break;
        case 15: ip->frag = htons(0x4000); break;
        }
        ip->checksum = ip_checksum(ip, ihl);
        if (fault == 8) ip->checksum ^= 1;
        unsigned before = bootstrap_received;
        ap_frame(0x800, p, n, false);
        for (unsigned i = 0; i < 4; i++) net_poll_wait();
        CHECK(bootstrap_received == before + (fault == 0 || fault == 14 || fault == 15));
        CHECK(!fallback_received && !our_ip[0]);
    }
    CHECK(!net_udp_unlisten(68, bootstrap_probe));
    CHECK(!net_udp_unlisten(0, fallback_probe));
}

int main(void)
{
    test_boot_selection();
    for (unsigned scenario = 0; scenario < 9; scenario++) {
        bool secure = scenario % 3 != 0;
        security_tkip = scenario % 3 == 2;
        ap_dhcp_unicast = scenario >= 3;
        if (secure) security_m1(false, false);
        else { link_fixture(); hw.tx_queue = 511; hw.tx_write_pointer = 0x12ff; }
        nic_bind_iwx(); CHECK(nic_is_iwx());
        uint8_t zero[4] = {0}; net_init(zero);
        uint64_t epoch = net_link_epoch();
        CHECK(!net_link_ready() && !nic_link_up());
        if (secure) {
            uint8_t eapol[WPA2_EAPOL_MAX];
            unsigned n = eapol_message(eapol, true, 2, 1, 10);
            queue_eapol(eapol, n, false, 0);
        } else CHECK(!iwx_connect_network(&runtime.bss[0], NULL));
        CHECK(iwx_poll() > 0 && iwx_poll() > 0);
        net_poll(); CHECK(net_link_ready() && net_link_epoch() != epoch);
        ap_pn = 47; ap_packets = 0;
        test_bootstrap_demux();
        if (scenario >= 6) {
            const uint8_t local_ip[4] = {169,254,1,2}, mask[4] = {255,255,0,0};
            CHECK(!net_apply_ipv4(net_link_epoch(), local_ip, mask, zero, zero));
        }
        CHECK(!dhcp_discover() && !memcmp(our_ip, client_ip, 4));
        CHECK(!memcmp(gateway_ip, ap_ip, 4) && !memcmp(dns_server, ap_ip, 4));
        CHECK(ap_packets >= 2);
        uint8_t address[4] = {0};
        CHECK(!net_dns_resolve("example.test", address));
        CHECK(!memcmp(address, "\xcb\x00\x71\x07", 4));
        unsigned before = net_icmp_get_rx_count();
        net_icmp_send_echo(address, 1);
        for (unsigned i = 0; i < 4; i++) net_poll_wait();
        CHECK(net_icmp_get_rx_count() == before + 1);
        CHECK(runtime.link.status.data_tx >= 5 && runtime.link.status.data_rx >= 5);
        unsigned listeners = udp_listener_count;
        unsigned cancelled_count = 0;
        CHECK(!net_dns_resolve_async("cancel.test", address, cancelled, &cancelled_count));
        CHECK(!net_tcp_connect_async(address, 443, 40000, cancelled, &cancelled_count));
        const uint8_t other_local[4] = {192,168,7,200};
        CHECK(!net_tcp_connect_async(other_local, 80, 40001, cancelled, &cancelled_count));
        CHECK(net_async_pending());
        iwx_probe_report_t report; CHECK(!iwx_stop(&report)); net_poll();
        CHECK(!net_link_ready() && !our_ip[0] && !gateway_ip[0] && !dns_server[0]);
        CHECK(udp_listener_count == (int)listeners && !hw.violations && !hw.live_pages);
        CHECK(cancelled_count == 3 && !net_async_pending() && !tcp_conns[0].claimed);
        net_poll(); CHECK(cancelled_count == 3);
    }
    printf("AX200/NIC/DHCP/ARP/DNS/ICMP integration: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
