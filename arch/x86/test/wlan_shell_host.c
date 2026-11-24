/* Exercise the real shell connection entry point, with radio/console mocks.
 * Cryptography, RF and DHCP implementations are covered by other suites. */
#include "../kernel/shell.c"
#include "../kernel/driver_manager.c"

extern int printf(const char *format, ...);
extern void *malloc(size_t size);
extern void free(void *ptr);
extern char *strstr(const char *haystack, const char *needle);
extern int snprintf(char *s, size_t n, const char *format, ...);

static unsigned checks, failures, allocations, connects, secrets, derives, dhcps, stops, clears;
static bool selected_nic, fail_alloc, authorized, fail_link, fail_dhcp;
static bool busy_runtime, busy_link, mutate_input, mutate_secret, snapshot_race;
static bool fail_input, fail_sleep, scan_busy_after_start;
static int current_pid, scan_return, secret_length, derive_result;
static unsigned scans, reads, script_at, scan_delay;
static uint64_t ticks, scan_until;
static const char *connect_error, *scan_error;
static const char *script[12];
static enum iwx_runtime_state runtime_state;
static enum iwx_link_state initial_link;
static enum iwx_scan_state completed_scan;
static iwx_scan_status_t scan_result;
static iwx_bss_t networks[IWX_SCAN_BSS_MAX], expected;
static wpa2_rx_diagnostic_t rejection;
static unsigned ingress_drops, pmkid_mismatches;
static char output[IWX_SCAN_BSS_MAX * 1024];
static size_t output_length;
static unsigned selftest_failures;
unsigned wpa2_selftest(void) { return selftest_failures; }
int driver_wlan_command(int argc, char **argv) { (void)argc; (void)argv; return 0; }

#define CHECK(test) do { checks++; if (!(test)) { \
    printf("FAIL line %d: %s\n", __LINE__, #test); failures++; \
} } while (0)

