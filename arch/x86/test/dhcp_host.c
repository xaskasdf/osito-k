#include "../kernel/dhcp.c"

extern int printf(const char *, ...);
extern void abort(void);
extern void *malloc(size_t);
extern void free(void *);
static unsigned checks, sends, applied, requests, mode, pending;
static uint64_t now, epoch = 1;
static uint64_t sent_at[8], reply_at, first_request_at, link_change_at;
static uint8_t sent_type[8];
static uint32_t first_xid;
static bool link_up = true, listen_fail;
static unsigned apply_busy;
static uint8_t mac[6] = {2,3,4,5,6,7}, address[4], gateway[4], dns[4];
static udp_handler_t handler;
static dhcp_packet_t sent;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); abort(); } } while (0)

void serial_puts(const char *s) { (void)s; }
void serial_puthex(uint64_t n, int digits) { (void)n; (void)digits; }
void serial_putdec(uint64_t n) { (void)n; }
void fb_puts(const char *s) { (void)s; }
void boot_diag_mark(const char *s) { (void)s; }
void boot_diag_heartbeat(const char *s) { (void)s; }
uint64_t idt_get_ticks(void) { return now++; }
uint64_t net_link_epoch(void) { return epoch; }
bool net_link_ready(void) { return link_up; }
void net_get_mac(uint8_t m[6]) { memcpy(m, mac, 6); }
void net_poll(void) { }
int net_udp_listen(uint16_t port, udp_handler_t cb)
{ CHECK(port == 68 && cb); if (listen_fail) return -1; handler = cb; return 0; }
int net_apply_ipv4(uint64_t e, const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], const uint8_t d[4])
{
    (void)mask;
    if (mode == 6) { epoch++; mode = 0; }
    if (!link_up || e != epoch) return -1;
    if (apply_busy) { apply_busy--; return -1; }
    memcpy(address, ip, 4); memcpy(gateway, gw, 4); memcpy(dns, d, 4); applied++;
    return 0;
}
static int send_request(uint16_t dst, uint16_t src, const void *data, uint32_t len, bool unicast)
{
    CHECK(dst == 67 && src == 68 && len == sizeof(sent));
    memcpy(&sent, data, len);
    CHECK(sends < 8);
    sent_at[sends] = now; sent_type[sends] = sent.options[2];
    if (!sends) first_xid = sent.xid;
    else CHECK(sent.xid == first_xid);
    sends++;
    CHECK(!memcmp(sent.chaddr, mac, 6) && sent.op == 1 && sent.htype == 1 && sent.hlen == 6);
    CHECK(sent.options[0] == 53 && sent.options[1] == 1 && sent.xid == dhcp_xid);
    CHECK(sent.cookie == be32(DHCP_MAGIC));
    pending = sent.options[2] == DHCP_DISCOVER ? DHCP_OFFER : DHCP_ACK;
    if (pending == DHCP_ACK && !requests++) first_request_at = now;
    if (mode == 11) reply_at = pending == DHCP_OFFER ? sent_at[0] + 2000 : first_request_at + 600;
    if (mode == 12) link_change_at = sent_at[0] + 1500;
    if (unicast) {
        CHECK(dhcp_renewing && !sent.flags && !memcmp(sent.ciaddr, dhcp_bound.ip, 4));
        CHECK(sent.options[3] == 55); /* No requested-address/server-id when renewing. */
    } else {
        CHECK(sent.flags == 0x80 && !memcmp(sent.ciaddr, "\0\0\0\0", 4));
        if (pending == DHCP_ACK) CHECK(sent.options[3] == 50 && sent.options[9] == 54);
    }
    return 0;
}
int net_udp_send_broadcast(uint16_t d, uint16_t s, const void *p, uint32_t n)
{ return send_request(d, s, p, n, false); }
int net_udp_send(const uint8_t ip[4], uint16_t d, uint16_t s, const void *p, uint32_t n)
{ CHECK(!memcmp(ip, server_ip, 4)); return send_request(d, s, p, n, true); }

static unsigned reply(dhcp_packet_t *p, unsigned type)
{
    memset(p, 0, sizeof(*p));
    p->op = 2; p->htype = 1; p->hlen = 6; p->xid = dhcp_xid; p->cookie = be32(DHCP_MAGIC);
    memcpy(p->chaddr, mac, 6); memcpy(p->yiaddr, "\xc0\xa8\x01\x2a", 4);
    const uint8_t options[] = {53,1,2, 54,4,192,168,1,1, 51,4,0,0,14,16,
        1,4,255,255,255,0, 3,4,192,168,1,1, 6,4,1,1,1,1, 255};
    memcpy(p->options, options, sizeof(options)); p->options[2] = type;
    return 240 + sizeof(options);
}
void net_poll_wait(void)
{
    now++;
    if (link_change_at && now >= link_change_at) { epoch++; link_change_at = 0; }
    if (!pending) return;
    if (now < reply_at) return;
    unsigned type = pending; pending = 0;
    if (mode == 1 || mode == 12 || (mode == 2 && type == DHCP_ACK)) return;
    if (mode == 4) { link_up = false; return; }
    if (mode == 5 && type == DHCP_ACK) epoch++;
    if (mode == 9 && type == DHCP_ACK && requests == 1) return;
    dhcp_packet_t p;
    unsigned n = reply(&p, mode == 3 && type == DHCP_ACK ? DHCP_NAK : type);
    if (mode == 8) { p.options[21] = 255; n = 262; } /* No router/DNS. */
    if (mode == 10 && type == DHCP_ACK) p.options[8] ^= 1;
    CHECK(handler != NULL);
    handler((uint8_t *)"\xc0\xa8\x01\xfe", 67, &p, n); /* Relay source. */
}

