/*
 * OsitoK x86-64 — TLS 1.2 Client
 *
 * Minimal TLS 1.2 with ECDHE (X25519) and AES-128-GCM-SHA256, the server's
 * certificate RSA (0xC02F) or ECDSA (0xC02B): the fallback for servers
 * without TLS 1.3 (http.c). The server is authenticated: its chain must
 * verify for the host name (tls_verify_chain) and the leaf mustn't be
 * revoked (tls_check_revocation), its ServerKeyExchange must
 * carry the leaf key's signature over both randoms and the key, and its
 * Finished must match. Handshake messages are reassembled across records.
 * No session resumption. No client certs. No renegotiation.
 *
 * Flow: ClientHello → ServerHello + Cert + ServerKeyExchange +
 *       ServerHelloDone → ClientKeyExchange + ChangeCipherSpec +
 *       Finished → ChangeCipherSpec + Finished → Application Data
 */

#include "tls.h"
#include "tls13.h"
#include "crypto.h"
#include "ecdh.h"

/* ── External dependencies ───────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);
extern void random_bytes(void *buf, uint32_t len);     /* random_drbg.c */

/* TCP API */
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size,
                                  uint32_t timeout_ticks);
extern int  net_tcp_state(int conn);

/* ── Helpers ─────────────────────────────────────────────────── */

static void *tmemcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void tmemset(void *dst, int c, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)c;
}

static uint32_t tstrlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void put24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline uint16_t get16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static inline uint32_t get24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static inline uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* ── TLS Record Send ─────────────────────────────────────────── */

/* Send a TLS record (plaintext, before encryption is active) */
static int tls_send_record(tls_conn_t *tls, uint8_t content_type,
                            const void *data, uint32_t len)
{
    uint8_t hdr[5];
    hdr[0] = content_type;
    put16(hdr + 1, TLS_VER_12);
    put16(hdr + 3, (uint16_t)len);

    if (net_tcp_send(tls->tcp_conn, hdr, 5) < 0) return -1;
    if (len > 0 && net_tcp_send(tls->tcp_conn, data, len) < 0) return -1;

    return 0;
}

/* Send encrypted TLS record (after ChangeCipherSpec) */
static int tls_send_encrypted(tls_conn_t *tls, uint8_t content_type,
                                const void *data, uint32_t len)
{
    /* GCM nonce = implicit_iv(4) || explicit_nonce(8) */
    uint8_t nonce[12];
    tmemcpy(nonce, tls->client_write_iv, 4);
    /* Explicit nonce = sequence number (big-endian) */
    uint8_t seq_bytes[8];
    uint64_t seq = tls->client_seq;
    for (int i = 7; i >= 0; i--) {
        seq_bytes[i] = (uint8_t)(seq & 0xFF);
        seq >>= 8;
    }
    tmemcpy(nonce + 4, seq_bytes, 8);

    /* AAD = seq_num(8) + content_type(1) + version(2) + length(2) */
    uint8_t aad[13];
    tmemcpy(aad, seq_bytes, 8);
    aad[8] = content_type;
    put16(aad + 9, TLS_VER_12);
    put16(aad + 11, (uint16_t)len);

    /* Encrypt */
    static uint8_t ct[TLS_MAX_RECORD];      /* off the task stack; one TLS user at a time (http.c's owner) */
    uint8_t tag[16];
    if (len > TLS_MAX_RECORD) return -1;

    aes128_gcm_encrypt(tls->client_write_key, nonce,
                        aad, 13, data, len, ct, tag);

    /* TLS record: hdr(5) + explicit_nonce(8) + ciphertext(len) + tag(16) */
    uint32_t record_len = 8 + len + 16;
    uint8_t hdr[5];
    hdr[0] = content_type;
    put16(hdr + 1, TLS_VER_12);
    put16(hdr + 3, (uint16_t)record_len);

    if (net_tcp_send(tls->tcp_conn, hdr, 5) < 0) return -1;
    if (net_tcp_send(tls->tcp_conn, seq_bytes, 8) < 0) return -1;
    if (net_tcp_send(tls->tcp_conn, ct, len) < 0) return -1;
    if (net_tcp_send(tls->tcp_conn, tag, 16) < 0) return -1;

    tls->client_seq++;
    return (int)len;
}