void serial_puts(const char *s)
{
    size_t length = strlen(s);
    CHECK(length < sizeof(output) - output_length);
    if (length >= sizeof(output) - output_length) return;
    memcpy(output + output_length, s, length + 1); output_length += length;
}
void fb_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n)
{
    char text[24]; snprintf(text, sizeof(text), "%llu", (unsigned long long)n); serial_puts(text);
}
void fb_putdec(uint64_t n) { (void)n; }
void *kmalloc(uint64_t size)
{
    if (fail_alloc) return NULL;
    void *p = malloc(size);
    if (p) { allocations++; memset(p, 0xa5, size); }
    return p;
}
void kfree(void *p) { if (p) { CHECK(allocations > 0); allocations--; free(p); } }
bool nic_is_iwx(void) { return selected_nic; }
int sched_current_get(void) { return current_pid; }
uint64_t idt_get_ticks(void) { return ticks; }
int sched_sleep_ticks(uint64_t n) { if (fail_sleep) return -1; ticks += n; return 0; }
void sched_yield(void) { ticks++; }
void net_poll(void) { }
bool net_link_ready(void) { return authorized; }
int dhcp_discover(void) { CHECK(authorized); dhcps++; return fail_dhcp ? -1 : 0; }
int iwx_stop(iwx_probe_report_t *report) { stops++; memset(report, 0, sizeof(*report)); return 0; }
int iwx_get_status(iwx_status_t *status)
{
    memset(status, 0, sizeof(*status)); status->state = runtime_state; return busy_runtime ? -1 : 0;
}
int iwx_link_status(iwx_link_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->state = !connects ? initial_link : fail_link ? IWX_LINK_FAILED : IWX_LINK_ASSOCIATED;
    status->port_open = connects && authorized;
    status->eapol_last_drop = rejection;
    status->eapol_ingress_dropped = ingress_drops;
    status->pmkid_mismatches = pmkid_mismatches;
    if (connects) {
        status->queue_response = true; status->queue_bytes = 8;
        status->queue_id = 511; status->queue_flags = connect_error ? 256 : 0;
        status->queue_write_pointer = 255;
    }
    return busy_link ? -1 : 0;
}
int iwx_scan_results(iwx_scan_status_t *status, iwx_bss_t *bss, unsigned capacity)
{
    CHECK(capacity == IWX_SCAN_BSS_MAX || (!capacity && !bss));
    if (scan_return < 0 || (scans && scan_busy_after_start)) return -1;
    if ((scan_result.state == IWX_SCAN_RUNNING || scan_result.state == IWX_SCAN_ABORTING) &&
        ticks >= scan_until) scan_result.state = completed_scan;
    *status = scan_result;
    if (capacity && snapshot_race) { snapshot_race = false; status->state = IWX_SCAN_RUNNING; }
    unsigned count = (unsigned)scan_return < capacity ? (unsigned)scan_return : capacity;
    if (count) memcpy(bss, networks, count * sizeof(*bss));
    return count;
}
const char *iwx_scan_start(void)
{
    scans++;
    if (!scan_error) {
        scan_result.state = IWX_SCAN_RUNNING; scan_until = ticks + scan_delay;
        scan_result.generation++;
    }
    return scan_error;
}
int term_readline_alloc(const char *prompt, char **out, uint64_t *length)
{
    CHECK(prompt && !strcmp(prompt, "Wi-Fi> ") && script_at < 12);
    CHECK(!sh_stdin_buf && !sh_redir_fn);
    reads++;
    *out = NULL; *length = 0;
    if (fail_input) return -2;
    const char *text = script[script_at++];
    if (!text) return -1;
    *length = strlen(text); *out = kmalloc(*length + 1);
    CHECK(*out != NULL); memcpy(*out, text, *length + 1);
    if (mutate_input) networks[0].ssid[0] = 'z';
    return 0;
}
int term_read_secret(const char *prompt, char *password, unsigned capacity, bool echo)
{
    CHECK(prompt && capacity == 64 && !sh_stdin_buf && !sh_redir_fn);
    CHECK(!echo && !strcmp(prompt, "WPA2 password (not echoed): "));
    secrets++; memset(password, 0, capacity);
    if (secret_length > 0) memset(password, 'x', secret_length);
    if (mutate_secret) networks[0].ssid[0] = 'z';
    return secret_length;
}
int wpa2_pmk(const uint8_t *password, size_t length, const uint8_t *ssid,
             size_t ssid_length, uint8_t pmk[32])
{
    derives++;
    CHECK(length == (size_t)secret_length && password[0] == 'x');
    CHECK(ssid_length == expected.ssid_len && !memcmp(ssid, expected.ssid, ssid_length));
    memset(pmk, 0x5a, 32);
    return derive_result;
}
void wpa2_clear(void *p, size_t length) { clears++; memset(p, 0, length); }
int driver_wlan_remember(const void *bss, const unsigned char *pmk)
{
    CHECK(authorized && !fail_link && !connect_error);
    const iwx_bss_t *network = bss;
    CHECK(network->ssid_len == expected.ssid_len && !memcmp(network->ssid, expected.ssid, expected.ssid_len));
    CHECK(!!pmk == network->rsn);
    if (pmk) for (unsigned i = 0; i < 32; i++) CHECK(pmk[i] == 0x5a);
    return 0;
}
const char *iwx_connect_network(const iwx_bss_t *bss, const uint8_t *pmk)
{
    connects++;
    CHECK(!allocations && !memcmp(bss, &expected, sizeof(*bss)));
    CHECK((pmk != NULL) == expected.rsn);
    if (pmk) for (unsigned i = 0; i < 32; i++) CHECK(pmk[i] == 0x5a);
    return connect_error;
}
static void redirect(const char *s, size_t length) { (void)length; serial_puts(s); }

