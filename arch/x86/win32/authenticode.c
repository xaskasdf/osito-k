/* Authenticode verification for PE files and WINTRUST_ACTION_GENERIC_VERIFY_V2. */

#include "authenticode.h"
#include "kernel32_shim.h"
#include "pe.h"
#include "../kernel/certmgr.h"
#include "../kernel/crypto.h"
#include "../kernel/ecdsa_p256.h"
#include "../kernel/ecdsa_p384.h"
#include "../kernel/x509.h"

extern void sha384(const uint8_t *data, uint64_t len, uint8_t hash[48]);
extern int rsa_pkcs1_v15_sha256_verify(
    const uint8_t *sig, uint32_t sig_len, const uint8_t *modulus,
    uint32_t modulus_len, const uint8_t *exponent, uint32_t exponent_len,
    const uint8_t hash[32]);
extern int rsa_pkcs1_v15_sha384_verify(
    const uint8_t *sig, uint32_t sig_len, const uint8_t *modulus,
    uint32_t modulus_len, const uint8_t *exponent, uint32_t exponent_len,
    const uint8_t hash[48]);
extern uint32_t ntp_get_utc(void);
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t value, int digits);
extern void serial_putdec(uint64_t value);

#define AUTH_OPEN_EXISTING             3U
#define AUTH_FILE_BEGIN                0U
#define AUTH_WIN_CERT_TYPE_PKCS_SIGNED 0x0002U
#define AUTH_MAX_CERTS                 16U
#define AUTH_MAX_CHAIN                 12U
#define AUTH_MAX_PKCS7                 (2U * 1024U * 1024U)
#define AUTH_HASH_BUFFER               4096U

#define AUTH_WTD_REVOKE_WHOLECHAIN              1U
#define AUTH_WTD_REVOCATION_CHECK_END_CERT       0x00000020U
#define AUTH_WTD_REVOCATION_CHECK_CHAIN          0x00000040U
#define AUTH_WTD_REVOCATION_CHECK_CHAIN_EX_ROOT  0x00000080U
#define AUTH_WTD_CACHE_ONLY_URL_RETRIEVAL        0x00001000U

typedef enum {
    AUTH_HASH_UNKNOWN = 0,
    AUTH_HASH_SHA1,
    AUTH_HASH_SHA256,
    AUTH_HASH_SHA384,
} auth_hash_algorithm_t;

typedef struct {
    BYTE tag;
    const BYTE *tlv;
    DWORD tlv_len;
    const BYTE *value;
    DWORD value_len;
} auth_der_t;

typedef struct {
    const BYTE *der;
    DWORD der_len;
} auth_cert_t;

typedef struct {
    auth_der_t sid;
    auth_hash_algorithm_t digest_algorithm;
    auth_der_t signed_attributes;
    auth_der_t signature_algorithm;
    const BYTE *signature;
    DWORD signature_len;
    auth_der_t unsigned_attributes;
} auth_signer_t;

typedef struct {
    auth_hash_algorithm_t file_digest_algorithm;
    const BYTE *file_digest;
    DWORD file_digest_len;
    const BYTE *content;
    DWORD content_len;
    auth_cert_t certs[AUTH_MAX_CERTS];
    DWORD cert_count;
    auth_signer_t signer;
} auth_cms_t;

typedef struct {
    DWORD file_size;
    DWORD checksum_offset;
    DWORD security_entry_offset;
    DWORD certificate_offset;
    DWORD certificate_size;
} auth_pe_info_t;

typedef struct {
    auth_hash_algorithm_t algorithm;
    union {
        sha1_ctx sha1;
        sha256_ctx sha256;
    } state;
} auth_hash_ctx_t;

typedef struct {
    DWORD Signature;
    IMAGE_FILE_HEADER FileHeader;
} auth_nt_prefix_t;

_Static_assert(sizeof(auth_nt_prefix_t) == 24, "PE NT prefix layout");

static const BYTE oid_pkcs7_signed_data[] =
    { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x07,0x02 };
static const BYTE oid_spc_indirect_data[] =
    { 0x2B,0x06,0x01,0x04,0x01,0x82,0x37,0x02,0x01,0x04 };
static const BYTE oid_sha1[] =
    { 0x2B,0x0E,0x03,0x02,0x1A };
static const BYTE oid_sha256[] =
    { 0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01 };
static const BYTE oid_sha384[] =
    { 0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02 };
static const BYTE oid_rsa_encryption[] =
    { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01 };
static const BYTE oid_sha256_rsa[] =
    { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B };
static const BYTE oid_sha384_rsa[] =
    { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C };
static const BYTE oid_ecdsa_sha256[] =
    { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02 };
static const BYTE oid_ecdsa_sha384[] =
    { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03 };
static const BYTE oid_content_type[] =
    { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x09,0x03 };
static const BYTE oid_message_digest[] =
    { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x09,0x04 };
static const BYTE oid_signing_time[] =
    { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x09,0x05 };
static const BYTE oid_counter_signature[] =
    { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x09,0x06 };

static int auth_bytes_equal(const BYTE *left, const BYTE *right, DWORD length)
{
    if (!left || !right) return 0;
    for (DWORD i = 0; i < length; i++)
        if (left[i] != right[i]) return 0;
    return 1;
}

static void auth_copy(BYTE *destination, const BYTE *source, DWORD length)
{
    while (length--) *destination++ = *source++;
}