/* ── TLS Record Receive ──────────────────────────────────────── */

/* Read exactly `needed` bytes from TCP into tls->rx_buf + tls->rx_len.
 * Returns 0 on success, -1 on timeout/error. */
static int tls_read_exact(tls_conn_t *tls, uint32_t needed,
                           uint32_t timeout_ticks)
{
    uint64_t deadline = idt_get_ticks() + timeout_ticks;

    while (tls->rx_len < needed) {
        if (idt_get_ticks() > deadline) return -1;

        uint32_t space = sizeof(tls->rx_buf) - tls->rx_len;
        if (space == 0) return -1;

        int r = net_tcp_recv_timeout(tls->tcp_conn,
                                      tls->rx_buf + tls->rx_len,
                                      space, 100);  /* 1s poll */
        if (r > 0)
            tls->rx_len += (uint32_t)r;
        else if (r < 0)
            return -1;  /* Connection closed */
    }
    return 0;
}

/* Consume `n` bytes from front of rx_buf */
static void tls_consume(tls_conn_t *tls, uint32_t n)
{
    if (n >= tls->rx_len) {
        tls->rx_len = 0;
        return;
    }
    /* Shift remaining data forward */
    uint32_t remaining = tls->rx_len - n;
    for (uint32_t i = 0; i < remaining; i++)
        tls->rx_buf[i] = tls->rx_buf[i + n];
    tls->rx_len = remaining;
}

/* Read a TLS record. Returns content type, fills `out` with plaintext,
 * sets `out_len`. Returns -1 on error. */
static int tls_recv_record(tls_conn_t *tls, uint8_t *out, uint32_t *out_len,
                            uint32_t out_cap, uint32_t timeout_ticks)
{
    /* Read 5-byte record header */
    if (tls_read_exact(tls, 5, timeout_ticks) < 0) return -1;

    uint8_t content_type = tls->rx_buf[0];
    /* uint16_t version = get16(tls->rx_buf + 1); */
    uint16_t rec_len = get16(tls->rx_buf + 3);

    if (rec_len > TLS_MAX_RECORD + 256) {
        serial_puts("[TLS] Record too large: ");
        serial_putdec(rec_len);
        serial_puts("\n");
        return -1;
    }

    /* Read record body */
    if (tls_read_exact(tls, 5 + rec_len, timeout_ticks) < 0) return -1;

    uint8_t *body = tls->rx_buf + 5;

    if (tls->server_cipher_active) {
        /* Decrypt: body = explicit_nonce(8) + ciphertext + tag(16) */
        if (rec_len < 24) return -1;  /* 8 + 0 + 16 minimum */

        uint32_t ct_len = rec_len - 8 - 16;
        if (ct_len > out_cap) return -1;

        uint8_t nonce[12];
        tmemcpy(nonce, tls->server_write_iv, 4);
        tmemcpy(nonce + 4, body, 8);  /* explicit nonce */

        /* AAD */
        uint8_t aad[13];
        uint8_t seq_bytes[8];
        uint64_t seq = tls->server_seq;
        for (int i = 7; i >= 0; i--) {
            seq_bytes[i] = (uint8_t)(seq & 0xFF);
            seq >>= 8;
        }
        tmemcpy(aad, seq_bytes, 8);
        aad[8] = content_type;
        put16(aad + 9, TLS_VER_12);
        put16(aad + 11, (uint16_t)ct_len);

        int r = aes128_gcm_decrypt(tls->server_write_key, nonce,
                                    aad, 13,
                                    body + 8, ct_len,
                                    out,
                                    body + 8 + ct_len);
        if (r != 0) {
            serial_puts("[TLS] GCM decrypt/verify failed\n");
            tls_consume(tls, 5 + rec_len);
            return -1;
        }

        *out_len = ct_len;
        tls->server_seq++;
    } else {
        /* Plaintext record (handshake phase) */
        if (rec_len > out_cap) return -1;
        tmemcpy(out, body, rec_len);
        *out_len = rec_len;
    }

    tls_consume(tls, 5 + rec_len);
    return content_type;
}

