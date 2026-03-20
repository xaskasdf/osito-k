/*
 * stubs.c -- Stub functions for x86-specific features not on ARM64
 *
 * AVX2 tensor ops: never called (avx2_detected=0) but linker needs symbols.
 * syscall_capture: used by claude.c tool use, stubbed for now.
 * proc_run_cmd: used by claude.c, stubbed (ARM has proc_run instead).
 */

#include "../include/types.h"

/* AVX2 tensor stubs (never called — avx2_detected=0 on ARM64) */
void rmsnorm_avx2(float *o, const float *x, const float *w, int n) { (void)o; (void)x; (void)w; (void)n; }
void matvec_q4_0_avx2(float *out, const void *w, const float *x, int n, int d) { (void)out; (void)w; (void)x; (void)n; (void)d; }
void vec_add_avx2(float *a, const float *b, int n) { (void)a; (void)b; (void)n; }
void vec_mul_avx2(float *a, const float *b, int n) { (void)a; (void)b; (void)n; }

/* Claude tool output capture stubs */
void syscall_capture_start(void) {}
void syscall_capture_stop(char *buf, int bufsize) { (void)buf; (void)bufsize; if (buf && bufsize > 0) buf[0] = 0; }

/* Process stub (ARM has proc_run) */
int proc_exec(const char *path, const char **argv, const char **envp) { (void)path; (void)argv; (void)envp; return -1; }
