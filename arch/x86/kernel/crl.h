/*
 * crl.h — Certificate Revocation List checker (RFC 5280 §5).
 *
 * Batch-cached alternative to OCSP.  Where OCSP issues one query
 * per cert (live, per-handshake), CRL downloads the whole list
 * of revoked serials issued by the CA from the URL in the cert's
 * cRLDistributionPoints (OID 2.5.29.31) extension, then does a
 * linear scan for our cert's serial.
 *
 * Trade-offs vs OCSP:
 *  + One request can cover many checks (cache it for the lease
 *    window between thisUpdate and nextUpdate).
 *  + Survives responder-down events (we have the last CRL on disk).
 *  - Bigger payload than OCSP (KBs to MBs vs ~hundreds of bytes).
 *  - Coarser freshness (CRLs typically refresh every 24h-7d).
 *
 * RFC 5280 §5.1.1 + §5.2.5 signature verification: when an issuer
 * cert is plumbed through `crl_check_revoked_with_issuer`, the CRL
 * parser captures the tbsCertList TLV, signatureAlgorithm OID, and
 * the signatureValue BIT STRING, then dispatches to the matching
 * primitive (RSA-PKCS1-v1.5-SHA256 or ECDSA-P256-SHA256) keyed by
 * the issuer's SPKI.  Verify result is logged ([CRL] sig verify: ...)
 * but informative-only — the CRL_GOOD/CRL_REVOKED status is still
 * returned; the caller decides policy.  The legacy 1-arg variant
 * (`crl_check_revoked`) skips verification with a SKIPPED log.
 */

#ifndef OSITOA_CRL_H
#define OSITOA_CRL_H

#include "../include/types.h"

typedef enum {
    CRL_GOOD     = 0,   /* serial not on the list — cert is OK */
    CRL_REVOKED  = 1,   /* serial found on the list */
    CRL_ERROR    = -1,  /* network/parse/no-CDP failure */
} crl_status_t;

/* Check whether `cert_der` (a single DER-encoded X.509 cert) is
 * revoked according to the CRL named in its cRLDistributionPoints
 * extension.
 *
 * Behavior:
 *   1. Pull the CRLDP URL from the cert's v3 extensions.
 *   2. Try to load `tls/crl-<hash8>.der` from osfs2 (hash8 = first
 *      8 hex chars of SHA-256 of the URL).  If present and the
 *      parser is happy, use that.
 *   3. Otherwise http_plain_get the URL, parse, persist the bytes
 *      to osfs2 for next boot.
 *   4. Walk the revokedCertificates list and compare serials.
 *
 * Returns CRL_GOOD / CRL_REVOKED / CRL_ERROR. */
int crl_check_revoked(const uint8_t *cert_der, uint32_t cert_len);

/* Same as crl_check_revoked, plus an issuer cert (DER) used to
 * verify the CRL signature over tbsCertList (RFC 5280 §5.1.1 +
 * §5.2.5).  The signature check is informative: pass/fail is
 * logged but does not change the return code.  If `issuer_der`
 * is NULL or `issuer_len == 0`, behaves like crl_check_revoked. */
int crl_check_revoked_with_issuer(const uint8_t *cert_der,   uint32_t cert_len,
                                  const uint8_t *issuer_der, uint32_t issuer_len);

/* Lower-level entrypoint: given the raw CRL DER and a target
 * serial (big-endian, as it appears in the cert's INTEGER body),
 * return CRL_GOOD if not found, CRL_REVOKED if found, CRL_ERROR
 * if the CRL is unparseable.  Exposed so tests / callers with
 * out-of-band CRL bytes (e.g. an admin-pushed list) can reuse
 * the parser without going through the URL fetch. */
int crl_parse_and_check(const uint8_t *crl, uint32_t crl_len,
                        const uint8_t *serial, uint32_t serial_len);

#endif
