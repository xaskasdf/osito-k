#include "iwx_link.h"
#include "iwx_data.h"
#include "iwx_scan.h"
#include "../kernel/wpa2_rsn.h"

static uint16_t link_u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t link_u32(const uint8_t *p) { return link_u16(p) | ((uint32_t)link_u16(p + 2) << 16); }
static void link_put16(uint8_t *p, unsigned n) { p[0] = n; p[1] = n >> 8; }
static void link_put32(uint8_t *p, uint32_t n) { link_put16(p, n); link_put16(p + 2, n >> 16); }

const char *iwx_link_profile_check(const iwx_radio_profile_t *p)
{
    static const uint8_t versions[] = {4, 2, 5, 2, 12, 9, 3, 1, 99};
    if (!p) return "missing association firmware profile";
    for (unsigned i = 0; i < sizeof(versions); i++) {
        unsigned id = IWX_ABI_PHY_CTX + i;
        if (!(p->seen & (1U << id)) || p->command[id] != versions[i])
            return "unsupported/missing association command ABI";
    }
    if (p->notification[IWX_ABI_TX] != 7 || p->notification[IWX_ABI_SESSION_NOTIF] != 2 ||
        p->notification[IWX_ABI_TX_QUEUE] != 2)
        return "unsupported association/TX notification ABI";
    if (!iwx_radio_capability(p, IWX_CAPA_UHB_CHANNEL_ABI) || iwx_radio_capability(p, IWX_CAPA_CDB) ||
        !iwx_radio_capability(p, IWX_CAPA_SESSION))
        return "unsupported UHB/CDB/session association profile";
    return NULL;
}

const char *iwx_link_rsn_check(const uint8_t *ie, unsigned n)
{
    return wpa2_rsn_check(ie, n);
}

bool iwx_link_tx_allowed(const iwx_regulatory_t *reg, unsigned channel)
{
    if (!reg || !reg->valid || reg->count > IWX_SCAN_CHANNELS) return false;
    for (unsigned i = 0; i < reg->count; i++)
        if (reg->channel[i].number == channel)
            return (reg->channel[i].flags & (IWX_CHANNEL_VALID | IWX_CHANNEL_ACTIVE)) ==
                   (IWX_CHANNEL_VALID | IWX_CHANNEL_ACTIVE);
    return false;
}

