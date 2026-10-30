/*
 * The two string functions BearSSL calls that the kernel has only as
 * static inlines (include/types.h); memcpy, memmove and memset are mem.S's.
 */
#include "string.h"

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    for (size_t i = 0; i < n; i++)
        if (pa[i] != pb[i])
            return pa[i] - pb[i];
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}
