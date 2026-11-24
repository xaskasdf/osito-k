/*
 * hkdf.h -- HKDF-SHA256 and TLS 1.3's labels (kernel/hkdf.c)
 */
#ifndef OSITO_ARM_HKDF_H
#define OSITO_ARM_HKDF_H

#include "../include/types.h"

#define HKDF_INFO_MAX 520       /* 2 + 1 + 255 + 1 + 255, rounded */

void hkdf_extract(const uint8_t *salt, uint32_t salt_len,
                  const uint8_t *ikm, uint32_t ikm_len, uint8_t prk[32]);
void hkdf_expand(const uint8_t prk[32], const uint8_t *info, uint32_t info_len,
                 uint8_t *okm, uint32_t okm_len);
void hkdf_expand_label(const uint8_t prk[32], const char *label,
                       const uint8_t *context, uint32_t context_len,
                       uint8_t *okm, uint32_t okm_len);
void hkdf_derive_secret(const uint8_t prk[32], const char *label,
                        const uint8_t transcript_hash[32], uint8_t out[32]);

#endif
