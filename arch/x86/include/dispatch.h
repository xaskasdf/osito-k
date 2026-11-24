/*
 * arch/x86/include/dispatch.h — function pointer table for SIMD variants
 *
 * Wired at boot from cpu_features. Callers (inference.c, net.c, ...) can
 * take an indirect branch once instead of runtime feature-checking.
 *
 * Current implementations:
 *   matvec_q4_0: scalar | AVX2+FMA (tensor_avx2.c has the AVX2 kernel;
 *                tensor.c's matvec_q4_0 wrapper still does the dispatch,
 *                which this table makes explicit for future use).
 *   memcpy_fast: plain | rep movsb (ERMS)
 *
 * Future slots: matvec_q4_0_avx512, matvec_q4_0_f16c, softmax_avx2, ...
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
