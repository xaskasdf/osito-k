/*
 * ecdh_p256.h -- ECDHE on P-256 (kernel/ecdh_p256.c)
 */
#ifndef OSITO_ARM_ECDH_P256_H
#define OSITO_ARM_ECDH_P256_H

#include <stdint.h>

/* A private scalar in [1, n-1] and its uncompressed public point: 0 or -1 */
int ecdh_p256_keygen(uint8_t priv[32], uint8_t pub[65]);
/* The shared X coordinate with the peer's uncompressed point: 0, or -1 if
 * it isn't a point on the curve */
int ecdh_p256_shared(const uint8_t priv[32], const uint8_t *peer, uint32_t len,
                     uint8_t secret[32]);

#endif
