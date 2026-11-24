/* Minimal crypt32 memory-store and certificate-chain support for PE apps. */

#include "crypt32_shim.h"
#include "authenticode.h"
#include "kernel32_shim.h"
#include "advapi32_shim.h"
#include "../include/paging.h"
#include "../kernel/certmgr.h"
#include "../kernel/crypto.h"
#include "../kernel/x509.h"

extern void *mem_alloc_pages(uint64_t count);
extern void mem_free_pages(void *addr, uint64_t count);
extern uint32_t ntp_get_utc(void);
extern void random_get_bytes(void *buf, uint32_t len);
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

static void *cert_alloc_pages(uint64_t pages)
{
    void *phys = mem_alloc_pages(pages);
    return (!phys || g_compat32_mode) ? phys : PHYS_TO_VIRT(phys);
}

static void cert_free_pages(void *addr, uint64_t pages)
{
    mem_free_pages((void *)kvirt_to_phys(addr), pages);
}

#define CERT_STORE_MAGIC   0x43535452U
#define CERT_OBJECT_MAGIC  0x43455254U
#define CERT_CHAIN_MAGIC   0x43484E31U
#define CERT_STORE_MAX     8
#define CERT_CHAIN_MAX     AUTHENTICODE_CHAIN_MAX

#define X509_ASN_ENCODING  0x00000001U
#define CERT_STORE_PROV_MEMORY 2U
#define CERT_STORE_ADD_ALWAYS  4U

#define CERT_TRUST_IS_NOT_TIME_VALID          0x00000001U
#define CERT_TRUST_IS_REVOKED                 0x00000004U
#define CERT_TRUST_IS_NOT_SIGNATURE_VALID     0x00000008U
#define CERT_TRUST_IS_NOT_VALID_FOR_USAGE      0x00000010U
#define CERT_TRUST_IS_UNTRUSTED_ROOT          0x00000020U
#define CERT_TRUST_REVOCATION_STATUS_UNKNOWN  0x00000040U
#define CERT_TRUST_INVALID_BASIC_CONSTRAINTS  0x00000400U
#define CERT_TRUST_IS_PARTIAL_CHAIN            0x00010000U
#define CERT_TRUST_IS_OFFLINE_REVOCATION      0x01000000U

#define CERT_CHAIN_REVOCATION_CHECK_END_CERT           0x10000000U
#define CERT_CHAIN_REVOCATION_CHECK_CHAIN              0x20000000U
#define CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT 0x40000000U
#define CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY         0x80000000U
#define CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL             0x00000004U

#define CRYPT_E_NOT_FOUND       0x80092004U
#define TRUST_E_PROVIDER_UNKNOWN    ((LONG)0x800B0001U)
#define TRUST_E_ACTION_UNKNOWN      ((LONG)0x800B0002U)
#define TRUST_E_SUBJECT_FORM_UNKNOWN ((LONG)0x800B0003U)
#define TRUST_E_SUBJECT_NOT_TRUSTED ((LONG)0x800B0004U)
#define TRUST_E_NOSIGNATURE         ((LONG)0x800B0100U)
#define CERT_USAGE_MAX          16U
#define CERT_USAGE_OID_MAX      128U

#define WTD_CHOICE_FILE         1U
#define WTD_STATEACTION_CLOSE   2U

typedef struct __attribute__((packed)) {
    DWORD cbStruct;
    uint32_t pPolicyCallbackData;
    uint32_t pSIPClientData;
    DWORD dwUIChoice;
    DWORD fdwRevocationChecks;
    DWORD dwUnionChoice;
    uint32_t pFile;
    DWORD dwStateAction;
    uint32_t hWVTStateData;
    uint32_t pwszURLReference;
    DWORD dwProvFlags;
    DWORD dwUIContext;
} wintrust_data32_t;

typedef struct {
    DWORD cbStruct;
    PVOID pPolicyCallbackData;
    PVOID pSIPClientData;
    DWORD dwUIChoice;
    DWORD fdwRevocationChecks;
    DWORD dwUnionChoice;
    PVOID pFile;
    DWORD dwStateAction;
    HANDLE hWVTStateData;
    PCWSTR pwszURLReference;
    DWORD dwProvFlags;
    DWORD dwUIContext;
} wintrust_data64_t;

typedef struct __attribute__((packed)) {
    DWORD cbStruct;
    uint32_t pcwszFilePath;
    uint32_t hFile;
    uint32_t pgKnownSubject;
} wintrust_file_info32_t;

typedef struct {
    DWORD cbStruct;
    PCWSTR pcwszFilePath;
    HANDLE hFile;
    const GUID *pgKnownSubject;
} wintrust_file_info64_t;

typedef struct {
    uint32_t dwCertEncodingType;
    uint32_t pbCertEncoded;
    uint32_t cbCertEncoded;
    uint32_t pCertInfo;
    uint32_t hCertStore;
} cert_context32_t;

typedef struct {
    uint32_t dwCertEncodingType;
    uint32_t padding0;
    uint64_t pbCertEncoded;
    uint32_t cbCertEncoded;
    uint32_t padding1;
    uint64_t pCertInfo;
    uint64_t hCertStore;
} cert_context64_t;

typedef union {
    cert_context32_t v32;
    cert_context64_t v64;
} cert_context_t;

typedef struct {
    cert_context_t context;
    uint32_t magic;
    uint32_t pages;
    volatile uint32_t refs;
    uint8_t der[];
} cert_object_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
    cert_object_t *certs[CERT_STORE_MAX];
} cert_store_t;

typedef struct __attribute__((packed)) {
    uint32_t cbSize;
    uint32_t dwType;
    uint32_t cUsageIdentifier;
    uint32_t rgpszUsageIdentifier;
} cert_chain_para32_min_t;

typedef struct {
    uint32_t cbSize;
    uint32_t padding0;
    uint32_t dwType;
    uint32_t padding1;
    uint32_t cUsageIdentifier;
    uint32_t padding2;
    uint64_t rgpszUsageIdentifier;
} cert_chain_para64_min_t;

typedef struct {
    const char *oids[CERT_USAGE_MAX];
    uint32_t count;
    int match_all;
} cert_usage_request_t;

typedef struct {
    uint32_t dwLowDateTime;
    uint32_t dwHighDateTime;
} cert_filetime_t;

typedef struct {
    uint32_t dwErrorStatus;
    uint32_t dwInfoStatus;
} cert_trust_status_t;

typedef struct __attribute__((packed)) {
    uint32_t cbSize;
    cert_trust_status_t TrustStatus;
    uint32_t cChain;
    uint32_t rgpChain;
    uint32_t cLowerQualityChain;
    uint32_t rgpLowerQualityChain;
    uint32_t fHasRevocationFreshnessTime;
    uint32_t dwRevocationFreshnessTime;
    uint32_t dwCreateFlags;
    uint8_t ChainId[16];
} cert_chain_context32_t;

typedef struct {
    uint32_t cbSize;
    cert_trust_status_t TrustStatus;
    uint32_t cChain;
    uint64_t rgpChain;
    uint32_t cLowerQualityChain;
    uint32_t padding0;
    uint64_t rgpLowerQualityChain;
    uint32_t fHasRevocationFreshnessTime;
    uint32_t dwRevocationFreshnessTime;
    uint32_t dwCreateFlags;
    uint8_t ChainId[16];
} cert_chain_context64_t;

typedef struct __attribute__((packed)) {
    uint32_t cbSize;
    cert_trust_status_t TrustStatus;
    uint32_t cElement;
    uint32_t rgpElement;
    uint32_t pTrustListInfo;
    uint32_t fHasRevocationFreshnessTime;
    uint32_t dwRevocationFreshnessTime;
} cert_simple_chain32_t;

typedef struct {
    uint32_t cbSize;
    cert_trust_status_t TrustStatus;
    uint32_t cElement;
    uint64_t rgpElement;
    uint64_t pTrustListInfo;
    uint32_t fHasRevocationFreshnessTime;
    uint32_t dwRevocationFreshnessTime;
} cert_simple_chain64_t;

typedef union {
    cert_chain_context32_t v32;
    cert_chain_context64_t v64;
} cert_chain_context_t;

typedef union {
    cert_simple_chain32_t v32;
    cert_simple_chain64_t v64;
} cert_simple_chain_t;

typedef struct __attribute__((packed)) {
    uint32_t cbSize;
    uint32_t pCertContext;
    cert_trust_status_t TrustStatus;
    uint32_t pRevocationInfo;
    uint32_t pIssuanceUsage;
    uint32_t pApplicationUsage;
    uint32_t pwszExtendedErrorInfo;
} cert_chain_element32_t;

typedef struct {
    uint32_t cbSize;
    uint32_t padding0;
    uint64_t pCertContext;
    cert_trust_status_t TrustStatus;
    uint64_t pRevocationInfo;
    uint64_t pIssuanceUsage;
    uint64_t pApplicationUsage;
    uint64_t pwszExtendedErrorInfo;
} cert_chain_element64_t;

typedef union {
    cert_chain_element32_t v32;
    cert_chain_element64_t v64;
} cert_chain_element_t;

