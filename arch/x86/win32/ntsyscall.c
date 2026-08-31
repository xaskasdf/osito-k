/*
 * OsitoK Windows Compatibility Layer — NT Syscall Dispatch & Handlers
 *
 * Implemented:
 *   - NtCreateFile / NtReadFile / NtWriteFile / NtClose
 *   - NtQueryInformationFile / NtSetInformationFile
 *   - NtAllocateVirtualMemory / NtFreeVirtualMemory
 *   - NtProtectVirtualMemory / NtQueryVirtualMemory
 *   - NtDuplicateObject
 *   - NtTerminateProcess
 *   - NtDelayExecution
 *   - NtQueryPerformanceCounter
 *   - NtYieldExecution
 *
 * Stubs return STATUS_NOT_IMPLEMENTED for unhandled syscalls.
 */

#include "ntsyscall.h"
#include "handle.h"
#include "pe.h"
#include "dllloader.h"
#include "dinput8_shim.h"
#include "kernel32_shim.h"
#include "msvcrt_shim.h"
#include "../include/fd.h"
#include "../include/paging.h"
#include "../kernel/smp.h"
#include "../fs/vfs.h"

/* ── External kernel interfaces ─────────────────────────────── */

/* Serial debug output */
extern void serial_puts(const char *s);
extern void serial_write(const char *data, uint64_t length);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void sched_yield(void);
extern BOOL win32_current_is_gui_app(void);
extern DWORD win32_current_process_id(void);

/* Heap */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Framebuffer console */
extern void fb_puts(const char *s);
extern void fb_putc(char c, uint32_t color);

/* OsitoFS */
extern void *osfs2_find(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
extern int   osfs2_truncate(void *file, uint64_t size);
extern int   osfs2_file_retain(void *file);
extern void  osfs2_file_release(void *file);
extern uint64_t osfs2_file_revision(void *file);
extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_rename(const char *from, const char *to, bool replace);
extern uint64_t osfs2_file_size(void *file);
extern const char *osfs2_file_name(void *file);

/* Physical memory */
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

/* Paging */
extern int paging_map_page_in_cr3(uint64_t cr3, uint64_t virt,
                                  uint64_t phys, uint64_t flags);
extern int paging_unmap_page_in_cr3(uint64_t cr3, uint64_t virt);
extern int paging_set_flags_in_cr3(uint64_t cr3, uint64_t virt,
                                   uint64_t flags);
extern uint64_t *paging_get_pte_in_cr3(uint64_t cr3, uint64_t virt);
extern uint64_t paging_get_kernel_cr3(void);
extern uint64_t proc_current_cr3(void);

static void nt_log_hex(const char *prefix, ULONGLONG val);

/* PTE flags (must match paging.c) */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* ── Virtual address allocator for Win32 VirtualAlloc ────────── */
/*
 * Win32 VirtualAlloc semantics: each allocation returns a UNIQUE virtual
 * address mapped to freshly zeroed pages.  Previous implementation used
 * identity-mapped mem_alloc_pages which recycled physical addresses,
 * causing live data destruction (FName::Names bug in UT99).
 *
 * Fix: allocate physical pages + map at unique VA via paging_map_page().
 * VA range: 0x20000000-0x78000000 (Win32 user heap region, below 2GB).
 */
#define WIN32_VA_BASE  0x20000000ULL
/* WAS 0x7FFF0000 — but the kernel's pre-MMU UEFI stack lives around
 * 0x7FE60000, INSIDE the old VA range. UT99 making >900 MB of
 * VirtualAlloc would push win32_va_next past 0x7FE60000 and start
 * overwriting the shell's saved registers / setjmp buffer that lives
 * on that stack. After UT99 crash + longjmp returns to shell, the
 * corrupted return addresses caused #PF at RIP=0.
 *
 * Cap at 0x78000000 (= 1920 MB user range, 0x40000000..0x78000000) to
 * leave a 128 MB gap below the kernel's UEFI stack. Plenty for UT99
 * engine init (1737 VirtualAllocs observed; even with the 256MB cap
 * burst, total ~600 MB sustained). */
#define WIN32_VA_LIMIT 0x78000000ULL
#define WIN64_VA_BASE        0x0000000200000000ULL
#define WIN64_AUTO_VA_FLOOR  0x0000004000000000ULL
#define WIN64_VA_LIMIT       0x0000010000000000ULL
#define WIN64_FIXED_VA_BASE  0x0000000000010000ULL
#define WIN64_FIXED_VA_LIMIT 0x0000800000000000ULL

/* Track VA→phys mapping for cleanup on VirtualFree */
#define VM_TRACK_MAX 32768
#define VM_PROCESS_STATE_MAX 128

typedef struct {
    uint64_t va;
    uint64_t phys;
    uint64_t section_offset;
    uint64_t cr3;
    uint64_t allocation_base;
    SIZE_T   size;
    ULONG    allocation_protect;
    ULONG    protect;
    ULONG    owner_pid;
    BOOL     owns_phys;
    PVOID    section;
} vm_track_entry_t;

typedef struct {
    ULONG    owner_pid;
    uint64_t next32;
    uint64_t next64;
    BOOL     used;
} vm_process_state_t;

static vm_track_entry_t vm_track[VM_TRACK_MAX];
static vm_process_state_t vm_process_states[VM_PROCESS_STATE_MAX];
static int vm_track_count = 0;
static int vm_track_overflow_reported = 0;
static spinlock_t vm_track_lock = SPINLOCK_INIT;
static uint64_t vm_safe_auto_base = WIN64_AUTO_VA_FLOOR;
static volatile uint32_t vm_init_state;

static void section_view_release(PVOID section);
static void section_view_retain_existing(PVOID section);
static void section_release_handle(PVOID section);
static BOOL section_view_allows_protect(PVOID section, ULONG protect);
static NTSTATUS section_flush_mapping(PVOID section, uint64_t view_phys,
                                      SIZE_T view_size, ULONG protect,
                                      SIZE_T section_offset,
                                      SIZE_T relative_offset,
                                      SIZE_T bytes_to_flush);
static int vm_map_private_backing(uint64_t cr3, uint64_t va, SIZE_T size,
                                  ULONG protect, uint64_t *out_phys);

static ULONG nt_current_owner_pid(void)
{
    ULONG owner_pid = win32_current_process_id();
    return owner_pid ? owner_pid : 1;
}

static uint64_t nt_current_cr3(void)
{
    uint64_t cr3 = proc_current_cr3();
    return cr3 ? cr3 : paging_get_kernel_cr3();
}

static int nt_map_page_in(uint64_t cr3, uint64_t va, uint64_t phys,
                          uint64_t flags)
{
    return paging_map_page_in_cr3(cr3 ? cr3 : paging_get_kernel_cr3(),
                                  va, phys, flags);
}

static int nt_unmap_page_in(uint64_t cr3, uint64_t va)
{
    return paging_unmap_page_in_cr3(cr3 ? cr3 : paging_get_kernel_cr3(),
                                    va);
}

static int nt_set_page_flags_in(uint64_t cr3, uint64_t va, uint64_t flags)
{
    return paging_set_flags_in_cr3(cr3 ? cr3 : paging_get_kernel_cr3(),
                                   va, flags);
}

static uint64_t *nt_get_pte_in(uint64_t cr3, uint64_t va)
{
    return paging_get_pte_in_cr3(cr3 ? cr3 : paging_get_kernel_cr3(), va);
}

static void vm_global_init_once(void)
{
    uint32_t state = __atomic_load_n(&vm_init_state, __ATOMIC_ACQUIRE);
    if (state == 2)
        return;

    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&vm_init_state, &expected, 1, FALSE,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        extern uint64_t mem_get_highest_address(void);
        const uint64_t one_gib = 1ULL << 30;
        uint64_t identity_end = mem_get_highest_address();

        if (identity_end < 4ULL * one_gib)
            identity_end = 4ULL * one_gib;
        if (identity_end <= UINT64_MAX - (one_gib - 1))
            identity_end = (identity_end + one_gib - 1) & ~(one_gib - 1);
        else
            identity_end = WIN64_VA_LIMIT;

        if (identity_end >= vm_safe_auto_base) {
            vm_safe_auto_base = identity_end <= WIN64_VA_LIMIT - one_gib
                              ? identity_end + one_gib
                              : WIN64_VA_LIMIT;
        }
        __atomic_store_n(&vm_init_state, 2, __ATOMIC_RELEASE);
        return;
    }

    while (__atomic_load_n(&vm_init_state, __ATOMIC_ACQUIRE) != 2)
        __asm__ volatile ("pause");
}

/* Must be called with vm_track_lock held. */
static vm_process_state_t *vm_process_state_get_locked(ULONG owner_pid,
                                                        BOOL create)
{
    int free_slot = -1;
    for (int i = 0; i < VM_PROCESS_STATE_MAX; i++) {
        if (vm_process_states[i].used &&
            vm_process_states[i].owner_pid == owner_pid)
            return &vm_process_states[i];
        if (!vm_process_states[i].used && free_slot < 0)
            free_slot = i;
    }
    if (!create || free_slot < 0)
        return NULL;

    vm_process_state_t *state = &vm_process_states[free_slot];
    state->owner_pid = owner_pid;
    state->next32 = WIN32_VA_BASE;
    state->next64 = vm_safe_auto_base;
    state->used = TRUE;
    return state;
}

/* Must be called with vm_track_lock held. */
static uint64_t *vm_auto_next_locked(ULONG owner_pid, int compat32)
{
    vm_process_state_t *state = vm_process_state_get_locked(owner_pid, TRUE);
    if (!state)
        return NULL;
    return compat32 ? &state->next32 : &state->next64;
}

/* Must be called with vm_track_lock held. */
static void vm_advance_auto_next_locked(ULONG owner_pid, int compat32,
                                        uint64_t va, SIZE_T size)
{
    uint64_t *next = vm_auto_next_locked(owner_pid, compat32);
    uint64_t end = va + size;
    if (!next || end < va)
        return;

    if (compat32) {
        if (va >= WIN32_VA_BASE && end <= WIN32_VA_LIMIT && end > *next)
            *next = end;
    } else if (va >= WIN64_VA_BASE && end <= WIN64_VA_LIMIT && end > *next) {
        *next = end;
    }
}

/* Reserve-only probes are commonly used to find an aligned address. Rewind
 * the allocation hint when such a probe is released so the next, usually
 * slightly larger, reservation can reuse the hole. Ranges that had physical
 * backing remain monotonic for now because legacy Win32 workloads have relied
 * on that quarantine to expose stale pointers instead of aliasing new data. */
static void vm_rewind_auto_next_locked(ULONG owner_pid, uint64_t va)
{
    vm_process_state_t *state = vm_process_state_get_locked(owner_pid, FALSE);
    if (!state)
        return;

    if (va >= WIN32_VA_BASE && va < WIN32_VA_LIMIT && va < state->next32)
        state->next32 = va;
    else if (va >= WIN64_VA_BASE && va < WIN64_VA_LIMIT && va < state->next64)
        state->next64 = va;
}

static BOOL vm_track_entry_committed(const vm_track_entry_t *entry)
{
    return entry->phys != 0 || entry->section != NULL;
}

static BOOL vm_track_entry_reserved(const vm_track_entry_t *entry)
{
    return !vm_track_entry_committed(entry);
}

void nt_vm_debug_address(uint64_t address)
{
    ULONG owner_pid = nt_current_owner_pid();
    int count = vm_track_count;
    int matches = 0;

    for (int i = 0; i < count; i++) {
        vm_track_entry_t entry = vm_track[i];
        uint64_t end = entry.va + entry.size;
        if (entry.owner_pid != owner_pid || end < entry.va ||
            address < entry.va || address >= end)
            continue;

        serial_puts("  [VM-FAULT] idx=");
        serial_putdec((uint64_t)(uint32_t)i);
        serial_puts(vm_track_entry_committed(&entry)
                    ? " committed" : " reserved");
        serial_puts(" range=0x");
        serial_puthex(entry.va, 16);
        serial_puts("..0x");
        serial_puthex(end, 16);
        serial_puts(" alloc=0x");
        serial_puthex(entry.allocation_base, 16);
        serial_puts(" protect=0x");
        serial_puthex(entry.protect, 8);
        serial_puts(" alloc_protect=0x");
        serial_puthex(entry.allocation_protect, 8);
        serial_puts(" phys=0x");
        serial_puthex(entry.phys, 16);
        serial_puts(" cr3=0x");
        serial_puthex(entry.cr3, 16);
        serial_puts(" section=0x");
        serial_puthex((uint64_t)(uintptr_t)entry.section, 16);
        serial_puts("\n");
        matches++;
    }
    if (!matches)
        serial_puts("  [VM-FAULT] no owner VMA covers address\n");
}

static inline uint64_t vm_track_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&vm_track_lock);
    return flags;
}

static inline void vm_track_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&vm_track_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static int vm_track_add(uint64_t va, uint64_t phys, SIZE_T size, ULONG protect,
                        BOOL owns_phys, PVOID section,
                        uint64_t section_offset)
{
    ULONG owner_pid = nt_current_owner_pid();
    uint64_t cr3 = nt_current_cr3();
    uint64_t allocation_base = va;
    ULONG allocation_protect = protect;
    if (phys != 0 || section != NULL) {
        uint64_t end = va + size;
        for (int i = 0; i < vm_track_count; i++) {
            uint64_t reservation_end = vm_track[i].va + vm_track[i].size;
            if (vm_track[i].owner_pid == owner_pid &&
                vm_track_entry_reserved(&vm_track[i]) &&
                reservation_end >= vm_track[i].va && va >= vm_track[i].va &&
                end >= va && end <= reservation_end) {
                allocation_base = vm_track[i].va;
                allocation_protect = vm_track[i].allocation_protect;
                break;
            }
        }
    }

    for (int i = 0; i < vm_track_count; i++) {
        if (vm_track[i].va == va &&
            vm_track[i].owner_pid == owner_pid &&
            vm_track_entry_committed(&vm_track[i]) ==
                (phys != 0 || section != NULL)) {
            if (size > vm_track[i].size)
                vm_track[i].size = size;
            vm_track[i].protect = protect;
            vm_track[i].owns_phys = owns_phys;
            vm_track[i].section = section;
            vm_track[i].section_offset = section_offset;
            vm_track[i].cr3 = cr3;
            vm_track[i].allocation_base = allocation_base;
            vm_track[i].allocation_protect = allocation_protect;
            return 1;
        }
    }
    if (vm_track_count < VM_TRACK_MAX) {
        vm_track[vm_track_count].va   = va;
        vm_track[vm_track_count].phys = phys;
        vm_track[vm_track_count].section_offset = section_offset;
        vm_track[vm_track_count].cr3 = cr3;
        vm_track[vm_track_count].allocation_base = allocation_base;
        vm_track[vm_track_count].size = size;
        vm_track[vm_track_count].allocation_protect = allocation_protect;
        vm_track[vm_track_count].protect = protect;
        vm_track[vm_track_count].owner_pid = owner_pid;
        vm_track[vm_track_count].owns_phys = owns_phys;
        vm_track[vm_track_count].section = section;
        vm_track_count++;
        return 1;
    } else {
        if (!vm_track_overflow_reported) {
            vm_track_overflow_reported = 1;
            serial_puts("[VM-TRACK] table full; later mappings are untracked\n");
        }
    }
    return 0;
}

/* Must be called with vm_track_lock held. */
static int vm_track_find_reservation_locked(ULONG owner_pid, uint64_t va,
                                             SIZE_T size)
{
    uint64_t end = va + size;
    if (end < va)
        return -1;
    for (int i = 0; i < vm_track_count; i++) {
        uint64_t entry_end = vm_track[i].va + vm_track[i].size;
        if (vm_track[i].owner_pid == owner_pid &&
            vm_track_entry_reserved(&vm_track[i]) &&
            entry_end >= vm_track[i].va && va >= vm_track[i].va &&
            end <= entry_end)
            return i;
    }
    return -1;
}

/* Must be called with vm_track_lock held. */
static vm_track_entry_t *vm_track_find_committed_locked(ULONG owner_pid,
                                                         uint64_t va)
{
    vm_track_entry_t *best = NULL;
    for (int i = 0; i < vm_track_count; i++) {
        uint64_t end = vm_track[i].va + vm_track[i].size;
        if (vm_track[i].owner_pid != owner_pid ||
            !vm_track_entry_committed(&vm_track[i]) ||
            end < vm_track[i].va || va < vm_track[i].va || va >= end)
            continue;
        if (!best || vm_track[i].size < best->size)
            best = &vm_track[i];
    }
    return best;
}

/* Private VMA records describe virtual ranges. Their physical pages need not
 * be contiguous after independently committed ranges are merged. */
static uint64_t vm_track_page_phys(const vm_track_entry_t *entry, uint64_t va)
{
    uint64_t *pte = nt_get_pte_in(entry->cr3, va);
    return pte ? (*pte & PTE_ADDR_MASK) : 0;
}

static void vm_track_unmap_range(const vm_track_entry_t *entry,
                                 uint64_t start, uint64_t end)
{
    static uint32_t missing_phys_logs;

    for (uint64_t va = start; va < end; va += 4096) {
        uint64_t phys = entry->owns_phys && !entry->section
                      ? vm_track_page_phys(entry, va) : 0;
        nt_unmap_page_in(entry->cr3, va);
        if (phys) {
            mem_free_pages((void *)(uintptr_t)phys, 1);
        } else if (entry->owns_phys && !entry->section &&
                   __atomic_fetch_add(&missing_phys_logs, 1,
                                      __ATOMIC_RELAXED) < 8) {
            serial_puts("[VM-TRACK] missing private PTE while releasing va=0x");
            serial_puthex(va, 16);
            serial_puts("\n");
        }
    }
}

static BOOL vm_track_private_mergeable(const vm_track_entry_t *left,
                                       const vm_track_entry_t *right)
{
    uint64_t left_end = left->va + left->size;
    return left->owner_pid == right->owner_pid &&
           left->cr3 == right->cr3 &&
           left->allocation_base == right->allocation_base &&
           left->allocation_protect == right->allocation_protect &&
           left->protect == right->protect &&
           left->owns_phys && right->owns_phys &&
           !left->section && !right->section &&
           vm_track_entry_committed(left) &&
           vm_track_entry_committed(right) &&
           left_end >= left->va && left_end == right->va;
}

/* Merge the private range containing va with all compatible neighbours. The
 * lower range keeps its first-page physical address as a committed marker. */
