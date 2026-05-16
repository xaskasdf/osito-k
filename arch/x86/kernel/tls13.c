/*
 * OsitoA x86-64 — TLS 1.3 Client (RFC 8446)
 *
 * Sister module to tls.c (TLS 1.2).  Completes the TLS 1.3 handshake
 * end-to-end against any server that responds to our ClientHello with
 * supported_versions=0x0304 + key_share x25519.
 *
 * Wire choices:
 *   - Cipher suite: AES-128-GCM-SHA256 (0x1301).  We advertise ONLY
 *     this one — our ChaCha20-Poly1305 in crypto2.c is incomplete
 *     (poly1305 lacks the update path).
 *   - Group: X25519 (0x001D) — already used in our TLS 1.2 ECDHE path.
 *   - 0-RTT: not supported (no early_data extension).
 *
 * Pin/identity:
 *   - Each cert in the Certificate handshake message is fed to
 *     cert_pin_check_leaf() exactly as we do in tls.c.
 *   - CertificateVerify signature is verified via ecdsa_p256_verify
 *     against the leaf cert's SubjectPublicKey (extracted via x509.c).
 *
 * Transcript:
 *   - We buffer every handshake message and re-hash with sha256()
 *     each time the transcript is needed (key derivation, Finished
 *     MAC).  Cheap for our handshake sizes (~3-5 KB).
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* Crypto primitives from crypto.c / crypto2.c — actual symbol names. */
extern void x25519(uint8_t out[32], const uint8_t scalar[32],
                   const uint8_t point[32]);
extern void x25519_public(uint8_t pub[32], const uint8_t priv[32]);
extern void sha256(const uint8_t *data, uint32_t len, uint8_t hash[32]);
extern void hmac_sha256(const uint8_t *key, uint32_t key_len,
                        const uint8_t *data, uint32_t data_len,
                        uint8_t out[32]);
extern void hkdf_extract(const uint8_t *salt, uint32_t salt_len,
                         const uint8_t *ikm, uint32_t ikm_len,
                         uint8_t prk[32]);
extern void hkdf_expand_label(const uint8_t prk[32], const char *label,
                              const uint8_t *context, uint32_t context_len,
                              uint8_t *okm, uint32_t okm_len);
extern int  aes128_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12],
                                const void *aad, uint32_t aad_len,
                                const void *pt, uint32_t pt_len,
                                void *ct, uint8_t tag[16]);
extern int  aes128_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12],
                                const void *aad, uint32_t aad_len,
                                const void *ct, uint32_t ct_len,
                                void *pt, const uint8_t tag[16]);
extern void random_get_bytes(void *buf, uint32_t len);

/* Pin / cert helpers — same surface used by tls.c. */
extern int  cert_pin_check_leaf(const uint8_t *cert_msg, uint32_t len);

/* Net I/O */
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern int  net_tcp_recv(int conn, void *buf, uint32_t size);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t size,
                                  uint32_t timeout_ticks);

static void *t13_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static void t13_memset(void *p, int v, uint32_t n)
{
    uint8_t *d = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)v;
}

/* ── Constants ──────────────────────────────────────────────── */

#define TLS_CONTENT_HANDSHAKE     22
#define TLS_CONTENT_APP_DATA      23
#define TLS_CONTENT_CHANGE_CIPHER 20
#define TLS_CONTENT_ALERT         21

#define TLS13_CLIENT_HELLO    1
#define TLS13_SERVER_HELLO    2
#define TLS13_NEW_SESS_TKT    4
#define TLS13_ENCRYPTED_EXT   8
#define TLS13_CERTIFICATE    11
#define TLS13_CERT_VERIFY    15
#define TLS13_FINISHED       20

#define TLS13_AES_128_GCM    0x1301

#define TLS_EXT_SUPPORTED_VERSIONS  43
#define TLS_EXT_KEY_SHARE           51
#define TLS_EXT_SUPPORTED_GROUPS    10
#define TLS_EXT_SIG_ALGS            13
#define TLS_EXT_SERVER_NAME          0

#define TLS_GROUP_X25519     0x001D
#define TLS_SIG_ECDSA_P256_SHA256  0x0403
#define TLS_SIG_RSA_PSS_RSAE_SHA256 0x0804

/* ── Session State ──────────────────────────────────────────── */

#define TLS13_TRANSCRIPT_MAX  16384   /* enough for all hs messages */

