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

authenticode_result_t authenticode_verify_file(PCWSTR path, HANDLE supplied_file,
                                                DWORD revocation_checks,
                                                DWORD provider_flags);

#endif
