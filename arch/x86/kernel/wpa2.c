#include "wpa2.h"
#include "wpa2_rsn.h"
#include "random_secure.h"
#include <string.h>

static unsigned be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static void put16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }

void wpa2_reset(wpa2_t *s) { if (s) wpa2_clear(s, sizeof(*s)); }

static int fail(wpa2_t *s, const char *reason)
{
    wpa2_reset(s);
    s->state = WPA2_FAILED; s->error = reason;
    return -1;
}

int wpa2_start(wpa2_t *s, const uint8_t pmk[32], const uint8_t local[6],
               const uint8_t peer[6], const uint8_t *ap_rsn, unsigned rsn_len, uint64_t now)
{
    static const uint8_t zero[6] = {0};
    if (!s || s->state != WPA2_OFF) return -1;
    enum wpa2_cipher group;
    if (!pmk || !local || !peer || wpa2_rsn_select(ap_rsn, rsn_len, &group) ||
        (local[0] & 1) || (peer[0] & 1) || !memcmp(local, peer, 6) ||
        !memcmp(local, zero, 6) || !memcmp(peer, zero, 6)) return -1;
    if (random_secure_bytes(s->snonce, 32)) return fail(s, "cryptographic random source unavailable");
    memcpy(s->pmk, pmk, 32); memcpy(s->local, local, 6); memcpy(s->peer, peer, 6);
    memcpy(s->ap_rsn, ap_rsn, rsn_len); s->ap_rsn_len = rsn_len;
    s->group_cipher = group; s->group_len = group == WPA2_CIPHER_TKIP ? 32 : 16;
    wpa2_rsn_assoc(s->assoc_rsn, ap_rsn, rsn_len);
    s->started = now; s->state = WPA2_WAIT_M1;
    return 0;
}

static void mic(const uint8_t key[16], const uint8_t *frame, unsigned n, uint8_t out[20])
{
    uint8_t copy[WPA2_EAPOL_MAX];
    memcpy(copy, frame, n); memset(copy + 81, 0, 16);
    wpa2_hmac_sha1(key, 16, copy, n, out);
    wpa2_clear(copy, sizeof(copy));
}

static void reply(wpa2_t *s, enum wpa2_tx_kind kind, const uint8_t *request)
{
    unsigned data_len = kind == WPA2_TX_M2 ? sizeof(wpa2_assoc_rsn) : 0;
    memset(s->tx, 0, sizeof(s->tx));
    s->tx[0] = request[0]; s->tx[1] = 3; s->tx[4] = 2;
    put16(s->tx + 2, 95 + data_len);
    put16(s->tx + 5, kind == WPA2_TX_M2 ? 0x010a : kind == WPA2_TX_M4 ? 0x030a : 0x0302);
    memcpy(s->tx + 9, request + 9, 8);
    if (kind == WPA2_TX_M2) memcpy(s->tx + 17, s->snonce, 32);
    put16(s->tx + 97, data_len);
    if (data_len) memcpy(s->tx + 99, s->assoc_rsn, data_len);
    uint8_t digest[20];
    s->tx_len = 99 + data_len;
    mic(s->ptk, s->tx, s->tx_len, digest);
    memcpy(s->tx + 81, digest, 16); wpa2_clear(digest, sizeof(digest));
    s->tx_kind = kind;
}

typedef struct { const uint8_t *gtk, *pmkid; unsigned id; bool rsn; } key_data_t;

