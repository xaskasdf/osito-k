#include "../lib/intel_d3d9/win32_port.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Individually sized buffers let ASan catch reads beyond either input. The
 * unique nonzero byte identifies each match; zeros exercise overlapping runs.
 */
int main(void)
{
    unsigned checks = 0;
    for (size_t bytes = 1; bytes <= 64; ++bytes) {
        unsigned char *data = calloc(bytes, 1);
        assert(data);
        data[bytes - 1] = 0xa5;
        for (size_t length = 1; length <= bytes + 1; ++length) {
            unsigned char *pattern = calloc(length, 1);
            assert(pattern);
            pattern[length - 1] = 0xa5;
            const void *expected = length <= bytes ? data + bytes - length : NULL;
            assert(intel_d3d9_memmem(data, bytes, pattern, length) == expected);
            ++checks;
            pattern[length - 1] = 0x5a;
            assert(intel_d3d9_memmem(data, bytes, pattern, length) == NULL);
            ++checks;
            free(pattern);
        }
        assert(intel_d3d9_memmem(data, bytes, NULL, 0) == data);
        assert(intel_d3d9_memmem(data, 0, data, 1) == NULL);
        checks += 2;
        free(data);
    }
    assert(intel_d3d9_memmem(NULL, 0, NULL, 0) == NULL);
    printf("Native compiler memory search: %u checks passed\n", checks + 1);
    return 0;
}
