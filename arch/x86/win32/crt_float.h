#ifndef OSITOK_CRT_FLOAT_H
#define OSITOK_CRT_FLOAT_H

#include <stdint.h>
#include <stddef.h>

enum {
    CRT_FLOAT_LEFT = 1, CRT_FLOAT_ZERO = 2, CRT_FLOAT_PLUS = 4,
    CRT_FLOAT_SPACE = 8, CRT_FLOAT_ALT = 16
};

#define CRT_FLOAT_LEGACY_SPECIAL (1ULL << 3)
#define CRT_FLOAT_THREE_DIGITS   (1ULL << 4)
#define CRT_FLOAT_STANDARD_ROUND (1ULL << 5)
#define CRT_FLOAT_LEGACY_EXPORT (1ULL << 32)
#define CRT_FLOAT_LEGACY (CRT_FLOAT_LEGACY_SPECIAL | CRT_FLOAT_THREE_DIGITS | \
                          CRT_FLOAT_LEGACY_EXPORT)

typedef struct {
    void *context;
    int (*write)(void *, const char *, size_t);
    int (*repeat)(void *, char, int);
    int failed;
} CRT_FLOAT_SINK;

/* IEEE binary64 bits; rounding uses x87/SSE RC encoding (nearest/down/up/chop).
 * The sink receives no terminator. A negative result denotes output overflow
 * or a failed sink. Decimal conversion does not change the caller's FPU state. */
int crt_float_format(CRT_FLOAT_SINK *sink, uint64_t bits, int width,
                     int precision, unsigned flags, char conversion,
                     uint64_t options, unsigned rounding);

#endif
