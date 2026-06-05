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

/* ── External kernel interfaces ─────────────────────────────── */

/* Serial debug output */
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

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
extern void *osfs2_create(const char *name, uint64_t size);
extern uint64_t osfs2_file_size(void *file);

/* Physical memory */
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

/* Paging */
extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern int paging_win32_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern int paging_unmap_page(uint64_t virt);
extern int paging_set_flags(uint64_t virt, uint64_t flags);

/* PTE flags (must match paging.c) */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_NX        (1ULL << 63)

/* ── Virtual address allocator for Win32 VirtualAlloc ────────── */
/*
 * Win32 VirtualAlloc semantics: each allocation returns a UNIQUE virtual
 * address mapped to freshly zeroed pages.  Previous implementation used
 * identity-mapped mem_alloc_pages which recycled physical addresses,
 * causing live data destruction (FName::Names bug in UT99).
 *
 * Fix: allocate physical pages + map at unique VA via paging_map_page().
 * VA range: 0x40000000–0x7FFF0000 (Win32 user heap region, below 2GB).
 */
#define WIN32_VA_BASE  0x40000000ULL
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

static uint64_t win32_va_next = WIN32_VA_BASE;

/* Track VA→phys mapping for cleanup on VirtualFree */
#define VM_TRACK_MAX 256

typedef struct {
    uint64_t va;
    uint64_t phys;
    SIZE_T   size;
} vm_track_entry_t;

static vm_track_entry_t vm_track[VM_TRACK_MAX];
static int vm_track_count = 0;

/* VA free-list: ranges released by MEM_RELEASE that get recycled.
 * Pages keep their tombstone pattern (0xDEADC0DE) UNTIL the engine
 * either writes (overwriting it) or reads it as a pointer (triggering
 * STALE-PTR detector in idt.c).  This is a *detector* mode — designed
 * to flush out use-after-free patterns, not to be safe long-term. */
#define VM_FREELIST_MAX 128

typedef struct {
    uint64_t va;
    SIZE_T   size;
} vm_freelist_entry_t;

static vm_freelist_entry_t vm_freelist[VM_FREELIST_MAX];
static int vm_freelist_count = 0;

static uint64_t vm_freelist_take(SIZE_T size)
{
    for (int i = 0; i < vm_freelist_count; i++) {
        if (vm_freelist[i].size >= size) {
            uint64_t va = vm_freelist[i].va;
            SIZE_T leftover = vm_freelist[i].size - size;
            if (leftover > 0) {
                vm_freelist[i].va   += size;
                vm_freelist[i].size  = leftover;
            } else {
                vm_freelist[i] = vm_freelist[--vm_freelist_count];
            }
            return va;
        }
    }
    return 0;
}

static void vm_freelist_add(uint64_t va, SIZE_T size)
{
    if (size == 0) return;
    for (int i = 0; i < vm_freelist_count; i++) {
        if (vm_freelist[i].va + vm_freelist[i].size == va) {
            vm_freelist[i].size += size; return;
        }
        if (va + size == vm_freelist[i].va) {
            vm_freelist[i].va = va; vm_freelist[i].size += size; return;
        }
    }
    if (vm_freelist_count < VM_FREELIST_MAX) {
        vm_freelist[vm_freelist_count].va   = va;
        vm_freelist[vm_freelist_count].size = size;
        vm_freelist_count++;
    }
}

static void vm_track_add(uint64_t va, uint64_t phys, SIZE_T size)
{
    for (int i = 0; i < vm_track_count; i++) {
        if (vm_track[i].va == va) {
            if (size > vm_track[i].size)
                vm_track[i].size = size;
            return;
        }
    }
    if (vm_track_count < VM_TRACK_MAX) {
        vm_track[vm_track_count].va   = va;
        vm_track[vm_track_count].phys = phys;
        vm_track[vm_track_count].size = size;
        vm_track_count++;
    }
}

static SIZE_T vm_track_remove(uint64_t va, uint64_t *out_phys)
{
    for (int i = 0; i < vm_track_count; i++) {
        if (vm_track[i].va == va) {
            SIZE_T size = vm_track[i].size;
            if (out_phys) *out_phys = vm_track[i].phys;
            vm_track[i] = vm_track[--vm_track_count];
            return size;
        }
    }
    if (out_phys) *out_phys = 0;
    return 0;
}

