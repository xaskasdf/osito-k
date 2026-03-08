/* OsitoK shim — setjmp.h */
#ifndef _SETJMP_H
#define _SETJMP_H
typedef long jmp_buf[8]; /* 64 bytes — matches syscall.S */
int _setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
#define setjmp(env) _setjmp(env)
#endif
