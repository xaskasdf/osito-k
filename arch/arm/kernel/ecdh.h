/*
 * ecdh.h -- ECDHE on the NIST curves P-256 and P-384 (kernel/ecdh.c)
 */
#ifndef OSITO_ARM_ECDH_H
#define OSITO_ARM_ECDH_H

#include <stdint.h>

#define ECDH_MAX_SCALAR 48              /* P-384's private key and secret */
#define ECDH_MAX_POINT  97              /* P-384's uncompressed point */

/* A TLS NamedGroup's sizes: 0x0017 (secp256r1) 32 and 65, 0x0018
 * (secp384r1) 48 and 97; 0 for any other group */
uint32_t ecdh_scalar_len(uint16_t group);
uint32_t ecdh_point_len(uint16_t group);

/* A private scalar in [1, n-1] and its uncompressed public point: 0 or -1 */
int ecdh_keygen(uint16_t group, uint8_t *priv, uint8_t *pub);
/* The shared X coordinate (ecdh_scalar_len bytes) with the peer's
 * uncompressed point: 0, or -1 if it isn't a point on the curve */
int ecdh_shared(uint16_t group, const uint8_t *priv, const uint8_t *peer,
                uint32_t len, uint8_t *secret);

#endif