/* Forward declaration (defined below with other nt_* helpers) */
static inline void nt_memset(void *s, int c, SIZE_T n);

/* Allocate VA range and map physical pages into it */
static PVOID win32_va_alloc(SIZE_T size, uint64_t *out_phys, ULONG protect)
{
    uint64_t pages = size / 4096;
    if (pages == 0) return NULL;

    void *phys = mem_alloc_pages(pages);
    if (!phys) return NULL;

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
    uint64_t va = (win32_va_next + 0xFFFF) & ~0xFFFFULL;
    uint64_t va_end = va + size;
    int recycled = 0;
    (void)vm_freelist_take;
    if (va_end > WIN32_VA_LIMIT) {
        mem_free_pages(phys, pages);
        return NULL;
    }
    win32_va_next = va_end;

    /* Map each 4KB page in Win32 page table.
     * Set NX (no-execute) unless PAGE_EXECUTE* was requested.
     * PAGE_EXECUTE_READWRITE = 0x40, PAGE_EXECUTE_READ = 0x20,
     * PAGE_EXECUTE = 0x10, PAGE_EXECUTE_WRITECOPY = 0x80 */
    int is_exec = (protect & 0xF0) != 0; /* any PAGE_EXECUTE* bit */
    uint64_t pte_flags = PTE_PRESENT | PTE_WRITABLE;
    if (!is_exec)
        pte_flags |= PTE_NX;

    uint64_t pa = (uint64_t)phys;
    for (uint64_t i = 0; i < pages; i++) {
        paging_win32_map_page(va + i * 4096, pa + i * 4096, pte_flags);
    }

    /* Zero via the newly mapped VA (recycling disabled — always zero). */
    (void)recycled;
    nt_memset((void *)va, 0, size);

    /* VA-ALLOC trace — every fresh allocation. Caller EIP comes from
     * compat32's saved per-thunk return address (compat32_get_last_caller_eip)
     * — that's the PE32 instruction immediately after the VirtualAlloc
     * INT 0x2E thunk return. Cross-reference with FMW-ASSERT logs to
     * identify which alloc became a Pool->Mem that later failed the
     * pool-integrity check. */
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

/* VFS path resolution — replaces ad-hoc strip_nt_path */
#include "../fs/vfs.h"
extern void *osfs2_find_ci(const char *name);

static const char *strip_nt_path(const char *path)
{
    const char *resolved = vfs_resolve(path, VFS_MODE_WIN32);
    return resolved ? resolved : path;
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
    /* ULONG            CreateOptions    = (ULONG)args[8]; */
    /* PVOID            EaBuffer         = (PVOID)args[9]; */
    /* ULONG            EaLength         = (ULONG)args[10]; */

    if (!FileHandle || !ObjectAttributes || !ObjectAttributes->ObjectName)
        return STATUS_INVALID_PARAMETER;

    /* Convert UNICODE_STRING to ASCII path */
    char path_buf[260];
    unicode_to_ascii(ObjectAttributes->ObjectName, path_buf, sizeof(path_buf));
    const char *path = strip_nt_path(path_buf);

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

    /* For BMP files that don't exist in OsitoFS, provide a minimal 1x1 BMP
     * so that Bitmap.LoadFile() assertions pass. The engine requires a valid
     * BMP for the splash screen — returning NOT_FOUND triggers a fatal assert. */

    /* Try to find/create in OsitoFS (case-insensitive for Win32) */
    void *osfs_file = osfs2_find_ci(path);

    if (!osfs_file && (CreateDisposition == FILE_CREATE ||
                       CreateDisposition == FILE_OPEN_IF ||
                       CreateDisposition == FILE_OVERWRITE_IF ||
                       CreateDisposition == FILE_SUPERSEDE)) {
        osfs_file = osfs2_create(path, 0);
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
        nt_log(" NOT FOUND\n");
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            IoStatusBlock->Information = 0;
        }
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    /* Create file object. The pool is reused: a free slot has flags==0 (an
     * in-use object always has FILE_OBJ_DISK_FILE set); NtClose returns the slot
     * by zeroing flags. Previously this was a monotonic bump allocator that never
     * freed, so after 64 opens NtCreateFile failed for everything — which is why
     * UT99 opened Entry.unr fine early but got "Can't find file" at LoadMap once
     * the 64 slots were exhausted (64 opens / 5 closes). */
    static FILE_OBJECT file_pool[64];

    FILE_OBJECT *fobj = NULL;
    for (int i = 0; i < 64; i++) {
        if (file_pool[i].flags == 0) { fobj = &file_pool[i]; break; }
    }
    if (!fobj)
        return STATUS_INSUFFICIENT_RESOURCES;

    fobj->flags     = FILE_OBJ_DISK_FILE;
    fobj->osfs_file = osfs_file;
    fobj->position  = 0;
    fobj->size      = osfs2_file_size(osfs_file);

    /* Allocate handle */
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_FILE,
                                   DesiredAccess, fobj, FileHandle);
    if (!NT_SUCCESS(status))
        return status;

    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (osfs_file ? 1 /* FILE_OPENED */ : 2 /* FILE_CREATED */);
    }

    nt_log_hex("NtCreateFile: handle = ", (ULONGLONG)*FileHandle);
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

    serial_puts("[NtReadFile] result=");
    serial_puthex((uint64_t)(int64_t)result, 8);
    serial_puts("\n");

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

    if (result < 0)
        return STATUS_UNSUCCESSFUL;

    fobj->position = offset + result;

    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (ULONG_PTR)result;
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
        for (ULONG i = 0; i < Length; i++) {
            serial_puts((const char[]){s[i], '\0'});
            fb_putc(s[i], 0x00CCCCCC);
        }
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = Length;
        }
        return STATUS_SUCCESS;
    }

    /* Disk file — echo text content to serial for engine log capture */
    {
        const char *s = (const char *)Buffer;
        if (Length > 0 && Length < 4096) {
            serial_puts("[WRITE] ");
            for (ULONG i = 0; i < Length && i < 300; i++) {
                char c = s[i];
                if (c >= 32 && c < 127) {
                    serial_puts((const char[]){c, '\0'});
                } else if (c == '\n') {
                    serial_puts("\n[WRITE] ");
                }
            }
            serial_puts("\n");
        }
    }

    LONGLONG offset = ByteOffset ? ByteOffset->QuadPart : fobj->position;

    int result = osfs2_write(fobj->osfs_file, (uint64_t)offset, Buffer, Length);
    if (result < 0)
        return STATUS_UNSUCCESSFUL;

    fobj->position = offset + result;
    if (fobj->position > fobj->size)
        fobj->size = fobj->position;

    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (ULONG_PTR)result;
    }

    return STATUS_SUCCESS;
}

