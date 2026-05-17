/*
 * ocsp.h — Online Certificate Status Protocol client (RFC 6960).
 *
 * Live revocation check: queries the OCSP responder named in a
 * cert's AIA extension and asks "is this cert good, revoked, or
 * unknown?".  Used to detect cert compromise without waiting for
 * a natural expiration.
 */

#ifndef OSITOA_OCSP_H
#define OSITOA_OCSP_H

#include "../include/types.h"

typedef enum {
    OCSP_GOOD     = 0,
    OCSP_REVOKED  = 1,
    OCSP_UNKNOWN  = 2,
    OCSP_ERROR    = -1,
} ocsp_status_t;

/* Query the OCSP responder for the status of `cert` (signed by
 * `issuer`).  Returns:
 *   OCSP_GOOD     — responder asserts the cert is not revoked
 *   OCSP_REVOKED  — responder asserts the cert is revoked
 *   OCSP_UNKNOWN  — responder doesn't know about this cert
 *   OCSP_ERROR    — network/parse failure
 *
 * The responder's BasicOCSPResponse signature IS verified per RFC
 * 6960 §4.2.2.2 (RSA-SHA256 / ECDSA-P256-SHA256).  Verification
 * uses the embedded delegated-signer cert if `certs [0]` is present
 * and its issuer DN matches the target cert's issuer DN (same-CA
 * constraint); otherwise it falls back to the issuer cert key.
 * Verify failures emit "[OCSP] sig verify: FAIL (<reason>)" to
 * serial but do not change the returned status — tls13.c decides
 * whether to enforce. */
ocsp_status_t ocsp_check(const uint8_t *cert,    uint32_t cert_len,
                         const uint8_t *issuer,  uint32_t issuer_len);

/* Parse + verify a STAPLED OCSP response (RFC 6066 §8 / RFC 8446
 * §4.4.2.1).  The server attaches the DER OCSPResponse inside the
 * Certificate handshake message — no outbound HTTP needed.  Returns:
 *   OCSP_GOOD     — responder asserts the cert is not revoked
 *   OCSP_REVOKED  — responder asserts the cert is revoked
 *   OCSP_UNKNOWN  — responder doesn't know about this cert
 *   OCSP_ERROR    — parse failure
 * Signature verification follows the same informative-mode policy as
 * ocsp_check: result logged but does not change the returned status. */
ocsp_status_t ocsp_parse_stapled(const uint8_t *resp,   uint32_t resp_len,
                                 const uint8_t *cert,   uint32_t cert_len,
                                 const uint8_t *issuer, uint32_t issuer_len);

#endif
