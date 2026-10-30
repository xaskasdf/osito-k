#include "driver_manager.h"
#include "driver_store.h"
#include "driver_firmware.h"
#include "wlan_profiles.h"
#include "wpa2_crypto.h"
#include "wpa2_rsn.h"
#include "net.h"
#include "nic.h"
#include "sshd.h"
#include "../drivers/iwx.h"
#include "../drivers/hda.h"
#include "../fs/ositofs3.h"

extern void *kmalloc(size_t);
extern void kfree(void *);
extern uint64_t idt_get_monotonic_ns(void);
extern int sched_spawn(const char *, void (*)(void));
extern int sched_sleep_ticks(uint64_t);
extern void sched_yield(void);
extern void serial_puts(const char *);
extern void serial_putdec(uint64_t);
extern void boot_diag_mark(const char *);
extern void sh_puts(const char *);
extern uint8_t *net_get_ip_ptr(void);
extern int dhcp_discover(void);
extern int apipa_assign(void);
extern pci_dev_t *pci_get_hda(void);

static enum driver_state wlan_step(driver_service_t *);
static enum driver_state ssh_step(driver_service_t *);
static enum driver_state hda_step(driver_service_t *);
static enum driver_state ipv4_step(driver_service_t *);
static unsigned long long now_ms(void);
enum { HDA, IPV4, SSH, WLAN, SERVICE_COUNT };
static driver_service_t services[SERVICE_COUNT] = {
    [WLAN] = {.name = "wlan", .step = wlan_step, .clock_ms = now_ms},
    [SSH] = {.name = "sshd", .step = ssh_step, .clock_ms = now_ms},
    [HDA] = {.name = "hda", .step = hda_step, .clock_ms = now_ms},
    [IPV4] = {.name = "ipv4", .step = ipv4_step, .clock_ms = now_ms}
};
static struct {
    wlan_profiles_t profiles;
    bool loaded, joining;
    unsigned candidate;
    uint64_t deadline;
} wifi;
static bool worker_started;
static bool ssh_config_loaded;
static unsigned char ssh_config[4];

static unsigned long long now_ms(void) { return idt_get_monotonic_ns() / 1000000; }
static bool addressed(void)
{
    uint8_t *ip = net_get_ip_ptr();
    return net_link_ready() && ip[0] != 0;
}

static int profiles_load(void)
{
    if (wifi.loaded) return 0;
    unsigned n = 0;
    int rc = driver_config_read("wlan", &wifi.profiles, sizeof(wifi.profiles), &n);
    if (rc == 1) wifi.profiles = (wlan_profiles_t){.version = 1, .enabled = 1};
    else if (rc || !wlan_profiles_valid(&wifi.profiles, n)) {
        wpa2_clear(&wifi.profiles, sizeof(wifi.profiles)); return -1;
    }
    wifi.loaded = true; return 0;
}

static unsigned group_cipher(const iwx_bss_t *b)
{
    enum wpa2_cipher group;
    if (!b->ssid_len || b->ssid_len > 32 || (b->capability & 3) != 1) return 255;
    if (b->rsn) {
        if (!(b->capability & 0x10) || wpa2_rsn_select(b->rsn_ie, b->rsn_len, &group)) return 255;
        return group;
    }
    return b->wpa || (b->capability & 0x10) ? 255 : 0;
}

static enum driver_state wlan_failure(driver_service_t *s, const char *error)
{
    iwx_probe_report_t report;
    wifi.joining = false;
    s->detail = error; s->retry_ms = 30000;
    if (iwx_stop(&report)) {
        s->detail = "DMA shutdown incomplete; manual recovery required";
        s->paused = 1; return DRIVER_QUARANTINED;
    }
    net_poll();
    return DRIVER_FAILED;
}

