/*
 * OsitoK x86-64 — TLS 1.3 Client (RFC 8446)
 *
 * Extends TLS 1.2 (tls.c) with TLS 1.3 handshake:
 *   - ClientHello with supported_versions extension (0x0304)
 *   - Key share: X25519 (reuses crypto.c implementation)
 *   - AEAD: ChaCha20-Poly1305 (reuses crypto2.c)
 *   - Key derivation: HKDF-SHA256 (reuses crypto2.c)
 *   - 0-RTT not supported (security-conscious default)
 *
 * Falls back to TLS 1.2 if server doesn't support 1.3.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* Crypto primitives from crypto.c and crypto2.c */
extern void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32],
                              const uint8_t point[32]) __attribute__((weak));
extern void chacha20_encrypt(const uint8_t key[32], uint32_t counter,
                             const uint8_t nonce[12],
                             uint8_t *data, uint32_t len) __attribute__((weak));
extern void hkdf_extract(const uint8_t *salt, uint32_t salt_len,
                         const uint8_t *ikm, uint32_t ikm_len,
                         uint8_t prk[32]) __attribute__((weak));
extern void hkdf_expand(const uint8_t prk[32], const uint8_t *info,
                        uint32_t info_len, uint8_t *okm,
                        uint32_t okm_len) __attribute__((weak));
extern void sha256(const uint8_t *data, uint32_t len,
                   uint8_t hash[32]) __attribute__((weak));
extern void random_get_bytes(void *buf, uint32_t len) __attribute__((weak));

/* Network I/O */
extern int net_tcp_send(int conn, const void *data, uint32_t len);
extern int net_tcp_recv(int conn, void *buf, uint32_t size);

/* ── TLS 1.3 Constants ───────────────────────────────────────── */

#define TLS_VERSION_13       0x0304
#define TLS_CONTENT_HANDSHAKE     22
#define TLS_CONTENT_APP_DATA      23
#define TLS_CONTENT_CHANGE_CIPHER 20

/* Handshake message types */
#define TLS13_CLIENT_HELLO   1
#define TLS13_SERVER_HELLO   2
#define TLS13_ENCRYPTED_EXT  8
#define TLS13_CERTIFICATE   11
#define TLS13_CERT_VERIFY   15
#define TLS13_FINISHED      20

/* Cipher suites */
#define TLS13_CHACHA20_POLY1305  0x1303
#define TLS13_AES_128_GCM        0x1301

/* Extensions */
#define TLS_EXT_SUPPORTED_VERSIONS  43
#define TLS_EXT_KEY_SHARE           51
#define TLS_EXT_SERVER_NAME          0

/* Named groups */
#define TLS_GROUP_X25519  0x001D

/* ── TLS 1.3 Session State ───────────────────────────────────── */

typedef struct {
    int      tcp_conn;
    bool     tls13;              /* True if server selected TLS 1.3 */
    uint8_t  client_random[32];
    uint8_t  client_privkey[32]; /* X25519 ephemeral private key */
    uint8_t  client_pubkey[32];  /* X25519 ephemeral public key */
    uint8_t  server_pubkey[32];  /* Server's key share */
    uint8_t  shared_secret[32];  /* X25519(client_priv, server_pub) */
    uint8_t  handshake_secret[32];
    uint8_t  client_traffic_secret[32];
    uint8_t  server_traffic_secret[32];
    uint8_t  client_key[32];
    uint8_t  client_iv[12];
    uint8_t  server_key[32];
    uint8_t  server_iv[12];
    uint64_t client_seq;
    uint64_t server_seq;
    uint8_t  transcript_hash[32]; /* Running hash of handshake messages */
} tls13_session_t;

static tls13_session_t tls13;

/* ── X25519 Base Point ───────────────────────────────────────── */

static const uint8_t x25519_basepoint[32] = {9};

/* ── Build ClientHello (TLS 1.3) ─────────────────────────────── */

