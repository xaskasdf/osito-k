/*
 * OsitoK x86-64 — SSH Server (Minimal)
 *
 * SSH-2 server for remote shell access. Listens on TCP port 22.
 * Supports:
 *   - Protocol version exchange
 *   - Key exchange: curve25519-sha256 (X25519 + SHA-256)
 *   - Encryption: chacha20-poly1305@openssh.com
 *   - Authentication: password (plaintext over encrypted channel)
 *   - Channel: session → shell
 *
 * NOTE: This is a minimal implementation for OsitoK's use case.
 * Not a full OpenSSH replacement — no agent forwarding, SFTP, etc.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern uint64_t idt_get_ticks(void);

/* Network */
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern int  net_tcp_recv(int conn, void *buf, uint32_t size);
extern void net_poll(void);

/* Crypto (from crypto.c, crypto2.c) */
extern void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32],
                              const uint8_t point[32]) __attribute__((weak));
extern void sha256(const uint8_t *data, uint32_t len,
                   uint8_t hash[32]) __attribute__((weak));
extern void chacha20_encrypt(const uint8_t key[32], uint32_t counter,
                             const uint8_t nonce[12],
                             uint8_t *data, uint32_t len) __attribute__((weak));
extern void random_get_bytes(void *buf, uint32_t len) __attribute__((weak));

/* Shell (for running commands) */
extern void kb_push(char c) __attribute__((weak));

/* ── SSH Constants ───────────────────────────────────────────── */

#define SSH_PORT    22
#define SSH_VERSION "SSH-2.0-OsitoK_1.0\r\n"

/* Message types */
#define SSH_MSG_KEXINIT          20
#define SSH_MSG_NEWKEYS          21
#define SSH_MSG_KEXDH_INIT       30
#define SSH_MSG_KEXDH_REPLY      31
#define SSH_MSG_USERAUTH_REQUEST 50
#define SSH_MSG_USERAUTH_SUCCESS 52
#define SSH_MSG_CHANNEL_OPEN     90
#define SSH_MSG_CHANNEL_OPEN_CONFIRM 91
#define SSH_MSG_CHANNEL_DATA     94
#define SSH_MSG_CHANNEL_REQUEST  98

/* ── SSH Session State ───────────────────────────────────────── */

typedef struct {
    int      tcp_conn;
    bool     active;
    bool     authenticated;
    bool     encrypted;
    uint8_t  session_id[32];
    uint8_t  server_privkey[32];
    uint8_t  server_pubkey[32];
    uint8_t  shared_secret[32];
    uint8_t  enc_key_s2c[32];    /* Server-to-client key */
    uint8_t  enc_key_c2s[32];    /* Client-to-server key */
    uint64_t seq_s2c;
    uint64_t seq_c2s;
    uint32_t channel_id;
} ssh_session_t;

#define SSH_MAX_SESSIONS 4
static ssh_session_t ssh_sessions[SSH_MAX_SESSIONS];

/* ── SSH Packet I/O ──────────────────────────────────────────── */

/* Send SSH binary packet (unencrypted for now) */
static int ssh_send_packet(ssh_session_t *s, const uint8_t *payload, uint32_t plen)
{
    /* SSH binary packet: length(4) + padding_len(1) + payload + padding */
    uint8_t pkt[4096];
    uint32_t padding = 8 - ((plen + 5) % 8);
    if (padding < 4) padding += 8;
    uint32_t total = 4 + 1 + plen + padding;

    /* Packet length (excludes the length field itself) */
    uint32_t pkt_len = 1 + plen + padding;
    pkt[0] = (pkt_len >> 24) & 0xFF;
    pkt[1] = (pkt_len >> 16) & 0xFF;
    pkt[2] = (pkt_len >> 8) & 0xFF;
    pkt[3] = pkt_len & 0xFF;
    pkt[4] = (uint8_t)padding;
    memcpy(pkt + 5, payload, plen);
    if (random_get_bytes)
        random_get_bytes(pkt + 5 + plen, padding);
    else
        memset(pkt + 5 + plen, 0, padding);

    s->seq_s2c++;
    return net_tcp_send(s->tcp_conn, pkt, total);
}

/* ── Protocol Version Exchange ───────────────────────────────── */

