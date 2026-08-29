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
#include "wsock32_shim.h"
#include "shell32_shim.h"
#include "winmm_shim.h"
#include "ole32_shim.h"
#include "oleacc_shim.h"
#include "crypt32_shim.h"
#include "comctl32_shim.h"
#include "comdlg32_shim.h"
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
extern int   mem_reserve_range(uint64_t phys, uint64_t count);
extern void  proc_exit(int32_t code);
extern int32_t proc_current_pid(void);
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

        uint32_t stale_pid = expected - 1U;
        void *stale_process = stale_pid && stale_pid <= 0xFFFFU
            ? proc_find_ptr((uint16_t)stale_pid)
            : NULL;
        if (expected != owner_key &&
            (!stale_process ||
             proc_state_of(stale_process) == PE_VA_PROC_ZOMBIE)) {
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

static int pe_va_conflict_snapshot(uint64_t base, uint64_t size)
{
    if (!size || base + size < base) return 1;
    if (compat32_runtime_range_conflicts(base, size)) return 1;
    uint64_t end = base + size;
    DWORD owner_pid = pe_va_owner();
    int count = __atomic_load_n(&pe_va_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++) {
        uint64_t rsize = __atomic_load_n(&pe_va_ranges[i].size,
                                         __ATOMIC_ACQUIRE);
        if (!rsize || pe_va_ranges[i].owner_pid != owner_pid) continue;
        uint64_t rend = pe_va_ranges[i].base + rsize;
        if (rend < pe_va_ranges[i].base) continue;
        if (base < rend && end > pe_va_ranges[i].base)
            return 1;
    }
    return 0;
}

BOOL pe_va_range_conflicts(ULONGLONG base, ULONGLONG size)
{
    return pe_va_conflict_snapshot(base, size) ? TRUE : FALSE;
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
        if (!pe_va_conflict_snapshot(base, size))
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
        if (!pe_va_conflict_snapshot(base, size))
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
    if (preferred && pe_va_conflict_snapshot((uint64_t)preferred, size)) {
        serial_puts("[pe_alloc] CONFLICT: VA 0x");
        serial_puthex((uint64_t)preferred, 8);
        serial_puts(" already occupied, relocating\n");
        preferred = NULL;  /* force relocation */
    }

    if (is_32bit) {
        uint64_t va = 0;

        /* PE32 currently shares the kernel's lower-half identity map. Claim
         * the physical pages whose addresses equal the requested VA before
         * changing their PTEs. Otherwise a preferred ImageBase can overwrite
         * a live kernel allocation at the same numeric address. */
        if (preferred && (((uint64_t)preferred & 0xFFFULL) == 0) &&
            mem_reserve_range((uint64_t)preferred, pages) == 0) {
            va = (uint64_t)preferred;
        } else {
            uint64_t cursor = PE32_RELOC_TOP;
            while ((va = pe32_reloc_address(size, cursor)) != 0) {
                if (mem_reserve_range(va, pages) == 0)
                    break;
                cursor = va;
            }
        }

        if (!va) {
            serial_puts("[pe_alloc] ERROR: PE32 relocation VA exhausted\n");
            return NULL;
        }

        if (pe_map_pages(cr3, va, va, pages) == 0) {
            pe_va_record(va, size, cr3, va, pages);
            serial_puts(va == (uint64_t)preferred
                ? "[pe_alloc] reserved PE32 preferred VA/PA 0x"
                : "[pe_alloc] relocated PE32 VA/PA 0x");
            serial_puthex(va, 8);
            serial_puts(" pages=0x");
            serial_puthex(pages, 4);
            serial_puts("\n");
            return (PVOID)(ULONG_PTR)va;
        }

        /* Keep the claimed pages quarantined if page-table installation
         * failed; returning them while their identity mappings are missing
         * would let a later kernel allocation alias an invalid VA. */
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

    if (preferred && pe_va_conflict_snapshot((uint64_t)preferred, size)) {
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

typedef struct {
    TEB32 teb;
    BYTE teb_padding[0x100 - sizeof(TEB32)];
    PEB32 peb;
    BYTE peb_padding[0x120 - 0x100 - sizeof(PEB32)];
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
} WIN32_COMPAT_PROCESS_BLOCK;

_Static_assert(__builtin_offsetof(WIN32_COMPAT_PROCESS_BLOCK, peb) == 0x100,
               "Win32 PEB must remain at TEB page offset 0x100");

static RTL_USER_PROCESS_PARAMETERS g_process_parameters;
static WCHAR g_image_path_w[WIN32_PROCESS_IMAGE_PATH_CAP];
static WCHAR g_command_line_w[WIN32_CHILD_COMMAND_LINE_CAP];
static WCHAR g_current_directory_w[WIN32_PROCESS_IMAGE_PATH_CAP];
static WCHAR g_dll_path_w[WIN32_PROCESS_DLL_PATH_CAP];
static WCHAR g_desktop_info_w[WIN32_PROCESS_DESKTOP_CAP];
static WCHAR g_environment_w[WIN32_PROCESS_ENVIRONMENT_CAP];
static PVOID g_process_heaps[1];

typedef struct {
    BOOL used;
    BOOL exit_ready;
    int kernel_pid;
    ULONG process_id;
    ULONG parent_process_id;
    ULONG thread_id;
    USHORT subsystem;
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
    PVOID *tls_vector;
    WIN32_COMPAT_PROCESS_BLOCK *compat_environment32;
    PEB32 *peb32;
    TEB32 *teb32;
    uint32_t *tls_vector32;
    PVOID saved_spew_output;
    BOOL saved_spew_output_valid;
    HANDLE inherited_handles[WIN32_CHILD_INHERITED_HANDLE_CAP];
    DWORD inherited_handle_count;
    uint64_t exit_jmpbuf[9];
} WIN32_CHILD_CONTEXT;

static WIN32_CHILD_CONTEXT g_win32_children[MAX_WIN32_CHILDREN];

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
    DWORD thread_owner_pid = win32_current_thread_process_id();
    TEB *teb = (TEB *)(ULONG_PTR)proc_get_gs_base();
    for (int i = 0; i < MAX_WIN32_CHILDREN; i++) {
        if (!g_win32_children[i].used)
            continue;
        if ((thread_owner_pid &&
             g_win32_children[i].process_id == thread_owner_pid) ||
            (!thread_owner_pid &&
             (g_win32_children[i].kernel_pid == pid ||
              (teb && teb->ProcessEnvironmentBlock ==
                          &g_win32_children[i].peb))))
            return &g_win32_children[i];
    }
#endif
    return NULL;
}

static BOOL win32_ascii_prefix_ci(const char *text, const char *prefix)
{
    while (*prefix) {
        char a = *text++;
        char b = *prefix++;
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
        if (a != b) return FALSE;
    }
    return TRUE;
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
    static const char prefix[] = "C:\\System\\";
    SIZE_T length = 0;
    if (!relative) relative = "";
    if (relative[0] && relative[1] == ':') relative += 2;
    while (*relative == '\\' || *relative == '/') relative++;
    if (win32_ascii_prefix_ci(relative, "System\\")) relative += 7;
    return win32_wide_append_ascii(destination, capacity, &length, prefix,
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

static BOOL win32_populate_compat_process_block(WIN32_CHILD_CONTEXT *child,
                                                 BOOL reset)
{
    WIN32_COMPAT_PROCESS_BLOCK *block = child->compat_environment32;
    if (!block) return FALSE;

    if (!win32_copy_ascii(child->image_path, block->image_path,
                          sizeof(block->image_path)) ||
        !win32_copy_ascii(child->exe_name, block->exe_name,
                          sizeof(block->exe_name)) ||
        !win32_copy_ascii(child->command_line, block->command_line,
                          sizeof(block->command_line)) ||
        !win32_copy_ascii(child->current_directory, block->current_directory,
                          sizeof(block->current_directory)) ||
        !win32_build_image_path_w(child->image_path, block->image_path_w,
                                  WIN32_PROCESS_IMAGE_PATH_CAP) ||
        !win32_copy_ascii_w(child->command_line, block->command_line_w,
                            WIN32_CHILD_COMMAND_LINE_CAP) ||
        !win32_build_current_directory_w(
            child->current_directory, block->current_directory_w,
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
        child->process_id, block->environment_w,
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
    parameters->ProcessGroupId = child->process_id;
    parameters->LoaderThreads = 1;
    block->process_heaps[0] = 0xBEEF0001U;
    block->peb.ProcessParameters = (uint32_t)(ULONG_PTR)parameters;
    block->peb.ProcessHeap = block->process_heaps[0];
    return TRUE;
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
                                 PVOID image_base, PVOID process_heaps[1])
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
}

BOOL win32_refresh_current_process_parameters(void)
{
    extern BOOL nt_process_set_peb(PVOID process_object, const PEB *source);
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) {
        if (!child->peb.ProcessParameters) return TRUE;
        if (!win32_populate_process_parameters(
                &child->process_parameters,
                child->image_path_w, WIN32_PROCESS_IMAGE_PATH_CAP,
                child->command_line_w, WIN32_CHILD_COMMAND_LINE_CAP,
                child->current_directory_w, WIN32_PROCESS_IMAGE_PATH_CAP,
                child->dll_path_w, WIN32_PROCESS_DLL_PATH_CAP,
                child->desktop_info_w, WIN32_PROCESS_DESKTOP_CAP,
                child->environment_w, WIN32_PROCESS_ENVIRONMENT_CAP,
                child->process_id, child->image_path, child->command_line,
                child->current_directory, FALSE))
            return FALSE;
        if (child->compat_environment32 &&
            !win32_populate_compat_process_block(child, FALSE))
            return FALSE;
        return nt_process_set_peb(child->process_object, &child->peb);
    }

    if (!g_peb.ProcessParameters) return TRUE;
    extern char win32_image_path[260];
    extern char win32_command_line[4096];
    if (!win32_populate_process_parameters(
            &g_process_parameters,
            g_image_path_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            g_command_line_w, WIN32_CHILD_COMMAND_LINE_CAP,
            g_current_directory_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            g_dll_path_w, WIN32_PROCESS_DLL_PATH_CAP,
            g_desktop_info_w, WIN32_PROCESS_DESKTOP_CAP,
            g_environment_w, WIN32_PROCESS_ENVIRONMENT_CAP,
            1, win32_image_path, win32_command_line,
            kernel32_current_directory_relative(), FALSE))
        return FALSE;
    return nt_process_set_peb(NULL, &g_peb);
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

const char *win32_current_exe_name(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) return child->compat_environment32
        ? child->compat_environment32->exe_name : child->exe_name;
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
    extern char win32_image_path[260];
    return win32_image_path;
}

const char *win32_current_command_line(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) return child->compat_environment32
        ? child->compat_environment32->command_line : child->command_line;
    extern char win32_command_line[4096];
    return win32_command_line;
}

const WCHAR *win32_current_command_line_w(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) return child->compat_environment32
        ? child->compat_environment32->command_line_w
        : child->command_line_w;
    return g_command_line_w[0] ? g_command_line_w : NULL;
}

const char *win32_current_directory_override(void)
{
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (!child) return NULL;
    const char *directory = child->compat_environment32
        ? child->compat_environment32->current_directory
        : child->current_directory;
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
    if (child) return (ULONG_PTR)child->peb.ImageBaseAddress;
    return g_exe_image_base;
}

void win32_publish_current_image_base(PVOID image_base)
{
    extern BOOL nt_process_set_image_base(PVOID process_object,
                                           PVOID image_base);
    WIN32_CHILD_CONTEXT *child = win32_current_child();
    if (child) {
        child->peb.ImageBaseAddress = image_base;
        (void)nt_process_set_image_base(child->process_object, image_base);
        return;
    }

    g_exe_image_base = (ULONG_PTR)image_base;
    g_peb.ImageBaseAddress = image_base;
    (void)nt_process_set_image_base(NULL, image_base);
}

DWORD win32_current_process_id(void)
{
#ifndef TEST_HARNESS
    extern DWORD win32_current_thread_process_id(void);
    DWORD thread_owner_pid = win32_current_thread_process_id();
    if (thread_owner_pid)
        return thread_owner_pid;
#endif
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
    extern void x86_tss_reset_ist1(void);
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
    x86_tss_reset_ist1();
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
static PEB32  g_peb32;
TEB32  g_teb32;  /* non-static: accessed by ntdll_shim.c for SEH/LastError */

static BOOL setup_environment(PVOID image_base, int is32bit)
{
    extern BOOL nt_process_set_peb(PVOID process_object, const PEB *source);
    extern char win32_image_path[260];
    extern char win32_command_line[4096];

    memset(&g_teb, 0, sizeof(g_teb));
    if (!win32_populate_process_parameters(
            &g_process_parameters,
            g_image_path_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            g_command_line_w, WIN32_CHILD_COMMAND_LINE_CAP,
            g_current_directory_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            g_dll_path_w, WIN32_PROCESS_DLL_PATH_CAP,
            g_desktop_info_w, WIN32_PROCESS_DESKTOP_CAP,
            g_environment_w, WIN32_PROCESS_ENVIRONMENT_CAP,
            1, win32_image_path, win32_command_line,
            kernel32_current_directory_relative(), TRUE))
        return FALSE;
    win32_initialize_peb(&g_peb, &g_process_parameters, image_base,
                         g_process_heaps);
    if (!nt_process_set_peb(NULL, &g_peb)) return FALSE;
    if (!nt_process_set_image_path(NULL, win32_image_path)) return FALSE;

    g_teb.Self                       = &g_teb;
    g_teb.ProcessEnvironmentBlock    = &g_peb;
    g_teb.ClientId.UniqueProcess     = (HANDLE)(ULONG_PTR)1;
    g_teb.ClientId.UniqueThread      = (HANDLE)(ULONG_PTR)1;
    g_teb.LastErrorValue             = 0;
    g_teb.ExceptionList              = (PVOID)(ULONG_PTR)-1; /* empty SEH chain */

    if (is32bit) {
        /*
         * TEB32/PEB32 already pre-initialized before pe_load().
         * Just update the image base address (now known) and log.
         */
        g_peb32.ImageBaseAddress = (uint32_t)(ULONG_PTR)image_base;

        serial_puts("[WINEXEC] TEB32 at 0x");
        serial_puthex((uint64_t)(ULONG_PTR)&g_teb32, 16);
        serial_puts(" PEB32 at 0x");
        serial_puthex((uint64_t)(ULONG_PTR)&g_peb32, 16);
        serial_puts("\n");
    }

    /*
     * Windows stores TEB pointer in GS:0x30 (x86-64).
     * Set GS base to point to our TEB structure.
     * On OsitoK, we use MSR_GS_BASE (0xC0000101).
     */
#ifndef TEST_HARNESS
    uint64_t teb_addr = (uint64_t)&g_teb;
    __asm__ volatile (
        "mov $0xC0000101, %%ecx\n"   /* MSR_GS_BASE */
        "mov %0, %%rax\n"
        "mov %0, %%rdx\n"
        "shr $32, %%rdx\n"
        "wrmsr\n"
        :
        : "r"(teb_addr)
        : "rax", "rcx", "rdx"
    );
    {
        extern void proc_set_gs_base(uint64_t addr);
        proc_set_gs_base(teb_addr);
    }
#else
    /* In test harness mode on Linux, use arch_prctl to set GS base.
     * This is needed for PE code that accesses TEB via gs:0 (e.g., SEH). */
    {
        #include <asm/prctl.h>
        extern int arch_prctl(int code, unsigned long addr);
        arch_prctl(ARCH_SET_GS, (unsigned long)&g_teb);
    }
#endif
    return TRUE;
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

/* ── Pre-load all DLLs from filesystem ──────────────────────── */
/*
 * Load all .dll files from OsitoFS before the EXE entry point runs.
 * This ensures all native UE1 classes (IMPLEMENT_CLASS) are registered
 * during _initterm, so ProcessRegistrants() finds them all.
 * Without this, dynamic LoadLibrary fails due to FName corruption
 * (VirtualAlloc identity-map recycling bug).
 */
extern void     *osfs2_find(const char *name);
extern void     *osfs2_find_ci(const char *name);
extern int       osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t  osfs2_file_size(void *file);
extern uint32_t  osfs2_file_count(void);
extern void     *osfs2_file_by_index(uint32_t idx);
extern const char *osfs2_file_name(void *file);

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

static BOOL child_string_contains(const char *text, const char *needle)
{
    if (!text || !needle || !*needle) return FALSE;
    for (; *text; text++) {
        SIZE_T i = 0;
        while (needle[i] && text[i] == needle[i]) i++;
        if (!needle[i]) return TRUE;
    }
    return FALSE;
}

static void child_preload_swiftshader(WIN32_CHILD_CONTEXT *child)
{
    static const char dll_name[] = "vk_swiftshader.dll";
    char path[320];
    SIZE_T prefix = 0;

    if (!child_string_contains(child->command_line, "--type=gpu-process") ||
        !child_string_contains(child->command_line,
                               "--use-angle=swiftshader"))
        return;

    for (SIZE_T i = 0; child->image_path[i]; i++)
        if (child->image_path[i] == '\\' || child->image_path[i] == '/')
            prefix = i + 1;
    if (!prefix || 3 + prefix + sizeof(dll_name) > sizeof(path)) {
        serial_puts("[WINEXEC-CHILD] SwiftShader preload path too long\n");
        return;
    }

    path[0] = 'C';
    path[1] = ':';
    path[2] = '\\';
    for (SIZE_T i = 0; i < prefix; i++) path[3 + i] = child->image_path[i];
    for (SIZE_T i = 0; i < sizeof(dll_name); i++)
        path[3 + prefix + i] = dll_name[i];

    HANDLE module = LoadLibraryA(path);
    serial_puts("[WINEXEC-CHILD] SwiftShader preload ");
    if (module) {
        serial_puts("ok base=0x");
        serial_puthex((uint64_t)(ULONG_PTR)module, 16);
    } else {
        serial_puts("failed error=");
        serial_putdec(GetLastError());
    }
    serial_puts("\n");
}

static void child_save_shared_dll_state(WIN32_CHILD_CONTEXT *child)
{
    LOADED_MODULE *tier0 = dll_find_module("tier0_s64.dll");
    if (!tier0) return;

    typedef PVOID (WINAPI *get_spew_fn)(void);
    get_spew_fn get_spew = (get_spew_fn)dll_resolve_export(
        tier0, "GetSpewOutputFunc", 0, FALSE);
    if (!get_spew) return;

    child->saved_spew_output = get_spew();
    child->saved_spew_output_valid = TRUE;
}

static void child_restore_shared_dll_state(WIN32_CHILD_CONTEXT *child)
{
    if (!child->saved_spew_output_valid) return;

    LOADED_MODULE *tier0 = dll_find_module("tier0_s64.dll");
    typedef void (WINAPI *set_spew_fn)(PVOID);
    set_spew_fn set_spew = tier0 ? (set_spew_fn)dll_resolve_export(
        tier0, "SpewOutputFunc", 0, FALSE) : NULL;
    if (set_spew) {
        set_spew(child->saved_spew_output);
        serial_puts("[WINEXEC-CHILD] restored tier0 spew callback\n");
    }
    child->saved_spew_output_valid = FALSE;
}

static int child_image_bitness(const BYTE *data, uint64_t size)
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
    USHORT magic = *(USHORT *)(nt + sizeof(ULONG) + sizeof(IMAGE_FILE_HEADER));
    if (file->Machine == IMAGE_FILE_MACHINE_I386 &&
        magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return 32;
    if (file->Machine == IMAGE_FILE_MACHINE_AMD64 &&
        magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return 64;
    return 0;
}

static void child_free_compat_environment32(WIN32_CHILD_CONTEXT *child)
{
    if (!child) return;
    if (child->tls_vector32)
        VirtualFree(child->tls_vector32, 0, MEM_RELEASE);
    if (child->compat_environment32)
        VirtualFree(child->compat_environment32, 0, MEM_RELEASE);
    child->tls_vector32 = NULL;
    child->compat_environment32 = NULL;
    child->peb32 = NULL;
    child->teb32 = NULL;
}

static NTSTATUS setup_child_environment(WIN32_CHILD_CONTEXT *child,
                                        PVOID image_base,
                                        PVOID stack_base,
                                        PVOID stack_top,
                                        BOOL compat32)
{
    extern BOOL nt_process_set_peb(PVOID process_object, const PEB *source);
    void *tls_phys = mem_alloc_pages(4);
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
    child->teb32 = compat32
        ? &child->compat_environment32->teb : NULL;
    child->peb32 = compat32
        ? &child->compat_environment32->peb : NULL;
    if (!child->tls_vector ||
        (compat32 && (!child->tls_vector32 || !child->compat_environment32 ||
         (uint64_t)(ULONG_PTR)child->tls_vector32 > UINT32_MAX ||
         (uint64_t)(ULONG_PTR)child->compat_environment32 +
             sizeof(WIN32_COMPAT_PROCESS_BLOCK) > UINT32_MAX))) {
        if (tls_phys) mem_free_pages(tls_phys, 4);
        child_free_compat_environment32(child);
        child->tls_vector = NULL;
        return STATUS_NO_MEMORY;
    }
    memset(child->tls_vector, 0, 4 * 4096);
    if (child->tls_vector32)
        memset(child->tls_vector32, 0, 4096);
    if (child->compat_environment32)
        memset(child->compat_environment32, 0,
               sizeof(*child->compat_environment32));
    memset(&child->peb, 0, sizeof(child->peb));
    memset(&child->teb, 0, sizeof(child->teb));

    if (!win32_populate_process_parameters(
            &child->process_parameters,
            child->image_path_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            child->command_line_w, WIN32_CHILD_COMMAND_LINE_CAP,
            child->current_directory_w, WIN32_PROCESS_IMAGE_PATH_CAP,
            child->dll_path_w, WIN32_PROCESS_DLL_PATH_CAP,
            child->desktop_info_w, WIN32_PROCESS_DESKTOP_CAP,
            child->environment_w, WIN32_PROCESS_ENVIRONMENT_CAP,
            child->process_id, child->image_path, child->command_line,
            child->current_directory, TRUE)) {
        mem_free_pages(tls_phys, 4);
        child_free_compat_environment32(child);
        child->tls_vector = NULL;
        return STATUS_NO_MEMORY;
    }
    if (compat32 && !win32_populate_compat_process_block(child, TRUE)) {
        mem_free_pages(tls_phys, 4);
        child_free_compat_environment32(child);
        child->tls_vector = NULL;
        return STATUS_NO_MEMORY;
    }
    win32_initialize_peb(&child->peb, &child->process_parameters, image_base,
                         child->process_heaps);
    if (!nt_process_set_peb(child->process_object, &child->peb)) {
        mem_free_pages(tls_phys, 4);
        child_free_compat_environment32(child);
        child->tls_vector = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    if (!nt_process_set_image_path(child->process_object,
                                   child->image_path)) {
        mem_free_pages(tls_phys, 4);
        child_free_compat_environment32(child);
        child->tls_vector = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    child->teb.Self = &child->teb;
    child->teb.ProcessEnvironmentBlock = &child->peb;
    child->teb.ClientId.UniqueProcess =
        (HANDLE)(ULONG_PTR)child->process_id;
    child->teb.ClientId.UniqueThread =
        (HANDLE)(ULONG_PTR)child->thread_id;
    child->teb.StackBase = stack_top;
    child->teb.StackLimit = stack_base;
    child->teb.DeallocationStack = stack_base;
    child->teb.ExceptionList = (PVOID)(ULONG_PTR)-1;
    child->teb.ThreadLocalStoragePointer = child->tls_vector;
    child->teb.TlsExpansionSlots =
        child->tls_vector + (2 * 4096 / sizeof(PVOID));

    if (compat32) {
        child->peb32->BeingDebugged = 0;
        child->peb32->ImageBaseAddress = (uint32_t)(ULONG_PTR)image_base;
        child->teb32->ExceptionList = UINT32_MAX;
        child->teb32->StackBase = (uint32_t)(ULONG_PTR)stack_top;
        child->teb32->StackLimit = (uint32_t)(ULONG_PTR)stack_base;
        child->teb32->Self = (uint32_t)(ULONG_PTR)child->teb32;
        child->teb32->ClientId_UniqueProcess = child->process_id;
        child->teb32->ClientId_UniqueThread = child->thread_id;
        child->teb32->ThreadLocalStoragePointer =
            (uint32_t)(ULONG_PTR)child->tls_vector32;
        child->teb32->ProcessEnvironmentBlock =
            (uint32_t)(ULONG_PTR)child->peb32;
    }

#ifndef TEST_HARNESS
    uint64_t teb_addr = (uint64_t)(ULONG_PTR)&child->teb;
    __asm__ volatile (
        "mov $0xC0000101, %%ecx\n"
        "mov %0, %%rax\n"
        "mov %0, %%rdx\n"
        "shr $32, %%rdx\n"
        "wrmsr\n"
        : : "r"(teb_addr) : "rax", "rcx", "rdx", "memory"
    );
    {
        extern void proc_set_gs_base(uint64_t addr);
        proc_set_gs_base(teb_addr);
    }
#endif
    if (compat32)
        compat32_setup_teb(child->teb32);
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
    PE_IMAGE_INFO info;
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
    int image_bits = child_image_bitness(file_data, file_size);
    if (!image_bits) {
        child->exit_status = STATUS_INVALID_IMAGE_FORMAT;
        serial_puts("[WINEXEC-CHILD] unsupported PE image\n");
        goto done;
    }
    compat32 = image_bits == 32;
    if (compat32) {
        extern int sched_alloc_compat_ist1(uint32_t pid);
        if (sched_alloc_compat_ist1((uint32_t)child->kernel_pid) < 0) {
            child->exit_status = STATUS_NO_MEMORY;
            goto done;
        }
        compat32_init();
    }
    g_compat32_mode = compat32 ? 1 : 0;

    child_save_shared_dll_state(child);

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
    if (compat32) {
        child->peb32->ImageBaseAddress =
            (uint32_t)(ULONG_PTR)info.ImageBase;
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

    if (!compat32)
        child_preload_swiftshader(child);

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
            uint32_t *sp = (uint32_t *)(ULONG_PTR)sp32;
            extern uint32_t crt_get_base_seh_thunk(void);
            uint32_t handler = crt_get_base_seh_thunk();
            if (handler) {
                sp -= 3;
                sp[0] = child->teb32->ExceptionList;
                sp[1] = handler;
                sp[2] = 0;
                child->teb32->ExceptionList = (uint32_t)(ULONG_PTR)sp;
                sp32 = (uint32_t)(ULONG_PTR)sp;
            }
            compat32_enter((uint32_t)(ULONG_PTR)info.EntryPoint, sp32);
        } else {
            child->exit_status = win64_attach_tls(&info);
        }
        if (!compat32 && NT_SUCCESS(child->exit_status)) {
            win64_call_child_entry(info.EntryPoint, stack_top, &child->peb,
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
            &child->peb, (DWORD)child->exit_status);
        if (stopped) {
            serial_puts("[WINEXEC-CHILD] stopped owned threads=");
            serial_putdec((uint64_t)stopped);
            serial_puts("\n");
        }
    }

    wsock_release_process(child->process_id);
    advapi32_crypto_release_process(child->process_id);
    advapi32_service_release_process(child->process_id);

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
    shell32_release_process(child->process_id);
    user32_release_process(child->process_id);

    /* Restore any legacy shared callback, then discard this process's
     * private DLL images before its PEB/TLS vector disappear. */
    child_restore_shared_dll_state(child);
    dll_release_process(child->process_id);
    nt_process_complete_child(child->process_object, child->thread_object,
                              child->exit_status);
    serial_puts("[WINEXEC-CHILD] exit pid=");
    serial_putdec(child->process_id);
    serial_puts(" status=0x");
    serial_puthex((uint32_t)child->exit_status, 8);
    serial_puts("\n");
    if (compat32 && child->tls_vector32) {
        extern void win32_tls_release_process32(TEB32 *teb);
        win32_tls_release_process32(child->teb32);
    } else if (child->tls_vector) {
        extern void win64_tls_release_process(TEB *teb);
        win64_tls_release_process(&child->teb);
    }
    child_free_compat_environment32(child);
    if (child->tls_vector) {
        mem_free_pages((void *)VIRT_TO_PHYS(child->tls_vector), 4);
        child->tls_vector = NULL;
    }
    if (image_loaded) pe_unload(&info);
    if (file_phys) mem_free_pages(file_phys, file_pages);
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
                &child->peb, (DWORD)status);
            serial_puts("[WINEXEC-TERM] forced pid=");
            serial_putdec(child->process_id);
            serial_puts(" stopped_threads=");
            serial_putdec((uint64_t)(uint32_t)stopped);
            serial_puts("\n");
        }

        extern int proc_kill_pid(int pid);
        child->exit_ready = FALSE;
        child->exit_status = status;
        nt_process_complete_child(child->process_object, child->thread_object,
                                  status);
        if (child->kernel_pid > 0) proc_kill_pid(child->kernel_pid);
        wsock_release_process(child->process_id);
        advapi32_crypto_release_process(child->process_id);
        kernel32_release_process_environment(child->process_id);
        __atomic_store_n(&child->used, FALSE, __ATOMIC_RELEASE);
        return TRUE;
    }
    return FALSE;
}

NTSTATUS win32_spawn_child(const char *image_path, const char *command_line,
                           const char *current_directory,
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

    kernel32_inherit_process_environment(GetCurrentProcessId(),
                                         child->process_id);

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

static int str_ends_with_dll(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    if (len < 4) return 0;
    char c0 = s[len-4], c1 = s[len-3], c2 = s[len-2], c3 = s[len-1];
    if (c0 >= 'A' && c0 <= 'Z') c0 += 32;
    if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
    if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
    if (c3 >= 'A' && c3 <= 'Z') c3 += 32;
    return c0 == '.' && c1 == 'd' && c2 == 'l' && c3 == 'l';
}

/* Case-insensitive equality for short DLL names. */
static int dll_name_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Render-device plugin DLLs must NOT be preloaded: their _initterm static
 * initializers do engine-level work (object loading, large allocations) that
 * needs FName/GObj to be live — but the EXE's appInit (which runs
 * FName::StaticInit + ProcessRegistrants) hasn't executed during preload.
 * Preloading OpenGlDrv crashed mid-_initterm at a "Loading objects..." site,
 * so preload never completed and the EXE entry was never reached. In real
 * Windows these load via LoadLibrary AFTER appInit, when the engine selects a
 * renderer — our dll_load runs their _initterm then, with the engine up. */
static int is_deferred_render_dll(const char *name)
{
    static const char *deferred[] = {
        "OpenGlDrv.dll", "D3DDrv.dll", "GlideDrv.dll",
        "MeTaLDrv.dll", "SoftDrv.dll", 0
    };
    for (int i = 0; deferred[i]; i++)
        if (dll_name_ieq(name, deferred[i])) return 1;
    return 0;
}

static void winexec_preload_dlls(void)
{
    uint32_t count = osfs2_file_count();
    int loaded = 0;

    serial_puts("[WINEXEC] pre-loading DLLs from filesystem...\n");

    for (uint32_t i = 0; i < count; i++) {
        void *f = osfs2_file_by_index(i);
        if (!f) continue;
        const char *name = osfs2_file_name(f);
        if (!name || !str_ends_with_dll(name)) continue;

        /* Skip if already loaded (import DLLs or shim DLLs) */
        if (dll_find_module(name)) continue;

        /* Defer render-device plugins to runtime LoadLibrary (see above). */
        if (is_deferred_render_dll(name)) {
            serial_puts("[WINEXEC] preload SKIP (render device, load on demand): ");
            serial_puts(name);
            serial_puts("\n");
            continue;
        }

        uint64_t fsize = osfs2_file_size(f);
        if (fsize == 0) continue;

        serial_puts("[WINEXEC] preload: ");
        serial_puts(name);
        serial_puts(" (");
        serial_putdec(fsize / 1024);
        serial_puts(" KB)...\n");

        uint64_t pages = (fsize + 0xFFF) / 4096;
        uint8_t *buf = (uint8_t *)mem_alloc_pages(pages);
        if (!buf) continue;

        osfs2_read(f, 0, buf, fsize);
        dll_load(name, (const BYTE *)buf, (SIZE_T)fsize);
        serial_puts("[WINEXEC] preload done: ");
        serial_puts(name);
        serial_puts("\n");
        loaded++;
        /* Intentionally leak temp buffer — freeing pages allows
         * mem_alloc_pages to recycle them for VirtualAlloc, which
         * zero-fills, potentially corrupting live PE32 heap data
         * (FName::Names entries, UClass objects, etc.) */
        loaded++;
    }

    serial_puts("[WINEXEC] preloaded ");
    serial_puthex(loaded, 2);
    serial_puts(" DLLs\n");
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

    /* A completed main PE may leave private VirtualAlloc/section mappings in
     * the shell's kernel CR3. Release them before assigning owner PID 1 to the
     * next run; resetting tracker metadata would leak both PTEs and pages. */
    wsock_release_process(1);
    advapi32_crypto_release_process(1);
    nt_vm_release_process(1);

    /* A previous PE32 process may have left the shared shim mode selected. */
    g_compat32_mode = 0;

    /* Initialize subsystems */
    NT_SERVICE_TABLE ssdt;
    nt_syscall_init(&ssdt);
    ntdll_shim_init();
    kernel32_shim_init();
    msvcrt_shim_init();
    shell32_shim_init();  /* release tray-owned icon copies before user32 */
    user32_shim_init();   /* re-exec: window/input/activation state reset */
    oleacc_shim_init();
    ddraw_shim_init();    /* re-exec: surfaces/COM proxies/display-mode reset */
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
    dll_register_shim("libusb-1.0.dll", libusb_resolve);

    /* Phase 1: register each shim's co-located ABI table (argc + callconv
     * derived from the prototype) so the IAT thunk's RET N comes from the
     * real signature, not the name-keyed guess_num_args default-4. */
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
    compat32_init();

    /* Create base SEH handler thunk (needs thunk pool from compat32_init) */
    {
        extern void crt_install_base_seh_thunk(void);
        crt_install_base_seh_thunk();
    }

    /* Native DLL entry points execute during pe_load(). Establish GS, the
     * live PEB and normalized process parameters before the loader can call
     * any of them. win32_tls_reset() below then installs the fresh TLS vectors
     * into this TEB without erasing the ABI fields. */
    if (!setup_environment(NULL, FALSE)) {
        serial_puts("[WINEXEC] failed to initialize process environment\n");
        g_compat32_mode = 0;
        return -1;
    }

    /*
     * Pre-initialize TEB32/PEB32 and set FS base BEFORE pe_load().
     *
     * pe_load() recursively loads DLLs and calls DllMain via compat32
     * callbacks. DllMain's CRT startup code registers SEH handlers:
     *   push dword ptr fs:[0]   ; save current ExceptionList
     *   mov  fs:[0], esp        ; install new handler
     *
     * If FS_BASE isn't pointing to g_teb32 yet, fs:[0] reads garbage
     * from whatever address FS_BASE points to (0 or stale kernel TLS).
     * That garbage gets saved as the "previous" SEH chain link.
     * Later, SEH unwind restores it: mov fs:[0], eax → ExceptionList
     * becomes the garbage value (e.g. 0x6), corrupting the SEH chain.
     *
     * Fix: set up TEB32 with ExceptionList=0xFFFFFFFF and point
     * FS_BASE to it before any 32-bit code executes.
     */
    {
        uint8_t *p;

        p = (uint8_t *)&g_peb32;
        for (int i = 0; i < (int)sizeof(g_peb32); i++) p[i] = 0;
        g_peb32.BeingDebugged = 0;
        g_peb32.ProcessHeap   = 0xBEEF0001;

        p = (uint8_t *)&g_teb32;
        for (int i = 0; i < (int)sizeof(g_teb32); i++) p[i] = 0;
        g_teb32.Self                    = (uint32_t)(ULONG_PTR)&g_teb32;
        g_teb32.ProcessEnvironmentBlock = (uint32_t)(ULONG_PTR)&g_peb32;
        g_teb32.ClientId_UniqueProcess  = 1;
        g_teb32.ClientId_UniqueThread   = 1;
        g_teb32.LastErrorValue          = 0;
        g_teb32.ExceptionList           = 0xFFFFFFFF; /* empty SEH chain */

        win32_tls_reset();
        compat32_setup_teb(&g_teb32);

        serial_puts("[WINEXEC] TEB32 pre-initialized, FS base set\n");
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
        g_compat32_mode = 0;
        return -1;
    }

    win32_publish_current_image_base(info.ImageBase);
    g_exe_subsystem = info.Subsystem;
    serial_puts("[WINEXEC] PE loaded successfully\n");
    serial_puts("[WINEXEC] subsystem: ");
    serial_puthex(info.Subsystem, 4);
    serial_puts(info.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI
                ? " (console)\n" : " (other)\n");

    if (info.Is32Bit)
        g_peb32.ImageBaseAddress = (uint32_t)(ULONG_PTR)info.ImageBase;
    serial_puts("[WINEXEC] PEB/TEB initialized, GS base set\n");

    /* For PE32 (i386): patch EXE IAT and set up FS:TEB */
    if (info.Is32Bit) {
        g_compat32_mode = 1;
        serial_puts("[WINEXEC] PE32 (i386) detected — patching EXE IAT\n");

        /* Patch IAT: replace truncated 64-bit ptrs with 32-bit thunk addrs */
        NTSTATUS compat_st = compat32_patch_iat(&info);
        if (!NT_SUCCESS(compat_st)) {
            serial_puts("[WINEXEC] compat32 IAT patch failed\n");
            pe_unload(&info);
            g_compat32_mode = 0;
            return -1;
        }

        compat_st = pe_finalize_image_protections(&info);
        if (!NT_SUCCESS(compat_st)) {
            serial_puts("[WINEXEC] final protection setup failed: 0x");
            serial_puthex((uint32_t)compat_st, 8);
            serial_puts("\n");
            pe_unload(&info);
            g_compat32_mode = 0;
            return -1;
        }

        compat_st = compat32_attach_tls(&info);
        if (!NT_SUCCESS(compat_st)) {
            serial_puts("[WINEXEC] compat32 TLS setup failed: 0x");
            serial_puthex((uint32_t)compat_st, 8);
            serial_puts("\n");
            pe_unload(&info);
            g_compat32_mode = 0;
            return -1;
        }

        /* Set FS base for 32-bit TEB access (Windows i386 uses FS:0) */
        compat32_setup_teb(&g_teb32);

        serial_puts("[WINEXEC] TEB32.ExceptionList after setup = 0x");
        serial_puthex(g_teb32.ExceptionList, 8);
        serial_puts("\n");
    } else {
        NTSTATUS tls_st = pe_finalize_image_protections(&info);
        if (!NT_SUCCESS(tls_st)) {
            serial_puts("[WINEXEC] final protection setup failed: 0x");
            serial_puthex((uint32_t)tls_st, 8);
            serial_puts("\n");
            pe_unload(&info);
            g_compat32_mode = 0;
            return -1;
        }

        tls_st = win64_attach_tls(&info);
        if (!NT_SUCCESS(tls_st)) {
            serial_puts("[WINEXEC] PE64 TLS setup failed: 0x");
            serial_puthex((uint32_t)tls_st, 8);
            serial_puts("\n");
            pe_unload(&info);
            g_compat32_mode = 0;
            return -1;
        }
    }

    /* UT99 pre-loads all engine DLLs to register native classes.
     * Force the GMalloc stub during preload: UE1 IMPLEMENT_CLASS constructors
     * call appMalloc, but FMallocWindows::Init (appInit) hasn't run, so the
     * real allocator's Heap is NULL and would fault. The stub routes to the
     * same HeapAlloc pool, so the handoff to the real allocator is seamless. */
    extern char win32_exe_name[64];
    if (dll_name_ieq(win32_exe_name, "UnrealTournament.exe")) {
        extern int g_gmalloc_preload_phase;
        g_gmalloc_preload_phase = 1;
        winexec_preload_dlls();
        g_gmalloc_preload_phase = 0;
    }

    /* ── Diagnostic: inspect UE1 GAutoRegister linked list ─────── */
    {
        LOADED_MODULE *core = dll_find_module("Core.dll");
        if (!core) { serial_puts("[DIAG] Core.dll not found!\n"); }
        else {
            PVOID ar_ptr = dll_resolve_export(core,
                "?GAutoRegister@UObject@@0PAV1@A", 0, FALSE);
            PVOID uobj_psc = dll_resolve_export(core,
                "?PrivateStaticClass@UObject@@0VUClass@@A", 0, FALSE);
            /* GetSuperClass: read first few bytes to find SuperField offset */
            PVOID gsc_fn = dll_resolve_export(core,
                "?GetSuperClass@UClass@@QBEPAV1@XZ", 0, FALSE);

            serial_puts("\n[DIAG] === GAutoRegister Inspection ===\n");
            serial_puts("[DIAG] GAutoRegister @");
            serial_puthex((uint64_t)(ULONG_PTR)ar_ptr, 8);
            serial_puts("\n[DIAG] UObject::PrivateStaticClass @");
            serial_puthex((uint64_t)(ULONG_PTR)uobj_psc, 8);
            serial_puts("\n");

            /* Disassemble GetSuperClass to find SuperField offset.
             * Expected: mov eax,[ecx+XX]; ret  →  8B 41 XX C3 */
            int super_offset = -1;
            if (gsc_fn) {
                uint8_t *code = (uint8_t *)gsc_fn;
                serial_puts("[DIAG] GetSuperClass bytes:");
                for (int b = 0; b < 8; b++) {
                    serial_puts(" ");
                    serial_puthex(code[b], 2);
                }
                serial_puts("\n");
                if (code[0] == 0x8B && code[1] == 0x41) {
                    super_offset = (int)(int8_t)code[2];
                } else if (code[0] == 0x8B && code[1] == 0x81) {
                    super_offset = *(int32_t *)(code + 2);
                }
                if (super_offset >= 0) {
                    serial_puts("[DIAG] SuperField offset = +0x");
                    serial_puthex(super_offset, 2);
                    serial_puts("\n");
                }
            }

            if (ar_ptr) {
                uint32_t head = *(uint32_t *)ar_ptr;
                serial_puts("[DIAG] GAutoRegister head = 0x");
                serial_puthex(head, 8);
                serial_puts("\n");

                if (head == 0) {
                    serial_puts("[DIAG] ** NULL — NO classes! **\n");
                } else {
                    /* Discover next offset */
                    uint32_t first_vt = *(uint32_t *)(ULONG_PTR)head;
                    int next_off = -1;
                    for (int off = 4; off <= 32; off += 4) {
                        uint32_t v = *(uint32_t *)((ULONG_PTR)head + off);
                        if (v >= 0x100000 && v < 0x12000000
                            && v != first_vt && v != head) {
                            uint32_t pv = *(uint32_t *)(ULONG_PTR)v;
                            if (pv == first_vt) { next_off = off; break; }
                        }
                    }
                    serial_puts("[DIAG] next_off=+0x");
                    serial_puthex(next_off >= 0 ? next_off : 0xFF, 2);
                    serial_puts("\n");

                    /* Count entries and find UObject's class */
                    uint32_t cur = head;
                    int total = 0;
                    int uobj_idx = -1;
                    uint32_t uobj_addr = uobj_psc
                        ? (uint32_t)(ULONG_PTR)uobj_psc : 0;

                    while (cur && total < 5000) {
                        if (cur == uobj_addr) uobj_idx = total;
                        total++;
                        if (next_off < 0) break;
                        uint32_t n = *(uint32_t *)((ULONG_PTR)cur + next_off);
                        if (n == 0 || n < 0x1000) break;
                        /* Validate: same vtable? */
                        uint32_t nv = *(uint32_t *)(ULONG_PTR)n;
                        if (nv != first_vt) break;
                        cur = n;
                    }

                    serial_puts("[DIAG] total=");
                    serial_putdec(total);
                    serial_puts(" UObject_idx=");
                    if (uobj_idx >= 0) serial_putdec(uobj_idx);
                    else serial_puts("NOT_FOUND");
                    serial_puts("\n");

                    /* Dump UObject's PrivateStaticClass fields */
                    if (uobj_psc) {
                        uint8_t *p = (uint8_t *)uobj_psc;
                        serial_puts("[DIAG] UObject UClass dump:\n");
                        for (int j = 0; j < 64; j += 4) {
                            uint32_t v = *(uint32_t *)(p + j);
                            serial_puts("[DIAG]   +0x");
                            serial_puthex(j, 2);
                            serial_puts(": 0x");
                            serial_puthex(v, 8);
                            if (j == 0) serial_puts(" (vtable)");
                            if (super_offset >= 0 && j == super_offset)
                                serial_puts(" (SuperField)");
                            if (j == 0x1C) serial_puts(" (flags?)");
                            serial_puts("\n");
                        }
                    }

                    /* Dump 3 entries: first, middle, last */
                    int show_idx[] = {0, total/2, total-1};
                    for (int si = 0; si < 3; si++) {
                        int target = show_idx[si];
                        cur = head;
                        for (int k = 0; k < target && next_off >= 0; k++) {
                            uint32_t n = *(uint32_t *)((ULONG_PTR)cur + next_off);
                            if (n == 0 || n < 0x1000) break;
                            cur = n;
                        }
                        serial_puts("[DIAG] entry[");
                        serial_putdec(target);
                        serial_puts("] @0x");
                        serial_puthex(cur, 8);
                        serial_puts(":\n");
                        uint8_t *p = (uint8_t *)(ULONG_PTR)cur;
                        for (int j = 0; j < 48; j += 4) {
                            uint32_t v = *(uint32_t *)(p + j);
                            serial_puts("[DIAG]   +0x");
                            serial_puthex(j, 2);
                            serial_puts(": 0x");
                            serial_puthex(v, 8);
                            if (super_offset >= 0 && j == super_offset)
                                serial_puts(" <SuperField>");
                            serial_puts("\n");
                        }
                    }
                }
            }
            serial_puts("[DIAG] === End ===\n\n");
        }
    }

    /*
     * Pre-allocate GObjRegistrants TArray buffer ONLY if dllloader.c didn't
     * already pre-allocate via Core.dll's DllMain hook (typical case).
     *
     * Previous bug: unconditional re-allocation here was DESTRUCTIVE — by
     * this point Engine.dll/Window.dll/etc. had run their C++ static
     * initializers and registered UClass entries (UGameEngine etc.) into
     * GObjRegistrants @ dllloader's buffer (0x01FF6000). Overwriting
     * tarray[0..2] with a fresh empty buffer (0x01F74000, Num=0) orphaned
     * those entries. The engine's later ProcessRegistrants then iterated
     * an empty list → no UClass("GameEngine") registered in GObj →
     * StaticLoadClass("Engine.GameEngine") fails → "Failed to load
     * 'None None.GameEngine'" cascade.
     *
     * Fix: only allocate if tarray[0] is NULL (i.e. dllloader didn't run
     * yet, e.g. running a PE that doesn't load Core.dll).
     */
    {
        LOADED_MODULE *core = dll_find_module("Core.dll");
        if (core) {
            PVOID gobjreg_ptr = dll_resolve_export(core,
                "?GObjRegistrants@UObject@@0V?$TArray@PAVUObject@@@@A", 0, FALSE);
            if (gobjreg_ptr) {
                uint32_t *tarray = (uint32_t *)gobjreg_ptr;
                if (tarray[0] == 0) {
                    /* Not pre-allocated yet — allocate a fallback buffer. */
                    void *buf = mem_alloc_pages(1);
                    if (buf) {
                        uint64_t pa = (uint64_t)buf;
                        uint8_t *p = (uint8_t *)pa;
                        for (int i = 0; i < 4096; i++) p[i] = 0;
                        tarray[0] = (uint32_t)pa;
                        tarray[1] = 0;
                        tarray[2] = 1024;
                        serial_puts("[WINEXEC] Pre-allocated GObjRegistrants (fallback): "
                                    "Data=0x");
                        serial_puthex(pa, 8);
                        serial_puts(" Max=1024 @TArray=0x");
                        serial_puthex((uint64_t)(ULONG_PTR)gobjreg_ptr, 8);
                        serial_puts("\n");
                    }
                } else {
                    serial_puts("[WINEXEC] GObjRegistrants already populated by "
                                "dllloader: Data=0x");
                    serial_puthex((uint64_t)tarray[0], 8);
                    serial_puts(" Num=");
                    serial_putdec((uint64_t)tarray[1]);
                    serial_puts(" Max=");
                    serial_putdec((uint64_t)tarray[2]);
                    serial_puts(" — preserving DLL static-init registrants\n");
                }
            }
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
        pe_unload(&info);
        g_compat32_mode = 0;
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
        g_teb32.StackBase = (uint32_t)(ULONG_PTR)stack_base;
        g_teb32.StackLimit = (uint32_t)(ULONG_PTR)stack_usable;
    } else {
        g_teb.StackBase = stack_base;
        g_teb.StackLimit = stack_usable;
        g_teb.DeallocationStack = stack_allocation;
    }

    if (info.Is32Bit) {
        serial_puts("[WINEXEC] TEB32.ExceptionList before EXE entry = 0x");
        serial_puthex(g_teb32.ExceptionList, 8);
        serial_puts("\n");

        /* Hardware watchpoint on TEB32 removed — was causing #GP
         * when #DB fires from compat mode during DLL init */
    }

    serial_puts("[WINEXEC] jumping to entry point at ");
    serial_puthex((uint64_t)info.EntryPoint, 16);
    if (info.Is32Bit) serial_puts(" (32-bit compat mode)");
    serial_puts("\n");

    /* Dump UGameEngine class hierarchy BEFORE EXE entry.
     * ConstructObject fails because SuperField may be NULL at this point. */
    if ((uint32_t)(uintptr_t)info.ImageBase == 0x10900000u) {
        volatile uint32_t *ge_cls = (volatile uint32_t *)(uintptr_t)0x105928A0;
        volatile uint32_t *ue_iat = (volatile uint32_t *)(uintptr_t)0x10958D74;
        serial_puts("[DIAG] Before EXE: UGameEngine::SC SuperField=0x");
        serial_puthex(ge_cls[0x28/4], 8);
        serial_puts(" UEngine::SC(IAT)=0x");
        serial_puthex(*ue_iat, 8);
        serial_puts("\n");

        /* Patch INT3 at EXE+0xBC72 (after ConstructObject) and at
         * EXE+0xBC81 (load GEngine into ECX before Init call). */
        uint8_t *bp1 = (uint8_t *)((uintptr_t)info.ImageBase + 0xBC72);
        uint8_t *bp2 = (uint8_t *)((uintptr_t)info.ImageBase + 0xBC81);
        serial_puts("[DIAG] INT3 at 0x");
        serial_puthex((uint64_t)(uintptr_t)bp1, 8);
        serial_puts(" + 0x");
        serial_puthex((uint64_t)(uintptr_t)bp2, 8);
        serial_puts("\n");
        bp1[0] = 0xCC;
        bp2[0] = 0xCC;
    }

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

        /* Install permanent base SEH frame on PE stack.
         * This sits at the bottom of the chain and survives all
         * stack corruption from inner frames (WinDrv.dll bug). */
        {
            extern uint32_t crt_get_base_seh_thunk(void);
            uint32_t handler = crt_get_base_seh_thunk();
            if (handler) {
                uint32_t *sp = (uint32_t *)(uintptr_t)sp32;
                sp -= 3;
                sp[0] = g_teb32.ExceptionList; /* Next = current head */
                sp[1] = handler;               /* Handler = catch-all */
                sp[2] = 0;                     /* Scope/state = 0 */
                g_teb32.ExceptionList = (uint32_t)(uintptr_t)sp;
                extern uint32_t g_base_seh_frame_addr;
                g_base_seh_frame_addr = (uint32_t)(uintptr_t)sp;
                sp32 = (uint32_t)(uintptr_t)sp;
                serial_puts("[WINEXEC] Base SEH frame at 0x");
                serial_puthex((uint32_t)(uintptr_t)sp, 8);
                serial_puts(" handler=0x");
                serial_puthex(handler, 8);
                serial_puts("\n");
            }
        }

        /* Clear GErrorHist and GIsCriticalError before WinMain.
         * DLL _initterm callbacks trigger null-pointer faults (handled by our
         * null-page write handler), which cause the engine's error handler to
         * set GErrorHist="General protection fault!". If GErrorHist is set when
         * the engine tries to Browse() a map, it skips rendering → error exit.
         * Clear both so the engine starts fresh. */
        if ((uint32_t)(uintptr_t)info.ImageBase == 0x10900000u) {
            /* GErrorHist: Core.dll + RVA 0xE3474 (TCHAR[1024], wide string) */
            volatile uint16_t *gerr = (volatile uint16_t *)(uintptr_t)0x101E3474;
            /* GIsCriticalError: Core.dll + RVA 0xE568C (INT, flag) */
            volatile uint32_t *gcrit = (volatile uint32_t *)(uintptr_t)0x101E568C;
            if (*gerr != 0 || *gcrit != 0) {
                *gerr = 0;   /* Clear error string */
                *gcrit = 0;  /* Clear critical error flag */
                serial_puts("[WINEXEC] Cleared GErrorHist + GIsCriticalError\n");
            }
        }

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
                /* NtTerminateProcess returned here — PE process has exited.
                 *
                 * Two cleanup steps before we let the scheduler see
                 * this kernel context again:
                 *
                 * (1) Restore 64-bit kernel data segments. compat32_enter
                 *     set DS/ES/SS to GDT_SEL_DATA32 (0x48) for the PE
                 *     lifetime. proc_exit longjmped back, restoring
                 *     RIP/RSP but NOT segment selectors. If we don't
                 *     fix them, the next scheduler tick observes PID 1
                 *     with SS=0x48 and bails with "[SCHED] CORRUPT PID 1",
                 *     leaving the shell unschedulable.
                 *
                 * (2) Re-enable the APIC LVT timer. compat32_enter
                 *     masked it for the entire PE lifetime; without
                 *     re-enabling, the preemptive scheduler can't tick
                 *     and `desktop` (which sched_spawns a compositor
                 *     thread) never runs because PID 1 never yields. */
                __asm__ volatile (
                    "mov $0x30, %%ax\n"
                    "mov %%ax, %%ds\n"
                    "mov %%ax, %%es\n"
                    "mov %%ax, %%ss\n"
                    ::: "ax"
                );

                /* Reap any orphan win32 threads spawned by the PE via
                 * CreateThread. Why this is necessary: if we leave
                 * them schedulable, the scheduler will dispatch them
                 * and they'll re-enter compat32_callback_args, which
                 * re-masks the APIC LVT timer (the entire-PE-lifetime
                 * mask from compat32_enter). The callback path
                 * deliberately doesn't unmask on the way out
                 * (compat32.c:1294), so any subsequent kernel
                 * `hlt`-wait — including compositor's
                 * display_wait_vblank — would deadlock forever.
                 *
                 * proc_kill_pid runs the full proc_free cleanup so
                 * the slot is reusable for the next PE invocation,
                 * not left as a permanent ZOMBIE. */
                {
                    typedef struct {
                        int      kernel_pid;
                        uint32_t tid;
                        ULONG_PTR func_addr;
                    } win32_orphan_info_t;
                    extern int proc_kill_pid(int pid);
                    extern int win32_collect_orphan_threads(
                        win32_orphan_info_t *out, int max);

                    win32_orphan_info_t orphans[48];
                    int n = win32_collect_orphan_threads(orphans, 48);
                    for (int i = 0; i < n; i++) {
                        int rc = proc_kill_pid(orphans[i].kernel_pid);
                        serial_puts(rc == 0
                            ? "[winexec] reaped orphan PE thread tid="
                            : "[winexec] FAILED to reap orphan PE thread tid=");
                        serial_putdec((uint64_t)orphans[i].tid);
                        serial_puts(" pid=");
                        serial_putdec((uint64_t)orphans[i].kernel_pid);
                        serial_puts(" entry=0x");
                        serial_puthex(orphans[i].func_addr, 16);
                        serial_puts("\n");
                    }
                }

                {
                    extern volatile uint32_t *idt_get_apic_base(void);
                    volatile uint32_t *apic = idt_get_apic_base();
                    if (apic) apic[0x320/4] &= ~0x10000u;  /* LVT_TIMER &= ~MASKED */
                }
                __asm__ volatile ("sti");
                g_compat32_mode = 0;
                user32_shim_init();
                ddraw_shim_init();
                serial_puts("[WINEXEC] PE process exited, code=");
                serial_putdec((uint32_t)exit_status);
                serial_puts(" (SS restored, APIC re-enabled)\n");
                wsock_release_process(1);
                advapi32_crypto_release_process(1);
                (void)nt_vm_free_stack(stack_allocation);
                pe_unload(&info);
                g_compat32_mode = 0;
                return (int32_t)exit_status;
            }
            __atomic_store_n(&g_win32_main.active, TRUE,
                             __ATOMIC_RELEASE);
        }

        /* HWBP bisect instrumentation removed (Phase 2 cleanup) -- the
         * EDI-clobber root cause it isolated is fixed; see git history. */
        if (entry32 == 0x0054D26Du) {
            extern uint8_t g_swbreak_saved;
            extern uint32_t g_swbreak_addr;
            g_swbreak_addr = 0x00454D08u;
            g_swbreak_saved = *(uint8_t *)(uintptr_t)g_swbreak_addr;
            *(uint8_t *)(uintptr_t)g_swbreak_addr = 0xCC;
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

            {
                extern int win32_terminate_process_threads(PPEB owner,
                                                            DWORD exit_code);
                int stopped = win32_terminate_process_threads(
                    &g_peb, (DWORD)exit_status);
                if (stopped) {
                    serial_puts("[WINEXEC] stopped owned PE64 threads=");
                    serial_putdec((uint64_t)stopped);
                    serial_puts("\n");
                }
            }

            wsock_release_process(1);
            advapi32_crypto_release_process(1);
            {
                extern uint64_t paging_get_kernel_cr3(void);
                __asm__ volatile ("mov %0, %%cr3" : :
                                  "r"(paging_get_kernel_cr3()) : "memory");
            }
            (void)nt_vm_free_stack(stack_allocation);
            pe_unload(&info);
            g_compat32_mode = 0;
            return (int32_t)exit_status;
        }
        __atomic_store_n(&g_win32_main.active, TRUE, __ATOMIC_RELEASE);

        win64_call_child_entry(info.EntryPoint, stack_top, &g_peb,
                               info.Subsystem == IMAGE_SUBSYSTEM_NATIVE);
    }

    /* If entry point returns (unusual — most call ExitProcess) */
    __atomic_store_n(&g_win32_main.active, FALSE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_win32_main.exit_requested, FALSE, __ATOMIC_RELEASE);
    serial_puts("[WINEXEC] PE entry returned\n");

    /* Restore kernel CR3 */
    {
        extern uint64_t paging_get_kernel_cr3(void);
        __asm__ volatile ("mov %0, %%cr3" : : "r"(paging_get_kernel_cr3()) : "memory");
    }

    /* Cleanup */
    wsock_release_process(1);
    advapi32_crypto_release_process(1);
    (void)nt_vm_free_stack(stack_allocation);
    pe_unload(&info);

    g_compat32_mode = 0;
    return 0;
}
