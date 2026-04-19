/*
 * arch/x86/kernel/dispatch.c — CPUID-driven function pointer table
 *
 * Wires the `disp` table from cpu_features at boot. The hot inference
 * path (tensor.c:matvec_q4_0) already dispatches via a cached has_avx2
 * flag, so this table is primarily for future SIMD variants and for
 * memcpy/memset that want ERMS when available.
 */

#include "../include/dispatch.h"
#include "../include/cpu_features.h"

extern void serial_puts(const char *s);

/* Existing externs from tensor.c / tensor_avx2.c. The scalar versions
 * are exposed via the public wrappers (matvec_q4_0, rmsnorm, etc.) which
 * themselves do the dispatch — so we just point at those for now. */
extern void matvec_q4_0(float *, const void *, const float *, uint32_t, uint32_t);
extern void matvec_q8_0(float *, const void *, const float *, uint32_t, uint32_t);
extern void rmsnorm   (float *, const float *, const float *, uint32_t);
extern void vec_add   (float *, const float *, const float *, uint32_t);
extern void vec_mul   (float *, const float *, const float *, uint32_t);
/* memcpy/memset are provided by libc; callers that don't need ERMS can
 * just call them directly. We only table-ize the ERMS fast paths. */

cpu_dispatch_t disp;

/* ERMS memcpy: one REP MOVSB, skips the per-byte loop in software memcpy.
 * Much faster for >=64B copies on modern CPUs (Ivy Bridge+). */
static void *memcpy_erms(void *dst, const void *src, uint64_t n)
{
    void *ret = dst;
    __asm__ volatile("rep movsb"
        : "+D"(dst), "+S"(src), "+c"(n)
        :
        : "memory");
    return ret;
}

static void *memset_erms(void *dst, int c, uint64_t n)
{
    void *ret = dst;
    __asm__ volatile("rep stosb"
        : "+D"(dst), "+c"(n)
        : "a"((uint8_t)c)
        : "memory");
    return ret;
}

void dispatch_init(void)
{
    /* Tensor math: defer to existing wrappers (they already dispatch
     * via tensor_has_avx2 cache). Expose through disp so future code
     * can take a direct indirect call. */
    disp.matvec_q4_0 = matvec_q4_0;
    disp.matvec_q8_0 = matvec_q8_0;
    disp.rmsnorm     = rmsnorm;
    disp.vec_add     = vec_add;
    disp.vec_mul     = vec_mul;

    /* Memory ops: use ERMS when available; else leave NULL — callers
     * then fall back to plain memcpy/memset directly. */
    disp.memcpy_fast = cpu_features.erms ? memcpy_erms : 0;
    disp.memset_fast = cpu_features.erms ? memset_erms : 0;

    /* Name the path for logs */
    if (cpu_features.avx512f) {
        disp.path_name = "AVX-512";
    } else if (cpu_features.avx2 && cpu_features.fma) {
        disp.path_name = cpu_features.f16c ? "AVX2+FMA+F16C" : "AVX2+FMA";
    } else if (cpu_features.avx) {
        disp.path_name = "AVX";
    } else if (cpu_features.sse4_2) {
        disp.path_name = "SSE4.2";
    } else {
        disp.path_name = "scalar";
    }

    serial_puts("[DISPATCH] compute path: ");
    serial_puts(disp.path_name);
    if (cpu_features.erms) serial_puts(" +ERMS-memcpy");
    serial_puts("\n");
}
