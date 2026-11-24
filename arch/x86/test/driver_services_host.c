/* Real lifecycle and profile policy, with hardware/filesystem/network fakes. */
#include "../kernel/driver_services.c"
extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
extern char *strcat(char *, const char *);
static unsigned checks, failures, starts, stops, connects, scans, dhcps, apipas, sshs, allocations;
static bool mounted, present, firmware, bad_firmware, bad_config, fail_join, fail_stop, wired;
static bool hda_present, hda_ready, fail_hda, dhcp_fails;
static uint64_t clock_ms;
static uint8_t ip[4];
static unsigned char config[4][4096];
static unsigned sizes[4];
static enum iwx_runtime_state radio;
static iwx_link_status_t link;
static iwx_scan_status_t scan;
static iwx_bss_t bss[3];
static unsigned bss_count;
static char diagnostics[512], serial_text[1024];
static pci_dev_t device = {.vendor_id = 0x8086, .device_id = 0x2723, .bar = {0x100000}};
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
void *kmalloc(size_t n) { void *p = malloc(n); if (p) allocations++; return p; }
void kfree(void *p) { if (p) { allocations--; free(p); } }
void wpa2_clear(void *p, size_t n) { memset(p, 0, n); }
void serial_puts(const char *s)
{
    CHECK(strlen(serial_text) + strlen(s) < sizeof(serial_text));
    strcat(serial_text, s);
}
void serial_putdec(uint64_t n) { CHECK(n <= 196); }
void boot_diag_mark(const char *s)
{
    CHECK(strlen(diagnostics) + strlen(s) + 1 < sizeof(diagnostics));
    strcat(diagnostics, s); strcat(diagnostics, "\n");
}
void sh_puts(const char *s) { (void)s; }
uint64_t idt_get_monotonic_ns(void) { return clock_ms * 1000000; }
int sched_spawn(const char *n, void (*f)(void)) { (void)n; (void)f; return 1; }
int sched_sleep_ticks(uint64_t n) { clock_ms += n * 10; return 0; }
void sched_yield(void) { clock_ms++; }
bool osfs3_is_mounted(void) { return mounted; }
void *osfs3_find_ci(const char *s) { return !strcmp(s, "fw") && firmware ? &device : NULL; }
const pci_dev_t *pci_get_wlan(void) { return present ? &device : NULL; }
pci_dev_t *pci_get_hda(void) { return hda_present ? &device : NULL; }
bool nic_is_iwx(void) { return !wired; }
bool hda_is_ready(void) { return hda_ready; }
int hda_init(uint64_t bar, uint8_t b, uint8_t d, uint8_t f)
{ (void)bar; (void)b; (void)d; (void)f; hda_ready = !fail_hda; return fail_hda ? -1 : 0; }
bool net_link_ready(void) { return wired || link.port_open; }
uint8_t *net_get_ip_ptr(void) { return ip; }
void net_poll(void) { }
int dhcp_discover(void) { dhcps++; if (!dhcp_fails) ip[0] = 192; return dhcp_fails ? -1 : 0; }
int apipa_assign(void) { apipas++; ip[0] = 169; return 0; }
const char *sshd_ensure_running(uint16_t port) { CHECK(port == 22); sshs++; return NULL; }
int iwx_get_status(iwx_status_t *s) { memset(s, 0, sizeof(*s)); s->state = radio; return 0; }
int iwx_link_status(iwx_link_status_t *s) { *s = link; return 0; }
int iwx_start(const void *d, uint32_t n, iwx_probe_report_t *r, iwx_nvm_report_t *nv, iwx_progress_fn fn, void *arg)
{ (void)d; (void)n; (void)nv; (void)fn; (void)arg; memset(r, 0, sizeof(*r)); starts++; radio = IWX_ACTIVE; link.state = IWX_LINK_IDLE; return 0; }
int iwx_stop(iwx_probe_report_t *r)
{ memset(r, 0, sizeof(*r)); stops++; radio = fail_stop ? IWX_QUARANTINED : IWX_OFF; memset(&link, 0, sizeof(link)); return fail_stop ? -1 : 0; }
int iwx_scan_results(iwx_scan_status_t *s, iwx_bss_t *p, unsigned cap)
{ *s = scan; unsigned n = bss_count < cap ? bss_count : cap; if (p) memcpy(p, bss, n * sizeof(*p)); return n; }
const char *iwx_scan_start(void) { scans++; scan.state = IWX_SCAN_RUNNING; return NULL; }
const char *iwx_connect_network(const iwx_bss_t *b, const uint8_t *pmk)
{ connects++; CHECK(!!pmk == b->rsn); if (pmk) CHECK(pmk[0] == 0xa5); link.state = fail_join ? IWX_LINK_FAILED : IWX_LINK_AUTH; return NULL; }
const driver_firmware_t driver_ax200_firmware = {.file = "fw"}, driver_ax200_license = {.file = "license"};
int driver_firmware_get(const driver_firmware_t *f, void **out, unsigned *n, const char **error)
{
    (void)f; *out = NULL; *n = 0; *error = NULL;
    if (bad_firmware) { *error = "bad hash"; return -1; }
    if (!firmware && !addressed()) return 1;
    firmware = true; *out = kmalloc(1); *n = 1; return 0;
}
static int config_index(const char *id)
{ for (unsigned i = 0; i < SERVICE_COUNT; i++) if (!strcmp(id, services[i].name)) return i; return -1; }
int driver_config_read(const char *id, void *p, unsigned cap, unsigned *n)
{
    int i = config_index(id);
    if (!mounted || i < 0 || bad_config) return -1;
    if (!sizes[i]) return 1;
    if (sizes[i] > cap) return -1;
    *n = sizes[i]; memcpy(p, config[i], *n); return 0;
}
int driver_config_write(const char *id, const void *p, unsigned n)
{
    int i = config_index(id);
    if (!mounted || i < 0 || bad_config || n > sizeof(config[0])) return -1;
    sizes[i] = n; memcpy(config[i], p, n); return 0;
}
static void reset(void)
{
    mounted = present = firmware = true;
    bad_firmware = bad_config = fail_join = fail_stop = wired = false;
    hda_present = hda_ready = fail_hda = dhcp_fails = false;
    starts = stops = connects = scans = dhcps = apipas = sshs = 0;
    diagnostics[0] = serial_text[0] = 0;
    clock_ms = 0; radio = IWX_OFF; memset(ip, 0, sizeof(ip));
    memset(&wifi, 0, sizeof(wifi)); ssh_config_loaded = false;
    memset(sizes, 0, sizeof(sizes)); memset(&link, 0, sizeof(link)); memset(&scan, 0, sizeof(scan));
    memset(bss, 0, sizeof(bss)); bss_count = 1;
    bss[0].ssid_len = 4; memcpy(bss[0].ssid, "home", 4); bss[0].capability = 0x11;
    bss[0].rsn = true; bss[0].rsn_len = 20; memcpy(bss[0].rsn_ie, wpa2_assoc_rsn + 2, 20);
    for (unsigned i = 0; i < SERVICE_COUNT; i++) {
        services[i].paused = services[i].busy = 0; services[i].next_ms = 0; services[i].state = DRIVER_DETECTED;
    }
}
static enum driver_state step(unsigned i)
{
    CHECK(driver_service_claim(&services[i]));
    enum driver_state state = services[i].step(&services[i]); services[i].state = state;
    driver_service_release(&services[i]); CHECK(!allocations); return state;
}
static void remember(void)
{
    uint8_t pmk[32]; memset(pmk, 0xa5, sizeof(pmk));
    CHECK(!driver_wlan_remember(&bss[0], pmk)); CHECK(wifi.profiles.count == 1);
    wifi.loaded = false; /* Simulate reboot: only disk state remains. */
}
int main(void)
{
    reset(); present = false; CHECK(step(WLAN) == DRIVER_ABSENT && !starts);
    reset(); mounted = false; CHECK(step(WLAN) == DRIVER_WAIT_FS);
    reset(); firmware = false; CHECK(step(WLAN) == DRIVER_WAIT_FIRMWARE && !starts);
    reset(); bad_firmware = true; CHECK(step(WLAN) == DRIVER_FAILED && !starts);
    reset(); bad_config = true; CHECK(step(WLAN) == DRIVER_FAILED && !starts);
    reset(); CHECK(step(WLAN) == DRIVER_STARTING && starts == 1);
    CHECK(step(WLAN) == DRIVER_WAIT_CONFIG && !scans && !connects);
    reset(); remember(); CHECK(step(WLAN) == DRIVER_STARTING);
    CHECK(step(WLAN) == DRIVER_CONNECTING && scans == 1);
    scan.state = IWX_SCAN_COMPLETE;
    CHECK(step(WLAN) == DRIVER_CONNECTING && connects == 1);
    CHECK(!strcmp(diagnostics, "wlan-auto-selected\n"));
    CHECK(!strcmp(serial_text, "[DRIVER] WLAN auto-selected SSID=\"home\" channel=\n"));
    link.state = IWX_LINK_ASSOCIATED; link.port_open = true;
    CHECK(step(WLAN) == DRIVER_READY); CHECK(step(IPV4) == DRIVER_READY && dhcps == 1);
    CHECK(!strcmp(diagnostics, "wlan-auto-selected\nwlan-auto-linked\ndriver-ipv4-ready\n"));
    CHECK(step(WLAN) == DRIVER_READY && step(IPV4) == DRIVER_READY);
    CHECK(!strcmp(diagnostics, "wlan-auto-selected\nwlan-auto-linked\ndriver-ipv4-ready\n"));
    CHECK(step(SSH) == DRIVER_DISABLED && !sshs);
    CHECK(!driver_sshd_autostart(1, 22)); CHECK(step(SSH) == DRIVER_READY && sshs == 1);
    memset(ip, 0, 4); CHECK(step(SSH) == DRIVER_WAIT_NETWORK);
    dhcp_fails = true; CHECK(step(IPV4) == DRIVER_READY && apipas == 1);
    unsigned char dhcp_only[] = {1, 1}; driver_config_write("ipv4", dhcp_only, 2); memset(ip, 0, 4);
    diagnostics[0] = 0;
    CHECK(step(IPV4) == DRIVER_FAILED && apipas == 1);
    CHECK(!strcmp(diagnostics, "driver-ipv4-failed\n"));
    reset(); memcpy(bss[0].ssid, "x\n\"\\", 4); remember(); step(WLAN); scan.state = IWX_SCAN_COMPLETE;
    CHECK(step(WLAN) == DRIVER_CONNECTING);
    CHECK(!strcmp(serial_text, "[DRIVER] WLAN auto-selected SSID=\"x???\" channel=\n"));
    reset(); remember(); step(WLAN); scan.state = IWX_SCAN_COMPLETE;
    bss[0].rsn = false; bss[0].capability = 1;
    CHECK(step(WLAN) == DRIVER_WAIT_NETWORK && !connects); /* no security downgrade */
    reset(); remember(); step(WLAN); scan.state = IWX_SCAN_COMPLETE; bss[0].ssid[0] = 'X';
    CHECK(step(WLAN) == DRIVER_WAIT_NETWORK && !connects); /* unknown networks ignored */
    reset(); remember(); step(WLAN); scan.state = IWX_SCAN_COMPLETE; fail_join = true;
    step(WLAN); CHECK(step(WLAN) == DRIVER_FAILED && stops == 1 && services[WLAN].retry_ms == 30000);
    reset(); remember(); step(WLAN); scan.state = IWX_SCAN_COMPLETE; fail_join = fail_stop = true;
    step(WLAN); CHECK(step(WLAN) == DRIVER_QUARANTINED && services[WLAN].paused);
    reset(); CHECK(driver_wlan_claim()); driver_manager_tick(&services[WLAN], 1, 0); CHECK(!starts);
    driver_wlan_release(1); driver_manager_tick(&services[WLAN], 1, 10000); CHECK(!starts);
    CHECK(driver_wlan_claim()); driver_wlan_release(-1); CHECK(services[WLAN].paused);
    CHECK(driver_wlan_claim()); driver_wlan_release(0); driver_manager_tick(&services[WLAN], 1, 20000); CHECK(starts == 1);
    driver_manager_tick(&services[WLAN], 1, 20000); CHECK(starts == 1 && services[WLAN].state == DRIVER_STARTING);
    reset(); wired = true; firmware = false; ip[0] = 192;
    CHECK(step(WLAN) == DRIVER_STARTING); CHECK(step(WLAN) == DRIVER_READY && !connects);
    reset(); hda_present = true; CHECK(step(HDA) == DRIVER_READY && hda_ready);
    reset(); hda_present = fail_hda = true; CHECK(step(HDA) == DRIVER_FAILED && services[HDA].paused);
    reset(); remember();
    CHECK(wlan_profiles_valid(&wifi.profiles, sizeof(wifi.profiles)));
    wifi.profiles.network[0].reserved = 1; CHECK(!wlan_profiles_valid(&wifi.profiles, sizeof(wifi.profiles)));
    wifi.profiles.network[0].reserved = 0; wlan_profiles_forget(&wifi.profiles, 0);
    CHECK(!wifi.profiles.count && wlan_profiles_valid(&wifi.profiles, sizeof(wifi.profiles)));
    CHECK(!allocations);
    printf("driver-services: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