static enum driver_state wlan_step(driver_service_t *s)
{
    s->detail = NULL;
    if (!pci_get_wlan()) return DRIVER_ABSENT;
    if (!osfs3_is_mounted()) return DRIVER_WAIT_FS;
    if (profiles_load()) { s->detail = "invalid WLAN configuration; preserved"; s->retry_ms = 60000; return DRIVER_FAILED; }
    if (!wifi.profiles.enabled) return DRIVER_DISABLED;
    iwx_status_t status;
    if (iwx_get_status(&status)) { s->detail = "driver owned by another operation"; return DRIVER_STARTING; }
    if (status.state == IWX_QUARANTINED) { s->paused = 1; return DRIVER_QUARANTINED; }
    if (status.state != IWX_ACTIVE) {
        /* A live wired NIC remains selected. Firmware may still be prepared
         * over Ethernet, but the manager never steals the active route. */
        void *data = NULL; unsigned n = 0;
        int rc;
        if (!osfs3_find_ci(driver_ax200_firmware.file)) {
            rc = driver_firmware_get(&driver_ax200_license, &data, &n, &s->detail);
            kfree(data); data = NULL;
            if (rc) { s->retry_ms = 60000; return rc == 1 ? DRIVER_WAIT_FIRMWARE : DRIVER_FAILED; }
        }
        rc = driver_firmware_get(&driver_ax200_firmware, &data, &n, &s->detail);
        if (rc) { s->retry_ms = 60000; return rc == 1 ? DRIVER_WAIT_FIRMWARE : DRIVER_FAILED; }
        iwx_probe_report_t report;
        iwx_nvm_report_t nvm;
        rc = iwx_start(data, n, &report, &nvm, NULL, NULL);
        kfree(data);
        if (rc) return wlan_failure(s, report.error ? report.error : "firmware start failed");
        s->retry_ms = 100; return DRIVER_STARTING;
    }
    if (!nic_is_iwx()) { s->detail = "wired adapter selected; WLAN standby"; return DRIVER_READY; }
    iwx_link_status_t link;
    if (iwx_link_status(&link)) return DRIVER_CONNECTING;
    if (link.port_open && net_link_ready()) {
        if (wifi.joining) {
            serial_puts("[DRIVER] WLAN data port open; channel=");
            serial_putdec(link.channel); serial_puts("\n");
            boot_diag_mark("wlan-auto-linked");
        }
        wifi.joining = false; return DRIVER_READY;
    }
    if (wifi.joining) {
        if (link.state == IWX_LINK_FAILED || link.state == IWX_LINK_STOPPED || now_ms() >= wifi.deadline)
            return wlan_failure(s, "known-network association failed; retrying with backoff");
        s->retry_ms = 100; return DRIVER_CONNECTING;
    }
    if (link.state != IWX_LINK_IDLE) return wlan_failure(s, "link lost; restarting transport");
    if (!wifi.profiles.count) { s->detail = "use wlan connect once to remember a network"; return DRIVER_WAIT_CONFIG; }
    iwx_scan_status_t scan;
    if (iwx_scan_results(&scan, NULL, 0) < 0) return DRIVER_CONNECTING;
    if (scan.state == IWX_SCAN_RUNNING || scan.state == IWX_SCAN_ABORTING) {
        s->retry_ms = 250; return DRIVER_CONNECTING;
    }
    if (scan.state != IWX_SCAN_COMPLETE) {
        const char *error = iwx_scan_start();
        if (error) return wlan_failure(s, error);
        s->retry_ms = 250; return DRIVER_CONNECTING;
    }
    iwx_bss_t *networks = kmalloc(IWX_SCAN_BSS_MAX * sizeof(*networks));
    if (!networks) { s->detail = "scan snapshot allocation failed"; return DRIVER_FAILED; }
    int count = iwx_scan_results(&scan, networks, IWX_SCAN_BSS_MAX);
    int selected = -1; unsigned profile = 0;
    for (unsigned i = 0; i < wifi.profiles.count && selected < 0; i++) {
        unsigned index = (wifi.candidate + i) % wifi.profiles.count;
        const wlan_profile_t *p = &wifi.profiles.network[index];
        for (int j = 0; j < count; j++) {
            iwx_bss_t *b = &networks[j];
            if (wlan_profiles_match(p, b->ssid, b->ssid_len, b->rsn, group_cipher(b)) &&
                (selected < 0 || b->signal_dbm > networks[selected].signal_dbm)) {
                selected = j; profile = index;
            }
        }
    }
    const char *error = NULL;
    if (selected >= 0) {
        const wlan_profile_t *p = &wifi.profiles.network[profile];
        wifi.candidate = (profile + 1) % wifi.profiles.count;
        char ssid[33];
        for (unsigned i = 0; i < p->length; i++) {
            unsigned char c = p->ssid[i];
            ssid[i] = c >= 32 && c < 127 && c != '"' && c != '\\' ? c : '?';
        }
        ssid[p->length] = 0;
        serial_puts("[DRIVER] WLAN auto-selected SSID=\""); serial_puts(ssid);
        serial_puts("\" channel="); serial_putdec(networks[selected].channel);
        serial_puts("\n");
        boot_diag_mark("wlan-auto-selected");
        error = iwx_connect_network(&networks[selected], p->secure ? p->pmk : NULL);
        if (!error) { wifi.joining = true; wifi.deadline = now_ms() + 30000; }
    }
    kfree(networks);
    if (error) return wlan_failure(s, error);
    if (selected < 0) {
        s->detail = "no compatible known network visible"; s->retry_ms = 30000;
        error = iwx_scan_start();
        if (error) return wlan_failure(s, error);
        return DRIVER_WAIT_NETWORK;
    }
    s->retry_ms = 100; return DRIVER_CONNECTING;
}

