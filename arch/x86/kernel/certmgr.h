#ifndef OSITOK_CERTMGR_H
#define OSITOK_CERTMGR_H

#include "../include/types.h"

/* Root certificates are data, not policy compiled into the kernel. The
 * manager loads tls/ca-roots.bin from the mounted VFS and indexes it once. */
int certmgr_load_system_roots(void);
int certmgr_root_count(void);

/* Exact trust-anchor lookup. A match means the complete DER certificate is
 * present in the system root store; SPKI-only matches are not sufficient. */
int certmgr_is_trust_anchor(const uint8_t *cert, uint32_t cert_len);

/* Check both exact root membership and the host trust policy attached to it.
 * `usage_oids` contains ASCII dotted OIDs from CERT_CHAIN_PARA. When no usage
 * is requested, every configured anchor is allowed. `match_all` selects AND
 * rather than OR semantics. The return value reports anchor membership; the
 * optional output distinguishes a valid anchor with a usage-policy mismatch. */
int certmgr_check_trust_anchor(const uint8_t *cert, uint32_t cert_len,
                               const char *const *usage_oids,
                               uint32_t usage_count, int match_all,
                               int *usage_allowed);

typedef struct {
    const uint8_t *issuer_name;
    uint32_t issuer_name_len;
    const uint8_t *authority_key_id;
    uint32_t authority_key_id_len;
    int16_t next_index;
    uint8_t phase;
} certmgr_issuer_iter_t;

/* Iterate configured roots that can issue `child`. AKI/SKI is preferred and
 * encoded issuer/subject Name is always checked. Each returned certificate
 * still requires cryptographic signature verification by the caller. */
int certmgr_issuer_iter_begin(certmgr_issuer_iter_t *iter,
                              const uint8_t *child, uint32_t child_len);
int certmgr_issuer_iter_next(certmgr_issuer_iter_t *iter,
                             const uint8_t **cert, uint32_t *cert_len);

typedef enum {
    CERTMGR_REVOCATION_UNAVAILABLE = -1,
    CERTMGR_REVOCATION_GOOD = 0,
    CERTMGR_REVOCATION_REVOKED = 1,
    CERTMGR_REVOCATION_UNKNOWN = 2,
} certmgr_revocation_status_t;

/* Authenticated OCSP and CRL clients publish short-lived results here. */
int certmgr_revocation_lookup(const uint8_t *cert, uint32_t cert_len,
                              uint32_t validation_time,
                              uint32_t *valid_until);
void certmgr_revocation_store(const uint8_t *cert, uint32_t cert_len,
                              certmgr_revocation_status_t status,
                              uint32_t valid_until);

/* Resolve from the authenticated cache and enqueue OCSP/CRL work on a miss.
 * The caller never waits for network I/O. Concurrent requests are coalesced
 * and failed responders receive a short retry backoff. */
int certmgr_revocation_resolve(const uint8_t *cert, uint32_t cert_len,
                               const uint8_t *issuer, uint32_t issuer_len,
                               uint32_t validation_time, int allow_network,
                               int *network_attempted);

#endif
