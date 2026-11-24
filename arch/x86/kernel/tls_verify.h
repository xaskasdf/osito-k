/*
 * tls_verify.h -- checking the server's certificates and signature (BearSSL)
 */
#ifndef OSITO_X86_TLS_VERIFY_H
#define OSITO_X86_TLS_VERIFY_H

#include <stdint.h>

/* The leaf certificate's key, copied out of the X.509 engine */
typedef struct {
    uint8_t  type;              /* 1 RSA, 2 EC (BearSSL's BR_KEYTYPE_*) */
    uint8_t  n[512], e[8];      /* RSA, up to 4096 bits */
    uint32_t nlen, elen;
    int      curve;             /* EC: 23 P-256, 24 P-384, 25 P-521 */
    uint8_t  q[133];
    uint32_t qlen;
} tls_pubkey_t;

/* The chain the server sent, leaf first, against Mozilla's roots
 * (kernel/ca_roots.c): every signature (SHA-256 or better), CA flags and
 * path lengths, the dates against NTP's time, and `host` against the
 * leaf's names. 0 with the leaf's key in *leaf, or -1 with the reason
 * printed. */
int tls_verify_chain(const uint8_t *const *certs, const uint32_t *lens, int n,
                     const char *host, tls_pubkey_t *leaf);

/* A TLS SignatureScheme (RFC 8446 4.2.3) over msg by `key`: ECDSA
 * (0x0403, 0x0503, 0x0603), RSA-PSS (0x0804-0x0806) and, for TLS 1.2,
 * RSA PKCS#1 v1.5 (0x0401, 0x0501, 0x0601). 0 if it checks out. */
int tls_verify_signature(const tls_pubkey_t *key, uint16_t scheme,
                         const uint8_t *msg, uint32_t len,
                         const uint8_t *sig, uint32_t siglen);

/* For the ClientHello: the schemes tls_verify_signature takes, big-endian
 * pairs. tls13: the TLS 1.3 ones only (no PKCS#1 v1.5). */
uint32_t tls_signature_schemes(uint8_t *out, int tls13);
const char *tls_scheme_name(uint16_t scheme);     /* for the log */

#endif
