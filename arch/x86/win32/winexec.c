/*
 * OsitoK Windows Compatibility Layer — PE Execution Engine
 *
 * This is the main entry point for running Windows PE executables.
 * Orchestrates: PE load → import resolution → PEB/TEB setup → jump to entry.
 *
 * Called from the OsitoK shell or process subsystem:
 *   winexec_run(file_data, file_size, argc, argv)
 */

#include "pe.h"
#include "ntsyscall.h"
#include "ntdll_shim.h"
#include "kernel32_shim.h"
#include "msvcrt_shim.h"
#include "dllloader.h"

/* EXE ImageBase — used by GetModuleHandleA(NULL) */
ULONG_PTR g_exe_image_base;
static USHORT g_exe_subsystem;
static USHORT g_exe_subsystem_major_version;
static USHORT g_exe_subsystem_minor_version;
#include "advapi32_shim.h"
#include "scm.h"
#include "user32_shim.h"
#include "gdi32_shim.h"
#include "opengl32_shim.h"
#include "dxgi_shim.h"
#include "version_shim.h"
#include "libusb_shim.h"
#include "ddraw_shim.h"
#include "dsound_shim.h"
#include "dinput8_shim.h"
#include "wsock32_shim.h"
#include "shell32_shim.h"
#include "winmm_shim.h"
#include "ole32_shim.h"
#include "oleacc_shim.h"
#include "crypt32_shim.h"
#include "comctl32_shim.h"
#include "comdlg32_shim.h"
#include "richedit_shim.h"
#include "compat32.h"
#include "win32_abi.h"
#include "handle.h"
#include "../include/paging.h"

/* ── External kernel interfaces ─────────────────────────────── */

extern void  serial_puts(const char *s);
extern void  serial_puthex(uint64_t val, int digits);
extern void  serial_putdec(uint64_t val);
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);
extern void  proc_exit(int32_t code);
extern int32_t proc_current_pid(void);
extern int32_t proc_current_tgid(void);
extern void *proc_find_ptr(uint16_t pid);
extern uint32_t proc_state_of(void *process);
extern void  sched_yield(void);
extern HANDLE_TABLE g_handle_table;
extern void nt_process_complete_child(PVOID process_object,
                                      PVOID thread_object,
                                      NTSTATUS exit_status);
extern BOOL nt_process_set_image_path(PVOID process_object,
                                      const char *source);

/* ── PE allocator callbacks (used by pe.c) ──────────────────── */

extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern int paging_map_page_in_cr3(uint64_t cr3, uint64_t virt,
                                  uint64_t phys, uint64_t flags);
extern int paging_unmap_page(uint64_t virt);
extern int paging_unmap_page_in_cr3(uint64_t cr3, uint64_t virt);
extern uint64_t paging_get_kernel_cr3(void);
extern uint64_t proc_current_cr3(void);
extern DWORD win32_current_process_id(void);
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)

/* ── PE VA range tracker (detect ImageBase collisions) ───────── */

#define PE_VA_MAX               1024
#define PE32_RELOC_FLOOR        0x78000000ULL
#define PE32_RELOC_TOP          0x7F000000ULL
#define PE32_RELOC_GRANULARITY  0x10000ULL
#define PE64_RELOC_FLOOR        0x0000600000000000ULL
#define PE64_RELOC_TOP          0x0000700000000000ULL
#define PE64_RELOC_GRANULARITY  0x10000ULL
typedef struct {
    uint64_t base;
    uint64_t size;
    uint64_t cr3;
    uint64_t phys;
    uint64_t pages;
    DWORD owner_pid;
} PE_VA_RANGE;

static PE_VA_RANGE pe_va_ranges[PE_VA_MAX];
static int pe_va_count = 0;
/* The value is kernel PID + 1, so ownership is published by one atomic CAS. */
static volatile uint32_t pe_va_lock_owner;

#define PE_VA_PROC_ZOMBIE 4U

static DWORD pe_va_owner(void)
{
#ifdef TEST_HARNESS
    return 1;
#else
    return win32_current_process_id();
#endif
}

static uint64_t pe_va_cr3(void)
{
#ifdef TEST_HARNESS
    return 0;
#else
    uint64_t cr3 = proc_current_cr3();
    return cr3 ? cr3 : paging_get_kernel_cr3();
#endif
}

static void pe_trace_large_range(const char *operation,
                                 const PE_VA_RANGE *range)
{
#ifndef TEST_HARNESS
    if (!range || range->size < 64ULL * 1024 * 1024)
        return;
    serial_puts("[PE-RANGE] ");
    serial_puts(operation);
    serial_puts(" owner=");
    serial_putdec(range->owner_pid);
    serial_puts(" cr3=0x");
    serial_puthex(range->cr3, 16);
    serial_puts(" va=0x");
    serial_puthex(range->base, 16);
    serial_puts(" size=0x");
    serial_puthex(range->size, 16);
    serial_puts(" phys=0x");
    serial_puthex(range->phys, 16);
    serial_puts("\n");
#else
    (void)operation;
    (void)range;
#endif
}