static DWORD auth_read_u32(const BYTE *value)
{
    return (DWORD)value[0] | ((DWORD)value[1] << 8) |
           ((DWORD)value[2] << 16) | ((DWORD)value[3] << 24);
}

static int auth_read_at(HANDLE file, DWORD offset, PVOID buffer, DWORD length)
{
    BYTE *output = (BYTE *)buffer;
    if (offset > 0x7FFFFFFFU ||
        SetFilePointer(file, (LONG)offset, NULL, AUTH_FILE_BEGIN) != offset)
        return -1;

    while (length) {
        DWORD read = 0;
        if (!ReadFile(file, output, length, &read, NULL) || read == 0)
            return -1;
        output += read;
        length -= read;
    }
    return 0;
}

static int auth_der_read(const BYTE **cursor, const BYTE *end, auth_der_t *out)
{
    if (!cursor || !*cursor || !end || !out || *cursor >= end)
        return -1;

    const BYTE *start = *cursor;
    const BYTE *p = start;
    BYTE tag = *p++;
    if (p >= end) return -1;

    DWORD length = *p++;
    if (length & 0x80U) {
        DWORD bytes = length & 0x7FU;
        if (bytes == 0 || bytes > 4U || (DWORD)(end - p) < bytes)
            return -1;
        length = 0;
        for (DWORD i = 0; i < bytes; i++)
            length = (length << 8) | *p++;
        if (length < 128U) return -1;
    }
    if ((DWORD)(end - p) < length) return -1;

    out->tag = tag;
    out->tlv = start;
    out->tlv_len = (DWORD)(p - start) + length;
    out->value = p;
    out->value_len = length;
    *cursor = p + length;
    return 0;
}

static int auth_der_expect(const BYTE **cursor, const BYTE *end, BYTE tag,
                           auth_der_t *out)
{
    return auth_der_read(cursor, end, out) == 0 && out->tag == tag ? 0 : -1;
}

static int auth_der_oid_equal(const auth_der_t *oid, const BYTE *value,
                              DWORD length)
{
    return oid && oid->tag == 0x06 && oid->value_len == length &&
           auth_bytes_equal(oid->value, value, length);
}

static auth_hash_algorithm_t auth_algorithm_identifier(
    const auth_der_t *identifier)
{
    if (!identifier || identifier->tag != 0x30) return AUTH_HASH_UNKNOWN;
    const BYTE *p = identifier->value;
    const BYTE *end = p + identifier->value_len;
    auth_der_t oid;
    if (auth_der_expect(&p, end, 0x06, &oid) != 0)
        return AUTH_HASH_UNKNOWN;
    if (auth_der_oid_equal(&oid, oid_sha1, sizeof(oid_sha1)))
        return AUTH_HASH_SHA1;
    if (auth_der_oid_equal(&oid, oid_sha256, sizeof(oid_sha256)))
        return AUTH_HASH_SHA256;
    if (auth_der_oid_equal(&oid, oid_sha384, sizeof(oid_sha384)))
        return AUTH_HASH_SHA384;
    return AUTH_HASH_UNKNOWN;
}

static DWORD auth_hash_length(auth_hash_algorithm_t algorithm)
{
    if (algorithm == AUTH_HASH_SHA1) return 20;
    if (algorithm == AUTH_HASH_SHA256) return 32;
    if (algorithm == AUTH_HASH_SHA384) return 48;
    return 0;
}

static int auth_hash_init(auth_hash_ctx_t *ctx,
                          auth_hash_algorithm_t algorithm)
{
    ctx->algorithm = algorithm;
    if (algorithm == AUTH_HASH_SHA1) {
        sha1_init(&ctx->state.sha1);
        return 0;
    }
    if (algorithm == AUTH_HASH_SHA256) {
        sha256_init(&ctx->state.sha256);
        return 0;
    }
    return -1;
}

static void auth_hash_update(auth_hash_ctx_t *ctx, const BYTE *data,
                             DWORD length)
{
    if (ctx->algorithm == AUTH_HASH_SHA1)
        sha1_update(&ctx->state.sha1, data, length);
    else if (ctx->algorithm == AUTH_HASH_SHA256)
        sha256_update(&ctx->state.sha256, data, length);
}

static void auth_hash_final(auth_hash_ctx_t *ctx, BYTE *digest)
{
    if (ctx->algorithm == AUTH_HASH_SHA1)
        sha1_final(&ctx->state.sha1, digest);
    else if (ctx->algorithm == AUTH_HASH_SHA256)
        sha256_final(&ctx->state.sha256, digest);
}

static int auth_hash_memory(auth_hash_algorithm_t algorithm,
                            const BYTE *data, DWORD length, BYTE *digest)
{
    if (algorithm == AUTH_HASH_SHA384) {
        sha384(data, length, digest);
        return 0;
    }
    auth_hash_ctx_t ctx;
    if (auth_hash_init(&ctx, algorithm) != 0) return -1;
    auth_hash_update(&ctx, data, length);
    auth_hash_final(&ctx, digest);
    return 0;
}

static int auth_hash_signed_attributes(auth_hash_algorithm_t algorithm,
                                       const auth_der_t *attributes,
                                       BYTE *digest)
{
    if (!attributes || attributes->tag != 0xA0 || attributes->tlv_len < 2)
        return -1;

    if (algorithm == AUTH_HASH_SHA384) {
        if (attributes->tlv_len > AUTH_HASH_BUFFER) return -1;
        BYTE copy[AUTH_HASH_BUFFER];
        auth_copy(copy, attributes->tlv, attributes->tlv_len);
        copy[0] = 0x31;
        sha384(copy, attributes->tlv_len, digest);
        return 0;
    }

    auth_hash_ctx_t ctx;
    if (auth_hash_init(&ctx, algorithm) != 0) return -1;
    const BYTE set_tag = 0x31;
    auth_hash_update(&ctx, &set_tag, 1);
    auth_hash_update(&ctx, attributes->tlv + 1, attributes->tlv_len - 1);
    auth_hash_final(&ctx, digest);
    return 0;
}

