#include "../drivers/iwx_link.c"
#include "../drivers/iwx_scan.c"
#include "../drivers/iwx_fw.c"

extern int printf(const char *format, ...);
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int close(int fd);
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static iwx_radio_profile_t profile;
static iwx_nvm_report_t nvm = {.valid = true, .mac = {2,0,0,0,0,1}, .tx_chains = 3, .rx_chains = 3};
static iwx_regulatory_t regulatory = {.valid = true, .count = 2, .channel = {{1,9},{36,9}}};
static const uint8_t rsn[] = {1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
static iwx_bss_t ap;
static uint8_t packet[4096], page[4096], image[IWX_FW_MAX_SIZE];

static void fixture(void)
{
    memset(&profile, 0, sizeof(profile));
    static const uint8_t versions[] = {4,2,5,2,12,9,3,1,99};
    for (unsigned i = 0; i < sizeof(versions); i++) {
        profile.command[IWX_ABI_PHY_CTX + i] = versions[i];
        profile.seen |= 1U << (IWX_ABI_PHY_CTX + i);
    }
    profile.notification[IWX_ABI_TX] = 7;
    profile.notification[IWX_ABI_TX_QUEUE] = 2;
    profile.notification[IWX_ABI_SESSION_NOTIF] = 2;
    profile.capa[1] = (1U << 16) | (1U << 22);
    profile.phy_config = 0x00330018;
    ap = (iwx_bss_t){.bssid = {2,1,2,3,4,5}, .ssid = {'t','e','s','t'}, .ssid_len = 4,
        .channel = 1, .beacon_interval = 100, .capability = 0x421,
        .rates = {0x82,0x84,0x8b,0x96,12,18,24,36,48,72,96,108}, .rates_len = 12,
        .dtim_count = 1, .dtim_period = 2, .system_time = 0xfffff000, .tsf = 0x123456789aULL};
}

static void test_selection(void)
{
    fixture();
    iwx_link_t l = {0};
    CHECK(!iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, false));
    CHECK(l.status.state == IWX_LINK_PREPARING && !l.status.port_open && l.nrates == 12);
    CHECK(l.rate_flags == 0x4000 && l.cck == 15 && !memcmp(l.local, nvm.mac, 6));
    for (unsigned id = IWX_ABI_PHY_CTX; id <= IWX_ABI_SESSION_NOTIF; id++) {
        iwx_radio_profile_t p = profile;
        p.seen &= ~(1U << id); CHECK(iwx_link_profile_check(&p));
        p = profile; p.command[id]++; CHECK(iwx_link_profile_check(&p));
    }
    unsigned notif[] = {IWX_ABI_TX, IWX_ABI_TX_QUEUE, IWX_ABI_SESSION_NOTIF};
    for (unsigned i = 0; i < 3; i++) {
        iwx_radio_profile_t p = profile;
        p.notification[notif[i]]++; CHECK(iwx_link_profile_check(&p));
    }
    for (unsigned i = 0; i < 3; i++) {
        iwx_radio_profile_t p = profile;
        p.capa[1] ^= 1U << (i == 0 ? 16 : i == 1 ? 8 : 22);
        CHECK(iwx_link_profile_check(&p));
    }
    iwx_link_t saved = l;
    for (unsigned bad = 0; bad < 17; bad++) {
        iwx_bss_t b = ap;
        switch (bad) {
        case 0: b.channel = 2; break;
        case 1: b.bssid[0] |= 1; break;
        case 2: memset(b.bssid, 0, 6); break;
        case 3: memcpy(b.bssid, nvm.mac, 6); break;
        case 4: b.capability |= 2; break;
        case 5: b.capability &= ~1; break;
        case 6: b.beacon_interval = 0; break;
        case 7: b.ssid_len = 0; break;
        case 8: b.ssid_len = 33; break;
        case 9: b.rates_len = 0; break;
        case 10: b.rates_len = 17; break;
        case 11: b.rates[0] = 0xff; break;
        case 12: b.rates[0] = b.rates[1]; break;
        case 13: for (unsigned i = 0; i < b.rates_len; i++) b.rates[i] &= 127; break;
        case 14: b.rsn = true; break;
        case 15: b.wpa = true; break;
        case 16: b.capability |= 16; break;
        }
        CHECK(iwx_link_select(&l, &b, &nvm, &profile, &regulatory, false));
        CHECK(!memcmp(&l, &saved, sizeof(l)));
    }
    CHECK(iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, true));
    ap.rsn = true; ap.capability |= 16;
    ap.rsn_len = sizeof(rsn); memcpy(ap.rsn_ie, rsn, sizeof(rsn));
    CHECK(!iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, true));
    CHECK(l.status.rsn && !l.status.port_open);
    ap.channel = 36;
    ap.rates_len = 8;
    static const uint8_t ofdm[] = {0x8c,18,0x98,36,0xb0,72,96,108};
    memcpy(ap.rates, ofdm, 8);
    CHECK(!iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, true));
    CHECK(l.rate_flags == 0x4100 && l.cck == 0 && l.ofdm == 0x15);
    fixture();
    ap.rates[11] |= 128;
    CHECK(!iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, false));
    CHECK(l.cck == 15 && l.ofdm == 0x95); /* Per-modulation mandatory ACK rates. */
    ap.erp = 6;
    CHECK(!iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, false));
    iwx_link_mac_t mac;
    iwx_link_mac_build(&mac, &l, false);
    CHECK(mac.protection == 8 && !mac.preamble && mac.ac[0].cw_min == 15);
    ap.rates_len = 4;
    CHECK(!iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, false));
    iwx_link_mac_build(&mac, &l, false);
    CHECK(mac.ac[0].cw_min == 31 && !l.ofdm_supported);
    for (unsigned bits = 0; bits < 32; bits++) {
        iwx_regulatory_t r = regulatory; r.channel[0].flags = bits;
        CHECK(iwx_link_tx_allowed(&r, 1) == ((bits & 9) == 9));
    }
    iwx_regulatory_t r = regulatory; r.valid = false;
    CHECK(!iwx_link_tx_allowed(&r, 1)); r.valid = true; r.count = 52;
    CHECK(!iwx_link_tx_allowed(&r, 1));
}