typedef union {
    uint32_t v32;
    uint64_t v64;
} cert_pointer_t;

typedef struct {
    cert_chain_context_t context;
    uint32_t magic;
    uint32_t is_32bit;
    cert_pointer_t chain_ptr;
    cert_simple_chain_t simple;
    uint32_t element_count;
    uint32_t padding;
    cert_object_t *element_certs[CERT_CHAIN_MAX];
    uint32_t element_ptrs32[CERT_CHAIN_MAX];
    uint64_t element_ptrs64[CERT_CHAIN_MAX];
    cert_chain_element_t elements[CERT_CHAIN_MAX];
} cert_chain_object_t;

_Static_assert(sizeof(cert_context32_t) == 20, "CERT_CONTEXT32 layout");
_Static_assert(sizeof(cert_context64_t) == 40, "CERT_CONTEXT64 layout");
_Static_assert(sizeof(cert_chain_context32_t) == 56, "CERT_CHAIN_CONTEXT32 layout");
_Static_assert(sizeof(cert_chain_context64_t) == 72, "CERT_CHAIN_CONTEXT64 layout");
_Static_assert(sizeof(cert_simple_chain32_t) == 32, "CERT_SIMPLE_CHAIN32 layout");
_Static_assert(sizeof(cert_simple_chain64_t) == 40, "CERT_SIMPLE_CHAIN64 layout");
_Static_assert(sizeof(cert_chain_element32_t) == 32, "CERT_CHAIN_ELEMENT32 layout");
_Static_assert(sizeof(cert_chain_element64_t) == 56, "CERT_CHAIN_ELEMENT64 layout");
_Static_assert(sizeof(cert_chain_para32_min_t) == 16, "CERT_CHAIN_PARA32 minimum layout");
_Static_assert(sizeof(cert_chain_para64_min_t) == 32, "CERT_CHAIN_PARA64 minimum layout");
_Static_assert(sizeof(cert_chain_object_t) <= 4096, "chain object fits one page");

static PVOID cert_public_context(cert_object_t *obj)
{
    return &obj->context;
}

static uint32_t cert_encoded_len(const cert_object_t *obj)
{
    return g_compat32_mode ? obj->context.v32.cbCertEncoded
                           : obj->context.v64.cbCertEncoded;
}

static void cert_write_pointer(PVOID output, PVOID value)
{
    if (g_compat32_mode)
        *(uint32_t *)output = (uint32_t)(uintptr_t)value;
    else
        *(PVOID *)output = value;
}

static cert_object_t *cert_object(PVOID context)
{
    uintptr_t p = (uintptr_t)context;
    if (g_compat32_mode ? (p < 0x10000 || p >= 0x80000000ULL)
                        : (p < KERNEL_VBASE)) return NULL;
    cert_object_t *obj = (cert_object_t *)context;
    return obj->magic == CERT_OBJECT_MAGIC ? obj : NULL;
}

static cert_store_t *cert_store(PVOID handle)
{
    uintptr_t p = (uintptr_t)handle;
    if (g_compat32_mode ? (p < 0x10000 || p >= 0x80000000ULL)
                        : (p < KERNEL_VBASE)) return NULL;
    cert_store_t *store = (cert_store_t *)handle;
    return store->magic == CERT_STORE_MAGIC ? store : NULL;
}

static int cert_user_pointer_valid(uintptr_t pointer)
{
    return g_compat32_mode
        ? pointer >= 0x10000 && pointer < 0x80000000ULL
        : pointer >= 0x10000 && pointer < KERNEL_VBASE;
}

static int cert_validation_time(PVOID filetime, uint32_t *unix_time)
{
    if (!unix_time) return -1;
    if (!filetime) {
        *unix_time = ntp_get_utc();
        return 0;
    }
    if (!cert_user_pointer_valid((uintptr_t)filetime)) return -1;

    const cert_filetime_t *value = (const cert_filetime_t *)filetime;
    uint64_t ticks = ((uint64_t)value->dwHighDateTime << 32) |
                     value->dwLowDateTime;
    const uint64_t unix_epoch_ticks = 116444736000000000ULL;
    if (ticks < unix_epoch_ticks) return -1;
    uint64_t seconds = (ticks - unix_epoch_ticks) / 10000000ULL;
    if (seconds > UINT32_MAX) return -1;
    *unix_time = (uint32_t)seconds;
    return 0;
}

static int cert_usage_oid_valid(const char *oid)
{
    if (!cert_user_pointer_valid((uintptr_t)oid)) return 0;
    uint32_t i = 0;
    int saw_dot = 0;
    while (i < CERT_USAGE_OID_MAX && oid[i]) {
        char c = oid[i++];
        if (c == '.') saw_dot = 1;
        else if (c < '0' || c > '9') return 0;
    }
    return i > 0 && i < CERT_USAGE_OID_MAX && saw_dot;
}

static int cert_parse_requested_usage(PVOID chain_para,
                                      cert_usage_request_t *request)
{
    memset(request, 0, sizeof(*request));
    request->match_all = 1;
    if (!chain_para) return 0;
    if (!cert_user_pointer_valid((uintptr_t)chain_para)) return -1;

    uint32_t cb_size;
    uint32_t match_type;
    uint32_t count;
    uintptr_t pointers;
    if (g_compat32_mode) {
        const cert_chain_para32_min_t *para =
            (const cert_chain_para32_min_t *)chain_para;
        cb_size = para->cbSize;
        match_type = para->dwType;
        count = para->cUsageIdentifier;
        pointers = para->rgpszUsageIdentifier;
        if (cb_size < sizeof(*para)) return -1;
    } else {
        const cert_chain_para64_min_t *para =
            (const cert_chain_para64_min_t *)chain_para;
        cb_size = para->cbSize;
        match_type = para->dwType;
        count = para->cUsageIdentifier;
        pointers = (uintptr_t)para->rgpszUsageIdentifier;
        if (cb_size < sizeof(*para)) return -1;
    }
    if (match_type > 1 || count > CERT_USAGE_MAX ||
        (count && !cert_user_pointer_valid(pointers)))
        return -1;

    request->count = count;
    request->match_all = match_type == 0;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t oid_pointer = g_compat32_mode
            ? ((const uint32_t *)pointers)[i]
            : (uintptr_t)((const uint64_t *)pointers)[i];
        const char *oid = (const char *)oid_pointer;
        if (!cert_usage_oid_valid(oid)) return -1;
        request->oids[i] = oid;
    }
    return 0;
}

static int cert_chain_allows_usage(const uint8_t *const *chain,
                                   const uint32_t *lens, uint32_t cert_count,
                                   const cert_usage_request_t *request)
{
    if (!request->count) return 1;
    for (uint32_t usage = 0; usage < request->count; usage++) {
        int supported = 1;
        for (uint32_t cert = 0; cert < cert_count; cert++) {
            if (x509_allows_extended_key_usage(chain[cert], lens[cert],
                                               request->oids[usage]) != 1) {
                supported = 0;
                break;
            }
        }
        if (request->match_all && !supported) return 0;
        if (!request->match_all && supported) return 1;
    }
    return request->match_all ? 1 : 0;
}

static uint32_t cert_revocation_error(const uint8_t *cert,
                                      uint32_t cert_len,
                                      const uint8_t *issuer,
                                      uint32_t issuer_len,
                                      uint32_t validation_time,
                                      int allow_network)
{
    int network_attempted = 0;
    int status = certmgr_revocation_resolve(
        cert, cert_len, issuer, issuer_len, validation_time,
        allow_network, &network_attempted);
    if (status == CERTMGR_REVOCATION_GOOD) return 0;
    if (status == CERTMGR_REVOCATION_REVOKED)
        return CERT_TRUST_IS_REVOKED;
    if (status == CERTMGR_REVOCATION_UNKNOWN)
        return CERT_TRUST_REVOCATION_STATUS_UNKNOWN;

    uint32_t error = CERT_TRUST_REVOCATION_STATUS_UNKNOWN;
    if (network_attempted)
        error |= CERT_TRUST_IS_OFFLINE_REVOCATION;
    return error;
}

static void cert_retain(cert_object_t *obj)
{
    __sync_add_and_fetch(&obj->refs, 1);
}

static void cert_release(cert_object_t *obj)
{
    if (__sync_sub_and_fetch(&obj->refs, 1) == 0) {
        uint32_t pages = obj->pages;
        obj->magic = 0;
        cert_free_pages(obj, pages);
    }
}

static PVOID WINAPI shim_CertOpenStore(PVOID provider, DWORD encoding,
                                       ULONG_PTR crypt_prov, DWORD flags,
                                       PVOID para)
{
    (void)encoding; (void)crypt_prov; (void)flags;
    if ((uintptr_t)provider != CERT_STORE_PROV_MEMORY || para) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    cert_store_t *store = (cert_store_t *)cert_alloc_pages(1);
    if (!store) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    memset(store, 0, 4096);
    store->magic = CERT_STORE_MAGIC;
    return store;
}