static int key_data(const wpa2_t *s, const uint8_t *p, unsigned n, key_data_t *result)
{
    memset(result, 0, sizeof(*result));
    for (unsigned pos = 0; pos < n;) {
        /* RFC 3394 padding: 0xdd followed by zero octets, including a lone dd. */
        if (p[pos] == 221 && (pos + 1 == n || p[pos + 1] == 0)) {
            for (unsigned i = pos + 1; i < n; i++) if (p[i]) return -1;
            return 0;
        }
        if (n - pos < 2 || p[pos + 1] > n - pos - 2) return -1;
        unsigned len = p[pos + 1]; const uint8_t *body = p + pos + 2;
        if (p[pos] == 48) {
            if (result->rsn || len != s->ap_rsn_len || memcmp(body, s->ap_rsn, len)) return -1;
            result->rsn = true;
        } else if (p[pos] == 221 && len >= 4 && !memcmp(body, "\x00\x0f\xac", 3)) {
            if (body[3] == 1) {
                if (result->gtk || len != 6U + s->group_len || (body[4] & ~7U) || body[5]) return -1;
                result->gtk = body + 6; result->id = body[4] & 3;
            } else if (body[3] == 4) {
                if (result->pmkid || len != 20) return -1;
                result->pmkid = body + 4;
            } else if (body[3] == 9 || body[3] == 10) {
                return -1; /* IGTK/Key ID need unnegotiated PMF/extended key ID. */
            }
        }
        pos += len + 2;
    }
    return 0;
}

static int message1(wpa2_t *s, const uint8_t *p, unsigned n, const char **reason)
{
    if (s->state != WPA2_WAIT_M1 && s->state != WPA2_WAIT_M3) {
        *reason = "M1 not expected in current state"; return 0;
    }
    key_data_t data;
    if (key_data(s, p + 99, n - 99, &data) || data.gtk || data.rsn) {
        *reason = "invalid M1 key data"; return 0;
    }
    bool pmkid_mismatch = false;
    if (data.pmkid) {
        uint8_t name[20], digest[20];
        memcpy(name, "PMK Name", 8); memcpy(name + 8, s->peer, 6); memcpy(name + 14, s->local, 6);
        wpa2_hmac_sha1(s->pmk, 32, name, sizeof(name), digest);
        pmkid_mismatch = !wpa2_equal(digest, data.pmkid, 16);
        wpa2_clear(digest, sizeof(digest));
    }
    if (s->state == WPA2_WAIT_M3) {
        int order = memcmp(p + 9, s->m1_counter, 8);
        if (order < 0 || (order == 0 && memcmp(s->anonce, p + 17, 32))) {
            *reason = "M1 replay counter or nonce mismatch"; return 0;
        }
    }
    /* PSK-only, no PMKSA cache: an unauthenticated M1 PMKID is advisory.
     * Keep the configured PMK; only a verified M3 can request key installation. */
    if (pmkid_mismatch) s->pmkid_mismatches++;
    memcpy(s->anonce, p + 17, 32); memcpy(s->m1_counter, p + 9, 8);
    wpa2_ptk(s->pmk, s->peer, s->local, s->anonce, s->snonce, s->ptk);
    s->state = WPA2_WAIT_M3;
    reply(s, WPA2_TX_M2, p);
    return 1;
}

