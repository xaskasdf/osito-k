/*
 * stubs.c -- Stub/redirect functions for x86-specific features on ARM64
 *
 * AVX2 tensor ops: never called (avx2_detected=0) but linker needs symbols.
 * proc_exec: redirects to proc_run (ARM process model).
 * syscall_capture: now implemented in syscall.c (removed from stubs).
 */

#include "../include/hal.h"
#include "../include/types.h"

/* AVX2 tensor stubs (never called — avx2_detected=0 on ARM64) */
void rmsnorm_avx2(float *o, const float *x, const float *w, int n) { (void)o; (void)x; (void)w; (void)n; }
void matvec_q4_0_avx2(float *out, const void *w, const float *x, int n, int d) { (void)out; (void)w; (void)x; (void)n; (void)d; }
void vec_add_avx2(float *a, const float *b, int n) { (void)a; (void)b; (void)n; }
void vec_mul_avx2(float *a, const float *b, int n) { (void)a; (void)b; (void)n; }

/* Process execution — redirect to ARM's proc_run */
extern int proc_run(const char *filename);
int proc_exec(const char *path, const char **argv, const char **envp)
{
    (void)argv; (void)envp;
    return proc_run(path);
}
