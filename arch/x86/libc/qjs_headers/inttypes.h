/* OsitoK shim — inttypes.h */
#ifndef _INTTYPES_H
#define _INTTYPES_H
#include <stdint.h>
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRId64 "ld"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRIX64 "lX"
#define PRIi64 "ld"
intmax_t strtoimax(const char *s, char **endp, int base);
uintmax_t strtoumax(const char *s, char **endp, int base);
#endif
