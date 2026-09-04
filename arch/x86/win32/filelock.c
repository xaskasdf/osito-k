/*
 * Process-aware Win32 byte-range locks.
 *
 * Lock identity follows the underlying file plus the FILE_OBJECT used to open
 * it. Duplicate handles therefore share lock ownership, while a second open
 * of the same path does not.
 */

#include "filelock.h"
#include "ntsyscall.h"
#include "../kernel/smp.h"

extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);
extern void sched_yield(void);
extern HANDLE_TABLE g_handle_table;

typedef struct _NT_FILE_RANGE_LOCK {
    struct _NT_FILE_RANGE_LOCK *next;
    PVOID identity;
    PFILE_OBJECT owner_file;
    ULONG owner_pid;
    ULONGLONG offset;
    ULONGLONG length;
    ULONG key;
    BOOL exclusive;
    HANDLE issuing_handle;
    ULONG owner_tid;
    PVOID cancel_key;
    NT_FILE_LOCK_COMPLETION completion;
    PVOID completion_context;
} NT_FILE_RANGE_LOCK;

static spinlock_t g_file_range_lock = SPINLOCK_INIT;
static NT_FILE_RANGE_LOCK *g_file_range_locks;
static NT_FILE_IO_GUARD *g_file_io_guards;
static NT_FILE_RANGE_LOCK *g_file_lock_waiters_head;
static NT_FILE_RANGE_LOCK *g_file_lock_waiters_tail;

static uint64_t file_range_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&g_file_range_lock);
    return flags;
}

static void file_range_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&g_file_range_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static ULONGLONG file_range_last(ULONGLONG offset, ULONGLONG length)
{
    ULONGLONG delta = length - 1;
    if (delta > ~0ULL - offset)
        return ~0ULL;
    return offset + delta;
}

static BOOL file_ranges_overlap(ULONGLONG left_offset, ULONGLONG left_length,
                                ULONGLONG right_offset,
                                ULONGLONG right_length)
{
    if (!left_length || !right_length)
        return FALSE;
    return left_offset <= file_range_last(right_offset, right_length) &&
           right_offset <= file_range_last(left_offset, left_length);
}

static BOOL file_lock_same_owner(const NT_FILE_RANGE_LOCK *lock,
                                 ULONG owner_pid, PFILE_OBJECT owner_file)
{
    return lock->owner_pid == owner_pid && lock->owner_file == owner_file;
}

static NTSTATUS file_lock_resolve(HANDLE handle, ULONG owner_pid,
                                  PFILE_OBJECT *file)
{
    ACCESS_MASK access = 0;
    NTSTATUS status = handle_lookup_access_for_process(
        &g_handle_table, handle, owner_pid, OBJ_TYPE_FILE,
        (PVOID *)file, &access);
    if (!NT_SUCCESS(status))
        return status;
    if (!(*file)->osfs_file || !((*file)->flags & FILE_OBJ_DISK_FILE))
        return STATUS_INVALID_DEVICE_REQUEST;
    if (!(access & (GENERIC_READ | GENERIC_WRITE | FILE_READ_DATA |
                    FILE_WRITE_DATA | FILE_APPEND_DATA)))
        return STATUS_ACCESS_DENIED;
    return STATUS_SUCCESS;
}

static BOOL file_lock_conflicts_locked(PVOID identity,
                                       PFILE_OBJECT owner_file,
                                       ULONG owner_pid,
                                       ULONGLONG offset, ULONGLONG length,
                                       BOOL exclusive)
{
    for (NT_FILE_RANGE_LOCK *lock = g_file_range_locks; lock;
         lock = lock->next) {
        if (lock->identity != identity ||
            !file_ranges_overlap(lock->offset, lock->length,
                                 offset, length))
            continue;
        if (exclusive ||
            (lock->exclusive &&
             !file_lock_same_owner(lock, owner_pid, owner_file)))
            return TRUE;
    }

    for (NT_FILE_IO_GUARD *guard = g_file_io_guards; guard;
         guard = guard->next) {
        if (guard->identity != identity ||
            !file_ranges_overlap(guard->offset, guard->length,
                                 offset, length))
            continue;
        if (exclusive || guard->write)
            return TRUE;
    }
    return FALSE;
}

