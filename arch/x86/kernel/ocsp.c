/* Authenticated RFC 6960 OCSP client and parser. */

#include "../include/types.h"
#include "certmgr.h"
#include "ocsp.h"
#include "x509.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t value);
extern void sha1(const void *data, uint32_t len, uint8_t digest[20]);
extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);
extern uint32_t ntp_get_utc(void);
extern int http_plain_post(const char *url, const char *content_type,
                           const uint8_t *body, uint32_t body_len,
                           uint8_t *out, uint32_t out_cap);

#define OCSP_MAX_SIGNERS       4U
#define OCSP_CLOCK_SKEW        300U
#define OCSP_DEFAULT_MAX_AGE   86400U
#define OCSP_UNKNOWN_MAX_AGE   300U
#define OCSP_RESPONSE_MAX      (64U * 1024U)
#define OCSP_SIGNING_EKU       "1.3.6.1.5.5.7.3.9"

static const uint8_t SHA1_ALGORITHM_ID[] = {
    0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00
};
static const uint8_t OCSP_OID_BASIC_RESPONSE[] = {
    0x06, 0x09, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x30, 0x01, 0x01
};
static const uint8_t OCSP_OID_SHA1[] = {
    0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a
};

typedef struct {
    const uint8_t *tbs;
    uint32_t tbs_len;
    const uint8_t *signature_algorithm;
    uint32_t signature_algorithm_len;
    const uint8_t *signature;
    uint32_t signature_len;
    const uint8_t *responder_id;
    uint32_t responder_id_len;
    const uint8_t *signers[OCSP_MAX_SIGNERS];
    uint32_t signer_lens[OCSP_MAX_SIGNERS];
    uint32_t signer_count;
    uint32_t produced_at;
    uint32_t this_update;
    uint32_t next_update;
    uint32_t revocation_time;
    uint8_t has_next_update;
    uint8_t has_revocation_time;
    ocsp_status_t status;
} ocsp_material_t;

static uint32_t emit_length(uint8_t *buffer, uint32_t offset, uint32_t len)
{
    if (len < 128) {
        buffer[offset++] = (uint8_t)len;
    } else if (len < 256) {
        buffer[offset++] = 0x81;
        buffer[offset++] = (uint8_t)len;
    } else {
        buffer[offset++] = 0x82;
        buffer[offset++] = (uint8_t)(len >> 8);
        buffer[offset++] = (uint8_t)len;
    }
    return offset;
}

static uint32_t emit_tlv(uint8_t *buffer, uint32_t offset, uint8_t tag,
                         const uint8_t *body, uint32_t body_len)
{
    buffer[offset++] = tag;
    offset = emit_length(buffer, offset, body_len);
    for (uint32_t i = 0; i < body_len; i++) buffer[offset++] = body[i];
    return offset;
}

static int build_ocsp_request(const uint8_t *cert, uint32_t cert_len,
                              const uint8_t *issuer, uint32_t issuer_len,
                              uint8_t *out, uint32_t out_cap)
{
    const uint8_t *issuer_name;
    const uint8_t *issuer_key;
    const uint8_t *serial;
    uint32_t issuer_name_len, issuer_key_len, serial_len;
    if (x509_get_issuer_der(cert, cert_len, &issuer_name,
                            &issuer_name_len) < 0 ||
        x509_get_subject_pubkey_bits(issuer, issuer_len, &issuer_key,
                                     &issuer_key_len) < 0 ||
        x509_get_serial_number(cert, cert_len, &serial, &serial_len) < 0 ||
        !serial_len || serial_len > 63)
        return -1;

    uint8_t name_hash[20], key_hash[20];
    sha1(issuer_name, issuer_name_len, name_hash);
    sha1(issuer_key, issuer_key_len, key_hash);

    uint8_t cert_id_body[256];
    uint32_t offset = 0;
    for (uint32_t i = 0; i < sizeof(SHA1_ALGORITHM_ID); i++)
        cert_id_body[offset++] = SHA1_ALGORITHM_ID[i];
    offset = emit_tlv(cert_id_body, offset, 0x04, name_hash, sizeof(name_hash));
    offset = emit_tlv(cert_id_body, offset, 0x04, key_hash, sizeof(key_hash));
    uint8_t positive_serial[64];
    uint32_t positive_len = 0;
    if (serial[0] & 0x80) positive_serial[positive_len++] = 0;
    for (uint32_t i = 0; i < serial_len; i++)
        positive_serial[positive_len++] = serial[i];
    offset = emit_tlv(cert_id_body, offset, 0x02,
                      positive_serial, positive_len);

    uint8_t cert_id[320], request[384], request_list[448], tbs[512];
    uint32_t cert_id_len = emit_tlv(cert_id, 0, 0x30,
                                    cert_id_body, offset);
    uint32_t request_len = emit_tlv(request, 0, 0x30,
                                    cert_id, cert_id_len);
    uint32_t request_list_len = emit_tlv(request_list, 0, 0x30,
                                         request, request_len);
    uint32_t tbs_len = emit_tlv(tbs, 0, 0x30,
                                request_list, request_list_len);
    if (out_cap < tbs_len + 5) return -1;
    return (int)emit_tlv(out, 0, 0x30, tbs, tbs_len);
}

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

