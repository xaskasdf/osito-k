/*
 * setjmp / longjmp for Xtensa LX106 CALL0 ABI
 *
 * jmp_buf layout (6 × 4 = 24 bytes):
 *   [0]  a0   return address
 *   [4]  a1   stack pointer
 *   [8]  a12  callee-saved
 *   [12] a13  callee-saved
 *   [16] a14  callee-saved
 *   [20] a15  callee-saved
 */

    .text
    .align 4
    .global setjmp
    .type   setjmp, @function
setjmp:
    /* a2 = pointer to jmp_buf */
    s32i    a0,  a2, 0
    s32i    a1,  a2, 4
    s32i    a12, a2, 8
    s32i    a13, a2, 12
    s32i    a14, a2, 16
    s32i    a15, a2, 20
    movi    a2, 0           /* return 0 on first call */
    ret

    .align 4
    .global longjmp
    .type   longjmp, @function
longjmp:
    /* a2 = pointer to jmp_buf, a3 = return value */
    l32i    a0,  a2, 0
    l32i    a1,  a2, 4
    l32i    a12, a2, 8
    l32i    a13, a2, 12
    l32i    a14, a2, 16
    l32i    a15, a2, 20
    mov     a2, a3          /* set return value */
    bnez    a2, 1f
    movi    a2, 1           /* if val==0, return 1 (C standard) */
1:
    ret