static BOOL WINAPI shim_CertCloseStore(PVOID handle, DWORD flags)
{
    (void)flags;
    cert_store_t *store = cert_store(handle);
    if (!store) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    for (uint32_t i = 0; i < store->count; i++)
        cert_release(store->certs[i]);
    store->magic = 0;
    cert_free_pages(store, 1);
    return TRUE;
}

static PVOID WINAPI shim_CertCreateCertificateContext(DWORD encoding,
                                                       const BYTE *encoded,
                                                       DWORD encoded_len)
{
    if (!(encoding & X509_ASN_ENCODING) || !encoded || encoded_len < 4 ||
        encoded[0] != 0x30) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    uint64_t bytes = sizeof(cert_object_t) + encoded_len;
    uint32_t pages = (uint32_t)((bytes + 4095) / 4096);
    cert_object_t *obj = (cert_object_t *)cert_alloc_pages(pages);
    if (!obj) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    memset(obj, 0, pages * 4096ULL);
    if (g_compat32_mode) {
        obj->context.v32.dwCertEncodingType = encoding;
        obj->context.v32.pbCertEncoded = (uint32_t)(uintptr_t)obj->der;
        obj->context.v32.cbCertEncoded = encoded_len;
    } else {
        obj->context.v64.dwCertEncodingType = encoding;
        obj->context.v64.pbCertEncoded = (uint64_t)(uintptr_t)obj->der;
        obj->context.v64.cbCertEncoded = encoded_len;
    }
    obj->magic = CERT_OBJECT_MAGIC;
    obj->pages = pages;
    obj->refs = 1;
    memcpy(obj->der, encoded, encoded_len);
    return cert_public_context(obj);
}

static BOOL WINAPI shim_CertFreeCertificateContext(PVOID context)
{
    cert_object_t *obj = cert_object(context);
    if (!obj) return FALSE;
    cert_release(obj);
    return TRUE;
}