static void pe_va_lock_acquire(void)
{
#ifndef TEST_HARNESS
    int32_t kernel_pid = proc_current_pid();
    uint32_t owner_key = kernel_pid > 0 ? (uint32_t)kernel_pid + 1U : 1U;

    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&pe_va_lock_owner, &expected,
                                        owner_key, FALSE, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return;

        /* Win32 exception/termination paths can leave this process-owned
         * lock held after a non-local exit.  PE VA operations do not recurse,
         * so seeing our own owner key means the previous critical section was
         * abandoned and teardown must recover it before freeing DLL images. */
        if (expected == owner_key) {
            uint32_t abandoned_owner = expected;
            if (__atomic_compare_exchange_n(&pe_va_lock_owner,
                                            &abandoned_owner, 0U, FALSE,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                serial_puts("[PE-LOCK] recovered abandoned self owner kpid=");
                serial_putdec(kernel_pid > 0 ? (uint32_t)kernel_pid : 0U);
                serial_puts("\n");
            }
            continue;
        }

        uint32_t stale_pid = expected - 1U;
        void *stale_process = stale_pid && stale_pid <= 0xFFFFU
            ? proc_find_ptr((uint16_t)stale_pid)
            : NULL;
        if (!stale_process ||
            proc_state_of(stale_process) == PE_VA_PROC_ZOMBIE) {
            uint32_t stale_owner = expected;
            if (__atomic_compare_exchange_n(&pe_va_lock_owner, &stale_owner,
                                            0U, FALSE, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                serial_puts("[PE-LOCK] recovered stale owner kpid=");
                serial_putdec(stale_pid);
                serial_puts("\n");
            }
            continue;
        }

        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
        if (flags & (1ULL << 9)) {
            sched_yield();
        } else {
            for (int i = 0; i < 64; i++)
                __asm__ volatile ("pause");
        }
    }
#endif
}

static void pe_va_lock_release(void)
{
#ifndef TEST_HARNESS
    int32_t kernel_pid = proc_current_pid();
    uint32_t owner_key = kernel_pid > 0 ? (uint32_t)kernel_pid + 1U : 1U;
    uint32_t expected = owner_key;
    (void)__atomic_compare_exchange_n(&pe_va_lock_owner, &expected, 0U,
                                      FALSE, __ATOMIC_RELEASE,
                                      __ATOMIC_RELAXED);
#endif
}

static uint64_t pe_va_range_conflict_end_snapshot(uint64_t base,
                                                   uint64_t size)
{
    if (!size || base + size < base)
        return UINT64_MAX;

    uint64_t end = base + size;
    uint64_t conflict_end = 0;
    DWORD owner_pid = pe_va_owner();
    int count = __atomic_load_n(&pe_va_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++) {
        uint64_t rsize = __atomic_load_n(&pe_va_ranges[i].size,
                                         __ATOMIC_ACQUIRE);
        if (!rsize || pe_va_ranges[i].owner_pid != owner_pid) continue;
        uint64_t rend = pe_va_ranges[i].base + rsize;
        if (rend < pe_va_ranges[i].base) continue;
        if (base < rend && end > pe_va_ranges[i].base &&
            rend > conflict_end)
            conflict_end = rend;
    }
    return conflict_end;
}

ULONGLONG pe_va_range_conflict_end(ULONGLONG base, ULONGLONG size)
{
    return pe_va_range_conflict_end_snapshot(base, size);
}

static int pe_va_conflict_snapshot(uint64_t base, uint64_t size)
{
    if (compat32_runtime_range_conflicts(base, size))
        return 1;
    if (pe_va_range_conflict_end_snapshot(base, size))
        return 1;

    return 0;
}

BOOL pe_va_range_conflicts(ULONGLONG base, ULONGLONG size)
{
    /* NtAllocateVirtualMemory calls this while holding vm_track_lock. Keep
     * this public query limited to the lock-free PE/runtime snapshots. */
    return pe_va_conflict_snapshot(base, size) ? TRUE : FALSE;
}

/* PE placement itself must also avoid mmap/brk and VirtualAlloc ranges. This
 * is separate from pe_va_range_conflicts so NT allocation cannot recurse into
 * its own lock while checking image occupancy. */
static int pe_allocation_conflict_snapshot(uint64_t base, uint64_t size)
{
    if (pe_va_conflict_snapshot(base, size))
        return 1;

#ifndef TEST_HARNESS
    extern uint64_t syscall_vma_range_conflict_end(uint64_t candidate,
                                                    uint64_t candidate_size);
    if (syscall_vma_range_conflict_end(base, size))
        return 1;
    if (nt_vm_range_conflict_end(base, size))
        return 1;

    uint64_t cr3 = pe_va_cr3();
    if (cr3 == paging_get_kernel_cr3()) {
        extern uint64_t mem_identity_reservation_conflict_end(
            uint64_t candidate, uint64_t candidate_size);
        if (mem_identity_reservation_conflict_end(base, size))
            return 1;
    } else if (cr3) {
        uint64_t mapped_end =
            paging_first_mapped_end_in_cr3(cr3, base, size);
        if (mapped_end)
            return 1;
    }
#endif
    return 0;
}

BOOL pe_va_query_range(ULONGLONG address, ULONGLONG *base,
                       ULONGLONG *size, ULONGLONG *next_base)
{
    uint64_t found_base = 0;
    uint64_t found_size = 0;
    uint64_t next = 0;
    DWORD owner_pid = pe_va_owner();
    int count = __atomic_load_n(&pe_va_count, __ATOMIC_ACQUIRE);

    for (int i = 0; i < count; i++) {
        uint64_t rbase = pe_va_ranges[i].base;
        uint64_t rsize = __atomic_load_n(&pe_va_ranges[i].size,
                                         __ATOMIC_ACQUIRE);
        uint64_t rend = rbase + rsize;
        if (!rsize || pe_va_ranges[i].owner_pid != owner_pid ||
            rend < rbase)
            continue;
        if (address >= rbase && address < rend) {
            found_base = rbase;
            found_size = rsize;
        } else if (rbase > address && (!next || rbase < next)) {
            next = rbase;
        }
    }

    if (base) *base = found_base;
    if (size) *size = found_size;
    if (next_base) *next_base = next;
    return found_size ? TRUE : FALSE;
}

BOOL pe_va_range_contains(ULONGLONG base, ULONGLONG size)
{
    uint64_t end = base + size;
    if (!size || end < base)
        return FALSE;

    DWORD owner_pid = pe_va_owner();
    uint64_t owner_cr3 = pe_va_cr3();
    int count = __atomic_load_n(&pe_va_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++) {
        uint64_t range_base = pe_va_ranges[i].base;
        uint64_t range_size = __atomic_load_n(&pe_va_ranges[i].size,
                                               __ATOMIC_ACQUIRE);
        uint64_t range_end = range_base + range_size;
        if (!range_size || pe_va_ranges[i].owner_pid != owner_pid ||
            pe_va_ranges[i].cr3 != owner_cr3 || range_end < range_base)
            continue;
        if (base >= range_base && end <= range_end)
            return TRUE;
    }
    return FALSE;
}

static void pe_va_record(uint64_t base, uint64_t size, uint64_t cr3,
                         uint64_t phys, uint64_t pages)
{
    DWORD owner_pid = pe_va_owner();
    for (int i = 0; i < pe_va_count; i++) {
        if (__atomic_load_n(&pe_va_ranges[i].size, __ATOMIC_ACQUIRE) == 0) {
            pe_va_ranges[i].base = base;
            pe_va_ranges[i].cr3 = cr3;
            pe_va_ranges[i].phys = phys;
            pe_va_ranges[i].pages = pages;
            pe_va_ranges[i].owner_pid = owner_pid;
            __atomic_store_n(&pe_va_ranges[i].size, size, __ATOMIC_RELEASE);
            return;
        }
    }
    if (pe_va_count < PE_VA_MAX) {
        int slot = pe_va_count;
        pe_va_ranges[slot].base = base;
        pe_va_ranges[slot].cr3 = cr3;
        pe_va_ranges[slot].phys = phys;
        pe_va_ranges[slot].pages = pages;
        pe_va_ranges[slot].owner_pid = owner_pid;
        __atomic_store_n(&pe_va_ranges[slot].size, size, __ATOMIC_RELEASE);
        __atomic_store_n(&pe_va_count, slot + 1, __ATOMIC_RELEASE);
    }
}

static BOOL pe_va_forget(uint64_t base, uint64_t size, DWORD owner_pid,
                         PE_VA_RANGE *removed)
{
    int count = __atomic_load_n(&pe_va_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++) {
        if (pe_va_ranges[i].base == base &&
            pe_va_ranges[i].owner_pid == owner_pid &&
            __atomic_load_n(&pe_va_ranges[i].size, __ATOMIC_ACQUIRE) == size) {
            if (removed) *removed = pe_va_ranges[i];
            __atomic_store_n(&pe_va_ranges[i].size, 0, __ATOMIC_RELEASE);
            return TRUE;
        }
    }
    return FALSE;
}

static uint64_t pe32_reloc_address(SIZE_T size, uint64_t top)
{
    uint64_t span = ((uint64_t)size + PE32_RELOC_GRANULARITY - 1) &
                    ~(PE32_RELOC_GRANULARITY - 1);
    if (!span || span < size || span > PE32_RELOC_TOP - PE32_RELOC_FLOOR)
        return 0;

    uint64_t cursor = top && top < PE32_RELOC_TOP ? top : PE32_RELOC_TOP;
    while (cursor >= PE32_RELOC_FLOOR + span) {
        uint64_t base = (cursor - span) & ~(PE32_RELOC_GRANULARITY - 1);
        if (base < PE32_RELOC_FLOOR) break;
        if (!pe_allocation_conflict_snapshot(base, size))
            return base;
        cursor = base;
    }
    return 0;
}

static uint64_t pe64_reloc_address(SIZE_T size)
{
    uint64_t span = ((uint64_t)size + PE64_RELOC_GRANULARITY - 1) &
                    ~(PE64_RELOC_GRANULARITY - 1);
    if (!span || span < size) return 0;

    uint64_t cursor = PE64_RELOC_TOP;
    while (cursor >= PE64_RELOC_FLOOR + span) {
        uint64_t base = (cursor - span) & ~(PE64_RELOC_GRANULARITY - 1);
        if (base < PE64_RELOC_FLOOR) break;
        if (!pe_allocation_conflict_snapshot(base, size))
            return base;
        cursor = base >= PE64_RELOC_FLOOR + PE64_RELOC_GRANULARITY
               ? base - PE64_RELOC_GRANULARITY
               : PE64_RELOC_FLOOR;
    }
    return 0;
}

static int pe_unmap_page_from(uint64_t cr3, uint64_t va)
{
    return cr3 && cr3 != paging_get_kernel_cr3()
         ? paging_unmap_page_in_cr3(cr3, va)
         : paging_unmap_page(va);
}

static int pe_map_pages(uint64_t cr3, uint64_t va, uint64_t pa,
                        uint64_t pages)
{
    for (uint64_t i = 0; i < pages; i++) {
        int result = cr3 && cr3 != paging_get_kernel_cr3()
            ? paging_map_page_in_cr3(cr3, va + i * 4096, pa + i * 4096,
                                     PTE_PRESENT | PTE_WRITABLE)
            : paging_map_page(va + i * 4096, pa + i * 4096,
                              PTE_PRESENT | PTE_WRITABLE);
        if (result != 0) {
            while (i)
                pe_unmap_page_from(cr3, va + --i * 4096);
            return -1;
        }
    }
    return 0;
}

static PVOID pe_alloc_legacy(PVOID preferred, SIZE_T size, BOOL is_32bit)
{
    uint64_t pages = (size + 0xFFF) / 4096;
    uint64_t cr3 = pe_va_cr3();
#ifdef TEST_HARNESS
    (void)is_32bit;
    #include <sys/mman.h>
    if (preferred) {
        void *p = mmap(preferred, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) {
            pe_va_record((uint64_t)p, size, 0, 0, pages);
            return p;
        }
    }
    void *r = mem_alloc_pages(pages);
    if (r) pe_va_record((uint64_t)r, size, 0, (uint64_t)r, pages);
    return r;
#else
    /* Check for VA range conflict before mapping at preferred address */
    if (preferred &&
        pe_allocation_conflict_snapshot((uint64_t)preferred, size)) {
        serial_puts("[pe_alloc] CONFLICT: VA 0x");
        serial_puthex((uint64_t)preferred, 8);
        serial_puts(" already occupied, relocating\n");
        preferred = NULL;  /* force relocation */
    }

    if (is_32bit) {
        if (!pages) return NULL;

        /* Process CR3s own their complete lower half. Keep PE32 virtual
         * placement below 2 GB, but back it with ordinary physical pages;
         * tying VA to PA exhausts as soon as that physical range is in use. */
        uint64_t va = preferred && (((uint64_t)preferred & 0xFFFULL) == 0)
                    ? (uint64_t)preferred
                    : pe32_reloc_address(size, PE32_RELOC_TOP);

        if (!va) {
            serial_puts("[pe_alloc] ERROR: PE32 relocation VA exhausted\n");
            return NULL;
        }

        void *phys = mem_alloc_pages(pages);
        if (!phys)
            return NULL;

        uint64_t pa = (uint64_t)phys;
        if (pe_map_pages(cr3, va, pa, pages) == 0) {
            pe_va_record(va, size, cr3, pa, pages);
            serial_puts(va == (uint64_t)preferred
                ? "[pe_alloc] mapped PE32 preferred VA 0x"
                : "[pe_alloc] relocated PE32 VA 0x");
            serial_puthex(va, 8);
            serial_puts(" -> PA 0x");
            serial_puthex(pa, 16);
            serial_puts(" pages=0x");
            serial_puthex(pages, 4);
            serial_puts("\n");
            return (PVOID)(ULONG_PTR)va;
        }

        mem_free_pages(phys, pages);
        serial_puts("[pe_alloc] ERROR: PE32 page-table map failed\n");
        return NULL;
    }

    void *phys = mem_alloc_pages(pages);
    if (!phys) return NULL;

    if (preferred) {
        uint64_t va = (uint64_t)preferred;
        uint64_t pa = (uint64_t)phys;
        if (pe_map_pages(cr3, va, pa, pages) == 0) {
            pe_va_record(va, size, cr3, pa, pages);
            return (void *)va;
        }
    }

    mem_free_pages(phys, pages);
    return NULL;
#endif
}

PVOID pe_alloc(PVOID preferred, SIZE_T size, BOOL is_32bit)
{
    pe_va_lock_acquire();

#ifdef TEST_HARNESS
    PVOID result = pe_alloc_legacy(preferred, size, is_32bit);
    pe_va_lock_release();
    return result;
#else
    if (is_32bit) {
        PVOID result = pe_alloc_legacy(preferred, size, TRUE);
        pe_va_lock_release();
        return result;
    }

    uint64_t pages = (size + 0xFFF) / 4096;
    if (!pages) {
        pe_va_lock_release();
        return NULL;
    }

    if (preferred &&
        pe_allocation_conflict_snapshot((uint64_t)preferred, size)) {
        serial_puts("[pe_alloc] CONFLICT: VA 0x");
        serial_puthex((uint64_t)preferred, 16);
        serial_puts(" already occupied, relocating\n");
        preferred = NULL;
    }

    void *phys = mem_alloc_pages(pages);
    if (!phys) {
        pe_va_lock_release();
        return NULL;
    }

    uint64_t cr3 = pe_va_cr3();
    if (preferred) {
        uint64_t va = (uint64_t)preferred;
        int failed = pe_map_pages(cr3, va, (uint64_t)phys, pages);
        if (failed == 0) {
            pe_va_record(va, size, cr3, (uint64_t)phys, pages);
            PE_VA_RANGE trace = {
                va, size, cr3, (uint64_t)phys, pages, pe_va_owner()
            };
            pe_trace_large_range("map", &trace);
            pe_va_lock_release();
            return preferred;
        }
        serial_puts("[pe_alloc] preferred map failed, relocating VA 0x");
        serial_puthex(va, 16);
        serial_puts(" failed_pages=0x");
        serial_puthex((uint64_t)failed, 8);
        serial_puts("\n");
    }

    uint64_t reloc_va = pe64_reloc_address(size);
    if (reloc_va &&
        pe_map_pages(cr3, reloc_va, (uint64_t)phys, pages) == 0) {
        pe_va_record(reloc_va, size, cr3, (uint64_t)phys, pages);
        PE_VA_RANGE trace = {
            reloc_va, size, cr3, (uint64_t)phys, pages, pe_va_owner()
        };
        pe_trace_large_range("map", &trace);
        serial_puts("[pe_alloc] relocated PE64 VA 0x");
        serial_puthex(reloc_va, 16);
        serial_puts(" -> PA 0x");
        serial_puthex((uint64_t)phys, 8);
        serial_puts("\n");
        pe_va_lock_release();
        return (PVOID)(ULONG_PTR)reloc_va;
    }

    mem_free_pages(phys, pages);
    serial_puts("[pe_alloc] ERROR: PE64 relocation VA exhausted\n");
    pe_va_lock_release();
    return NULL;
#endif
}

void pe_free_for_owner(PVOID addr, SIZE_T size, ULONG owner_pid)
{
    PE_VA_RANGE allocation;
    memset(&allocation, 0, sizeof(allocation));
    pe_va_lock_acquire();
    BOOL found = pe_va_forget((uint64_t)addr, size, owner_pid, &allocation);
    pe_va_lock_release();

    if (!found) {
        serial_puts("[pe_free] allocation record missing for VA 0x");
        serial_puthex((uint64_t)addr, 16);
        serial_puts("\n");
        return;
    }

    pe_trace_large_range("free", &allocation);

#ifdef TEST_HARNESS
    if (allocation.phys)
        mem_free_pages((void *)allocation.phys, allocation.pages);
    else
        munmap(addr, size);
#else
    for (uint64_t i = 0; i < allocation.pages; i++)
        pe_unmap_page_from(allocation.cr3,
                           allocation.base + i * 4096);
    if (allocation.phys == allocation.base && allocation.phys) {
        if (pe_map_pages(allocation.cr3, allocation.base, allocation.phys,
                         allocation.pages) != 0) {
            serial_puts("[pe_free] identity-map restore failed; quarantining PA 0x");
            serial_puthex(allocation.phys, 16);
            serial_puts("\n");
            return;
        }
    }
    if (allocation.phys)
        mem_free_pages((void *)allocation.phys, allocation.pages);
#endif
}

void pe_free(PVOID addr, SIZE_T size)
{
    pe_free_for_owner(addr, size, pe_va_owner());
}

void pe_log(const char *msg)
{
#ifdef OK_QUIET
    (void)msg;
#else
    serial_puts(msg);
    serial_puts("\n");
#endif
}

void pe_log_hex(const char *prefix, ULONGLONG val)
{
#ifdef OK_QUIET
    (void)prefix;
    (void)val;
#else
    serial_puts(prefix);
    serial_puthex(val, 16);
    serial_puts("\n");
#endif
}

/* ── Import resolver (used by pe.c) ─────────────────────────── */
/*
 * Delegates to the DLL loader which checks built-in shims first,
 * then loaded PE modules, then falls back to searching all shims.
 */
PVOID pe_resolve_import(const char *dll_name, const char *func_name,
                        USHORT ordinal, BOOL by_ordinal)
{
    return dll_resolve_import(dll_name, func_name, ordinal, by_ordinal);
}

/* ── PEB/TEB setup ──────────────────────────────────────────── */

static PEB  g_peb;
TEB  g_teb;   /* non-static: accessed by ntdll_shim.c for SEH dispatch */

#define MAX_WIN32_CHILDREN 32
#define WIN32_CHILD_INHERITED_HANDLE_CAP 64
#define WIN32_CHILD_COMMAND_LINE_CAP 4096
#define WIN32_CHILD_CURRENT_DIRECTORY_CAP 260
#define WIN32_PROCESS_IMAGE_PATH_CAP 320
#define WIN32_PROCESS_DLL_PATH_CAP 512
#define WIN32_PROCESS_DESKTOP_CAP 32
#define WIN32_PROCESS_ENVIRONMENT_CAP 32768

typedef struct __attribute__((packed)) {
    USHORT Length;
    USHORT MaximumLength;
    uint32_t Buffer;
} WIN32_UNICODE_STRING32;

typedef struct __attribute__((packed)) {
    WIN32_UNICODE_STRING32 DosPath;
    uint32_t Handle;
} WIN32_CURDIR32;

typedef struct __attribute__((packed)) {
    USHORT Flags;
    USHORT Length;
    ULONG TimeStamp;
    WIN32_UNICODE_STRING32 DosPath;
} WIN32_DRIVE_LETTER_CURDIR32;

typedef struct __attribute__((packed)) {
    ULONG MaximumLength;
    ULONG Length;
    ULONG Flags;
    ULONG DebugFlags;
    uint32_t ConsoleHandle;
    ULONG ConsoleFlags;
    uint32_t StandardInput;
    uint32_t StandardOutput;
    uint32_t StandardError;
    WIN32_CURDIR32 CurrentDirectory;
    WIN32_UNICODE_STRING32 DllPath;
    WIN32_UNICODE_STRING32 ImagePathName;
    WIN32_UNICODE_STRING32 CommandLine;
    uint32_t Environment;
    ULONG StartingX;
    ULONG StartingY;
    ULONG CountX;
    ULONG CountY;
    ULONG CountCharsX;
    ULONG CountCharsY;
    ULONG FillAttribute;
    ULONG WindowFlags;
    ULONG ShowWindowFlags;
    WIN32_UNICODE_STRING32 WindowTitle;
    WIN32_UNICODE_STRING32 DesktopInfo;
    WIN32_UNICODE_STRING32 ShellInfo;
    WIN32_UNICODE_STRING32 RuntimeData;
    WIN32_DRIVE_LETTER_CURDIR32 CurrentDirectories[32];
    ULONG EnvironmentSize;
    ULONG EnvironmentVersion;
    uint32_t PackageDependencyData;
    ULONG ProcessGroupId;
    ULONG LoaderThreads;
} WIN32_RTL_USER_PROCESS_PARAMETERS32;

_Static_assert(__builtin_offsetof(WIN32_RTL_USER_PROCESS_PARAMETERS32,
                                  CurrentDirectory) == 0x24,
               "Win32 process parameters CWD offset changed");
_Static_assert(__builtin_offsetof(WIN32_RTL_USER_PROCESS_PARAMETERS32,
                                  ImagePathName) == 0x38,
               "Win32 process parameters image offset changed");
_Static_assert(__builtin_offsetof(WIN32_RTL_USER_PROCESS_PARAMETERS32,
                                  CommandLine) == 0x40,
               "Win32 process parameters command line offset changed");
_Static_assert(__builtin_offsetof(WIN32_RTL_USER_PROCESS_PARAMETERS32,
                                  Environment) == 0x48,
               "Win32 process parameters environment offset changed");

typedef struct __attribute__((packed)) {
    uint32_t Flink;
    uint32_t Blink;
} WIN32_LIST_ENTRY32;

typedef struct __attribute__((packed)) {
    ULONG Length;
    BYTE Initialized;
    BYTE Reserved1[3];
    uint32_t SsHandle;
    WIN32_LIST_ENTRY32 InLoadOrderModuleList;
    WIN32_LIST_ENTRY32 InMemoryOrderModuleList;
    WIN32_LIST_ENTRY32 InInitializationOrderModuleList;
    uint32_t EntryInProgress;
    BYTE ShutdownInProgress;
    BYTE Reserved2[3];
    uint32_t ShutdownThreadId;
} WIN32_PEB_LDR_DATA32;

typedef struct __attribute__((packed)) {
    WIN32_LIST_ENTRY32 InLoadOrderLinks;
    WIN32_LIST_ENTRY32 InMemoryOrderLinks;
    WIN32_LIST_ENTRY32 InInitializationOrderLinks;
    uint32_t DllBase;
    uint32_t EntryPoint;
    ULONG SizeOfImage;
    WIN32_UNICODE_STRING32 FullDllName;
    WIN32_UNICODE_STRING32 BaseDllName;
    ULONG Flags;
    USHORT LoadCount;
    USHORT TlsIndex;
    WIN32_LIST_ENTRY32 HashLinks;
    ULONG TimeDateStamp;
    uint32_t EntryPointActivationContext;
    uint32_t PatchInformation;
    WIN32_LIST_ENTRY32 ForwarderLinks;
    WIN32_LIST_ENTRY32 ServiceTagLinks;
    WIN32_LIST_ENTRY32 StaticLinks;
    uint32_t ContextInformation;
    uint32_t OriginalBase;
    int64_t LoadTime;
    ULONG BaseNameHashValue;
    ULONG LoadReason;
    ULONG ImplicitPathOptions;
    ULONG ReferenceCount;
    ULONG DependentLoadFlags;
    BYTE SigningLevel;
    BYTE Reserved[0x1B];
} WIN32_LDR_DATA_TABLE_ENTRY32;

_Static_assert(sizeof(WIN32_PEB_LDR_DATA32) == 0x30,
               "Win32 PEB loader data size changed");
_Static_assert(__builtin_offsetof(WIN32_PEB_LDR_DATA32,
                                  InLoadOrderModuleList) == 0x0C,
               "Win32 loader list head offset changed");
_Static_assert(__builtin_offsetof(WIN32_LDR_DATA_TABLE_ENTRY32,
                                  DllBase) == 0x18,
               "Win32 loader DllBase offset changed");
_Static_assert(__builtin_offsetof(WIN32_LDR_DATA_TABLE_ENTRY32,
                                  FullDllName) == 0x24,
               "Win32 loader full-name offset changed");
_Static_assert(__builtin_offsetof(WIN32_LDR_DATA_TABLE_ENTRY32,
                                  BaseDllName) == 0x2C,
               "Win32 loader base-name offset changed");
_Static_assert(sizeof(WIN32_LDR_DATA_TABLE_ENTRY32) == 0xA8,
               "Win32 loader entry size changed");

#define WIN32_LDR_MODULE_CAPACITY (MAX_LOADED_MODULES + 1)

_Static_assert(WIN32_LDR_MODULE_CAPACITY <= 0xFFFFU,
               "Win32 loader slots must fit in the stable order index");

typedef struct {
    WIN32_LDR_DATA_TABLE_ENTRY32 entry;
    WCHAR full_name[WIN32_PROCESS_IMAGE_PATH_CAP];
    WCHAR base_name[64];
    BOOL used;
} WIN32_LDR_MODULE32;

typedef struct {
    ULONG Length;
    BYTE Initialized;
    BYTE Reserved1[3];
    PVOID SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
    PVOID EntryInProgress;
    BYTE ShutdownInProgress;
    BYTE Reserved2[7];
    HANDLE ShutdownThreadId;
} WIN64_PEB_LDR_DATA;

typedef struct {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    ULONG Reserved0;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
    ULONG Flags;
    USHORT LoadCount;
    USHORT TlsIndex;
    LIST_ENTRY HashLinks;
    ULONG TimeDateStamp;
    ULONG Reserved1;
    PVOID EntryPointActivationContext;
    PVOID Lock;
    PVOID DdagNode;
    LIST_ENTRY NodeModuleLink;
    PVOID LoadContext;
    PVOID ParentDllBase;
    PVOID SwitchBackContext;
    BYTE BaseAddressIndexNode[0x18];
    BYTE MappingInfoIndexNode[0x18];
    ULONG_PTR OriginalBase;
    LARGE_INTEGER LoadTime;
    ULONG BaseNameHashValue;
    ULONG LoadReason;
    ULONG ImplicitPathOptions;
    ULONG ReferenceCount;
    ULONG DependentLoadFlags;
    BYTE SigningLevel;
    BYTE Reserved2[3];
} WIN64_LDR_DATA_TABLE_ENTRY;

_Static_assert(sizeof(WIN64_PEB_LDR_DATA) == 0x58,
               "Win64 PEB loader data size changed");
_Static_assert(__builtin_offsetof(WIN64_PEB_LDR_DATA,
                                  InLoadOrderModuleList) == 0x10,
               "Win64 loader list head offset changed");
_Static_assert(__builtin_offsetof(WIN64_LDR_DATA_TABLE_ENTRY,
                                  DllBase) == 0x30,
               "Win64 loader DllBase offset changed");
_Static_assert(__builtin_offsetof(WIN64_LDR_DATA_TABLE_ENTRY,
                                  FullDllName) == 0x48,
               "Win64 loader full-name offset changed");
_Static_assert(__builtin_offsetof(WIN64_LDR_DATA_TABLE_ENTRY,
                                  BaseDllName) == 0x58,
               "Win64 loader base-name offset changed");
_Static_assert(__builtin_offsetof(WIN64_LDR_DATA_TABLE_ENTRY,
                                  ReferenceCount) == 0x114,
               "Win64 loader reference-count offset changed");
_Static_assert(sizeof(WIN64_LDR_DATA_TABLE_ENTRY) == 0x120,
               "Win64 loader entry size changed");

typedef struct {
    WIN64_LDR_DATA_TABLE_ENTRY entry;
    WCHAR full_name[WIN32_PROCESS_IMAGE_PATH_CAP];
    WCHAR base_name[64];
    BOOL used;
} WIN64_LDR_MODULE;

typedef struct {
    RTL_CRITICAL_SECTION peb_lock;
    RTL_CRITICAL_SECTION loader_lock;
    WIN64_PEB_LDR_DATA loader_data;
    USHORT loader_order[WIN32_LDR_MODULE_CAPACITY];
    USHORT loader_count;
    USHORT loader_reserved;
    WIN64_LDR_MODULE loader_modules[WIN32_LDR_MODULE_CAPACITY];
} WIN64_LOADER_PROCESS_BLOCK;

#define WIN64_PEB_STORAGE_SIZE 0x1000U
#define WIN64_TLS_SLOT_CAPACITY 1024U

typedef struct {
    TEB teb;
    PEB peb;
    BYTE peb_padding[WIN64_PEB_STORAGE_SIZE - sizeof(PEB)];
    RTL_USER_PROCESS_PARAMETERS process_parameters;
    PVOID process_heaps[1];
    WCHAR image_path_w[WIN32_PROCESS_IMAGE_PATH_CAP];
    WCHAR command_line_w[WIN32_CHILD_COMMAND_LINE_CAP];
    WCHAR current_directory_w[WIN32_PROCESS_IMAGE_PATH_CAP];
    WCHAR dll_path_w[WIN32_PROCESS_DLL_PATH_CAP];
    WCHAR desktop_info_w[WIN32_PROCESS_DESKTOP_CAP];
    WCHAR environment_w[WIN32_PROCESS_ENVIRONMENT_CAP];
    PVOID tls_vector[WIN64_TLS_SLOT_CAPACITY];
    PVOID tls_expansion[WIN64_TLS_SLOT_CAPACITY - 64];
    WIN64_LOADER_PROCESS_BLOCK loader;
} WIN64_PROCESS_BLOCK;

_Static_assert(__builtin_offsetof(WIN64_PROCESS_BLOCK, peb) ==
                   TEB64_STORAGE_SIZE,
               "Win64 PEB must follow the two-page TEB");
_Static_assert(__builtin_offsetof(WIN64_PROCESS_BLOCK,
                                  process_parameters) ==
                   TEB64_STORAGE_SIZE + WIN64_PEB_STORAGE_SIZE,
               "Win64 process parameters must not overlap the PEB page");

typedef struct {
    TEB32 teb;
    BYTE teb_padding[TEB32_STORAGE_SIZE - sizeof(TEB32)];
    PEB32 peb;
    BYTE peb_padding[PEB32_STORAGE_SIZE - sizeof(PEB32)];
    WIN32_RTL_USER_PROCESS_PARAMETERS32 process_parameters;
    uint32_t process_heaps[1];
    char image_path[260];
    char exe_name[64];
    char command_line[WIN32_CHILD_COMMAND_LINE_CAP];
    char current_directory[WIN32_CHILD_CURRENT_DIRECTORY_CAP];
    WCHAR image_path_w[WIN32_PROCESS_IMAGE_PATH_CAP];
    WCHAR command_line_w[WIN32_CHILD_COMMAND_LINE_CAP];
    WCHAR current_directory_w[WIN32_PROCESS_IMAGE_PATH_CAP];
    WCHAR dll_path_w[WIN32_PROCESS_DLL_PATH_CAP];
    WCHAR desktop_info_w[WIN32_PROCESS_DESKTOP_CAP];
    WCHAR environment_w[WIN32_PROCESS_ENVIRONMENT_CAP];
    RTL_CRITICAL_SECTION32 peb_lock;
    RTL_CRITICAL_SECTION32 loader_lock;
    WIN32_PEB_LDR_DATA32 loader_data;
    USHORT loader_order[WIN32_LDR_MODULE_CAPACITY];
    USHORT loader_count;
    USHORT loader_reserved;
    WIN32_LDR_MODULE32 loader_modules[WIN32_LDR_MODULE_CAPACITY];
} WIN32_COMPAT_PROCESS_BLOCK;

_Static_assert(__builtin_offsetof(WIN32_COMPAT_PROCESS_BLOCK, peb) ==
                   TEB32_STORAGE_SIZE,
               "Win32 PEB must start on the page after the TEB");
_Static_assert(__builtin_offsetof(WIN32_COMPAT_PROCESS_BLOCK,
                                  process_parameters) ==
                   TEB32_STORAGE_SIZE + PEB32_STORAGE_SIZE,
               "Win32 process parameters must not overlap the PEB page");

static RTL_USER_PROCESS_PARAMETERS g_process_parameters;
static WCHAR g_image_path_w[WIN32_PROCESS_IMAGE_PATH_CAP];
static WCHAR g_command_line_w[WIN32_CHILD_COMMAND_LINE_CAP];
static WCHAR g_current_directory_w[WIN32_PROCESS_IMAGE_PATH_CAP];
static WCHAR g_dll_path_w[WIN32_PROCESS_DLL_PATH_CAP];
static WCHAR g_desktop_info_w[WIN32_PROCESS_DESKTOP_CAP];
static WCHAR g_environment_w[WIN32_PROCESS_ENVIRONMENT_CAP];
static PVOID g_process_heaps[1];
static WIN32_COMPAT_PROCESS_BLOCK *g_main_compat_environment32;
static WIN64_PROCESS_BLOCK *g_main_environment64;

typedef struct {
    BOOL used;
    BOOL exit_ready;
    int kernel_pid;
    ULONG process_id;
    ULONG parent_process_id;
    ULONG thread_id;
    USHORT subsystem;
    USHORT subsystem_major_version;
    USHORT subsystem_minor_version;
    PVOID process_object;
    PVOID thread_object;
    HANDLE process_handle;
    HANDLE thread_handle;
    NTSTATUS exit_status;
    volatile BOOL exit_requested;
    int exit_requester_kernel_pid;
    char image_path[260];
    char exe_name[64];
    char command_line[WIN32_CHILD_COMMAND_LINE_CAP];
    WCHAR command_line_w[WIN32_CHILD_COMMAND_LINE_CAP];
    char current_directory[WIN32_CHILD_CURRENT_DIRECTORY_CAP];
    RTL_USER_PROCESS_PARAMETERS process_parameters;
    WCHAR image_path_w[WIN32_PROCESS_IMAGE_PATH_CAP];
    WCHAR current_directory_w[WIN32_PROCESS_IMAGE_PATH_CAP];
    WCHAR dll_path_w[WIN32_PROCESS_DLL_PATH_CAP];
    WCHAR desktop_info_w[WIN32_PROCESS_DESKTOP_CAP];
    WCHAR environment_w[WIN32_PROCESS_ENVIRONMENT_CAP];
    PVOID process_heaps[1];
    PEB peb;
    TEB teb;
    WIN64_PROCESS_BLOCK *environment64;
    PVOID *tls_vector;
    WIN32_COMPAT_PROCESS_BLOCK *compat_environment32;
    PEB32 *peb32;
    TEB32 *teb32;
    uint32_t *tls_vector32;
    WIN64_LOADER_PROCESS_BLOCK *loader_environment64;
    HANDLE inherited_handles[WIN32_CHILD_INHERITED_HANDLE_CAP];
    DWORD inherited_handle_count;
    uint64_t exit_jmpbuf[9];
} WIN32_CHILD_CONTEXT;

static WIN32_CHILD_CONTEXT g_win32_children[MAX_WIN32_CHILDREN];

static PPEB win32_child_peb(WIN32_CHILD_CONTEXT *child)
{
    if (!child) return NULL;
    return child->environment64 ? &child->environment64->peb : &child->peb;
}

static TEB *win32_child_teb(WIN32_CHILD_CONTEXT *child)
{
    if (!child) return NULL;
    return child->environment64 ? &child->environment64->teb : &child->teb;
}

static PPEB win64_main_peb(void)
{
    return g_main_environment64 ? &g_main_environment64->peb : &g_peb;
}

static TEB *win64_main_teb(void)
{
    return g_main_environment64 ? &g_main_environment64->teb : &g_teb;
}

typedef struct {
    volatile BOOL active;
    volatile BOOL exit_requested;
    int owner_kernel_pid;
    int exit_requester_kernel_pid;
    NTSTATUS exit_status;
    uint64_t exit_jmpbuf[9];
} WIN32_MAIN_CONTEXT;

static WIN32_MAIN_CONTEXT g_win32_main;

static WIN32_CHILD_CONTEXT *win32_current_child(void)
{
#ifndef TEST_HARNESS
    extern DWORD win32_current_thread_process_id(void);
    extern int32_t proc_current_pid(void);
    extern uint64_t proc_get_gs_base(void);
    int pid = proc_current_pid();

    /* A child process's primary scheduler task is not represented in the
     * Win32 worker-thread table. Resolve it by kernel PID before consulting
     * that much larger table; this path is also used from exception handlers. */
    for (int i = 0; i < MAX_WIN32_CHILDREN; i++) {
        WIN32_CHILD_CONTEXT *child = &g_win32_children[i];
        if (!__atomic_load_n(&child->used, __ATOMIC_ACQUIRE))
            continue;
        if (__atomic_load_n(&child->kernel_pid, __ATOMIC_ACQUIRE) == pid)
            return child;
    }

    /* The root PE task intentionally has no child context or worker-table
     * entry. A negative answer is definitive for its primary scheduler PID. */
    if (__atomic_load_n(&g_win32_main.active, __ATOMIC_ACQUIRE) &&
        g_win32_main.owner_kernel_pid == pid)
        return NULL;

    /* Worker TEBs identify their owning child without touching the sparse,
     * dynamically sized thread-context array. */
    TEB *teb = (TEB *)(ULONG_PTR)proc_get_gs_base();
    PPEB process_peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    if (process_peb) {
        for (int i = 0; i < MAX_WIN32_CHILDREN; i++) {
            WIN32_CHILD_CONTEXT *child = &g_win32_children[i];
            if (__atomic_load_n(&child->used, __ATOMIC_ACQUIRE) &&
                (process_peb == win32_child_peb(child) ||
                 process_peb == &child->peb))
                return child;
        }
    }

    /* Fall back for early worker startup, before its TEB is authoritative. */
    DWORD thread_owner_pid = win32_current_thread_process_id();
    if (thread_owner_pid) {
        for (int i = 0; i < MAX_WIN32_CHILDREN; i++) {
            WIN32_CHILD_CONTEXT *child = &g_win32_children[i];
            if (__atomic_load_n(&child->used, __ATOMIC_ACQUIRE) &&
                child->process_id == thread_owner_pid)
                return child;
        }
    }
#endif
    return NULL;
}

static void win32_apply_console_parameters(
    PRTL_USER_PROCESS_PARAMETERS parameters, DWORD process_id)
{
    HANDLE console = NULL;
    HANDLE input = NULL;
    HANDLE output = NULL;
    HANDLE error = NULL;
    (void)kernel32_query_process_console(process_id, &console, &input,
                                         &output, &error);
    parameters->ConsoleHandle = console;
    parameters->StandardInput = input;
    parameters->StandardOutput = output;
    parameters->StandardError = error;
}

static void win32_apply_console_parameters32(
    WIN32_RTL_USER_PROCESS_PARAMETERS32 *parameters, DWORD process_id)
{
    HANDLE console = NULL;
    HANDLE input = NULL;
    HANDLE output = NULL;
    HANDLE error = NULL;
    (void)kernel32_query_process_console(process_id, &console, &input,
                                         &output, &error);
    parameters->ConsoleHandle = (uint32_t)(ULONG_PTR)console;
    parameters->StandardInput = (uint32_t)(ULONG_PTR)input;
    parameters->StandardOutput = (uint32_t)(ULONG_PTR)output;
    parameters->StandardError = (uint32_t)(ULONG_PTR)error;
}

void win32_refresh_current_console_parameters(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) {
        PRTL_USER_PROCESS_PARAMETERS parameters = child->environment64
            ? &child->environment64->process_parameters
            : &child->process_parameters;
        win32_apply_console_parameters(parameters,
                                       child->process_id);
        if (child->compat_environment32)
            win32_apply_console_parameters32(
                &child->compat_environment32->process_parameters,
                child->process_id);
        return;
    }

    PRTL_USER_PROCESS_PARAMETERS parameters = g_main_environment64
        ? &g_main_environment64->process_parameters
        : &g_process_parameters;
    win32_apply_console_parameters(parameters, 1);
    if (g_main_compat_environment32)
        win32_apply_console_parameters32(
            &g_main_compat_environment32->process_parameters, 1);
}

