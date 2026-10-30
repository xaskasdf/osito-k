/*
 * tls13.h -- the TLS 1.3 client (kernel/tls13.c)
 */
#ifndef OSITO_ARM_TLS13_H
#define OSITO_ARM_TLS13_H

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

#endif