static BOOL WINAPI shim_CertAddCertificateContextToStore(
    PVOID store_handle, PVOID context, DWORD disposition,
    PVOID out_store_context)
{
    cert_store_t *store = cert_store(store_handle);
    cert_object_t *obj = cert_object(context);
    if (!store || !obj || disposition != CERT_STORE_ADD_ALWAYS ||
        store->count >= CERT_STORE_MAX) {
        SetLastError(store && store->count >= CERT_STORE_MAX
                     ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    cert_retain(obj);
    store->certs[store->count++] = obj;
    if (out_store_context) {
        cert_retain(obj);
        cert_write_pointer(out_store_context, cert_public_context(obj));
    }
    return TRUE;
}

static void chain_release_elements(cert_chain_object_t *obj)
{
    for (uint32_t i = 0; i < obj->element_count; i++) {
        if (obj->element_certs[i])
            cert_release(obj->element_certs[i]);
        obj->element_certs[i] = NULL;
    }
    obj->element_count = 0;
}

static cert_chain_object_t *chain_alloc(
    uint32_t trust_error, DWORD flags,
    const uint8_t *const *chain, const uint32_t *chain_lens,
    cert_object_t *const *source_objects,
    const uint32_t *element_errors, uint32_t chain_count)
{
    if (!chain || !chain_lens || !source_objects || !element_errors ||
        !chain_count || chain_count > CERT_CHAIN_MAX)
        return NULL;

    cert_chain_object_t *obj = (cert_chain_object_t *)cert_alloc_pages(1);
    if (!obj) return NULL;
    memset(obj, 0, 4096);
    obj->magic = CERT_CHAIN_MAGIC;
    obj->is_32bit = g_compat32_mode ? 1U : 0U;

    if (g_compat32_mode) {
        obj->context.v32.cbSize = sizeof(obj->context.v32);
        obj->context.v32.TrustStatus.dwErrorStatus = trust_error;
        obj->context.v32.cChain = 1;
        obj->context.v32.rgpChain = (uint32_t)(uintptr_t)&obj->chain_ptr.v32;
        obj->context.v32.dwCreateFlags = flags;
        obj->chain_ptr.v32 = (uint32_t)(uintptr_t)&obj->simple.v32;
        obj->simple.v32.cbSize = sizeof(obj->simple.v32);
        obj->simple.v32.TrustStatus.dwErrorStatus = trust_error;
        obj->simple.v32.cElement = chain_count;
        obj->simple.v32.rgpElement =
            (uint32_t)(uintptr_t)obj->element_ptrs32;
    } else {
        obj->context.v64.cbSize = sizeof(obj->context.v64);
        obj->context.v64.TrustStatus.dwErrorStatus = trust_error;
        obj->context.v64.cChain = 1;
        obj->context.v64.rgpChain = (uint64_t)(uintptr_t)&obj->chain_ptr.v64;
        obj->context.v64.dwCreateFlags = flags;
        obj->chain_ptr.v64 = (uint64_t)(uintptr_t)&obj->simple.v64;
        obj->simple.v64.cbSize = sizeof(obj->simple.v64);
        obj->simple.v64.TrustStatus.dwErrorStatus = trust_error;
        obj->simple.v64.cElement = chain_count;
        obj->simple.v64.rgpElement =
            (uint64_t)(uintptr_t)obj->element_ptrs64;
    }

    uint8_t chain_id[32];
    sha256(chain[0], chain_lens[0], chain_id);
    if (g_compat32_mode)
        memcpy(obj->context.v32.ChainId, chain_id, 16);
    else
        memcpy(obj->context.v64.ChainId, chain_id, 16);

    for (uint32_t i = 0; i < chain_count; i++) {
        cert_object_t *cert = source_objects[i];
        if (cert) {
            cert_retain(cert);
        } else {
            PVOID context = shim_CertCreateCertificateContext(
                X509_ASN_ENCODING, chain[i], chain_lens[i]);
            cert = cert_object(context);
            if (!cert) {
                chain_release_elements(obj);
                obj->magic = 0;
                cert_free_pages(obj, 1);
                return NULL;
            }
        }
        obj->element_certs[i] = cert;
        obj->element_count++;

        PVOID public_context = cert_public_context(cert);
        cert_chain_element_t *element = &obj->elements[i];
        if (g_compat32_mode) {
            element->v32.cbSize = sizeof(element->v32);
            element->v32.pCertContext =
                (uint32_t)(uintptr_t)public_context;
            element->v32.TrustStatus.dwErrorStatus = element_errors[i];
            obj->element_ptrs32[i] =
                (uint32_t)(uintptr_t)&element->v32;
        } else {
            element->v64.cbSize = sizeof(element->v64);
            element->v64.pCertContext =
                (uint64_t)(uintptr_t)public_context;
            element->v64.TrustStatus.dwErrorStatus = element_errors[i];
            obj->element_ptrs64[i] =
                (uint64_t)(uintptr_t)&element->v64;
        }
    }
    return obj;
}

static BOOL WINAPI shim_CertGetCertificateChain(
    PVOID chain_engine, PVOID end_context, PVOID time,
    PVOID additional_store, PVOID chain_para, DWORD flags,
    PVOID reserved, PVOID out_chain_context)
{
    (void)chain_engine; (void)reserved;
    cert_object_t *leaf = cert_object(end_context);
    cert_store_t *store = additional_store ? cert_store(additional_store) : NULL;
    cert_usage_request_t requested_usage;
    uint32_t validation_time;
    if (!leaf || (additional_store && !store) || !out_chain_context ||
        cert_parse_requested_usage(chain_para, &requested_usage) < 0 ||
        cert_validation_time(time, &validation_time) < 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    const uint8_t *chain[CERT_CHAIN_MAX];
    uint32_t lens[CERT_CHAIN_MAX];
    cert_object_t *chain_objects[CERT_CHAIN_MAX] = {0};
    uint32_t element_errors[CERT_CHAIN_MAX] = {0};
    uint8_t used[CERT_STORE_MAX] = {0};
    uint32_t count = 1;
    chain[0] = leaf->der;
    lens[0] = cert_encoded_len(leaf);
    chain_objects[0] = leaf;

    uint32_t trust_error = 0;
    int root_usage_allowed = 1;
    int trusted = certmgr_check_trust_anchor(
        chain[0], lens[0], requested_usage.oids, requested_usage.count,
        requested_usage.match_all, &root_usage_allowed);
    int signature_error = 0;
    int partial_chain = 0;
    while (!trusted && count < CERT_CHAIN_MAX) {
        int found = -1;
        int identity_candidates = 0;
        uint32_t store_count = store ? store->count : 0;
        for (uint32_t i = 0; i < store_count; i++) {
            if (used[i]) continue;
            cert_object_t *issuer = store->certs[i];
            if (!issuer) continue;
            uint32_t issuer_len = cert_encoded_len(issuer);
            if (x509_issuer_matches_subject(chain[count - 1],
                                            lens[count - 1], issuer->der,
                                            issuer_len) != 1)
                continue;
            identity_candidates++;
            if (x509_verify_chain_link(chain[count - 1], lens[count - 1],
                                       issuer->der, issuer_len) == 0) {
                found = (int)i;
                break;
            }
        }
        if (found >= 0) {
            used[found] = 1;
            cert_object_t *issuer = store->certs[found];
            chain[count] = issuer->der;
            lens[count] = cert_encoded_len(issuer);
            chain_objects[count] = issuer;
            trusted = certmgr_check_trust_anchor(
                chain[count], lens[count], requested_usage.oids,
                requested_usage.count, requested_usage.match_all,
                &root_usage_allowed);
            count++;
            continue;
        }

        certmgr_issuer_iter_t roots;
        const uint8_t *root_der;
        uint32_t root_len;
        int root_found = 0;
        if (certmgr_issuer_iter_begin(&roots, chain[count - 1],
                                      lens[count - 1]) == 0) {
            while (certmgr_issuer_iter_next(&roots, &root_der, &root_len)) {
                identity_candidates++;
                if (x509_verify_chain_link(chain[count - 1], lens[count - 1],
                                           root_der, root_len) == 0) {
                    chain[count] = root_der;
                    lens[count] = root_len;
                    count++;
                    trusted = certmgr_check_trust_anchor(
                        root_der, root_len, requested_usage.oids,
                        requested_usage.count, requested_usage.match_all,
                        &root_usage_allowed);
                    root_found = 1;
                    break;
                }
            }
        }
        if (root_found) break;

        if (identity_candidates) {
            signature_error = 1;
        } else if (x509_issuer_matches_subject(chain[count - 1],
                                               lens[count - 1],
                                               chain[count - 1],
                                               lens[count - 1]) == 1) {
            partial_chain = 0;
        } else {
            partial_chain = 1;
        }
        break;
    }

    if (!trusted) {
        if (signature_error) {
            trust_error |= CERT_TRUST_IS_NOT_SIGNATURE_VALID;
            element_errors[count - 1] |= CERT_TRUST_IS_NOT_SIGNATURE_VALID;
        }
        if (partial_chain || count == CERT_CHAIN_MAX) {
            trust_error |= CERT_TRUST_IS_PARTIAL_CHAIN;
            element_errors[count - 1] |= CERT_TRUST_IS_PARTIAL_CHAIN;
        } else {
            trust_error |= CERT_TRUST_IS_UNTRUSTED_ROOT;
            element_errors[count - 1] |= CERT_TRUST_IS_UNTRUSTED_ROOT;
        }
    }
    if (validation_time) {
        for (uint32_t i = 0; i < count; i++) {
            if (x509_check_validity(chain[i], lens[i], validation_time) != 0) {
                trust_error |= CERT_TRUST_IS_NOT_TIME_VALID;
                element_errors[i] |= CERT_TRUST_IS_NOT_TIME_VALID;
            }
        }
    }
    uint32_t constrained_count = trusted && count ? count - 1U : count;
    if ((trusted && !root_usage_allowed) ||
        !cert_chain_allows_usage(chain, lens, constrained_count,
                                 &requested_usage)) {
        trust_error |= CERT_TRUST_IS_NOT_VALID_FOR_USAGE;
        element_errors[0] |= CERT_TRUST_IS_NOT_VALID_FOR_USAGE;
    }
    uint32_t bad_constraint = 0;
    if (constrained_count > 1 &&
        x509_check_chain_constraints_at(chain, lens, constrained_count,
                                        &bad_constraint) != 0) {
        trust_error |= CERT_TRUST_INVALID_BASIC_CONSTRAINTS;
        if (bad_constraint < count)
            element_errors[bad_constraint] |=
                CERT_TRUST_INVALID_BASIC_CONSTRAINTS;
    }
    if (flags & (CERT_CHAIN_REVOCATION_CHECK_END_CERT |
                 CERT_CHAIN_REVOCATION_CHECK_CHAIN |
                 CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT)) {
        int allow_network = !(flags &
            (CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY |
             CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL));
        if (flags & CERT_CHAIN_REVOCATION_CHECK_END_CERT) {
            uint32_t issuer = count > 1 ? 1U : 0U;
            uint32_t revocation_error = cert_revocation_error(
                chain[0], lens[0], chain[issuer], lens[issuer],
                validation_time, allow_network);
            trust_error |= revocation_error;
            element_errors[0] |= revocation_error;
        } else {
            uint32_t checked = count;
            if ((flags & CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT) &&
                trusted && checked)
                checked--;
            for (uint32_t i = 0; i < checked; i++) {
                uint32_t issuer = i + 1U < count ? i + 1U : i;
                uint32_t revocation_error = cert_revocation_error(
                    chain[i], lens[i], chain[issuer], lens[issuer],
                    validation_time, allow_network);
                trust_error |= revocation_error;
                element_errors[i] |= revocation_error;
            }
        }
    }

    cert_chain_object_t *result = chain_alloc(
        trust_error, flags, chain, lens, chain_objects, element_errors, count);
    if (!result) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    cert_write_pointer(out_chain_context, &result->context);
    serial_puts("[CRYPT32] chain flags=");
    serial_puthex(flags, 8);
    serial_puts(" trust=");
    serial_puthex(trust_error, 8);
    serial_puts("\n");
    return TRUE;
}

static void WINAPI shim_CertFreeCertificateChain(PVOID context)
{
    uintptr_t p = (uintptr_t)context;
    if (g_compat32_mode ? (p < 0x10000 || p >= 0x80000000ULL)
                        : (p < KERNEL_VBASE)) return;
    cert_chain_object_t *obj = (cert_chain_object_t *)context;
    if (obj->magic != CERT_CHAIN_MAGIC) return;
    obj->magic = 0;
    chain_release_elements(obj);
    cert_free_pages(obj, 1);
}

/* CERT_CHAIN_POLICY_MICROSOFT_ROOT identifies public keys, independently of
 * trust, expiry and the certificate's subject string. These SHA-256 values
 * cover the subjectPublicKey BIT STRING (without its unused-bits octet).
 * Policy verification still requires a separately authenticated chain. */
static const BYTE cert_microsoft_root_keys[][32] = {
    /* Microsoft Root Authority (1997). */
    {0xee,0x09,0xb0,0x7a,0x85,0xe8,0xf2,0x4a,0x01,0xef,0x63,0x1a,0xe6,0x71,0xfe,0xf8,0xde,0xa8,0x01,0x5a,0x09,0xa7,0x15,0xe6,0xa6,0x73,0x90,0x11,0x90,0x92,0xb8,0x16},
    /* Microsoft Root Certificate Authority (2001). */
    {0x60,0xbd,0xed,0x75,0xc5,0xfd,0x11,0x90,0x10,0xd6,0x83,0x2f,0x76,0xde,0xfc,0x39,0x34,0x73,0xd7,0xa0,0xce,0x64,0xfb,0xd6,0x8d,0xab,0xa2,0x9b,0xfd,0x0b,0x2f,0x7c},
    /* Microsoft Root Certificate Authority 2010. */
    {0x12,0xeb,0x31,0xfd,0xc8,0x92,0x49,0xa0,0xeb,0x67,0xeb,0x65,0xc2,0x97,0x7d,0xbe,0x2a,0xd9,0x6a,0x90,0x9c,0xcb,0xd1,0x80,0xf7,0xe2,0xe1,0x6b,0x27,0x82,0xca,0xee},
    /* Microsoft Time Stamp Root Certificate Authority 2014. */
    {0x29,0x94,0x70,0xb8,0xbb,0x26,0x78,0x78,0x0c,0xf0,0x20,0xa3,0x32,0x2f,0x69,0xb4,0x0a,0x11,0x62,0x24,0x05,0x71,0xaf,0xd5,0x3a,0x80,0x5b,0x9b,0x03,0xf2,0xeb,0x5d},
    /* Microsoft ECC Product Root Certificate Authority 2018. */
    {0x9a,0xb5,0xe4,0xc6,0x11,0x55,0xa9,0x39,0x6e,0xa1,0xc8,0xbf,0x48,0x29,0xd3,0xe1,0x08,0x53,0x52,0x57,0xb7,0x1b,0xe3,0x80,0x97,0x21,0x62,0x32,0x0f,0xfb,0x13,0x3b},
    /* Microsoft ECC TS Root Certificate Authority 2018. */
    {0x8b,0x24,0xff,0x56,0x20,0xc8,0x19,0x1f,0xd9,0x6d,0x52,0xec,0xb6,0xb6,0x0c,0x55,0xe8,0x2b,0xa8,0x49,0x04,0x3f,0xea,0x27,0x39,0x27,0xdb,0xf4,0x35,0x53,0xe6,0x59},
};
static const BYTE cert_microsoft_application_key[32] = {
    /* Microsoft Root Certificate Authority 2011. */
    0x4a,0xbb,0x05,0x94,0xd3,0x03,0xef,0x70,0x77,0x13,0x88,0x34,0xab,0x31,0x5e,0x94,0x1e,0x96,0x30,0x93,0xe0,0x5b,0x4b,0x14,0xaf,0x5d,0xcb,0x52,0x77,0x12,0xc0,0x0a
};

static ULONG_PTR cert_policy_pointer(PCVOID data, DWORD offset)
{
    ULONG_PTR value = 0;
    memcpy(&value, (const BYTE *)data + offset, g_compat32_mode ? 4 : 8);
    return value;
}

static BOOL cert_policy_root_key(PCVOID context, DWORD flags)
{
    const BYTE *der = (const BYTE *)cert_policy_pointer(
        context, g_compat32_mode ? 4 : 8);
    DWORD length = *(const DWORD *)((const BYTE *)context +
                                    (g_compat32_mode ? 8 : 16));
    const BYTE *key;
    DWORD key_length;
    BYTE digest[32];
    if (!der || !length || length > 1024U * 1024U ||
        x509_get_subject_pubkey_bits(der, length, &key, &key_length) != 0)
        return FALSE;
    sha256(key, key_length, digest);
    if (flags & 0x00020000U)
        return memcmp(digest, cert_microsoft_application_key, 32) == 0;
    for (DWORD i = 0; i < sizeof(cert_microsoft_root_keys) / 32; i++)
        if (memcmp(digest, cert_microsoft_root_keys[i], 32) == 0)
            return TRUE;
    /* No unrecognized test or flight key is accepted. */
    return FALSE;
}

static BOOL WINAPI shim_CertVerifyCertificateChainPolicy(
    PCSTR policy, PCVOID context, PCVOID parameters, PVOID status)
{
    DWORD *output = (DWORD *)status;
    if (!output || output[0] < (g_compat32_mode ? 20U : 24U) ||
        !context || *(const DWORD *)context <
            (g_compat32_mode ? 56U : 72U) ||
        (parameters && *(const DWORD *)parameters <
            (g_compat32_mode ? 12U : 16U))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((ULONG_PTR)policy != 7) {
        SetLastError(120); /* Unsupported policies must never report success. */
        return FALSE;
    }
    DWORD flags = parameters ? ((const DWORD *)parameters)[1] : 0;
    output[1] = 0x800B0109U; /* CERT_E_UNTRUSTEDROOT */
    output[2] = output[3] = 0;
    if ((flags & 0x00030000U) == 0x00030000U) {
        output[1] = 0x80070057U; /* Application and test roots are exclusive. */
        return TRUE;
    }
    if (!((const DWORD *)context)[3]) return TRUE;
    PCVOID chains = (PCVOID)cert_policy_pointer(context, 16);
    PCVOID simple = chains ? (PCVOID)cert_policy_pointer(chains, 0) : NULL;
    if (!simple || *(const DWORD *)simple <
            (g_compat32_mode ? 32U : 40U)) return TRUE;
    DWORD count = ((const DWORD *)simple)[3];
    if (!count || count > 1024U) return TRUE;
    output[3] = count - 1;
    PCVOID elements = (PCVOID)cert_policy_pointer(simple, 16);
    PCVOID last = elements ? (PCVOID)cert_policy_pointer(
        elements, (count - 1) * (g_compat32_mode ? 4U : 8U)) : NULL;
    if (!last || *(const DWORD *)last <
            (g_compat32_mode ? 32U : 56U)) return TRUE;
    PCVOID root = (PCVOID)cert_policy_pointer(last, g_compat32_mode ? 4 : 8);
    if (root && cert_policy_root_key(root, flags)) output[1] = 0;
    return TRUE;
}

typedef struct __attribute__((packed)) {
    uint32_t size;
    uint32_t data;
} crypt_data_blob32_t;

typedef struct {
    uint32_t size;
    uint32_t padding;
    uint64_t data;
} crypt_data_blob64_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint32_t plaintext_size;
    uint8_t nonce[12];
    uint8_t tag[16];
} osito_dpapi_header_t;

_Static_assert(sizeof(crypt_data_blob32_t) == 8, "DATA_BLOB32 layout");
_Static_assert(sizeof(crypt_data_blob64_t) == 16, "DATA_BLOB64 layout");
_Static_assert(sizeof(osito_dpapi_header_t) == 36, "DPAPI envelope layout");

static const uint8_t osito_dpapi_master_key[16] = {
    0x4F, 0x73, 0x69, 0x74, 0x6F, 0x2D, 0x4B, 0x20,
    0x44, 0x50, 0x41, 0x50, 0x49, 0x20, 0x76, 0x31,
};

static BOOL crypt_blob_read(PVOID blob, DWORD *size, const uint8_t **data)
{
    if (!blob || !size || !data) return FALSE;
    if (g_compat32_mode) {
        const crypt_data_blob32_t *value =
            (const crypt_data_blob32_t *)blob;
        *size = value->size;
        *data = (const uint8_t *)(uintptr_t)value->data;
    } else {
        const crypt_data_blob64_t *value =
            (const crypt_data_blob64_t *)blob;
        *size = value->size;
        *data = (const uint8_t *)(uintptr_t)value->data;
    }
    return !*size || *data;
}

static void crypt_blob_write(PVOID blob, DWORD size, PVOID data)
{
    if (g_compat32_mode) {
        crypt_data_blob32_t *value = (crypt_data_blob32_t *)blob;
        value->size = size;
        value->data = (uint32_t)(uintptr_t)data;
    } else {
        crypt_data_blob64_t *value = (crypt_data_blob64_t *)blob;
        value->size = size;
        value->padding = 0;
        value->data = (uint64_t)(uintptr_t)data;
    }
}

static void crypt_write_null_pointer(PVOID output)
{
    if (!output) return;
    if (g_compat32_mode)
        *(uint32_t *)output = 0;
    else
        *(uint64_t *)output = 0;
}

static BOOL crypt_dpapi_key(PVOID entropy_blob, uint8_t key[16])
{
    if (!entropy_blob) {
        memcpy(key, osito_dpapi_master_key, sizeof(osito_dpapi_master_key));
        return TRUE;
    }

    DWORD entropy_size;
    const uint8_t *entropy;
    if (!crypt_blob_read(entropy_blob, &entropy_size, &entropy))
        return FALSE;
    uint8_t digest[32];
    hmac_sha256(osito_dpapi_master_key, sizeof(osito_dpapi_master_key),
                entropy, entropy_size, digest);
    memcpy(key, digest, 16);
    memset(digest, 0, sizeof(digest));
    return TRUE;
}

static BOOL WINAPI shim_CryptProtectData(PVOID data_in, PVOID description,
                                         PVOID entropy, PVOID reserved,
                                         PVOID prompt, DWORD flags,
                                         PVOID data_out)
{
    (void)description;
    (void)flags;
    if (!data_in || !data_out || reserved || prompt) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    crypt_blob_write(data_out, 0, NULL);

    DWORD plaintext_size;
    const uint8_t *plaintext;
    uint8_t key[16];
    if (!crypt_blob_read(data_in, &plaintext_size, &plaintext) ||
        !crypt_dpapi_key(entropy, key) ||
        plaintext_size > UINT32_MAX - sizeof(osito_dpapi_header_t)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    DWORD envelope_size = (DWORD)sizeof(osito_dpapi_header_t) + plaintext_size;
    osito_dpapi_header_t *envelope =
        (osito_dpapi_header_t *)LocalAlloc(0, envelope_size);
    if (!envelope) {
        memset(key, 0, sizeof(key));
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    envelope->magic[0] = 'O';
    envelope->magic[1] = 'D';
    envelope->magic[2] = 'P';
    envelope->magic[3] = '1';
    envelope->plaintext_size = plaintext_size;
    random_get_bytes(envelope->nonce, sizeof(envelope->nonce));
    uint8_t *ciphertext = (uint8_t *)(envelope + 1);
    int result = aes128_gcm_encrypt(
        key, envelope->nonce, envelope, 20, plaintext, plaintext_size,
        ciphertext, envelope->tag);
    memset(key, 0, sizeof(key));
    if (result < 0) {
        LocalFree(envelope);
        SetLastError(13); /* ERROR_INVALID_DATA */
        return FALSE;
    }

    crypt_blob_write(data_out, envelope_size, envelope);
    SetLastError(0);
    serial_puts("[CRYPT32-DPAPI] protected bytes=");
    serial_putdec(plaintext_size);
    serial_puts("\n");
    return TRUE;
}

static BOOL WINAPI shim_CryptUnprotectData(PVOID data_in, PVOID description,
                                           PVOID entropy, PVOID reserved,
                                           PVOID prompt, DWORD flags,
                                           PVOID data_out)
{
    (void)flags;
    if (!data_in || !data_out || reserved || prompt) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    crypt_write_null_pointer(description);
    crypt_blob_write(data_out, 0, NULL);

    DWORD envelope_size;
    const uint8_t *envelope_data;
    uint8_t key[16];
    if (!crypt_blob_read(data_in, &envelope_size, &envelope_data) ||
        envelope_size < sizeof(osito_dpapi_header_t) ||
        !crypt_dpapi_key(entropy, key)) {
        SetLastError(13); /* ERROR_INVALID_DATA */
        return FALSE;
    }

    const osito_dpapi_header_t *envelope =
        (const osito_dpapi_header_t *)envelope_data;
    DWORD plaintext_size = envelope->plaintext_size;
    if (envelope->magic[0] != 'O' || envelope->magic[1] != 'D' ||
        envelope->magic[2] != 'P' || envelope->magic[3] != '1' ||
        plaintext_size != envelope_size - sizeof(*envelope)) {
        memset(key, 0, sizeof(key));
        SetLastError(13); /* ERROR_INVALID_DATA */
        return FALSE;
    }

    uint8_t *plaintext =
        (uint8_t *)LocalAlloc(0, plaintext_size ? plaintext_size : 1);
    if (!plaintext) {
        memset(key, 0, sizeof(key));
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    int result = aes128_gcm_decrypt(
        key, envelope->nonce, envelope, 20, envelope + 1, plaintext_size,
        plaintext, envelope->tag);
    memset(key, 0, sizeof(key));
    if (result < 0) {
        LocalFree(plaintext);
        SetLastError(13); /* ERROR_INVALID_DATA */
        return FALSE;
    }

    crypt_blob_write(data_out, plaintext_size, plaintext);
    SetLastError(0);
    serial_puts("[CRYPT32-DPAPI] unprotected bytes=");
    serial_putdec(plaintext_size);
    serial_puts("\n");
    return TRUE;
}

#define NTE_NOT_SUPPORTED ((LONG)0x80090029U)

static void ncrypt_zero_handle(PVOID output)
{
    if (!output) return;
    if (g_compat32_mode)
        *(uint32_t *)output = 0;
    else
        *(uint64_t *)output = 0;
}

static LONG WINAPI shim_NCryptCreatePersistedKey(
    PVOID provider, PVOID key, PCWSTR algorithm, PCWSTR name,
    DWORD legacy_spec, DWORD flags)
{
    (void)provider; (void)algorithm; (void)name;
    (void)legacy_spec; (void)flags;
    ncrypt_zero_handle(key);
    return NTE_NOT_SUPPORTED;
}

static LONG WINAPI shim_NCryptDeleteKey(PVOID key, DWORD flags)
{
    (void)key; (void)flags;
    return NTE_NOT_SUPPORTED;
}

static LONG WINAPI shim_NCryptExportKey(
    PVOID key, PVOID export_key, PCWSTR blob_type, PVOID parameters,
    PVOID output, DWORD output_size, DWORD *result_size, DWORD flags)
{
    (void)key; (void)export_key; (void)blob_type; (void)parameters;
    (void)output; (void)output_size; (void)flags;
    if (result_size) *result_size = 0;
    return NTE_NOT_SUPPORTED;
}

static LONG WINAPI shim_NCryptFinalizeKey(PVOID key, DWORD flags)
{
    (void)key; (void)flags;
    return NTE_NOT_SUPPORTED;
}

static LONG WINAPI shim_NCryptFreeObject(PVOID object)
{
    (void)object;
    return NTE_NOT_SUPPORTED;
}

static LONG WINAPI shim_NCryptGetProperty(
    PVOID object, PCWSTR property, PVOID output, DWORD output_size,
    DWORD *result_size, DWORD flags)
{
    (void)object; (void)property; (void)output; (void)output_size; (void)flags;
    if (result_size) *result_size = 0;
    return NTE_NOT_SUPPORTED;
}

static LONG WINAPI shim_NCryptImportKey(
    PVOID provider, PVOID import_key, PCWSTR blob_type, PVOID parameters,
    PVOID key, PVOID data, DWORD data_size, DWORD flags)
{
    (void)provider; (void)import_key; (void)blob_type; (void)parameters;
    (void)data; (void)data_size; (void)flags;
    ncrypt_zero_handle(key);
    return NTE_NOT_SUPPORTED;
}

static LONG WINAPI shim_NCryptIsAlgSupported(
    PVOID provider, PCWSTR algorithm, DWORD flags)
{
    (void)provider; (void)algorithm; (void)flags;
    return NTE_NOT_SUPPORTED;
}

static LONG WINAPI shim_NCryptOpenKey(
    PVOID provider, PVOID key, PCWSTR name, DWORD legacy_spec, DWORD flags)
{
    (void)provider; (void)name; (void)legacy_spec; (void)flags;
    ncrypt_zero_handle(key);
    return NTE_NOT_SUPPORTED;
}

static LONG WINAPI shim_NCryptOpenStorageProvider(
    PVOID provider, PCWSTR provider_name, DWORD flags)
{
    (void)provider_name; (void)flags;
    ncrypt_zero_handle(provider);
    return NTE_NOT_SUPPORTED;
}

static LONG WINAPI shim_NCryptSignHash(
    PVOID key, PVOID padding, PVOID hash, DWORD hash_size,
    PVOID signature, DWORD signature_size, DWORD *result_size, DWORD flags)
{
    (void)key; (void)padding; (void)hash; (void)hash_size;
    (void)signature; (void)signature_size; (void)flags;
    if (result_size) *result_size = 0;
    return NTE_NOT_SUPPORTED;
}

static int wintrust_guid_equal(const GUID *left, const GUID *right)
{
    if (!left || !right) return 0;
    const BYTE *a = (const BYTE *)left;
    const BYTE *b = (const BYTE *)right;
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* Fixed-width provider layouts are shared by WinVerifyTrust and its helpers.
 * Returned certificate contexts belong to the retained provider state. */
#define WINTRUST_PROVIDER_FIELDS(P) \
    DWORD cbStruct; P pWintrustData; BOOL fOpenedFile; P hWndParent, pgActionID, hProv; \
    DWORD dwError, dwRegSecuritySettings, dwRegPolicySettings; P psPfns; \
    DWORD cdwTrustStepErrors; P padwTrustStepErrors; DWORD chStores; P pahStores; \
    DWORD dwEncoding; P hMsg; DWORD csSigners; P pasSigners; \
    DWORD csProvPrivData; P pasProvPrivData; DWORD dwSubjectChoice; P pPDSip, pszUsageOID; \
    BOOL fRecallWithState; cert_filetime_t sftSystemTime; P pszCTLSignerUsageOID; \
    DWORD dwProvFlags, dwFinalError; P pRequestUsage; DWORD dwTrustPubSettings, dwUIStateFlags; \
    P pSigState, pSigSettings
#define WINTRUST_SIGNER_FIELDS(P) \
    DWORD cbStruct; cert_filetime_t sftVerifyAsOf; DWORD csCertChain; P pasCertChain; \
    DWORD dwSignerType; P psSigner; DWORD dwError, csCounterSigners; P pasCounterSigners, pChainContext
#define WINTRUST_CERT_FIELDS(P) \
    DWORD cbStruct; P pCert; BOOL fCommercial, fTrustedRoot, fSelfSigned, fTestCert; \
    DWORD dwRevokedReason, dwConfidence, dwError; P pTrustListContext; BOOL fTrustListSignerCert; \
    P pCtlContext; DWORD dwCtlError; BOOL fIsCyclic; P pChainElement
typedef struct { WINTRUST_PROVIDER_FIELDS(uint32_t); } wintrust_provider32_t;
typedef struct { WINTRUST_PROVIDER_FIELDS(uint64_t); } wintrust_provider64_t;
typedef struct { WINTRUST_SIGNER_FIELDS(uint32_t); } wintrust_signer32_t;
typedef struct { WINTRUST_SIGNER_FIELDS(uint64_t); } wintrust_signer64_t;
typedef struct { WINTRUST_CERT_FIELDS(uint32_t); } wintrust_cert32_t;
typedef struct { WINTRUST_CERT_FIELDS(uint64_t); } wintrust_cert64_t;
#undef WINTRUST_PROVIDER_FIELDS
#undef WINTRUST_SIGNER_FIELDS
#undef WINTRUST_CERT_FIELDS
typedef union { wintrust_provider32_t v32; wintrust_provider64_t v64; } wintrust_provider_t;
typedef union { wintrust_signer32_t v32; wintrust_signer64_t v64; } wintrust_signer_t;
typedef union {
    wintrust_cert32_t v32[CERT_CHAIN_MAX];
    wintrust_cert64_t v64[CERT_CHAIN_MAX];
} wintrust_certs_t;
typedef struct {
    wintrust_provider_t provider;
    wintrust_signer_t signer[2];
    wintrust_certs_t certificates[2];
    GUID action;
    DWORD steps[38];
} wintrust_public_t;
typedef struct {
    volatile int used;
    DWORD owner;
    BOOL compat;
    HANDLE handle;
    wintrust_public_t *data;
    cert_chain_object_t *chains[2];
} wintrust_state_t;
#define WINTRUST_STATE_LIMIT 64
static wintrust_state_t wintrust_states[WINTRUST_STATE_LIMIT];
static volatile DWORD wintrust_generation;
_Static_assert(sizeof(wintrust_provider32_t) == 136, "CRYPT_PROVIDER_DATA32");
_Static_assert(sizeof(wintrust_provider64_t) == 240, "CRYPT_PROVIDER_DATA64");
_Static_assert(sizeof(wintrust_signer32_t) == 44, "CRYPT_PROVIDER_SGNR32");
_Static_assert(sizeof(wintrust_signer64_t) == 64, "CRYPT_PROVIDER_SGNR64");
_Static_assert(sizeof(wintrust_cert32_t) == 60, "CRYPT_PROVIDER_CERT32");
_Static_assert(sizeof(wintrust_cert64_t) == 88, "CRYPT_PROVIDER_CERT64");
_Static_assert(sizeof(wintrust_public_t) <= 4096, "provider state fits one page");

static DWORD wintrust_policy_settings(void)
{
    DWORD policy = 0x00023c00U;
    HKEY key = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\WinTrust\\Trust Providers\\Software Publishing",
        0, KEY_QUERY_VALUE, &key) == 0) {
        DWORD value = 0, size = sizeof(value), type = 0;
        if (RegQueryValueExA(key, "State", NULL, &type, (BYTE *)&value, &size) == 0 &&
            type == REG_DWORD && size == sizeof(value)) policy = value;
        RegCloseKey(key);
    }
    return policy;
}

static wintrust_state_t *wintrust_state_find(HANDLE handle, PCVOID provider)
{
    DWORD owner = GetCurrentProcessId();
    for (int i = 0; i < WINTRUST_STATE_LIMIT; i++) {
        wintrust_state_t *state = &wintrust_states[i];
        if (state->used == 1 && state->owner == owner &&
            ((handle && state->handle == handle) ||
             (provider && provider == (PCVOID)&state->data->provider)))
            return state;
    }
    return NULL;
}

static void wintrust_state_release(wintrust_state_t *state)
{
    if (!state || !__sync_bool_compare_and_swap(&state->used, 1, -1)) return;
    for (int i = 0; i < 2; i++) {
        if (state->chains[i]) {
            state->chains[i]->magic = 0;
            chain_release_elements(state->chains[i]);
            cert_free_pages(state->chains[i], 1);
            state->chains[i] = NULL;
        }
    }
    cert_free_pages(state->data, 1);
    state->data = NULL;
    state->handle = NULL;
    state->owner = 0;
    __sync_lock_release(&state->used);
}

void crypt32_release_process(DWORD owner)
{
    for (int i = 0; i < WINTRUST_STATE_LIMIT; i++)
        if (wintrust_states[i].used == 1 && wintrust_states[i].owner == owner)
            wintrust_state_release(&wintrust_states[i]);
}

static wintrust_state_t *wintrust_state_create(HANDLE window, const GUID *action,
                                              PVOID trust_data, DWORD flags,
                                              DWORD software_policy)
{
    for (int i = 0; i < WINTRUST_STATE_LIMIT; i++) {
        wintrust_state_t *state = &wintrust_states[i];
        if (!__sync_bool_compare_and_swap(&state->used, 0, -1)) continue;
        wintrust_public_t *data = cert_alloc_pages(1);
        if (!data) { __sync_lock_release(&state->used); return NULL; }
        memset(data, 0, 4096);
        data->action = *action;
        state->owner = GetCurrentProcessId();
        state->compat = g_compat32_mode;
        state->data = data;
        DWORD generation = __sync_add_and_fetch(&wintrust_generation, 1);
        state->handle = (HANDLE)(ULONG_PTR)(0x78000000U | ((generation & 0xffffU) << 8) | (DWORD)i);
        if (state->compat) {
            wintrust_provider32_t *p = &data->provider.v32;
            p->cbStruct = sizeof(*p);
            p->pWintrustData = (uint32_t)(ULONG_PTR)trust_data;
            p->hWndParent = (uint32_t)(ULONG_PTR)window;
            p->pgActionID = (uint32_t)(ULONG_PTR)&data->action;
            p->dwProvFlags = flags;
            p->dwRegPolicySettings = software_policy;
            p->cdwTrustStepErrors = 38;
            p->padwTrustStepErrors = (uint32_t)(ULONG_PTR)data->steps;
        } else {
            wintrust_provider64_t *p = &data->provider.v64;
            p->cbStruct = sizeof(*p);
            p->pWintrustData = (ULONG_PTR)trust_data;
            p->hWndParent = (ULONG_PTR)window;
            p->pgActionID = (ULONG_PTR)&data->action;
            p->dwProvFlags = flags;
            p->dwRegPolicySettings = software_policy;
            p->cdwTrustStepErrors = 38;
            p->padwTrustStepErrors = (ULONG_PTR)data->steps;
        }
        __sync_synchronize();
        state->used = 1;
        return state;
    }
    return NULL;
}

static BOOL wintrust_chain_capture(const authenticode_report_t *report, PVOID opaque)
{
    wintrust_state_t *state = (wintrust_state_t *)opaque;
    wintrust_public_t *data = state->data;
    for (int index = 0; index < 2; index++) {
        const authenticode_chain_t *input = index ? &report->timestamp : &report->signer;
        if (!input->count) continue;
        cert_object_t *objects[CERT_CHAIN_MAX] = {0};
        DWORD errors = 0;
        for (DWORD i = 0; i < input->count && i < CERT_CHAIN_MAX; i++)
            errors |= input->errors[i];
        cert_chain_object_t *chain = chain_alloc(errors, 0, input->certificates,
            input->lengths, objects, input->errors, input->count);
        if (!chain) return FALSE;
        state->chains[index] = chain;
        uint64_t time = ((uint64_t)input->validation_time + 11644473600ULL) * 10000000ULL;
        cert_filetime_t verify_time = {(DWORD)time, (DWORD)(time >> 32)};
        if (state->compat) {
            wintrust_signer32_t *signer = &data->signer[index].v32;
            signer->cbStruct = sizeof(*signer);
            signer->sftVerifyAsOf = verify_time;
            signer->csCertChain = input->count;
            signer->pasCertChain = (uint32_t)(ULONG_PTR)data->certificates[index].v32;
            signer->pChainContext = (uint32_t)(ULONG_PTR)&chain->context;
            signer->dwSignerType = index ? 0x10U : 0;
        } else {
            wintrust_signer64_t *signer = &data->signer[index].v64;
            signer->cbStruct = sizeof(*signer);
            signer->sftVerifyAsOf = verify_time;
            signer->csCertChain = input->count;
            signer->pasCertChain = (ULONG_PTR)data->certificates[index].v64;
            signer->pChainContext = (ULONG_PTR)&chain->context;
            signer->dwSignerType = index ? 0x10U : 0;
        }
        for (DWORD i = 0; i < input->count; i++) {
            BOOL self_signed = x509_issuer_matches_subject(input->certificates[i],
                input->lengths[i], input->certificates[i], input->lengths[i]) == 1;
            if (state->compat) {
                wintrust_cert32_t *cert = &data->certificates[index].v32[i];
                cert->cbStruct = sizeof(*cert);
                cert->pCert = (uint32_t)(ULONG_PTR)cert_public_context(chain->element_certs[i]);
                cert->fTrustedRoot = i + 1 == input->count;
                cert->fSelfSigned = self_signed;
                cert->dwConfidence = 0x11000000U;
                cert->pChainElement = (uint32_t)(ULONG_PTR)&chain->elements[i].v32;
            } else {
                wintrust_cert64_t *cert = &data->certificates[index].v64[i];
                cert->cbStruct = sizeof(*cert);
                cert->pCert = (ULONG_PTR)cert_public_context(chain->element_certs[i]);
                cert->fTrustedRoot = i + 1 == input->count;
                cert->fSelfSigned = self_signed;
                cert->dwConfidence = 0x11000000U;
                cert->pChainElement = (ULONG_PTR)&chain->elements[i].v64;
            }
        }
    }
    if (state->compat) {
        data->provider.v32.csSigners = 1;
        data->provider.v32.pasSigners = (uint32_t)(ULONG_PTR)&data->signer[0].v32;
        data->provider.v32.dwEncoding = 0x10001U;
        if (state->chains[1]) {
            data->signer[0].v32.csCounterSigners = 1;
            data->signer[0].v32.pasCounterSigners = (uint32_t)(ULONG_PTR)&data->signer[1].v32;
        }
    } else {
        data->provider.v64.csSigners = 1;
        data->provider.v64.pasSigners = (ULONG_PTR)&data->signer[0].v64;
        data->provider.v64.dwEncoding = 0x10001U;
        if (state->chains[1]) {
            data->signer[0].v64.csCounterSigners = 1;
            data->signer[0].v64.pasCounterSigners = (ULONG_PTR)&data->signer[1].v64;
        }
    }
    return TRUE;
}

static PVOID WINAPI shim_WTHelperProvDataFromStateData(HANDLE handle)
{
    wintrust_state_t *state = wintrust_state_find(handle, NULL);
    return state ? &state->data->provider : NULL;
}

static PVOID WINAPI shim_WTHelperGetProvSignerFromChain(
    PVOID provider, DWORD signer, BOOL counter, DWORD counter_index)
{
    wintrust_state_t *state = wintrust_state_find(NULL, provider);
    if (!state || signer || (counter && counter_index)) return NULL;
    int index = counter ? 1 : 0;
    if (!state->chains[index]) return NULL;
    return &state->data->signer[index];
}

static LONG WINAPI shim_WinVerifyTrust(HANDLE window, const GUID *action,
                                        PVOID trust_data)
{
    static const GUID action_generic_verify_v2 = {
        0x00AAC56B, 0xCD44, 0x11D0,
        { 0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE }
    };
    (void)window;

    if (!trust_data) return TRUST_E_SUBJECT_FORM_UNKNOWN;

    DWORD union_choice;
    DWORD state_action;
    DWORD revocation_checks;
    DWORD provider_flags;
    PVOID file_data;
    HANDLE previous_state;
    PVOID output_state;
    if (g_compat32_mode) {
        const wintrust_data32_t *data =
            (const wintrust_data32_t *)trust_data;
        if (data->cbStruct < sizeof(*data))
            return TRUST_E_SUBJECT_FORM_UNKNOWN;
        union_choice = data->dwUnionChoice;
        state_action = data->dwStateAction;
        revocation_checks = data->fdwRevocationChecks;
        provider_flags = data->dwProvFlags;
        file_data = (PVOID)(ULONG_PTR)data->pFile;
        previous_state = (HANDLE)(ULONG_PTR)data->hWVTStateData;
        output_state = (BYTE *)trust_data + 32;
    } else {
        const wintrust_data64_t *data =
            (const wintrust_data64_t *)trust_data;
        if (data->cbStruct < sizeof(*data))
            return TRUST_E_SUBJECT_FORM_UNKNOWN;
        union_choice = data->dwUnionChoice;
        state_action = data->dwStateAction;
        revocation_checks = data->fdwRevocationChecks;
        provider_flags = data->dwProvFlags;
        file_data = data->pFile;
        previous_state = data->hWVTStateData;
        output_state = &((wintrust_data64_t *)trust_data)->hWVTStateData;
    }

    if (state_action == WTD_STATEACTION_CLOSE) {
        wintrust_state_release(wintrust_state_find(previous_state, NULL));
        cert_write_pointer(output_state, NULL);
        return 0;
    }
    if (!wintrust_guid_equal(action, &action_generic_verify_v2))
        return action ? TRUST_E_ACTION_UNKNOWN : TRUST_E_PROVIDER_UNKNOWN;
    if (union_choice != WTD_CHOICE_FILE || !file_data)
        return TRUST_E_SUBJECT_FORM_UNKNOWN;
    if (state_action > 1) return TRUST_E_SUBJECT_FORM_UNKNOWN;

    PCWSTR path;
    HANDLE file;
    if (g_compat32_mode) {
        const wintrust_file_info32_t *info =
            (const wintrust_file_info32_t *)file_data;
        if (info->cbStruct < sizeof(*info))
            return TRUST_E_SUBJECT_FORM_UNKNOWN;
        path = (PCWSTR)(ULONG_PTR)info->pcwszFilePath;
        file = (HANDLE)(ULONG_PTR)info->hFile;
    } else {
        const wintrust_file_info64_t *info =
            (const wintrust_file_info64_t *)file_data;
        if (info->cbStruct < sizeof(*info))
            return TRUST_E_SUBJECT_FORM_UNKNOWN;
        path = info->pcwszFilePath;
        file = info->hFile;
    }
    if (!path && (!file || file == INVALID_HANDLE_VALUE))
        return TRUST_E_SUBJECT_FORM_UNKNOWN;
    wintrust_state_t *state = NULL;
    DWORD software_policy = wintrust_policy_settings();
    if (state_action == 1) {
        wintrust_state_release(wintrust_state_find(previous_state, NULL));
        cert_write_pointer(output_state, NULL);
        state = wintrust_state_create(window, action, trust_data, provider_flags, software_policy);
        if (!state) return (LONG)0x8007000EU;
        cert_write_pointer(output_state, state->handle);
    }
    authenticode_result_t result = path && GetFileAttributesW(path) == 0xFFFFFFFFU
        ? AUTHENTICODE_VERIFY_IO_ERROR : authenticode_verify_file_report(
            path, file, revocation_checks, provider_flags,
            software_policy,
            state ? wintrust_chain_capture : NULL, state);
    LONG status = result == AUTHENTICODE_VERIFY_OK ? 0 :
        result == AUTHENTICODE_VERIFY_NO_SIGNATURE ? TRUST_E_NOSIGNATURE :
        result == AUTHENTICODE_VERIFY_IO_ERROR ? (LONG)0x80092003U :
        result == AUTHENTICODE_VERIFY_BAD_DIGEST ? (LONG)0x80096010U :
        TRUST_E_SUBJECT_NOT_TRUSTED;
    if (state) {
        if (state->compat) state->data->provider.v32.dwFinalError = (DWORD)status;
        else state->data->provider.v64.dwFinalError = (DWORD)status;
        state->data->steps[35] = (DWORD)status;
    }
    if (!status) return 0;

    serial_puts("[WINTRUST] Authenticode rejected, reason=");
    serial_putdec((uint32_t)result);
    serial_puts("\n");
    return status;
}

static const WIN32_EXPORT crypt32_exports[] = {
    WX_STD("CertVerifyCertificateChainPolicy",    shim_CertVerifyCertificateChainPolicy,    4),
    WX_STD("CertGetCertificateChain",             shim_CertGetCertificateChain,             8),
    WX_STD("CertOpenStore",                       shim_CertOpenStore,                       5),
    WX_STD("CertCloseStore",                      shim_CertCloseStore,                      2),
    WX_STD("CertCreateCertificateContext",        shim_CertCreateCertificateContext,        3),
    WX_STD("CertFreeCertificateContext",          shim_CertFreeCertificateContext,          1),
    WX_STD("CertAddCertificateContextToStore",    shim_CertAddCertificateContextToStore,    4),
    WX_STD("CertFreeCertificateChain",            shim_CertFreeCertificateChain,            1),
    WX_STD("CryptProtectData",                     shim_CryptProtectData,                     7),
    WX_STD("CryptUnprotectData",                   shim_CryptUnprotectData,                   7),
    { NULL, NULL, 0, CC_STDCALL }
};

static const WIN32_EXPORT ncrypt_exports[] = {
    WX_STD("NCryptCreatePersistedKey", shim_NCryptCreatePersistedKey, 6),
    WX_STD("NCryptDeleteKey",          shim_NCryptDeleteKey,          2),
    WX_STD("NCryptExportKey",          shim_NCryptExportKey,          8),
    WX_STD("NCryptFinalizeKey",        shim_NCryptFinalizeKey,        2),
    WX_STD("NCryptFreeObject",         shim_NCryptFreeObject,         1),
    WX_STD("NCryptGetProperty",        shim_NCryptGetProperty,        6),
    WX_STD("NCryptImportKey",          shim_NCryptImportKey,          8),
    WX_STD("NCryptIsAlgSupported",     shim_NCryptIsAlgSupported,     3),
    WX_STD("NCryptOpenKey",            shim_NCryptOpenKey,            5),
    WX_STD("NCryptOpenStorageProvider", shim_NCryptOpenStorageProvider, 3),
    WX_STD("NCryptSignHash",           shim_NCryptSignHash,           8),
    { NULL, NULL, 0, CC_STDCALL }
};

static const WIN32_EXPORT wintrust_exports[] = {
    WX_STD("WinVerifyTrust", shim_WinVerifyTrust, 3),
    WX_STD("WinVerifyTrustEx", shim_WinVerifyTrust, 3),
    WX_STD("WTHelperProvDataFromStateData", shim_WTHelperProvDataFromStateData, 1),
    WX_STD("WTHelperGetProvSignerFromChain", shim_WTHelperGetProvSignerFromChain, 4),
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *crypt32_abi_table(int *count)
{
    *count = (int)(sizeof(crypt32_exports) / sizeof(crypt32_exports[0]));
    return crypt32_exports;
}

const WIN32_EXPORT *ncrypt_abi_table(int *count)
{
    *count = (int)(sizeof(ncrypt_exports) / sizeof(ncrypt_exports[0]));
    return ncrypt_exports;
}

const WIN32_EXPORT *wintrust_abi_table(int *count)
{
    *count = (int)(sizeof(wintrust_exports) / sizeof(wintrust_exports[0]));
    return wintrust_exports;
}

static int crypt32_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID crypt32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal || !func_name) return NULL;
    for (int i = 0; crypt32_exports[i].name; i++)
        if (crypt32_strcmp(func_name, crypt32_exports[i].name) == 0)
            return crypt32_exports[i].func;
    SetLastError(CRYPT_E_NOT_FOUND);
    return NULL;
}

PVOID ncrypt_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal || !func_name) return NULL;
    for (int i = 0; ncrypt_exports[i].name; i++)
        if (crypt32_strcmp(func_name, ncrypt_exports[i].name) == 0)
            return ncrypt_exports[i].func;
    SetLastError(CRYPT_E_NOT_FOUND);
    return NULL;
}

PVOID wintrust_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal || !func_name) return NULL;
    for (int i = 0; wintrust_exports[i].name; i++)
        if (crypt32_strcmp(func_name, wintrust_exports[i].name) == 0)
            return wintrust_exports[i].func;
    SetLastError(CRYPT_E_NOT_FOUND);
    return NULL;
}
