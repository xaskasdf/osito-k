/*
 * string.c -- string and memory functions (Osito-K AArch64 libc)
 */

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (!(((uintptr_t)d | (uintptr_t)s) & 7)) {
        for (; n >= 8; n -= 8, d += 8, s += 8)
            *(uint64_t *)d = *(const uint64_t *)s;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);
    while (n--)
        d[n] = s[n];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint64_t v = (uint8_t)c * 0x0101010101010101ULL;
    while (n && ((uintptr_t)d & 7)) {
        *d++ = (uint8_t)c;
        n--;
    }
    for (; n >= 8; n -= 8, d += 8)
        *(uint64_t *)d = v;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

void *memchr(const void *p, int c, size_t n)
{
    const uint8_t *s = p;
    for (; n; n--, s++)
        if (*s == (uint8_t)c)
            return (void *)s;
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src)
        *d++ = *src++;
    *d = 0;
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
        a++, b++;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return (char *)hay;
    for (; *hay; hay++)
        if (*hay == *needle && !strncmp(hay, needle, n))
            return (char *)hay;
    return 0;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    s += strcspn(s, accept);
    return *s ? (char *)s : 0;
}

char *strtok_r(char *s, const char *delim, char **save)
{
    if (!s)
        s = *save;
    s += strspn(s, delim);
    if (!*s) {
        *save = s;
        return 0;
    }
    char *end = s + strcspn(s, delim);
    if (*end)
        *end++ = 0;
    *save = end;
    return s;
}

char *strtok(char *s, const char *delim)
{
    static char *save;
    return strtok_r(s, delim, &save);
}

char *strdup(const char *s)
{
    return strndup(s, strlen(s));
}

char *strndup(const char *s, size_t n)
{
    n = strnlen(s, n);
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

char *strerror(int err)
{
    switch (err) {
    case 0:  return "Success";
    case 1:  return "Operation not permitted";
    case 2:  return "No such file or directory";
    case 5:  return "I/O error";
    case 9:  return "Bad file descriptor";
    case 12: return "Out of memory";
    case 13: return "Permission denied";
    case 17: return "File exists";
    case 22: return "Invalid argument";
    case 24: return "Too many open files";
    case 28: return "No space left on device";
    case 34: return "Result out of range";
    case 38: return "Function not implemented";
    default: return "Unknown error";
    }
}
