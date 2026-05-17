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

/* Extract the EC P-384 (secp384r1) public key from a single DER-
 * encoded cert.  pub_x and pub_y receive 48 big-endian bytes.  Used
 * to verify the WE1 → GTS Root R4 chain link (root cert is P-384). */
int x509_extract_ec_pubkey_p384(const uint8_t *cert, uint32_t cert_len,
                                uint8_t pub_x[48], uint8_t pub_y[48]);

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

/* ── SAN / hostname matching (A12.8) ───────────────────────────
 *
 * Walk the Subject Alternative Name extension (OID 2.5.29.17) and
 * test each dNSName against `hostname`.  Wildcards (`*.example.com`)
 * match exactly one label per RFC 6125 §6.4.3 — `foo.example.com`
 * matches but `a.b.example.com` and `example.com` do not.
 * Case-insensitive ASCII matching.
 *
 * Returns:
 *    0   at least one dNSName matched
 *   -1   no match, SAN missing/malformed, or hostname empty
 *
 * Falls back to comparing against the Subject CN ONLY if no SAN
 * extension is present (RFC 6125 §6.4.4 — CN-matching is deprecated
 * but still encountered in self-signed certs and old internal CAs). */
int x509_match_hostname(const uint8_t *cert, uint32_t cert_len,
                        const char    *hostname);

/* ── X.509 v3 extension parsing (A12.10) ──────────────────────
 *
 * Two security-critical extensions enforced per RFC 5280 §4.2.1.9
 * (BasicConstraints) and §4.2.1.3 (KeyUsage).  Used during chain
 * walking to ensure intermediates actually have CA authority and
 * leaves don't masquerade as CAs.
 *
 * Without these checks, an attacker holding ANY valid leaf cert
 * could potentially be accepted as an intermediate by our chain
 * walker — minted sub-certs would chain through their stolen
 * leaf instead of failing closed.  This is the classic
 * "X.509 confusion" vulnerability.
 */

/* Bit flags for KeyUsage (RFC 5280 §4.2.1.3 — order matches the
 * BIT STRING bit ordering used in the extension). */
#define X509_KU_DIGITAL_SIGNATURE  0x0001
#define X509_KU_NON_REPUDIATION    0x0002
#define X509_KU_KEY_ENCIPHERMENT   0x0004
#define X509_KU_DATA_ENCIPHERMENT  0x0008
#define X509_KU_KEY_AGREEMENT      0x0010
#define X509_KU_KEY_CERT_SIGN      0x0020
#define X509_KU_CRL_SIGN           0x0040
#define X509_KU_ENCIPHER_ONLY      0x0080
#define X509_KU_DECIPHER_ONLY      0x0100

typedef struct {
    bool     has_bc;          /* BasicConstraints present */
    bool     is_ca;           /* BasicConstraints.cA = TRUE */
    int      path_len;        /* BasicConstraints.pathLenConstraint, -1 = unset */
    bool     has_ku;          /* KeyUsage present */
    uint32_t key_usage_flags; /* X509_KU_* bitmask */
} x509_v3_t;

/* Parse the v3 extension fields we care about (BasicConstraints +
 * KeyUsage) from a single DER cert.  Other extensions are silently
 * skipped.  Returns 0 always — caller checks the populated fields
 * to decide what to enforce (CA validation, signature authority,
 * etc.).  If the cert has no v3 extensions block, all fields stay
 * at their zero defaults. */
int x509_parse_v3(const uint8_t *cert, uint32_t cert_len, x509_v3_t *out);

/* Walk-time chain check: assert that every intermediate in the
 * chain has BasicConstraints.cA = TRUE AND KeyUsage.keyCertSign
 * (when KeyUsage is present).  `chain[0]` is the leaf; chain
 * intermediates are `chain[1..count-1]`.  Returns 0 if all
 * intermediates pass, -1 on first violation.  Logs the failing
 * cert index and reason. */
int x509_check_chain_constraints(const uint8_t **chain,
                                 const uint32_t *chain_lens,
                                 uint32_t        count);

/* ── Authority Information Access (A12.11) ─────────────────────
 *
 * RFC 5280 §4.2.2.1.  Two URLs of interest typically appear in
 * the AIA extension on a leaf cert:
 *
 *   id-ad-caIssuers (1.3.6.1.5.5.7.48.2): URL of the *parent* cert,
 *     used to fetch the intermediate when a server doesn't send
 *     the full chain.  AIA chasing reads this.
 *   id-ad-ocsp      (1.3.6.1.5.5.7.48.1): URL of the OCSP
 *     responder for this cert.  OCSP queries POST to this URL.
 *
 * Both functions return the URL as a NUL-terminated string in
 * `out`, capped at `cap` bytes (truncated with no NUL if too small,
 * which the caller detects via the returned length).  Returns the
 * length of the URL (excluding NUL) on success, -1 if the cert has
 * no AIA extension or the requested OID isn't present. */
int x509_get_aia_caissuers(const uint8_t *cert, uint32_t cert_len,
                          char *out, uint32_t cap);
int x509_get_aia_ocsp     (const uint8_t *cert, uint32_t cert_len,
                          char *out, uint32_t cap);

/* ── CRL Distribution Points (RFC 5280 §4.2.1.13) ───────────────
 *
 * Extension OID 2.5.29.31.  Each cert may list one or more URLs
 * where its CRL can be fetched.  We only care about the first
 * URI distributionPoint encountered — multi-URL CDP rotation is
 * a higher-tier concern (handled by retrying with the next URL
 * if the first download fails).
 *
 * The DER shape per RFC 5280:
 *   CRLDistributionPoints ::= SEQUENCE OF DistributionPoint
 *   DistributionPoint ::= SEQUENCE {
 *     distributionPoint [0] DistributionPointName OPTIONAL,
 *     reasons           [1] ReasonFlags OPTIONAL,
 *     cRLIssuer         [2] GeneralNames OPTIONAL }
 *   DistributionPointName ::= CHOICE {
 *     fullName              [0] GeneralNames,
 *     nameRelativeToCRLIssuer [1] RelativeDistinguishedName }
 *   GeneralName for URI ::= [6] IMPLICIT IA5String
 *
 * Returns URL length on success, -1 if the extension is missing
 * or no URI fullName entry exists. */
int x509_get_crldp_url(const uint8_t *cert, uint32_t cert_len,
                       char *out, uint32_t cap);

/* ── OCSP request fields (A12.11) ──────────────────────────────
 *
 * Helpers used by the OCSP module to build a CertID.  Each
 * returns the *raw DER bytes* of the requested field within the
 * cert (pointers into the input buffer; no allocation).  Returns
 * 0 with `out_ptr`/`out_len` populated on success, -1 on parse
 * failure. */
int x509_get_issuer_der(const uint8_t *cert, uint32_t cert_len,
                        const uint8_t **out_ptr, uint32_t *out_len);
int x509_get_subject_pubkey_bits(const uint8_t *cert, uint32_t cert_len,
                                 const uint8_t **out_ptr, uint32_t *out_len);
int x509_get_serial_number(const uint8_t *cert, uint32_t cert_len,
                           const uint8_t **out_ptr, uint32_t *out_len);

#endif