static int auth_pe_info(HANDLE file, DWORD file_size, auth_pe_info_t *info)
{
    IMAGE_DOS_HEADER dos;
    auth_nt_prefix_t nt;
    union {
        IMAGE_OPTIONAL_HEADER32 header32;
        IMAGE_OPTIONAL_HEADER64 header64;
        BYTE bytes[sizeof(IMAGE_OPTIONAL_HEADER64)];
    } optional;

    if (file_size < sizeof(dos) || auth_read_at(file, 0, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        (DWORD)dos.e_lfanew > file_size - sizeof(nt) ||
        auth_read_at(file, (DWORD)dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.SizeOfOptionalHeader > sizeof(optional))
        return -1;

    DWORD optional_offset = (DWORD)dos.e_lfanew + sizeof(nt);
    if (optional_offset > file_size ||
        nt.FileHeader.SizeOfOptionalHeader > file_size - optional_offset ||
        auth_read_at(file, optional_offset, optional.bytes,
                     nt.FileHeader.SizeOfOptionalHeader))
        return -1;

    DWORD security_relative;
    IMAGE_DATA_DIRECTORY security;
    if (optional.header32.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        nt.FileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32) &&
        optional.header32.NumberOfRvaAndSizes >
            IMAGE_DIRECTORY_ENTRY_SECURITY) {
        security_relative =
            (DWORD)((BYTE *)&optional.header32
                        .DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] -
                    optional.bytes);
        security = optional.header32
                       .DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    } else if (optional.header64.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
               nt.FileHeader.SizeOfOptionalHeader >=
                   sizeof(IMAGE_OPTIONAL_HEADER64) &&
               optional.header64.NumberOfRvaAndSizes >
                   IMAGE_DIRECTORY_ENTRY_SECURITY) {
        security_relative =
            (DWORD)((BYTE *)&optional.header64
                        .DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] -
                    optional.bytes);
        security = optional.header64
                       .DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    } else {
        return -1;
    }

    info->file_size = file_size;
    info->checksum_offset = optional_offset + 64U;
    info->security_entry_offset = optional_offset + security_relative;
    info->certificate_offset = security.VirtualAddress;
    info->certificate_size = security.Size;

    if (info->checksum_offset + 4U > info->security_entry_offset ||
        info->security_entry_offset + sizeof(IMAGE_DATA_DIRECTORY) > file_size)
        return -1;
    if (!info->certificate_offset || info->certificate_size < 8U)
        return 1;
    if (info->certificate_offset < info->security_entry_offset + 8U ||
        info->certificate_offset > file_size ||
        info->certificate_size > file_size - info->certificate_offset)
        return -1;
    return 0;
}

static int auth_hash_file_range(HANDLE file, auth_hash_ctx_t *ctx,
                                DWORD offset, DWORD length)
{
    BYTE buffer[AUTH_HASH_BUFFER];
    while (length) {
        DWORD part = length > sizeof(buffer) ? sizeof(buffer) : length;
        if (auth_read_at(file, offset, buffer, part) != 0) return -1;
        auth_hash_update(ctx, buffer, part);
        offset += part;
        length -= part;
    }
    return 0;
}

static int auth_pe_digest(HANDLE file, const auth_pe_info_t *info,
                          auth_hash_algorithm_t algorithm, BYTE *digest)
{
    auth_hash_ctx_t ctx;
    if (auth_hash_init(&ctx, algorithm) != 0) return -1;

    DWORD after_checksum = info->checksum_offset + 4U;
    DWORD after_security = info->security_entry_offset + 8U;
    DWORD after_certificate =
        info->certificate_offset + info->certificate_size;
    if (auth_hash_file_range(file, &ctx, 0, info->checksum_offset) ||
        auth_hash_file_range(file, &ctx, after_checksum,
                             info->security_entry_offset - after_checksum) ||
        auth_hash_file_range(file, &ctx, after_security,
                             info->certificate_offset - after_security) ||
        (after_certificate < info->file_size &&
         auth_hash_file_range(file, &ctx, after_certificate,
                              info->file_size - after_certificate)))
        return -1;

    auth_hash_final(&ctx, digest);
    return 0;
}

static int auth_parse_signer(const auth_der_t *sequence, auth_signer_t *signer)
{
    if (!sequence || sequence->tag != 0x30 || !signer) return -1;
    *signer = (auth_signer_t){0};

    const BYTE *p = sequence->value;
    const BYTE *end = p + sequence->value_len;
    auth_der_t value;
    if (auth_der_expect(&p, end, 0x02, &value) ||
        auth_der_read(&p, end, &signer->sid) ||
        auth_der_expect(&p, end, 0x30, &value))
        return -1;
    signer->digest_algorithm = auth_algorithm_identifier(&value);
    if (signer->digest_algorithm == AUTH_HASH_UNKNOWN) return -1;

    if (p < end && *p == 0xA0 &&
        auth_der_expect(&p, end, 0xA0, &signer->signed_attributes))
        return -1;
    if (auth_der_expect(&p, end, 0x30, &signer->signature_algorithm) ||
        auth_der_expect(&p, end, 0x04, &value))
        return -1;
    signer->signature = value.value;
    signer->signature_len = value.value_len;

    if (p < end && *p == 0xA1 &&
        auth_der_expect(&p, end, 0xA1, &signer->unsigned_attributes))
        return -1;
    return p == end ? 0 : -1;
}

