/*
 * tls13.c -- TLS 1.3 client (RFC 8446) that authenticates the server
 *
 * The record layer, ClientHello and key schedule are x86's
 * (arch/x86/kernel/tls13.c): TLS_AES_128_GCM_SHA256 over X25519, no 0-RTT,
 * no resumption, no client certificate. What x86 only logged is enforced
 * here, and the handshake fails otherwise:
 *  - the server's chain verifies up to one of Mozilla's roots, for the host
 *    name, at NTP's time (tls_verify_chain: BearSSL's X.509 engine);
 *  - its CertificateVerify is a valid signature by the leaf's key over the
 *    transcript (tls_verify_signature);
 *  - its Finished MAC is right.
 * Handshake messages are reassembled across records (a long chain spans
 * several) and must come in order: EncryptedExtensions, Certificate,
 * CertificateVerify, Finished. The transcript is a running SHA-256.
 *
 * One connection at a time: the state is static, and callers hold the
 * network (net_hold).
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "crypto.h"
#include "hkdf.h"
#include "tls_verify.h"
#include "tls13.h"

extern int net_tcp_send(int conn, const void *data, uint32_t len);
extern int net_tcp_recv_timeout(int conn, void *buf, uint32_t size,
                                uint32_t timeout_ticks);
extern void *memmove(void *dst, const void *src, size_t n);    /* kernel/mem.S */

#define CT_CHANGE_CIPHER    20
#define CT_ALERT            21
#define CT_HANDSHAKE        22
#define CT_APP_DATA         23

#define HS_CLIENT_HELLO     1
#define HS_SERVER_HELLO     2
#define HS_ENCRYPTED_EXT    8
#define HS_CERTIFICATE      11
#define HS_CERT_REQUEST     13
#define HS_CERT_VERIFY      15
#define HS_FINISHED         20

#define AES_128_GCM_SHA256  0x1301
#define EXT_SERVER_NAME     0
#define EXT_SUPPORTED_GROUPS 10
#define EXT_SIG_ALGS        13
#define EXT_SUPPORTED_VERSIONS 43
#define EXT_KEY_SHARE       51
#define GROUP_X25519        0x001D

#define ALERT_BAD_CERTIFICATE   42
#define ALERT_DECRYPT_ERROR     51
#define ALERT_UNEXPECTED        10

#define RECORD_MAX      (16384 + 256)   /* a ciphertext: 2^14 plus expansion */
#define HS_MAX          65536           /* handshake bytes held at once */
#define CHAIN_MAX       8
#define RECORD_TICKS    1500            /* 15 s for a record, at 100 Hz */

static struct {
    int      tcp_conn;
    int      active;
    uint8_t  client_priv[32], client_pub[32], server_pub[32], shared[32];
    uint8_t  handshake_secret[32];
    uint8_t  c_hs_traffic[32], s_hs_traffic[32];
    uint8_t  c_ap_traffic[32], s_ap_traffic[32];
    uint8_t  c_hs_key[16], c_hs_iv[12], s_hs_key[16], s_hs_iv[12];
    uint8_t  c_ap_key[16], c_ap_iv[12], s_ap_key[16], s_ap_iv[12];
    uint64_t c_seq, s_seq;
    sha256_ctx th;                      /* the transcript so far */
    uint8_t  rec_hdr[5];                /* the last record's header: its AAD */
    uint8_t  app_buf[RECORD_MAX];       /* decrypted data not yet returned */
    uint32_t app_len, app_pos;
} s13;

static uint8_t rec_buf[RECORD_MAX];
static uint8_t hs_buf[HS_MAX];          /* decrypted handshake bytes */
static uint32_t hs_len;

static const uint8_t zero32[32];

/* ── Transcript and keys ─────────────────────────────────────── */

static void th_append(const uint8_t *msg, uint32_t len)
{
    sha256_update(&s13.th, msg, len);
}

/* The transcript's hash so far, the running one left as it is */
static void th_hash(uint8_t out[32])
{
    sha256_ctx copy = s13.th;
    sha256_final(&copy, out);
}

static void derive_key_iv(const uint8_t secret[32], uint8_t key[16], uint8_t iv[12])
{
    hkdf_expand_label(secret, "key", 0, 0, key, 16);
    hkdf_expand_label(secret, "iv", 0, 0, iv, 12);
}

/* ── Records ─────────────────────────────────────────────────── */

