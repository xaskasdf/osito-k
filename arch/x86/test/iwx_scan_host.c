#include "../drivers/iwx_scan.c"
#include "../drivers/iwx_fw.c"

extern int printf(const char *format, ...);
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int close(int fd);
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static iwx_nvm_report_t nvm = {.valid = true, .mac_sku = 3, .n_channels = 51,
    .lar_enabled = 1, .tx_chains = 3, .rx_chains = 3, .mac = {2,1,2,3,4,5}};
static uint8_t mcc[224], packet[4096], image[IWX_FW_MAX_SIZE];
static iwx_regulatory_t regulatory;

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) p[i] = value >> (8 * i);
}

static void regulatory_fixture(void)
{
    memset(mcc, 0, sizeof(mcc));
    put32(mcc + 4, 0x00005553); /* US is only a synthetic test reply. */
    mcc[12] = 0x10;
    put32(mcc + 16, 51);
    for (unsigned i = 0; i < 51; i++) put32(mcc + 20 + 4 * i, 1 | (i < 11 ? 8 : 16));
    CHECK(iwx_mcc_decode(&regulatory, &nvm, mcc, sizeof(mcc)) == NULL);
}

static void test_regulatory(void)
{
    regulatory_fixture();
    CHECK(regulatory.valid && regulatory.count == 51 && regulatory.channel[14].number == 36);
    CHECK(regulatory.channel[50].number == 181 && regulatory.mcc == 0x5553);
    iwx_regulatory_t output, zero = {0};
    for (unsigned length = 0; length < sizeof(mcc); length++) {
        memset(&output, 0xa5, sizeof(output));
        CHECK(iwx_mcc_decode(&output, &nvm, mcc, length) != NULL);
        CHECK(!memcmp(&output, &zero, sizeof(output)));
    }
    CHECK(iwx_mcc_decode(&output, &nvm, NULL, sizeof(mcc)) != NULL);
    CHECK(iwx_mcc_decode(NULL, &nvm, mcc, sizeof(mcc)) != NULL);
    for (unsigned status = 0; status < 10; status++) {
        put32(mcc, status);
        CHECK((iwx_mcc_decode(&output, &nvm, mcc, sizeof(mcc)) == NULL) == (status < 2));
    }
    put32(mcc, 0);
    for (unsigned count = 0; count < 120; count++) {
        put32(mcc + 16, count);
        CHECK((iwx_mcc_decode(&output, &nvm, mcc, sizeof(mcc)) == NULL) == (count == 51));
    }
    put32(mcc + 16, UINT32_MAX);
    CHECK(iwx_mcc_decode(&output, &nvm, mcc, sizeof(mcc)) != NULL);
    put32(mcc + 16, 51);
    put32(mcc + 4, 0x1b0a);
    CHECK(iwx_mcc_decode(&output, &nvm, mcc, sizeof(mcc)) != NULL);
    for (unsigned m = 0; m < 2; m++) {
        put32(mcc + 4, m ? 0x3030 : 0);
        CHECK(iwx_mcc_decode(&output, &nvm, mcc, sizeof(mcc)) == NULL);
    }
    put32(mcc + 20, 0);
    CHECK(iwx_mcc_decode(&output, &nvm, mcc, sizeof(mcc)) == NULL && output.count == 50);
    CHECK(output.channel[0].number == 2);
    iwx_nvm_report_t bands = nvm;
    bands.mac_sku = 1;
    CHECK(iwx_mcc_decode(&output, &bands, mcc, sizeof(mcc)) == NULL && output.count == 13);
    bands.mac_sku = 2;
    CHECK(iwx_mcc_decode(&output, &bands, mcc, sizeof(mcc)) == NULL && output.count == 37);
    memset(mcc + 20, 0, sizeof(mcc) - 20);
    CHECK(iwx_mcc_decode(&output, &nvm, mcc, sizeof(mcc)) != NULL && !output.valid);
}

