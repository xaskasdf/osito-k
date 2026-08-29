/* Authenticated RFC 5280 certificate revocation list client. */

#include "../include/types.h"
#include "../fs/ositofs3.h"
#include "../fs/vfs.h"
#include "certmgr.h"
#include "crl.h"
#include "x509.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t value);
extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);
extern void sha256(const void *data, uint32_t len, uint8_t digest[32]);
extern uint32_t ntp_get_utc(void);
extern int http_plain_get(const char *url, uint8_t *out, uint32_t out_cap);
extern bool osfs2_is_mounted(void);
extern void *osfs2_create(const char *name, uint64_t size);
extern int osfs2_write(void *file, uint64_t offset,
                       const void *buffer, uint64_t len);
extern int osfs2_delete(const char *name);

#define CRL_MAX_BYTES       (4U * 1024U * 1024U)
#define CRL_CLOCK_SKEW      300U
#define CRL_DEFAULT_MAX_AGE 86400U

typedef struct {
    const uint8_t *tbs;
    uint32_t tbs_len;
    const uint8_t *inner_algorithm;
    uint32_t inner_algorithm_len;
    const uint8_t *outer_algorithm;
    uint32_t outer_algorithm_len;
    const uint8_t *signature;
    uint32_t signature_len;
    const uint8_t *issuer_name;
    uint32_t issuer_name_len;
    uint32_t this_update;
    uint32_t next_update;
    uint32_t revocation_time;
    uint8_t has_next_update;
    uint8_t has_revocation_time;
} crl_material_t;

static int der_read_len(const uint8_t **cursor, const uint8_t *end,
                        uint32_t *length)
{
    if (*cursor >= end) return -1;
    uint8_t first = *(*cursor)++;
    if (!(first & 0x80)) {
        *length = first;
        return 0;
    }
    uint8_t count = first & 0x7f;
    if (!count || count > 4 || *cursor + count > end) return -1;
    uint32_t value = 0;
    for (uint8_t i = 0; i < count; i++)
        value = (value << 8) | *(*cursor)++;
    *length = value;
    return 0;
}

static int der_enter(const uint8_t **cursor, const uint8_t *end,
                     uint8_t tag, const uint8_t **inner_end)
{
    if (*cursor >= end || *(*cursor)++ != tag) return -1;
    uint32_t len;
    if (der_read_len(cursor, end, &len) < 0 || *cursor + len > end)
        return -1;
    *inner_end = *cursor + len;
    return 0;
}

static int der_skip(const uint8_t **cursor, const uint8_t *end)
{
    if (*cursor >= end) return -1;
    (*cursor)++;
    uint32_t len;
    if (der_read_len(cursor, end, &len) < 0 || *cursor + len > end)
        return -1;
    *cursor += len;
    return 0;
}

static int der_read_value(const uint8_t **cursor, const uint8_t *end,
                          uint8_t tag, const uint8_t **value,
                          uint32_t *value_len)
{
    const uint8_t *p = *cursor;
    if (p >= end || *p++ != tag) return -1;
    uint32_t len;
    if (der_read_len(&p, end, &len) < 0 || p + len > end) return -1;
    *value = p;
    *value_len = len;
    *cursor = p + len;
    return 0;
}

static int parse_time(const uint8_t **cursor, const uint8_t *end,
                      uint32_t *unix_time)
{
    uint32_t consumed;
    if (x509_parse_time_der(*cursor, (uint32_t)(end - *cursor),
                            unix_time, &consumed) < 0)
        return -1;
    *cursor += consumed;
    return 0;
}

static int bytes_equal(const uint8_t *a, uint32_t a_len,
                       const uint8_t *b, uint32_t b_len)
{
    if (a_len != b_len) return 0;
    uint8_t difference = 0;
    for (uint32_t i = 0; i < a_len; i++) difference |= a[i] ^ b[i];
    return difference == 0;
}

static int serial_equal(const uint8_t *a, uint32_t a_len,
                        const uint8_t *b, uint32_t b_len)
{
    while (a_len > 1 && *a == 0) { a++; a_len--; }
    while (b_len > 1 && *b == 0) { b++; b_len--; }
    return bytes_equal(a, a_len, b, b_len);
}