static int authenticated(wpa2_t *s, const uint8_t *p, unsigned n, bool pairwise,
                         uint64_t now, const char **reason)
{
    if (pairwise) {
        if (s->state != WPA2_WAIT_M3 && s->state != WPA2_INSTALL &&
            s->state != WPA2_WAIT_M4_TX && s->state != WPA2_WAIT_GROUP &&
            s->state != WPA2_ESTABLISHED) {
            *reason = "pairwise key message not expected in current state"; return 0;
        }
        if (memcmp(p + 17, s->anonce, 32) || memcmp(p + 9, s->m1_counter, 8) <= 0) {
            *reason = "pairwise nonce or replay counter mismatch"; return 0;
        }
    } else if (s->state != WPA2_ESTABLISHED && s->state != WPA2_GROUP_INSTALL &&
               s->state != WPA2_WAIT_GROUP_TX && s->state != WPA2_WAIT_GROUP &&
               s->state != WPA2_WAIT_M4_TX) {
        *reason = "group key message not expected in current state"; return 0;
    }
    if (s->have_replay) {
        int order = memcmp(p + 9, s->replay, 8);
        if (order < 0 || (!order && (n != s->last_rx_len || memcmp(p, s->last_rx, n)))) {
            *reason = "authenticated replay counter or retransmission mismatch"; return 0;
        }
    }
    uint8_t digest[20];
    mic(s->ptk, p, n, digest);
    int valid = wpa2_equal(digest, p + 81, 16);
    wpa2_clear(digest, sizeof(digest));
    if (!valid) { s->bad_mic++; *reason = "EAPOL-Key MIC mismatch"; return 0; }
    uint8_t plain[WPA2_KEY_DATA_MAX];
    bool encrypted = (be16(p + 5) & 0x1000) != 0;
    int plain_len;
    if (encrypted) plain_len = wpa2_unwrap(s->ptk + 16, p + 99, n - 99, plain, sizeof(plain));
    else { plain_len = n - 99; memcpy(plain, p + 99, plain_len); }
    key_data_t data;
    int rc = 0;
    if (plain_len < 0) { *reason = "EAPOL-Key unwrap failed"; goto out; }
    *reason = "invalid authenticated key data";
    if (key_data(s, plain, plain_len, &data) || (!encrypted && data.gtk) ||
        (!pairwise && !data.gtk) || (pairwise && !data.rsn) ||
        (!pairwise && data.rsn) || p[71] || p[72]) goto out;
    /* Repeated key material cannot be installed at another slot or with a
     * reset PN. Identical M3/G1 can only cause another acknowledgement. */
    bool installed = !data.gtk || ((s->gtk_set & (1U << data.id)) &&
        wpa2_equal(s->gtk[data.id], data.gtk, s->group_len));
    for (unsigned i = 0; i < 4; i++)
        if (data.gtk && (s->gtk_set & (1U << i)) && wpa2_equal(s->gtk[i], data.gtk, 16) &&
            (i != data.id || !wpa2_equal(s->gtk[i], data.gtk, s->group_len))) {
            *reason = "temporal key reused with another slot or MIC key"; goto out;
        }
    if (s->install_pending) {
        if (s->keys.set_pairwise != pairwise || s->keys.set_group != (data.gtk != NULL) ||
            (data.gtk && (s->keys.group_id != data.id || !wpa2_equal(s->keys.group, data.gtk, s->group_len)))) {
            *reason = "conflicting pending key installation"; goto out;
        }
        s->duplicates++;
    } else if (pairwise && s->pairwise_installed && !installed) {
        *reason = "changed GTK in pairwise retransmission"; goto out;
    } else if (installed && (!pairwise || s->pairwise_installed)) {
        s->duplicates++;
    } else {
        s->keys.generation++;
        if (!s->keys.generation) { rc = fail(s, "key generation exhausted"); goto out; }
        s->keys.set_pairwise = pairwise; s->keys.set_group = data.gtk != NULL;
        memcpy(s->keys.pairwise, s->ptk + 32, 16);
        if (data.gtk) memcpy(s->keys.group, data.gtk, s->group_len);
        s->keys.group_cipher = s->group_cipher;
        memcpy(s->keys.rsc, p + 65, 6);
        s->keys.group_id = data.id; s->install_pending = true;
        s->state = pairwise ? WPA2_INSTALL : WPA2_GROUP_INSTALL;
        if (!pairwise) s->started = now;
        s->authorized = false;
    }
    memcpy(s->replay, p + 9, 8); s->have_replay = true;
    memcpy(s->last_rx, p, n); s->last_rx_len = n;
    s->tx_len = 0; s->tx_kind = WPA2_TX_NONE;
    if (!s->install_pending) reply(s, pairwise ? WPA2_TX_M4 : WPA2_TX_GROUP, p);
    rc = 1;
out:
    wpa2_clear(plain, sizeof(plain));
    return rc;
}