static int auth_parse_spc_content(const auth_der_t *content,
                                  auth_cms_t *cms)
{
    if (!content || content->tag != 0x30) return -1;
    const BYTE *p = content->value;
    const BYTE *end = p + content->value_len;
    auth_der_t data_type;
    auth_der_t digest_info;
    auth_der_t algorithm;
    auth_der_t digest;
    if (auth_der_expect(&p, end, 0x30, &data_type) ||
        auth_der_expect(&p, end, 0x30, &digest_info))
        return -1;

    p = digest_info.value;
    end = p + digest_info.value_len;
    if (auth_der_expect(&p, end, 0x30, &algorithm) ||
        auth_der_expect(&p, end, 0x04, &digest) || p != end)
        return -1;

    cms->file_digest_algorithm = auth_algorithm_identifier(&algorithm);
    cms->file_digest = digest.value;
    cms->file_digest_len = digest.value_len;
    return cms->file_digest_algorithm != AUTH_HASH_UNKNOWN &&
                   digest.value_len ==
                       auth_hash_length(cms->file_digest_algorithm)
               ? 0
               : -1;
}

static int auth_parse_certificates(const auth_der_t *wrapper, auth_cms_t *cms)
{
    if (!wrapper || wrapper->tag != 0xA0) return -1;
    const BYTE *p = wrapper->value;
    const BYTE *end = p + wrapper->value_len;
    while (p < end) {
        auth_der_t certificate;
        if (auth_der_read(&p, end, &certificate)) return -1;
        if (certificate.tag != 0x30) continue;
        if (cms->cert_count >= AUTH_MAX_CERTS) return -1;
        cms->certs[cms->cert_count].der = certificate.tlv;
        cms->certs[cms->cert_count].der_len = certificate.tlv_len;
        cms->cert_count++;
    }
    return cms->cert_count ? 0 : -1;
}

static int auth_parse_cms(const BYTE *der, DWORD der_len, auth_cms_t *cms)
{
    *cms = (auth_cms_t){0};
    const BYTE *p = der;
    const BYTE *end = der + der_len;
    auth_der_t content_info;
    auth_der_t content_type;
    auth_der_t explicit_content;
    auth_der_t signed_data;
    if (auth_der_expect(&p, end, 0x30, &content_info)) return -1;

    p = content_info.value;
    end = p + content_info.value_len;
    if (auth_der_expect(&p, end, 0x06, &content_type) ||
        !auth_der_oid_equal(&content_type, oid_pkcs7_signed_data,
                            sizeof(oid_pkcs7_signed_data)) ||
        auth_der_expect(&p, end, 0xA0, &explicit_content) || p != end)
        return -1;

    p = explicit_content.value;
    end = p + explicit_content.value_len;
    if (auth_der_expect(&p, end, 0x30, &signed_data) || p != end)
        return -1;

    p = signed_data.value;
    end = p + signed_data.value_len;
    auth_der_t version;
    auth_der_t digest_algorithms;
    auth_der_t encapsulated;
    if (auth_der_expect(&p, end, 0x02, &version) ||
        auth_der_expect(&p, end, 0x31, &digest_algorithms) ||
        auth_der_expect(&p, end, 0x30, &encapsulated))
        return -1;

    const BYTE *ep = encapsulated.value;
    const BYTE *eend = ep + encapsulated.value_len;
    auth_der_t encapsulated_type;
    auth_der_t encapsulated_value;
    if (auth_der_expect(&ep, eend, 0x06, &encapsulated_type) ||
        !auth_der_oid_equal(&encapsulated_type, oid_spc_indirect_data,
                            sizeof(oid_spc_indirect_data)) ||
        auth_der_expect(&ep, eend, 0xA0, &explicit_content) || ep != eend)
        return -1;

    const BYTE *cp = explicit_content.value;
    const BYTE *cend = cp + explicit_content.value_len;
    if (auth_der_read(&cp, cend, &encapsulated_value) || cp != cend)
        return -1;

    auth_der_t spc_content;
    if (encapsulated_value.tag == 0x30) {
        spc_content = encapsulated_value;
        /* Legacy Authenticode signs the SEQUENCE contents, not its TLV. */
        cms->content = spc_content.value;
        cms->content_len = spc_content.value_len;
    } else if (encapsulated_value.tag == 0x04) {
        cp = encapsulated_value.value;
        cend = cp + encapsulated_value.value_len;
        if (auth_der_expect(&cp, cend, 0x30, &spc_content) || cp != cend)
            return -1;
        cms->content = encapsulated_value.value;
        cms->content_len = encapsulated_value.value_len;
    } else {
        return -1;
    }
    if (auth_parse_spc_content(&spc_content, cms)) return -1;

    auth_der_t certificates;
    if (p >= end || *p != 0xA0 ||
        auth_der_expect(&p, end, 0xA0, &certificates) ||
        auth_parse_certificates(&certificates, cms))
        return -1;
    if (p < end && *p == 0xA1) {
        auth_der_t crls;
        if (auth_der_expect(&p, end, 0xA1, &crls)) return -1;
    }

    auth_der_t signer_infos;
    auth_der_t signer_sequence;
    if (auth_der_expect(&p, end, 0x31, &signer_infos) || p != end)
        return -1;
    const BYTE *sp = signer_infos.value;
    const BYTE *send = sp + signer_infos.value_len;
    if (auth_der_expect(&sp, send, 0x30, &signer_sequence) ||
        auth_parse_signer(&signer_sequence, &cms->signer))
        return -1;
    return 0;
}

