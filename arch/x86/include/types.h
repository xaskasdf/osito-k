/*
 * OsitoK x86-64 — Freestanding type definitions
 *
 * No libc dependency. Used by all kernel code.
 */

#ifndef OSITOK_X86_TYPES_H
#define OSITOK_X86_TYPES_H

#define __KERNEL_X86__ 1

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

typedef uint64_t            uintptr_t;
typedef int64_t             intptr_t;
typedef uint64_t            size_t;
typedef int64_t             ssize_t;

#if !defined(__bool_true_false_are_defined) && !defined(__cplusplus) && (__STDC_VERSION__ < 202311L)
typedef _Bool               bool;
#define true  1
#define false 0
#define __bool_true_false_are_defined 1
#endif

#define NULL ((void *)0)

#define UINT32_MAX 0xFFFFFFFFU
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL

/* Volatile MMIO access */
static inline void mmio_write32(volatile void *addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(volatile void *addr) {
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write64(volatile void *addr, uint64_t val) {
    *(volatile uint64_t *)addr = val;
}

static inline uint64_t mmio_read64(volatile void *addr) {
    return *(volatile uint64_t *)addr;
}

/* Port I/O */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* Memory barriers */
#define mb()  __asm__ volatile ("mfence" ::: "memory")
#define wmb() __asm__ volatile ("sfence" ::: "memory")
#define rmb() __asm__ volatile ("lfence" ::: "memory")

/* Memset/memcpy — minimal freestanding versions */
static inline void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

static inline void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

static inline size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

static inline char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

static inline int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

#endif /* OSITOK_X86_TYPES_H */