static int parse_sha1_algorithm(const uint8_t **cursor,
                                const uint8_t *end)
{
    const uint8_t *p = *cursor;
    const uint8_t *algorithm_end;
    if (der_enter(&p, end, 0x30, &algorithm_end) < 0 ||
        (uint32_t)(algorithm_end - p) < sizeof(OCSP_OID_SHA1) ||
        !bytes_equal(p, sizeof(OCSP_OID_SHA1), OCSP_OID_SHA1,
                     sizeof(OCSP_OID_SHA1)))
        return -1;
    p += sizeof(OCSP_OID_SHA1);
    if (p < algorithm_end) {
        if ((uint32_t)(algorithm_end - p) != 2 || p[0] != 0x05 || p[1])
            return -1;
        p += 2;
    }
    if (p != algorithm_end) return -1;
    *cursor = algorithm_end;
    return 0;
}

static int cert_id_matches(const uint8_t **cursor, const uint8_t *end,
                           const uint8_t *cert, uint32_t cert_len,
                           const uint8_t *issuer, uint32_t issuer_len)
{
    const uint8_t *p = *cursor;
    const uint8_t *cert_id_end;
    if (der_enter(&p, end, 0x30, &cert_id_end) < 0 ||
        parse_sha1_algorithm(&p, cert_id_end) < 0)
        return -1;

    const uint8_t *name_hash, *key_hash, *serial;
    uint32_t name_hash_len, key_hash_len, serial_len;
    if (der_read_value(&p, cert_id_end, 0x04, &name_hash,
                       &name_hash_len) < 0 ||
        der_read_value(&p, cert_id_end, 0x04, &key_hash,
                       &key_hash_len) < 0 ||
        der_read_value(&p, cert_id_end, 0x02, &serial,
                       &serial_len) < 0 || p != cert_id_end ||
        name_hash_len != 20 || key_hash_len != 20 || !serial_len)
        return -1;

    const uint8_t *issuer_name, *issuer_key, *expected_serial;
    uint32_t issuer_name_len, issuer_key_len, expected_serial_len;
    if (x509_get_issuer_der(cert, cert_len, &issuer_name,
                            &issuer_name_len) < 0 ||
        x509_get_subject_pubkey_bits(issuer, issuer_len, &issuer_key,
                                     &issuer_key_len) < 0 ||
        x509_get_serial_number(cert, cert_len, &expected_serial,
                               &expected_serial_len) < 0)
        return -1;
    uint8_t expected_name_hash[20], expected_key_hash[20];
    sha1(issuer_name, issuer_name_len, expected_name_hash);
    sha1(issuer_key, issuer_key_len, expected_key_hash);
    *cursor = cert_id_end;
    return bytes_equal(name_hash, name_hash_len,
                       expected_name_hash, sizeof(expected_name_hash)) &&
           bytes_equal(key_hash, key_hash_len,
                       expected_key_hash, sizeof(expected_key_hash)) &&
           serial_equal(serial, serial_len,
                        expected_serial, expected_serial_len);
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

static int parse_explicit_time(const uint8_t **cursor, const uint8_t *end,
                               uint8_t tag, uint32_t *unix_time)
{
    const uint8_t *p = *cursor;
    const uint8_t *explicit_end;
    if (der_enter(&p, end, tag, &explicit_end) < 0 ||
        parse_time(&p, explicit_end, unix_time) < 0 || p != explicit_end)
        return -1;
    *cursor = explicit_end;
    return 0;
}

static int parse_single_response(const uint8_t **cursor, const uint8_t *end,
                                 const uint8_t *cert, uint32_t cert_len,
                                 const uint8_t *issuer, uint32_t issuer_len,
                                 ocsp_material_t *material)
{
    const uint8_t *p = *cursor;
    const uint8_t *single_end;
    if (der_enter(&p, end, 0x30, &single_end) < 0) return -1;
    int matches = cert_id_matches(&p, single_end, cert, cert_len,
                                  issuer, issuer_len);
    if (matches < 0 || p >= single_end) return -1;

    ocsp_status_t status;
    uint32_t revocation_time = 0;
    uint8_t has_revocation_time = 0;
    if (*p == 0x80 || *p == 0x82) {
        uint8_t tag = *p++;
        uint32_t len;
        if (der_read_len(&p, single_end, &len) < 0 || len) return -1;
        status = tag == 0x80 ? OCSP_GOOD : OCSP_UNKNOWN;
    } else if (*p == 0xA1) {
        const uint8_t *revoked_end;
        if (der_enter(&p, single_end, 0xA1, &revoked_end) < 0 ||
            parse_time(&p, revoked_end, &revocation_time) < 0)
            return -1;
        if (p < revoked_end && *p == 0xA0 && der_skip(&p, revoked_end) < 0)
            return -1;
        if (p != revoked_end) return -1;
        status = OCSP_REVOKED;
        has_revocation_time = 1;
    } else {
        return -1;
    }

    uint32_t this_update, next_update = 0;
    uint8_t has_next_update = 0;
    if (parse_time(&p, single_end, &this_update) < 0) return -1;
    if (p < single_end && *p == 0xA0) {
        if (parse_explicit_time(&p, single_end, 0xA0,
                                &next_update) < 0)
            return -1;
        has_next_update = 1;
    }
    if (p < single_end && *p == 0xA1 && der_skip(&p, single_end) < 0)
        return -1;
    if (p != single_end) return -1;

    if (matches) {
        material->status = status;
        material->this_update = this_update;
        material->next_update = next_update;
        material->has_next_update = has_next_update;
        material->revocation_time = revocation_time;
        material->has_revocation_time = has_revocation_time;
    }
    *cursor = single_end;
    return matches;
}

static int parse_ocsp_response(const uint8_t *body, uint32_t body_len,
                               const uint8_t *cert, uint32_t cert_len,
                               const uint8_t *issuer, uint32_t issuer_len,
                               ocsp_material_t *material)
{
    if (!body || !body_len || !cert || !cert_len || !issuer || !issuer_len ||
        !material)
        return -1;
    memset(material, 0, sizeof(*material));

    const uint8_t *p = body;
    const uint8_t *end = body + body_len;
    const uint8_t *outer_end;
    if (der_enter(&p, end, 0x30, &outer_end) < 0 || outer_end != end)
        return -1;
    const uint8_t *response_status;
    uint32_t response_status_len;
    if (der_read_value(&p, outer_end, 0x0A, &response_status,
                       &response_status_len) < 0 ||
        response_status_len != 1 || response_status[0])
        return -1;

    const uint8_t *response_bytes_end;
    const uint8_t *response_sequence_end;
    if (der_enter(&p, outer_end, 0xA0, &response_bytes_end) < 0 ||
        response_bytes_end != outer_end ||
        der_enter(&p, response_bytes_end, 0x30,
                  &response_sequence_end) < 0 ||
        response_sequence_end != response_bytes_end ||
        (uint32_t)(response_sequence_end - p) <
            sizeof(OCSP_OID_BASIC_RESPONSE) ||
        !bytes_equal(p, sizeof(OCSP_OID_BASIC_RESPONSE),
                     OCSP_OID_BASIC_RESPONSE,
                     sizeof(OCSP_OID_BASIC_RESPONSE)))
        return -1;
    p += sizeof(OCSP_OID_BASIC_RESPONSE);

    const uint8_t *basic_bytes;
    uint32_t basic_len;
    if (der_read_value(&p, response_sequence_end, 0x04, &basic_bytes,
                       &basic_len) < 0 || p != response_sequence_end)
        return -1;
    p = basic_bytes;
    const uint8_t *basic_end;
    if (der_enter(&p, basic_bytes + basic_len, 0x30, &basic_end) < 0 ||
        basic_end != basic_bytes + basic_len)
        return -1;

    material->tbs = p;
    const uint8_t *response_data_end;
    if (der_enter(&p, basic_end, 0x30, &response_data_end) < 0)
        return -1;
    material->tbs_len = (uint32_t)(response_data_end - material->tbs);
    if (p < response_data_end && *p == 0xA0 &&
        der_skip(&p, response_data_end) < 0)
        return -1;
    if (p >= response_data_end ||
        (*p != 0xA1 && *p != 0xA2 && *p != 0x82))
        return -1;
    material->responder_id = p;
    if (der_skip(&p, response_data_end) < 0) return -1;
    material->responder_id_len =
        (uint32_t)(p - material->responder_id);
    if (parse_time(&p, response_data_end, &material->produced_at) < 0)
        return -1;

    const uint8_t *responses_end;
    if (der_enter(&p, response_data_end, 0x30, &responses_end) < 0)
        return -1;
    int matches = 0;
    while (p < responses_end) {
        int result = parse_single_response(&p, responses_end,
                                           cert, cert_len,
                                           issuer, issuer_len, material);
        if (result < 0) return -1;
        matches += result;
    }
    if (p != responses_end || matches != 1) return -1;
    p = responses_end;
    if (p < response_data_end && *p == 0xA1 &&
        der_skip(&p, response_data_end) < 0)
        return -1;
    if (p != response_data_end) return -1;

    p = response_data_end;
    if (p >= basic_end || *p != 0x30) return -1;
    material->signature_algorithm = p;
    if (der_skip(&p, basic_end) < 0) return -1;
    material->signature_algorithm_len =
        (uint32_t)(p - material->signature_algorithm);
    const uint8_t *signature_bits;
    uint32_t signature_bits_len;
    if (der_read_value(&p, basic_end, 0x03, &signature_bits,
                       &signature_bits_len) < 0 ||
        signature_bits_len < 2 || signature_bits[0])
        return -1;
    material->signature = signature_bits + 1;
    material->signature_len = signature_bits_len - 1;

    if (p < basic_end && *p == 0xA0) {
        const uint8_t *explicit_end;
        const uint8_t *certificates_end;
        if (der_enter(&p, basic_end, 0xA0, &explicit_end) < 0 ||
            der_enter(&p, explicit_end, 0x30, &certificates_end) < 0 ||
            certificates_end != explicit_end)
            return -1;
        while (p < certificates_end) {
            const uint8_t *certificate = p;
            const uint8_t *certificate_end;
            if (der_enter(&p, certificates_end, 0x30,
                          &certificate_end) < 0)
                return -1;
            if (material->signer_count < OCSP_MAX_SIGNERS) {
                uint32_t index = material->signer_count++;
                material->signers[index] = certificate;
                material->signer_lens[index] =
                    (uint32_t)(certificate_end - certificate);
            }
            p = certificate_end;
        }
        p = explicit_end;
    }
    return p == basic_end ? 0 : -1;
}

static int responder_id_matches(const ocsp_material_t *material,
                                const uint8_t *signer,
                                uint32_t signer_len)
{
    const uint8_t *p = material->responder_id;
    const uint8_t *end = p + material->responder_id_len;
    uint8_t tag = *p;
    if (tag == 0xA1) {
        const uint8_t *name_end;
        const uint8_t *subject;
        uint32_t subject_len;
        if (der_enter(&p, end, 0xA1, &name_end) < 0 || name_end != end ||
            x509_get_subject_name_der(signer, signer_len, &subject,
                                      &subject_len) < 0)
            return 0;
        if (bytes_equal(p, (uint32_t)(name_end - p), subject, subject_len))
            return 1;
        const uint8_t *subject_body = subject;
        const uint8_t *subject_end;
        return der_enter(&subject_body, subject + subject_len, 0x30,
                         &subject_end) == 0 &&
               subject_end == subject + subject_len &&
               bytes_equal(p, (uint32_t)(name_end - p), subject_body,
                           (uint32_t)(subject_end - subject_body));
    }

    const uint8_t *key_hash = NULL;
    uint32_t key_hash_len = 0;
    if (tag == 0x82) {
        if (der_read_value(&p, end, 0x82, &key_hash, &key_hash_len) < 0 ||
            p != end)
            return 0;
    } else if (tag == 0xA2) {
        const uint8_t *key_end;
        if (der_enter(&p, end, 0xA2, &key_end) < 0 || key_end != end)
            return 0;
        if (p < key_end && *p == 0x04) {
            if (der_read_value(&p, key_end, 0x04, &key_hash,
                               &key_hash_len) < 0 || p != key_end)
                return 0;
        } else {
            key_hash = p;
            key_hash_len = (uint32_t)(key_end - p);
        }
    }
    if (!key_hash || key_hash_len != 20) return 0;
    const uint8_t *subject_key;
    uint32_t subject_key_len;
    uint8_t expected[20];
    if (x509_get_subject_pubkey_bits(signer, signer_len, &subject_key,
                                     &subject_key_len) < 0)
        return 0;
    sha1(subject_key, subject_key_len, expected);
    return bytes_equal(key_hash, key_hash_len, expected, sizeof(expected));
}

static int verify_response_signature(const ocsp_material_t *material,
                                     const uint8_t *signer,
                                     uint32_t signer_len)
{
    return x509_verify_signed_blob(
        material->tbs, material->tbs_len,
        material->signature_algorithm, material->signature_algorithm_len,
        material->signature, material->signature_len,
        signer, signer_len);
}

static int authorize_signer(const ocsp_material_t *material,
                            const uint8_t *issuer, uint32_t issuer_len,
                            uint32_t now_utc)
{
    if (responder_id_matches(material, issuer, issuer_len) &&
        verify_response_signature(material, issuer, issuer_len) == 0) {
        serial_puts("[OCSP] authorized issuer responder\n");
        return 0;
    }
    for (uint32_t i = 0; i < material->signer_count; i++) {
        const uint8_t *signer = material->signers[i];
        uint32_t signer_len = material->signer_lens[i];
        x509_v3_t extensions;
        if (!responder_id_matches(material, signer, signer_len) ||
            x509_issuer_matches_subject(signer, signer_len,
                                        issuer, issuer_len) != 1 ||
            x509_verify_chain_link(signer, signer_len,
                                   issuer, issuer_len) != 0 ||
            x509_check_validity(signer, signer_len, now_utc) != 0 ||
            x509_has_extended_key_usage(signer, signer_len,
                                        OCSP_SIGNING_EKU) != 1 ||
            x509_parse_v3(signer, signer_len, &extensions) < 0 ||
            (extensions.has_ku &&
             !(extensions.key_usage_flags & X509_KU_DIGITAL_SIGNATURE)) ||
            verify_response_signature(material, signer, signer_len) != 0)
            continue;
        serial_puts("[OCSP] authorized delegated responder\n");
        return 0;
    }
    serial_puts("[OCSP] signer authorization failed\n");
    return -1;
}

static int check_freshness(const ocsp_material_t *material,
                           uint32_t now_utc, uint32_t *valid_until)
{
    if (!now_utc) return -1;
    uint64_t latest_now = (uint64_t)now_utc + OCSP_CLOCK_SKEW;
    if (material->produced_at > latest_now ||
        material->this_update > latest_now ||
        (uint64_t)material->produced_at + OCSP_CLOCK_SKEW <
            material->this_update ||
        (material->has_revocation_time &&
         material->revocation_time > latest_now))
        return -1;
    uint64_t expiry = material->has_next_update
        ? material->next_update
        : (uint64_t)material->this_update + OCSP_DEFAULT_MAX_AGE;
    if ((material->has_next_update &&
         material->next_update < material->this_update) ||
        (uint64_t)now_utc > expiry + OCSP_CLOCK_SKEW)
        return -1;
    if (material->status == OCSP_UNKNOWN &&
        expiry > (uint64_t)now_utc + OCSP_UNKNOWN_MAX_AGE)
        expiry = (uint64_t)now_utc + OCSP_UNKNOWN_MAX_AGE;
    expiry += OCSP_CLOCK_SKEW;
    *valid_until = expiry > UINT32_MAX ? UINT32_MAX : (uint32_t)expiry;
    return 0;
}

ocsp_status_t ocsp_validate_response(const uint8_t *response,
                                     uint32_t response_len,
                                     const uint8_t *cert, uint32_t cert_len,
                                     const uint8_t *issuer,
                                     uint32_t issuer_len,
                                     uint32_t now_utc,
                                     uint32_t *valid_until)
{
    if (valid_until) *valid_until = 0;
    ocsp_material_t material;
    uint32_t expiry;
    if (parse_ocsp_response(response, response_len, cert, cert_len,
                            issuer, issuer_len, &material) < 0 ||
        check_freshness(&material, now_utc, &expiry) < 0 ||
        authorize_signer(&material, issuer, issuer_len, now_utc) < 0) {
        serial_puts("[OCSP] authenticated response validation failed\n");
        return OCSP_ERROR;
    }
    if (valid_until) *valid_until = expiry;
    return material.status;
}

ocsp_status_t ocsp_check(const uint8_t *cert, uint32_t cert_len,
                         const uint8_t *issuer, uint32_t issuer_len)
{
    uint32_t now_utc = ntp_get_utc();
    int cached = certmgr_revocation_lookup(cert, cert_len, now_utc, NULL);
    if (cached != CERTMGR_REVOCATION_UNAVAILABLE) {
        serial_puts("[OCSP] authenticated cache hit\n");
        return (ocsp_status_t)cached;
    }
    if (!now_utc) {
        serial_puts("[OCSP] no trusted wall clock; status unavailable\n");
        return OCSP_ERROR;
    }

    char url[512];
    int url_len = x509_get_aia_ocsp(cert, cert_len, url, sizeof(url));
    if (url_len <= 0 || (uint32_t)url_len >= sizeof(url)) {
        serial_puts("[OCSP] no usable AIA OCSP URL\n");
        return OCSP_ERROR;
    }

    uint8_t request[1024];
    int request_len = build_ocsp_request(cert, cert_len, issuer, issuer_len,
                                         request, sizeof(request));
    if (request_len < 0) {
        serial_puts("[OCSP] request construction failed\n");
        return OCSP_ERROR;
    }
    uint8_t *response = (uint8_t *)kmalloc(OCSP_RESPONSE_MAX);
    if (!response) return OCSP_ERROR;
    int response_len = http_plain_post(url, "application/ocsp-request",
                                       request, (uint32_t)request_len,
                                       response, OCSP_RESPONSE_MAX);
    if (response_len <= 0) {
        serial_puts("[OCSP] responder unavailable\n");
        kfree(response);
        return OCSP_ERROR;
    }

    uint32_t valid_until;
    ocsp_status_t status = ocsp_validate_response(
        response, (uint32_t)response_len, cert, cert_len,
        issuer, issuer_len, now_utc, &valid_until);
    kfree(response);
    if (status != OCSP_ERROR)
        certmgr_revocation_store(cert, cert_len,
                                 (certmgr_revocation_status_t)status,
                                 valid_until);
    return status;
}

ocsp_status_t ocsp_parse_stapled(const uint8_t *response,
                                 uint32_t response_len,
                                 const uint8_t *cert, uint32_t cert_len,
                                 const uint8_t *issuer, uint32_t issuer_len)
{
    uint32_t now_utc = ntp_get_utc();
    uint32_t valid_until;
    ocsp_status_t status = ocsp_validate_response(
        response, response_len, cert, cert_len, issuer, issuer_len,
        now_utc, &valid_until);
    if (status != OCSP_ERROR)
        certmgr_revocation_store(cert, cert_len,
                                 (certmgr_revocation_status_t)status,
                                 valid_until);
    return status;
}