/* ── NtClose ────────────────────────────────────────────────── */

NTSTATUS sys_NtClose(ULONG_PTR *args)
{
    HANDLE Handle = (HANDLE)args[0];

    /* Don't close console handles */
    if (Handle == STD_INPUT_HANDLE_VALUE ||
        Handle == STD_OUTPUT_HANDLE_VALUE ||
        Handle == STD_ERROR_HANDLE_VALUE) {
        return STATUS_SUCCESS;
    }

    /* Free section backing memory before closing handle */
    HANDLE_ENTRY *entry = handle_get_entry(&g_handle_table, Handle);
    if (entry && entry->type == OBJ_TYPE_SECTION && entry->object) {
        /* backing is first pointer field after uint64_t size */
        void **backing_ptr = (void **)((char *)entry->object + sizeof(uint64_t));
        if (*backing_ptr) { kfree(*backing_ptr); *backing_ptr = NULL; }
    }

    /* Return the FILE_OBJECT pool slot so NtCreateFile can reuse it (zeroing
     * flags marks it free). Without this the 64-slot disk-file pool leaks and
     * later opens (e.g. Entry.unr at LoadMap) fail with INSUFFICIENT_RESOURCES. */
    if (entry && entry->type == OBJ_TYPE_FILE && entry->object) {
        FILE_OBJECT *fobj = (FILE_OBJECT *)entry->object;
        if (fobj->flags & FILE_OBJ_DISK_FILE)
            fobj->flags = 0;  /* slot reusable; console FILE_OBJECTs are left alone */
    }

    return handle_close(&g_handle_table, Handle);
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

    SIZE_T size = *RegionSize;
    /* Round up to page boundary. Minimum 1 page (4KB).
     * Windows allows VirtualAlloc with size=0 for MEM_COMMIT on
     * existing reservations, but for new allocations, treat 0 as 4KB. */
    if (size == 0) size = 4096;
    size = (size + 0xFFF) & ~0xFFFULL;
    uint64_t pages = size / 4096;

    if (pages == 0)
        return STATUS_INVALID_PARAMETER;

    PVOID addr = NULL;
    uint64_t phys = 0;

    if (*BaseAddress && (AllocationType & MEM_COMMIT)) {
        /* Fixed address — recommit already-mapped pages.
         * Windows does NOT zero on recommit — data must be preserved.
         * This is critical for UE1's TArray allocator which uses
         * VirtualAlloc(ptr, size, MEM_COMMIT) to grow arrays. */
        addr = *BaseAddress;
        phys = 0;  /* no new physical alloc */
    } else {
        /* Allocate fresh VA + physical pages, zeroed via new VA */
        addr = win32_va_alloc(size, &phys, Protect);
        if (!addr) {
            nt_log_hex("NtAllocateVirtualMemory FAILED: size=", size);
            return STATUS_NO_MEMORY;
        }
    }

    vm_track_add((uint64_t)addr, phys, size);

    *BaseAddress = addr;
    *RegionSize  = size;

    /* Always log VA allocs for debugging */
    nt_log_hex("NtAllocateVirtualMemory: ", (ULONGLONG)addr);
    nt_log_hex("  size = ", size);

    return STATUS_SUCCESS;
}