static void test_request(void)
{
    regulatory_fixture();
    iwx_scan_request_t s;
    memset(&s, 0xa5, sizeof(s));
    CHECK(iwx_scan_build(&s, &regulatory, &nvm) == NULL);
    CHECK(sizeof(s) == 1940 && s.uid == 0 && s.priority == 6);
    const uint8_t *wire = (const uint8_t *)&s;
    CHECK(wire[8] == 0x86 && wire[9] == 0x08 && wire[17] == 0); /* PASSIVE, no flags2. */
    CHECK(wire[44] == 32 && wire[45] == 51 && wire[586] == 1);
    CHECK(s.general.passive_dwell[0] == 110 && s.general.adwell_budget == 300);
    for (unsigned i = 0; i < 51; i++) {
        CHECK(s.channels.channel[i].channel == regulatory.channel[i].number);
        CHECK(s.channels.channel[i].flags == 0 && s.channels.channel[i].iterations == 1);
        CHECK(s.channels.channel[i].band == (i < 14 ? 1 : 0));
    }
    for (unsigned i = 51; i < 67; i++) CHECK(!s.channels.channel[i].channel);
    CHECK(s.probe.mac.length == 26 && s.probe.data[0] == 0x40 && s.probe.data[24] == 0);
    CHECK(!memcmp(s.probe.data + 10, nvm.mac, 6));
    for (unsigned i = 0; i < 20; i++) CHECK(!s.probe.ssid[i].length && !s.probe.ssid[i].id);
    CHECK(s.probe.band[0].offset == 26 && s.probe.band[1].offset == 42);
    CHECK(s.probe.common.offset == 52 && s.probe.common.length == 0);
    regulatory.valid = false;
    CHECK(iwx_scan_build(&s, &regulatory, &nvm) != NULL);
    regulatory.valid = true; regulatory.count = 52;
    CHECK(iwx_scan_build(&s, &regulatory, &nvm) != NULL);
}

static unsigned beacon(bool padded)
{
    memset(packet, 0, sizeof(packet));
    unsigned length = 48 + 24 + (padded ? 2 : 0) + 12 + 6 + 3 + 4;
    packet[0] = length - 48;
    packet[3] = padded ? 0x20 : 0;
    packet[12] = 3;
    packet[32] = 60; packet[33] = 42; packet[34] = 6;
    packet[48] = 0x80;
    memcpy(packet + 64, nvm.mac, 6);
    uint8_t *b = packet + 48 + 24 + (padded ? 2 : 0);
    b[8] = 100; b[10] = 0x11;
    b[12] = 0; b[13] = 4; memcpy(b + 14, "test", 4);
    b[18] = 3; b[19] = 1; b[20] = 6;
    b[21] = 48; b[22] = 2; b[23] = 1;
    return length;
}

static void test_mpdu(void)
{
    regulatory_fixture();
    iwx_bss_t b, sentinel;
    for (unsigned pad = 0; pad < 2; pad++) {
        unsigned length = beacon(pad);
        CHECK(iwx_scan_decode_mpdu(&b, packet, length, &regulatory) == 1);
        CHECK(b.ssid_len == 4 && !memcmp(b.ssid, "test", 4) && b.rsn && !b.wpa);
        CHECK(b.signal_dbm == -42 && b.channel == 6 && b.beacon_interval == 100);
        CHECK(b.observations == 1 && !memcmp(b.bssid, nvm.mac, 6));
        for (unsigned n = 0; n < length; n++)
            CHECK(iwx_scan_decode_mpdu(&b, packet, n, &regulatory) == -1);
        packet[48] = 0x50;
        CHECK(iwx_scan_decode_mpdu(&b, packet, length, &regulatory) == 1);
        packet[48] = 8;
        CHECK(iwx_scan_decode_mpdu(&b, packet, length, &regulatory) == 0);
        packet[48] = 0x80; packet[49] = 0x40;
        CHECK(iwx_scan_decode_mpdu(&b, packet, length, &regulatory) == -1);
    }
    unsigned length = beacon(false);
    packet[12] = 1;
    CHECK(iwx_scan_decode_mpdu(&b, packet, length, &regulatory) == 0);
    packet[12] = 3; packet[64] = 1;
    CHECK(iwx_scan_decode_mpdu(&b, packet, length, &regulatory) == -1);
    length = beacon(false); packet[85] = 33;
    CHECK(iwx_scan_decode_mpdu(&b, packet, length, &regulatory) == -1);
    length = beacon(false); packet[34] = 200;
    CHECK(iwx_scan_decode_mpdu(&b, packet, length, &regulatory) == 0);
    length = beacon(false); packet[32] = packet[33] = 0;
    CHECK(iwx_scan_decode_mpdu(&b, packet, length, &regulatory) == 1 && b.signal_dbm == -256);
    memset(packet + 86, 0, 4);
    CHECK(iwx_scan_decode_mpdu(&b, packet, length, &regulatory) == 1 && !b.ssid_len);
    memset(&sentinel, 0xa5, sizeof(sentinel));
    uint32_t random = 17381;
    for (unsigned i = 0; i < 20000; i++) {
        length = beacon(i & 1);
        random = random * 1664525 + 1013904223;
        packet[random % length] ^= (random >> 16) | 1;
        random = random * 1664525 + 1013904223;
        b = sentinel;
        int rc = iwx_scan_decode_mpdu(&b, packet, random % (length + 1), &regulatory);
        CHECK(rc >= -1 && rc <= 1);
        if (rc != 1) CHECK(!memcmp(&b, &sentinel, sizeof(b)));
        else CHECK(b.ssid_len <= 32 && b.channel && b.observations == 1);
    }
}

