#include "../drivers/iwx_data.c"
#include "../drivers/iwx_link.c"
#include "../drivers/iwx_scan.c"
#include "../drivers/iwx_fw.c"

extern int printf(const char *format, ...);
extern void *malloc(size_t n);
extern void free(void *p);
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static iwx_link_t link;
static iwx_data_t data;
static uint8_t packet[2048];
static const uint8_t local[6] = {2,0,0,0,0,1}, peer[6] = {2,1,2,3,4,5};

static void fixture(void)
{
    memset(&link, 0, sizeof(link)); memset(&data, 0, sizeof(data));
    memcpy(link.local, local, 6); memcpy(link.ap.bssid, peer, 6);
    link.status.state = IWX_LINK_ASSOCIATED;
    link.ap.channel = 36;
    for (unsigned i = 0; i < 5; i++) data.cipher[i] = WPA2_CIPHER_CCMP;
}

static unsigned make_rx(bool protect, bool pad, unsigned slot, unsigned type, uint64_t pn, unsigned length)
{
    memset(packet, 0, sizeof(packet));
    packet[3] = pad ? 0x20 : 0;
    link_put32(packet + 12, protect ? 0xa43 : 3);
    packet[34] = link.ap.channel;
    uint8_t *f = packet + 48;
    f[0] = 8; f[1] = 2 | (protect ? 0x40 : 0);
    memcpy(f + 4, local, 6); if (slot) memset(f + 4, 255, 6);
    memcpy(f + 10, peer, 6); memcpy(f + 16, peer, 6);
    f[22] = 0x50;
    unsigned header = 24;
    if (protect) {
        f[24] = pn; f[25] = pn >> 8; f[27] = 0x20 | (slot ? (slot - 1) << 6 : 0);
        for (unsigned i = 0; i < 4; i++) f[28 + i] = pn >> (16 + i * 8);
        header += 8;
    }
    if (pad) { f[header] = 0xab; f[header + 1] = 0xcd; header += 2; }
    memcpy(f + header, data_snap, 6);
    f[header + 6] = type >> 8; f[header + 7] = type;
    for (unsigned i = 0; i < length; i++) f[header + 8 + i] = i;
    if (type == 0x888e && length >= 4) {
        f[header + 8] = 2; f[header + 9] = 3;
        f[header + 10] = (length - 4) >> 8; f[header + 11] = length - 4;
    }
    link_put16(packet, header + 8 + length);
    return 48 + header + 8 + length;
}

static void test_keys(void)
{
    iwx_radio_profile_t p = {0};
    CHECK(iwx_key_profile_check(NULL) && iwx_key_profile_check(&p));
    p.command[IWX_ABI_STA_KEY] = 3;
    CHECK(iwx_key_profile_check(&p));
    p.seen = 1U << IWX_ABI_STA_KEY;
    CHECK(!iwx_key_profile_check(&p));
    for (unsigned v = 0; v < 256; v++) {
        p.command[IWX_ABI_STA_KEY] = v;
        CHECK((iwx_key_profile_check(&p) == NULL) == (v == 3));
    }
    uint8_t key[16], rsc[6] = {1,2,3,4,5,6};
    for (unsigned i = 0; i < 16; i++) key[i] = i + 1;
    iwx_key_cmd_t c;
    fixture();
    for (unsigned slot = 0; slot < 5; slot++) {
        memset(&c, 0xa5, sizeof(c));
        CHECK(!iwx_key_build(&c, slot, WPA2_CIPHER_CCMP, key, 16));
        CHECK(!c.station && c.offset == slot && !memcmp(c.key, key, 16));
        CHECK(c.flags == (slot ? (0x400a | ((slot - 1) << 8)) : 0xa));
        for (unsigned i = 20; i < sizeof(c); i++) CHECK(((uint8_t *)&c)[i] == 0);
        CHECK(iwx_data_key_commit(&data, slot, 5, WPA2_CIPHER_CCMP, rsc) == 1);
        CHECK(data.pn[slot] == 0x060504030201ULL && data.generation[slot] == 5);
        data.pn[slot]++;
        CHECK(iwx_data_key_commit(&data, slot, 5, WPA2_CIPHER_CCMP, rsc) == 0 && data.pn[slot] == 0x060504030202ULL);
        CHECK(iwx_data_key_commit(&data, slot, 4, WPA2_CIPHER_CCMP, rsc) == -1 && data.pn[slot] == 0x060504030202ULL);
        CHECK(iwx_data_key_commit(&data, slot, 6, WPA2_CIPHER_CCMP, rsc) == 1 && data.pn[slot] == 0x060504030201ULL);
    }
    iwx_key_cmd_t saved = c;
    CHECK(iwx_key_build(&c, 5, WPA2_CIPHER_CCMP, key, 16) == -1 && !memcmp(&c, &saved, sizeof(c)));
    CHECK(iwx_key_build(&c, 0, WPA2_CIPHER_CCMP, NULL, 16) == -1 && !memcmp(&c, &saved, sizeof(c)));
    CHECK(iwx_data_key_commit(&data, 5, 1, WPA2_CIPHER_CCMP, rsc) == -1);
    CHECK(iwx_data_key_commit(&data, 0, 0, WPA2_CIPHER_CCMP, rsc) == -1);
}