static enum driver_state ssh_step(driver_service_t *s)
{
    unsigned n;
    s->detail = NULL;
    if (!osfs3_is_mounted()) return DRIVER_WAIT_FS;
    if (!ssh_config_loaded) {
        int rc = driver_config_read("sshd", ssh_config, sizeof(ssh_config), &n);
        if (rc == 1) { ssh_config[0] = 1; ssh_config[1] = 0; ssh_config[2] = 0; ssh_config[3] = 22; }
        else if (rc || n != 4 || ssh_config[0] != 1 || ssh_config[1] > 1 || (!ssh_config[2] && !ssh_config[3])) {
            s->detail = "invalid SSH autostart policy"; s->retry_ms = 60000; return DRIVER_FAILED;
        }
        ssh_config_loaded = true;
    }
    if (!ssh_config[1]) return DRIVER_DISABLED;
    if (!addressed()) return DRIVER_WAIT_NETWORK;
    s->detail = sshd_ensure_running((unsigned)ssh_config[2] << 8 | ssh_config[3]);
    s->retry_ms = s->detail ? 5000 : 1000;
    return s->detail ? DRIVER_FAILED : DRIVER_READY;
}

static enum driver_state hda_step(driver_service_t *s)
{
    s->detail = NULL;
    const pci_dev_t *dev = pci_get_hda();
    if (!dev || !dev->bar[0]) return DRIVER_ABSENT;
    if (hda_is_ready()) return DRIVER_READY;
    unsigned char config[2] = {1, 1}; unsigned n;
    if (osfs3_is_mounted()) {
        int rc = driver_config_read("hda", config, sizeof(config), &n);
        if (rc < 0 || (!rc && (n != 2 || config[0] != 1 || config[1] > 1))) {
            s->detail = "invalid HDA policy"; s->paused = 1; return DRIVER_FAILED;
        }
    }
    if (!config[1]) return DRIVER_DISABLED;
    if (hda_init(dev->bar[0], dev->bus, dev->dev, dev->func)) {
        s->detail = "no supported audio output; console remains available";
        s->paused = 1; return DRIVER_FAILED;
    }
    return DRIVER_READY;
}

static enum driver_state ipv4_step(driver_service_t *s)
{
    s->detail = NULL;
    if (!net_link_ready()) return DRIVER_WAIT_NETWORK;
    if (addressed()) return DRIVER_READY;
    if (!osfs3_is_mounted()) return DRIVER_WAIT_FS;
    unsigned char config[2] = {1, 0}; unsigned n;
    int rc = driver_config_read("ipv4", config, sizeof(config), &n);
    if (rc < 0 || (!rc && (n != 2 || config[0] != 1 || config[1] > 2))) {
        s->detail = "invalid IPv4 policy"; s->retry_ms = 60000; return DRIVER_FAILED;
    }
    /* Serialize against interactive WLAN connect, which also runs DHCP. */
    if (!driver_service_claim(&services[WLAN])) return DRIVER_CONNECTING;
    s->detail = "requesting IPv4";
    rc = config[1] == 2 ? -1 : dhcp_discover();
    if (rc && config[1] != 1 && !addressed() && net_link_ready()) rc = apipa_assign();
    driver_service_release(&services[WLAN]);
    boot_diag_mark(rc ? "driver-ipv4-failed" : "driver-ipv4-ready");
    if (rc) { s->detail = "IPv4 configuration failed"; s->retry_ms = 30000; return DRIVER_FAILED; }
    s->detail = NULL; return DRIVER_READY;
}

