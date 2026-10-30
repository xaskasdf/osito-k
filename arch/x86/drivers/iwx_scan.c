#include "iwx_scan.h"
#include "iwxreg.h"

static uint16_t scan_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t scan_le32(const uint8_t *p)
{
    return (uint32_t)scan_le16(p) | ((uint32_t)scan_le16(p + 2) << 16);
}

bool iwx_radio_api(const iwx_radio_profile_t *p, unsigned bit)
{
    return p && bit < 96 && (p->api[bit / 32] & (1U << (bit % 32)));
}

bool iwx_radio_capability(const iwx_radio_profile_t *p, unsigned bit)
{
    return p && bit < 128 && (p->capa[bit / 32] & (1U << (bit % 32)));
}

const char *iwx_scan_profile_check(const iwx_radio_profile_t *p, const iwx_nvm_report_t *n)
{
    static const uint8_t versions[] = {1, 6, 2, 1, 1, 6, 1, 5, 15, 1, 4, 99, 3};
    if (!p || !n || !n->valid) return "scan requires a valid firmware/NVM session";
    for (unsigned i = 0; i < sizeof(versions); i++) {
        /* DQA v1 predates CMD_VERSIONS and is declared by CAPA_DQA on this
         * API. If explicitly versioned, reject anything other than v1. */
        if (i == IWX_ABI_DQA && !(p->seen & (1U << i))) continue;
        if (!(p->seen & (1U << i)) || p->command[i] != versions[i])
            return "unsupported/missing radio or scan command ABI";
    }
    if (p->notification[IWX_ABI_MCC] != 5 || p->notification[IWX_ABI_RX_MPDU] != 4)
        return "unsupported MCC/RX notification ABI";
    if (!iwx_radio_api(p, IWX_API_REDUCED_SCAN) ||
        !iwx_radio_api(p, IWX_API_EXT_SCAN_CHANNEL) ||
        !iwx_radio_capability(p, IWX_CAPA_UMAC_SCAN) ||
        !iwx_radio_capability(p, IWX_CAPA_FW_CT_KILL) ||
        !iwx_radio_capability(p, IWX_CAPA_MCC_11AX) ||
        !iwx_radio_capability(p, IWX_CAPA_LAR) || !n->lar_enabled ||
        (!iwx_radio_api(p, IWX_API_WIFI_MCC) && !iwx_radio_capability(p, IWX_CAPA_MULTI_MCC)))
        return "unsupported regulatory/thermal/scan capabilities";
    if (p->scan_channels < IWX_SCAN_CHANNELS || n->n_channels < IWX_SCAN_CHANNELS ||
        !(n->mac_sku & 3)) return "incomplete AX200 band/channel metadata";
    unsigned tx = (p->phy_config >> 16) & 15, rx = (p->phy_config >> 20) & 15;
    if (n->tx_chains) tx &= n->tx_chains;
    if (n->rx_chains) rx &= n->rx_chains;
    if (!tx || !rx || (tx & ~3U) || (rx & ~3U)) return "invalid AX200 antenna masks";
    return NULL;
}

const char *iwx_mcc_decode(iwx_regulatory_t *out, const iwx_nvm_report_t *n,
                          const uint8_t *p, unsigned length)
{
    /* AX200 NVM/MCC index encoding, not a list of permitted channels. Only
     * firmware VALID entries in an enabled hardware band enter the result. */
    static const uint8_t channel[IWX_SCAN_CHANNELS] = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,
        36,40,44,48,52,56,60,64,68,72,76,80,84,88,92,96,
        100,104,108,112,116,120,124,128,132,136,140,144,
        149,153,157,161,165,169,173,177,181
    };
    if (!out) return "missing regulatory output";
    memset(out, 0, sizeof(*out));
    if (!n || !n->valid || !p || length < 20) return "truncated MCC response";
    unsigned count = scan_le32(p + 16);
    if (count != IWX_SCAN_CHANNELS || length != 20 + count * 4)
        return "unsupported AX200 MCC channel map/length";
    if (scan_le32(p) > 1) return "firmware rejected regulatory query";
    uint16_t mcc = scan_le16(p + 4);
    /* Firmware may return 0 for world. Other values are ISO alpha-2 or 00. */
    if (mcc && mcc != 0x3030 &&
        ((mcc >> 8) < 'A' || (mcc >> 8) > 'Z' || (mcc & 255) < 'A' || (mcc & 255) > 'Z'))
        return "invalid firmware country code";
    iwx_regulatory_t reg = {.mcc = mcc, .capabilities = scan_le16(p + 6),
                            .geo = scan_le16(p + 10), .source = p[12]};
    for (unsigned i = 0; i < count; i++) {
        uint32_t flags = scan_le32(p + 20 + i * 4);
        if (!(flags & IWX_CHANNEL_VALID) || !(n->mac_sku & (i < 14 ? 1 : 2))) continue;
        reg.channel[reg.count].number = channel[i];
        reg.channel[reg.count++].flags = flags;
    }
    if (!reg.count) return "firmware permits no scan channels";
    reg.valid = true;
    *out = reg;
    return NULL;
}