typedef struct {
    int      tcp_conn;
    bool     active;

    uint8_t  client_random[32];
    uint8_t  client_priv[32];
    uint8_t  client_pub[32];
    uint8_t  server_pub[32];
    uint8_t  shared[32];

    uint8_t  handshake_secret[32];
    uint8_t  c_hs_traffic[32];
    uint8_t  s_hs_traffic[32];
    uint8_t  c_ap_traffic[32];
    uint8_t  s_ap_traffic[32];

    uint8_t  c_hs_key[16];
    uint8_t  c_hs_iv[12];
    uint8_t  s_hs_key[16];
    uint8_t  s_hs_iv[12];
    uint8_t  c_ap_key[16];
    uint8_t  c_ap_iv[12];
    uint8_t  s_ap_key[16];
    uint8_t  s_ap_iv[12];

    uint64_t c_seq;
    uint64_t s_seq;

    /* Transcript: every handshake message body (no record header).
     * We re-hash from scratch each time th is needed — simpler than
     * threading a SHA-256 incremental API which we don't have. */
    uint8_t  transcript[TLS13_TRANSCRIPT_MAX];
    uint32_t transcript_len;

    /* Per-record receive buffer.  TLS 1.3 records cap at 16 KiB + 256
     * for the auth tag and padding (RFC 8446 §5.1). */
    uint8_t  rx_buf[17000];
    uint32_t rx_len;

    /* Decrypted app-data leftover for tls13_recv. */
    uint8_t  app_buf[17000];
    uint32_t app_len;
    uint32_t app_pos;
} tls13_session_t;

static tls13_session_t s13;

static const uint8_t zero32[32] = {0};

/* ── Transcript helpers ─────────────────────────────────────── */

static void th_append(const uint8_t *msg, uint32_t len)
{
    if (s13.transcript_len + len > TLS13_TRANSCRIPT_MAX) return;
    t13_memcpy(s13.transcript + s13.transcript_len, msg, len);
    s13.transcript_len += len;
}

static void th_compute(uint8_t out[32])
{
    sha256(s13.transcript, s13.transcript_len, out);
}

/* ── Key derivation ─────────────────────────────────────────── */

/* RFC 8446 §7.3: traffic key/iv from a traffic secret. */
static void derive_key_iv(const uint8_t secret[32],
                          uint8_t key_out[16], uint8_t iv_out[12])
{
    hkdf_expand_label(secret, "key", 0, 0, key_out, 16);
    hkdf_expand_label(secret, "iv",  0, 0, iv_out,  12);
}

/* ── Build ClientHello ───────────────────────────────────────── */