static BOOL file_lock_waiter_precedes_locked(
    const NT_FILE_RANGE_LOCK *request)
{
    for (NT_FILE_RANGE_LOCK *waiter = g_file_lock_waiters_head;
         waiter && waiter != request; waiter = waiter->next) {
        if (waiter->identity == request->identity &&
            file_ranges_overlap(waiter->offset, waiter->length,
                                request->offset, request->length) &&
            (waiter->exclusive || request->exclusive))
            return TRUE;
    }
    return FALSE;
}

static void file_lock_enqueue_waiter_locked(NT_FILE_RANGE_LOCK *request)
{
    request->next = NULL;
    if (g_file_lock_waiters_tail)
        g_file_lock_waiters_tail->next = request;
    else
        g_file_lock_waiters_head = request;
    g_file_lock_waiters_tail = request;
}

static BOOL file_lock_grant_one_waiter(void)
{
    NT_FILE_LOCK_COMPLETION completion = NULL;
    PVOID completion_context = NULL;
    uint64_t flags = file_range_lock_irqsave();
    NT_FILE_RANGE_LOCK *previous = NULL;
    NT_FILE_RANGE_LOCK **link = &g_file_lock_waiters_head;

    while (*link) {
        NT_FILE_RANGE_LOCK *request = *link;
        BOOL conflict = file_lock_conflicts_locked(
            request->identity, request->owner_file, request->owner_pid,
            request->offset, request->length, request->exclusive);
        if (!conflict && !file_lock_waiter_precedes_locked(request)) {
            *link = request->next;
            if (g_file_lock_waiters_tail == request)
                g_file_lock_waiters_tail = previous;
            request->next = g_file_range_locks;
            g_file_range_locks = request;
            completion = request->completion;
            completion_context = request->completion_context;
            request->completion = NULL;
            request->completion_context = NULL;
            file_range_unlock_irqrestore(flags);
            if (completion)
                completion(completion_context, STATUS_SUCCESS);
            return TRUE;
        }
        previous = request;
        link = &request->next;
    }

    file_range_unlock_irqrestore(flags);
    return FALSE;
}

static void file_lock_service_waiters(void)
{
    while (file_lock_grant_one_waiter()) { }
}

static NT_FILE_RANGE_LOCK *file_lock_detach_waiter_locked(
    PFILE_OBJECT owner_file, ULONG owner_pid, HANDLE issuing_handle,
    BOOL match_handle, ULONG owner_tid, BOOL match_thread,
    PVOID cancel_key, BOOL match_key)
{
    NT_FILE_RANGE_LOCK *previous = NULL;
    NT_FILE_RANGE_LOCK **link = &g_file_lock_waiters_head;
    while (*link) {
        NT_FILE_RANGE_LOCK *request = *link;
        if ((!owner_file || request->owner_file == owner_file) &&
            (!owner_pid || request->owner_pid == owner_pid) &&
            (!match_handle || request->issuing_handle == issuing_handle) &&
            (!match_thread || request->owner_tid == owner_tid) &&
            (!match_key || request->cancel_key == cancel_key)) {
            *link = request->next;
            if (g_file_lock_waiters_tail == request)
                g_file_lock_waiters_tail = previous;
            request->next = NULL;
            return request;
        }
        previous = request;
        link = &request->next;
    }
    return NULL;
}

static BOOL file_lock_cancel_waiters(
    PFILE_OBJECT owner_file, ULONG owner_pid, HANDLE issuing_handle,
    BOOL match_handle, ULONG owner_tid, BOOL match_thread,
    PVOID cancel_key, BOOL match_key)
{
    BOOL found = FALSE;
    for (;;) {
        uint64_t flags = file_range_lock_irqsave();
        NT_FILE_RANGE_LOCK *request = file_lock_detach_waiter_locked(
            owner_file, owner_pid, issuing_handle, match_handle,
            owner_tid, match_thread, cancel_key, match_key);
        file_range_unlock_irqrestore(flags);
        if (!request)
            break;

        NT_FILE_LOCK_COMPLETION completion = request->completion;
        PVOID completion_context = request->completion_context;
        kfree(request);
        found = TRUE;
        if (completion)
            completion(completion_context, STATUS_CANCELLED);
    }
    if (found)
        file_lock_service_waiters();
    return found;
}

NTSTATUS nt_file_lock_range(HANDLE handle, ULONG owner_pid,
                            ULONGLONG offset, ULONGLONG length, ULONG key,
                            BOOL fail_immediately, BOOL exclusive)
{
    return nt_file_lock_range_request(
        handle, owner_pid, 0, offset, length, key, fail_immediately,
        exclusive, NULL, NULL, NULL);
}