static void fixture(unsigned count, unsigned index, bool secure)
{
    CHECK(!allocations && index < IWX_SCAN_BSS_MAX && count <= IWX_SCAN_BSS_MAX);
    connects = secrets = derives = dhcps = stops = clears = 0;
    selftest_failures = 0;
    memset(&rejection, 0, sizeof(rejection)); ingress_drops = pmkid_mismatches = 0;
    selected_nic = authorized = true; fail_alloc = fail_link = fail_dhcp = false;
    busy_runtime = busy_link = mutate_input = mutate_secret = snapshot_race = false;
    fail_input = fail_sleep = scan_busy_after_start = false;
    runtime_state = IWX_ACTIVE; initial_link = IWX_LINK_IDLE;
    scans = reads = script_at = 0; scan_until = 3000; scan_delay = 3;
    completed_scan = IWX_SCAN_COMPLETE; scan_error = NULL;
    memset(script, 0, sizeof(script));
    current_pid = shell_console_owner = 7;
    scan_return = count; secret_length = 12; derive_result = 0; ticks = 0;
    sh_stdin_buf = NULL; sh_redir_fn = NULL; connect_error = NULL;
    memset(output, 0, sizeof(output)); output_length = 0;
    memset(networks, 0, sizeof(networks));
    scan_result = (iwx_scan_status_t){.state = IWX_SCAN_COMPLETE, .count = count};
    for (unsigned i = 0; i < count; i++) {
        networks[i].bssid[0] = 2; networks[i].bssid[5] = i;
        networks[i].ssid[0] = 't'; networks[i].ssid_len = 1;
        networks[i].channel = 1; networks[i].rsn = secure;
        networks[i].capability = secure ? 17 : 1;
        networks[i].rsn_len = 20; memcpy(networks[i].rsn_ie, wpa2_assoc_rsn + 2, 20);
    }
    expected = networks[index];
}

static void test_security_report(void)
{
    static const char *reasons[] = {
        "WPA2-PSK/CCMP", "WPA2-PSK/CCMP (TKIP group)",
        "CCMP pairwise cipher is not offered", "WPA2-PSK is not offered",
        "WPA2-PSK is not offered", "required protected management frames are not implemented",
        "truncated RSN capabilities", "legacy WPA/WEP is not supported",
        "hidden SSID is not supported", "inconsistent RSN/privacy advertisement"
    };
    for (unsigned i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
        fixture(1, 0, true);
        switch (i) {
        case 1: networks[0].rsn_ie[5] = 2; break;
        case 2: networks[0].rsn_ie[11] = 2; break;
        case 3: networks[0].rsn_ie[17] = 1; break;
        case 4: networks[0].rsn_ie[17] = 8; break;
        case 5: networks[0].rsn_ie[18] = 64; break;
        case 6: networks[0].rsn_len = 19; break;
        case 7: networks[0].rsn = false; networks[0].wpa = true; break;
        case 8: networks[0].ssid_len = 0; break;
        case 9: networks[0].capability &= ~16; break;
        }
        initial_link = IWX_LINK_ASSOCIATED; current_pid++; sh_redir_fn = redirect;
        wlan_scan_report(true);
        CHECK(strstr(output, reasons[i]) && strstr(output, "  Capability: 0x"));
        CHECK((strstr(output, "RSN IE payload (") != NULL) == networks[0].rsn);
        if (!i) CHECK(strstr(output, "0100000fac040100000fac040100000f\n    ac020000\n"));
        if (i == 4) CHECK(strstr(output, "ac080000\n"));
        output_length = 0; output[0] = 0;
        wlan_network_print(&networks[0]);
        CHECK(strstr(output, reasons[i]));
        CHECK((strstr(output, "unavailable:") != NULL) == (i > 1));
        CHECK(!connects && !secrets && !derives && !dhcps && !stops && !scans && !allocations);
    }
    for (unsigned length = 0; length <= 255; length++) {
        fixture(1, 0, true);
        networks[0].rsn_len = length;
        memset(networks[0].rsn_ie, 0x1b, sizeof(networks[0].rsn_ie));
        wlan_scan_report(true);
        CHECK(!strstr(output, "\033") && !allocations);
        char label[40]; snprintf(label, sizeof(label), "RSN IE payload (%u bytes):\n", length);
        CHECK(strstr(output, label));
        CHECK(!connects && !secrets && !stops && !scans);
    }
    fixture(IWX_SCAN_BSS_MAX, 0, true);
    for (unsigned i = 0; i < IWX_SCAN_BSS_MAX; i++) {
        networks[i].rsn_len = 255; memset(networks[i].rsn_ie, 0xa5, 255);
        networks[i].ssid_len = 32; memset(networks[i].ssid, 27, 32);
    }
    wlan_scan_report(true);
    CHECK(!allocations && output_length < sizeof(output) && !strstr(output, "\033"));
    fixture(1, 0, true); wlan_scan_report(false);
    CHECK(!strstr(output, "Connect:") && !strstr(output, "RSN IE payload") && !allocations);
    fixture(1, 0, false); wlan_scan_report(true);
    CHECK(strstr(output, "  Connect: OPEN\n") && !strstr(output, "RSN IE payload"));
    fixture(1, 0, true); fail_alloc = true; wlan_scan_report(true);
    CHECK(strstr(output, "no memory") && !allocations);
    fixture(1, 0, true); scan_return = -1; wlan_scan_report(true);
    CHECK(strstr(output, "WLAN scan: busy") && !allocations);
    fixture(1, 0, true);
}

