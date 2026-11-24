/*
 * ecdh.c -- ECDHE on P-256 and P-384 for TLS 1.2, over BearSSL's
 * ec_prime_i31
 *
 * Servers whose TLS doesn't do X25519 (older OpenSSL builds, as
 * badssl.com's) take P-256. P-384 is there because a TLS 1.2 server
 * (OpenSSL) uses an ECDSA certificate only if the client lists its curve:
 * ecc384.badssl.com's is P-384, though the key exchange is on P-256.
 * Not hal.h, for the same reason as tls_verify.c: bearssl.h brings its own
 * string.h.
 */

#include "../lib/bearssl/inc/bearssl.h"
#include "ecdh.h"

void random_bytes(void *buf, uint32_t len);

/* BearSSL's curve ID for a TLS NamedGroup, or 0 */
static int curve_of(uint16_t group)
{
    return group == 0x0017 ? BR_EC_secp256r1 : group == 0x0018 ? BR_EC_secp384r1 : 0;
}

uint32_t ecdh_scalar_len(uint16_t group)
{
    return group == 0x0017 ? 32 : group == 0x0018 ? 48 : 0;
}

uint32_t ecdh_point_len(uint16_t group)
{
    return group == 0x0017 ? 65 : group == 0x0018 ? 97 : 0;
}

int ecdh_keygen(uint16_t group, uint8_t *priv, uint8_t *pub)
{
    int curve = curve_of(group);
    size_t olen, slen = ecdh_scalar_len(group);
    if (!curve)
        return -1;
    const unsigned char *n = br_ec_prime_i31.order(curve, &olen);
    if (olen != slen)
        return -1;
    for (int tries = 0; tries < 16; tries++) {
        random_bytes(priv, (uint32_t)slen);
        /* Keep it if 0 < priv < n (big-endian) */
        int below = 0, nonzero = 0;
        for (size_t i = 0; i < slen; i++) {
            nonzero |= priv[i];
            if (priv[i] != n[i]) {
                below = priv[i] < n[i];
                break;
            }
        }
        if (below && nonzero)
            return br_ec_prime_i31.mulgen(pub, priv, slen, curve) == ecdh_point_len(group)
                   ? 0 : -1;
    }
    return -1;
}

int ecdh_shared(uint16_t group, const uint8_t *priv, const uint8_t *peer,
                uint32_t len, uint8_t *secret)
{
    unsigned char g[ECDH_MAX_POINT];
    int curve = curve_of(group);
    size_t xlen;
    if (!curve || len != ecdh_point_len(group) || peer[0] != 0x04)
        return -1;
    memcpy(g, peer, len);
    if (!br_ec_prime_i31.mul(g, len, priv, ecdh_scalar_len(group), curve))
        return -1;                      /* not a point on the curve */
    size_t xoff = br_ec_prime_i31.xoff(curve, &xlen);
    memcpy(secret, g + xoff, xlen);
    return 0;
}
