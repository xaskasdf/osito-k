#include "../kernel/apipa.c"
extern int printf(const char *, ...);
extern void abort(void);
static unsigned checks, probes, commits, busy, change_at;
static uint64_t now, epoch;
static bool ready;
static uint8_t address[4], gateway[4], dns[4];
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); abort(); } } while (0)
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void fb_puts(const char *s) { (void)s; }
void fb_putdec(uint64_t n) { (void)n; }
void boot_diag_mark(const char *s) { (void)s; }
void boot_diag_heartbeat(const char *s) { (void)s; }
uint64_t idt_get_ticks(void) { return now++; }
uint64_t net_link_epoch(void) { return epoch; }
bool net_link_ready(void) { return ready; }
void net_get_mac(uint8_t m[6]) { memcpy(m, "\x02\x01\x02\x03\x04\x05", 6); }
void net_poll(void) {}
void net_poll_wait(void)
{
    now++;
    if (change_at && now >= change_at) {
        change_at = 0; epoch++;
        memset(address, 0, 4); memset(gateway, 0, 4); memset(dns, 0, 4);
    }
}
int net_arp_lookup_nowait(const uint8_t ip[4], uint8_t mac[6]) { (void)ip; (void)mac; return -1; }
void net_arp_probe(const uint8_t ip[4])
{ CHECK(ip[0] == 169 && ip[1] == 254 && ip[2] >= 1 && ip[2] <= 254); probes++; }
int net_apply_ipv4(uint64_t e, const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], const uint8_t d[4])
{
    CHECK(e == epoch && ready);
    CHECK(!memcmp(mask, "\xff\xff\0\0", 4));
    if (busy) { busy--; return -1; }
    memcpy(address, ip, 4); memcpy(gateway, gw, 4); memcpy(dns, d, 4); commits++;
    return 0;
}
int main(void)
{
    for (unsigned mode = 0; mode < 4; mode++) {
        now = 0; epoch = 1; ready = true; probes = commits = busy = 0;
        memset(gateway, 99, 4); memset(dns, 88, 4); memset(address, 0, 4);
        change_at = mode == 1 ? 300 : mode == 2 ? 810 : 0;
        if (mode == 3) busy = 5;
        int rc = apipa_assign();
        CHECK(rc == (mode == 1 || mode == 2 ? -1 : 0));
        if (!rc) CHECK(commits == 1 && probes == 5 && address[0] == 169 && !gateway[0] && !dns[0]);
        else CHECK(!address[0] && !gateway[0] && !dns[0]);
        if (mode == 1) CHECK(!commits);
        if (mode == 2) CHECK(commits == 1);
    }
    ready = false; CHECK(apipa_assign() == -1);
    printf("APIPA link epoch/configuration: %u checks\n", checks);
    return 0;
}