static unsigned make_tkip(bool pad, unsigned slot, uint64_t pn, unsigned length)
{
    unsigned size = make_rx(true, pad, slot, 0x0800, pn, length);
    link_put32(packet + 12, 0xb6f);
    packet[72] = pn >> 8; packet[73] = ((pn >> 8) | 0x20) & 0x7f; packet[74] = pn;
    return size;
}

static void test_tkip(void)
{
    uint8_t gtk[32], rsc[6] = {1,2,3,4,5,6};
    for (unsigned i = 0; i < sizeof(gtk); i++) gtk[i] = i + 7;
    iwx_key_cmd_t c, saved;
    for (unsigned slot = 1; slot < 5; slot++) {
        fixture();
        CHECK(!iwx_key_build(&c, slot, WPA2_CIPHER_TKIP, gtk, 32));
        CHECK(c.flags == (0x400b | ((slot - 1) << 8)) && c.offset == slot && !c.station);
        CHECK(!memcmp(c.key, gtk, 16) && !memcmp(c.key + 16, gtk + 24, 8) && !memcmp(c.key + 24, gtk + 16, 8));
        CHECK(!memcmp(&c.rx_mic, gtk + 16, 8) && !memcmp(&c.tx_mic, gtk + 24, 8));
        for (unsigned i = 0; i < 16; i++) CHECK(!c.receive_counter[i]);
        CHECK(!c.transmit_counter);
        CHECK(iwx_data_key_commit(&data, slot, 7, WPA2_CIPHER_TKIP, rsc) == 1);
        CHECK(data.cipher[slot] == WPA2_CIPHER_TKIP && data.pn[slot] == 0x060504030201ULL);
        data.pn[slot]++;
        CHECK(!iwx_data_key_commit(&data, slot, 7, WPA2_CIPHER_TKIP, rsc) && data.pn[slot] == 0x060504030202ULL);
        CHECK(iwx_data_key_commit(&data, slot, 7, WPA2_CIPHER_CCMP, rsc) == -1);
        CHECK(iwx_data_key_commit(&data, slot, 8, WPA2_CIPHER_CCMP, rsc) == -1);
    }
    saved = c;
    for (unsigned len = 0; len <= 40; len++) if (len != 32)
        CHECK(iwx_key_build(&c, 1, WPA2_CIPHER_TKIP, gtk, len) == -1 && !memcmp(&c, &saved, sizeof(c)));
    CHECK(iwx_key_build(&c, 0, WPA2_CIPHER_TKIP, gtk, 32) == -1 && !memcmp(&c, &saved, sizeof(c)));
    CHECK(iwx_data_key_commit(&data, 0, 7, WPA2_CIPHER_TKIP, rsc) == -1);
    iwx_data_rx_t out, sentinel; memset(&sentinel, 0xa5, sizeof(sentinel));
    iwx_rx_sample_t d;
    for (unsigned pad = 0; pad < 2; pad++) for (unsigned slot = 1; slot < 5; slot++)
    for (unsigned length = 0; length <= 1500; length++) {
        fixture(); link.status.rsn = link.status.port_open = true;
        data.generation[slot] = 1; data.cipher[slot] = WPA2_CIPHER_TKIP;
        unsigned size = make_tkip(pad, slot, 0x060504030201ULL, length);
        unsigned trailer = length & 1 ? 4 : 0;
        packet[2] = trailer << 3; link_put16(packet, size - 48 + trailer);
        memset(packet + size, 0xa5, trailer);
        CHECK(iwx_data_receive_diag(&data, &link, packet, size + trailer, &out, &d, IWX_RX_RADA) == 1);
        CHECK(out.length == 14 + length && data.pn[slot] == 0x060504030201ULL && !out.eapol);
        CHECK(!memcmp(out.ethernet + 14, packet + size - length, length));
        out = sentinel;
        CHECK(!iwx_data_receive_diag(&data, &link, packet, size + trailer, &out, &d, IWX_RX_RADA));
        CHECK(d.reason == IWX_RX_REPLAY && !memcmp(&out, &sentinel, sizeof(out)));
    }
    for (unsigned bad = 0; bad < 12; bad++) {
        fixture(); link.status.rsn = link.status.port_open = true;
        data.generation[2] = 1; data.cipher[2] = WPA2_CIPHER_TKIP;
        unsigned size = make_tkip(true, 2, 1, 40), flags = IWX_RX_RADA;
        switch (bad) {
        case 0: packet[12] &= ~0x20; break; /* ICV */
        case 1: packet[12] &= ~0x40; break; /* Michael, only this generates a report. */
        case 2: packet[12] &= ~8; break; /* no matching hardware key */
        case 3: packet[13] &= ~8; break; /* not decrypted */
        case 4: packet[73] ^= 1; break; /* WEP seed octet */
        case 5: packet[75] &= ~0x20; break;
        case 6: data.cipher[2] = WPA2_CIPHER_CCMP; break;
        case 7: flags = 0; break;
        case 8: memcpy(packet + 52, local, 6); break;
        case 9: packet[49] |= 4; break; /* fragmented MSDU */
        case 10: packet[48] = 0x88; break; /* no QoS negotiated */
        case 11: data.generation[2] = 0; break;
        }
        out = sentinel;
        CHECK(iwx_data_receive_diag(&data, &link, packet, size, &out, &d, flags) <= 0);
        CHECK(!memcmp(&out, &sentinel, sizeof(out)) && !data.pn[2]);
        CHECK((d.reason == IWX_RX_MIC) == (bad == 1));
    }
    fixture(); link.status.rsn = link.status.port_open = true;
    data.generation[2] = 1; data.cipher[2] = WPA2_CIPHER_TKIP;
    unsigned size = make_tkip(false, 2, IWX_CCMP_PN_MAX, 40);
    CHECK(iwx_data_receive_diag(&data, &link, packet, size, &out, &d, IWX_RX_RADA) == 1);
    size = make_tkip(false, 2, 0, 40);
    CHECK(!iwx_data_receive_diag(&data, &link, packet, size, &out, &d, IWX_RX_RADA) && d.reason == IWX_RX_REPLAY);
    size = make_tkip(false, 2, 1, 40); data.pn[2] = 0;
    for (unsigned len = 0; len < size; len++) {
        uint8_t *short_packet = malloc(len ? len : 1);
        CHECK(short_packet != NULL); if (!short_packet) return;
        memcpy(short_packet, packet, len); out = sentinel;
        CHECK(iwx_data_receive_diag(&data, &link, short_packet, len, &out, &d, IWX_RX_RADA) < 0);
        CHECK(!memcmp(&out, &sentinel, sizeof(out)) && !data.pn[2]);
        free(short_packet);
    }
}

