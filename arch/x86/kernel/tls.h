/*
 * OsitoK x86-64 — TLS 1.2 Client
 *
 * Minimal TLS 1.2 implementation for HTTPS client, for servers without
 * TLS 1.3 (kernel/tls13.c). ECDHE (X25519 or P-256) with an RSA or ECDSA
 * certificate and AES-128-GCM-SHA256 (0xC02F, 0xC02B). The server is
 * authenticated (tls_verify.c): its chain, and its signature over the key
 * exchange. No client certificates, no session resumption.
 */

#ifndef OSITOK_TLS_H
#define OSITOK_TLS_H

#include "../include/types.h"
#include "crypto.h"
#include "tls_verify.h"

/* ── TLS Record Layer ────────────────────────────────────────── */

#define TLS_VER_10          0x0301
#define TLS_VER_12          0x0303

/* Content types */
#define TLS_CHANGE_CIPHER   20
#define TLS_ALERT           21
#define TLS_HANDSHAKE       22
#define TLS_APP_DATA        23

/* Handshake types */
#define TLS_HS_CLIENT_HELLO     1
#define TLS_HS_SERVER_HELLO     2
#define TLS_HS_CERTIFICATE      11
#define TLS_HS_SERVER_KEY_EXCH  12
#define TLS_HS_CERT_REQUEST     13
#define TLS_HS_SERVER_HELLO_DONE 14
#define TLS_HS_CLIENT_KEY_EXCH  16
#define TLS_HS_FINISHED         20

/* Alert levels */
#define TLS_ALERT_WARNING   1
#define TLS_ALERT_FATAL     2

/* States */
#define TLS_STATE_INIT              0
#define TLS_STATE_CLIENT_HELLO_SENT 1
#define TLS_STATE_SERVER_HELLO_DONE 2
#define TLS_STATE_KEYS_EXCHANGED   3
#define TLS_STATE_ESTABLISHED      4
#define TLS_STATE_ERROR            5
#define TLS_STATE_CLOSED           6

/* Max sizes */
#define TLS_MAX_RECORD      16384
#define TLS_MAX_HS_MSG      8192

/* ── TLS Connection State ────────────────────────────────────── */

typedef struct {
    int       tcp_conn;         /* Underlying TCP connection index */
    int       state;

    /* Handshake transcript (for Finished verify) */
    sha256_ctx hs_hash;         /* Running SHA-256 of all handshake messages */

    /* Random values */
    uint8_t   client_random[32];
    uint8_t   server_random[32];

    /* ECDHE: X25519's key; the secret, 32 bytes or P-384's 48 */
    uint8_t   ecdhe_privkey[32];
    uint8_t   ecdhe_pubkey[32];
    uint8_t   premaster_secret[48];
    uint32_t  premaster_len;

    /* Master secret + key material */
    uint8_t   master_secret[48];

    /* AES-128-GCM keys (derived from master secret) */
    uint8_t   client_write_key[16];
    uint8_t   server_write_key[16];
    uint8_t   client_write_iv[4];   /* Implicit IV (4 bytes) */
    uint8_t   server_write_iv[4];

    /* Sequence numbers for GCM nonce construction */
    uint64_t  client_seq;
    uint64_t  server_seq;

    /* Flags */
    bool      server_cipher_active;  /* Server CCS received, decrypt incoming */
    bool      client_cipher_active;  /* Client CCS sent, encrypt outgoing */

    /* Receive buffer for record reassembly */
    uint8_t   rx_buf[TLS_MAX_RECORD + 256];
    uint32_t  rx_len;

    /* Authenticated application bytes not yet consumed by tls_recv's caller.
     * TCP record reassembly and application reads have independent boundaries. */
    uint8_t   app_buf[TLS_MAX_RECORD];
    uint32_t  app_len;
    uint32_t  app_offset;

    /* The suite the server chose, and its certificate's verified key */
    uint16_t  suite;
    tls_pubkey_t leaf;

    /* The ECDHE group the server chose; our P-256 or P-384 key (ecdh.c) */
    uint16_t  group;
    uint8_t   ec_priv[48];
    uint8_t   ec_pub[97];

    /* Error info */
    uint8_t   alert_level;
    uint8_t   alert_desc;
} tls_conn_t;

/* ── TLS API ─────────────────────────────────────────────────── */

/* Initialize TLS connection over an existing TCP connection.
 * Performs full TLS 1.2 handshake (ClientHello → ... → Finished).
 * Returns 0 on success, -1 on failure. */
int tls_connect(tls_conn_t *tls, int tcp_conn, const char *hostname);

/* Send application data over TLS.
 * Returns bytes sent, or -1 on error. */
int tls_send(tls_conn_t *tls, const void *data, uint32_t len);

/* Receive application data from TLS.
 * Blocks until data arrives or timeout (in ticks).
 * Returns bytes read, 0 on timeout, -1 on error/close. */
int tls_recv(tls_conn_t *tls, void *buf, uint32_t buf_size,
             uint32_t timeout_ticks);

/* Close TLS connection (sends close_notify alert). */
void tls_close(tls_conn_t *tls);

#endif /* OSITOK_TLS_H */