static void test_rsn(void)
{
    CHECK(!iwx_link_rsn_check(rsn, sizeof(rsn)));
    for (unsigned n = 0; n < sizeof(rsn); n++) CHECK((iwx_link_rsn_check(rsn, n) == NULL) == (n == 18));
    uint8_t x[255] = {0};
    for (unsigned k = 0; k < 6; k++) {
        memcpy(x, rsn, sizeof(rsn));
        if (k == 0) x[0] = 2;
        if (k == 1) x[5] = 1;
        if (k == 2) x[6] = 255;
        if (k == 3) x[11] = 8;
        if (k == 4) x[17] = 1;
        if (k == 5) x[18] = 64;
        CHECK(iwx_link_rsn_check(x, sizeof(rsn)));
    }
    memcpy(x, rsn, sizeof(rsn)); x[18] = 128; /* Optional MFP may be declined. */
    CHECK(!iwx_link_rsn_check(x, 20));
    x[20] = 1;
    CHECK(iwx_link_rsn_check(x, 21)); CHECK(iwx_link_rsn_check(x, 22));
    CHECK(!iwx_link_rsn_check(x, 38));
    CHECK(iwx_link_rsn_check(x, 39)); CHECK(!iwx_link_rsn_check(x, 42));
    CHECK(iwx_link_rsn_check(NULL, 20));
    for (unsigned group = 0; group < 256; group++) {
        memcpy(x, rsn, sizeof(rsn)); x[5] = group;
        enum wpa2_cipher selected = WPA2_CIPHER_CCMP;
        uint8_t assoc[22]; memset(assoc, 0xa5, sizeof(assoc));
        const char *error = wpa2_rsn_select(x, sizeof(rsn), &selected);
        bool valid = group == 2 || group == 4;
        CHECK((error == NULL) == valid && selected == (valid ? (enum wpa2_cipher)group : WPA2_CIPHER_NONE));
        CHECK((wpa2_rsn_assoc(assoc, x, sizeof(rsn)) == 0) == valid);
        if (valid) CHECK(assoc[7] == group && assoc[13] == 4 && assoc[19] == 2);
        else for (unsigned i = 0; i < sizeof(assoc); i++) CHECK(assoc[i] == 0xa5);
    }
}

