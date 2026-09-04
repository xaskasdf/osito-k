/*
 * OsitoK Windows Compatibility Layer — ntdll.dll Shim Implementation
 *
 * Provides Rtl* utility functions and an export lookup table
 * so PE imports from "ntdll.dll" resolve to our implementations.
 *
 * The Nt* functions here are kernel-side handlers called directly
 * (in OsitoK's identity-mapped model, user/kernel share address space).
 * When we add proper ring separation, these become SYSCALL wrappers.
 */

#include "ntdll_shim.h"
#include "ntsyscall.h"
#include "handle.h"
#include "filelock.h"
#include "advapi32_shim.h"
#include "kernel32_shim.h"
#include "dllloader.h"
#include "compat32.h"
#include "unwind64.h"
#include "win32_abi.h"

extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern DWORD win32_current_process_id(void);
extern PVOID win32_current_peb_lock(void);
extern HANDLE_TABLE g_handle_table;
extern int g_compat32_mode;
extern int win32_user_range_writable(void *pointer, SIZE_T size,
                                     BOOL compat32);
extern int win32_user_range_readable(const void *pointer, SIZE_T size,
                                     BOOL compat32);
extern int win32_user_range_executable(const void *pointer, SIZE_T size,
                                       BOOL compat32);
extern NTSTATUS ntsync_set_event_for_process(HANDLE event, ULONG owner_pid,
                                              LONG *previous_state);
extern BOOL win32_process_cr3(DWORD process_id, uint64_t *out_cr3);
extern int paging_copy_between_cr3(uint64_t destination_cr3,
                                   uint64_t destination_address,
                                   uint64_t source_cr3,
                                   uint64_t source_address, uint64_t size,
                                   uint64_t *bytes_copied);

/* ── Rtl* Utilities ─────────────────────────────────────────── */

void NTAPI RtlInitUnicodeString(PUNICODE_STRING dest, PCWSTR src)
{
    if (!dest) return;

    if (!src) {
        dest->Length        = 0;
        dest->MaximumLength = 0;
        dest->Buffer        = NULL;
        return;
    }

    USHORT len = 0;
    while (src[len]) len++;

    dest->Length        = len * sizeof(WCHAR);
    dest->MaximumLength = (len + 1) * sizeof(WCHAR);
    dest->Buffer        = (PWSTR)src;
}

NTSTATUS NTAPI RtlFormatCurrentUserKeyPath(PUNICODE_STRING path)
{
    static const WCHAR current_user[] = {
        '\\', 'R', 'e', 'g', 'i', 's', 't', 'r', 'y',
        '\\', 'U', 's', 'e', 'r',
        '\\', 'S', '-', '1', '-', '5', '-', '2', '1', '-', '0', 0
    };
    if (!path) return STATUS_INVALID_PARAMETER;

    PWSTR buffer = (PWSTR)kmalloc(sizeof(current_user));
    if (!buffer) return STATUS_NO_MEMORY;
    RtlCopyMemory(buffer, current_user, sizeof(current_user));

    path->Length = sizeof(current_user) - sizeof(WCHAR);
    path->MaximumLength = sizeof(current_user);
    path->Buffer = buffer;
    return STATUS_SUCCESS;
}

void NTAPI RtlFreeUnicodeString(PUNICODE_STRING value)
{
    if (!value) return;
    if (value->Buffer) kfree(value->Buffer);
    value->Length = 0;
    value->MaximumLength = 0;
    value->Buffer = NULL;
}

NTSTATUS NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION section)
{
    if (!section) return STATUS_INVALID_PARAMETER;
    InitializeCriticalSection(section);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtlInitializeCriticalSectionAndSpinCount(
    PRTL_CRITICAL_SECTION section, ULONG spin_count)
{
    if (!section) return STATUS_INVALID_PARAMETER;
    return InitializeCriticalSectionAndSpinCount(section, spin_count)
        ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

NTSTATUS NTAPI RtlInitializeCriticalSectionEx(PRTL_CRITICAL_SECTION section,
                                               ULONG spin_count,
                                               ULONG flags)
{
    (void)flags;
    return RtlInitializeCriticalSectionAndSpinCount(section, spin_count);
}

NTSTATUS NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION section)
{
    if (!section) return STATUS_INVALID_PARAMETER;
    EnterCriticalSection(section);
    return STATUS_SUCCESS;
}

BOOL NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION section)
{
    return section ? TryEnterCriticalSection(section) : FALSE;
}

NTSTATUS NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION section)
{
    if (!section) return STATUS_INVALID_PARAMETER;
    LeaveCriticalSection(section);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION section)
{
    if (!section) return STATUS_INVALID_PARAMETER;
    DeleteCriticalSection(section);
    return STATUS_SUCCESS;
}

void NTAPI RtlAcquirePebLock(void)
{
    PRTL_CRITICAL_SECTION lock =
        (PRTL_CRITICAL_SECTION)win32_current_peb_lock();
    if (lock) EnterCriticalSection(lock);
}

BOOL NTAPI RtlTryAcquirePebLock(void)
{
    PRTL_CRITICAL_SECTION lock =
        (PRTL_CRITICAL_SECTION)win32_current_peb_lock();
    return lock ? TryEnterCriticalSection(lock) : FALSE;
}

void NTAPI RtlReleasePebLock(void)
{
    PRTL_CRITICAL_SECTION lock =
        (PRTL_CRITICAL_SECTION)win32_current_peb_lock();
    if (lock) LeaveCriticalSection(lock);
}

NTSTATUS NTAPI RtlUnicodeStringToAnsiString(PSTR dest, PCUNICODE_STRING src,
                                       ULONG dest_size)
{
    if (!dest || !src)
        return STATUS_INVALID_PARAMETER;

    ULONG chars = src->Length / sizeof(WCHAR);
    if (chars >= dest_size)
        chars = dest_size - 1;

    for (ULONG i = 0; i < chars; i++)
        dest[i] = (char)(src->Buffer[i] & 0xFF);

    dest[chars] = '\0';
    return STATUS_SUCCESS;
}

void NTAPI RtlCopyMemory(PVOID dest, PCVOID src, SIZE_T length)
{
    BYTE *d = (BYTE *)dest;
    const BYTE *s = (const BYTE *)src;
    while (length--) *d++ = *s++;
}

void NTAPI RtlZeroMemory(PVOID dest, SIZE_T length)
{
    BYTE *d = (BYTE *)dest;
    while (length--) *d++ = 0;
}

void NTAPI RtlFillMemory(PVOID dest, SIZE_T length, BYTE fill)
{
    BYTE *d = (BYTE *)dest;
    while (length--) *d++ = fill;
}

/* NTSTATUS → Win32 error code (simplified mapping) */
ULONG NTAPI RtlNtStatusToDosError(NTSTATUS status)
{
    switch (status) {
    case STATUS_SUCCESS:                return 0;      /* ERROR_SUCCESS */
    case STATUS_INVALID_PARAMETER:      return 87;     /* ERROR_INVALID_PARAMETER */
    case STATUS_NO_MEMORY:              return 8;      /* ERROR_NOT_ENOUGH_MEMORY */
    case STATUS_INVALID_HANDLE:         return 6;      /* ERROR_INVALID_HANDLE */
    case STATUS_HANDLE_NOT_CLOSABLE:    return 6;      /* ERROR_INVALID_HANDLE */
    case STATUS_ACCESS_DENIED:          return 5;      /* ERROR_ACCESS_DENIED */
    case STATUS_INVALID_DEVICE_REQUEST: return 1;      /* ERROR_INVALID_FUNCTION */
    case STATUS_OBJECT_NAME_NOT_FOUND:  return 2;      /* ERROR_FILE_NOT_FOUND */
    case STATUS_OBJECT_PATH_NOT_FOUND:  return 3;      /* ERROR_PATH_NOT_FOUND */
    case STATUS_OBJECT_NAME_EXISTS:     return 183;    /* ERROR_ALREADY_EXISTS */
    case STATUS_OBJECT_NAME_COLLISION:  return 183;    /* ERROR_ALREADY_EXISTS */
    case STATUS_ACCESS_VIOLATION:       return 998;    /* ERROR_NOACCESS */
    case STATUS_NOT_IMPLEMENTED:        return 120;    /* ERROR_CALL_NOT_IMPLEMENTED */
    case STATUS_NOT_SUPPORTED:          return 50;     /* ERROR_NOT_SUPPORTED */
    case STATUS_INSUFFICIENT_RESOURCES: return 8;      /* ERROR_NOT_ENOUGH_MEMORY */
    case STATUS_END_OF_FILE:            return 38;     /* ERROR_HANDLE_EOF */
    case STATUS_FILE_LOCK_CONFLICT:     return 33;     /* ERROR_LOCK_VIOLATION */
    case STATUS_LOCK_NOT_GRANTED:       return 33;     /* ERROR_LOCK_VIOLATION */
    case STATUS_RANGE_NOT_LOCKED:       return 158;    /* ERROR_NOT_LOCKED */
    case STATUS_CANCELLED:              return 995;    /* ERROR_OPERATION_ABORTED */
    case STATUS_IO_TIMEOUT:             return 121;    /* ERROR_SEM_TIMEOUT */
    case STATUS_CONNECTION_RESET:       return 64;     /* ERROR_NETNAME_DELETED */
    case STATUS_CONNECTION_REFUSED:     return 1225;   /* ERROR_CONNECTION_REFUSED */
    default:                            return 317;    /* ERROR_MR_MID_NOT_FOUND */
    }
}

/* ── Export table ────────────────────────────────────────────── */