static void test_association_metadata(void)
{
    regulatory_fixture();
    unsigned end = beacon(false);
    static const uint8_t ies[] = {1,8,0x82,0x84,0x8b,0x96,12,18,24,36,
        50,4,48,72,96,108,5,4,1,2,0,0,42,1,6};
    memcpy(packet + end, ies, sizeof(ies));
    unsigned total = end + sizeof(ies);
    packet[0] = total - 48;
    put32(packet + 36, 0x12345678);
    put32(packet + 72, 0x1234); put32(packet + 76, 0xabcdef);
    iwx_bss_t b, saved;
    CHECK(iwx_scan_decode_mpdu(&b, packet, total, &regulatory) == 1);
    CHECK(b.rates_len == 12 && b.rates[11] == 108 && b.dtim_count == 1 && b.dtim_period == 2 && b.erp == 6);
    CHECK(b.tsf == 0xabcdef00001234ULL && b.system_time == 0x12345678);
    CHECK(b.rsn_len == 2 && b.rsn_ie[0] == 1 && !b.rsn_ie[1]);
    saved = b;
    packet[end + 19] = 0;
    CHECK(iwx_scan_decode_mpdu(&b, packet, total, &regulatory) == -1 && !memcmp(&b, &saved, sizeof(b)));
    packet[end + 19] = 2;
    packet[end + 18] = 2;
    CHECK(iwx_scan_decode_mpdu(&b, packet, total, &regulatory) == -1);
    packet[end + 18] = 1;
    for (unsigned id = 0; id < 4; id++) {
        static const uint8_t duplicate[] = {1,50,5,42};
        packet[0] = total + 3 - 48;
        packet[total] = duplicate[id]; packet[total + 1] = 1; packet[total + 2] = 1;
        CHECK(iwx_scan_decode_mpdu(&b, packet, total + 3, &regulatory) == -1);
    }
}

static void test_cache(void)
{
    iwx_scan_status_t s = {0};
    iwx_bss_t cache[IWX_SCAN_BSS_MAX], b = {.ssid_len = 4, .ssid = "test", .channel = 6,
                                           .observations = 1, .bssid = {2,1,2,3,4,5}};
    iwx_scan_cache(&s, cache, &b);
    CHECK(s.count == 1 && cache[0].observations == 1);
    b.ssid_len = 0;
    iwx_scan_cache(&s, cache, &b);
    CHECK(s.count == 1 && cache[0].observations == 2 && cache[0].ssid_len == 4);
    for (unsigned i = 0; i < 128; i++) {
        b.bssid[5] = i;
        iwx_scan_cache(&s, cache, &b);
    }
    CHECK(s.count == 128 && !s.dropped);
    b.bssid[4]++;
    iwx_scan_cache(&s, cache, &b);
    CHECK(s.count == 128 && s.dropped == 1);
    char text[129];
    iwx_ssid_text(text, (const uint8_t *)"x\n\x1b\"\\\xff", 6);
    CHECK(!strcmp(text, "x\\x0a\\x1b\\x22\\x5c\\xff"));
    uint8_t all[33] = {0};
    memset(text, 0xa5, sizeof(text));
    iwx_ssid_text(text, all, 33);
    CHECK(strlen(text) == 128 && text[128] == 0);
    iwx_ssid_text(text, all, 0);
    CHECK(!text[0]);
}