const char *iwx_scan_build(iwx_scan_request_t *s, const iwx_regulatory_t *reg,
                          const iwx_nvm_report_t *n)
{
    if (!s) return "missing scan request output";
    memset(s, 0, sizeof(*s));
    if (!reg || !reg->valid || !reg->count || reg->count > IWX_SCAN_CHANNELS || !n || !n->valid)
        return "scan requires current regulatory data";
    /* UID zero owns the only scan slot. It is reused only after completion;
     * timeout recovery stops firmware before another slot can be allocated. */
    s->priority = s->general.priority = 6;
    s->general.flags = IWX_SCAN_FORCE_PASSIVE | IWX_SCAN_PASS_ALL |
                       IWX_SCAN_ITER_COMPLETE | IWX_SCAN_ADAPTIVE_DWELL;
    s->general.active_dwell[0] = s->general.active_dwell[1] = 10;
    s->general.passive_dwell[0] = s->general.passive_dwell[1] = 110;
    s->general.adwell_2g = 2;
    s->general.adwell_5g = 8;
    s->general.adwell_social = 10;
    s->general.adwell_budget = 300;
    s->channels.flags = IWX_SCAN_CHANNEL_ORDER;
    s->channels.count = reg->count;
    s->channels.aps_override[0] = 10;
    s->channels.aps_override[1] = 2;
    for (unsigned i = 0; i < reg->count; i++) {
        if (!(reg->channel[i].flags & IWX_CHANNEL_VALID)) return "invalid scan channel permission";
        s->channels.channel[i].channel = reg->channel[i].number;
        s->channels.channel[i].band = reg->channel[i].number <= 14 ? 1 : 0;
        s->channels.channel[i].iterations = 1;
    }
    s->periodic.schedule[0].iterations = 1;
    /* A valid template is part of this ABI even for FORCE_PASSIVE. No directed
     * SSIDs or passive-to-active flags are enabled, so it is never transmitted. */
    uint8_t *frame = s->probe.data;
    frame[0] = 0x40;
    memset(frame + 4, 0xff, 6);
    memcpy(frame + 10, n->mac, 6);
    memset(frame + 16, 0xff, 6);
    s->probe.mac.length = 26; /* 24-byte header + wildcard SSID IE. */
    static const uint8_t rates2[] = {1,8,2,4,11,22,12,18,24,36,50,4,48,72,96,108};
    static const uint8_t rates5[] = {1,8,12,18,24,36,48,72,96,108};
    memcpy(frame + 26, rates2, sizeof(rates2));
    memcpy(frame + 26 + sizeof(rates2), rates5, sizeof(rates5));
    s->probe.band[0] = (iwx_probe_segment_t){26, sizeof(rates2)};
    s->probe.band[1] = (iwx_probe_segment_t){26 + sizeof(rates2), sizeof(rates5)};
    s->probe.common.offset = 26 + sizeof(rates2) + sizeof(rates5);
    return NULL;
}

static bool scan_channel_allowed(const iwx_regulatory_t *reg, unsigned channel)
{
    if (!reg || !reg->valid || reg->count > IWX_SCAN_CHANNELS) return false;
    for (unsigned i = 0; i < reg->count; i++)
        if (reg->channel[i].number == channel) return true;
    return false;
}

