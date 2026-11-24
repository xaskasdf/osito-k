/* Minimal self-contained 32-bit cdecl qsort + bsearch, compiled to a
 * position-independent .text blob (no libc, no globals, no relocations) that the
 * win32 layer installs in PE-executable memory and points the MSVCRT qsort/
 * bsearch imports at — so the guest calls them natively in 32-bit mode and the
 * comparator runs 32->32 native (no INT2E / no compat32 callback / no IST1
 * round-trip). Fixes B3 without the New-Game triple-fault.
 *
 * Build (extract .text bytes):
 *   clang --target=i386-unknown-none -m32 -O2 -ffreestanding -fno-pic \
 *         -fno-stack-protector -fcf-protection=none -c qsort32.c -o qsort32.o
 *   llvm-objcopy -O binary -j .text qsort32.o qsort32.bin
 *   (verify: llvm-objdump -dr qsort32.o  -> no relocations)
 *
 * Insertion sort (O(n^2) but the UE1 scene sort is ~64 elements; correct, stable,
 * non-recursive -> bounded stack). bsearch is a plain binary search.
 */
typedef int (*cmp_t)(const void *, const void *);

/* qsort MUST be the first symbol so .text starts at it (offset 0 in the blob). */
void qsort(void *base, unsigned num, unsigned width, cmp_t cmp)
{
    char *a = (char *)base;
    for (unsigned i = 1; i < num; i++) {
        for (unsigned j = i; j > 0; j--) {
            char *p = a + (j - 1) * width;
            char *q = a + j * width;
            if (cmp(p, q) <= 0) break;
            for (unsigned k = 0; k < width; k++) {
                char t = p[k]; p[k] = q[k]; q[k] = t;
            }
        }
    }
}

void *bsearch(const void *key, const void *base, unsigned num,
              unsigned width, cmp_t cmp)
{
    const char *a = (const char *)base;
    unsigned lo = 0, hi = num;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        const char *m = a + mid * width;
        int c = cmp(key, m);
        if (c == 0) return (void *)m;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}