/* ── ClientHello ─────────────────────────────────────────────── */

static int tls_send_client_hello(tls_conn_t *tls, const char *hostname)
{
    uint8_t msg[512];
    uint32_t pos = 0;

    /* Handshake header (will patch length later) */
    msg[pos++] = TLS_HS_CLIENT_HELLO;
    pos += 3;  /* length placeholder */

    /* Client version */
    put16(msg + pos, TLS_VER_12); pos += 2;

    /* Client random */
    random_bytes(tls->client_random, 32);
    tmemcpy(msg + pos, tls->client_random, 32); pos += 32;

    /* Session ID (empty) */
    msg[pos++] = 0;

    /* Cipher suites: ECDHE with an ECDSA or RSA certificate, AES-128-GCM */
    put16(msg + pos, 4); pos += 2;
    put16(msg + pos, 0xC02B); pos += 2;
    put16(msg + pos, 0xC02F); pos += 2;

    /* Compression methods: null only */
    msg[pos++] = 1;  /* 1 method */
    msg[pos++] = 0;  /* null */

    /* Extensions */
    uint32_t ext_start = pos;
    pos += 2;  /* extensions length placeholder */

    /* SNI extension (0x0000) */
    if (hostname) {
        uint32_t hn_len = tstrlen(hostname);
        put16(msg + pos, 0x0000); pos += 2;  /* extension type */
        put16(msg + pos, (uint16_t)(hn_len + 5)); pos += 2;  /* ext data len */
        put16(msg + pos, (uint16_t)(hn_len + 3)); pos += 2;  /* server name list len */
        msg[pos++] = 0;  /* host_name type */
        put16(msg + pos, (uint16_t)hn_len); pos += 2;
        tmemcpy(msg + pos, hostname, hn_len); pos += hn_len;
    }

    /* Supported Groups extension (0x000A) — x25519, secp256r1, secp384r1 */
    put16(msg + pos, 0x000A); pos += 2;
    put16(msg + pos, 8); pos += 2;  /* ext len */
    put16(msg + pos, 6); pos += 2;  /* named curve list len */
    put16(msg + pos, 0x001D); pos += 2;  /* x25519 */
    put16(msg + pos, 0x0017); pos += 2;  /* secp256r1 */
    put16(msg + pos, 0x0018); pos += 2;  /* secp384r1 */

    /* EC Point Formats (0x000B) — uncompressed only */
    put16(msg + pos, 0x000B); pos += 2;
    put16(msg + pos, 2); pos += 2;
    msg[pos++] = 1;  /* 1 format */
    msg[pos++] = 0;  /* uncompressed */

    /* Signature Algorithms (0x000D): what tls_verify_signature checks */
    uint8_t sigs[32];
    uint32_t sl = tls_signature_schemes(sigs, 0);
    put16(msg + pos, 0x000D); pos += 2;
    put16(msg + pos, (uint16_t)(sl + 2)); pos += 2;
    put16(msg + pos, (uint16_t)sl); pos += 2;
    tmemcpy(msg + pos, sigs, sl); pos += sl;

    /* Patch extensions length */
    uint32_t ext_len = pos - ext_start - 2;
    put16(msg + ext_start, (uint16_t)ext_len);

    /* Patch handshake length (total - 4 byte header) */
    uint32_t hs_len = pos - 4;
    put24(msg + 1, hs_len);

    /* Hash this handshake message */
    sha256_update(&tls->hs_hash, msg, pos);

    /* Send as TLS record */
    if (tls_send_record(tls, TLS_HANDSHAKE, msg, pos) < 0) return -1;

    serial_puts("[TLS] ClientHello sent (");
    serial_putdec(pos);
    serial_puts(" bytes)\n");

    tls->state = TLS_STATE_CLIENT_HELLO_SENT;
    return 0;
}