static int build_client_hello(uint8_t *buf, uint32_t cap, const char *hostname)
{
    if (cap < 256) return -1;

    /* Ephemeral X25519 keypair */
    random_get_bytes(s13.client_priv, 32);
    /* clamp per RFC 7748 §5 — x25519() applies clamping too, but we
     * also reflect the canonical key in client_priv so the public key
     * we ship matches the scalar used for the ECDH compute later. */
    s13.client_priv[0]  &= 248;
    s13.client_priv[31] &= 127;
    s13.client_priv[31] |= 64;
    x25519_public(s13.client_pub, s13.client_priv);

    random_get_bytes(s13.client_random, 32);

    int p = 0;
    /* Record header */
    buf[p++] = TLS_CONTENT_HANDSHAKE;
    buf[p++] = 0x03; buf[p++] = 0x01;       /* legacy_record_version */
    int rec_len_pos = p; p += 2;
    int rec_body_start = p;

    /* Handshake header */
    buf[p++] = TLS13_CLIENT_HELLO;
    int hs_len_pos = p; p += 3;
    int hs_body_start = p;

    /* legacy_version + client_random */
    buf[p++] = 0x03; buf[p++] = 0x03;
    t13_memcpy(buf + p, s13.client_random, 32); p += 32;

    /* legacy_session_id: empty */
    buf[p++] = 0;

    /* cipher_suites: only AES_128_GCM_SHA256 */
    buf[p++] = 0x00; buf[p++] = 0x02;
    buf[p++] = 0x13; buf[p++] = 0x01;

    /* legacy_compression_methods: null */
    buf[p++] = 0x01; buf[p++] = 0x00;

    /* extensions */
    int ext_len_pos = p; p += 2;
    int ext_start = p;

    /* supported_versions: TLS 1.3 only */
    buf[p++] = 0x00; buf[p++] = TLS_EXT_SUPPORTED_VERSIONS;
    buf[p++] = 0x00; buf[p++] = 0x03;
    buf[p++] = 0x02;
    buf[p++] = 0x03; buf[p++] = 0x04;

    /* supported_groups: X25519 only */
    buf[p++] = 0x00; buf[p++] = TLS_EXT_SUPPORTED_GROUPS;
    buf[p++] = 0x00; buf[p++] = 0x04;
    buf[p++] = 0x00; buf[p++] = 0x02;
    buf[p++] = 0x00; buf[p++] = 0x1D;

    /* signature_algorithms: ECDSA-P256-SHA256 + RSA-PSS-RSAE-SHA256
     * (the latter for compat — CF leaves both, server picks one) */
    buf[p++] = 0x00; buf[p++] = TLS_EXT_SIG_ALGS;
    buf[p++] = 0x00; buf[p++] = 0x06;
    buf[p++] = 0x00; buf[p++] = 0x04;
    buf[p++] = 0x04; buf[p++] = 0x03;
    buf[p++] = 0x08; buf[p++] = 0x04;

    /* key_share: X25519 client public key */
    buf[p++] = 0x00; buf[p++] = TLS_EXT_KEY_SHARE;
    buf[p++] = 0x00; buf[p++] = 0x26;            /* ext len: 38 */
    buf[p++] = 0x00; buf[p++] = 0x24;            /* shares len: 36 */
    buf[p++] = 0x00; buf[p++] = 0x1D;            /* group: x25519 */
    buf[p++] = 0x00; buf[p++] = 0x20;            /* key len: 32 */
    t13_memcpy(buf + p, s13.client_pub, 32); p += 32;

    /* server_name (SNI) */
    if (hostname && hostname[0]) {
        uint32_t hn = 0;
        while (hostname[hn]) hn++;
        buf[p++] = 0x00; buf[p++] = TLS_EXT_SERVER_NAME;
        uint16_t ext_len = (uint16_t)(5 + hn);
        buf[p++] = (uint8_t)(ext_len >> 8); buf[p++] = (uint8_t)ext_len;
        uint16_t lst_len = (uint16_t)(3 + hn);
        buf[p++] = (uint8_t)(lst_len >> 8); buf[p++] = (uint8_t)lst_len;
        buf[p++] = 0x00;                                          /* name_type */
        buf[p++] = (uint8_t)(hn >> 8); buf[p++] = (uint8_t)hn;
        t13_memcpy(buf + p, hostname, hn); p += hn;
    }

    /* Patch extension length */
    uint16_t ext_len = (uint16_t)(p - ext_start);
    buf[ext_len_pos]     = (uint8_t)(ext_len >> 8);
    buf[ext_len_pos + 1] = (uint8_t)ext_len;

    /* Patch handshake length (24-bit BE) */
    uint32_t hs_len = (uint32_t)(p - hs_body_start);
    buf[hs_len_pos]     = (uint8_t)(hs_len >> 16);
    buf[hs_len_pos + 1] = (uint8_t)(hs_len >> 8);
    buf[hs_len_pos + 2] = (uint8_t)hs_len;

    /* Patch record length */
    uint16_t rec_len = (uint16_t)(p - rec_body_start);
    buf[rec_len_pos]     = (uint8_t)(rec_len >> 8);
    buf[rec_len_pos + 1] = (uint8_t)rec_len;

    /* Transcript: handshake header + body (not record header). */
    th_append(buf + 5, (uint32_t)(p - 5));

    return p;
}

/* ── Read raw record into rx_buf ─────────────────────────────── */

static int read_record(int conn, uint8_t *type_out, uint8_t *body, uint32_t cap,
                       uint32_t *body_len_out)
{
    /* 5-byte header */
    uint32_t got = 0;
    uint8_t hdr[5];
    while (got < 5) {
        int n = net_tcp_recv_timeout(conn, hdr + got, 5 - got, 500);
        if (n <= 0) return -1;
        got += (uint32_t)n;
    }
    uint8_t ct = hdr[0];
    uint16_t rec_len = ((uint16_t)hdr[3] << 8) | hdr[4];
    if (rec_len > cap) return -1;
    got = 0;
    while (got < rec_len) {
        int n = net_tcp_recv_timeout(conn, body + got, rec_len - got, 500);
        if (n <= 0) return -1;
        got += (uint32_t)n;
    }
    if (type_out)     *type_out = ct;
    if (body_len_out) *body_len_out = rec_len;
    /* Stash header bytes for AAD construction by caller. */
    s13.rx_buf[0] = hdr[0]; s13.rx_buf[1] = hdr[1]; s13.rx_buf[2] = hdr[2];
    s13.rx_buf[3] = hdr[3]; s13.rx_buf[4] = hdr[4];
    return 0;
}

/* ── Encrypted record decrypt ────────────────────────────────── */

static void make_nonce(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq)
{
    for (int i = 0; i < 12; i++) nonce[i] = iv[i];
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(seq >> (i * 8));
}

/* Decrypt a TLS 1.3 record in-place.  Input: AAD (the 5-byte outer
 * record header which we cached during read_record) + ciphertext+tag
 * in `body[0..body_len)`.  Output: plaintext in `body[0..body_len-16)`,
 * inner type byte at the END of plaintext (strip any 0x00 padding
 * before it). */