const char *iwx_link_select(iwx_link_t *out, const iwx_bss_t *ap,
                           const iwx_nvm_report_t *n, const iwx_radio_profile_t *p,
                           const iwx_regulatory_t *reg, bool rsn)
{
    static const uint8_t rates[] = {2,4,11,22,12,18,24,36,48,72,96,108};
    if (!out || !ap || !n || !n->valid || !p) return "invalid association parameters";
    const char *err = iwx_link_profile_check(p);
    if (err) return err;
    if (!iwx_link_tx_allowed(reg, ap->channel)) return "firmware has not permitted active use of AP channel";
    static const uint8_t zero[6] = {0};
    if (!memcmp(ap->bssid, zero, 6) || (ap->bssid[0] & 1) || !memcmp(ap->bssid, n->mac, 6) ||
        !memcmp(n->mac, zero, 6) || (n->mac[0] & 1)) return "invalid association MAC address";
    if ((ap->capability & 3) != 1 || !ap->beacon_interval || !ap->ssid_len || ap->ssid_len > 32)
        return "association requires a visible infrastructure BSS";
    if (!ap->rates_len || ap->rates_len > sizeof(ap->rates)) return "AP has no usable supported rates";
    if (rsn) {
        if (!ap->rsn || !(ap->capability & 16)) return "selected AP does not advertise WPA2";
        err = iwx_link_rsn_check(ap->rsn_ie, ap->rsn_len);
        if (err) return err;
    } else if (ap->rsn || ap->wpa || (ap->capability & 16)) return "open mode cannot join a secured AP";
    iwx_link_t l = {.ap = *ap};
    l.status.state = IWX_LINK_PREPARING;
    l.status.rsn = rsn; l.status.channel = ap->channel;
    memcpy(l.status.bssid, ap->bssid, 6); memcpy(l.local, n->mac, 6);
    unsigned lowest = 255, lowest_id = 0;
    int lowest_cck = -1, lowest_ofdm = -1;
    for (unsigned i = 0; i < ap->rates_len; i++) {
        unsigned r = ap->rates[i] & 127, j;
        for (j = ap->channel <= 14 ? 0 : 4; j < sizeof(rates) && rates[j] != r; j++) {}
        if (j == sizeof(rates)) {
            if (ap->rates[i] & 128) return "AP requires an unsupported basic rate";
            continue;
        }
        bool duplicate = false;
        for (unsigned k = 0; k < l.nrates; k++) if ((l.rates[k] & 127) == r) duplicate = true;
        if (duplicate) return "duplicate AP supported rate";
        if (l.nrates == sizeof(l.rates)) return "too many AP rates";
        l.rates[l.nrates++] = ap->rates[i];
        if (j >= 4) l.ofdm_supported = true;
        if (ap->rates[i] & 128) {
            if (r < lowest) { lowest = r; lowest_id = j; }
            if (j < 4) {
                l.cck |= 1U << j;
                if (lowest_cck < 0 || (int)r < lowest_cck) lowest_cck = r;
            } else {
                l.ofdm |= 1U << (j - 4);
                if (lowest_ofdm < 0 || (int)r < lowest_ofdm) lowest_ofdm = r;
            }
        }
    }
    if (!l.nrates || lowest == 255) return "AP has no common basic rate";
    /* Mandatory lower rates are also ACK-capable (802.11 response-rate rule). */
    l.ofdm |= 1;
    if (lowest_ofdm > 24) l.ofdm |= 1U << 2;
    if (lowest_ofdm > 48) l.ofdm |= 1U << 4;
    if (ap->channel <= 14) {
        l.cck |= 1;
        if (lowest_cck > 4) l.cck |= 2;
        if (lowest_cck > 11) l.cck |= 4;
        if (lowest_cck > 22) l.cck |= 8;
    }
    unsigned ant = ((p->phy_config >> 16) & 3) & n->tx_chains;
    if (!ant) return "no TX antenna for association";
    ant &= 0U - ant;
    l.rate_flags = (ant << 14) | (lowest_id < 4 ? lowest_id : 0x100U | (lowest_id - 4));
    for (unsigned i = 0; i < l.nrates; i++) {
        unsigned rate = l.rates[i] & 127, at = i;
        while (at && l.data_rates[at - 1] > rate) {
            l.data_rates[at] = l.data_rates[at - 1]; at--;
        }
        l.data_rates[at] = rate;
    }
    l.status.data_rate_flags = iwx_link_data_rate(&l);
    l.status.data_rate_kbps = l.data_rates[0] * 500;
    l.capability = 1 | (ap->capability & (0x20 | 0x400)) | (rsn ? 16 : 0);
    if (ap->erp & 4) l.capability &= ~0x20U; /* Barker preamble required. */
    *out = l;
    return NULL;
}

uint32_t iwx_link_data_rate(const iwx_link_t *l)
{
    static const uint8_t rates[] = {2,4,11,22,12,18,24,36,48,72,96,108};
    unsigned rate = l->data_rates[l->data_rate_index];
    for (unsigned i = 0; i < sizeof(rates); i++) if (rates[i] == rate)
        return (l->rate_flags & ~0x107U) | (i < 4 ? i : 0x100U | (i - 4));
    return l->rate_flags;
}

void iwx_link_rate_feedback(iwx_link_t *l, bool acknowledged, unsigned retries)
{
    /* Probe one advertised legacy rate after a clean success run. Fall back
     * on loss/excess retries and wait longer before probing again. Management
     * and EAPOL never use this controller. HT/VHT/aggregation are not implied. */
    if (!acknowledged || retries >= 3) {
        if (l->data_rate_index) l->data_rate_index--;
        l->data_successes = 0; l->data_cooldown = 100;
    } else if (retries) {
        l->data_successes = 0;
    } else if (l->data_cooldown) {
        l->data_cooldown--;
    } else if (++l->data_successes >= 20) {
        if (l->data_rate_index + 1 < l->nrates) l->data_rate_index++;
        l->data_successes = 0;
    }
    l->status.data_rate_flags = iwx_link_data_rate(l);
    l->status.data_rate_kbps = l->data_rates[l->data_rate_index] * 500;
}