static void vm_track_coalesce_private_at_locked(ULONG owner_pid, uint64_t va)
{
    int index = -1;
    for (int i = 0; i < vm_track_count; i++) {
        uint64_t end = vm_track[i].va + vm_track[i].size;
        if (vm_track[i].owner_pid == owner_pid && vm_track[i].owns_phys &&
            !vm_track[i].section && end >= vm_track[i].va &&
            va >= vm_track[i].va && va < end) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return;

    for (;;) {
        int neighbour = -1;
        BOOL neighbour_is_left = FALSE;
        for (int i = 0; i < vm_track_count; i++) {
            if (i == index)
                continue;
            if (vm_track_private_mergeable(&vm_track[index], &vm_track[i])) {
                neighbour = i;
                break;
            }
            if (vm_track_private_mergeable(&vm_track[i], &vm_track[index])) {
                neighbour = i;
                neighbour_is_left = TRUE;
                break;
            }
        }
        if (neighbour < 0)
            break;

        int keep = neighbour_is_left ? neighbour : index;
        int drop = neighbour_is_left ? index : neighbour;
        vm_track[keep].size += vm_track[drop].size;

        int last = --vm_track_count;
        if (drop != last) {
            vm_track[drop] = vm_track[last];
            if (keep == last)
                keep = drop;
        }
        index = keep;
    }
}

/* Undo only entries appended after initial_count. Used by commit paths that
 * must either install every missing page or leave the reservation unchanged. */
static void vm_track_rollback_appended_locked(int initial_count)
{
    while (vm_track_count > initial_count) {
        vm_track_entry_t entry = vm_track[--vm_track_count];
        if (!vm_track_entry_committed(&entry))
            continue;
        vm_track_unmap_range(&entry, entry.va, entry.va + entry.size);
        if (entry.section)
            section_view_release(entry.section);
    }
}

static int vm_protect_tracked_range_locked(ULONG owner_pid, uint64_t cr3,
                                           uint64_t base, SIZE_T size,
                                           ULONG new_protect,
                                           ULONG *old_protect);

/* Commit the holes in an existing reservation, then apply the requested
 * protection to the complete range. Win32 recommit preserves existing page
 * contents but still updates their protection. Must hold vm_track_lock. */
static int vm_commit_range_locked(ULONG owner_pid, uint64_t cr3, uint64_t va,
                                  SIZE_T size, ULONG protect)
{
    uint64_t end = va + size;
    if (end < va ||
        vm_track_find_reservation_locked(owner_pid, va, size) < 0)
        return 0;

    int initial_count = vm_track_count;
    uint64_t cursor = va;
    while (cursor < end) {
        vm_track_entry_t *cover = vm_track_find_committed_locked(owner_pid,
                                                                  cursor);
        if (cover) {
            uint64_t cover_end = cover->va + cover->size;
            cursor = cover_end < end ? cover_end : end;
            continue;
        }

        uint64_t gap_end = end;
        for (int i = 0; i < vm_track_count; i++) {
            if (vm_track[i].owner_pid == owner_pid &&
                vm_track_entry_committed(&vm_track[i]) &&
                vm_track[i].va > cursor && vm_track[i].va < gap_end)
                gap_end = vm_track[i].va;
        }
        SIZE_T gap_size = gap_end - cursor;
        if (!gap_size || vm_track_count >= VM_TRACK_MAX) {
            vm_track_rollback_appended_locked(initial_count);
            return 0;
        }

        uint64_t phys = 0;
        if (!vm_map_private_backing(cr3, cursor, gap_size, protect, &phys) ||
            !vm_track_add(cursor, phys, gap_size, protect, TRUE, NULL, 0)) {
            if (phys) {
                SIZE_T pages = gap_size / 4096;
                for (SIZE_T page = 0; page < pages; page++)
                    nt_unmap_page_in(cr3, cursor + page * 4096);
                mem_free_pages((void *)(uintptr_t)phys, pages);
            }
            vm_track_rollback_appended_locked(initial_count);
            return 0;
        }
        cursor = gap_end;
    }

    ULONG old_protect = 0;
    if (vm_protect_tracked_range_locked(owner_pid, cr3, va, size, protect,
                                        &old_protect) != 1) {
        vm_track_rollback_appended_locked(initial_count);
        return 0;
    }
    return 1;
}

/* Decommit private pages while retaining the surrounding reservation. Returns
 * 1 on success, 0 for an invalid range, and -1 for a mapped section. */
static int vm_decommit_range_locked(ULONG owner_pid, uint64_t va, SIZE_T size)
{
    uint64_t end = va + size;
    if (end < va ||
        vm_track_find_reservation_locked(owner_pid, va, size) < 0)
        return 0;

    int extra_entries = 0;
    for (int i = 0; i < vm_track_count; i++) {
        vm_track_entry_t *entry = &vm_track[i];
        uint64_t entry_end = entry->va + entry->size;
        if (entry->owner_pid != owner_pid ||
            !vm_track_entry_committed(entry) || entry_end < entry->va ||
            va >= entry_end || entry->va >= end)
            continue;
        if (entry->section)
            return -1;
        if (entry->va < va && entry_end > end)
            extra_entries++;
    }
    if (vm_track_count + extra_entries > VM_TRACK_MAX)
        return 0;

    int original_count = vm_track_count;
    for (int i = original_count - 1; i >= 0; i--) {
        vm_track_entry_t original = vm_track[i];
        uint64_t entry_end = original.va + original.size;
        if (original.owner_pid != owner_pid ||
            !vm_track_entry_committed(&original) || entry_end < original.va ||
            va >= entry_end || original.va >= end)
            continue;

        uint64_t cut_start = original.va > va ? original.va : va;
        uint64_t cut_end = entry_end < end ? entry_end : end;
        vm_track_unmap_range(&original, cut_start, cut_end);

        BOOL keep_left = original.va < cut_start;
        BOOL keep_right = cut_end < entry_end;
        if (keep_left) {
            vm_track[i].size = cut_start - original.va;
            if (keep_right) {
                vm_track_entry_t right = original;
                right.va = cut_end;
                right.phys = vm_track_page_phys(&original, cut_end);
                right.size = entry_end - cut_end;
                vm_track[vm_track_count++] = right;
            }
        } else if (keep_right) {
            vm_track[i].va = cut_end;
            vm_track[i].phys = vm_track_page_phys(&original, cut_end);
            vm_track[i].size = entry_end - cut_end;
        } else {
            vm_track[i] = vm_track[--vm_track_count];
        }
    }
    return 1;
}

void nt_vm_release_process(ULONG owner_pid)
{
    if (!owner_pid) return;

    /* Individual thread stacks are reservations in this same owner table.
     * Transfer their pending cleanup before removing any entry so the task
     * reaper cannot race process teardown and report a false double free. */
    kernel32_prepare_process_vm_release(owner_pid);

    /* The CRT caches pointers into this process's user mappings. Remove those
     * references before the mappings themselves are torn down. */
    dinput8_release_process(owner_pid);
    msvcrt_release_process(owner_pid);

    uint64_t released_pages = 0;
    uint32_t released_entries = 0;

    for (;;) {
        vm_track_entry_t entry = {0};
        BOOL found = FALSE;
        uint64_t vm_irq_flags = vm_track_lock_irqsave();
        for (int i = 0; i < vm_track_count; i++) {
            if (vm_track[i].owner_pid != owner_pid)
                continue;
            entry = vm_track[i];
            vm_track[i] = vm_track[--vm_track_count];
            found = TRUE;
            break;
        }
        vm_track_unlock_irqrestore(vm_irq_flags);
        if (!found)
            break;

        released_entries++;
        if (!vm_track_entry_committed(&entry)) continue;

        SIZE_T pages = (entry.size + 4095) / 4096;
        if (entry.section) {
            NTSTATUS flush_status = section_flush_mapping(
                entry.section, entry.phys, entry.size, entry.protect,
                entry.section_offset, 0, 0);
            if (!NT_SUCCESS(flush_status))
                nt_log_hex("mapped-file flush failed, status=", flush_status);
        }
        vm_track_unmap_range(&entry, entry.va, entry.va + entry.size);
        if (entry.section)
            section_view_release(entry.section);
        released_pages += pages;
    }

    uint64_t vm_irq_flags = vm_track_lock_irqsave();
    for (int i = 0; i < VM_PROCESS_STATE_MAX; i++) {
        if (vm_process_states[i].used &&
            vm_process_states[i].owner_pid == owner_pid) {
            vm_process_states[i].used = FALSE;
            break;
        }
    }
    vm_track_unlock_irqrestore(vm_irq_flags);

    if (released_entries) {
        serial_puts("[VM-TRACK] released owner=0x");
        serial_puthex(owner_pid, 8);
        serial_puts(" entries=0x");
        serial_puthex(released_entries, 8);
        serial_puts(" pages=0x");
        serial_puthex(released_pages, 16);
        serial_puts("\n");
    }
}

void nt_vm_get_stats(uint32_t *entries, uint64_t *private_pages,
                     uint64_t *mapped_pages)
{
    uint32_t entry_count = 0;
    uint64_t private_count = 0;
    uint64_t mapped_count = 0;
    uint64_t vm_irq_flags = vm_track_lock_irqsave();

    for (int i = 0; i < vm_track_count; i++) {
        const vm_track_entry_t *entry = &vm_track[i];
        if (!vm_track_entry_committed(entry))
            continue;

        uint64_t pages = (entry->size + 4095) / 4096;
        entry_count++;
        mapped_count += pages;
        if (entry->owns_phys && !entry->section)
            private_count += pages;
    }

    vm_track_unlock_irqrestore(vm_irq_flags);
    if (entries) *entries = entry_count;
    if (private_pages) *private_pages = private_count;
    if (mapped_pages) *mapped_pages = mapped_count;
}

static int vm_track_contains(uint64_t va, SIZE_T size)
{
    ULONG owner_pid = nt_current_owner_pid();
    uint64_t end = va + size;
    if (end < va) return 0;

    /* ponytail: entries are unsorted; rescan until contiguous coverage ends. */
    uint64_t covered = va;
    while (covered < end) {
        uint64_t next = covered;
        for (int i = 0; i < vm_track_count; i++) {
            if (vm_track[i].owner_pid != owner_pid) continue;
            uint64_t track_end = vm_track[i].va + vm_track[i].size;
            if (track_end < vm_track[i].va) continue;
            if (vm_track_entry_committed(&vm_track[i]) &&
                vm_track[i].va <= covered &&
                track_end > next)
                next = track_end;
        }
        if (next == covered) return 0;
        covered = next;
    }
    return 1;
}

static int vm_track_overlaps(uint64_t va, SIZE_T size)
{
    ULONG owner_pid = nt_current_owner_pid();
    uint64_t end = va + size;
    if (end < va) return 1;

    for (int i = 0; i < vm_track_count; i++) {
        if (vm_track[i].owner_pid != owner_pid) continue;
        uint64_t track_end = vm_track[i].va + vm_track[i].size;
        if (va < track_end && vm_track[i].va < end)
            return 1;
    }
    return 0;
}

ULONGLONG nt_vm_range_conflict_end(ULONGLONG va, ULONGLONG size)
{
    uint64_t end = va + size;
    if (!size || end < va)
        return UINT64_MAX;

    ULONG owner_pid = nt_current_owner_pid();
    uint64_t owner_cr3 = nt_current_cr3();
    uint64_t conflict_end = 0;
    uint64_t vm_irq_flags = vm_track_lock_irqsave();
    for (int i = 0; i < vm_track_count; i++) {
        uint64_t entry_end = vm_track[i].va + vm_track[i].size;
        if (vm_track[i].owner_pid != owner_pid ||
            vm_track[i].cr3 != owner_cr3 ||
            entry_end < vm_track[i].va || va >= entry_end ||
            vm_track[i].va >= end)
            continue;
        if (entry_end > conflict_end)
            conflict_end = entry_end;
    }
    vm_track_unlock_irqrestore(vm_irq_flags);
    return conflict_end;
}

/* Return the end of the first owner-local VMA or PE image intersecting the
 * candidate range. Must hold vm_track_lock. */
static uint64_t vm_range_conflict_end_locked(uint64_t va, SIZE_T size,
                                             int compat32)
{
    ULONG owner_pid = nt_current_owner_pid();
    uint64_t end = va + size;
    uint64_t conflict_end = 0;
    if (end < va)
        return UINT64_MAX;

    for (int i = 0; i < vm_track_count; i++) {
        uint64_t entry_end = vm_track[i].va + vm_track[i].size;
        if (vm_track[i].owner_pid == owner_pid &&
            entry_end >= vm_track[i].va && va < entry_end &&
            vm_track[i].va < end && entry_end > conflict_end)
            conflict_end = entry_end;
    }

    extern uint64_t syscall_vma_range_conflict_end(uint64_t base,
                                                    uint64_t size);
    uint64_t mmap_end = syscall_vma_range_conflict_end(va, size);
    if (mmap_end == UINT64_MAX)
        return UINT64_MAX;
    if (mmap_end > conflict_end)
        conflict_end = mmap_end;

    uint64_t image_base = 0;
    uint64_t image_size = 0;
    uint64_t next_image = 0;
    if (pe_va_query_range(va, &image_base, &image_size, &next_image)) {
        uint64_t image_end = image_base +
                             ((image_size + 0xFFFULL) & ~0xFFFULL);
        if (image_end > conflict_end)
            conflict_end = image_end;
    } else if (next_image && next_image < end &&
               pe_va_query_range(next_image, &image_base, &image_size,
                                 &next_image)) {
        uint64_t image_end = image_base +
                             ((image_size + 0xFFFULL) & ~0xFFFULL);
        if (image_end > conflict_end)
            conflict_end = image_end;
    }

    uint64_t cr3 = nt_current_cr3();
    /* The shell-hosted PE32 main process still runs on the kernel CR3, whose
     * lower half identity-maps RAM. Win32 allocations intentionally replace
     * pages in its dedicated 0x40000000..0x78000000 window, so those inherited
     * identity PTEs are not user-space conflicts. Owner VMAs and PE ranges
     * above remain authoritative. Private process CR3s have no identity map
     * and must continue checking their actual page tables. */
    if (!(compat32 && cr3 == paging_get_kernel_cr3())) {
        uint64_t mapped_end = paging_first_mapped_end_in_cr3(cr3, va, size);
        if (mapped_end == UINT64_MAX)
            return UINT64_MAX;
        if (mapped_end > conflict_end)
            conflict_end = mapped_end;
    }
    return conflict_end;
}

static BOOL vm_address_allowed(uint64_t va, SIZE_T size, BOOL requested,
                               int compat32)
{
    uint64_t end = va + size;
    if (end < va)
        return FALSE;

    if (compat32)
        return va >= WIN32_VA_BASE && end <= WIN32_VA_LIMIT;
    if (requested)
        return va >= WIN64_FIXED_VA_BASE && end <= WIN64_FIXED_VA_LIMIT;
    return va >= WIN64_VA_BASE && end <= WIN64_VA_LIMIT;
}

/* Choose a 64 KB allocation-granularity address. Automatic allocations skip
 * occupied ranges instead of reporting false OOM at the first collision. */
static uint64_t vm_choose_address_locked(SIZE_T size, uint64_t requested_va,
                                         int compat32)
{
    uint64_t *auto_next = vm_auto_next_locked(nt_current_owner_pid(),
                                               compat32);
    if (!auto_next)
        return 0;

    uint64_t va = requested_va
                ? requested_va & ~0xFFFULL
                : (*auto_next + 0xFFFFULL) & ~0xFFFFULL;
    for (;;) {
        if (!vm_address_allowed(va, size, requested_va != 0, compat32))
            return 0;

        uint64_t conflict_end = vm_range_conflict_end_locked(va, size,
                                                              compat32);
        if (!conflict_end)
            return va;
        if (requested_va || conflict_end == UINT64_MAX ||
            conflict_end > UINT64_MAX - 0xFFFFULL)
            return 0;
        va = (conflict_end + 0xFFFFULL) & ~0xFFFFULL;
    }
}

/* Forward declaration (defined below with other nt_* helpers) */
static inline void nt_memset(void *s, int c, SIZE_T n);
static uint64_t nt_prot_to_page_flags(ULONG protect);

/* Allocate zeroed physical backing and install it atomically in one address
 * space. The final PTE flags reflect the requested protection. */
static int vm_map_private_backing(uint64_t cr3, uint64_t va, SIZE_T size,
                                  ULONG protect, uint64_t *out_phys)
{
    uint64_t pages = size / 4096;
    void *phys = mem_alloc_pages(pages);
    if (!phys)
        return 0;

    nt_memset(PHYS_TO_VIRT(phys), 0, size);
    uint64_t pa = (uint64_t)(uintptr_t)phys;
    uint64_t final_flags = nt_prot_to_page_flags(protect);
    uint64_t mapped_pages = 0;

    while (mapped_pages < pages) {
        uint64_t page_va = va + mapped_pages * 4096;
        if (nt_map_page_in(cr3, page_va, pa + mapped_pages * 4096,
                           PTE_PRESENT | PTE_WRITABLE) != 0)
            break;
        mapped_pages++;
        if (final_flags != (PTE_PRESENT | PTE_WRITABLE) &&
            nt_set_page_flags_in(cr3, page_va, final_flags) != 0)
            break;
    }

    if (mapped_pages != pages) {
        while (mapped_pages)
            nt_unmap_page_in(cr3, va + --mapped_pages * 4096);
        mem_free_pages(phys, pages);
        return 0;
    }

    if (out_phys)
        *out_phys = pa;
    return 1;
}

/* Allocate VA range and map physical pages into it */
static PVOID win32_va_alloc(SIZE_T size, uint64_t *out_phys, ULONG protect,
                            uint64_t requested_va, int reserve_only,
                            int compat32)
{
    ULONG owner_pid = nt_current_owner_pid();
    uint64_t cr3 = nt_current_cr3();
    uint64_t pages = size / 4096;
    if (pages == 0) return NULL;

    /* Free-list recycling DISABLED (regression investigation 2026-05-15).
     * GMalloc state logging confirmed GMalloc is INTACT across the entire
     * run (525+ calls, no CHANGED events after init).  Tombstone +
     * STALE-PTR detector saw 0 hits.  Yet engine still crashes at
     * vec=14 NX-fault RIP=0x401BC870 → engine treats DATA as code at a
     * freshly-allocated NX page.  Root cause not pinpointed; engine
     * derives a function pointer from somewhere that points to a
     * data buffer.  Cannot fix without engine source.
     *
     * Keep the free-list scaffolding so future investigations can
     * re-enable trivially.  See project_ut99_thunks_at_13d5.md and
     * project_ut99_stale_ptr_falsified.md for the trail. */
    uint64_t *auto_next = vm_auto_next_locked(owner_pid, compat32);
    uint64_t va = vm_choose_address_locked(size, requested_va, compat32);
    if (!auto_next || !va) {
        static uint32_t auto_reject_logs;
        static uint32_t fixed_reject_logs;
        uint32_t *reject_logs = requested_va ? &fixed_reject_logs
                                             : &auto_reject_logs;
        if ((*reject_logs)++ < 16) {
            serial_puts("[VA-REJECT] requested=0x");
            serial_puthex(requested_va, 16);
            serial_puts(compat32 ? " next32=0x" : " next64=0x");
            serial_puthex(auto_next ? *auto_next : 0, 16);
            serial_puts("\n");
        }
        return NULL;
    }
    if (reserve_only) {
        if (out_phys) *out_phys = 0;
        return (PVOID)va;
    }

    uint64_t pa = 0;
    if (!vm_map_private_backing(cr3, va, size, protect, &pa)) {
        serial_puts("[VA-PHYS-FAIL] pages=0x");
        serial_puthex(pages, 16);
        serial_puts(" size=0x");
        serial_puthex(size, 16);
        serial_puts("\n");
        return NULL;
    }

    /* VA-ALLOC trace — every fresh allocation. Caller EIP comes from
     * compat32's saved per-thunk return address (compat32_get_last_caller_eip)
     * — that's the PE32 instruction immediately after the VirtualAlloc
     * INT 0x2E thunk return. Cross-reference with FMW-ASSERT logs to
     * identify which alloc became a Pool->Mem that later failed the
     * pool-integrity check. */
#if defined(WIN32_VM_TRACE) && WIN32_VM_TRACE
    {
        extern uint32_t compat32_get_last_caller_eip(void);
        uint32_t ceip = compat32_get_last_caller_eip();
        serial_puts("[VA-ALLOC] base=0x");
        serial_puthex((uint64_t)(uintptr_t)va, 8);
        serial_puts(" size=0x");
        serial_puthex((uint64_t)size, 8);
        serial_puts(" prot=0x");
        serial_puthex((uint64_t)protect, 8);
        serial_puts(" caller=0x");
        serial_puthex((uint64_t)ceip, 8);
        serial_puts("\n");
    }
#endif

    if (out_phys) *out_phys = pa;
    return (PVOID)va;
}

/* ── Per-process state ──────────────────────────────────────── */

HANDLE_TABLE g_handle_table;
static BOOL  g_initialized = FALSE;

/* Standard console handles (pre-allocated) */
static FILE_OBJECT  g_console_in;
static FILE_OBJECT  g_console_out;
static FILE_OBJECT  g_console_err;

#define NT_FILE_OBJECT_POOL_SIZE 512
static FILE_OBJECT g_file_pool[NT_FILE_OBJECT_POOL_SIZE];
static spinlock_t g_file_pool_lock = SPINLOCK_INIT;

typedef struct {
    pipe_buf_t a_to_b;
    pipe_buf_t b_to_a;
    FILE_OBJECT end_a;
    FILE_OBJECT end_b;
    BOOL in_use;
    BOOL duplex;
} WIN32_PIPE_OBJECT;

#define MAX_WIN32_PIPES 64

static WIN32_PIPE_OBJECT g_win32_pipes[MAX_WIN32_PIPES];
/* One lock covers the small Win32 pipe pool; split it only if contention appears. */
static spinlock_t g_win32_pipe_lock = SPINLOCK_INIT;

#define STD_INPUT_HANDLE_VALUE   INDEX_TO_HANDLE(1)
#define STD_OUTPUT_HANDLE_VALUE  INDEX_TO_HANDLE(2)
#define STD_ERROR_HANDLE_VALUE   INDEX_TO_HANDLE(3)

/* ── Helpers ────────────────────────────────────────────────── */

static void nt_log(const char *msg)
{
    serial_puts("[NT] ");
    serial_puts(msg);
    serial_puts("\n");
}

static void nt_log_hex(const char *prefix, ULONGLONG val)
{
    serial_puts("[NT] ");
    serial_puts(prefix);
    serial_puthex(val, 16);
    serial_puts("\n");
}

static inline void nt_memcpy(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--) *d++ = *s++;
}

static inline void nt_memset(void *s, int c, SIZE_T n)
{
    BYTE *p = (BYTE *)s;
    while (n--) *p++ = (BYTE)c;
}

static FILE_OBJECT *nt_file_object_allocate(ULONG flags)
{
    FILE_OBJECT *object = NULL;
    uint32_t used = 0;
    spin_lock(&g_file_pool_lock);
    for (int i = 0; i < NT_FILE_OBJECT_POOL_SIZE; i++) {
        if (!g_file_pool[i].flags) {
            object = &g_file_pool[i];
            nt_memset(object, 0, sizeof(*object));
            object->flags = flags;
            break;
        }
        used++;
    }
    spin_unlock(&g_file_pool_lock);
    if (!object) {
        static uint32_t exhaustion_logs;
        uint32_t index = __atomic_fetch_add(&exhaustion_logs, 1,
                                             __ATOMIC_RELAXED);
        if (index < 16) {
            serial_puts("[NT-FILE-POOL] exhausted used=");
            serial_putdec(used);
            serial_puts(" requested_flags=0x");
            serial_puthex(flags, 8);
            serial_puts("\n");
        }
    }
    return object;
}

static void nt_file_object_release(FILE_OBJECT *object)
{
    if (!object) return;
    spin_lock(&g_file_pool_lock);
    object->flags = 0;
    object->osfs_file = NULL;
    spin_unlock(&g_file_pool_lock);
}

static void nt_pipe_buf_init(pipe_buf_t *pipe)
{
    pipe->in_use = true;
    pipe->read_refs = 1;
    pipe->write_refs = 1;
}

static NTSTATUS nt_pipe_create_pair(PHANDLE first_handle,
                                    PHANDLE second_handle, BOOL duplex)
{
    if (!first_handle || !second_handle)
        return STATUS_INVALID_PARAMETER;
    *first_handle = NULL;
    *second_handle = NULL;

    WIN32_PIPE_OBJECT *obj = NULL;
    spin_lock(&g_win32_pipe_lock);
    for (int i = 0; i < MAX_WIN32_PIPES; i++) {
        if (!g_win32_pipes[i].in_use) {
            obj = &g_win32_pipes[i];
            nt_memset(obj, 0, sizeof(*obj));
            obj->in_use = TRUE;
            obj->duplex = duplex;
            nt_pipe_buf_init(&obj->a_to_b);
            if (duplex)
                nt_pipe_buf_init(&obj->b_to_a);
            obj->end_a.flags = FILE_OBJ_PIPE_WRITE | FILE_OBJ_PIPE_SIDE_A;
            obj->end_b.flags = FILE_OBJ_PIPE_READ | FILE_OBJ_PIPE_SIDE_B;
            if (duplex) {
                obj->end_a.flags |= FILE_OBJ_PIPE_READ;
                obj->end_b.flags |= FILE_OBJ_PIPE_WRITE;
            }
            obj->end_a.osfs_file = obj;
            obj->end_b.osfs_file = obj;
            break;
        }
    }
    spin_unlock(&g_win32_pipe_lock);
    if (!obj) {
        extern DWORD win32_current_process_id(void);
        serial_puts("[NT-PIPE-FAIL] table full pid=");
        serial_putdec(win32_current_process_id());
        serial_puts("\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    FILE_OBJECT *first = duplex ? &obj->end_a : &obj->end_b;
    FILE_OBJECT *second = duplex ? &obj->end_b : &obj->end_a;
    ACCESS_MASK first_access = duplex ? GENERIC_READ | GENERIC_WRITE
                                      : GENERIC_READ;
    ACCESS_MASK second_access = duplex ? GENERIC_READ | GENERIC_WRITE
                                       : GENERIC_WRITE;
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_FILE,
                                   first_access, first, first_handle);
    if (NT_SUCCESS(status))
        status = handle_alloc(&g_handle_table, OBJ_TYPE_FILE, second_access,
                              second, second_handle);
    if (!NT_SUCCESS(status)) {
        extern DWORD win32_current_process_id(void);
        serial_puts("[NT-PIPE-FAIL] handle alloc pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" status=0x");
        serial_puthex((uint32_t)status, 8);
        serial_puts("\n");
        if (*first_handle)
            handle_close(&g_handle_table, *first_handle);
        spin_lock(&g_win32_pipe_lock);
        nt_memset(obj, 0, sizeof(*obj));
        spin_unlock(&g_win32_pipe_lock);
    }
    return status;
}

NTSTATUS nt_pipe_create(PHANDLE read_handle, PHANDLE write_handle)
{
    return nt_pipe_create_pair(read_handle, write_handle, FALSE);
}

NTSTATUS nt_pipe_create_duplex(PHANDLE server_handle, PHANDLE client_handle)
{
    return nt_pipe_create_pair(server_handle, client_handle, TRUE);
}

static pipe_buf_t *nt_pipe_read_buffer(WIN32_PIPE_OBJECT *obj,
                                       PFILE_OBJECT end)
{
    if (end->flags & FILE_OBJ_PIPE_SIDE_A)
        return &obj->b_to_a;
    if (end->flags & FILE_OBJ_PIPE_SIDE_B)
        return &obj->a_to_b;
    return NULL;
}

static pipe_buf_t *nt_pipe_write_buffer(WIN32_PIPE_OBJECT *obj,
                                        PFILE_OBJECT end)
{
    if (end->flags & FILE_OBJ_PIPE_SIDE_A)
        return &obj->a_to_b;
    if (end->flags & FILE_OBJ_PIPE_SIDE_B)
        return &obj->b_to_a;
    return NULL;
}

PVOID nt_pipe_stream_identity(HANDLE handle, ULONG owner_pid, BOOL write)
{
    FILE_OBJECT *end = NULL;
    NTSTATUS status = handle_lookup_for_process(
        &g_handle_table, handle, owner_pid, OBJ_TYPE_FILE, (PVOID *)&end);
    ULONG required_flag = write ? FILE_OBJ_PIPE_WRITE : FILE_OBJ_PIPE_READ;
    if (!NT_SUCCESS(status) || !end || !(end->flags & required_flag))
        return NULL;

    WIN32_PIPE_OBJECT *obj = (WIN32_PIPE_OBJECT *)end->osfs_file;
    pipe_buf_t *pipe = obj ? (write ? nt_pipe_write_buffer(obj, end)
                                    : nt_pipe_read_buffer(obj, end))
                           : NULL;
    PVOID identity = NULL;
    spin_lock(&g_win32_pipe_lock);
    if (obj && obj->in_use && pipe && pipe->in_use)
        identity = pipe;
    spin_unlock(&g_win32_pipe_lock);
    return identity;
}

static NTSTATUS nt_pipe_read(PFILE_OBJECT end, PVOID buffer, ULONG length,
                             PIO_STATUS_BLOCK iosb)
{
    WIN32_PIPE_OBJECT *obj = (WIN32_PIPE_OBJECT *)end->osfs_file;
    pipe_buf_t *pipe = obj ? nt_pipe_read_buffer(obj, end) : NULL;
    for (;;) {
        spin_lock(&g_win32_pipe_lock);
        if (!obj || !obj->in_use || !pipe || !pipe->in_use ||
            !(end->flags & FILE_OBJ_PIPE_READ)) {
            spin_unlock(&g_win32_pipe_lock);
            return STATUS_INVALID_HANDLE;
        }
        if (pipe->count) {
            ULONG count = length < pipe->count ? length : pipe->count;
            BYTE *dst = (BYTE *)buffer;
            for (ULONG i = 0; i < count; i++) {
                dst[i] = pipe->buf[pipe->tail];
                pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
            }
            pipe->count -= count;
            spin_unlock(&g_win32_pipe_lock);
            if (iosb) {
                iosb->Status = STATUS_SUCCESS;
                iosb->Information = count;
            }
            return STATUS_SUCCESS;
        }
        BOOL eof = pipe->write_refs == 0;
        spin_unlock(&g_win32_pipe_lock);
        if (eof) {
            if (iosb) {
                iosb->Status = STATUS_END_OF_FILE;
                iosb->Information = 0;
            }
            return STATUS_END_OF_FILE;
        }
        sched_yield();
    }
}

NTSTATUS nt_pipe_try_read(HANDLE handle, PVOID buffer, ULONG length,
                          ULONG owner_pid, PIO_STATUS_BLOCK iosb)
{
    FILE_OBJECT *end = NULL;
    NTSTATUS status = handle_lookup_for_process(
        &g_handle_table, handle, owner_pid, OBJ_TYPE_FILE, (PVOID *)&end);
    if (!NT_SUCCESS(status) || !end || !(end->flags & FILE_OBJ_PIPE_READ))
        return STATUS_INVALID_HANDLE;

    WIN32_PIPE_OBJECT *obj = (WIN32_PIPE_OBJECT *)end->osfs_file;
    pipe_buf_t *pipe = obj ? nt_pipe_read_buffer(obj, end) : NULL;
    spin_lock(&g_win32_pipe_lock);
    if (!obj || !obj->in_use || !pipe || !pipe->in_use) {
        spin_unlock(&g_win32_pipe_lock);
        return STATUS_INVALID_HANDLE;
    }
    if (!pipe->count) {
        BOOL eof = pipe->write_refs == 0;
        spin_unlock(&g_win32_pipe_lock);
        if (iosb) {
            iosb->Status = eof ? STATUS_END_OF_FILE : STATUS_PENDING;
            iosb->Information = 0;
        }
        return eof ? STATUS_END_OF_FILE : STATUS_PENDING;
    }

    ULONG count = length < pipe->count ? length : pipe->count;
    BYTE *dst = (BYTE *)buffer;
    for (ULONG i = 0; i < count; i++) {
        dst[i] = pipe->buf[pipe->tail];
        pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
    }
    pipe->count -= count;
    spin_unlock(&g_win32_pipe_lock);
    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = count;
    }
    return STATUS_SUCCESS;
}

NTSTATUS nt_pipe_try_write(HANDLE handle, PCVOID buffer, ULONG length,
                           ULONG owner_pid, PIO_STATUS_BLOCK iosb)
{
    FILE_OBJECT *end = NULL;
    NTSTATUS status = handle_lookup_for_process(
        &g_handle_table, handle, owner_pid, OBJ_TYPE_FILE, (PVOID *)&end);
    if (!NT_SUCCESS(status) || !end || !(end->flags & FILE_OBJ_PIPE_WRITE))
        return STATUS_INVALID_HANDLE;

    WIN32_PIPE_OBJECT *obj = (WIN32_PIPE_OBJECT *)end->osfs_file;
    pipe_buf_t *pipe = obj ? nt_pipe_write_buffer(obj, end) : NULL;
    spin_lock(&g_win32_pipe_lock);
    if (!obj || !obj->in_use || !pipe || !pipe->in_use ||
        pipe->read_refs == 0) {
        spin_unlock(&g_win32_pipe_lock);
        return STATUS_INVALID_HANDLE;
    }

    ULONG space = PIPE_BUF_SIZE - pipe->count;
    ULONG count = length < space ? length : space;
    const BYTE *src = (const BYTE *)buffer;
    for (ULONG i = 0; i < count; i++) {
        pipe->buf[pipe->head] = src[i];
        pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
    }
    pipe->count += count;
    spin_unlock(&g_win32_pipe_lock);

    status = count == length ? STATUS_SUCCESS : STATUS_PENDING;
    if (iosb) {
        iosb->Status = status;
        iosb->Information = count;
    }
    return status;
}

static NTSTATUS nt_pipe_write(PFILE_OBJECT end, PCVOID buffer, ULONG length,
                              PIO_STATUS_BLOCK iosb)
{
    WIN32_PIPE_OBJECT *obj = (WIN32_PIPE_OBJECT *)end->osfs_file;
    pipe_buf_t *pipe = obj ? nt_pipe_write_buffer(obj, end) : NULL;
    ULONG written = 0;
    while (written < length) {
        spin_lock(&g_win32_pipe_lock);
        if (!obj || !obj->in_use || !pipe || !pipe->in_use ||
            !(end->flags & FILE_OBJ_PIPE_WRITE) || pipe->read_refs == 0) {
            spin_unlock(&g_win32_pipe_lock);
            return STATUS_INVALID_HANDLE;
        }
        ULONG space = PIPE_BUF_SIZE - pipe->count;
        if (space) {
            ULONG count = length - written;
            if (count > space) count = space;
            const BYTE *src = (const BYTE *)buffer;
            for (ULONG i = 0; i < count; i++) {
                pipe->buf[pipe->head] = src[written + i];
                pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
            }
            pipe->count += count;
            written += count;
        }
        spin_unlock(&g_win32_pipe_lock);
        if (space)
            k32_pipe_service_pending();
        if (!space)
            sched_yield();
    }
    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = written;
    }
    return STATUS_SUCCESS;
}