static unsigned response(const iwx_link_t *l, uint8_t type, unsigned pad, unsigned status)
{
    memset(packet, 0, sizeof(packet));
    unsigned len = 30 + pad;
    link_put16(packet, len); packet[3] = pad ? 0x20 : 0;
    packet[12] = 3; packet[34] = l->ap.channel;
    uint8_t *f = packet + 48, *b = f + 24 + pad;
    f[0] = type;
    memcpy(f + 4, l->local, 6); memcpy(f + 10, l->ap.bssid, 6); memcpy(f + 16, l->ap.bssid, 6);
    if (type == 0xb0) { b[2] = 2; link_put16(b + 4, status); }
    else { b[0] = 1; link_put16(b + 2, status); link_put16(b + 4, 0xc023); }
    return len + 48;
}

static void test_frames(void)
{
    fixture();
    iwx_link_t l;
    CHECK(!iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, false));
    iwx_link_mac_t c;
    iwx_link_mac_build(&c, &l, false);
    const uint8_t *wire = (const uint8_t *)&c;
    CHECK(sizeof(c) == 148 && link_u32(wire + 4) == 1 && link_u32(wire + 8) == 5);
    CHECK(!memcmp(wire + 16, nvm.mac, 6) && !memcmp(wire + 24, ap.bssid, 6));
    CHECK(link_u32(wire + 52) == 68 && link_u32(wire + 116) == 100);
    CHECK(link_u32(wire + 124) == 200 && link_u32(wire + 132) == 10 && !link_u32(wire + 100));
    uint8_t f[256];
    CHECK(!iwx_link_request(&l, f));
    l.status.state = IWX_LINK_AUTH;
    CHECK(iwx_link_request(&l, f) == 30);
    CHECK(f[0] == 0xb0 && link_u16(f + 24) == 0 && link_u16(f + 26) == 1);
    CHECK(!iwx_link_request(&l, f));
    unsigned size = response(&l, 0xb0, 2, 0);
    iwx_link_t saved = l;
    for (unsigned n = 0; n < size; n++) {
        CHECK(iwx_link_receive(&l, packet, n) == -1);
        CHECK(!memcmp(&l, &saved, sizeof(l)));
    }
    packet[48 + 4] ^= 2; CHECK(!iwx_link_receive(&l, packet, size)); packet[48 + 4] ^= 2;
    packet[12] = 1; CHECK(!iwx_link_receive(&l, packet, size)); packet[12] = 3;
    CHECK(iwx_link_receive(&l, packet, size) == 1 && l.status.state == IWX_LINK_ASSOC && !l.request_sent);
    CHECK(!iwx_link_receive(&l, packet, size));
    CHECK(iwx_link_request(&l, f) == 50 && f[0] == 0 && link_u16(f + 22) == 16);
    CHECK(link_u16(f + 24) == 0x421 && f[28] == 0 && f[29] == 4 && !memcmp(f + 30, "test", 4));
    size = response(&l, 0x10, 0, 0);
    for (unsigned i = 0; i < 65536; i++) {
        link_put16(packet + 48 + 28, i);
        iwx_link_t t = l;
        int rc = iwx_link_receive(&t, packet, size);
        bool valid = (i & 0xc000) == 0xc000 && (i & 0x3fff) && (i & 0x3fff) <= 2007;
        CHECK((rc == 1) == valid);
        if (valid) CHECK(!t.status.port_open && t.status.state == IWX_LINK_CONFIGURE);
        else CHECK(!memcmp(&t, &l, sizeof(t)));
    }
    link_put16(packet + 48 + 28, 0xc023);
    CHECK(iwx_link_receive(&l, packet, size) == 1 && l.status.aid == 35);
    iwx_link_mac_build(&c, &l, true);
    CHECK(c.sta.associated && c.sta.aid == 35 && c.sta.dtim_time == 98304);
    CHECK(c.sta.dtim_tsf == ap.tsf + 102400 && c.action == 2);
    l.status.state = IWX_LINK_ASSOCIATED;
    size = response(&l, 0xc0, 0, 0); link_put16(packet + 72, 3);
    CHECK(iwx_link_receive(&l, packet, size) == 1 && l.status.state == IWX_LINK_FAILED && l.status.reason == 3);
    l.status.state = IWX_LINK_ASSOCIATED;
    memset(packet + 52, 255, 6);
    CHECK(iwx_link_receive(&l, packet, size) == 1 && l.status.state == IWX_LINK_FAILED);

    ap.rsn = true; ap.capability |= 16; ap.rsn_len = sizeof(rsn); memcpy(ap.rsn_ie, rsn, sizeof(rsn));
    CHECK(!iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, true));
    l.status.state = IWX_LINK_ASSOC;
    CHECK(iwx_link_request(&l, f) == 72 && f[50] == 48 && f[51] == 20 && !memcmp(f + 52, rsn, 20));
    size = response(&l, 0x10, 0, 0);
    CHECK(iwx_link_receive(&l, packet, size) == -1 && l.status.state == IWX_LINK_ASSOC);
    packet[72] |= 16;
    iwx_link_t secure = l;
    CHECK(iwx_link_receive(&secure, packet, size) == 1 && secure.status.state == IWX_LINK_CONFIGURE && !secure.status.port_open);
    size = response(&l, 0x10, 0, 17);
    CHECK(iwx_link_receive(&l, packet, size) == 1 && l.status.state == IWX_LINK_FAILED && l.status.peer_status == 17);
    l = saved;
    size = response(&l, 0xb0, 0, 13);
    CHECK(iwx_link_receive(&l, packet, size) == 1 && l.status.state == IWX_LINK_FAILED && l.status.peer_status == 13);
}