/* ── NtFreeVirtualMemory ────────────────────────────────────── */

NTSTATUS sys_NtFreeVirtualMemory(ULONG_PTR *args)
{
    /* HANDLE ProcessHandle = (HANDLE)args[0]; */
    PVOID  *BaseAddress    = (PVOID *)args[1];
    SIZE_T *RegionSize     = (SIZE_T *)args[2];
    ULONG   FreeType       = (ULONG)args[3];

    if (!BaseAddress || !*BaseAddress)
        return STATUS_INVALID_PARAMETER;

    nt_log_hex("NtFreeVirtualMemory: ", (ULONGLONG)*BaseAddress);
    nt_log_hex("  FreeType = ", FreeType);

    if (FreeType & MEM_DECOMMIT) {
        /* MEM_DECOMMIT: pages become inaccessible but VA stays reserved.
         * For now, keep pages mapped (no-op) — decommit would require
         * tracking reserved vs committed state per page. */
        SIZE_T size = (RegionSize && *RegionSize) ? *RegionSize : 4096;
        nt_log_hex("  decommit size = ", size);
        return STATUS_SUCCESS;
    }

    if (FreeType & MEM_RELEASE) {
        uint64_t phys = 0;
        uint64_t va    = (uint64_t)*BaseAddress;
        SIZE_T tracked = vm_track_remove(va, &phys);

        /* TOMBSTONE: keep VA mapped (engine may read briefly), but
         * overwrite the freed pages with a recognizable sentinel
         * pattern.  Any subsequent stale-pointer dereference that
         * was holding a `Data*` from inside this range will read
         * 0xDEADC0DE / 0xDEADC0DE / ... — if the engine then calls
         * through it, RIP becomes 0xDEADC0DE which our PF handler
         * recognizes and logs (see idt.c [STALE-PTR] tag).  This
         * gives us precise visibility into UE1 use-after-free
         * patterns without crashing the engine. */
        if (tracked > 0) {
            uint32_t *p = (uint32_t *)(uintptr_t)va;
            SIZE_T words = tracked / 4;
            for (SIZE_T i = 0; i < words; i++) p[i] = 0xDEADC0DE;
            (void)vm_freelist_add;
        }
        nt_log_hex("  release (tombstone) size = ", tracked);

        *BaseAddress = NULL;
        if (RegionSize) *RegionSize = 0;
    }

    return STATUS_SUCCESS;
}

/* ── NtQueryInformationFile ──────────────────────────────────── */

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
        info->Directory     = FALSE;
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
        info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
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
        /* Truncate/extend — we can't really resize in OsitoFS, but record it */
        if (Length < sizeof(LARGE_INTEGER))
            return STATUS_INFO_LENGTH_MISMATCH;
        PLARGE_INTEGER new_size = (PLARGE_INTEGER)FileInformation;
        fobj->size = new_size->QuadPart;
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 0;
        }
        return STATUS_SUCCESS;
    }

    default:
        nt_log_hex("NtSetInformationFile: unsupported class ", (ULONGLONG)InfoClass);
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ── NtDuplicateObject ──────────────────────────────────────── */