static int read_exact(uint8_t *buf, uint32_t len, uint64_t deadline)
{
    uint32_t got = 0;
    while (got < len) {
        if (timer_get_tick_count() > deadline)
            return -1;
        int n = net_tcp_recv_timeout(s13.tcp_conn, buf + got, len - got, 10);
        if (n < 0)
            return -1;                  /* the connection closed */
        got += (uint32_t)n;
    }
    return 0;
}

static int read_record(uint8_t *type, uint8_t *body, uint32_t cap, uint32_t *len)
{
    uint64_t deadline = timer_get_tick_count() + RECORD_TICKS;
    if (read_exact(s13.rec_hdr, 5, deadline) < 0)
        return -1;
    uint32_t n = ((uint32_t)s13.rec_hdr[3] << 8) | s13.rec_hdr[4];
    if (n > cap || read_exact(body, n, deadline) < 0)
        return -1;
    *type = s13.rec_hdr[0];
    *len = n;
    return 0;
}

static void make_nonce(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq)
{
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (uint8_t)(seq >> (i * 8));
}

/* A record's body decrypted in place: the content without padding, its
 * inner type */
static int decrypt_record(uint8_t *body, uint32_t len, const uint8_t key[16],
                          const uint8_t iv[12], uint64_t seq,
                          uint8_t *inner, uint32_t *pt_len)
{
    static uint8_t pt[RECORD_MAX];
    if (len < 17)
        return -1;
    uint8_t nonce[12];
    make_nonce(nonce, iv, seq);
    uint32_t ct_len = len - 16;
    if (aes128_gcm_decrypt(key, nonce, s13.rec_hdr, 5, body, ct_len, pt, body + ct_len))
        return -1;
    uint32_t end = ct_len;
    while (end > 0 && pt[end - 1] == 0)
        end--;
    if (end == 0)
        return -1;
    *inner = pt[end - 1];
    *pt_len = end - 1;
    memcpy(body, pt, end - 1);
    return 0;
}

static int send_encrypted(uint8_t inner, const uint8_t *data, uint32_t len,
                          const uint8_t key[16], const uint8_t iv[12], uint64_t seq)
{
    static uint8_t out[5 + RECORD_MAX];
    if (len > 16384)
        return -1;
    uint32_t ct_len = len + 1 + 16;
    out[0] = CT_APP_DATA;
    out[1] = 0x03;
    out[2] = 0x03;
    out[3] = (uint8_t)(ct_len >> 8);
    out[4] = (uint8_t)ct_len;
    memcpy(out + 5, data, len);
    out[5 + len] = inner;
    uint8_t nonce[12], tag[16];
    make_nonce(nonce, iv, seq);
    aes128_gcm_encrypt(key, nonce, out, 5, out + 5, len + 1, out + 5, tag);
    memcpy(out + 5 + len + 1, tag, 16);
    return net_tcp_send(s13.tcp_conn, out, 5 + ct_len);
}

/* A fatal alert under the handshake keys, before giving up */
static int fail(uint8_t alert, const char *why)
{
    uint8_t msg[2] = { 2, alert };
    serial_puts("[TLS1.3] ");
    serial_puts(why);
    serial_puts("\n");
    send_encrypted(CT_ALERT, msg, 2, s13.c_hs_key, s13.c_hs_iv, s13.c_seq++);
    return alert == ALERT_BAD_CERTIFICATE || alert == ALERT_DECRYPT_ERROR
           ? TLS13_AUTH_FAILED : -1;
}

/* ── ClientHello and ServerHello ─────────────────────────────── */

