/* Win32 byte-range lock manager. */

#ifndef WIN32_FILELOCK_H
#define WIN32_FILELOCK_H

#include "handle.h"

typedef struct _NT_FILE_IO_GUARD {
    /* Only this token lives on the caller's possibly process-private stack. */
    ULONGLONG cookie;
} NT_FILE_IO_GUARD;

typedef void (*NT_FILE_LOCK_COMPLETION)(PVOID context, NTSTATUS status);

NTSTATUS nt_file_lock_range(HANDLE handle, ULONG owner_pid,
                            ULONGLONG offset, ULONGLONG length, ULONG key,
                            BOOL fail_immediately, BOOL exclusive);
NTSTATUS nt_file_lock_range_request(
    HANDLE handle, ULONG owner_pid, ULONG owner_tid,
    ULONGLONG offset, ULONGLONG length, ULONG key,
    BOOL fail_immediately, BOOL exclusive, PVOID cancel_key,
    NT_FILE_LOCK_COMPLETION completion, PVOID completion_context);
NTSTATUS nt_file_unlock_range(HANDLE handle, ULONG owner_pid,
                              ULONGLONG offset, ULONGLONG length, ULONG key);

BOOL nt_file_lock_cancel(HANDLE handle, ULONG owner_pid, ULONG owner_tid,
                         PVOID cancel_key, BOOL current_thread_only);

NTSTATUS nt_file_io_guard_begin(PFILE_OBJECT file, ULONG owner_pid,
                                ULONGLONG offset, ULONGLONG length,
                                BOOL write, NT_FILE_IO_GUARD *guard);
void nt_file_io_guard_end(NT_FILE_IO_GUARD *guard);
/* The owner's threads must have stopped before retiring abandoned I/O. */
void nt_file_io_guards_release_process(ULONG owner_pid);

void nt_file_locks_release_owner(PFILE_OBJECT file, ULONG owner_pid);
void nt_file_locks_release_object(PFILE_OBJECT file);

#endif /* WIN32_FILELOCK_H */
