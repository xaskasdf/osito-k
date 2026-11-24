#include "iwx_data.h"

static uint16_t data_le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint16_t data_be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t data_le32(const uint8_t *p) { return data_le16(p) | ((uint32_t)data_le16(p + 2) << 16); }
static uint64_t data_pn(const uint8_t *p)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < 6; i++) n |= (uint64_t)p[i] << (8 * i);
    return n;
}
static const uint8_t data_snap[6] = {0xaa, 0xaa, 3, 0, 0, 0};

const char *iwx_key_profile_check(const iwx_radio_profile_t *p)
{
    if (!p || !(p->seen & (1U << IWX_ABI_STA_KEY)) || p->command[IWX_ABI_STA_KEY] != 3)
        return "WPA2 requires ADD_STA_KEY v3";
    return NULL;
}

int iwx_key_build(iwx_key_cmd_t *c, unsigned slot, enum wpa2_cipher cipher,
                    const uint8_t *key, unsigned key_len)
{
    if (!c || !key || slot >= 5 ||
        (cipher != WPA2_CIPHER_CCMP && cipher != WPA2_CIPHER_TKIP) ||
        (!slot && cipher != WPA2_CIPHER_CCMP) ||
        key_len != (cipher == WPA2_CIPHER_TKIP ? 32U : 16U)) return -1;
    memset(c, 0, sizeof(*c));
    c->offset = slot;
    c->flags = (cipher == WPA2_CIPHER_TKIP ? 3 : 2) | (1U << 3);
    if (slot) c->flags |= (1U << 14) | ((slot - 1) << 8);
    memcpy(c->key, key, 16);
    if (cipher == WPA2_CIPHER_TKIP) {
        /* GTK KDE order is AP TX MIC then AP RX MIC; reverse for this STA. */
        memcpy(c->key + 16, key + 24, 8);
        memcpy(c->key + 24, key + 16, 8);
        memcpy(&c->rx_mic, key + 16, 8);
        memcpy(&c->tx_mic, key + 24, 8);
    }
    /* Like iwx(4), keep RX replay checking in the host. Firmware owns TX PN;
     * only a NEW key is installed with TX PN zero. No repeated installation. */
    return 0;
}

int iwx_data_key_commit(iwx_data_t *s, unsigned slot, uint32_t generation,
                         enum wpa2_cipher cipher, const uint8_t rsc[6])
{
    if (!s || slot >= 5 || !generation || !rsc || generation < s->generation[slot] ||
        (cipher != WPA2_CIPHER_CCMP && cipher != WPA2_CIPHER_TKIP) ||
        (!slot && cipher != WPA2_CIPHER_CCMP) ||
        (s->generation[slot] && s->cipher[slot] != cipher)) return -1;
    if (generation == s->generation[slot]) return 0;
    s->generation[slot] = generation;
    s->cipher[slot] = cipher;
    s->pn[slot] = data_pn(rsc);
    return 1;
}

static int data_result(iwx_rx_sample_t *d, enum iwx_rx_reason reason, int rc)
{
    if (d) d->reason = reason;
    return rc;
}

static void data_payload_header(iwx_rx_sample_t *d, const uint8_t *llc, unsigned n)
{
    if (!d || n < 8 || memcmp(llc, data_snap, 6)) return;
    d->ether_type = data_be16(llc + 6);
    if (d->ether_type == 0x0800 && n >= 28) {
        const uint8_t *ip = llc + 8;
        unsigned ihl = (ip[0] & 15) * 4, total = data_be16(ip + 2);
        if ((ip[0] >> 4) != 4 || ihl < 20 || total < ihl + 8 || total > n - 8 ||
            (data_be16(ip + 6) & 0x3fff) || ip[9] != 17) return;
        const uint8_t *udp = ip + ihl;
        unsigned length = data_be16(udp + 4);
        if (length < 8 || length > total - ihl) return;
        d->udp_source = data_be16(udp); d->udp_destination = data_be16(udp + 2);
        if (length >= 16 && ((d->udp_source == 67 && d->udp_destination == 68) ||
                            (d->udp_source == 68 && d->udp_destination == 67)))
            d->dhcp_xid = ((uint32_t)data_be16(udp + 12) << 16) | data_be16(udp + 14);
        return;
    }
    if (d->ether_type != 0x888e || n < 12) return;
    d->version = llc[8]; d->packet_type = llc[9];
    d->eapol_bytes = data_be16(llc + 10);
    if (d->packet_type == 3 && n >= 15 && d->eapol_bytes >= 3)
        d->key_info = data_be16(llc + 13);
}

