#include "crt_args.h"

static uint16_t crt_arg_unit(const void *input, size_t index, unsigned width)
{
    return width == 1 ? ((const uint8_t *)input)[index]
                      : ((const uint16_t *)input)[index];
}

static int crt_args_emit(void *strings, size_t capacity, unsigned width,
                         uint32_t *used, uint16_t value)
{
    if (*used >= CRT_ARGS_MAX_UNITS) return -1;
    if (strings) {
        if (*used >= capacity) return -1;
        if (width == 1) ((uint8_t *)strings)[*used] = (uint8_t)value;
        else ((uint16_t *)strings)[*used] = value;
    }
    ++*used;
    return 0;
}

int crt_args_parse(const void *input, size_t input_units, unsigned width,
                   void *strings, size_t string_units,
                   CRT_ARG_RECORD *args, size_t arg_capacity,
                   CRT_ARG_COUNTS *counts)
{
    if (!input || !counts || (width != 1 && width != 2) ||
        !input_units || input_units > CRT_ARGS_MAX_UNITS ||
        (!!strings != !!args)) return -1;
    counts->argc = counts->units = 0;
    size_t end = 0;
    while (end < input_units && crt_arg_unit(input, end, width)) ++end;
    if (end == input_units) return -1;
    size_t at = 0;
    uint32_t argc = 0, used = 0;
    do {
        if (args && argc >= arg_capacity) return -1;
        uint32_t offset = used, wildcard = 0;
        int quoted = 0;
        int first_quoted = crt_arg_unit(input, at, width) == '"';
        while (at < end) {
            uint16_t c = crt_arg_unit(input, at, width);
            if (!quoted && (c == ' ' || c == '\t')) break;
            if (argc == 0) {
                /* The program name has its own rules: backslashes never
                 * escape quotes here, even immediately before a quote. */
                ++at;
                if (c == '"') { quoted = !quoted; continue; }
            } else {
                size_t slashes = 0;
                while (at < end && crt_arg_unit(input, at, width) == '\\') {
                    ++slashes; ++at;
                }
                c = crt_arg_unit(input, at, width);
                size_t literal = c == '"' ? slashes / 2 : slashes;
                for (size_t i = 0; i < literal; ++i)
                    if (crt_args_emit(strings, string_units, width, &used, '\\'))
                        return -1;
                if (c == '"') {
                    ++at;
                    if (!(slashes & 1)) {
                        if (quoted && at < end &&
                            crt_arg_unit(input, at, width) == '"') {
                            /* msvcrt.dll's legacy double-quote rule also
                             * closes the quoted span (unlike newer UCRT). */
                            ++at;
                            quoted = 0;
                        }
                        else { quoted = !quoted; continue; }
                    }
                } else {
                    if (at == end || (!quoted && (c == ' ' || c == '\t')))
                        break;
                    ++at;
                }
            }
            if (argc && !first_quoted && (c == '*' || c == '?')) wildcard = 1;
            if (crt_args_emit(strings, string_units, width, &used, c)) return -1;
        }
        if (crt_args_emit(strings, string_units, width, &used, 0)) return -1;
        if (args) args[argc] = (CRT_ARG_RECORD){offset, wildcard};
        ++argc;
        while (at < end && (crt_arg_unit(input, at, width) == ' ' ||
                            crt_arg_unit(input, at, width) == '\t')) ++at;
    } while (at < end);
    counts->argc = argc;
    counts->units = used;
    return 0;
}