static int parse_crl(const uint8_t *crl, uint32_t crl_len,
                     const uint8_t *serial, uint32_t serial_len,
                     crl_material_t *material)
{
    if (!crl || !crl_len || !serial || !serial_len) return CRL_ERROR;
    crl_material_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    const uint8_t *p = crl;
    const uint8_t *end = crl + crl_len;
    const uint8_t *list_end;
    if (der_enter(&p, end, 0x30, &list_end) < 0 || list_end != end)
        return CRL_ERROR;

    parsed.tbs = p;
    const uint8_t *tbs_end;
    if (der_enter(&p, list_end, 0x30, &tbs_end) < 0)
        return CRL_ERROR;
    parsed.tbs_len = (uint32_t)(tbs_end - parsed.tbs);
    if (p < tbs_end && *p == 0x02 && der_skip(&p, tbs_end) < 0)
        return CRL_ERROR;

    parsed.inner_algorithm = p;
    if (p >= tbs_end || *p != 0x30 || der_skip(&p, tbs_end) < 0)
        return CRL_ERROR;
    parsed.inner_algorithm_len =
        (uint32_t)(p - parsed.inner_algorithm);
    parsed.issuer_name = p;
    if (p >= tbs_end || *p != 0x30 || der_skip(&p, tbs_end) < 0)
        return CRL_ERROR;
    parsed.issuer_name_len = (uint32_t)(p - parsed.issuer_name);
    if (parse_time(&p, tbs_end, &parsed.this_update) < 0)
        return CRL_ERROR;
    if (p < tbs_end && (*p == 0x17 || *p == 0x18)) {
        if (parse_time(&p, tbs_end, &parsed.next_update) < 0)
            return CRL_ERROR;
        parsed.has_next_update = 1;
    }

    int status = CRL_GOOD;
    if (p < tbs_end && *p == 0x30) {
        const uint8_t *revoked_end;
        if (der_enter(&p, tbs_end, 0x30, &revoked_end) < 0)
            return CRL_ERROR;
        while (p < revoked_end) {
            const uint8_t *entry_end;
            if (der_enter(&p, revoked_end, 0x30, &entry_end) < 0)
                return CRL_ERROR;
            const uint8_t *entry_serial;
            uint32_t entry_serial_len;
            if (der_read_value(&p, entry_end, 0x02, &entry_serial,
                               &entry_serial_len) < 0 || !entry_serial_len)
                return CRL_ERROR;
            uint32_t revocation_time;
            if (parse_time(&p, entry_end, &revocation_time) < 0)
                return CRL_ERROR;
            if (p < entry_end && *p == 0x30 && der_skip(&p, entry_end) < 0)
                return CRL_ERROR;
            if (p != entry_end) return CRL_ERROR;
            if (serial_equal(entry_serial, entry_serial_len,
                             serial, serial_len)) {
                status = CRL_REVOKED;
                parsed.revocation_time = revocation_time;
                parsed.has_revocation_time = 1;
            }
            p = entry_end;
        }
        p = revoked_end;
    }
    if (p < tbs_end && *p == 0xA0 && der_skip(&p, tbs_end) < 0)
        return CRL_ERROR;
    if (p != tbs_end) return CRL_ERROR;

    p = tbs_end;
    parsed.outer_algorithm = p;
    if (p >= list_end || *p != 0x30 || der_skip(&p, list_end) < 0)
        return CRL_ERROR;
    parsed.outer_algorithm_len =
        (uint32_t)(p - parsed.outer_algorithm);
    const uint8_t *signature_bits;
    uint32_t signature_bits_len;
    if (der_read_value(&p, list_end, 0x03, &signature_bits,
                       &signature_bits_len) < 0 || p != list_end ||
        signature_bits_len < 2 || signature_bits[0])
        return CRL_ERROR;
    parsed.signature = signature_bits + 1;
    parsed.signature_len = signature_bits_len - 1;
    if (material) *material = parsed;
    return status;
}