int iwx_data_receive_diag(iwx_data_t *s, const iwx_link_t *l, const uint8_t *p,
                           unsigned size, iwx_data_rx_t *out, iwx_rx_sample_t *d,
                           uint32_t packet_flags)
{
    if (d) memset(d, 0, sizeof(*d));
    if (!s || !l || !p || !out || size < 48) return data_result(d, IWX_RX_BOUNDS, -1);
    unsigned n = data_le16(p);
    if (n < 24 || n > size - 48) return data_result(d, IWX_RX_BOUNDS, -1);
    uint32_t status = data_le32(p + 12);
    const uint8_t *f = p + 48;
    if (d) {
        d->status = status; d->frame_control = data_le16(f); d->frame_bytes = n;
        d->sequence = data_le16(f + 22);
        d->channel = p[34]; d->flags = p[3];
        d->addresses = (!memcmp(f + 4, l->local, 6) ? 1 : 0) |
            (!memcmp(f + 10, l->ap.bssid, 6) ? 2 : 0) |
            (!memcmp(f + 16, l->ap.bssid, 6) ? 4 : 0) | ((f[4] & 1) ? 8 : 0) |
            (!memcmp(f + 16, l->local, 6) ? 16 : 0) | (!memcmp(f + 16, data_snap, 6) ? 32 : 0);
        /* Observe plaintext QoS candidates too, without accepting them. Skip
         * encrypted/aggregated bodies; never reinterpret ciphertext as LLC. */
        unsigned header = f[0] == 0x88 ? 26 : 24;
        if (f[1] & 0x80) header += 4;
        unsigned padded = header + ((p[3] & 0x20) ? 2 : 0);
        if ((f[0] == 8 || f[0] == 0x88) && (f[1] & 0x43) == 2 &&
            !(p[3] & 0x40) && n >= padded && (f[0] != 0x88 || !(f[24] & 0x80))) {
            d->header_bytes = padded;
            data_payload_header(d, f + padded, n - padded);
        }
    }
    if (l->status.state < IWX_LINK_CONFIGURE || l->status.state > IWX_LINK_ASSOCIATED)
        return data_result(d, IWX_RX_STATE, 0);
    if ((status & 3) != 3) return data_result(d, IWX_RX_STATUS, 0);
    if (p[34] != l->ap.channel) return data_result(d, IWX_RX_CHANNEL, 0);
    if (p[3] & 0x40) return data_result(d, IWX_RX_AMSDU, 0);
    if (f[0] != 8 || (f[1] & 0x87) != 2) return data_result(d, IWX_RX_FORMAT, 0);
    if (data_le16(f + 22) & 15) return data_result(d, IWX_RX_FRAGMENT, 0);
    if (memcmp(f + 10, l->ap.bssid, 6)) return data_result(d, IWX_RX_PEER, 0);
    bool group = (f[4] & 1) != 0, protect = (f[1] & 0x40) != 0;
    if ((!group && memcmp(f + 4, l->local, 6)) || (f[16] & 1))
        return data_result(d, IWX_RX_DESTINATION, 0);
    unsigned header = 24, slot = 0;
    bool tkip = false;
    uint64_t pn = 0;
    if (protect) {
        tkip = group && (status & 0x700) == 0x300;
        if (!l->status.rsn || (!tkip && ((status & 0x700) != 0x200 || (status & 0x840) != 0x840)))
            return data_result(d, IWX_RX_CCMP, 0);
        if (tkip && ((status & 0x82c) != 0x82c)) return data_result(d, IWX_RX_TKIP, 0);
        /* AX200 Gen2 validates Michael. Only consume its stripped RADA output;
         * never guess whether a MIC/ICV trailer is ciphertext or payload. */
        if (tkip && !(packet_flags & IWX_RX_RADA)) return data_result(d, IWX_RX_OFFLOAD, 0);
        if (n < 32) return data_result(d, IWX_RX_BOUNDS, -1);
        const uint8_t *iv = f + 24;
        if ((tkip ? iv[1] != ((iv[0] | 0x20) & 0x7f) : iv[2] != 0) ||
            (iv[3] & 0x3f) != 0x20) return data_result(d, IWX_RX_IV, 0);
        unsigned id = iv[3] >> 6;
        if (!group && id) return data_result(d, IWX_RX_IV, 0);
        slot = group ? id + 1 : 0;
        if (s->generation[slot] && s->cipher[slot] != (tkip ? WPA2_CIPHER_TKIP : WPA2_CIPHER_CCMP))
            return data_result(d, IWX_RX_TKIP, 0);
        pn = (tkip ? iv[2] | ((uint64_t)iv[0] << 8) : iv[0] | ((uint64_t)iv[1] << 8)) |
             ((uint64_t)iv[4] << 16) |
             ((uint64_t)iv[5] << 24) | ((uint64_t)iv[6] << 32) | ((uint64_t)iv[7] << 40);
        if (d) {
            d->pn = pn; d->previous_pn = s->pn[slot]; d->key_slot = slot;
            unsigned padded = 32 + ((p[3] & 0x20) ? 2 : 0);
            if (n >= padded && (status & 0x40)) {
                d->header_bytes = padded;
                /* Inspect authenticated firmware plaintext even on replay drops,
                 * without accepting it or advancing the replay window. */
                data_payload_header(d, f + padded, n - padded);
            }
        }
        if (!s->generation[slot] || pn <= s->pn[slot]) return data_result(d, IWX_RX_REPLAY, 0);
        if (tkip && !(status & 0x40)) return data_result(d, IWX_RX_MIC, 0);
        header += 8;
    } else if (status & 0x700) return data_result(d, IWX_RX_PLAIN_CIPHER, 0);
    /* AX200's two pad bytes follow the MAC header AND CCMP IV. Its CCMP
     * notification retains the IV, strips MIC, and supplies authenticated data. */
    if (p[3] & 0x20) header += 2;
    if (tkip) {
        unsigned trailer = (p[2] & 0xf0) >> 3;
        if (trailer > n) return data_result(d, IWX_RX_BOUNDS, -1);
        n -= trailer;
    }
    if (n < header + 8) return data_result(d, IWX_RX_BOUNDS, -1);
    const uint8_t *llc = f + header, *body = llc + 8;
    unsigned type = data_be16(llc + 6), length = n - header - 8;
    if (d) d->header_bytes = header;
    data_payload_header(d, llc, n - header);
    if (memcmp(llc, data_snap, sizeof(data_snap)) || type < 0x600)
        return data_result(d, IWX_RX_LLC, 0);
    bool eapol = type == 0x888e;
    if (eapol) {
        if (group || memcmp(f + 16, l->ap.bssid, 6)) return data_result(d, IWX_RX_EAPOL_ADDRESS, 0);
        if (length < 4 || data_be16(body + 2) > length - 4) return data_result(d, IWX_RX_BOUNDS, -1);
        length = 4 + data_be16(body + 2); /* EAPOL length excludes Ethernet padding. */
    } else if (!l->status.port_open || (l->status.rsn && !protect)) return data_result(d, IWX_RX_PORT, 0);
    if (length > 1500) return data_result(d, IWX_RX_BOUNDS, -1);
    uint16_t seq = data_le16(f + 22);
    if (!protect && !group && !eapol && (f[1] & 8) && s->have_sequence && seq == s->last_sequence)
        return data_result(d, IWX_RX_DUPLICATE, 0);
    memcpy(out->ethernet, f + 4, 6);
    memcpy(out->ethernet + 6, f + 16, 6);
    memcpy(out->ethernet + 12, llc + 6, 2);
    memcpy(out->ethernet + 14, body, length);
    out->length = length + 14;
    out->eapol = eapol;
    if (protect) s->pn[slot] = pn;
    else if (!group && !eapol) { s->last_sequence = seq; s->have_sequence = true; }
    return data_result(d, IWX_RX_ACCEPTED, 1);
}

