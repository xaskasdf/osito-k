#include "win32_port.h"
#include <string.h>

/* Used by Intel's instruction validator to deduplicate diagnostic strings. */
void *intel_d3d9_memmem(const void *data, size_t bytes,
                       const void *pattern, size_t pattern_bytes)
{
    const unsigned char *cursor = data;
    if (!pattern_bytes) return (void *)data;
    if (pattern_bytes > bytes) return NULL;
    size_t remaining = bytes - pattern_bytes + 1;
    while (remaining) {
        const unsigned char *found = memchr(cursor,
            *(const unsigned char *)pattern, remaining);
        if (!found) return NULL;
        if (!memcmp(found, pattern, pattern_bytes)) return (void *)found;
        remaining -= (size_t)(found - cursor) + 1;
        cursor = found + 1;
    }
    return NULL;
}