NTSTATUS nt_file_lock_range_request(
    HANDLE handle, ULONG owner_pid, ULONG owner_tid,
    ULONGLONG offset, ULONGLONG length, ULONG key,
    BOOL fail_immediately, BOOL exclusive, PVOID cancel_key,
    NT_FILE_LOCK_COMPLETION completion, PVOID completion_context)
{
    if (!owner_pid)
        return STATUS_INVALID_PARAMETER;

    PFILE_OBJECT initial_file = NULL;
    PVOID identity = NULL;
    ULONG create_options = 0;
    uint64_t flags = file_range_lock_irqsave();
    NTSTATUS status = file_lock_resolve(handle, owner_pid, &initial_file);
    if (NT_SUCCESS(status)) {
        identity = initial_file->osfs_file;
        create_options = initial_file->create_options;
    }
    file_range_unlock_irqrestore(flags);
    if (!NT_SUCCESS(status) || !length)
        return status;

    NT_FILE_RANGE_LOCK *request =
        (NT_FILE_RANGE_LOCK *)kmalloc(sizeof(*request));
    if (!request)
        return STATUS_INSUFFICIENT_RESOURCES;

    request->next = NULL;
    request->identity = identity;
    request->owner_file = initial_file;
    request->owner_pid = owner_pid;
    request->offset = offset;
    request->length = length;
    request->key = key;
    request->exclusive = exclusive ? TRUE : FALSE;
    request->issuing_handle = handle;
    request->owner_tid = owner_tid;
    request->cancel_key = cancel_key;
    request->completion = completion;
    request->completion_context = completion_context;

    BOOL asynchronous = completion &&
        !(create_options &
          (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT));

    for (;;) {
        flags = file_range_lock_irqsave();
        PFILE_OBJECT locked_file = NULL;
        status = file_lock_resolve(handle, owner_pid, &locked_file);
        if (!NT_SUCCESS(status) || locked_file != initial_file ||
            locked_file->osfs_file != request->identity) {
            file_range_unlock_irqrestore(flags);
            kfree(request);
            return NT_SUCCESS(status) ? STATUS_INVALID_HANDLE : status;
        }
        BOOL conflict = file_lock_conflicts_locked(
            request->identity, request->owner_file, request->owner_pid,
            request->offset, request->length, request->exclusive);
        if (!conflict)
            conflict = file_lock_waiter_precedes_locked(request);
        if (!conflict) {
            request->next = g_file_range_locks;
            g_file_range_locks = request;
            file_range_unlock_irqrestore(flags);
            return STATUS_SUCCESS;
        }
        if (fail_immediately) {
            file_range_unlock_irqrestore(flags);
            kfree(request);
            return STATUS_LOCK_NOT_GRANTED;
        }
        if (asynchronous) {
            file_lock_enqueue_waiter_locked(request);
            file_range_unlock_irqrestore(flags);
            return STATUS_PENDING;
        }
        file_range_unlock_irqrestore(flags);
        /* Synchronous file objects wait in the issuing thread. */
        sched_yield();
    }
}

NTSTATUS nt_file_unlock_range(HANDLE handle, ULONG owner_pid,
                              ULONGLONG offset, ULONGLONG length, ULONG key)
{
    if (!owner_pid)
        return STATUS_INVALID_PARAMETER;

    uint64_t flags = file_range_lock_irqsave();
    PFILE_OBJECT file = NULL;
    NTSTATUS status = file_lock_resolve(handle, owner_pid, &file);
    if (!NT_SUCCESS(status) || !length) {
        file_range_unlock_irqrestore(flags);
        return status;
    }
    NT_FILE_RANGE_LOCK **shared_match = NULL;
    NT_FILE_RANGE_LOCK **link = &g_file_range_locks;
    while (*link) {
        NT_FILE_RANGE_LOCK *lock = *link;
        if (lock->identity == file->osfs_file &&
            file_lock_same_owner(lock, owner_pid, file) &&
            lock->offset == offset && lock->length == length &&
            lock->key == key) {
            if (lock->exclusive) {
                *link = lock->next;
                file_range_unlock_irqrestore(flags);
                kfree(lock);
                file_lock_service_waiters();
                return STATUS_SUCCESS;
            }
            if (!shared_match)
                shared_match = link;
        }
        link = &lock->next;
    }

    if (shared_match) {
        NT_FILE_RANGE_LOCK *lock = *shared_match;
        *shared_match = lock->next;
        file_range_unlock_irqrestore(flags);
        kfree(lock);
        file_lock_service_waiters();
        return STATUS_SUCCESS;
    }
    file_range_unlock_irqrestore(flags);
    return STATUS_RANGE_NOT_LOCKED;
}