static int build_client_hello(uint8_t *buf, uint32_t cap, const char *hostname)
{
    uint32_t hn = (uint32_t)strlen(hostname);
    if (hn > 255 || cap < 256 + hn)
        return -1;
    random_bytes(s13.client_priv, 32);
    s13.client_priv[0] &= 248;          /* RFC 7748 5: the clamped scalar */
    s13.client_priv[31] &= 127;
    s13.client_priv[31] |= 64;
    x25519_public(s13.client_pub, s13.client_priv);

    uint32_t p = 0;
    buf[p++] = CT_HANDSHAKE;
    buf[p++] = 0x03; buf[p++] = 0x01;   /* legacy_record_version */
    uint32_t rec_len_pos = p; p += 2;
    buf[p++] = HS_CLIENT_HELLO;
    uint32_t hs_len_pos = p; p += 3;
    buf[p++] = 0x03; buf[p++] = 0x03;   /* legacy_version */
    random_bytes(buf + p, 32);          /* random */
    p += 32;
    buf[p++] = 0;                       /* legacy_session_id: empty */
    buf[p++] = 0x00; buf[p++] = 0x02;   /* cipher_suites */
    buf[p++] = 0x13; buf[p++] = 0x01;
    buf[p++] = 0x01; buf[p++] = 0x00;   /* compression: null */
    uint32_t ext_len_pos = p; p += 2;
    uint32_t ext_start = p;

    /* supported_versions: TLS 1.3 */
    buf[p++] = 0x00; buf[p++] = EXT_SUPPORTED_VERSIONS;
    buf[p++] = 0x00; buf[p++] = 0x03;
    buf[p++] = 0x02; buf[p++] = 0x03; buf[p++] = 0x04;
    /* supported_groups: X25519 */
    buf[p++] = 0x00; buf[p++] = EXT_SUPPORTED_GROUPS;
    buf[p++] = 0x00; buf[p++] = 0x04;
    buf[p++] = 0x00; buf[p++] = 0x02;
    buf[p++] = 0x00; buf[p++] = 0x1D;
    /* signature_algorithms: what tls_verify_signature checks */
    uint8_t sigs[32];
    uint32_t sl = tls_signature_schemes(sigs, 1);
    buf[p++] = 0x00; buf[p++] = EXT_SIG_ALGS;
    buf[p++] = (uint8_t)((sl + 2) >> 8); buf[p++] = (uint8_t)(sl + 2);
    buf[p++] = (uint8_t)(sl >> 8); buf[p++] = (uint8_t)sl;
    memcpy(buf + p, sigs, sl);
    p += sl;
    /* key_share: our X25519 key */
    buf[p++] = 0x00; buf[p++] = EXT_KEY_SHARE;
    buf[p++] = 0x00; buf[p++] = 0x26;
    buf[p++] = 0x00; buf[p++] = 0x24;
    buf[p++] = 0x00; buf[p++] = 0x1D;
    buf[p++] = 0x00; buf[p++] = 0x20;
    memcpy(buf + p, s13.client_pub, 32);
    p += 32;
    /* server_name */
    buf[p++] = 0x00; buf[p++] = EXT_SERVER_NAME;
    buf[p++] = (uint8_t)((5 + hn) >> 8); buf[p++] = (uint8_t)(5 + hn);
    buf[p++] = (uint8_t)((3 + hn) >> 8); buf[p++] = (uint8_t)(3 + hn);
    buf[p++] = 0x00;                    /* host_name */
    buf[p++] = (uint8_t)(hn >> 8); buf[p++] = (uint8_t)hn;
    memcpy(buf + p, hostname, hn);
    p += hn;

    uint32_t ext_len = p - ext_start;
    buf[ext_len_pos] = (uint8_t)(ext_len >> 8);
    buf[ext_len_pos + 1] = (uint8_t)ext_len;
    uint32_t hs_len_v = p - (hs_len_pos + 3);
    buf[hs_len_pos] = (uint8_t)(hs_len_v >> 16);
    buf[hs_len_pos + 1] = (uint8_t)(hs_len_v >> 8);
    buf[hs_len_pos + 2] = (uint8_t)hs_len_v;
    uint32_t rec_len = p - 5;
    buf[rec_len_pos] = (uint8_t)(rec_len >> 8);
    buf[rec_len_pos + 1] = (uint8_t)rec_len;
    th_append(buf + 5, p - 5);
    return (int)p;
}

/* SHA-256("HelloRetryRequest"): a ServerHello with this random asks for
 * another key share, which we don't do */
static const uint8_t hrr_random[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02,
    0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
    0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
};