static int auth_find_attribute(const auth_der_t *attributes,
                               const BYTE *oid_value, DWORD oid_len,
                               auth_der_t *result)
{
    if (!attributes ||
        (attributes->tag != 0xA0 && attributes->tag != 0xA1))
        return -1;
    const BYTE *p = attributes->value;
    const BYTE *end = p + attributes->value_len;
    while (p < end) {
        auth_der_t attribute;
        if (auth_der_expect(&p, end, 0x30, &attribute)) return -1;
        const BYTE *ap = attribute.value;
        const BYTE *aend = ap + attribute.value_len;
        auth_der_t oid;
        auth_der_t values;
        if (auth_der_expect(&ap, aend, 0x06, &oid) ||
            auth_der_expect(&ap, aend, 0x31, &values) || ap != aend)
            return -1;
        if (!auth_der_oid_equal(&oid, oid_value, oid_len)) continue;
        const BYTE *vp = values.value;
        const BYTE *vend = vp + values.value_len;
        return auth_der_read(&vp, vend, result);
    }
    return 1;
}

static void auth_strip_integer_zero(const BYTE **value, DWORD *length)
{
    while (*length > 1U && **value == 0) {
        (*value)++;
        (*length)--;
    }
}

static int auth_signer_matches_cert(const auth_signer_t *signer,
                                    const auth_cert_t *certificate)
{
    if (signer->sid.tag == 0x30) {
        const BYTE *p = signer->sid.value;
        const BYTE *end = p + signer->sid.value_len;
        auth_der_t issuer;
        auth_der_t serial;
        if (auth_der_read(&p, end, &issuer) || issuer.tag != 0x30 ||
            auth_der_expect(&p, end, 0x02, &serial) || p != end)
            return 0;

        const BYTE *cert_issuer;
        DWORD cert_issuer_len;
        const BYTE *cert_serial;
        DWORD cert_serial_len;
        if (x509_get_issuer_name_der(certificate->der,
                                     certificate->der_len, &cert_issuer,
                                     &cert_issuer_len) ||
            x509_get_serial_number(certificate->der, certificate->der_len,
                                   &cert_serial, &cert_serial_len))
            return 0;
        const BYTE *signer_serial = serial.value;
        DWORD signer_serial_len = serial.value_len;
        auth_strip_integer_zero(&signer_serial, &signer_serial_len);
        return cert_issuer_len == issuer.tlv_len &&
               auth_bytes_equal(cert_issuer, issuer.tlv, cert_issuer_len) &&
               cert_serial_len == signer_serial_len &&
               auth_bytes_equal(cert_serial, signer_serial, cert_serial_len);
    }

    if (signer->sid.tag == 0x80) {
        const BYTE *ski;
        DWORD ski_len;
        return x509_get_subject_key_id(certificate->der,
                                       certificate->der_len, &ski,
                                       &ski_len) == 0 &&
               ski_len == signer->sid.value_len &&
               auth_bytes_equal(ski, signer->sid.value, ski_len);
    }
    return 0;
}

static int auth_find_signer_cert(const auth_signer_t *signer,
                                 const auth_cert_t *certificates,
                                 DWORD certificate_count)
{
    for (DWORD i = 0; i < certificate_count; i++)
        if (auth_signer_matches_cert(signer, &certificates[i])) return (int)i;
    return -1;
}

static int auth_signature_algorithm(const auth_der_t *identifier,
                                    int *is_rsa, int *is_ecdsa)
{
    *is_rsa = 0;
    *is_ecdsa = 0;
    if (!identifier || identifier->tag != 0x30) return -1;
    const BYTE *p = identifier->value;
    const BYTE *end = p + identifier->value_len;
    auth_der_t oid;
    if (auth_der_expect(&p, end, 0x06, &oid)) return -1;
    if (auth_der_oid_equal(&oid, oid_rsa_encryption,
                           sizeof(oid_rsa_encryption)) ||
        auth_der_oid_equal(&oid, oid_sha256_rsa, sizeof(oid_sha256_rsa)) ||
        auth_der_oid_equal(&oid, oid_sha384_rsa, sizeof(oid_sha384_rsa))) {
        *is_rsa = 1;
        return 0;
    }
    if (auth_der_oid_equal(&oid, oid_ecdsa_sha256,
                           sizeof(oid_ecdsa_sha256)) ||
        auth_der_oid_equal(&oid, oid_ecdsa_sha384,
                           sizeof(oid_ecdsa_sha384))) {
        *is_ecdsa = 1;
        return 0;
    }
    return -1;
}