static void test_tx(void)
{
    uint8_t f[256] = {0xb0};
    for (unsigned n = 24; n <= 256; n++) for (unsigned i = 0; i < 256; i++) {
        iwx_tfh_tfd_t t;
        uint16_t bc;
        CHECK(!iwx_link_tx_build(&t, page, 0x10000000, 3, i, 0x4100, f, n, &bc));
        CHECK(page[0] == 0x1c && !page[1] && page[2] == i && page[3] == 3);
        CHECK(link_u16(page + 4) == n && link_u16(page + 6) == 0xc00 && link_u32(page + 8) == 3);
        CHECK(link_u32(page + 20) == 0x4100 && !memcmp(page + 24, f, 24));
        CHECK(t.tb[0].length == 20 && t.tb[1].length == 28 && bc == (n + 3) / 4);
        CHECK(t.num_tbs == (n == 24 ? 2 : 3));
        if (n > 24) CHECK(t.tb[2].address == 0x10000040 && t.tb[2].length == n - 24);
    }
    for (unsigned q = 1; q < 512; q++) for (unsigned i = 0; i < 256; i += 255) {
        iwx_tfh_tfd_t t;
        uint16_t bc;
        CHECK(!iwx_link_tx_build(&t, page, 0x10000000, q, i, 0x4100, f, 30, &bc));
        CHECK(page[2] == i && page[3] == (q & 31));
    }
    iwx_tfh_tfd_t t = {0}, saved = t;
    uint16_t bc = 0;
    CHECK(iwx_link_tx_build(&t, page, 0x10000000, 0, 0, 0x4000, f, 30, &bc));
    CHECK(iwx_link_tx_build(&t, page, 0x10000000, 512, 0, 0x4000, f, 30, &bc));
    CHECK(iwx_link_tx_build(&t, page, 0x10000000, 0xffff, 0, 0x4000, f, 30, &bc));
    CHECK(iwx_link_tx_build(&t, page, 0x10000000, 1, 256, 0x4000, f, 30, &bc));
    CHECK(iwx_link_tx_build(&t, page, 0x100000000ULL, 1, 0, 0x4000, f, 30, &bc));
    CHECK(iwx_link_tx_build(&t, page, 0x10000001, 1, 0, 0x4000, f, 30, &bc));
    CHECK(iwx_link_tx_build(&t, page, 0, 1, 0, 0x4000, f, 30, &bc));
    CHECK(iwx_link_tx_build(&t, page, 0x10000000, 1, 0, 0x4000, f, 23, &bc));
    CHECK(iwx_link_tx_build(&t, page, 0x10000000, 1, 0, 0x4000, f, 257, &bc));
    f[1] = 0x40; CHECK(iwx_link_tx_build(&t, page, 0x10000000, 1, 0, 0x4000, f, 30, &bc));
    CHECK(!memcmp(&t, &saved, sizeof(t)));
}

