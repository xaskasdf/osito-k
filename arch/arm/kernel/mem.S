/*
 * mem.S -- out-of-line memset / memcpy / memmove
 *
 * include/types.h has static inline versions for kernel code, but GCC
 * requires a freestanding environment to provide these symbols anyway: it
 * lowers struct copies and zero-initialization to calls, and the shared gui/
 * code calls them directly. Written in assembly so they can't clash with the
 * static inline definitions or get turned back into calls to themselves.
 *
 * Aligned accesses only: with the MMU off (SM8350 boot) every data access
 * is Device memory, where an unaligned access faults.
 */

.text

/* void *memset(void *s, int c, size_t n) */
.globl memset
.type memset, %function
memset:
    mov     x3, x0
    and     w1, w1, #0xff
    orr     w1, w1, w1, lsl #8
    orr     w1, w1, w1, lsl #16
    orr     x1, x1, x1, lsl #32     // byte replicated to 64 bits
1:  cbz     x2, 4f                  // head: bytes until 8-aligned
    tst     x3, #7
    b.eq    2f
    strb    w1, [x3], #1
    sub     x2, x2, #1
    b       1b
2:  cmp     x2, #8                  // body: 8 bytes at a time
    b.lo    3f
    str     x1, [x3], #8
    sub     x2, x2, #8
    b       2b
3:  cbz     x2, 4f                  // tail
    strb    w1, [x3], #1
    sub     x2, x2, #1
    b       3b
4:  ret
.size memset, . - memset

/* void *memcpy(void *dst, const void *src, size_t n) */
.globl memcpy
.type memcpy, %function
memcpy:
    mov     x3, x0
    eor     x4, x0, x1              // 8-byte chunks only if dst and src
    tst     x4, #7                  // can be aligned together
    b.ne    3f
1:  cbz     x2, 4f
    tst     x3, #7
    b.eq    2f
    ldrb    w4, [x1], #1
    strb    w4, [x3], #1
    sub     x2, x2, #1
    b       1b
2:  cmp     x2, #8
    b.lo    3f
    ldr     x4, [x1], #8
    str     x4, [x3], #8
    sub     x2, x2, #8
    b       2b
3:  cbz     x2, 4f
    ldrb    w4, [x1], #1
    strb    w4, [x3], #1
    sub     x2, x2, #1
    b       3b
4:  ret
.size memcpy, . - memcpy

/* void *memmove(void *dst, const void *src, size_t n) */
.globl memmove
.type memmove, %function
memmove:
    cmp     x0, x1
    b.ls    memcpy                  // dst <= src: forward copy is safe
    add     x4, x1, x2
    cmp     x0, x4
    b.hs    memcpy                  // no overlap
    add     x3, x0, x2              // dst > src, overlapping: copy backwards
1:  cbz     x2, 2f
    ldrb    w5, [x4, #-1]!
    strb    w5, [x3, #-1]!
    sub     x2, x2, #1
    b       1b
2:  ret
.size memmove, . - memmove