NTSTATUS sys_NtDuplicateObject(ULONG_PTR *args)
{
    /* HANDLE SourceProcessHandle = (HANDLE)args[0]; */
    HANDLE  SourceHandle         = (HANDLE)args[1];
    /* HANDLE TargetProcessHandle = (HANDLE)args[2]; */
    PHANDLE TargetHandle         = (PHANDLE)args[3];
    ACCESS_MASK DesiredAccess    = (ACCESS_MASK)args[4];
    /* ULONG  HandleAttributes   = (ULONG)args[5]; */
    ULONG   Options              = (ULONG)args[6];

    if (!TargetHandle && !(Options & DUPLICATE_CLOSE_SOURCE))
        return STATUS_INVALID_PARAMETER;

    /* Single-process model: source and target are same table */
    HANDLE new_handle = NULL;
    NTSTATUS status = handle_duplicate(&g_handle_table, SourceHandle,
                                       &g_handle_table, &new_handle,
                                       DesiredAccess, FALSE, Options);
    if (NT_SUCCESS(status) && TargetHandle)
        *TargetHandle = new_handle;

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

NTSTATUS sys_NtProtectVirtualMemory(ULONG_PTR *args)
{
    /* HANDLE ProcessHandle = (HANDLE)args[0]; */
    PVOID  *BaseAddress    = (PVOID *)args[1];
    SIZE_T *RegionSize     = (SIZE_T *)args[2];
    ULONG   NewProtect     = (ULONG)args[3];
    ULONG  *OldProtect     = (ULONG *)args[4];

    if (!BaseAddress || !*BaseAddress || !RegionSize || !OldProtect)
        return STATUS_INVALID_PARAMETER;

    /* Report old protection as PAGE_READWRITE (we don't track per-page yet) */
    *OldProtect = PAGE_READWRITE;

    uint64_t base = (uint64_t)*BaseAddress;
    SIZE_T size = *RegionSize;
    size = (size + 0xFFF) & ~0xFFFULL;
    uint64_t pages = size / 4096;
    uint64_t flags = nt_prot_to_page_flags(NewProtect);

    for (uint64_t i = 0; i < pages; i++) {
        paging_set_flags(base + i * 4096, flags);
    }

    nt_log_hex("NtProtectVirtualMemory: base=", base);
    nt_log_hex("  new_protect=", NewProtect);

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

    if (MemoryInformationLength < sizeof(MEMORY_BASIC_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;

    MEMORY_BASIC_INFORMATION *mbi = (MEMORY_BASIC_INFORMATION *)MemoryInformation;

    /* Simplified: report everything as committed private memory.
     * A real implementation would walk the VMA table. */
    uint64_t addr = (uint64_t)BaseAddress;
    uint64_t page_base = addr & ~0xFFFULL;

    mbi->BaseAddress       = (PVOID)page_base;
    mbi->AllocationBase    = (PVOID)page_base;
    mbi->AllocationProtect = PAGE_READWRITE;
    mbi->PartitionId       = 0;
    mbi->Padding0          = 0;
    mbi->RegionSize        = 4096;  /* one page */
    mbi->State             = MEM_COMMIT;
    mbi->Protect           = PAGE_READWRITE;
    mbi->Type              = MEM_PRIVATE;
    mbi->Padding1          = 0;

    if (ReturnLength)
        *ReturnLength = sizeof(MEMORY_BASIC_INFORMATION);

    return STATUS_SUCCESS;
}

/* ── NtYieldExecution ───────────────────────────────────────── */

NTSTATUS sys_NtYieldExecution(ULONG_PTR *args)
{
    (void)args;
    __asm__ volatile("sti; hlt; cli");
    return STATUS_SUCCESS;
}

/* ── NtTerminateProcess ─────────────────────────────────────── */

extern void proc_exit(int32_t code);

NTSTATUS sys_NtTerminateProcess(ULONG_PTR *args)
{
    /* HANDLE ProcessHandle = (HANDLE)args[0]; */
    NTSTATUS ExitStatus   = (NTSTATUS)args[1];

    nt_log_hex("NtTerminateProcess: exit code = ", (ULONGLONG)ExitStatus);
    proc_exit((int32_t)ExitStatus);

    /* Never reached */
    return STATUS_SUCCESS;
}

/* ── NtDelayExecution (Sleep) ───────────────────────────────── */

extern uint64_t idt_get_ticks(void);

NTSTATUS sys_NtDelayExecution(ULONG_PTR *args)
{
    /* BOOLEAN Alertable = (BOOLEAN)args[0]; */
    PLARGE_INTEGER DelayInterval = (PLARGE_INTEGER)args[1];

    if (!DelayInterval)
        return STATUS_INVALID_PARAMETER;

    /* Negative = relative time in 100ns units */
    LONGLONG delay_100ns = DelayInterval->QuadPart;
    if (delay_100ns < 0) delay_100ns = -delay_100ns;

    /* Use RDTSC instead of idt_get_ticks(): the APIC timer is masked
     * for the entire compat32 lifetime (commit 0311d5f) so kernel ticks
     * don't advance while UT99 is in user code, and a sti/hlt wait
     * here would never wake. RDTSC always increments regardless of
     * interrupt state. Assume ~3 GHz TSC, so 100ns = 300 cycles. */
    uint64_t delay_cycles = (uint64_t)delay_100ns * 300ULL;
    if (delay_cycles == 0) delay_cycles = 300;  /* min 100ns */

    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t start = ((uint64_t)hi << 32) | lo;

    /* Cooperative yield once so other in-flight callbacks get a turn,
     * then spin on RDTSC. We can't sti/hlt because no IRQ wakes us. */
    extern void sched_yield(void);
    sched_yield();

    for (;;) {
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t now = ((uint64_t)hi << 32) | lo;
        if (now - start >= delay_cycles) break;
        __asm__ volatile("pause" ::: "memory");
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
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        PerformanceCounter->QuadPart = (LONGLONG)(((uint64_t)hi << 32) | lo);
    }

    if (PerformanceFrequency)
        PerformanceFrequency->QuadPart = 3000000000LL;  /* ~3 GHz TSC */

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

typedef struct _SECTION_OBJECT {
    uint64_t    size;           /* section size in bytes */
    void       *backing;        /* kmalloc'd memory backing (NULL = free slot) */
    uint32_t    flags;          /* SEC_COMMIT, SEC_IMAGE, SEC_RESERVE */
    uint32_t    protect;        /* PAGE_READWRITE, PAGE_READONLY, etc. */
    void       *file;           /* osfs2_file_t* if file-backed, NULL if pagefile */
} SECTION_OBJECT;

#define SECTION_POOL_MAX 64
static SECTION_OBJECT section_pool[SECTION_POOL_MAX];

static SECTION_OBJECT *section_alloc(void)
{
    for (int i = 0; i < SECTION_POOL_MAX; i++)
        if (!section_pool[i].backing)
            return &section_pool[i];
    return NULL;
}

void section_free(SECTION_OBJECT *sec)
{
    if (sec && sec->backing) {
        kfree(sec->backing);
        sec->backing = NULL;
        sec->size = 0;
    }
}

NTSTATUS sys_NtCreateSection(ULONG_PTR *args)
{
    PHANDLE             SectionHandle       = (PHANDLE)args[0];
    ACCESS_MASK         DesiredAccess       = (ACCESS_MASK)args[1];
    /* POBJECT_ATTRIBUTES ObjectAttributes  = (POBJECT_ATTRIBUTES)args[2]; -- ignored */
    PLARGE_INTEGER      MaximumSize         = (PLARGE_INTEGER)args[3];
    ULONG               SectionPageProtection = (ULONG)args[4];
    ULONG               AllocationAttributes  = (ULONG)args[5];
    HANDLE              FileHandle            = (HANDLE)args[6];

    if (!SectionHandle)
        return STATUS_INVALID_PARAMETER;

    /* Determine section size */
    uint64_t size = 0;

    if (FileHandle) {
        /* File-backed section: get size from the file handle */
        FILE_OBJECT *fobj = NULL;
        NTSTATUS st = handle_lookup(&g_handle_table, FileHandle,
                                    OBJ_TYPE_FILE, (PVOID *)&fobj);
        if (!NT_SUCCESS(st)) {
            nt_log("NtCreateSection: invalid FileHandle");
            return st;
        }
        size = (uint64_t)fobj->size;
        /* If MaximumSize given and larger, use that */
        if (MaximumSize && (uint64_t)MaximumSize->QuadPart > size)
            size = (uint64_t)MaximumSize->QuadPart;
    } else {
        /* Pagefile-backed: MaximumSize is required */
        if (!MaximumSize || MaximumSize->QuadPart == 0) {
            nt_log("NtCreateSection: pagefile section requires MaximumSize");
            return STATUS_INVALID_PARAMETER;
        }
        size = (uint64_t)MaximumSize->QuadPart;
    }

    /* Round up to page boundary */
    size = (size + 0xFFF) & ~0xFFFULL;
    if (size == 0) size = 4096;

    /* Allocate physical backing memory */
    void *mem = kmalloc(size);
    if (!mem) {
        nt_log_hex("NtCreateSection: kmalloc FAILED, size=", size);
        return STATUS_NO_MEMORY;
    }

    /* Zero the backing memory */
    nt_memset(mem, 0, size);

    /* If file-backed, read file contents into section */
    if (FileHandle) {
        FILE_OBJECT *fobj = NULL;
        handle_lookup(&g_handle_table, FileHandle, OBJ_TYPE_FILE, (PVOID *)&fobj);
        if (fobj && fobj->osfs_file && fobj->size > 0) {
            uint64_t to_read = (uint64_t)fobj->size;
            if (to_read > size) to_read = size;
            osfs2_read(fobj->osfs_file, 0, mem, to_read);
        }
    }

    /* Allocate a section object from pool */
    SECTION_OBJECT *sec = section_alloc();
    if (!sec) {
        kfree(mem);
        nt_log("NtCreateSection: section pool exhausted");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    sec->size    = size;
    sec->backing = mem;
    sec->flags   = AllocationAttributes;
    sec->protect = SectionPageProtection;
    sec->file    = FileHandle ? (void *)1 : NULL;

    /* Allocate handle */
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_SECTION,
                                   DesiredAccess, sec, SectionHandle);
    if (!NT_SUCCESS(status)) {
        section_free(sec);
        return status;
    }

    nt_log_hex("NtCreateSection: handle=", (ULONGLONG)*SectionHandle);
    nt_log_hex("  size=", size);
    nt_log_hex("  backing=", (ULONGLONG)(ULONG_PTR)mem);

    return STATUS_SUCCESS;
}

NTSTATUS sys_NtMapViewOfSection(ULONG_PTR *args)
{
    HANDLE  SectionHandle  = (HANDLE)args[0];
    /* HANDLE ProcessHandle = (HANDLE)args[1]; -- ignored, always current */
    PVOID  *BaseAddress    = (PVOID *)args[2];
    /* ULONG_PTR ZeroBits  = args[3]; */
    /* SIZE_T CommitSize   = args[4]; */
    /* PLARGE_INTEGER SectionOffset = (PLARGE_INTEGER)args[5]; */
    SIZE_T *ViewSize       = (SIZE_T *)args[6];
    /* ULONG InheritDisposition = (ULONG)args[7]; */
    /* ULONG AllocationType = (ULONG)args[8]; */
    /* ULONG Win32Protect  = (ULONG)args[9]; */

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

    /*
     * Identity-mapped v1: return the kmalloc'd backing address directly.
     * The backing memory lives in identity-mapped kernel space, so it's
     * accessible from both kernel and Win32 page tables.
     *
     * Future: allocate a VA in the Win32 range and map section pages
     * through paging_win32_map_page for proper per-process isolation.
     */
    *BaseAddress = sec->backing;
    if (ViewSize)
        *ViewSize = sec->size;

    nt_log_hex("NtMapViewOfSection: base=", (ULONGLONG)(ULONG_PTR)sec->backing);
    nt_log_hex("  size=", sec->size);

    return STATUS_SUCCESS;
}

NTSTATUS sys_NtUnmapViewOfSection(ULONG_PTR *args)
{
    /* HANDLE ProcessHandle = (HANDLE)args[0]; */
    /* PVOID BaseAddress    = (PVOID)args[1]; */

    /*
     * v1 stub: no-op. Section backing is kmalloc'd and stays valid
     * until the section handle is closed. Proper unmap would remove
     * VA mappings from the process page table.
     */
    nt_log("NtUnmapViewOfSection: stub (no-op)");
    return STATUS_SUCCESS;
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