static void test_fuzz(void)
{
    fixture();
    iwx_link_t l;
    CHECK(!iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, false));
    l.status.state = IWX_LINK_ASSOC; l.request_sent = true;
    uint32_t seed = 0x8b9472;
    for (unsigned i = 0; i < 20000; i++) {
        unsigned size = response(&l, 0x10, 0, 0);
        seed = seed * 1664525U + 1013904223U;
        packet[seed % size] ^= seed >> 24;
        iwx_link_t tmp = l;
        int rc = iwx_link_receive(&tmp, packet, seed % 512);
        CHECK(rc >= -1 && rc <= 1 && !tmp.status.port_open);
        if (rc <= 0) CHECK(!memcmp(&tmp, &l, sizeof(l)));
        uint8_t ie[255] = {0}; memcpy(ie, rsn, sizeof(rsn));
        ie[seed % 255] ^= seed >> 24;
        (void)iwx_link_rsn_check(ie, seed % 256);
    }
}

static void test_firmware(const char *path)
{
    int fd = open(path, 0); CHECK(fd >= 0);
    if (fd < 0) return;
    unsigned size = 0;
    for (;;) {
        long n = read(fd, image + size, sizeof(image) - size);
        if (n <= 0) { CHECK(n == 0); break; }
        size += n;
        if (size == sizeof(image)) break;
    }
    close(fd);
    iwx_fw_plan_t p;
    CHECK(!iwx_fw_plan(image, size, &p));
    const char *err = iwx_link_profile_check(&p.radio);
    if (err) printf("Actual firmware link gate: %s\n", err);
    CHECK(!err);
    CHECK(p.radio.command[IWX_ABI_TX_QUEUE] == 3 && p.radio.command[IWX_ABI_PHY_CTX] == 4);
}

static void test_rate_control(void)
{
    fixture(); iwx_link_t l;
    CHECK(!iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, false));
    uint32_t management = l.rate_flags;
    CHECK(l.status.data_rate_kbps == 1000);
    for (unsigned i = 0; i < 1000; i++) iwx_link_rate_feedback(&l, true, 0);
    CHECK(l.status.data_rate_kbps == 54000 && iwx_link_data_rate(&l) == 0x4107);
    CHECK(l.rate_flags == management); /* Authentication rate never changes. */
    iwx_link_rate_feedback(&l, false, 0);
    CHECK(l.status.data_rate_kbps == 48000 && l.data_cooldown == 100);
    for (unsigned i = 0; i < 119; i++) iwx_link_rate_feedback(&l, true, 0);
    CHECK(l.status.data_rate_kbps == 48000);
    iwx_link_rate_feedback(&l, true, 0); CHECK(l.status.data_rate_kbps == 54000);
    for (unsigned i = 0; i < 30; i++) iwx_link_rate_feedback(&l, true, 3);
    CHECK(l.status.data_rate_kbps == 1000 && !l.data_rate_index);
    fixture(); ap.channel = 36; ap.rates_len = 2; ap.rates[0] = 0x8c; ap.rates[1] = 48;
    CHECK(!iwx_link_select(&l, &ap, &nvm, &profile, &regulatory, false));
    for (unsigned i = 0; i < 100; i++) iwx_link_rate_feedback(&l, true, 0);
    CHECK(l.status.data_rate_kbps == 24000 && iwx_link_data_rate(&l) == 0x4104);
    iwx_link_rate_feedback(&l, false, 0); CHECK(l.status.data_rate_kbps == 6000);
}

int main(int argc, char **argv)
{
    test_rate_control();
    test_selection(); test_rsn(); test_frames(); test_tx(); test_fuzz();
    if (argc > 1) test_firmware(argv[1]);
    printf("iwx association/frame contracts: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