static int parse_server_hello(const uint8_t *m, uint32_t len)
{
    if (len < 4 + 2 + 32 + 1 || m[0] != HS_SERVER_HELLO)
        return -1;
    uint32_t hl = ((uint32_t)m[1] << 16) | ((uint32_t)m[2] << 8) | m[3];
    if (4 + hl != len)
        return -1;
    if (memcmp(m + 6, hrr_random, 32) == 0) {
        serial_puts("[TLS1.3] the server asks for another key share (HelloRetryRequest)\n");
        return -1;
    }
    uint32_t off = 4 + 2 + 32;
    uint8_t sid = m[off++];
    off += sid;
    if (off + 3 > len)
        return -1;
    uint16_t suite = ((uint16_t)m[off] << 8) | m[off + 1];
    off += 3;                           /* suite, compression */
    if (suite != AES_128_GCM_SHA256)
        return -1;
    if (off + 2 > len)
        return -1;
    uint32_t ext_end = off + 2 + (((uint32_t)m[off] << 8) | m[off + 1]);
    off += 2;
    if (ext_end > len)
        return -1;
    int version = 0, key = 0;
    while (off + 4 <= ext_end) {
        uint16_t type = ((uint16_t)m[off] << 8) | m[off + 1];
        uint16_t elen = ((uint16_t)m[off + 2] << 8) | m[off + 3];
        off += 4;
        if (off + elen > ext_end)
            return -1;
        if (type == EXT_SUPPORTED_VERSIONS && elen == 2)
            version = m[off] == 0x03 && m[off + 1] == 0x04;
        else if (type == EXT_KEY_SHARE && elen == 36 &&
                 m[off] == 0x00 && m[off + 1] == 0x1D &&
                 m[off + 2] == 0x00 && m[off + 3] == 0x20) {
            memcpy(s13.server_pub, m + off + 4, 32);
            key = 1;
        }
        off += elen;
    }
    return version && key ? 0 : -1;
}

/* ── Encrypted handshake messages ────────────────────────────── */

/* The next whole handshake message (header included) at hs_buf, reading
 * and decrypting records until it's all there */
static int hs_next(uint32_t *msg_len)
{
    for (;;) {
        if (hs_len >= 4) {
            uint32_t ml = 4 + (((uint32_t)hs_buf[1] << 16) |
                               ((uint32_t)hs_buf[2] << 8) | hs_buf[3]);
            if (ml > HS_MAX)
                return -1;
            if (hs_len >= ml) {
                *msg_len = ml;
                return 0;
            }
        }
        uint8_t type, inner;
        uint32_t rl, pl;
        if (read_record(&type, rec_buf, sizeof(rec_buf), &rl) < 0)
            return -1;
        if (type == CT_CHANGE_CIPHER)
            continue;                   /* middlebox compatibility */
        if (type != CT_APP_DATA ||
            decrypt_record(rec_buf, rl, s13.s_hs_key, s13.s_hs_iv, s13.s_seq++,
                           &inner, &pl) < 0)
            return -1;
        if (inner == CT_ALERT) {
            serial_puts("[TLS1.3] the server sent an alert");
            if (pl >= 2) {
                serial_puts(", code ");
                serial_putdec(rec_buf[1]);
            }
            serial_puts("\n");
            return -1;
        }
        if (inner != CT_HANDSHAKE || hs_len + pl > HS_MAX)
            return -1;
        memcpy(hs_buf + hs_len, rec_buf, pl);
        hs_len += pl;
    }
}

/* That message into the transcript, and out of hs_buf */
static void hs_done(uint32_t ml)
{
    th_append(hs_buf, ml);
    memmove(hs_buf, hs_buf + ml, hs_len - ml);
    hs_len -= ml;
}

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* Certificate (RFC 8446 4.4.2): context, then entries of a DER cert and its
 * extensions; the chain verified for the host name */
static int check_certificate(uint32_t ml, const char *hostname, tls_pubkey_t *leaf)
{
    const uint8_t *certs[CHAIN_MAX];
    uint32_t lens[CHAIN_MAX];
    int n = 0;
    const uint8_t *m = hs_buf + 4, *end = hs_buf + ml;
    if (m + 1 > end || m + 1 + m[0] + 3 > end)
        return -1;
    m += 1 + m[0];                      /* certificate_request_context */
    const uint8_t *list_end = m + 3 + be24(m);
    m += 3;
    if (list_end != end)
        return -1;
    while (m < list_end) {
        if (m + 3 > list_end)
            return -1;
        uint32_t cl = be24(m);
        m += 3;
        if (m + cl + 2 > list_end || n == CHAIN_MAX)
            return -1;
        certs[n] = m;
        lens[n++] = cl;
        m += cl;
        m += 2 + (((uint32_t)m[0] << 8) | m[1]);   /* its extensions */
    }
    if (m != list_end)
        return -1;
    return tls_verify_chain(certs, lens, n, hostname, leaf);
}

/* CertificateVerify (RFC 8446 4.4.3): the leaf key's signature over 64
 * spaces, the context string, a zero and the transcript's hash */
