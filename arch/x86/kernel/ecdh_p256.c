/*
 * ecdh_p256.c -- ECDHE on P-256 for TLS 1.2, over BearSSL's ec_prime_i31
 *
 * Servers whose TLS doesn't do X25519 (older OpenSSL builds, as
 * badssl.com's) take P-256. The same as arch/arm/kernel's.
 */

#include "../lib/bearssl/inc/bearssl.h"
#include "ecdh_p256.h"

extern void random_bytes(void *buf, uint32_t len);     /* random_drbg.c */

int ecdh_p256_keygen(uint8_t priv[32], uint8_t pub[65])
{
    size_t olen;
    const unsigned char *n = br_ec_prime_i31.order(BR_EC_secp256r1, &olen);
    if (olen != 32)
        return -1;
    for (int tries = 0; tries < 16; tries++) {
        random_bytes(priv, 32);
        /* Keep it if 0 < priv < n (big-endian) */
        int below = 0, nonzero = 0;
        for (int i = 0; i < 32; i++) {
            nonzero |= priv[i];
            if (priv[i] != n[i]) {
                below = priv[i] < n[i];
                break;
            }
        }
        if (below && nonzero)
            return br_ec_prime_i31.mulgen(pub, priv, 32, BR_EC_secp256r1) == 65 ? 0 : -1;
    }
    return -1;
}

int ecdh_p256_shared(const uint8_t priv[32], const uint8_t *peer, uint32_t len,
                     uint8_t secret[32])
{
    unsigned char g[65];
    size_t xlen;
    if (len != 65 || peer[0] != 0x04)
        return -1;
    memcpy(g, peer, 65);
    if (!br_ec_prime_i31.mul(g, 65, priv, 32, BR_EC_secp256r1))
        return -1;                      /* not a point on the curve */
    size_t xoff = br_ec_prime_i31.xoff(BR_EC_secp256r1, &xlen);
    memcpy(secret, g + xoff, 32);
    return 0;
}