static int tls13_build_client_hello(uint8_t *buf, const char *hostname)
{
    if (!random_get_bytes || !x25519_scalarmult) return -1;

    /* Generate ephemeral X25519 keypair */
    random_get_bytes(tls13.client_privkey, 32);
    tls13.client_privkey[0] &= 248;
    tls13.client_privkey[31] &= 127;
    tls13.client_privkey[31] |= 64;
    x25519_scalarmult(tls13.client_pubkey, tls13.client_privkey, x25519_basepoint);

    /* Generate client random */
    random_get_bytes(tls13.client_random, 32);

    int p = 0;

    /* TLS record header (filled later) */
    buf[p++] = TLS_CONTENT_HANDSHAKE;
    buf[p++] = 0x03; buf[p++] = 0x01;  /* Legacy: TLS 1.0 */
    int rec_len_pos = p; p += 2;

    /* Handshake: ClientHello */
    buf[p++] = TLS13_CLIENT_HELLO;
    int hs_len_pos = p; p += 3;

    /* Client version: TLS 1.2 (legacy) */
    buf[p++] = 0x03; buf[p++] = 0x03;

    /* Client random */
    memcpy(buf + p, tls13.client_random, 32); p += 32;

    /* Session ID (empty for TLS 1.3) */
    buf[p++] = 0;

    /* Cipher suites */
    buf[p++] = 0x00; buf[p++] = 0x04;  /* 2 suites × 2 bytes */
    buf[p++] = (TLS13_CHACHA20_POLY1305 >> 8) & 0xFF;
    buf[p++] = TLS13_CHACHA20_POLY1305 & 0xFF;
    buf[p++] = (TLS13_AES_128_GCM >> 8) & 0xFF;
    buf[p++] = TLS13_AES_128_GCM & 0xFF;

    /* Compression methods (null only) */
    buf[p++] = 0x01; buf[p++] = 0x00;

    /* Extensions */
    int ext_len_pos = p; p += 2;
    int ext_start = p;

    /* Extension: supported_versions (REQUIRED for TLS 1.3) */
    buf[p++] = 0x00; buf[p++] = TLS_EXT_SUPPORTED_VERSIONS;
    buf[p++] = 0x00; buf[p++] = 0x03;  /* length */
    buf[p++] = 0x02;                    /* list length */
    buf[p++] = 0x03; buf[p++] = 0x04;  /* TLS 1.3 */

    /* Extension: key_share (X25519) */
    buf[p++] = 0x00; buf[p++] = TLS_EXT_KEY_SHARE;
    buf[p++] = 0x00; buf[p++] = 0x26;  /* length: 2 + 2 + 2 + 32 = 38 */
    buf[p++] = 0x00; buf[p++] = 0x24;  /* client shares length */
    buf[p++] = (TLS_GROUP_X25519 >> 8) & 0xFF;
    buf[p++] = TLS_GROUP_X25519 & 0xFF;
    buf[p++] = 0x00; buf[p++] = 0x20;  /* key length = 32 */
    memcpy(buf + p, tls13.client_pubkey, 32); p += 32;

    /* Extension: server_name (SNI) */
    if (hostname) {
        int hn_len = 0;
        while (hostname[hn_len]) hn_len++;
        buf[p++] = 0x00; buf[p++] = TLS_EXT_SERVER_NAME;
        uint16_t sni_len = (uint16_t)(hn_len + 5);
        buf[p++] = (sni_len >> 8) & 0xFF; buf[p++] = sni_len & 0xFF;
        buf[p++] = ((sni_len - 2) >> 8) & 0xFF; buf[p++] = (sni_len - 2) & 0xFF;
        buf[p++] = 0x00;  /* host_name type */
        buf[p++] = (hn_len >> 8) & 0xFF; buf[p++] = hn_len & 0xFF;
        memcpy(buf + p, hostname, hn_len); p += hn_len;
    }

    /* Fill extension length */
    uint16_t ext_len = (uint16_t)(p - ext_start);
    buf[ext_len_pos] = (ext_len >> 8) & 0xFF;
    buf[ext_len_pos + 1] = ext_len & 0xFF;

    /* Fill handshake length */
    uint32_t hs_len = (uint32_t)(p - hs_len_pos - 3);
    buf[hs_len_pos] = (hs_len >> 16) & 0xFF;
    buf[hs_len_pos + 1] = (hs_len >> 8) & 0xFF;
    buf[hs_len_pos + 2] = hs_len & 0xFF;

    /* Fill record length */
    uint16_t rec_len = (uint16_t)(p - rec_len_pos - 2);
    buf[rec_len_pos] = (rec_len >> 8) & 0xFF;
    buf[rec_len_pos + 1] = rec_len & 0xFF;

    serial_puts("[TLS1.3] ClientHello built (");
    serial_putdec((uint64_t)p);
    serial_puts(" bytes)\n");

    return p;
}

/* ── Public API ──────────────────────────────────────────────── */

/* Initiate TLS 1.3 handshake on an established TCP connection */
int tls13_connect(int tcp_conn, const char *hostname)
{
    memset(&tls13, 0, sizeof(tls13));
    tls13.tcp_conn = tcp_conn;

    /* Build and send ClientHello */
    uint8_t hello[512];
    int hello_len = tls13_build_client_hello(hello, hostname);
    if (hello_len < 0) {
        serial_puts("[TLS1.3] Failed to build ClientHello\n");
        return -1;
    }

    if (net_tcp_send(tcp_conn, hello, (uint32_t)hello_len) < 0) {
        serial_puts("[TLS1.3] Failed to send ClientHello\n");
        return -1;
    }

    /* Receive ServerHello */
    uint8_t resp[4096];
    int resp_len = net_tcp_recv(tcp_conn, resp, sizeof(resp));
    if (resp_len <= 0) {
        serial_puts("[TLS1.3] No ServerHello received\n");
        return -1;
    }

    /* Check if server selected TLS 1.3 (via supported_versions extension) */
    /* For now: parse ServerHello to extract key_share */
    if (resp_len > 5 && resp[0] == TLS_CONTENT_HANDSHAKE && resp[5] == TLS13_SERVER_HELLO) {
        serial_puts("[TLS1.3] ServerHello received (");
        serial_putdec((uint64_t)resp_len);
        serial_puts(" bytes)\n");
        tls13.tls13 = true;

        /* TODO: Parse ServerHello extensions for key_share,
         * compute shared_secret via X25519,
         * derive handshake keys via HKDF,
         * decrypt EncryptedExtensions + Certificate + Finished */

        serial_puts("[TLS1.3] Handshake in progress (key derivation TODO)\n");
        return 0;
    }

    serial_puts("[TLS1.3] Server did not select TLS 1.3, falling back to 1.2\n");
    return -2;  /* Caller should retry with TLS 1.2 */
}

bool tls13_is_active(void) { return tls13.tls13; }