static void test_receive(void)
{
    iwx_data_rx_t out;
    for (unsigned protect = 0; protect < 2; protect++) for (unsigned pad = 0; pad < 2; pad++)
    for (unsigned slot = 0; slot < 5; slot++) for (unsigned length = 0; length <= 1500; length++) {
        fixture(); link.status.rsn = protect; link.status.port_open = true;
        data.generation[slot] = 1;
        unsigned size = make_rx(protect, pad, slot, 0x0800, 0x060504030201ULL, length);
        CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 1);
        CHECK(out.length == 14 + length && !out.eapol && out.ethernet[12] == 8 && out.ethernet[13] == 0);
        CHECK(!memcmp(out.ethernet, packet + 52, 6) && !memcmp(out.ethernet + 6, peer, 6));
        CHECK(!memcmp(out.ethernet + 14, packet + size - length, length));
        if (protect) CHECK(data.pn[slot] == 0x060504030201ULL);
    }
    fixture(); link.status.rsn = true; link.status.port_open = true; data.generation[0] = 1;
    unsigned size = make_rx(true, true, 0, 0x0800, IWX_CCMP_PN_MAX, 40);
    CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 1);
    CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 0);
    make_rx(true, true, 0, 0x0800, 0, 40);
    CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 0 && data.pn[0] == IWX_CCMP_PN_MAX);

    const enum iwx_rx_reason reasons[] = {
        IWX_RX_FORMAT, IWX_RX_FORMAT, IWX_RX_FRAGMENT, IWX_RX_STATUS, IWX_RX_DESTINATION,
        IWX_RX_PEER, IWX_RX_FORMAT, IWX_RX_FORMAT, IWX_RX_IV, IWX_RX_IV, IWX_RX_IV,
        IWX_RX_CCMP, IWX_RX_CCMP, IWX_RX_CCMP, IWX_RX_CHANNEL, IWX_RX_AMSDU,
        IWX_RX_REPLAY, IWX_RX_PORT, IWX_RX_LLC, IWX_RX_DESTINATION, IWX_RX_CCMP,
        IWX_RX_STATE, IWX_RX_LLC
    };
    for (unsigned bad = 0; bad < 23; bad++) {
        fixture(); link.status.rsn = link.status.port_open = true; data.generation[0] = 1;
        size = make_rx(true, true, 0, 0x0800, 1, 40);
        switch (bad) {
        case 0: packet[48] = 0x88; break; /* no QoS negotiated */
        case 1: packet[49] |= 4; break;
        case 2: packet[70] |= 1; break;
        case 3: packet[12] &= ~1; break;
        case 4: packet[52] ^= 2; break;
        case 5: packet[58] ^= 2; break;
        case 6: packet[49] |= 1; break;
        case 7: packet[49] |= 0x80; break;
        case 8: packet[74] = 1; break;
        case 9: packet[75] &= ~0x20; break;
        case 10: packet[75] |= 0x40; break;
        case 11: packet[12] &= ~0x40; break;
        case 12: packet[13] &= ~8; break;
        case 13: packet[13] ^= 1; break;
        case 14: packet[34]++; break;
        case 15: packet[3] |= 0x40; break;
        case 16: data.generation[0] = 0; break;
        case 17: link.status.port_open = false; break;
        case 18: packet[82] = 0; break; /* SNAP */
        case 19: packet[64] |= 1; break;
        case 20: link.status.rsn = false; break;
        case 21: link.status.state = IWX_LINK_AUTH; break;
        case 22: packet[88] = 1; break; /* length instead of EtherType */
        }
        memset(&out, 0xa5, sizeof(out)); iwx_data_rx_t saved = out;
        CHECK(iwx_data_receive(&data, &link, packet, size, &out) <= 0);
        CHECK(!memcmp(&out, &saved, sizeof(out)) && !data.pn[0]);
        iwx_rx_sample_t d;
        CHECK(iwx_data_receive_diag(&data, &link, packet, size, &out, &d, 0) <= 0);
        CHECK(d.reason == reasons[bad] && !memcmp(&out, &saved, sizeof(out)) && !data.pn[0]);
    }
    fixture(); link.status.rsn = link.status.port_open = true; data.generation[0] = 1;
    size = make_rx(true, true, 0, 0x0800, 1, 1500);
    for (unsigned n = 0; n < size; n++) CHECK(iwx_data_receive(&data, &link, packet, n, &out) < 0);
    size = make_rx(true, true, 0, 0x0800, 1, 1501);
    CHECK(iwx_data_receive(&data, &link, packet, size, &out) < 0);
    fixture(); link.status.port_open = true;
    size = make_rx(false, false, 0, 0x0800, 0, 40);
    CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 1);
    packet[49] |= 8;
    CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 0);
    packet[70] += 16;
    CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 1);
}