static int check_cert_verify(uint32_t ml, const tls_pubkey_t *leaf)
{
    if (ml < 8)
        return -1;
    uint16_t scheme = ((uint16_t)hs_buf[4] << 8) | hs_buf[5];
    uint32_t sl = ((uint32_t)hs_buf[6] << 8) | hs_buf[7];
    if (8 + sl != ml || (scheme & 0xFF) == 0x01)    /* no PKCS#1 v1.5 in 1.3 */
        return -1;
    static const char ctx[] = "TLS 1.3, server CertificateVerify";
    uint8_t content[64 + sizeof(ctx) + 32];
    memset(content, 0x20, 64);
    memcpy(content + 64, ctx, sizeof(ctx));         /* with its zero */
    th_hash(content + 64 + sizeof(ctx));
    return tls_verify_signature(leaf, scheme, content, sizeof(content),
                                hs_buf + 8, sl);
}

/* ── The handshake ───────────────────────────────────────────── */

int tls13_connect(int tcp_conn, const char *hostname)
{
    memset(&s13, 0, sizeof(s13));
    hs_len = 0;
    s13.tcp_conn = tcp_conn;
    sha256_init(&s13.th);

    static uint8_t hello[512];
    int hello_len = build_client_hello(hello, sizeof(hello), hostname);
    if (hello_len < 0 || net_tcp_send(tcp_conn, hello, (uint32_t)hello_len) < 0)
        return -1;

    /* ServerHello, in the clear */
    uint8_t type;
    uint32_t rl;
    if (read_record(&type, rec_buf, sizeof(rec_buf), &rl) < 0 || type != CT_HANDSHAKE) {
        serial_puts("[TLS1.3] no ServerHello\n");
        return -1;
    }
    if (parse_server_hello(rec_buf, rl) < 0) {
        serial_puts("[TLS1.3] the server doesn't do TLS 1.3 as we ask it to\n");
        return -1;
    }
    th_append(rec_buf, rl);

    /* Handshake secrets (RFC 8446 7.1) */
    x25519(s13.shared, s13.client_priv, s13.server_pub);
    uint8_t early[32], empty_hash[32], derived[32], th[32];
    hkdf_extract(zero32, 32, zero32, 32, early);
    sha256("", 0, empty_hash);
    hkdf_expand_label(early, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(derived, 32, s13.shared, 32, s13.handshake_secret);
    th_hash(th);
    hkdf_expand_label(s13.handshake_secret, "c hs traffic", th, 32, s13.c_hs_traffic, 32);
    hkdf_expand_label(s13.handshake_secret, "s hs traffic", th, 32, s13.s_hs_traffic, 32);
    derive_key_iv(s13.c_hs_traffic, s13.c_hs_key, s13.c_hs_iv);
    derive_key_iv(s13.s_hs_traffic, s13.s_hs_key, s13.s_hs_iv);

    uint32_t ml;
    tls_pubkey_t leaf;
    if (hs_next(&ml) < 0 || hs_buf[0] != HS_ENCRYPTED_EXT)
        return fail(ALERT_UNEXPECTED, "expected EncryptedExtensions");
    hs_done(ml);
    if (hs_next(&ml) < 0)
        return fail(ALERT_UNEXPECTED, "no Certificate");
    if (hs_buf[0] == HS_CERT_REQUEST)
        return fail(ALERT_UNEXPECTED, "the server wants a client certificate");
    if (hs_buf[0] != HS_CERTIFICATE)
        return fail(ALERT_UNEXPECTED, "expected Certificate");
    if (check_certificate(ml, hostname, &leaf) < 0)
        return fail(ALERT_BAD_CERTIFICATE, "the server's certificate is refused");
    hs_done(ml);
    if (hs_next(&ml) < 0 || hs_buf[0] != HS_CERT_VERIFY)
        return fail(ALERT_UNEXPECTED, "expected CertificateVerify");
    if (check_cert_verify(ml, &leaf) < 0)
        return fail(ALERT_DECRYPT_ERROR, "the server's CertificateVerify signature is wrong");
    serial_puts("[TLS1.3] the server proved it holds the certificate's key (");
    serial_puts(tls_scheme_name(((uint16_t)hs_buf[4] << 8) | hs_buf[5]));
    serial_puts(")\n");
    hs_done(ml);
    if (hs_next(&ml) < 0 || hs_buf[0] != HS_FINISHED || ml != 36)
        return fail(ALERT_UNEXPECTED, "expected Finished");
    uint8_t fk[32], expect[32];
    hkdf_expand_label(s13.s_hs_traffic, "finished", 0, 0, fk, 32);
    th_hash(th);
    hmac_sha256(fk, 32, th, 32, expect);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= expect[i] ^ hs_buf[4 + i];
    if (diff)
        return fail(ALERT_DECRYPT_ERROR, "the server's Finished is wrong");
    hs_done(ml);

    /* Our Finished, then the application secrets over the transcript
     * through the server's Finished */
    uint8_t th_sf[32], cfk[32], fin[36] = { HS_FINISHED, 0, 0, 32 };
    th_hash(th_sf);
    hkdf_expand_label(s13.c_hs_traffic, "finished", 0, 0, cfk, 32);
    hmac_sha256(cfk, 32, th_sf, 32, fin + 4);
    if (send_encrypted(CT_HANDSHAKE, fin, 36, s13.c_hs_key, s13.c_hs_iv, s13.c_seq++) < 0)
        return -1;
    th_append(fin, 36);
    uint8_t master[32];
    hkdf_expand_label(s13.handshake_secret, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(derived, 32, zero32, 32, master);
    hkdf_expand_label(master, "c ap traffic", th_sf, 32, s13.c_ap_traffic, 32);
    hkdf_expand_label(master, "s ap traffic", th_sf, 32, s13.s_ap_traffic, 32);
    derive_key_iv(s13.c_ap_traffic, s13.c_ap_key, s13.c_ap_iv);
    derive_key_iv(s13.s_ap_traffic, s13.s_ap_key, s13.s_ap_iv);
    s13.c_seq = s13.s_seq = 0;
    s13.active = 1;
    serial_puts("[TLS1.3] handshake complete: TLS_AES_128_GCM_SHA256, server authenticated\n");
    return 0;
}

/* ── Application data ────────────────────────────────────────── */

int tls13_send(const void *data, uint32_t len)
{
    if (!s13.active)
        return -1;
    const uint8_t *p = data;
    for (uint32_t done = 0; done < len; ) {
        uint32_t n = len - done < 16384 ? len - done : 16384;
        if (send_encrypted(CT_APP_DATA, p + done, n, s13.c_ap_key, s13.c_ap_iv,
                           s13.c_seq++) < 0)
            return -1;
        done += n;
    }
    return (int)len;
}

/* Up to cap bytes of application data; 0 when the server closed, -1 on
 * an error */
int tls13_recv(void *buf, uint32_t cap)
{
    if (!s13.active)
        return -1;
    while (s13.app_pos >= s13.app_len) {
        uint8_t type, inner;
        uint32_t rl, pl;
        if (read_record(&type, rec_buf, sizeof(rec_buf), &rl) < 0)
            return -1;
        if (type == CT_CHANGE_CIPHER)
            continue;
        if (type != CT_APP_DATA ||
            decrypt_record(rec_buf, rl, s13.s_ap_key, s13.s_ap_iv, s13.s_seq++,
                           &inner, &pl) < 0) {
            serial_puts("[TLS1.3] a record didn't decrypt\n");
            return -1;
        }
        if (inner == CT_ALERT) {
            s13.active = 0;
            return pl >= 2 && rec_buf[1] == 0 ? 0 : -1;    /* close_notify */
        }
        if (inner == CT_HANDSHAKE)
            continue;                   /* NewSessionTicket: no resumption */
        if (inner != CT_APP_DATA)
            return -1;
        memcpy(s13.app_buf, rec_buf, pl);
        s13.app_len = pl;
        s13.app_pos = 0;
    }
    uint32_t n = s13.app_len - s13.app_pos;
    if (n > cap)
        n = cap;
    memcpy(buf, s13.app_buf + s13.app_pos, n);
    s13.app_pos += n;
    return (int)n;
}

void tls13_close(void)
{
    if (s13.active) {
        uint8_t msg[2] = { 1, 0 };      /* warning, close_notify */
        send_encrypted(CT_ALERT, msg, 2, s13.c_ap_key, s13.c_ap_iv, s13.c_seq++);
    }
    s13.active = 0;
    memset(&s13, 0, sizeof(s13));       /* the keys too */
}

int tls13_is_active(void)
{
    return s13.active;
}