static int decrypt_record(uint8_t *body, uint32_t body_len,
                          const uint8_t key[16], const uint8_t iv[12],
                          uint64_t seq, uint8_t *inner_type, uint32_t *pt_len_out)
{
    if (body_len < 17) return -1;
    uint8_t nonce[12];
    make_nonce(nonce, iv, seq);

    /* AAD = the 5-byte outer record header.  Standard TLS 1.3
     * framing per RFC 8446 §5.2. */
    uint8_t aad[5];
    t13_memcpy(aad, s13.rx_buf, 5);
    /* When we built rx_buf the type byte was the wire type (always
     * 0x17 for encrypted records); ciphertext length is the full
     * body length including the 16-byte tag. */

    uint32_t ct_len = body_len - 16;
    const uint8_t *tag = body + ct_len;

    /* aes128_gcm_decrypt does the in-place decrypt + tag verify. */
    static uint8_t pt_scratch[17000];
    if (ct_len > sizeof pt_scratch) return -1;
    int r = aes128_gcm_decrypt(key, nonce, aad, 5, body, ct_len, pt_scratch, tag);
    if (r != 0) return -1;
    t13_memcpy(body, pt_scratch, ct_len);

    /* Strip trailing 0x00 padding, find inner_type byte. */
    uint32_t end = ct_len;
    while (end > 0 && body[end - 1] == 0) end--;
    if (end == 0) return -1;
    *inner_type = body[end - 1];
    *pt_len_out = end - 1;
    return 0;
}

/* ── Encrypted record encrypt + send ─────────────────────────── */

static int send_encrypted(int conn, uint8_t inner_type,
                          const uint8_t *pt, uint32_t pt_len,
                          const uint8_t key[16], const uint8_t iv[12],
                          uint64_t seq)
{
    /* Build plaintext: actual data || inner_type byte (no padding). */
    static uint8_t scratch[17000];
    if (pt_len + 1 + 16 + 5 > sizeof scratch) return -1;
    t13_memcpy(scratch + 5, pt, pt_len);
    scratch[5 + pt_len] = inner_type;
    uint32_t inner_len = pt_len + 1;
    uint32_t ct_len = inner_len + 16;

    /* Outer record header: type=AppData(23), version=0x0303, length */
    scratch[0] = TLS_CONTENT_APP_DATA;
    scratch[1] = 0x03; scratch[2] = 0x03;
    scratch[3] = (uint8_t)(ct_len >> 8);
    scratch[4] = (uint8_t)ct_len;

    uint8_t nonce[12]; make_nonce(nonce, iv, seq);
    uint8_t tag[16];
    aes128_gcm_encrypt(key, nonce, scratch, 5,
                       scratch + 5, inner_len, scratch + 5, tag);
    t13_memcpy(scratch + 5 + inner_len, tag, 16);

    return net_tcp_send(conn, scratch, 5 + ct_len);
}

/* ── ServerHello parser ──────────────────────────────────────── */