NTSTATUS nt_pipe_peek(HANDLE handle, PVOID buffer, ULONG length,
                      ULONG *bytes_read, ULONG *bytes_available,
                      ULONG *bytes_left)
{
    FILE_OBJECT *end = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, handle, OBJ_TYPE_FILE,
                                    (PVOID *)&end);
    if (!NT_SUCCESS(status) || !(end->flags & FILE_OBJ_PIPE_READ))
        return STATUS_INVALID_HANDLE;

    WIN32_PIPE_OBJECT *obj = (WIN32_PIPE_OBJECT *)end->osfs_file;
    if (!obj)
        return STATUS_INVALID_HANDLE;
    pipe_buf_t *pipe = nt_pipe_read_buffer(obj, end);
    spin_lock(&g_win32_pipe_lock);
    if (!obj->in_use || !pipe || !pipe->in_use) {
        spin_unlock(&g_win32_pipe_lock);
        return STATUS_INVALID_HANDLE;
    }
    ULONG available = pipe->count;
    ULONG count = length < available ? length : available;
    BYTE *dst = (BYTE *)buffer;
    ULONG pos = pipe->tail;
    for (ULONG i = 0; dst && i < count; i++) {
        dst[i] = pipe->buf[pos];
        pos = (pos + 1) % PIPE_BUF_SIZE;
    }
    spin_unlock(&g_win32_pipe_lock);

    if (bytes_read) *bytes_read = buffer ? count : 0;
    if (bytes_available) *bytes_available = available;
    if (bytes_left) *bytes_left = 0; /* anonymous pipes are byte streams */
    return STATUS_SUCCESS;
}

static void nt_pipe_release(PFILE_OBJECT end)
{
    WIN32_PIPE_OBJECT *obj = (WIN32_PIPE_OBJECT *)end->osfs_file;
    spin_lock(&g_win32_pipe_lock);
    pipe_buf_t *read_pipe = nt_pipe_read_buffer(obj, end);
    pipe_buf_t *write_pipe = nt_pipe_write_buffer(obj, end);
    if ((end->flags & FILE_OBJ_PIPE_READ) && read_pipe)
        read_pipe->read_refs = 0;
    if ((end->flags & FILE_OBJ_PIPE_WRITE) && write_pipe)
        write_pipe->write_refs = 0;
    end->flags = 0;
    end->osfs_file = NULL;
    if (obj->a_to_b.read_refs == 0 && obj->a_to_b.write_refs == 0 &&
        obj->b_to_a.read_refs == 0 && obj->b_to_a.write_refs == 0)
        nt_memset(obj, 0, sizeof(*obj));
    spin_unlock(&g_win32_pipe_lock);
}

/* Convert UTF-16LE UNICODE_STRING to ASCII (simple — strips high byte).
 * Returns number of chars written (not counting NUL). */
static int unicode_to_ascii(PCUNICODE_STRING us, char *buf, int buf_size)
{
    if (!us || !us->Buffer || us->Length == 0) {
        if (buf_size > 0) buf[0] = '\0';
        return 0;
    }

    int chars = us->Length / sizeof(WCHAR);
    if (chars >= buf_size) chars = buf_size - 1;

    for (int i = 0; i < chars; i++)
        buf[i] = (char)(us->Buffer[i] & 0xFF);

    buf[chars] = '\0';
    return chars;
}

extern void *osfs2_find_ci(const char *name);
extern bool osfs3_is_mounted(void);

static bool nt_object_name_is_absolute(const char *path)
{
    const char *p = path;
    if (p[0] == '\\' && p[1] == '?' && p[2] == '?' && p[3] == '\\')
        p += 4;
    else if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\')
        p += 4;
    return p[0] == '\\' || p[0] == '/' ||
           (p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/'));
}

static NTSTATUS nt_resolve_object_path(POBJECT_ATTRIBUTES attributes,
                                       char path[260])
{
    char raw[260];
    if (!attributes || !attributes->ObjectName)
        return STATUS_INVALID_PARAMETER;
    if (attributes->ObjectName->Length >= sizeof(raw) * sizeof(WCHAR))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    unicode_to_ascii(attributes->ObjectName, raw, sizeof(raw));

    if (attributes->RootDirectory && !nt_object_name_is_absolute(raw)) {
        FILE_OBJECT *root = NULL;
        NTSTATUS status = handle_lookup(&g_handle_table,
                                        attributes->RootDirectory,
                                        OBJ_TYPE_FILE, (PVOID *)&root);
        if (!NT_SUCCESS(status) || !root ||
            !(root->flags & FILE_OBJ_DIRECTORY))
            return NT_SUCCESS(status) ? STATUS_INVALID_HANDLE : status;

        char joined[524];
        int pos = 0;
        joined[pos++] = 'C';
        joined[pos++] = ':';
        joined[pos++] = '\\';
        for (int i = 0; root->name[i] && pos < (int)sizeof(joined) - 1; i++)
            joined[pos++] = (char)(root->name[i] & 0xFF);
        if (pos > 3 && joined[pos - 1] != '\\' && joined[pos - 1] != '/')
            joined[pos++] = '\\';
        for (int i = 0; raw[i] && pos < (int)sizeof(joined) - 1; i++)
            joined[pos++] = raw[i];
        if (raw[0] && pos == (int)sizeof(joined) - 1)
            return STATUS_OBJECT_PATH_NOT_FOUND;
        joined[pos] = 0;
        return win32_normalize_path(joined, path)
            ? STATUS_SUCCESS : STATUS_OBJECT_PATH_NOT_FOUND;
    }

    const char *resolved = vfs_resolve(raw, VFS_MODE_WIN32);
    if (!resolved) return STATUS_OBJECT_PATH_NOT_FOUND;
    while (*resolved == '\\' || *resolved == '/') resolved++;

    char absolute[524];
    int pos = 0;
    absolute[pos++] = 'C';
    absolute[pos++] = ':';
    absolute[pos++] = '\\';
    int i = 0;
    while (resolved[i] && pos < (int)sizeof(absolute) - 1)
        absolute[pos++] = resolved[i++];
    if (resolved[i]) return STATUS_OBJECT_PATH_NOT_FOUND;
    absolute[pos] = 0;
    return win32_normalize_path(absolute, path)
        ? STATUS_SUCCESS : STATUS_OBJECT_PATH_NOT_FOUND;
}

static bool nt_path_equal_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return false;
    }
    return *a == *b;
}

static bool nt_path_contains_ci(const char *path, const char *needle)
{
    if (!path || !needle || !*needle) return false;
    for (; *path; path++) {
        const char *a = path;
        const char *b = needle;
        while (*a && *b) {
            char ca = *a++, cb = *b++;
            if (ca == '/') ca = '\\';
            if (cb == '/') cb = '\\';
            if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
            if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
            if (ca != cb) break;
        }
        if (!*b) return true;
    }
    return false;
}

static bool nt_is_cef_cache_path(const char *path)
{
    return nt_path_contains_ci(path, "httpcache") ||
           nt_path_contains_ci(path, "htmlcache");
}

static void nt_trace_cache_create(uint32_t id, const char *stage,
                                  const char *path, ACCESS_MASK access,
                                  ULONG disposition, ULONG options,
                                  NTSTATUS status, bool existed)
{
#if defined(OK_QUIET) && OK_QUIET
    (void)id;
    (void)stage;
    (void)path;
    (void)access;
    (void)disposition;
    (void)options;
    (void)status;
    (void)existed;
#else
    serial_puts("[NT-CACHE-CREATE] id=");
    serial_putdec(id);
    serial_puts(" stage=");
    serial_puts(stage);
    serial_puts(" status=0x");
    serial_puthex((uint64_t)(uint32_t)status, 8);
    serial_puts(" existed=");
    serial_putdec(existed);
    serial_puts(" access=0x");
    serial_puthex(access, 8);
    serial_puts(" disp=0x");
    serial_puthex(disposition, 2);
    serial_puts(" opts=0x");
    serial_puthex(options, 8);
    serial_puts(" path='");
    serial_puts(path ? path : "<null>");
    serial_puts("'\n");
#endif
}

static void nt_log_create_failure(const char *path, NTSTATUS status,
                                  ULONG disposition, ULONG options,
                                  HANDLE root)
{
    static uint32_t count;
    if (count < 32) {
        serial_puts("[NT-CREATE-FAIL] path='");
        serial_puts(path ? path : "<null>");
        serial_puts("' status=0x");
        serial_puthex((uint64_t)(uint32_t)status, 8);
        serial_puts(" disp=0x");
        serial_puthex(disposition, 2);
        serial_puts(" opts=0x");
        serial_puthex(options, 8);
        serial_puts(" root=0x");
        serial_puthex((uint64_t)(ULONG_PTR)root, 8);
        serial_puts("\n");
    } else if (count == 32) {
        serial_puts("[NT-CREATE-FAIL] further failures suppressed\n");
    }
    count++;
}

/* ── NtCreateFile ───────────────────────────────────────────── */

NTSTATUS sys_NtCreateFile(ULONG_PTR *args)
{
    PHANDLE             FileHandle       = (PHANDLE)args[0];
    ACCESS_MASK         DesiredAccess    = (ACCESS_MASK)args[1];
    POBJECT_ATTRIBUTES  ObjectAttributes = (POBJECT_ATTRIBUTES)args[2];
    PIO_STATUS_BLOCK    IoStatusBlock    = (PIO_STATUS_BLOCK)args[3];
    /* PLARGE_INTEGER   AllocationSize   = (PLARGE_INTEGER)args[4]; */
    /* ULONG            FileAttributes   = (ULONG)args[5]; */
    /* ULONG            ShareAccess      = (ULONG)args[6]; */
    ULONG               CreateDisposition = (ULONG)args[7];
    ULONG               CreateOptions     = (ULONG)args[8];
    /* PVOID            EaBuffer         = (PVOID)args[9]; */
    /* ULONG            EaLength         = (ULONG)args[10]; */

    if (!FileHandle || !ObjectAttributes || !ObjectAttributes->ObjectName)
        return STATUS_INVALID_PARAMETER;

    char path_buf[260];
    NTSTATUS path_status = nt_resolve_object_path(ObjectAttributes, path_buf);
    if (!NT_SUCCESS(path_status)) {
        if (IoStatusBlock) {
            IoStatusBlock->Status = path_status;
            IoStatusBlock->Information = 0;
        }
        return path_status;
    }
    const char *path = path_buf;

    static uint32_t cache_create_sequence;
    static uint32_t cache_create_new_sequence;
    uint32_t cache_create_id = 0;
    bool trace_cache = nt_is_cef_cache_path(path);
    if (trace_cache) {
        cache_create_id = __atomic_fetch_add(&cache_create_sequence, 1,
                                              __ATOMIC_RELAXED);
        trace_cache = cache_create_id < 256;
        if (!trace_cache && CreateDisposition == FILE_CREATE) {
            uint32_t create_new_id = __atomic_fetch_add(
                &cache_create_new_sequence, 1, __ATOMIC_RELAXED);
            trace_cache = create_new_id < 128;
            cache_create_id = 100000U + create_new_id;
        }
    }

    static uint32_t rebuild_create_logs;
    bool trace_rebuild = nt_path_contains_ci(path, "__tmp_for_rebuild");
    if (trace_rebuild) {
        uint32_t index = __atomic_fetch_add(&rebuild_create_logs, 1,
                                             __ATOMIC_RELAXED);
        trace_rebuild = index < 128;
    }
    if (trace_rebuild) {
        serial_puts("[NT-REBUILD] create '");
        serial_puts(path);
        serial_puts("' access=0x");
        serial_puthex(DesiredAccess, 8);
        serial_puts(" disp=0x");
        serial_puthex(CreateDisposition, 2);
        serial_puts(" opts=0x");
        serial_puthex(CreateOptions, 8);
        serial_puts("\n");
    }

#ifndef OK_QUIET
    serial_puts("[NT] NtCreateFile: '");
    serial_puts(path);
    serial_puts("' buf=0x");
    serial_puthex((uint64_t)(ObjectAttributes->ObjectName ?
        (uint64_t)ObjectAttributes->ObjectName->Buffer : 0), 16);
    serial_puts(" len=");
    serial_puthex((uint64_t)(ObjectAttributes->ObjectName ?
        ObjectAttributes->ObjectName->Length : 0), 4);
    serial_puts(" raw[0]=0x");
    if (ObjectAttributes->ObjectName && ObjectAttributes->ObjectName->Buffer)
        serial_puthex((uint64_t)ObjectAttributes->ObjectName->Buffer[0], 4);
    else
        serial_puts("NULL");
    serial_puts("\n");
#endif

    /* For BMP files that don't exist in OsitoFS, provide a minimal 1x1 BMP
     * so that Bitmap.LoadFile() assertions pass. The engine requires a valid
     * BMP for the splash screen — returning NOT_FOUND triggers a fatal assert. */

    /* Try to find/create in OsitoFS (case-insensitive for Win32). */
    void *osfs_file = osfs2_find_ci(path);
    bool existed_before = osfs_file != NULL;
    if (trace_cache)
        nt_trace_cache_create(cache_create_id, "lookup", path, DesiredAccess,
                              CreateDisposition, CreateOptions,
                              STATUS_SUCCESS, existed_before);

    if (CreateOptions & FILE_DIRECTORY_FILE) {
        bool existed = win32_directory_exists_normalized(path);
        if (trace_rebuild) {
            serial_puts("[NT-REBUILD] directory existed=");
            serial_putdec(existed);
            serial_puts(" file_collision=");
            serial_putdec(osfs_file != NULL);
            serial_puts("\n");
        }
        if (osfs_file || (existed && CreateDisposition == FILE_CREATE)) {
            if (IoStatusBlock) {
                IoStatusBlock->Status = STATUS_OBJECT_NAME_COLLISION;
                IoStatusBlock->Information = 0;
            }
            nt_log_create_failure(path, STATUS_OBJECT_NAME_COLLISION,
                                  CreateDisposition, CreateOptions,
                                  ObjectAttributes->RootDirectory);
            if (trace_cache)
                nt_trace_cache_create(cache_create_id, "dir-collision", path,
                                      DesiredAccess, CreateDisposition,
                                      CreateOptions,
                                      STATUS_OBJECT_NAME_COLLISION, existed);
            return STATUS_OBJECT_NAME_COLLISION;
        }
        if (!existed) {
            if (CreateDisposition == FILE_OPEN ||
                CreateDisposition == FILE_OVERWRITE) {
                if (IoStatusBlock) {
                    IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
                    IoStatusBlock->Information = 0;
                }
                nt_log_create_failure(path, STATUS_OBJECT_NAME_NOT_FOUND,
                                      CreateDisposition, CreateOptions,
                                      ObjectAttributes->RootDirectory);
                if (trace_cache)
                    nt_trace_cache_create(cache_create_id, "dir-missing", path,
                                          DesiredAccess, CreateDisposition,
                                          CreateOptions,
                                          STATUS_OBJECT_NAME_NOT_FOUND, false);
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            DWORD error = win32_directory_create_normalized(path);
            if (trace_rebuild) {
                serial_puts("[NT-REBUILD] mkdir error=");
                serial_putdec(error);
                serial_puts(" exists_after=");
                serial_putdec(win32_directory_exists_normalized(path));
                serial_puts("\n");
            }
            if (error) {
                NTSTATUS create_status = error == 3
                    ? STATUS_OBJECT_PATH_NOT_FOUND
                    : error == 8 ? STATUS_INSUFFICIENT_RESOURCES
                    : error == 183 ? STATUS_OBJECT_NAME_COLLISION
                    : STATUS_UNSUCCESSFUL;
                if (IoStatusBlock) {
                    IoStatusBlock->Status = create_status;
                    IoStatusBlock->Information = 0;
                }
                nt_log_create_failure(path, create_status, CreateDisposition,
                                      CreateOptions,
                                      ObjectAttributes->RootDirectory);
                if (trace_cache)
                    nt_trace_cache_create(cache_create_id, "mkdir-failed", path,
                                          DesiredAccess, CreateDisposition,
                                          CreateOptions, create_status, false);
                return create_status;
            }
        }

        FILE_OBJECT *directory = nt_file_object_allocate(FILE_OBJ_DIRECTORY);
        if (!directory) {
            if (trace_cache)
                nt_trace_cache_create(cache_create_id, "dir-pool", path,
                                      DesiredAccess, CreateDisposition,
                                      CreateOptions,
                                      STATUS_INSUFFICIENT_RESOURCES, existed);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        int name_length = 0;
        while (path[name_length] && name_length < 259) {
            directory->name[name_length] =
                (WCHAR)(unsigned char)path[name_length];
            name_length++;
        }
        directory->name[name_length] = 0;

        NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_FILE,
                                       DesiredAccess, directory, FileHandle);
        if (!NT_SUCCESS(status)) {
            nt_file_object_release(directory);
            if (trace_cache)
                nt_trace_cache_create(cache_create_id, "dir-handle", path,
                                      DesiredAccess, CreateDisposition,
                                      CreateOptions, status, existed);
            return status;
        }
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = existed ? 1 /* FILE_OPENED */
                                                  : 2 /* FILE_CREATED */;
        }
        if (trace_rebuild) {
            serial_puts("[NT-REBUILD] directory success handle=0x");
            serial_puthex((uint64_t)(ULONG_PTR)*FileHandle, 8);
            serial_puts(" info=");
            serial_putdec(existed ? 1 : 2);
            serial_puts("\n");
        }
        if (trace_cache)
            nt_trace_cache_create(cache_create_id, "dir-success", path,
                                  DesiredAccess, CreateDisposition,
                                  CreateOptions, STATUS_SUCCESS, existed);
        return STATUS_SUCCESS;
    }

    if (trace_rebuild) {
        serial_puts("[NT-REBUILD] regular existed=");
        serial_putdec(osfs_file != NULL);
        serial_puts("\n");
    }

    /* Old Win32 images stored only the basename. Move a fallback match to the
     * canonical path as soon as the caller supplies one, preserving its data. */
    if (osfs_file) {
        const char *stored = osfs2_file_name(osfs_file);
        if (stored && !nt_path_equal_ci(stored, path) &&
            (osfs3_is_mounted() || strlen(path) < 128) &&
            osfs2_rename(stored, path, false) == 0)
            osfs_file = osfs2_find_ci(path);
    }

    if (osfs_file && CreateDisposition == FILE_CREATE) {
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_OBJECT_NAME_COLLISION;
            IoStatusBlock->Information = 0;
        }
        if (trace_cache)
            nt_trace_cache_create(cache_create_id, "file-collision", path,
                                  DesiredAccess, CreateDisposition,
                                  CreateOptions, STATUS_OBJECT_NAME_COLLISION,
                                  true);
        return STATUS_OBJECT_NAME_COLLISION;
    }

    if (!osfs_file && (CreateDisposition == FILE_CREATE ||
                       CreateDisposition == FILE_OPEN_IF ||
                       CreateDisposition == FILE_OVERWRITE_IF ||
                       CreateDisposition == FILE_SUPERSEDE)) {
        osfs_file = osfs2_create(path, 0);
        if (trace_cache)
            nt_trace_cache_create(cache_create_id,
                                  osfs_file ? "fs-create-ok" : "fs-create-null",
                                  path, DesiredAccess, CreateDisposition,
                                  CreateOptions,
                                  osfs_file ? STATUS_SUCCESS
                                            : STATUS_OBJECT_NAME_NOT_FOUND,
                                  false);
    } else if (osfs_file && (CreateDisposition == FILE_OVERWRITE ||
                             CreateDisposition == FILE_OVERWRITE_IF ||
                             CreateDisposition == FILE_SUPERSEDE)) {
        if (osfs2_truncate(osfs_file, 0) < 0) {
            if (trace_cache)
                nt_trace_cache_create(cache_create_id, "truncate-failed", path,
                                      DesiredAccess, CreateDisposition,
                                      CreateOptions, STATUS_UNSUCCESSFUL, true);
            return STATUS_UNSUCCESSFUL;
        }
    }

    if (!osfs_file) {
        /* Check if this is a BMP file — provide minimal 1x1 BMP to avoid
         * fatal assertion in Bitmap.LoadFile(). The engine asserts on
         * missing splash/logo BMPs with no recovery path. */
        int is_bmp = 0;
        for (const char *p = path; *p; p++) {
            if (p[0] == '.' && (p[1] == 'b' || p[1] == 'B') &&
                (p[2] == 'm' || p[2] == 'M') && (p[3] == 'p' || p[3] == 'P') &&
                p[4] == '\0') {
                is_bmp = 1;
                break;
            }
        }
        if (is_bmp) {
            serial_puts("[NT] BMP fallback: creating minimal ");
            serial_puts(path);
            serial_puts("\n");
            osfs_file = osfs2_create(path, 0);
            if (osfs_file) {
                /* Write a valid 1x1 24-bit BMP (58 bytes) */
                static const uint8_t bmp_1x1[] = {
                    0x42, 0x4D, 0x3A, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
                    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
                    0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00,
                };
                osfs2_write(osfs_file, 0, bmp_1x1, sizeof(bmp_1x1));
            }
        }
    }

    if (!osfs_file) {
        nt_log_create_failure(path, STATUS_OBJECT_NAME_NOT_FOUND,
                              CreateDisposition, CreateOptions,
                              ObjectAttributes->RootDirectory);
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            IoStatusBlock->Information = 0;
        }
        if (trace_cache)
            nt_trace_cache_create(cache_create_id, "file-missing", path,
                                  DesiredAccess, CreateDisposition,
                                  CreateOptions, STATUS_OBJECT_NAME_NOT_FOUND,
                                  existed_before);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    /* Create file object. The pool is reused: a free slot has flags==0 (an
     * in-use object always has FILE_OBJ_DISK_FILE set); NtClose returns the slot
     * by zeroing flags. Previously this was a monotonic bump allocator that never
     * freed, so after 64 opens NtCreateFile failed for everything — which is why
     * UT99 opened Entry.unr fine early but got "Can't find file" at LoadMap once
     * the 64 slots were exhausted (64 opens / 5 closes). */
    FILE_OBJECT *fobj = nt_file_object_allocate(FILE_OBJ_DISK_FILE);
    if (!fobj) {
        if (trace_cache)
            nt_trace_cache_create(cache_create_id, "file-pool", path,
                                  DesiredAccess, CreateDisposition,
                                  CreateOptions, STATUS_INSUFFICIENT_RESOURCES,
                                  existed_before);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fobj->osfs_file = osfs_file;
    fobj->position  = 0;
    fobj->size      = osfs2_file_size(osfs_file);
    int name_length = 0;
    while (path[name_length] && name_length < 259) {
        fobj->name[name_length] = (WCHAR)(unsigned char)path[name_length];
        name_length++;
    }
    fobj->name[name_length] = 0;

    /* Allocate handle */
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_FILE,
                                   DesiredAccess, fobj, FileHandle);
    if (!NT_SUCCESS(status)) {
        nt_file_object_release(fobj);
        if (trace_cache)
            nt_trace_cache_create(cache_create_id, "file-handle", path,
                                  DesiredAccess, CreateDisposition,
                                  CreateOptions, status, existed_before);
        return status;
    }

    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (osfs_file ? 1 /* FILE_OPENED */ : 2 /* FILE_CREATED */);
    }

    if (trace_rebuild) {
        serial_puts("[NT-REBUILD] regular success handle=0x");
        serial_puthex((uint64_t)(ULONG_PTR)*FileHandle, 8);
        serial_puts(" size=");
        serial_putdec(fobj->size);
        serial_puts("\n");
    }

    if (trace_cache)
        nt_trace_cache_create(cache_create_id, "file-success", path,
                              DesiredAccess, CreateDisposition, CreateOptions,
                              STATUS_SUCCESS, existed_before);

#ifndef OK_QUIET
    nt_log_hex("NtCreateFile: handle = ", (ULONGLONG)*FileHandle);
#endif
    return STATUS_SUCCESS;
}

/* ── NtReadFile ─────────────────────────────────────────────── */

NTSTATUS sys_NtReadFile(ULONG_PTR *args)
{
    HANDLE              FileHandle   = (HANDLE)args[0];
    /* HANDLE           Event        = (HANDLE)args[1]; */
    /* PIO_APC_ROUTINE  ApcRoutine   = (PIO_APC_ROUTINE)args[2]; */
    /* PVOID            ApcContext   = (PVOID)args[3]; */
    PIO_STATUS_BLOCK    IoStatusBlock = (PIO_STATUS_BLOCK)args[4];
    PVOID               Buffer       = (PVOID)args[5];
    ULONG               Length       = (ULONG)args[6];
    PLARGE_INTEGER      ByteOffset   = (PLARGE_INTEGER)args[7];
    /* PULONG           Key          = (PULONG)args[8]; */

    FILE_OBJECT *fobj = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, FileHandle,
                                    OBJ_TYPE_FILE, (PVOID *)&fobj);
    if (!NT_SUCCESS(status))
        return status;

    if (!Buffer || Length == 0) {
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 0;
        }
        return STATUS_SUCCESS;
    }

    /* Console input */
    if (fobj->flags & FILE_OBJ_CONSOLE_IN) {
        /* Stub: return 0 bytes (no keyboard driver yet) */
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_END_OF_FILE;
            IoStatusBlock->Information = 0;
        }
        return STATUS_END_OF_FILE;
    }

    if (fobj->flags & FILE_OBJ_PIPE_READ)
        return nt_pipe_read(fobj, Buffer, Length, IoStatusBlock);

    /* Disk file */
    LONGLONG offset = ByteOffset ? ByteOffset->QuadPart : fobj->position;

    if (offset >= fobj->size) {
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_END_OF_FILE;
            IoStatusBlock->Information = 0;
        }
        return STATUS_END_OF_FILE;
    }

    ULONG to_read = Length;
    if (offset + to_read > (ULONGLONG)fobj->size)
        to_read = (ULONG)(fobj->size - offset);

#if !defined(OK_QUIET) || !OK_QUIET
    static uint32_t read_trace_count;
    uint32_t read_trace_seq = __atomic_fetch_add(&read_trace_count, 1,
                                                  __ATOMIC_RELAXED);
    BOOL trace_read = read_trace_seq < 64;
    if (trace_read) {
        serial_puts("[NtReadFile] h=0x");
        serial_puthex((uint64_t)FileHandle, 4);
        serial_puts(" buf=0x");
        serial_puthex((uint64_t)Buffer, 8);
        serial_puts(" len=");
        serial_puthex(to_read, 4);
        serial_puts(" off=");
        serial_puthex(offset, 8);
        serial_puts(" fsz=");
        serial_puthex(fobj->size, 8);
        serial_puts("\n");
    } else if (read_trace_seq == 64) {
        serial_puts("[NtReadFile] repetitive success tracing suppressed\n");
    }