static int auth_verify_crypto_signature(const auth_signer_t *signer,
                                        const auth_cert_t *certificate,
                                        const BYTE *digest)
{
    int is_rsa;
    int is_ecdsa;
    if (auth_signature_algorithm(&signer->signature_algorithm,
                                 &is_rsa, &is_ecdsa))
        return -1;

    if (is_rsa) {
        const BYTE *modulus;
        const BYTE *exponent;
        DWORD modulus_len;
        DWORD exponent_len;
        if (x509_extract_rsa_pubkey(certificate->der, certificate->der_len,
                                    &modulus, &modulus_len, &exponent,
                                    &exponent_len))
            return -1;
        if (signer->digest_algorithm == AUTH_HASH_SHA256)
            return rsa_pkcs1_v15_sha256_verify(
                signer->signature, signer->signature_len, modulus,
                modulus_len, exponent, exponent_len, digest);
        if (signer->digest_algorithm == AUTH_HASH_SHA384)
            return rsa_pkcs1_v15_sha384_verify(
                signer->signature, signer->signature_len, modulus,
                modulus_len, exponent, exponent_len, digest);
        return -1;
    }

    if (is_ecdsa) {
        BYTE x256[32], y256[32];
        if (x509_extract_ec_pubkey(certificate->der, certificate->der_len,
                                   x256, y256) == 0) {
            const BYTE *hash = digest;
            return ecdsa_p256_verify(x256, y256, hash, signer->signature,
                                     signer->signature_len);
        }
        BYTE x384[48], y384[48], hash384[48];
        if (x509_extract_ec_pubkey_p384(certificate->der,
                                        certificate->der_len,
                                        x384, y384))
            return -1;
        if (signer->digest_algorithm == AUTH_HASH_SHA384) {
            auth_copy(hash384, digest, 48);
        } else if (signer->digest_algorithm == AUTH_HASH_SHA256) {
            for (int i = 0; i < 16; i++) hash384[i] = 0;
            auth_copy(hash384 + 16, digest, 32);
        } else {
            return -1;
        }
        return ecdsa_p384_verify(x384, y384, hash384, signer->signature,
                                 signer->signature_len);
    }
    return -1;
}

static int auth_verify_signer(const auth_signer_t *signer,
                              const auth_cert_t *certificates,
                              DWORD certificate_count, const BYTE *content,
                              DWORD content_len, int require_content_type,
                              int *certificate_index)
{
    int signer_index = auth_find_signer_cert(signer, certificates,
                                             certificate_count);
    if (signer_index < 0) return -1;

    BYTE content_digest[48];
    DWORD digest_len = auth_hash_length(signer->digest_algorithm);
    if (!digest_len || auth_hash_memory(signer->digest_algorithm, content,
                                        content_len, content_digest))
        return -1;

    BYTE signed_digest[48];
    if (signer->signed_attributes.tlv) {
        auth_der_t message_digest;
        if (auth_find_attribute(&signer->signed_attributes,
                                oid_message_digest,
                                sizeof(oid_message_digest),
                                &message_digest) != 0 ||
            message_digest.tag != 0x04 ||
            message_digest.value_len != digest_len ||
            !auth_bytes_equal(message_digest.value, content_digest,
                              digest_len))
            return -1;

        if (require_content_type) {
            auth_der_t content_type;
            if (auth_find_attribute(&signer->signed_attributes,
                                    oid_content_type,
                                    sizeof(oid_content_type),
                                    &content_type) != 0 ||
                !auth_der_oid_equal(&content_type, oid_spc_indirect_data,
                                    sizeof(oid_spc_indirect_data)))
                return -1;
        }
        if (auth_hash_signed_attributes(signer->digest_algorithm,
                                        &signer->signed_attributes,
                                        signed_digest))
            return -1;
    } else {
        auth_copy(signed_digest, content_digest, digest_len);
    }

    if (auth_verify_crypto_signature(signer, &certificates[signer_index],
                                     signed_digest))
        return -1;
    if (certificate_index) *certificate_index = signer_index;
    return 0;
}

static int auth_chain_revocation_ok(const BYTE *const *chain,
                                    const DWORD *chain_lens,
                                    DWORD chain_count, int trusted,
                                    uint32_t validation_time,
                                    DWORD revocation_checks,
                                    DWORD provider_flags)
{
    int check_end =
        (provider_flags & AUTH_WTD_REVOCATION_CHECK_END_CERT) != 0;
    int check_chain = revocation_checks == AUTH_WTD_REVOKE_WHOLECHAIN ||
        (provider_flags & AUTH_WTD_REVOCATION_CHECK_CHAIN) != 0;
    int check_excluding_root =
        (provider_flags & AUTH_WTD_REVOCATION_CHECK_CHAIN_EX_ROOT) != 0;
    if (!check_end && !check_chain && !check_excluding_root) return 1;

    DWORD count = check_end ? 1U : chain_count;
    if (check_excluding_root && trusted && count) count--;
    int allow_network =
        !(provider_flags & AUTH_WTD_CACHE_ONLY_URL_RETRIEVAL);
    for (DWORD i = 0; i < count; i++) {
        DWORD issuer = i + 1U < chain_count ? i + 1U : i;
        int attempted = 0;
        int status = certmgr_revocation_resolve(
            chain[i], chain_lens[i], chain[issuer], chain_lens[issuer],
            validation_time, allow_network, &attempted);
        if (status != CERTMGR_REVOCATION_GOOD) {
            serial_puts(status == CERTMGR_REVOCATION_REVOKED
                            ? "[AUTH] certificate revoked\n"
                            : "[AUTH] revocation status unavailable\n");
            return 0;
        }
    }
    return 1;
}