static BOOL win32_wide_append_ascii(WCHAR *destination, SIZE_T capacity,
                                     SIZE_T *length, const char *source,
                                     BOOL path)
{
    if (!destination || !capacity || !length || !source) return FALSE;
    while (*source) {
        if (*length + 1 >= capacity) return FALSE;
        char value = *source++;
        if (path && value == '/') value = '\\';
        destination[(*length)++] = (WCHAR)(BYTE)value;
    }
    destination[*length] = 0;
    return TRUE;
}

static BOOL win32_build_image_path_w(const char *relative, WCHAR *destination,
                                      SIZE_T capacity)
{
    SIZE_T length = 0;
    if (!relative) relative = "";
    if (relative[0] && relative[1] == ':')
        return win32_wide_append_ascii(destination, capacity, &length,
                                        relative, TRUE);
    while (*relative == '\\' || *relative == '/') relative++;
    return win32_wide_append_ascii(destination, capacity, &length, "C:\\",
                                    TRUE) &&
           win32_wide_append_ascii(destination, capacity, &length, relative,
                                    TRUE);
}

static BOOL win32_build_current_directory_w(const char *relative,
                                             WCHAR *destination,
                                             SIZE_T capacity)
{
    SIZE_T length = 0;
    if (!relative) relative = "";
    if (relative[0] && relative[1] == ':') {
        return win32_wide_append_ascii(destination, capacity, &length,
                                        relative, TRUE);
    }
    while (*relative == '\\' || *relative == '/') relative++;
    return win32_wide_append_ascii(destination, capacity, &length,
                                    "C:\\", TRUE) &&
           win32_wide_append_ascii(destination, capacity, &length, relative,
                                    TRUE);
}

static BOOL win32_copy_ascii_w(const char *source, WCHAR *destination,
                               SIZE_T capacity)
{
    SIZE_T length = 0;
    if (!source) source = "";
    return win32_wide_append_ascii(destination, capacity, &length, source,
                                    FALSE);
}

static void win32_init_unicode_string(PUNICODE_STRING string, WCHAR *buffer)
{
    SIZE_T chars = 0;
    while (buffer && buffer[chars]) chars++;
    string->Length = (USHORT)(chars * sizeof(WCHAR));
    string->MaximumLength = (USHORT)((chars + 1) * sizeof(WCHAR));
    string->Buffer = buffer;
}

static BOOL win32_copy_ascii(const char *source, char *destination,
                             SIZE_T capacity)
{
    SIZE_T length = 0;
    if (!source) source = "";
    if (!destination || !capacity) return FALSE;
    while (source[length] && length + 1 < capacity) {
        destination[length] = source[length];
        length++;
    }
    if (source[length]) return FALSE;
    destination[length] = 0;
    return TRUE;
}

static void win32_init_unicode_string32(WIN32_UNICODE_STRING32 *string,
                                        WCHAR *buffer)
{
    SIZE_T chars = 0;
    while (buffer && buffer[chars]) chars++;
    string->Length = (USHORT)(chars * sizeof(WCHAR));
    string->MaximumLength = (USHORT)((chars + 1) * sizeof(WCHAR));
    string->Buffer = (uint32_t)(ULONG_PTR)buffer;
}

static WIN32_COMPAT_PROCESS_BLOCK *win32_compat_block_for_process(
    DWORD process_id)
{
    if (process_id == 1 && g_main_compat_environment32)
        return g_main_compat_environment32;

    for (int i = 0; i < MAX_WIN32_CHILDREN; i++) {
        WIN32_CHILD_CONTEXT *child = &g_win32_children[i];
        if (__atomic_load_n(&child->used, __ATOMIC_ACQUIRE) &&
            child->process_id == process_id)
            return child->compat_environment32;
    }
    return NULL;
}

static WIN64_LOADER_PROCESS_BLOCK *win64_loader_block_for_process(
    DWORD process_id)
{
    if (process_id == 1 && g_main_environment64)
        return &g_main_environment64->loader;

    for (int i = 0; i < MAX_WIN32_CHILDREN; i++) {
        WIN32_CHILD_CONTEXT *child = &g_win32_children[i];
        if (__atomic_load_n(&child->used, __ATOMIC_ACQUIRE) &&
            child->process_id == process_id)
            return child->loader_environment64;
    }
    return NULL;
}

static WIN32_LIST_ENTRY32 *win32_loader_head32(
    WIN32_COMPAT_PROCESS_BLOCK *block, unsigned list)
{
    if (list == 0) return &block->loader_data.InLoadOrderModuleList;
    if (list == 1) return &block->loader_data.InMemoryOrderModuleList;
    return &block->loader_data.InInitializationOrderModuleList;
}

static WIN32_LIST_ENTRY32 *win32_loader_link32(
    WIN32_LDR_MODULE32 *module, unsigned list)
{
    if (list == 0) return &module->entry.InLoadOrderLinks;
    if (list == 1) return &module->entry.InMemoryOrderLinks;
    return &module->entry.InInitializationOrderLinks;
}

static uint32_t win32_loader_pointer32(const void *pointer)
{
    return (uint32_t)(ULONG_PTR)pointer;
}

static LIST_ENTRY *win64_loader_head(WIN64_LOADER_PROCESS_BLOCK *block,
                                     unsigned list)
{
    if (list == 0) return &block->loader_data.InLoadOrderModuleList;
    if (list == 1) return &block->loader_data.InMemoryOrderModuleList;
    return &block->loader_data.InInitializationOrderModuleList;
}

static LIST_ENTRY *win64_loader_link(WIN64_LDR_MODULE *module,
                                     unsigned list)
{
    if (list == 0) return &module->entry.InLoadOrderLinks;
    if (list == 1) return &module->entry.InMemoryOrderLinks;
    return &module->entry.InInitializationOrderLinks;
}

static void win32_initialize_loader32(WIN32_COMPAT_PROCESS_BLOCK *block)
{
    if (!block) return;

    memset(&block->peb_lock, 0, sizeof(block->peb_lock));
    block->peb_lock.LockCount = -1;
    memset(&block->loader_lock, 0, sizeof(block->loader_lock));
    block->loader_lock.LockCount = -1;
    memset(&block->loader_data, 0, sizeof(block->loader_data));
    block->loader_count = 0;
    block->loader_data.Length = sizeof(block->loader_data);
    block->loader_data.Initialized = TRUE;

    for (unsigned list = 0; list < 3; list++) {
        WIN32_LIST_ENTRY32 *head = win32_loader_head32(block, list);
        head->Flink = win32_loader_pointer32(head);
        head->Blink = win32_loader_pointer32(head);
    }
    block->peb.FastPebLock = win32_loader_pointer32(&block->peb_lock);
    block->peb.Ldr = win32_loader_pointer32(&block->loader_data);
    block->peb.LoaderLock = win32_loader_pointer32(&block->loader_lock);
}

static void win64_initialize_loader(PPEB peb,
                                    WIN64_LOADER_PROCESS_BLOCK *block)
{
    if (!peb || !block) return;

    memset(block, 0, sizeof(*block));
    block->peb_lock.LockCount = -1;
    block->loader_lock.LockCount = -1;
    block->loader_data.Length = sizeof(block->loader_data);
    block->loader_data.Initialized = TRUE;
    for (unsigned list = 0; list < 3; list++) {
        LIST_ENTRY *head = win64_loader_head(block, list);
        head->Flink = head;
        head->Blink = head;
    }
    peb->FastPebLock = &block->peb_lock;
    peb->Ldr = &block->loader_data;
    peb->LoaderLock = &block->loader_lock;
}

PVOID win32_current_peb_lock(void)
{
    DWORD process_id = win32_current_process_id();
    if (!process_id) process_id = 1;

    WIN32_COMPAT_PROCESS_BLOCK *block =
        win32_compat_block_for_process(process_id);
    if (block) return &block->peb_lock;

    WIN64_LOADER_PROCESS_BLOCK *block64 =
        win64_loader_block_for_process(process_id);
    return block64 ? &block64->peb_lock : NULL;
}

BOOL win32_is_current_loader_lock(PVOID lock)
{
    if (!lock) return FALSE;
    DWORD process_id = win32_current_process_id();
    if (!process_id) process_id = 1;
    WIN32_COMPAT_PROCESS_BLOCK *block =
        win32_compat_block_for_process(process_id);
    if (block && lock == (PVOID)&block->loader_lock) return TRUE;
    WIN64_LOADER_PROCESS_BLOCK *block64 =
        win64_loader_block_for_process(process_id);
    return block64 && lock == (PVOID)&block64->loader_lock;
}

void win32_set_loader_lock_state(ULONG process_id, DWORD thread_id,
                                 unsigned depth)
{
    WIN32_COMPAT_PROCESS_BLOCK *block =
        win32_compat_block_for_process(process_id);
    LONG recursion = depth > 0x7FFFFFFFU
        ? 0x7FFFFFFF : (LONG)depth;
    if (block) {
        RTL_CRITICAL_SECTION32 *lock = &block->loader_lock;
        __atomic_store_n(&lock->OwningThread, depth ? thread_id : 0,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&lock->RecursionCount, recursion, __ATOMIC_RELAXED);
        __atomic_store_n(&lock->LockCount, depth ? recursion - 1 : -1,
                         __ATOMIC_RELEASE);
    }

    WIN64_LOADER_PROCESS_BLOCK *block64 =
        win64_loader_block_for_process(process_id);
    if (block64) {
        RTL_CRITICAL_SECTION *lock = &block64->loader_lock;
        __atomic_store_n(&lock->OwningThread,
                         depth ? (HANDLE)(ULONG_PTR)thread_id : NULL,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&lock->RecursionCount, recursion, __ATOMIC_RELAXED);
        __atomic_store_n(&lock->LockCount, depth ? recursion - 1 : -1,
                         __ATOMIC_RELEASE);
    }
}

static const char *win32_loader_base_name(const char *path)
{
    const char *base = path ? path : "";
    for (const char *cursor = base; *cursor; cursor++) {
        if (*cursor == '\\' || *cursor == '/') base = cursor + 1;
    }
    return base;
}

static BOOL win32_loader_fill_names32(WIN32_LDR_MODULE32 *module,
                                      const char *image_name,
                                      BOOL system_module)
{
    const char *base_name = win32_loader_base_name(image_name);
    if (!base_name[0]) base_name = "<image>";

    if (system_module) {
        SIZE_T length = 0;
        if (!win32_wide_append_ascii(
                module->full_name, WIN32_PROCESS_IMAGE_PATH_CAP, &length,
                "C:\\Windows\\System32\\", TRUE) ||
            !win32_wide_append_ascii(
                module->full_name, WIN32_PROCESS_IMAGE_PATH_CAP, &length,
                base_name, TRUE))
            return FALSE;
    } else if (!win32_build_image_path_w(
                   image_name, module->full_name,
                   WIN32_PROCESS_IMAGE_PATH_CAP)) {
        return FALSE;
    }

    if (!win32_copy_ascii_w(base_name, module->base_name,
                            sizeof(module->base_name) /
                                sizeof(module->base_name[0])))
        return FALSE;

    win32_init_unicode_string32(&module->entry.FullDllName,
                                module->full_name);
    win32_init_unicode_string32(&module->entry.BaseDllName,
                                module->base_name);
    return TRUE;
}

static BOOL win64_loader_fill_names(WIN64_LDR_MODULE *module,
                                    const char *image_name,
                                    BOOL system_module)
{
    const char *base_name = win32_loader_base_name(image_name);
    if (!base_name[0]) base_name = "<image>";

    if (system_module) {
        SIZE_T length = 0;
        if (!win32_wide_append_ascii(
                module->full_name, WIN32_PROCESS_IMAGE_PATH_CAP, &length,
                "C:\\Windows\\System32\\", TRUE) ||
            !win32_wide_append_ascii(
                module->full_name, WIN32_PROCESS_IMAGE_PATH_CAP, &length,
                base_name, TRUE))
            return FALSE;
    } else if (!win32_build_image_path_w(
                   image_name, module->full_name,
                   WIN32_PROCESS_IMAGE_PATH_CAP)) {
        return FALSE;
    }

    if (!win32_copy_ascii_w(base_name, module->base_name,
                            sizeof(module->base_name) /
                                sizeof(module->base_name[0])))
        return FALSE;

    win32_init_unicode_string(&module->entry.FullDllName,
                              module->full_name);
    win32_init_unicode_string(&module->entry.BaseDllName,
                              module->base_name);
    return TRUE;
}

static void win32_loader_append32(WIN32_COMPAT_PROCESS_BLOCK *block,
                                  USHORT slot)
{
    WIN32_LDR_MODULE32 *module = &block->loader_modules[slot];
    for (unsigned list = 0; list < 3; list++) {
        WIN32_LIST_ENTRY32 *head = win32_loader_head32(block, list);
        WIN32_LIST_ENTRY32 *node = win32_loader_link32(module, list);
        WIN32_LIST_ENTRY32 *tail = head;
        if (block->loader_count) {
            USHORT tail_slot =
                block->loader_order[block->loader_count - 1];
            tail = win32_loader_link32(
                &block->loader_modules[tail_slot], list);
        }

        node->Flink = win32_loader_pointer32(head);
        node->Blink = win32_loader_pointer32(tail);
        tail->Flink = win32_loader_pointer32(node);
        head->Blink = win32_loader_pointer32(node);
    }

    block->loader_order[block->loader_count++] = slot;
}

static void win32_loader_remove32(WIN32_COMPAT_PROCESS_BLOCK *block,
                                  USHORT slot)
{
    USHORT position = block->loader_count;
    for (USHORT i = 0; i < block->loader_count; i++) {
        if (block->loader_order[i] == slot) {
            position = i;
            break;
        }
    }

    if (position < block->loader_count) {
        for (unsigned list = 0; list < 3; list++) {
            WIN32_LIST_ENTRY32 *head = win32_loader_head32(block, list);
            WIN32_LIST_ENTRY32 *previous = position
                ? win32_loader_link32(
                      &block->loader_modules[
                          block->loader_order[position - 1]], list)
                : head;
            WIN32_LIST_ENTRY32 *next = position + 1 < block->loader_count
                ? win32_loader_link32(
                      &block->loader_modules[
                          block->loader_order[position + 1]], list)
                : head;
            previous->Flink = win32_loader_pointer32(next);
            next->Blink = win32_loader_pointer32(previous);
        }

        for (USHORT i = position + 1; i < block->loader_count; i++)
            block->loader_order[i - 1] = block->loader_order[i];
        block->loader_count--;
        block->loader_order[block->loader_count] = 0;
    }

    memset(&block->loader_modules[slot], 0,
           sizeof(block->loader_modules[slot]));
}

static void win64_loader_append(WIN64_LOADER_PROCESS_BLOCK *block,
                                USHORT slot)
{
    WIN64_LDR_MODULE *module = &block->loader_modules[slot];
    for (unsigned list = 0; list < 3; list++) {
        LIST_ENTRY *head = win64_loader_head(block, list);
        LIST_ENTRY *node = win64_loader_link(module, list);
        LIST_ENTRY *tail = head;
        if (block->loader_count) {
            USHORT tail_slot =
                block->loader_order[block->loader_count - 1];
            tail = win64_loader_link(
                &block->loader_modules[tail_slot], list);
        }

        node->Flink = head;
        node->Blink = tail;
        tail->Flink = node;
        head->Blink = node;
    }
    block->loader_order[block->loader_count++] = slot;
}

static void win64_loader_remove(WIN64_LOADER_PROCESS_BLOCK *block,
                                USHORT slot)
{
    USHORT position = block->loader_count;
    for (USHORT i = 0; i < block->loader_count; i++) {
        if (block->loader_order[i] == slot) {
            position = i;
            break;
        }
    }

    if (position < block->loader_count) {
        for (unsigned list = 0; list < 3; list++) {
            LIST_ENTRY *head = win64_loader_head(block, list);
            LIST_ENTRY *previous = position
                ? win64_loader_link(
                      &block->loader_modules[
                          block->loader_order[position - 1]], list)
                : head;
            LIST_ENTRY *next = position + 1 < block->loader_count
                ? win64_loader_link(
                      &block->loader_modules[
                          block->loader_order[position + 1]], list)
                : head;
            previous->Flink = next;
            next->Blink = previous;
        }

        for (USHORT i = position + 1; i < block->loader_count; i++)
            block->loader_order[i - 1] = block->loader_order[i];
        block->loader_count--;
        block->loader_order[block->loader_count] = 0;
    }

    memset(&block->loader_modules[slot], 0,
           sizeof(block->loader_modules[slot]));
}

static NTSTATUS win64_publish_loader_image(
    WIN64_LOADER_PROCESS_BLOCK *block, PPE_IMAGE_INFO info,
    const char *image_name, BOOL system_module)
{
    if (!block) return STATUS_SUCCESS;

    for (USHORT slot = 0; slot < WIN32_LDR_MODULE_CAPACITY; slot++) {
        WIN64_LDR_MODULE *module = &block->loader_modules[slot];
        if (module->used && module->entry.DllBase == info->ImageBase)
            return STATUS_SUCCESS;
    }

    USHORT slot = 0;
    if (info->IsDLL) {
        slot = 1;
        while (slot < WIN32_LDR_MODULE_CAPACITY &&
               block->loader_modules[slot].used)
            slot++;
        if (slot == WIN32_LDR_MODULE_CAPACITY ||
            block->loader_count == WIN32_LDR_MODULE_CAPACITY)
            return STATUS_NO_MEMORY;
    } else if (block->loader_modules[0].used) {
        win64_loader_remove(block, 0);
    }

    WIN64_LDR_MODULE *module = &block->loader_modules[slot];
    memset(module, 0, sizeof(*module));
    module->entry.DllBase = info->ImageBase;
    module->entry.EntryPoint = info->EntryPoint;
    module->entry.SizeOfImage = info->SizeOfImage;
    module->entry.LoadCount = info->IsDLL ? 1 : 0xFFFFU;
    module->entry.OriginalBase = (ULONG_PTR)info->PreferredBase;
    module->entry.ReferenceCount = 1;
    if (!win64_loader_fill_names(module, image_name, system_module)) {
        memset(module, 0, sizeof(*module));
        return STATUS_NAME_TOO_LONG;
    }
    module->used = TRUE;
    win64_loader_append(block, slot);
    return STATUS_SUCCESS;
}

NTSTATUS win32_publish_loader_image(PPE_IMAGE_INFO info,
                                    const char *image_name,
                                    BOOL system_module)
{
    if (!info || !info->ImageBase || !info->SizeOfImage)
        return STATUS_INVALID_PARAMETER;

    DWORD process_id = win32_current_process_id();
    if (!process_id) process_id = 1;
    if (!info->Is32Bit) {
        return win64_publish_loader_image(
            win64_loader_block_for_process(process_id), info,
            image_name, system_module);
    }
    WIN32_COMPAT_PROCESS_BLOCK *block =
        win32_compat_block_for_process(process_id);
    /* Loader self-tests can construct PE32 facades without running a PE32
     * process. There is no guest PEB to publish into in that case. */
    if (!block) return STATUS_SUCCESS;

    uint32_t image_base = (uint32_t)(ULONG_PTR)info->ImageBase;
    for (USHORT slot = 0; slot < WIN32_LDR_MODULE_CAPACITY; slot++) {
        WIN32_LDR_MODULE32 *module = &block->loader_modules[slot];
        if (module->used && module->entry.DllBase == image_base)
            return STATUS_SUCCESS;
    }

    USHORT slot = 0;
    if (info->IsDLL) {
        slot = 1;
        while (slot < WIN32_LDR_MODULE_CAPACITY &&
               block->loader_modules[slot].used)
            slot++;
        if (slot == WIN32_LDR_MODULE_CAPACITY ||
            block->loader_count == WIN32_LDR_MODULE_CAPACITY)
            return STATUS_NO_MEMORY;
    } else if (block->loader_modules[0].used) {
        win32_loader_remove32(block, 0);
    }

    WIN32_LDR_MODULE32 *module = &block->loader_modules[slot];
    memset(module, 0, sizeof(*module));
    module->entry.DllBase = image_base;
    module->entry.EntryPoint = (uint32_t)(ULONG_PTR)info->EntryPoint;
    module->entry.SizeOfImage = info->SizeOfImage;
    module->entry.LoadCount = info->IsDLL ? 1 : 0xFFFFU;
    module->entry.OriginalBase = (uint32_t)info->PreferredBase;
    module->entry.ReferenceCount = 1;
    if (!win32_loader_fill_names32(module, image_name, system_module)) {
        memset(module, 0, sizeof(*module));
        return STATUS_NAME_TOO_LONG;
    }
    module->used = TRUE;
    win32_loader_append32(block, slot);
    return STATUS_SUCCESS;
}