int wpa2_receive(wpa2_t *s, const uint8_t src[6], const uint8_t *p, unsigned length, uint64_t now)
{
    if (!s || s->state == WPA2_OFF || s->state == WPA2_FAILED) return -1;
    if (wpa2_tick(s, now)) return -1;
    const char *reason;
    if (s->countermeasures) { reason = "TKIP countermeasures active"; goto drop; }
    if (!src) { reason = "invalid EAPOL source"; goto drop; }
    if (!p || length < 99) { reason = "short EAPOL-Key frame"; goto drop; }
    if (memcmp(src, s->peer, 6)) { reason = "EAPOL-Key source is not associated AP"; goto drop; }
    unsigned n = 4 + be16(p + 2);
    if (n < 99 || n > length || n > WPA2_EAPOL_MAX) { reason = "invalid EAPOL-Key length"; goto drop; }
    if (p[0] < 1 || p[0] > 3) { reason = "unsupported EAPOL version"; goto drop; }
    if (p[1] != 3) { reason = "not an EAPOL-Key packet"; goto drop; }
    if (p[4] != 2) { reason = "unsupported EAPOL-Key descriptor"; goto drop; }
    unsigned info = be16(p + 5);
    unsigned key_len = (info & 8) ? 16 : s->group_len;
    if (be16(p + 7) != key_len) { reason = "invalid negotiated key length"; goto drop; }
    if (be16(p + 97) != n - 99) { reason = "invalid EAPOL-Key data length"; goto drop; }
    int rc = 0;
    reason = "unsupported EAPOL-Key flags";
    if (info == 0x008a) rc = message1(s, p, n, &reason);
    else if (info == 0x13ca || info == 0x03ca || info == 0x1382)
        rc = authenticated(s, p, n, info != 0x1382, now, &reason);
    if (rc) return rc;
drop:
    memset(&s->last_drop, 0, sizeof(s->last_drop));
    s->last_drop.reason = reason; s->last_drop.state = s->state; s->last_drop.bytes = length;
    if (p) {
        if (length >= 1) s->last_drop.version = p[0];
        if (length >= 2) s->last_drop.packet_type = p[1];
        if (length >= 4) s->last_drop.body_bytes = be16(p + 2);
        if (length >= 5) s->last_drop.descriptor = p[4];
        if (length >= 7) s->last_drop.key_info = be16(p + 5);
        if (length >= 9) s->last_drop.key_bytes = be16(p + 7);
        if (length >= 99) s->last_drop.data_bytes = be16(p + 97);
    }
    s->dropped++;
    return 0;
}

int wpa2_keys_installed(wpa2_t *s, uint32_t generation, bool success)
{
    if (!s || !s->install_pending || s->keys.generation != generation) return -1;
    if (!success) return fail(s, "firmware key installation failed");
    if (s->keys.set_pairwise) s->pairwise_installed = true;
    if (s->keys.set_group) {
        memcpy(s->gtk[s->keys.group_id], s->keys.group, s->group_len);
        s->gtk_set |= 1U << s->keys.group_id;
    }
    bool pairwise = s->keys.set_pairwise;
    wpa2_clear(&s->keys, sizeof(s->keys)); s->keys.generation = generation;
    s->install_pending = false; s->installs++;
    s->state = pairwise ? WPA2_WAIT_M4_TX : WPA2_WAIT_GROUP_TX;
    reply(s, pairwise ? WPA2_TX_M4 : WPA2_TX_GROUP, s->last_rx);
    return 0;
}