void iwx_link_mac_build(iwx_link_mac_t *c, const iwx_link_t *l, bool associated)
{
    memset(c, 0, sizeof(*c));
    c->action = associated ? 2 : 1; c->type = 5;
    memcpy(c->local, l->local, 6); memcpy(c->bssid, l->ap.bssid, 6);
    c->cck = l->cck; c->ofdm = l->ofdm;
    c->preamble = (l->capability & 0x20) ? 0x20 : 0;
    c->slot = (l->capability & 0x400) ? 0x10 : 0;
    c->protection = (l->ap.erp & 2) ? 8 : 0;
    c->filter = 4 | 64; /* Group frames and beacons; not promiscuous. */
    /* Non-QoS station: DCF defaults on every FIFO, no HT/aggregation claimed. */
    for (unsigned i = 0; i < 4; i++)
        c->ac[i] = (iwx_link_ac_t){.cw_min = l->ofdm_supported ? 15 : 31,
                                  .cw_max = 1023, .aifs = 2, .fifo = 1U << i};
    c->sta.associated = associated;
    c->sta.beacon_interval = l->ap.beacon_interval;
    c->sta.dtim_interval = l->ap.beacon_interval * l->ap.dtim_period;
    c->sta.listen_interval = 10;
    if (associated) {
        uint32_t offset = l->ap.dtim_count * l->ap.beacon_interval * 1024U;
        c->sta.dtim_time = l->ap.system_time + offset;
        c->sta.dtim_tsf = l->ap.tsf + offset;
        c->sta.beacon_time = l->ap.system_time;
        c->sta.aid = l->status.aid;
    }
}

unsigned iwx_link_request(iwx_link_t *l, uint8_t f[256])
{
    if (!l || !f || l->request_sent || (l->status.state != IWX_LINK_AUTH && l->status.state != IWX_LINK_ASSOC))
        return 0;
    memset(f, 0, 256);
    memcpy(f + 4, l->ap.bssid, 6); memcpy(f + 10, l->local, 6); memcpy(f + 16, l->ap.bssid, 6);
    link_put16(f + 22, (l->sequence++ & 4095) << 4);
    unsigned length;
    if (l->status.state == IWX_LINK_AUTH) {
        f[0] = 0xb0;
        link_put16(f + 26, 1); /* Open System, transaction 1, status zero. */
        length = 30;
    } else {
        link_put16(f + 24, l->capability); link_put16(f + 26, 10);
        f[28] = 0; f[29] = l->ap.ssid_len;
        memcpy(f + 30, l->ap.ssid, l->ap.ssid_len);
        length = 30 + l->ap.ssid_len;
        unsigned first = l->nrates < 8 ? l->nrates : 8;
        f[length++] = 1; f[length++] = first;
        memcpy(f + length, l->rates, first); length += first;
        if (l->nrates > first) {
            f[length++] = 50; f[length++] = l->nrates - first;
            memcpy(f + length, l->rates + first, l->nrates - first); length += l->nrates - first;
        }
        if (l->status.rsn) {
            if (wpa2_rsn_assoc(f + length, l->ap.rsn_ie, l->ap.rsn_len)) return 0;
            length += sizeof(wpa2_assoc_rsn);
        }
    }
    l->request_sent = true;
    return length;
}

