#ifndef OSITOK_OCSP_H
#define OSITOK_OCSP_H

#include "../include/types.h"

typedef enum {
    OCSP_GOOD = 0,
    OCSP_REVOKED = 1,
    OCSP_UNKNOWN = 2,
    OCSP_ERROR = -1,
} ocsp_status_t;

/* Query the certificate's AIA responder. Only an authenticated, matching,
 * fresh BasicOCSPResponse can return GOOD, REVOKED, or UNKNOWN. */
ocsp_status_t ocsp_check(const uint8_t *cert, uint32_t cert_len,
                         const uint8_t *issuer, uint32_t issuer_len);

/* Validate a raw response without network I/O. This is also the fixture-test
 * entry point. valid_until receives an absolute Unix timestamp. */
ocsp_status_t ocsp_validate_response(const uint8_t *response,
                                     uint32_t response_len,
                                     const uint8_t *cert, uint32_t cert_len,
                                     const uint8_t *issuer,
                                     uint32_t issuer_len,
                                     uint32_t now_utc,
                                     uint32_t *valid_until);

/* Validate and cache a TLS-stapled response. */
ocsp_status_t ocsp_parse_stapled(const uint8_t *response,
                                 uint32_t response_len,
                                 const uint8_t *cert, uint32_t cert_len,
                                 const uint8_t *issuer, uint32_t issuer_len);

#endif
