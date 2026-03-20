/*
 * types.h -- Minimal freestanding type definitions for Osito-K AArch64
 *
 * No libc — inline implementations of memset, memcpy, strlen, strcmp, strncmp.
 */

#ifndef OSITO_ARM_TYPES_H
#define OSITO_ARM_TYPES_H

#include <stdint.h>
#include <stddef.h>

static inline void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    uint8_t val = (uint8_t)c;
    /* Use 64-bit stores for aligned bulk fills */
    if (n >= 16 && ((uintptr_t)p & 7) == 0) {
        uint64_t v64 = val;
        v64 |= v64 << 8; v64 |= v64 << 16; v64 |= v64 << 32;
        uint64_t *p64 = (uint64_t *)p;
        size_t qwords = n / 8;
        for (size_t i = 0; i < qwords; i++) p64[i] = v64;
        p += qwords * 8;
        n -= qwords * 8;
    }
    while (n--) *p++ = val;
    return s;
}

static inline void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

static inline int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

static inline int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? *(unsigned char *)a - *(unsigned char *)b : 0;
}

#endif /* OSITO_ARM_TYPES_H */
