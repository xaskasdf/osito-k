#ifndef OSITO_CRT_ARGS_H
#define OSITO_CRT_ARGS_H
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define CRT_ARGS_MAX_UNITS 32768u
typedef struct {
    uint32_t offset; /* Units, not bytes, into the string buffer. */
    uint32_t expand; /* Unquoted wildcard in a non-program argument. */
} CRT_ARG_RECORD;
typedef struct {
    uint32_t argc;
    uint32_t units; /* Includes every terminating NUL. */
} CRT_ARG_COUNTS;

/* Bounded MS CRT syntax parser. unit_bytes is 1 (ANSI) or 2 (UTF-16).
 * A NULL output requests counts only; otherwise both capacities are checked.
 * Input must contain a NUL within input_units. Partial output on error must
 * be discarded. The caller owns an immutable input snapshot across passes. */
int crt_args_parse(const void *input, size_t input_units, unsigned unit_bytes,
                   void *strings, size_t string_units,
                   CRT_ARG_RECORD *args, size_t arg_capacity,
                   CRT_ARG_COUNTS *counts);
#endif