int iwx_link_receive(iwx_link_t *l, const uint8_t *p, unsigned size)
{
    if (!l || !p || size < 48) return -1;
    unsigned len = link_u16(p), header = 24 + ((p[3] & 0x20) ? 2 : 0);
    if (len > size - 48 || len < header) return -1;
    if ((link_u32(p + 12) & 3) != 3 || p[34] != l->ap.channel) return 0;
    const uint8_t *f = p + 48, *body = f + header;
    if ((f[0] & 15) || (f[1] & 0xc7) || (link_u16(f + 22) & 15) || (p[3] & 0x40)) return 0;
    static const uint8_t broadcast[6] = {255,255,255,255,255,255};
    bool disconnect = f[0] == 0xc0 || f[0] == 0xa0;
    if ((memcmp(f + 4, l->local, 6) && (!disconnect || memcmp(f + 4, broadcast, 6))) ||
        memcmp(f + 10, l->ap.bssid, 6) || memcmp(f + 16, l->ap.bssid, 6)) return 0;
    len -= header;
    if ((f[0] == 0xc0 || f[0] == 0xa0) && l->status.state >= IWX_LINK_AUTH &&
        l->status.state <= IWX_LINK_ASSOCIATED) {
        if (len < 2) return -1;
        l->status.reason = link_u16(body);
        l->status.error = "AP disconnected the station";
        l->status.state = IWX_LINK_FAILED;
        return 1;
    }
    if (!l->request_sent) return 0;
    if (f[0] == 0xb0 && l->status.state == IWX_LINK_AUTH) {
        if (len < 6) return -1;
        if (link_u16(body) || link_u16(body + 2) != 2) return 0;
        l->status.peer_status = link_u16(body + 4);
        if (l->status.peer_status) {
            l->status.state = IWX_LINK_FAILED; l->status.error = "AP rejected authentication";
        } else { l->status.state = IWX_LINK_ASSOC; l->request_sent = false; }
        return 1;
    }
    if (f[0] == 0x10 && l->status.state == IWX_LINK_ASSOC) {
        if (len < 6) return -1;
        unsigned status = link_u16(body + 2), aid = link_u16(body + 4);
        if (!status && ((link_u16(body) & 19) != (1U | (l->status.rsn ? 16U : 0U)) || (aid & 0xc000) != 0xc000 ||
                        !(aid & 0x3fff) || (aid & 0x3fff) > 2007)) return -1;
        for (unsigned pos = 6; pos < len;) {
            if (len - pos < 2 || body[pos + 1] > len - pos - 2) return -1;
            if (body[pos] == 48) {
                enum wpa2_cipher group;
                if (!l->status.rsn || wpa2_rsn_select(body + pos + 2, body[pos + 1], &group) ||
                    group != l->ap.rsn_ie[5]) return -1;
            }
            pos += 2 + body[pos + 1];
        }
        l->status.peer_status = status;
        if (status) { l->status.state = IWX_LINK_FAILED; l->status.error = "AP rejected association"; }
        else { l->status.aid = aid & 0x3fff; l->status.state = IWX_LINK_CONFIGURE; }
        return 1;
    }
    return 0;
}

const char *iwx_link_tx_build(iwx_tfh_tfd_t *t, uint8_t page[4096], uint64_t phys,
                             unsigned q, unsigned idx, uint32_t rate,
                             const uint8_t *frame, unsigned n, uint16_t *bc)
{
    if (!t || !page || !frame || !bc || !q || q >= IWX_TX_QUEUE_COUNT || idx >= 256 || n < 24 || n > IWX_DATA_MAX ||
        (phys & 4095) || !phys || phys > 0xfffff000ULL)
        return "invalid link TX descriptor input";
    bool data = frame[0] == 8;
    if (data ? ((frame[1] & ~0x40U) != 1 || n < 32) :
               ((frame[0] & 15) || (frame[1] & 0xc7) || n > 256))
        return "unsupported link TX frame";
    memset(t, 0, sizeof(*t)); memset(page, 0, 4096);
    page[0] = IWX_TX_FRAME; page[2] = idx;
    page[3] = q & IWX_TX_SEQUENCE_QUEUE_MASK; /* Full queue ID is in TX_RSP, not this tag. */
    link_put16(page + 4, n); link_put16(page + 6, 12 << 8);
    link_put32(page + 8, (frame[1] & 0x40) ? 1 : 3); /* Fixed legacy rate; firmware CCMP for protected data. */
    link_put32(page + 20, rate);
    memcpy(page + 24, frame, 24);
    if (n > 24) memcpy(page + 64, frame + 24, n - 24);
    t->num_tbs = n > 24 ? 3 : 2;
    t->tb[0] = (iwx_tfh_tb_t){20, phys};
    t->tb[1] = (iwx_tfh_tb_t){28, phys + 20};
    if (n > 24) t->tb[2] = (iwx_tfh_tb_t){n - 24, phys + 64};
    *bc = (n + 3) / 4; /* 32 filled descriptor bytes: one fetch chunk. */
    return NULL;
}