static unsigned automatic_steps;
static enum driver_state automatic_step(driver_service_t *service)
{
    (void)service;
    automatic_steps++;
    return DRIVER_READY;
}

static void manual_done(driver_service_t *service, int argc, char **argv)
{
    CHECK(driver_service_claim(service));
    int pause = wlan_manual_pause(argc, argv);
    if (pause >= 0) { service->paused = pause != 0; service->next_ms = 0; }
    driver_service_release(service);
    driver_manager_tick(service, 1, 10000);
}

static void test_manual_start_ownership(void)
{
    char *stop[] = {"wlan", "stop"};
    char *start[] = {"wlan", "start", "iwlwifi-cc-a0-77.ucode"};
    char *trace[] = {"wlan", "starttrace", "iwlwifi-cc-a0-77.ucode"};
    char *status[] = {"wlan", "status"};
    char *scan[] = {"wlan", "scan"};
    char *connect[] = {"wlan", "connect", "wpa2", "02:00:00:00:00:01"};
    char *resume[] = {"wlan", "auto", "on"};
    for (unsigned tracing = 0; tracing < 2; tracing++) {
        for (unsigned paused = 0; paused < 2; paused++) {
            driver_service_t service = {.step = automatic_step, .paused = paused};
            automatic_steps = 0;
            manual_done(&service, 3, tracing ? trace : start);
            CHECK(service.paused && !automatic_steps);
            manual_done(&service, 2, status);
            manual_done(&service, 2, scan);
            CHECK(service.paused && !automatic_steps);
            manual_done(&service, 2, connect);
            CHECK(!service.paused && automatic_steps == 1);
            manual_done(&service, 2, stop);
            manual_done(&service, 3, tracing ? trace : start);
            CHECK(service.paused && automatic_steps == 1);
            manual_done(&service, 4, connect);
            CHECK(!service.paused && automatic_steps == 2);
            manual_done(&service, 2, stop);
            manual_done(&service, 3, resume);
            CHECK(!service.paused && automatic_steps == 3);
        }
    }
    CHECK(wlan_manual_pause(1, stop) == -1);
    CHECK(wlan_manual_pause(2, start) == -1);
    CHECK(wlan_manual_pause(2, trace) == -1);
}

