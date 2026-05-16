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
 * Currently does NOT cryptographically verify the responder's
 * signature on the BasicOCSPResponse — that requires either the
 * issuer key (if delegated) or a OCSP signing cert in the response
 * (which itself needs chain validation).  This is informative-mode;
 * a "revoked" status is logged but not enforced.  Sig verify is
 * tracked as a follow-up. */
ocsp_status_t ocsp_check(const uint8_t *cert,    uint32_t cert_len,
                         const uint8_t *issuer,  uint32_t issuer_len);

#endif