void win32_unpublish_loader_image(ULONG process_id, PVOID image_base)
{
    if (!process_id || !image_base) return;
    WIN32_COMPAT_PROCESS_BLOCK *block =
        win32_compat_block_for_process(process_id);
    if (block) {
        uint32_t base32 = (uint32_t)(ULONG_PTR)image_base;
        for (USHORT slot = 0; slot < WIN32_LDR_MODULE_CAPACITY; slot++) {
            WIN32_LDR_MODULE32 *module = &block->loader_modules[slot];
            if (module->used && module->entry.DllBase == base32) {
                win32_loader_remove32(block, slot);
                return;
            }
        }
    }

    WIN64_LOADER_PROCESS_BLOCK *block64 =
        win64_loader_block_for_process(process_id);
    if (!block64) return;
    for (USHORT slot = 0; slot < WIN32_LDR_MODULE_CAPACITY; slot++) {
        WIN64_LDR_MODULE *module = &block64->loader_modules[slot];
        if (module->used && module->entry.DllBase == image_base) {
            win64_loader_remove(block64, slot);
            return;
        }
    }
}

void win32_update_loader_image_reference(ULONG process_id, PVOID image_base,
                                         ULONG references, BOOL pinned)
{
    if (!process_id || !image_base) return;
    WIN32_COMPAT_PROCESS_BLOCK *block =
        win32_compat_block_for_process(process_id);
    if (block) {
        uint32_t base32 = (uint32_t)(ULONG_PTR)image_base;
        for (USHORT slot = 0; slot < WIN32_LDR_MODULE_CAPACITY; slot++) {
            WIN32_LDR_MODULE32 *module = &block->loader_modules[slot];
            if (!module->used || module->entry.DllBase != base32) continue;
            module->entry.LoadCount = pinned || references > 0xFFFEU
                ? 0xFFFFU : (USHORT)references;
            module->entry.ReferenceCount = pinned ? 0xFFFFFFFFU : references;
            return;
        }
    }

    WIN64_LOADER_PROCESS_BLOCK *block64 =
        win64_loader_block_for_process(process_id);
    if (!block64) return;
    for (USHORT slot = 0; slot < WIN32_LDR_MODULE_CAPACITY; slot++) {
        WIN64_LDR_MODULE *module = &block64->loader_modules[slot];
        if (!module->used || module->entry.DllBase != image_base) continue;
        module->entry.LoadCount = pinned || references > 0xFFFEU
            ? 0xFFFFU : (USHORT)references;
        module->entry.ReferenceCount = pinned ? 0xFFFFFFFFU : references;
        return;
    }
}

static BOOL win32_populate_compat_process_block_data(
    WIN32_COMPAT_PROCESS_BLOCK *block, DWORD process_id,
    const char *image_path, const char *exe_name, const char *command_line,
    const char *current_directory, BOOL reset)
{
    if (!block) return FALSE;

    if (!win32_copy_ascii(image_path, block->image_path,
                          sizeof(block->image_path)) ||
        !win32_copy_ascii(exe_name, block->exe_name,
                          sizeof(block->exe_name)) ||
        !win32_copy_ascii(command_line, block->command_line,
                          sizeof(block->command_line)) ||
        !win32_copy_ascii(current_directory, block->current_directory,
                          sizeof(block->current_directory)) ||
        !win32_build_image_path_w(image_path, block->image_path_w,
                                  WIN32_PROCESS_IMAGE_PATH_CAP) ||
        !win32_copy_ascii_w(command_line, block->command_line_w,
                            WIN32_CHILD_COMMAND_LINE_CAP) ||
        !win32_build_current_directory_w(
            current_directory, block->current_directory_w,
            WIN32_PROCESS_IMAGE_PATH_CAP) ||
        !win32_copy_ascii_w("WinSta0\\Default", block->desktop_info_w,
                            WIN32_PROCESS_DESKTOP_CAP))
        return FALSE;

    SIZE_T dll_length = 0;
    while (block->current_directory_w[dll_length] &&
           dll_length + 1 < WIN32_PROCESS_DLL_PATH_CAP) {
        block->dll_path_w[dll_length] =
            block->current_directory_w[dll_length];
        dll_length++;
    }
    if (block->current_directory_w[dll_length] ||
        !win32_wide_append_ascii(
            block->dll_path_w, WIN32_PROCESS_DLL_PATH_CAP, &dll_length,
            ";C:\\System;C:\\Windows\\System32;C:\\Windows", TRUE))
        return FALSE;

    SIZE_T environment_chars = kernel32_build_environment_block_w(
        process_id, block->environment_w,
        WIN32_PROCESS_ENVIRONMENT_CAP);
    if (environment_chars < 2 ||
        environment_chars > WIN32_PROCESS_ENVIRONMENT_CAP)
        return FALSE;

    WIN32_RTL_USER_PROCESS_PARAMETERS32 *parameters =
        &block->process_parameters;
    ULONG environment_version = parameters->EnvironmentVersion;
    if (reset) memset(parameters, 0, sizeof(*parameters));
    parameters->MaximumLength = sizeof(*parameters);
    parameters->Length = sizeof(*parameters);
    parameters->Flags = RTL_USER_PROC_PARAMS_NORMALIZED;
    win32_apply_console_parameters32(parameters, process_id);
    win32_init_unicode_string32(&parameters->CurrentDirectory.DosPath,
                                block->current_directory_w);
    parameters->CurrentDirectory.Handle = 0;
    win32_init_unicode_string32(&parameters->DllPath, block->dll_path_w);
    win32_init_unicode_string32(&parameters->ImagePathName,
                                block->image_path_w);
    win32_init_unicode_string32(&parameters->CommandLine,
                                block->command_line_w);
    parameters->Environment = (uint32_t)(ULONG_PTR)block->environment_w;
    parameters->WindowTitle = parameters->ImagePathName;
    win32_init_unicode_string32(&parameters->DesktopInfo,
                                block->desktop_info_w);
    parameters->EnvironmentSize = environment_chars * sizeof(WCHAR);
    parameters->EnvironmentVersion = environment_version + 1;
    parameters->ProcessGroupId = process_id;
    parameters->LoaderThreads = 1;
    block->process_heaps[0] = 0xBEEF0001U;
    block->peb.ProcessParameters = (uint32_t)(ULONG_PTR)parameters;
    block->peb.ProcessHeap = block->process_heaps[0];
    return TRUE;
}

static BOOL win32_populate_compat_process_block(WIN32_CHILD_CONTEXT *child,
                                                 BOOL reset)
{
    if (!child) return FALSE;
    return win32_populate_compat_process_block_data(
        child->compat_environment32, child->process_id,
        child->image_path, child->exe_name, child->command_line,
        child->current_directory, reset);
}

static BOOL win32_populate_process_parameters(
    PRTL_USER_PROCESS_PARAMETERS parameters,
    WCHAR *image_path_w, SIZE_T image_capacity,
    WCHAR *command_line_w, SIZE_T command_capacity,
    WCHAR *current_directory_w, SIZE_T current_directory_capacity,
    WCHAR *dll_path_w, SIZE_T dll_path_capacity,
    WCHAR *desktop_info_w, SIZE_T desktop_capacity,
    WCHAR *environment_w, SIZE_T environment_capacity,
    DWORD process_id, const char *image_path, const char *command_line,
    const char *current_directory, BOOL reset)
{
    if (reset) memset(parameters, 0, sizeof(*parameters));
    if (!win32_build_image_path_w(image_path, image_path_w, image_capacity) ||
        !win32_copy_ascii_w(command_line, command_line_w, command_capacity) ||
        !win32_build_current_directory_w(current_directory,
                                          current_directory_w,
                                          current_directory_capacity) ||
        !win32_copy_ascii_w("WinSta0\\Default", desktop_info_w,
                            desktop_capacity))
        return FALSE;

    SIZE_T dll_length = 0;
    while (current_directory_w[dll_length] &&
           dll_length + 1 < dll_path_capacity) {
        dll_path_w[dll_length] = current_directory_w[dll_length];
        dll_length++;
    }
    if (current_directory_w[dll_length] ||
        !win32_wide_append_ascii(
            dll_path_w, dll_path_capacity, &dll_length,
            ";C:\\System;C:\\Windows\\System32;C:\\Windows", TRUE))
        return FALSE;

    SIZE_T environment_chars = kernel32_build_environment_block_w(
        process_id, environment_w, environment_capacity);
    if (environment_chars < 2 || environment_chars > environment_capacity)
        return FALSE;

    parameters->MaximumLength = sizeof(*parameters);
    parameters->Length = sizeof(*parameters);
    parameters->Flags = RTL_USER_PROC_PARAMS_NORMALIZED;
    win32_apply_console_parameters(parameters, process_id);
    win32_init_unicode_string(&parameters->CurrentDirectory.DosPath,
                              current_directory_w);
    parameters->CurrentDirectory.Handle = NULL;
    win32_init_unicode_string(&parameters->DllPath, dll_path_w);
    win32_init_unicode_string(&parameters->ImagePathName, image_path_w);
    win32_init_unicode_string(&parameters->CommandLine, command_line_w);
    parameters->Environment = environment_w;
    parameters->WindowTitle = parameters->ImagePathName;
    win32_init_unicode_string(&parameters->DesktopInfo, desktop_info_w);
    parameters->EnvironmentSize = environment_chars * sizeof(WCHAR);
    parameters->EnvironmentVersion++;
    parameters->ProcessGroupId = process_id;
    parameters->LoaderThreads = 1;
    return TRUE;
}

static void win32_initialize_peb(PPEB peb,
                                 PRTL_USER_PROCESS_PARAMETERS parameters,
                                 PVOID image_base, PVOID process_heaps[1],
                                 USHORT subsystem,
                                 USHORT subsystem_major_version,
                                 USHORT subsystem_minor_version)
{
    extern uint32_t smp_cpu_count(void);
    memset(peb, 0, sizeof(*peb));
    process_heaps[0] = (PVOID)(ULONG_PTR)0xBEEF0001;
    peb->ImageBaseAddress = image_base;
    peb->ProcessParameters = parameters;
    peb->ProcessHeap = process_heaps[0];
    peb->NumberOfProcessors = smp_cpu_count();
    if (!peb->NumberOfProcessors) peb->NumberOfProcessors = 1;
    peb->HeapSegmentReserve = 64ULL * 1024 * 1024;
    peb->HeapSegmentCommit = 64ULL * 1024;
    peb->NumberOfHeaps = 1;
    peb->MaximumNumberOfHeaps = 1;
    peb->ProcessHeaps = process_heaps;
    peb->GdiSharedHandleTable = ntdll_shared_gdi_table();
    peb->OSMajorVersion = WIN32_NT_VERSION_MAJOR;
    peb->OSMinorVersion = WIN32_NT_VERSION_MINOR;
    peb->OSBuildNumber = WIN32_NT_VERSION_BUILD;
    peb->OSPlatformId = WIN32_NT_PLATFORM_ID;
    peb->ImageSubsystem = subsystem;
    peb->ImageSubsystemMajorVersion = subsystem_major_version;
    peb->ImageSubsystemMinorVersion = subsystem_minor_version;
    peb->ActiveProcessAffinityMask = peb->NumberOfProcessors >= 64
        ? UINT64_MAX : ((1ULL << peb->NumberOfProcessors) - 1ULL);
}

static void win32_initialize_peb32(
    PEB32 *peb, WIN32_RTL_USER_PROCESS_PARAMETERS32 *parameters,
    uint32_t image_base, uint32_t process_heaps[1], USHORT subsystem,
    USHORT subsystem_major_version, USHORT subsystem_minor_version)
{
    extern uint32_t smp_cpu_count(void);
    uint32_t processors = smp_cpu_count();
    if (!processors) processors = 1;

    memset(peb, 0, sizeof(*peb));
    process_heaps[0] = 0xBEEF0001U;
    peb->ImageBaseAddress = image_base;
    peb->ProcessParameters = (uint32_t)(ULONG_PTR)parameters;
    peb->ProcessHeap = process_heaps[0];
    peb->NumberOfProcessors = processors;
    peb->HeapSegmentReserve = 64U * 1024U * 1024U;
    peb->HeapSegmentCommit = 64U * 1024U;
    peb->NumberOfHeaps = 1;
    peb->MaximumNumberOfHeaps = 1;
    peb->ProcessHeaps = (uint32_t)(ULONG_PTR)process_heaps;
    peb->OSMajorVersion = WIN32_NT_VERSION_MAJOR;
    peb->OSMinorVersion = WIN32_NT_VERSION_MINOR;
    peb->OSBuildNumber = WIN32_NT_VERSION_BUILD;
    peb->OSPlatformId = WIN32_NT_PLATFORM_ID;
    peb->ImageSubsystem = subsystem;
    peb->ImageSubsystemMajorVersion = subsystem_major_version;
    peb->ImageSubsystemMinorVersion = subsystem_minor_version;
    peb->ActiveProcessAffinityMask = processors >= 32
        ? UINT32_MAX : ((1U << processors) - 1U);
}

BOOL win32_refresh_current_process_parameters(void)
{
    extern BOOL nt_process_set_peb(PVOID process_object, const PEB *source);
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) {
        PPEB peb = win32_child_peb(child);
        PRTL_USER_PROCESS_PARAMETERS parameters = child->environment64
            ? &child->environment64->process_parameters
            : &child->process_parameters;
        WCHAR *image_path_w = child->environment64
            ? child->environment64->image_path_w : child->image_path_w;
        WCHAR *command_line_w = child->environment64
            ? child->environment64->command_line_w : child->command_line_w;
        WCHAR *current_directory_w = child->environment64
            ? child->environment64->current_directory_w
            : child->current_directory_w;
        WCHAR *dll_path_w = child->environment64
            ? child->environment64->dll_path_w : child->dll_path_w;
        WCHAR *desktop_info_w = child->environment64
            ? child->environment64->desktop_info_w : child->desktop_info_w;
        WCHAR *environment_w = child->environment64
            ? child->environment64->environment_w : child->environment_w;
        if (!peb->ProcessParameters) return TRUE;
        if (!win32_populate_process_parameters(
                parameters,
                image_path_w, WIN32_PROCESS_IMAGE_PATH_CAP,
                command_line_w, WIN32_CHILD_COMMAND_LINE_CAP,
                current_directory_w, WIN32_PROCESS_IMAGE_PATH_CAP,
                dll_path_w, WIN32_PROCESS_DLL_PATH_CAP,
                desktop_info_w, WIN32_PROCESS_DESKTOP_CAP,
                environment_w, WIN32_PROCESS_ENVIRONMENT_CAP,
                child->process_id, child->image_path, child->command_line,
                child->current_directory, FALSE))
            return FALSE;
        if (child->compat_environment32 &&
            !win32_populate_compat_process_block(child, FALSE))
            return FALSE;
        return nt_process_set_peb(child->process_object, peb);
    }

    PPEB peb = win64_main_peb();
    PRTL_USER_PROCESS_PARAMETERS parameters = g_main_environment64
        ? &g_main_environment64->process_parameters : &g_process_parameters;
    WCHAR *image_path_w = g_main_environment64
        ? g_main_environment64->image_path_w : g_image_path_w;
    WCHAR *command_line_w = g_main_environment64
        ? g_main_environment64->command_line_w : g_command_line_w;
    WCHAR *current_directory_w = g_main_environment64
        ? g_main_environment64->current_directory_w : g_current_directory_w;
    WCHAR *dll_path_w = g_main_environment64
        ? g_main_environment64->dll_path_w : g_dll_path_w;
    WCHAR *desktop_info_w = g_main_environment64
        ? g_main_environment64->desktop_info_w : g_desktop_info_w;
    WCHAR *environment_w = g_main_environment64
        ? g_main_environment64->environment_w : g_environment_w;
    if (!peb->ProcessParameters) return TRUE;
    extern char win32_image_path[260];
    extern char win32_exe_name[64];
    extern char win32_command_line[4096];
    if (!win32_populate_process_parameters(
            parameters,
            image_path_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            command_line_w, WIN32_CHILD_COMMAND_LINE_CAP,
            current_directory_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            dll_path_w, WIN32_PROCESS_DLL_PATH_CAP,
            desktop_info_w, WIN32_PROCESS_DESKTOP_CAP,
            environment_w, WIN32_PROCESS_ENVIRONMENT_CAP,
            1, win32_image_path, win32_command_line,
            kernel32_current_directory_relative(), FALSE))
        return FALSE;
    if (g_main_compat_environment32 &&
        !win32_populate_compat_process_block_data(
            g_main_compat_environment32, 1, win32_image_path,
            win32_exe_name, win32_command_line,
            kernel32_current_directory_relative(), FALSE))
        return FALSE;
    return nt_process_set_peb(NULL, peb);
}

TEB *win64_current_teb(void)
{
#ifndef TEST_HARNESS
    extern uint64_t proc_get_gs_base(void);
    TEB *teb = (TEB *)(ULONG_PTR)proc_get_gs_base();
    if (teb) return teb;
#endif
    return &g_teb;
}

void win64_set_current_teb(TEB *teb)
{
    uint64_t teb_addr = (uint64_t)(ULONG_PTR)teb;
#ifndef TEST_HARNESS
    __asm__ volatile (
        "mov $0xC0000101, %%ecx\n"
        "mov %0, %%rax\n"
        "mov %0, %%rdx\n"
        "shr $32, %%rdx\n"
        "wrmsr\n"
        :
        : "r"(teb_addr)
        : "rax", "rcx", "rdx", "memory"
    );
    {
        extern void proc_set_gs_base(uint64_t addr);
        proc_set_gs_base(teb_addr);
    }
#else
    #include <asm/prctl.h>
    extern int arch_prctl(int code, unsigned long addr);
    arch_prctl(ARCH_SET_GS, (unsigned long)teb_addr);
#endif
}

static void win64_reset_bootstrap_teb(void)
{
    memset(&g_teb, 0, sizeof(g_teb));
    g_teb.Self = &g_teb;
    g_teb.ProcessEnvironmentBlock = &g_peb;
    g_teb.ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)1;
    g_teb.ClientId.UniqueThread = (HANDLE)(ULONG_PTR)1;
    g_teb.ExceptionList = (PVOID)(ULONG_PTR)-1;
    win64_set_current_teb(&g_teb);
}

void win64_initialize_bootstrap_thread(void)
{
    if (g_teb.Self == &g_teb &&
        g_teb.ProcessEnvironmentBlock == &g_peb)
        return;

    win64_reset_bootstrap_teb();
}

const char *win32_current_exe_name(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) return child->compat_environment32
        ? child->compat_environment32->exe_name : child->exe_name;
    if (g_main_compat_environment32)
        return g_main_compat_environment32->exe_name;
    extern char win32_exe_name[64];
    return win32_exe_name;
}

BOOL win32_current_is_gui_app(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    USHORT subsystem = child ? child->subsystem : g_exe_subsystem;
    return subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI;
}

const char *win32_current_image_path(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) return child->compat_environment32
        ? child->compat_environment32->image_path : child->image_path;
    if (g_main_compat_environment32)
        return g_main_compat_environment32->image_path;
    extern char win32_image_path[260];
    return win32_image_path;
}

const char *win32_current_command_line(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) return child->compat_environment32
        ? child->compat_environment32->command_line : child->command_line;
    if (g_main_compat_environment32)
        return g_main_compat_environment32->command_line;
    extern char win32_command_line[4096];
    return win32_command_line;
}

const WCHAR *win32_current_command_line_w(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) {
        if (child->compat_environment32)
            return child->compat_environment32->command_line_w;
        if (child->environment64)
            return child->environment64->command_line_w;
        return child->command_line_w;
    }
    if (g_main_compat_environment32)
        return g_main_compat_environment32->command_line_w;
    if (g_main_environment64)
        return g_main_environment64->command_line_w;
    return g_command_line_w[0] ? g_command_line_w : NULL;
}

const char *win32_current_directory_override(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (!child) return NULL;
    /* The child context is authoritative.  The low-memory PE32 block is a
     * process-parameter snapshot and is refreshed after a CWD change. */
    const char *directory = child->current_directory;
    return directory[0] ? directory : NULL;
}

BOOL win32_set_current_directory_override(const char *path)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (!child || !path) return FALSE;

    SIZE_T i = 0;
    while (path[i] && i < WIN32_CHILD_CURRENT_DIRECTORY_CAP - 1) {
        child->current_directory[i] = path[i];
        i++;
    }
    if (path[i]) return FALSE;
    child->current_directory[i] = 0;
    return TRUE;
}

ULONG_PTR win32_current_image_base(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) return (ULONG_PTR)win32_child_peb(child)->ImageBaseAddress;
    return g_exe_image_base;
}

void win32_publish_current_image_base(PVOID image_base)
{
    extern BOOL nt_process_set_image_base(PVOID process_object,
                                           PVOID image_base);
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) {
        child->peb.ImageBaseAddress = image_base;
        win32_child_peb(child)->ImageBaseAddress = image_base;
        (void)nt_process_set_image_base(child->process_object, image_base);
        return;
    }

    g_exe_image_base = (ULONG_PTR)image_base;
    g_peb.ImageBaseAddress = image_base;
    win64_main_peb()->ImageBaseAddress = image_base;
    (void)nt_process_set_image_base(NULL, image_base);
}

DWORD win32_current_process_id(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    return child ? child->process_id : 1;
}

BOOL win32_process_cr3(DWORD process_id, uint64_t *out_cr3)
{
    extern uint64_t proc_get_cr3_pid(int pid);
    if (!process_id || !out_cr3) return FALSE;

    if (process_id == win32_current_process_id()) {
        uint64_t cr3 = proc_current_cr3();
        if (!cr3) cr3 = paging_get_kernel_cr3();
        *out_cr3 = cr3;
        return TRUE;
    }

    int kernel_pid = -1;
    if (process_id == 1 &&
        __atomic_load_n(&g_win32_main.active, __ATOMIC_ACQUIRE)) {
        kernel_pid = g_win32_main.owner_kernel_pid;
    } else {
        for (int i = 0; i < MAX_WIN32_CHILDREN; i++) {
            WIN32_CHILD_CONTEXT *child = &g_win32_children[i];
            if (__atomic_load_n(&child->used, __ATOMIC_ACQUIRE) &&
                child->process_id == process_id) {
                kernel_pid = child->kernel_pid;
                break;
            }
        }
    }
    if (kernel_pid <= 0) return FALSE;

    uint64_t cr3 = proc_get_cr3_pid(kernel_pid);
    if (!cr3) return FALSE;
    *out_cr3 = cr3;
    return TRUE;
}

DWORD win32_current_process_thread_id(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    return child ? child->thread_id : 1;
}