static int auth_chain_trusted(const auth_cert_t *leaf,
                              const auth_cert_t *certificates,
                              DWORD certificate_count, int leaf_index,
                              const char *usage_oid, uint32_t validation_time,
                              DWORD revocation_checks, DWORD provider_flags)
{
    const BYTE *chain[AUTH_MAX_CHAIN];
    DWORD chain_lens[AUTH_MAX_CHAIN];
    BYTE used[AUTH_MAX_CERTS] = {0};
    DWORD count = 1;
    chain[0] = leaf->der;
    chain_lens[0] = leaf->der_len;
    if (leaf_index >= 0 && (DWORD)leaf_index < certificate_count)
        used[leaf_index] = 1;

    x509_v3_t leaf_extensions;
    if (x509_parse_v3(leaf->der, leaf->der_len, &leaf_extensions) != 0 ||
        (leaf_extensions.has_ku &&
         !(leaf_extensions.key_usage_flags & X509_KU_DIGITAL_SIGNATURE)) ||
        x509_has_extended_key_usage(leaf->der, leaf->der_len,
                                    usage_oid) != 1) {
        serial_puts("[AUTH] signer certificate usage rejected\n");
        return 0;
    }

    const char *usages[1] = { usage_oid };
    int root_usage_allowed = 0;
    int trusted = certmgr_check_trust_anchor(
        chain[0], chain_lens[0], usages, 1, 1, &root_usage_allowed);
    while (!trusted && count < AUTH_MAX_CHAIN) {
        int found = -1;
        for (DWORD i = 0; i < certificate_count; i++) {
            if (used[i]) continue;
            if (x509_issuer_matches_subject(
                    chain[count - 1], chain_lens[count - 1],
                    certificates[i].der, certificates[i].der_len) != 1)
                continue;
            if (x509_verify_chain_link(chain[count - 1],
                                       chain_lens[count - 1],
                                       certificates[i].der,
                                       certificates[i].der_len) == 0) {
                found = (int)i;
                break;
            }
        }
        if (found >= 0) {
            used[found] = 1;
            chain[count] = certificates[found].der;
            chain_lens[count] = certificates[found].der_len;
            count++;
            trusted = certmgr_check_trust_anchor(
                chain[count - 1], chain_lens[count - 1], usages, 1, 1,
                &root_usage_allowed);
            continue;
        }

        certmgr_issuer_iter_t roots;
        const BYTE *root;
        DWORD root_len;
        int root_found = 0;
        if (certmgr_issuer_iter_begin(&roots, chain[count - 1],
                                      chain_lens[count - 1]) == 0) {
            while (certmgr_issuer_iter_next(&roots, &root, &root_len)) {
                if (x509_verify_chain_link(chain[count - 1],
                                           chain_lens[count - 1], root,
                                           root_len) != 0)
                    continue;
                chain[count] = root;
                chain_lens[count] = root_len;
                count++;
                trusted = certmgr_check_trust_anchor(
                    root, root_len, usages, 1, 1, &root_usage_allowed);
                root_found = 1;
                break;
            }
        }
        if (!root_found) break;
    }

    if (!trusted || !root_usage_allowed) {
        serial_puts("[AUTH] signer chain has no trusted root\n");
        return 0;
    }

    for (DWORD i = 0; i < count; i++) {
        if (validation_time &&
            x509_check_validity(chain[i], chain_lens[i],
                                validation_time) != 0) {
            serial_puts("[AUTH] certificate outside validity window\n");
            return 0;
        }
        if (i + 1U < count &&
            x509_allows_extended_key_usage(chain[i], chain_lens[i],
                                            usage_oid) != 1) {
            serial_puts("[AUTH] certificate chain EKU rejected\n");
            return 0;
        }
    }

    DWORD constrained_count = count ? count - 1U : 0;
    if (constrained_count > 1U &&
        x509_check_chain_constraints(chain, chain_lens,
                                     constrained_count) != 0) {
        serial_puts("[AUTH] certificate chain constraints rejected\n");
        return 0;
    }
    return auth_chain_revocation_ok(
        chain, chain_lens, count, trusted, validation_time,
        revocation_checks, provider_flags);
}

static int auth_counter_signature(const auth_cms_t *cms,
                                  DWORD revocation_checks,
                                  DWORD provider_flags,
                                  uint32_t *signing_time)
{
    *signing_time = 0;
    if (!cms->signer.unsigned_attributes.tlv) return 1;

    auth_der_t counter_value;
    int found = auth_find_attribute(
        &cms->signer.unsigned_attributes, oid_counter_signature,
        sizeof(oid_counter_signature), &counter_value);
    if (found != 0) return found > 0 ? 1 : -1;
    if (counter_value.tag != 0x30) return -1;

    auth_signer_t counter;
    int counter_cert_index = -1;
    if (auth_parse_signer(&counter_value, &counter) ||
        auth_verify_signer(&counter, cms->certs, cms->cert_count,
                           cms->signer.signature,
                           cms->signer.signature_len, 0,
                           &counter_cert_index)) {
        serial_puts("[AUTH] timestamp countersignature invalid\n");
        return -1;
    }

    auth_der_t time_value;
    if (!counter.signed_attributes.tlv ||
        auth_find_attribute(&counter.signed_attributes, oid_signing_time,
                            sizeof(oid_signing_time), &time_value) != 0 ||
        x509_parse_time_der(time_value.tlv, time_value.tlv_len,
                            signing_time, NULL) != 0 ||
        !*signing_time) {
        serial_puts("[AUTH] timestamp signingTime missing\n");
        return -1;
    }

    static const char timestamp_usage[] = "1.3.6.1.5.5.7.3.8";
    if (!auth_chain_trusted(&cms->certs[counter_cert_index], cms->certs,
                            cms->cert_count, counter_cert_index,
                            timestamp_usage, *signing_time,
                            revocation_checks, provider_flags)) {
        serial_puts("[AUTH] timestamp chain rejected\n");
        return -1;
    }
    serial_puts("[AUTH] trusted timestamp at ");
    serial_putdec(*signing_time);
    serial_puts("\n");
    return 0;
}

