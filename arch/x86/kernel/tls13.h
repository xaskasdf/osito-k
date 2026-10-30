/*
 * tls13.h -- the TLS 1.3 client (kernel/tls13.c)
 */
#ifndef OSITO_X86_TLS13_H
#define OSITO_X86_TLS13_H

#include "../include/types.h"

#define TLS13_AUTH_FAILED   (-3)    /* the server failed authentication */

/* The handshake over an open TCP connection: 0, TLS13_AUTH_FAILED (don't
 * retry with TLS 1.2: the server isn't who it claims), or -1 (it may not
 * speak TLS 1.3) */
int  tls13_connect(int tcp_conn, const char *hostname);
int  tls13_send(const void *data, uint32_t len);
int  tls13_recv(void *buf, uint32_t cap);   /* 0: closed by the server */
void tls13_close(void);
int  tls13_is_active(void);

/* Revocation of a verified chain (leaf, issuer, ...): -1 if OCSP or the CRL
 * says the leaf is revoked, else 0. stapled may be 0. */
int  tls_check_revocation(const uint8_t *const *certs, const uint32_t *lens,
                          const uint8_t *stapled, uint32_t stapled_len);

#endif