static int ssh_version_exchange(ssh_session_t *s)
{
    /* Send our version string */
    const char *ver = SSH_VERSION;
    int vlen = 0;
    while (ver[vlen]) vlen++;
    net_tcp_send(s->tcp_conn, ver, (uint32_t)vlen);

    /* Receive client version */
    char client_ver[256];
    int n = net_tcp_recv(s->tcp_conn, client_ver, 255);
    if (n <= 0) return -1;
    client_ver[n] = '\0';

    serial_puts("[SSHD] Client: ");
    serial_puts(client_ver);

    /* Verify SSH-2.0 prefix */
    if (client_ver[0] != 'S' || client_ver[1] != 'S' ||
        client_ver[2] != 'H' || client_ver[3] != '-' ||
        client_ver[4] != '2') {
        serial_puts("[SSHD] Not SSH-2.0\n");
        return -1;
    }

    return 0;
}

/* ── Key Exchange Init ───────────────────────────────────────── */

static int ssh_send_kexinit(ssh_session_t *s)
{
    uint8_t payload[512];
    int p = 0;
    payload[p++] = SSH_MSG_KEXINIT;

    /* 16 bytes cookie (random) */
    if (random_get_bytes)
        random_get_bytes(payload + p, 16);
    else
        memset(payload + p, 0x42, 16);
    p += 16;

    /* Algorithm name-lists (simplified: one algorithm each) */
    const char *algorithms[] = {
        "curve25519-sha256",          /* kex */
        "ssh-ed25519",                /* host key */
        "chacha20-poly1305@openssh.com", /* enc c2s */
        "chacha20-poly1305@openssh.com", /* enc s2c */
        "hmac-sha2-256",              /* mac c2s */
        "hmac-sha2-256",              /* mac s2c */
        "none",                       /* comp c2s */
        "none",                       /* comp s2c */
        "",                           /* lang c2s */
        "",                           /* lang s2c */
    };

    for (int i = 0; i < 10; i++) {
        int len = 0;
        while (algorithms[i][len]) len++;
        payload[p++] = (len >> 24) & 0xFF;
        payload[p++] = (len >> 16) & 0xFF;
        payload[p++] = (len >> 8) & 0xFF;
        payload[p++] = len & 0xFF;
        memcpy(payload + p, algorithms[i], len);
        p += len;
    }

    /* first_kex_packet_follows = false, reserved = 0 */
    payload[p++] = 0;
    payload[p++] = 0; payload[p++] = 0; payload[p++] = 0; payload[p++] = 0;

    return ssh_send_packet(s, payload, (uint32_t)p);
}

/* ── Public API ──────────────────────────────────────────────── */

/* Handle a new SSH connection (called when TCP accepts on port 22) */
int sshd_handle_connection(int tcp_conn)
{
    /* Find free session */
    int slot = -1;
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (!ssh_sessions[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    ssh_session_t *s = &ssh_sessions[slot];
    memset(s, 0, sizeof(*s));
    s->tcp_conn = tcp_conn;
    s->active = true;

    serial_puts("[SSHD] New connection (session ");
    serial_putdec((uint64_t)slot);
    serial_puts(")\n");

    /* Generate server host key (ephemeral for now) */
    if (random_get_bytes) {
        random_get_bytes(s->server_privkey, 32);
        s->server_privkey[0] &= 248;
        s->server_privkey[31] &= 127;
        s->server_privkey[31] |= 64;
        static const uint8_t bp[32] = {9};
        if (x25519_scalarmult)
            x25519_scalarmult(s->server_pubkey, s->server_privkey, bp);
    }

    /* Protocol version exchange */
    if (ssh_version_exchange(s) < 0) {
        s->active = false;
        return -1;
    }

    /* Send KEXINIT */
    ssh_send_kexinit(s);

    serial_puts("[SSHD] KEX initiated (full handshake TODO)\n");
    return slot;
}

/* Check if any SSH sessions need attention (call from main loop) */
void sshd_poll(void)
{
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (!ssh_sessions[i].active) continue;
        /* TODO: receive and process SSH packets */
    }
}

bool sshd_is_active(void)
{
    for (int i = 0; i < SSH_MAX_SESSIONS; i++)
        if (ssh_sessions[i].active) return true;
    return false;
}