static authenticode_result_t auth_verify_pkcs7(
    HANDLE file, const auth_pe_info_t *pe, const BYTE *der, DWORD der_len,
    DWORD revocation_checks, DWORD provider_flags)
{
    auth_cms_t cms;
    if (auth_parse_cms(der, der_len, &cms)) {
        serial_puts("[AUTH] malformed PKCS#7 SignedData\n");
        return AUTHENTICODE_VERIFY_UNSUPPORTED;
    }

    BYTE file_digest[48];
    if (cms.file_digest_algorithm == AUTH_HASH_SHA384) {
        serial_puts("[AUTH] SHA-384 PE digest streaming is unsupported\n");
        return AUTHENTICODE_VERIFY_UNSUPPORTED;
    }
    if (auth_pe_digest(file, pe, cms.file_digest_algorithm, file_digest))
        return AUTHENTICODE_VERIFY_IO_ERROR;
    if (!auth_bytes_equal(file_digest, cms.file_digest,
                          cms.file_digest_len)) {
        serial_puts("[AUTH] PE digest mismatch\n");
        return AUTHENTICODE_VERIFY_BAD_DIGEST;
    }
    serial_puts("[AUTH] PE digest verified\n");

    int signer_index = -1;
    if (auth_verify_signer(&cms.signer, cms.certs, cms.cert_count,
                           cms.content, cms.content_len, 1,
                           &signer_index)) {
        serial_puts("[AUTH] CMS signer signature invalid\n");
        return AUTHENTICODE_VERIFY_BAD_SIGNATURE;
    }
    serial_puts("[AUTH] CMS signer signature verified\n");

    uint32_t timestamp = 0;
    int timestamp_status = auth_counter_signature(
        &cms, revocation_checks, provider_flags, &timestamp);
    uint32_t validation_time = timestamp_status == 0 ? timestamp
                                                     : ntp_get_utc();
    static const char code_signing_usage[] = "1.3.6.1.5.5.7.3.3";
    if (!auth_chain_trusted(&cms.certs[signer_index], cms.certs,
                            cms.cert_count, signer_index,
                            code_signing_usage, validation_time,
                            revocation_checks, provider_flags))
        return AUTHENTICODE_VERIFY_UNTRUSTED;

    serial_puts("[AUTH] Authenticode chain trusted\n");
    return AUTHENTICODE_VERIFY_OK;
}

authenticode_result_t authenticode_verify_file(PCWSTR path,
                                                HANDLE supplied_file,
                                                DWORD revocation_checks,
                                                DWORD provider_flags)
{
    HANDLE file = supplied_file;
    int close_file = 0;
    if (!file || file == INVALID_HANDLE_VALUE) {
        if (!path) return AUTHENTICODE_VERIFY_IO_ERROR;
        file = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           NULL, AUTH_OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
            return AUTHENTICODE_VERIFY_IO_ERROR;
        close_file = 1;
    }

    DWORD high = 0;
    DWORD file_size = GetFileSize(file, &high);
    if (high || file_size == 0xFFFFFFFFU || file_size > 0x7FFFFFFFU) {
        if (close_file) CloseHandle(file);
        return AUTHENTICODE_VERIFY_IO_ERROR;
    }

    auth_pe_info_t pe;
    int pe_status = auth_pe_info(file, file_size, &pe);
    if (pe_status != 0) {
        if (close_file) CloseHandle(file);
        return pe_status > 0 ? AUTHENTICODE_VERIFY_NO_SIGNATURE
                             : AUTHENTICODE_VERIFY_IO_ERROR;
    }
    if (pe.certificate_size > AUTH_MAX_PKCS7) {
        if (close_file) CloseHandle(file);
        return AUTHENTICODE_VERIFY_UNSUPPORTED;
    }

    BYTE *certificates = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
                                           pe.certificate_size);
    if (!certificates ||
        auth_read_at(file, pe.certificate_offset, certificates,
                     pe.certificate_size)) {
        if (certificates) HeapFree(GetProcessHeap(), 0, certificates);
        if (close_file) CloseHandle(file);
        return AUTHENTICODE_VERIFY_IO_ERROR;
    }

    authenticode_result_t result = AUTHENTICODE_VERIFY_NO_SIGNATURE;
    DWORD offset = 0;
    while (offset + 8U <= pe.certificate_size) {
        DWORD certificate_length = auth_read_u32(certificates + offset);
        WORD certificate_type = (WORD)(certificates[offset + 6] |
                                       (certificates[offset + 7] << 8));
        if (certificate_length < 8U ||
            certificate_length > pe.certificate_size - offset)
            break;
        if (certificate_type == AUTH_WIN_CERT_TYPE_PKCS_SIGNED) {
            result = auth_verify_pkcs7(
                file, &pe, certificates + offset + 8U,
                certificate_length - 8U, revocation_checks,
                provider_flags);
            if (result == AUTHENTICODE_VERIFY_OK) break;
        }
        DWORD aligned = (certificate_length + 7U) & ~7U;
        if (aligned < certificate_length || aligned > pe.certificate_size - offset)
            break;
        offset += aligned;
    }

    HeapFree(GetProcessHeap(), 0, certificates);
    if (close_file) CloseHandle(file);
    return result;
}
