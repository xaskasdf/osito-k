#ifndef OSITOK_WIN32_AUTHENTICODE_H
#define OSITOK_WIN32_AUTHENTICODE_H

#include "nttypes.h"

typedef enum {
    AUTHENTICODE_VERIFY_OK = 0,
    AUTHENTICODE_VERIFY_NO_SIGNATURE,
    AUTHENTICODE_VERIFY_BAD_DIGEST,
    AUTHENTICODE_VERIFY_BAD_SIGNATURE,
    AUTHENTICODE_VERIFY_UNTRUSTED,
    AUTHENTICODE_VERIFY_UNSUPPORTED,
    AUTHENTICODE_VERIFY_IO_ERROR,
} authenticode_result_t;

#define AUTHENTICODE_CHAIN_MAX 12U
typedef struct {
    const BYTE *certificates[AUTHENTICODE_CHAIN_MAX];
    DWORD lengths[AUTHENTICODE_CHAIN_MAX];
    DWORD errors[AUTHENTICODE_CHAIN_MAX];
    DWORD count;
    DWORD validation_time;
} authenticode_chain_t;

typedef struct {
    authenticode_chain_t signer;
    authenticode_chain_t timestamp;
} authenticode_report_t;

/* The callback runs only after digest, signatures and both applicable chain
 * policies succeeded. DER pointers are borrowed only during the callback. */
typedef BOOL (*authenticode_chain_sink_t)(const authenticode_report_t *, PVOID);
authenticode_result_t authenticode_verify_file_report(
    PCWSTR path, HANDLE supplied_file, DWORD revocation_checks,
    DWORD provider_flags, DWORD software_policy,
    authenticode_chain_sink_t sink, PVOID sink_context);

authenticode_result_t authenticode_verify_file(PCWSTR path, HANDLE supplied_file,
                                                DWORD revocation_checks,
                                                DWORD provider_flags);

#endif
