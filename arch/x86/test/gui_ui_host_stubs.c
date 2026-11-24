#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

bool gui_test_fail_alloc;

void *kmalloc(uint64_t size)
{
    return gui_test_fail_alloc ? NULL : malloc((size_t)size);
}

void kfree(void *ptr)
{
    free(ptr);
}