static void reset(void)
{
    now = 1000; epoch = 1; link_up = true; sends = applied = requests = pending = mode = apply_busy = 0;
    reply_at = first_request_at = link_change_at = 0;
    listen_fail = false; handler = NULL;
    dhcp_phase = dhcp_state_lock = dhcp_operation_lock = 0;
    dhcp_epoch = 1; dhcp_lease_epoch = dhcp_lease_start = dhcp_lease_ticks = 0;
    dhcp_renewing = false; dhcp_got_reply = false; dhcp_xid = 0x12345678;
    memset(&dhcp_bound, 0, sizeof(dhcp_bound));
    dhcp_restore_bound(); net_get_mac(dhcp_mac);
    memset(address, 0, 4); memset(gateway, 77, 4); memset(dns, 88, 4);
}

static void test_retry_timing(void)
{
    reset();
    dhcp_retry_seed = 0;
    bool seen[201] = {0};
    for (unsigned i = 0; i < 10000; i++) {
        uint64_t ticks = dhcp_retry_ticks(0);
        CHECK(ticks >= 300 && ticks <= 500);
        seen[ticks - 300] = true;
    }
    for (unsigned i = 0; i < 201; i++) CHECK(seen[i]);
    for (unsigned attempt = 1; attempt <= 32; attempt++) {
        uint64_t base = 400ULL << (attempt < 4 ? attempt : 4);
        for (unsigned i = 0; i < 100; i++) {
            uint64_t ticks = dhcp_retry_ticks(attempt);
            CHECK(ticks >= base - 100 && ticks <= base + 100 && ticks <= 6400);
        }
    }
    CHECK(dhcp_retry_ticks(~0U) <= 6400);

    for (unsigned phase = 0; phase < 2; phase++) {
        reset(); mode = phase ? 2 : 1;
        memcpy(address, "\xa9\xfe\x38\xbd", 4);
        CHECK(dhcp_discover() == -1 && sends == 4 && !applied);
        CHECK(!memcmp(address, "\xa9\xfe\x38\xbd", 4) && gateway[0] == 77 && dns[0] == 88);
        unsigned first = phase ? 1 : 0;
        for (unsigned i = first; i < sends; i++) {
            CHECK(sent_type[i] == (phase ? DHCP_REQUEST : DHCP_DISCOVER));
            uint64_t elapsed = (i + 1 < sends ? sent_at[i + 1] : now) - sent_at[i];
            uint64_t base = 400ULL << (i - first);
            CHECK(elapsed >= base - 100 && elapsed <= base + 103);
        }
        uint64_t elapsed = now - sent_at[first];
        CHECK(elapsed >= (phase ? 2500U : 5600U) && elapsed <= (phase ? 3110U : 6412U));
        CHECK(!dhcp_phase && !dhcp_operation_lock && !dhcp_state_lock);
    }

    reset(); mode = 11;
    CHECK(!dhcp_discover() && sends == 5 && requests == 2 && applied == 1);
    CHECK(first_request_at - sent_at[0] >= 2000 && now - first_request_at >= 600);
    CHECK(address[3] == 42 && !dhcp_operation_lock);

    reset(); mode = 12;
    CHECK(dhcp_discover() == -1 && sends == 3 && !applied && epoch == 2);
    CHECK(now - sent_at[0] < 1510 && !dhcp_phase && !dhcp_operation_lock);

    reset(); mode = 1; now = ~(uint64_t)0 - 200;
    CHECK(dhcp_discover() == -1 && sends == 4 && now < 6500);
    CHECK(!dhcp_phase && !dhcp_operation_lock);
}

