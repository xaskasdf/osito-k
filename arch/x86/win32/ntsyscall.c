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
#define WIN32_VA_LIMIT 0x7FFF0000ULL

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
static PVOID win32_va_alloc(SIZE_T size, uint64_t *out_phys)
{
    uint64_t pages = size / 4096;
    if (pages == 0) return NULL;

    void *phys = mem_alloc_pages(pages);
    if (!phys) return NULL;

    uint64_t va = win32_va_next;
    if (va + size > WIN32_VA_LIMIT) {
        mem_free_pages(phys, pages);
        return NULL;
    }
    win32_va_next = va + size;

    /* Map each 4KB page: VA → PA */
    uint64_t pa = (uint64_t)phys;
    for (uint64_t i = 0; i < pages; i++) {
        paging_map_page(va + i * 4096, pa + i * 4096,
                        PTE_PRESENT | PTE_WRITABLE);
    }

    /* Zero via the new VA — safe, this address is fresh */
    nt_memset((void *)va, 0, size);

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

/* Strip NT path prefix: \??\C:\foo → foo, \Device\HarddiskN\foo → foo */
static const char *strip_nt_path(const char *path)
{
    /* Skip \??\ prefix */
    if (path[0] == '\\' && path[1] == '?' && path[2] == '?' && path[3] == '\\')
        path += 4;

    /* Skip drive letter (C:\) */
    if (path[0] && path[1] == ':' && path[2] == '\\')
        path += 3;

    /* Convert remaining backslashes to forward slashes is done by caller */
    return path;
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

    /* Try to find/create in OsitoFS */
    void *osfs_file = osfs2_find(path);

    if (!osfs_file && (CreateDisposition == FILE_CREATE ||
                       CreateDisposition == FILE_OPEN_IF ||
                       CreateDisposition == FILE_OVERWRITE_IF ||
                       CreateDisposition == FILE_SUPERSEDE)) {
        osfs_file = osfs2_create(path, 0);
    }

    if (!osfs_file) {
        nt_log(" NOT FOUND\n");
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            IoStatusBlock->Information = 0;
        }
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    /* Create file object */
    /* NOTE: In kernel integration, use kmalloc. Here we use static pool. */
    static FILE_OBJECT file_pool[64];
    static int file_pool_next = 0;

    if (file_pool_next >= 64)
        return STATUS_INSUFFICIENT_RESOURCES;

    FILE_OBJECT *fobj = &file_pool[file_pool_next++];
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

    int result = osfs2_read(fobj->osfs_file, (uint64_t)offset, Buffer, to_read);

    serial_puts("[NtReadFile] result=");
    serial_puthex((uint64_t)(int64_t)result, 8);
    serial_puts("\n");

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

    /* Disk file */
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
    /* ULONG  Protect       = (ULONG)args[5]; */

    if (!BaseAddress || !RegionSize)
        return STATUS_INVALID_PARAMETER;

    SIZE_T size = *RegionSize;
    /* Round up to page boundary */
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
        addr = win32_va_alloc(size, &phys);
        if (!addr)
            return STATUS_NO_MEMORY;
    }

    vm_track_add((uint64_t)addr, phys, size);

    *BaseAddress = addr;
    *RegionSize  = size;

    /* Throttle VA alloc logging to reduce noise */
    {
        static int va_log_count = 0;
        va_log_count++;
        if (va_log_count <= 10 || (va_log_count % 50) == 0) {
            nt_log_hex("NtAllocateVirtualMemory: ", (ULONGLONG)addr);
            nt_log_hex("  size = ", size);
        }
    }

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
        SIZE_T tracked = vm_track_remove((uint64_t)*BaseAddress, &phys);

        /* Keep pages mapped — Win32 apps may access freed VA briefly
         * (FMallocWindows, UE1 TArray realloc patterns). Windows doesn't
         * tear down PTEs immediately on MEM_RELEASE, and game code relies
         * on this. We leak the physical pages; PE32 compat doesn't need
         * to be memory-efficient. */
        nt_log_hex("  release (lazy) size = ", tracked);

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

    /* Convert to ticks (assuming 100Hz timer = 10ms per tick) */
    uint64_t delay_ticks = (uint64_t)(delay_100ns / 100000);
    if (delay_ticks == 0) delay_ticks = 1;

    uint64_t start = idt_get_ticks();
    while (idt_get_ticks() - start < delay_ticks) {
        __asm__ volatile("sti; hlt; cli");
    }

    return STATUS_SUCCESS;
}

/* ── NtQueryPerformanceCounter ──────────────────────────────── */

NTSTATUS sys_NtQueryPerformanceCounter(ULONG_PTR *args)
{
    PLARGE_INTEGER PerformanceCounter   = (PLARGE_INTEGER)args[0];
    PLARGE_INTEGER PerformanceFrequency = (PLARGE_INTEGER)args[1];

    if (PerformanceCounter)
        PerformanceCounter->QuadPart = (LONGLONG)idt_get_ticks();

    if (PerformanceFrequency)
        PerformanceFrequency->QuadPart = 100;  /* 100 Hz timer */

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