BOOL win32_main_termination_pending(void)
{
#ifndef TEST_HARNESS
    extern int32_t proc_current_pid(void);
    if (proc_current_pid() != g_win32_main.owner_kernel_pid)
        return FALSE;
#endif
    return __atomic_load_n(&g_win32_main.active, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&g_win32_main.exit_requested,
                           __ATOMIC_ACQUIRE) == TRUE;
}

void win32_main_termination_checkpoint(void)
{
    if (!win32_main_termination_pending()) return;

#ifndef TEST_HARNESS
    extern int32_t proc_current_pid(void);
    extern void kern_longjmp(uint64_t *buf, int val);
    extern void sched_reset_current_compat_ist1(void);
    NTSTATUS status = g_win32_main.exit_status;
    __atomic_store_n(&g_win32_main.exit_requested, FALSE, __ATOMIC_RELEASE);
    serial_puts("[WINEXEC-EXIT] delivered to main kpid=");
    serial_putdec((uint64_t)(uint32_t)proc_current_pid());
    serial_puts(" requester=");
    serial_putdec((uint64_t)(uint32_t)
                  g_win32_main.exit_requester_kernel_pid);
    serial_puts(" status=0x");
    serial_puthex((uint32_t)status, 8);
    serial_puts("\n");
    sched_reset_current_compat_ist1();
    kern_longjmp(g_win32_main.exit_jmpbuf, 2);
    __builtin_unreachable();
#endif
}

BOOL win32_terminate_current_main(NTSTATUS status)
{
    if (!__atomic_load_n(&g_win32_main.active, __ATOMIC_ACQUIRE) ||
        win32_current_process_id() != 1)
        return FALSE;

#ifndef TEST_HARNESS
    extern int32_t proc_current_pid(void);
    extern int proc_wake_pid(int pid);
    extern void proc_exit(int32_t code);
    int current_kernel_pid = proc_current_pid();

    if (current_kernel_pid == g_win32_main.owner_kernel_pid) {
        g_win32_main.exit_status = status;
        g_win32_main.exit_requester_kernel_pid = current_kernel_pid;
        __atomic_store_n(&g_win32_main.exit_requested, TRUE,
                         __ATOMIC_RELEASE);
        win32_main_termination_checkpoint();
        __builtin_unreachable();
    }

    BOOL expected = FALSE;
    if (__atomic_compare_exchange_n(&g_win32_main.exit_requested, &expected,
                                    (BOOL)2, FALSE, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        /* State 2 reserves the request slot but is not deliverable. Publish
         * the payload, then transition to TRUE with release ordering. */
        g_win32_main.exit_status = status;
        g_win32_main.exit_requester_kernel_pid = current_kernel_pid;
        __atomic_store_n(&g_win32_main.exit_requested, TRUE,
                         __ATOMIC_RELEASE);
        if (proc_wake_pid(g_win32_main.owner_kernel_pid) != 0) {
            __atomic_store_n(&g_win32_main.exit_requested, FALSE,
                             __ATOMIC_RELEASE);
            return FALSE;
        }
        serial_puts("[WINEXEC-EXIT] requested by worker kpid=");
        serial_putdec((uint64_t)(uint32_t)current_kernel_pid);
        serial_puts(" main=");
        serial_putdec((uint64_t)(uint32_t)g_win32_main.owner_kernel_pid);
        serial_puts(" status=0x");
        serial_puthex((uint32_t)status, 8);
        serial_puts("\n");
    }

    proc_exit((int32_t)status);
    __builtin_unreachable();
#else
    (void)status;
    return FALSE;
#endif
}

HANDLE win32_current_process_thread_handle(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    return child ? child->thread_handle : NULL;
}

PVOID win32_current_primary_thread_object(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (!child)
        return NULL;
#ifndef TEST_HARNESS
    extern int32_t proc_current_pid(void);
    if (child->kernel_pid != proc_current_pid())
        return NULL;
#endif
    return child->thread_object;
}

BOOL win32_resume_primary_thread_execution(PVOID thread_object)
{
    if (!thread_object) return FALSE;

    for (int i = 0; i < MAX_WIN32_CHILDREN; i++) {
        WIN32_CHILD_CONTEXT *child = &g_win32_children[i];
        if (!__atomic_load_n(&child->used, __ATOMIC_ACQUIRE) ||
            child->thread_object != thread_object)
            continue;

        int kernel_pid = __atomic_load_n(&child->kernel_pid,
                                         __ATOMIC_ACQUIRE);
        if (kernel_pid <= 0)
            return FALSE;

        extern int proc_wake_pid(int pid);
        if (proc_wake_pid(kernel_pid) < 0)
            return FALSE;

        serial_puts("[WINEXEC-CHILD] resumed pid=");
        serial_putdec(child->process_id);
        serial_puts(" kpid=");
        serial_putdec((uint64_t)(uint32_t)kernel_pid);
        serial_puts("\n");
        return TRUE;
    }
    return FALSE;
}

uint64_t *win32_current_child_jmpbuf(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (!child || !child->exit_ready) return NULL;
#ifndef TEST_HARNESS
    extern int32_t proc_current_pid(void);
    if (child->kernel_pid != proc_current_pid()) return NULL;
#endif
    return child->exit_jmpbuf;
}

/*
 * 32-bit TEB/PEB for PE32 compat mode.
 * PE32 (i386) code accesses TEB via FS segment with 4-byte pointer offsets.
 * The 64-bit TEB has 8-byte pointers, so offsets are all wrong for 32-bit code.
 * Example: 32-bit TEB.Self is at +0x18, but 64-bit TEB.Self is at +0x30.
 */
/* Native shims can query LastError while no PE32 task is active. The guest
 * never receives this bootstrap TEB: every live PE32 TEB is low-mapped. */
TEB32 g_teb32 = { .ExceptionList = UINT32_MAX };

static TEB32 *win32_main_teb32(void)
{
    return g_main_compat_environment32
        ? &g_main_compat_environment32->teb : NULL;
}

static PEB32 *win32_main_peb32(void)
{
    return g_main_compat_environment32
        ? &g_main_compat_environment32->peb : NULL;
}

static BOOL setup_main_compat_environment32(void)
{
    extern char win32_image_path[260];
    extern char win32_exe_name[64];
    extern char win32_command_line[4096];

    PVOID allocation = NULL;
    SIZE_T allocation_size = sizeof(WIN32_COMPAT_PROCESS_BLOCK);
    NTSTATUS status = nt_vm_allocate_compat32(
        &allocation, &allocation_size, MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);
    if (!NT_SUCCESS(status) || !allocation ||
        (uint64_t)(ULONG_PTR)allocation +
            sizeof(WIN32_COMPAT_PROCESS_BLOCK) > UINT32_MAX) {
        if (allocation)
            (void)nt_vm_release_allocation_for_process(1, allocation);
        return FALSE;
    }

    WIN32_COMPAT_PROCESS_BLOCK *block =
        (WIN32_COMPAT_PROCESS_BLOCK *)allocation;
    memset(block, 0, sizeof(*block));
    if (!win32_populate_compat_process_block_data(
            block, 1, win32_image_path, win32_exe_name,
            win32_command_line, kernel32_current_directory_relative(),
            TRUE)) {
        (void)nt_vm_release_allocation_for_process(1, allocation);
        return FALSE;
    }

    win32_initialize_peb32(&block->peb, &block->process_parameters,
                           0, block->process_heaps, g_exe_subsystem,
                           g_exe_subsystem_major_version,
                           g_exe_subsystem_minor_version);
    win32_initialize_loader32(block);
    block->teb.ExceptionList = UINT32_MAX;
    block->teb.Self = (uint32_t)(ULONG_PTR)&block->teb;
    block->teb.ClientId_UniqueProcess = 1;
    block->teb.ClientId_UniqueThread = 1;
    block->teb.ProcessEnvironmentBlock =
        (uint32_t)(ULONG_PTR)&block->peb;

    g_main_compat_environment32 = block;
    compat32_setup_teb(&block->teb);
    win32_tls_reset();

    if (compat32_current_teb() != &block->teb ||
        block->teb.Self != (uint32_t)(ULONG_PTR)&block->teb ||
        block->teb.ProcessEnvironmentBlock !=
            (uint32_t)(ULONG_PTR)&block->peb) {
        compat32_setup_teb(NULL);
        g_main_compat_environment32 = NULL;
        (void)nt_vm_release_allocation_for_process(1, allocation);
        return FALSE;
    }

    serial_puts("[WINEXEC] low TEB32=0x");
    serial_puthex((uint64_t)(ULONG_PTR)&block->teb, 8);
    serial_puts(" PEB32=0x");
    serial_puthex((uint64_t)(ULONG_PTR)&block->peb, 8);
    serial_puts(" params=0x");
    serial_puthex(block->peb.ProcessParameters, 8);
    serial_puts("\n");
    return TRUE;
}

static BOOL setup_environment(PVOID image_base, BOOL initialize_loader64)
{
    extern BOOL nt_process_set_peb(PVOID process_object, const PEB *source);
    extern char win32_image_path[260];
    extern char win32_command_line[4096];

    WIN64_PROCESS_BLOCK *environment = NULL;
    PRTL_USER_PROCESS_PARAMETERS parameters = &g_process_parameters;
    PVOID *process_heaps = g_process_heaps;
    WCHAR *image_path_w = g_image_path_w;
    WCHAR *command_line_w = g_command_line_w;
    WCHAR *current_directory_w = g_current_directory_w;
    WCHAR *dll_path_w = g_dll_path_w;
    WCHAR *desktop_info_w = g_desktop_info_w;
    WCHAR *environment_w = g_environment_w;
    PPEB peb = &g_peb;
    TEB *teb = &g_teb;

    memset(&g_peb, 0, sizeof(g_peb));
    memset(&g_teb, 0, sizeof(g_teb));
    g_main_environment64 = NULL;
    if (initialize_loader64) {
        environment = (WIN64_PROCESS_BLOCK *)VirtualAlloc(
            NULL, sizeof(*environment), MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE);
        if (!environment)
            return FALSE;
        memset(environment, 0, sizeof(*environment));
        g_main_environment64 = environment;
        parameters = &environment->process_parameters;
        process_heaps = environment->process_heaps;
        image_path_w = environment->image_path_w;
        command_line_w = environment->command_line_w;
        current_directory_w = environment->current_directory_w;
        dll_path_w = environment->dll_path_w;
        desktop_info_w = environment->desktop_info_w;
        environment_w = environment->environment_w;
        peb = &environment->peb;
        teb = &environment->teb;
    }

    if (!win32_populate_process_parameters(
            parameters, image_path_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            command_line_w, WIN32_CHILD_COMMAND_LINE_CAP,
            current_directory_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            dll_path_w, WIN32_PROCESS_DLL_PATH_CAP,
            desktop_info_w, WIN32_PROCESS_DESKTOP_CAP,
            environment_w, WIN32_PROCESS_ENVIRONMENT_CAP,
            1, win32_image_path, win32_command_line,
            kernel32_current_directory_relative(), TRUE))
        goto fail;
    win32_initialize_peb(peb, parameters, image_base, process_heaps,
                         g_exe_subsystem, g_exe_subsystem_major_version,
                         g_exe_subsystem_minor_version);
    if (environment)
        win64_initialize_loader(peb, &environment->loader);
    if (!nt_process_set_peb(NULL, peb) ||
        !nt_process_set_image_path(NULL, win32_image_path))
        goto fail;

    teb->Self = teb;
    teb->ProcessEnvironmentBlock = peb;
    teb->ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)1;
    teb->ClientId.UniqueThread = (HANDLE)(ULONG_PTR)1;
    teb->LastErrorValue = 0;
    teb->ExceptionList = (PVOID)(ULONG_PTR)-1;
    if (environment) {
        teb->ThreadLocalStoragePointer = environment->tls_vector;
        teb->TlsExpansionSlots = environment->tls_expansion;
    }
    win64_set_current_teb(teb);

    if (environment) {
        serial_puts("[WINEXEC] user TEB64=0x");
        serial_puthex((uint64_t)(ULONG_PTR)teb, 16);
        serial_puts(" PEB64=0x");
        serial_puthex((uint64_t)(ULONG_PTR)peb, 16);
        serial_puts(" params=0x");
        serial_puthex((uint64_t)(ULONG_PTR)parameters, 16);
        serial_puts("\n");
    }
    return TRUE;

fail:
    if (environment) {
        g_main_environment64 = NULL;
        (void)nt_vm_release_allocation_for_process(1, environment);
    }
    win64_reset_bootstrap_teb();
    return FALSE;
}

NTSTATUS win64_attach_tls(PE_IMAGE_INFO *info)
{
    if (!info || !info->ImageBase || info->Is32Bit ||
        info->SizeOfImage < sizeof(IMAGE_DOS_HEADER))
        return STATUS_INVALID_PARAMETER;

    BYTE *base = (BYTE *)info->ImageBase;
    uint64_t image_start = (uint64_t)(ULONG_PTR)base;
    uint64_t image_end = image_start + info->SizeOfImage;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        (ULONG)dos->e_lfanew > info->SizeOfImage - sizeof(IMAGE_NT_HEADERS64))
        return STATUS_INVALID_PARAMETER;

    PIMAGE_NT_HEADERS64 nt =
        (PIMAGE_NT_HEADERS64)(base + (ULONG)dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return STATUS_INVALID_PARAMETER;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_TLS)
        return STATUS_SUCCESS;

    IMAGE_DATA_DIRECTORY *dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir->VirtualAddress || !dir->Size)
        return STATUS_SUCCESS;
    if (dir->Size < sizeof(IMAGE_TLS_DIRECTORY64) ||
        dir->VirtualAddress > info->SizeOfImage - sizeof(IMAGE_TLS_DIRECTORY64))
        return STATUS_INVALID_PARAMETER;

    PIMAGE_TLS_DIRECTORY64 tls =
        (PIMAGE_TLS_DIRECTORY64)(base + dir->VirtualAddress);
    uint64_t raw_start = tls->StartAddressOfRawData;
    uint64_t raw_end = tls->EndAddressOfRawData;
    uint64_t index_addr = tls->AddressOfIndex;
    uint64_t callbacks_addr = tls->AddressOfCallBacks;
    if (raw_end < raw_start ||
        (raw_end != raw_start &&
         (raw_start < image_start || raw_end > image_end)) ||
        index_addr < image_start || index_addr > image_end - sizeof(uint32_t))
        return STATUS_INVALID_PARAMETER;

    uint64_t raw_size = raw_end - raw_start;
    uint64_t total_size = raw_size + tls->SizeOfZeroFill;
    if (total_size < raw_size || total_size > 16ULL * 1024 * 1024)
        return STATUS_NO_MEMORY;

    uint32_t callback_count = 0;
    if (callbacks_addr) {
        while (callback_count < 64) {
            uint64_t entry_addr = callbacks_addr +
                                  callback_count * sizeof(uint64_t);
            if (entry_addr < image_start ||
                entry_addr > image_end - sizeof(uint64_t))
                return STATUS_INVALID_PARAMETER;
            uint64_t callback = *(uint64_t *)(ULONG_PTR)entry_addr;
            if (!callback) break;
            if (callback < image_start || callback >= image_end)
                return STATUS_INVALID_PARAMETER;
            callback_count++;
        }
        if (callback_count == 64)
            return STATUS_INVALID_PARAMETER;
    }

    uint64_t pages = (total_size + 4095) / 4096;
    if (!pages) pages = 1;
    void *block_phys = mem_alloc_pages(pages);
    BYTE *block = block_phys ? (BYTE *)PHYS_TO_VIRT(block_phys) : NULL;
    if (!block) return STATUS_NO_MEMORY;
    for (uint64_t i = 0; i < pages * 4096; i++) block[i] = 0;
    for (uint64_t i = 0; i < raw_size; i++)
        block[i] = *(BYTE *)(ULONG_PTR)(raw_start + i);

    extern DWORD win64_tls_alloc_static_index(void);
    DWORD index = win64_tls_alloc_static_index();
    TEB *teb = win64_current_teb();
    PVOID *static_vector = teb
        ? (PVOID *)teb->ThreadLocalStoragePointer : NULL;
    if (index == (DWORD)-1 || !static_vector) {
        mem_free_pages(block_phys, pages);
        return STATUS_NO_MEMORY;
    }
    static_vector[index] = block;
    *(uint32_t *)(ULONG_PTR)index_addr = index;

    extern BOOL win64_tls_register_static(DWORD, PVOID, PVOID, SIZE_T,
                                           SIZE_T, PVOID, DWORD);
    if (!win64_tls_register_static(index, info->ImageBase,
                                   (PVOID)(ULONG_PTR)raw_start, raw_size,
                                   total_size,
                                   (PVOID)(ULONG_PTR)callbacks_addr,
                                   callback_count)) {
        static_vector[index] = NULL;
        mem_free_pages(block_phys, pages);
        return STATUS_NO_MEMORY;
    }

    serial_puts("[TLS64] slot=");
    serial_putdec(index);
    serial_puts(" template=0x");
    serial_puthex(raw_size, 8);
    serial_puts(" block=0x");
    serial_puthex((uint64_t)(ULONG_PTR)block, 16);
    serial_puts(" callbacks=");
    serial_putdec(callback_count);
    serial_puts("\n");

    typedef void (WINAPI *tls_callback_fn)(PVOID, DWORD, PVOID);
    for (uint32_t i = 0; i < callback_count; i++) {
        tls_callback_fn callback = (tls_callback_fn)(ULONG_PTR)
            (*(uint64_t *)(ULONG_PTR)(callbacks_addr + i * sizeof(uint64_t)));
        callback(info->ImageBase, DLL_PROCESS_ATTACH, NULL);
    }
    return STATUS_SUCCESS;
}

/* Filesystem access used by the PE image loader. */
extern void     *osfs2_find_ci(const char *name);
extern int       osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t  osfs2_file_size(void *file);

static void win32_child_run(WIN32_CHILD_CONTEXT *child);

#define WIN32_CHILD_ENTRY(n) \
    static void win32_child_entry_##n(void) \
    { \
        win32_child_run(&g_win32_children[n]); \
    }
WIN32_CHILD_ENTRY(0)
WIN32_CHILD_ENTRY(1)
WIN32_CHILD_ENTRY(2)
WIN32_CHILD_ENTRY(3)
WIN32_CHILD_ENTRY(4)
WIN32_CHILD_ENTRY(5)
WIN32_CHILD_ENTRY(6)
WIN32_CHILD_ENTRY(7)
WIN32_CHILD_ENTRY(8)
WIN32_CHILD_ENTRY(9)
WIN32_CHILD_ENTRY(10)
WIN32_CHILD_ENTRY(11)
WIN32_CHILD_ENTRY(12)
WIN32_CHILD_ENTRY(13)
WIN32_CHILD_ENTRY(14)
WIN32_CHILD_ENTRY(15)
#undef WIN32_CHILD_ENTRY

static void (*const win32_child_entries[MAX_WIN32_CHILDREN])(void) = {
    win32_child_entry_0, win32_child_entry_1,
    win32_child_entry_2, win32_child_entry_3,
    win32_child_entry_4, win32_child_entry_5,
    win32_child_entry_6, win32_child_entry_7,
    win32_child_entry_8, win32_child_entry_9,
    win32_child_entry_10, win32_child_entry_11,
    win32_child_entry_12, win32_child_entry_13,
    win32_child_entry_14, win32_child_entry_15,
};

static void child_copy_string(char *dst, SIZE_T capacity, const char *src)
{
    SIZE_T i = 0;
    if (!capacity) return;
    if (src)
        while (src[i] && i + 1 < capacity) {
            dst[i] = src[i];
            i++;
        }
    dst[i] = 0;
}

DWORD win32_process_snapshot_capacity(void)
{
    return MAX_WIN32_CHILDREN + 1;
}

BOOL win32_process_snapshot_slot(DWORD slot, DWORD *process_id,
                                 DWORD *parent_process_id, char *exe_name,
                                 SIZE_T exe_name_capacity)
{
    if (!process_id || !parent_process_id || !exe_name ||
        !exe_name_capacity || slot > MAX_WIN32_CHILDREN)
        return FALSE;

    const char *name;
    if (slot == 0) {
        extern char win32_exe_name[64];
        *process_id = 1;
        *parent_process_id = 0;
        name = win32_exe_name;
    } else {
        WIN32_CHILD_CONTEXT *child = &g_win32_children[slot - 1];
        if (!__atomic_load_n(&child->used, __ATOMIC_ACQUIRE) ||
            !child->process_id)
            return FALSE;
        *process_id = child->process_id;
        *parent_process_id = child->parent_process_id;
        name = child->exe_name;
    }

    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;

    SIZE_T i = 0;
    while (base[i] && i + 1 < exe_name_capacity) {
        exe_name[i] = base[i];
        i++;
    }
    exe_name[i] = 0;
    return TRUE;
}

static int child_image_bitness(const BYTE *data, uint64_t size,
                               USHORT *subsystem,
                               USHORT *subsystem_major_version,
                               USHORT *subsystem_minor_version,
                               USHORT *dll_characteristics)
{
    if (!data || size < sizeof(IMAGE_DOS_HEADER)) return 0;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
        return 0;
    uint64_t nt_offset = (uint32_t)dos->e_lfanew;
    uint64_t need = sizeof(ULONG) + sizeof(IMAGE_FILE_HEADER) + sizeof(USHORT);
    if (nt_offset > size || need > size - nt_offset) return 0;
    BYTE *nt = (BYTE *)data + nt_offset;
    if (*(ULONG *)nt != IMAGE_NT_SIGNATURE) return 0;
    PIMAGE_FILE_HEADER file = (PIMAGE_FILE_HEADER)(nt + sizeof(ULONG));
    uint64_t optional_offset = nt_offset + sizeof(ULONG) +
                               sizeof(IMAGE_FILE_HEADER);
    USHORT magic = *(USHORT *)(data + optional_offset);
    if (file->Machine == IMAGE_FILE_MACHINE_I386 &&
        magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        if (file->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32) ||
            optional_offset > size ||
            sizeof(IMAGE_OPTIONAL_HEADER32) > size - optional_offset)
            return 0;
        PIMAGE_OPTIONAL_HEADER32 optional =
            (PIMAGE_OPTIONAL_HEADER32)(data + optional_offset);
        if (subsystem)
            *subsystem = optional->Subsystem;
        if (subsystem_major_version)
            *subsystem_major_version = optional->MajorSubsystemVersion;
        if (subsystem_minor_version)
            *subsystem_minor_version = optional->MinorSubsystemVersion;
        if (dll_characteristics)
            *dll_characteristics = optional->DllCharacteristics;
        return 32;
    }
    if (file->Machine == IMAGE_FILE_MACHINE_AMD64 &&
        magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (file->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
            optional_offset > size ||
            sizeof(IMAGE_OPTIONAL_HEADER64) > size - optional_offset)
            return 0;
        PIMAGE_OPTIONAL_HEADER64 optional =
            (PIMAGE_OPTIONAL_HEADER64)(data + optional_offset);
        if (subsystem)
            *subsystem = optional->Subsystem;
        if (subsystem_major_version)
            *subsystem_major_version = optional->MajorSubsystemVersion;
        if (subsystem_minor_version)
            *subsystem_minor_version = optional->MinorSubsystemVersion;
        if (dll_characteristics)
            *dll_characteristics = optional->DllCharacteristics;
        return 64;
    }
    return 0;
}