static void driver_worker(void)
{
    enum driver_state previous[SERVICE_COUNT];
    for (unsigned i = 0; i < SERVICE_COUNT; i++) previous[i] = -1;
    for (;;) {
        driver_manager_tick(services, SERVICE_COUNT, now_ms());
        for (unsigned i = 0; i < SERVICE_COUNT; i++) {
            if (!driver_service_claim(&services[i])) continue;
            if (services[i].state != previous[i]) {
                previous[i] = services[i].state;
                serial_puts("[DRIVER] "); serial_puts(services[i].name); serial_puts(": ");
                serial_puts(driver_state_name(previous[i]));
                if (services[i].detail) { serial_puts("; "); serial_puts(services[i].detail); }
                serial_puts("\n");
            }
            driver_service_release(&services[i]);
        }
        if (sched_sleep_ticks(10) < 0) sched_yield();
    }
}

void driver_services_start(void)
{
    if (worker_started || (osfs3_is_mounted() && osfs3_find_ci(".nodrivers"))) return;
    worker_started = sched_spawn("drivers", driver_worker) >= 0;
    if (!worker_started) serial_puts("[DRIVER] cannot create worker; manual commands remain available\n");
}

int driver_wlan_claim(void) { return driver_service_claim(&services[WLAN]); }
void driver_wlan_release(int pause)
{
    if (pause >= 0) { services[WLAN].paused = pause != 0; services[WLAN].next_ms = 0; }
    driver_service_release(&services[WLAN]);
}
int driver_ipv4_claim(void) { return driver_service_claim(&services[IPV4]); }
void driver_ipv4_release(void) { driver_service_release(&services[IPV4]); }

int driver_wlan_remember(const void *network, const unsigned char *pmk)
{
    const iwx_bss_t *b = network;
    unsigned group = group_cipher(b);
    if (group == 255 || (!!pmk != b->rsn) || profiles_load()) return -1;
    wlan_profile_t profile = {.length = b->ssid_len, .secure = b->rsn, .group_cipher = group};
    memcpy(profile.ssid, b->ssid, b->ssid_len);
    if (pmk) memcpy(profile.pmk, pmk, 32);
    wlan_profiles_t next = wifi.profiles;
    int rc = wlan_profiles_remember(&next, &profile);
    if (!rc) rc = driver_config_write("wlan", &next, sizeof(next));
    if (!rc) { wifi.profiles = next; wifi.candidate = 0; }
    wpa2_clear(&profile, sizeof(profile)); wpa2_clear(&next, sizeof(next));
    return rc;
}

static void number(unsigned n)
{
    char text[12]; unsigned i = sizeof(text) - 1; text[i] = 0;
    do { text[--i] = '0' + n % 10; n /= 10; } while (n);
    sh_puts(text + i);
}

