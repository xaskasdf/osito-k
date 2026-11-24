/*
 * Minimal setjmp/longjmp for Xtensa LX106 (CALL0 ABI)
 *
 * Saves/restores: a0 (retaddr), a1 (SP), a12-a15 (callee-saved).
 * jmp_buf = 6 × uint32_t = 24 bytes.
 */
#ifndef OSITO_SETJMP_H
#define OSITO_SETJMP_H

#include <stdint.h>

typedef uint32_t jmp_buf[6];

#ifdef __cplusplus
extern "C" {
#endif

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* OSITO_SETJMP_H */
