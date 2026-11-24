/* A process-private guard must never become a global list node. */
#define FILELOCK_HOST_TEST
#define sched_yield filelock_test_yield
#include "../include/types.h"
#include "../win32/filelock.c"

extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
extern void *mmap(void *, size_t, int, int, int, long);
extern int mprotect(void *, size_t, int);
extern int munmap(void *, size_t);
extern void abort(void);

#define CHECK(x) do { checks++; if (!(x)) { \
    printf("FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

static unsigned checks, failures, allocations, fail_alloc, locked;
static uint64_t irq_flags = 0x202;
static FILE_OBJECT files[3];
HANDLE_TABLE g_handle_table;
static unsigned completions;
static NTSTATUS completed_status;
static ULONG sync_state;
static BOOL cancel_on_yield;

static uint64_t file_range_lock_irqsave(void)
{
    if (locked) abort();
    locked = 1;
    uint64_t old = irq_flags;
    irq_flags &= ~0x200ULL;
    return old;
}

static void file_range_unlock_irqrestore(uint64_t flags)
{
    if (!locked) abort();
    locked = 0;
    irq_flags = flags;
}

void *kmalloc(uint64_t size)
{
    CHECK(!locked);
    if (fail_alloc) return NULL;
    void *ptr = malloc(size);
    if (ptr) allocations++;
    return ptr;
}

void kfree(void *ptr)
{
    CHECK(!locked);
    if (ptr) {
        CHECK(allocations != 0);
        allocations--;
        free(ptr);
    }
}

PVOID nt_thread_sync_io_begin(void)
{
    CHECK(!sync_state && !locked);
    sync_state = 1;
    return &sync_state;
}

BOOL nt_thread_sync_io_cancelled(PVOID token)
{
    return token && sync_state == 2;
}

void nt_thread_sync_io_end(PVOID token)
{
    if (!token) return;
    CHECK(token == &sync_state && sync_state && !locked);
    sync_state = 0;
}

void sched_yield(void)
{
    if (!cancel_on_yield || sync_state != 1 || locked) abort();
    sync_state = 2;
    cancel_on_yield = FALSE;
}

static void completed(PVOID context, NTSTATUS status)
{
    CHECK(!locked && context == &completions);
    completions++;
    completed_status = status;
    /* Completion can synchronously re-enter the manager. */
    CHECK(!nt_file_lock_cancel(INDEX_TO_HANDLE(3), 3, 0, NULL, FALSE));
}

NTSTATUS handle_lookup_access_for_process(
    PHANDLE_TABLE table, HANDLE handle, ULONG pid, OBJECT_TYPE_ID type,
    PVOID *object, ACCESS_MASK *access)
{
    CHECK(table == &g_handle_table && type == OBJ_TYPE_FILE);
    ULONG index = HANDLE_TO_INDEX(handle);
    if (!pid || index < 1 || index > 3) return STATUS_INVALID_HANDLE;
    *object = &files[index - 1];
    *access = GENERIC_READ | GENERIC_WRITE;
    return STATUS_SUCCESS;
}

int main(void)
{
    for (unsigned i = 0; i < 3; i++) {
        files[i].flags = FILE_OBJ_DISK_FILE;
        files[i].osfs_file = (void *)(uintptr_t)(i == 2 ? 2 : 1);
    }
    /* PROT_NONE models the issuing stack being absent under another CR3. */
    NT_FILE_IO_GUARD *private_guard = mmap(NULL, 4096, 3, 0x22, -1, 0);
    CHECK(private_guard != (void *)-1);
    if (private_guard == (void *)-1) return 1;
    CHECK(nt_file_io_guard_begin(&files[0], 1, 10, 20, TRUE,
                                private_guard) == STATUS_SUCCESS);
    CHECK(mprotect(private_guard, 4096, 0) == 0);
    NT_FILE_IO_GUARD other;
    CHECK(nt_file_io_guard_begin(&files[2], 2, 100, 10, FALSE, &other) == STATUS_SUCCESS);
    nt_file_io_guard_end(&other);
    CHECK(nt_file_lock_range(INDEX_TO_HANDLE(2), 2, 10, 20, 0,
                            TRUE, TRUE) == STATUS_LOCK_NOT_GRANTED);
    CHECK(nt_file_lock_range(INDEX_TO_HANDLE(3), 2, 10, 20, 0,
                            TRUE, TRUE) == STATUS_SUCCESS);
    CHECK(nt_file_unlock_range(INDEX_TO_HANDLE(3), 2, 10, 20, 0) == STATUS_SUCCESS);
    CHECK(mprotect(private_guard, 4096, 3) == 0);
    nt_file_io_guard_end(private_guard);
    nt_file_io_guard_end(private_guard);
    CHECK(!g_file_io_guards && !g_file_range_locks && !allocations);

    /* Same virtual address, different contents must not change the record. */
    CHECK(nt_file_io_guard_begin(&files[0], 1, 10, 20, TRUE, private_guard) == STATUS_SUCCESS);
    NT_FILE_IO_GUARD saved = *private_guard;
    memset(private_guard, 0xa5, sizeof(*private_guard));
    CHECK(nt_file_lock_range(INDEX_TO_HANDLE(2), 2, 10, 20, 0,
                            TRUE, FALSE) == STATUS_LOCK_NOT_GRANTED);
    *private_guard = saved;
    CHECK(mprotect(private_guard, 4096, 0) == 0);
    nt_file_io_guards_release_process(2);
    CHECK(allocations == 1);
    nt_file_io_guards_release_process(0);
    CHECK(allocations == 1);
    nt_file_io_guards_release_process(1);
    nt_file_io_guards_release_process(1);
    CHECK(!allocations && !g_file_io_guards);
    CHECK(nt_file_io_guard_begin(&files[0], 1, 10, 20, TRUE, &other) == STATUS_SUCCESS);
    CHECK(mprotect(private_guard, 4096, 3) == 0);
    nt_file_io_guard_end(private_guard);
    CHECK(allocations == 1);
    nt_file_io_guard_end(&other);
    CHECK(!allocations);

    CHECK(nt_file_io_guard_begin(&files[0], 1, 10, 20, FALSE, private_guard) == STATUS_SUCCESS);
    CHECK(mprotect(private_guard, 4096, 0) == 0);
    CHECK(nt_file_lock_range(INDEX_TO_HANDLE(2), 2, 10, 20, 0,
                            TRUE, FALSE) == STATUS_SUCCESS);
    CHECK(nt_file_unlock_range(INDEX_TO_HANDLE(2), 2, 10, 20, 0) == STATUS_SUCCESS);
    CHECK(nt_file_lock_range_request(INDEX_TO_HANDLE(2), 2, 7, 10, 20, 0,
                                    FALSE, TRUE, NULL, completed, &completions) == STATUS_PENDING);
    CHECK(completions == 0);
    nt_file_io_guards_release_process(1);
    CHECK(completions == 1 && completed_status == STATUS_SUCCESS);
    CHECK(nt_file_unlock_range(INDEX_TO_HANDLE(2), 2, 10, 20, 0) == STATUS_SUCCESS);
    CHECK(!allocations && !g_file_lock_waiters_head && !g_file_lock_waiters_tail);
    CHECK(mprotect(private_guard, 4096, 3) == 0);
    nt_file_io_guard_end(private_guard);

    CHECK(nt_file_io_guard_begin(&files[0], 1, 0, 1, TRUE, private_guard) == STATUS_SUCCESS);
    CHECK(nt_file_lock_range_request(INDEX_TO_HANDLE(2), 2, 7, 0, 1, 0,
                                    FALSE, TRUE, NULL, completed, &completions) == STATUS_PENDING);
    CHECK(nt_file_lock_cancel(INDEX_TO_HANDLE(2), 2, 7, NULL, TRUE));
    CHECK(completions == 2 && completed_status == STATUS_CANCELLED);
    nt_file_io_guard_end(private_guard);
    CHECK(!allocations);

    CHECK(nt_file_lock_range(INDEX_TO_HANDLE(1), 1, 0, 1, 0, TRUE, TRUE) == STATUS_SUCCESS);
    CHECK(nt_file_io_guard_begin(&files[1], 2, 0, 1, FALSE, &other) == STATUS_FILE_LOCK_CONFLICT);
    CHECK(other.cookie == 0 && allocations == 1);
    CHECK(nt_file_unlock_range(INDEX_TO_HANDLE(1), 1, 0, 1, 0) == STATUS_SUCCESS);
    CHECK(!allocations);

    /* Cancellation frees a synchronous waiter without releasing another
     * open's lock, publishing an async completion, or poisoning the next call. */
    CHECK(nt_file_lock_range(INDEX_TO_HANDLE(1), 1, 0, 1, 0, TRUE, TRUE) == STATUS_SUCCESS);
    files[1].create_options = FILE_SYNCHRONOUS_IO_NONALERT;
    cancel_on_yield = TRUE;
    CHECK(nt_file_lock_range_request(INDEX_TO_HANDLE(2), 2, 7, 0, 1, 0,
        FALSE, TRUE, NULL, completed, &completions) == STATUS_CANCELLED);
    CHECK(!cancel_on_yield && !sync_state && completions == 2);
    CHECK(allocations == 1 && !g_file_lock_waiters_head);
    CHECK(nt_file_lock_range(INDEX_TO_HANDLE(2), 2, 0, 1, 0, TRUE, TRUE) == STATUS_LOCK_NOT_GRANTED);
    CHECK(nt_file_unlock_range(INDEX_TO_HANDLE(1), 1, 0, 1, 0) == STATUS_SUCCESS);
    CHECK(nt_file_lock_range(INDEX_TO_HANDLE(2), 2, 0, 1, 0, FALSE, TRUE) == STATUS_SUCCESS);
    CHECK(!sync_state && allocations == 1);
    CHECK(nt_file_unlock_range(INDEX_TO_HANDLE(2), 2, 0, 1, 0) == STATUS_SUCCESS);
    CHECK(!allocations);
    files[1].create_options = 0;

    fail_alloc = 1;
    CHECK(nt_file_io_guard_begin(&files[0], 1, 0, 1, FALSE, &other) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(!other.cookie && !allocations && !g_file_io_guards);
    nt_file_io_guard_end(&other);
    CHECK(nt_file_io_guard_begin(&files[0], 1, 0, 0, FALSE, &other) == STATUS_SUCCESS);
    CHECK(!other.cookie);
    CHECK(nt_file_io_guard_begin(NULL, 1, 0, 1, FALSE, &other) == STATUS_INVALID_PARAMETER);
    CHECK(nt_file_io_guard_begin(&files[0], 0, 0, 1, FALSE, &other) == STATUS_INVALID_PARAMETER);
    CHECK(nt_file_io_guard_begin(&files[0], 1, 0, 1, FALSE, NULL) == STATUS_INVALID_PARAMETER);
    nt_file_io_guard_end(NULL);
    fail_alloc = 0;
    g_file_io_cookie = ~0ULL;
    CHECK(nt_file_io_guard_begin(&files[0], 1, 0, 1, FALSE, &other) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(!allocations && !g_file_io_guards && !other.cookie);
    g_file_io_cookie = 0;

    for (unsigned i = 0; i < 1024; i++) {
        irq_flags = i & 1 ? 0x202 : 2;
        CHECK(nt_file_io_guard_begin(&files[0], 1, ~0ULL - 1, 4, TRUE, &other) == STATUS_SUCCESS);
        CHECK(nt_file_lock_range(INDEX_TO_HANDLE(2), 2, ~0ULL, 1, 0, TRUE, TRUE) == STATUS_LOCK_NOT_GRANTED);
        CHECK(nt_file_lock_range(INDEX_TO_HANDLE(2), 2, 0, 1, 0, TRUE, TRUE) == STATUS_SUCCESS);
        nt_file_locks_release_owner(&files[1], 2);
        nt_file_io_guard_end(&other);
        CHECK(!allocations && !g_file_io_guards && !g_file_range_locks);
        CHECK(irq_flags == (i & 1 ? 0x202U : 2));
    }
    CHECK(munmap(private_guard, 4096) == 0);
    CHECK(!locked && irq_flags == 0x202);
    printf("filelock host: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