/* ── Parse Server Handshake Messages ─────────────────────────── */

/* Parse ServerHello from handshake body (after type + length) */
static int parse_server_hello(tls_conn_t *tls, const uint8_t *data, uint32_t len)
{
    if (len < 38) return -1;

    uint16_t version = get16(data);
    if (version != TLS_VER_12) {
        serial_puts("[TLS] Unsupported version: 0x");
        serial_puthex(version, 4);
        serial_puts("\n");
        return -1;
    }

    tmemcpy(tls->server_random, data + 2, 32);

    /* A TLS 1.3 server negotiating 1.2 says so at the end of its random
     * (RFC 8446 4.1.3). We tried 1.3 first, so this means someone made
     * that attempt fail: a downgrade. */
    static const uint8_t downgrade[8] = { 'D', 'O', 'W', 'N', 'G', 'R', 'D', 1 };
    int sentinel = 1;
    for (int i = 0; i < 8; i++)
        sentinel &= tls->server_random[24 + i] == downgrade[i];
    if (sentinel) {
        serial_puts("[TLS] The server does TLS 1.3, but our 1.3 attempt failed: downgrade, refusing\n");
        return -1;
    }

    uint8_t sid_len = data[34];
    uint32_t pos = 35 + sid_len;

    if (pos + 3 > len) return -1;
    uint16_t cipher = get16(data + pos);
    pos += 2;

    if (cipher != 0xC02F && cipher != 0xC02B) {
        serial_puts("[TLS] Server chose unsupported cipher: 0x");
        serial_puthex(cipher, 4);
        serial_puts("\n");
        return -1;
    }
    tls->suite = cipher;

    /* uint8_t compression = data[pos]; */
    /* Skip extensions */

    serial_puts(cipher == 0xC02B ? "[TLS] ServerHello: TLS 1.2, ECDHE-ECDSA-AES128-GCM-SHA256\n"
                                 : "[TLS] ServerHello: TLS 1.2, ECDHE-RSA-AES128-GCM-SHA256\n");
    return 0;
}

/* Certificate: the chain (each a 3-byte length and the DER), verified for
 * the host name and checked for revocation; the leaf's key kept for the
 * ServerKeyExchange */
static int verify_certificate(tls_conn_t *tls, const uint8_t *data, uint32_t len,
                              const char *hostname)
{
    const uint8_t *certs[8];
    uint32_t lens[8];
    int n = 0;
    if (len < 3 || 3 + get24(data) != len)
        return -1;
    for (uint32_t pos = 3; pos < len; ) {
        if (pos + 3 > len || n == 8)
            return -1;
        uint32_t cl = get24(data + pos);
        pos += 3;
        if (pos + cl > len)
            return -1;
        certs[n] = data + pos;
        lens[n++] = cl;
        pos += cl;
    }
    if (tls_verify_chain(certs, lens, n, hostname, &tls->leaf) < 0)
        return -1;
    return n >= 2 ? tls_check_revocation(certs, lens, 0, 0) : 0;
}