static void test_options(void)
{
    for (unsigned opti = 0; opti < 6; opti++) {
        unsigned code = (unsigned[]){1,3,6,51,53,54}[opti];
        for (unsigned size = 0; size < 256; size++) {
            uint8_t *p = malloc(size + 3); CHECK(p);
            p[0] = code; p[1] = size;
            memset(p + 2, 1, size); p[size + 2] = 255;
            dhcp_options_t out = {0};
            bool valid = code == 3 || code == 6 ? size && !(size & 3) : size == (code == 53 ? 1U : 4U);
            CHECK(parse_options(&out, p, size + 3, true) == valid);
            for (unsigned cut = 0; cut < size + 3; cut++) {
                dhcp_options_t truncated = {0};
                CHECK(!parse_options(&truncated, p, cut, true));
            }
            free(p);
        }
    }
    dhcp_options_t out = {0};
    CHECK(!parse_options(&out, (uint8_t[]){53,1,2,53,1,5,255}, 7, true));
    out = (dhcp_options_t){0}; CHECK(!parse_options(&out, (uint8_t[]){52,1,4,255}, 4, true));
    out = (dhcp_options_t){0}; CHECK(!parse_options(&out, (uint8_t[]){52,1,1,255}, 4, false));
}

static void test_replies(void)
{
    for (unsigned fault = 0; fault <= 18; fault++) {
        reset(); dhcp_wait_phase(DHCP_OFFER);
        dhcp_packet_t p; unsigned n = reply(&p, DHCP_OFFER), port = 67;
        switch (fault) {
        case 1: port = 68; break;
        case 2: p.op = 1; break;
        case 3: p.htype = 2; break;
        case 4: p.hlen = 5; break;
        case 5: p.chaddr[5] ^= 1; break;
        case 6: p.xid++; break;
        case 7: p.cookie++; break;
        case 8: n = 239; break;
        case 9: n = 250; break;
        case 10: p.options[2] = DHCP_ACK; break;
        case 11: p.options[5] = 0; break;
        case 12: memset(p.options + 11, 0, 4); break;
        case 13: p.options[18] = 0; break;
        case 14: p.yiaddr[0] = 224; break;
        case 15: epoch++; break;
        case 16: link_up = false; break;
        case 17: dhcp_phase = 0; break;
        case 18: dhcp_state_lock = 1; break;
        }
        dhcp_handler((uint8_t *)"\xc0\xa8\x01\x01", port, &p, n);
        CHECK(dhcp_got_reply == (fault == 0));
        if (fault) CHECK(!offered_ip[0]);
    }
    reset(); dhcp_wait_phase(DHCP_OFFER);
    dhcp_packet_t p; reply(&p, DHCP_OFFER);
    memcpy(p.file, p.options + 15, 6); p.file[6] = 255;
    p.options[15] = 52; p.options[16] = 1; p.options[17] = 1; p.options[18] = 255;
    dhcp_handler(mac, 67, &p, 259);
    CHECK(dhcp_got_reply && offered_mask[0] == 255 && offered_lease == 3600);
    dhcp_wait_phase(0);
    p.yiaddr[3] = 90; dhcp_handler(mac, 67, &p, sizeof(p)); CHECK(offered_ip[3] == 42);
}

int main(void)
{
    test_options(); test_replies(); test_retry_timing();
    for (unsigned m = 0; m <= 10; m++) {
        reset(); mode = m; if (m == 7) apply_busy = 3;
        int rc = dhcp_discover();
        bool success = m == 0 || m == 7 || m == 8 || m == 9;
        CHECK(rc == (success ? 0 : -1));
        CHECK(applied == (unsigned)success && !dhcp_phase && !dhcp_operation_lock && !dhcp_state_lock);
        if (success) {
            CHECK(address[3] == 42 && dhcp_lease_ticks == 360000 && dhcp_lease_epoch == epoch);
            CHECK(gateway[0] == (m == 8 ? 0 : 192) && dns[0] == (m == 8 ? 0 : 1));
            if (m == 9) CHECK(requests == 2);
        } else CHECK(!dhcp_lease_ticks);
    }
    reset(); listen_fail = true; CHECK(dhcp_discover() == -1 && !sends && !dhcp_operation_lock);
    reset(); link_up = false; CHECK(dhcp_discover() == -1 && !sends);
    reset(); dhcp_operation_lock = 1; CHECK(dhcp_discover() == -1 && !sends);
    reset(); CHECK(!dhcp_discover());
    uint64_t lease = dhcp_lease_start;
    mode = 2; sends = requests = 0;
    CHECK(dhcp_discover() == -1 && dhcp_lease_start == lease && dhcp_lease_ticks == 360000);
    CHECK(!memcmp(offered_ip, dhcp_bound.ip, 4));
    mode = 0; now = lease + 180001;
    sends = requests = 0;
    dhcp_check_renewal(); CHECK(applied == 2 && dhcp_lease_start > lease && !dhcp_renewing);
    epoch++; dhcp_check_renewal(); CHECK(!dhcp_lease_ticks && applied == 2);
    reset(); CHECK(!dhcp_discover()); now = dhcp_lease_start + dhcp_lease_ticks;
    dhcp_check_renewal(); CHECK(!dhcp_lease_ticks && !address[0] && !gateway[0] && !dns[0]);
    printf("DHCP validation/link epochs: %u checks\n", checks);
    return 0;
}
