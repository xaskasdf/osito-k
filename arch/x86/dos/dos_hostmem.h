/* Host backing pages, distinct from addresses in the emulated DOS machine. */
#ifndef OSITOK_DOS_HOSTMEM_H
#define OSITOK_DOS_HOSTMEM_H

#include "../include/paging.h"

extern void *mem_alloc_pages(uint64_t count);
extern void mem_free_pages(void *address, uint64_t count);

/* Low identity aliases may be absent or belong to a Win32 image. Only the
 * kernel direct map is a stable pointer, including under a native DOS CR3. */
static inline void *dos_host_alloc_pages(uint64_t count)
{
    void *physical = mem_alloc_pages(count);
    return physical ? PHYS_TO_VIRT(physical) : NULL;
}

static inline void dos_host_free_pages(void *address, uint64_t count)
{
    if (address)
        mem_free_pages((void *)(uintptr_t)VIRT_TO_PHYS(address), count);
}

int dos_hostmem_selftest(void);

#endif