/* Parse ServerKeyExchange — extract ECDHE public key */
static int parse_server_key_exchange(tls_conn_t *tls,
                                      const uint8_t *data, uint32_t len)
{
    if (len < 4) return -1;

    /* EC params: curve_type(1) + named_curve(2) + pubkey_len(1) + pubkey */
    uint8_t curve_type = data[0];
    uint16_t named_curve = get16(data + 1);
    uint8_t pubkey_len = data[3];

    if (curve_type != 3) {  /* named_curve */
        serial_puts("[TLS] Unsupported curve type: ");
        serial_putdec(curve_type);
        serial_puts("\n");
        return -1;
    }

    if (named_curve != 0x001D && !ecdh_point_len(named_curve)) {  /* x25519, P-256, P-384 */
        serial_puts("[TLS] Unsupported curve: 0x");
        serial_puthex(named_curve, 4);
        serial_puts("\n");
        return -1;
    }
    tls->group = named_curve;

    if (pubkey_len != (named_curve == 0x001D ? 32 : ecdh_point_len(named_curve)) ||
        4 + (uint32_t)pubkey_len > len) return -1;

    /* The signature: algorithm (2), length (2), the bytes; by the leaf's
     * key, of the kind the suite names, over both randoms and the params */
    uint32_t params = 4 + pubkey_len;
    if (params + 4 > len) return -1;
    uint16_t scheme = get16(data + params);
    uint32_t sig_len = get16(data + params + 2);
    if (params + 4 + sig_len != len) return -1;
    int want_ec = tls->suite == 0xC02B;
    if ((tls->leaf.type == 2) != want_ec || (scheme & 0xFF) == (want_ec ? 0x01 : 0x03)) {
        serial_puts("[TLS] ServerKeyExchange: the signature doesn't match the suite\n");
        return -1;
    }
    static uint8_t signed_data[64 + 4 + 255];
    tmemcpy(signed_data, tls->client_random, 32);
    tmemcpy(signed_data + 32, tls->server_random, 32);
    tmemcpy(signed_data + 64, data, params);
    if (tls_verify_signature(&tls->leaf, scheme, signed_data, 64 + params,
                             data + params + 4, sig_len) < 0) {
        serial_puts("[TLS] ServerKeyExchange: the server's signature is wrong\n");
        return -1;
    }

    /* Compute shared secret */
    if (named_curve != 0x001D) {
        if (ecdh_keygen(named_curve, tls->ec_priv, tls->ec_pub) < 0 ||
            ecdh_shared(named_curve, tls->ec_priv, data + 4, pubkey_len, tls->premaster_secret) < 0)
            return -1;
        tls->premaster_len = ecdh_scalar_len(named_curve);
        serial_puts("[TLS] ServerKeyExchange: signed by the certificate's key (");
        serial_puts(tls_scheme_name(scheme));
        serial_puts(named_curve == 0x0017 ? "); P-256 secret computed\n"
                                          : "); P-384 secret computed\n");
        return 0;
    }
    uint8_t server_pubkey[32];
    tmemcpy(server_pubkey, data + 4, 32);

    x25519(tls->premaster_secret, tls->ecdhe_privkey, server_pubkey);
    tls->premaster_len = 32;

    serial_puts("[TLS] ServerKeyExchange: signed by the certificate's key (");
    serial_puts(tls_scheme_name(scheme));
    serial_puts("); x25519 secret computed\n");
    return 0;
}

/* ── Derive Keys ─────────────────────────────────────────────── */

static void tls_derive_keys(tls_conn_t *tls)
{
    /* Master secret = PRF(pre_master_secret, "master secret",
     *                     client_random + server_random)[0..47] */
    uint8_t seed[64];
    tmemcpy(seed, tls->client_random, 32);
    tmemcpy(seed + 32, tls->server_random, 32);

    tls_prf_sha256(tls->premaster_secret, tls->premaster_len,
                    "master secret", seed, 64,
                    tls->master_secret, 48);

    /* Key expansion = PRF(master_secret, "key expansion",
     *                     server_random + client_random)
     * For AES-128-GCM: client_write_key(16) + server_write_key(16) +
     *                   client_write_IV(4) + server_write_IV(4) = 40 bytes */
    uint8_t seed2[64];
    tmemcpy(seed2, tls->server_random, 32);
    tmemcpy(seed2 + 32, tls->client_random, 32);

    uint8_t key_block[40];
    tls_prf_sha256(tls->master_secret, 48,
                    "key expansion", seed2, 64,
                    key_block, 40);

    tmemcpy(tls->client_write_key, key_block, 16);
    tmemcpy(tls->server_write_key, key_block + 16, 16);
    tmemcpy(tls->client_write_iv, key_block + 32, 4);
    tmemcpy(tls->server_write_iv, key_block + 36, 4);

    tls->client_seq = 0;
    tls->server_seq = 0;

    serial_puts("[TLS] Keys derived\n");
}

/* ── ClientKeyExchange + ChangeCipherSpec + Finished ──────── */

