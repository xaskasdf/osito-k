/*
 * x509.h — narrow X.509 leaf-cert pubkey extraction.
 *
 * Pairs with cert_pin.c (already trusts the leaf via SHA-256 pin)
 * and ecdsa_p256.c (verifies signatures with the extracted key).
 * Together: A12 ECDSA-side closure for TLS 1.2 ECDHE-ECDSA on
 * secp256r1 leaf certs.
 *
 * Caller MUST have already pin-checked the cert before relying
 * on the extracted key material.
 */

#ifndef OSITOA_X509_H
#define OSITOA_X509_H

#include "../include/types.h"

/* Extract the EC P-256 public key from a single DER-encoded leaf
 * cert. pub_x and pub_y receive the 32-byte big-endian coordinates.
 * Returns 0 on success, -1 if the cert is malformed or the SPKI is
 * not an uncompressed P-256 point. */
int x509_extract_ec_pubkey(const uint8_t *cert, uint32_t cert_len,
                           uint8_t pub_x[32], uint8_t pub_y[32]);

/* Convenience: parse a TLS Certificate handshake message body
 * (uint24 list_len + per-cert {uint24 len + DER}) and extract from
 * the LEAF (first) cert. */
int x509_extract_ec_pubkey_from_msg(const uint8_t *cert_msg, uint32_t msg_len,
                                     uint8_t pub_x[32], uint8_t pub_y[32]);

/* ── Chain link validation (A12.5) ─────────────────────────────
 *
 * Verify that `child_cert` was signed by the entity owning
 * `issuer_cert` — i.e. that they form a valid step in an X.509
 * chain.  Supports both signature algorithms used by the chains
 * we care about (CF / GTS):
 *
 *   - sha256WithRSAEncryption (1.2.840.113549.1.1.11)  — RSA-2048
 *   - ecdsa-with-SHA256       (1.2.840.10045.4.3.2)    — P-256
 *
 * For RSA the issuer must expose an RSA pubkey in its SPKI; for
 * ECDSA the issuer must expose a P-256 EC pubkey.  Wrong-curve
 * or wrong-algorithm mismatches return -1.
 *
 * Returns:
 *    0  signature verified, chain link OK
 *   -1  parse error, alg unsupported, or signature mismatch
 *
 * Caller responsibility: that the issuer cert is itself trusted
 * (root pin / dynamic pin / static intermediate pin).  This
 * function only verifies the *cryptographic link*; trust anchoring
 * is the pin table's job. */
int x509_verify_chain_link(const uint8_t *child_cert,  uint32_t child_len,
                           const uint8_t *issuer_cert, uint32_t issuer_len);

/* ── Validity-window check (A12.6) ─────────────────────────────
 *
 * Parse the cert's notBefore/notAfter fields and check that `now`
 * (UTC seconds since the Unix epoch) lies in the window.  Both
 * UTCTime (`YYMMDDHHMMSSZ`, tag 0x17) and GeneralizedTime
 * (`YYYYMMDDHHMMSSZ`, tag 0x18) accepted per RFC 5280 §4.1.2.5.
 *
 * Returns:
 *    0   in-window  OR  caller didn't supply a real clock (now == 0)
 *   -1   expired, not-yet-valid, or unparseable
 *
 * The now==0 escape hatch lets us keep boot working when NTP hasn't
 * synced yet — the cert pin is still authoritative, and we'd rather
 * accept a cert in a pre-NTP boot than fail closed on an unbacked
 * clock reading. */
int x509_check_validity(const uint8_t *cert, uint32_t cert_len, uint32_t now_utc);

#endif
