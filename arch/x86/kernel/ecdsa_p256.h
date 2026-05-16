/*
 * ecdsa_p256.h — minimal ECDSA-P256 verify for kernel TLS.
 *
 * Closes the second half of A12: cert pinning (commit 9aaaf84)
 * verifies that the certificate is the one we trust; this verifies
 * that the ServerKeyExchange signature actually comes from the
 * private key matching that cert. Together they form a complete
 * MITM defense for the TLS 1.2 ECDHE-ECDSA-AES128-GCM-SHA256
 * cipher suite we negotiate against Cloudflare.
 *
 * Surface: only verify. No signing, no key generation, no curve
 * negotiation. The only curve supported is P-256 (secp256r1) with
 * SHA-256 as the hash. That's exactly what CF presents and what
 * we already advertise in our ClientHello signature_algorithms list.
 *
 * Scope is bounded:
 *   - 256-bit bigints as uint64_t[4] little-endian
 *   - schoolbook multiply (asm mulq) + shift-subtract reduction
 *   - Fermat inversion via mod_pow
 *   - affine point arithmetic (double + add + scalar-multiply)
 *   - ASN.1 SEQUENCE { INTEGER r, INTEGER s } parser for the wire sig
 *
 * Performance is acceptable for once-per-handshake verification on
 * a 256-bit curve: ~0.1–0.5 s per verify in this naive style.
 *
 * No constant-time guarantees — verify keys are public, so timing
 * leaks of those don't compromise security. Don't reuse this code
 * for signing or for any private-key operation.
 */

#ifndef OSITOA_ECDSA_P256_H
#define OSITOA_ECDSA_P256_H

#include "../include/types.h"

/* Verify an ECDSA-P256 signature.
 *
 *   pub_x, pub_y :  the curve point that is the server's public key
 *                   (32 bytes each, big-endian, as carried in the
 *                   uncompressed point of the X.509 SubjectPublicKey)
 *   hash         :  the SHA-256 digest of the signed message
 *   sig_der      :  the DER-encoded ASN.1 signature
 *                   (SEQUENCE { INTEGER r, INTEGER s })
 *   sig_der_len  :  length of sig_der in bytes
 *
 * Returns 0 on a valid signature, -1 on any failure (parse error,
 * bad point, signature does not verify).
 */
int ecdsa_p256_verify(const uint8_t pub_x[32], const uint8_t pub_y[32],
                      const uint8_t hash[32],
                      const uint8_t *sig_der, uint32_t sig_der_len);

/* Run a self-test against a known-good NIST CAVS test vector.
 * Returns 0 on pass, -1 on any internal mismatch.  Called from
 * tls_init() so a broken bigint impl (e.g. compiler regression)
 * surfaces immediately at boot rather than at first handshake. */
int ecdsa_p256_self_test(void);

#endif