int driver_wlan_command(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "saved") && strcmp(argv[1], "forget") && strcmp(argv[1], "auto"))) return 0;
    if (profiles_load()) { sh_puts("wlan: cannot read saved profiles\n"); return 1; }
    if (argc == 2 && !strcmp(argv[1], "saved")) {
        sh_puts(wifi.profiles.enabled ? "Autoconnect: on\n" : "Autoconnect: off\n");
        for (unsigned i = 0; i < wifi.profiles.count; i++) {
            wlan_profile_t *p = &wifi.profiles.network[i];
            number(i + 1); sh_puts(". ");
            for (unsigned j = 0; j < p->length; j++) {
                char c[2] = {p->ssid[j] >= 32 && p->ssid[j] < 127 ? p->ssid[j] : '?', 0};
                sh_puts(c);
            }
            sh_puts(p->secure ? " (WPA2)\n" : " (trusted open)\n");
        }
        return 1;
    }
    wlan_profiles_t next = wifi.profiles;
    bool valid = false;
    if (argc == 3 && !strcmp(argv[1], "auto") && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        next.enabled = !strcmp(argv[2], "on"); valid = true;
    } else if (argc == 3 && !strcmp(argv[1], "forget")) {
        unsigned index = 0;
        for (const char *p = argv[2]; *p; p++) {
            if (*p < '0' || *p > '9' || index > 16) { index = 0; break; }
            index = index * 10 + (unsigned)(*p - '0');
        }
        if (index && index <= next.count) { wlan_profiles_forget(&next, index - 1); valid = true; }
    }
    if (!valid) sh_puts("wlan saved\nwlan forget <number>\nwlan auto on\nwlan auto off\n");
    else if (driver_config_write("wlan", &next, sizeof(next))) sh_puts("wlan: cannot save profiles\n");
    else { wifi.profiles = next; sh_puts("wlan: profile policy saved\n"); }
    wpa2_clear(&next, sizeof(next));
    return 1;
}

int driver_sshd_autostart(int enabled, unsigned port)
{
    if (!port || port > 65535 || !driver_service_claim(&services[SSH])) return -1;
    unsigned char config[4] = {1, enabled != 0, port >> 8, port};
    int rc = driver_config_write("sshd", config, sizeof(config));
    if (!rc) ssh_config_loaded = false;
    services[SSH].next_ms = 0;
    driver_service_release(&services[SSH]);
    return rc;
}

void driver_services_command(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) {
        sh_puts(worker_started ? "Driver worker: running\n" : "Driver worker: stopped (.nodrivers disables autostart)\n");
        sh_puts("Storage/input/display: boot-owned; not reset by background manager\n");
        for (unsigned i = 0; i < SERVICE_COUNT; i++) {
            driver_service_t *s = &services[i]; sh_puts(s->name); sh_puts(": ");
            if (!driver_service_claim(s)) { sh_puts("operation in progress\n"); continue; }
            sh_puts(driver_state_name(s->state));
            if (s->paused) sh_puts(" (paused)");
            if (s->detail) { sh_puts("; "); sh_puts(s->detail); }
            sh_puts("\n"); driver_service_release(s);
        }
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "retry")) {
        for (unsigned i = 0; i < SERVICE_COUNT; i++) if (!strcmp(argv[2], services[i].name)) {
            if (!driver_service_claim(&services[i])) { sh_puts("drivers: operation in progress\n"); return; }
            services[i].next_ms = services[i].paused = 0;
            if (i == WLAN) wifi.loaded = false;
            if (i == SSH) ssh_config_loaded = false;
            driver_service_release(&services[i]);
            driver_services_start(); sh_puts("drivers: retry scheduled\n"); return;
        }
    }
    if (argc == 3 && (!strcmp(argv[1], "hda") || !strcmp(argv[1], "ipv4"))) {
        unsigned id = !strcmp(argv[1], "hda") ? HDA : IPV4;
        int mode = -1;
        if (id == HDA) mode = !strcmp(argv[2], "on") ? 1 : !strcmp(argv[2], "off") ? 0 : -1;
        else mode = !strcmp(argv[2], "auto") ? 0 : !strcmp(argv[2], "dhcp") ? 1 : !strcmp(argv[2], "apipa") ? 2 : -1;
        if (mode >= 0 && driver_service_claim(&services[id])) {
            unsigned char config[2] = {1, (unsigned char)mode};
            int rc = driver_config_write(services[id].name, config, sizeof(config));
            driver_service_release(&services[id]);
            sh_puts(rc ? "drivers: cannot persist policy\n" : "drivers: policy saved for next initialization; current hardware/link unchanged\n"); return;
        }
    }
    sh_puts("drivers status\ndrivers retry <wlan/sshd/hda/ipv4>\ndrivers hda on\ndrivers hda off\ndrivers ipv4 auto\ndrivers ipv4 dhcp\ndrivers ipv4 apipa\n");
}
