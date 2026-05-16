/*
 * ecdsa_p384.h — ECDSA P-384 (secp384r1) signature verification.
 *
 * Verify-only.  Used by chain link validation when the issuer cert
 * exposes a P-384 EC pubkey (e.g. GTS Root R4 → its intermediates).
 *
 * Sister module to ecdsa_p256.c — same shape, different curve
 * parameters, 6×uint64 limbs instead of 4.
 */

#ifndef OSITOA_ECDSA_P384_H
#define OSITOA_ECDSA_P384_H

#include "../include/types.h"

/* Verify an ECDSA P-384 signature.  `sig_der` is a DER-encoded
 * SEQUENCE { r, s }.  Returns 0 on success, -1 on any failure. */
int ecdsa_p384_verify(const uint8_t pub_x[48], const uint8_t pub_y[48],
                      const uint8_t hash[48],
                      const uint8_t *sig_der, uint32_t sig_der_len);

/* Boot-time self-test against an offline-generated NIST P-384 +
 * SHA-384("abc") vector.  Returns 0 on PASS, -1 on FAIL. */
int ecdsa_p384_self_test(void);

#endif
