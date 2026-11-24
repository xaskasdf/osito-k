/*
 * OsitoK - Freestanding type definitions
 */
#ifndef OSITO_TYPES_H
#define OSITO_TYPES_H

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;

typedef uint32_t size_t;
typedef int32_t  ssize_t;
typedef int32_t  ptrdiff_t;

#ifndef __cplusplus
typedef _Bool bool;
#define true  1
#define false 0
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void*)0)
#endif
#endif

#define INLINE       static inline __attribute__((always_inline))
#define NOINLINE     __attribute__((noinline))
#define NORETURN     __attribute__((noreturn))
#define PACKED       __attribute__((packed))
#define ALIGNED(n)   __attribute__((aligned(n)))
#define WEAK         __attribute__((weak))
#define USED         __attribute__((used))
#define SECTION(s)   __attribute__((section(s)))

/* Barrier macros */
#define MEMORY_BARRIER()  __asm__ volatile("memw" ::: "memory")
#define ISR_BARRIER()     __asm__ volatile("isync" ::: "memory")

/* Xtensa special register access */
INLINE uint32_t RSR(int reg) {
    uint32_t val;
    switch (reg) {
        case 230: __asm__ volatile("rsr %0, ps"   : "=a"(val)); break; /* PS */
        case  3:  __asm__ volatile("rsr %0, sar"  : "=a"(val)); break; /* SAR */
        case 177: __asm__ volatile("rsr %0, epc1" : "=a"(val)); break; /* EPC1 */
        default: val = 0; break;
    }
    return val;
}

INLINE void WSR(int reg, uint32_t val) {
    switch (reg) {
        case 230: __asm__ volatile("wsr %0, ps"   :: "a"(val)); break;
        case  3:  __asm__ volatile("wsr %0, sar"  :: "a"(val)); break;
        case 177: __asm__ volatile("wsr %0, epc1" :: "a"(val)); break;
        case 209: __asm__ volatile("wsr %0, vecbase" :: "a"(val)); break;
    }
    ISR_BARRIER();
}

/* Interrupt enable/disable (returns old PS) */
INLINE uint32_t irq_save(void) {
    uint32_t old_ps;
    __asm__ volatile("rsil %0, 15" : "=a"(old_ps));
    return old_ps;
}

INLINE void irq_restore(uint32_t ps) {
    __asm__ volatile("wsr %0, ps; isync" :: "a"(ps));
}

#endif /* OSITO_TYPES_H */