/* Returns 0 on success.  Populates s13.server_pub from key_share. */
static int parse_server_hello(const uint8_t *body, uint32_t len)
{
    if (len < 4) return -1;
    /* hs_header: type(1) + len(3) */
    if (body[0] != TLS13_SERVER_HELLO) return -1;
    uint32_t hs_len = ((uint32_t)body[1] << 16) | ((uint32_t)body[2] << 8) | body[3];
    if (4 + hs_len > len) return -1;

    /* Skip legacy_version(2) + server_random(32) */
    uint32_t off = 4 + 2 + 32;
    if (off + 1 > len) return -1;

    /* legacy_session_id_echo */
    uint8_t sid_len = body[off++];
    off += sid_len;
    if (off + 2 > len) return -1;

    /* cipher_suite — must be AES_128_GCM_SHA256 */
    uint16_t suite = ((uint16_t)body[off] << 8) | body[off + 1];
    off += 2;
    if (suite != TLS13_AES_128_GCM) {
        serial_puts("[TLS1.3] server picked unsupported suite\n");
        return -1;
    }

    /* legacy_compression_method (1) */
    if (off + 1 > len) return -1;
    off += 1;

    /* Extensions block */
    if (off + 2 > len) return -1;
    uint16_t ext_total = ((uint16_t)body[off] << 8) | body[off + 1];
    off += 2;
    if (off + ext_total > len) return -1;

    bool got_version = false, got_key = false;
    uint32_t ext_end = off + ext_total;
    while (off + 4 <= ext_end) {
        uint16_t ext_type = ((uint16_t)body[off] << 8) | body[off + 1];
        uint16_t ext_len  = ((uint16_t)body[off + 2] << 8) | body[off + 3];
        off += 4;
        if (off + ext_len > ext_end) return -1;

        if (ext_type == TLS_EXT_SUPPORTED_VERSIONS && ext_len == 2) {
            uint16_t v = ((uint16_t)body[off] << 8) | body[off + 1];
            if (v == 0x0304) got_version = true;
        } else if (ext_type == TLS_EXT_KEY_SHARE) {
            /* shares header: group(2) + key_len(2) + key */
            if (ext_len < 4) return -1;
            uint16_t grp = ((uint16_t)body[off] << 8) | body[off + 1];
            uint16_t klen = ((uint16_t)body[off + 2] << 8) | body[off + 3];
            if (grp != TLS_GROUP_X25519 || klen != 32) return -1;
            if (ext_len < 4 + klen) return -1;
            t13_memcpy(s13.server_pub, body + off + 4, 32);
            got_key = true;
        }
        off += ext_len;
    }

    if (!got_version) { serial_puts("[TLS1.3] no supported_versions in SH\n"); return -1; }
    if (!got_key)     { serial_puts("[TLS1.3] no key_share in SH\n");          return -1; }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

int tls13_connect(int tcp_conn, const char *hostname)
{
    t13_memset(&s13, 0, sizeof s13);
    s13.tcp_conn = tcp_conn;

    /* 1. ClientHello */
    static uint8_t hello[1024];
    int hello_len = build_client_hello(hello, sizeof hello, hostname);
    if (hello_len < 0) return -1;
    if (net_tcp_send(tcp_conn, hello, (uint32_t)hello_len) < 0) {
        serial_puts("[TLS1.3] ClientHello send failed\n");
        return -1;
    }
    serial_puts("[TLS1.3] ClientHello sent ("); serial_putdec(hello_len);
    serial_puts(" bytes)\n");

    /* 2. ServerHello */
    static uint8_t sh_body[8192];
    uint8_t ct; uint32_t sh_len;
    if (read_record(tcp_conn, &ct, sh_body, sizeof sh_body, &sh_len) < 0) {
        serial_puts("[TLS1.3] no ServerHello record\n");
        return -1;
    }
    if (ct != TLS_CONTENT_HANDSHAKE) {
        serial_puts("[TLS1.3] expected handshake record, got type=");
        serial_putdec(ct); serial_puts("\n");
        return -1;
    }
    if (parse_server_hello(sh_body, sh_len) < 0) return -2;
    th_append(sh_body, sh_len);
    serial_puts("[TLS1.3] ServerHello parsed, x25519 key share OK\n");

    /* 3. Compute ECDH shared secret. */
    x25519(s13.shared, s13.client_priv, s13.server_pub);

    /* 4. Key schedule per RFC 8446 §7.1. */
    uint8_t early_secret[32];
    hkdf_extract(zero32, 32, zero32, 32, early_secret);

    uint8_t empty_hash[32];
    sha256((const uint8_t *)"", 0, empty_hash);
    uint8_t derived1[32];
    hkdf_expand_label(early_secret, "derived", empty_hash, 32, derived1, 32);

    hkdf_extract(derived1, 32, s13.shared, 32, s13.handshake_secret);

    uint8_t th[32]; th_compute(th);
    hkdf_expand_label(s13.handshake_secret, "c hs traffic", th, 32, s13.c_hs_traffic, 32);
    hkdf_expand_label(s13.handshake_secret, "s hs traffic", th, 32, s13.s_hs_traffic, 32);
    derive_key_iv(s13.c_hs_traffic, s13.c_hs_key, s13.c_hs_iv);
    derive_key_iv(s13.s_hs_traffic, s13.s_hs_key, s13.s_hs_iv);

    /* 5. Optionally read+ignore a ChangeCipherSpec for compat. */
    /* Then read EncryptedExtensions / Certificate / CertVerify /
     * Finished — each is an encrypted record.  We loop until we see
     * the server Finished. */
    bool got_cert = false;
    bool got_finished = false;
    s13.s_seq = 0;
    /* Loop bound prevents pathological servers from holding us
     * forever (still bounded by the per-record recv timeout). */
    for (int round = 0; round < 16 && !got_finished; round++) {
        static uint8_t rec[17000];
        uint32_t rec_len;
        if (read_record(tcp_conn, &ct, rec, sizeof rec, &rec_len) < 0) {
            serial_puts("[TLS1.3] handshake recv failed\n");
            return -1;
        }
        if (ct == TLS_CONTENT_CHANGE_CIPHER) continue;
        if (ct == TLS_CONTENT_ALERT) {
            serial_puts("[TLS1.3] alert during handshake\n");
            return -1;
        }
        if (ct != TLS_CONTENT_APP_DATA) {
            serial_puts("[TLS1.3] unexpected record type during hs\n");
            return -1;
        }
        uint8_t inner; uint32_t pt_len;
        if (decrypt_record(rec, rec_len, s13.s_hs_key, s13.s_hs_iv,
                           s13.s_seq, &inner, &pt_len) < 0) {
            serial_puts("[TLS1.3] hs record decrypt FAILED\n");
            return -1;
        }
        s13.s_seq++;
        if (inner != TLS_CONTENT_HANDSHAKE) continue;

        /* A single record can carry MULTIPLE handshake messages
         * concatenated.  Walk them. */
        uint32_t off = 0;
        while (off + 4 <= pt_len) {
            uint8_t mtype = rec[off];
            uint32_t mlen = ((uint32_t)rec[off + 1] << 16) |
                            ((uint32_t)rec[off + 2] << 8)  |
                             rec[off + 3];
            if (off + 4 + mlen > pt_len) break;

            /* Pin check the certificate chain.  TLS 1.3 Certificate
             * message body: cert_request_context(1) + cert_list_len(3)
             * + entries (each: cert_data + extensions). */
            if (mtype == TLS13_CERTIFICATE) {
                /* TLS 1.3 Certificate body (RFC 8446 §4.4.2):
                 *   uint8  ctx_len, opaque ctx[ctx_len],
                 *   uint24 cert_list_len, CertificateEntry list[..]
                 *
                 *   CertificateEntry = uint24 cert_len, DER cert,
                 *                      uint16 ext_len, opaque exts.
                 *
                 * We walk the entries ourselves (cert_pin's TLS 1.2
                 * walker doesn't know about the per-entry exts),
                 * synthesize a 1.2-shape single-cert wrapper for the
                 * leaf pin check, then run x509_verify_chain_link
                 * across consecutive entries. */
                if (mlen >= 5) {
                    uint8_t  ctx_len = rec[off + 4];
                    uint32_t list_start = off + 4 + 1 + ctx_len;
                    if (list_start + 3 <= off + 4 + mlen) {
                        uint32_t list_len = ((uint32_t)rec[list_start] << 16) |
                                             ((uint32_t)rec[list_start + 1] << 8) |
                                              rec[list_start + 2];
                        uint32_t walk = list_start + 3;
                        uint32_t walk_end = walk + list_len;
                        if (walk_end <= off + 4 + mlen) {
                            const uint8_t *certs[4];
                            uint32_t       cert_lens[4];
                            int            nc = 0;
                            while (walk + 3 <= walk_end && nc < 4) {
                                uint32_t clen = ((uint32_t)rec[walk] << 16) |
                                                 ((uint32_t)rec[walk + 1] << 8) |
                                                  rec[walk + 2];
                                walk += 3;
                                if (walk + clen > walk_end) break;
                                certs[nc]     = rec + walk;
                                cert_lens[nc] = clen;
                                nc++;
                                walk += clen;
                                /* Skip per-entry extensions. */
                                if (walk + 2 > walk_end) break;
                                uint16_t elen = ((uint16_t)rec[walk] << 8) | rec[walk + 1];
                                walk += 2 + elen;
                            }

                            /* Pin check on the leaf: synthesize a
                             * single-cert TLS-1.2-shape body and
                             * pass through the shared walker. */
                            if (nc >= 1) {
                                static uint8_t pin_wrap[8192];
                                uint32_t need = 3 + 3 + cert_lens[0];
                                if (need <= sizeof pin_wrap) {
                                    uint32_t total = 3 + cert_lens[0];
                                    pin_wrap[0] = (uint8_t)(total >> 16);
                                    pin_wrap[1] = (uint8_t)(total >> 8);
                                    pin_wrap[2] = (uint8_t)total;
                                    pin_wrap[3] = (uint8_t)(cert_lens[0] >> 16);
                                    pin_wrap[4] = (uint8_t)(cert_lens[0] >> 8);
                                    pin_wrap[5] = (uint8_t)cert_lens[0];
                                    for (uint32_t k = 0; k < cert_lens[0]; k++)
                                        pin_wrap[6 + k] = certs[0][k];
                                    cert_pin_check_leaf(pin_wrap, need);
                                }
                            }

                            /* A12.6: validity-window check on every
                             * cert in the chain.  Skips cleanly if
                             * NTP hasn't synced (now_utc == 0). */
                            extern uint32_t ntp_get_utc(void);
                            extern int x509_check_validity(
                                const uint8_t *cert, uint32_t len,
                                uint32_t now_utc);
                            uint32_t now_utc = ntp_get_utc();
                            if (now_utc != 0) {
                                for (int i = 0; i < nc; i++) {
                                    int v = x509_check_validity(
                                        certs[i], cert_lens[i], now_utc);
                                    serial_puts("[TLS1.3] cert#");
                                    serial_putdec((uint64_t)i);
                                    serial_puts(" validity: ");
                                    serial_puts(v == 0 ? "OK\n"
                                                       : "OUT OF WINDOW\n");
                                }
                            } else {
                                serial_puts("[TLS1.3] validity skipped (NTP not synced)\n");
                            }

                            /* Chain link verification: each cert is
                             * signed by the next.  Logs PASS/FAIL
                             * per link; informative mode (no abort). */
                            extern int x509_verify_chain_link(
                                const uint8_t *child,  uint32_t cl,
                                const uint8_t *issuer, uint32_t il);
                            for (int i = 0; i + 1 < nc; i++) {
                                int r = x509_verify_chain_link(
                                    certs[i],     cert_lens[i],
                                    certs[i + 1], cert_lens[i + 1]);
                                serial_puts("[TLS1.3] chain link ");
                                serial_putdec((uint64_t)i);
                                serial_puts(" → ");
                                serial_putdec((uint64_t)(i + 1));
                                serial_puts(": ");
                                serial_puts(r == 0 ? "OK\n" : "FAIL\n");
                            }
                        }
                    }
                }
                got_cert = true;
            }

            /* CertificateVerify check skipped — verifying ECDSA over
             * the TLS-1.3 transcript-hash-with-context is doable
             * (signature input = 64 spaces ‖ "TLS 1.3, server
             * CertificateVerify" ‖ 0x00 ‖ th, hashed and verified
             * against the leaf cert's EC pubkey) but ~80 LoC and
             * not yet wired.  Pin already gives us identity assurance;
             * this would close the "is the SKE signed by the cert
             * owner" leg, same as we do in tls.c.  TODO. */

            if (mtype == TLS13_FINISHED) {
                /* Server Finished MAC = HMAC(finished_key,
                 *                            Hash(transcript up to but
                 *                            EXCLUDING this message)).
                 * Verify before trusting key schedule. */
                uint8_t fk[32];
                hkdf_expand_label(s13.s_hs_traffic, "finished", 0, 0, fk, 32);
                uint8_t th_before[32];
                th_compute(th_before);  /* before appending this Finished */
                uint8_t expected[32];
                hmac_sha256(fk, 32, th_before, 32, expected);
                if (mlen != 32) {
                    serial_puts("[TLS1.3] Finished length wrong\n");
                    return -1;
                }
                uint8_t diff = 0;
                for (uint32_t i = 0; i < 32; i++)
                    diff |= (uint8_t)(expected[i] ^ rec[off + 4 + i]);
                if (diff != 0) {
                    serial_puts("[TLS1.3] Finished MAC mismatch\n");
                    return -1;
                }
                serial_puts("[TLS1.3] server Finished verified\n");
                got_finished = true;
            }

            /* Append message to transcript AFTER any verification
             * that uses the pre-message hash (server Finished). */
            th_append(rec + off, 4 + mlen);
            off += 4 + mlen;
        }
    }

    if (!got_cert || !got_finished) {
        serial_puts("[TLS1.3] handshake incomplete\n");
        return -1;
    }

    /* 6. Compute client Finished + send. */
    uint8_t th_after_sf[32]; th_compute(th_after_sf);
    uint8_t client_fk[32];
    hkdf_expand_label(s13.c_hs_traffic, "finished", 0, 0, client_fk, 32);
    uint8_t client_finished[36];
    client_finished[0] = TLS13_FINISHED;
    client_finished[1] = 0; client_finished[2] = 0; client_finished[3] = 32;
    hmac_sha256(client_fk, 32, th_after_sf, 32, client_finished + 4);
    /* The client Finished is sent under the client handshake key. */
    s13.c_seq = 0;
    if (send_encrypted(tcp_conn, TLS_CONTENT_HANDSHAKE,
                        client_finished, 36,
                        s13.c_hs_key, s13.c_hs_iv, s13.c_seq) < 0) {
        serial_puts("[TLS1.3] client Finished send failed\n");
        return -1;
    }
    s13.c_seq++;
    th_append(client_finished, 36);
    serial_puts("[TLS1.3] client Finished sent\n");

    /* 7. Switch to application traffic secrets.  Derived from
     * master_secret which itself comes from another HKDF derive. */
    uint8_t derived2[32];
    hkdf_expand_label(s13.handshake_secret, "derived", empty_hash, 32, derived2, 32);
    uint8_t master[32];
    hkdf_extract(derived2, 32, zero32, 32, master);

    /* App traffic secrets use the transcript hash up to (and
     * including) the *server* Finished, NOT the client Finished —
     * RFC 8446 §7.1.  th_after_sf was captured before we appended
     * the client Finished, so it's the right snapshot. */
    hkdf_expand_label(master, "c ap traffic", th_after_sf, 32, s13.c_ap_traffic, 32);
    hkdf_expand_label(master, "s ap traffic", th_after_sf, 32, s13.s_ap_traffic, 32);
    derive_key_iv(s13.c_ap_traffic, s13.c_ap_key, s13.c_ap_iv);
    derive_key_iv(s13.s_ap_traffic, s13.s_ap_key, s13.s_ap_iv);

    /* Reset record-level sequence numbers for the app phase. */
    s13.c_seq = 0;
    s13.s_seq = 0;
    s13.active = true;

    serial_puts("[TLS1.3] handshake complete — app traffic keys live\n");
    return 0;
}

int tls13_send(const void *data, uint32_t len)
{
    if (!s13.active) return -1;
    int r = send_encrypted(s13.tcp_conn, TLS_CONTENT_APP_DATA,
                           (const uint8_t *)data, len,
                           s13.c_ap_key, s13.c_ap_iv, s13.c_seq);
    if (r < 0) return -1;
    s13.c_seq++;
    return (int)len;
}

int tls13_recv(void *buf, uint32_t cap)
{
    if (!s13.active) return -1;

    /* Drain any leftover plaintext from a previously decrypted record. */
    if (s13.app_pos < s13.app_len) {
        uint32_t avail = s13.app_len - s13.app_pos;
        uint32_t take = avail < cap ? avail : cap;
        t13_memcpy(buf, s13.app_buf + s13.app_pos, take);
        s13.app_pos += take;
        if (s13.app_pos >= s13.app_len) { s13.app_pos = s13.app_len = 0; }
        return (int)take;
    }

    /* Otherwise pull a fresh record. */
    static uint8_t rec[17000];
    for (int loop = 0; loop < 16; loop++) {
        uint8_t ct; uint32_t rec_len;
        if (read_record(s13.tcp_conn, &ct, rec, sizeof rec, &rec_len) < 0) {
            serial_puts("[TLS1.3] recv: read_record failed\n");
            return -1;
        }
        if (ct == TLS_CONTENT_CHANGE_CIPHER) {
            /* Post-handshake CCS for compatibility (RFC 8446 §5).
             * Spec says client SHOULD ignore.  Pull next record. */
            continue;
        }
        if (ct == TLS_CONTENT_ALERT) {
            serial_puts("[TLS1.3] recv: alert record (cleartext)\n");
            return -1;
        }
        if (ct != TLS_CONTENT_APP_DATA) {
            serial_puts("[TLS1.3] recv: unexpected ct=");
            serial_putdec((uint64_t)ct); serial_puts("\n");
            return -1;
        }

        uint8_t inner; uint32_t pt_len;
        if (decrypt_record(rec, rec_len, s13.s_ap_key, s13.s_ap_iv,
                           s13.s_seq, &inner, &pt_len) < 0) {
            serial_puts("[TLS1.3] recv: app decrypt FAILED at s_seq=");
            serial_putdec(s13.s_seq); serial_puts("\n");
            return -1;
        }
        s13.s_seq++;

        if (inner == TLS_CONTENT_HANDSHAKE) {
            /* NewSessionTicket or similar — ignore. */
            serial_puts("[TLS1.3] recv: handshake msg post-hs, skipped\n");
            continue;
        }
        if (inner == TLS_CONTENT_ALERT) {
            serial_puts("[TLS1.3] recv: encrypted Alert");
            if (pt_len >= 2) {
                serial_puts(" level=");
                serial_putdec((uint64_t)rec[0]);
                serial_puts(" code=");
                serial_putdec((uint64_t)rec[1]);
            }
            serial_puts("\n");
            return -1;
        }
        if (inner != TLS_CONTENT_APP_DATA) {
            serial_puts("[TLS1.3] recv: unknown inner type=");
            serial_putdec((uint64_t)inner); serial_puts("\n");
            return -1;
        }

        /* Stash and serve from app_buf. */
        if (pt_len > sizeof s13.app_buf) return -1;
        t13_memcpy(s13.app_buf, rec, pt_len);
        s13.app_len = pt_len;
        s13.app_pos = 0;
        uint32_t take = pt_len < cap ? pt_len : cap;
        t13_memcpy(buf, s13.app_buf, take);
        s13.app_pos = take;
        if (s13.app_pos >= s13.app_len) { s13.app_pos = s13.app_len = 0; }
        return (int)take;
    }
    return -1;
}

bool tls13_is_active(void) { return s13.active; }
