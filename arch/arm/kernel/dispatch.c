/*
 * dispatch.c -- the inference's function table (include/dispatch.h)
 *
 * On x86 this picks AVX2 / AVX-512 / ERMS paths from cpu_features. Here the
 * tensor wrappers (tensor.c) already choose NEON or scalar by tensor_simd,
 * so the table points at them; memcpy_fast/memset_fast stay null, which
 * means "use memcpy/memset". llm.c calls dispatch_init before a model loads.
 * It also turns on the SDOT matvecs (tensor_sdot.c) where the CPU has them.
 */

#include "../include/dispatch.h"
#include "tensor_sdot.h"

extern void serial_puts(const char *s);
extern int tensor_simd;

extern void matvec_q4_0(float *, const void *, const float *, uint32_t, uint32_t);
extern void matvec_q8_0(float *, const void *, const float *, uint32_t, uint32_t);
extern void rmsnorm   (float *, const float *, const float *, uint32_t);
extern void vec_add   (float *, const float *, const float *, uint32_t);
extern void vec_mul   (float *, const float *, const float *, uint32_t);

cpu_dispatch_t disp;

void dispatch_init(void)
{
    disp.matvec_q4_0 = matvec_q4_0;
    disp.matvec_q8_0 = matvec_q8_0;
    disp.rmsnorm     = rmsnorm;
    disp.vec_add     = vec_add;
    disp.vec_mul     = vec_mul;
    disp.memcpy_fast = 0;
    disp.memset_fast = 0;
    tensor_sdot = tensor_simd && tensor_sdot_wanted && tensor_sdot_detect();
    disp.path_name   = tensor_sdot ? "NEON + SDOT" : tensor_simd ? "NEON" : "scalar";
}