static int tls_send_client_finish(tls_conn_t *tls)
{
    /* 1. ClientKeyExchange: our ECDHE public key, X25519, P-256 or P-384 */
    {
        uint8_t msg[4 + 1 + ECDH_MAX_POINT];
        uint32_t kl = tls->group == 0x001D ? 32 : ecdh_point_len(tls->group);
        msg[0] = TLS_HS_CLIENT_KEY_EXCH;
        put24(msg + 1, 1 + kl);
        msg[4] = (uint8_t)kl;
        tmemcpy(msg + 5, tls->group == 0x001D ? tls->ecdhe_pubkey : tls->ec_pub, kl);

        sha256_update(&tls->hs_hash, msg, 5 + kl);

        if (tls_send_record(tls, TLS_HANDSHAKE, msg, 5 + kl) < 0)
            return -1;

        serial_puts("[TLS] ClientKeyExchange sent\n");
    }

    /* Derive keys now that we have all key material */
    tls_derive_keys(tls);

    /* 2. ChangeCipherSpec */
    {
        uint8_t ccs = 1;
        if (tls_send_record(tls, TLS_CHANGE_CIPHER, &ccs, 1) < 0)
            return -1;
        tls->client_cipher_active = true;
        serial_puts("[TLS] ChangeCipherSpec sent\n");
    }

    /* 3. Finished (encrypted) */
    {
        /* verify_data = PRF(master_secret, "client finished",
         *                   Hash(handshake_messages))[0..11] */
        sha256_ctx hash_copy;
        tmemcpy(&hash_copy, &tls->hs_hash, sizeof(sha256_ctx));

        uint8_t hs_digest[32];
        sha256_final(&hash_copy, hs_digest);

        uint8_t verify_data[12];
        tls_prf_sha256(tls->master_secret, 48,
                        "client finished", hs_digest, 32,
                        verify_data, 12);

        /* Build Finished handshake message */
        uint8_t finished[16];
        finished[0] = TLS_HS_FINISHED;
        put24(finished + 1, 12);
        tmemcpy(finished + 4, verify_data, 12);

        /* Hash the Finished message too (needed for server's verify) */
        sha256_update(&tls->hs_hash, finished, 16);

        /* Send encrypted */
        if (tls_send_encrypted(tls, TLS_HANDSHAKE, finished, 16) < 0)
            return -1;

        serial_puts("[TLS] Finished sent (encrypted)\n");
    }

    tls->state = TLS_STATE_KEYS_EXCHANGED;
    return 0;
}

/* ── Server Handshake Processing ─────────────────────────────── */

/* The server's flight, reassembled: a certificate chain often spans
 * several records, and one record can hold several messages */
static uint8_t hs_buf[65536];
static uint32_t hs_len;

/* The next whole handshake message (header included) at hs_buf */
static int next_message(tls_conn_t *tls, uint32_t *msg_len)
{
    static uint8_t rec[TLS_MAX_RECORD + 256];
    for (;;) {
        if (hs_len >= 4) {
            uint32_t ml = 4 + get24(hs_buf + 1);
            if (ml > sizeof(hs_buf))
                return -1;
            if (hs_len >= ml) {
                *msg_len = ml;
                return 0;
            }
        }
        uint32_t rl;
        int ct = tls_recv_record(tls, rec, &rl, sizeof(rec), 1000);
        if (ct == TLS_ALERT && rl >= 2) {
            serial_puts("[TLS] Alert received: level=");
            serial_putdec(rec[0]);
            serial_puts(" desc=");
            serial_putdec(rec[1]);
            serial_puts("\n");
            return -1;
        }
        if (ct != TLS_HANDSHAKE || hs_len + rl > sizeof(hs_buf)) {
            serial_puts("[TLS] Failed to receive the server's handshake\n");
            return -1;
        }
        tmemcpy(hs_buf + hs_len, rec, rl);
        hs_len += rl;
    }
}

/* That message into the transcript, and out of hs_buf */
static void message_done(tls_conn_t *tls, uint32_t ml)
{
    sha256_update(&tls->hs_hash, hs_buf, ml);
    for (uint32_t i = ml; i < hs_len; i++)
        hs_buf[i - ml] = hs_buf[i];
    hs_len -= ml;
}

