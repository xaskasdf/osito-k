/*
 * hkdf.c -- HKDF-SHA256 and TLS 1.3's HKDF-Expand-Label (RFC 5869, RFC 8446
 * 7.1), for tls13.c's key schedule. x86's crypto2.c, ported.
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "crypto.h"
#include "hkdf.h"

/* PRK = HMAC-Hash(salt, IKM) */
void hkdf_extract(const uint8_t *salt, uint32_t salt_len,
                  const uint8_t *ikm, uint32_t ikm_len, uint8_t prk[32])
{
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

/* OKM = T(1) || T(2) || ..., T(i) = HMAC-Hash(PRK, T(i-1) || info || i) */
void hkdf_expand(const uint8_t prk[32], const uint8_t *info, uint32_t info_len,
                 uint8_t *okm, uint32_t okm_len)
{
    uint8_t input[32 + HKDF_INFO_MAX + 1];
    uint8_t t[32];
    uint32_t t_len = 0, pos = 0;
    uint8_t counter = 1;
    if (info_len > HKDF_INFO_MAX)
        info_len = HKDF_INFO_MAX;
    while (pos < okm_len) {
        memcpy(input, t, t_len);
        memcpy(input + t_len, info, info_len);
        input[t_len + info_len] = counter++;
        hmac_sha256(prk, 32, input, t_len + info_len + 1, t);
        t_len = 32;
        uint32_t copy = okm_len - pos < 32 ? okm_len - pos : 32;
        memcpy(okm + pos, t, copy);
        pos += copy;
    }
}

/* info = uint16 length || uint8-prefixed "tls13 " label || uint8-prefixed
 * context */
void hkdf_expand_label(const uint8_t prk[32], const char *label,
                       const uint8_t *context, uint32_t context_len,
                       uint8_t *okm, uint32_t okm_len)
{
    uint8_t info[HKDF_INFO_MAX];
    uint32_t io = 0;
    uint32_t label_len = (uint32_t)strlen(label);
    if (label_len > 249)
        label_len = 249;
    if (context_len > 255)
        context_len = 255;
    info[io++] = (uint8_t)(okm_len >> 8);
    info[io++] = (uint8_t)okm_len;
    info[io++] = (uint8_t)(6 + label_len);
    memcpy(info + io, "tls13 ", 6);
    io += 6;
    memcpy(info + io, label, label_len);
    io += label_len;
    info[io++] = (uint8_t)context_len;
    memcpy(info + io, context, context_len);
    io += context_len;
    hkdf_expand(prk, info, io, okm, okm_len);
}

/* Derive-Secret(Secret, Label, Messages), the messages' SHA-256 given */
void hkdf_derive_secret(const uint8_t prk[32], const char *label,
                        const uint8_t transcript_hash[32], uint8_t out[32])
{
    hkdf_expand_label(prk, label, transcript_hash, 32, out, 32);
}
