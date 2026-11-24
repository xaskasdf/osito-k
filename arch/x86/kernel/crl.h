#ifndef OSITOK_CRL_H
#define OSITOK_CRL_H

#include "../include/types.h"

typedef enum {
    CRL_GOOD = 0,
    CRL_REVOKED = 1,
    CRL_ERROR = -1,
} crl_status_t;

/* An issuer is mandatory: an unsigned or unverifiable CRL never produces a
 * revocation decision. */
int crl_check_revoked_with_issuer(const uint8_t *cert, uint32_t cert_len,
                                  const uint8_t *issuer,
                                  uint32_t issuer_len);
int crl_check_revoked(const uint8_t *cert, uint32_t cert_len);

/* Pure parser retained for fuzzing and malformed-DER tests. It does not make
 * a trust decision; production callers use crl_validate_and_check. */
int crl_parse_and_check(const uint8_t *crl, uint32_t crl_len,
                        const uint8_t *serial, uint32_t serial_len);

int crl_validate_and_check(const uint8_t *crl, uint32_t crl_len,
                           const uint8_t *cert, uint32_t cert_len,
                           const uint8_t *issuer, uint32_t issuer_len,
                           uint32_t now_utc, uint32_t *valid_until);

#endif