int main(void)
{
    test_manual_start_ownership();
    test_security_report();
    for (unsigned secure = 0; secure < 2; secure++) {
        for (unsigned count = 1; count <= IWX_SCAN_BSS_MAX; count++) {
            fixture(count, count - 1, secure != 0);
            wlan_connect_command(expected.bssid, secure != 0);
            CHECK(connects == 1 && dhcps == 1 && !stops && !allocations);
            CHECK(secrets == secure && derives == secure && clears == 1 + secure);
            CHECK(strstr(output, "IPv4 configured") != NULL);
            CHECK(strstr(output, "xxxxxxxxxxxx") == NULL);
        }
    }
    /* Only the returned entries are initialized, even in a truncated snapshot. */
    fixture(1, 0, false); scan_result.count = IWX_SCAN_BSS_MAX + 1;
    wlan_connect_command(expected.bssid, false);
    CHECK(connects == 1 && dhcps == 1 && !allocations);
    for (unsigned bad = 0; bad < 16; bad++) {
        fixture(1, 0, true);
        switch (bad) {
        case 0: scan_return = -1; break;
        case 1: scan_return = scan_result.count = 0; break;
        case 2: scan_result.state = IWX_SCAN_RUNNING; break;
        case 3: networks[0].bssid[5]++; break;
        case 4: networks[1] = networks[0]; scan_return = scan_result.count = 2; break;
        case 5: selected_nic = false; break;
        case 6: shell_console_owner = -1; break;
        case 7: current_pid++; break;
        case 8: sh_stdin_buf = "untrusted input"; break;
        case 9: sh_redir_fn = redirect; break;
        case 10: fail_alloc = true; break;
        case 11: networks[0].rsn = false; break;
        case 12: networks[0].ssid_len = 0; break;
        case 13: secret_length = -1; break;
        case 14: derive_result = -1; break;
        case 15: expected.rsn = networks[0].rsn = false; break;
        }
        wlan_connect_command(expected.bssid, expected.rsn);
        CHECK(!connects && !dhcps && !stops && !allocations);
        CHECK(secrets == (unsigned)(bad == 13 || bad == 14));
        CHECK(clears == (unsigned)((bad == 13 || bad == 14) ? 2 : 0));
    }
    fixture(1, 0, true); connect_error = "changed scan identity";
    wlan_connect_command(expected.bssid, true);
    CHECK(connects == 1 && !dhcps && !stops && clears == 2 && !allocations);
    CHECK(strstr(output, "TX queue response bytes/id/flags/write/reserved: 8/511/256/255/0") != NULL);
    CHECK(strstr(output, "xxxxxxxxxxxx") == NULL);
    fixture(1, 0, true); authorized = false; fail_link = true; ingress_drops = 2;
    pmkid_mismatches = 3;
    rejection = (wpa2_rx_diagnostic_t){.reason = "invalid M1 key data", .state = WPA2_WAIT_M1,
        .bytes = 121, .body_bytes = 117, .key_info = 0x8a, .key_bytes = 16,
        .data_bytes = 22, .version = 2, .packet_type = 3, .descriptor = 2};
    wlan_connect_command(expected.bssid, true);
    CHECK(connects == 1 && !dhcps && stops == 1 && !allocations);
    CHECK(strstr(output, "WPA2 last rejection: invalid M1 key data\n") != NULL);
    CHECK(strstr(output, "WPA2 M1 PMKID mismatches (advisory): 3\n") != NULL);
    CHECK(strstr(output, "WPA2 RX state: wait M1\n") != NULL);
    CHECK(strstr(output, "WPA2 RX bytes/body/version/type/descriptor: 121/117/2/3/2\n") != NULL);
    CHECK(strstr(output, "WPA2 RX key info: 0x008A\n") != NULL);
    CHECK(strstr(output, "WPA2 RX key/data bytes: 16/22\n") != NULL);
    CHECK(strstr(output, "EAPOL RX queue/size drops: 2\n") != NULL);
    CHECK(strstr(output, "xxxxxxxxxxxx") == NULL);
    output[0] = 0; output_length = 0; sh_redir_fn = redirect;
    wlan_link_report();
    CHECK(strstr(output, "WPA2 last rejection: invalid M1 key data\n") != NULL);
    CHECK(strstr(output, "WPA2 M1 PMKID mismatches (advisory): 3\n") != NULL);
    rejection.state = (enum wpa2_state)100;
    wlan_link_report();
    CHECK(strstr(output, "WPA2 RX state: unknown\n") != NULL);
    fixture(1, 0, false); authorized = false; fail_link = true;
    wlan_connect_command(expected.bssid, false);
    CHECK(connects == 1 && !dhcps && stops == 1 && !allocations);
    fixture(1, 0, false); authorized = false;
    wlan_connect_command(expected.bssid, false);
    CHECK(connects == 1 && !dhcps && stops == 1 && ticks == 3000 && !allocations);
    fixture(1, 0, false); fail_dhcp = true;
    wlan_connect_command(expected.bssid, false);
    CHECK(connects == 1 && dhcps == 1 && !stops && !allocations);
    CHECK(strstr(output, "DHCP failed") != NULL);

    /* Both entry paths guard console, transport, link and security before secrets. */
    for (unsigned assistant = 0; assistant < 2; assistant++) {
        for (unsigned bad = 0; bad < 17; bad++) {
            fixture(1, 0, true); script[0] = "1"; script[1] = "q";
            switch (bad) {
            case 0: selected_nic = false; break;
            case 1: shell_console_owner = -1; break;
            case 2: current_pid++; break;
            case 3: sh_stdin_buf = "1\nsecret\n"; break;
            case 4: sh_redir_fn = redirect; break;
            case 5: runtime_state = IWX_OFF; break;
            case 6: runtime_state = IWX_FAILED; break;
            case 7: runtime_state = IWX_QUARANTINED; break;
            case 8: busy_runtime = true; break;
            case 9: busy_link = true; break;
            case 10: initial_link = IWX_LINK_ASSOCIATED; break;
            case 11: initial_link = IWX_LINK_FAILED; break;
            case 12: networks[0].ssid_len = 0; break;
            case 13: networks[0].capability = 2; break;
            case 14: networks[0].rsn_ie[17] = 1; break; /* Enterprise AKM. */
            case 15: networks[0].rsn_ie[18] = 64; break; /* Required PMF. */
            case 16: networks[0].rsn_len = 2; break;
            }
            if (assistant) wlan_connect_assistant();
            else wlan_connect_command(expected.bssid, true);
            CHECK(!connects && !dhcps && !secrets && !scans && !stops && !allocations);
            CHECK(reads == (unsigned)(assistant && bad >= 12 ? 2 : 0));
        }
    }
    for (unsigned secure = 0; secure < 2; secure++) {
        for (unsigned count = 1; count <= IWX_SCAN_BSS_MAX; count++) {
            fixture(count, count - 1, secure != 0);
            char choice[4]; snprintf(choice, sizeof(choice), "%u", count); script[0] = choice;
            wlan_connect_assistant();
            CHECK(connects == 1 && dhcps == 1 && !stops && !scans && !allocations && reads == 1);
            CHECK(secrets == secure && derives == secure && clears == 1 + secure);
            CHECK(strstr(output, "xxxxxxxxxxxx") == NULL);
        }
    }
    fixture(7, 6, true);
    script[0] = "p"; script[1] = "n"; script[2] = "n"; script[3] = "p"; script[4] = "7";
    wlan_connect_assistant();
    CHECK(connects == 1 && reads == 5 && !allocations);
    CHECK(strstr(output, "page 2/2") && strstr(output, "7. \"t\""));
    fixture(2, 1, false); networks[0].capability = 17; script[0] = "1"; script[1] = "2";
    wlan_connect_assistant();
    CHECK(connects == 1 && !secrets && !allocations);
    CHECK(strstr(output, "legacy WPA/WEP") != NULL);
    for (unsigned mode = 0; mode < 7; mode++) {
        fixture(1, 0, true); script[0] = "1";
        switch (mode) {
        case 0: scan_result.state = IWX_SCAN_IDLE; break;
        case 1: scan_result.state = IWX_SCAN_RUNNING; break;
        case 2: scan_result.state = IWX_SCAN_ABORTING; break;
        case 3: snapshot_race = true; break;
        case 4: script[0] = "r"; script[1] = "1"; break;
        case 5: fail_sleep = true; scan_result.state = IWX_SCAN_IDLE; break;
        case 6: scan_result.count = IWX_SCAN_BSS_MAX + 1; break;
        }
        wlan_connect_assistant();
        CHECK(connects == 1 && dhcps == 1 && !allocations);
        CHECK(scans == (unsigned)(mode == 0 || mode == 4 || mode == 5));
    }
    for (unsigned bad = 0; bad < 10; bad++) {
        fixture(1, 0, true); script[0] = "1";
        scan_result.state = IWX_SCAN_IDLE;
        switch (bad) {
        case 0: fail_alloc = true; break;
        case 1: scan_return = -1; break;
        case 2: scan_error = "radio blocked"; break;
        case 3: completed_scan = IWX_SCAN_FAILED; scan_result.error = "firmware error"; break;
        case 4: completed_scan = IWX_SCAN_ABORTED; break;
        case 5: scan_delay = 4000; break;
        case 6: scan_busy_after_start = true; break;
        case 7: fail_input = true; break;
        case 8: script[0] = NULL; break;
        case 9: script[0] = ""; break;
        }
        wlan_connect_assistant();
        CHECK(!connects && !dhcps && !secrets && !stops && !allocations);
        if (bad == 5 || bad == 6) CHECK(ticks == 3500 && strstr(output, "wait timed out"));
    }
    fixture(0, 0, false); script[0] = "n"; script[1] = "r"; script[2] = "q";
    wlan_connect_assistant();
    CHECK(!connects && !allocations && scans == 1 && reads == 3);
    CHECK(strstr(output, "No networks found") != NULL);
    const char *invalid[] = {"0", "2", "999", "-1", "+1", "1garbage", "1 ", " 1", "1\t", "0001"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        fixture(1, 0, true); script[0] = invalid[i]; script[1] = "q";
        wlan_connect_assistant();
        CHECK(!connects && !secrets && !allocations && reads == 2);
        CHECK(strstr(output, "invalid selection") != NULL);
    }
    fixture(1, 0, true);
    char pasted[8192]; memset(pasted, '1', sizeof(pasted)); pasted[sizeof(pasted) - 1] = 0;
    script[0] = pasted; script[1] = "q";
    wlan_connect_assistant();
    CHECK(!connects && !secrets && !allocations && reads == 2);
    /* The menu and PMK use the displayed snapshot, not a later cache entry. */
    for (unsigned where = 0; where < 2; where++) {
        fixture(1, 0, true); script[0] = "1";
        mutate_input = where == 0; mutate_secret = where == 1;
        connect_error = "changed scan identity";
        wlan_connect_assistant();
        CHECK(connects == 1 && !dhcps && clears == 2 && !allocations);
    }
    fixture(1, 0, false);
    const uint8_t evil[] = {'a', 27, '[', '2', 'J', '"', '\\', 0};
    memcpy(networks[0].ssid, evil, sizeof(evil)); networks[0].ssid_len = sizeof(evil);
    expected = networks[0]; script[0] = "1";
    wlan_connect_assistant();
    CHECK(connects == 1 && !allocations && !strstr(output, "\033[2J"));
    CHECK(strstr(output, "a\\x1b[2J\\x22\\x5c\\x00") != NULL);
    char *args[] = {"wlan", "connect", "wpa2", "02:00:00:00:00:00", "inline-password"};
    fixture(1, 0, true); cmd_wlan_connect(4, args);
    CHECK(connects == 1 && secrets == 1 && !allocations);
    fixture(1, 0, true); script[0] = "1"; cmd_wlan_connect(2, args);
    CHECK(connects == 1 && secrets == 1 && reads == 1 && !allocations);
    for (unsigned argc = 3; argc <= 5; argc += 2) {
        fixture(1, 0, true); cmd_wlan_connect(argc, args);
        CHECK(!connects && !secrets && !reads && !allocations && strstr(output, "Usage:"));
        CHECK(strstr(output, "wlan connect\n") && strstr(output, "wlan connect wpa2 <BSSID>\n"));
        CHECK(strstr(output, "wlan connect open <BSSID>\n") && !strstr(output, "|"));
    }
    fixture(1, 0, false); args[2] = "open"; cmd_wlan_connect(4, args);
    CHECK(connects == 1 && !secrets && !allocations);
    fixture(1, 0, true); args[2] = "open|wpa2"; cmd_wlan_connect(4, args);
    CHECK(!connects && !secrets && !allocations && strstr(output, "Usage:"));
    args[2] = "wpa2";
    const char *bad_mac[] = {"", "00:00", "02:00:00:00:00:0g", "020000000000", "02:00:00:00:00:00x"};
    for (unsigned i = 0; i < sizeof(bad_mac) / sizeof(bad_mac[0]); i++) {
        fixture(1, 0, true); args[3] = (char *)bad_mac[i]; cmd_wlan_connect(4, args);
        CHECK(!connects && !secrets && !reads && !allocations);
    }
    for (unsigned fault = 0; fault <= WPA2_TEST_COUNT; fault++) {
        fixture(1, 0, true);
        selftest_failures = fault ? 1U << (fault - 1) : 0;
        sh_redir_fn = redirect;
        wlan_selftest_report();
        CHECK(strstr(output, fault ? "WPA2 selftest: FAIL" : "WPA2 selftest: PASS (6/6)"));
        CHECK(!connects && !secrets && !derives && !dhcps && !stops && !allocations);
        sh_redir_fn = NULL;
    }
    fixture(1, 0, true);
    iwx_link_status_t diagnostic = {0};
    diagnostic.rx_reasons[IWX_RX_FORMAT] = 2;
    diagnostic.rx_trace_count = diagnostic.rx_trace_next = 1;
    diagnostic.rx_trace[0] = (iwx_rx_sample_t){.reason = IWX_RX_FORMAT, .key_info = 0x13ca,
        .frame_control = 0x288, .ether_type = 0x888e, .addresses = 7};
    diagnostic.eapol_tx_count = diagnostic.eapol_tx_next = 1;
    diagnostic.eapol_tx[0] = (iwx_tx_sample_t){.kind = 1, .dma_match = true, .key_info = 0x10a};
    diagnostic.data_trace_count = 1;
    diagnostic.data_trace[0] = (iwx_rx_sample_t){.reason = IWX_RX_REPLAY,
        .pn = 91, .previous_pn = 92, .key_slot = 2, .header_bytes = 34,
        .udp_source = 67, .udp_destination = 68, .dhcp_xid = 0xa1b2c3d4};
    wlan_link_diagnostics(&diagnostic);
    CHECK(strstr(output, "format=2") && strstr(output, "kind=M2") && strstr(output, "DMA-copy=match"));
    CHECK(strstr(output, "Completion: pending") && strstr(output, "Key info candidate: 0x13CA"));
    CHECK(strstr(output, "Key slot/PN/previous=2/91/92") &&
          strstr(output, "Header bytes/UDP source/destination=34/67/68"));
    CHECK(strstr(output, "DHCP xid candidate: 0xA1B2C3D4") && strstr(output, "WLAN RX reasons:"));
    diagnostic.eapol_tx[0].completed = true; diagnostic.eapol_tx[0].status = 2;
    wlan_link_diagnostics(&diagnostic);
    CHECK(strstr(output, "Firmware TX status (not WPA acceptance): 0x0002"));
    printf("WLAN shell connection: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