static int nt_registry_prefix(const char *path, const char *prefix,
                              const char **subkey)
{
    while (*prefix) {
        if (!*path) return 0;
        char a = *path++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    if (*path == '\\') path++;
    else if (*path) return 0;
    *subkey = path;
    return 1;
}

static NTSTATUS nt_registry_object(POBJECT_ATTRIBUTES attributes,
                                   HKEY *root, const char **subkey,
                                   char *path, SIZE_T path_size)
{
    if (!attributes || !attributes->ObjectName ||
        !attributes->ObjectName->Buffer ||
        (attributes->ObjectName->Length & 1))
        return STATUS_INVALID_PARAMETER;

    PCUNICODE_STRING name = attributes->ObjectName;
    ULONG chars = name->Length / sizeof(WCHAR);
    if (chars >= path_size) return STATUS_OBJECT_NAME_INVALID;
    for (ULONG i = 0; i < chars; i++) {
        if (name->Buffer[i] > 0x7f) return STATUS_OBJECT_NAME_INVALID;
        path[i] = (char)name->Buffer[i];
    }
    path[chars] = 0;

    *root = (HKEY)attributes->RootDirectory;
    *subkey = path;
    if (*root) return STATUS_SUCCESS;
    if (nt_registry_prefix(path, "\\Registry\\Machine", subkey))
        *root = HKEY_LOCAL_MACHINE;
    else if (nt_registry_prefix(path, "\\Registry\\User\\S-1-5-21-0", subkey))
        *root = HKEY_CURRENT_USER;
    else if (nt_registry_prefix(path, "\\Registry\\User", subkey))
        *root = HKEY_USERS;
    else
        return STATUS_OBJECT_PATH_SYNTAX_BAD;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtOpenKeyEx(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess,
                           POBJECT_ATTRIBUTES ObjectAttributes,
                           ULONG OpenOptions)
{
    if (!KeyHandle) return STATUS_INVALID_PARAMETER;
    char path[256];
    HKEY root;
    const char *subkey;
    NTSTATUS status = nt_registry_object(ObjectAttributes, &root, &subkey,
                                         path, sizeof(path));
    if (!NT_SUCCESS(status)) return status;

    *KeyHandle = NULL;
    LONG error = RegOpenKeyExA(root, subkey, OpenOptions, DesiredAccess,
                               (PHKEY)KeyHandle);
    return error == ERROR_SUCCESS ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS NTAPI NtCreateKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess,
                           POBJECT_ATTRIBUTES ObjectAttributes,
                           ULONG TitleIndex, PUNICODE_STRING Class,
                           ULONG CreateOptions, ULONG *Disposition)
{
    (void)TitleIndex;
    (void)Class;
    if (!KeyHandle) return STATUS_INVALID_PARAMETER;

    char path[256];
    HKEY root;
    const char *subkey;
    NTSTATUS status = nt_registry_object(ObjectAttributes, &root, &subkey,
                                         path, sizeof(path));
    if (!NT_SUCCESS(status)) return status;

    *KeyHandle = NULL;
    LONG error = RegCreateKeyExA(root, subkey, 0, NULL, CreateOptions,
                                 DesiredAccess, NULL, (PHKEY)KeyHandle,
                                 (DWORD *)Disposition);
    return error == ERROR_SUCCESS ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS NTAPI NtQueryValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName,
                               ULONG InformationClass, PVOID Information,
                               ULONG Length, ULONG *ResultLength)
{
    if (!ValueName || (ValueName->Length & 1) ||
        (ValueName->Length && !ValueName->Buffer))
        return STATUS_INVALID_PARAMETER;
    if (InformationClass != 1) return STATUS_INVALID_INFO_CLASS;

    char value_name[128];
    ULONG chars = ValueName->Length / sizeof(WCHAR);
    if (chars >= sizeof(value_name)) return STATUS_OBJECT_NAME_INVALID;
    for (ULONG i = 0; i < chars; i++) {
        if (ValueName->Buffer[i] > 0x7f) return STATUS_OBJECT_NAME_INVALID;
        value_name[i] = (char)ValueName->Buffer[i];
    }
    value_name[chars] = 0;

    DWORD type = 0, data_length = 0;
    LONG error = RegQueryValueExA((HKEY)KeyHandle, value_name, NULL, &type,
                                  NULL, &data_length);
    if (error != ERROR_SUCCESS) return STATUS_OBJECT_NAME_NOT_FOUND;

    ULONG name_length = ValueName->Length;
    ULONG data_offset = (20 + name_length + 3) & ~3U;
    ULONG required = data_offset + data_length;
    if (ResultLength) *ResultLength = required;
    if (!Information || Length < required) return STATUS_BUFFER_OVERFLOW;

    ULONG *header = (ULONG *)Information;
    header[0] = 0;
    header[1] = type;
    header[2] = data_offset;
    header[3] = data_length;
    header[4] = name_length;
    RtlCopyMemory((BYTE *)Information + 20, ValueName->Buffer, name_length);

    DWORD capacity = data_length;
    error = RegQueryValueExA((HKEY)KeyHandle, value_name, NULL, NULL,
                             (BYTE *)Information + data_offset, &capacity);
    return error == ERROR_SUCCESS ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS NTAPI NtSetValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName,
                             ULONG TitleIndex, ULONG Type, PVOID Data,
                             ULONG DataSize)
{
    (void)TitleIndex;
    if (!KeyHandle || !ValueName || (ValueName->Length & 1) ||
        (ValueName->Length && !ValueName->Buffer) ||
        (DataSize && !Data))
        return STATUS_INVALID_PARAMETER;

    char value_name[128];
    ULONG chars = ValueName->Length / sizeof(WCHAR);
    if (chars >= sizeof(value_name))
        return STATUS_OBJECT_NAME_INVALID;
    for (ULONG i = 0; i < chars; i++) {
        if (ValueName->Buffer[i] > 0x7f)
            return STATUS_OBJECT_NAME_INVALID;
        value_name[i] = (char)ValueName->Buffer[i];
    }
    value_name[chars] = 0;

    LONG error = RegSetValueExA((HKEY)KeyHandle, value_name, 0, Type,
                                (const BYTE *)Data, DataSize);
    if (error == ERROR_SUCCESS)
        return STATUS_SUCCESS;
    if (error == 6) /* ERROR_INVALID_HANDLE */
        return STATUS_INVALID_HANDLE;
    if (error == 8) /* ERROR_NOT_ENOUGH_MEMORY */
        return STATUS_NO_MEMORY;
    if (error == ERROR_FILE_NOT_FOUND)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS NTAPI NtDeleteKey_stub(HANDLE key_handle)
{
    if (!key_handle) return STATUS_INVALID_HANDLE;
    /* Registry keys are currently in-memory objects without persistent child
     * enumeration. Treat deletion as successful and let NtClose retire the
     * handle normally. */
    return STATUS_SUCCESS;
}

typedef struct _SHIM_EXPORT {
    const char *name;
    PVOID       func;
    uint8_t     argc;
    uint8_t     cc;
} SHIM_EXPORT;

/* Forward declare NT handlers — these are the kernel-side implementations
 * from ntsyscall.c. In OsitoK's flat address space, PE code calls them
 * directly. With ring separation, replace with SYSCALL stubs. */
extern NTSTATUS sys_NtCreateFile(ULONG_PTR *args);
extern NTSTATUS sys_NtReadFile(ULONG_PTR *args);
extern NTSTATUS sys_NtWriteFile(ULONG_PTR *args);
extern NTSTATUS sys_NtClose(ULONG_PTR *args);
extern NTSTATUS sys_NtAllocateVirtualMemory(ULONG_PTR *args);
extern NTSTATUS sys_NtFreeVirtualMemory(ULONG_PTR *args);
extern NTSTATUS sys_NtTerminateProcess(ULONG_PTR *args);
extern NTSTATUS sys_NtDelayExecution(ULONG_PTR *args);
extern NTSTATUS sys_NtQueryPerformanceCounter(ULONG_PTR *args);

/*
 * NOTE: The Nt* functions as exported to user code take normal C args,
 * not ULONG_PTR arrays. We need thin wrapper functions that match the
 * actual NT API signatures. In the identity-mapped model, these wrappers
 * pack args into an array and call the kernel handler. With proper SYSCALL,
 * they'd do: mov eax, SYS_NUM; syscall; ret.
 */

/* Wrapper: NtCreateFile with proper signature → array-based handler */
NTSTATUS NTAPI NtCreateFile(PHANDLE fh, ACCESS_MASK access, POBJECT_ATTRIBUTES oa,
                      PIO_STATUS_BLOCK iosb, PLARGE_INTEGER alloc_size,
                      ULONG attrs, ULONG share, ULONG disp, ULONG opts,
                      PVOID ea, ULONG ea_len)
{
    ULONG_PTR args[11] = {
        (ULONG_PTR)fh, (ULONG_PTR)access, (ULONG_PTR)oa,
        (ULONG_PTR)iosb, (ULONG_PTR)alloc_size, (ULONG_PTR)attrs,
        (ULONG_PTR)share, (ULONG_PTR)disp, (ULONG_PTR)opts,
        (ULONG_PTR)ea, (ULONG_PTR)ea_len
    };
    return sys_NtCreateFile(args);
}

NTSTATUS NTAPI NtReadFile(HANDLE fh, HANDLE event, PVOID apc_routine,
                    PVOID apc_ctx, PIO_STATUS_BLOCK iosb,
                    PVOID buf, ULONG len, PLARGE_INTEGER offset, PVOID key)
{
    ULONG_PTR args[9] = {
        (ULONG_PTR)fh, (ULONG_PTR)event, (ULONG_PTR)apc_routine,
        (ULONG_PTR)apc_ctx, (ULONG_PTR)iosb, (ULONG_PTR)buf,
        (ULONG_PTR)len, (ULONG_PTR)offset, (ULONG_PTR)key
    };
    return sys_NtReadFile(args);
}

NTSTATUS NTAPI NtWriteFile(HANDLE fh, HANDLE event, PVOID apc_routine,
                     PVOID apc_ctx, PIO_STATUS_BLOCK iosb,
                     PVOID buf, ULONG len, PLARGE_INTEGER offset, PVOID key)
{
    ULONG_PTR args[9] = {
        (ULONG_PTR)fh, (ULONG_PTR)event, (ULONG_PTR)apc_routine,
        (ULONG_PTR)apc_ctx, (ULONG_PTR)iosb, (ULONG_PTR)buf,
        (ULONG_PTR)len, (ULONG_PTR)offset, (ULONG_PTR)key
    };
    return sys_NtWriteFile(args);
}

typedef struct _NTDLL_PENDING_FILE_LOCK {
    HANDLE event;
    PIO_STATUS_BLOCK iosb;
    ULONG owner_pid;
    BOOL compat32;
} NTDLL_PENDING_FILE_LOCK;

static void ntdll_store_file_lock_iosb(PIO_STATUS_BLOCK iosb, BOOL compat32,
                                       NTSTATUS status)
{
    if (!iosb)
        return;
    if (compat32) {
        volatile uint32_t *values = (volatile uint32_t *)(PVOID)iosb;
        values[0] = (uint32_t)status;
        values[1] = 0;
    } else {
        iosb->Status = status;
        iosb->Information = 0;
    }
}

static BOOL ntdll_store_file_lock_iosb_for_owner(
    PIO_STATUS_BLOCK iosb, ULONG owner_pid, BOOL compat32, NTSTATUS status)
{
    if (owner_pid == win32_current_process_id()) {
        ntdll_store_file_lock_iosb(iosb, compat32, status);
        return TRUE;
    }

    uint64_t destination_cr3;
    uint64_t source_cr3;
    uint64_t copied = 0;
    if (!iosb || !win32_process_cr3(owner_pid, &destination_cr3))
        return FALSE;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(source_cr3));

    if (compat32) {
        uint32_t values[2] = { (uint32_t)status, 0 };
        return paging_copy_between_cr3(
                   destination_cr3, (uint64_t)(ULONG_PTR)iosb,
                   source_cr3, (uint64_t)(ULONG_PTR)values,
                   sizeof(values), &copied) == 0 &&
               copied == sizeof(values);
    }

    IO_STATUS_BLOCK value;
    value.Status = status;
    value.Information = 0;
    return paging_copy_between_cr3(
               destination_cr3, (uint64_t)(ULONG_PTR)iosb,
               source_cr3, (uint64_t)(ULONG_PTR)&value,
               sizeof(value), &copied) == 0 &&
           copied == sizeof(value);
}

static NTSTATUS ntdll_complete_file_lock_for_owner(
    HANDLE event, PIO_STATUS_BLOCK iosb, ULONG owner_pid, BOOL compat32,
    NTSTATUS status)
{
    (void)ntdll_store_file_lock_iosb_for_owner(
        iosb, owner_pid, compat32, status);
    if (event)
        (void)ntsync_set_event_for_process(event, owner_pid, NULL);
    return status;
}

static NTSTATUS ntdll_complete_file_lock(HANDLE event,
                                         PIO_STATUS_BLOCK iosb,
                                         NTSTATUS status)
{
    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    return ntdll_complete_file_lock_for_owner(
        event, iosb, owner_pid, g_compat32_mode ? TRUE : FALSE, status);
}

static void ntdll_pending_file_lock_complete(PVOID context, NTSTATUS status)
{
    NTDLL_PENDING_FILE_LOCK *pending =
        (NTDLL_PENDING_FILE_LOCK *)context;
    if (!pending)
        return;
    (void)ntdll_complete_file_lock_for_owner(
        pending->event, pending->iosb, pending->owner_pid,
        pending->compat32, status);
    kfree(pending);
}

NTSTATUS NTAPI NtLockFile(HANDLE file, HANDLE event, PVOID apc_routine,
                          PVOID apc_context, PIO_STATUS_BLOCK iosb,
                          PLARGE_INTEGER offset, PLARGE_INTEGER length,
                          ULONG key, BOOL fail_immediately,
                          BOOL exclusive)
{
    (void)apc_context;
    if (!iosb || !offset || !length)
        return STATUS_INVALID_PARAMETER;
    if (offset->QuadPart < 0 || length->QuadPart < 0)
        return ntdll_complete_file_lock(NULL, iosb,
                                        STATUS_INVALID_PARAMETER);
    if (apc_routine)
        return ntdll_complete_file_lock(NULL, iosb, STATUS_NOT_SUPPORTED);

    if (event) {
        NTSTATUS event_status = NtResetEvent(event, NULL);
        if (!NT_SUCCESS(event_status))
            return ntdll_complete_file_lock(NULL, iosb, event_status);
    }

    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    NTDLL_PENDING_FILE_LOCK *pending = NULL;
    if (!fail_immediately) {
        pending = (NTDLL_PENDING_FILE_LOCK *)kmalloc(sizeof(*pending));
        if (!pending)
            return ntdll_complete_file_lock(
                event, iosb, STATUS_INSUFFICIENT_RESOURCES);
        pending->event = event;
        pending->iosb = iosb;
        pending->owner_pid = owner_pid;
        pending->compat32 = g_compat32_mode ? TRUE : FALSE;
        /* A completion can race the return from the queueing call. */
        ntdll_store_file_lock_iosb(iosb, pending->compat32, STATUS_PENDING);
    }

    NTSTATUS status = nt_file_lock_range_request(
        file, owner_pid, GetCurrentThreadId(),
        (ULONGLONG)offset->QuadPart, (ULONGLONG)length->QuadPart, key,
        fail_immediately ? TRUE : FALSE, exclusive ? TRUE : FALSE, iosb,
        pending ? ntdll_pending_file_lock_complete : NULL, pending);
    if (status == STATUS_PENDING)
        return status;
    if (pending)
        kfree(pending);
    return ntdll_complete_file_lock(event, iosb, status);
}

NTSTATUS NTAPI NtUnlockFile(HANDLE file, PIO_STATUS_BLOCK iosb,
                            PLARGE_INTEGER offset, PLARGE_INTEGER length,
                            ULONG key)
{
    if (!iosb || !offset || !length)
        return STATUS_INVALID_PARAMETER;
    if (offset->QuadPart < 0 || length->QuadPart < 0) {
        ntdll_store_file_lock_iosb(
            iosb, g_compat32_mode ? TRUE : FALSE, STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    NTSTATUS status = nt_file_unlock_range(
        file, owner_pid, (ULONGLONG)offset->QuadPart,
        (ULONGLONG)length->QuadPart, key);
    ntdll_store_file_lock_iosb(iosb, g_compat32_mode ? TRUE : FALSE, status);
    return status;
}

NTSTATUS NTAPI NtClose(HANDLE h)
{
    ULONG_PTR args[1] = { (ULONG_PTR)h };
    return sys_NtClose(args);
}

enum {
    ObjectBasicInformation = 0,
    ObjectNameInformation = 1,
    ObjectTypeInformation = 2,
};

typedef struct _PUBLIC_OBJECT_BASIC_INFORMATION_LOCAL {
    ULONG Attributes;
    ACCESS_MASK GrantedAccess;
    ULONG HandleCount;
    ULONG PointerCount;
    ULONG Reserved[10];
} PUBLIC_OBJECT_BASIC_INFORMATION_LOCAL;

typedef struct _PUBLIC_OBJECT_TYPE_INFORMATION32_LOCAL {
    USHORT Length;
    USHORT MaximumLength;
    ULONG Buffer;
    ULONG Reserved[22];
} PUBLIC_OBJECT_TYPE_INFORMATION32_LOCAL;

typedef struct _PUBLIC_OBJECT_TYPE_INFORMATION64_LOCAL {
    UNICODE_STRING TypeName;
    ULONG Reserved[22];
} PUBLIC_OBJECT_TYPE_INFORMATION64_LOCAL;

static NTSTATUS nt_object_snapshot(HANDLE handle,
                                   HANDLE_OBJECT_SNAPSHOT *snapshot)
{
    RtlZeroMemory(snapshot, sizeof(*snapshot));
    if (handle == NT_CURRENT_PROCESS) {
        snapshot->type = OBJ_TYPE_PROCESS;
        snapshot->access = 0x1FFFFFU;
        snapshot->handle_count = 1;
        snapshot->pointer_count = 1;
        return STATUS_SUCCESS;
    }
    if (handle == NT_CURRENT_THREAD) {
        snapshot->type = OBJ_TYPE_THREAD;
        snapshot->access = 0x1FFFFFU;
        snapshot->handle_count = 1;
        snapshot->pointer_count = 1;
        return STATUS_SUCCESS;
    }

    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    return handle_snapshot_for_process(&g_handle_table, handle, owner_pid,
                                       snapshot);
}

static const char *nt_object_type_name(OBJECT_TYPE_ID type)
{
    switch (type) {
    case OBJ_TYPE_FILE:      return "File";
    case OBJ_TYPE_PROCESS:   return "Process";
    case OBJ_TYPE_THREAD:    return "Thread";
    case OBJ_TYPE_EVENT:     return "Event";
    case OBJ_TYPE_SECTION:   return "Section";
    case OBJ_TYPE_MUTANT:    return "Mutant";
    case OBJ_TYPE_SEMAPHORE: return "Semaphore";
    case OBJ_TYPE_SNAPSHOT:  return "Snapshot";
    case OBJ_TYPE_JOB:       return "Job";
    case OBJ_TYPE_TOKEN:     return "Token";
    case OBJ_TYPE_POWER_REQUEST: return "PowerRequest";
    default:                 return "Unknown";
    }
}

static ULONG nt_append_ascii_w(WCHAR *dst, ULONG pos, ULONG capacity,
                               const char *src)
{
    while (*src && pos < capacity)
        dst[pos++] = (WCHAR)(UCHAR)*src++;
    return pos;
}

static ULONG nt_object_build_name(const HANDLE_OBJECT_SNAPSHOT *snapshot,
                                  WCHAR name[272])
{
    ULONG pos = 0;
    if (snapshot->type != OBJ_TYPE_FILE)
        return 0;

    if (!snapshot->name_length) {
        if (snapshot->file_flags & FILE_OBJ_CONSOLE_IN)
            return nt_append_ascii_w(name, 0, 271, "\\Device\\ConDrv\\Input");
        if (snapshot->file_flags &
            (FILE_OBJ_CONSOLE_OUT | FILE_OBJ_CONSOLE_ERR))
            return nt_append_ascii_w(name, 0, 271,
                                     "\\Device\\ConDrv\\Output");
        return 0;
    }

    if (snapshot->name[0] == '\\') {
        while (pos < snapshot->name_length && pos < 271) {
            name[pos] = snapshot->name[pos];
            pos++;
        }
        return pos;
    }

    pos = nt_append_ascii_w(name, pos, 271, "\\??\\");
    if (!(snapshot->name_length > 1 && snapshot->name[1] == ':'))
        pos = nt_append_ascii_w(name, pos, 271, "C:\\");
    for (ULONG i = 0; i < snapshot->name_length && pos < 271; i++)
        name[pos++] = snapshot->name[i] == '/' ? '\\' : snapshot->name[i];
    return pos;
}

static void nt_store_unicode_string(PVOID information, ULONG string_offset,
                                    const WCHAR *value, ULONG chars)
{
    BYTE *bytes = (BYTE *)information;
    PWSTR buffer = chars ? (PWSTR)(void *)(bytes + string_offset) : NULL;
    USHORT length = (USHORT)(chars * sizeof(WCHAR));
    USHORT maximum = chars ? (USHORT)(length + sizeof(WCHAR)) : 0;

    if (g_compat32_mode) {
        *(USHORT *)(void *)(bytes + 0) = length;
        *(USHORT *)(void *)(bytes + 2) = maximum;
        *(ULONG *)(void *)(bytes + 4) = (ULONG)(ULONG_PTR)buffer;
    } else {
        PUNICODE_STRING string = (PUNICODE_STRING)information;
        string->Length = length;
        string->MaximumLength = maximum;
        string->Buffer = buffer;
    }

    if (!chars) return;
    for (ULONG i = 0; i < chars; i++) buffer[i] = value[i];
    buffer[chars] = 0;
}

static void nt_query_object_trace(HANDLE handle, ULONG info_class,
                                  const HANDLE_OBJECT_SNAPSHOT *snapshot,
                                  const WCHAR *name, ULONG name_chars)
{
    static uint32_t trace_count;
    uint32_t index = __atomic_fetch_add(&trace_count, 1, __ATOMIC_RELAXED);
    if (index >= 32 || info_class != ObjectNameInformation) return;

    char ascii[272];
    ULONG chars = name_chars < sizeof(ascii) - 1
                ? name_chars : sizeof(ascii) - 1;
    for (ULONG i = 0; i < chars; i++)
        ascii[i] = name[i] <= 0x7F ? (char)name[i] : '?';
    ascii[chars] = 0;

    serial_puts("[NTQOBJ] pid=");
    serial_putdec(win32_current_process_id());
    serial_puts(" handle=0x");
    serial_puthex((uint64_t)(ULONG_PTR)handle, 8);
    serial_puts(" type=");
    serial_puts(nt_object_type_name(snapshot->type));
    serial_puts(" name='");
    serial_puts(ascii);
    serial_puts("'\n");
}

NTSTATUS NTAPI NtQueryObject(HANDLE Handle, ULONG ObjectInformationClass,
                             PVOID ObjectInformation,
                             ULONG ObjectInformationLength,
                             ULONG *ReturnLength)
{
    if (ReturnLength) *ReturnLength = 0;
    if (ObjectInformationClass > ObjectTypeInformation)
        return STATUS_INVALID_INFO_CLASS;

    HANDLE_OBJECT_SNAPSHOT snapshot;
    NTSTATUS status = nt_object_snapshot(Handle, &snapshot);
    if (!NT_SUCCESS(status)) return status;

    if (ObjectInformationClass == ObjectBasicInformation) {
        ULONG required = sizeof(PUBLIC_OBJECT_BASIC_INFORMATION_LOCAL);
        if (ReturnLength) *ReturnLength = required;
        if (!ObjectInformation || ObjectInformationLength < required)
            return STATUS_INFO_LENGTH_MISMATCH;

        PUBLIC_OBJECT_BASIC_INFORMATION_LOCAL *info =
            (PUBLIC_OBJECT_BASIC_INFORMATION_LOCAL *)ObjectInformation;
        RtlZeroMemory(info, required);
        info->GrantedAccess = snapshot.access;
        info->HandleCount = snapshot.handle_count;
        info->PointerCount = snapshot.pointer_count;
        return STATUS_SUCCESS;
    }

    if (ObjectInformationClass == ObjectNameInformation) {
        WCHAR name[272];
        ULONG chars = nt_object_build_name(&snapshot, name);
        ULONG header = g_compat32_mode ? 8 : sizeof(UNICODE_STRING);
        ULONG required = header + (chars ? (chars + 1) * sizeof(WCHAR) : 0);
        if (ReturnLength) *ReturnLength = required;
        if (!ObjectInformation || ObjectInformationLength < required)
            return STATUS_INFO_LENGTH_MISMATCH;

        RtlZeroMemory(ObjectInformation, required);
        nt_store_unicode_string(ObjectInformation, header, name, chars);
        nt_query_object_trace(Handle, ObjectInformationClass, &snapshot,
                              name, chars);
        return STATUS_SUCCESS;
    }

    const char *type_name = nt_object_type_name(snapshot.type);
    WCHAR type_name_w[16];
    ULONG chars = 0;
    while (type_name[chars] && chars < 15) {
        type_name_w[chars] = (WCHAR)(UCHAR)type_name[chars];
        chars++;
    }
    ULONG header = g_compat32_mode
                 ? sizeof(PUBLIC_OBJECT_TYPE_INFORMATION32_LOCAL)
                 : sizeof(PUBLIC_OBJECT_TYPE_INFORMATION64_LOCAL);
    ULONG required = header + (chars + 1) * sizeof(WCHAR);
    if (ReturnLength) *ReturnLength = required;
    if (!ObjectInformation || ObjectInformationLength < required)
        return STATUS_INFO_LENGTH_MISMATCH;

    RtlZeroMemory(ObjectInformation, required);
    nt_store_unicode_string(ObjectInformation, header, type_name_w, chars);
    return STATUS_SUCCESS;
}

static void nt_object_test_expect(BOOL condition, const char *name,
                                  int *checks, int *failures)
{
    (*checks)++;
    if (condition) return;
    (*failures)++;
    serial_puts("[NTOBJTEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

int ntdll_object_selftest(void)
{
    FILE_OBJECT file;
    HANDLE handle = NULL;
    BYTE buffer[640] __attribute__((aligned(8)));
    ULONG required = 0;
    int checks = 0, failures = 0;
    int saved_mode = g_compat32_mode;
    static const char raw_name[] = "System\\query-probe.bin";

    g_compat32_mode = 0;
    RtlZeroMemory(&file, sizeof(file));
    file.flags = FILE_OBJ_DISK_FILE;
    for (ULONG i = 0; raw_name[i] && i < 259; i++)
        file.name[i] = (WCHAR)(UCHAR)raw_name[i];

    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_FILE,
                                   0x00120089U, &file, &handle);
    nt_object_test_expect(NT_SUCCESS(status), "allocate probe handle",
                          &checks, &failures);
    if (!NT_SUCCESS(status)) goto done;

    status = NtQueryObject(handle, ObjectNameInformation, NULL, 0, &required);
    nt_object_test_expect(status == STATUS_INFO_LENGTH_MISMATCH && required > 16,
                          "name size query", &checks, &failures);

    status = NtQueryObject(handle, ObjectNameInformation, buffer,
                           sizeof(buffer), &required);
    PUNICODE_STRING name = (PUNICODE_STRING)(void *)buffer;
    BOOL valid_name = NT_SUCCESS(status) && name->Buffer &&
                      name->Length >= 14 && name->Buffer[0] == '\\' &&
                      name->Buffer[1] == '?' && name->Buffer[2] == '?' &&
                      name->Buffer[3] == '\\' && name->Buffer[4] == 'C' &&
                      name->Buffer[5] == ':' && name->Buffer[6] == '\\';
    nt_object_test_expect(valid_name, "native file object name",
                          &checks, &failures);

    status = NtQueryObject(handle, ObjectTypeInformation, buffer,
                           sizeof(buffer), &required);
    PUNICODE_STRING type = (PUNICODE_STRING)(void *)buffer;
    nt_object_test_expect(NT_SUCCESS(status) && type->Length == 8 &&
                          type->Buffer && type->Buffer[0] == 'F' &&
                          type->Buffer[3] == 'e', "file type name",
                          &checks, &failures);

    status = NtQueryObject(handle, ObjectBasicInformation, buffer,
                           sizeof(buffer), &required);
    PUBLIC_OBJECT_BASIC_INFORMATION_LOCAL *basic =
        (PUBLIC_OBJECT_BASIC_INFORMATION_LOCAL *)(void *)buffer;
    nt_object_test_expect(NT_SUCCESS(status) &&
                          basic->GrantedAccess == 0x00120089U &&
                          basic->HandleCount >= 1 && basic->PointerCount >= 1,
                          "basic handle metadata", &checks, &failures);

    status = NtQueryObject(NT_CURRENT_PROCESS, ObjectTypeInformation, buffer,
                           sizeof(buffer), &required);
    type = (PUNICODE_STRING)(void *)buffer;
    nt_object_test_expect(NT_SUCCESS(status) && type->Length == 14 &&
                          type->Buffer && type->Buffer[0] == 'P',
                          "current-process pseudo handle", &checks, &failures);

    status = NtQueryObject((HANDLE)(ULONG_PTR)0xFFFFFFFCU,
                           ObjectNameInformation, buffer, sizeof(buffer),
                           &required);
    nt_object_test_expect(status == STATUS_INVALID_HANDLE,
                          "invalid handle rejection", &checks, &failures);

    handle_close(&g_handle_table, handle);
done:
    g_compat32_mode = saved_mode;
    serial_puts("[NTOBJTEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

/*
 * WoW64-style pointer-width thunking for NtAllocateVirtualMemory.
 *
 * When called from 32-bit PE32 code via INT 0x2E thunk, the `base` and
 * `size` parameters are pointers to 4-byte DWORDs (PVOID32/SIZE_T32).
 * But sys_NtAllocateVirtualMemory reads/writes them as 8-byte values.
 * Without thunking, it reads 4 bytes of garbage above each DWORD and
 * writes 4 bytes of corruption after each DWORD.
 *
 * Fix: detect compat32 mode, read DWORD→uint64_t, call syscall with
 * 64-bit temporaries, then truncate results back to DWORD.
 */
NTSTATUS NTAPI NtAllocateVirtualMemory(HANDLE proc, PVOID *base, ULONG_PTR zbits,
                                  SIZE_T *size, ULONG type, ULONG prot)
{

    if (g_compat32_mode) {
        /* base and size point to 4-byte DWORDs in 32-bit memory */
        uint32_t *base32 = (uint32_t *)base;
        uint32_t *size32 = (uint32_t *)size;

        PVOID  base64 = (PVOID)(ULONG_PTR)*base32;
        SIZE_T size64 = (SIZE_T)*size32;

        ULONG_PTR args[6] = {
            (ULONG_PTR)proc, (ULONG_PTR)&base64, zbits,
            (ULONG_PTR)&size64, type, prot
        };
        NTSTATUS st = sys_NtAllocateVirtualMemory(args);

        if (st == 0) {  /* STATUS_SUCCESS */
            *base32 = (uint32_t)(ULONG_PTR)base64;
            *size32 = (uint32_t)size64;
        }
        return st;
    }

    ULONG_PTR args[6] = {
        (ULONG_PTR)proc, (ULONG_PTR)base, zbits,
        (ULONG_PTR)size, type, prot
    };
    return sys_NtAllocateVirtualMemory(args);
}

NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE proc, PVOID *base, SIZE_T *size, ULONG type)
{

    if (g_compat32_mode) {
        uint32_t *base32 = (uint32_t *)base;
        uint32_t *size32 = (uint32_t *)size;

        PVOID  base64 = (PVOID)(ULONG_PTR)*base32;
        SIZE_T size64 = size32 ? (SIZE_T)*size32 : 0;

        ULONG_PTR args[4] = {
            (ULONG_PTR)proc, (ULONG_PTR)&base64,
            (ULONG_PTR)(size32 ? &size64 : NULL), type
        };
        NTSTATUS st = sys_NtFreeVirtualMemory(args);

        if (st == 0) {
            *base32 = (uint32_t)(ULONG_PTR)base64;
            if (size32) *size32 = (uint32_t)size64;
        }
        return st;
    }

    ULONG_PTR args[4] = {
        (ULONG_PTR)proc, (ULONG_PTR)base,
        (ULONG_PTR)size, type
    };
    return sys_NtFreeVirtualMemory(args);
}

NTSTATUS NTAPI NtTerminateProcess(HANDLE proc, NTSTATUS exit_status)
{
    ULONG_PTR args[2] = { (ULONG_PTR)proc, (ULONG_PTR)exit_status };
    return sys_NtTerminateProcess(args);
}

NTSTATUS NTAPI NtDelayExecution(BOOL alertable, PLARGE_INTEGER delay)
{
    ULONG_PTR args[2] = { (ULONG_PTR)alertable, (ULONG_PTR)delay };
    return sys_NtDelayExecution(args);
}

NTSTATUS NTAPI NtQueryPerformanceCounter(PLARGE_INTEGER counter, PLARGE_INTEGER freq)
{
    ULONG_PTR args[2] = { (ULONG_PTR)counter, (ULONG_PTR)freq };
    return sys_NtQueryPerformanceCounter(args);
}

/* New handlers */
extern NTSTATUS sys_NtQueryInformationFile(ULONG_PTR *args);
extern NTSTATUS sys_NtSetInformationFile(ULONG_PTR *args);
extern NTSTATUS sys_NtDuplicateObject(ULONG_PTR *args);
extern NTSTATUS sys_NtProtectVirtualMemory(ULONG_PTR *args);
extern NTSTATUS sys_NtQueryVirtualMemory(ULONG_PTR *args);
extern NTSTATUS sys_NtQueryInformationProcess(ULONG_PTR *args);

NTSTATUS NTAPI NtQueryInformationFile(HANDLE fh, PIO_STATUS_BLOCK iosb,
                                 PVOID info, ULONG len,
                                 FILE_INFORMATION_CLASS cls)
{
    ULONG_PTR args[5] = {
        (ULONG_PTR)fh, (ULONG_PTR)iosb, (ULONG_PTR)info,
        (ULONG_PTR)len, (ULONG_PTR)cls
    };
    return sys_NtQueryInformationFile(args);
}

NTSTATUS NTAPI NtSetInformationFile(HANDLE fh, PIO_STATUS_BLOCK iosb,
                               PVOID info, ULONG len,
                               FILE_INFORMATION_CLASS cls)
{
    ULONG_PTR args[5] = {
        (ULONG_PTR)fh, (ULONG_PTR)iosb, (ULONG_PTR)info,
        (ULONG_PTR)len, (ULONG_PTR)cls
    };
    return sys_NtSetInformationFile(args);
}

NTSTATUS NTAPI NtDuplicateObject(HANDLE src_proc, HANDLE src_handle,
                            HANDLE tgt_proc, PHANDLE tgt_handle,
                            ACCESS_MASK access, ULONG attrs, ULONG options)
{
    HANDLE target64 = NULL;
    PHANDLE syscall_target = g_compat32_mode && tgt_handle
        ? &target64 : tgt_handle;
    ULONG_PTR args[7] = {
        (ULONG_PTR)src_proc, (ULONG_PTR)src_handle,
        (ULONG_PTR)tgt_proc, (ULONG_PTR)syscall_target,
        (ULONG_PTR)access, (ULONG_PTR)attrs, (ULONG_PTR)options
    };
    NTSTATUS status = sys_NtDuplicateObject(args);
    if (NT_SUCCESS(status) && g_compat32_mode && tgt_handle)
        *(uint32_t *)(void *)tgt_handle = (uint32_t)(ULONG_PTR)target64;
    return status;
}

NTSTATUS NTAPI NtProtectVirtualMemory(HANDLE proc, PVOID *base, SIZE_T *size,
                                 ULONG new_prot, ULONG *old_prot)
{

    if (g_compat32_mode) {
        uint32_t *base32 = (uint32_t *)base;
        uint32_t *size32 = (uint32_t *)size;

        PVOID  base64 = (PVOID)(ULONG_PTR)*base32;
        SIZE_T size64 = (SIZE_T)*size32;

        ULONG_PTR args[5] = {
            (ULONG_PTR)proc, (ULONG_PTR)&base64, (ULONG_PTR)&size64,
            (ULONG_PTR)new_prot, (ULONG_PTR)old_prot
        };
        NTSTATUS st = sys_NtProtectVirtualMemory(args);

        if (st == 0) {
            *base32 = (uint32_t)(ULONG_PTR)base64;
            *size32 = (uint32_t)size64;
        }
        return st;
    }

    ULONG_PTR args[5] = {
        (ULONG_PTR)proc, (ULONG_PTR)base, (ULONG_PTR)size,
        (ULONG_PTR)new_prot, (ULONG_PTR)old_prot
    };
    return sys_NtProtectVirtualMemory(args);
}

typedef struct {
    PVOID  BaseAddress;
    PVOID  AllocationBase;
    ULONG  AllocationProtect;
    USHORT PartitionId;
    USHORT Padding0;
    SIZE_T RegionSize;
    ULONG  State;
    ULONG  Protect;
    ULONG  Type;
    ULONG  Padding1;
} NTDLL_MEMORY_BASIC_INFORMATION64;

typedef struct {
    uint32_t BaseAddress;
    uint32_t AllocationBase;
    ULONG    AllocationProtect;
    uint32_t RegionSize;
    ULONG    State;
    ULONG    Protect;
    ULONG    Type;
} NTDLL_MEMORY_BASIC_INFORMATION32;

_Static_assert(sizeof(NTDLL_MEMORY_BASIC_INFORMATION64) == 48,
               "native MEMORY_BASIC_INFORMATION layout changed");
_Static_assert(sizeof(NTDLL_MEMORY_BASIC_INFORMATION32) == 28,
               "WOW32 MEMORY_BASIC_INFORMATION layout changed");

NTSTATUS NTAPI NtQueryVirtualMemory(HANDLE proc, PVOID base, ULONG info_class,
                               PVOID info, SIZE_T info_len, SIZE_T *ret_len)
{
    if (g_compat32_mode && info_class == 0) {
        uint32_t *ret_len32 = (uint32_t *)(PVOID)ret_len;
        if (ret_len32)
            *ret_len32 = sizeof(NTDLL_MEMORY_BASIC_INFORMATION32);
        if (info_len < sizeof(NTDLL_MEMORY_BASIC_INFORMATION32))
            return STATUS_INFO_LENGTH_MISMATCH;
        if (!info)
            return STATUS_INVALID_PARAMETER;

        NTDLL_MEMORY_BASIC_INFORMATION64 native_info = {0};
        SIZE_T native_ret_len = 0;
        ULONG_PTR args[6] = {
            (ULONG_PTR)proc, (ULONG_PTR)base, (ULONG_PTR)info_class,
            (ULONG_PTR)&native_info, sizeof(native_info),
            (ULONG_PTR)&native_ret_len
        };
        NTSTATUS status = sys_NtQueryVirtualMemory(args);
        if (!NT_SUCCESS(status))
            return status;

        NTDLL_MEMORY_BASIC_INFORMATION32 *info32 =
            (NTDLL_MEMORY_BASIC_INFORMATION32 *)info;
        info32->BaseAddress = (uint32_t)(ULONG_PTR)native_info.BaseAddress;
        info32->AllocationBase =
            (uint32_t)(ULONG_PTR)native_info.AllocationBase;
        info32->AllocationProtect = native_info.AllocationProtect;
        info32->RegionSize = native_info.RegionSize > 0xFFFFFFFFULL
                             ? 0xFFFFFFFFU
                             : (uint32_t)native_info.RegionSize;
        info32->State = native_info.State;
        info32->Protect = native_info.Protect;
        info32->Type = native_info.Type;
        return STATUS_SUCCESS;
    }

    ULONG_PTR args[6] = {
        (ULONG_PTR)proc, (ULONG_PTR)base, (ULONG_PTR)info_class,
        (ULONG_PTR)info, (ULONG_PTR)info_len, (ULONG_PTR)ret_len
    };
    return sys_NtQueryVirtualMemory(args);
}

typedef PEB NTDLL_INSPECTION_PEB64;

typedef struct {
    PVOID Reserved1[2];
    ULONGLONG PebBaseAddress;
    PVOID Reserved2[4];
    ULONG_PTR UniqueProcessId[2];
    PVOID Reserved3[2];
} PROCESS_BASIC_INFORMATION_WOW64;

_Static_assert(sizeof(NTDLL_INSPECTION_PEB64) == 0x2C8,
               "native inspection PEB layout changed");
_Static_assert(sizeof(PROCESS_BASIC_INFORMATION_WOW64) == 88,
               "WOW64 process information layout changed");

#define NTDLL_INSPECTION_PROCESS_SLOTS 32

typedef struct {
    ULONG process_id;
    NTDLL_INSPECTION_PEB64 peb;
} NTDLL_PROCESS_INSPECTION;

static NTDLL_PROCESS_INSPECTION
    ntdll_process_inspections[NTDLL_INSPECTION_PROCESS_SLOTS];
static volatile ULONG ntdll_process_inspection_lock;

/* Chromium reads the complete 64-bit GDI shared table while collecting crash
 * diagnostics: 65,536 GDICELL entries at 24 bytes each.  Keep a zero-filled
 * inspection table mapped so NtReadVirtualMemory can satisfy that ABI. */
#define NTDLL_GDI_TABLE_ENTRY_COUNT 65536u
#define NTDLL_GDI_TABLE_ENTRY_SIZE  24u
static BYTE ntdll_inspection_gdi_table[
    NTDLL_GDI_TABLE_ENTRY_COUNT * NTDLL_GDI_TABLE_ENTRY_SIZE];

PVOID ntdll_shared_gdi_table(void)
{
    return ntdll_inspection_gdi_table;
}

static void ntdll_inspection_lock(void)
{
    while (__atomic_exchange_n(&ntdll_process_inspection_lock, 1,
                               __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void ntdll_inspection_unlock(void)
{
    __atomic_store_n(&ntdll_process_inspection_lock, 0, __ATOMIC_RELEASE);
}

static NTDLL_INSPECTION_PEB64 *
ntdll_prepare_inspection_peb(ULONG process_id, const PEB *native_peb)
{
    if (!process_id) return NULL;

    NTDLL_PROCESS_INSPECTION *available = NULL;
    ntdll_inspection_lock();
    for (ULONG i = 0; i < NTDLL_INSPECTION_PROCESS_SLOTS; i++) {
        NTDLL_PROCESS_INSPECTION *slot = &ntdll_process_inspections[i];
        if (slot->process_id == process_id) {
            available = slot;
            break;
        }
        if (!slot->process_id && !available)
            available = slot;
    }
    if (!available) {
        ntdll_inspection_unlock();
        return NULL;
    }

    if (!available->process_id)
        available->process_id = process_id;
    if (native_peb)
        available->peb = *native_peb;
    else
        RtlZeroMemory(&available->peb, sizeof(available->peb));
    if (!available->peb.ProcessHeap)
        available->peb.ProcessHeap = (PVOID)(ULONG_PTR)0xBEEF0001;
    if (!available->peb.GdiSharedHandleTable)
        available->peb.GdiSharedHandleTable = ntdll_inspection_gdi_table;
    NTDLL_INSPECTION_PEB64 *peb = &available->peb;
    ntdll_inspection_unlock();
    return peb;
}

void ntdll_release_process_inspection(ULONG process_id)
{
    if (!process_id) return;
    ntdll_inspection_lock();
    for (ULONG i = 0; i < NTDLL_INSPECTION_PROCESS_SLOTS; i++) {
        NTDLL_PROCESS_INSPECTION *slot = &ntdll_process_inspections[i];
        if (slot->process_id == process_id) {
            slot->process_id = 0;
            break;
        }
    }
    ntdll_inspection_unlock();
}

static BOOL ntdll_is_current_process_handle(HANDLE process)
{
    ULONG_PTR value = (ULONG_PTR)process;
    return value == (ULONG_PTR)NT_CURRENT_PROCESS || value == UINT32_MAX;
}

NTSTATUS NTAPI NtQueryInformationProcess(HANDLE process,
                                          PROCESSINFOCLASS info_class,
                                          PVOID info, ULONG info_len,
                                          ULONG *return_len)
{
    if (!info) return STATUS_INVALID_PARAMETER;

    if (ntdll_is_current_process_handle(process))
        process = NT_CURRENT_PROCESS;

    ULONG_PTR args[5] = {
        (ULONG_PTR)process, (ULONG_PTR)info_class, (ULONG_PTR)info,
        (ULONG_PTR)info_len, (ULONG_PTR)return_len
    };
    NTSTATUS status = sys_NtQueryInformationProcess(args);
    if (NT_SUCCESS(status) && info_class == ProcessBasicInformation) {
        PROCESS_BASIC_INFORMATION *basic = (PROCESS_BASIC_INFORMATION *)info;
        PPEB native_peb = basic->PebBaseAddress;
        NTDLL_INSPECTION_PEB64 *inspection =
            ntdll_prepare_inspection_peb((ULONG)basic->UniqueProcessId,
                                         native_peb);
        if (!inspection) return STATUS_INSUFFICIENT_RESOURCES;
        basic->PebBaseAddress = (PPEB)inspection;
    }
    return status;
}

NTSTATUS NTAPI NtWow64QueryInformationProcess64(HANDLE process,
                                                 PROCESSINFOCLASS info_class,
                                                 PVOID info, ULONG info_len,
                                                 ULONG *return_len)
{
    if (info_class != ProcessBasicInformation)
        return STATUS_INVALID_INFO_CLASS;
    if (return_len) *return_len = sizeof(PROCESS_BASIC_INFORMATION_WOW64);
    if (!info || info_len < sizeof(PROCESS_BASIC_INFORMATION_WOW64))
        return STATUS_INFO_LENGTH_MISMATCH;

    if (ntdll_is_current_process_handle(process))
        process = NT_CURRENT_PROCESS;

    PROCESS_BASIC_INFORMATION native_basic;
    ULONG native_len = 0;
    ULONG_PTR args[5] = {
        (ULONG_PTR)process, (ULONG_PTR)info_class,
        (ULONG_PTR)&native_basic, sizeof(native_basic),
        (ULONG_PTR)&native_len
    };
    NTSTATUS status = sys_NtQueryInformationProcess(args);
    if (!NT_SUCCESS(status)) return status;

    PROCESS_BASIC_INFORMATION_WOW64 *wow =
        (PROCESS_BASIC_INFORMATION_WOW64 *)info;
    RtlZeroMemory(wow, sizeof(*wow));
    PPEB native_peb = native_basic.PebBaseAddress;
    NTDLL_INSPECTION_PEB64 *inspection =
        ntdll_prepare_inspection_peb(
            (ULONG)native_basic.UniqueProcessId, native_peb);
    if (!inspection) return STATUS_INSUFFICIENT_RESOURCES;
    wow->PebBaseAddress = (ULONGLONG)(ULONG_PTR)inspection;
    wow->UniqueProcessId[0] = native_basic.UniqueProcessId;
    return STATUS_SUCCESS;
}

static NTSTATUS ntdll_read_process_memory(HANDLE process, ULONGLONG address,
                                           PVOID buffer, ULONGLONG size,
                                           ULONGLONG *bytes_read)
{
    extern BOOL nt_process_id(HANDLE handle, DWORD *process_id);
    extern BOOL win32_process_cr3(DWORD process_id, uint64_t *out_cr3);
    extern int paging_copy_between_cr3(uint64_t, uint64_t, uint64_t,
                                        uint64_t, uint64_t, uint64_t *);

    if (bytes_read) *bytes_read = 0;
    uint64_t source_cr3 = 0;
    if (!ntdll_is_current_process_handle(process)) {
        DWORD process_id = 0;
        if (!nt_process_id(process, &process_id) ||
            !win32_process_cr3(process_id, &source_cr3))
            return STATUS_INVALID_HANDLE;
    } else {
        __asm__ volatile ("mov %%cr3, %0" : "=r"(source_cr3));
    }
    if (size == 0) return STATUS_SUCCESS;
    if (!address || !buffer || address + size < address)
        return STATUS_ACCESS_VIOLATION;

    ULONGLONG buffer_address = (ULONGLONG)(ULONG_PTR)buffer;
    if (buffer_address + size < buffer_address)
        return STATUS_ACCESS_VIOLATION;

    uint64_t destination_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(destination_cr3));
    uint64_t copied = 0;
    int result = paging_copy_between_cr3(
        destination_cr3, buffer_address, source_cr3, address, size, &copied);
    if (bytes_read) *bytes_read = copied;
    if (result == 0) return STATUS_SUCCESS;
    return copied ? STATUS_PARTIAL_COPY : STATUS_ACCESS_VIOLATION;
}

static NTSTATUS ntdll_write_process_memory(HANDLE process, ULONGLONG address,
                                            PCVOID buffer, ULONGLONG size,
                                            ULONGLONG *bytes_written)
{
    extern BOOL nt_process_id(HANDLE handle, DWORD *process_id);
    extern BOOL win32_process_cr3(DWORD process_id, uint64_t *out_cr3);
    extern int paging_copy_between_cr3(uint64_t, uint64_t, uint64_t,
                                        uint64_t, uint64_t, uint64_t *);

    if (bytes_written) *bytes_written = 0;
    uint64_t destination_cr3 = 0;
    if (!ntdll_is_current_process_handle(process)) {
        DWORD process_id = 0;
        if (!nt_process_id(process, &process_id) ||
            !win32_process_cr3(process_id, &destination_cr3))
            return STATUS_INVALID_HANDLE;
    } else {
        __asm__ volatile ("mov %%cr3, %0" : "=r"(destination_cr3));
    }
    if (size == 0) return STATUS_SUCCESS;
    if (!address || !buffer || address + size < address)
        return STATUS_ACCESS_VIOLATION;

    ULONGLONG source = (ULONGLONG)(ULONG_PTR)buffer;
    if (source + size < source)
        return STATUS_ACCESS_VIOLATION;

    uint64_t source_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(source_cr3));
    uint64_t copied = 0;
    int result = paging_copy_between_cr3(
        destination_cr3, address, source_cr3, source, size, &copied);
    if (bytes_written) *bytes_written = copied;
    if (result == 0) return STATUS_SUCCESS;
    return copied ? STATUS_PARTIAL_COPY : STATUS_ACCESS_VIOLATION;
}

static void ntdll_trace_read_memory(const char *api, HANDLE process,
                                    ULONGLONG address, PVOID buffer,
                                    ULONGLONG size, ULONGLONG bytes_read,
                                    NTSTATUS status, ULONG_PTR caller)
{
    static uint32_t trace_count;
    uint32_t trace = __atomic_fetch_add(&trace_count, 1, __ATOMIC_RELAXED);
    if (NT_SUCCESS(status) && trace >= 64) return;

    extern void serial_puts(const char *s);
    extern void serial_puthex(uint64_t val, int digits);
    serial_puts("[NTDLL-RVM] api=");
    serial_puts(api);
    serial_puts(" caller=0x");
    serial_puthex((uint64_t)caller, 16);
    serial_puts(" process=0x");
    serial_puthex((uint64_t)(ULONG_PTR)process, 16);
    serial_puts(" source=0x");
    serial_puthex(address, 16);
    serial_puts(" dest=0x");
    serial_puthex((uint64_t)(ULONG_PTR)buffer, 16);
    serial_puts(" size=0x");
    serial_puthex(size, 16);
    serial_puts(" read=0x");
    serial_puthex(bytes_read, 16);
    serial_puts(" status=0x");
    serial_puthex((uint32_t)status, 8);
    serial_puts("\n");
}

NTSTATUS NTAPI NtReadVirtualMemory(HANDLE process, PVOID address,
                                    PVOID buffer, SIZE_T size,
                                    SIZE_T *bytes_read)
{
    ULONGLONG read = 0;
    NTSTATUS status = ntdll_read_process_memory(
        process, (ULONGLONG)(ULONG_PTR)address, buffer, size,
        bytes_read ? &read : NULL);
    if (bytes_read) {
        if (g_compat32_mode)
            *(uint32_t *)(void *)bytes_read = (uint32_t)read;
        else
            *bytes_read = (SIZE_T)read;
    }
    ntdll_trace_read_memory("native", process,
                            (ULONGLONG)(ULONG_PTR)address, buffer, size, read,
                            status, (ULONG_PTR)__builtin_return_address(0));
    return status;
}

NTSTATUS NTAPI NtWriteVirtualMemory(HANDLE process, PVOID address,
                                     PCVOID buffer, SIZE_T size,
                                     SIZE_T *bytes_written)
{
    ULONGLONG written = 0;
    NTSTATUS status = ntdll_write_process_memory(
        process, (ULONGLONG)(ULONG_PTR)address, buffer, size,
        bytes_written ? &written : NULL);
    if (bytes_written) {
        if (g_compat32_mode)
            *(uint32_t *)(void *)bytes_written = (uint32_t)written;
        else
            *bytes_written = (SIZE_T)written;
    }
    return status;
}

NTSTATUS NTAPI NtWow64ReadVirtualMemory64(HANDLE process, ULONGLONG address,
                                           PVOID buffer, ULONGLONG size,
                                           ULONGLONG *bytes_read)
{
    NTSTATUS status = ntdll_read_process_memory(process, address, buffer, size,
                                                 bytes_read);
    ntdll_trace_read_memory("wow64", process, address, buffer, size,
                            bytes_read ? *bytes_read : 0, status,
                            (ULONG_PTR)__builtin_return_address(0));
    return status;
}

/* ── Synchronization wrappers (Phase 21) ────────────────────── */

extern NTSTATUS sys_NtCreateEvent(ULONG_PTR *args);
extern NTSTATUS sys_NtSetEvent(ULONG_PTR *args);
extern NTSTATUS sys_NtResetEvent(ULONG_PTR *args);
extern NTSTATUS sys_NtPulseEvent(ULONG_PTR *args);
extern NTSTATUS sys_NtWaitForMultipleObjects(ULONG_PTR *args);

NTSTATUS NTAPI NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                       POBJECT_ATTRIBUTES ObjectAttributes,
                       ULONG EventType, BOOL InitialState)
{
    ULONG_PTR args[5] = {
        (ULONG_PTR)EventHandle, (ULONG_PTR)DesiredAccess,
        (ULONG_PTR)ObjectAttributes, (ULONG_PTR)EventType,
        (ULONG_PTR)InitialState
    };
    return sys_NtCreateEvent(args);
}

NTSTATUS NTAPI NtOpenEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                           POBJECT_ATTRIBUTES ObjectAttributes)
{
    return kernel32_nt_open_named_event(ObjectAttributes, DesiredAccess,
                                        EventHandle);
}

NTSTATUS NTAPI NtSetEvent(HANDLE EventHandle, LONG *PreviousState)
{
    ULONG_PTR args[2] = {
        (ULONG_PTR)EventHandle, (ULONG_PTR)PreviousState
    };
    return sys_NtSetEvent(args);
}

NTSTATUS NTAPI NtResetEvent(HANDLE EventHandle, LONG *PreviousState)
{
    ULONG_PTR args[2] = {
        (ULONG_PTR)EventHandle, (ULONG_PTR)PreviousState
    };
    return sys_NtResetEvent(args);
}

NTSTATUS NTAPI NtPulseEvent(HANDLE EventHandle, LONG *PreviousState)
{
    ULONG_PTR args[2] = {
        (ULONG_PTR)EventHandle, (ULONG_PTR)PreviousState
    };
    return sys_NtPulseEvent(args);
}

NTSTATUS NTAPI NtWaitForSingleObject(HANDLE Handle, BOOL Alertable,
                                PLARGE_INTEGER Timeout)
{
    /* Wrap as single-object wait via NtWaitForMultipleObjects */
    ULONG_PTR args[5] = {
        (ULONG_PTR)1,              /* Count = 1 */
        (ULONG_PTR)&Handle,        /* array of 1 handle */
        (ULONG_PTR)1,              /* WaitAny */
        (ULONG_PTR)Alertable,
        (ULONG_PTR)Timeout
    };
    return sys_NtWaitForMultipleObjects(args);
}

/* ── Section wrappers (memory-mapped files) ────────────────── */

extern NTSTATUS sys_NtCreateSection(ULONG_PTR *args);
extern NTSTATUS sys_NtQuerySection(ULONG_PTR *args);
extern NTSTATUS sys_NtMapViewOfSection(ULONG_PTR *args);
extern NTSTATUS sys_NtUnmapViewOfSection(ULONG_PTR *args);

NTSTATUS NTAPI NtCreateSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                         POBJECT_ATTRIBUTES ObjectAttributes,
                         PLARGE_INTEGER MaximumSize,
                         ULONG SectionPageProtection,
                         ULONG AllocationAttributes,
                         HANDLE FileHandle)
{
    ULONG_PTR args[7] = {
        (ULONG_PTR)SectionHandle, (ULONG_PTR)DesiredAccess,
        (ULONG_PTR)ObjectAttributes, (ULONG_PTR)MaximumSize,
        (ULONG_PTR)SectionPageProtection, (ULONG_PTR)AllocationAttributes,
        (ULONG_PTR)FileHandle
    };
    return sys_NtCreateSection(args);
}

NTSTATUS NTAPI NtQuerySection(HANDLE SectionHandle,
                              ULONG SectionInformationClass,
                              PVOID SectionInformation,
                              SIZE_T SectionInformationLength,
                              SIZE_T *ReturnLength)
{
    ULONG_PTR args[5] = {
        (ULONG_PTR)SectionHandle, (ULONG_PTR)SectionInformationClass,
        (ULONG_PTR)SectionInformation, (ULONG_PTR)SectionInformationLength,
        (ULONG_PTR)ReturnLength
    };
    return sys_NtQuerySection(args);
}

NTSTATUS NTAPI NtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle,
                            PVOID *BaseAddress, ULONG_PTR ZeroBits,
                            SIZE_T CommitSize, PLARGE_INTEGER SectionOffset,
                            SIZE_T *ViewSize, ULONG InheritDisposition,
                            ULONG AllocationType, ULONG Win32Protect)
{
    ULONG_PTR args[10] = {
        (ULONG_PTR)SectionHandle, (ULONG_PTR)ProcessHandle,
        (ULONG_PTR)BaseAddress, (ULONG_PTR)ZeroBits,
        (ULONG_PTR)CommitSize, (ULONG_PTR)SectionOffset,
        (ULONG_PTR)ViewSize, (ULONG_PTR)InheritDisposition,
        (ULONG_PTR)AllocationType, (ULONG_PTR)Win32Protect
    };
    return sys_NtMapViewOfSection(args);
}

NTSTATUS NTAPI NtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress)
{
    ULONG_PTR args[2] = {
        (ULONG_PTR)ProcessHandle, (ULONG_PTR)BaseAddress
    };
    return sys_NtUnmapViewOfSection(args);
}

/* ── SEH Support (Phase 17) ─────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern TEB g_teb;
extern TEB *win64_current_teb(void);
extern void NTAPI win32_restore_context_asm(PCONTEXT context);
typedef PVOID (NTAPI *RTL_UNWIND_CONSOLIDATE_CALLBACK)(
    PEXCEPTION_RECORD record);
extern PVOID NTAPI win32_unwind64_call_consolidate(
    PCONTEXT context, RTL_UNWIND_CONSOLIDATE_CALLBACK callback,
    PEXCEPTION_RECORD record);

typedef struct __attribute__((aligned(16))) _WIN32_JUMP_BUFFER64 {
    ULONGLONG Frame;
    ULONGLONG Rbx;
    ULONGLONG Rsp;
    ULONGLONG Rbp;
    ULONGLONG Rsi;
    ULONGLONG Rdi;
    ULONGLONG R12;
    ULONGLONG R13;
    ULONGLONG R14;
    ULONGLONG R15;
    ULONGLONG Rip;
    DWORD MxCsr;
    WORD FpCsr;
    WORD Spare;
    M128A Xmm6[10];
} WIN32_JUMP_BUFFER64;

_Static_assert(sizeof(WIN32_JUMP_BUFFER64) == 0x100,
               "AMD64 jump buffer size changed");
_Static_assert(__builtin_offsetof(WIN32_JUMP_BUFFER64, Xmm6) == 0x60,
               "AMD64 jump buffer XMM offset changed");

static BOOL rtl_restore_target_valid(PCONTEXT context)
{
    TEB *teb = win64_current_teb();
    ULONG_PTR stack_limit = teb ? (ULONG_PTR)teb->StackLimit : 0;
    ULONG_PTR stack_base = teb ? (ULONG_PTR)teb->StackBase : 0;
    return context && stack_limit && stack_limit < stack_base &&
           !(context->Rsp & (sizeof(ULONG_PTR) - 1U)) &&
           context->Rsp >= stack_limit && context->Rsp <= stack_base &&
           context->Rip && win32_user_range_executable(
               (const void *)(ULONG_PTR)context->Rip, 1, FALSE);
}

/* RtlCaptureContext itself is the assembly entry in context64.S. */
void NTAPI RtlRestoreContext(PCONTEXT context,
                             PEXCEPTION_RECORD exception_record)
{
    if (g_compat32_mode) {
        NTSTATUS status = compat32_rtl_restore_context(
            (uint32_t)(ULONG_PTR)context,
            (uint32_t)(ULONG_PTR)exception_record);
        if (!NT_SUCCESS(status)) {
            serial_puts("[SEH32] RtlRestoreContext rejected context\n");
            RtlRaiseStatus(status);
        }
        return;
    }

    if (!context || !win32_user_range_writable(
            context, sizeof(*context), FALSE) ||
        (exception_record && !win32_user_range_readable(
            exception_record, sizeof(*exception_record), FALSE))) {
        serial_puts("[SEH] RtlRestoreContext rejected invalid context\n");
        RtlRaiseStatus(STATUS_INVALID_PARAMETER);
        return;
    }

    if (exception_record &&
        exception_record->ExceptionCode == (DWORD)STATUS_LONGJUMP &&
        exception_record->NumberParameters >= 1) {
        const WIN32_JUMP_BUFFER64 *source =
            (const WIN32_JUMP_BUFFER64 *)(ULONG_PTR)
                exception_record->ExceptionInformation[0];
        WIN32_JUMP_BUFFER64 jump;
        if (!win32_user_range_readable(source, sizeof(*source), FALSE)) {
            serial_puts("[SEH] RtlRestoreContext rejected jump buffer\n");
            RtlRaiseStatus(STATUS_INVALID_PARAMETER);
            return;
        }
        RtlCopyMemory(&jump, source, sizeof(jump));
        context->Rbx = jump.Rbx;
        context->Rsp = jump.Rsp;
        context->Rbp = jump.Rbp;
        context->Rsi = jump.Rsi;
        context->Rdi = jump.Rdi;
        context->R12 = jump.R12;
        context->R13 = jump.R13;
        context->R14 = jump.R14;
        context->R15 = jump.R15;
        context->Rip = jump.Rip;
        context->MxCsr = jump.MxCsr;
        context->FltSave.MxCsr = jump.MxCsr;
        context->FltSave.ControlWord = jump.FpCsr;
        RtlCopyMemory(&context->FltSave.XmmRegisters[6], jump.Xmm6,
                      sizeof(jump.Xmm6));
    } else if (exception_record &&
               exception_record->ExceptionCode ==
                   (DWORD)STATUS_UNWIND_CONSOLIDATE &&
               exception_record->NumberParameters >= 1) {
        RTL_UNWIND_CONSOLIDATE_CALLBACK callback =
            (RTL_UNWIND_CONSOLIDATE_CALLBACK)(ULONG_PTR)
                exception_record->ExceptionInformation[0];
        if (!rtl_restore_target_valid(context) ||
            !win32_user_range_executable((const void *)callback, 1,
                                         FALSE)) {
            serial_puts("[SEH] RtlRestoreContext rejected consolidate callback\n");
            RtlRaiseStatus(STATUS_INVALID_PARAMETER);
            return;
        }
        PVOID continuation = win32_unwind64_call_consolidate(
            context, callback, exception_record);
        if (!continuation || !win32_user_range_executable(
                continuation, 1, FALSE)) {
            serial_puts("[SEH] RtlRestoreContext rejected consolidate target\n");
            RtlRaiseStatus(STATUS_INVALID_PARAMETER);
            return;
        }
        context->Rip = (ULONGLONG)(ULONG_PTR)continuation;
    }

    if (!rtl_restore_target_valid(context)) {
        serial_puts("[SEH] RtlRestoreContext rejected invalid target\n");
        RtlRaiseStatus(STATUS_INVALID_PARAMETER);
        return;
    }

    ULONG_PTR live_flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(live_flags));
    const DWORD mutable_flags = 0x00250DD5U;
    context->EFlags = ((DWORD)live_flags & ~mutable_flags) |
                      (context->EFlags & mutable_flags);
    context->MxCsr &= 0x0000FFBFU;
    context->FltSave.MxCsr = context->MxCsr;
    win32_restore_context_asm(context);
}

static PVOID NTAPI RtlLookupFunctionEntry_stub(ULONG_PTR control_pc,
                                                ULONG_PTR *image_base,
                                                PVOID history_table)
{
    return win32_unwind64_lookup_function_entry(
        control_pc, image_base, (PUNWIND_HISTORY_TABLE)history_table);
}

static BOOL NTAPI RtlAddFunctionTable_ntdll(PVOID function_table,
                                             DWORD entry_count,
                                             ULONGLONG base_address)
{
    return win32_unwind64_add_function_table(
        (PRUNTIME_FUNCTION)function_table, entry_count, base_address);
}

static BOOL NTAPI RtlDeleteFunctionTable_ntdll(PVOID function_table)
{
    return win32_unwind64_delete_function_table(
        (PRUNTIME_FUNCTION)function_table);
}

/*
 * RtlRaiseException — dispatch an exception through the SEH chain.
 *
 * Walks TEB.ExceptionList, calling each handler. If a handler returns
 * ExceptionContinueExecution, we return (caller continues). If all
 * handlers return ExceptionContinueSearch, calls the unhandled exception
 * filter if set, then terminates.
 */

void NTAPI win32_rtl_raise_exception_impl(
    PEXCEPTION_RECORD ExceptionRecord, PCONTEXT context)
{
    if (!ExceptionRecord)
        return;

    NTSTATUS terminal_status = ExceptionRecord->ExceptionCode;

    serial_puts("[SEH] RtlRaiseException: code=0x");
    serial_puthex(ExceptionRecord->ExceptionCode, 8);
    serial_puts("\n");

    /* Delegate to compat32 SEH dispatch which correctly reads 32-bit
     * SEH frames (4-byte Next + 4-byte Handler). The previous code
     * used 64-bit EXCEPTION_REGISTRATION_RECORD (8+8 bytes) which
     * misread the 32-bit frame, concatenating Handler with stack garbage
     * → non-canonical RIP → #GP. */
    if (g_compat32_mode) {
        extern int compat32_seh_dispatch(PEXCEPTION_RECORD ExceptionRecord);
        int dispatch_rc = compat32_seh_dispatch(ExceptionRecord);
        if (dispatch_rc > 0) {
            serial_puts("[SEH] exception handled by compat32\n");
            return;
        }
    }

    if (!g_compat32_mode) {
        if (!context) {
            terminal_status = STATUS_INVALID_PARAMETER;
            goto terminate;
        }
        if (!ExceptionRecord->ExceptionAddress)
            ExceptionRecord->ExceptionAddress =
                (PVOID)(ULONG_PTR)context->Rip;

        LONG disposition = kernel32_dispatch_vectored_exception(
            ExceptionRecord, context);
        if (disposition == EXCEPTION_CONTINUE_EXECUTION) {
            if (ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) {
                terminal_status = STATUS_NONCONTINUABLE_EXCEPTION;
                goto terminate;
            }
            RtlRestoreContext(context, ExceptionRecord);
            terminal_status = STATUS_INVALID_DISPOSITION;
            goto terminate;
        }

        NTSTATUS frame_status = win32_unwind64_dispatch_exception(
            ExceptionRecord, context);
        if (frame_status == STATUS_SUCCESS) {
            RtlRestoreContext(context, ExceptionRecord);
            terminal_status = STATUS_INVALID_DISPOSITION;
            goto terminate;
        }
        if (frame_status != STATUS_UNHANDLED_EXCEPTION) {
            terminal_status = frame_status;
            goto terminate;
        }
    }

    /* compat32_seh_dispatch already walked the 32-bit SEH chain with
     * the correct 32-bit semantics (4-byte Next + 4-byte Handler). If
     * it returned 0, the chain was traversed and no handler caught the
     * exception. Don't run a second buggy 64-bit walker that:
     *   (a) re-reads the same chain with wrong 8+8 byte layout,
     *   (b) casts a 32-bit handler address as a 64-bit function pointer
     *       and calls it from kernel-mode 64-bit context (→ crash).
     *
     * Skip directly to the UnhandledExceptionFilter / terminate path. */

    /* No handler caught the exception — try unhandled filter */
    PVOID unhandled_filter = kernel32_get_unhandled_exception_filter();
    if (!g_compat32_mode && unhandled_filter &&
        win32_user_range_executable(unhandled_filter, 1, FALSE)) {
        serial_puts("[SEH] calling UnhandledExceptionFilter\n");
        EXCEPTION_POINTERS ep;
        ep.ExceptionRecord = ExceptionRecord;
        ep.ContextRecord   = context;

        typedef LONG (WINAPI *uef_fn)(PEXCEPTION_POINTERS);
        uef_fn filter = (uef_fn)unhandled_filter;
        LONG result = filter(&ep);

        if (result == EXCEPTION_CONTINUE_EXECUTION &&
            !(ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) {
            RtlRestoreContext(context, ExceptionRecord);
            terminal_status = STATUS_INVALID_DISPOSITION;
        } else if (result == EXCEPTION_CONTINUE_EXECUTION) {
            terminal_status = STATUS_NONCONTINUABLE_EXCEPTION;
        }
    }

terminate:
    serial_puts("[SEH] UNHANDLED EXCEPTION 0x");
    serial_puthex((DWORD)terminal_status, 8);
    serial_puts(" — terminating\n");

    extern BOOL win32_terminate_current_child(NTSTATUS status);
    extern BOOL win32_terminate_current_main(NTSTATUS status);
    if (win32_terminate_current_child(terminal_status) ||
        win32_terminate_current_main(terminal_status))
        return;

    extern void proc_exit(int32_t code);
    proc_exit((int32_t)terminal_status);
}

void NTAPI win32_rtl_raise_status_impl(NTSTATUS status, PCONTEXT context)
{
    EXCEPTION_RECORD record;
    BYTE *bytes = (BYTE *)&record;

    for (SIZE_T i = 0; i < sizeof(record); i++)
        bytes[i] = 0;

    record.ExceptionCode = status;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    if (g_compat32_mode) {
        extern uint32_t compat32_get_last_caller_eip(void);
        record.ExceptionAddress =
            (PVOID)(ULONG_PTR)compat32_get_last_caller_eip();
    } else if (context) {
        record.ExceptionAddress = (PVOID)(ULONG_PTR)context->Rip;
    }

    /* The PE32 thunk commits a selected SEH transfer after this returns. */
    win32_rtl_raise_exception_impl(&record, context);
}

typedef struct {
    CONTEXT Context;
    ULONGLONG Argument5;
    ULONGLONG Argument6;
} WIN32_UNWIND_ENTRY;

_Static_assert(__builtin_offsetof(WIN32_UNWIND_ENTRY, Argument5) ==
                   sizeof(CONTEXT),
               "unwind entry argument layout changed");

/* context64.S records the import caller before entering compiler-generated
 * code. The first four API arguments remain in the captured register slots;
 * the two stack arguments follow the CONTEXT in this private entry record. */
ULONG_PTR NTAPI win32_rtl_unwind_entry_impl(WIN32_UNWIND_ENTRY *entry,
                                            BOOL extended)
{
    if (!entry) return 0;

    PVOID target_frame = (PVOID)(ULONG_PTR)entry->Context.Rcx;
    PVOID target_ip = (PVOID)(ULONG_PTR)entry->Context.Rdx;
    PEXCEPTION_RECORD exception_record =
        (PEXCEPTION_RECORD)(ULONG_PTR)entry->Context.R8;
    PVOID return_value = (PVOID)(ULONG_PTR)entry->Context.R9;

    if (g_compat32_mode) {
        NTSTATUS exit_status = STATUS_SUCCESS;
        NTSTATUS status = compat32_rtl_unwind(
            (uint32_t)(ULONG_PTR)target_frame,
            (uint32_t)(ULONG_PTR)target_ip,
            (uint32_t)(ULONG_PTR)exception_record,
            (uint32_t)(ULONG_PTR)return_value,
            &exit_status);
        if (!NT_SUCCESS(status)) {
            win32_rtl_raise_status_impl(status, &entry->Context);
            return 0;
        }
        if (!target_frame) {
            extern BOOL win32_terminate_current_child(NTSTATUS status);
            extern BOOL win32_terminate_current_main(NTSTATUS status);
            if (win32_terminate_current_child(exit_status) ||
                win32_terminate_current_main(exit_status))
                return 0;
            extern void proc_exit(int32_t code);
            proc_exit((int32_t)exit_status);
            return 0;
        }
        return (ULONG_PTR)return_value;
    }

    PCONTEXT context = &entry->Context;
    PUNWIND_HISTORY_TABLE history = NULL;
    if (extended) {
        PCONTEXT caller_context =
            (PCONTEXT)(ULONG_PTR)entry->Argument5;
        history = (PUNWIND_HISTORY_TABLE)(ULONG_PTR)entry->Argument6;
        if (!caller_context ||
            !win32_user_range_writable(caller_context, sizeof(*caller_context),
                                       FALSE) ||
            (history && !win32_user_range_writable(
                            history, sizeof(*history), FALSE))) {
            win32_rtl_raise_status_impl(STATUS_INVALID_PARAMETER, context);
            return 0;
        }
        *caller_context = *context;
        context = caller_context;
    }

    EXCEPTION_RECORD local_record;
    if (!exception_record) {
        BYTE *bytes = (BYTE *)&local_record;
        for (SIZE_T i = 0; i < sizeof(local_record); i++) bytes[i] = 0;
        local_record.ExceptionCode = STATUS_UNWIND;
        local_record.ExceptionAddress = (PVOID)(ULONG_PTR)context->Rip;
        exception_record = &local_record;
    } else if (!win32_user_range_writable(
                   exception_record, sizeof(*exception_record), FALSE)) {
        win32_rtl_raise_status_impl(STATUS_INVALID_PARAMETER, context);
        return 0;
    }

    NTSTATUS status = win32_unwind64_unwind_ex(
        exception_record, context, target_frame, target_ip, return_value,
        history);
    if (status != STATUS_SUCCESS) {
        win32_rtl_raise_status_impl(status, context);
        return 0;
    }

    if (!target_frame) {
        NTSTATUS exit_status = exception_record->ExceptionCode;
        if (exit_status == STATUS_UNWIND) exit_status = STATUS_SUCCESS;
        extern BOOL win32_terminate_current_child(NTSTATUS status);
        extern BOOL win32_terminate_current_main(NTSTATUS status);
        if (win32_terminate_current_child(exit_status) ||
            win32_terminate_current_main(exit_status))
            return 0;
        extern void proc_exit(int32_t code);
        proc_exit((int32_t)exit_status);
        return 0;
    }

    RtlRestoreContext(context, exception_record);
    win32_rtl_raise_status_impl(STATUS_INVALID_DISPOSITION, context);
    return 0;
}

static PVOID NTAPI RtlVirtualUnwind_stub(
    DWORD handler_type, ULONG_PTR image_base, ULONG_PTR control_pc,
    PVOID function_entry, PCONTEXT context, PVOID *handler_data,
    ULONG_PTR *establisher_frame, PVOID context_pointers)
{
    return win32_unwind64_virtual_unwind(
        handler_type, image_base, control_pc,
        (PRUNTIME_FUNCTION)function_entry, context, handler_data,
        establisher_frame,
        (PKNONVOLATILE_CONTEXT_POINTERS)context_pointers);
}

/*
 * NtRaiseException — NT syscall wrapper for exception dispatch.
 * Delegates to RtlRaiseException.
 */
NTSTATUS NTAPI NtRaiseException(PEXCEPTION_RECORD ExceptionRecord,
                           PCONTEXT ContextRecord, BOOL FirstChance)
{
    (void)FirstChance;

    if (!ExceptionRecord || (!g_compat32_mode && !ContextRecord))
        return STATUS_INVALID_PARAMETER;

    win32_rtl_raise_exception_impl(ExceptionRecord, ContextRecord);
    return STATUS_SUCCESS;
}

/* ── LdrLoadDll — NT loader API ─────────────────────────────── */

/*
 * LdrLoadDll — loads a DLL by name. Used internally by ntdll.
 * We delegate to kernel32 LoadLibraryA.
 */
extern HANDLE WINAPI LoadLibraryA(PCSTR lpLibFileName);
extern PVOID WINAPI GetProcAddress(HANDLE hModule, PCSTR lpProcName);

static NTSTATUS NTAPI LdrLoadDll(PVOID SearchPath, ULONG *DllCharacteristics,
                                  PUNICODE_STRING DllName, PVOID *BaseAddress)
{
    (void)SearchPath;
    (void)DllCharacteristics;

    if (!DllName || !BaseAddress) return STATUS_INVALID_PARAMETER;

    /* Convert UNICODE_STRING to ANSI */
    char name[256];
    ULONG chars = DllName->Length / sizeof(WCHAR);
    if (chars >= sizeof(name)) chars = sizeof(name) - 1;
    for (ULONG i = 0; i < chars; i++)
        name[i] = (char)(DllName->Buffer[i] & 0xFF);
    name[chars] = '\0';

    serial_puts("[NTDLL] LdrLoadDll: ");
    serial_puts(name);
    serial_puts("\n");

    HANDLE h = LoadLibraryA(name);
    if (h) {
        *BaseAddress = h;
        return STATUS_SUCCESS;
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static NTSTATUS NTAPI LdrGetProcedureAddress(PVOID BaseAddress,
                                               PCANSI_STRING FunctionName,
                                               ULONG Ordinal,
                                               PVOID *FunctionAddress)
{
    if (!FunctionAddress) return STATUS_INVALID_PARAMETER;
    *FunctionAddress = NULL;
    if (!BaseAddress) return STATUS_DLL_NOT_FOUND;

    PCSTR lookup;
    char *name = NULL;
    if (FunctionName) {
        if ((!FunctionName->Buffer && FunctionName->Length) ||
            FunctionName->MaximumLength < FunctionName->Length)
            return STATUS_INVALID_PARAMETER;

        name = (char *)kmalloc((SIZE_T)FunctionName->Length + 1);
        if (!name) return STATUS_NO_MEMORY;
        for (USHORT i = 0; i < FunctionName->Length; i++)
            name[i] = FunctionName->Buffer[i];
        name[FunctionName->Length] = 0;
        lookup = name;
    } else {
        if (!Ordinal || Ordinal > 0xFFFF) return STATUS_INVALID_PARAMETER;
        lookup = (PCSTR)(ULONG_PTR)Ordinal;
    }

    PVOID address = GetProcAddress((HANDLE)BaseAddress, lookup);
    if (name) kfree(name);
    if (!address) return STATUS_PROCEDURE_NOT_FOUND;

    *FunctionAddress = address;
    return STATUS_SUCCESS;
}

#define LDR_LOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS 0x00000001U
#define LDR_LOCK_LOADER_LOCK_FLAG_TRY_ONLY        0x00000002U
#define LDR_UNLOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS 0x00000001U
#define LDR_LOCK_LOADER_LOCK_DISPOSITION_INVALID           0U
#define LDR_LOCK_LOADER_LOCK_DISPOSITION_LOCK_ACQUIRED     1U
#define LDR_LOCK_LOADER_LOCK_DISPOSITION_LOCK_NOT_ACQUIRED 2U

static volatile ULONG ldr_lock_cookie_sequence;

static void ldr_store_lock_cookie(ULONG_PTR *cookie, ULONG_PTR value)
{
    if (g_compat32_mode)
        *(uint32_t *)(void *)cookie = (uint32_t)value;
    else
        *cookie = value;
}

static ULONG_PTR ldr_make_lock_cookie(void)
{
    ULONG sequence = __atomic_add_fetch(&ldr_lock_cookie_sequence, 1,
                                         __ATOMIC_RELAXED) & 0xFFFFU;
    if (!sequence)
        sequence = __atomic_add_fetch(&ldr_lock_cookie_sequence, 1,
                                      __ATOMIC_RELAXED) & 0xFFFFU;
    return ((ULONG_PTR)(GetCurrentThreadId() & 0x0FFFU) << 16) | sequence;
}

static NTSTATUS ldr_status_or_raise(ULONG flags, NTSTATUS status)
{
    if (flags & LDR_LOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS)
        RtlRaiseStatus(status);
    return status;
}

static NTSTATUS NTAPI LdrLockLoaderLock(ULONG flags, ULONG *disposition,
                                        ULONG_PTR *cookie)
{
    if (disposition)
        *disposition = LDR_LOCK_LOADER_LOCK_DISPOSITION_INVALID;
    if (cookie) ldr_store_lock_cookie(cookie, 0);

    if (flags & ~(LDR_LOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS |
                  LDR_LOCK_LOADER_LOCK_FLAG_TRY_ONLY))
        return ldr_status_or_raise(flags, STATUS_INVALID_PARAMETER_1);
    if (!cookie)
        return ldr_status_or_raise(flags, STATUS_INVALID_PARAMETER_3);

    BOOL acquired;
    if (flags & LDR_LOCK_LOADER_LOCK_FLAG_TRY_ONLY)
        acquired = dll_loader_lock_try_enter();
    else {
        dll_loader_lock_enter();
        acquired = TRUE;
    }

    if (!acquired) {
        if (disposition)
            *disposition =
                LDR_LOCK_LOADER_LOCK_DISPOSITION_LOCK_NOT_ACQUIRED;
        return STATUS_SUCCESS;
    }

    if (disposition)
        *disposition = LDR_LOCK_LOADER_LOCK_DISPOSITION_LOCK_ACQUIRED;
    ldr_store_lock_cookie(cookie, ldr_make_lock_cookie());
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI LdrUnlockLoaderLock(ULONG flags, ULONG_PTR cookie)
{
    if (flags & ~LDR_UNLOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS)
        return ldr_status_or_raise(flags, STATUS_INVALID_PARAMETER_1);
    if (!cookie) return STATUS_SUCCESS;

    ULONG_PTR expected_thread =
        (ULONG_PTR)(GetCurrentThreadId() & 0x0FFFU) << 16;
    if ((cookie & 0xF0000000U) ||
        (cookie & 0x0FFF0000U) != expected_thread ||
        !dll_loader_lock_owned_by_current_thread())
        return ldr_status_or_raise(flags, STATUS_INVALID_PARAMETER_2);

    if (!dll_loader_lock_leave())
        return ldr_status_or_raise(flags, STATUS_INVALID_PARAMETER_2);
    return STATUS_SUCCESS;
}

/* ── Export resolution table ────────────────────────────────── */

static const SHIM_EXPORT ntdll_exports[] = {
    /* NT API */
    { "NtCreateFile",              (PVOID)NtCreateFile,              11, CC_STDCALL },
    { "NtReadFile",                (PVOID)NtReadFile,                 9, CC_STDCALL },
    { "NtWriteFile",               (PVOID)NtWriteFile,                9, CC_STDCALL },
    { "NtLockFile",                (PVOID)NtLockFile,                 10, CC_STDCALL },
    { "NtUnlockFile",              (PVOID)NtUnlockFile,                5, CC_STDCALL },
    { "NtClose",                   (PVOID)NtClose,                    1, CC_STDCALL },
    { "NtQueryObject",             (PVOID)NtQueryObject,              5, CC_STDCALL },
    { "NtAllocateVirtualMemory",   (PVOID)NtAllocateVirtualMemory,    6, CC_STDCALL },
    { "NtFreeVirtualMemory",       (PVOID)NtFreeVirtualMemory,        4, CC_STDCALL },
    { "NtTerminateProcess",        (PVOID)NtTerminateProcess,         2, CC_STDCALL },
    { "NtDelayExecution",          (PVOID)NtDelayExecution,           2, CC_STDCALL },
    { "NtQueryPerformanceCounter", (PVOID)NtQueryPerformanceCounter,  2, CC_STDCALL },
    { "NtQueryInformationFile",    (PVOID)NtQueryInformationFile,     5, CC_STDCALL },
    { "NtSetInformationFile",      (PVOID)NtSetInformationFile,       5, CC_STDCALL },
    { "NtDuplicateObject",         (PVOID)NtDuplicateObject,          7, CC_STDCALL },
    { "NtProtectVirtualMemory",    (PVOID)NtProtectVirtualMemory,     5, CC_STDCALL },
    { "NtQueryVirtualMemory",      (PVOID)NtQueryVirtualMemory,       6, CC_STDCALL },
    { "NtQueryInformationProcess", (PVOID)NtQueryInformationProcess,  5, CC_STDCALL },
    { "NtReadVirtualMemory",       (PVOID)NtReadVirtualMemory,        5, CC_STDCALL },
    { "NtWriteVirtualMemory",      (PVOID)NtWriteVirtualMemory,       5, CC_STDCALL },
    { "NtWow64QueryInformationProcess64",
      (PVOID)NtWow64QueryInformationProcess64,                       5, CC_STDCALL },
    { "NtWow64ReadVirtualMemory64",
      (PVOID)NtWow64ReadVirtualMemory64,                              5, CC_STDCALL },
    { "NtOpenKeyEx",              (PVOID)NtOpenKeyEx,               4, CC_STDCALL },
    { "NtCreateKey",              (PVOID)NtCreateKey,               7, CC_STDCALL },
    { "NtQueryValueKey",          (PVOID)NtQueryValueKey,           6, CC_STDCALL },
    { "NtSetValueKey",            (PVOID)NtSetValueKey,             6, CC_STDCALL },
    { "NtDeleteKey",              (PVOID)NtDeleteKey_stub,          1, CC_STDCALL },
    /* Section (memory-mapped files) */
    { "NtCreateSection",           (PVOID)NtCreateSection,            7, CC_STDCALL },
    { "NtQuerySection",            (PVOID)NtQuerySection,             5, CC_STDCALL },
    { "NtMapViewOfSection",        (PVOID)NtMapViewOfSection,        10, CC_STDCALL },
    { "NtUnmapViewOfSection",      (PVOID)NtUnmapViewOfSection,       2, CC_STDCALL },
    { "ZwCreateSection",           (PVOID)NtCreateSection,            7, CC_STDCALL },
    { "ZwQuerySection",            (PVOID)NtQuerySection,             5, CC_STDCALL },
    { "ZwMapViewOfSection",        (PVOID)NtMapViewOfSection,        10, CC_STDCALL },
    { "ZwUnmapViewOfSection",      (PVOID)NtUnmapViewOfSection,       2, CC_STDCALL },
    /* Synchronization (Phase 21) */
    { "NtCreateEvent",             (PVOID)NtCreateEvent,              5, CC_STDCALL },
    { "NtOpenEvent",               (PVOID)NtOpenEvent,                3, CC_STDCALL },
    { "NtSetEvent",                (PVOID)NtSetEvent,                 2, CC_STDCALL },
    { "NtResetEvent",              (PVOID)NtResetEvent,               2, CC_STDCALL },
    { "NtPulseEvent",              (PVOID)NtPulseEvent,               2, CC_STDCALL },
    { "NtWaitForSingleObject",     (PVOID)NtWaitForSingleObject,      3, CC_STDCALL },
    /* Zw aliases (identical in user mode) */
    { "ZwCreateFile",              (PVOID)NtCreateFile,              11, CC_STDCALL },
    { "ZwReadFile",                (PVOID)NtReadFile,                 9, CC_STDCALL },
    { "ZwWriteFile",               (PVOID)NtWriteFile,                9, CC_STDCALL },
    { "ZwLockFile",                (PVOID)NtLockFile,                 10, CC_STDCALL },
    { "ZwUnlockFile",              (PVOID)NtUnlockFile,                5, CC_STDCALL },
    { "ZwClose",                   (PVOID)NtClose,                    1, CC_STDCALL },
    { "ZwQueryObject",             (PVOID)NtQueryObject,              5, CC_STDCALL },
    { "ZwQueryInformationFile",    (PVOID)NtQueryInformationFile,     5, CC_STDCALL },
    { "ZwSetInformationFile",      (PVOID)NtSetInformationFile,       5, CC_STDCALL },
    { "ZwDuplicateObject",         (PVOID)NtDuplicateObject,          7, CC_STDCALL },
    { "ZwCreateEvent",             (PVOID)NtCreateEvent,              5, CC_STDCALL },
    { "ZwOpenEvent",               (PVOID)NtOpenEvent,                3, CC_STDCALL },
    { "ZwSetEvent",                (PVOID)NtSetEvent,                 2, CC_STDCALL },
    { "ZwResetEvent",              (PVOID)NtResetEvent,               2, CC_STDCALL },
    { "ZwPulseEvent",              (PVOID)NtPulseEvent,               2, CC_STDCALL },
    { "ZwWaitForSingleObject",     (PVOID)NtWaitForSingleObject,      3, CC_STDCALL },
    /* Rtl utilities */
    { "RtlInitUnicodeString",      (PVOID)RtlInitUnicodeString,       2, CC_STDCALL },
    { "RtlFormatCurrentUserKeyPath", (PVOID)RtlFormatCurrentUserKeyPath, 1, CC_STDCALL },
    { "RtlFreeUnicodeString",      (PVOID)RtlFreeUnicodeString,       1, CC_STDCALL },
    { "RtlInitializeCriticalSection", (PVOID)RtlInitializeCriticalSection, 1, CC_STDCALL },
    { "RtlInitializeCriticalSectionAndSpinCount", (PVOID)RtlInitializeCriticalSectionAndSpinCount, 2, CC_STDCALL },
    { "RtlInitializeCriticalSectionEx", (PVOID)RtlInitializeCriticalSectionEx, 3, CC_STDCALL },
    { "RtlEnterCriticalSection",  (PVOID)RtlEnterCriticalSection,    1, CC_STDCALL },
    { "RtlTryEnterCriticalSection", (PVOID)RtlTryEnterCriticalSection, 1, CC_STDCALL },
    { "RtlLeaveCriticalSection",  (PVOID)RtlLeaveCriticalSection,    1, CC_STDCALL },
    { "RtlDeleteCriticalSection", (PVOID)RtlDeleteCriticalSection,   1, CC_STDCALL },
    { "RtlAcquirePebLock",        (PVOID)RtlAcquirePebLock,          0, CC_STDCALL },
    { "RtlTryAcquirePebLock",     (PVOID)RtlTryAcquirePebLock,       0, CC_STDCALL },
    { "RtlReleasePebLock",        (PVOID)RtlReleasePebLock,          0, CC_STDCALL },
    { "RtlCopyMemory",             (PVOID)RtlCopyMemory,              3, CC_STDCALL },
    { "RtlZeroMemory",             (PVOID)RtlZeroMemory,              2, CC_STDCALL },
    { "RtlFillMemory",             (PVOID)RtlFillMemory,              3, CC_STDCALL },
    { "RtlNtStatusToDosError",     (PVOID)RtlNtStatusToDosError,      1, CC_STDCALL },
    /* SEH support */
    { "RtlRaiseException",         (PVOID)RtlRaiseException,          1, CC_STDCALL },
    { "RtlRaiseStatus",            (PVOID)RtlRaiseStatus,             1, CC_STDCALL },
    { "RtlUnwind",                 (PVOID)RtlUnwind,                  4, CC_STDCALL },
    { "RtlUnwindEx",               (PVOID)RtlUnwindEx,                6, CC_STDCALL },
    { "RtlVirtualUnwind",          (PVOID)RtlVirtualUnwind_stub,      8, CC_STDCALL },
    { "RtlCaptureContext",         (PVOID)RtlCaptureContext,          1,
                                      CC_STDCALL | CC_CONTEXT_CAPTURE },
    { "RtlRestoreContext",         (PVOID)RtlRestoreContext,          2, CC_CDECL },
    { "RtlLookupFunctionEntry",    (PVOID)RtlLookupFunctionEntry_stub, 3, CC_STDCALL },
    { "RtlAddFunctionTable",       (PVOID)RtlAddFunctionTable_ntdll,  3, CC_STDCALL },
    { "RtlDeleteFunctionTable",    (PVOID)RtlDeleteFunctionTable_ntdll, 1, CC_STDCALL },
    { "__C_specific_handler",      (PVOID)win32_unwind64_c_specific_handler, 4, CC_STDCALL },
    { "NtRaiseException",          (PVOID)NtRaiseException,           3, CC_STDCALL },
    { "ZwRaiseException",          (PVOID)NtRaiseException,           3, CC_STDCALL },
    /* Loader */
    { "LdrLoadDll",                (PVOID)LdrLoadDll,                 4, CC_STDCALL },
    { "LdrGetProcedureAddress",    (PVOID)LdrGetProcedureAddress,     4, CC_STDCALL },
    { "LdrLockLoaderLock",         (PVOID)LdrLockLoaderLock,          3, CC_STDCALL },
    { "LdrUnlockLoaderLock",       (PVOID)LdrUnlockLoaderLock,        2, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *ntdll_abi_table(int *count) {
    *count = (int)(sizeof(ntdll_exports)/sizeof(ntdll_exports[0]));
    return (const WIN32_EXPORT *)ntdll_exports;
}

#define NTDLL_EXPORT_COUNT \
    (sizeof(ntdll_exports) / sizeof(ntdll_exports[0]) - 1)

/* ── Resolve by name ────────────────────────────────────────── */

static int shim_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID ntdll_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) {
        /* We don't support ordinal-based import for ntdll */
        return NULL;
    }

    for (int i = 0; ntdll_exports[i].name; i++) {
        if (shim_strcmp(func_name, ntdll_exports[i].name) == 0)
            return ntdll_exports[i].func;
    }

    return NULL;
}

/* ── Shim init ──────────────────────────────────────────────── */

PVOID ntdll_shim_init(void)
{
    /* Nothing to allocate — exports are static.
     * Return a non-NULL sentinel so callers know init succeeded. */
    return (PVOID)ntdll_exports;
}
