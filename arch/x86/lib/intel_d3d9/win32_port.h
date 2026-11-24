#ifndef INTEL_D3D9_WIN32_PORT_H
#define INTEL_D3D9_WIN32_PORT_H

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *intel_d3d9_memmem(const void *data, size_t bytes,
                       const void *pattern, size_t pattern_bytes);
#ifdef __cplusplus
}
#endif

#ifdef _WIN32
#include "util/u_string.h"
/* Newer MinGW supplies strndup; older toolchains need Mesa's fallback. */
#if INTEL_D3D9_HAVE_STRNDUP
#define STRNDUP_H
#else
#include "util/strndup.h"
#endif
#define memmem intel_d3d9_memmem
#endif
#endif