static int check_freshness(const crl_material_t *material,
                           uint32_t now_utc, uint32_t *valid_until)
{
    if (!now_utc) return -1;
    uint64_t latest_now = (uint64_t)now_utc + CRL_CLOCK_SKEW;
    if (material->this_update > latest_now) return -1;
    uint64_t expiry = material->has_next_update
        ? material->next_update
        : (uint64_t)material->this_update + CRL_DEFAULT_MAX_AGE;
    if ((material->has_next_update &&
         material->next_update < material->this_update) ||
        (uint64_t)now_utc > expiry + CRL_CLOCK_SKEW)
        return -1;
    expiry += CRL_CLOCK_SKEW;
    *valid_until = expiry > UINT32_MAX ? UINT32_MAX : (uint32_t)expiry;
    return 0;
}

int crl_validate_and_check(const uint8_t *crl, uint32_t crl_len,
                           const uint8_t *cert, uint32_t cert_len,
                           const uint8_t *issuer, uint32_t issuer_len,
                           uint32_t now_utc, uint32_t *valid_until)
{
    if (valid_until) *valid_until = 0;
    const uint8_t *serial;
    uint32_t serial_len;
    if (!cert || !issuer ||
        x509_get_serial_number(cert, cert_len, &serial, &serial_len) < 0)
        return CRL_ERROR;
    crl_material_t material;
    int status = parse_crl(crl, crl_len, serial, serial_len, &material);
    uint32_t expiry;
    const uint8_t *issuer_subject;
    uint32_t issuer_subject_len;
    x509_v3_t extensions;
    if (status == CRL_ERROR ||
        !bytes_equal(material.inner_algorithm,
                     material.inner_algorithm_len,
                     material.outer_algorithm,
                     material.outer_algorithm_len) ||
        x509_get_subject_name_der(issuer, issuer_len, &issuer_subject,
                                  &issuer_subject_len) < 0 ||
        !bytes_equal(material.issuer_name, material.issuer_name_len,
                     issuer_subject, issuer_subject_len) ||
        x509_issuer_matches_subject(cert, cert_len,
                                    issuer, issuer_len) != 1 ||
        x509_check_validity(issuer, issuer_len, now_utc) != 0 ||
        x509_parse_v3(issuer, issuer_len, &extensions) < 0 ||
        (extensions.has_ku &&
         !(extensions.key_usage_flags & X509_KU_CRL_SIGN)) ||
        check_freshness(&material, now_utc, &expiry) < 0 ||
        x509_verify_signed_blob(
            material.tbs, material.tbs_len,
            material.outer_algorithm, material.outer_algorithm_len,
            material.signature, material.signature_len,
            issuer, issuer_len) != 0) {
        serial_puts("[CRL] authenticated list validation failed\n");
        return CRL_ERROR;
    }
    if (status == CRL_REVOKED && material.has_revocation_time &&
        material.revocation_time > (uint64_t)now_utc + CRL_CLOCK_SKEW)
        status = CRL_GOOD;
    if (valid_until) *valid_until = expiry;
    return status;
}

int crl_parse_and_check(const uint8_t *crl, uint32_t crl_len,
                        const uint8_t *serial, uint32_t serial_len)
{
    return parse_crl(crl, crl_len, serial, serial_len, NULL);
}

static void cache_path(const char *url, char *out, uint32_t capacity)
{
    uint32_t url_len = 0;
    while (url[url_len]) url_len++;
    uint8_t digest[32];
    sha256(url, url_len, digest);
    static const char prefix[] = "tls/crl-";
    static const char hex[] = "0123456789abcdef";
    static const char suffix[] = ".der";
    uint32_t offset = 0;
    for (uint32_t i = 0; prefix[i] && offset + 1 < capacity; i++)
        out[offset++] = prefix[i];
    for (uint32_t i = 0; i < 4 && offset + 2 < capacity; i++) {
        out[offset++] = hex[digest[i] >> 4];
        out[offset++] = hex[digest[i] & 0x0f];
    }
    for (uint32_t i = 0; suffix[i] && offset + 1 < capacity; i++)
        out[offset++] = suffix[i];
    out[offset] = 0;
}

