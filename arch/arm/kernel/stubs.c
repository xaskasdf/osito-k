/*
 * stubs.c -- Stub/redirect functions for x86-specific features on ARM64
 *
 * AVX2 tensor ops: never called (avx2_detected=0) but linker needs symbols.
 * proc_exec: redirects to proc_run (ARM process model).
 * syscall_capture: now implemented in syscall.c (removed from stubs).
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "../drivers/gpu.h"

/* GPU device global (populated by PCI scan if GPU found) */
gpu_device_t gpu_dev;

/* GPU PCI accessor — returns pointer to gpu_dev if populated, else NULL */
gpu_device_t *pci_get_gpu(void)
{
    if (gpu_dev.vendor_id != 0)
        return &gpu_dev;
    return (gpu_device_t *)0;
}

/* AVX2 stubs → redirect to NEON implementations */
extern void rmsnorm_neon(float *o, const float *x, const float *w, int n);
extern void matvec_q4_0_neon(float *out, const void *w, const float *x, int n, int d);
extern void vec_add_neon(float *a, const float *b, int n);
extern void vec_mul_neon(float *a, const float *b, int n);

void rmsnorm_avx2(float *o, const float *x, const float *w, int n) { rmsnorm_neon(o, x, w, n); }
void matvec_q4_0_avx2(float *out, const void *w, const float *x, int n, int d) { matvec_q4_0_neon(out, w, x, n, d); }
void vec_add_avx2(float *a, const float *b, int n) { vec_add_neon(a, b, n); }
void vec_mul_avx2(float *a, const float *b, int n) { vec_mul_neon(a, b, n); }

/* Process execution — redirect to ARM's proc_run */
extern int proc_run(const char *filename);
int proc_exec(const char *path, const char **argv, const char **envp)
{
    (void)argv; (void)envp;
    return proc_run(path);
}