static void test_diagnostics(void)
{
    fixture();
    iwx_data_rx_t out, saved;
    memset(&out, 0xa5, sizeof(out)); saved = out;
    unsigned size = make_rx(false, false, 0, 0x888e, 0, 99);
    uint8_t *f = packet + 48;
    f[36] = 2; f[37] = 0x13; f[38] = 0xca;
    iwx_rx_sample_t d;
    CHECK(iwx_data_receive_diag(&data, &link, packet, size, &out, &d, 0) == 1);
    CHECK(d.reason == IWX_RX_ACCEPTED && d.ether_type == 0x888e && d.key_info == 0x13ca);
    CHECK(d.header_bytes == 24 && d.eapol_bytes == 95 && d.addresses == 7 && d.packet_type == 3);
    for (unsigned n = 0; n < size; n++) {
        uint8_t *short_packet = malloc(n ? n : 1);
        CHECK(short_packet != NULL);
        if (!short_packet) return;
        memcpy(short_packet, packet, n);
        memset(&d, 0xa5, sizeof(d)); out = saved;
        CHECK(iwx_data_receive_diag(&data, &link, short_packet, n, &out, &d, 0) == -1);
        CHECK(d.reason == IWX_RX_BOUNDS && !memcmp(&out, &saved, sizeof(out)));
        free(short_packet);
    }
    /* A QoS M3 candidate is observable, but still not accepted by a non-QoS link. */
    for (unsigned i = size - 48; i > 24; i--) f[i + 1] = f[i - 1];
    f[0] = 0x88; f[24] = f[25] = 0; size += 2; link_put16(packet, size - 48);
    CHECK(iwx_data_receive_diag(&data, &link, packet, size, &out, &d, 0) == 0);
    CHECK(d.reason == IWX_RX_FORMAT && d.ether_type == 0x888e && d.key_info == 0x13ca && d.header_bytes == 26);
    CHECK(!memcmp(&out, &saved, sizeof(out)));
    /* Protected data must not export ciphertext as guessed EAPOL metadata. */
    f[1] |= 0x40;
    CHECK(iwx_data_receive_diag(&data, &link, packet, size, &out, &d, 0) == 0);
    CHECK(!d.ether_type && !d.key_info && !d.version && !d.eapol_bytes);

    for (unsigned pad = 0; pad < 2; pad++) for (unsigned slot = 0; slot < 5; slot++) {
        fixture(); link.status.rsn = link.status.port_open = true;
        data.generation[slot] = 1; data.pn[slot] = 91;
        size = make_rx(true, pad, slot, 0x0800, 91, 36);
        uint8_t *ip = packet + size - 36;
        memset(ip, 0, 36); ip[0] = 0x45; ip[3] = 36; ip[9] = 17;
        ip[21] = 67; ip[23] = 68; ip[25] = 16;
        ip[32] = 0xa1; ip[33] = 0xb2; ip[34] = 0xc3; ip[35] = 0xd4;
        out = saved;
        CHECK(iwx_data_receive_diag(&data, &link, packet, size, &out, &d, 0) == 0);
        CHECK(d.reason == IWX_RX_REPLAY && d.pn == 91 && d.previous_pn == 91 && d.key_slot == slot);
        CHECK(d.udp_source == 67 && d.udp_destination == 68 && d.dhcp_xid == 0xa1b2c3d4);
        CHECK(d.sequence == 0x50 && d.header_bytes == 32 + pad * 2);
        CHECK(data.pn[slot] == 91 && !memcmp(&out, &saved, sizeof(out)));
        packet[12] &= ~0x40;
        CHECK(iwx_data_receive_diag(&data, &link, packet, size, &out, &d, 0) == 0);
        CHECK(d.reason == IWX_RX_CCMP && !d.udp_source && !d.dhcp_xid && !d.pn);
        packet[12] |= 0x40;
        data.pn[slot] = 90;
        CHECK(iwx_data_receive_diag(&data, &link, packet, size, &out, &d, 0) == 1);
        CHECK(d.reason == IWX_RX_ACCEPTED && d.previous_pn == 90 && data.pn[slot] == 91);
        CHECK(d.udp_source == 67 && d.dhcp_xid == 0xa1b2c3d4);
    }
    /* Diagnostic parsing does not make L3 admission decisions, and never reads
     * beyond the declared LLC/IP/UDP extents even on replayed packets. */
    uint8_t llc[44] = {0xaa,0xaa,3,0,0,0,8,0,0x45,0,0,36};
    llc[17] = 17; llc[29] = 68; llc[31] = 67; llc[33] = 16;
    llc[43] = 9;
    for (unsigned n = 0; n < sizeof(llc); n++) {
        uint8_t *short_llc = malloc(n ? n : 1);
        CHECK(short_llc != NULL);
        if (!short_llc) return;
        memcpy(short_llc, llc, n); memset(&d, 0, sizeof(d));
        data_payload_header(&d, short_llc, n);
        CHECK(!d.udp_source && !d.udp_destination && !d.dhcp_xid);
        free(short_llc);
    }
    for (unsigned bad = 0; bad < 9; bad++) {
        uint8_t b[44]; memcpy(b, llc, sizeof(b));
        switch (bad) {
        case 0: b[8] = 0x65; break;
        case 1: b[8] = 0x44; break;
        case 2: b[8] = 0x4f; break;
        case 3: b[11] = 27; break;
        case 4: b[11] = 37; break;
        case 5: b[14] = 0x20; break;
        case 6: b[17] = 6; break;
        case 7: b[33] = 7; break;
        case 8: b[33] = 17; break;
        }
        memset(&d, 0, sizeof(d)); data_payload_header(&d, b, sizeof(b));
        CHECK(!d.udp_source && !d.udp_destination && !d.dhcp_xid);
    }
    memset(&d, 0, sizeof(d)); data_payload_header(&d, llc, sizeof(llc));
    CHECK(d.udp_source == 68 && d.udp_destination == 67 && d.dhcp_xid == 9);
}