static uint8_t *load_cached_crl(const char *path, uint32_t *out_len)
{
    vfs_node_t node;
    if (!vfs_find(path, VFS_MODE_POSIX, &node) || !node.size ||
        node.size > CRL_MAX_BYTES)
        return NULL;
    uint8_t *buffer = (uint8_t *)kmalloc(node.size);
    if (!buffer) return NULL;
    int amount = vfs_read(&node, 0, buffer, node.size);
    if (amount < 0 || (uint64_t)amount != node.size) {
        kfree(buffer);
        return NULL;
    }
    *out_len = (uint32_t)node.size;
    return buffer;
}

static void persist_crl(const char *path, const uint8_t *crl,
                        uint32_t crl_len)
{
    if (!osfs2_is_mounted()) return;
    if (osfs3_is_mounted()) {
        char temporary[64];
        uint32_t i = 0;
        while (path[i] && i + 5 < sizeof(temporary)) {
            temporary[i] = path[i];
            i++;
        }
        temporary[i++] = '.';
        temporary[i++] = 'n';
        temporary[i++] = 'e';
        temporary[i++] = 'w';
        temporary[i] = 0;
        (void)osfs3_delete(temporary);
        void *file = osfs3_create(temporary, crl_len);
        if (!file || osfs3_write(file, 0, crl, crl_len) < 0 ||
            osfs3_truncate(file, crl_len) < 0 ||
            osfs3_rename(temporary, path, true) < 0) {
            (void)osfs3_delete(temporary);
            serial_puts("[CRL] cache publish failed\n");
        }
        return;
    }
    (void)osfs2_delete(path);
    void *file = osfs2_create(path, crl_len);
    if (!file || osfs2_write(file, 0, crl, crl_len) < 0)
        serial_puts("[CRL] cache write failed\n");
}

static int crl_check_impl(const uint8_t *cert, uint32_t cert_len,
                          const uint8_t *issuer, uint32_t issuer_len)
{
    uint32_t now_utc = ntp_get_utc();
    int cached_status = certmgr_revocation_lookup(cert, cert_len,
                                                  now_utc, NULL);
    if (cached_status == CERTMGR_REVOCATION_GOOD ||
        cached_status == CERTMGR_REVOCATION_REVOKED) {
        serial_puts("[CRL] authenticated memory cache hit\n");
        return cached_status;
    }
    if (!now_utc) {
        serial_puts("[CRL] no trusted wall clock; status unavailable\n");
        return CRL_ERROR;
    }

    char url[512];
    int url_len = x509_get_crldp_url(cert, cert_len, url, sizeof(url));
    if (url_len <= 0 || (uint32_t)url_len >= sizeof(url))
        return CRL_ERROR;
    char path[32];
    cache_path(url, path, sizeof(path));

    uint32_t raw_len = 0;
    uint8_t *raw = load_cached_crl(path, &raw_len);
    uint32_t valid_until;
    int status = CRL_ERROR;
    if (raw) {
        status = crl_validate_and_check(raw, raw_len, cert, cert_len,
                                        issuer, issuer_len, now_utc,
                                        &valid_until);
        if (status != CRL_ERROR) {
            certmgr_revocation_store(
                cert, cert_len, (certmgr_revocation_status_t)status,
                valid_until);
            kfree(raw);
            return status;
        }
        kfree(raw);
    }

    raw = (uint8_t *)kmalloc(CRL_MAX_BYTES);
    if (!raw) return CRL_ERROR;
    int amount = http_plain_get(url, raw, CRL_MAX_BYTES);
    if (amount > 0) {
        raw_len = (uint32_t)amount;
        status = crl_validate_and_check(raw, raw_len, cert, cert_len,
                                        issuer, issuer_len, now_utc,
                                        &valid_until);
        if (status != CRL_ERROR) {
            certmgr_revocation_store(
                cert, cert_len, (certmgr_revocation_status_t)status,
                valid_until);
            persist_crl(path, raw, raw_len);
        }
    }
    kfree(raw);
    return status;
}

int crl_check_revoked(const uint8_t *cert, uint32_t cert_len)
{
    (void)cert;
    (void)cert_len;
    serial_puts("[CRL] issuer required for authenticated validation\n");
    return CRL_ERROR;
}

int crl_check_revoked_with_issuer(const uint8_t *cert, uint32_t cert_len,
                                  const uint8_t *issuer,
                                  uint32_t issuer_len)
{
    return crl_check_impl(cert, cert_len, issuer, issuer_len);
}
