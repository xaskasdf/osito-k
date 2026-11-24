/*
 * dispatch.h — function pointer table for SIMD variants (from arch/x86)
 *
 * inference.c calls the matvecs through it. On AArch64, dispatch.c points
 * the slots at tensor.c's wrappers, which pick NEON or scalar by
 * tensor_simd; llm.c fills it before a model loads.
 */
#ifndef OSITOK_DISPATCH_H
#define OSITOK_DISPATCH_H

#include "types.h"
#include "stdint.h"

typedef struct {
    /* Quantized matvec (primary hot path in inference) */
    void  (*matvec_q4_0)(float *out, const void *w, const float *in,
                         uint32_t rows, uint32_t cols);
    void  (*matvec_q8_0)(float *out, const void *w, const float *in,
                         uint32_t rows, uint32_t cols);

    /* Vector math */
    void  (*rmsnorm)  (float *out, const float *x, const float *w, uint32_t n);
    void  (*vec_add)  (float *out, const float *a, const float *b, uint32_t n);
    void  (*vec_mul)  (float *out, const float *a, const float *b, uint32_t n);

    /* Memory ops */
    void *(*memcpy_fast)(void *dst, const void *src, uint64_t n);
    void *(*memset_fast)(void *dst, int c, uint64_t n);

    /* Human-readable label e.g. "AVX2+FMA" */
    const char *path_name;
} cpu_dispatch_t;

extern cpu_dispatch_t disp;

void dispatch_init(void);

#endif /* OSITOK_DISPATCH_H */