int iwx_data_receive(iwx_data_t *s, const iwx_link_t *l, const uint8_t *p,
                      unsigned size, iwx_data_rx_t *out)
{
    return iwx_data_receive_diag(s, l, p, size, out, NULL, 0);
}

int iwx_data_frame(iwx_data_t *s, iwx_link_t *l, const uint8_t *e,
                    unsigned n, bool protect, uint8_t *out, unsigned capacity)
{
    if (!s || !l || !e || !out || n < 14 || n > IWX_ETH_MAX || capacity < n + 18 ||
        l->status.state != IWX_LINK_ASSOCIATED || memcmp(e + 6, l->local, 6)) return -1;
    unsigned type = data_be16(e + 12);
    bool eapol = type == 0x888e;
    if (type < 0x600 || (protect && (!l->status.rsn || !s->generation[0])) ||
        (eapol && memcmp(e, l->ap.bssid, 6)) ||
        (!eapol && (!l->status.port_open || protect != l->status.rsn))) return -1;
    memset(out, 0, 24);
    out[0] = 8; out[1] = 1 | (protect ? 0x40 : 0); /* To DS, non-QoS. */
    memcpy(out + 4, l->ap.bssid, 6);
    memcpy(out + 10, l->local, 6);
    memcpy(out + 16, e, 6);
    uint16_t seq = (l->sequence++ & 0xfff) << 4;
    out[22] = seq; out[23] = seq >> 8;
    memcpy(out + 24, data_snap, sizeof(data_snap));
    memcpy(out + 30, e + 12, n - 12);
    return n + 18; /* Firmware inserts CCMP IV/MIC and increments its TX PN. */
}