static void test_eapol(void)
{
    iwx_data_rx_t out;
    for (unsigned protect = 0; protect < 2; protect++) for (unsigned pad = 0; pad < 2; pad++) {
        fixture(); link.status.rsn = true; data.generation[0] = 1;
        unsigned size = make_rx(protect, pad, 0, 0x888e, 1, 99);
        CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 1 && out.eapol && out.length == 113);
        CHECK(!link.status.port_open);
        data.pn[0] = 0; link.status.state = IWX_LINK_CONFIGURE;
        CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 1);
        data.pn[0] = 0; packet[64] ^= 2;
        CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 0);
        packet[64] ^= 2; memset(packet + 52, 255, 6);
        CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 0);
    }
    fixture(); unsigned size = make_rx(false, false, 0, 0x888e, 0, 99);
    packet[83]++; /* declared body exceeds bytes available */
    CHECK(iwx_data_receive(&data, &link, packet, size, &out) == -1);
    packet[83] -= 2;
    CHECK(iwx_data_receive(&data, &link, packet, size, &out) == 1 && out.length == 112);
}

static void test_transmit(void)
{
    uint8_t ethernet[IWX_ETH_MAX], f[IWX_DATA_MAX + 1], page[4096];
    iwx_tfh_tfd_t t;
    uint16_t bc;
    memset(ethernet, 0, sizeof(ethernet));
    memcpy(ethernet, peer, 6); memcpy(ethernet + 6, local, 6); ethernet[12] = 8;
    for (unsigned protect = 0; protect < 2; protect++) for (unsigned n = 14; n <= IWX_ETH_MAX; n++) {
        fixture(); link.status.rsn = protect; link.status.port_open = true; data.generation[0] = 1;
        memset(f, 0xa5, sizeof(f));
        CHECK(iwx_data_frame(&data, &link, ethernet, n, protect, f, sizeof(f) - 1) == (int)n + 18);
        CHECK(f[n + 18] == 0xa5 && f[0] == 8 && f[1] == (1 | (protect ? 0x40 : 0)));
        CHECK(!memcmp(f + 4, peer, 6) && !memcmp(f + 10, local, 6) && !memcmp(f + 16, peer, 6));
        CHECK(!memcmp(f + 24, data_snap, 6) && !memcmp(f + 30, ethernet + 12, n - 12));
        CHECK(!iwx_link_tx_build(&t, page, 0x10000000, 7, 255, 0x4000, f, n + 18, &bc));
        CHECK(link_u32(page + 8) == (protect ? 1U : 3U) && link_u16(page + 4) == n + 18);
        CHECK(bc == (n + 21) / 4 && t.num_tbs == 3 && t.tb[2].length == n - 6);
        CHECK(!memcmp(page + 24, f, 24) && !memcmp(page + 64, f + 24, n - 6));
    }
    fixture(); link.status.rsn = true;
    CHECK(iwx_data_frame(&data, &link, ethernet, 50, true, f, sizeof(f)) == -1);
    data.generation[0] = 1;
    CHECK(iwx_data_frame(&data, &link, ethernet, 50, true, f, sizeof(f)) == -1); /* port closed */
    ethernet[12] = 0x88; ethernet[13] = 0x8e;
    CHECK(iwx_data_frame(&data, &link, ethernet, 50, false, f, sizeof(f)) == 68);
    CHECK(iwx_data_frame(&data, &link, ethernet, 50, true, f, sizeof(f)) == 68);
    ethernet[0] ^= 2;
    CHECK(iwx_data_frame(&data, &link, ethernet, 50, false, f, sizeof(f)) == -1);
    ethernet[0] ^= 2;
    CHECK(iwx_data_frame(&data, &link, ethernet, 50, true, f, 67) == -1);
    CHECK(iwx_data_frame(&data, &link, ethernet, IWX_ETH_MAX + 1, true, f, sizeof(f)) == -1);
}

int main(void)
{
    test_keys(); test_receive(); test_diagnostics(); test_eapol(); test_transmit(); test_tkip();
    printf("iwx CCMP/TKIP data ABI: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