/* Read and process the server's handshake messages, in order, until
 * ServerHelloDone. Then read ChangeCipherSpec + encrypted Finished. */
static int tls_process_server_handshake(tls_conn_t *tls, const char *hostname)
{
    static uint8_t buf[TLS_MAX_HS_MSG];
    uint32_t buf_len, ml;

    hs_len = 0;
    if (next_message(tls, &ml) < 0 || hs_buf[0] != TLS_HS_SERVER_HELLO ||
        parse_server_hello(tls, hs_buf + 4, ml - 4) < 0)
        return -1;
    message_done(tls, ml);
    if (next_message(tls, &ml) < 0 || hs_buf[0] != TLS_HS_CERTIFICATE) {
        serial_puts("[TLS] Expected the server's Certificate\n");
        return -1;
    }
    if (verify_certificate(tls, hs_buf + 4, ml - 4, hostname) < 0)
        return -1;
    message_done(tls, ml);
    if (next_message(tls, &ml) < 0 || hs_buf[0] != TLS_HS_SERVER_KEY_EXCH ||
        parse_server_key_exchange(tls, hs_buf + 4, ml - 4) < 0)
        return -1;
    message_done(tls, ml);
    if (next_message(tls, &ml) < 0)
        return -1;
    if (hs_buf[0] == TLS_HS_CERT_REQUEST) {
        serial_puts("[TLS] The server wants a client certificate\n");
        return -1;
    }
    if (hs_buf[0] != TLS_HS_SERVER_HELLO_DONE || ml != 4 || hs_len != ml) {
        serial_puts("[TLS] Expected ServerHelloDone\n");
        return -1;
    }
    message_done(tls, ml);
    serial_puts("[TLS] ServerHelloDone\n");

    /* Send our side: ClientKeyExchange + CCS + Finished */
    if (tls_send_client_finish(tls) < 0) return -1;

    /* Phase 2: Receive server's ChangeCipherSpec + Finished */
    /* CCS is a separate content type, not handshake */
    {
        int ct = tls_recv_record(tls, buf, &buf_len, sizeof(buf), 1000);
        if (ct == TLS_ALERT && buf_len >= 2) {
            serial_puts("[TLS] Server alert: level=");
            serial_putdec(buf[0]);
            serial_puts(" desc=");
            serial_putdec(buf[1]);
            serial_puts("\n");
            return -1;
        }
        if (ct != TLS_CHANGE_CIPHER || buf_len != 1 || buf[0] != 1) {
            serial_puts("[TLS] Expected ChangeCipherSpec, got type ");
            serial_putdec(ct);
            serial_puts(" len=");
            serial_putdec(buf_len);
            serial_puts("\n");
            return -1;
        }
        tls->server_cipher_active = true;
        serial_puts("[TLS] Server ChangeCipherSpec received\n");
    }

    tls->state = TLS_STATE_ESTABLISHED;

    /* Receive server Finished (encrypted) */
    {
        int ct = tls_recv_record(tls, buf, &buf_len, sizeof(buf), 1000);
        if (ct != TLS_HANDSHAKE) {
            serial_puts("[TLS] Expected Finished, got type ");
            serial_putdec(ct);
            serial_puts("\n");
            return -1;
        }

        if (buf_len < 16 || buf[0] != TLS_HS_FINISHED) {
            serial_puts("[TLS] Invalid Finished message\n");
            return -1;
        }

        /* Verify server's verify_data */
        sha256_ctx hash_copy;
        tmemcpy(&hash_copy, &tls->hs_hash, sizeof(sha256_ctx));
        uint8_t hs_digest[32];
        sha256_final(&hash_copy, hs_digest);

        uint8_t expected_verify[12];
        tls_prf_sha256(tls->master_secret, 48,
                        "server finished", hs_digest, 32,
                        expected_verify, 12);

        uint8_t diff = 0;
        for (int i = 0; i < 12; i++)
            diff |= buf[4 + i] ^ expected_verify[i];

        if (diff) {
            /* The transcript isn't what the server saw: someone in the way */
            serial_puts("[TLS] Server Finished verify_data mismatch\n");
            tls->state = TLS_STATE_ERROR;
            return -1;
        }
        serial_puts("[TLS] Server Finished verified OK\n");
    }

    serial_puts("[TLS] Handshake complete: TLS 1.2, server authenticated\n");
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

int tls_connect(tls_conn_t *tls, int tcp_conn, const char *hostname)
{
    tmemset(tls, 0, sizeof(tls_conn_t));
    tls->tcp_conn = tcp_conn;
    tls->state = TLS_STATE_INIT;

    /* Initialize handshake hash */
    sha256_init(&tls->hs_hash);

    /* The ECDHE keypair */
    random_bytes(tls->ecdhe_privkey, 32);
    x25519_public(tls->ecdhe_pubkey, tls->ecdhe_privkey);

    serial_puts("[TLS] Connecting to ");
    if (hostname) serial_puts(hostname);
    serial_puts("...\n");

    /* Send ClientHello */
    if (tls_send_client_hello(tls, hostname) < 0) {
        tls->state = TLS_STATE_ERROR;
        return -1;
    }

    /* Process full server handshake */
    if (!hostname || tls_process_server_handshake(tls, hostname) < 0) {
        tls->state = TLS_STATE_ERROR;
        return -1;
    }

    return 0;
}

int tls_send(tls_conn_t *tls, const void *data, uint32_t len)
{
    if (tls->state != TLS_STATE_ESTABLISHED) return -1;
    return tls_send_encrypted(tls, TLS_APP_DATA, data, len);
}

int tls_recv(tls_conn_t *tls, void *buf, uint32_t buf_size,
             uint32_t timeout_ticks)
{
    if (!buf_size) return 0;
    if (!buf) return -1;
    if (tls->state != TLS_STATE_ESTABLISHED) return -1;

    if (tls->app_offset < tls->app_len) {
        uint32_t remaining = tls->app_len - tls->app_offset;
        uint32_t copy = remaining < buf_size ? remaining : buf_size;
        tmemcpy(buf,tls->app_buf+tls->app_offset,copy);
        tls->app_offset += copy;
        return (int)copy;
    }
    tls->app_offset = tls->app_len = 0;
    uint32_t rec_len = 0;

    int ct = tls_recv_record(tls, tls->app_buf, &rec_len, sizeof(tls->app_buf),
                              timeout_ticks);

    if (ct < 0) return -1;
    if (ct == TLS_ALERT) {
        if (rec_len < 2) { tls->state = TLS_STATE_ERROR; return -1; }
        serial_puts("[TLS] Alert: ");
        serial_putdec(tls->app_buf[0]);
        serial_puts("/");
        serial_putdec(tls->app_buf[1]);
        serial_puts("\n");
        if (tls->app_buf[0] == TLS_ALERT_FATAL || tls->app_buf[1] == 0) {
            tls->state = TLS_STATE_CLOSED;
            return -1;
        }
        return 0;
    }
    if (ct != TLS_APP_DATA) {
        serial_puts("[TLS] Unexpected record type in data phase: ");
        serial_putdec(ct);
        serial_puts("\n");
        return 0;
    }

    uint32_t copy = rec_len < buf_size ? rec_len : buf_size;
    tmemcpy(buf, tls->app_buf, copy);
    tls->app_len = rec_len;
    tls->app_offset = copy;
    return (int)copy;
}

void tls_close(tls_conn_t *tls)
{
    if (tls->state == TLS_STATE_ESTABLISHED) {
        /* Send close_notify alert */
        uint8_t alert[2] = { TLS_ALERT_WARNING, 0 };  /* close_notify = 0 */
        tls_send_encrypted(tls, TLS_ALERT, alert, 2);
    }
    tls->state = TLS_STATE_CLOSED;
    tls->app_len = tls->app_offset = 0;
}

/* Helper for shell.c which doesn't include tls.h */
uint32_t tls_conn_size(void)
{
    return sizeof(tls_conn_t);
}