static void child_free_compat_environment32(WIN32_CHILD_CONTEXT *child)
{
    if (!child) return;
    if (child->tls_vector32)
        VirtualFree(child->tls_vector32, 0, MEM_RELEASE);
    if (child->compat_environment32)
        VirtualFree(child->compat_environment32, 0, MEM_RELEASE);
    if (child->environment64)
        (void)nt_vm_release_allocation_for_process(
            child->process_id, child->environment64);
    else if (child->loader_environment64)
        VirtualFree(child->loader_environment64, 0, MEM_RELEASE);
    child->tls_vector32 = NULL;
    child->compat_environment32 = NULL;
    child->peb32 = NULL;
    child->teb32 = NULL;
    child->environment64 = NULL;
    child->loader_environment64 = NULL;
}

static NTSTATUS setup_child_environment(WIN32_CHILD_CONTEXT *child,
                                        PVOID image_base,
                                        PVOID stack_base,
                                        PVOID stack_top,
                                        BOOL compat32)
{
    extern BOOL nt_process_set_peb(PVOID process_object, const PEB *source);
    void *tls_phys = compat32 ? mem_alloc_pages(4) : NULL;
    child->tls_vector = tls_phys ? (PVOID *)PHYS_TO_VIRT(tls_phys) : NULL;
    child->tls_vector32 = compat32
        ? (uint32_t *)VirtualAlloc(NULL, 4096,
                                   MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE)
        : NULL;
    child->compat_environment32 = compat32
        ? (WIN32_COMPAT_PROCESS_BLOCK *)VirtualAlloc(
            NULL, sizeof(WIN32_COMPAT_PROCESS_BLOCK),
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
        : NULL;
    child->environment64 = !compat32
        ? (WIN64_PROCESS_BLOCK *)VirtualAlloc(
            NULL, sizeof(WIN64_PROCESS_BLOCK),
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
        : NULL;
    child->loader_environment64 = child->environment64
        ? &child->environment64->loader : NULL;
    if (child->environment64)
        child->tls_vector = child->environment64->tls_vector;
    child->teb32 = child->compat_environment32
        ? &child->compat_environment32->teb : NULL;
    child->peb32 = child->compat_environment32
        ? &child->compat_environment32->peb : NULL;
    if ((!compat32 && !child->environment64) ||
        (compat32 && (!child->tls_vector || !child->tls_vector32 ||
         !child->compat_environment32 ||
         (uint64_t)(ULONG_PTR)child->tls_vector32 > UINT32_MAX ||
         (uint64_t)(ULONG_PTR)child->compat_environment32 +
             sizeof(WIN32_COMPAT_PROCESS_BLOCK) > UINT32_MAX))) {
        if (tls_phys) mem_free_pages(tls_phys, 4);
        child_free_compat_environment32(child);
        child->tls_vector = NULL;
        return STATUS_NO_MEMORY;
    }
    if (compat32)
        memset(child->tls_vector, 0, 4 * 4096);
    if (child->tls_vector32)
        memset(child->tls_vector32, 0, 4096);
    if (child->compat_environment32)
        memset(child->compat_environment32, 0,
               sizeof(*child->compat_environment32));
    if (child->environment64)
        memset(child->environment64, 0, sizeof(*child->environment64));
    memset(&child->peb, 0, sizeof(child->peb));
    memset(&child->teb, 0, sizeof(child->teb));

    PRTL_USER_PROCESS_PARAMETERS parameters = child->environment64
        ? &child->environment64->process_parameters
        : &child->process_parameters;
    PVOID *process_heaps = child->environment64
        ? child->environment64->process_heaps : child->process_heaps;
    WCHAR *image_path_w = child->environment64
        ? child->environment64->image_path_w : child->image_path_w;
    WCHAR *command_line_w = child->environment64
        ? child->environment64->command_line_w : child->command_line_w;
    WCHAR *current_directory_w = child->environment64
        ? child->environment64->current_directory_w
        : child->current_directory_w;
    WCHAR *dll_path_w = child->environment64
        ? child->environment64->dll_path_w : child->dll_path_w;
    WCHAR *desktop_info_w = child->environment64
        ? child->environment64->desktop_info_w : child->desktop_info_w;
    WCHAR *environment_w = child->environment64
        ? child->environment64->environment_w : child->environment_w;
    PPEB peb = win32_child_peb(child);
    TEB *teb = win32_child_teb(child);

    if (!win32_populate_process_parameters(
            parameters,
            image_path_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            command_line_w, WIN32_CHILD_COMMAND_LINE_CAP,
            current_directory_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            dll_path_w, WIN32_PROCESS_DLL_PATH_CAP,
            desktop_info_w, WIN32_PROCESS_DESKTOP_CAP,
            environment_w, WIN32_PROCESS_ENVIRONMENT_CAP,
            child->process_id, child->image_path, child->command_line,
            child->current_directory, TRUE)) {
        if (tls_phys) mem_free_pages(tls_phys, 4);
        child_free_compat_environment32(child);
        child->tls_vector = NULL;
        return STATUS_NO_MEMORY;
    }
    if (compat32 && !win32_populate_compat_process_block(child, TRUE)) {
        if (tls_phys) mem_free_pages(tls_phys, 4);
        child_free_compat_environment32(child);
        child->tls_vector = NULL;
        return STATUS_NO_MEMORY;
    }
    if (compat32) {
        win32_initialize_peb32(child->peb32,
                               &child->compat_environment32->process_parameters,
                               (uint32_t)(ULONG_PTR)image_base,
                               child->compat_environment32->process_heaps,
                               child->subsystem,
                               child->subsystem_major_version,
                               child->subsystem_minor_version);
        win32_initialize_loader32(child->compat_environment32);
    }
    win32_initialize_peb(peb, parameters, image_base, process_heaps,
                         child->subsystem,
                         child->subsystem_major_version,
                         child->subsystem_minor_version);
    if (!compat32)
        win64_initialize_loader(peb, child->loader_environment64);
    if (!nt_process_set_peb(child->process_object, peb)) {
        if (tls_phys) mem_free_pages(tls_phys, 4);
        child_free_compat_environment32(child);
        child->tls_vector = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    if (!nt_process_set_image_path(child->process_object,
                                   child->image_path)) {
        if (tls_phys) mem_free_pages(tls_phys, 4);
        child_free_compat_environment32(child);
        child->tls_vector = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    teb->Self = teb;
    teb->ProcessEnvironmentBlock = peb;
    teb->ClientId.UniqueProcess =
        (HANDLE)(ULONG_PTR)child->process_id;
    teb->ClientId.UniqueThread =
        (HANDLE)(ULONG_PTR)child->thread_id;
    teb->StackBase = stack_top;
    teb->StackLimit = stack_base;
    teb->DeallocationStack = stack_base;
    teb->ExceptionList = (PVOID)(ULONG_PTR)-1;
    teb->ThreadLocalStoragePointer = child->tls_vector;
    teb->TlsExpansionSlots = child->environment64
        ? child->environment64->tls_expansion
        : child->tls_vector + (2 * 4096 / sizeof(PVOID));

    if (child->environment64) {
        child->peb = *peb;
        child->teb = *teb;
        child->teb.Self = &child->teb;
        child->teb.ProcessEnvironmentBlock = &child->peb;
        child->teb.ThreadLocalStoragePointer = NULL;
        child->teb.TlsExpansionSlots = NULL;
    }

    if (compat32) {
        child->teb32->ExceptionList = UINT32_MAX;
        child->teb32->StackBase = (uint32_t)(ULONG_PTR)stack_top;
        child->teb32->StackLimit = (uint32_t)(ULONG_PTR)stack_base;
        child->teb32->Self = (uint32_t)(ULONG_PTR)child->teb32;
        child->teb32->ClientId_UniqueProcess = child->process_id;
        child->teb32->ClientId_UniqueThread = child->thread_id;
        child->teb32->ThreadLocalStoragePointer =
            (uint32_t)(ULONG_PTR)child->tls_vector32;
        child->teb32->TlsExpansionSlots =
            (uint32_t)(ULONG_PTR)(child->tls_vector32 + 64);
        child->teb32->ProcessEnvironmentBlock =
            (uint32_t)(ULONG_PTR)child->peb32;
    }

    win64_set_current_teb(teb);
    if (compat32)
        compat32_setup_teb(child->teb32);
    else {
        serial_puts("[WINEXEC-CHILD] user TEB64=0x");
        serial_puthex((uint64_t)(ULONG_PTR)teb, 16);
        serial_puts(" PEB64=0x");
        serial_puthex((uint64_t)(ULONG_PTR)peb, 16);
        serial_puts(" params=0x");
        serial_puthex((uint64_t)(ULONG_PTR)parameters, 16);
        serial_puts("\n");
    }
    return STATUS_SUCCESS;
}

static void win64_call_child_entry(PVOID entry, PVOID stack_top,
                                   PPEB peb, BOOL native)
{
#ifndef TEST_HARNESS
    if (native) {
        __asm__ volatile (
            "mov %%rsp, %%r15\n"
            "mov %0, %%rsp\n"
            "and $-16, %%rsp\n"
            "sub $32, %%rsp\n"
            "mov %2, %%rcx\n"
            "call *%1\n"
            "mov %%r15, %%rsp\n"
            : : "r"(stack_top), "r"(entry), "r"(peb)
            : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r15",
              "cc", "memory"
        );
    } else {
        __asm__ volatile (
            "mov %%rsp, %%r15\n"
            "mov %0, %%rsp\n"
            "and $-16, %%rsp\n"
            "sub $32, %%rsp\n"
            "call *%1\n"
            "mov %%r15, %%rsp\n"
            : : "r"(stack_top), "r"(entry)
            : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r15",
              "cc", "memory"
        );
    }
#else
    (void)stack_top;
    if (native)
        ((void (*)(PPEB))entry)(peb);
    else
        ((void (*)(void))entry)();
#endif
}

static void win32_child_run(WIN32_CHILD_CONTEXT *child)
{
    extern int32_t proc_current_pid(void);
    extern int kern_setjmp(uint64_t *buf) __attribute__((returns_twice));

    child->kernel_pid = proc_current_pid();
    child->exit_status = STATUS_UNSUCCESSFUL;
    g_compat32_mode = 0;

    serial_puts("[WINEXEC-CHILD] loading ");
    serial_puts(child->image_path);
    serial_puts(" pid=");
    serial_putdec(child->process_id);
    serial_puts("\n");

    void *file = osfs2_find_ci(child->image_path);
    uint64_t file_size = osfs2_file_size(file);
    uint64_t file_pages = (file_size + 4095) / 4096;
    void *file_phys = file_pages ? mem_alloc_pages(file_pages) : NULL;
    BYTE *file_data = file_phys ? (BYTE *)PHYS_TO_VIRT(file_phys) : NULL;
    PE_IMAGE_INFO info = {0};
    PVOID stack_allocation = NULL;
    PVOID stack_limit = NULL;
    PVOID stack_base = NULL;
    BOOL image_loaded = FALSE;
    BOOL compat32 = FALSE;

    if (!file || !file_size) {
        child->exit_status = STATUS_OBJECT_NAME_NOT_FOUND;
        goto done;
    }
    if (!file_data || osfs2_read(file, 0, file_data, file_size) < 0) {
        child->exit_status = STATUS_NO_MEMORY;
        goto done;
    }
    USHORT image_subsystem = 0;
    USHORT image_subsystem_major_version = 0;
    USHORT image_subsystem_minor_version = 0;
    USHORT image_dll_characteristics = 0;
    int image_bits = child_image_bitness(file_data, file_size,
                                         &image_subsystem,
                                         &image_subsystem_major_version,
                                         &image_subsystem_minor_version,
                                         &image_dll_characteristics);
    if (!image_bits) {
        child->exit_status = STATUS_INVALID_IMAGE_FORMAT;
        serial_puts("[WINEXEC-CHILD] unsupported PE image\n");
        goto done;
    }
    compat32 = image_bits == 32;
    child->subsystem = image_subsystem;
    child->subsystem_major_version = image_subsystem_major_version;
    child->subsystem_minor_version = image_subsystem_minor_version;
    BOOL dep_enabled = !compat32 ||
        (image_dll_characteristics & IMAGE_DLLCHARACTERISTICS_NX_COMPAT);
    nt_vm_configure_process_dep(child->process_id, dep_enabled);
    serial_puts("[WIN32-DEP] pid=");
    serial_putdec(child->process_id);
    serial_puts(compat32 ? " PE32 " : " PE64 ");
    serial_puts(dep_enabled ? "enabled\n" : "legacy OptIn disabled\n");
    if (compat32) {
        extern int sched_alloc_compat_ist1(uint32_t pid);
        if (sched_alloc_compat_ist1((uint32_t)child->kernel_pid) < 0) {
            child->exit_status = STATUS_NO_MEMORY;
            goto done;
        }
        compat32_init();
    }
    g_compat32_mode = compat32 ? 1 : 0;

    child->exit_status = setup_child_environment(child, NULL, NULL, NULL,
                                                  compat32);
    if (!NT_SUCCESS(child->exit_status)) goto done;

    int load_jump = kern_setjmp(child->exit_jmpbuf);
    if (load_jump == 0) {
        child->exit_ready = TRUE;
        child->exit_status = pe_load_named(file_data, file_size, &info,
                                           child->image_path);
    } else if (load_jump != 2) {
        child->exit_status = STATUS_UNSUCCESSFUL;
    }
    child->exit_ready = FALSE;
    if (load_jump != 0 || !NT_SUCCESS(child->exit_status)) goto done;
    image_loaded = TRUE;
    if (info.Is32Bit != compat32) {
        child->exit_status = STATUS_INVALID_IMAGE_FORMAT;
        goto done;
    }

    win32_publish_current_image_base(info.ImageBase);
    PPEB child_peb = win32_child_peb(child);
    child_peb->ImageBaseAddress = info.ImageBase;
    child_peb->ImageSubsystem = info.Subsystem;
    child_peb->ImageSubsystemMajorVersion = info.MajorSubsystemVersion;
    child_peb->ImageSubsystemMinorVersion = info.MinorSubsystemVersion;
    child->peb = *child_peb;
    {
        extern BOOL nt_process_set_peb(PVOID process_object,
                                       const PEB *source);
        if (!nt_process_set_peb(child->process_object, child_peb)) {
            child->exit_status = STATUS_UNSUCCESSFUL;
            goto done;
        }
    }
    if (compat32) {
        child->peb32->ImageBaseAddress =
            (uint32_t)(ULONG_PTR)info.ImageBase;
        child->peb32->ImageSubsystem = info.Subsystem;
        child->peb32->ImageSubsystemMajorVersion =
            info.MajorSubsystemVersion;
        child->peb32->ImageSubsystemMinorVersion =
            info.MinorSubsystemVersion;
        child->exit_status = compat32_patch_iat(&info);
        if (NT_SUCCESS(child->exit_status))
            child->exit_status = pe_finalize_image_protections(&info);
        if (NT_SUCCESS(child->exit_status))
            child->exit_status = compat32_attach_tls(&info);
        compat32_setup_teb(child->teb32);
    } else {
        child->exit_status = pe_finalize_image_protections(&info);
    }
    if (!NT_SUCCESS(child->exit_status)) {
        serial_puts("[WINEXEC-CHILD] final protection setup failed: 0x");
        serial_puthex((uint32_t)child->exit_status, 8);
        serial_puts("\n");
        goto done;
    }
    child->subsystem = info.Subsystem;
    serial_puts("[WINEXEC-CHILD] image=0x");
    serial_puthex((uint64_t)(ULONG_PTR)info.ImageBase, 16);
    serial_puts(" entry=0x");
    serial_puthex((uint64_t)(ULONG_PTR)info.EntryPoint, 16);
    serial_puts("\n");

    uint64_t stack_size = info.StackReserve;
    if (stack_size < 65536) stack_size = 65536;
    if (stack_size > 8ULL * 1024 * 1024) stack_size = 8ULL * 1024 * 1024;
    child->exit_status = nt_vm_allocate_stack(
        stack_size, &stack_allocation, &stack_limit, &stack_base);
    if (!NT_SUCCESS(child->exit_status)) {
        goto done;
    }
    if (compat32 && (uint64_t)(ULONG_PTR)stack_base > UINT32_MAX) {
        child->exit_status = STATUS_NO_MEMORY;
        goto done;
    }
    BYTE *stack_top = (BYTE *)stack_base - 64;
    stack_top = (BYTE *)((ULONG_PTR)stack_top & ~0xFULL);

    win32_publish_current_image_base(info.ImageBase);
    TEB *child_teb = win32_child_teb(child);
    child_teb->StackBase = stack_base;
    child_teb->StackLimit = stack_limit;
    child_teb->DeallocationStack = stack_allocation;
    child->teb.StackBase = stack_base;
    child->teb.StackLimit = stack_limit;
    child->teb.DeallocationStack = stack_allocation;
    if (compat32) {
        child->teb32->StackBase = (uint32_t)(ULONG_PTR)stack_base;
        child->teb32->StackLimit = (uint32_t)(ULONG_PTR)stack_limit;
        compat32_setup_teb(child->teb32);
    }

    int jump_reason = kern_setjmp(child->exit_jmpbuf);
    if (jump_reason == 0) {
        child->exit_ready = TRUE;
        if (compat32) {
            uint32_t sp32 = (uint32_t)(ULONG_PTR)stack_top;
            compat32_enter((uint32_t)(ULONG_PTR)info.EntryPoint, sp32);
        } else {
            child->exit_status = win64_attach_tls(&info);
        }
        if (!compat32 && NT_SUCCESS(child->exit_status)) {
            win64_call_child_entry(info.EntryPoint, stack_top, child_peb,
                                   info.Subsystem == IMAGE_SUBSYSTEM_NATIVE);
            child->exit_status = STATUS_SUCCESS;
        }
    } else if (jump_reason != 2) {
        child->exit_status = STATUS_UNSUCCESSFUL;
    }
    child->exit_ready = FALSE;
    if (compat32) {
        __asm__ volatile (
            "mov $0x30, %%ax\n"
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            "mov %%ax, %%ss\n"
            ::: "ax", "memory"
        );
    }

done:
    /* A PE exception reaches this frame through kern_longjmp(), bypassing
     * iretq and therefore preserving the trap gate's IF=0 state. Teardown
     * can wait on process-owned locks, so resume normal task interrupt state
     * before stopping workers and releasing their resources. */
    {
        uint64_t cleanup_flags;
        __asm__ volatile ("pushfq; popq %0" : "=r"(cleanup_flags));
        if (!(cleanup_flags & (1ULL << 9))) {
            serial_puts("[WINEXEC-CHILD] restoring interrupts for teardown\n");
            __asm__ volatile ("sti" ::: "memory");
        }
    }

    /* Threads created by this child share its image and static TLS. Stop them
     * before releasing either resource; otherwise they resume in freed PE
     * code after the child main thread exits. */
    {
        extern int win32_terminate_process_threads(PPEB owner,
                                                    DWORD exit_code);
        int stopped = win32_terminate_process_threads(
            win32_child_peb(child), (DWORD)child->exit_status);
        if (stopped) {
            serial_puts("[WINEXEC-CHILD] stopped owned threads=");
            serial_putdec((uint64_t)stopped);
            serial_puts("\n");
        }
    }

    kernel32_release_process_waits(child->process_id);

    wsock_release_process(child->process_id);
    advapi32_crypto_release_process(child->process_id);
    advapi32_service_release_process(child->process_id);
    advapi32_registry_flush();

    {
        extern DWORD kernel32_release_process_handles(DWORD process_id);
        DWORD released = kernel32_release_process_handles(child->process_id);
        if (released) {
            serial_puts("[WINEXEC-CHILD] closed owned handles=");
            serial_putdec(released);
            serial_puts("\n");
        }
    }

    child->inherited_handle_count = 0;

    oleacc_release_process(child->process_id);
    ole32_release_process(child->process_id);
    shell32_release_process(child->process_id);
    opengl32_release_process((DWORD)child->kernel_pid);
    user32_release_process(child->process_id);

    /* Static TLS ownership belongs to the child PID, not to whichever thread
     * performs cleanup. Release it while the child's TEB and DLL metadata are
     * both authoritative, then discard the private images. */
    if (compat32 && child->tls_vector32) {
        extern void win32_tls_release_process32(TEB32 *teb);
        win32_tls_release_process32(child->teb32);
    } else if (child->environment64) {
        extern void win64_tls_release_process(TEB *teb);
        win64_tls_release_process(win32_child_teb(child));
    }
    dll_release_process(child->process_id);
    nt_process_complete_child(child->process_object, child->thread_object,
                              child->exit_status);
    serial_puts("[WINEXEC-CHILD] exit pid=");
    serial_putdec(child->process_id);
    serial_puts(" status=0x");
    serial_puthex((uint32_t)child->exit_status, 8);
    serial_puts("\n");
    if (compat32 && child->tls_vector) {
        mem_free_pages((void *)VIRT_TO_PHYS(child->tls_vector), 4);
        child->tls_vector = NULL;
    }
    if (image_loaded)
        pe_unload_for_owner(&info, child->process_id);
    if (file_phys) mem_free_pages(file_phys, file_pages);
    /* Keep the compat TEB mapped until every Win32-facing unload operation
     * has completed. VirtualFree of this block is the final API call allowed
     * to consult the child's LastError slot. */
    if (!compat32 && child->environment64)
        win64_set_current_teb(&child->teb);
    child_free_compat_environment32(child);
    nt_vm_release_process(child->process_id);
    kernel32_release_process_environment(child->process_id);
    __atomic_store_n(&child->used, FALSE, __ATOMIC_RELEASE);
}

static void __attribute__((noreturn)) win32_deliver_child_exit(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    extern int32_t proc_current_pid(void);
    extern void proc_exit(int32_t code);
    extern void kern_longjmp(uint64_t *buf, int val);

    if (!child || child->kernel_pid != proc_current_pid() ||
        !child->exit_ready) {
        serial_puts("[WINEXEC-EXIT] invalid main-thread delivery kpid=");
        serial_putdec((uint64_t)(uint32_t)proc_current_pid());
        serial_puts("\n");
        proc_exit((int32_t)STATUS_UNSUCCESSFUL);
        __builtin_unreachable();
    }

    NTSTATUS status = child->exit_status;
    __atomic_store_n(&child->exit_requested, FALSE, __ATOMIC_RELEASE);
    serial_puts("[WINEXEC-EXIT] delivered to main kpid=");
    serial_putdec((uint64_t)(uint32_t)child->kernel_pid);
    serial_puts(" requester=");
    serial_putdec((uint64_t)(uint32_t)child->exit_requester_kernel_pid);
    serial_puts(" status=0x");
    serial_puthex((uint32_t)status, 8);
    serial_puts("\n");
    kern_longjmp(child->exit_jmpbuf, 2);
    __builtin_unreachable();
}

BOOL win32_terminate_current_child(NTSTATUS status)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (!child || !child->exit_ready) return FALSE;

    extern int32_t proc_current_pid(void);
    extern void kern_longjmp(uint64_t *buf, int val);
    int current_kernel_pid = proc_current_pid();

    if (child->kernel_pid == current_kernel_pid) {
        child->exit_status = status;
        __atomic_store_n(&child->exit_requested, FALSE, __ATOMIC_RELEASE);
        kern_longjmp(child->exit_jmpbuf, 2);
        __builtin_unreachable();
    }

    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) :: "memory");
    BOOL expected = FALSE;
    if (__atomic_compare_exchange_n(&child->exit_requested, &expected, TRUE,
                                    FALSE, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        extern int proc_inject_noreturn_pid(int pid, void (*entry)(void));
        child->exit_status = status;
        child->exit_requester_kernel_pid = current_kernel_pid;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        if (proc_inject_noreturn_pid(child->kernel_pid,
                                    win32_deliver_child_exit) != 0) {
            __atomic_store_n(&child->exit_requested, FALSE,
                             __ATOMIC_RELEASE);
            serial_puts("[WINEXEC-EXIT] failed to schedule main kpid=");
            serial_putdec((uint64_t)(uint32_t)child->kernel_pid);
            serial_puts(" requester=");
            serial_putdec((uint64_t)(uint32_t)current_kernel_pid);
            serial_puts("\n");
            if (irq_flags & (1ULL << 9))
                __asm__ volatile ("sti" ::: "memory");
            return FALSE;
        }
        serial_puts("[WINEXEC-EXIT] requested by worker kpid=");
        serial_putdec((uint64_t)(uint32_t)current_kernel_pid);
        serial_puts(" main=");
        serial_putdec((uint64_t)(uint32_t)child->kernel_pid);
        serial_puts(" status=0x");
        serial_puthex((uint32_t)status, 8);
        serial_puts("\n");
    }
    if (irq_flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");

    /* ExitProcess never returns to the requesting worker. The scheduler will
     * run the injected main-thread trampoline and perform process cleanup on
     * the stack that owns the saved setjmp context. */
    {
        extern void proc_exit(int32_t code);
        proc_exit((int32_t)status);
    }
    __builtin_unreachable();
}

BOOL win32_terminate_child(HANDLE process_handle, NTSTATUS status)
{
    HANDLE_ENTRY *entry = handle_get_entry(&g_handle_table, process_handle);
    if (!entry || entry->type != OBJ_TYPE_PROCESS) return FALSE;

    for (int i = 0; i < MAX_WIN32_CHILDREN; i++) {
        WIN32_CHILD_CONTEXT *child = &g_win32_children[i];
        if (!child->used || child->process_object != entry->object) continue;

        /* Windows makes TerminateProcess asynchronous: stop execution in the
         * target, run process teardown, then signal its process object.  Hand
         * control to the target's main-thread exit context so win32_child_run
         * can stop every worker before releasing DLLs, VMAs and the PEB. */
        if (child->kernel_pid > 0 && child->exit_ready) {
            extern int32_t proc_current_pid(void);
            extern int proc_inject_noreturn_pid(int pid, void (*entry)(void));
            uint64_t irq_flags;
            BOOL requested = FALSE;

            __asm__ volatile ("pushfq; popq %0; cli"
                              : "=r"(irq_flags) :: "memory");
            if (__atomic_load_n(&child->exit_requested, __ATOMIC_ACQUIRE)) {
                requested = TRUE;
            } else {
                child->exit_status = status;
                child->exit_requester_kernel_pid = proc_current_pid();
                __atomic_store_n(&child->exit_requested, TRUE,
                                 __ATOMIC_RELEASE);
                if (proc_inject_noreturn_pid(child->kernel_pid,
                                             win32_deliver_child_exit) == 0) {
                    requested = TRUE;
                } else {
                    __atomic_store_n(&child->exit_requested, FALSE,
                                     __ATOMIC_RELEASE);
                }
            }
            if (irq_flags & (1ULL << 9))
                __asm__ volatile ("sti" ::: "memory");

            if (requested) {
                serial_puts("[WINEXEC-TERM] queued pid=");
                serial_putdec(child->process_id);
                serial_puts(" kpid=");
                serial_putdec((uint64_t)(uint32_t)child->kernel_pid);
                serial_puts(" status=0x");
                serial_puthex((uint32_t)status, 8);
                serial_puts("\n");
                return TRUE;
            }
        }

        /* A target that has not published an exit context cannot run normal
         * teardown. Quiesce all workers before making the owning task a
         * zombie; this keeps the reaper from freeing a live address space. */
        {
            extern int win32_terminate_process_threads(PPEB owner,
                                                        DWORD exit_code);
            int stopped = win32_terminate_process_threads(
                win32_child_peb(child), (DWORD)status);
            serial_puts("[WINEXEC-TERM] forced pid=");
            serial_putdec(child->process_id);
            serial_puts(" stopped_threads=");
            serial_putdec((uint64_t)(uint32_t)stopped);
            serial_puts("\n");
        }

        kernel32_release_process_waits(child->process_id);

        extern int proc_kill_pid(int pid);
        child->exit_ready = FALSE;
        child->exit_status = status;
        nt_process_complete_child(child->process_object, child->thread_object,
                                  status);
        if (child->kernel_pid > 0) proc_kill_pid(child->kernel_pid);
        if (child->kernel_pid > 0)
            opengl32_release_process((DWORD)child->kernel_pid);
        ddraw_release_process(child->process_id);
        wsock_release_process(child->process_id);
        ole32_release_process(child->process_id);
        advapi32_crypto_release_process(child->process_id);
        advapi32_registry_flush();
        kernel32_release_process_environment(child->process_id);
        __atomic_store_n(&child->used, FALSE, __ATOMIC_RELEASE);
        return TRUE;
    }
    return FALSE;
}

NTSTATUS win32_spawn_child(const char *image_path, const char *command_line,
                           const char *current_directory,
                           PCVOID environment,
                           PHANDLE process_handle, PHANDLE thread_handle,
                           DWORD *process_id, DWORD *thread_id,
                           const HANDLE *inherited_handles,
                           DWORD inherited_handle_count,
                           DWORD creation_flags)
{
    extern int sched_spawn_in_address_space_blocked(
        const char *, void (*)(void), uint64_t, int);
    extern int proc_wake_pid(int pid);
    extern uint64_t paging_create_process_cr3(void);
    extern void paging_free_process_cr3(uint64_t);
    extern NTSTATUS nt_process_create_child(PHANDLE, PHANDLE, ULONG *, ULONG *,
                                             PVOID *, PVOID *, BOOL);

    BOOL create_suspended = (creation_flags & 0x4) != 0;

    if (!image_path || !*image_path || !process_handle || !thread_handle)
        return STATUS_INVALID_PARAMETER;
    if (inherited_handle_count > WIN32_CHILD_INHERITED_HANDLE_CAP ||
        (inherited_handle_count && !inherited_handles))
        return STATUS_INVALID_PARAMETER;

    WIN32_CHILD_CONTEXT *child = NULL;
    int slot = -1;
    for (int i = 0; i < MAX_WIN32_CHILDREN; i++) {
        BOOL expected = FALSE;
        if (__atomic_compare_exchange_n(&g_win32_children[i].used, &expected,
                                        TRUE, FALSE, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            child = &g_win32_children[i];
            slot = i;
            memset((BYTE *)child + sizeof(child->used), 0,
                   sizeof(*child) - sizeof(child->used));
            child->kernel_pid = -1;
            child->parent_process_id = win32_current_process_id();
            break;
        }
    }
    if (!child) {
        serial_puts("[WINEXEC-CHILD] child context table full\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NTSTATUS status = nt_process_create_child(
        &child->process_handle, &child->thread_handle,
        &child->process_id, &child->thread_id,
        &child->process_object, &child->thread_object, create_suspended);
    if (!NT_SUCCESS(status)) goto fail;

    for (DWORD i = 0; i < inherited_handle_count; i++) {
        status = handle_retain_for_process(&g_handle_table,
                                           inherited_handles[i],
                                           child->process_id);
        if (!NT_SUCCESS(status)) {
            while (child->inherited_handle_count)
                handle_release_for_process(
                    &g_handle_table,
                    child->inherited_handles[--child->inherited_handle_count],
                    child->process_id);
            nt_process_complete_child(child->process_object,
                                      child->thread_object, status);
            handle_close(&g_handle_table, child->thread_handle);
            handle_close(&g_handle_table, child->process_handle);
            goto fail;
        }
        child->inherited_handles[child->inherited_handle_count++] =
            inherited_handles[i];
    }

    status = kernel32_inherit_process_console(
        child->parent_process_id, child->process_id, creation_flags);
    if (!NT_SUCCESS(status))
        goto fail_child;

    if (environment) {
        status = kernel32_set_process_environment_block(
            child->process_id, environment,
            (creation_flags & 0x00000400U) != 0); /* CREATE_UNICODE_ENVIRONMENT */
    } else {
        status = kernel32_inherit_process_environment(
            GetCurrentProcessId(), child->process_id);
    }
    if (!NT_SUCCESS(status))
        goto fail_child;

    child_copy_string(child->image_path, sizeof(child->image_path), image_path);
    const char *name = image_path;
    if (name[0] == 'S' && name[1] == 'y' && name[2] == 's' &&
        name[3] == 't' && name[4] == 'e' && name[5] == 'm' && name[6] == '\\')
        name += 7;
    child_copy_string(child->exe_name, sizeof(child->exe_name), name);
    child_copy_string(child->command_line, sizeof(child->command_line),
                      command_line ? command_line : image_path);
    for (SIZE_T i = 0; i < WIN32_CHILD_COMMAND_LINE_CAP; i++) {
        child->command_line_w[i] = (WCHAR)(BYTE)child->command_line[i];
        if (!child->command_line[i]) break;
    }
    child_copy_string(child->current_directory,
                      sizeof(child->current_directory),
                      current_directory ? current_directory
                                        : kernel32_current_directory_relative());
    PVOID spawned_object = child->process_object;
    uint64_t child_cr3 = paging_create_process_cr3();
    int kernel_pid = child_cr3
        ? sched_spawn_in_address_space_blocked("win32_process",
                                               win32_child_entries[slot],
                                               child_cr3, TRUE)
        : -1;
    if (kernel_pid < 0) {
        if (child_cr3)
            paging_free_process_cr3(child_cr3);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail_child;
    }
    if (__atomic_load_n(&child->used, __ATOMIC_ACQUIRE) &&
        child->process_object == spawned_object &&
        child->kernel_pid < 0)
        __atomic_store_n(&child->kernel_pid, kernel_pid, __ATOMIC_RELEASE);

    if (!create_suspended && proc_wake_pid(kernel_pid) < 0) {
        extern int proc_kill_pid(int pid);
        proc_kill_pid(kernel_pid);
        status = STATUS_UNSUCCESSFUL;
        goto fail_child;
    }

    if (create_suspended) {
        serial_puts("[WINEXEC-CHILD] created suspended pid=");
        serial_putdec(child->process_id);
        serial_puts(" kpid=");
        serial_putdec((uint64_t)(uint32_t)kernel_pid);
        serial_puts("\n");
    }
    *process_handle = child->process_handle;
    *thread_handle = child->thread_handle;
    if (process_id) *process_id = child->process_id;
    if (thread_id) *thread_id = child->thread_id;
    return STATUS_SUCCESS;

fail_child:
    while (child->inherited_handle_count)
        handle_release_for_process(
            &g_handle_table,
            child->inherited_handles[--child->inherited_handle_count],
            child->process_id);
    nt_process_complete_child(child->process_object, child->thread_object,
                              status);
    handle_close(&g_handle_table, child->thread_handle);
    handle_close(&g_handle_table, child->process_handle);
    kernel32_release_process_environment(child->process_id);
fail:
    __atomic_store_n(&child->used, FALSE, __ATOMIC_RELEASE);
    return status;
}

/* DLLs are loaded from the executable import graph and by LoadLibrary.
 * Scanning the whole filesystem here would run unrelated DllMain routines
 * before the process entry point, unlike the Windows loader. */

static void win32_cleanup_main_process(PE_IMAGE_INFO *info,
                                       PVOID stack_allocation,
                                       BOOL compat32,
                                       NTSTATUS exit_status)
{
    const DWORD process_id = 1;
    PPEB process_peb = win64_main_peb();
    TEB *process_teb = win64_main_teb();

    /* A compat32 non-local exit restores RIP/RSP but not the segment state or
     * APIC timer mask changed by compat32_enter(). Teardown can block while
     * workers stop, so make the root task schedulable before touching them. */
    if (compat32) {
        __asm__ volatile (
            "mov $0x30, %%ax\n"
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            "mov %%ax, %%ss\n"
            ::: "ax", "memory"
        );
        extern volatile uint32_t *idt_get_apic_base(void);
        volatile uint32_t *apic = idt_get_apic_base();
        if (apic)
            apic[0x320 / 4] &= ~0x10000U;
    }

    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    if (!(flags & (1ULL << 9)))
        __asm__ volatile ("sti" ::: "memory");

    uint64_t kernel_cr3 = paging_get_kernel_cr3();
    if (kernel_cr3)
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");

    /* Multimedia callbacks can re-enter guest code. Ask their dispatcher to
     * stop gracefully before force-retiring any remaining process threads. */
    winmm_release_process(process_id);

    extern int win32_terminate_process_threads(PPEB owner, DWORD exit_code);
    int stopped = win32_terminate_process_threads(
        process_peb, (DWORD)exit_status);
    if (stopped) {
        serial_puts("[WINEXEC] stopped owned threads=");
        serial_putdec((uint64_t)stopped);
        serial_puts("\n");
    }

    kernel32_release_process_waits(process_id);

    wsock_release_process(process_id);
    advapi32_crypto_release_process(process_id);
    advapi32_service_release_process(process_id);
    advapi32_registry_flush();

    extern DWORD kernel32_release_process_handles(DWORD process_id);
    DWORD released_handles = kernel32_release_process_handles(process_id);
    if (released_handles) {
        serial_puts("[WINEXEC] closed owned handles=");
        serial_putdec(released_handles);
        serial_puts("\n");
    }

    oleacc_release_process(process_id);
    shell32_release_process(process_id);
    opengl32_release_process((DWORD)proc_current_tgid());
    user32_release_process(process_id);

    /* Stop native backends and discard cached guest pointers before static
     * TLS and module images cease to be valid. nt_vm_release_process() repeats
     * these calls defensively, so each release routine remains idempotent. */
    ddraw_release_process(process_id);
    dinput8_release_process(process_id);
    dsound_release_process(process_id);
    ole32_release_process(process_id);
    msvcrt_release_process(process_id);

    if (compat32 && g_main_compat_environment32) {
        extern void win32_tls_release_process32(TEB32 *teb);
        win32_tls_release_process32(&g_main_compat_environment32->teb);
    } else {
        extern void win64_tls_release_process(TEB *teb);
        win64_tls_release_process(process_teb);
    }
    dll_release_process(process_id);

    if (stack_allocation) {
        NTSTATUS stack_status = nt_vm_free_stack(stack_allocation);
        if (!NT_SUCCESS(stack_status)) {
            serial_puts("[WINEXEC] stack release failed status=0x");
            serial_puthex((uint32_t)stack_status, 8);
            serial_puts("\n");
        }
    }
    if (info && info->ImageBase)
        pe_unload_for_owner(info, process_id);

    if (g_main_compat_environment32) {
        compat32_setup_teb(NULL);
        g_main_compat_environment32 = NULL;
    }
    if (!compat32 && g_main_environment64) {
        win64_reset_bootstrap_teb();
        g_main_environment64 = NULL;
    }
    g_compat32_mode = 0;

    nt_vm_release_process(process_id);
    kernel32_release_process_environment(process_id);

    g_exe_image_base = 0;
    g_exe_subsystem = 0;
    g_exe_subsystem_major_version = 0;
    g_exe_subsystem_minor_version = 0;
    g_peb.ImageBaseAddress = NULL;
    g_teb.StackBase = NULL;
    g_teb.StackLimit = NULL;
    g_teb.DeallocationStack = NULL;
}

/* ── Main execution entry ───────────────────────────────────── */

/*
 * Load and execute a Windows PE executable.
 *
 * file_data: raw PE file bytes (read from OsitoFS or network)
 * file_size: size of file_data in bytes
 *
 * Returns: exit code from the PE, or -1 on load failure
 */
int winexec_run(const uint8_t *file_data, uint64_t file_size)
{
    serial_puts("\n=== OsitoK Windows Compatibility Layer ===\n");

    USHORT image_subsystem = 0;
    USHORT image_subsystem_major_version = 0;
    USHORT image_subsystem_minor_version = 0;
    USHORT image_dll_characteristics = 0;
    int image_bits = child_image_bitness(file_data, file_size,
                                         &image_subsystem,
                                         &image_subsystem_major_version,
                                         &image_subsystem_minor_version,
                                         &image_dll_characteristics);
    if (image_bits != 32 && image_bits != 64) {
        serial_puts("[WINEXEC] unsupported PE image\n");
        return -1;
    }
    BOOL compat32 = image_bits == 32;
    g_exe_subsystem = image_subsystem;
    g_exe_subsystem_major_version = image_subsystem_major_version;
    g_exe_subsystem_minor_version = image_subsystem_minor_version;

    /* A completed main PE may leave private VirtualAlloc/section mappings in
     * the shell's kernel CR3. Release them before assigning owner PID 1 to the
     * next run; resetting tracker metadata would leak both PTEs and pages. */
    if (g_main_compat_environment32) {
        compat32_setup_teb(NULL);
        g_main_compat_environment32 = NULL;
    }
    if (g_main_environment64) {
        win64_reset_bootstrap_teb();
        g_main_environment64 = NULL;
    }
    wsock_release_process(1);
    ole32_release_process(1);
    advapi32_crypto_release_process(1);
    nt_vm_release_process(1);
    kernel32_release_process_environment(1);
    opengl32_release_process((DWORD)proc_current_tgid());

    BOOL dep_enabled = !compat32 ||
        (image_dll_characteristics & IMAGE_DLLCHARACTERISTICS_NX_COMPAT);
    nt_vm_configure_process_dep(1, dep_enabled);
    serial_puts("[WIN32-DEP] pid=1");
    serial_puts(compat32 ? " PE32 " : " PE64 ");
    serial_puts(dep_enabled ? "enabled\n" : "legacy OptIn disabled\n");

    /* Import resolution and dependency DllMain calls happen inside pe_load().
     * Select the process ABI before initializing shims or entering the loader. */
    g_compat32_mode = compat32 ? 1 : 0;

    /* Initialize subsystems */
    NT_SERVICE_TABLE ssdt;
    nt_syscall_init(&ssdt);
    ntdll_shim_init();
    kernel32_shim_init();
    if (!NT_SUCCESS(kernel32_initialize_process_console(
            1, image_subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI))) {
        serial_puts("[WINEXEC] failed to initialize console state\n");
        g_compat32_mode = 0;
        g_exe_subsystem = 0;
        g_exe_subsystem_major_version = 0;
        g_exe_subsystem_minor_version = 0;
        return -1;
    }
    msvcrt_shim_init();
    shell32_shim_init();  /* release tray-owned icon copies before user32 */
    user32_shim_init();   /* re-exec: window/input/activation state reset */
    oleacc_shim_init();
    ddraw_shim_init();    /* re-exec: surfaces/COM proxies/display-mode reset */
    dinput8_shim_init();  /* re-exec: DirectInput COM32 thunks are process-local */
    dsound_shim_init();   /* publish the DirectSound COM class provider */
    winmm_shim_init();    /* process-local multimedia timer callback bridge */
    opengl32_shim_init(); /* re-exec: WGL query contexts reset */
    libusb_shim_init();   /* native xHCI owns USB; SDL sees an empty bus */

    /* Initialize DLL loader and register built-in shims */
    dll_loader_init();
    dll_register_shim("ntdll.dll",    ntdll_resolve);
    dll_register_shim("kernel32.dll", kernel32_resolve);
    dll_register_shim("psapi.dll",    kernel32_resolve);
    dll_register_shim("setupapi.dll", kernel32_resolve);
    dll_register_shim("cfgmgr32.dll", kernel32_resolve);
    dll_register_shim("hid.dll",      kernel32_resolve);
    dll_register_shim("mf.dll",       kernel32_resolve);
    dll_register_shim("mfplat.dll",   kernel32_resolve);
    dll_register_shim("mfreadwrite.dll", kernel32_resolve);
    dll_register_shim("d3d9.dll",     kernel32_resolve);
    dll_register_shim("dxva2.dll",    kernel32_resolve);
    dll_register_shim("dbghelp.dll",  kernel32_resolve);
    dll_register_shim("api-ms-win-power-base-l1-1-0.dll", kernel32_resolve);
    dll_register_shim("api-ms-win-power-setting-l1-1-0.dll", kernel32_resolve);
    dll_register_shim("msvcrt.dll",   msvcrt_resolve);
    dll_register_shim("ucrtbase.dll", msvcrt_resolve);
    dll_register_shim("vcruntime140.dll", msvcrt_resolve);
    dll_register_shim("vcruntime140_1.dll", msvcrt_resolve);
    dll_register_shim("advapi32.dll", advapi32_resolve);
    dll_register_shim("wevtapi.dll",  advapi32_resolve);
    dll_register_shim("bcrypt.dll",   advapi32_resolve);
    dll_register_shim("bcryptprimitives.dll", advapi32_resolve);
    dll_register_shim("user32.dll",   user32_resolve);
    dll_register_shim("imm32.dll",    user32_resolve);
    dll_register_shim("api-ms-win-shcore-scaling-l1-1-1.dll", user32_resolve);
    dll_register_shim("wtsapi32.dll", user32_resolve);
    dll_register_shim("dwmapi.dll",   user32_resolve);
    dll_register_shim("gdi32.dll",    gdi32_resolve);
    dll_register_shim("msimg32.dll",  gdi32_resolve);
    dll_register_shim("dwrite.dll",   gdi32_resolve);
    dll_register_shim("opengl32.dll", opengl32_resolve);
    dll_register_shim("dxgi.dll",     dxgi_resolve);
    dll_register_shim("version.dll",  version_resolve);
    dll_register_shim("ddraw.dll",    ddraw_resolve);
    dll_register_shim("dsound.dll",   dsound_resolve);
    dll_register_shim("dinput8.dll",  dinput8_resolve);
    dll_register_shim("wsock32.dll",  wsock32_resolve);
    dll_register_shim("ws2_32.dll",   ws2_32_resolve);
    dll_register_shim("mswsock.dll",  ws2_32_resolve);
    dll_register_shim("iphlpapi.dll", ws2_32_resolve);
    dll_register_shim("wininet.dll",  ws2_32_resolve);
    dll_register_shim("winhttp.dll",  ws2_32_resolve);
    dll_register_shim("shell32.dll",  shell32_resolve);
    dll_register_shim("shlwapi.dll",  shlwapi_resolve);
    dll_register_shim("winmm.dll",   winmm_resolve);
    dll_register_shim("ole32.dll",   ole32_resolve);
    dll_register_shim("oleaut32.dll", oleaut32_resolve);
    dll_register_shim("oleacc.dll", oleacc_resolve);
    dll_register_shim("crypt32.dll", crypt32_resolve);
    dll_register_shim("ncrypt.dll", ncrypt_resolve);
    dll_register_shim("wintrust.dll", wintrust_resolve);
    dll_register_shim("comctl32.dll", comctl32_resolve);
    dll_register_shim("comdlg32.dll", comdlg32_resolve);
    dll_register_shim_ex("riched32.dll", richedit_resolve,
                         richedit_module_event);
    dll_register_shim_ex("riched20.dll", richedit_resolve,
                         richedit_module_event);
    dll_register_shim_ex("msftedit.dll", richedit_resolve,
                         richedit_module_event);
    dll_register_shim("libusb-1.0.dll", libusb_resolve);

    /* Register each shim's co-located ABI table (argc + callconv derived
     * from the prototype) so every IAT thunk uses its export contract. */
    {
        win32_abi_reset();
        extern const WIN32_EXPORT *ntdll_abi_table(int *);
        extern const WIN32_EXPORT *kernel32_abi_table(int *);
        extern const WIN32_EXPORT *msvcrt_abi_table(int *);
        extern const WIN32_EXPORT *advapi32_abi_table(int *);
        extern const WIN32_EXPORT *user32_abi_table(int *);
        extern const WIN32_EXPORT *gdi32_abi_table(int *);
        extern const WIN32_EXPORT *opengl32_abi_table(int *);
        extern const WIN32_EXPORT *dxgi_abi_table(int *);
        extern const WIN32_EXPORT *version_abi_table(int *);
        extern const WIN32_EXPORT *ddraw_abi_table(int *);
        extern const WIN32_EXPORT *dsound_abi_table(int *);
        extern const WIN32_EXPORT *dinput8_abi_table(int *);
        extern const WIN32_EXPORT *wsock32_abi_table(int *);
        extern const WIN32_EXPORT *shell32_abi_table(int *);
        extern const WIN32_EXPORT *shlwapi_abi_table(int *);
        extern const WIN32_EXPORT *winmm_abi_table(int *);
        extern const WIN32_EXPORT *ole32_abi_table(int *);
        extern const WIN32_EXPORT *oleaut32_abi_table(int *);
        extern const WIN32_EXPORT *oleacc_abi_table(int *);
        extern const WIN32_EXPORT *crypt32_abi_table(int *);
        extern const WIN32_EXPORT *ncrypt_abi_table(int *);
        extern const WIN32_EXPORT *wintrust_abi_table(int *);
        extern const WIN32_EXPORT *comctl32_abi_table(int *);
        extern const WIN32_EXPORT *comdlg32_abi_table(int *);
        extern const WIN32_EXPORT *libusb_abi_table(int *);
        int n;
        win32_abi_register("ntdll.dll",    ntdll_abi_table(&n),    n);
        win32_abi_register("kernel32.dll", kernel32_abi_table(&n), n);
        win32_abi_register("psapi.dll",    kernel32_abi_table(&n), n);
        win32_abi_register("setupapi.dll", kernel32_abi_table(&n), n);
        win32_abi_register("cfgmgr32.dll", kernel32_abi_table(&n), n);
        win32_abi_register("hid.dll",      kernel32_abi_table(&n), n);
        win32_abi_register("mf.dll",       kernel32_abi_table(&n), n);
        win32_abi_register("mfplat.dll",   kernel32_abi_table(&n), n);
        win32_abi_register("mfreadwrite.dll", kernel32_abi_table(&n), n);
        win32_abi_register("d3d9.dll",     kernel32_abi_table(&n), n);
        win32_abi_register("dxva2.dll",    kernel32_abi_table(&n), n);
        win32_abi_register("dbghelp.dll",  kernel32_abi_table(&n), n);
        win32_abi_register("api-ms-win-power-base-l1-1-0.dll",
                           kernel32_abi_table(&n), n);
        win32_abi_register("api-ms-win-power-setting-l1-1-0.dll",
                           kernel32_abi_table(&n), n);
        win32_abi_register("msvcrt.dll",   msvcrt_abi_table(&n),   n);
        win32_abi_register("ucrtbase.dll", msvcrt_abi_table(&n),   n);
        win32_abi_register("vcruntime140.dll",
                           msvcrt_abi_table(&n), n);
        win32_abi_register("vcruntime140_1.dll",
                           msvcrt_abi_table(&n), n);
        win32_abi_register("advapi32.dll", advapi32_abi_table(&n), n);
        win32_abi_register("wevtapi.dll",  advapi32_abi_table(&n), n);
        win32_abi_register("bcrypt.dll",   advapi32_abi_table(&n), n);
        win32_abi_register("bcryptprimitives.dll",
                           advapi32_abi_table(&n), n);
        win32_abi_register("user32.dll",   user32_abi_table(&n),   n);
        win32_abi_register("imm32.dll",    user32_abi_table(&n),   n);
        win32_abi_register("api-ms-win-shcore-scaling-l1-1-1.dll",
                           user32_abi_table(&n), n);
        win32_abi_register("wtsapi32.dll", user32_abi_table(&n), n);
        win32_abi_register("dwmapi.dll",   user32_abi_table(&n), n);
        win32_abi_register("gdi32.dll",    gdi32_abi_table(&n),    n);
        win32_abi_register("msimg32.dll",  gdi32_abi_table(&n),    n);
        win32_abi_register("dwrite.dll",   gdi32_abi_table(&n),    n);
        win32_abi_register("opengl32.dll", opengl32_abi_table(&n), n);
        win32_abi_register("dxgi.dll",     dxgi_abi_table(&n),     n);
        win32_abi_register("version.dll",  version_abi_table(&n),  n);
        win32_abi_register("ddraw.dll",    ddraw_abi_table(&n),    n);
        win32_abi_register("dsound.dll",   dsound_abi_table(&n),   n);
        win32_abi_register("dinput8.dll",  dinput8_abi_table(&n),  n);
        win32_abi_register("wsock32.dll",  wsock32_abi_table(&n),  n);
        win32_abi_register("ws2_32.dll",   wsock32_abi_table(&n),  n);
        win32_abi_register("mswsock.dll",  wsock32_abi_table(&n),  n);
        win32_abi_register("iphlpapi.dll", wsock32_abi_table(&n),  n);
        win32_abi_register("wininet.dll",  wsock32_abi_table(&n),  n);
        win32_abi_register("winhttp.dll",  wsock32_abi_table(&n),  n);
        win32_abi_register("shell32.dll",  shell32_abi_table(&n),  n);
        win32_abi_register("shlwapi.dll",  shlwapi_abi_table(&n),  n);
        win32_abi_register("winmm.dll",    winmm_abi_table(&n),    n);
        win32_abi_register("ole32.dll",    ole32_abi_table(&n),    n);
        win32_abi_register("oleaut32.dll", oleaut32_abi_table(&n), n);
        win32_abi_register("oleacc.dll", oleacc_abi_table(&n), n);
        win32_abi_register("crypt32.dll", crypt32_abi_table(&n), n);
        win32_abi_register("ncrypt.dll", ncrypt_abi_table(&n), n);
        win32_abi_register("wintrust.dll", wintrust_abi_table(&n), n);
        win32_abi_register("comctl32.dll", comctl32_abi_table(&n), n);
        win32_abi_register("comdlg32.dll", comdlg32_abi_table(&n), n);
        win32_abi_register("libusb-1.0.dll", libusb_abi_table(&n), n);
    }

    /*
     * Initialize compat32 thunk pool BEFORE pe_load(), because pe_load()
     * recursively loads DLLs (via dll_resolve_import → dll_load) and those
     * DLLs need the thunk pool to exist for IAT patching.
     */
    /* pe_load() can execute PE32 DllMain callbacks before the EXE metadata is
     * published. Give the direct winexec task its private syscall/fault stacks
     * now; CreateProcess and CreateThread perform the same setup in their own
     * launch paths. */
    if (compat32) {
        extern int32_t proc_current_pid(void);
        extern int sched_alloc_compat_ist1(uint32_t pid);
        int32_t pid = proc_current_pid();
        if (pid <= 0 || sched_alloc_compat_ist1((uint32_t)pid) < 0) {
            serial_puts("[WINEXEC] failed to allocate PE32 private IST stacks\n");
            return -1;
        }
        serial_puts("[WINEXEC] PE32 private IST stacks ready pid=");
        serial_putdec((uint32_t)pid);
        serial_puts("\n");
    }

    if (compat32)
        compat32_init();

    /* Native DLL entry points execute during pe_load(). Establish GS, the
     * live PEB and normalized process parameters before the loader can call
     * any of them. win32_tls_reset() below then installs the fresh TLS vectors
     * into this TEB without erasing the ABI fields. */
    if (!setup_environment(NULL, !compat32)) {
        serial_puts("[WINEXEC] failed to initialize process environment\n");
        g_compat32_mode = 0;
        return -1;
    }

    /* DLL entry points execute inside pe_load(). Install the process's real
     * low TEB/PEB before any PE32 CRT can touch FS:[0] or FS:[0x30]. */
    if (compat32) {
        if (!setup_main_compat_environment32()) {
            serial_puts("[WINEXEC] failed to allocate low PE32 environment\n");
            g_compat32_mode = 0;
            return -1;
        }
    } else {
        win32_tls_reset();
    }

    serial_puts("[WINEXEC] loading PE...\n");

    /* Load PE */
    PE_IMAGE_INFO info;
    NTSTATUS status = pe_load_named(file_data, file_size, &info,
                                    win32_current_image_path());

    if (!NT_SUCCESS(status)) {
        serial_puts("[WINEXEC] PE load failed: ");
        serial_puthex((uint64_t)status, 8);
        serial_puts("\n");
        win32_cleanup_main_process(&info, NULL, compat32, status);
        return -1;
    }

    win32_publish_current_image_base(info.ImageBase);
    g_exe_subsystem = info.Subsystem;
    g_exe_subsystem_major_version = info.MajorSubsystemVersion;
    g_exe_subsystem_minor_version = info.MinorSubsystemVersion;
    PPEB main_peb = win64_main_peb();
    main_peb->ImageBaseAddress = info.ImageBase;
    main_peb->ImageSubsystem = info.Subsystem;
    main_peb->ImageSubsystemMajorVersion = info.MajorSubsystemVersion;
    main_peb->ImageSubsystemMinorVersion = info.MinorSubsystemVersion;
    g_peb = *main_peb;
    {
        extern BOOL nt_process_set_peb(PVOID process_object,
                                       const PEB *source);
        if (!nt_process_set_peb(NULL, main_peb)) {
            serial_puts("[WINEXEC] failed to publish loaded PEB\n");
            win32_cleanup_main_process(&info, NULL, compat32,
                                       STATUS_UNSUCCESSFUL);
            return -1;
        }
    }
    serial_puts("[WINEXEC] PE loaded successfully\n");
    serial_puts("[WINEXEC] subsystem: ");
    serial_puthex(info.Subsystem, 4);
    serial_puts(info.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI
                ? " (console)\n" : " (other)\n");

    if (info.Is32Bit)
    {
        win32_main_peb32()->ImageBaseAddress =
            (uint32_t)(ULONG_PTR)info.ImageBase;
        win32_main_peb32()->ImageSubsystem = info.Subsystem;
        win32_main_peb32()->ImageSubsystemMajorVersion =
            info.MajorSubsystemVersion;
        win32_main_peb32()->ImageSubsystemMinorVersion =
            info.MinorSubsystemVersion;
    }
    serial_puts("[WINEXEC] PEB/TEB initialized, GS base set\n");

    /* For PE32 (i386): patch EXE IAT and set up FS:TEB */
    if (info.Is32Bit) {
        g_compat32_mode = 1;
        serial_puts("[WINEXEC] PE32 (i386) detected — patching EXE IAT\n");

        /* Patch IAT: replace truncated 64-bit ptrs with 32-bit thunk addrs */
        NTSTATUS compat_st = compat32_patch_iat(&info);
        if (!NT_SUCCESS(compat_st)) {
            serial_puts("[WINEXEC] compat32 IAT patch failed\n");
            win32_cleanup_main_process(&info, NULL, compat32, compat_st);
            return -1;
        }

        compat_st = pe_finalize_image_protections(&info);
        if (!NT_SUCCESS(compat_st)) {
            serial_puts("[WINEXEC] final protection setup failed: 0x");
            serial_puthex((uint32_t)compat_st, 8);
            serial_puts("\n");
            win32_cleanup_main_process(&info, NULL, compat32, compat_st);
            return -1;
        }

        compat_st = compat32_attach_tls(&info);
        if (!NT_SUCCESS(compat_st)) {
            serial_puts("[WINEXEC] compat32 TLS setup failed: 0x");
            serial_puthex((uint32_t)compat_st, 8);
            serial_puts("\n");
            win32_cleanup_main_process(&info, NULL, compat32, compat_st);
            return -1;
        }

        /* Set FS base for 32-bit TEB access (Windows i386 uses FS:0) */
        compat32_setup_teb(win32_main_teb32());

        serial_puts("[WINEXEC] TEB32.ExceptionList after setup = 0x");
        serial_puthex(win32_main_teb32()->ExceptionList, 8);
        serial_puts("\n");
    } else {
        NTSTATUS tls_st = pe_finalize_image_protections(&info);
        if (!NT_SUCCESS(tls_st)) {
            serial_puts("[WINEXEC] final protection setup failed: 0x");
            serial_puthex((uint32_t)tls_st, 8);
            serial_puts("\n");
            win32_cleanup_main_process(&info, NULL, compat32, tls_st);
            return -1;
        }

        tls_st = win64_attach_tls(&info);
        if (!NT_SUCCESS(tls_st)) {
            serial_puts("[WINEXEC] PE64 TLS setup failed: 0x");
            serial_puthex((uint32_t)tls_st, 8);
            serial_puts("\n");
            win32_cleanup_main_process(&info, NULL, compat32, tls_st);
            return -1;
        }
    }


    /* Allocate user stack — MUST be zeroed.
     * On Windows, stack pages come from VirtualAlloc (MEM_COMMIT) which
     * always zeroes pages. PE32 code relies on this: local variables of
     * class types (FString, TArray) have constructors that expect zeroed
     * memory for their fields (Data=NULL, ArrayNum=0, ArrayMax=0).
     * Without zeroing, destructors read garbage → REP MOVSD 4GB hang. */
    uint64_t stack_size = info.StackReserve;
    if (stack_size < 65536) stack_size = 65536;  /* minimum 64KB */
    if (stack_size > 8ULL * 1024 * 1024) stack_size = 8ULL * 1024 * 1024;  /* cap 8MB */
    PVOID stack_allocation = NULL;
    PVOID stack_limit = NULL;
    PVOID stack_base = NULL;
    status = nt_vm_allocate_stack(stack_size, &stack_allocation,
                                  &stack_limit, &stack_base);
    if (!NT_SUCCESS(status)) {
        serial_puts("[WINEXEC] failed to allocate stack\n");
        win32_cleanup_main_process(&info, NULL, compat32, status);
        return -1;
    }

    serial_puts("[WINEXEC] Stack: ");
    serial_putdec(stack_size / 1024);
    serial_puts("KB at 0x");
    extern void serial_puthex(uint64_t val, int digits);
    serial_puthex((uint64_t)(ULONG_PTR)stack_allocation,
                  info.Is32Bit ? 8 : 16);
    serial_puts(" (private VMA, bottom page reserved)\n");

    /* Usable stack starts after the guard page */
    uint8_t *stack_usable = (uint8_t *)stack_limit;
    /* Stack grows down — entry RSP/ESP points near top */
    uint8_t *stack_top = (uint8_t *)stack_base - 64;
    /* Align to 16-byte boundary */
    stack_top = (uint8_t *)((uint64_t)stack_top & ~0xFULL);

    if (info.Is32Bit) {
        TEB32 *teb32 = win32_main_teb32();
        teb32->StackBase = (uint32_t)(ULONG_PTR)stack_base;
        teb32->StackLimit = (uint32_t)(ULONG_PTR)stack_usable;
    } else {
        TEB *teb = win64_main_teb();
        teb->StackBase = stack_base;
        teb->StackLimit = stack_usable;
        teb->DeallocationStack = stack_allocation;
        g_teb.StackBase = stack_base;
        g_teb.StackLimit = stack_usable;
        g_teb.DeallocationStack = stack_allocation;
    }

    if (info.Is32Bit) {
        serial_puts("[WINEXEC] TEB32.ExceptionList before EXE entry = 0x");
        serial_puthex(win32_main_teb32()->ExceptionList, 8);
        serial_puts("\n");

        /* Hardware watchpoint on TEB32 removed — was causing #GP
         * when #DB fires from compat mode during DLL init */
    }

    serial_puts("[WINEXEC] jumping to entry point at ");
    serial_puthex((uint64_t)info.EntryPoint, 16);
    if (info.Is32Bit) serial_puts(" (32-bit compat mode)");
    serial_puts("\n");

    /*
     * Windows CUI entry point signature:
     *   For EXE: mainCRTStartup(void) — CRT calls main()
     *   For native: NtProcessStartup(PPEB)
     *   For DLL: DllMain(HINSTANCE, DWORD, LPVOID)
     *
     * We call it as a void(void) function. The CRT startup
     * will call GetCommandLineA/W and eventually main().
     * Without a CRT, native PE apps receive PEB in RCX.
     */

    if (info.Is32Bit) {
        /* PE32 (i386): enter 32-bit compatibility mode */
        uint32_t entry32 = (uint32_t)(ULONG_PTR)info.EntryPoint;
        uint32_t sp32    = (uint32_t)(ULONG_PTR)stack_top;

        /* Install a process-local exit context. compat32_enter never returns;
         * NtTerminateProcess reaches this context after restoring long mode. */
        {
            extern int kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
            extern int32_t proc_current_pid(void);
            g_win32_main.owner_kernel_pid = proc_current_pid();
            g_win32_main.exit_requester_kernel_pid =
                g_win32_main.owner_kernel_pid;
            g_win32_main.exit_status = STATUS_SUCCESS;
            __atomic_store_n(&g_win32_main.exit_requested, FALSE,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&g_win32_main.active, FALSE,
                             __ATOMIC_RELEASE);
            if (kern_setjmp(g_win32_main.exit_jmpbuf) != 0) {
                NTSTATUS exit_status = g_win32_main.exit_status;
                __atomic_store_n(&g_win32_main.active, FALSE,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&g_win32_main.exit_requested, FALSE,
                                 __ATOMIC_RELEASE);
                serial_puts("[WINEXEC] PE process exited, code=");
                serial_putdec((uint32_t)exit_status);
                serial_puts("\n");
                win32_cleanup_main_process(&info, stack_allocation,
                                           compat32, exit_status);
                return (int32_t)exit_status;
            }
            __atomic_store_n(&g_win32_main.active, TRUE,
                             __ATOMIC_RELEASE);
        }

        compat32_enter(entry32, sp32);
        /* never reached — control returns via proc_exit → longjmp above */
    } else {
        /* PE32+ (x86-64): direct 64-bit execution */
        extern int kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
        extern int32_t proc_current_pid(void);
        g_win32_main.owner_kernel_pid = proc_current_pid();
        g_win32_main.exit_requester_kernel_pid =
            g_win32_main.owner_kernel_pid;
        g_win32_main.exit_status = STATUS_SUCCESS;
        __atomic_store_n(&g_win32_main.exit_requested, FALSE,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_win32_main.active, FALSE, __ATOMIC_RELEASE);
        if (kern_setjmp(g_win32_main.exit_jmpbuf) != 0) {
            NTSTATUS exit_status = g_win32_main.exit_status;
            __atomic_store_n(&g_win32_main.active, FALSE,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&g_win32_main.exit_requested, FALSE,
                             __ATOMIC_RELEASE);
            serial_puts("[WINEXEC] PE64 process exited, code=");
            serial_putdec((uint32_t)exit_status);
            serial_puts("\n");

            win32_cleanup_main_process(&info, stack_allocation,
                                       compat32, exit_status);
            return (int32_t)exit_status;
        }
        __atomic_store_n(&g_win32_main.active, TRUE, __ATOMIC_RELEASE);

        win64_call_child_entry(info.EntryPoint, stack_top, win64_main_peb(),
                               info.Subsystem == IMAGE_SUBSYSTEM_NATIVE);
    }

    /* If entry point returns (unusual — most call ExitProcess) */
    __atomic_store_n(&g_win32_main.active, FALSE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_win32_main.exit_requested, FALSE, __ATOMIC_RELEASE);
    serial_puts("[WINEXEC] PE entry returned\n");

    win32_cleanup_main_process(&info, stack_allocation,
                               compat32, STATUS_SUCCESS);
    return 0;
}
