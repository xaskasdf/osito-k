/* External memcpy/memset/memcmp symbols for compiler-generated calls.
 *
 * The kernel headers provide these three as `static inline` — perfect for
 * explicit calls (they get inlined), but they emit no *external* symbol.
 * A compiler may still generate an external call to them: clang lowers large
 * struct assignments (e.g. `saved_boot_info = *info`) to a `memcpy` call, and
 * TCC emits memcpy/memset for struct copies.  GCC happens to inline those, so
 * the gap only shows up on clang/TCC as `undefined symbol: memcpy`.
 *
 * This translation unit provides the external definitions.  It deliberately
 * includes NO header — the kernel's `-Iinclude` would otherwise resolve
 * <stdint.h>/<stddef.h> to kernel headers that re-pull the static-inline
 * versions (redefinition).  `size_t` comes from the compiler builtin
 * __SIZE_TYPE__ so it matches the target exactly.  The `volatile` pointers
 * stop the optimiser's loop-idiom / loop-distribution passes from lowering
 * these loops back into calls to themselves (infinite recursion).
 *
 * (memmove already has an external definition in memory.c.)
 */
typedef __SIZE_TYPE__ size_t;

void *memcpy(void *dst, const void *src, size_t n)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const volatile unsigned char *pa = (const volatile unsigned char *)a;
    const volatile unsigned char *pb = (const volatile unsigned char *)b;
    for (size_t i = 0; i < n; i++)
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    return 0;
}