#endif

    /*
     * Buffered I/O: read into a kernel-allocated temp buffer, then copy
     * to the user buffer. This matches NT's DO_BUFFERED_IO approach and
     * solves the Win32 page table issue: VirtualAlloc VAs (0x40000000+)
     * are only mapped in the Win32 page table, and nvme_read_bytes does
     * memcpy under kernel identity map where those VAs don't resolve.
     *
     * By reading to a kernel buffer first (identity-mapped, accessible
     * from any CR3) and then copying to the user VA (accessible under
     * the current Win32 CR3), we guarantee correct data delivery.
     */
    extern void *kmalloc(uint64_t size);
    extern void kfree(void *ptr);
    void *sys_buf = kmalloc(to_read);
    int result;
    if (sys_buf) {
        result = osfs2_read(fobj->osfs_file, (uint64_t)offset, sys_buf, to_read);
        if (result >= 0) {
            /* Debug: check if sys_buf has data */
            uint8_t *sb = (uint8_t *)sys_buf;
            if (to_read <= 8192 && offset == 0 && sb[0] == 0 && sb[1] == 0) {
                serial_puts("[NtReadFile] SYS_BUF ZERO! buf=0x");
                serial_puthex((uint64_t)sys_buf, 16);
                serial_puts("\n");
            }
            /* Copy directly to user buffer. Use volatile to prevent
             * the compiler from optimizing away the copy. */
            volatile uint8_t *dst = (volatile uint8_t *)Buffer;
            uint8_t *src = (uint8_t *)sys_buf;
            for (ULONG i = 0; i < to_read; i++)
                dst[i] = src[i];
        }
        kfree(sys_buf);
    } else {
        /* Fallback: direct read (may fail for VirtualAlloc buffers) */
        result = osfs2_read(fobj->osfs_file, (uint64_t)offset, Buffer, to_read);
    }

#if !defined(OK_QUIET) || !OK_QUIET
    if (trace_read) {
        serial_puts("[NtReadFile] result=");
        serial_puthex((uint64_t)(int64_t)result, 8);
        serial_puts("\n");
    }
#endif

#ifndef OK_QUIET
    /* Dump first 16 bytes as hex for localization debugging */
    if (result > 0 && result <= 8192 && offset == 0) {
        serial_puts("[NtReadFile] hex: ");
        const uint8_t *d = (const uint8_t *)Buffer;
        int dlen = result < 16 ? result : 16;
        for (int di = 0; di < dlen; di++) {
            serial_puthex(d[di], 2);
            serial_puts(" ");
        }
        serial_puts("= \"");
        for (int di = 0; di < dlen; di++) {
            char c = d[di];
            if (c >= 32 && c < 127) {
                char buf[2] = { c, 0 };
                serial_puts(buf);
            } else {
                serial_puts(".");
            }
        }
        serial_puts("\"\n");
    }
#endif

    if (result < 0) {
        serial_puts("[NtReadFile] read failed: h=0x");
        serial_puthex((uint64_t)FileHandle, 4);
        serial_puts(" off=0x");
        serial_puthex(offset, 16);
        serial_puts(" len=0x");
        serial_puthex(to_read, 8);
        serial_puts(" result=0x");
        serial_puthex((uint64_t)(int64_t)result, 16);
        serial_puts("\n");
        return STATUS_UNSUCCESSFUL;
    }

    if (!ByteOffset)
        fobj->position = offset + to_read;

    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = to_read;
    }

    return STATUS_SUCCESS;
}

/* ── NtWriteFile ────────────────────────────────────────────── */

NTSTATUS sys_NtWriteFile(ULONG_PTR *args)
{
    HANDLE              FileHandle    = (HANDLE)args[0];
    /* HANDLE           Event         = (HANDLE)args[1]; */
    /* PIO_APC_ROUTINE  ApcRoutine    = (PIO_APC_ROUTINE)args[2]; */
    /* PVOID            ApcContext    = (PVOID)args[3]; */
    PIO_STATUS_BLOCK    IoStatusBlock = (PIO_STATUS_BLOCK)args[4];
    PVOID               Buffer        = (PVOID)args[5];
    ULONG               Length        = (ULONG)args[6];
    PLARGE_INTEGER      ByteOffset    = (PLARGE_INTEGER)args[7];
    /* PULONG           Key           = (PULONG)args[8]; */

    FILE_OBJECT *fobj = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, FileHandle,
                                    OBJ_TYPE_FILE, (PVOID *)&fobj);
    if (!NT_SUCCESS(status))
        return status;

    if (!Buffer || Length == 0) {
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 0;
        }
        return STATUS_SUCCESS;
    }

    /* Console output */
    if (fobj->flags & (FILE_OBJ_CONSOLE_OUT | FILE_OBJ_CONSOLE_ERR)) {
        const char *s = (const char *)Buffer;
        serial_write(s, Length);
        if (!win32_current_is_gui_app())
            for (ULONG i = 0; i < Length; i++)
                fb_putc(s[i], 0x00CCCCCC);
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = Length;
        }
        return STATUS_SUCCESS;
    }

    if (fobj->flags & FILE_OBJ_PIPE_WRITE)
        return nt_pipe_write(fobj, Buffer, Length, IoStatusBlock);

    LONGLONG offset = ByteOffset ? ByteOffset->QuadPart : fobj->position;
    if (offset < 0)
        return STATUS_INVALID_PARAMETER;

    int result = osfs2_write(fobj->osfs_file, (uint64_t)offset, Buffer, Length);
    if (result < 0) {
        serial_puts("[NtWriteFile] failed h=0x");
        serial_puthex((uint64_t)FileHandle, 8);
        serial_puts(" fobj=0x");
        serial_puthex((uint64_t)fobj, 16);
        serial_puts(" flags=0x");
        serial_puthex(fobj->flags, 8);
        serial_puts(" file=0x");
        serial_puthex((uint64_t)fobj->osfs_file, 16);
        serial_puts(" off=0x");
        serial_puthex((uint64_t)offset, 16);
        serial_puts(" len=0x");
        serial_puthex(Length, 8);
        serial_puts(" size=0x");
        serial_puthex((uint64_t)fobj->size, 16);
        serial_puts("\n");
        return STATUS_UNSUCCESSFUL;
    }

    LONGLONG end = offset + Length;
    if (!ByteOffset)
        fobj->position = end;
    if (end > fobj->size)
        fobj->size = end;

    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = Length;
    }

    return STATUS_SUCCESS;
}

/* ── NtClose ────────────────────────────────────────────────── */

NTSTATUS nt_close_handle_for_process(HANDLE Handle, ULONG owner_pid)
{
    /* Don't close console handles */
    if (Handle == STD_INPUT_HANDLE_VALUE ||
        Handle == STD_OUTPUT_HANDLE_VALUE ||
        Handle == STD_ERROR_HANDLE_VALUE) {
        return STATUS_SUCCESS;
    }

    HANDLE_ENTRY closed;
    NTSTATUS status = handle_close_entry_for_process(
        &g_handle_table, Handle, owner_pid, &closed);
    if (!NT_SUCCESS(status) || !closed.object ||
        handle_object_referenced(&g_handle_table, closed.type, closed.object))
        return status;

    /* Return the FILE_OBJECT pool slot so NtCreateFile can reuse it (zeroing
     * flags marks it free). Without this the disk-file pool leaks and later
     * opens fail with INSUFFICIENT_RESOURCES. */
    if (closed.type == OBJ_TYPE_FILE) {
        FILE_OBJECT *fobj = (FILE_OBJECT *)closed.object;
        if (fobj->flags & (FILE_OBJ_PIPE_READ | FILE_OBJ_PIPE_WRITE))
            nt_pipe_release(fobj);
        else if (fobj->flags & (FILE_OBJ_DISK_FILE | FILE_OBJ_DIRECTORY))
            nt_file_object_release(fobj);
    } else if (closed.type == OBJ_TYPE_SECTION) {
        section_release_handle(closed.object);
    } else if (closed.type == OBJ_TYPE_SNAPSHOT) {
        kfree(closed.object);
    } else if (closed.type == OBJ_TYPE_POWER_REQUEST) {
        k32_power_request_release(closed.object);
    } else if (closed.type == OBJ_TYPE_EVENT ||
               closed.type == OBJ_TYPE_MUTANT ||
               closed.type == OBJ_TYPE_SEMAPHORE) {
        extern void ntsync_release_object(OBJECT_TYPE_ID type, PVOID object);
        ntsync_release_object(closed.type, closed.object);
    } else if (closed.type == OBJ_TYPE_THREAD) {
        extern void nt_process_release_thread_object(PVOID object);
        nt_process_release_thread_object(closed.object);
    }

    return STATUS_SUCCESS;
}

NTSTATUS sys_NtClose(ULONG_PTR *args)
{
    extern DWORD win32_current_process_id(void);
    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    return nt_close_handle_for_process((HANDLE)args[0], owner_pid);
}

/* ── NtAllocateVirtualMemory ────────────────────────────────── */

NTSTATUS sys_NtAllocateVirtualMemory(ULONG_PTR *args)
{
    /* HANDLE   ProcessHandle = (HANDLE)args[0]; */
    PVOID   *BaseAddress   = (PVOID *)args[1];
    /* ULONG_PTR ZeroBits   = args[2]; */
    SIZE_T  *RegionSize    = (SIZE_T *)args[3];
    ULONG    AllocationType = (ULONG)args[4];
    ULONG    Protect        = (ULONG)args[5];

    if (!BaseAddress || !RegionSize)
        return STATUS_INVALID_PARAMETER;

    SIZE_T requested_size = *RegionSize;
    uint64_t requested_address = (uint64_t)(ULONG_PTR)*BaseAddress;
    uint64_t requested_va = requested_address & ~0xFFFULL;
    SIZE_T page_offset = requested_address - requested_va;
    if (!requested_size || requested_size > UINT64_MAX - page_offset ||
        requested_size + page_offset > UINT64_MAX - 0xFFFULL)
        return STATUS_INVALID_PARAMETER;
    SIZE_T size = (requested_size + page_offset + 0xFFFULL) & ~0xFFFULL;
    uint64_t pages = size / 4096;

    if (pages == 0)
        return STATUS_INVALID_PARAMETER;

    vm_global_init_once();
    uint64_t vm_irq_flags = vm_track_lock_irqsave();

    PVOID addr = NULL;
    uint64_t phys = 0;

    /* MEM_RESET is advisory: the caller no longer cares about the current
     * contents, but the address remains committed and usable. Our allocator
     * cannot discard backing pages independently yet, so preserve them and
     * report the original range. This is preferable to a false OOM in callers
     * such as Chromium's PartitionAlloc. */
    if (AllocationType & MEM_RESET) {
        if (AllocationType != MEM_RESET || !requested_va ||
            !vm_track_contains(requested_va, size)) {
            vm_track_unlock_irqrestore(vm_irq_flags);
            return STATUS_CONFLICTING_ADDRESSES;
        }
        vm_track_unlock_irqrestore(vm_irq_flags);
        *BaseAddress = (PVOID)requested_va;
        *RegionSize = size;
        return STATUS_SUCCESS;
    }

    BOOL want_commit = (AllocationType & MEM_COMMIT) != 0;
    BOOL want_reserve = (AllocationType & MEM_RESERVE) != 0;
    if (!want_reserve && !want_commit) {
        vm_track_unlock_irqrestore(vm_irq_flags);
        return STATUS_INVALID_PARAMETER;
    }

    /* Windows implicitly reserves the region when MEM_COMMIT is requested
     * with a NULL base. A non-NULL commit-only request must still refer to an
     * existing reservation. */
    if (want_commit && !want_reserve && requested_address == 0)
        want_reserve = TRUE;

    ULONG owner_pid = nt_current_owner_pid();
    uint64_t cr3 = nt_current_cr3();

    if (want_reserve) {
        if (requested_va &&
            (vm_track_overlaps(requested_va, size) ||
             pe_va_range_conflicts(requested_va, size))) {
            vm_track_unlock_irqrestore(vm_irq_flags);
            return STATUS_CONFLICTING_ADDRESSES;
        }

        int initial_count = vm_track_count;
        int entries_needed = want_commit ? 2 : 1;
        if (vm_track_count > VM_TRACK_MAX - entries_needed) {
            vm_track_unlock_irqrestore(vm_irq_flags);
            return STATUS_NO_MEMORY;
        }

        addr = win32_va_alloc(size, &phys, Protect, requested_va,
                              !want_commit, g_compat32_mode);
        if (!addr) {
            vm_track_unlock_irqrestore(vm_irq_flags);
            return requested_va ? STATUS_CONFLICTING_ADDRESSES
                                : STATUS_NO_MEMORY;
        }
        if (!vm_track_add((uint64_t)addr, 0, size, Protect,
                          FALSE, NULL, 0) ||
            (want_commit &&
             !vm_track_add((uint64_t)addr, phys, size, Protect,
                           TRUE, NULL, 0))) {
            vm_track_count = initial_count;
            if (phys) {
                for (uint64_t page = 0; page < pages; page++)
                    nt_unmap_page_in(cr3, (uint64_t)addr + page * 4096);
                mem_free_pages((void *)(uintptr_t)phys, pages);
            }
            vm_track_unlock_irqrestore(vm_irq_flags);
            return STATUS_NO_MEMORY;
        }
        vm_advance_auto_next_locked(owner_pid, g_compat32_mode,
                                    (uint64_t)addr, size);
    } else {
        if (!requested_va ||
            vm_track_find_reservation_locked(owner_pid, requested_va, size) < 0) {
            vm_track_unlock_irqrestore(vm_irq_flags);
            return STATUS_CONFLICTING_ADDRESSES;
        }
        if (!vm_commit_range_locked(owner_pid, cr3, requested_va, size,
                                    Protect)) {
            vm_track_unlock_irqrestore(vm_irq_flags);
            return STATUS_NO_MEMORY;
        }
        addr = (PVOID)(ULONG_PTR)requested_va;
    }

    vm_track_unlock_irqrestore(vm_irq_flags);

    *BaseAddress = addr;
    *RegionSize  = size;

#if defined(WIN32_VM_TRACE) && WIN32_VM_TRACE
    nt_log_hex("NtAllocateVirtualMemory: ", (ULONGLONG)addr);
    nt_log_hex("  size = ", size);
#endif

    return STATUS_SUCCESS;
}

/* ── NtFreeVirtualMemory ────────────────────────────────────── */

/* Release one complete private reservation. Must hold vm_track_lock. */
static NTSTATUS vm_release_reservation_locked(ULONG owner_pid, uint64_t va,
                                               SIZE_T *released_size)
{
    int reservation_index = -1;
    for (int i = 0; i < vm_track_count; i++) {
        if (vm_track[i].owner_pid == owner_pid && vm_track[i].va == va &&
            vm_track_entry_reserved(&vm_track[i])) {
            reservation_index = i;
            break;
        }
    }
    if (reservation_index < 0)
        return STATUS_UNABLE_TO_FREE_VM;

    SIZE_T tracked = vm_track[reservation_index].size;
    uint64_t end = va + tracked;
    if (end < va)
        return STATUS_UNABLE_TO_FREE_VM;

    BOOL had_committed_backing = FALSE;
    for (int i = 0; i < vm_track_count; i++) {
        vm_track_entry_t *entry = &vm_track[i];
        uint64_t entry_end = entry->va + entry->size;
        if (entry->owner_pid != owner_pid ||
            !vm_track_entry_committed(entry) || entry_end < entry->va ||
            va >= entry_end || entry->va >= end)
            continue;
        if (entry->section || entry->va < va || entry_end > end)
            return STATUS_UNABLE_TO_FREE_VM;
        had_committed_backing = TRUE;
    }

    vm_track[reservation_index] = vm_track[--vm_track_count];
    for (int i = 0; i < vm_track_count;) {
        vm_track_entry_t entry = vm_track[i];
        uint64_t entry_end = entry.va + entry.size;
        if (entry.owner_pid == owner_pid &&
            vm_track_entry_committed(&entry) &&
            entry.va >= va && entry_end <= end) {
            vm_track[i] = vm_track[--vm_track_count];
            vm_track_unmap_range(&entry, entry.va, entry.va + entry.size);
        } else {
            i++;
        }
    }

    if (!had_committed_backing)
        vm_rewind_auto_next_locked(owner_pid, va);

    if (released_size)
        *released_size = tracked;
    return STATUS_SUCCESS;
}

NTSTATUS sys_NtFreeVirtualMemory(ULONG_PTR *args)
{
    /* HANDLE ProcessHandle = (HANDLE)args[0]; */
    PVOID  *BaseAddress    = (PVOID *)args[1];
    SIZE_T *RegionSize     = (SIZE_T *)args[2];
    ULONG   FreeType       = (ULONG)args[3];

    if (!BaseAddress || !*BaseAddress || !RegionSize ||
        (FreeType != MEM_DECOMMIT && FreeType != MEM_RELEASE))
        return STATUS_INVALID_PARAMETER;

#if defined(WIN32_VM_TRACE) && WIN32_VM_TRACE
    nt_log_hex("NtFreeVirtualMemory: ", (ULONGLONG)*BaseAddress);
    nt_log_hex("  FreeType = ", FreeType);
#endif

    uint64_t vm_irq_flags = vm_track_lock_irqsave();

    if (FreeType == MEM_DECOMMIT) {
        uint64_t start = (uint64_t)(ULONG_PTR)*BaseAddress;
        uint64_t va = start & ~0xFFFULL;
        SIZE_T requested = *RegionSize;
        if (!requested) {
            vm_track_unlock_irqrestore(vm_irq_flags);
            return STATUS_INVALID_PARAMETER;
        }
        SIZE_T size = requested + (start - va);
        if (size < requested || size > UINT64_MAX - 0xFFFULL) {
            vm_track_unlock_irqrestore(vm_irq_flags);
            return STATUS_INVALID_PARAMETER;
        }
        size = (size + 0xFFFULL) & ~0xFFFULL;
        int result = vm_decommit_range_locked(nt_current_owner_pid(), va, size);
        if (result <= 0) {
            vm_track_unlock_irqrestore(vm_irq_flags);
            return STATUS_UNABLE_TO_FREE_VM;
        }
        *BaseAddress = (PVOID)va;
        *RegionSize = size;
        vm_track_unlock_irqrestore(vm_irq_flags);
#if defined(WIN32_VM_TRACE) && WIN32_VM_TRACE
        nt_log_hex("  decommit size = ", size);
#endif
        return STATUS_SUCCESS;
    }

    if (*RegionSize != 0 || ((uint64_t)(ULONG_PTR)*BaseAddress & 0xFFFULL)) {
        vm_track_unlock_irqrestore(vm_irq_flags);
        return STATUS_INVALID_PARAMETER;
    }

    ULONG owner_pid = nt_current_owner_pid();
    uint64_t va = (uint64_t)(ULONG_PTR)*BaseAddress;
    SIZE_T tracked = 0;
    NTSTATUS release_status = vm_release_reservation_locked(owner_pid, va,
                                                             &tracked);
    vm_track_unlock_irqrestore(vm_irq_flags);
    if (!NT_SUCCESS(release_status))
        return release_status;
#if defined(WIN32_VM_TRACE) && WIN32_VM_TRACE
    nt_log_hex("  release size = ", tracked);
#endif

    *BaseAddress = NULL;
    *RegionSize = 0;
    return STATUS_SUCCESS;
}

NTSTATUS nt_vm_free_stack_for_process(ULONG owner_pid,
                                      PVOID allocation_base)
{
    if (!owner_pid || !allocation_base ||
        ((uint64_t)(ULONG_PTR)allocation_base & 0xFFFULL))
        return STATUS_INVALID_PARAMETER;

    uint64_t vm_irq_flags = vm_track_lock_irqsave();
    NTSTATUS status = vm_release_reservation_locked(
        owner_pid, (uint64_t)(ULONG_PTR)allocation_base, NULL);
    vm_track_unlock_irqrestore(vm_irq_flags);
    return status;
}

NTSTATUS nt_vm_free_stack(PVOID allocation_base)
{
    return nt_vm_free_stack_for_process(nt_current_owner_pid(),
                                        allocation_base);
}

/* ── NtQueryInformationFile ──────────────────────────────────── */

NTSTATUS nt_vm_allocate_stack(SIZE_T reserve_size, PVOID *allocation_base,
                              PVOID *stack_limit, PVOID *stack_base)
{
    if (!allocation_base || !stack_limit || !stack_base)
        return STATUS_INVALID_PARAMETER;

    *allocation_base = NULL;
    *stack_limit = NULL;
    *stack_base = NULL;

    if (reserve_size < 64 * 1024)
        reserve_size = 64 * 1024;
    if (reserve_size > UINT64_MAX - 0xFFFULL)
        return STATUS_INVALID_PARAMETER;
    reserve_size = (reserve_size + 0xFFFULL) & ~0xFFFULL;
    if (reserve_size <= 4096)
        return STATUS_INVALID_PARAMETER;

    PVOID allocation = NULL;
    SIZE_T size = reserve_size;
    ULONG_PTR reserve_args[6] = {
        (ULONG_PTR)NT_CURRENT_PROCESS, (ULONG_PTR)&allocation, 0,
        (ULONG_PTR)&size, MEM_RESERVE, PAGE_READWRITE
    };
    NTSTATUS status = sys_NtAllocateVirtualMemory(reserve_args);
    if (!NT_SUCCESS(status))
        return status;

    PVOID limit = (PVOID)((uint64_t)(ULONG_PTR)allocation + 4096);
    SIZE_T commit_size = reserve_size - 4096;
    ULONG_PTR commit_args[6] = {
        (ULONG_PTR)NT_CURRENT_PROCESS, (ULONG_PTR)&limit, 0,
        (ULONG_PTR)&commit_size, MEM_COMMIT, PAGE_READWRITE
    };
    status = sys_NtAllocateVirtualMemory(commit_args);
    if (!NT_SUCCESS(status)) {
        (void)nt_vm_free_stack(allocation);
        return status;
    }

    *allocation_base = allocation;
    *stack_limit = limit;
    *stack_base = (PVOID)((uint64_t)(ULONG_PTR)allocation + reserve_size);
    return STATUS_SUCCESS;
}