int iwx_scan_decode_mpdu(iwx_bss_t *out, const uint8_t *p, unsigned size,
                         const iwx_regulatory_t *reg)
{
    if (!out || !p || size < 48) return -1;
    unsigned length = scan_le16(p);
    if (length > size - 48 || length < 24) return -1;
    if ((scan_le32(p + 12) & 3) != 3) return 0;
    const uint8_t *frame = p + 48;
    if (frame[0] != 0x80 && frame[0] != 0x50) return 0;
    if ((frame[1] & 0xc7) || (scan_le16(frame + 22) & 15) || (p[3] & 0x40)) return -1;
    unsigned header = 24 + ((p[3] & 0x20) ? 2 : 0);
    if (length < header + 12) return -1;
    iwx_bss_t b = {.channel = p[34], .observations = 1};
    if (!scan_channel_allowed(reg, b.channel)) return 0;
    memcpy(b.bssid, frame + 16, 6);
    static const uint8_t zero[6] = {0};
    if ((b.bssid[0] & 1) || !memcmp(b.bssid, zero, 6)) return -1;
    int a = p[32] ? -(int)p[32] : -256, c = p[33] ? -(int)p[33] : -256;
    b.signal_dbm = a > c ? a : c;
    const uint8_t *body = frame + header;
    b.beacon_interval = scan_le16(body + 8);
    b.capability = scan_le16(body + 10);
    b.tsf = scan_le32(body) | ((uint64_t)scan_le32(body + 4) << 32);
    b.system_time = scan_le32(p + 36);
    bool ssid_seen = false, channel_seen = false, rates_seen = false, ext_seen = false, tim_seen = false;
    bool erp_seen = false;
    for (unsigned pos = header + 12; pos < length;) {
        if (length - pos < 2) return -1;
        unsigned id = frame[pos], len = frame[pos + 1];
        pos += 2;
        if (len > length - pos) return -1;
        const uint8_t *v = frame + pos;
        if (id == 0) {
            if (ssid_seen || len > sizeof(b.ssid)) return -1;
            ssid_seen = true;
            bool visible = false;
            for (unsigned i = 0; i < len; i++) visible |= v[i] != 0;
            if (visible) { memcpy(b.ssid, v, len); b.ssid_len = len; }
        } else if (id == 3) {
            if (channel_seen || len != 1) return -1;
            channel_seen = true;
            if (!scan_channel_allowed(reg, v[0])) return 0;
            b.channel = v[0];
        } else if (id == 48) {
            if (b.rsn || len < 2 || scan_le16(v) != 1) return -1;
            b.rsn = true;
            b.rsn_len = len;
            memcpy(b.rsn_ie, v, len);
        } else if (id == 1 || id == 50) {
            bool *seen = id == 1 ? &rates_seen : &ext_seen;
            if (*seen || !len || (id == 1 && len > 8)) return -1;
            *seen = true;
            if (len > sizeof(b.rates) - b.rates_len) return -1;
            memcpy(b.rates + b.rates_len, v, len);
            b.rates_len += len;
        } else if (id == 5) {
            if (tim_seen || len < 4 || !v[1] || v[0] >= v[1]) return -1;
            tim_seen = true;
            b.dtim_count = v[0];
            b.dtim_period = v[1];
        } else if (id == 42) {
            if (erp_seen || len != 1) return -1;
            erp_seen = true;
            b.erp = v[0] & 7;
        } else if (id == 221 && len >= 4 && !memcmp(v, "\x00\x50\xf2\x01", 4)) {
            if (b.wpa || len < 6 || scan_le16(v + 4) != 1) return -1;
            b.wpa = true;
        }
        pos += len;
    }
    if (!ssid_seen) return -1;
    *out = b;
    return 1;
}

void iwx_scan_cache(iwx_scan_status_t *s, iwx_bss_t *cache, const iwx_bss_t *b)
{
    for (unsigned i = 0; i < s->count; i++) {
        if (cache[i].channel != b->channel || memcmp(cache[i].bssid, b->bssid, 6)) continue;
        iwx_bss_t updated = *b;
        updated.observations = cache[i].observations;
        if (updated.observations != UINT32_MAX) updated.observations++;
        if (!b->ssid_len && cache[i].ssid_len) {
            updated.ssid_len = cache[i].ssid_len;
            memcpy(updated.ssid, cache[i].ssid, sizeof(updated.ssid));
        }
        cache[i] = updated;
        return;
    }
    if (s->count == IWX_SCAN_BSS_MAX) { s->dropped++; return; }
    cache[s->count++] = *b;
}

void iwx_ssid_text(char out[129], const uint8_t *ssid, unsigned length)
{
    static const char hex[] = "0123456789abcdef";
    unsigned pos = 0;
    if (length > 32) length = 32;
    for (unsigned i = 0; i < length; i++) {
        uint8_t c = ssid[i];
        if (c >= 32 && c <= 126 && c != '\\' && c != '"') out[pos++] = c;
        else {
            out[pos++] = '\\'; out[pos++] = 'x';
            out[pos++] = hex[c >> 4]; out[pos++] = hex[c & 15];
        }
    }
    out[pos] = 0;
}