int wpa2_take_tx(wpa2_t *s, uint8_t *frame, unsigned capacity, uint32_t *token,
                 bool *protected_frame, uint64_t now)
{
    if (!s || s->state == WPA2_OFF || s->state == WPA2_FAILED ||
        !frame || !token || !protected_frame) return -1;
    if (wpa2_tick(s, now)) return -1;
    if (s->inflight_tx || (!s->tx_len && !s->mic_reports)) return 0;
    unsigned n = s->mic_reports ? 99 : s->tx_len;
    if (capacity < n) return -1;
    if (++s->next_tx == 0) return fail(s, "EAPOL TX token exhausted");
    *token = s->inflight_tx = s->next_tx;
    s->inflight_generation = s->keys.generation;
    if (s->mic_reports) {
        if (s->request_counter == UINT64_MAX) return fail(s, "EAPOL request counter exhausted");
        memset(frame, 0, n);
        frame[0] = 2; frame[1] = 3; frame[4] = 2;
        put16(frame + 2, 95); put16(frame + 5, 0x0f02); /* Secure/MIC/Error/Request, group. */
        uint64_t counter = s->request_counter++;
        for (unsigned i = 0; i < 8; i++) { frame[16 - i] = counter; counter >>= 8; }
        uint8_t digest[20];
        mic(s->ptk, frame, n, digest); memcpy(frame + 81, digest, 16);
        wpa2_clear(digest, sizeof(digest));
        s->mic_reports--; s->inflight_kind = WPA2_TX_MIC_REPORT;
        *protected_frame = true;
    } else {
        memcpy(frame, s->tx, n); s->inflight_kind = s->tx_kind;
        *protected_frame = s->tx_kind == WPA2_TX_GROUP;
        s->tx_len = 0; s->tx_kind = WPA2_TX_NONE;
        wpa2_clear(s->tx, sizeof(s->tx));
    }
    s->tx_started = now;
    return n;
}

int wpa2_tx_complete(wpa2_t *s, uint32_t token, bool success)
{
    if (!s || !token || s->inflight_tx != token) return -1;
    if (!success) return fail(s, "EAPOL transmission failed");
    enum wpa2_tx_kind kind = s->inflight_kind;
    s->inflight_tx = 0; s->inflight_kind = WPA2_TX_NONE;
    if (kind == WPA2_TX_MIC_REPORT && !s->mic_reports) s->mic_deadline = 0;
    if (s->inflight_generation == s->keys.generation &&
        ((kind == WPA2_TX_M4 && s->state == WPA2_WAIT_M4_TX) ||
         (kind == WPA2_TX_GROUP && s->state == WPA2_WAIT_GROUP_TX))) {
        s->state = s->gtk_set ? WPA2_ESTABLISHED : WPA2_WAIT_GROUP;
        s->authorized = s->gtk_set != 0 && !s->countermeasures;
    }
    return 0;
}

int wpa2_tick(wpa2_t *s, uint64_t now)
{
    if (!s || s->state == WPA2_OFF || s->state == WPA2_FAILED) return -1;
    if (s->mic_deadline && now >= s->mic_deadline)
        return fail(s, "TKIP MIC failure report timed out");
    if (s->inflight_tx && now - s->tx_started >= WPA2_TIMEOUT_NS)
        return fail(s, "EAPOL TX completion timed out");
    if (s->state != WPA2_ESTABLISHED && now - s->started >= WPA2_TIMEOUT_NS)
        return fail(s, "WPA2 handshake timed out");
    return 0;
}

int wpa2_michael_failure(wpa2_t *s, wpa2_michael_t *cm, uint64_t now)
{
    if (!s || !cm || s->group_cipher != WPA2_CIPHER_TKIP || !s->pairwise_installed ||
        !s->gtk_set || s->state == WPA2_FAILED || s->countermeasures) return 0;
    if (cm->have_first && now >= cm->first_ns && now - cm->first_ns < WPA2_MIC_WINDOW_NS) {
        cm->blocked_until = now > UINT64_MAX - WPA2_MIC_WINDOW_NS ? UINT64_MAX : now + WPA2_MIC_WINDOW_NS;
        s->countermeasures = true; s->authorized = false;
        cm->have_first = false;
    } else {
        cm->have_first = true; cm->first_ns = now;
    }
    if (s->mic_reports < 2) s->mic_reports++;
    if (!s->mic_deadline) s->mic_deadline = now > UINT64_MAX - WPA2_TIMEOUT_NS ? UINT64_MAX : now + WPA2_TIMEOUT_NS;
    return s->countermeasures ? 2 : 1;
}