static void test_firmware(const char *path)
{
    int fd = open(path, 0);
    CHECK(fd >= 0);
    if (fd < 0) return;
    unsigned size = 0;
    for (;;) {
        long n = read(fd, image + size, sizeof(image) - size);
        if (n <= 0) { CHECK(n == 0); break; }
        size += n;
        if (size == sizeof(image)) break;
    }
    close(fd);
    iwx_fw_plan_t plan;
    CHECK(iwx_fw_plan(image, size, &plan) == NULL);
    CHECK(iwx_scan_profile_check(&plan.radio, &nvm) == NULL);
    CHECK(plan.radio.scan_channels == 67 && plan.radio.command[IWX_ABI_SCAN_REQ] == 15);
    CHECK(!(plan.radio.seen & (1U << IWX_ABI_DQA))); /* Legacy v1, CAPA-declared. */
    CHECK(iwx_radio_api(&plan.radio, 58) && iwx_radio_capability(&plan.radio, 74));
    for (unsigned i = 0; i <= IWX_ABI_LTR; i++) {
        iwx_radio_profile_t p = plan.radio;
        p.seen |= 1U << i;
        p.command[i] = 98;
        CHECK(iwx_scan_profile_check(&p, &nvm) != NULL);
    }
    const unsigned cap[] = {1,2,74,89}, api[] = {56,58};
    for (unsigned i = 0; i < sizeof(cap) / sizeof(cap[0]); i++) {
        iwx_radio_profile_t p = plan.radio;
        p.capa[cap[i] / 32] &= ~(1U << (cap[i] % 32));
        CHECK(iwx_scan_profile_check(&p, &nvm) != NULL);
    }
    for (unsigned i = 0; i < sizeof(api) / sizeof(api[0]); i++) {
        iwx_radio_profile_t p = plan.radio;
        p.api[api[i] / 32] &= ~(1U << (api[i] % 32));
        CHECK(iwx_scan_profile_check(&p, &nvm) != NULL);
    }
    CHECK(!iwx_radio_api(&plan.radio, 96) && !iwx_radio_capability(&plan.radio, 128));
    iwx_fw_plan_t empty = {0};
    for (unsigned type = IWX_TLV_API_CHANGES; type <= IWX_TLV_CAPABILITIES; type++) {
        for (unsigned word = 0; word < (type == IWX_TLV_API_CHANGES ? 3U : 4U); word++) {
            put32(image + size, type); put32(image + size + 4, 8);
            put32(image + size + 8, word); put32(image + size + 12, 0);
            CHECK(iwx_fw_plan(image, size + 16, &plan) != NULL);
            CHECK(!memcmp(&plan, &empty, sizeof(plan)));
        }
    }
    for (unsigned pos = 88; pos < size; pos += 8 + ((le32(image + pos + 4) + 3) & ~3U)) {
        unsigned type = le32(image + pos);
        if (type == IWX_TLV_SCAN_CHANNELS) {
            unsigned saved = le32(image + pos + 8);
            put32(image + pos + 8, 0);
            CHECK(iwx_fw_plan(image, size, &plan) != NULL);
            put32(image + pos + 8, saved);
        } else if (type == IWX_TLV_CMD_VERSIONS) {
            unsigned saved = le32(image + pos + 8);
            put32(image + pos + 8, 0x000f010d); /* Second scan request ABI. */
            CHECK(iwx_fw_plan(image, size, &plan) != NULL);
            put32(image + pos + 8, saved);
        }
    }
    CHECK(iwx_fw_plan(image, size, &plan) == NULL && !iwx_scan_profile_check(&plan.radio, &nvm));
}

int main(int argc, char **argv)
{
    test_regulatory(); test_request(); test_mpdu(); test_association_metadata(); test_cache();
    if (argc > 1) test_firmware(argv[1]);
    printf("iwx passive scan contracts: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