NTSTATUS sys_NtQueryInformationFile(ULONG_PTR *args)
{
    HANDLE              FileHandle       = (HANDLE)args[0];
    PIO_STATUS_BLOCK    IoStatusBlock    = (PIO_STATUS_BLOCK)args[1];
    PVOID               FileInformation  = (PVOID)args[2];
    ULONG               Length           = (ULONG)args[3];
    FILE_INFORMATION_CLASS InfoClass     = (FILE_INFORMATION_CLASS)args[4];

    FILE_OBJECT *fobj = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, FileHandle,
                                    OBJ_TYPE_FILE, (PVOID *)&fobj);
    if (!NT_SUCCESS(status))
        return status;

    switch (InfoClass) {
    case FileStandardInformation: {
        if (Length < sizeof(FILE_STANDARD_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        FILE_STANDARD_INFORMATION *info = (FILE_STANDARD_INFORMATION *)FileInformation;
        info->EndOfFile.QuadPart    = fobj->size;
        info->AllocationSize.QuadPart = (fobj->size + 4095) & ~4095LL;
        info->NumberOfLinks = 1;
        info->DeletePending = FALSE;
        info->Directory     = (fobj->flags & FILE_OBJ_DIRECTORY) != 0;
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_STANDARD_INFORMATION);
        }
        return STATUS_SUCCESS;
    }

    case FilePositionInformation: {
        if (Length < sizeof(FILE_POSITION_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        FILE_POSITION_INFORMATION *info = (FILE_POSITION_INFORMATION *)FileInformation;
        info->CurrentByteOffset.QuadPart = fobj->position;
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_POSITION_INFORMATION);
        }
        return STATUS_SUCCESS;
    }

    case FileBasicInformation: {
        if (Length < sizeof(FILE_BASIC_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        FILE_BASIC_INFORMATION *info = (FILE_BASIC_INFORMATION *)FileInformation;
        /* We don't have timestamps — zero them */
        info->CreationTime.QuadPart   = 0;
        info->LastAccessTime.QuadPart = 0;
        info->LastWriteTime.QuadPart  = 0;
        info->ChangeTime.QuadPart     = 0;
        info->FileAttributes = (fobj->flags & FILE_OBJ_DIRECTORY)
            ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (fobj->flags & (FILE_OBJ_CONSOLE_IN | FILE_OBJ_CONSOLE_OUT | FILE_OBJ_CONSOLE_ERR))
            info->FileAttributes = 0; /* device, not a file */
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_BASIC_INFORMATION);
        }
        return STATUS_SUCCESS;
    }

    default:
        nt_log_hex("NtQueryInformationFile: unsupported class ", (ULONGLONG)InfoClass);
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ── NtSetInformationFile ───────────────────────────────────── */

NTSTATUS sys_NtSetInformationFile(ULONG_PTR *args)
{
    HANDLE              FileHandle       = (HANDLE)args[0];
    PIO_STATUS_BLOCK    IoStatusBlock    = (PIO_STATUS_BLOCK)args[1];
    PVOID               FileInformation  = (PVOID)args[2];
    ULONG               Length           = (ULONG)args[3];
    FILE_INFORMATION_CLASS InfoClass     = (FILE_INFORMATION_CLASS)args[4];

    FILE_OBJECT *fobj = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, FileHandle,
                                    OBJ_TYPE_FILE, (PVOID *)&fobj);
    if (!NT_SUCCESS(status))
        return status;

    switch (InfoClass) {
    case FileBasicInformation:
        if (Length < sizeof(FILE_BASIC_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 0;
        }
        return STATUS_SUCCESS;

    case FilePositionInformation: {
        if (Length < sizeof(FILE_POSITION_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        FILE_POSITION_INFORMATION *info = (FILE_POSITION_INFORMATION *)FileInformation;
        fobj->position = info->CurrentByteOffset.QuadPart;
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 0;
        }
        return STATUS_SUCCESS;
    }

    case FileEndOfFileInformation: {
        if (Length < sizeof(LARGE_INTEGER))
            return STATUS_INFO_LENGTH_MISMATCH;
        PLARGE_INTEGER new_size = (PLARGE_INTEGER)FileInformation;
        if (new_size->QuadPart < 0 || (fobj->flags & FILE_OBJ_DIRECTORY))
            return STATUS_INVALID_PARAMETER;
        if ((fobj->flags & FILE_OBJ_DISK_FILE) && fobj->osfs_file &&
            osfs2_truncate(fobj->osfs_file,
                           (uint64_t)new_size->QuadPart) < 0)
            return STATUS_UNSUCCESSFUL;
        fobj->size = new_size->QuadPart;
        if (fobj->position > fobj->size)
            fobj->position = fobj->size;
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 0;
        }
        return STATUS_SUCCESS;
    }

    case FileAllocationInformation:
        if (Length < sizeof(LARGE_INTEGER))
            return STATUS_INFO_LENGTH_MISMATCH;
        if (((PLARGE_INTEGER)FileInformation)->QuadPart < 0 ||
            (fobj->flags & FILE_OBJ_DIRECTORY))
            return STATUS_INVALID_PARAMETER;
        /* OsitoFS allocates extents on write; preallocation is advisory. */
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 0;
        }
        return STATUS_SUCCESS;

    default:
        nt_log_hex("NtSetInformationFile: unsupported class ", (ULONGLONG)InfoClass);
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ── NtDuplicateObject ──────────────────────────────────────── */

static void nt_duplicate_trace_failure(const char *stage, NTSTATUS status,
                                       DWORD current_pid, DWORD source_pid,
                                       DWORD target_pid,
                                       HANDLE source_process,
                                       HANDLE source_handle,
                                       HANDLE target_process,
                                       PHANDLE target_handle,
                                       ACCESS_MASK desired_access,
                                       ULONG options)
{
    static ULONG failure_count;
    if (__atomic_fetch_add(&failure_count, 1, __ATOMIC_RELAXED) >= 32)
        return;

    HANDLE_ENTRY *source = handle_get_entry(&g_handle_table, source_handle);
    HANDLE_ENTRY *source_process_entry =
        handle_get_entry(&g_handle_table, source_process);
    HANDLE_ENTRY *target_process_entry =
        handle_get_entry(&g_handle_table, target_process);

    serial_puts("[NT-DUP-FAIL] stage=");
    serial_puts(stage);
    serial_puts(" status=0x");
    serial_puthex((uint32_t)status, 8);
    serial_puts(" current=");
    serial_putdec(current_pid);
    serial_puts(" source_pid=");
    serial_putdec(source_pid);
    serial_puts(" target_pid=");
    serial_putdec(target_pid);
    serial_puts(" source_proc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)source_process, 16);
    serial_puts(" source=0x");
    serial_puthex((uint64_t)(ULONG_PTR)source_handle, 16);
    serial_puts(" source_type=");
    serial_putdec(source ? source->type : 0);
    serial_puts(" source_access=0x");
    serial_puthex(source ? source->access : 0, 8);
    serial_puts(" source_owner=");
    serial_putdec(source ? source->owner_pid : 0);
    serial_puts(" source_refs=");
    serial_putdec(source ? source->refs : 0);
    serial_puts(" target_proc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)target_process, 16);
    serial_puts(" source_proc_owner=");
    serial_putdec(source_process_entry ? source_process_entry->owner_pid : 0);
    serial_puts(" target_proc_owner=");
    serial_putdec(target_process_entry ? target_process_entry->owner_pid : 0);
    serial_puts(" out=0x");
    serial_puthex((uint64_t)(ULONG_PTR)target_handle, 16);
    serial_puts(" access=0x");
    serial_puthex(desired_access, 8);
    serial_puts(" options=0x");
    serial_puthex(options, 8);
    serial_puts("\n");
}

static bool nt_is_current_process_handle(HANDLE handle)
{
    return (uint32_t)(ULONG_PTR)handle ==
           (uint32_t)(ULONG_PTR)NT_CURRENT_PROCESS;
}

static bool nt_is_current_thread_handle(HANDLE handle)
{
    return (uint32_t)(ULONG_PTR)handle ==
           (uint32_t)(ULONG_PTR)NT_CURRENT_THREAD;
}

#define NT_PROCESS_DUP_HANDLE_ACCESS 0x00000040U

static NTSTATUS nt_duplicate_resolve_process(HANDLE process_handle,
                                             DWORD current_pid,
                                             DWORD *process_id)
{
    extern BOOL nt_process_id(HANDLE, DWORD *);

    if (nt_is_current_process_handle(process_handle)) {
        *process_id = current_pid;
        return STATUS_SUCCESS;
    }

    HANDLE_OBJECT_SNAPSHOT snapshot;
    NTSTATUS status = handle_snapshot_for_process(
        &g_handle_table, process_handle, current_pid, &snapshot);
    if (!NT_SUCCESS(status) || snapshot.type != OBJ_TYPE_PROCESS)
        return STATUS_INVALID_HANDLE;
    if (!(snapshot.access & GENERIC_ALL) &&
        !(snapshot.access & NT_PROCESS_DUP_HANDLE_ACCESS))
        return STATUS_ACCESS_DENIED;
    if (!nt_process_id(process_handle, process_id))
        return STATUS_INVALID_HANDLE;
    return STATUS_SUCCESS;
}

static void nt_trace_section_transfer(PVOID object,
                                      DWORD current_pid,
                                      DWORD source_pid,
                                      DWORD target_pid,
                                      HANDLE source_handle,
                                      HANDLE target_handle,
                                      ACCESS_MASK source_access,
                                      ACCESS_MASK desired_access,
                                      ULONG options,
                                      NTSTATUS status);

NTSTATUS sys_NtDuplicateObject(ULONG_PTR *args)
{
    HANDLE  SourceProcessHandle  = (HANDLE)args[0];
    HANDLE  SourceHandle         = (HANDLE)args[1];
    HANDLE  TargetProcessHandle  = (HANDLE)args[2];
    PHANDLE TargetHandle         = (PHANDLE)args[3];
    ACCESS_MASK DesiredAccess    = (ACCESS_MASK)args[4];
    /* ULONG  HandleAttributes   = (ULONG)args[5]; */
    ULONG   Options              = (ULONG)args[6];

    if (!TargetHandle && !(Options & DUPLICATE_CLOSE_SOURCE))
        return STATUS_INVALID_PARAMETER;

    extern DWORD win32_current_process_id(void);
    extern NTSTATUS nt_process_open_for_process(ULONG, ACCESS_MASK, ULONG,
                                                PHANDLE);
    extern NTSTATUS nt_thread_open_current_for_process(ACCESS_MASK, ULONG,
                                                       PHANDLE);
    DWORD current_pid = win32_current_process_id();
    if (!current_pid) current_pid = 1;
    DWORD source_pid = current_pid;
    DWORD target_pid = current_pid;

    NTSTATUS process_status = nt_duplicate_resolve_process(
        SourceProcessHandle, current_pid, &source_pid);
    if (!NT_SUCCESS(process_status)) {
        nt_duplicate_trace_failure(
            "source-process", process_status, current_pid, 0,
            target_pid, SourceProcessHandle, SourceHandle,
            TargetProcessHandle, TargetHandle, DesiredAccess, Options);
        return process_status;
    }
    process_status = nt_duplicate_resolve_process(
        TargetProcessHandle, current_pid, &target_pid);
    if (!NT_SUCCESS(process_status)) {
        nt_duplicate_trace_failure(
            "target-process", process_status, current_pid, source_pid,
            0, SourceProcessHandle, SourceHandle, TargetProcessHandle,
            TargetHandle, DesiredAccess, Options);
        return process_status;
    }

    HANDLE new_handle = NULL;
    NTSTATUS status;
    if (nt_is_current_process_handle(SourceHandle)) {
        ACCESS_MASK access = (Options & DUPLICATE_SAME_ACCESS)
                             ? GENERIC_ALL : DesiredAccess;
        status = nt_process_open_for_process(source_pid, access, target_pid,
                                             &new_handle);
    } else if (nt_is_current_thread_handle(SourceHandle)) {
        if (source_pid != current_pid) {
            status = STATUS_INVALID_HANDLE;
        } else {
            ACCESS_MASK access = (Options & DUPLICATE_SAME_ACCESS)
                                 ? GENERIC_ALL : DesiredAccess;
            status = nt_thread_open_current_for_process(access, target_pid,
                                                        &new_handle);
        }
    } else {
        HANDLE_ENTRY *source_entry =
            handle_get_entry(&g_handle_table, SourceHandle);
        status = handle_duplicate_for_process(
            &g_handle_table, SourceHandle, source_pid,
            &g_handle_table, &new_handle, target_pid,
            DesiredAccess, FALSE, Options);
        if (source_entry && source_entry->type == OBJ_TYPE_SECTION &&
            source_pid != target_pid) {
            nt_trace_section_transfer(source_entry->object, current_pid,
                                      source_pid, target_pid, SourceHandle,
                                      new_handle, source_entry->access,
                                      DesiredAccess, Options, status);
        }
    }
    if (NT_SUCCESS(status) && TargetHandle)
        *TargetHandle = new_handle;
    if (!NT_SUCCESS(status))
        nt_duplicate_trace_failure(
            "duplicate", status, current_pid, source_pid, target_pid,
            SourceProcessHandle, SourceHandle, TargetProcessHandle,
            TargetHandle, DesiredAccess, Options);

    return status;
}

/* ── NtProtectVirtualMemory ─────────────────────────────────── */

/* Convert NT page protection to x86 page flags */
static uint64_t nt_prot_to_page_flags(ULONG protect)
{
    uint64_t flags = 0x01;  /* Present */

    switch (protect & 0xFF) {
    case PAGE_READONLY:
    case PAGE_EXECUTE_READ:
        /* read-only: no write bit */
        break;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        flags |= 0x02;  /* Writable */
        break;
    case PAGE_NOACCESS:
        flags = 0;       /* Not present */
        break;
    default:
        break;
    }

    /* NX bit: set bit 63 if NOT executable */
    if (!(protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        flags |= (1ULL << 63);  /* NX */
    }

    return flags;
}

static ULONG nt_page_flags_to_protect(uint64_t pte)
{
    if (!(pte & PTE_PRESENT))
        return PAGE_NOACCESS;
    BOOL writable = (pte & PTE_WRITABLE) != 0;
    BOOL executable = (pte & PTE_NX) == 0;
    return executable
         ? (writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
         : (writable ? PAGE_READWRITE : PAGE_READONLY);
}

static BOOL nt_valid_page_protection(ULONG protect)
{
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return TRUE;
    default:
        return FALSE;
    }
}

static vm_track_entry_t vm_track_slice(const vm_track_entry_t *source,
                                        uint64_t start, uint64_t end,
                                        ULONG protect)
{
    vm_track_entry_t slice = *source;
    uint64_t delta = start - source->va;
    slice.va = start;
    slice.size = end - start;
    if (slice.section) {
        if (slice.phys)
            slice.phys += delta;
        slice.section_offset += delta;
    } else if (slice.owns_phys) {
        slice.phys = vm_track_page_phys(source, start);
    }
    slice.protect = protect;
    return slice;
}

/* Apply protection to a fully committed tracked range and split VMA records at
 * both boundaries. Returns 1 on success, 0 when the range is not tracked, and
 * -1 for inconsistent backing. Must hold vm_track_lock. */
static int vm_protect_tracked_range_locked(ULONG owner_pid, uint64_t cr3,
                                           uint64_t base, SIZE_T size,
                                           ULONG new_protect,
                                           ULONG *old_protect)
{
    uint64_t end = base + size;
    if (end < base || !vm_track_contains(base, size))
        return 0;

    vm_track_entry_t *first = vm_track_find_committed_locked(owner_pid, base);
    if (!first)
        return 0;
    *old_protect = first->protect;

    int extra_entries = 0;
    for (int i = 0; i < vm_track_count; i++) {
        vm_track_entry_t *entry = &vm_track[i];
        uint64_t entry_end = entry->va + entry->size;
        if (entry->owner_pid != owner_pid ||
            !vm_track_entry_committed(entry) || entry_end < entry->va ||
            base >= entry_end || entry->va >= end)
            continue;
        if (entry->va < base)
            extra_entries++;
        if (entry_end > end)
            extra_entries++;
    }
    if (vm_track_count + extra_entries > VM_TRACK_MAX)
        return -1;

    uint64_t pages = size / 4096;
    for (uint64_t page = 0; page < pages; page++) {
        uint64_t page_va = base + page * 4096;
        vm_track_entry_t *entry = vm_track_find_committed_locked(owner_pid,
                                                                  page_va);
        uint64_t *pte = nt_get_pte_in(cr3, page_va);
        if (!entry || ((!pte || !(*pte & PTE_ADDR_MASK)) && !entry->section))
            return -1;
    }

    uint64_t flags = nt_prot_to_page_flags(new_protect);
    for (uint64_t page = 0; page < pages; page++) {
        uint64_t page_va = base + page * 4096;
        uint64_t *pte = nt_get_pte_in(cr3, page_va);
        if (pte && (*pte & PTE_ADDR_MASK) &&
            nt_set_page_flags_in(cr3, page_va, flags) != 0)
            return -1;
    }

    int original_count = vm_track_count;
    for (int i = original_count - 1; i >= 0; i--) {
        vm_track_entry_t original = vm_track[i];
        uint64_t entry_end = original.va + original.size;
        if (original.owner_pid != owner_pid ||
            !vm_track_entry_committed(&original) || entry_end < original.va ||
            base >= entry_end || original.va >= end)
            continue;

        uint64_t middle_start = original.va > base ? original.va : base;
        uint64_t middle_end = entry_end < end ? entry_end : end;
        BOOL keep_left = original.va < middle_start;
        BOOL keep_right = middle_end < entry_end;

        if (keep_left) {
            vm_track[i] = vm_track_slice(&original, original.va, middle_start,
                                         original.protect);
            vm_track_entry_t middle = vm_track_slice(
                &original, middle_start, middle_end, new_protect);
            if (middle.section)
                section_view_retain_existing(middle.section);
            vm_track[vm_track_count++] = middle;
            if (keep_right) {
                vm_track_entry_t right = vm_track_slice(
                    &original, middle_end, entry_end, original.protect);
                if (right.section)
                    section_view_retain_existing(right.section);
                vm_track[vm_track_count++] = right;
            }
        } else if (keep_right) {
            vm_track[i] = vm_track_slice(&original, middle_start, middle_end,
                                         new_protect);
            vm_track_entry_t right = vm_track_slice(
                &original, middle_end, entry_end, original.protect);
            if (right.section)
                section_view_retain_existing(right.section);
            vm_track[vm_track_count++] = right;
        } else {
            vm_track[i].protect = new_protect;
        }
    }
    vm_track_coalesce_private_at_locked(owner_pid, base);
    vm_track_coalesce_private_at_locked(owner_pid, end - 1);
    return 1;
}

NTSTATUS sys_NtProtectVirtualMemory(ULONG_PTR *args)
{
    /* HANDLE ProcessHandle = (HANDLE)args[0]; */
    PVOID  *BaseAddress    = (PVOID *)args[1];
    SIZE_T *RegionSize     = (SIZE_T *)args[2];
    ULONG   NewProtect     = (ULONG)args[3];
    ULONG  *OldProtect     = (ULONG *)args[4];

    if (!BaseAddress || !*BaseAddress || !RegionSize || !*RegionSize ||
        !OldProtect || !nt_valid_page_protection(NewProtect))
        return STATUS_INVALID_PARAMETER;

    uint64_t requested = (uint64_t)*BaseAddress;
    uint64_t base = requested & ~0xFFFULL;
    SIZE_T size = *RegionSize + (requested - base);
    if (size < *RegionSize || size > UINT64_MAX - 0xFFFULL)
        return STATUS_INVALID_PARAMETER;
    size = (size + 0xFFF) & ~0xFFFULL;
    if (!size)
        return STATUS_INVALID_PARAMETER;
    uint64_t pages = size / 4096;

    ULONG owner_pid = nt_current_owner_pid();
    uint64_t cr3 = nt_current_cr3();
    uint64_t vm_irq_flags = vm_track_lock_irqsave();

    uint64_t protect_end = base + size;
    for (int i = 0; i < vm_track_count; i++) {
        vm_track_entry_t *entry = &vm_track[i];
        uint64_t entry_end = entry->va + entry->size;
        if (entry->owner_pid != owner_pid || !entry->section ||
            entry_end < entry->va || base >= entry_end ||
            entry->va >= protect_end)
            continue;
        if (!section_view_allows_protect(entry->section, NewProtect)) {
            vm_track_unlock_irqrestore(vm_irq_flags);
            return STATUS_ACCESS_DENIED;
        }
    }

    int tracked = vm_protect_tracked_range_locked(
        owner_pid, cr3, base, size, NewProtect, OldProtect);
    if (tracked == 0) {
        uint64_t image_base = 0;
        uint64_t image_size = 0;
        uint64_t next_image = 0;
        uint64_t range_end = base + size;
        BOOL image_range = pe_va_query_range(base, &image_base, &image_size,
                                             &next_image) &&
                           range_end >= base &&
                           range_end <= image_base +
                                        ((image_size + 0xFFFULL) & ~0xFFFULL);
        uint64_t *first_pte = image_range ? nt_get_pte_in(cr3, base) : NULL;
        if (!first_pte || !(*first_pte & PTE_ADDR_MASK)) {
            vm_track_unlock_irqrestore(vm_irq_flags);
            return STATUS_CONFLICTING_ADDRESSES;
        }
        *OldProtect = nt_page_flags_to_protect(*first_pte);
        for (uint64_t page = 0; page < pages; page++) {
            uint64_t *pte = nt_get_pte_in(cr3, base + page * 4096);
            if (!pte || !(*pte & PTE_ADDR_MASK)) {
                vm_track_unlock_irqrestore(vm_irq_flags);
                return STATUS_CONFLICTING_ADDRESSES;
            }
        }
        uint64_t flags = nt_prot_to_page_flags(NewProtect);
        for (uint64_t page = 0; page < pages; page++)
            nt_set_page_flags_in(cr3, base + page * 4096, flags);
    } else if (tracked < 0) {
        vm_track_unlock_irqrestore(vm_irq_flags);
        return STATUS_UNSUCCESSFUL;
    }

    vm_track_unlock_irqrestore(vm_irq_flags);

    *BaseAddress = (PVOID)base;
    *RegionSize = size;

#if defined(WIN32_VM_TRACE) && WIN32_VM_TRACE
    nt_log_hex("NtProtectVirtualMemory: base=", base);
    nt_log_hex("  new_protect=", NewProtect);
#endif

    return STATUS_SUCCESS;
}

/* ── NtQueryVirtualMemory ───────────────────────────────────── */

/* MEMORY_BASIC_INFORMATION for NtQueryVirtualMemory */
typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID       BaseAddress;
    PVOID       AllocationBase;
    ULONG       AllocationProtect;
    USHORT      PartitionId;
    USHORT      Padding0;
    SIZE_T      RegionSize;
    ULONG       State;          /* MEM_COMMIT, MEM_FREE, MEM_RESERVE */
    ULONG       Protect;
    ULONG       Type;           /* MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE */
    ULONG       Padding1;
} MEMORY_BASIC_INFORMATION;

#define MEM_PRIVATE     0x00020000
#define MEM_MAPPED      0x00040000
#define MEM_IMAGE       0x01000000

typedef enum _MEMORY_INFORMATION_CLASS {
    MemoryBasicInformation = 0,
} MEMORY_INFORMATION_CLASS;

NTSTATUS sys_NtQueryVirtualMemory(ULONG_PTR *args)
{
    /* HANDLE ProcessHandle          = (HANDLE)args[0]; */
    PVOID  BaseAddress               = (PVOID)args[1];
    MEMORY_INFORMATION_CLASS InfoClass = (MEMORY_INFORMATION_CLASS)args[2];
    PVOID  MemoryInformation         = (PVOID)args[3];
    SIZE_T MemoryInformationLength   = (SIZE_T)args[4];
    SIZE_T *ReturnLength             = (SIZE_T *)args[5];

    if (InfoClass != MemoryBasicInformation)
        return STATUS_INVALID_INFO_CLASS;

    if (ReturnLength)
        *ReturnLength = sizeof(MEMORY_BASIC_INFORMATION);

    if (MemoryInformationLength < sizeof(MEMORY_BASIC_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;

    if (!MemoryInformation)
        return STATUS_INVALID_PARAMETER;

    MEMORY_BASIC_INFORMATION *mbi = (MEMORY_BASIC_INFORMATION *)MemoryInformation;
    uint64_t addr = (uint64_t)BaseAddress;
    uint64_t page_base = addr & ~0xFFFULL;
    static uint32_t allocator_query_logs;
    int trace_query = 0;
    if (addr >= 0x50000000ULL && addr < WIN32_VA_LIMIT &&
        __sync_fetch_and_add(&allocator_query_logs, 1) < 128)
        trace_query = 1;

    if (trace_query) {
        serial_puts("[VQ-CALL] addr=0x");
        serial_puthex(addr, 16);
        serial_puts("\n");
    }

    mbi->PartitionId       = 0;
    mbi->Padding0          = 0;
    mbi->Padding1          = 0;

    ULONG owner_pid = nt_current_owner_pid();
    uint64_t cr3 = nt_current_cr3();
    vm_track_entry_t committed = {0};
    vm_track_entry_t reservation = {0};
    int have_committed = 0;
    int have_reservation = 0;
    uint64_t next_mapping = 0;
    uint64_t image_base = 0;
    uint64_t image_size = 0;
    uint64_t next_image = 0;
    int have_image = pe_va_query_range(page_base, &image_base, &image_size,
                                       &next_image);

    uint64_t vm_irq_flags = vm_track_lock_irqsave();
    for (int i = 0; i < vm_track_count; i++) {
        if (vm_track[i].owner_pid != owner_pid)
            continue;
        uint64_t end = vm_track[i].va + vm_track[i].size;
        if (end < vm_track[i].va)
            continue;
        if (page_base >= vm_track[i].va && page_base < end) {
            if (vm_track_entry_committed(&vm_track[i])) {
                if (!have_committed || vm_track[i].size < committed.size) {
                    committed = vm_track[i];
                    have_committed = 1;
                }
            } else if (!have_reservation ||
                       vm_track[i].size < reservation.size) {
                reservation = vm_track[i];
                have_reservation = 1;
            }
        } else if (vm_track[i].va > page_base &&
                   (!next_mapping || vm_track[i].va < next_mapping)) {
            next_mapping = vm_track[i].va;
        }
    }

    if (next_image && (!next_mapping || next_image < next_mapping))
        next_mapping = next_image;

    if (have_committed) {
        uint64_t region_end = committed.va + committed.size;
        BOOL expanded;
        do {
            expanded = FALSE;
            for (int i = 0; i < vm_track_count; i++) {
                uint64_t entry_end = vm_track[i].va + vm_track[i].size;
                if (vm_track[i].owner_pid != owner_pid ||
                    !vm_track_entry_committed(&vm_track[i]) ||
                    vm_track[i].protect != committed.protect ||
                    vm_track[i].section != committed.section ||
                    vm_track[i].allocation_base != committed.allocation_base ||
                    entry_end < vm_track[i].va)
                    continue;
                if (vm_track[i].va == region_end) {
                    region_end = entry_end;
                    expanded = TRUE;
                }
            }
        } while (expanded);

        mbi->BaseAddress = (PVOID)page_base;
        mbi->AllocationBase = (PVOID)committed.allocation_base;
        mbi->AllocationProtect = have_reservation
                                 ? reservation.allocation_protect
                                 : committed.allocation_protect;
        mbi->RegionSize = region_end - page_base;
        mbi->State = MEM_COMMIT;
        mbi->Protect = committed.protect;
        mbi->Type = committed.section ? MEM_MAPPED : MEM_PRIVATE;

    } else if (have_reservation) {
        uint64_t region_end = reservation.va + reservation.size;

        /* VirtualQuery starts at the queried page and scans forward. Stop at
         * the next committed chunk inside this allocation. */
        for (int i = 0; i < vm_track_count; i++) {
            if (vm_track[i].owner_pid != owner_pid)
                continue;
            uint64_t end = vm_track[i].va + vm_track[i].size;
            if (!vm_track_entry_committed(&vm_track[i]) ||
                end < vm_track[i].va ||
                vm_track[i].va < reservation.va || end > region_end)
                continue;
            if (vm_track[i].va > page_base && vm_track[i].va < region_end)
                region_end = vm_track[i].va;
        }

        mbi->BaseAddress = (PVOID)page_base;
        mbi->AllocationBase = (PVOID)reservation.allocation_base;
        mbi->AllocationProtect = reservation.allocation_protect;
        mbi->RegionSize = region_end - page_base;
        mbi->State = MEM_RESERVE;
        mbi->Protect = 0;
        mbi->Type = MEM_PRIVATE;
    }
    vm_track_unlock_irqrestore(vm_irq_flags);

    if (have_committed || have_reservation) {
        if (trace_query) {
            serial_puts("[VQ] addr=0x");
            serial_puthex(addr, 16);
            serial_puts(" base=0x");
            serial_puthex((uint64_t)mbi->BaseAddress, 16);
            serial_puts(" alloc=0x");
            serial_puthex((uint64_t)mbi->AllocationBase, 16);
            serial_puts(" size=0x");
            serial_puthex(mbi->RegionSize, 16);
            serial_puts(" prot=0x");
            serial_puthex(mbi->Protect, 8);
            serial_puts("\n");
        }
        return STATUS_SUCCESS;
    }

    /* PE images are tracked separately, but pe_va_query_range applies the
     * same Win32 owner boundary as the VMA tracker. */
    if (have_image) {
        uint64_t image_end = image_base +
                             ((image_size + 0xFFFULL) & ~0xFFFULL);
        uint64_t region_start = page_base;
        uint64_t region_end = page_base + 4096;
        uint64_t *image_pte = nt_get_pte_in(cr3, page_base);
        BOOL committed = image_pte && (*image_pte & PTE_ADDR_MASK);
        ULONG protect = committed
                        ? nt_page_flags_to_protect(*image_pte) : 0;

        /* Section protections are encoded in the process page tables. Report
         * only the contiguous run with matching attributes so callers such as
         * MinGW's pseudo-relocator do not restore .data/.bss as executable
         * read-only memory after temporarily unprotecting .text. */
        while (region_start > image_base) {
            uint64_t previous = region_start - 4096;
            uint64_t *pte = nt_get_pte_in(cr3, previous);
            BOOL page_committed = pte && (*pte & PTE_ADDR_MASK);
            if (page_committed != committed ||
                (committed && nt_page_flags_to_protect(*pte) != protect))
                break;
            region_start = previous;
        }
        while (region_end < image_end) {
            uint64_t *pte = nt_get_pte_in(cr3, region_end);
            BOOL page_committed = pte && (*pte & PTE_ADDR_MASK);
            if (page_committed != committed ||
                (committed && nt_page_flags_to_protect(*pte) != protect))
                break;
            region_end += 4096;
        }

        mbi->BaseAddress = (PVOID)region_start;
        mbi->AllocationBase = (PVOID)image_base;
        mbi->AllocationProtect = PAGE_EXECUTE_READ;
        mbi->RegionSize = region_end - region_start;
        mbi->State = committed ? MEM_COMMIT : MEM_RESERVE;
        mbi->Protect = protect;
        mbi->Type = MEM_IMAGE;
        if (trace_query) {
            serial_puts("[VQ-IMAGE] addr=0x");
            serial_puthex(addr, 16);
            serial_puts(" base=0x");
            serial_puthex(region_start, 16);
            serial_puts(" size=0x");
            serial_puthex(mbi->RegionSize, 16);
            serial_puts(" prot=0x");
            serial_puthex(protect, 8);
            serial_puts("\n");
        }
        return STATUS_SUCCESS;
    }

    /* Preserve compatibility for mapped kernel/section pages that predate
     * vm_track. They remain page-sized until their owners gain VMA records. */
    uint64_t *pte = nt_get_pte_in(cr3, page_base);
    if (pte && (*pte & PTE_PRESENT)) {
        int writable = (*pte & PTE_WRITABLE) != 0;
        int executable = (*pte & PTE_NX) == 0;
        ULONG protect = executable
                        ? (writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                        : (writable ? PAGE_READWRITE : PAGE_READONLY);
        mbi->BaseAddress = (PVOID)page_base;
        mbi->AllocationBase = (PVOID)page_base;
        mbi->AllocationProtect = protect;
        mbi->RegionSize = 4096;
        mbi->State = MEM_COMMIT;
        mbi->Protect = protect;
        mbi->Type = MEM_PRIVATE;
        return STATUS_SUCCESS;
    }

    uint64_t region_end = page_base < WIN32_VA_LIMIT
                          ? WIN32_VA_LIMIT : WIN64_FIXED_VA_LIMIT;
    if (next_mapping > page_base && next_mapping < region_end)
        region_end = next_mapping;
    if (region_end <= page_base)
        region_end = page_base + 4096;

    mbi->BaseAddress = (PVOID)page_base;
    mbi->AllocationBase = NULL;
    mbi->AllocationProtect = 0;
    mbi->RegionSize = region_end - page_base;
    mbi->State = MEM_FREE;
    mbi->Protect = 0;
    mbi->Type = 0;

    return STATUS_SUCCESS;
}

/* ── NtYieldExecution ───────────────────────────────────────── */

static void vm_test_expect(BOOL condition, const char *name,
                           int *checks, int *failures)
{
    (*checks)++;
    if (condition)
        return;
    (*failures)++;
    serial_puts("[VMTEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

static NTSTATUS vm_test_allocate(PVOID *base, SIZE_T *size, ULONG type,
                                 ULONG protect)
{
    ULONG_PTR args[6] = {
        (ULONG_PTR)NT_CURRENT_PROCESS, (ULONG_PTR)base, 0,
        (ULONG_PTR)size, type, protect
    };
    return sys_NtAllocateVirtualMemory(args);
}

static NTSTATUS vm_test_free(PVOID *base, SIZE_T *size, ULONG type)
{
    ULONG_PTR args[4] = {
        (ULONG_PTR)NT_CURRENT_PROCESS, (ULONG_PTR)base,
        (ULONG_PTR)size, type
    };
    return sys_NtFreeVirtualMemory(args);
}

static NTSTATUS vm_test_protect(PVOID *base, SIZE_T *size, ULONG protect,
                                ULONG *old_protect)
{
    ULONG_PTR args[5] = {
        (ULONG_PTR)NT_CURRENT_PROCESS, (ULONG_PTR)base,
        (ULONG_PTR)size, protect, (ULONG_PTR)old_protect
    };
    return sys_NtProtectVirtualMemory(args);
}

static NTSTATUS vm_test_query(PVOID base, MEMORY_BASIC_INFORMATION *info)
{
    SIZE_T returned = 0;
    ULONG_PTR args[6] = {
        (ULONG_PTR)NT_CURRENT_PROCESS, (ULONG_PTR)base,
        MemoryBasicInformation, (ULONG_PTR)info, sizeof(*info),
        (ULONG_PTR)&returned
    };
    return sys_NtQueryVirtualMemory(args);
}

int nt_vm_selftest(void)
{
    int checks = 0;
    int failures = 0;
    PVOID allocation = NULL;
    PVOID implicit_commit_allocation = NULL;
    PVOID noaccess_allocation = NULL;
    PVOID stack_allocation = NULL;
    PVOID stack_limit = NULL;
    PVOID stack_base = NULL;
    SIZE_T size = 4 * 4096;
    MEMORY_BASIC_INFORMATION info;
    NTSTATUS status;

    serial_puts("[VMTEST] starting NT virtual-memory contract test\n");
    vm_test_expect(
        vm_address_allowed(0x00000001F0000000ULL, 0x0FFE0000ULL,
                           TRUE, FALSE),
        "allow explicit low Win64 reservation", &checks, &failures);
    vm_test_expect(
        !vm_address_allowed(0x00000001F0000000ULL, 0x0FFE0000ULL,
                            TRUE, TRUE),
        "reject out-of-range Win32 reservation", &checks, &failures);
    vm_test_expect(
        vm_address_allowed(WIN32_VA_BASE + 0x04000000ULL,
                           0x40000000ULL, FALSE, TRUE),
        "Win32 arena admits a 1 GiB reservation", &checks, &failures);

    size = 4096;
    status = vm_test_allocate(&implicit_commit_allocation, &size,
                              MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    vm_test_expect(NT_SUCCESS(status) && implicit_commit_allocation &&
                   size == 4096,
                   "commit with NULL base implicitly reserves",
                   &checks, &failures);
    if (NT_SUCCESS(status) && implicit_commit_allocation) {
        status = vm_test_query(implicit_commit_allocation, &info);
        vm_test_expect(NT_SUCCESS(status) && info.State == MEM_COMMIT &&
                       info.Protect == PAGE_EXECUTE_READWRITE &&
                       info.AllocationBase == implicit_commit_allocation,
                       "query implicit commit allocation",
                       &checks, &failures);

        PVOID implicit_release = implicit_commit_allocation;
        PVOID released_implicit_address = implicit_commit_allocation;
        size = 0;
        status = vm_test_free(&implicit_release, &size, MEM_RELEASE);
        vm_test_expect(NT_SUCCESS(status) && implicit_release == NULL,
                       "release implicit commit allocation",
                       &checks, &failures);
        if (NT_SUCCESS(status))
            implicit_commit_allocation = NULL;

        size = 4096;
        status = vm_test_allocate(&released_implicit_address, &size,
                                  MEM_COMMIT, PAGE_READWRITE);
        vm_test_expect(!NT_SUCCESS(status),
                       "commit rejects explicit unreserved address",
                       &checks, &failures);
    }

    size = 4 * 4096;
    status = vm_test_allocate(&allocation, &size, MEM_RESERVE, PAGE_READWRITE);
    vm_test_expect(NT_SUCCESS(status) && allocation && size == 4 * 4096,
                   "reserve four pages", &checks, &failures);
    if (!NT_SUCCESS(status) || !allocation)
        goto cleanup;

    status = vm_test_query(allocation, &info);
    vm_test_expect(NT_SUCCESS(status) && info.State == MEM_RESERVE &&
                   info.AllocationBase == allocation &&
                   info.RegionSize == 4 * 4096,
                   "query reservation", &checks, &failures);

    PVOID committed = (PVOID)((uint64_t)(ULONG_PTR)allocation + 4096);
    size = 2 * 4096;
    status = vm_test_allocate(&committed, &size, MEM_COMMIT, PAGE_READWRITE);
    vm_test_expect(NT_SUCCESS(status), "commit reservation middle",
                   &checks, &failures);
    if (NT_SUCCESS(status)) {
        volatile BYTE *bytes = (volatile BYTE *)committed;
        bytes[0] = 0x5A;
        bytes[4096] = 0xA5;
    }

    status = vm_test_query(committed, &info);
    vm_test_expect(NT_SUCCESS(status) && info.State == MEM_COMMIT &&
                   info.Protect == PAGE_READWRITE &&
                   info.AllocationBase == allocation &&
                   info.RegionSize == 2 * 4096,
                   "query committed span", &checks, &failures);

    PVOID protected_page = (PVOID)((uint64_t)(ULONG_PTR)committed + 4096);
    size = 4096;
    ULONG old_protect = 0;
    status = vm_test_protect(&protected_page, &size, PAGE_READONLY,
                             &old_protect);
    vm_test_expect(NT_SUCCESS(status) && old_protect == PAGE_READWRITE,
                   "protect committed subrange", &checks, &failures);
    status = vm_test_query(protected_page, &info);
    vm_test_expect(NT_SUCCESS(status) && info.Protect == PAGE_READONLY &&
                   info.RegionSize == 4096,
                   "query split protection", &checks, &failures);

    PVOID decommit = committed;
    size = 4096;
    status = vm_test_free(&decommit, &size, MEM_DECOMMIT);
    vm_test_expect(NT_SUCCESS(status), "decommit one page",
                   &checks, &failures);
    uint64_t *pte = nt_get_pte_in(nt_current_cr3(),
                                  (uint64_t)(ULONG_PTR)committed);
    vm_test_expect(!pte || !(*pte & PTE_PRESENT), "decommit removes PTE",
                   &checks, &failures);
    status = vm_test_query(committed, &info);
    vm_test_expect(NT_SUCCESS(status) && info.State == MEM_RESERVE &&
                   info.RegionSize == 4096,
                   "query decommitted gap", &checks, &failures);

    PVOID recommit = committed;
    size = 4096;
    status = vm_test_allocate(&recommit, &size, MEM_COMMIT, PAGE_READWRITE);
    vm_test_expect(NT_SUCCESS(status), "recommit decommitted page",
                   &checks, &failures);
    if (NT_SUCCESS(status)) {
        volatile BYTE *byte = (volatile BYTE *)recommit;
        vm_test_expect(*byte == 0, "recommit is zero-filled",
                       &checks, &failures);
        *byte = 0x3C;
    }

    PVOID existing = committed;
    size = 2 * 4096;
    status = vm_test_allocate(&existing, &size, MEM_COMMIT, PAGE_EXECUTE_READ);
    vm_test_expect(NT_SUCCESS(status) &&
                   *(volatile BYTE *)committed == 0x3C,
                   "recommit preserves committed contents",
                   &checks, &failures);
    status = vm_test_query(committed, &info);
    vm_test_expect(NT_SUCCESS(status) &&
                   info.Protect == PAGE_EXECUTE_READ,
                   "recommit updates existing protection",
                   &checks, &failures);
    status = vm_test_query(protected_page, &info);
    vm_test_expect(NT_SUCCESS(status) &&
                   info.Protect == PAGE_EXECUTE_READ,
                   "recommit updates split protection",
                   &checks, &failures);

    PVOID invalid_range = allocation;
    size = 2 * 4096;
    old_protect = 0;
    status = vm_test_protect(&invalid_range, &size, PAGE_READONLY,
                             &old_protect);
    vm_test_expect(!NT_SUCCESS(status), "protect rejects reserved hole",
                   &checks, &failures);

    PVOID invalid_release = allocation;
    size = 4096;
    status = vm_test_free(&invalid_release, &size, MEM_RELEASE);
    vm_test_expect(!NT_SUCCESS(status) && invalid_release == allocation,
                   "release requires zero size", &checks, &failures);
    invalid_release = (PVOID)((uint64_t)(ULONG_PTR)allocation + 4096);
    size = 0;
    status = vm_test_free(&invalid_release, &size, MEM_RELEASE);
    vm_test_expect(!NT_SUCCESS(status) && invalid_release != NULL,
                   "release requires allocation base", &checks, &failures);

    PVOID released_address = allocation;
    PVOID released_base = allocation;
    size = 0;
    status = vm_test_free(&released_base, &size, MEM_RELEASE);
    vm_test_expect(NT_SUCCESS(status) && released_base == NULL,
                   "release full reservation", &checks, &failures);
    if (NT_SUCCESS(status))
        allocation = NULL;
    status = vm_test_query(released_address, &info);
    vm_test_expect(NT_SUCCESS(status) && info.State == MEM_FREE,
                   "query free range after release", &checks, &failures);

    size = 4096;
    status = vm_test_allocate(&noaccess_allocation, &size,
                              MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    vm_test_expect(NT_SUCCESS(status) && noaccess_allocation,
                   "reserve+commit noaccess", &checks, &failures);
    if (NT_SUCCESS(status)) {
        status = vm_test_query(noaccess_allocation, &info);
        vm_test_expect(NT_SUCCESS(status) && info.State == MEM_COMMIT &&
                       info.Protect == PAGE_NOACCESS,
                       "query noaccess commit", &checks, &failures);
        PVOID writable = noaccess_allocation;
        size = 4096;
        old_protect = 0;
        status = vm_test_protect(&writable, &size, PAGE_READWRITE,
                                 &old_protect);
        vm_test_expect(NT_SUCCESS(status) && old_protect == PAGE_NOACCESS,
                       "restore access to noaccess page", &checks, &failures);
        if (NT_SUCCESS(status)) {
            *(volatile BYTE *)noaccess_allocation = 0xC3;
            vm_test_expect(*(volatile BYTE *)noaccess_allocation == 0xC3,
                           "restored page is writable", &checks, &failures);
        }
    }

    status = nt_vm_allocate_stack(256 * 1024, &stack_allocation,
                                  &stack_limit, &stack_base);
    vm_test_expect(NT_SUCCESS(status) && stack_allocation && stack_limit &&
                   stack_base && stack_limit > stack_allocation &&
                   stack_base > stack_limit,
                   "allocate process-private stack", &checks, &failures);
    if (NT_SUCCESS(status)) {
        PVOID stack_cursor = (PVOID)((uint64_t)(ULONG_PTR)stack_base - 64);
        status = vm_test_query(stack_cursor, &info);
        vm_test_expect(NT_SUCCESS(status) && info.State == MEM_COMMIT &&
                       info.Protect == PAGE_READWRITE &&
                       info.AllocationBase == stack_allocation &&
                       (uint64_t)(ULONG_PTR)stack_base -
                           (uint64_t)(ULONG_PTR)info.AllocationBase > 0x4000,
                       "query Win32 stack allocation base", &checks,
                       &failures);

        size = 1;
        old_protect = 0;
        status = vm_test_protect(&stack_cursor, &size, PAGE_READONLY,
                                 &old_protect);
        vm_test_expect(NT_SUCCESS(status) &&
                       old_protect == PAGE_READWRITE,
                       "protect Win32 stack page reports RW", &checks,
                       &failures);
        if (NT_SUCCESS(status)) {
            size = 1;
            old_protect = 0;
            status = vm_test_protect(&stack_cursor, &size, PAGE_READWRITE,
                                     &old_protect);
            vm_test_expect(NT_SUCCESS(status) &&
                           old_protect == PAGE_READONLY,
                           "restore Win32 stack page protection", &checks,
                           &failures);
        }

        status = vm_test_query(stack_allocation, &info);
        vm_test_expect(NT_SUCCESS(status) && info.State == MEM_RESERVE &&
                       info.AllocationBase == stack_allocation &&
                       info.RegionSize == 4096,
                       "query Win32 stack guard page", &checks, &failures);
    }

cleanup:
    if (allocation) {
        size = 0;
        (void)vm_test_free(&allocation, &size, MEM_RELEASE);
    }
    if (implicit_commit_allocation) {
        size = 0;
        (void)vm_test_free(&implicit_commit_allocation, &size, MEM_RELEASE);
    }
    if (noaccess_allocation) {
        size = 0;
        (void)vm_test_free(&noaccess_allocation, &size, MEM_RELEASE);
    }
    if (stack_allocation)
        (void)nt_vm_free_stack(stack_allocation);
    serial_puts("[VMTEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

NTSTATUS sys_NtYieldExecution(ULONG_PTR *args)
{
    (void)args;
    sched_yield();
    return STATUS_SUCCESS;
}

/* ── NtTerminateProcess ─────────────────────────────────────── */

extern void proc_exit(int32_t code);
int32_t win32_last_exit_code;

NTSTATUS sys_NtTerminateProcess(ULONG_PTR *args)
{
    HANDLE ProcessHandle = (HANDLE)args[0];
    NTSTATUS ExitStatus   = (NTSTATUS)args[1];

    nt_log_hex("NtTerminateProcess: exit code = ", (ULONGLONG)ExitStatus);

    BOOL terminate_current = ProcessHandle == NT_CURRENT_PROCESS ||
                             !ProcessHandle;
    if (!terminate_current) {
        extern BOOL nt_process_id(HANDLE handle, DWORD *process_id);
        DWORD process_id = 0;
        terminate_current = nt_process_id(ProcessHandle, &process_id) &&
                            process_id == win32_current_process_id();
    }

    if (terminate_current) {
        extern BOOL win32_terminate_current_child(NTSTATUS status);
        if (win32_terminate_current_child(ExitStatus))
            return STATUS_SUCCESS;

        extern BOOL win32_terminate_current_main(NTSTATUS status);
        if (win32_terminate_current_main(ExitStatus))
            return STATUS_SUCCESS;

        /* A native kernel task can still reach this path without winexec. */
        proc_exit((int32_t)ExitStatus);
        __builtin_unreachable();
    }

    extern BOOL win32_terminate_child(HANDLE process_handle, NTSTATUS status);
    if (win32_terminate_child(ProcessHandle, ExitStatus))
        return STATUS_SUCCESS;

    return STATUS_INVALID_HANDLE;
}

/* ── NtDelayExecution (Sleep) ───────────────────────────────── */

extern uint64_t idt_get_ticks(void);
extern uint64_t idt_get_tsc_freq(void);
extern int sched_sleep_ticks(uint64_t ticks);

static inline uint64_t nt_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void nt_win32_exit_checkpoint(void)
{
    extern void win32_main_termination_checkpoint(void);
    win32_main_termination_checkpoint();
}

NTSTATUS sys_NtDelayExecution(ULONG_PTR *args)
{
    /* BOOLEAN Alertable = (BOOLEAN)args[0]; */
    PLARGE_INTEGER DelayInterval = (PLARGE_INTEGER)args[1];

    if (!DelayInterval)
        return STATUS_INVALID_PARAMETER;

    /* Negative = relative time in 100ns units. Absolute deadlines are not yet
     * backed by a wall clock, so preserve the existing duration behavior. */
    LONGLONG interval = DelayInterval->QuadPart;
    uint64_t delay = interval < 0
        ? (uint64_t)(-(interval + 1)) + 1
        : (uint64_t)interval;

    if (!delay) {
        sched_yield();
        nt_win32_exit_checkpoint();
        return STATUS_SUCCESS;
    }

    /* The scheduler clock is 100Hz. Rounding up matches Windows' guarantee
     * that a relative delay does not complete before its requested interval. */
    uint64_t delay_ticks = delay / 100000ULL;
    if (delay % 100000ULL)
        delay_ticks++;
    if (sched_sleep_ticks(delay_ticks) == 0) {
        nt_win32_exit_checkpoint();
        return STATUS_SUCCESS;
    }

    uint64_t tsc_freq = idt_get_tsc_freq();
    if (!tsc_freq) tsc_freq = 3000000000ULL;
    uint64_t delay_cycles = (delay / 10000000ULL) * tsc_freq +
                            (delay % 10000000ULL) * tsc_freq / 10000000ULL;
    if (delay_cycles == 0) delay_cycles = 1;

    uint64_t start = nt_rdtsc();

    /* Scheduler unavailable (or no runnable peer): retain the cooperative TSC
     * fallback used by early boot and UT99's timer-masked compat32 mode. */
    sched_yield();
    nt_win32_exit_checkpoint();
    uint64_t last_yield = nt_rdtsc();
    uint64_t yield_period = tsc_freq / 1000;  /* 1 ms */

    for (;;) {
        uint64_t now = nt_rdtsc();
        if (now - start >= delay_cycles) break;
        if (now - last_yield >= yield_period) {
            sched_yield();
            nt_win32_exit_checkpoint();
            last_yield = nt_rdtsc();
        } else {
            __asm__ volatile("pause" ::: "memory");
        }
    }

    return STATUS_SUCCESS;
}

/* ── NtQueryPerformanceCounter ──────────────────────────────── */

NTSTATUS sys_NtQueryPerformanceCounter(ULONG_PTR *args)
{
    PLARGE_INTEGER PerformanceCounter   = (PLARGE_INTEGER)args[0];
    PLARGE_INTEGER PerformanceFrequency = (PLARGE_INTEGER)args[1];

    if (PerformanceCounter) {
        /* Use rdtsc for monotonic counter — idt_get_ticks doesn't advance
         * during compat32 (APIC timer blocked in compatibility mode) */
        PerformanceCounter->QuadPart = (LONGLONG)nt_rdtsc();
    }

    if (PerformanceFrequency)
        PerformanceFrequency->QuadPart = (LONGLONG)idt_get_tsc_freq();

    return STATUS_SUCCESS;
}

/* ── Section objects (memory-mapped files) ──────────────────── */

/*
 * SEC_* allocation attributes (NtCreateSection AllocationAttributes).
 * These match the Windows SDK values.
 */
#define SEC_COMMIT      0x8000000
#define SEC_IMAGE       0x1000000
#define SEC_RESERVE     0x4000000

/* Large data mappings are committed virtually and materialized on demand.
 * Pagefile sections start zeroed; file-backed sections page in their data.
 * Image sections retain contiguous backing for loader compatibility. */
#define SECTION_SPARSE_MIN_SIZE (1024ULL * 1024ULL)
#define SECTION_FAULT_BATCH_PAGES 32
#define SECTION_CONTROL_CACHE_ENTRIES 32
#define SECTION_CONTROL_CACHE_LIMIT (256ULL * 1024ULL * 1024ULL)

typedef struct _SECTION_PAGE {
    struct _SECTION_PAGE *next;
    uint64_t phys;
    uint32_t index;
} SECTION_PAGE;

/* Windows keeps the file-backed segment separate from individual section
 * handles and views. This bounded control-area cache provides the same
 * ownership boundary for immutable sparse mappings: objects remain distinct,
 * while clean file pages survive close/unmap churn and are shared coherently. */
typedef struct _SECTION_CONTROL_AREA {
    void         *file;
    uint64_t      revision;
    uint64_t      size;
    uint64_t      file_size;
    SECTION_PAGE *sparse_pages;
    uint64_t      materialized_pages;
    uint64_t      last_use;
    uint32_t      object_refs;
    BOOL          in_use;
} SECTION_CONTROL_AREA;

typedef struct _SECTION_OBJECT {
    uint64_t    identity;       /* changes whenever this pool slot is reused */
    uint64_t    size;           /* section size in bytes */
    uint64_t    file_size;      /* logical file-backed extent */
    uint64_t    backing_phys;   /* page-aligned shared physical backing */
    uint32_t    flags;          /* SEC_COMMIT, SEC_IMAGE, SEC_RESERVE */
    uint32_t    protect;        /* PAGE_READWRITE, PAGE_READONLY, etc. */
    void       *file;           /* osfs2_file_t* if file-backed, NULL if pagefile */
    SECTION_PAGE *sparse_pages; /* materialized demand-zero pages */
    SECTION_CONTROL_AREA *control; /* shared immutable file backing */
    uint32_t    view_refs;
    BOOL        sparse;
    BOOL        deny_access_escalation;
    BOOL        handles_closed;
    BOOL        in_use;
} SECTION_OBJECT;

static void nt_trace_section_transfer(PVOID object,
                                      DWORD current_pid,
                                      DWORD source_pid,
                                      DWORD target_pid,
                                      HANDLE source_handle,
                                      HANDLE target_handle,
                                      ACCESS_MASK source_access,
                                      ACCESS_MASK desired_access,
                                      ULONG options,
                                      NTSTATUS status)
{
    static uint32_t transfer_count;
    if (__atomic_fetch_add(&transfer_count, 1, __ATOMIC_RELAXED) >= 512)
        return;

    SECTION_OBJECT *sec = (SECTION_OBJECT *)object;
    serial_puts("[SECTION-XFER] current=");
    serial_putdec(current_pid);
    serial_puts(" source_pid=");
    serial_putdec(source_pid);
    serial_puts(" target_pid=");
    serial_putdec(target_pid);
    serial_puts(" source=0x");
    serial_puthex((uint64_t)(ULONG_PTR)source_handle, 16);
    serial_puts(" target=0x");
    serial_puthex((uint64_t)(ULONG_PTR)target_handle, 16);
    serial_puts(" id=0x");
    serial_puthex(sec && sec->in_use ? sec->identity : 0, 16);
    serial_puts(" size=0x");
    serial_puthex(sec && sec->in_use ? sec->size : 0, 16);
    serial_puts(" sparse=");
    serial_putdec(sec && sec->in_use ? sec->sparse : 0);
    serial_puts(" restricted=");
    serial_putdec(sec && sec->in_use ? sec->deny_access_escalation : 0);
    serial_puts(" source_access=0x");
    serial_puthex(source_access, 8);
    serial_puts(" desired=0x");
    serial_puthex(desired_access, 8);
    serial_puts(" options=0x");
    serial_puthex(options, 8);
    serial_puts(" status=0x");
    serial_puthex((uint32_t)status, 8);
    serial_puts("\n");
}

typedef struct _SECTION_CLEANUP {
    uint64_t backing_phys;
    uint64_t size;
    void    *file;
    SECTION_PAGE *sparse_pages;
    BOOL trim_controls;
} SECTION_CLEANUP;

typedef struct _SECTION_CONTROL_CLEANUP {
    void         *file;
    SECTION_PAGE *sparse_pages;
} SECTION_CONTROL_CLEANUP;

#define SECTION_POOL_MAX MAX_HANDLES
static SECTION_OBJECT section_pool[SECTION_POOL_MAX];
static SECTION_CONTROL_AREA section_control_cache[SECTION_CONTROL_CACHE_ENTRIES];
static spinlock_t section_lock = SPINLOCK_INIT;
static uint64_t section_next_identity;
static uint64_t section_control_clock;
static uint64_t section_control_cache_bytes;
static uint64_t section_control_hits;
static uint64_t section_control_misses;
static uint64_t section_control_bypasses;
static uint64_t section_control_evictions;
static uint64_t section_control_page_hits;
static uint64_t section_control_disk_bytes;
static uint64_t section_control_saved_bytes;

static inline uint64_t section_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&section_lock);
    return flags;
}

static inline void section_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&section_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static void section_page_list_free(SECTION_PAGE *pages)
{
    while (pages) {
        SECTION_PAGE *page = pages;
        pages = page->next;
        if (page->phys)
            mem_free_pages((void *)(uintptr_t)page->phys, 1);
        kfree(page);
    }
}

static void section_control_cleanup_finish(SECTION_CONTROL_CLEANUP *cleanup)
{
    if (!cleanup)
        return;
    if (cleanup->file)
        osfs2_file_release(cleanup->file);
    section_page_list_free(cleanup->sparse_pages);
    cleanup->file = NULL;
    cleanup->sparse_pages = NULL;
}

static void section_control_collect_locked(
    SECTION_CONTROL_AREA *control, SECTION_CONTROL_CLEANUP *cleanup)
{
    if (!control || !control->in_use || control->object_refs)
        return;
    cleanup->file = control->file;
    cleanup->sparse_pages = control->sparse_pages;
    uint64_t bytes = control->materialized_pages * 4096;
    section_control_cache_bytes = bytes <= section_control_cache_bytes
        ? section_control_cache_bytes - bytes : 0;
    nt_memset(control, 0, sizeof(*control));
}

static BOOL section_control_evict_one(BOOL force)
{
    SECTION_CONTROL_CLEANUP cleanup = {0};
    uint64_t irq_flags = section_lock_irqsave();
    if (!force &&
        section_control_cache_bytes <= SECTION_CONTROL_CACHE_LIMIT) {
        section_unlock_irqrestore(irq_flags);
        return FALSE;
    }

    SECTION_CONTROL_AREA *victim = NULL;
    for (int i = 0; i < SECTION_CONTROL_CACHE_ENTRIES; i++) {
        SECTION_CONTROL_AREA *control = &section_control_cache[i];
        if (!control->in_use || control->object_refs)
            continue;
        if (!victim || control->last_use < victim->last_use)
            victim = control;
    }
    if (victim) {
        section_control_collect_locked(victim, &cleanup);
        section_control_evictions++;
    }
    section_unlock_irqrestore(irq_flags);
    section_control_cleanup_finish(&cleanup);
    return victim != NULL;
}

static void section_control_trim(void)
{
    while (section_control_evict_one(FALSE)) {
    }
}

static BOOL section_control_read_only_protect(ULONG protect)
{
    ULONG base = protect & 0xFF;
    return base == PAGE_NOACCESS || base == PAGE_READONLY ||
           base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ;
}

static BOOL section_view_allows_protect(PVOID section, ULONG protect)
{
    SECTION_OBJECT *sec = (SECTION_OBJECT *)section;
    uint64_t irq_flags = section_lock_irqsave();
    BOOL allowed = sec && sec->in_use &&
                   (!sec->control ||
                    section_control_read_only_protect(protect));
    section_unlock_irqrestore(irq_flags);
    return allowed;
}

/* The caller owns one file retain. On a cache miss it is transferred into the
 * new control area; on a hit the caller keeps it and must release it. */
static SECTION_CONTROL_AREA *section_control_acquire(
    void *file, uint64_t size, uint64_t file_size, BOOL *file_ref_consumed)
{
    if (file_ref_consumed)
        *file_ref_consumed = FALSE;
    uint64_t revision = osfs2_file_revision(file);
    if (!file || (revision & 1)) {
        __atomic_add_fetch(&section_control_bypasses, 1, __ATOMIC_RELAXED);
        return NULL;
    }

    SECTION_CONTROL_CLEANUP cleanup = {0};
    SECTION_CONTROL_AREA *result = NULL;
    BOOL hit = FALSE;
    uint64_t irq_flags = section_lock_irqsave();
    for (int i = 0; i < SECTION_CONTROL_CACHE_ENTRIES; i++) {
        SECTION_CONTROL_AREA *control = &section_control_cache[i];
        if (control->in_use && control->file == file &&
            control->revision == revision && control->size == size &&
            control->file_size == file_size) {
            control->object_refs++;
            control->last_use = ++section_control_clock;
            result = control;
            hit = TRUE;
            section_control_hits++;
            break;
        }
    }

    if (!result) {
        SECTION_CONTROL_AREA *slot = NULL;
        SECTION_CONTROL_AREA *victim = NULL;
        for (int i = 0; i < SECTION_CONTROL_CACHE_ENTRIES; i++) {
            SECTION_CONTROL_AREA *control = &section_control_cache[i];
            if (!control->in_use) {
                slot = control;
                break;
            }
            if (!control->object_refs &&
                (!victim || control->last_use < victim->last_use))
                victim = control;
        }
        if (!slot)
            slot = victim;
        if (slot) {
            if (slot->in_use) {
                section_control_collect_locked(slot, &cleanup);
                section_control_evictions++;
            }
            slot->file = file;
            slot->revision = revision;
            slot->size = size;
            slot->file_size = file_size;
            slot->last_use = ++section_control_clock;
            slot->object_refs = 1;
            slot->in_use = TRUE;
            result = slot;
            section_control_misses++;
            if (file_ref_consumed)
                *file_ref_consumed = TRUE;
        } else {
            section_control_bypasses++;
        }
    }
    section_unlock_irqrestore(irq_flags);
    section_control_cleanup_finish(&cleanup);

    static uint32_t trace_count;
    if (result &&
        __atomic_fetch_add(&trace_count, 1, __ATOMIC_RELAXED) < 64) {
        serial_puts("[SECTION-CONTROL] ");
        serial_puts(hit ? "hit" : "miss");
        serial_puts(" file='");
        serial_puts(osfs2_file_name(file));
        serial_puts("' rev=0x");
        serial_puthex(revision, 16);
        serial_puts(" size=0x");
        serial_puthex(size, 16);
        serial_puts("\n");
    }
    return result;
}

static void section_control_release_unattached(SECTION_CONTROL_AREA *control)
{
    BOOL became_idle = FALSE;
    uint64_t irq_flags = section_lock_irqsave();
    if (control && control->in_use && control->object_refs) {
        control->object_refs--;
        control->last_use = ++section_control_clock;
        became_idle = !control->object_refs;
    }
    section_unlock_irqrestore(irq_flags);
    if (became_idle)
        section_control_trim();
}

void nt_section_cache_dump(void)
{
    uint64_t entries = 0;
    uint64_t active = 0;
    uint64_t pages = 0;
    uint64_t bytes = 0;
    uint64_t irq_flags = section_lock_irqsave();
    for (int i = 0; i < SECTION_CONTROL_CACHE_ENTRIES; i++) {
        SECTION_CONTROL_AREA *control = &section_control_cache[i];
        if (!control->in_use)
            continue;
        entries++;
        if (control->object_refs)
            active++;
        pages += control->materialized_pages;
    }
    bytes = section_control_cache_bytes;
    section_unlock_irqrestore(irq_flags);

    serial_puts("[SECTION-CACHE] entries=");
    serial_putdec(entries);
    serial_puts(" active=");
    serial_putdec(active);
    serial_puts(" pages=");
    serial_putdec(pages);
    serial_puts(" bytes=");
    serial_putdec(bytes);
    serial_puts(" hits=");
    serial_putdec(__atomic_load_n(&section_control_hits, __ATOMIC_RELAXED));
    serial_puts(" misses=");
    serial_putdec(__atomic_load_n(&section_control_misses, __ATOMIC_RELAXED));
    serial_puts(" page_hits=");
    serial_putdec(__atomic_load_n(&section_control_page_hits,
                                  __ATOMIC_RELAXED));
    serial_puts(" evictions=");
    serial_putdec(__atomic_load_n(&section_control_evictions,
                                  __ATOMIC_RELAXED));
    serial_puts(" bypasses=");
    serial_putdec(__atomic_load_n(&section_control_bypasses,
                                  __ATOMIC_RELAXED));
    serial_puts(" disk_bytes=");
    serial_putdec(__atomic_load_n(&section_control_disk_bytes,
                                  __ATOMIC_RELAXED));
    serial_puts(" saved_bytes=");
    serial_putdec(__atomic_load_n(&section_control_saved_bytes,
                                  __ATOMIC_RELAXED));
    serial_puts("\n");
}

int nt_section_cache_flush_unused(void)
{
    int evicted = 0;
    while (section_control_evict_one(TRUE))
        evicted++;
    return evicted;
}

static inline void *section_backing_file(SECTION_OBJECT *sec)
{
    return sec->control ? sec->control->file : sec->file;
}

static inline uint64_t section_backing_file_size(SECTION_OBJECT *sec)
{
    return sec->control ? sec->control->file_size : sec->file_size;
}

static inline SECTION_PAGE **section_sparse_pages_head(SECTION_OBJECT *sec)
{
    return sec->control ? &sec->control->sparse_pages : &sec->sparse_pages;
}

static SECTION_OBJECT *section_alloc(uint64_t backing_phys, uint64_t size,
                                     uint32_t flags, uint32_t protect,
                                     void *file, SECTION_CONTROL_AREA *control,
                                     BOOL sparse,
                                     BOOL deny_access_escalation)
{
    uint64_t irq_flags = section_lock_irqsave();
    SECTION_OBJECT *sec = NULL;
    for (int i = 0; i < SECTION_POOL_MAX; i++) {
        if (!section_pool[i].in_use) {
            sec = &section_pool[i];
            nt_memset(sec, 0, sizeof(*sec));
            section_next_identity++;
            if (!section_next_identity)
                section_next_identity++;
            sec->identity = section_next_identity;
            sec->size = size;
            sec->backing_phys = backing_phys;
            sec->flags = flags;
            sec->protect = protect;
            sec->file = file;
            sec->control = control;
            sec->sparse = sparse;
            sec->deny_access_escalation = deny_access_escalation;
            sec->in_use = TRUE;
            break;
        }
    }
    section_unlock_irqrestore(irq_flags);
    return sec;
}

static BOOL section_security_has_empty_dacl(PVOID descriptor)
{
    const USHORT se_dacl_present = 0x0004;
    const USHORT se_self_relative = 0x8000;
    const UCHAR *sd = (const UCHAR *)descriptor;
    if (!sd || sd[0] != 1)
        return FALSE;

    USHORT control = *(const USHORT *)(sd + 2);
    if (!(control & se_dacl_present))
        return FALSE;

    PVOID dacl;
    if (control & se_self_relative) {
        DWORD offset = *(const DWORD *)(sd + 16);
        dacl = offset ? (PVOID)(sd + offset) : NULL;
    } else {
        dacl = g_compat32_mode
            ? (PVOID)(ULONG_PTR)*(const DWORD *)(sd + 16)
            : *(PVOID const *)(sd + 32);
    }

    if (!dacl)
        return FALSE; /* A NULL DACL grants full access. */
    const UCHAR *acl = (const UCHAR *)dacl;
    USHORT acl_size = *(const USHORT *)(acl + 2);
    USHORT ace_count = *(const USHORT *)(acl + 4);
    return acl_size >= 8 && ace_count == 0;
}

BOOL nt_section_allows_access_escalation(PVOID section,
                                         ACCESS_MASK desired_access)
{
    (void)desired_access;
    SECTION_OBJECT *sec = (SECTION_OBJECT *)section;
    uint64_t irq_flags = section_lock_irqsave();
    BOOL allowed = sec && sec->in_use && !sec->deny_access_escalation;
    section_unlock_irqrestore(irq_flags);
    return allowed;
}

BOOL nt_section_is_alive(PVOID section)
{
    SECTION_OBJECT *sec = (SECTION_OBJECT *)section;
    uint64_t irq_flags = section_lock_irqsave();
    BOOL alive = sec && sec->in_use;
    section_unlock_irqrestore(irq_flags);
    return alive;
}

uint64_t nt_section_identity(PVOID section)
{
    SECTION_OBJECT *sec = (SECTION_OBJECT *)section;
    uint64_t irq_flags = section_lock_irqsave();
    uint64_t identity = sec && sec->in_use ? sec->identity : 0;
    section_unlock_irqrestore(irq_flags);
    return identity;
}

BOOL nt_section_reopen_handle(PVOID section)
{
    SECTION_OBJECT *sec = (SECTION_OBJECT *)section;
    uint64_t irq_flags = section_lock_irqsave();
    BOOL reopened = sec && sec->in_use;
    if (reopened)
        sec->handles_closed = FALSE;
    section_unlock_irqrestore(irq_flags);
    return reopened;
}

static void section_collect_free_locked(SECTION_OBJECT *sec,
                                        SECTION_CLEANUP *cleanup)
{
    if (!sec || !sec->in_use)
        return;
    cleanup->backing_phys = sec->backing_phys;
    cleanup->size = sec->size;
    if (sec->control) {
        SECTION_CONTROL_AREA *control = sec->control;
        if (control->in_use && control->object_refs) {
            control->object_refs--;
            control->last_use = ++section_control_clock;
            cleanup->trim_controls = !control->object_refs;
        } else {
            serial_puts("[SECTION-CONTROL] invalid object reference\n");
        }
    } else {
        cleanup->file = sec->file;
        cleanup->sparse_pages = sec->sparse_pages;
    }
    nt_memset(sec, 0, sizeof(*sec));
}

static void section_finish_cleanup(SECTION_CLEANUP *cleanup)
{
    if (cleanup->backing_phys)
        mem_free_pages((void *)(uintptr_t)cleanup->backing_phys,
                       (cleanup->size + 4095) / 4096);
    if (cleanup->file)
        osfs2_file_release(cleanup->file);
    section_page_list_free(cleanup->sparse_pages);
    if (cleanup->trim_controls)
        section_control_trim();
}

static int section_view_pin(SECTION_OBJECT *sec)
{
    uint64_t irq_flags = section_lock_irqsave();
    int ok = sec && sec->in_use;
    if (ok)
        sec->view_refs++;
    section_unlock_irqrestore(irq_flags);
    return ok;
}

/* Splitting one tracked view into several VMA records must transfer one view
 * reference to each new record even after the section handle was closed. */
static void section_view_retain_existing(PVOID section)
{
    SECTION_OBJECT *sec = (SECTION_OBJECT *)section;
    uint64_t irq_flags = section_lock_irqsave();
    if (sec && sec->in_use && sec->view_refs)
        sec->view_refs++;
    else
        serial_puts("[SECTION] invalid view split reference\n");
    section_unlock_irqrestore(irq_flags);
}

static BOOL section_writes_back(ULONG protect)
{
    ULONG base = protect & 0xFF;
    return base == PAGE_READWRITE || base == PAGE_EXECUTE_READWRITE;
}

static uint64_t section_sparse_page_phys(SECTION_OBJECT *sec,
                                         uint32_t page_index)
{
    uint64_t phys = 0;
    uint64_t irq_flags = section_lock_irqsave();
    if (sec && sec->in_use && sec->sparse) {
        SECTION_PAGE *page = *section_sparse_pages_head(sec);
        while (page && page->index > page_index)
            page = page->next;
        if (page && page->index == page_index)
            phys = page->phys;
    }
    section_unlock_irqrestore(irq_flags);
    return phys;
}

static NTSTATUS section_flush_mapping(PVOID section, uint64_t view_phys,
                                      SIZE_T view_size, ULONG protect,
                                      SIZE_T section_offset,
                                      SIZE_T relative_offset,
                                      SIZE_T bytes_to_flush)
{
    SECTION_OBJECT *sec = (SECTION_OBJECT *)section;
    if (!sec || !sec->in_use)
        return STATUS_INVALID_PARAMETER;
    void *file = section_backing_file(sec);
    uint64_t file_size = section_backing_file_size(sec);
    if (!file || !section_writes_back(protect))
        return STATUS_SUCCESS;
    if (relative_offset > view_size || section_offset > sec->size ||
        view_size > sec->size - section_offset)
        return STATUS_INVALID_PARAMETER;
    if (!sec->sparse &&
        view_phys != sec->backing_phys + section_offset)
        return STATUS_INVALID_PARAMETER;

    SIZE_T available = view_size - relative_offset;
    SIZE_T length = bytes_to_flush ? bytes_to_flush : available;
    if (length > available)
        return STATUS_INVALID_PARAMETER;

    uint64_t file_offset = section_offset + relative_offset;
    if (file_offset >= file_size)
        return STATUS_SUCCESS;
    if (length > file_size - file_offset)
        length = (SIZE_T)(file_size - file_offset);
    if (!length)
        return STATUS_SUCCESS;

    SIZE_T written = 0;
    if (!sec->sparse) {
        const void *source = PHYS_TO_VIRT(view_phys + relative_offset);
        if (osfs2_write(file, file_offset, source, length) < 0)
            goto write_failed;
        written = length;
    } else {
        uint64_t cursor = file_offset;
        SIZE_T remaining = length;
        while (remaining) {
            uint32_t page_index = (uint32_t)(cursor / 4096);
            SIZE_T in_page = (SIZE_T)(cursor & 0xFFFULL);
            SIZE_T chunk = 4096 - in_page;
            if (chunk > remaining)
                chunk = remaining;

            uint64_t page_phys = section_sparse_page_phys(sec, page_index);
            if (!page_phys) {
                cursor += chunk;
                remaining -= chunk;
                continue;
            }

            uint64_t run_phys = page_phys + in_page;
            uint64_t last_page_phys = page_phys;
            uint64_t run_file_offset = cursor;
            SIZE_T run_length = chunk;
            cursor += chunk;
            remaining -= chunk;

            while (remaining && !(cursor & 0xFFFULL)) {
                uint64_t next_phys = section_sparse_page_phys(
                    sec, (uint32_t)(cursor / 4096));
                if (!next_phys || next_phys != last_page_phys + 4096)
                    break;
                chunk = remaining < 4096 ? remaining : 4096;
                run_length += chunk;
                cursor += chunk;
                remaining -= chunk;
                last_page_phys = next_phys;
            }

            if (osfs2_write(file, run_file_offset,
                            PHYS_TO_VIRT(run_phys), run_length) < 0) {
                file_offset = run_file_offset;
                length = run_length;
                goto write_failed;
            }
            written += run_length;
        }
    }

    static uint32_t trace_count;
    if (trace_count++ < 32) {
        serial_puts("[MMAP-FLUSH] file='");
        serial_puts(osfs2_file_name(file));
        serial_puts("' off=0x");
        serial_puthex(file_offset, 16);
        serial_puts(" len=0x");
        serial_puthex(length, 16);
        if (sec->sparse) {
            serial_puts(" materialized=0x");
            serial_puthex(written, 16);
        }
        serial_puts("\n");
    }
    return STATUS_SUCCESS;

write_failed:
    serial_puts("[MMAP-FLUSH] failed file='");
    serial_puts(osfs2_file_name(file));
    serial_puts("' off=0x");
    serial_puthex(file_offset, 16);
    serial_puts(" len=0x");
    serial_puthex(length, 16);
    serial_puts("\n");
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS nt_flush_view_of_section(PVOID base_address, SIZE_T bytes_to_flush)
{
    uint64_t address = (uint64_t)(ULONG_PTR)base_address;
    ULONG owner_pid = nt_current_owner_pid();
    vm_track_entry_t snapshot = {0};
    BOOL found = FALSE;
    uint64_t view_end = 0;

    uint64_t irq_flags = vm_track_lock_irqsave();
    for (int i = 0; i < vm_track_count; i++) {
        vm_track_entry_t *entry = &vm_track[i];
        uint64_t end = entry->va + entry->size;
        if (!entry->section || entry->owner_pid != owner_pid ||
            address < entry->va || end < entry->va || address >= end)
            continue;
        if (section_view_pin((SECTION_OBJECT *)entry->section)) {
            snapshot = *entry;
            found = TRUE;
            for (int j = 0; j < vm_track_count; j++) {
                uint64_t segment_end = vm_track[j].va + vm_track[j].size;
                if (vm_track[j].owner_pid == owner_pid &&
                    vm_track[j].section == entry->section &&
                    vm_track[j].allocation_base == entry->allocation_base &&
                    segment_end >= vm_track[j].va && segment_end > view_end)
                    view_end = segment_end;
            }
        }
        break;
    }
    vm_track_unlock_irqrestore(irq_flags);

    if (!found)
        return STATUS_INVALID_PARAMETER;

    uint64_t flush_end = bytes_to_flush ? address + bytes_to_flush : view_end;
    if (flush_end < address || flush_end > view_end) {
        section_view_release(snapshot.section);
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS status = STATUS_SUCCESS;
    uint64_t cursor = address;
    while (cursor < flush_end) {
        vm_track_entry_t segment = {0};
        BOOL have_segment = FALSE;
        irq_flags = vm_track_lock_irqsave();
        for (int i = 0; i < vm_track_count; i++) {
            uint64_t segment_end = vm_track[i].va + vm_track[i].size;
            if (vm_track[i].owner_pid == owner_pid &&
                vm_track[i].section == snapshot.section &&
                vm_track[i].allocation_base == snapshot.allocation_base &&
                segment_end >= vm_track[i].va && cursor >= vm_track[i].va &&
                cursor < segment_end) {
                segment = vm_track[i];
                have_segment = TRUE;
                break;
            }
        }
        vm_track_unlock_irqrestore(irq_flags);
        if (!have_segment) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        uint64_t segment_end = segment.va + segment.size;
        uint64_t piece_end = segment_end < flush_end ? segment_end : flush_end;
        NTSTATUS piece_status = section_flush_mapping(
            segment.section, segment.phys, segment.size, segment.protect,
            segment.section_offset,
            (SIZE_T)(cursor - segment.va), (SIZE_T)(piece_end - cursor));
        if (!NT_SUCCESS(piece_status) && NT_SUCCESS(status))
            status = piece_status;
        cursor = piece_end;
    }
    section_view_release(snapshot.section);
    return status;
}

static void section_free(SECTION_OBJECT *sec)
{
    SECTION_CLEANUP cleanup = {0};
    uint64_t irq_flags = section_lock_irqsave();
    section_collect_free_locked(sec, &cleanup);
    section_unlock_irqrestore(irq_flags);
    section_finish_cleanup(&cleanup);
}

static int section_view_acquire(SECTION_OBJECT *sec)
{
    uint64_t irq_flags = section_lock_irqsave();
    int ok = sec && sec->in_use && !sec->handles_closed;
    if (ok)
        sec->view_refs++;
    section_unlock_irqrestore(irq_flags);
    return ok;
}

static void section_view_release(PVOID section)
{
    SECTION_OBJECT *sec = (SECTION_OBJECT *)section;
    SECTION_CLEANUP cleanup = {0};
    uint64_t irq_flags = section_lock_irqsave();
    if (sec && sec->in_use && sec->view_refs) {
        sec->view_refs--;
        if (!sec->view_refs && sec->handles_closed)
            section_collect_free_locked(sec, &cleanup);
    }
    section_unlock_irqrestore(irq_flags);
    section_finish_cleanup(&cleanup);
}

static void section_release_handle(PVOID section)
{
    SECTION_OBJECT *sec = (SECTION_OBJECT *)section;
    SECTION_CLEANUP cleanup = {0};
    uint64_t irq_flags = section_lock_irqsave();
    if (sec && sec->in_use) {
        sec->handles_closed = TRUE;
        if (!sec->view_refs)
            section_collect_free_locked(sec, &cleanup);
    }
    section_unlock_irqrestore(irq_flags);
    section_finish_cleanup(&cleanup);
}

static SECTION_PAGE **section_sparse_page_slot(SECTION_OBJECT *sec,
                                                uint32_t page_index)
{
    SECTION_PAGE **slot = section_sparse_pages_head(sec);
    while (*slot && (*slot)->index > page_index)
        slot = &(*slot)->next;
    return slot;
}

static uint32_t section_sparse_pages_get(
    SECTION_OBJECT *sec, uint32_t first_page_index, uint32_t page_count,
    uint64_t phys_pages[SECTION_FAULT_BATCH_PAGES])
{
    if (!page_count || page_count > SECTION_FAULT_BATCH_PAGES)
        return 0;

    nt_memset(phys_pages, 0, page_count * sizeof(*phys_pages));
    uint64_t irq_flags = section_lock_irqsave();
    if (!sec || !sec->in_use || !sec->sparse ||
        first_page_index >= sec->size / 4096 ||
        page_count > sec->size / 4096 - first_page_index) {
        section_unlock_irqrestore(irq_flags);
        return 0;
    }

    SECTION_CONTROL_AREA *control = sec->control;
    void *file = section_backing_file(sec);
    uint64_t file_size = section_backing_file_size(sec);
    uint64_t expected_revision = control ? control->revision : 0;

    uint32_t missing_count = 0;
    SECTION_PAGE *page = *section_sparse_pages_head(sec);
    for (uint32_t remaining = page_count; remaining; remaining--) {
        uint32_t offset = remaining - 1;
        uint32_t page_index = first_page_index + offset;
        while (page && page->index > page_index)
            page = page->next;
        if (page && page->index == page_index)
            phys_pages[offset] = page->phys;
        else
            missing_count++;
    }
    section_unlock_irqrestore(irq_flags);

    uint32_t reused_pages = page_count - missing_count;
    if (control && reused_pages) {
        __atomic_add_fetch(&section_control_page_hits, reused_pages,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&section_control_saved_bytes,
                           (uint64_t)reused_pages * 4096,
                           __ATOMIC_RELAXED);
    }

    if (!missing_count)
        return page_count;

    SECTION_PAGE *candidates[SECTION_FAULT_BATCH_PAGES] = {0};
    for (uint32_t offset = 0; offset < page_count; offset++) {
        if (phys_pages[offset])
            continue;
        SECTION_PAGE *candidate =
            (SECTION_PAGE *)kmalloc(sizeof(*candidate));
        if (!candidate)
            goto allocation_failed;
        nt_memset(candidate, 0, sizeof(*candidate));
        candidate->index = first_page_index + offset;
        candidates[offset] = candidate;
    }

    void *batch_phys = mem_alloc_pages(missing_count);
    if (batch_phys) {
        nt_memset(PHYS_TO_VIRT(batch_phys), 0,
                  (uint64_t)missing_count * 4096);
        uint32_t phys_offset = 0;
        for (uint32_t offset = 0; offset < page_count; offset++) {
            if (!candidates[offset])
                continue;
            candidates[offset]->phys =
                (uint64_t)(uintptr_t)batch_phys +
                (uint64_t)phys_offset++ * 4096;
        }
    } else {
        /* Preserve correctness when the PMM cannot find one contiguous run. */
        for (uint32_t offset = 0; offset < page_count; offset++) {
            if (!candidates[offset])
                continue;
            void *candidate_phys = mem_alloc_pages(1);
            if (!candidate_phys)
                goto allocation_failed;
            nt_memset(PHYS_TO_VIRT(candidate_phys), 0, 4096);
            candidates[offset]->phys =
                (uint64_t)(uintptr_t)candidate_phys;
        }
    }

    /* Page in contiguous candidate runs with one filesystem read. The pages
     * were zeroed above, so a partial final page keeps a zero-filled tail. */
    uint64_t disk_bytes = 0;
    if (file) {
        if (control) {
            uint64_t revision = osfs2_file_revision(file);
            if (revision != expected_revision || (revision & 1))
                goto stale_file;
        }
        for (uint32_t offset = 0; offset < page_count;) {
            SECTION_PAGE *first = candidates[offset];
            if (!first) {
                offset++;
                continue;
            }

            uint32_t next_offset = offset + 1;
            uint32_t last_index = first->index;
            uint64_t last_phys = first->phys;
            uint64_t run_length = 4096;
            while (next_offset < page_count) {
                SECTION_PAGE *next = candidates[next_offset];
                if (!next || next->index != last_index + 1 ||
                    next->phys != last_phys + 4096)
                    break;
                run_length += 4096;
                last_index = next->index;
                last_phys = next->phys;
                next_offset++;
            }

            uint64_t file_offset = (uint64_t)first->index * 4096;
            uint64_t read_length = 0;
            if (file_offset < file_size) {
                read_length = file_size - file_offset;
                if (read_length > run_length)
                    read_length = run_length;
            }
            if (read_length &&
                osfs2_read(file, file_offset,
                           PHYS_TO_VIRT(first->phys), read_length) !=
                    (int)read_length) {
                serial_puts("[SECTION-PAGEIN] failed file='");
                serial_puts(osfs2_file_name(file));
                serial_puts("' off=0x");
                serial_puthex(file_offset, 16);
                serial_puts(" len=0x");
                serial_puthex(read_length, 16);
                serial_puts("\n");
                goto allocation_failed;
            }
            disk_bytes += read_length;
            offset = next_offset;
        }
        if (control) {
            uint64_t revision = osfs2_file_revision(file);
            if (revision != expected_revision || (revision & 1))
                goto stale_file;
        }
    }

    irq_flags = section_lock_irqsave();
    if (!sec->in_use || !sec->sparse || sec->control != control ||
        (control && (!control->in_use ||
                     control->revision != expected_revision))) {
        section_unlock_irqrestore(irq_flags);
        goto allocation_failed;
    }

    for (uint32_t offset = 0; offset < page_count; offset++) {
        SECTION_PAGE *candidate = candidates[offset];
        if (!candidate)
            continue;
        SECTION_PAGE **slot = section_sparse_page_slot(sec, candidate->index);
        if (*slot && (*slot)->index == candidate->index) {
            phys_pages[offset] = (*slot)->phys;
            if (control)
                reused_pages++;
            continue;
        }
        candidate->next = *slot;
        *slot = candidate;
        phys_pages[offset] = candidate->phys;
        candidates[offset] = NULL;
        if (control) {
            control->materialized_pages++;
            section_control_cache_bytes += 4096;
        }
    }
    section_unlock_irqrestore(irq_flags);

    if (control) {
        if (disk_bytes)
            __atomic_add_fetch(&section_control_disk_bytes, disk_bytes,
                               __ATOMIC_RELAXED);
        if (reused_pages > page_count - missing_count) {
            uint32_t raced_pages = reused_pages - (page_count - missing_count);
            __atomic_add_fetch(&section_control_page_hits, raced_pages,
                               __ATOMIC_RELAXED);
            __atomic_add_fetch(&section_control_saved_bytes,
                               (uint64_t)raced_pages * 4096,
                               __ATOMIC_RELAXED);
        }
    }

    /* A competing fault may have published some pages while we allocated. */
    for (uint32_t offset = 0; offset < page_count; offset++) {
        SECTION_PAGE *candidate = candidates[offset];
        if (!candidate)
            continue;
        mem_free_pages((void *)(uintptr_t)candidate->phys, 1);
        kfree(candidate);
    }
    return page_count;

stale_file:
    serial_puts("[SECTION-PAGEIN] stale file revision file='");
    serial_puts(osfs2_file_name(file));
    serial_puts("' expected=0x");
    serial_puthex(expected_revision, 16);
    serial_puts(" current=0x");
    serial_puthex(osfs2_file_revision(file), 16);
    serial_puts("\n");

allocation_failed:
    nt_memset(phys_pages, 0, page_count * sizeof(*phys_pages));
    for (uint32_t offset = 0; offset < page_count; offset++) {
        SECTION_PAGE *candidate = candidates[offset];
        if (!candidate)
            continue;
        if (candidate->phys)
            mem_free_pages((void *)(uintptr_t)candidate->phys, 1);
        kfree(candidate);
    }
    return 0;
}

int nt_section_page_fault(uint64_t address, uint64_t error_code)
{
    if (error_code & 1)
        return -1;

    uint64_t page_va = address & ~0xFFFULL;
    ULONG owner_pid = nt_current_owner_pid();
    vm_track_entry_t mapping = {0};
    BOOL found = FALSE;

    uint64_t vm_irq_flags = vm_track_lock_irqsave();
    for (int i = 0; i < vm_track_count; i++) {
        uint64_t end = vm_track[i].va + vm_track[i].size;
        if (!vm_track[i].section || vm_track[i].owner_pid != owner_pid ||
            end < vm_track[i].va || page_va < vm_track[i].va ||
            page_va >= end)
            continue;
        SECTION_OBJECT *sec = (SECTION_OBJECT *)vm_track[i].section;
        if (sec->sparse && section_view_pin(sec)) {
            mapping = vm_track[i];
            found = TRUE;
        }
        break;
    }
    vm_track_unlock_irqrestore(vm_irq_flags);
    if (!found)
        return -1;

    SECTION_OBJECT *sec = (SECTION_OBJECT *)mapping.section;
    if ((mapping.protect & 0xFF) == PAGE_NOACCESS) {
        section_view_release(sec);
        return -1;
    }
    uint64_t view_offset = page_va - mapping.va;
    uint64_t byte_offset = mapping.section_offset + view_offset;
    uint32_t first_page_index = (uint32_t)(byte_offset / 4096);
    uint64_t pages_remaining = (mapping.size - view_offset) / 4096;
    uint64_t section_pages_remaining = sec->size / 4096 - first_page_index;
    uint64_t batch_pages = pages_remaining < section_pages_remaining
                         ? pages_remaining : section_pages_remaining;
    if (batch_pages > SECTION_FAULT_BATCH_PAGES)
        batch_pages = SECTION_FAULT_BATCH_PAGES;

    /* SteamChrome clears each new shared-memory stream sequentially. Map a
     * small run per fault while keeping every physical page independent. */
    uint64_t pte_flags = nt_prot_to_page_flags(mapping.protect);
    uint64_t phys_pages[SECTION_FAULT_BATCH_PAGES];
    uint32_t resolved_pages = section_sparse_pages_get(
        sec, first_page_index, (uint32_t)batch_pages, phys_pages);
    uint64_t mapped_pages = 0;
    for (; mapped_pages < resolved_pages; mapped_pages++) {
        uint64_t mapped_va = page_va + mapped_pages * 4096;
        if (!phys_pages[mapped_pages] ||
            nt_map_page_in(mapping.cr3, mapped_va,
                           phys_pages[mapped_pages],
                           PTE_PRESENT | PTE_WRITABLE) != 0)
            break;
        if (pte_flags != (PTE_PRESENT | PTE_WRITABLE) &&
            nt_set_page_flags_in(mapping.cr3, mapped_va, pte_flags) != 0) {
            mapped_pages++;
            break;
        }
    }
    section_view_release(sec);
    return mapped_pages ? 0 : -1;
}

NTSTATUS sys_NtCreateSection(ULONG_PTR *args)
{
    PHANDLE             SectionHandle       = (PHANDLE)args[0];
    ACCESS_MASK         DesiredAccess       = (ACCESS_MASK)args[1];
    POBJECT_ATTRIBUTES  ObjectAttributes    = (POBJECT_ATTRIBUTES)args[2];
    PLARGE_INTEGER      MaximumSize         = (PLARGE_INTEGER)args[3];
    ULONG               SectionPageProtection = (ULONG)args[4];
    ULONG               AllocationAttributes  = (ULONG)args[5];
    HANDLE              FileHandle            = (HANDLE)args[6];

    if (!SectionHandle)
        return STATUS_INVALID_PARAMETER;

    /* Determine section size */
    uint64_t size = 0;
    void *backing_file = NULL;
    FILE_OBJECT *file_object = NULL;

    if (FileHandle) {
        /* File-backed section: get size from the file handle */
        NTSTATUS st = handle_lookup(&g_handle_table, FileHandle,
                                    OBJ_TYPE_FILE, (PVOID *)&file_object);
        if (!NT_SUCCESS(st)) {
            nt_log("NtCreateSection: invalid FileHandle");
            return st;
        }
        backing_file = file_object->osfs_file;
        size = (uint64_t)file_object->size;
        /* If MaximumSize given and larger, use that */
        if (MaximumSize && MaximumSize->QuadPart < 0)
            return STATUS_INVALID_PARAMETER;
        if (MaximumSize && (uint64_t)MaximumSize->QuadPart > size)
            size = (uint64_t)MaximumSize->QuadPart;
    } else {
        /* Pagefile-backed: MaximumSize is required */
        if (!MaximumSize || MaximumSize->QuadPart <= 0) {
            nt_log("NtCreateSection: pagefile section requires MaximumSize");
            return STATUS_INVALID_PARAMETER;
        }
        size = (uint64_t)MaximumSize->QuadPart;
    }

    uint64_t logical_size = size;
    if (FileHandle && !logical_size)
        return STATUS_INVALID_PARAMETER;

    /* Round up to page boundary */
    if (size > UINT64_MAX - 0xFFFULL)
        return STATUS_INVALID_PARAMETER;
    size = (size + 0xFFF) & ~0xFFFULL;
    if (size == 0) size = 4096;

    if (backing_file && osfs2_file_retain(backing_file) < 0)
        return STATUS_INVALID_PARAMETER;

    BOOL sparse = size >= SECTION_SPARSE_MIN_SIZE &&
                  !(AllocationAttributes & SEC_IMAGE);

    if (FileHandle) {
        if (!file_object || !file_object->osfs_file ||
            (logical_size > (uint64_t)file_object->size &&
             osfs2_truncate(file_object->osfs_file, logical_size) < 0)) {
            if (backing_file)
                osfs2_file_release(backing_file);
            return STATUS_UNSUCCESSFUL;
        }
        if (logical_size > (uint64_t)file_object->size)
            file_object->size = (LONGLONG)logical_size;
    }

    /* Small mappings and image sections retain contiguous backing. */
    uint64_t pages = size / 4096;
    void *mem_phys = NULL;
    void *mem = NULL;
    if (!sparse) {
        mem_phys = mem_alloc_pages(pages);
        if (!mem_phys) {
            if (backing_file)
                osfs2_file_release(backing_file);
            nt_log_hex("NtCreateSection: physical allocation FAILED, size=", size);
            return STATUS_NO_MEMORY;
        }
        mem = PHYS_TO_VIRT(mem_phys);
        nt_memset(mem, 0, size);
    }

    /* Eager mappings load their file now. Sparse mappings page it in from the
     * fault path and therefore do no I/O during NtCreateSection. */
    if (FileHandle && !sparse && file_object->size > 0) {
        uint64_t to_read = (uint64_t)file_object->size;
        if (to_read > size) to_read = size;
        if (osfs2_read(file_object->osfs_file, 0, mem, to_read) !=
            (int)to_read) {
            mem_free_pages(mem_phys, pages);
            osfs2_file_release(backing_file);
            nt_log("NtCreateSection: file read failed");
            return STATUS_UNSUCCESSFUL;
        }
    }

    SECTION_CONTROL_AREA *control = NULL;
    if (backing_file && sparse &&
        section_control_read_only_protect(SectionPageProtection)) {
        BOOL file_ref_consumed = FALSE;
        control = section_control_acquire(backing_file, size, logical_size,
                                          &file_ref_consumed);
        if (control) {
            if (!file_ref_consumed)
                osfs2_file_release(backing_file);
            backing_file = NULL;
        }
    }

    /* Allocate a section object from pool */
    BOOL deny_access_escalation = section_security_has_empty_dacl(
        ObjectAttributes ? ObjectAttributes->SecurityDescriptor : NULL);
    SECTION_OBJECT *sec = section_alloc((uint64_t)(uintptr_t)mem_phys, size,
                                         AllocationAttributes,
                                         SectionPageProtection,
                                         backing_file, control,
                                         sparse,
                                         deny_access_escalation);
    if (!sec) {
        if (mem_phys)
            mem_free_pages(mem_phys, pages);
        if (backing_file)
            osfs2_file_release(backing_file);
        if (control)
            section_control_release_unattached(control);
        nt_log("NtCreateSection: section pool exhausted");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    sec->file_size = logical_size;

#if !defined(OK_QUIET) || !OK_QUIET
    static uint32_t demand_trace_count;
    void *demand_file = section_backing_file(sec);
    if (sparse && demand_file && demand_trace_count++ < 16) {
        serial_puts("[SECTION-DEMAND] file='");
        serial_puts(osfs2_file_name(demand_file));
        serial_puts("' size=0x");
        serial_puthex(logical_size, 16);
        serial_puts("\n");
    }
#endif

    /* Allocate handle */
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_SECTION,
                                   DesiredAccess, sec, SectionHandle);
    if (!NT_SUCCESS(status)) {
        section_free(sec);
        return status;
    }

#if !defined(OK_QUIET) || !OK_QUIET
    static uint32_t create_trace_count;
    if (__atomic_fetch_add(&create_trace_count, 1, __ATOMIC_RELAXED) < 512) {
        serial_puts("[SECTION-CREATE] pid=");
        serial_putdec(nt_current_owner_pid());
        serial_puts(" handle=0x");
        serial_puthex((ULONG_PTR)*SectionHandle, 16);
        serial_puts(" id=0x");
        serial_puthex(sec->identity, 16);
        serial_puts(" size=0x");
        serial_puthex(sec->size, 16);
        serial_puts(" sparse=");
        serial_putdec(sec->sparse);
        serial_puts(" restricted=");
        serial_putdec(sec->deny_access_escalation);
        serial_puts(" access=0x");
        serial_puthex(DesiredAccess, 8);
        serial_puts(" file=");
        serial_putdec(FileHandle != NULL);
        serial_puts("\n");
    }

    if (deny_access_escalation) {
        serial_puts("[SECTION-ACL] restricted handle=0x");
        serial_puthex((ULONG_PTR)*SectionHandle, 16);
        serial_puts(" size=0x");
        serial_puthex(size, 16);
        serial_puts("\n");
    }
#endif

#if !defined(OK_QUIET) || !OK_QUIET
    nt_log_hex("NtCreateSection: handle=", (ULONGLONG)*SectionHandle);
    nt_log_hex("  size=", size);
    nt_log_hex("  backing=", (ULONGLONG)(ULONG_PTR)mem_phys);
#endif

    return STATUS_SUCCESS;
}

typedef struct _SECTION_BASIC_INFORMATION_LOCAL {
    PVOID         BaseAddress;
    ULONG         AllocationAttributes;
    ULONG         Reserved;
    LARGE_INTEGER MaximumSize;
} SECTION_BASIC_INFORMATION_LOCAL;

NTSTATUS sys_NtQuerySection(ULONG_PTR *args)
{
    HANDLE  SectionHandle            = (HANDLE)args[0];
    ULONG   SectionInformationClass  = (ULONG)args[1];
    PVOID   SectionInformation       = (PVOID)args[2];
    SIZE_T  SectionInformationLength = (SIZE_T)args[3];
    SIZE_T *ReturnLength             = (SIZE_T *)args[4];
    const SIZE_T required = sizeof(SECTION_BASIC_INFORMATION_LOCAL);

    if (ReturnLength)
        *ReturnLength = required;
    if (SectionInformationClass != 0)
        return STATUS_INVALID_INFO_CLASS;
    if (!SectionInformation || SectionInformationLength < required)
        return STATUS_INFO_LENGTH_MISMATCH;

    SECTION_OBJECT *sec = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, SectionHandle,
                                    OBJ_TYPE_SECTION, (PVOID *)&sec);
    if (!NT_SUCCESS(status))
        return status;

    SECTION_BASIC_INFORMATION_LOCAL *info =
        (SECTION_BASIC_INFORMATION_LOCAL *)SectionInformation;
    info->BaseAddress = (PVOID)(ULONG_PTR)sec->backing_phys;
    info->AllocationAttributes = sec->flags;
    info->Reserved = 0;
    info->MaximumSize.QuadPart = (LONGLONG)sec->size;
    return STATUS_SUCCESS;
}

NTSTATUS sys_NtMapViewOfSection(ULONG_PTR *args)
{
    HANDLE  SectionHandle  = (HANDLE)args[0];
    /* HANDLE ProcessHandle = (HANDLE)args[1]; -- ignored, always current */
    PVOID  *BaseAddress    = (PVOID *)args[2];
    /* ULONG_PTR ZeroBits  = args[3]; */
    /* SIZE_T CommitSize   = args[4]; */
    PLARGE_INTEGER SectionOffset = (PLARGE_INTEGER)args[5];
    SIZE_T *ViewSize       = (SIZE_T *)args[6];
    /* ULONG InheritDisposition = (ULONG)args[7]; */
    /* ULONG AllocationType = (ULONG)args[8]; */
    ULONG Win32Protect     = (ULONG)args[9];

    if (!BaseAddress)
        return STATUS_INVALID_PARAMETER;

    /* Look up section object */
    SECTION_OBJECT *sec = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, SectionHandle,
                                    OBJ_TYPE_SECTION, (PVOID *)&sec);
    if (!NT_SUCCESS(status)) {
        nt_log("NtMapViewOfSection: invalid section handle");
        return status;
    }

    uint64_t offset = SectionOffset ? (uint64_t)SectionOffset->QuadPart : 0;
    if ((SectionOffset && SectionOffset->QuadPart < 0) ||
        (offset & 0xFFFULL) || offset >= sec->size)
        return STATUS_INVALID_PARAMETER;

    SIZE_T requested_size = ViewSize ? *ViewSize : 0;
    if (!requested_size)
        requested_size = sec->size - offset;
    if (requested_size > sec->size - offset ||
        requested_size > UINT64_MAX - 0xFFFULL)
        return STATUS_INVALID_PARAMETER;
    SIZE_T map_size = (requested_size + 0xFFFULL) & ~0xFFFULL;

    if (!section_view_acquire(sec))
        return STATUS_INVALID_HANDLE;

    ULONG owner_pid = nt_current_owner_pid();
    uint64_t cr3 = nt_current_cr3();
    vm_global_init_once();
    uint64_t irq_flags = vm_track_lock_irqsave();
    uint64_t requested_va = (uint64_t)(ULONG_PTR)*BaseAddress & ~0xFFFULL;
    uint64_t va = vm_choose_address_locked(map_size, requested_va,
                                           g_compat32_mode);
    if (!va) {
        vm_track_unlock_irqrestore(irq_flags);
        section_view_release(sec);
        return requested_va ? STATUS_CONFLICTING_ADDRESSES : STATUS_NO_MEMORY;
    }
    ULONG protect = Win32Protect ? Win32Protect : sec->protect;
    if (!nt_valid_page_protection(protect)) {
        vm_track_unlock_irqrestore(irq_flags);
        section_view_release(sec);
        return STATUS_INVALID_PARAMETER;
    }
    if (sec->control && !section_control_read_only_protect(protect)) {
        vm_track_unlock_irqrestore(irq_flags);
        section_view_release(sec);
        return STATUS_ACCESS_DENIED;
    }
    uint64_t pte_flags = nt_prot_to_page_flags(protect);

    uint64_t mapped_pages = 0;
    uint64_t pages = map_size / 4096;
    if (!sec->sparse) {
        for (; mapped_pages < pages; mapped_pages++) {
            uint64_t page_va = va + mapped_pages * 4096;
            if (nt_map_page_in(cr3, page_va,
                               sec->backing_phys + offset +
                                   mapped_pages * 4096,
                               PTE_PRESENT | PTE_WRITABLE) != 0)
                break;
            if (pte_flags != (PTE_PRESENT | PTE_WRITABLE) &&
                nt_set_page_flags_in(cr3, page_va, pte_flags) != 0) {
                mapped_pages++;
                break;
            }
        }
    }

    if ((!sec->sparse && mapped_pages != pages) ||
        !vm_track_add(va,
                      sec->sparse ? 0 : sec->backing_phys + offset,
                      map_size, protect, FALSE, sec, offset)) {
        while (mapped_pages)
            nt_unmap_page_in(cr3, va + --mapped_pages * 4096);
        vm_track_unlock_irqrestore(irq_flags);
        section_view_release(sec);
        return STATUS_NO_MEMORY;
    }
    vm_advance_auto_next_locked(owner_pid, g_compat32_mode, va, map_size);
    vm_track_unlock_irqrestore(irq_flags);

    *BaseAddress = (PVOID)(ULONG_PTR)va;
    if (ViewSize)
        *ViewSize = map_size;

#if !defined(OK_QUIET) || !OK_QUIET
    static uint32_t map_trace_count;
    if (__atomic_fetch_add(&map_trace_count, 1, __ATOMIC_RELAXED) < 512) {
        serial_puts("[SECTION-MAP] pid=");
        serial_putdec(owner_pid);
        serial_puts(" handle=0x");
        serial_puthex((ULONG_PTR)SectionHandle, 16);
        serial_puts(" id=0x");
        serial_puthex(sec->identity, 16);
        serial_puts(" size=0x");
        serial_puthex(map_size, 16);
        serial_puts(" offset=0x");
        serial_puthex(offset, 16);
        serial_puts(" base=0x");
        serial_puthex(va, 16);
        serial_puts(" protect=0x");
        serial_puthex(protect, 8);
        serial_puts(" sparse=");
        serial_putdec(sec->sparse);
        serial_puts("\n");
    }
#endif

#if !defined(OK_QUIET) || !OK_QUIET
    nt_log_hex("NtMapViewOfSection: base=", va);
    nt_log_hex("  size=", map_size);
    nt_log_hex("  offset=", offset);
#endif

    return STATUS_SUCCESS;
}

NTSTATUS sys_NtUnmapViewOfSection(ULONG_PTR *args)
{
    /* HANDLE ProcessHandle = (HANDLE)args[0]; */
    PVOID BaseAddress = (PVOID)args[1];
    if (!BaseAddress)
        return STATUS_INVALID_PARAMETER;

    ULONG owner_pid = nt_current_owner_pid();
    uint64_t address = (uint64_t)(ULONG_PTR)BaseAddress;
    uint64_t allocation_base = 0;
    PVOID section = NULL;
    uint64_t irq_flags = vm_track_lock_irqsave();
    for (int i = 0; i < vm_track_count; i++) {
        uint64_t end = vm_track[i].va + vm_track[i].size;
        if (vm_track[i].owner_pid == owner_pid && vm_track[i].section &&
            end >= vm_track[i].va && address >= vm_track[i].va &&
            address < end) {
            allocation_base = vm_track[i].allocation_base;
            section = vm_track[i].section;
            break;
        }
    }
    vm_track_unlock_irqrestore(irq_flags);
    if (!section)
        return STATUS_INVALID_PARAMETER;

    NTSTATUS flush_status = STATUS_SUCCESS;
    SIZE_T unmapped_size = 0;
    for (;;) {
        vm_track_entry_t entry = {0};
        BOOL found = FALSE;
        irq_flags = vm_track_lock_irqsave();
        for (int i = 0; i < vm_track_count; i++) {
            if (vm_track[i].owner_pid == owner_pid &&
                vm_track[i].section == section &&
                vm_track[i].allocation_base == allocation_base) {
                entry = vm_track[i];
                vm_track[i] = vm_track[--vm_track_count];
                found = TRUE;
                break;
            }
        }
        vm_track_unlock_irqrestore(irq_flags);
        if (!found)
            break;

        NTSTATUS status = section_flush_mapping(
            entry.section, entry.phys, entry.size, entry.protect,
            entry.section_offset, 0, 0);
        if (!NT_SUCCESS(status) && NT_SUCCESS(flush_status))
            flush_status = status;
        uint64_t pages = entry.size / 4096;
        for (uint64_t page = 0; page < pages; page++)
            nt_unmap_page_in(entry.cr3, entry.va + page * 4096);
        unmapped_size += entry.size;
        section_view_release(entry.section);
    }
    (void)unmapped_size;
#if !defined(OK_QUIET) || !OK_QUIET
    nt_log_hex("NtUnmapViewOfSection: base=", allocation_base);
    nt_log_hex("  size=", unmapped_size);
#endif
    return flush_status;
}

/* ── Unimplemented syscall stub ─────────────────────────────── */

static NTSTATUS sys_NtStub(ULONG_PTR *args)
{
    (void)args;
    nt_log("WARN: unimplemented NT syscall");
    return STATUS_NOT_IMPLEMENTED;
}

/* ── Initialize SSDT ────────────────────────────────────────── */

void nt_syscall_init(NT_SERVICE_TABLE *table)
{
    /* VM state is global and process-owned. Reinitializing the SSDT must not
     * discard live child mappings or their physical backing. */
    vm_global_init_once();

    /* Fill all slots with stub */
    for (ULONG i = 0; i < NTSYS_MAX; i++) {
        table->handlers[i]   = sys_NtStub;
        table->arg_counts[i] = 0;
    }
    table->limit = NTSYS_MAX;

    /* Register implemented handlers */
    #define REG(num, fn, nargs) do { \
        table->handlers[num]   = (fn); \
        table->arg_counts[num] = (nargs); \
    } while (0)

    /* File I/O */
    REG(NTSYS_ReadFile,               sys_NtReadFile,               9);
    REG(NTSYS_WriteFile,              sys_NtWriteFile,              9);
    REG(NTSYS_Close,                  sys_NtClose,                  1);
    REG(NTSYS_CreateFile,             sys_NtCreateFile,             11);
    REG(NTSYS_QueryInformationFile,   sys_NtQueryInformationFile,   5);
    REG(NTSYS_SetInformationFile,     sys_NtSetInformationFile,     5);

    /* Memory */
    REG(NTSYS_AllocateVirtualMemory,  sys_NtAllocateVirtualMemory,  6);
    REG(NTSYS_FreeVirtualMemory,      sys_NtFreeVirtualMemory,      4);
    REG(NTSYS_ProtectVirtualMemory,   sys_NtProtectVirtualMemory,   5);
    REG(NTSYS_QueryVirtualMemory,     sys_NtQueryVirtualMemory,     6);

    /* Object */
    REG(NTSYS_DuplicateObject,        sys_NtDuplicateObject,        7);

    /* Process */
    REG(NTSYS_TerminateProcess,       sys_NtTerminateProcess,       2);

    /* Section (memory-mapped files) */
    REG(NTSYS_CreateSection,          sys_NtCreateSection,          7);
    REG(NTSYS_MapViewOfSection,       sys_NtMapViewOfSection,      10);
    REG(NTSYS_UnmapViewOfSection,     sys_NtUnmapViewOfSection,     2);

    /* Misc */
    REG(NTSYS_DelayExecution,         sys_NtDelayExecution,         2);
    REG(NTSYS_QueryPerformanceCounter, sys_NtQueryPerformanceCounter, 2);
    REG(NTSYS_YieldExecution,         sys_NtYieldExecution,         0);

    #undef REG

    /* Initialize handle table */
    handle_table_init(&g_handle_table);
    nt_memset(g_win32_pipes, 0, sizeof(g_win32_pipes));

    /* Pre-allocate console handles */
    g_console_in.flags  = FILE_OBJ_CONSOLE_IN;
    g_console_out.flags = FILE_OBJ_CONSOLE_OUT;
    g_console_err.flags = FILE_OBJ_CONSOLE_ERR;

    HANDLE dummy;
    handle_alloc(&g_handle_table, OBJ_TYPE_FILE, GENERIC_READ,
                 &g_console_in, &dummy);   /* handle 4 → index 1 */
    handle_alloc(&g_handle_table, OBJ_TYPE_FILE, GENERIC_WRITE,
                 &g_console_out, &dummy);  /* handle 8 → index 2 */
    handle_alloc(&g_handle_table, OBJ_TYPE_FILE, GENERIC_WRITE,
                 &g_console_err, &dummy);  /* handle 12 → index 3 */

    /* Register process/thread handlers (Phase 6) */
    extern void nt_process_register_syscalls(NT_SERVICE_TABLE *table);
    nt_process_register_syscalls(table);

    /* Register synchronization handlers (Phase 7) */
    extern void nt_sync_register_syscalls(NT_SERVICE_TABLE *table);
    nt_sync_register_syscalls(table);

    g_initialized = TRUE;
    nt_log("NT syscall table initialized (file, mem, proc, sync)");
    nt_log("  console handles: stdin=4, stdout=8, stderr=12");
}

/* ── Dispatch ───────────────────────────────────────────────── */

NTSTATUS nt_syscall_dispatch(NT_SERVICE_TABLE *table,
                             ULONG nr, ULONG_PTR *args)
{
    if (!table || nr >= table->limit) {
        nt_log_hex("NT: invalid syscall number ", nr);
        return STATUS_INVALID_PARAMETER;
    }

    return table->handlers[nr](args);
}

/* ── Assembly entry wrapper ─────────────────────────────────── */
/*
 * Called from nt_syscall_entry.S / nt_int2e_entry.
 * Uses the global SSDT initialized by nt_syscall_init().
 */

static NT_SERVICE_TABLE g_ssdt;
static BOOL g_ssdt_initialized = FALSE;

void nt_syscall_init_global(void)
{
    nt_syscall_init(&g_ssdt);
    g_ssdt_initialized = TRUE;
}

NTSTATUS nt_syscall_dispatch_from_asm(ULONG nr, ULONG_PTR *args)
{
    if (!g_ssdt_initialized) {
        nt_log("FATAL: NT SSDT not initialized");
        return STATUS_UNSUCCESSFUL;
    }
    return nt_syscall_dispatch(&g_ssdt, nr, args);
}