BOOL nt_file_lock_cancel(HANDLE handle, ULONG owner_pid, ULONG owner_tid,
                         PVOID cancel_key, BOOL current_thread_only)
{
    if (!handle || !owner_pid)
        return FALSE;
    return file_lock_cancel_waiters(
        NULL, owner_pid, handle, TRUE, owner_tid, current_thread_only,
        cancel_key, cancel_key != NULL);
}

NTSTATUS nt_file_io_guard_begin(PFILE_OBJECT file, ULONG owner_pid,
                                ULONGLONG offset, ULONGLONG length,
                                BOOL write, NT_FILE_IO_GUARD *guard)
{
    if (!file || !file->osfs_file || !owner_pid || !guard)
        return STATUS_INVALID_PARAMETER;

    guard->next = NULL;
    guard->identity = file->osfs_file;
    guard->owner_file = file;
    guard->owner_pid = owner_pid;
    guard->offset = offset;
    guard->length = length;
    guard->write = write ? TRUE : FALSE;
    guard->active = FALSE;
    if (!length)
        return STATUS_SUCCESS;

    uint64_t flags = file_range_lock_irqsave();
    for (NT_FILE_RANGE_LOCK *lock = g_file_range_locks; lock;
         lock = lock->next) {
        if (lock->identity != guard->identity ||
            !file_ranges_overlap(lock->offset, lock->length,
                                 offset, length))
            continue;
        if ((write && !lock->exclusive) ||
            (lock->exclusive &&
             !file_lock_same_owner(lock, owner_pid, file))) {
            file_range_unlock_irqrestore(flags);
            return STATUS_FILE_LOCK_CONFLICT;
        }
    }

    guard->next = g_file_io_guards;
    guard->active = TRUE;
    g_file_io_guards = guard;
    file_range_unlock_irqrestore(flags);
    return STATUS_SUCCESS;
}

void nt_file_io_guard_end(NT_FILE_IO_GUARD *guard)
{
    if (!guard || !guard->active)
        return;

    uint64_t flags = file_range_lock_irqsave();
    NT_FILE_IO_GUARD **link = &g_file_io_guards;
    while (*link && *link != guard)
        link = &(*link)->next;
    if (*link == guard)
        *link = guard->next;
    guard->next = NULL;
    guard->active = FALSE;
    file_range_unlock_irqrestore(flags);
    file_lock_service_waiters();
}

static NT_FILE_RANGE_LOCK *file_lock_detach_owner_locked(
    PFILE_OBJECT file, ULONG owner_pid, BOOL match_process)
{
    NT_FILE_RANGE_LOCK **link = &g_file_range_locks;
    while (*link) {
        NT_FILE_RANGE_LOCK *lock = *link;
        if (lock->owner_file == file &&
            (!match_process || lock->owner_pid == owner_pid)) {
            *link = lock->next;
            lock->next = NULL;
            return lock;
        }
        link = &lock->next;
    }
    return NULL;
}

void nt_file_locks_release_owner(PFILE_OBJECT file, ULONG owner_pid)
{
    if (!file || !owner_pid)
        return;
    (void)file_lock_cancel_waiters(file, owner_pid, NULL, FALSE, 0, FALSE,
                                   NULL, FALSE);
    for (;;) {
        uint64_t flags = file_range_lock_irqsave();
        NT_FILE_RANGE_LOCK *lock =
            file_lock_detach_owner_locked(file, owner_pid, TRUE);
        file_range_unlock_irqrestore(flags);
        if (!lock)
            break;
        kfree(lock);
    }
    file_lock_service_waiters();
}

void nt_file_locks_release_object(PFILE_OBJECT file)
{
    if (!file)
        return;
    (void)file_lock_cancel_waiters(file, 0, NULL, FALSE, 0, FALSE,
                                   NULL, FALSE);
    for (;;) {
        uint64_t flags = file_range_lock_irqsave();
        NT_FILE_RANGE_LOCK *lock =
            file_lock_detach_owner_locked(file, 0, FALSE);
        file_range_unlock_irqrestore(flags);
        if (!lock)
            break;
        kfree(lock);
    }
    file_lock_service_waiters();
}
