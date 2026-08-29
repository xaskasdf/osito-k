/*
 * OsitoK — System Capabilities (hardware-derived resource limits)
 *
 * All resource limits are computed at boot from actual hardware.
 * No subsystem should hardcode memory sizes — query g_sys_caps instead.
 *
 * Populated by sys_caps_init() during kernel startup, after mem_init().
 */

#ifndef SYS_CAPS_H
#define SYS_CAPS_H

#include "types.h"

#define SYS_CAPS_PROCESS_MIN          256U
#define SYS_CAPS_PROCESS_MAX         4096U
#define SYS_CAPS_PROCESS_GRANULARITY   64U
#define SYS_CAPS_PROCESS_RAM_BUDGET \
    (8ULL * 1024ULL * 1024ULL)

typedef struct {
    /* ── Hardware facts (from UEFI memory map + CPUID) ── */
    uint64_t total_ram;         /* Total usable RAM in bytes */
    uint64_t available_ram;     /* RAM free after kernel + drivers */
    uint32_t cpu_count;         /* Number of CPUs (BSP + APs) */
    uint32_t page_count;        /* Total 4KB pages */

    /* ── Derived limits (computed from hardware) ── */

    /* Kernel heap */
    uint64_t heap_init_size;    /* Initial kernel heap: max(256KB, total_ram/256) */
    uint64_t heap_grow_size;    /* Heap growth increment: max(64KB, total_ram/1024) */

    /* Per-process */
    uint64_t brk_heap_size;     /* Process brk heap: max(16MB, total_ram/16) */
    uint64_t user_stack_size;   /* User stack: max(1MB, min(8MB, total_ram/64)) */
    uint32_t max_processes;     /* RAM-derived scheduler slot capacity */
    uint32_t max_fds_global;    /* max(128, min(4096, total_ram/256KB)) */

    /* ELF loader */
    uint64_t elf_max_size;      /* max(64MB, available_ram * 3/4) */
    uint64_t elf_max_alloc;     /* Max single allocation for ELF: available_ram / 2 */

    /* Win32 compatibility */
    uint64_t win32_heap_size;   /* HeapAlloc pool: max(16MB, total_ram/8) */
    uint64_t crt_pool_size;     /* MSVCRT malloc pool: max(4MB, total_ram/16) */
    uint64_t win32_va_limit;    /* VirtualAlloc VA ceiling */

    /* Networking */
    uint32_t tcp_max_conns;     /* max(8, min(256, total_ram/4MB)) */

} sys_caps_t;

extern sys_caps_t g_sys_caps;

/* Initialize from hardware — call after mem_init() */
void sys_caps_init(void);

/* Helper: check if allocation is feasible, log if not */
int sys_caps_check_alloc(uint64_t bytes, const char *what);

#endif /* SYS_CAPS_H */
