/*
 * stdlib.c -- memory, conversions, sorting, exit (Osito-K AArch64 libc)
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── malloc: first fit over the brk heap ────────────────────────
 * Every block starts with a 16-byte header: its size (header included,
 * a multiple of 16) and whether it is free. A search merges runs of free
 * blocks as it walks them; the heap only grows. Programs are short-lived
 * and the kernel frees the whole heap region when they end. */

typedef struct {
    size_t size;
    size_t free;
} block_t;

#define HDR         sizeof(block_t)
#define ALIGN(n)    (((n) + 15) & ~(size_t)15)

static uint8_t *heap_lo, *heap_hi;

void *malloc(size_t n)
{
    if (n > ((size_t)1 << 40))
        return 0;
    size_t need = ALIGN(n ? n : 1) + HDR;
    for (uint8_t *p = heap_lo; p && p < heap_hi; p += ((block_t *)p)->size) {
        block_t *b = (block_t *)p;
        if (!b->free)
            continue;
        while (p + b->size < heap_hi && ((block_t *)(p + b->size))->free)
            b->size += ((block_t *)(p + b->size))->size;        /* merge */
        if (b->size < need)
            continue;
        if (b->size >= need + HDR + 16) {                      /* split */
            block_t *rest = (block_t *)(p + need);
            rest->size = b->size - need;
            rest->free = 1;
            b->size = need;
        }
        b->free = 0;
        return p + HDR;
    }
    uint8_t *p = sbrk((intptr_t)need);
    if (p == (void *)-1) {
        errno = ENOMEM;
        return 0;
    }
    if (!heap_lo)
        heap_lo = p;
    heap_hi = p + need;
    block_t *b = (block_t *)p;
    b->size = need;
    b->free = 0;
    return p + HDR;
}

void free(void *ptr)
{
    if (ptr)
        ((block_t *)((uint8_t *)ptr - HDR))->free = 1;
}

void *calloc(size_t count, size_t n)
{
    if (n && count > ((size_t)1 << 40) / n)
        return 0;
    void *p = malloc(count * n);
    if (p)
        memset(p, 0, count * n);
    return p;
}

void *realloc(void *ptr, size_t n)
{
    if (!ptr)
        return malloc(n);
    block_t *b = (block_t *)((uint8_t *)ptr - HDR);
    if (b->size - HDR >= n)
        return ptr;
    void *fresh = malloc(n);
    if (fresh) {
        memcpy(fresh, ptr, b->size - HDR);
        free(ptr);
    }
    return fresh;
}

/* ── Numbers ────────────────────────────────────────────────── */

unsigned long long strtoull(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '+')
        p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = *p == '0' ? 8 : 10;
    }
    unsigned long long v = 0;
    const char *start = p;
    for (;; p++) {
        int d = isdigit((unsigned char)*p) ? *p - '0' :
                isalpha((unsigned char)*p) ? tolower((unsigned char)*p) - 'a' + 10 : 99;
        if (d >= base)
            break;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (end)
        *end = (char *)(p == start ? s : p);
    return v;
}

long long strtoll(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace((unsigned char)*p))
        p++;
    int neg = *p == '-';
    if (neg)
        p++;
    char *e;
    unsigned long long v = strtoull(p, &e, base);
    if (end)
        *end = e == p ? (char *)s : e;
    return neg ? -(long long)v : (long long)v;
}

long strtol(const char *s, char **end, int base)           { return (long)strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
int atoi(const char *s)        { return (int)strtol(s, 0, 10); }
long atol(const char *s)       { return strtol(s, 0, 10); }
long long atoll(const char *s) { return strtoll(s, 0, 10); }

double strtod(const char *s, char **end)
{
    const char *p = s;
    while (isspace((unsigned char)*p))
        p++;
    double sign = 1;
    if (*p == '-' || *p == '+')
        sign = *p++ == '-' ? -1 : 1;
    double v = 0;
    int digits = 0;
    for (; isdigit((unsigned char)*p); p++, digits++)
        v = v * 10 + (*p - '0');
    if (*p == '.') {
        double scale = 0.1;
        for (p++; isdigit((unsigned char)*p); p++, digits++, scale /= 10)
            v += (*p - '0') * scale;
    }
    if (digits && (*p == 'e' || *p == 'E')) {
        char *e;
        long x = strtol(p + 1, &e, 10);
        if (e != p + 1) {
            p = e;
            for (; x > 0; x--) v *= 10;
            for (; x < 0; x++) v /= 10;
        }
    }
    if (end)
        *end = (char *)(digits ? p : s);
    return sign * v;
}

double atof(const char *s) { return strtod(s, 0); }

int abs(int x)              { return x < 0 ? -x : x; }
long labs(long x)           { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

static unsigned long long rand_state = 1;

void srand(unsigned seed) { rand_state = seed ? seed : 1; }

int rand(void)
{
    rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)(rand_state >> 33);
}

/* ── Sorting and searching ──────────────────────────────────── */

static void swap_bytes(uint8_t *a, uint8_t *b, size_t n)
{
    while (n--) {
        uint8_t t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* Quicksort, median of three, insertion sort below 8 elements */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    uint8_t *a = base;
    while (n > 7) {
        uint8_t *lo = a, *mid = a + (n / 2) * size, *hi = a + (n - 1) * size;
        if (cmp(mid, lo) < 0) swap_bytes(mid, lo, size);
        if (cmp(hi, mid) < 0) swap_bytes(hi, mid, size);
        if (cmp(mid, lo) < 0) swap_bytes(mid, lo, size);
        swap_bytes(mid, hi - size, size);           /* pivot next to the end */
        uint8_t *pivot = hi - size, *i = lo, *j = pivot;
        for (;;) {
            do i += size; while (cmp(i, pivot) < 0);
            do j -= size; while (j > lo && cmp(j, pivot) > 0);
            if (i >= j)
                break;
            swap_bytes(i, j, size);
        }
        swap_bytes(i, pivot, size);
        size_t left = (size_t)(i - a) / size, right = n - left - 1;
        /* Recurse into the smaller side, loop over the larger */
        if (left < right) {
            qsort(a, left, size, cmp);
            a = i + size;
            n = right;
        } else {
            qsort(i + size, right, size, cmp);
            n = left;
        }
    }
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(a + (j - 1) * size, a + j * size) > 0; j--)
            swap_bytes(a + (j - 1) * size, a + j * size, size);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *))
{
    const uint8_t *a = base;
    while (n) {
        const uint8_t *mid = a + (n / 2) * size;
        int c = cmp(key, mid);
        if (!c)
            return (void *)mid;
        if (c > 0) {
            a = mid + size;
            n = n - n / 2 - 1;
        } else {
            n /= 2;
        }
    }
    return 0;
}

/* ── Environment and exit ───────────────────────────────────── */

char *getenv(const char *name)
{
    (void)name;
    return 0;
}

#define MAX_ATEXIT 32
static void (*atexit_fn[MAX_ATEXIT])(void);
static int atexit_count;

int atexit(void (*fn)(void))
{
    if (atexit_count == MAX_ATEXIT)
        return -1;
    atexit_fn[atexit_count++] = fn;
    return 0;
}

void exit(int status)
{
    while (atexit_count)
        atexit_fn[--atexit_count]();
    fflush(stdout);
    fflush(stderr);
    _exit(status);
}

void _Exit(int status) { _exit(status); }

void abort(void)
{
    fflush(stdout);
    write(2, "abort\n", 6);
    _exit(134);
}
