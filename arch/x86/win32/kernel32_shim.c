/*
 * OsitoK Windows Compatibility Layer — kernel32.dll Shim Implementation
 *
 * Translates Win32 API calls to NT API calls.
 * This is what makes regular Windows .exe files work.
 */

#include "kernel32_shim.h"
#include "ntdll_shim.h"
#include "../fs/ositofs3.h"
#include "ntsyscall.h"
#include "dllloader.h"
#include "handle.h"
#include "win32_abi.h"
#include "wsock32_shim.h"
#include "wintime.h"
#include "../include/paging.h"
#include "../kernel/smp.h"

/* ── Kernel interfaces (forward declarations) ─────────────── */
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

typedef struct _SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
} SYSTEMTIME;

#ifndef K32_IOCP_TRACE
#if defined(OK_QUIET) && OK_QUIET
#define K32_IOCP_TRACE 0
#else
#define K32_IOCP_TRACE 1
#endif
#endif

#define K32_IOCP_TRACE_LIMIT 160U

#if defined(OK_QUIET) && OK_QUIET
#define K32_VERBOSE_DIAGNOSTICS 0
#else
#define K32_VERBOSE_DIAGNOSTICS 1
#endif

/* ── Shim handle table for dynamically loaded shim DLLs ────── */
/* Keep high-volume CEF loader diagnostics bounded. */
static volatile LONG g_cef_delay_trace_budget = 96;

/* ── Per-thread last error ──────────────────────────────────── */

static void iocp_forget_handle(HANDLE handle);
static void iocp_reset_all(void);
static BOOL k32_pipe_read_overlapped(HANDLE file, PVOID buffer, DWORD length,
                                     DWORD *bytes_read, PVOID overlapped);
static BOOL k32_pipe_write_overlapped(HANDLE file, PCVOID buffer, DWORD length,
                                      DWORD *bytes_written, PVOID overlapped);
void k32_pipe_service_pending(void);
static BOOL k32_cancel_pipe_io(HANDLE file, PVOID target_overlapped,
                               BOOL current_thread_only);
static void k32_pipe_wait_quiescent(HANDLE file);
static void k32_store_overlapped_status(PVOID overlapped, BOOL compat32,
                                        NTSTATUS status, ULONG_PTR bytes);
extern void *kmalloc(uint64_t size);
extern void *kcalloc(uint64_t count, uint64_t size);
extern void  kfree(void *ptr);
extern NTSTATUS ntsync_set_event_for_process(HANDLE, ULONG, LONG *);
extern HANDLE_TABLE g_handle_table;

#define K32_MAX_JOB_OBJECTS 8
#define K32_MAX_JOB_PROCESSES 8
#define K32_JOB_OBJECT_LIMIT_KILL_ON_CLOSE 0x00002000U

typedef struct {
    BOOL in_use;
    DWORD limit_flags;
    HANDLE processes[K32_MAX_JOB_PROCESSES];
} K32_JOB_OBJECT;

static K32_JOB_OBJECT k32_jobs[K32_MAX_JOB_OBJECTS];

static void job_terminate_processes(K32_JOB_OBJECT *job, DWORD exit_code)
{
    extern BOOL win32_terminate_child(HANDLE, NTSTATUS);

    for (int i = 0; i < K32_MAX_JOB_PROCESSES; i++) {
        if (job->processes[i])
            win32_terminate_child(job->processes[i], (NTSTATUS)exit_code);
    }
}

static void job_release(K32_JOB_OBJECT *job, BOOL honor_kill_on_close)
{
    if (!job || !job->in_use)
        return;
    if (honor_kill_on_close &&
        (job->limit_flags & K32_JOB_OBJECT_LIMIT_KILL_ON_CLOSE)) {
        serial_puts("[K32-JOB] last handle closed; terminating processes\n");
        job_terminate_processes(job, 1);
    }
    for (int i = 0; i < K32_MAX_JOB_PROCESSES; i++) {
        if (job->processes[i])
            handle_close(&g_handle_table, job->processes[i]);
    }
    memset(job, 0, sizeof(*job));
}

/* Keep direct TEB reads in sync with GetLastError(). */
extern TEB g_teb;
extern TEB *win64_current_teb(void);
extern const char *win32_current_exe_name(void);
extern const char *win32_current_image_path(void);
extern const char *win32_current_command_line(void);
extern const WCHAR *win32_current_command_line_w(void);
extern const char *win32_current_directory_override(void);
extern BOOL win32_set_current_directory_override(const char *path);
extern BOOL win32_refresh_current_process_parameters(void);
extern ULONG_PTR win32_current_image_base(void);
extern DWORD win32_current_process_id(void);
extern DWORD win32_current_process_thread_id(void);
extern int32_t proc_current_pid(void);
PVOID win32_current_thread_process_peb(void);
extern DWORD WINAPI GetCurrentThreadId(void);
extern TEB32 g_teb32;
extern TEB32 *compat32_current_teb(void);
extern int g_compat32_ut99;
extern uint32_t compat32_callback_args(uint32_t func_addr, int nargs,
                                       const uint32_t *args);
static int k32_strcmp(const char *a, const char *b);
static BOOL process_command_contains(PCSTR command, PCSTR needle);
static void iocp_release_thread(DWORD tid);
static void k32_process_affinity_release(DWORD process_id);
static DWORD bootstrap_last_error;

#define K32_MAX_IO_COMPLETIONS 256
#define K32_WAIT_IO_COMPLETION 0x000000C0U

typedef struct {
    BOOL used;
    BOOL ready;
    BOOL compat32;
    DWORD owner_pid;
    DWORD owner_tid;
    DWORD generation;
    HANDLE file;
    PVOID overlapped;
    PVOID routine;
    NTSTATUS status;
    DWORD bytes;
} K32_IO_COMPLETION;

static K32_IO_COMPLETION g_io_completions[K32_MAX_IO_COMPLETIONS];
static volatile LONG g_io_completion_lock;
static DWORD g_io_completion_generation;
static volatile uint32_t g_io_completion_trace_count;

static uint64_t k32_io_completion_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&g_io_completion_lock, 1))
        __asm__ volatile ("pause");
    return flags;
}

static void k32_io_completion_unlock_irqrestore(uint64_t flags)
{
    __sync_lock_release(&g_io_completion_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static int k32_io_completion_reserve(HANDLE file, PVOID overlapped,
                                     PVOID routine, BOOL compat32,
                                     DWORD *generation)
{
    int slot = -1;
    uint64_t flags = k32_io_completion_lock_irqsave();
    for (int i = 0; i < K32_MAX_IO_COMPLETIONS; i++) {
        if (!g_io_completions[i].used) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        K32_IO_COMPLETION *completion = &g_io_completions[slot];
        memset(completion, 0, sizeof(*completion));
        completion->used = TRUE;
        completion->compat32 = compat32;
        completion->owner_pid = win32_current_process_id();
        completion->owner_tid = GetCurrentThreadId();
        completion->generation = ++g_io_completion_generation;
        if (!completion->generation)
            completion->generation = ++g_io_completion_generation;
        completion->file = file;
        completion->overlapped = overlapped;
        completion->routine = routine;
        *generation = completion->generation;
    }
    k32_io_completion_unlock_irqrestore(flags);
    return slot;
}

static void k32_io_completion_set_ready(int slot, DWORD generation,
                                        NTSTATUS status, DWORD bytes)
{
    uint64_t flags = k32_io_completion_lock_irqsave();
    if (slot >= 0 && slot < K32_MAX_IO_COMPLETIONS) {
        K32_IO_COMPLETION *completion = &g_io_completions[slot];
        if (completion->used && completion->generation == generation) {
            completion->status = status;
            completion->bytes = bytes;
            completion->ready = TRUE;
        }
    }
    k32_io_completion_unlock_irqrestore(flags);
}

static void k32_io_completion_cancel(int slot, DWORD generation)
{
    uint64_t flags = k32_io_completion_lock_irqsave();
    if (slot >= 0 && slot < K32_MAX_IO_COMPLETIONS &&
        g_io_completions[slot].used &&
        g_io_completions[slot].generation == generation)
        memset(&g_io_completions[slot], 0, sizeof(g_io_completions[slot]));
    k32_io_completion_unlock_irqrestore(flags);
}

static void k32_io_completion_release_thread(DWORD tid)
{
    uint64_t flags = k32_io_completion_lock_irqsave();
    for (int i = 0; i < K32_MAX_IO_COMPLETIONS; i++) {
        if (g_io_completions[i].used &&
            g_io_completions[i].owner_tid == tid)
            memset(&g_io_completions[i], 0, sizeof(g_io_completions[i]));
    }
    k32_io_completion_unlock_irqrestore(flags);
}

static void k32_io_completion_release_process(DWORD process_id)
{
    uint64_t flags = k32_io_completion_lock_irqsave();
    for (int i = 0; i < K32_MAX_IO_COMPLETIONS; i++) {
        if (g_io_completions[i].used &&
            g_io_completions[i].owner_pid == process_id)
            memset(&g_io_completions[i], 0, sizeof(g_io_completions[i]));
    }
    k32_io_completion_unlock_irqrestore(flags);
}

static DWORD k32_dispatch_io_completions(void);

#if K32_VERBOSE_DIAGNOSTICS
#define K32_MOJO_IO_TRACE_LIMIT        64U
#define K32_MOJO_XFER_TRACE_LIMIT      64U
#define K32_MOJO_DEQUEUE_TRACE_LIMIT   64U
#else
#define K32_MOJO_IO_TRACE_LIMIT         0U
#define K32_MOJO_XFER_TRACE_LIMIT       0U
#define K32_MOJO_DEQUEUE_TRACE_LIMIT    0U
#endif
#define K32_MOJO_TRACE_BYTES           16U
#define K32_MOJO_TRACE_PID_SLOTS      256U
#define K32_MOJO_RING_RECORDS         512U
#define K32_MOJO_RING_BYTES          4096U
#define K32_MOJO_RING_READ              1U
#define K32_MOJO_RING_WRITE             2U

typedef struct {
    uint64_t sequence;
    DWORD owner_pid;
    DWORD owner_tid;
    HANDLE file;
    PVOID overlapped;
    DWORD operation;
    NTSTATUS status;
    DWORD requested;
    DWORD transferred;
    DWORD captured;
    DWORD reserved;
    BYTE data[K32_MOJO_RING_BYTES];
} K32_MOJO_TRACE_RECORD;

volatile uint64_t g_k32_mojo_trace_sequence;
K32_MOJO_TRACE_RECORD g_k32_mojo_trace_records[K32_MOJO_RING_RECORDS];

static volatile uint32_t g_mojo_read_trace_count;
static volatile uint32_t g_mojo_write_trace_count;
static volatile uint32_t g_mojo_iocp_trace_count;
static volatile uint32_t g_mojo_xfer_trace_count;
static volatile uint32_t g_mojo_dequeue_trace_count;
static volatile BYTE g_mojo_trace_pids[K32_MOJO_TRACE_PID_SLOTS];

static BOOL k32_mojo_trace_current(void)
{
    const char *command = win32_current_command_line();
    BOOL trace = command &&
        process_command_contains(command, "steamwebhelper.exe");
    DWORD pid = win32_current_process_id();
    if (trace && pid < K32_MOJO_TRACE_PID_SLOTS)
        __atomic_store_n(&g_mojo_trace_pids[pid], 1, __ATOMIC_RELAXED);
    return trace;
}

static BOOL k32_mojo_trace_owner(DWORD pid)
{
    return pid < K32_MOJO_TRACE_PID_SLOTS &&
        __atomic_load_n(&g_mojo_trace_pids[pid], __ATOMIC_RELAXED);
}

static void k32_mojo_ring_record(DWORD owner_pid, DWORD owner_tid,
                                 HANDLE file, PVOID overlapped,
                                 DWORD operation, NTSTATUS status,
                                 DWORD requested, DWORD transferred,
                                 PCVOID data, DWORD data_length)
{
    if (!k32_mojo_trace_owner(owner_pid))
        return;

    uint64_t ticket = __atomic_fetch_add(&g_k32_mojo_trace_sequence, 1,
                                         __ATOMIC_RELAXED);
    K32_MOJO_TRACE_RECORD *record =
        &g_k32_mojo_trace_records[ticket % K32_MOJO_RING_RECORDS];
    __atomic_store_n(&record->sequence, 0, __ATOMIC_RELAXED);
    record->owner_pid = owner_pid;
    record->owner_tid = owner_tid;
    record->file = file;
    record->overlapped = overlapped;
    record->operation = operation;
    record->status = status;
    record->requested = requested;
    record->transferred = transferred;
    record->captured = data_length < K32_MOJO_RING_BYTES
        ? data_length : K32_MOJO_RING_BYTES;
    record->reserved = 0;
    for (DWORD i = 0; data && i < record->captured; i++)
        record->data[i] = ((const BYTE *)data)[i];
    __atomic_store_n(&record->sequence, ticket + 1, __ATOMIC_RELEASE);
}

static BOOL k32_mojo_trace_take(volatile uint32_t *counter, uint32_t limit)
{
    return __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED) < limit;
}

static void k32_mojo_trace_bytes(PCVOID data, DWORD length)
{
    const BYTE *bytes = (const BYTE *)data;
    DWORD shown = length < K32_MOJO_TRACE_BYTES ? length : K32_MOJO_TRACE_BYTES;

    for (DWORD i = 0; bytes && i < shown; i++) {
        if (i) serial_puts(":");
        serial_puthex(bytes[i], 2);
    }
    if (length > shown)
        serial_puts(":..");
}

static inline DWORD *last_error_slot(void)
{
    if (g_compat32_mode) {
        TEB32 *teb = compat32_current_teb();
        return teb ? &teb->LastErrorValue : &bootstrap_last_error;
    }

    TEB *teb = win64_current_teb();
    return teb ? &teb->LastErrorValue : &bootstrap_last_error;
}

#define g_last_error (*last_error_slot())

static inline void sync_last_error(void)
{
    /* Direct g_last_error accesses already target the current thread's TEB. */
}

/* ── Helpers ────────────────────────────────────────────────── */

static inline void set_last_error_from_status(NTSTATUS status)
{
    DWORD err = RtlNtStatusToDosError(status);
    g_last_error = err;
    sync_last_error();
    /* Log when we set error 8 (ERROR_NOT_ENOUGH_MEMORY) — diagnostic
     * for tracking the source of UT99's appError / Windows
     * GetLastError loops. Limit to first 8 to avoid spam. */
    if (err == 8) {
        static int log8 = 0;
        if (log8 < 8) {
            extern void serial_puts(const char *s);
            extern void serial_puthex(uint64_t val, int digits);
            extern uint32_t compat32_get_last_caller_eip(void);
            uint32_t ueip = compat32_get_last_caller_eip();
            serial_puts("[ERR8] from NTSTATUS 0x"); serial_puthex(status, 8);
            serial_puts(" userEIP=0x"); serial_puthex(ueip, 8);
            serial_puts("\n");
            log8++;
        }
    }
}

/* Convert ASCII string to UNICODE_STRING (stack-based, temporary) */
static void ascii_to_unicode_buf(const char *src, WCHAR *buf, int max_chars)
{
    int i;
    for (i = 0; src[i] && i < max_chars - 1; i++)
        buf[i] = (WCHAR)(unsigned char)src[i];
    buf[i] = 0;
}

#define K32_OBJECT_NAME_MAX 128
#define K32_MAX_NAMED_OBJECTS 512
#define K32_STEAMIPC_SERIAL_TRACE 0

typedef struct {
    char name[K32_OBJECT_NAME_MAX];
    PVOID object;
    uint64_t identity;
} K32_NAMED_OBJECT;

static spinlock_t named_object_lock = SPINLOCK_INIT;

static inline uint64_t named_object_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&named_object_lock);
    return flags;
}

static inline void named_object_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&named_object_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static char named_object_fold(char c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static BOOL named_object_is_steamchrome(const char *name)
{
    static const char needle[] = "steamchrome";
    if (!name) return FALSE;

    for (int i = 0; name[i]; i++) {
        int j = 0;
        while (needle[j] && name[i + j] &&
               named_object_fold(name[i + j]) == needle[j])
            j++;
        if (!needle[j]) return TRUE;
    }
    return FALSE;
}

static BOOL named_object_contains(const char *name, const char *needle)
{
    if (!name || !needle || !*needle)
        return FALSE;

    for (int i = 0; name[i]; i++) {
        int j = 0;
        while (needle[j] && name[i + j] &&
               named_object_fold(name[i + j]) ==
                   named_object_fold(needle[j]))
            j++;
        if (!needle[j])
            return TRUE;
    }
    return FALSE;
}

static BOOL named_object_is_steamipc(const char *name)
{
    return named_object_is_steamchrome(name) ||
           named_object_contains(name, "steamipc") ||
           named_object_contains(name, "steamservice") ||
           named_object_contains(name, "steamclientservice");
}

static BOOL named_object_canonicalize(const char *source,
                                      char name[K32_OBJECT_NAME_MAX])
{
    static const char base_named_objects[] = "\\basenamedobjects\\";
    const char *canonical = source ? source : "";

    for (const char *p = canonical; *p; p++) {
        int i = 0;
        while (base_named_objects[i] && p[i] &&
               named_object_fold(p[i] == '/' ? '\\' : p[i]) ==
                   base_named_objects[i])
            i++;
        if (!base_named_objects[i]) {
            canonical = p + i;
            break;
        }
    }

    if (named_object_fold(canonical[0]) == 'l' &&
        named_object_fold(canonical[1]) == 'o' &&
        named_object_fold(canonical[2]) == 'c' &&
        named_object_fold(canonical[3]) == 'a' &&
        named_object_fold(canonical[4]) == 'l' &&
        (canonical[5] == '\\' || canonical[5] == '/'))
        canonical += 6;

    int i = 0;
    while (canonical[i] && i < K32_OBJECT_NAME_MAX - 1) {
        char c = canonical[i];
        name[i++] = named_object_fold(c == '/' ? '\\' : c);
    }
    name[i] = 0;
    return !canonical[i];
}

static BOOL named_object_name_a(PCSTR source,
                                char name[K32_OBJECT_NAME_MAX])
{
    return named_object_canonicalize(source, name);
}

static BOOL named_object_name_w(PCWSTR source,
                                char name[K32_OBJECT_NAME_MAX])
{
    char ascii[K32_OBJECT_NAME_MAX * 2];
    int i = 0;
    if (source) {
        while (source[i] && i < (int)sizeof(ascii) - 1) {
            if (source[i] > 0x7f) return FALSE;
            ascii[i] = (char)source[i];
            i++;
        }
        if (source[i]) return FALSE;
    }
    ascii[i] = 0;
    return named_object_canonicalize(ascii, name);
}

static int named_object_find_locked(K32_NAMED_OBJECT *objects, int count,
                                    OBJECT_TYPE_ID type, const char *name)
{
    for (int i = 0; i < count; i++) {
        BOOL alive = FALSE;
        if (objects[i].object && type == OBJ_TYPE_SECTION) {
            uint64_t identity = nt_section_identity(objects[i].object);
            alive = identity && identity == objects[i].identity;
        } else if (objects[i].object) {
            alive = handle_object_referenced(&g_handle_table, type,
                                             objects[i].object);
        }
        if (objects[i].object && !alive) {
            objects[i].object = NULL;
            objects[i].name[0] = 0;
            objects[i].identity = 0;
        }
        if (objects[i].object && k32_strcmp(objects[i].name, name) == 0)
            return i;
    }
    return -1;
}

static HANDLE named_object_open_slot_locked(K32_NAMED_OBJECT *objects,
                                             int slot, OBJECT_TYPE_ID type,
                                             ACCESS_MASK access)
{
    HANDLE handle = NULL;
    NTSTATUS status = handle_alloc(&g_handle_table, type, access,
                                   objects[slot].object, &handle);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return NULL;
    }
    if (type == OBJ_TYPE_SECTION &&
        !nt_section_reopen_handle(objects[slot].object)) {
        handle_close(&g_handle_table, handle);
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return NULL;
    }
    return handle;
}

static HANDLE named_object_open(K32_NAMED_OBJECT *objects, int count,
                                OBJECT_TYPE_ID type, const char *name,
                                ACCESS_MASK access)
{
    uint64_t flags = named_object_lock_irqsave();
    int slot = named_object_find_locked(objects, count, type, name);
    HANDLE handle = NULL;
    if (slot >= 0)
        handle = named_object_open_slot_locked(objects, slot, type, access);
    else
        SetLastError(2); /* ERROR_FILE_NOT_FOUND */
    named_object_unlock_irqrestore(flags);
    return handle;
}

static HANDLE named_object_publish_access(K32_NAMED_OBJECT *objects, int count,
                                          OBJECT_TYPE_ID type,
                                          const char *name, HANDLE created,
                                          ACCESS_MASK existing_access,
                                          BOOL *already_exists)
{
    if (already_exists)
        *already_exists = FALSE;

    uint64_t flags = named_object_lock_irqsave();
    int existing = named_object_find_locked(objects, count, type, name);
    if (existing >= 0) {
        HANDLE handle = named_object_open_slot_locked(
            objects, existing, type, existing_access);
        if (handle && already_exists)
            *already_exists = TRUE;
        named_object_unlock_irqrestore(flags);
        return handle;
    }

    int slot;
    for (slot = 0; slot < count; slot++)
        if (!objects[slot].object) break;
    PVOID object = NULL;
    if (slot == count ||
        !NT_SUCCESS(handle_lookup(&g_handle_table, created, type, &object))) {
        named_object_unlock_irqrestore(flags);
        return NULL;
    }

    uint64_t identity = type == OBJ_TYPE_SECTION
                        ? nt_section_identity(object) : 0;
    if (type == OBJ_TYPE_SECTION && !identity) {
        named_object_unlock_irqrestore(flags);
        return NULL;
    }

    int i = 0;
    while (name[i]) { objects[slot].name[i] = name[i]; i++; }
    objects[slot].name[i] = 0;
    objects[slot].object = object;
    objects[slot].identity = identity;
    named_object_unlock_irqrestore(flags);
    return created;
}

static HANDLE named_object_publish(K32_NAMED_OBJECT *objects, int count,
                                   OBJECT_TYPE_ID type, const char *name,
                                   HANDLE created, BOOL *already_exists)
{
    return named_object_publish_access(objects, count, type, name, created,
                                       GENERIC_ALL, already_exists);
}

static const char *named_object_name_for_handle(K32_NAMED_OBJECT *objects,
                                                 int count,
                                                 OBJECT_TYPE_ID type,
                                                 HANDLE handle)
{
    uint64_t flags = named_object_lock_irqsave();
    PVOID object = NULL;
    if (!NT_SUCCESS(handle_lookup(&g_handle_table, handle, type, &object))) {
        named_object_unlock_irqrestore(flags);
        return NULL;
    }
    uint64_t identity = type == OBJ_TYPE_SECTION
                        ? nt_section_identity(object) : 0;
    const char *name = NULL;
    for (int i = 0; i < count; i++) {
        if (objects[i].object == object &&
            (type != OBJ_TYPE_SECTION || objects[i].identity == identity)) {
            name = objects[i].name;
            break;
        }
    }
    named_object_unlock_irqrestore(flags);
    if (name)
        return name;
    return NULL;
}

static const char *steamipc_name_for_handle(HANDLE handle);
static void steamipc_trace_handle(const char *operation, HANDLE handle,
                                  uint32_t argument, uint32_t result,
                                  uint64_t caller);

static char win32_current_directory[260] = "System";

const char *kernel32_current_directory_relative(void)
{
    const char *child_directory = win32_current_directory_override();
    return child_directory ? child_directory : win32_current_directory;
}

static void win32_reset_current_directory(void)
{
    extern char win32_exe_name[64];
    const char *last_sep = NULL;
    for (const char *p = win32_exe_name; *p; p++)
        if (*p == '\\' || *p == '/') last_sep = p;

    int n = 0;
    const char *root = "System";
    while (root[n]) { win32_current_directory[n] = root[n]; n++; }
    if (last_sep) {
        win32_current_directory[n++] = '\\';
        for (const char *p = win32_exe_name; p < last_sep && n < 259; p++)
            win32_current_directory[n++] = *p == '/' ? '\\' : *p;
    }
    win32_current_directory[n] = 0;
}

bool win32_normalize_path(PCSTR path, char out[260])
{
    if (!path || !out) return false;
    const char *p = path;
    if (p[0] == '\\' && p[1] == '?' && p[2] == '?' && p[3] == '\\')
        p += 4;
    else if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\')
        p += 4;

    bool absolute = false;
    if (p[0] && p[1] == ':') {
        absolute = p[2] == '\\' || p[2] == '/';
        p += 2;
    } else if (*p == '\\' || *p == '/') {
        absolute = true;
    }
    while (*p == '\\' || *p == '/') p++;

    char joined[512];
    int joined_len = 0;
    if (!absolute) {
        const char *current_directory =
            kernel32_current_directory_relative();
        for (int i = 0; current_directory[i]; i++)
            joined[joined_len++] = current_directory[i];
        if (*p && joined_len) joined[joined_len++] = '\\';
    }
    while (*p && joined_len < (int)sizeof(joined) - 1)
        joined[joined_len++] = *p++;
    if (*p) return false;
    joined[joined_len] = 0;

    int component_start[64];
    int components = 0;
    int write = 0;
    p = joined;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '\\' && *p != '/') p++;
        int len = (int)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (components) write = component_start[--components];
            continue;
        }
        if (components >= 64 || write + len + (write != 0) >= 260)
            return false;
        component_start[components++] = write;
        if (write) out[write++] = '\\';
        for (int i = 0; i < len; i++) out[write++] = start[i];
    }
    out[write] = 0;
    return true;
}

/* OsitoFS stores files by full path but has no directory records. Keep empty
 * directories in the Win32 namespace so Chromium can atomically rebuild cache
 * trees. This table is intentionally process-global and survives shim re-init. */
#define K32_MAX_VIRTUAL_DIRS 2048
#define K32_VIRTUAL_DIR_PATH 260

typedef void osfs2_file_t;
extern osfs2_file_t *osfs2_get_file(int index);
extern osfs2_file_t *osfs2_find_exact_ci(const char *name);
extern const char *osfs2_file_name(void *file);
extern uint32_t osfs2_max_files(void);
extern bool osfs2_directory_exists_ci(const char *directory);
extern bool osfs3_is_mounted(void);
extern int osfs3_mkdir(const char *path);
extern int osfs3_rmdir(const char *path);

typedef struct {
    bool in_use;
    char path[K32_VIRTUAL_DIR_PATH];
} K32_VIRTUAL_DIRECTORY;

static K32_VIRTUAL_DIRECTORY k32_virtual_directories[K32_MAX_VIRTUAL_DIRS];
static spinlock_t k32_virtual_directory_lock = SPINLOCK_INIT;

static inline uint64_t k32_directory_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&k32_virtual_directory_lock);
    return flags;
}

static inline void k32_directory_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&k32_virtual_directory_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static char k32_path_fold(char c)
{
    if (c == '/') return '\\';
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static bool k32_path_equal_ci(const char *a, const char *b)
{
    while (*a && *b && k32_path_fold(*a) == k32_path_fold(*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

static bool k32_path_contains_ci(const char *path, const char *needle)
{
    if (!path || !needle || !*needle) return false;
    for (; *path; path++) {
        const char *a = path;
        const char *b = needle;
        while (*a && *b && k32_path_fold(*a) == k32_path_fold(*b)) {
            a++;
            b++;
        }
        if (!*b) return true;
    }
    return false;
}

static bool k32_is_profile_path(const char *path)
{
    return k32_path_contains_ci(path, "htmlcache");
}

static bool k32_path_is_descendant(const char *directory, const char *path)
{
    while (*directory && *path &&
           k32_path_fold(*directory) == k32_path_fold(*path)) {
        directory++;
        path++;
    }
    return !*directory && (*path == '\\' || *path == '/');
}

static bool k32_path_is_self_or_descendant(const char *directory,
                                            const char *path)
{
    return k32_path_equal_ci(directory, path) ||
           k32_path_is_descendant(directory, path);
}

static int k32_virtual_directory_find_locked(const char *path)
{
    for (int i = 0; i < K32_MAX_VIRTUAL_DIRS; i++) {
        if (k32_virtual_directories[i].in_use &&
            k32_path_equal_ci(k32_virtual_directories[i].path, path))
            return i;
    }
    return -1;
}

static bool k32_virtual_directory_exists(const char *path)
{
    uint64_t flags = k32_directory_lock_irqsave();
    bool exists = k32_virtual_directory_find_locked(path) >= 0;
    k32_directory_unlock_irqrestore(flags);
    return exists;
}

static bool k32_virtual_directory_copy(int index,
                                       char path[K32_VIRTUAL_DIR_PATH])
{
    if (index < 0 || index >= K32_MAX_VIRTUAL_DIRS) return false;
    uint64_t flags = k32_directory_lock_irqsave();
    bool exists = k32_virtual_directories[index].in_use;
    if (exists)
        memcpy(path, k32_virtual_directories[index].path,
               K32_VIRTUAL_DIR_PATH);
    k32_directory_unlock_irqrestore(flags);
    return exists;
}

static bool k32_virtual_directory_insert(const char *path)
{
    if (!path || !*path || strlen(path) >= K32_VIRTUAL_DIR_PATH)
        return false;

    uint64_t flags = k32_directory_lock_irqsave();
    if (k32_virtual_directory_find_locked(path) >= 0) {
        k32_directory_unlock_irqrestore(flags);
        return true;
    }

    int free_slot = -1;
    for (int i = 0; i < K32_MAX_VIRTUAL_DIRS; i++) {
        if (!k32_virtual_directories[i].in_use) {
            free_slot = i;
            break;
        }
    }
    if (free_slot >= 0) {
        strcpy(k32_virtual_directories[free_slot].path, path);
        k32_virtual_directories[free_slot].in_use = true;
    }
    k32_directory_unlock_irqrestore(flags);
    return free_slot >= 0;
}

static bool k32_virtual_directory_remove(const char *path)
{
    uint64_t flags = k32_directory_lock_irqsave();
    int index = k32_virtual_directory_find_locked(path);
    if (index >= 0) {
        k32_virtual_directories[index].in_use = false;
        k32_virtual_directories[index].path[0] = 0;
    }
    k32_directory_unlock_irqrestore(flags);
    return index >= 0;
}

bool win32_directory_exists_normalized(const char *path)
{
    return !*path || k32_virtual_directory_exists(path) ||
           osfs2_directory_exists_ci(path);
}

static osfs2_file_t *k32_find_file_exact_ci(const char *path)
{
    return osfs2_find_exact_ci(path);
}

static bool k32_directory_has_file_children(const char *path)
{
    if (osfs3_is_mounted())
        return osfs3_directory_has_children_ci(path);
    uint32_t limit = osfs2_max_files();
    for (uint32_t i = 0; i < limit; i++) {
        osfs2_file_t *file = osfs2_get_file((int)i);
        if (file && k32_path_is_descendant(path, osfs2_file_name(file)))
            return true;
    }
    return false;
}

static bool k32_directory_has_virtual_children(const char *path)
{
    uint64_t flags = k32_directory_lock_irqsave();
    bool found = false;
    for (int i = 0; i < K32_MAX_VIRTUAL_DIRS; i++) {
        if (k32_virtual_directories[i].in_use &&
            k32_path_is_descendant(path, k32_virtual_directories[i].path)) {
            found = true;
            break;
        }
    }
    k32_directory_unlock_irqrestore(flags);
    return found;
}

static bool k32_directory_parent_exists(const char *path)
{
    char parent[K32_VIRTUAL_DIR_PATH];
    size_t length = strlen(path);
    if (length >= sizeof(parent)) return false;
    memcpy(parent, path, length + 1);
    while (length && parent[length - 1] != '\\' && parent[length - 1] != '/')
        length--;
    if (!length) return true;
    parent[length - 1] = 0;
    return win32_directory_exists_normalized(parent);
}

DWORD win32_directory_create_normalized(const char *path)
{
    if (!path) return 87; /* ERROR_INVALID_PARAMETER */
    if (strlen(path) >= K32_VIRTUAL_DIR_PATH) return 206;
    if (!*path || k32_find_file_exact_ci(path) ||
        win32_directory_exists_normalized(path))
        return 183; /* ERROR_ALREADY_EXISTS */
    if (!k32_directory_parent_exists(path))
        return 3; /* ERROR_PATH_NOT_FOUND */
    if (osfs3_is_mounted()) {
        int result = osfs3_mkdir(path);
        if (result == -2) return 183; /* ERROR_ALREADY_EXISTS */
        if (result == -3) return 3;   /* ERROR_PATH_NOT_FOUND */
        if (result < 0) return 5;     /* ERROR_ACCESS_DENIED */
        /* Keep it in the enumeration table while it is empty. */
        (void)k32_virtual_directory_insert(path);
        return 0;
    }
    if (!k32_virtual_directory_insert(path))
        return 8; /* ERROR_NOT_ENOUGH_MEMORY */
    return 0;
}

static bool k32_virtual_directory_move_preflight(const char *old_path,
                                                  const char *new_path)
{
    uint64_t flags = k32_directory_lock_irqsave();
    bool ok = true;
    size_t old_length = strlen(old_path);
    size_t new_length = strlen(new_path);
    for (int i = 0; i < K32_MAX_VIRTUAL_DIRS && ok; i++) {
        const char *path = k32_virtual_directories[i].path;
        if (!k32_virtual_directories[i].in_use ||
            !k32_path_is_self_or_descendant(old_path, path))
            continue;
        size_t suffix_length = strlen(path + old_length);
        if (new_length + suffix_length >= K32_VIRTUAL_DIR_PATH)
            ok = false;
    }
    k32_directory_unlock_irqrestore(flags);
    return ok;
}

static void k32_virtual_directory_move_commit(const char *old_path,
                                               const char *new_path)
{
    uint64_t flags = k32_directory_lock_irqsave();
    size_t old_length = strlen(old_path);
    for (int i = 0; i < K32_MAX_VIRTUAL_DIRS; i++) {
        K32_VIRTUAL_DIRECTORY *directory = &k32_virtual_directories[i];
        if (!directory->in_use ||
            !k32_path_is_self_or_descendant(old_path, directory->path))
            continue;
        char moved[K32_VIRTUAL_DIR_PATH];
        size_t new_length = strlen(new_path);
        size_t suffix_length = strlen(directory->path + old_length);
        memcpy(moved, new_path, new_length);
        memcpy(moved + new_length, directory->path + old_length,
               suffix_length + 1);
        memcpy(directory->path, moved, new_length + suffix_length + 1);
    }
    k32_directory_unlock_irqrestore(flags);
}

/* Win32 creation disposition → NT create disposition */
static ULONG win32_to_nt_disposition(DWORD dwCreationDisposition)
{
    switch (dwCreationDisposition) {
    case 1: /* CREATE_NEW */        return FILE_CREATE;
    case 2: /* CREATE_ALWAYS */     return FILE_OVERWRITE_IF;
    case 3: /* OPEN_EXISTING */     return FILE_OPEN;
    case 4: /* OPEN_ALWAYS */       return FILE_OPEN_IF;
    case 5: /* TRUNCATE_EXISTING */ return FILE_OVERWRITE;
    default:                        return FILE_OPEN;
    }
}

/* ── Console handles ────────────────────────────────────────── */

/* Map Win32 pseudo-handles (-10,-11,-12) to NT handles (4,8,12) */
static HANDLE console_handle(DWORD nStdHandle)
{
    switch (nStdHandle) {
    case WIN32_STD_INPUT_HANDLE:  return (HANDLE)(ULONG_PTR)4;
    case WIN32_STD_OUTPUT_HANDLE: return (HANDLE)(ULONG_PTR)8;
    case WIN32_STD_ERROR_HANDLE:  return (HANDLE)(ULONG_PTR)12;
    default:                      return INVALID_HANDLE_VALUE;
    }
}

/* ── File API ───────────────────────────────────────────────── */

#define K32_MAX_PENDING_NAMED_PIPES 64

typedef struct {
    BOOL used;
    char name[260];
    HANDLE server;
    HANDLE client;
} K32_PENDING_NAMED_PIPE;

static K32_PENDING_NAMED_PIPE g_pending_named_pipes[K32_MAX_PENDING_NAMED_PIPES];

static BOOL named_pipe_name_equal(const char *a, const char *b)
{
    if (!a || !b)
        return FALSE;
    while (*a && *b) {
        if (named_object_fold(*a++) != named_object_fold(*b++))
            return FALSE;
    }
    return *a == *b;
}

static HANDLE named_pipe_take_client(const char *name)
{
    for (int i = 0; i < K32_MAX_PENDING_NAMED_PIPES; i++) {
        K32_PENDING_NAMED_PIPE *pipe = &g_pending_named_pipes[i];
        if (!pipe->used || !named_pipe_name_equal(pipe->name, name))
            continue;
        HANDLE client = pipe->client;
        memset(pipe, 0, sizeof(*pipe));
        return client;
    }
    return INVALID_HANDLE_VALUE;
}

static BOOL named_pipe_has_pending(const char *name)
{
    for (int i = 0; i < K32_MAX_PENDING_NAMED_PIPES; i++) {
        K32_PENDING_NAMED_PIPE *pipe = &g_pending_named_pipes[i];
        if (pipe->used && named_pipe_name_equal(pipe->name, name))
            return TRUE;
    }
    return FALSE;
}

static HANDLE named_pipe_cancel_pending_server(HANDLE server)
{
    for (int i = 0; i < K32_MAX_PENDING_NAMED_PIPES; i++) {
        K32_PENDING_NAMED_PIPE *pipe = &g_pending_named_pipes[i];
        if (!pipe->used || pipe->server != server)
            continue;
        HANDLE client = pipe->client;
        memset(pipe, 0, sizeof(*pipe));
        return client;
    }
    return INVALID_HANDLE_VALUE;
}

#define K32_FILE_FLAG_BACKUP_SEMANTICS 0x02000000U

HANDLE WINAPI CreateFileA(PCSTR lpFileName, DWORD dwDesiredAccess,
                   DWORD dwShareMode, PVOID lpSecurityAttributes,
                   DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                   HANDLE hTemplateFile)
{
    (void)lpSecurityAttributes;
    (void)hTemplateFile;

    serial_puts("[CreateFileA] '");
    if (lpFileName) serial_puts(lpFileName);
    serial_puts("'\n");

    HANDLE pipe_client = named_pipe_take_client(lpFileName);
    if (pipe_client != INVALID_HANDLE_VALUE) {
        serial_puts("[K32-PIPE] connected client=0x");
        serial_puthex((uint64_t)(ULONG_PTR)pipe_client, 8);
        serial_puts("\n");
        SetLastError(0);
        return pipe_client;
    }

    /* Build NT path from Win32 path */
    char fs_path[260];
    if (!win32_normalize_path(lpFileName, fs_path)) {
        SetLastError(lpFileName ? 206 : 87);
        return INVALID_HANDLE_VALUE;
    }
    WCHAR name_buf[260];
    ascii_to_unicode_buf(fs_path, name_buf, 260);

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, name_buf);

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    IO_STATUS_BLOCK iosb = {0};
    HANDLE file_handle = INVALID_HANDLE_VALUE;

    /* Map Win32 access to NT access */
    ACCESS_MASK nt_access = 0;
    if (dwDesiredAccess & GENERIC_READ)  nt_access |= FILE_GENERIC_READ;
    if (dwDesiredAccess & GENERIC_WRITE) nt_access |= FILE_GENERIC_WRITE;
    nt_access |= SYNCHRONIZE;

    ULONG create_options = FILE_SYNCHRONOUS_IO_NONALERT |
        (((dwFlagsAndAttributes & K32_FILE_FLAG_BACKUP_SEMANTICS) &&
          win32_directory_exists_normalized(fs_path))
             ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
    NTSTATUS status = NtCreateFile(
        &file_handle,
        nt_access,
        &oa,
        &iosb,
        NULL,                                   /* AllocationSize */
        FILE_ATTRIBUTE_NORMAL,                  /* FileAttributes */
        dwShareMode,                            /* ShareAccess */
        win32_to_nt_disposition(dwCreationDisposition),
        create_options,
        NULL,                                   /* EaBuffer */
        0                                       /* EaLength */
    );

    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return INVALID_HANDLE_VALUE;
    }

    return file_handle;
}

HANDLE WINAPI CreateFileW(PCWSTR lpFileName, DWORD dwDesiredAccess,
                   DWORD dwShareMode, PVOID lpSecurityAttributes,
                   DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                   HANDLE hTemplateFile)
{
    static uint32_t profile_open_logs;
    (void)lpSecurityAttributes;
    (void)hTemplateFile;

    char fname_ascii[260];
    int fname_len = 0;
    if (lpFileName) {
        for (int i = 0; i < 259 && lpFileName[i]; i++) {
            char c = (char)(lpFileName[i] & 0xFF);
            fname_ascii[i] = c;
            fname_len = i + 1;
        }
        fname_ascii[fname_len] = 0;
    } else {
        fname_ascii[0] = 0;
    }

    HANDLE pipe_client = named_pipe_take_client(fname_ascii);
    if (pipe_client != INVALID_HANDLE_VALUE) {
        serial_puts("[K32-PIPE] connected client=0x");
        serial_puthex((uint64_t)(ULONG_PTR)pipe_client, 8);
        serial_puts("\n");
        SetLastError(0);
        return pipe_client;
    }
#ifndef OK_QUIET
    serial_puts("[CreateFileW] ptr=0x");
    serial_puthex((uint64_t)lpFileName, 16);
    serial_puts(" '");
    serial_puts(fname_ascii);
    serial_puts("'\n");
#endif

    char fs_path[260];
    if (!win32_normalize_path(lpFileName ? fname_ascii : NULL, fs_path)) {
        SetLastError(lpFileName ? 206 : 87);
        return INVALID_HANDLE_VALUE;
    }

    /* Block writes to .ini files. The engine's shutdown writes a PARTIAL
     * config (only modified sections) to UnrealTournament.ini, destroying
     * the complete Default.ini we placed on the NVMe. Also block Running.ini. */
    if (dwDesiredAccess & GENERIC_WRITE) {
        const char *fn = fname_ascii;
        for (int i = fname_len - 1; i >= 0; i--)
            if (fn[i] == '\\' || fn[i] == '/') { fn = &fname_ascii[i+1]; break; }
        int flen = 0;
        while (fn[flen]) flen++;
        if (flen > 4 &&
            fn[flen-4] == '.' &&
            (fn[flen-3]=='i'||fn[flen-3]=='I') &&
            (fn[flen-2]=='n'||fn[flen-2]=='N') &&
            (fn[flen-1]=='i'||fn[flen-1]=='I')) {
            serial_puts("[CreateFileW] BLOCKED .ini write: ");
            serial_puts(fn);
            serial_puts("\n");
            SetLastError(5); /* ERROR_ACCESS_DENIED */
            return INVALID_HANDLE_VALUE;
        }
    }

    WCHAR name_buf[260];
    ascii_to_unicode_buf(fs_path, name_buf, 260);
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, name_buf);

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    IO_STATUS_BLOCK iosb = {0};
    HANDLE file_handle = INVALID_HANDLE_VALUE;

    ACCESS_MASK nt_access = 0;
    if (dwDesiredAccess & GENERIC_READ)  nt_access |= FILE_GENERIC_READ;
    if (dwDesiredAccess & GENERIC_WRITE) nt_access |= FILE_GENERIC_WRITE;
    nt_access |= SYNCHRONIZE;

    bool directory_open =
        (dwFlagsAndAttributes & K32_FILE_FLAG_BACKUP_SEMANTICS) &&
        win32_directory_exists_normalized(fs_path);
    ULONG create_options = FILE_SYNCHRONOUS_IO_NONALERT |
        (directory_open ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
    bool trace_profile = false;
    if (K32_VERBOSE_DIAGNOSTICS && k32_is_profile_path(fs_path)) {
        uint32_t trace_index = __atomic_fetch_add(&profile_open_logs, 1,
                                                   __ATOMIC_RELAXED);
        trace_profile = trace_index < 512;
        if (trace_profile) {
            serial_puts("[K32-PROFILE] open '");
            serial_puts(fs_path);
            serial_puts("' access=");
            serial_puthex(dwDesiredAccess, 8);
            serial_puts(" share=");
            serial_puthex(dwShareMode, 8);
            serial_puts(" disp=");
            serial_puthex(dwCreationDisposition, 8);
            serial_puts(" flags=");
            serial_puthex(dwFlagsAndAttributes, 8);
            serial_puts(" dir=");
            serial_putdec(directory_open ? 1 : 0);
            serial_puts("\n");
        }
    }
    NTSTATUS status = NtCreateFile(
        &file_handle, nt_access, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, dwShareMode,
        win32_to_nt_disposition(dwCreationDisposition),
        create_options,
        NULL, 0
    );

    if (trace_profile) {
        serial_puts("[K32-PROFILE] open-result status=");
        serial_puthex((uint32_t)status, 8);
        serial_puts(" handle=");
        serial_puthex((uint64_t)(ULONG_PTR)file_handle, 8);
        serial_puts(" info=");
        serial_puthex((uint64_t)iosb.Information, 8);
        serial_puts("\n");
    }

    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return INVALID_HANDLE_VALUE;
    }

    return file_handle;
}

BOOL WINAPI ReadFile(HANDLE hFile, PVOID lpBuffer, DWORD nNumberOfBytesToRead,
              DWORD *lpNumberOfBytesRead, PVOID lpOverlapped)
{
    static volatile uint32_t regular_read_trace_count;
    FILE_OBJECT *file_object = NULL;
    BOOL pipe_read =
        NT_SUCCESS(handle_lookup(&g_handle_table, hFile, OBJ_TYPE_FILE,
                                 (PVOID *)&file_object)) &&
        (file_object->flags & FILE_OBJ_PIPE_READ);
    BOOL mojo_process = pipe_read && k32_mojo_trace_current();
    BOOL trace_mojo = mojo_process &&
        k32_mojo_trace_take(&g_mojo_read_trace_count,
                            K32_MOJO_IO_TRACE_LIMIT);
    BOOL trace_regular = !pipe_read && nNumberOfBytesToRead != 1 &&
        __sync_fetch_and_add(&regular_read_trace_count, 1) < 8;
    if (trace_regular) {
        serial_puts("[K32-READ] handle=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hFile, 8);
        serial_puts(" len=");
        serial_putdec(nNumberOfBytesToRead);
        serial_puts(" overlapped=0x");
        serial_puthex((uint64_t)(ULONG_PTR)lpOverlapped, 16);
        serial_puts("\n");
    }
    if (trace_mojo) {
        serial_puts("[MOJO-IO] ReadFile enter pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" handle=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hFile, 8);
        serial_puts(" len=");
        serial_putdec(nNumberOfBytesToRead);
        serial_puts(" overlapped=0x");
        serial_puthex((uint64_t)(ULONG_PTR)lpOverlapped, 16);
        serial_puts("\n");
    }
    if (lpOverlapped && pipe_read) {
        BOOL result = k32_pipe_read_overlapped(hFile, lpBuffer,
                                               nNumberOfBytesToRead,
                                               lpNumberOfBytesRead,
                                               lpOverlapped);
        if (trace_mojo) {
            DWORD error = GetLastError();
            serial_puts("[MOJO-IO] ReadFile overlapped result=");
            serial_putdec(result);
            serial_puts(" error=");
            serial_putdec(error);
            serial_puts("\n");
            SetLastError(error);
        }
        return result;
    }

    LARGE_INTEGER byte_offset;
    PLARGE_INTEGER byte_offset_ptr = NULL;
    if (lpOverlapped) {
        const volatile uint32_t *fields =
            (const volatile uint32_t *)lpOverlapped;
        if (g_compat32_mode) {
            byte_offset.LowPart = fields[2];
            byte_offset.HighPart = (LONG)fields[3];
        } else {
            const volatile uint32_t *offset_fields =
                (const volatile uint32_t *)((const BYTE *)lpOverlapped + 16);
            byte_offset.LowPart = offset_fields[0];
            byte_offset.HighPart = (LONG)offset_fields[1];
        }
        byte_offset_ptr = &byte_offset;
    }

    IO_STATUS_BLOCK iosb;
    NTSTATUS status = NtReadFile(hFile, NULL, NULL, NULL, &iosb,
                                 lpBuffer, nNumberOfBytesToRead,
                                 byte_offset_ptr, NULL);

    if (NT_SUCCESS(status)) {
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = (DWORD)iosb.Information;
        if (mojo_process)
            k32_mojo_ring_record(win32_current_process_id(),
                                 GetCurrentThreadId(), hFile, lpOverlapped,
                                 K32_MOJO_RING_READ, status,
                                 nNumberOfBytesToRead,
                                 (DWORD)iosb.Information, lpBuffer,
                                 (DWORD)iosb.Information);
        if (pipe_read)
            k32_pipe_service_pending();
        if (lpOverlapped)
            k32_iocp_complete_handle(hFile, (DWORD)iosb.Information,
                                     lpOverlapped);
        if (trace_regular) {
            serial_puts("[K32-READ] status=0x");
            serial_puthex((uint32_t)status, 8);
            serial_puts(" bytes=");
            serial_putdec((DWORD)iosb.Information);
            serial_puts("\n");
        }
        return TRUE;
    }

    /* EOF is not an error in Win32 — just 0 bytes read */
    if (status == STATUS_END_OF_FILE) {
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = 0;
        if (lpOverlapped)
            k32_iocp_complete_handle(hFile, 0, lpOverlapped);
        if (trace_regular)
            serial_puts("[K32-READ] EOF bytes=0\n");
        return TRUE;
    }

    if (lpOverlapped)
        k32_store_overlapped_status(lpOverlapped, g_compat32_mode,
                                    status, 0);
    set_last_error_from_status(status);
    if (trace_regular) {
        serial_puts("[K32-READ] failed status=0x");
        serial_puthex((uint32_t)status, 8);
        serial_puts(" error=");
        serial_putdec(GetLastError());
        serial_puts("\n");
    }
    return FALSE;
}

BOOL WINAPI WriteFile(HANDLE hFile, PCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
               DWORD *lpNumberOfBytesWritten, PVOID lpOverlapped)
{
    FILE_OBJECT *file_object = NULL;
    BOOL pipe_write =
        NT_SUCCESS(handle_lookup(&g_handle_table, hFile, OBJ_TYPE_FILE,
                                 (PVOID *)&file_object)) &&
        (file_object->flags & FILE_OBJ_PIPE_WRITE);
    BOOL mojo_process = pipe_write && k32_mojo_trace_current();
    BOOL trace_mojo = mojo_process &&
        k32_mojo_trace_take(&g_mojo_write_trace_count,
                            K32_MOJO_IO_TRACE_LIMIT);
    if (trace_mojo) {
        serial_puts("[MOJO-IO] WriteFile enter pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" handle=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hFile, 8);
        serial_puts(" len=");
        serial_putdec(nNumberOfBytesToWrite);
        serial_puts(" overlapped=0x");
        serial_puthex((uint64_t)(ULONG_PTR)lpOverlapped, 16);
        serial_puts(" data=");
        k32_mojo_trace_bytes(lpBuffer, nNumberOfBytesToWrite);
        serial_puts("\n");
    }
    if (lpOverlapped && pipe_write) {
        BOOL result = k32_pipe_write_overlapped(hFile, lpBuffer,
                                                nNumberOfBytesToWrite,
                                                lpNumberOfBytesWritten,
                                                lpOverlapped);
        if (trace_mojo) {
            DWORD error = GetLastError();
            serial_puts("[MOJO-IO] WriteFile overlapped result=");
            serial_putdec(result);
            serial_puts(" error=");
            serial_putdec(error);
            serial_puts("\n");
            SetLastError(error);
        }
        return result;
    }
    LARGE_INTEGER byte_offset;
    PLARGE_INTEGER byte_offset_ptr = NULL;
    if (lpOverlapped && !pipe_write) {
        const volatile uint32_t *fields =
            (const volatile uint32_t *)lpOverlapped;
        if (g_compat32_mode) {
            byte_offset.LowPart = fields[2];
            byte_offset.HighPart = (LONG)fields[3];
        } else {
            const volatile uint32_t *offset_fields =
                (const volatile uint32_t *)((const BYTE *)lpOverlapped + 16);
            byte_offset.LowPart = offset_fields[0];
            byte_offset.HighPart = (LONG)offset_fields[1];
        }
        byte_offset_ptr = &byte_offset;
    }
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = NtWriteFile(hFile, NULL, NULL, NULL, &iosb,
                                  (PVOID)lpBuffer, nNumberOfBytesToWrite,
                                  byte_offset_ptr, NULL);

    if (NT_SUCCESS(status)) {
        if (lpNumberOfBytesWritten)
            *lpNumberOfBytesWritten = (DWORD)iosb.Information;
        if (mojo_process)
            k32_mojo_ring_record(win32_current_process_id(),
                                 GetCurrentThreadId(), hFile, lpOverlapped,
                                 K32_MOJO_RING_WRITE, status,
                                 nNumberOfBytesToWrite,
                                 (DWORD)iosb.Information, lpBuffer,
                                 nNumberOfBytesToWrite);
        if (pipe_write)
            k32_pipe_service_pending();
        if (lpOverlapped)
            k32_iocp_complete_handle(hFile, (DWORD)iosb.Information,
                                     lpOverlapped);
        return TRUE;
    }

    serial_puts("[K32] WriteFile FAILED handle=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hFile, 8);
    serial_puts(" len=0x");
    serial_puthex(nNumberOfBytesToWrite, 8);
    serial_puts(" status=0x");
    serial_puthex((uint32_t)status, 8);
    serial_puts("\n");
    if (lpOverlapped)
        k32_store_overlapped_status(lpOverlapped, g_compat32_mode,
                                    status, 0);
    set_last_error_from_status(status);
    return FALSE;
}

static BOOL k32_file_ex(HANDLE file, PVOID buffer, DWORD length,
                        PVOID overlapped, PVOID completion_routine,
                        BOOL write)
{
    if (!overlapped || !completion_routine || (!buffer && length)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD generation = 0;
    int slot = k32_io_completion_reserve(file, overlapped,
                                         completion_routine,
                                         g_compat32_mode, &generation);
    if (slot < 0) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    DWORD transferred = 0;
    BOOL result = write
        ? WriteFile(file, buffer, length, &transferred, overlapped)
        : ReadFile(file, buffer, length, &transferred, overlapped);
    DWORD error = GetLastError();

    if (result) {
        k32_io_completion_set_ready(slot, generation, STATUS_SUCCESS,
                                    transferred);
    } else if (error != 997) { /* ERROR_IO_PENDING */
        k32_io_completion_cancel(slot, generation);
        SetLastError(error);
        return FALSE;
    }

    /* WriteFileEx/ReadFileEx report successful queueing even when the
     * underlying operation is still pending. The callback runs only when
     * this thread enters an alertable wait. */
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI ReadFileEx_k32(HANDLE file, PVOID buffer, DWORD length,
                                   PVOID overlapped,
                                   PVOID completion_routine)
{
    return k32_file_ex(file, buffer, length, overlapped,
                       completion_routine, FALSE);
}

static BOOL WINAPI WriteFileEx_k32(HANDLE file, PCVOID buffer, DWORD length,
                                    PVOID overlapped,
                                    PVOID completion_routine)
{
    return k32_file_ex(file, (PVOID)buffer, length, overlapped,
                       completion_routine, TRUE);
}

BOOL WINAPI CloseHandle(HANDLE hObject)
{
    K32_JOB_OBJECT *job = NULL;
    HANDLE_ENTRY *entry = handle_get_entry(&g_handle_table, hObject);
    if (entry && entry->type == OBJ_TYPE_JOB)
        job = (K32_JOB_OBJECT *)entry->object;

    steamipc_trace_handle("Close", hObject, 0, 0,
                          (uint64_t)(ULONG_PTR)__builtin_return_address(0));

    /* NtClose may release the pipe object while another CPU is servicing an
     * overlapped request. Cancel first and wait until no service routine still
     * owns the handle or its private write buffer. */
    if (entry && entry->type == OBJ_TYPE_FILE) {
        (void)k32_cancel_pipe_io(hObject, NULL, FALSE);
        k32_pipe_wait_quiescent(hObject);
    }

    NTSTATUS status = NtClose(hObject);
    if (!NT_SUCCESS(status)) {
        DWORD current_pid = win32_current_process_id();
        OBJECT_TYPE_ID type;
        ULONG refs;
        ULONG owner_refs;
        ULONG sole_owner;
        BOOL valid = handle_query_state(&g_handle_table, hObject, current_pid,
                                        &type, &refs, &owner_refs,
                                        &sole_owner);
        serial_puts("[K32-CLOSE-FAIL] handle=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hObject, 16);
        serial_puts(" status=0x");
        serial_puthex((uint32_t)status, 8);
        serial_puts(" pid=");
        serial_putdec(current_pid);
        serial_puts(" valid=");
        serial_putdec(valid);
        serial_puts(" type=");
        serial_putdec(type);
        serial_puts(" refs=");
        serial_putdec(refs);
        serial_puts(" owner_refs=");
        serial_putdec(owner_refs);
        serial_puts(" sole_owner=");
        serial_putdec(sole_owner);
        serial_puts(" caller=0x");
        serial_puthex((uint64_t)__builtin_return_address(0), 16);
        serial_puts("\n");
        set_last_error_from_status(status);
        return FALSE;
    }
    if (job &&
        !handle_object_referenced(&g_handle_table, OBJ_TYPE_JOB, job))
        job_release(job, TRUE);
    HANDLE pending_client = named_pipe_cancel_pending_server(hObject);
    if (pending_client != INVALID_HANDLE_VALUE)
        NtClose(pending_client);
    iocp_forget_handle(hObject);
    return TRUE;
}

/* -- Communications resources ------------------------------------------- */

typedef struct {
    DWORD DCBlength;
    DWORD BaudRate;
    DWORD Flags;
    WORD  wReserved;
    WORD  XonLim;
    WORD  XoffLim;
    BYTE  ByteSize;
    BYTE  Parity;
    BYTE  StopBits;
    char  XonChar;
    char  XoffChar;
    char  ErrorChar;
    char  EofChar;
    char  EvtChar;
    WORD  wReserved1;
} K32_DCB;

typedef struct {
    DWORD ReadIntervalTimeout;
    DWORD ReadTotalTimeoutMultiplier;
    DWORD ReadTotalTimeoutConstant;
    DWORD WriteTotalTimeoutMultiplier;
    DWORD WriteTotalTimeoutConstant;
} K32_COMMTIMEOUTS;

typedef struct {
    DWORD Flags;
    DWORD cbInQue;
    DWORD cbOutQue;
} K32_COMSTAT;

typedef struct {
    uint32_t magic;
    K32_DCB dcb;
    K32_COMMTIMEOUTS timeouts;
    DWORD event_mask;
    DWORD modem_status;
    DWORD errors;
    DWORD input_queue_size;
    DWORD output_queue_size;
    BOOL break_active;
} K32_COMM_STATE;

#define K32_COMM_STATE_MAGIC 0x4D4D4F43U /* "COMM" */

static K32_COMM_STATE *k32_comm_state(HANDLE handle)
{
    FILE_OBJECT *file = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, handle, OBJ_TYPE_FILE,
                                    (PVOID *)&file);
    if (!NT_SUCCESS(status) || !file ||
        !(file->flags & FILE_OBJ_SERIAL) || !file->osfs_file) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return NULL;
    }

    K32_COMM_STATE *state = (K32_COMM_STATE *)file->osfs_file;
    if (state->magic != K32_COMM_STATE_MAGIC) {
        SetLastError(6);
        return NULL;
    }
    return state;
}

static BOOL WINAPI ClearCommBreak_k32(HANDLE handle)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    state->break_active = FALSE;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetCommBreak_k32(HANDLE handle)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    state->break_active = TRUE;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI ClearCommError_k32(HANDLE handle, DWORD *errors,
                                      K32_COMSTAT *status)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    if (!errors) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    *errors = state->errors;
    state->errors = 0;
    if (status) {
        status->Flags = 0;
        status->cbInQue = 0;
        status->cbOutQue = 0;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetupComm_k32(HANDLE handle, DWORD input_size,
                                  DWORD output_size)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    if (!input_size || !output_size) {
        SetLastError(87);
        return FALSE;
    }
    state->input_queue_size = input_size;
    state->output_queue_size = output_size;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI EscapeCommFunction_k32(HANDLE handle, DWORD function)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    switch (function) {
    case 1: /* SETXOFF */
    case 2: /* SETXON */
    case 3: /* SETRTS */
    case 4: /* CLRRTS */
    case 5: /* SETDTR */
    case 6: /* CLRDTR */
    case 8: /* SETBREAK */
    case 9: /* CLRBREAK */
        if (function == 8) state->break_active = TRUE;
        if (function == 9) state->break_active = FALSE;
        SetLastError(0);
        return TRUE;
    default:
        SetLastError(87);
        return FALSE;
    }
}

static BOOL WINAPI GetCommModemStatus_k32(HANDLE handle, DWORD *status)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    if (!status) {
        SetLastError(87);
        return FALSE;
    }
    *status = state->modem_status;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI GetCommState_k32(HANDLE handle, K32_DCB *dcb)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    if (!dcb) {
        SetLastError(87);
        return FALSE;
    }
    *dcb = state->dcb;
    dcb->DCBlength = sizeof(*dcb);
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetCommState_k32(HANDLE handle, const K32_DCB *dcb)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    if (!dcb || dcb->DCBlength != sizeof(*dcb) ||
        dcb->ByteSize < 5 || dcb->ByteSize > 8 || dcb->StopBits > 2 ||
        dcb->Parity > 4) {
        SetLastError(87);
        return FALSE;
    }
    state->dcb = *dcb;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI GetCommTimeouts_k32(HANDLE handle,
                                        K32_COMMTIMEOUTS *timeouts)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    if (!timeouts) {
        SetLastError(87);
        return FALSE;
    }
    *timeouts = state->timeouts;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetCommTimeouts_k32(HANDLE handle,
                                        const K32_COMMTIMEOUTS *timeouts)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    if (!timeouts) {
        SetLastError(87);
        return FALSE;
    }
    state->timeouts = *timeouts;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI PurgeComm_k32(HANDLE handle, DWORD flags)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    if (!flags || (flags & ~0x0FU)) {
        SetLastError(87);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetCommMask_k32(HANDLE handle, DWORD event_mask)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    state->event_mask = event_mask;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI WaitCommEvent_k32(HANDLE handle, DWORD *event_mask,
                                     PVOID overlapped)
{
    K32_COMM_STATE *state = k32_comm_state(handle);
    if (!state) return FALSE;
    if (!event_mask) {
        SetLastError(87);
        return FALSE;
    }

    /* A zero mask completes immediately. Device-backed serial objects can
     * publish queued events here without changing the user-mode ABI. */
    *event_mask = 0;
    if (!state->event_mask) {
        SetLastError(0);
        return TRUE;
    }
    if (overlapped) {
        SetLastError(997); /* ERROR_IO_PENDING */
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

DWORD kernel32_release_process_handles(DWORD process_id)
{
    DWORD released = 0;
    HANDLE handle;
    while ((handle = handle_take_owned(&g_handle_table, process_id)) != NULL) {
        if (CloseHandle(handle))
            released++;
    }
    k32_process_affinity_release(process_id);
    return released;
}

/* ── Console API ────────────────────────────────────────────── */

HANDLE WINAPI CreateJobObjectW(PVOID job_attributes, PCWSTR name)
{
    (void)job_attributes;
    (void)name;

    for (int i = 0; i < K32_MAX_JOB_OBJECTS; i++) {
        K32_JOB_OBJECT *job = &k32_jobs[i];
        if (job->in_use &&
            !handle_object_referenced(&g_handle_table, OBJ_TYPE_JOB, job))
            job_release(job, TRUE);
        if (job->in_use)
            continue;

        memset(job, 0, sizeof(*job));
        job->in_use = TRUE;
        HANDLE handle = NULL;
        NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_JOB,
                                       GENERIC_ALL, job, &handle);
        if (!NT_SUCCESS(status)) {
            memset(job, 0, sizeof(*job));
            set_last_error_from_status(status);
            return NULL;
        }
        serial_puts("[K32-JOB] create handle=0x");
        serial_puthex((uint64_t)(ULONG_PTR)handle, 8);
        serial_puts("\n");
        SetLastError(0);
        return handle;
    }

    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return NULL;
}

BOOL WINAPI SetInformationJobObject(HANDLE job_handle, DWORD info_class,
                                    PCVOID info, DWORD info_size)
{
    K32_JOB_OBJECT *job = NULL;
    DWORD required = g_compat32_mode ? 108 : 144;
    if (!NT_SUCCESS(handle_lookup(&g_handle_table, job_handle, OBJ_TYPE_JOB,
                                  (PVOID *)&job))) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (info_class != 9 || !info || info_size < required) {
        SetLastError(info_class == 9 ? 24 : 87); /* BAD_LENGTH / INVALID_PARAMETER */
        return FALSE;
    }

    job->limit_flags = *(const DWORD *)((const BYTE *)info + 16);
    serial_puts("[K32-JOB] set limits=0x");
    serial_puthex(job->limit_flags, 8);
    serial_puts("\n");
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI AssignProcessToJobObject(HANDLE job_handle, HANDLE process_handle)
{
    K32_JOB_OBJECT *job = NULL;
    PVOID process_object = NULL;
    if (!NT_SUCCESS(handle_lookup(&g_handle_table, job_handle, OBJ_TYPE_JOB,
                                  (PVOID *)&job)) ||
        !NT_SUCCESS(handle_lookup(&g_handle_table, process_handle,
                                  OBJ_TYPE_PROCESS, &process_object))) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    int free_slot = -1;
    for (int i = 0; i < K32_MAX_JOB_PROCESSES; i++) {
        if (!job->processes[i]) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        PVOID assigned_object = NULL;
        if (NT_SUCCESS(handle_lookup(&g_handle_table, job->processes[i],
                                     OBJ_TYPE_PROCESS, &assigned_object)) &&
            assigned_object == process_object) {
            SetLastError(0);
            return TRUE;
        }
    }
    if (free_slot < 0) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    HANDLE retained = NULL;
    NTSTATUS status = handle_duplicate(&g_handle_table, process_handle,
                                       &g_handle_table, &retained, 0, FALSE,
                                       DUPLICATE_SAME_ACCESS);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    job->processes[free_slot] = retained;
    serial_puts("[K32-JOB] assign process handle=0x");
    serial_puthex((uint64_t)(ULONG_PTR)process_handle, 8);
    serial_puts("\n");
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI QueryInformationJobObject(HANDLE job_handle, DWORD info_class,
                                      PVOID info, DWORD info_size,
                                      DWORD *return_size)
{
    K32_JOB_OBJECT *job = NULL;
    if (!NT_SUCCESS(handle_lookup(&g_handle_table, job_handle, OBJ_TYPE_JOB,
                                  (PVOID *)&job))) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    DWORD required;
    if (info_class == 1)
        required = 48; /* JOBOBJECT_BASIC_ACCOUNTING_INFORMATION */
    else if (info_class == 9)
        required = g_compat32_mode ? 108 : 144;
    else {
        if (return_size) *return_size = 0;
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (return_size) *return_size = required;
    if (!info || info_size < required) {
        SetLastError(24); /* ERROR_BAD_LENGTH */
        return FALSE;
    }

    memset(info, 0, required);
    if (info_class == 9) {
        *(DWORD *)((BYTE *)info + 16) = job->limit_flags;
    } else {
        DWORD total = 0;
        DWORD active = 0;
        extern BOOL nt_process_object_signaled(PVOID object);
        for (int i = 0; i < K32_MAX_JOB_PROCESSES; i++) {
            PVOID process_object = NULL;
            if (!job->processes[i] ||
                !NT_SUCCESS(handle_lookup(&g_handle_table, job->processes[i],
                                          OBJ_TYPE_PROCESS,
                                          &process_object)))
                continue;
            total++;
            if (!nt_process_object_signaled(process_object))
                active++;
        }
        *(DWORD *)((BYTE *)info + 36) = total;
        *(DWORD *)((BYTE *)info + 40) = active;
        *(DWORD *)((BYTE *)info + 44) = total - active;
    }
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI TerminateJobObject(HANDLE job_handle, UINT exit_code)
{
    K32_JOB_OBJECT *job = NULL;
    if (!NT_SUCCESS(handle_lookup(&g_handle_table, job_handle, OBJ_TYPE_JOB,
                                  (PVOID *)&job))) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    job_terminate_processes(job, exit_code);
    SetLastError(0);
    return TRUE;
}

#define K32_ATTRIBUTE_LIST_MAGIC 0x41545452U
#define K32_MAX_PROCESS_ATTRIBUTES 16
#define K32_PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002ULL
#define K32_EXTENDED_STARTUPINFO_PRESENT 0x00080000U
#define K32_MAX_INHERITED_HANDLES 64

typedef struct {
    ULONG_PTR attribute;
    PVOID value;
    SIZE_T size;
} K32_PROCESS_ATTRIBUTE;

typedef struct {
    DWORD magic;
    DWORD capacity;
    DWORD count;
    DWORD reserved;
    K32_PROCESS_ATTRIBUTE entries[];
} K32_PROCESS_ATTRIBUTE_LIST;

static SIZE_T process_attribute_size_read(const SIZE_T *size)
{
    return g_compat32_mode ? *(const DWORD *)size : *size;
}

static void process_attribute_size_write(SIZE_T *size, SIZE_T value)
{
    if (g_compat32_mode)
        *(DWORD *)size = (DWORD)value;
    else
        *size = value;
}

BOOL WINAPI InitializeProcThreadAttributeList(PVOID list, DWORD attribute_count,
                                              DWORD flags, SIZE_T *list_size)
{
    if (!list_size || flags || attribute_count > K32_MAX_PROCESS_ATTRIBUTES) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    SIZE_T required = sizeof(K32_PROCESS_ATTRIBUTE_LIST) +
                      (SIZE_T)attribute_count * sizeof(K32_PROCESS_ATTRIBUTE);
    SIZE_T supplied = process_attribute_size_read(list_size);
    process_attribute_size_write(list_size, required);
    if (!list || supplied < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }

    memset(list, 0, required);
    K32_PROCESS_ATTRIBUTE_LIST *attributes =
        (K32_PROCESS_ATTRIBUTE_LIST *)list;
    attributes->magic = K32_ATTRIBUTE_LIST_MAGIC;
    attributes->capacity = attribute_count;
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI UpdateProcThreadAttribute(PVOID list, DWORD flags,
                                      ULONG_PTR attribute, PVOID value,
                                      SIZE_T value_size, PVOID previous_value,
                                      SIZE_T *return_size)
{
    K32_PROCESS_ATTRIBUTE_LIST *attributes =
        (K32_PROCESS_ATTRIBUTE_LIST *)list;
    if (!attributes || attributes->magic != K32_ATTRIBUTE_LIST_MAGIC ||
        flags > 1 || (!value && value_size) || previous_value) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD slot = attributes->count;
    for (DWORD i = 0; i < attributes->count; i++) {
        if (attributes->entries[i].attribute == attribute) {
            slot = i;
            break;
        }
    }
    if (slot == attributes->count) {
        if (attributes->count >= attributes->capacity) {
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            return FALSE;
        }
        attributes->count++;
    }

    attributes->entries[slot].attribute = attribute;
    attributes->entries[slot].value = value;
    attributes->entries[slot].size = value_size;
    if (return_size)
        process_attribute_size_write(return_size, value_size);
    serial_puts("[K32-ATTR] update attr=0x");
    serial_puthex((uint64_t)attribute, 16);
    serial_puts(" size=0x");
    serial_puthex((uint64_t)value_size, 8);
    serial_puts("\n");
    SetLastError(0);
    return TRUE;
}

void WINAPI DeleteProcThreadAttributeList(PVOID list)
{
    K32_PROCESS_ATTRIBUTE_LIST *attributes =
        (K32_PROCESS_ATTRIBUTE_LIST *)list;
    if (!attributes || attributes->magic != K32_ATTRIBUTE_LIST_MAGIC)
        return;
    SIZE_T size = sizeof(*attributes) +
                  (SIZE_T)attributes->capacity * sizeof(attributes->entries[0]);
    memset(attributes, 0, size);
}

static BOOL process_retain_inherited_handles(PVOID startup_info, DWORD flags,
                                             BOOL inherit_handles,
                                             HANDLE retained[],
                                             DWORD *retained_count)
{
    *retained_count = 0;
    if (!(flags & K32_EXTENDED_STARTUPINFO_PRESENT))
        return TRUE;
    if (!startup_info) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    BYTE *startup = (BYTE *)startup_info;
    DWORD startup_size = *(DWORD *)startup;
    K32_PROCESS_ATTRIBUTE_LIST *attributes;
    if (g_compat32_mode) {
        if (startup_size < 72) {
            SetLastError(87);
            return FALSE;
        }
        attributes = (K32_PROCESS_ATTRIBUTE_LIST *)(ULONG_PTR)
                     *(uint32_t *)(startup + 68);
    } else {
        if (startup_size < 112) {
            SetLastError(87);
            return FALSE;
        }
        attributes = *(K32_PROCESS_ATTRIBUTE_LIST **)(startup + 104);
    }

    if (!attributes || attributes->magic != K32_ATTRIBUTE_LIST_MAGIC ||
        attributes->count > attributes->capacity ||
        attributes->capacity > K32_MAX_PROCESS_ATTRIBUTES) {
        SetLastError(87);
        return FALSE;
    }

    for (DWORD i = 0; i < attributes->count; i++) {
        K32_PROCESS_ATTRIBUTE *entry = &attributes->entries[i];
        if (entry->attribute != K32_PROC_THREAD_ATTRIBUTE_HANDLE_LIST)
            continue;
        if (!inherit_handles || !entry->value) {
            SetLastError(87);
            goto fail;
        }

        SIZE_T handle_size = g_compat32_mode ? sizeof(uint32_t)
                                                : sizeof(HANDLE);
        if (entry->size % handle_size != 0 ||
            entry->size / handle_size > K32_MAX_INHERITED_HANDLES) {
            SetLastError(87);
            goto fail;
        }

        DWORD count = (DWORD)(entry->size / handle_size);
        for (DWORD j = 0; j < count; j++) {
            HANDLE handle = g_compat32_mode
                ? (HANDLE)(ULONG_PTR)((uint32_t *)entry->value)[j]
                : ((HANDLE *)entry->value)[j];
            PVOID object = NULL;
            NTSTATUS status = handle_lookup(&g_handle_table, handle,
                                            OBJ_TYPE_NONE, &object);
            if (!NT_SUCCESS(status)) {
                set_last_error_from_status(status);
                goto fail;
            }
            retained[(*retained_count)++] = handle;
            serial_puts("[K32-INHERIT] selected handle=0x");
            serial_puthex((uint64_t)(ULONG_PTR)handle, 8);
            serial_puts("\n");
        }
    }
    return TRUE;

fail:
    *retained_count = 0;
    return FALSE;
}

HANDLE WINAPI GetStdHandle(DWORD nStdHandle)
{
    return console_handle(nStdHandle);
}

BOOL WINAPI WriteConsoleA(HANDLE hConsoleOutput, PCVOID lpBuffer,
                   DWORD nNumberOfCharsToWrite,
                   DWORD *lpNumberOfCharsWritten, PVOID lpReserved)
{
    (void)lpReserved;
    return WriteFile(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite,
                     lpNumberOfCharsWritten, NULL);
}

/* ── Process API ────────────────────────────────────────────── */

void WINAPI ExitProcess(DWORD uExitCode)
{
    extern uint32_t compat32_get_last_caller_eip(void);
    extern uint32_t compat32_get_last_user_ebp(void);
    extern uint32_t compat32_get_last_stack_args(void);
    extern void compat32_dump_recent_calls(void);
    extern void wdbg_stack_scan(uint32_t esp, int depth, const char *label);
    serial_puts("[K32] ExitProcess called, pid=");
    serial_putdec(win32_current_process_id());
    serial_puts(" code=");
    serial_putdec(uExitCode);
    if (g_compat32_mode) {
        uint32_t eip = compat32_get_last_caller_eip();
        uint32_t ebp = compat32_get_last_user_ebp();
        uint32_t esp = compat32_get_last_stack_args();
        serial_puts(" caller=0x");
        serial_puthex(eip, 8);
        serial_puts(" EBP=0x");
        serial_puthex(ebp, 8);
        serial_puts("\n");
        wdbg_stack_scan(esp, 96, "ExitProcess-stack");
        if (k32_path_contains_ci(win32_current_exe_name(),
                                 "steamservice.exe"))
            compat32_dump_recent_calls();
    } else {
        serial_puts(" caller=0x");
        serial_puthex((uint64_t)__builtin_return_address(0), 16);
        serial_puts("\n");
    }
    NtTerminateProcess(NT_CURRENT_PROCESS, (NTSTATUS)uExitCode);
    /* Never returns */
    for (;;) __asm__ volatile("hlt");
}

HANDLE WINAPI GetCurrentProcess(void)
{
    return NT_CURRENT_PROCESS;
}

HANDLE WINAPI OpenProcess(DWORD desired_access, BOOL inherit_handle,
                          DWORD process_id)
{
    (void)inherit_handle;
    HANDLE handle = NULL;
    extern NTSTATUS nt_process_open(ULONG pid, ACCESS_MASK access,
                                    PHANDLE out_handle);
    NTSTATUS status = nt_process_open(process_id, desired_access, &handle);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return NULL;
    }
    return handle;
}

static BOOL k32_process_handle_valid(HANDLE process)
{
    ULONG_PTR value = (ULONG_PTR)process;
    if (value == (ULONG_PTR)NT_CURRENT_PROCESS || value == UINT32_MAX)
        return TRUE;
    if (!process) return FALSE;

    extern BOOL nt_process_id(HANDLE handle, DWORD *process_id);
    DWORD process_id = 0;
    return nt_process_id(process, &process_id);
}

static BOOL WINAPI GetProcessMitigationPolicy_k32(HANDLE process,
                                                    DWORD policy,
                                                    PVOID buffer,
                                                    SIZE_T length)
{
    (void)policy;
    if (!process || !buffer || length < sizeof(DWORD)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    *(DWORD *)buffer = 0; /* No process mitigations are enforced yet. */
    return TRUE;
}

static BOOL WINAPI SetProcessMitigationPolicy_k32(DWORD policy,
                                                    PCVOID buffer,
                                                    SIZE_T length)
{
    (void)policy;
    if (!buffer || length < sizeof(DWORD)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    /* The current address-space model does not enforce Windows mitigation
     * policy bits, but callers may safely proceed as if they were accepted. */
    return TRUE;
}

static BOOL WINAPI IsWow64Process_k32(HANDLE process, BOOL *wow64_process)
{
    (void)process;
    if (!wow64_process) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    *wow64_process = g_compat32_mode ? TRUE : FALSE;
    return TRUE;
}

static BOOL WINAPI IsWow64Process2_k32(HANDLE process,
                                        USHORT *process_machine,
                                        USHORT *native_machine)
{
    (void)process;
    if (!process_machine || !native_machine) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    *process_machine = g_compat32_mode ? IMAGE_FILE_MACHINE_I386 : 0;
    *native_machine = IMAGE_FILE_MACHINE_AMD64;
    return TRUE;
}

static BOOL WINAPI IsProcessorFeaturePresent_k32(DWORD feature)
{
    (void)feature;
    return FALSE;
}

DWORD WINAPI GetCurrentProcessId(void)
{
    return win32_current_process_id();
}

static BOOL WINAPI EnumProcessModules_psapi(HANDLE process, PVOID modules,
                                             DWORD cb, DWORD *needed)
{
    (void)process;
    if (!needed) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    ULONG_PTR image_base = win32_current_image_base();
    DWORD pointer_size = g_compat32_mode ? 4 : 8;
    *needed = pointer_size;
    if (!modules || cb < pointer_size) {
        if (!modules && cb == 0) return TRUE;
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }

    if (g_compat32_mode)
        *(uint32_t *)modules = (uint32_t)image_base;
    else
        *(ULONG_PTR *)modules = image_base;
    return TRUE;
}

static const char *module_base_name(HANDLE module)
{
    const char *base = win32_current_exe_name();
    LOADED_MODULE *loaded = dll_find_module_by_base((PVOID)module);
    if (loaded)
        return loaded->name;
    else
        for (const char *p = base; *p; p++)
            if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

static DWORD WINAPI GetModuleBaseNameA_psapi(HANDLE process, HANDLE module,
                                              char *name, DWORD size)
{
    (void)process;
    if (!name || size == 0) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    const char *base = module_base_name(module);
    DWORD copied = 0;
    while (base[copied] && copied + 1 < size) {
        name[copied] = base[copied];
        copied++;
    }
    name[copied] = 0;
    return copied;
}

static DWORD WINAPI GetModuleBaseNameW_psapi(HANDLE process, HANDLE module,
                                              WCHAR *name, DWORD size)
{
    (void)process;
    if (!name || size == 0) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    const char *base = module_base_name(module);
    DWORD copied = 0;
    while (base[copied] && copied + 1 < size) {
        name[copied] = (WCHAR)(unsigned char)base[copied];
        copied++;
    }
    name[copied] = 0;
    return copied;
}

static BOOL WINAPI GetProcessMemoryInfo_psapi(HANDLE process, PVOID counters,
                                               DWORD cb)
{
    (void)process;
    const DWORD base_size = g_compat32_mode ? 40 : 72;
    const DWORD ex2_size = g_compat32_mode ? 52 : 96;
    if (!counters || cb < base_size) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }

    /* PROCESS_MEMORY_COUNTERS_EX adds PrivateUsage after the base structure.
     * Chromium passes the 80-byte PE64 form and validates that last field. */
    DWORD fill_size = cb < ex2_size ? cb : ex2_size;
    memset(counters, 0, fill_size);
    *(DWORD *)counters = cb;
    return TRUE;
}

static BOOL WINAPI QueryWorkingSetEx_psapi(HANDLE process, PVOID entries,
                                            DWORD cb)
{
    (void)process;
    DWORD entry_size = g_compat32_mode ? 8U : 16U;
    if (!entries || !cb || cb % entry_size) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    BYTE *cursor = (BYTE *)entries;
    for (DWORD offset = 0; offset < cb; offset += entry_size) {
        if (g_compat32_mode)
            *(uint32_t *)(cursor + offset + 4) = 0x41;
        else
            *(uint64_t *)(cursor + offset + 8) = 0x41;
    }
    SetLastError(0);
    return TRUE;
}

typedef struct {
    DWORD cb;
    DWORD padding;
    uint64_t values[10];
    DWORD handle_count;
    DWORD process_count;
    DWORD thread_count;
    DWORD tail_padding;
} PERFORMANCE_INFORMATION64;

typedef struct {
    DWORD cb;
    uint32_t values[10];
    DWORD handle_count;
    DWORD process_count;
    DWORD thread_count;
} PERFORMANCE_INFORMATION32;

_Static_assert(sizeof(PERFORMANCE_INFORMATION64) == 104,
               "PE64 PERFORMANCE_INFORMATION layout mismatch");
_Static_assert(sizeof(PERFORMANCE_INFORMATION32) == 56,
               "PE32 PERFORMANCE_INFORMATION layout mismatch");

static BOOL WINAPI GetPerformanceInfo_psapi(PVOID info, DWORD cb)
{
    extern uint64_t mem_get_total(void);
    extern uint64_t mem_get_free(void);

    DWORD required = g_compat32_mode ? sizeof(PERFORMANCE_INFORMATION32)
                                     : sizeof(PERFORMANCE_INFORMATION64);
    if (!info || cb < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }

    uint64_t total_pages = mem_get_total() / 4096;
    uint64_t free_pages = mem_get_free() / 4096;
    uint64_t used_pages = total_pages - free_pages;

    memset(info, 0, required);
    if (g_compat32_mode) {
        PERFORMANCE_INFORMATION32 *perf = info;
        perf->cb = required;
        perf->values[0] = (uint32_t)used_pages;  /* CommitTotal */
        perf->values[1] = (uint32_t)total_pages; /* CommitLimit */
        perf->values[2] = (uint32_t)used_pages;  /* CommitPeak */
        perf->values[3] = (uint32_t)total_pages; /* PhysicalTotal */
        perf->values[4] = (uint32_t)free_pages;  /* PhysicalAvailable */
        perf->values[9] = 4096;                  /* PageSize */
        perf->process_count = 1;
        perf->thread_count = 1;
    } else {
        PERFORMANCE_INFORMATION64 *perf = info;
        perf->cb = required;
        perf->values[0] = used_pages;
        perf->values[1] = total_pages;
        perf->values[2] = used_pages;
        perf->values[3] = total_pages;
        perf->values[4] = free_pages;
        perf->values[9] = 4096;
        perf->process_count = 1;
        perf->thread_count = 1;
    }
    return TRUE;
}

static BOOL WINAPI GetModuleInformation_psapi(HANDLE process, HANDLE module,
                                                PVOID info, DWORD cb)
{
    (void)process;
    DWORD required = g_compat32_mode ? 12 : 24;
    BYTE *base = module ? (BYTE *)module
                        : (BYTE *)(ULONG_PTR)win32_current_image_base();
    BYTE *main_base = (BYTE *)win32_current_image_base();

    if (!info || cb < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }
    if (!base || (base != main_base && !dll_find_module_by_base(base))) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        SetLastError(193); /* ERROR_BAD_EXE_FORMAT */
        return FALSE;
    }
    BYTE *nt_base = base + dos->e_lfanew;
    if (*(ULONG *)nt_base != IMAGE_NT_SIGNATURE) {
        SetLastError(193);
        return FALSE;
    }

    USHORT magic = *(USHORT *)(nt_base + sizeof(ULONG) + sizeof(IMAGE_FILE_HEADER));
    DWORD size, entry_rva;
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        PIMAGE_NT_HEADERS32 nt = (PIMAGE_NT_HEADERS32)nt_base;
        size = nt->OptionalHeader.SizeOfImage;
        entry_rva = nt->OptionalHeader.AddressOfEntryPoint;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)nt_base;
        size = nt->OptionalHeader.SizeOfImage;
        entry_rva = nt->OptionalHeader.AddressOfEntryPoint;
    } else {
        SetLastError(193);
        return FALSE;
    }

    memset(info, 0, required);
    if (g_compat32_mode) {
        uint32_t *out = (uint32_t *)info;
        out[0] = (uint32_t)(ULONG_PTR)base;
        out[1] = size;
        out[2] = (uint32_t)(ULONG_PTR)(base + entry_rva);
    } else {
        *(ULONG_PTR *)((BYTE *)info + 0) = (ULONG_PTR)base;
        *(DWORD *)((BYTE *)info + 8) = size;
        *(ULONG_PTR *)((BYTE *)info + 16) = (ULONG_PTR)(base + entry_rva);
    }
    return TRUE;
}

static DWORD WINAPI GetProcessId_k32(HANDLE process)
{
    if (!process) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }
    if (process == NT_CURRENT_PROCESS) return GetCurrentProcessId();
    extern BOOL nt_process_id(HANDLE handle, DWORD *process_id);
    DWORD process_id = 0;
    if (!nt_process_id(process, &process_id)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }
    return process_id;
}

static DWORD WINAPI GetThreadId_k32(HANDLE thread)
{
    if (thread == NT_CURRENT_THREAD)
        return GetCurrentThreadId();
    if (!thread) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }

    extern BOOL nt_thread_id(HANDLE handle, DWORD *thread_id);
    DWORD thread_id = 0;
    if (!nt_thread_id(thread, &thread_id)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }
    SetLastError(0);
    return thread_id;
}

static BOOL WINAPI ProcessIdToSessionId_k32(DWORD process_id, DWORD *session_id)
{
    (void)process_id;
    if (!session_id) return FALSE;
    *session_id = 0;
    return TRUE;
}

#define TH32CS_SNAPPROCESS 0x00000002U

typedef struct {
    DWORD process_id;
    DWORD parent_process_id;
    char exe_name[64];
} K32_TOOLHELP_PROCESS;

typedef struct {
    DWORD count;
    DWORD cursor;
    BOOL started;
    K32_TOOLHELP_PROCESS processes[];
} K32_TOOLHELP_SNAPSHOT;

extern DWORD win32_process_snapshot_capacity(void);
extern BOOL win32_process_snapshot_slot(DWORD slot, DWORD *process_id,
                                        DWORD *parent_process_id,
                                        char *exe_name,
                                        SIZE_T exe_name_capacity);

static BOOL WINAPI K32EnumProcesses_k32(DWORD *process_ids, DWORD bytes,
                                         DWORD *bytes_returned)
{
    if (!bytes_returned || (bytes && !process_ids)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD capacity = bytes / sizeof(DWORD);
    DWORD written = 0;
    DWORD slots = win32_process_snapshot_capacity();
    for (DWORD slot = 0; slot < slots && written < capacity; slot++) {
        DWORD process_id = 0;
        if (win32_process_snapshot_slot(slot, &process_id, NULL, NULL, 0))
            process_ids[written++] = process_id;
    }

    *bytes_returned = written * sizeof(DWORD);
    SetLastError(0);
    return TRUE;
}

typedef struct __attribute__((packed)) {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ProcessID;
    uint32_t th32DefaultHeapID;
    DWORD th32ModuleID;
    DWORD cntThreads;
    DWORD th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    char szExeFile[260];
} PROCESSENTRY32_PE32;

typedef struct {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ProcessID;
    ULONG_PTR th32DefaultHeapID;
    DWORD th32ModuleID;
    DWORD cntThreads;
    DWORD th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    char szExeFile[260];
} PROCESSENTRY32_PE64;

typedef struct __attribute__((packed)) {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ProcessID;
    uint32_t th32DefaultHeapID;
    DWORD th32ModuleID;
    DWORD cntThreads;
    DWORD th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    WCHAR szExeFile[260];
} PROCESSENTRY32W_PE32;

typedef struct {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ProcessID;
    ULONG_PTR th32DefaultHeapID;
    DWORD th32ModuleID;
    DWORD cntThreads;
    DWORD th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    WCHAR szExeFile[260];
} PROCESSENTRY32W_PE64;

_Static_assert(sizeof(PROCESSENTRY32_PE32) == 296,
               "PE32 PROCESSENTRY32 layout");
_Static_assert(sizeof(PROCESSENTRY32_PE64) == 304,
               "PE64 PROCESSENTRY32 layout");
_Static_assert(sizeof(PROCESSENTRY32W_PE32) == 556,
               "PE32 PROCESSENTRY32W layout");
_Static_assert(sizeof(PROCESSENTRY32W_PE64) == 568,
               "PE64 PROCESSENTRY32W layout");

static HANDLE WINAPI CreateToolhelp32Snapshot_k32(DWORD flags, DWORD process_id)
{
    (void)process_id;
    if (!(flags & TH32CS_SNAPPROCESS)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return INVALID_HANDLE_VALUE;
    }

    DWORD capacity = win32_process_snapshot_capacity();
    uint64_t allocation_size = sizeof(K32_TOOLHELP_SNAPSHOT) +
        (uint64_t)capacity * sizeof(K32_TOOLHELP_PROCESS);
    K32_TOOLHELP_SNAPSHOT *object = kmalloc(allocation_size);
    if (!object) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return INVALID_HANDLE_VALUE;
    }
    memset(object, 0, allocation_size);

    for (DWORD slot = 0; slot < capacity; slot++) {
        K32_TOOLHELP_PROCESS *process = &object->processes[object->count];
        if (win32_process_snapshot_slot(slot, &process->process_id,
                                        &process->parent_process_id,
                                        process->exe_name,
                                        sizeof(process->exe_name)))
            object->count++;
    }

    HANDLE snapshot;
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_SNAPSHOT, 0,
                                   object, &snapshot);
    if (!NT_SUCCESS(status)) {
        kfree(object);
        set_last_error_from_status(status);
        return INVALID_HANDLE_VALUE;
    }
    return snapshot;
}

static BOOL toolhelp_fill_process(HANDLE snapshot, PVOID buffer, BOOL wide,
                                  BOOL first)
{
    PVOID raw_object = NULL;
    if (!NT_SUCCESS(handle_lookup(&g_handle_table, snapshot,
                                  OBJ_TYPE_SNAPSHOT, &raw_object)) ||
        !raw_object) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (!buffer) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD expected;
    if (wide)
        expected = g_compat32_mode ? sizeof(PROCESSENTRY32W_PE32)
                                   : sizeof(PROCESSENTRY32W_PE64);
    else
        expected = g_compat32_mode ? sizeof(PROCESSENTRY32_PE32)
                                   : sizeof(PROCESSENTRY32_PE64);
    if (*(DWORD *)buffer < expected) {
        SetLastError(24); /* ERROR_BAD_LENGTH */
        return FALSE;
    }

    K32_TOOLHELP_SNAPSHOT *object = raw_object;
    DWORD cursor;
    if (first) {
        cursor = 0;
    } else {
        if (!object->started) {
            SetLastError(18); /* ERROR_NO_MORE_FILES */
            return FALSE;
        }
        cursor = object->cursor + 1;
    }
    if (cursor >= object->count) {
        SetLastError(18); /* ERROR_NO_MORE_FILES */
        return FALSE;
    }

    object->cursor = cursor;
    object->started = TRUE;
    const K32_TOOLHELP_PROCESS *process = &object->processes[cursor];

    if (wide && g_compat32_mode) {
        PROCESSENTRY32W_PE32 *pe = buffer;
        memset(pe, 0, sizeof(*pe));
        pe->dwSize = sizeof(*pe);
        pe->th32ProcessID = process->process_id;
        pe->th32ParentProcessID = process->parent_process_id;
        pe->cntThreads = 1;
        pe->pcPriClassBase = 8;
        for (int i = 0; process->exe_name[i] && i < 259; i++)
            pe->szExeFile[i] = (WCHAR)(BYTE)process->exe_name[i];
    } else if (wide) {
        PROCESSENTRY32W_PE64 *pe = buffer;
        memset(pe, 0, sizeof(*pe));
        pe->dwSize = sizeof(*pe);
        pe->th32ProcessID = process->process_id;
        pe->th32ParentProcessID = process->parent_process_id;
        pe->cntThreads = 1;
        pe->pcPriClassBase = 8;
        for (int i = 0; process->exe_name[i] && i < 259; i++)
            pe->szExeFile[i] = (WCHAR)(BYTE)process->exe_name[i];
    } else if (g_compat32_mode) {
        PROCESSENTRY32_PE32 *pe = buffer;
        memset(pe, 0, sizeof(*pe));
        pe->dwSize = sizeof(*pe);
        pe->th32ProcessID = process->process_id;
        pe->th32ParentProcessID = process->parent_process_id;
        pe->cntThreads = 1;
        pe->pcPriClassBase = 8;
        for (int i = 0; process->exe_name[i] && i < 259; i++)
            pe->szExeFile[i] = process->exe_name[i];
    } else {
        PROCESSENTRY32_PE64 *pe = buffer;
        memset(pe, 0, sizeof(*pe));
        pe->dwSize = sizeof(*pe);
        pe->th32ProcessID = process->process_id;
        pe->th32ParentProcessID = process->parent_process_id;
        pe->cntThreads = 1;
        pe->pcPriClassBase = 8;
        for (int i = 0; process->exe_name[i] && i < 259; i++)
            pe->szExeFile[i] = process->exe_name[i];
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI Process32First_k32(HANDLE snapshot, PVOID process_entry)
{
    return toolhelp_fill_process(snapshot, process_entry, FALSE, TRUE);
}

static BOOL WINAPI Process32FirstW_k32(HANDLE snapshot, PVOID process_entry)
{
    return toolhelp_fill_process(snapshot, process_entry, TRUE, TRUE);
}

static BOOL WINAPI Process32Next_k32(HANDLE snapshot, PVOID process_entry)
{
    return toolhelp_fill_process(snapshot, process_entry, FALSE, FALSE);
}

static BOOL WINAPI Process32NextW_k32(HANDLE snapshot, PVOID process_entry)
{
    return toolhelp_fill_process(snapshot, process_entry, TRUE, FALSE);
}

/* ── Memory API ─────────────────────────────────────────────── */

/* try_patch_farray — given a user-space address that MIGHT be an FArray
 * (UE1 TArray header: { void* Data; INT Num; INT Max; }), check the
 * corrupt-pattern signature `*(cand+8)` ∈ PE-image .text range
 * [0x10000000, 0x20000000).  If matched, patch {+8}=2, zero Data/Num if
 * they also look like code/stack ptrs, and derive a safe dwSize.
 * Returns 1 if patched, 0 otherwise.
 *
 * Used by VirtualAlloc cap path to repair multiple class of corrupt
 * FArray sites: (a) saved on EBP-chain stack frames, (b) directly
 * pointed to by user-side callee-saved regs (ESI/EDI/EBX). */
static int try_patch_farray(uint32_t cand_addr, uint32_t newmax_hint,
                             SIZE_T *dwSize_out, const char *origin)
{
    if (cand_addr < 0x100000 || cand_addr >= 0xFFFE0000 || (cand_addr & 3))
        return 0;
    uint32_t *t = (uint32_t *)(uintptr_t)cand_addr;
    uint32_t plus0 = t[0];  /* Data */
    uint32_t plus4 = t[1];  /* Num  */
    uint32_t plus8 = t[2];  /* Max or ElementSize per disasm */
    /* {+8} must look like a leaked PE-image .text code pointer.  Range
     * covers Core.dll/Engine.dll/UT.exe/Window.dll. */
    if (plus8 < 0x10000000 || plus8 >= 0x12000000) return 0;
    /* CRITICAL: a real FArray's Data is either NULL (fresh array, never
     * allocated yet) OR a heap pointer (UT99 heap starts at 0x40000000).
     * If Data is in stack range (0x14xxxxxx) the "FArray" is actually a
     * stack frame whose saved-EBP points to the parent frame.  Patching
     * `{+8}` then writes 4 over the parent frame's first stack arg AND
     * we'd also zero `{+4}` (the saved return address!) — engine RET's
     * to address 0 → tight loop in null-call recovery.  Observed live
     * on UT99 with FArray@0x14001140 (= frame 3 EBP) where Data=
     * 0x14001174 (= frame 4 EBP). */
    if (plus0 != 0 && plus0 < 0x40000000) return 0;

    static int patch_log = 0;
    if (patch_log < 20) {
        serial_puts("[VA-FARRAY] ");
        serial_puts(origin);
        serial_puts(" FArray@0x"); serial_puthex(cand_addr, 8);
        serial_puts(" Data=0x"); serial_puthex(plus0, 8);
        serial_puts(" Num=0x"); serial_puthex(plus4, 8);
        serial_puts(" {+8}=0x"); serial_puthex(plus8, 8);
        patch_log++;
    }
    /* Patch {+8} to 4 (pointer-sized element).  Most UE1 TArrays hold
     * UObject* / FName / similar 4-byte values.  WCHAR=2 was empirically
     * too small for non-string arrays and put the engine in a tight
     * loop reading half-words as full structs. */
    t[2] = 4;
    if (t[0] >= 0x10000000 && t[0] < 0x12000000) t[0] = 0;  /* Data */
    if (t[1] >= 0x10000000 && t[1] < 0x12000000) t[1] = 0;  /* Num */
    /* dwSize: bigger is safer (with VA-CACHE deduping, the VA range
     * stays healthy).  Aim for ~256 KB worst case, derived from
     * NewMax * 4 if we know it. */
    if (newmax_hint > 0 && newmax_hint < 0x10000) {
        SIZE_T s = (SIZE_T)(newmax_hint * 4 + 0xFFF) & ~(SIZE_T)0xFFF;
        if (s < 0x40000) s = 0x40000;  /* min 256 KB */
        *dwSize_out = s;
    } else {
        *dwSize_out = 0x40000;  /* 256 KB fallback */
    }
    if (patch_log <= 20) {
        serial_puts(" -> {+8}=4 dwSize=0x"); serial_puthex(*dwSize_out, 8);
        serial_puts("\n");
    }
    return 1;
}

/* VA-CACHE: dedupe the bogus VirtualAlloc spam from UT99's corrupt
 * TArray::Realloc paths.  Background: an FArray with `{+8}` set to a
 * code-pointer (uninitialized stack local) computes `NewSize = NewMax *
 * code_ptr` ≈ 3-4 GB.  The FArray scan in this shim patches what it can
 * find via EBP walking, but a parallel call site that the scan misses
 * still produces 100+ bogus requests in a tight loop.  Allocating a
 * fresh 64KB block for each one exhausts the 896MB user VA range and
 * leads to STATUS_NO_MEMORY → appError → forced shell return.
 *
 * Strategy: keyed by caller-EIP, after the FIRST bogus request from a
 * given site is served with a capped buffer, every subsequent call
 * from the same EIP returns the SAME buffer.  UT99 doesn't free
 * between iterations, and the no-op'd rep-movsl @0x1010723E means
 * the buffer is effectively write-only metadata that nobody reads
 * back to a meaningful value.  Reuse is harmless and saves the VA. */
#define VA_CACHE_N 16
static struct {
    uint32_t eip;
    PVOID    base;
    SIZE_T   size;
    uint32_t hits;
} va_cache[VA_CACHE_N];

PVOID WINAPI VirtualAlloc(PVOID lpAddress, SIZE_T dwSize,
                   DWORD flAllocationType, DWORD flProtect)
{
    int      was_capped     = 0;
    uint32_t cache_eip_save = 0;

    /* Suppress normal VA logs — only log large/abnormal requests */
    if (g_compat32_mode && g_compat32_ut99 && dwSize > 0x1000000) {
        serial_puts("[VA] VirtualAlloc LARGE: size=0x");
        serial_puthex(dwSize, 8);
        serial_puts(" addr=0x");
        serial_puthex((uint64_t)(ULONG_PTR)lpAddress, 8);
        extern uint32_t compat32_get_last_caller_eip(void);
        uint32_t user_eip = compat32_get_last_caller_eip();
        if (user_eip) {
            serial_puts(" userEIP=0x");
            serial_puthex(user_eip, 8);
        }
        serial_puts("\n");

        /* VA-CACHE lookup: short-circuit repeat bogus requests from
         * the same caller-EIP.  This bypasses both the diagnostic dump
         * and the FArray-scan + cap fallback below.  Hit-count logged
         * only at powers of 10 to avoid log spam.
         *
         * Special case eip==0: after a NULL-CALL recovery, every
         * subsequent INT 0x2E has stack_args[-1] == 0 (synthesized
         * retaddr), so g_last_caller_eip stays 0 forever.  Without a
         * dedicated cache slot the engine spams thousands of LARGE
         * allocs that each consume 256KB → VA range exhaust in ~3500
         * calls → STATUS_NO_MEMORY → terminal appError.  Reserve a
         * dedicated "post-recovery sentinel" slot keyed at eip=0
         * + size>16MB (caller's intent obviously bogus). */
        if (lpAddress == NULL) {
            uint32_t key = user_eip ? user_eip : 0xDEAD0000;
            for (int i = 0; i < VA_CACHE_N; i++) {
                if (va_cache[i].eip == key && va_cache[i].base) {
                    va_cache[i].hits++;
                    if (va_cache[i].hits == 2 || va_cache[i].hits == 10 ||
                        va_cache[i].hits == 100 || va_cache[i].hits == 1000 ||
                        va_cache[i].hits == 10000) {
                        serial_puts("[VA] cache reuse eip=0x");
                        serial_puthex(key, 8);
                        serial_puts(" hits=");
                        serial_putdec(va_cache[i].hits);
                        serial_puts(" -> base=0x");
                        serial_puthex(
                            (uint64_t)(ULONG_PTR)va_cache[i].base, 8);
                        serial_puts("\n");
                    }
                    return va_cache[i].base;
                }
            }
        }
        cache_eip_save = user_eip ? user_eip : 0xDEAD0000;  /* for STORE after cap path */

        /* Walk the user-mode EBP frame-pointer chain to find every
         * caller of the FMallocWindows::Realloc wrapper. The first
         * frame above us is the wrapper itself; subsequent frames
         * lead back through the engine to the function whose
         * corrupt TArray is feeding the bogus size. */
        extern uint32_t compat32_get_last_user_ebp(void);
        uint32_t ebp = compat32_get_last_user_ebp();
        serial_puts("[VA]   user_ebp=0x"); serial_puthex(ebp, 8);
        serial_puts(" frames:\n");
        uint32_t cur = ebp;
        for (int f = 0; f < 8; f++) {
            if (cur < 0x100000 || cur >= 0xFFFE0000 || (cur & 3)) {
                serial_puts("[VA]   frame "); serial_putdec(f);
                serial_puts(": stop at ebp=0x"); serial_puthex(cur, 8);
                serial_puts("\n");
                break;
            }
            uint32_t *fp = (uint32_t *)(uintptr_t)cur;
            uint32_t saved_ebp = fp[0];
            uint32_t ret_addr  = fp[1];
            serial_puts("[VA]   frame "); serial_putdec(f);
            serial_puts(": ebp=0x"); serial_puthex(cur, 8);
            serial_puts(" ret=0x"); serial_puthex(ret_addr, 8);
            serial_puts(" sebp=0x"); serial_puthex(saved_ebp, 8);
            serial_puts("\n");
            if (saved_ebp <= cur) break;  /* not strictly increasing → stop */
            cur = saved_ebp;
        }

        /* Dump the args at each frame's [EBP+8..EBP+24] to catch the
         * actual count/element_size/tag passed to FArray::Realloc.
         * FArray::Realloc(void*, INT count, INT element_size, const char* tag).
         * The first non-wrapper frame above us SHOULD have these args. */
        cur = ebp;
        for (int f = 0; f < 4; f++) {
            if (cur < 0x100000 || cur >= 0xFFFE0000 || (cur & 3)) break;
            uint32_t *fp = (uint32_t *)(uintptr_t)cur;
            uint32_t saved = fp[0];
            serial_puts("[VA]   frame ");
            serial_putdec(f);
            serial_puts(" args:");
            for (int a = 2; a <= 6; a++) {
                serial_puts(" [+"); serial_puthex((uint32_t)(a * 4), 2);
                serial_puts("]=0x"); serial_puthex(fp[a], 8);
            }
            serial_puts("\n");

            /* Per disasm of Core.dll FArray::Realloc @ 0x1014A4A0:
             *   1014a4bd: mov esi, ecx        (save this in ESI)
             *   1014a4da: mov [ebp-0x18], esi (spill this to stack)
             * So frame 1's saved `this` (FArray *) lives at [EBP-0x18].
             * Dump it + this->{+0..+0x10} to see the FArray fields. */
            if (f == 1 || f == 2) {
                int32_t *neg = (int32_t *)(uintptr_t)cur;
                /* fp[i] = ebp + i*4. neg[-i] = ebp - i*4. */
                serial_puts("[VA]   frame ");
                serial_putdec(f);
                serial_puts(" locals:");
                for (int n = 1; n <= 8; n++) {
                    serial_puts(" [-"); serial_puthex((uint32_t)(n * 4), 2);
                    serial_puts("]=0x"); serial_puthex((uint32_t)*(neg - n), 8);
                }
                serial_puts("\n");

                /* If [EBP-0x18] looks like a heap pointer, dump *this[0..+0x14] */
                uint32_t this_ptr = (uint32_t)*(neg - 6); /* -0x18 / 4 = -6 */
                if (this_ptr >= 0x100000 && this_ptr < 0x80000000) {
                    uint32_t *t = (uint32_t *)(uintptr_t)this_ptr;
                    serial_puts("[VA]   frame ");
                    serial_putdec(f);
                    serial_puts(" *this@0x"); serial_puthex(this_ptr, 8);
                    serial_puts(":");
                    for (int i = 0; i < 6; i++) {
                        serial_puts(" [+"); serial_puthex((uint32_t)(i * 4), 2);
                        serial_puts("]=0x"); serial_puthex(t[i], 8);
                    }
                    serial_puts("\n");
                }
            }
            if (saved <= cur) break;
            cur = saved;
        }

        /* Dump user-mode regs at INT 0x2E entry. ECX = `this` for any
         * __thiscall method. If the bad NewSize is computed inside
         * FArray::Realloc as Num*ElementSize, then this->Num and
         * this->ElementSize live in the FArray struct that ECX points
         * to. UE1 FArray is { void* Data; INT Num; INT Max; }, with
         * ElementSize stored separately by the templated TArray<T>. */
        extern uint32_t compat32_get_last_user_ecx(void);
        extern uint32_t compat32_get_last_user_edx(void);
        extern uint32_t compat32_get_last_user_esi(void);
        extern uint32_t compat32_get_last_user_edi(void);
        extern uint32_t compat32_get_last_user_ebx(void);
        uint32_t ecx = compat32_get_last_user_ecx();
        uint32_t edx = compat32_get_last_user_edx();
        uint32_t esi = compat32_get_last_user_esi();
        uint32_t edi = compat32_get_last_user_edi();
        uint32_t ebx = compat32_get_last_user_ebx();
        serial_puts("[VA]   user regs: EBX=0x"); serial_puthex(ebx, 8);
        serial_puts(" ECX=0x"); serial_puthex(ecx, 8);
        serial_puts(" EDX=0x"); serial_puthex(edx, 8);
        serial_puts(" ESI=0x"); serial_puthex(esi, 8);
        serial_puts(" EDI=0x"); serial_puthex(edi, 8);
        serial_puts("\n");

        /* If ECX (this) looks like a valid pointer in heap range,
         * dump the first 32 bytes — that's enough to see Data/Num/Max
         * and any extra TArray fields. */
        if (ecx >= 0x100000 && ecx < 0x80000000) {
            uint32_t *t = (uint32_t *)(uintptr_t)ecx;
            serial_puts("[VA]   *ECX:");
            for (int i = 0; i < 8; i++) {
                serial_puts(" ["); serial_putdec((uint64_t)i);
                serial_puts("]=0x"); serial_puthex(t[i], 8);
            }
            serial_puts("\n");
        }

        /* Dump user stack from RSP_user — first 16 dwords = 64 bytes.
         * That's the args + saved EBP + ret + outer args. */
        extern uint32_t compat32_get_last_stack_args(void);
        uint32_t sa = compat32_get_last_stack_args();
        if (sa >= 0x100000 && sa < 0xFFFE0000 && (sa & 3) == 0) {
            uint32_t *s = (uint32_t *)(uintptr_t)sa;
            serial_puts("[VA]   user stack@0x"); serial_puthex(sa, 8);
            serial_puts(":");
            for (int i = 0; i < 16; i++) {
                if (i % 4 == 0) { serial_puts("\n[VA]    +"); serial_puthex((uint32_t)(i * 4), 2); serial_puts(":"); }
                serial_puts(" 0x"); serial_puthex(s[i], 8);
            }
            serial_puts("\n");
        }
    }

    /* Cap absurd sizes (> 256MB) to 256MB. Empirical sweet spot vs
     * NULL/16MB/1GB. Documented in commit log. The follow-on rep-
     * movsl that uses this buffer with a corrupt 2GB-class ECX gets
     * short-circuited by the #PF handler in idt.c (see VA-SHORT) when
     * the writes overrun the 256MB into unmapped pages — but only IF
     * those pages aren't already covered by the kernel direct-map.
     * In practice they ARE covered (winexec keeps PE32 under kernel
     * CR3, which has the low-memory identity map), so the rep-movsl
     * runs to completion through valid-but-irrelevant memory.
     *
     * Workaround: also pre-poison the buffer with a single byte at
     * the END of the cap so the engine's checksum/comparison loop
     * sees a sentinel — or simpler, just keep the cap and accept the
     * slow run. UT99 eventually completes the rep-movsl and moves on. */
    if (g_compat32_mode && g_compat32_ut99 && dwSize > 0x10000000ULL) {
        static int cap_log = 0;
        if (cap_log < 5) {
            serial_puts("[VA] Capped: 0x");
            serial_puthex(dwSize, 8);
            serial_puts(" -> 64KB sentinel\n");
            cap_log++;
        }

        /* W4-FArray-FIX: walk the EBP chain to find the FArray *this and
         * patch its corrupted {+8} field (Max/ElementSize). Disasm of
         * Core.dll FArray::Realloc proved:
         *   - Frame 2 of the EBP chain is FArray::Realloc itself
         *   - Its [ebp-0x18] holds the FArray *this (saved esi)
         *   - The bad NewSize is `this->{+8} * NewMax` via imul
         *   - `this->{+8}` contains a code pointer (uninitialized stack)
         *
         * Patch this->{+8} = 2 (assume wchar_t TArray, the most common
         * UE1 use case) so subsequent reallocs of the same TArray compute
         * a sane size instead of code_ptr * NewMax. */
        {
            extern uint32_t compat32_get_last_user_ebp(void);
            extern uint32_t compat32_get_last_user_ecx(void);
            extern uint32_t compat32_get_last_user_esi(void);
            extern uint32_t compat32_get_last_user_edi(void);
            extern uint32_t compat32_get_last_user_ebx(void);
            uint32_t walk = compat32_get_last_user_ebp();
            int patched = 0;

            /* Direct user-reg check before walking the EBP chain.  In
             * Core.dll FArray::Realloc, ECX/ESI both held the FArray
             * *this on entry; ESI/EDI/EBX are callee-saved across the
             * intermediate calls to FMallocWindows::Realloc → Malloc →
             * VirtualAlloc, so they typically still point at the
             * corrupt FArray on shim entry.  Engine.dll 0x1033E7D0
             * additionally has the FArray at `EBX + 0xC`. */
            uint32_t ebx = compat32_get_last_user_ebx();
            uint32_t cand_regs[5];
            cand_regs[0] = compat32_get_last_user_esi();
            cand_regs[1] = compat32_get_last_user_edi();
            cand_regs[2] = compat32_get_last_user_ecx();
            cand_regs[3] = ebx;
            cand_regs[4] = (ebx >= 0x100000 && ebx < 0xFFFE0000)
                           ? ebx + 0xC : 0;
            for (int r = 0; r < 5 && !patched; r++) {
                if (try_patch_farray(cand_regs[r], 0, &dwSize, "user-reg")) {
                    patched = 1;
                }
            }
            if (patched) goto va_proceed;
            /* Walk multiple depths AND multiple [ebp-N] offsets — any
             * frame on the chain might be FArray::Realloc, and inside it
             * `this` is saved at some negative-offset local. Try common
             * MSVC compiler offsets [-0x10..-0x28] (esi-spill in
             * SEH-decorated functions). For each candidate, check if
             * *(this+8) is a code ptr → patch to 2. */
            /* EBP walk: try locals [-0x10..-0x40] across up to 10 frames.
             * Each candidate goes through try_patch_farray for the same
             * tight {+8}∈[0x10000000,0x12000000) check and unified
             * {+8}=4 + 256KB buffer policy. */
            for (int depth = 0; depth < 10 && walk >= 0x100000 &&
                 walk < 0xFFFE0000 && (walk & 3) == 0; depth++) {
                int32_t *neg = (int32_t *)(uintptr_t)walk;
                /* NewMax hint = [ebp+8] = first stack arg of this frame */
                uint32_t newmax = ((uint32_t *)(uintptr_t)walk)[2];
                for (int local_off = 4; local_off <= 16 && !patched; local_off++) {
                    uint32_t cand = (uint32_t)*(neg - local_off);
                    if (try_patch_farray(cand, newmax, &dwSize, "ebp-walk")) {
                        patched = 1;
                    }
                }
                if (patched) goto va_proceed;
                /* advance to next frame */
                uint32_t *fp = (uint32_t *)(uintptr_t)walk;
                uint32_t next = fp[0];
                if (next <= walk) break;  /* not strictly increasing */
                walk = next;
            }
        }
        /* Cap to 64KB instead of 256MB. Reasons:
         *   - 256MB cap exhausted the 896MB VA range after 3-4 bogus
         *     FArray::Realloc requests, all subsequent VirtualAlloc
         *     returned NULL → STATUS_NO_MEMORY → ERROR_NOT_ENOUGH_MEMORY
         *     → UT99 appError loop.
         *   - The rep-movsl that follows was already patched to no-op
         *     (commit 02f9ca6) so the engine never actually writes 2GB.
         *   - 64KB is enough for the engine's metadata reads (it
         *     might read first few elements after Realloc to check
         *     existing-data preservation). Reads past 64KB will
         *     fault, hit demand-paging — but we don't allocate
         *     beyond, so faults page-fault back to a NULL handler
         *     and trigger NULL-CALL recovery (controlled).
         */
        dwSize = 0x10000;  /* 64KB sentinel */
        was_capped = 1;    /* triggers VA-CACHE STORE post-alloc */
        /* Self-modify the engine's memcpy helper at 0x1010723E so that
         * the upcoming bogus 2GB rep-movsl terminates instantly. */
        static int patched_memcpy = 0;
        if (!patched_memcpy) {
            volatile uint8_t *p = (uint8_t *)(uintptr_t)0x1010723E;
            if (p[0] == 0xF3 && p[1] == 0xA5) {
                p[0] = 0x31;  /* xor ecx, ecx */
                p[1] = 0xC9;
                patched_memcpy = 1;
                serial_puts("[VA] PATCHED rep-movsl @0x1010723E -> xor ecx,ecx\n");
            }
        }
        /* (Tried also patching the count read at 0x1033E7F5 to
         * `xor eax,eax; nop` — didn't help because the same NULL-call
         * cascade hits via a parallel path that doesn't go through
         * 0x1033E7D0. Reverting that patch; it may break legitimate
         * uses of 0x1033E7D0 elsewhere.) */
    }
va_proceed:

    PVOID base = lpAddress;
    SIZE_T size = dwSize;

    NTSTATUS status = NtAllocateVirtualMemory(
        NT_CURRENT_PROCESS, &base, 0, &size,
        flAllocationType, flProtect);

    if (!NT_SUCCESS(status)) {
        serial_puts("[VA] FAILED: size=0x");
        serial_puthex(dwSize, 8);
        serial_puts(" addr=0x");
        serial_puthex((uint64_t)(ULONG_PTR)lpAddress, 16);
        serial_puts(" type=0x");
        serial_puthex(flAllocationType, 8);
        serial_puts(" protect=0x");
        serial_puthex(flProtect, 8);
        serial_puts(g_compat32_mode ? " mode=32" : " mode=64");
        serial_puts(" status=0x");
        serial_puthex(status, 8);
        serial_puts("\n");
        set_last_error_from_status(status);
        return NULL;
    }

    /* VA-CACHE store: only when the original request was bogus and we
     * served it from the cap fallback.  Subsequent requests from this
     * EIP will short-circuit to the cached `base` (see lookup above). */
    if (was_capped && cache_eip_save && base) {
        for (int i = 0; i < VA_CACHE_N; i++) {
            if (!va_cache[i].eip) {
                va_cache[i].eip  = cache_eip_save;
                va_cache[i].base = base;
                va_cache[i].size = size;
                va_cache[i].hits = 1;
                serial_puts("[VA] cache STORE eip=0x");
                serial_puthex(cache_eip_save, 8);
                serial_puts(" base=0x");
                serial_puthex((uint64_t)(ULONG_PTR)base, 8);
                serial_puts(" (slot=");
                serial_putdec((uint64_t)i);
                serial_puts(")\n");
                break;
            }
        }
    }

    return base;
}

BOOL WINAPI VirtualFree(PVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    /* MEM_RELEASE (0x8000) DOES reclaim the VA range now.  The old no-op
     * comment said this avoided FName::Names use-after-free during
     * error cleanup.  But with VA-CACHE deduping LARGE bogus allocs,
     * the dominant VA consumer is now REAL engine asset loads (textures,
     * sounds, meshes — 4-14 MB each).  Without releasing those, UT99
     * hits VA-exhaust (STATUS_NO_MEMORY) on a 1.6 MB request well
     * before reaching gameplay.
     *
     * Tradeoff: if engine error-path accesses freed FName data, we'll
     * see a NULL-deref later.  Mitigated by the high NULL-CALL recovery
     * cap (5000); compared to guaranteed VA exhaust, this is the better
     * failure mode. */
    PVOID base = lpAddress;
    SIZE_T size = dwSize;

    NTSTATUS status = NtFreeVirtualMemory(
        NT_CURRENT_PROCESS, &base, &size, dwFreeType);

    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }

    return TRUE;
}

static BOOL WINAPI ReadProcessMemory_k32(HANDLE process,
                                          const void *base_address,
                                          PVOID buffer, SIZE_T size,
                                          SIZE_T *bytes_read)
{
    SIZE_T read = 0;
    NTSTATUS status = NtReadVirtualMemory(process, (PVOID)base_address,
                                           buffer, size, &read);

    if (bytes_read) {
        /* A PE32 caller supplies a 32-bit SIZE_T even though the shim itself
         * is compiled for x86-64.  Do not overwrite the adjacent argument. */
        if (g_compat32_mode)
            *(uint32_t *)bytes_read = (uint32_t)read;
        else
            *bytes_read = read;
    }

    static uint32_t log_count;
    if (!NT_SUCCESS(status) || log_count < 32) {
        serial_puts("[K32-RPM] process=0x");
        serial_puthex((uint64_t)(ULONG_PTR)process, 16);
        serial_puts(" source=0x");
        serial_puthex((uint64_t)(ULONG_PTR)base_address, 16);
        serial_puts(" dest=0x");
        serial_puthex((uint64_t)(ULONG_PTR)buffer, 16);
        serial_puts(" size=0x");
        serial_puthex(size, 16);
        serial_puts(" read=0x");
        serial_puthex(read, 16);
        serial_puts(" status=0x");
        serial_puthex((uint32_t)status, 8);
        serial_puts("\n");
        if (log_count < 32) log_count++;
    }

    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI WriteProcessMemory_k32(HANDLE process, PVOID address,
                                           PCVOID buffer, SIZE_T size,
                                           SIZE_T *bytes_written)
{
    SIZE_T written = 0;
    NTSTATUS status = NtWriteVirtualMemory(process, address, buffer, size,
                                            &written);
    if (bytes_written) {
        if (g_compat32_mode)
            *(uint32_t *)bytes_written = (uint32_t)written;
        else
            *bytes_written = written;
    }
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

static PVOID WINAPI VirtualAllocEx_k32(HANDLE process, PVOID address,
                                        SIZE_T size, DWORD allocation_type,
                                        DWORD protect)
{
    if (!k32_process_handle_valid(process)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return NULL;
    }
    return VirtualAlloc(address, size, allocation_type, protect);
}

static BOOL WINAPI VirtualFreeEx_k32(HANDLE process, PVOID address,
                                      SIZE_T size, DWORD free_type)
{
    if (!k32_process_handle_valid(process)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    return VirtualFree(address, size, free_type);
}

static BOOL WINAPI FlushInstructionCache_k32(HANDLE process,
                                               PCVOID address, SIZE_T size)
{
    (void)address;
    (void)size;
    if (!k32_process_handle_valid(process)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    __asm__ volatile ("mfence" ::: "memory");
    return TRUE;
}

static DWORD WINAPI DiscardVirtualMemory_k32(PVOID address, SIZE_T size)
{
    (void)address;
    (void)size;
    return 0; /* ERROR_SUCCESS; discarded contents are undefined by contract. */
}

static BOOL WINAPI PrefetchVirtualMemory_k32(HANDLE process,
                                              ULONG_PTR entry_count,
                                              PCVOID ranges, ULONG flags)
{
    (void)entry_count;
    (void)ranges;
    (void)flags;
    if (!k32_process_handle_valid(process)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    return TRUE;
}

/* ── Memory-Mapped File API ────────────────────────────────── */

/*
 * CreateFileMappingA/W → NtCreateSection
 * MapViewOfFile → NtMapViewOfSection
 * UnmapViewOfFile → NtUnmapViewOfSection (stub)
 */

/* SEC_* constants for CreateFileMapping's flProtect high bits */
#define K32_SEC_COMMIT   0x8000000
#define K32_SEC_IMAGE    0x1000000
#define K32_SEC_RESERVE  0x4000000

#define K32_MAX_NAMED_MAPPINGS K32_MAX_NAMED_OBJECTS
static K32_NAMED_OBJECT named_mappings[K32_MAX_NAMED_MAPPINGS];

typedef struct {
    DWORD length;
    PVOID security_descriptor;
    BOOL inherit_handle;
} K32_SECURITY_ATTRIBUTES64;

static PVOID file_mapping_security_descriptor(PVOID attributes)
{
    if (!attributes)
        return NULL;
    if (g_compat32_mode) {
        const DWORD *fields = (const DWORD *)attributes;
        return fields[0] >= 12 ? (PVOID)(ULONG_PTR)fields[1] : NULL;
    }

    const K32_SECURITY_ATTRIBUTES64 *sa =
        (const K32_SECURITY_ATTRIBUTES64 *)attributes;
    return sa->length >= sizeof(*sa) ? sa->security_descriptor : NULL;
}

static HANDLE create_file_mapping_k32(HANDLE hFile, PVOID mapping_attributes,
                                      DWORD flProtect,
                                      DWORD dwMaximumSizeHigh,
                                      DWORD dwMaximumSizeLow,
                                      const char *name)
{
    BOOL trace = K32_STEAMIPC_SERIAL_TRACE &&
                 named_object_is_steamchrome(name);
    if (trace) {
        serial_puts("[K32-STEAMIPC] CreateFileMapping pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" name='");
        serial_puts(name);
        serial_puts("' size=0x");
        serial_puthex(((uint64_t)dwMaximumSizeHigh << 32) | dwMaximumSizeLow,
                      16);
        serial_puts(" protect=0x");
        serial_puthex(flProtect, 8);
        serial_puts("\n");
    }
    if (name && *name) {
        HANDLE existing = named_object_open(
            named_mappings, K32_MAX_NAMED_MAPPINGS, OBJ_TYPE_SECTION,
            name, GENERIC_ALL);
        if (existing) {
            SetLastError(183); /* ERROR_ALREADY_EXISTS */
            if (trace) {
                serial_puts("[K32-STEAMIPC] mapping existing handle=0x");
                serial_puthex((ULONG_PTR)existing, 16);
                serial_puts(" last_error=183\n");
            }
            return existing;
        }
    }

    extern NTSTATUS sys_NtCreateSection(ULONG_PTR *args);

    LARGE_INTEGER max_size;
    max_size.QuadPart = ((LONGLONG)dwMaximumSizeHigh << 32) | dwMaximumSizeLow;

    /* Extract SEC_* flags from high bits of flProtect */
    ULONG alloc_attrs = K32_SEC_COMMIT;
    if (flProtect & K32_SEC_IMAGE)
        alloc_attrs = K32_SEC_IMAGE;
    else if (flProtect & K32_SEC_RESERVE)
        alloc_attrs = K32_SEC_RESERVE;

    /* Low bits of flProtect are PAGE_* constants */
    ULONG page_prot = flProtect & 0xFF;

    HANDLE section = NULL;
    ULONG_PTR file_value = (ULONG_PTR)hFile;
    HANDLE file_h = (file_value == (ULONG_PTR)INVALID_HANDLE_VALUE ||
                     file_value == 0xFFFFFFFFU) ? NULL : hFile;

    OBJECT_ATTRIBUTES object_attributes;
    POBJECT_ATTRIBUTES object_attributes_ptr = NULL;
    PVOID security_descriptor =
        file_mapping_security_descriptor(mapping_attributes);
    if (security_descriptor) {
        InitializeObjectAttributes(&object_attributes, NULL, 0, NULL,
                                   security_descriptor);
        object_attributes_ptr = &object_attributes;
    }

    ULONG_PTR args[7] = {
        (ULONG_PTR)&section, (ULONG_PTR)GENERIC_ALL,
        (ULONG_PTR)object_attributes_ptr, (ULONG_PTR)&max_size,
        (ULONG_PTR)page_prot, (ULONG_PTR)alloc_attrs,
        (ULONG_PTR)file_h
    };

    NTSTATUS status = sys_NtCreateSection(args);
    if (!NT_SUCCESS(status)) {
        if (trace) {
            serial_puts("[K32-STEAMIPC] mapping create failed status=0x");
            serial_puthex((uint32_t)status, 8);
            serial_puts("\n");
        }
        set_last_error_from_status(status);
        return NULL;
    }

    if (name && *name) {
        BOOL already_exists = FALSE;
        HANDLE published = named_object_publish(
            named_mappings, K32_MAX_NAMED_MAPPINGS, OBJ_TYPE_SECTION,
            name, section, &already_exists);
        if (!published) {
            CloseHandle(section);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        if (already_exists) {
            CloseHandle(section);
            SetLastError(183); /* ERROR_ALREADY_EXISTS */
            if (trace) {
                serial_puts("[K32-STEAMIPC] mapping won publish race handle=0x");
                serial_puthex((ULONG_PTR)published, 16);
                serial_puts(" last_error=183\n");
            }
            return published;
        }
        section = published;
    }
    SetLastError(0);
    if (trace) {
        serial_puts("[K32-STEAMIPC] mapping created handle=0x");
        serial_puthex((ULONG_PTR)section, 16);
        serial_puts(" last_error=0\n");
    }
    return section;
}

HANDLE WINAPI CreateFileMappingA(HANDLE hFile, PVOID lpFileMappingAttributes,
                                 DWORD flProtect, DWORD dwMaximumSizeHigh,
                                 DWORD dwMaximumSizeLow, PCSTR lpName)
{
    char name[K32_OBJECT_NAME_MAX];
    if (!named_object_name_a(lpName, name)) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NULL;
    }
    return create_file_mapping_k32(hFile, lpFileMappingAttributes, flProtect,
                                   dwMaximumSizeHigh, dwMaximumSizeLow, name);
}

HANDLE WINAPI CreateFileMappingW(HANDLE hFile, PVOID lpFileMappingAttributes,
                                 DWORD flProtect, DWORD dwMaximumSizeHigh,
                                 DWORD dwMaximumSizeLow, PCWSTR lpName)
{
    char name[K32_OBJECT_NAME_MAX];
    if (!named_object_name_w(lpName, name)) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NULL;
    }
    return create_file_mapping_k32(hFile, lpFileMappingAttributes, flProtect,
                                   dwMaximumSizeHigh, dwMaximumSizeLow, name);
}

HANDLE WINAPI OpenFileMappingA(DWORD desired_access, BOOL inherit_handle,
                               PCSTR mapping_name)
{
    (void)inherit_handle;
    char name[K32_OBJECT_NAME_MAX];
    if (!mapping_name || !named_object_name_a(mapping_name, name)) {
        SetLastError(mapping_name ? 206 : 87);
        return NULL;
    }
    HANDLE handle = named_object_open(named_mappings, K32_MAX_NAMED_MAPPINGS,
                                      OBJ_TYPE_SECTION, name, desired_access);
    static uint32_t steamchrome_miss_traces;
    if (K32_STEAMIPC_SERIAL_TRACE &&
        named_object_is_steamchrome(name) &&
        (handle || steamchrome_miss_traces++ < 32)) {
        serial_puts("[K32-STEAMIPC] OpenFileMappingA pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" name='");
        serial_puts(name);
        serial_puts("' handle=0x");
        serial_puthex((ULONG_PTR)handle, 16);
        serial_puts(" last_error=");
        serial_putdec(GetLastError());
        serial_puts("\n");
    }
    return handle;
}

HANDLE WINAPI OpenFileMappingW(DWORD desired_access, BOOL inherit_handle,
                               PCWSTR mapping_name)
{
    (void)inherit_handle;
    char name[K32_OBJECT_NAME_MAX];
    if (!mapping_name || !named_object_name_w(mapping_name, name)) {
        SetLastError(mapping_name ? 206 : 87);
        return NULL;
    }
    HANDLE handle = named_object_open(named_mappings, K32_MAX_NAMED_MAPPINGS,
                                      OBJ_TYPE_SECTION, name, desired_access);
    static uint32_t steamchrome_miss_traces;
    if (K32_STEAMIPC_SERIAL_TRACE &&
        named_object_is_steamchrome(name) &&
        (handle || steamchrome_miss_traces++ < 32)) {
        serial_puts("[K32-STEAMIPC] OpenFileMappingW pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" name='");
        serial_puts(name);
        serial_puts("' handle=0x");
        serial_puthex((ULONG_PTR)handle, 16);
        serial_puts(" last_error=");
        serial_putdec(GetLastError());
        serial_puts("\n");
    }
    return handle;
}

PVOID WINAPI MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
                           DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow,
                           SIZE_T dwNumberOfBytesToMap)
{
    extern NTSTATUS sys_NtMapViewOfSection(ULONG_PTR *args);

    const char *mapping_name = named_object_name_for_handle(
        named_mappings, K32_MAX_NAMED_MAPPINGS, OBJ_TYPE_SECTION,
        hFileMappingObject);
    BOOL trace = K32_STEAMIPC_SERIAL_TRACE &&
                 named_object_is_steamchrome(mapping_name);

    PVOID base = NULL;
    SIZE_T view_size = dwNumberOfBytesToMap;

    LARGE_INTEGER offset;
    offset.QuadPart = ((LONGLONG)dwFileOffsetHigh << 32) | dwFileOffsetLow;

    /* Map Win32 access flags to NT protection:
     * FILE_MAP_READ = SECTION_MAP_READ (0x4)
     * FILE_MAP_WRITE = SECTION_MAP_WRITE (0x2)
     * FILE_MAP_ALL_ACCESS = SECTION_ALL_ACCESS */
    ULONG prot = PAGE_READONLY;
    if (dwDesiredAccess & 0x2) /* FILE_MAP_WRITE */
        prot = PAGE_READWRITE;

    ULONG_PTR args[10] = {
        (ULONG_PTR)hFileMappingObject, (ULONG_PTR)NT_CURRENT_PROCESS,
        (ULONG_PTR)&base, (ULONG_PTR)0,      /* ZeroBits */
        (ULONG_PTR)0,                          /* CommitSize */
        (ULONG_PTR)&offset,                    /* SectionOffset */
        (ULONG_PTR)&view_size,                 /* ViewSize */
        (ULONG_PTR)1,                          /* ViewShare */
        (ULONG_PTR)0,                          /* AllocationType */
        (ULONG_PTR)prot                        /* Win32Protect */
    };

    NTSTATUS status = sys_NtMapViewOfSection(args);
    if (!NT_SUCCESS(status)) {
        if (trace) {
            serial_puts("[K32-STEAMIPC] MapViewOfFile failed pid=");
            serial_putdec(win32_current_process_id());
            serial_puts(" name='");
            serial_puts(mapping_name);
            serial_puts("' status=0x");
            serial_puthex((uint32_t)status, 8);
            serial_puts("\n");
        }
        set_last_error_from_status(status);
        return NULL;
    }

    if (trace) {
        serial_puts("[K32-STEAMIPC] MapViewOfFile pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" name='");
        serial_puts(mapping_name);
        serial_puts("' base=0x");
        serial_puthex((ULONG_PTR)base, 16);
        serial_puts(" size=0x");
        serial_puthex(view_size, 16);
        serial_puts("\n");
    }

    return base;
}

BOOL WINAPI UnmapViewOfFile(PCVOID lpBaseAddress)
{
    extern NTSTATUS sys_NtUnmapViewOfSection(ULONG_PTR *args);

    ULONG_PTR args[2] = {
        (ULONG_PTR)NT_CURRENT_PROCESS, (ULONG_PTR)lpBaseAddress
    };

    NTSTATUS status = sys_NtUnmapViewOfSection(args);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }

    return TRUE;
}

static BOOL WINAPI FlushViewOfFile_k32(PCVOID base_address,
                                        SIZE_T bytes_to_flush)
{
    if (!base_address) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    extern NTSTATUS nt_flush_view_of_section(PVOID base_address,
                                              SIZE_T bytes_to_flush);
    NTSTATUS status = nt_flush_view_of_section((PVOID)base_address,
                                               bytes_to_flush);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

/* ── Heap API (bump allocator with size headers) ────────────── */
/*
 * HeapReAlloc/HeapSize expect a size header before the user pointer.
 * HeapAlloc MUST write this header so realloc can copy the right amount.
 * Without it, HeapReAlloc reads garbage as old_size → data loss on grow.
 * This was the root cause of UE1 FName::Names corruption: TArray::Realloc
 * called appRealloc → HeapReAlloc, which failed to copy old entries.
 */

/* Win32 heap: dynamic size from sys_caps (scales with RAM).
 * Allocated lazily on first HeapAlloc call via kmalloc.
 * Falls back to 16MB static pool if kmalloc unavailable. */
#include "../include/sys_caps.h"

/* Heap pool allocated dynamically via kmalloc (no static fallback).
 * The 64MB static array was causing 134MB BSS and crashing NVMe boot. */
static BYTE  *heap_pool = NULL;
static SIZE_T heap_pool_size = 0;
static SIZE_T heap_offset = 0;
static volatile uint32_t heap_lock = 0;
static volatile uint32_t heap_lock_owner = 0;
static uint32_t heap_lock_depth = 0;

#define HEAP_HEADER_SIZE ((SIZE_T)16)
#define HEAP_ALLOC_MAGIC 0x4F5349544F484541ULL

typedef struct {
    SIZE_T size;
    uint64_t tag;
} heap_header_t;

_Static_assert(sizeof(heap_header_t) == HEAP_HEADER_SIZE,
               "Win32 heap header must remain 16 bytes");

static uint64_t heap_allocation_tag(const BYTE *block, SIZE_T size)
{
    return HEAP_ALLOC_MAGIC ^ (uint64_t)(uintptr_t)block ^ (uint64_t)size;
}

static void heap_mark_allocated(BYTE *block, SIZE_T size)
{
    heap_header_t *header = (heap_header_t *)block;
    header->size = size;
    header->tag = heap_allocation_tag(block, size);
}

static BOOL heap_block_is_allocated(const BYTE *block, SIZE_T available)
{
    const heap_header_t *header = (const heap_header_t *)block;
    SIZE_T size = header->size;

    return size >= 32 && !(size & 15) && size <= available &&
           header->tag == heap_allocation_tag(block, size);
}

/* ponytail: 32 chunks cover this 8 GB guest; make dynamic for larger guests. */
#define HEAP_MAX_CHUNKS 32
typedef struct {
    BYTE *base;
    SIZE_T size;
} heap_chunk_t;
static heap_chunk_t heap_chunks[HEAP_MAX_CHUNKS];
static uint32_t heap_chunk_count;

static BYTE *heap_alloc_chunk(SIZE_T size)
{
    extern void *mem_alloc_pages(uint64_t count);
    extern void mem_free_pages(void *addr, uint64_t count);
    uint64_t pages = (size + 4095) / 4096;
    void *phys = mem_alloc_pages(pages);

    if (!phys) return NULL;
    if ((uint64_t)phys + pages * 4096 > 0x100000000ULL) {
        mem_free_pages(phys, pages);
        return NULL;
    }
    return (BYTE *)PHYS_TO_VIRT(phys);
}

static void heap_lock_acquire(void)
{
    extern void sched_yield(void);
    uint32_t owner = (uint32_t)proc_current_pid() + 1;

    if (__atomic_load_n(&heap_lock, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&heap_lock_owner, __ATOMIC_RELAXED) == owner) {
        heap_lock_depth++;
        return;
    }

    while (__sync_lock_test_and_set(&heap_lock, 1)) {
        for (int spin = 0; spin < 100; spin++)
            __asm__ volatile ("pause" ::: "memory");
        sched_yield();
    }
    __atomic_store_n(&heap_lock_owner, owner, __ATOMIC_RELAXED);
    heap_lock_depth = 1;
}

static void heap_lock_release(void)
{
    uint32_t owner = (uint32_t)proc_current_pid() + 1;

    if (__atomic_load_n(&heap_lock_owner, __ATOMIC_RELAXED) != owner ||
        !heap_lock_depth)
        return;
    if (--heap_lock_depth)
        return;
    __atomic_store_n(&heap_lock_owner, 0, __ATOMIC_RELAXED);
    __sync_lock_release(&heap_lock);
}

static BOOL heap_lock_owned_by_current(void)
{
    uint32_t owner = (uint32_t)proc_current_pid() + 1;
    return __atomic_load_n(&heap_lock, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&heap_lock_owner, __ATOMIC_RELAXED) == owner &&
           heap_lock_depth != 0;
}

/* Simple free-list for HeapFree.
 * Free blocks are stored as a linked list: [size(8)] [next_ptr(8)] [padding...]
 * Sorted by address so adjacent blocks can be coalesced. */
typedef struct free_node {
    SIZE_T size;           /* block size including header */
    struct free_node *next;
} free_node_t;

static free_node_t *free_list = NULL;

/* Child processes run under private CR3s, so the process heap must use
 * process VMAs rather than low aliases of the kernel's physical heap. */
#define HEAP_PROCESS_STATE_MAX 128
#define HEAP_PROCESS_INITIAL_SIZE (1024 * 1024)
#define HEAP_PROCESS_GROW_LIMIT (16 * 1024 * 1024)

typedef struct {
    BOOL used;
    DWORD process_id;
    BYTE *pool;
    SIZE_T pool_size;
    SIZE_T offset;
    free_node_t *free_list;
    heap_chunk_t chunks[HEAP_MAX_CHUNKS];
    uint32_t chunk_count;
} heap_process_state_t;

static heap_process_state_t heap_process_states[HEAP_PROCESS_STATE_MAX];

static heap_process_state_t *heap_process_state(DWORD process_id, BOOL create)
{
    heap_process_state_t *free_state = NULL;

    if (process_id <= 1) return NULL;
    for (uint32_t i = 0; i < HEAP_PROCESS_STATE_MAX; i++) {
        heap_process_state_t *state = &heap_process_states[i];
        if (state->used && state->process_id == process_id)
            return state;
        if (!state->used && !free_state)
            free_state = state;
    }
    if (!create || !free_state) return NULL;

    memset(free_state, 0, sizeof(*free_state));
    free_state->used = TRUE;
    free_state->process_id = process_id;
    return free_state;
}

#define HEAP_EVENT_HISTORY_CAP 4096
enum {
    HEAP_EVENT_ALLOC = 1,
    HEAP_EVENT_FREE  = 2,
};

typedef struct {
    PVOID ptr;
    SIZE_T size;
    uint64_t caller;
    uint64_t sequence;
    DWORD pid;
    DWORD tid;
    uint8_t op;
} heap_event_record_t;

static heap_event_record_t heap_event_history[HEAP_EVENT_HISTORY_CAP];
static uint64_t heap_event_sequence;

static void heap_record_event(uint8_t op, PVOID ptr, SIZE_T size,
                              uint64_t caller)
{
    uint64_t sequence = ++heap_event_sequence;
    heap_event_record_t *record =
        &heap_event_history[(sequence - 1) % HEAP_EVENT_HISTORY_CAP];
    record->ptr = ptr;
    record->size = size;
    record->caller = caller;
    record->pid = win32_current_process_id();
    record->tid = GetCurrentThreadId();
    record->op = op;
    record->sequence = sequence;
}

static const heap_event_record_t *heap_find_last_event(PVOID ptr)
{
    const heap_event_record_t *latest = NULL;
    for (uint32_t i = 0; i < HEAP_EVENT_HISTORY_CAP; i++) {
        const heap_event_record_t *record = &heap_event_history[i];
        if (record->ptr == ptr && record->sequence &&
            (!latest || record->sequence > latest->sequence))
            latest = record;
    }
    return latest;
}

typedef struct {
    char data[320];
    uint32_t length;
} heap_log_line_t;

static void heap_log_append_char(heap_log_line_t *line, char ch)
{
    if (line->length + 1 >= sizeof(line->data)) return;
    line->data[line->length++] = ch;
    line->data[line->length] = '\0';
}

static void heap_log_append_str(heap_log_line_t *line, const char *str)
{
    while (*str && line->length + 1 < sizeof(line->data))
        line->data[line->length++] = *str++;
    line->data[line->length] = '\0';
}

static void heap_log_append_hex(heap_log_line_t *line, uint64_t value,
                                uint32_t digits)
{
    static const char hex[] = "0123456789ABCDEF";
    heap_log_append_str(line, "0x");
    if (digits > 16) digits = 16;
    for (uint32_t i = digits; i > 0; i--)
        heap_log_append_char(line, hex[(value >> ((i - 1) * 4)) & 0xF]);
}

static void heap_log_append_dec(heap_log_line_t *line, uint64_t value)
{
    char reversed[21];
    uint32_t count = 0;
    do {
        reversed[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(reversed));
    while (count) heap_log_append_char(line, reversed[--count]);
}

static bool heap_blocks_share_chunk(const BYTE *a, const BYTE *b)
{
    uintptr_t aa = (uintptr_t)a;
    uintptr_t bb = (uintptr_t)b;
    for (uint32_t i = 0; i < heap_chunk_count; i++) {
        uintptr_t base = (uintptr_t)heap_chunks[i].base;
        uintptr_t end = base + heap_chunks[i].size;
        if (aa >= base && aa < end)
            return bb >= base && bb < end;
    }
    return false;
}

static void heap_free_insert(BYTE *block, SIZE_T size)
{
    free_node_t *node = (free_node_t *)block;
    free_node_t *prev = NULL;
    free_node_t *cur = free_list;
    while (cur && (uintptr_t)cur < (uintptr_t)node) {
        prev = cur;
        cur = cur->next;
    }
    if (cur == node) return;

    node->size = size;
    node->next = cur;
    if (prev) prev->next = node;
    else free_list = node;

    if (cur && heap_blocks_share_chunk(block, (BYTE *)cur) &&
        block + node->size == (BYTE *)cur) {
        node->size += cur->size;
        node->next = cur->next;
    }
    if (prev && heap_blocks_share_chunk((BYTE *)prev, block) &&
        (BYTE *)prev + prev->size == block) {
        prev->size += node->size;
        prev->next = node->next;
    }
}

static bool heap_process_blocks_share_chunk(const heap_process_state_t *state,
                                            const BYTE *a, const BYTE *b)
{
    uintptr_t aa = (uintptr_t)a;
    uintptr_t bb = (uintptr_t)b;

    for (uint32_t i = 0; i < state->chunk_count; i++) {
        uintptr_t base = (uintptr_t)state->chunks[i].base;
        uintptr_t end = base + state->chunks[i].size;
        if (aa >= base && aa < end)
            return bb >= base && bb < end;
    }
    return false;
}

static void heap_process_free_insert(heap_process_state_t *state,
                                     BYTE *block, SIZE_T size)
{
    free_node_t *node = (free_node_t *)block;
    free_node_t *prev = NULL;
    free_node_t *cur = state->free_list;

    while (cur && (uintptr_t)cur < (uintptr_t)node) {
        prev = cur;
        cur = cur->next;
    }
    if (cur == node) return;

    node->size = size;
    node->next = cur;
    if (prev) prev->next = node;
    else state->free_list = node;

    if (cur && heap_process_blocks_share_chunk(state, block, (BYTE *)cur) &&
        block + node->size == (BYTE *)cur) {
        node->size += cur->size;
        node->next = cur->next;
    }
    if (prev && heap_process_blocks_share_chunk(state, (BYTE *)prev, block) &&
        (BYTE *)prev + prev->size == block) {
        prev->size += node->size;
        prev->next = node->next;
    }
}

static BYTE *heap_process_block_from_ptr(heap_process_state_t *state,
                                         PCVOID ptr, SIZE_T *available)
{
    uintptr_t addr = (uintptr_t)ptr;
    if (!state || !addr) return NULL;

    for (uint32_t i = 0; i < state->chunk_count; i++) {
        uintptr_t base = (uintptr_t)state->chunks[i].base;
        uintptr_t end = base + state->chunks[i].size;
        if (addr >= base + HEAP_HEADER_SIZE && addr < end) {
            BYTE *block = (BYTE *)addr - HEAP_HEADER_SIZE;
            if (available) *available = end - (uintptr_t)block;
            return block;
        }
    }
    return NULL;
}

static BOOL heap_process_grow(heap_process_state_t *state, SIZE_T minimum)
{
    SIZE_T grow = HEAP_PROCESS_INITIAL_SIZE;

    if (state->chunk_count) {
        grow = state->chunks[state->chunk_count - 1].size;
        if (grow < HEAP_PROCESS_GROW_LIMIT)
            grow *= 2;
    }
    if (grow < minimum)
        grow = (minimum + 4095) & ~(SIZE_T)4095;
    if (state->chunk_count >= HEAP_MAX_CHUNKS)
        return FALSE;

    BYTE *new_pool = (BYTE *)VirtualAlloc(NULL, grow,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!new_pool)
        return FALSE;

    SIZE_T remaining = state->pool_size - state->offset;
    if (state->pool && remaining >= 32)
        heap_process_free_insert(state, state->pool + state->offset,
                                 remaining);

    state->pool = new_pool;
    state->pool_size = grow;
    state->offset = 0;
    state->chunks[state->chunk_count].base = new_pool;
    state->chunks[state->chunk_count].size = grow;
    state->chunk_count++;

    serial_puts("[WIN32-HEAP] process pid=");
    serial_putdec(state->process_id);
    serial_puts(" chunk=0x");
    serial_puthex((uint64_t)(ULONG_PTR)new_pool, 16);
    serial_puts(" size=");
    serial_putdec(grow / 1024);
    serial_puts(" KB\n");
    return TRUE;
}

static PVOID heap_process_alloc(heap_process_state_t *state, DWORD flags,
                                SIZE_T bytes, SIZE_T total, uint64_t caller)
{
    free_node_t **previous = &state->free_list;
    free_node_t *current = state->free_list;

    while (current) {
        if (current->size >= total) {
            *previous = current->next;
            BYTE *block = (BYTE *)current;
            SIZE_T allocated_total = current->size;
            SIZE_T remaining = current->size - total;
            if (remaining >= 32) {
                heap_process_free_insert(state, block + total, remaining);
                allocated_total = total;
            }
            heap_mark_allocated(block, allocated_total);
            PVOID ptr = block + HEAP_HEADER_SIZE;
            heap_record_event(HEAP_EVENT_ALLOC, ptr, allocated_total, caller);
            if (flags & 0x00000008) /* HEAP_ZERO_MEMORY */
                RtlZeroMemory(ptr, bytes);
            return ptr;
        }
        previous = &current->next;
        current = current->next;
    }

    if (!state->pool || state->offset + total > state->pool_size) {
        if (!heap_process_grow(state, total))
            return NULL;
    }

    BYTE *block = state->pool + state->offset;
    state->offset += total;
    heap_mark_allocated(block, total);
    PVOID ptr = block + HEAP_HEADER_SIZE;
    heap_record_event(HEAP_EVENT_ALLOC, ptr, total, caller);
    if (flags & 0x00000008) /* HEAP_ZERO_MEMORY */
        RtlZeroMemory(ptr, bytes);
    return ptr;
}

static BYTE *heap_block_from_ptr(PCVOID ptr, SIZE_T *available)
{
    uintptr_t addr = (uintptr_t)ptr;
    if (!addr) return NULL;

    for (uint32_t i = 0; i < heap_chunk_count; i++) {
        uintptr_t base = (uintptr_t)heap_chunks[i].base;
        uintptr_t end = base + heap_chunks[i].size;
        uintptr_t low = VIRT_TO_PHYS(heap_chunks[i].base);

        if (addr >= low + HEAP_HEADER_SIZE && addr < low + heap_chunks[i].size)
            addr = (uintptr_t)PHYS_TO_VIRT(addr);
        if (addr >= base + HEAP_HEADER_SIZE && addr < end) {
            BYTE *block = (BYTE *)addr - HEAP_HEADER_SIZE;
            if (available) *available = end - (uintptr_t)block;
            return block;
        }
    }
    return NULL;
}

static BYTE *heap_current_block_from_ptr(PCVOID ptr, SIZE_T *available,
                                         heap_process_state_t **process_state)
{
    DWORD process_id = win32_current_process_id();
    if (process_state) *process_state = NULL;

    if (process_id > 1) {
        heap_process_state_t *state = heap_process_state(process_id, FALSE);
        if (process_state) *process_state = state;
        return heap_process_block_from_ptr(state, ptr, available);
    }
    return heap_block_from_ptr(ptr, available);
}

static void heap_release_process_state(DWORD process_id)
{
    if (process_id <= 1) return;

    heap_lock_acquire();
    heap_process_state_t *state = heap_process_state(process_id, FALSE);
    if (state)
        memset(state, 0, sizeof(*state));
    heap_lock_release();
}

static void heap_trace_vprof(const char *op, PVOID ptr, uint64_t caller)
{
#if defined(OK_QUIET) && OK_QUIET
    (void)op;
    (void)ptr;
    (void)caller;
#else
    heap_log_line_t line = {0};
    heap_log_append_str(&line, "[VPROF-HEAP] ");
    heap_log_append_str(&line, op);
    heap_log_append_str(&line, " ptr=");
    heap_log_append_hex(&line, (uint64_t)(ULONG_PTR)ptr, 16);
    heap_log_append_str(&line, " tid=");
    heap_log_append_dec(&line, GetCurrentThreadId());
    heap_log_append_str(&line, " caller=");
    heap_log_append_hex(&line, caller, 16);
    heap_log_append_char(&line, '\n');
    serial_puts(line.data);
#endif
}

static BOOL heap_is_vprof_ptr(PVOID ptr)
{
    SIZE_T available = 0;
    BYTE *block = heap_current_block_from_ptr(ptr, &available, NULL);
    return block && heap_block_is_allocated(block, available) &&
           ((heap_header_t *)block)->size == 0x190;
}

static void heap_trace_invalid(const char *op, PVOID ptr, BYTE *block,
                               uint64_t caller)
{
    static uint32_t invalid_log_count;
    if (invalid_log_count++ >= 64) return;

    PVOID canonical = block ? block + HEAP_HEADER_SIZE : ptr;
    const heap_event_record_t *latest = heap_find_last_event(canonical);
    heap_log_line_t line = {0};
    heap_log_append_str(&line, "[HEAP-INVALID-");
    heap_log_append_str(&line, op);
    heap_log_append_str(&line, "] ptr=");
    heap_log_append_hex(&line, (uint64_t)(ULONG_PTR)ptr, 16);
    heap_log_append_str(&line, " block=");
    heap_log_append_hex(&line, (uint64_t)(ULONG_PTR)block, 16);
    if (block) {
        heap_header_t *header = (heap_header_t *)block;
        heap_log_append_str(&line, " size=");
        heap_log_append_hex(&line, (uint64_t)header->size, 16);
        heap_log_append_str(&line, " tag=");
        heap_log_append_hex(&line, header->tag, 16);
    }
    heap_log_append_str(&line, " pid=");
    heap_log_append_dec(&line, win32_current_process_id());
    heap_log_append_str(&line, " tid=");
    heap_log_append_dec(&line, GetCurrentThreadId());
    heap_log_append_str(&line, " caller=");
    heap_log_append_hex(&line, caller, 16);
    if (latest) {
        heap_log_append_str(&line, " last=");
        heap_log_append_str(&line,
                            latest->op == HEAP_EVENT_ALLOC ? "alloc" : "free");
        heap_log_append_str(&line, " event_size=");
        heap_log_append_hex(&line, (uint64_t)latest->size, 16);
        heap_log_append_str(&line, " event_seq=");
        heap_log_append_dec(&line, latest->sequence);
        heap_log_append_str(&line, " event_tid=");
        heap_log_append_dec(&line, latest->tid);
        heap_log_append_str(&line, " event_caller=");
        heap_log_append_hex(&line, latest->caller, 16);
    } else {
        heap_log_append_str(&line, " last=none");
    }
    heap_log_append_char(&line, '\n');
    serial_puts(line.data);
}

static void heap_pool_init(void)
{
    if (heap_pool) return;
    uint64_t target = g_sys_caps.win32_heap_size;
    if (!target) target = 16ULL * 1024 * 1024;

    heap_pool = heap_alloc_chunk(target);
    if (heap_pool) {
        heap_pool_size = target;
        /* Zero the pool — Windows HeapAlloc returns pages from VirtualAlloc
         * which are always zeroed. PE32 code (TArray, FString) depends on
         * freshly allocated memory being zero-initialized. */
        memset(heap_pool, 0, target);
    } else {
        /* Dynamic alloc failed — try smaller fallback (1MB) */
        heap_pool = heap_alloc_chunk(1024 * 1024);
        heap_pool_size = heap_pool ? (1024 * 1024) : 0;
    }
    if (heap_pool) {
        heap_chunks[heap_chunk_count].base = heap_pool;
        heap_chunks[heap_chunk_count].size = heap_pool_size;
        heap_chunk_count++;
    }
    serial_puts("[WIN32-HEAP] pool=0x");
    serial_puthex((uint64_t)(uintptr_t)heap_pool, 8);
    serial_puts(" size=");
    serial_putdec(heap_pool_size / (1024 * 1024));
    serial_puts(" MB\n");
}

HANDLE WINAPI GetProcessHeap(void)
{
    /* Return a sentinel — we only have one heap */
    return (HANDLE)(ULONG_PTR)0xBEEF0001;
}

PVOID WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes)
{
    (void)hHeap;
    static int heap_log_count = 0;
    uint64_t caller = (uint64_t)__builtin_return_address(0);

    /* ponytail: one process heap; split this lock only if real heaps land. */
    heap_lock_acquire();

    /* Log allocations around UGameEngine size (0x3D8 = 984 bytes) */
    if (dwBytes >= 900 && dwBytes <= 1100) {
        static int ge_alloc_count = 0;
        ge_alloc_count++;
        if (ge_alloc_count <= 20) {
            serial_puts("[HEAP-ALLOC] size=");
            serial_putdec(dwBytes);
            serial_puts(" #");
            serial_putdec(ge_alloc_count);
            serial_puts("\n");
        }
    }

    if (dwBytes > (SIZE_T)-1 - HEAP_HEADER_SIZE - 15) {
        g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        sync_last_error();
        heap_lock_release();
        return NULL;
    }

    /* Keep returned pointers 16-byte aligned for Win64 lock-free structures. */
    SIZE_T total = (dwBytes + HEAP_HEADER_SIZE + 15) & ~(SIZE_T)15;
    if (total < 32) total = 32;  /* min block size for free-list node */

    DWORD process_id = win32_current_process_id();
    if (process_id > 1) {
        heap_process_state_t *state = heap_process_state(process_id, TRUE);
        PVOID ptr = state
            ? heap_process_alloc(state, dwFlags, dwBytes, total, caller)
            : NULL;
        if (!ptr) {
            g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
            sync_last_error();
        } else if (total == 0x1F0) {
            heap_trace_vprof("alloc-process", ptr, caller);
        }
        heap_lock_release();
        return ptr;
    }

    if (!heap_pool) heap_pool_init();

    /* First-fit search in free-list */
    free_node_t **prev = &free_list;
    free_node_t *cur = free_list;
    while (cur) {
        if (cur->size >= total) {
            /* Found a free block that fits — remove from list */
            *prev = cur->next;
            BYTE *block = (BYTE *)cur;
            SIZE_T allocated_total = cur->size;
            /* Split oversized blocks so HeapSize reflects this allocation. */
            SIZE_T remaining = cur->size - total;
            if (remaining >= 32) {
                heap_free_insert(block + total, remaining);
                allocated_total = total;
            }
            heap_mark_allocated(block, allocated_total);
            PVOID ptr = block + HEAP_HEADER_SIZE;
            heap_record_event(HEAP_EVENT_ALLOC, ptr, allocated_total, caller);
            if (dwFlags & 0x00000008) /* HEAP_ZERO_MEMORY */
                RtlZeroMemory(ptr, dwBytes);
            if (total == 0x1F0)
                heap_trace_vprof("alloc-reuse", ptr, caller);
            heap_lock_release();
            return ptr;
        }
        prev = &cur->next;
        cur = cur->next;
    }

    /* No free block found — bump allocate, grow if needed */
    if (heap_offset + total > heap_pool_size) {
        /* Auto-grow: allocate a new chunk from kernel heap */
        uint64_t grow = g_sys_caps.win32_heap_size;
        if (!grow) grow = 64ULL * 1024 * 1024;
        /* Grow by at least the request size */
        if (grow < total) grow = total;

        BYTE *new_pool = NULL;
        /* Try decreasing sizes until kmalloc succeeds */
        uint64_t try_size = grow;
        while (heap_chunk_count < HEAP_MAX_CHUNKS &&
               try_size >= total && try_size >= 1024 * 1024) {
            new_pool = heap_alloc_chunk(try_size);
            if (new_pool) { grow = try_size; break; }
            try_size /= 2;
        }
        if (new_pool) {
            memset(new_pool, 0, grow);
            serial_puts("[HEAP] Auto-grow: +");
            serial_putdec(grow / (1024 * 1024));
            serial_puts(" MB (used ");
            serial_putdec(heap_offset / (1024 * 1024));
            serial_puts("/");
            serial_putdec(heap_pool_size / (1024 * 1024));
            serial_puts(" MB)\n");

            /* Add remaining space from old pool to free-list */
            SIZE_T remaining = heap_pool_size - heap_offset;
            if (remaining >= 32)
                heap_free_insert(heap_pool + heap_offset, remaining);

            /* Switch to new pool */
            heap_pool = new_pool;
            heap_pool_size = grow;
            heap_offset = 0;
            heap_chunks[heap_chunk_count].base = new_pool;
            heap_chunks[heap_chunk_count].size = grow;
            heap_chunk_count++;
        } else {
            serial_puts("[WIN32-HEAP] EXHAUSTED! used=");
            serial_putdec(heap_offset / 1024);
            serial_puts("KB pool=");
            serial_putdec(heap_pool_size / 1024);
            serial_puts("KB req=");
            serial_putdec(total);
            serial_puts("\n");
            g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
            heap_lock_release();
            return NULL;
        }
    }

    BYTE *block = heap_pool + heap_offset;
    heap_offset += total;

    heap_mark_allocated(block, total);
    PVOID ptr = block + HEAP_HEADER_SIZE;
    heap_record_event(HEAP_EVENT_ALLOC, ptr, total, caller);

    if (dwFlags & 0x00000008) /* HEAP_ZERO_MEMORY */
        RtlZeroMemory(ptr, dwBytes);

    if (total == 0x1F0)
        heap_trace_vprof("alloc", ptr, caller);

    /* Log first few allocations to identify heap_pool base address */
    if (heap_log_count < 5) {
        heap_log_count++;
        serial_puts("[HEAP] alloc 0x");
        serial_puthex(dwBytes, 8);
        serial_puts(" -> 0x");
        serial_puthex((uint64_t)(ULONG_PTR)ptr, 16);
        serial_puts(" (pool=0x");
        serial_puthex((uint64_t)(ULONG_PTR)heap_pool, 16);
        serial_puts(")\n");
    }

    /* Log UGameEngine-sized allocations with returned pointer */
    if (dwBytes >= 900 && dwBytes <= 1100) {
        static int ge_result_count = 0;
        ge_result_count++;
        if (ge_result_count <= 10) {
            serial_puts("[HEAP-984] → 0x");
            serial_puthex((uint64_t)(ULONG_PTR)ptr, 8);
            serial_puts("\n");
        }
    }

    heap_lock_release();
    return ptr;
}

BOOL WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, PVOID lpMem)
{
    (void)hHeap;
    (void)dwFlags;

    if (!lpMem) return TRUE;

    heap_lock_acquire();

    /* Block header precedes the user pointer. */
    SIZE_T available = 0;
    heap_process_state_t *process_state = NULL;
    BYTE *block = heap_current_block_from_ptr(lpMem, &available,
                                              &process_state);
    if (!block || !heap_block_is_allocated(block, available)) {
        heap_trace_invalid("FREE", lpMem, block,
                           (uint64_t)__builtin_return_address(0));
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        sync_last_error();
        heap_lock_release();
        return FALSE;
    }
    SIZE_T block_size = ((heap_header_t *)block)->size;
    uint64_t caller = (uint64_t)__builtin_return_address(0);

    if (block_size == 0x1F0)
        heap_trace_vprof("free", lpMem, caller);

    heap_record_event(HEAP_EVENT_FREE, block + HEAP_HEADER_SIZE,
                      block_size, caller);

    /* Address-ordered insertion coalesces adjacent free blocks. */
    ((heap_header_t *)block)->tag = 0;
    if (process_state)
        heap_process_free_insert(process_state, block, block_size);
    else
        heap_free_insert(block, block_size);

    heap_lock_release();
    return TRUE;
}

/* ── Error API ──────────────────────────────────────────────── */

DWORD WINAPI GetLastError(void)
{
    return g_compat32_mode ? compat32_current_teb()->LastErrorValue
                           : win64_current_teb()->LastErrorValue;
}

void WINAPI SetLastError(DWORD dwErrCode)
{
    g_last_error = dwErrCode;
    sync_last_error();
}

static int WINAPI MulDiv_k32(int number, int numerator, int denominator)
{
    if (!denominator) return -1;

    int64_t product = (int64_t)number * numerator;
    int64_t quotient = product / denominator;
    int64_t remainder = product % denominator;
    int64_t abs_remainder = remainder < 0 ? -remainder : remainder;
    int64_t abs_denominator = denominator < 0 ? -(int64_t)denominator : denominator;
    if (abs_remainder * 2 >= abs_denominator)
        quotient += (product < 0) != (denominator < 0) ? -1 : 1;
    if (quotient < -2147483648LL || quotient > 2147483647LL) return -1;
    return (int)quotient;
}

/* ── Misc API ───────────────────────────────────────────────── */

void WINAPI Sleep(DWORD dwMilliseconds)
{
    LARGE_INTEGER delay;
    /* Negative = relative time in 100ns units */
    delay.QuadPart = -(LONGLONG)dwMilliseconds * 10000LL;
    NtDelayExecution(FALSE, &delay);
}

static DWORD WINAPI SleepEx_k32(DWORD dwMilliseconds, BOOL bAlertable)
{
    if (bAlertable && k32_dispatch_io_completions())
        return K32_WAIT_IO_COMPLETION;
    Sleep(dwMilliseconds);
    if (bAlertable && k32_dispatch_io_completions())
        return K32_WAIT_IO_COMPLETION;
    return 0;
}

static volatile uint32_t g_k32_qpc_if0_trace_count;
static volatile uint32_t g_k32_qpc_target_trace_count;

__attribute__((noinline))
static void k32_qpc_trace_target(const char *phase, uint64_t caller)
{
    /* libcef's timed-wait helper, immediately after its QPC import call. */
    if (caller != 0x00000001834FC2DEULL)
        return;

    uint32_t index = __atomic_fetch_add(&g_k32_qpc_target_trace_count, 1,
                                        __ATOMIC_RELAXED);
    if (index >= 128)
        return;

    extern int32_t proc_current_pid(void);
    extern uint64_t sched_current_frame_seq(void);
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) :: "memory");

    serial_puts("[K32-QPC-TARGET] phase=");
    serial_puts(phase);
    serial_puts(" proc=");
    serial_putdec(win32_current_process_id());
    serial_puts(" kpid=");
    serial_putdec((uint64_t)(uint32_t)proc_current_pid());
    serial_puts(" frame_seq=");
    serial_putdec(sched_current_frame_seq());
    serial_puts(" flags=0x");
    serial_puthex(flags, 16);
    serial_puts("\n");
}

__attribute__((noinline))
static void k32_qpc_trace_if0(const char *phase, uint64_t caller)
{
    extern int32_t proc_current_pid(void);
    extern uint64_t sched_current_frame_seq(void);
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) :: "memory");
    if (flags & (1ULL << 9))
        return;

    uint32_t index = __atomic_fetch_add(&g_k32_qpc_if0_trace_count, 1,
                                        __ATOMIC_RELAXED);
    if (index >= 32)
        return;

    serial_puts("[K32-QPC-IF0] phase=");
    serial_puts(phase);
    serial_puts(" proc=");
    serial_putdec(win32_current_process_id());
    serial_puts(" kpid=");
    serial_putdec((uint64_t)(uint32_t)proc_current_pid());
    serial_puts(" caller=0x");
    serial_puthex(caller, 16);
    serial_puts(" frame_seq=");
    serial_putdec(sched_current_frame_seq());
    serial_puts(" flags=0x");
    serial_puthex(flags, 16);
    serial_puts("\n");
}

BOOL WINAPI QueryPerformanceCounter(PLARGE_INTEGER lpPerformanceCount)
{
    uint64_t caller =
        (uint64_t)(ULONG_PTR)__builtin_return_address(0);
    k32_qpc_trace_target("entry", caller);
    k32_qpc_trace_if0("entry", caller);
    NTSTATUS status = NtQueryPerformanceCounter(lpPerformanceCount, NULL);
    k32_qpc_trace_target("post-nt", caller);
    k32_qpc_trace_if0("post-nt", caller);
    return NT_SUCCESS(status);
}

BOOL WINAPI QueryPerformanceFrequency(PLARGE_INTEGER lpFrequency)
{
    NTSTATUS status = NtQueryPerformanceCounter(NULL, lpFrequency);
    return NT_SUCCESS(status);
}

static void WINAPI QueryUnbiasedInterruptTimePrecise_k32(
    ULONGLONG *unbiased_time)
{
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;

    if (!unbiased_time)
        return;
    if (!QueryPerformanceCounter(&counter) ||
        !QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0) {
        *unbiased_time = 0;
        return;
    }

    ULONGLONG ticks = (ULONGLONG)counter.QuadPart;
    ULONGLONG hz = (ULONGLONG)frequency.QuadPart;
    ULONGLONG seconds = ticks / hz;
    ULONGLONG remainder = ticks % hz;
    *unbiased_time = seconds * 10000000ULL +
                     (remainder * 10000000ULL) / hz;
}

static uint32_t steamservice_start_thread32;
static uint32_t steamservice_shutdown32;

static BOOL WINAPI SteamService_StartThread_bridge(ULONG_PTR command_line)
{
    if (!steamservice_start_thread32 || !command_line) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    char *command_copy32 = NULL;
    uint64_t command_copy_pages = 0;
    uint32_t arg;
    if (command_line <= 0xFFFFFFFFULL) {
        arg = (uint32_t)command_line;
    } else {
        const char *source = (const char *)(ULONG_PTR)command_line;
        SIZE_T length = 0;
        while (length < 32767 && source[length])
            length++;
        if (length == 32767) {
            SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
            return FALSE;
        }

        command_copy_pages = (length + 1 + 4095) / 4096;
        extern void *mem_alloc_pages(uint64_t count);
        extern void mem_free_pages(void *addr, uint64_t count);
        command_copy32 = (char *)mem_alloc_pages(command_copy_pages);
        if (!command_copy32 ||
            (uint64_t)(ULONG_PTR)command_copy32 > 0xFFFFFFFFULL) {
            if (command_copy32)
                mem_free_pages(command_copy32, command_copy_pages);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return FALSE;
        }
        for (SIZE_T i = 0; i <= length; i++)
            command_copy32[i] = source[i];
        arg = (uint32_t)(ULONG_PTR)command_copy32;
    }

    serial_puts("[STEAMSVC-BRIDGE] StartThread source=0x");
    serial_puthex((uint64_t)command_line, 16);
    serial_puts(" arg32=0x");
    serial_puthex(arg, 8);
    serial_puts(" command='");
    serial_puts((const char *)(ULONG_PTR)arg);
    serial_puts("'\n");

    BOOL started = (BOOL)compat32_callback_args(
        steamservice_start_thread32, 1, &arg);
    if (command_copy32) {
        extern void mem_free_pages(void *addr, uint64_t count);
        mem_free_pages(command_copy32, command_copy_pages);
    }
    serial_puts("[STEAMSVC-BRIDGE] StartThread result=");
    serial_putdec(started ? 1 : 0);
    serial_puts(" error=");
    serial_putdec(GetLastError());
    serial_puts("\n");
    return started;
}

static void WINAPI SteamService_Shutdown_bridge(void)
{
    if (steamservice_shutdown32)
        compat32_callback_args(steamservice_shutdown32, 0, NULL);
}

static PVOID bridge_steamservice_export(LOADED_MODULE *mod, PCSTR name,
                                        PVOID target)
{
    if (!mod || !mod->image.Is32Bit || g_compat32_mode ||
        k32_strcmp(mod->name, "steamservice.dll") != 0)
        return target;

    if (k32_strcmp(name, "SteamService_StartThread") == 0) {
        steamservice_start_thread32 = (uint32_t)(ULONG_PTR)target;
        serial_puts("[GPA] bridge SteamService_StartThread PE64->PE32\n");
        return (PVOID)SteamService_StartThread_bridge;
    }
    if (k32_strcmp(name, "SteamService_Shutdown") == 0) {
        steamservice_shutdown32 = (uint32_t)(ULONG_PTR)target;
        serial_puts("[GPA] bridge SteamService_Shutdown PE64->PE32\n");
        return (PVOID)SteamService_Shutdown_bridge;
    }
    return target;
}

/* Trace ANGLE at the GetProcAddress boundary. This keeps the vendor DLLs
 * untouched while exposing which EGL backend CEF selects and where it fails. */
typedef PVOID (WINAPI *egl_get_display_fn)(PVOID native_display);
typedef PVOID (WINAPI *egl_get_platform_display_fn)(DWORD platform,
                                                     PVOID native_display,
                                                     const ULONG_PTR *attributes);
typedef BOOL (WINAPI *egl_initialize_fn)(PVOID display, int *major, int *minor);
typedef DWORD (WINAPI *egl_get_error_fn)(void);
typedef BOOL (WINAPI *egl_choose_config_fn)(PVOID display,
                                             const int *attributes,
                                             PVOID *configs, int config_size,
                                             int *config_count);
typedef BOOL (WINAPI *egl_bind_api_fn)(DWORD api);
typedef PVOID (WINAPI *egl_create_context_fn)(PVOID display, PVOID config,
                                               PVOID shared_context,
                                               const int *attributes);
typedef PVOID (WINAPI *egl_create_pbuffer_surface_fn)(PVOID display,
                                                       PVOID config,
                                                       const int *attributes);
typedef PVOID (WINAPI *egl_create_window_surface_fn)(PVOID display,
                                                      PVOID config,
                                                      PVOID native_window,
                                                      const int *attributes);
typedef BOOL (WINAPI *egl_make_current_fn)(PVOID display, PVOID draw,
                                            PVOID read, PVOID context);
typedef BOOL (WINAPI *egl_swap_buffers_fn)(PVOID display, PVOID surface);
typedef BOOL (WINAPI *egl_destroy_surface_fn)(PVOID display, PVOID surface);
typedef BOOL (WINAPI *egl_get_sync_values_fn)(PVOID display, PVOID surface,
                                               ULONGLONG *ust,
                                               ULONGLONG *msc,
                                               ULONGLONG *sbc);
typedef PVOID (WINAPI *egl_get_proc_address_fn)(PCSTR name);

static egl_get_display_fn angle_egl_get_display;
static egl_get_platform_display_fn angle_egl_get_platform_display;
static egl_get_platform_display_fn angle_egl_get_platform_display_ext;
static egl_initialize_fn angle_egl_initialize;
static egl_get_error_fn angle_egl_get_error;
static egl_choose_config_fn angle_egl_choose_config;
static egl_bind_api_fn angle_egl_bind_api;
static egl_create_context_fn angle_egl_create_context;
static egl_create_pbuffer_surface_fn angle_egl_create_pbuffer_surface;
static egl_create_window_surface_fn angle_egl_create_window_surface;
static egl_make_current_fn angle_egl_make_current;
static egl_swap_buffers_fn angle_egl_swap_buffers;
static egl_destroy_surface_fn angle_egl_destroy_surface;
static egl_get_sync_values_fn angle_egl_get_sync_values;
static egl_get_proc_address_fn angle_egl_get_proc_address;

#define ANGLE_TRACE_SURFACES 32
typedef struct {
    PVOID surface;
    PVOID native_window;
    uint32_t swaps;
} ANGLE_TRACE_SURFACE;

static ANGLE_TRACE_SURFACE angle_trace_surfaces[ANGLE_TRACE_SURFACES];

static ANGLE_TRACE_SURFACE *angle_trace_find_surface(PVOID surface,
                                                      BOOL allocate)
{
    ANGLE_TRACE_SURFACE *free_slot = NULL;
    for (int i = 0; i < ANGLE_TRACE_SURFACES; i++) {
        ANGLE_TRACE_SURFACE *entry = &angle_trace_surfaces[i];
        if (entry->surface == surface)
            return entry;
        if (!entry->surface && !free_slot)
            free_slot = entry;
    }
    if (allocate && free_slot) {
        free_slot->surface = surface;
        free_slot->native_window = NULL;
        free_slot->swaps = 0;
        return free_slot;
    }
    return NULL;
}

static PVOID angle_trace_egl_export(PCSTR name, PVOID target);

static PVOID WINAPI angle_trace_egl_get_display(PVOID native_display)
{
    PVOID result = angle_egl_get_display
        ? angle_egl_get_display(native_display) : NULL;
    serial_puts("[ANGLE-EGL] eglGetDisplay native=0x");
    serial_puthex((uint64_t)(ULONG_PTR)native_display, 16);
    serial_puts(" -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)result, 16);
    serial_puts("\n");
    return result;
}

static void angle_trace_egl_attributes(const ULONG_PTR *attributes)
{
    serial_puts(" attrs=");
    if (!attributes) {
        serial_puts("NULL");
        return;
    }
    serial_puts("[");
    for (int i = 0; i < 16; i += 2) {
        ULONG_PTR key = attributes[i];
        serial_puts("0x");
        serial_puthex((uint64_t)key, 8);
        if (key == 0x3038) break; /* EGL_NONE */
        serial_puts("=0x");
        serial_puthex((uint64_t)attributes[i + 1], 16);
        serial_puts(" ");
    }
    serial_puts("]");
}

static PVOID WINAPI angle_trace_egl_get_platform_display(
    DWORD platform, PVOID native_display, const ULONG_PTR *attributes)
{
    PVOID result = angle_egl_get_platform_display
        ? angle_egl_get_platform_display(platform, native_display, attributes)
        : NULL;
    serial_puts("[ANGLE-EGL] eglGetPlatformDisplay platform=0x");
    serial_puthex(platform, 8);
    serial_puts(" native=0x");
    serial_puthex((uint64_t)(ULONG_PTR)native_display, 16);
    angle_trace_egl_attributes(attributes);
    serial_puts(" -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)result, 16);
    serial_puts("\n");
    return result;
}

static PVOID WINAPI angle_trace_egl_get_platform_display_ext(
    DWORD platform, PVOID native_display, const ULONG_PTR *attributes)
{
    PVOID result = angle_egl_get_platform_display_ext
        ? angle_egl_get_platform_display_ext(platform, native_display,
                                             attributes)
        : NULL;
    serial_puts("[ANGLE-EGL] eglGetPlatformDisplayEXT platform=0x");
    serial_puthex(platform, 8);
    serial_puts(" native=0x");
    serial_puthex((uint64_t)(ULONG_PTR)native_display, 16);
    angle_trace_egl_attributes(attributes);
    serial_puts(" -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)result, 16);
    serial_puts("\n");
    return result;
}

static BOOL WINAPI angle_trace_egl_initialize(PVOID display, int *major,
                                               int *minor)
{
    BOOL result = angle_egl_initialize
        ? angle_egl_initialize(display, major, minor) : FALSE;
    serial_puts("[ANGLE-EGL] eglInitialize display=0x");
    serial_puthex((uint64_t)(ULONG_PTR)display, 16);
    serial_puts(" -> ");
    serial_putdec(result);
    if (result && major && minor) {
        serial_puts(" version=");
        serial_putdec((uint32_t)*major);
        serial_puts(".");
        serial_putdec((uint32_t)*minor);
    }
    serial_puts("\n");
    return result;
}

static DWORD WINAPI angle_trace_egl_get_error(void)
{
    DWORD result = angle_egl_get_error ? angle_egl_get_error() : 0x3000;
    serial_puts("[ANGLE-EGL] eglGetError -> 0x");
    serial_puthex(result, 8);
    serial_puts("\n");
    return result;
}

static void angle_trace_egl_int_attributes(const int *attributes)
{
    serial_puts(" attrs=");
    if (!attributes) {
        serial_puts("NULL");
        return;
    }
    serial_puts("[");
    for (int i = 0; i < 24; i += 2) {
        DWORD key = (DWORD)attributes[i];
        serial_puts("0x");
        serial_puthex(key, 8);
        if (key == 0x3038) break; /* EGL_NONE */
        serial_puts("=0x");
        serial_puthex((DWORD)attributes[i + 1], 8);
        serial_puts(" ");
    }
    serial_puts("]");
}

static BOOL WINAPI angle_trace_egl_choose_config(PVOID display,
                                                  const int *attributes,
                                                  PVOID *configs,
                                                  int config_size,
                                                  int *config_count)
{
    serial_puts("[ANGLE-EGL] eglChooseConfig enter display=0x");
    serial_puthex((uint64_t)(ULONG_PTR)display, 16);
    angle_trace_egl_int_attributes(attributes);
    serial_puts("\n");
    BOOL result = angle_egl_choose_config
        ? angle_egl_choose_config(display, attributes, configs, config_size,
                                  config_count) : FALSE;
    serial_puts("[ANGLE-EGL] eglChooseConfig -> ");
    serial_putdec(result);
    serial_puts(" count=");
    serial_putdec(config_count ? (uint32_t)*config_count : 0);
    serial_puts("\n");
    return result;
}

static BOOL WINAPI angle_trace_egl_bind_api(DWORD api)
{
    serial_puts("[ANGLE-EGL] eglBindAPI enter api=0x");
    serial_puthex(api, 8);
    serial_puts("\n");
    BOOL result = angle_egl_bind_api ? angle_egl_bind_api(api) : FALSE;
    serial_puts("[ANGLE-EGL] eglBindAPI -> ");
    serial_putdec(result);
    serial_puts("\n");
    return result;
}

static PVOID WINAPI angle_trace_egl_create_context(PVOID display, PVOID config,
                                                    PVOID shared_context,
                                                    const int *attributes)
{
    serial_puts("[ANGLE-EGL] eglCreateContext enter config=0x");
    serial_puthex((uint64_t)(ULONG_PTR)config, 16);
    angle_trace_egl_int_attributes(attributes);
    serial_puts("\n");
    PVOID result = angle_egl_create_context
        ? angle_egl_create_context(display, config, shared_context, attributes)
        : NULL;
    serial_puts("[ANGLE-EGL] eglCreateContext -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)result, 16);
    serial_puts("\n");
    return result;
}

static PVOID WINAPI angle_trace_egl_create_pbuffer_surface(
    PVOID display, PVOID config, const int *attributes)
{
    serial_puts("[ANGLE-EGL] eglCreatePbufferSurface enter config=0x");
    serial_puthex((uint64_t)(ULONG_PTR)config, 16);
    angle_trace_egl_int_attributes(attributes);
    serial_puts("\n");
    PVOID result = angle_egl_create_pbuffer_surface
        ? angle_egl_create_pbuffer_surface(display, config, attributes) : NULL;
    serial_puts("[ANGLE-EGL] eglCreatePbufferSurface -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)result, 16);
    serial_puts("\n");
    return result;
}

static PVOID WINAPI angle_trace_egl_create_window_surface(
    PVOID display, PVOID config, PVOID native_window, const int *attributes)
{
    serial_puts("[ANGLE-EGL] eglCreateWindowSurface enter hwnd=0x");
    serial_puthex((uint64_t)(ULONG_PTR)native_window, 16);
    angle_trace_egl_int_attributes(attributes);
    serial_puts("\n");
    PVOID result = angle_egl_create_window_surface
        ? angle_egl_create_window_surface(display, config, native_window,
                                          attributes) : NULL;
    if (result) {
        ANGLE_TRACE_SURFACE *entry = angle_trace_find_surface(result, TRUE);
        if (entry)
            entry->native_window = native_window;
    }
    serial_puts("[ANGLE-EGL] eglCreateWindowSurface -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)result, 16);
    serial_puts("\n");
    return result;
}

static BOOL WINAPI angle_trace_egl_make_current(PVOID display, PVOID draw,
                                                 PVOID read, PVOID context)
{
    serial_puts("[ANGLE-EGL] eglMakeCurrent enter ctx=0x");
    serial_puthex((uint64_t)(ULONG_PTR)context, 16);
    serial_puts(" draw=0x");
    serial_puthex((uint64_t)(ULONG_PTR)draw, 16);
    serial_puts("\n");
    BOOL result = angle_egl_make_current
        ? angle_egl_make_current(display, draw, read, context) : FALSE;
    serial_puts("[ANGLE-EGL] eglMakeCurrent -> ");
    serial_putdec(result);
    serial_puts("\n");
    return result;
}

static BOOL WINAPI angle_trace_egl_swap_buffers(PVOID display, PVOID surface)
{
    ANGLE_TRACE_SURFACE *entry = angle_trace_find_surface(surface, TRUE);
    uint32_t count = entry ? ++entry->swaps : 0;
    BOOL trace = count <= 4 || (count && (count % 256) == 0);
    if (trace) {
        serial_puts("[ANGLE-EGL] eglSwapBuffers surface=0x");
        serial_puthex((uint64_t)(ULONG_PTR)surface, 16);
        serial_puts(" hwnd=0x");
        serial_puthex((uint64_t)(ULONG_PTR)(entry ? entry->native_window : NULL),
                      16);
        serial_puts(" count=");
        serial_putdec(count);
        serial_puts("\n");
    }
    BOOL result = angle_egl_swap_buffers
        ? angle_egl_swap_buffers(display, surface) : FALSE;
    if (trace) {
        serial_puts("[ANGLE-EGL] eglSwapBuffers result=");
        serial_putdec(result);
        serial_puts("\n");
    } else if (!result) {
        serial_puts("[ANGLE-EGL] eglSwapBuffers FAILED surface=0x");
        serial_puthex((uint64_t)(ULONG_PTR)surface, 16);
        serial_puts(" hwnd=0x");
        serial_puthex((uint64_t)(ULONG_PTR)(entry ? entry->native_window : NULL),
                      16);
        serial_puts(" count=");
        serial_putdec(count);
        serial_puts("\n");
    }
    return result;
}

static BOOL WINAPI angle_trace_egl_destroy_surface(PVOID display,
                                                    PVOID surface)
{
    ANGLE_TRACE_SURFACE *entry = angle_trace_find_surface(surface, FALSE);
    serial_puts("[ANGLE-EGL] eglDestroySurface surface=0x");
    serial_puthex((uint64_t)(ULONG_PTR)surface, 16);
    serial_puts(" hwnd=0x");
    serial_puthex((uint64_t)(ULONG_PTR)(entry ? entry->native_window : NULL),
                  16);
    BOOL result = angle_egl_destroy_surface
        ? angle_egl_destroy_surface(display, surface) : FALSE;
    serial_puts(" -> ");
    serial_putdec(result);
    serial_puts("\n");
    if (result && entry) {
        entry->surface = NULL;
        entry->native_window = NULL;
        entry->swaps = 0;
    }
    return result;
}

static BOOL WINAPI angle_trace_egl_get_sync_values(PVOID display,
                                                    PVOID surface,
                                                    ULONGLONG *ust,
                                                    ULONGLONG *msc,
                                                    ULONGLONG *sbc)
{
    static uint32_t trace_count;
    BOOL result = angle_egl_get_sync_values
        ? angle_egl_get_sync_values(display, surface, ust, msc, sbc) : FALSE;
    if (trace_count++ < 32) {
        serial_puts("[ANGLE-EGL] eglGetSyncValuesCHROMIUM -> ");
        serial_putdec(result);
        serial_puts(" ust=");
        serial_putdec(ust ? *ust : 0);
        serial_puts(" msc=");
        serial_putdec(msc ? *msc : 0);
        serial_puts(" sbc=");
        serial_putdec(sbc ? *sbc : 0);
        serial_puts("\n");
    }
    return result;
}

typedef const BYTE *(WINAPI *angle_gl_get_string_fn)(DWORD name);
typedef const BYTE *(WINAPI *angle_gl_get_stringi_fn)(DWORD name, DWORD index);
typedef void (WINAPI *angle_gl_get_integerv_fn)(DWORD name, int *value);
typedef DWORD (WINAPI *angle_gl_get_error_fn)(void);
typedef void (WINAPI *angle_gl_void_fn)(void);
typedef void (WINAPI *angle_gl_gen_queries_fn)(int count, DWORD *ids);
typedef void (WINAPI *angle_gl_query_counter_fn)(DWORD id, DWORD target);
typedef void (WINAPI *angle_gl_get_query_object_uiv_fn)(DWORD id, DWORD pname,
                                                        DWORD *value);
typedef void (WINAPI *angle_gl_get_query_object_ui64v_fn)(DWORD id,
                                                          DWORD pname,
                                                          ULONGLONG *value);
typedef PVOID (WINAPI *angle_gl_fence_sync_fn)(DWORD condition, DWORD flags);
typedef DWORD (WINAPI *angle_gl_client_wait_sync_fn)(PVOID sync, DWORD flags,
                                                      ULONGLONG timeout);
typedef void (WINAPI *angle_gl_delete_sync_fn)(PVOID sync);

static angle_gl_get_string_fn angle_gl_get_string;
static angle_gl_get_stringi_fn angle_gl_get_stringi;
static angle_gl_get_integerv_fn angle_gl_get_integerv;
static angle_gl_get_error_fn angle_gl_get_error;
static angle_gl_void_fn angle_gl_finish;
static angle_gl_void_fn angle_gl_flush;
static angle_gl_gen_queries_fn angle_gl_gen_queries;
static angle_gl_gen_queries_fn angle_gl_gen_queries_ext;
static angle_gl_query_counter_fn angle_gl_query_counter;
static angle_gl_query_counter_fn angle_gl_query_counter_ext;
static angle_gl_get_query_object_uiv_fn angle_gl_get_query_object_uiv;
static angle_gl_get_query_object_uiv_fn angle_gl_get_query_object_uiv_ext;
static angle_gl_get_query_object_ui64v_fn angle_gl_get_query_object_ui64v;
static angle_gl_get_query_object_ui64v_fn angle_gl_get_query_object_ui64v_ext;
static angle_gl_fence_sync_fn angle_gl_fence_sync;
static angle_gl_client_wait_sync_fn angle_gl_client_wait_sync;
static angle_gl_delete_sync_fn angle_gl_delete_sync;

static const BYTE *WINAPI angle_trace_gl_get_string(DWORD name)
{
    serial_puts("[ANGLE-GL] glGetString enter name=0x");
    serial_puthex(name, 8);
    serial_puts("\n");
    const BYTE *result = angle_gl_get_string ? angle_gl_get_string(name) : NULL;
    serial_puts("[ANGLE-GL] glGetString -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)result, 16);
    serial_puts("\n");
    return result;
}

static const BYTE *WINAPI angle_trace_gl_get_stringi(DWORD name, DWORD index)
{
    static uint32_t trace_count;
    BOOL trace = trace_count++ < 64;
    if (trace) {
        serial_puts("[ANGLE-GL] glGetStringi enter name=0x");
        serial_puthex(name, 8);
        serial_puts(" index=");
        serial_putdec(index);
        serial_puts("\n");
    }
    const BYTE *result = angle_gl_get_stringi
        ? angle_gl_get_stringi(name, index) : NULL;
    if (trace) {
        serial_puts("[ANGLE-GL] glGetStringi -> 0x");
        serial_puthex((uint64_t)(ULONG_PTR)result, 16);
        serial_puts("\n");
    }
    return result;
}

static void WINAPI angle_trace_gl_get_integerv(DWORD name, int *value)
{
    static uint32_t trace_count;
    BOOL trace = trace_count++ < 96;
    if (trace) {
        serial_puts("[ANGLE-GL] glGetIntegerv enter name=0x");
        serial_puthex(name, 8);
        serial_puts("\n");
    }
    if (angle_gl_get_integerv) angle_gl_get_integerv(name, value);
    if (trace) {
        serial_puts("[ANGLE-GL] glGetIntegerv -> ");
        serial_putdec(value ? (uint32_t)*value : 0);
        serial_puts("\n");
    }
}

static DWORD WINAPI angle_trace_gl_get_error(void)
{
    static uint32_t trace_count;
    BOOL trace = trace_count++ < 32;
    if (trace) serial_puts("[ANGLE-GL] glGetError enter\n");
    DWORD result = angle_gl_get_error ? angle_gl_get_error() : 0;
    if (trace) {
        serial_puts("[ANGLE-GL] glGetError -> 0x");
        serial_puthex(result, 8);
        serial_puts("\n");
    }
    return result;
}

static void WINAPI angle_trace_gl_finish(void)
{
    serial_puts("[ANGLE-GL] glFinish enter\n");
    if (angle_gl_finish) angle_gl_finish();
    serial_puts("[ANGLE-GL] glFinish -> return\n");
}

static void WINAPI angle_trace_gl_flush(void)
{
    static uint32_t trace_count;
    BOOL trace = trace_count++ < 32;
    if (trace) serial_puts("[ANGLE-GL] glFlush enter\n");
    if (angle_gl_flush) angle_gl_flush();
    if (trace) serial_puts("[ANGLE-GL] glFlush -> return\n");
}

static void angle_trace_gl_query_ids(PCSTR name, int count, const DWORD *ids)
{
    serial_puts("[ANGLE-QUERY] ");
    serial_puts(name);
    serial_puts(" count=");
    serial_putdec((uint32_t)(count < 0 ? 0 : count));
    if (ids && count > 0) {
        serial_puts(" first=");
        serial_putdec(ids[0]);
    }
    serial_puts("\n");
}

static void WINAPI angle_trace_gl_gen_queries(int count, DWORD *ids)
{
    static uint32_t trace_count;
    if (angle_gl_gen_queries) angle_gl_gen_queries(count, ids);
    if (trace_count++ < 32)
        angle_trace_gl_query_ids("glGenQueries", count, ids);
}

static void WINAPI angle_trace_gl_gen_queries_ext(int count, DWORD *ids)
{
    static uint32_t trace_count;
    if (angle_gl_gen_queries_ext) angle_gl_gen_queries_ext(count, ids);
    if (trace_count++ < 32)
        angle_trace_gl_query_ids("glGenQueriesEXT", count, ids);
}

static void angle_trace_gl_query_counter_result(PCSTR name, DWORD id,
                                                 DWORD target)
{
    serial_puts("[ANGLE-QUERY] ");
    serial_puts(name);
    serial_puts(" id=");
    serial_putdec(id);
    serial_puts(" target=0x");
    serial_puthex(target, 8);
    serial_puts("\n");
}

static void WINAPI angle_trace_gl_query_counter(DWORD id, DWORD target)
{
    static uint32_t trace_count;
    if (angle_gl_query_counter) angle_gl_query_counter(id, target);
    if (trace_count++ < 32)
        angle_trace_gl_query_counter_result("glQueryCounter", id, target);
}

static void WINAPI angle_trace_gl_query_counter_ext(DWORD id, DWORD target)
{
    static uint32_t trace_count;
    if (angle_gl_query_counter_ext) angle_gl_query_counter_ext(id, target);
    if (trace_count++ < 32)
        angle_trace_gl_query_counter_result("glQueryCounterEXT", id, target);
}

static void angle_trace_gl_query_value(PCSTR name, DWORD id, DWORD pname,
                                       ULONGLONG value)
{
    serial_puts("[ANGLE-QUERY] ");
    serial_puts(name);
    serial_puts(" id=");
    serial_putdec(id);
    serial_puts(" pname=0x");
    serial_puthex(pname, 8);
    serial_puts(" value=");
    serial_putdec(value);
    serial_puts("\n");
}

static void WINAPI angle_trace_gl_get_query_object_uiv(DWORD id, DWORD pname,
                                                        DWORD *value)
{
    static uint32_t trace_count;
    if (angle_gl_get_query_object_uiv)
        angle_gl_get_query_object_uiv(id, pname, value);
    if (trace_count++ < 64)
        angle_trace_gl_query_value("glGetQueryObjectuiv", id, pname,
                                   value ? *value : 0);
}

static void WINAPI angle_trace_gl_get_query_object_uiv_ext(
    DWORD id, DWORD pname, DWORD *value)
{
    static uint32_t trace_count;
    if (angle_gl_get_query_object_uiv_ext)
        angle_gl_get_query_object_uiv_ext(id, pname, value);
    if (trace_count++ < 64)
        angle_trace_gl_query_value("glGetQueryObjectuivEXT", id, pname,
                                   value ? *value : 0);
}

static void WINAPI angle_trace_gl_get_query_object_ui64v(
    DWORD id, DWORD pname, ULONGLONG *value)
{
    static uint32_t trace_count;
    if (angle_gl_get_query_object_ui64v)
        angle_gl_get_query_object_ui64v(id, pname, value);
    if (trace_count++ < 32)
        angle_trace_gl_query_value("glGetQueryObjectui64v", id, pname,
                                   value ? *value : 0);
}

static void WINAPI angle_trace_gl_get_query_object_ui64v_ext(
    DWORD id, DWORD pname, ULONGLONG *value)
{
    static uint32_t trace_count;
    if (angle_gl_get_query_object_ui64v_ext)
        angle_gl_get_query_object_ui64v_ext(id, pname, value);
    if (trace_count++ < 32)
        angle_trace_gl_query_value("glGetQueryObjectui64vEXT", id, pname,
                                   value ? *value : 0);
}

static PVOID WINAPI angle_trace_gl_fence_sync(DWORD condition, DWORD flags)
{
    static uint32_t trace_count;
    PVOID sync = angle_gl_fence_sync
        ? angle_gl_fence_sync(condition, flags) : NULL;
    if (trace_count++ < 32) {
        serial_puts("[ANGLE-FENCE] glFenceSync condition=0x");
        serial_puthex(condition, 8);
        serial_puts(" flags=0x");
        serial_puthex(flags, 8);
        serial_puts(" -> 0x");
        serial_puthex((uint64_t)(ULONG_PTR)sync, 16);
        serial_puts("\n");
    }
    return sync;
}

static DWORD WINAPI angle_trace_gl_client_wait_sync(PVOID sync, DWORD flags,
                                                      ULONGLONG timeout)
{
    static uint32_t trace_count;
    DWORD result = angle_gl_client_wait_sync
        ? angle_gl_client_wait_sync(sync, flags, timeout) : 0x911D;
    if (trace_count++ < 64) {
        serial_puts("[ANGLE-FENCE] glClientWaitSync sync=0x");
        serial_puthex((uint64_t)(ULONG_PTR)sync, 16);
        serial_puts(" timeout=");
        serial_putdec(timeout);
        serial_puts(" -> 0x");
        serial_puthex(result, 8);
        serial_puts("\n");
    }
    return result;
}

static void WINAPI angle_trace_gl_delete_sync(PVOID sync)
{
    static uint32_t trace_count;
    if (trace_count++ < 32) {
        serial_puts("[ANGLE-FENCE] glDeleteSync sync=0x");
        serial_puthex((uint64_t)(ULONG_PTR)sync, 16);
        serial_puts("\n");
    }
    if (angle_gl_delete_sync) angle_gl_delete_sync(sync);
}

static PVOID angle_trace_gl_export(PCSTR name, PVOID target)
{
    if (k32_strcmp(name, "glGetString") == 0) {
        angle_gl_get_string = (angle_gl_get_string_fn)target;
        return (PVOID)angle_trace_gl_get_string;
    }
    if (k32_strcmp(name, "glGetStringi") == 0) {
        angle_gl_get_stringi = (angle_gl_get_stringi_fn)target;
        return (PVOID)angle_trace_gl_get_stringi;
    }
    if (k32_strcmp(name, "glGetIntegerv") == 0) {
        angle_gl_get_integerv = (angle_gl_get_integerv_fn)target;
        return (PVOID)angle_trace_gl_get_integerv;
    }
    if (k32_strcmp(name, "glGetError") == 0) {
        angle_gl_get_error = (angle_gl_get_error_fn)target;
        return (PVOID)angle_trace_gl_get_error;
    }
    if (k32_strcmp(name, "glFinish") == 0) {
        angle_gl_finish = (angle_gl_void_fn)target;
        return (PVOID)angle_trace_gl_finish;
    }
    if (k32_strcmp(name, "glFlush") == 0) {
        angle_gl_flush = (angle_gl_void_fn)target;
        return (PVOID)angle_trace_gl_flush;
    }
    if (k32_strcmp(name, "glGenQueries") == 0) {
        angle_gl_gen_queries = (angle_gl_gen_queries_fn)target;
        return (PVOID)angle_trace_gl_gen_queries;
    }
    if (k32_strcmp(name, "glGenQueriesEXT") == 0) {
        angle_gl_gen_queries_ext = (angle_gl_gen_queries_fn)target;
        return (PVOID)angle_trace_gl_gen_queries_ext;
    }
    if (k32_strcmp(name, "glQueryCounter") == 0) {
        angle_gl_query_counter = (angle_gl_query_counter_fn)target;
        return (PVOID)angle_trace_gl_query_counter;
    }
    if (k32_strcmp(name, "glQueryCounterEXT") == 0) {
        angle_gl_query_counter_ext = (angle_gl_query_counter_fn)target;
        return (PVOID)angle_trace_gl_query_counter_ext;
    }
    if (k32_strcmp(name, "glGetQueryObjectuiv") == 0) {
        angle_gl_get_query_object_uiv =
            (angle_gl_get_query_object_uiv_fn)target;
        return (PVOID)angle_trace_gl_get_query_object_uiv;
    }
    if (k32_strcmp(name, "glGetQueryObjectuivEXT") == 0) {
        angle_gl_get_query_object_uiv_ext =
            (angle_gl_get_query_object_uiv_fn)target;
        return (PVOID)angle_trace_gl_get_query_object_uiv_ext;
    }
    if (k32_strcmp(name, "glGetQueryObjectui64v") == 0) {
        angle_gl_get_query_object_ui64v =
            (angle_gl_get_query_object_ui64v_fn)target;
        return (PVOID)angle_trace_gl_get_query_object_ui64v;
    }
    if (k32_strcmp(name, "glGetQueryObjectui64vEXT") == 0) {
        angle_gl_get_query_object_ui64v_ext =
            (angle_gl_get_query_object_ui64v_fn)target;
        return (PVOID)angle_trace_gl_get_query_object_ui64v_ext;
    }
    if (k32_strcmp(name, "glFenceSync") == 0) {
        angle_gl_fence_sync = (angle_gl_fence_sync_fn)target;
        return (PVOID)angle_trace_gl_fence_sync;
    }
    if (k32_strcmp(name, "glClientWaitSync") == 0) {
        angle_gl_client_wait_sync = (angle_gl_client_wait_sync_fn)target;
        return (PVOID)angle_trace_gl_client_wait_sync;
    }
    if (k32_strcmp(name, "glDeleteSync") == 0) {
        angle_gl_delete_sync = (angle_gl_delete_sync_fn)target;
        return (PVOID)angle_trace_gl_delete_sync;
    }
    return target;
}

static PVOID WINAPI angle_trace_egl_get_proc_address(PCSTR name)
{
    static uint32_t trace_count;
    PVOID target = angle_egl_get_proc_address
        ? angle_egl_get_proc_address(name) : NULL;
    PVOID result = angle_trace_egl_export(name, target);
    if (trace_count++ < 128) {
        serial_puts("[ANGLE-EGL] eglGetProcAddress ");
        serial_puts(name ? name : "(null)");
        serial_puts(" raw=0x");
        serial_puthex((uint64_t)(ULONG_PTR)target, 16);
        serial_puts(" traced=0x");
        serial_puthex((uint64_t)(ULONG_PTR)result, 16);
        serial_puts("\n");
    }
    return result;
}

static PVOID angle_trace_egl_export(PCSTR name, PVOID target)
{
#if defined(OK_QUIET) && OK_QUIET
    (void)name;
    return target;
#endif
    if (!name || !target || g_compat32_mode) return target;
    if (k32_strcmp(name, "eglGetProcAddress") == 0) {
        angle_egl_get_proc_address = (egl_get_proc_address_fn)target;
        return (PVOID)angle_trace_egl_get_proc_address;
    }
    if (k32_strcmp(name, "eglGetDisplay") == 0) {
        angle_egl_get_display = (egl_get_display_fn)target;
        return (PVOID)angle_trace_egl_get_display;
    }
    if (k32_strcmp(name, "eglGetPlatformDisplay") == 0) {
        angle_egl_get_platform_display = (egl_get_platform_display_fn)target;
        return (PVOID)angle_trace_egl_get_platform_display;
    }
    if (k32_strcmp(name, "eglGetPlatformDisplayEXT") == 0) {
        angle_egl_get_platform_display_ext =
            (egl_get_platform_display_fn)target;
        return (PVOID)angle_trace_egl_get_platform_display_ext;
    }
    if (k32_strcmp(name, "eglInitialize") == 0) {
        angle_egl_initialize = (egl_initialize_fn)target;
        return (PVOID)angle_trace_egl_initialize;
    }
    if (k32_strcmp(name, "eglGetError") == 0) {
        angle_egl_get_error = (egl_get_error_fn)target;
        return (PVOID)angle_trace_egl_get_error;
    }
    if (k32_strcmp(name, "eglChooseConfig") == 0) {
        angle_egl_choose_config = (egl_choose_config_fn)target;
        return (PVOID)angle_trace_egl_choose_config;
    }
    if (k32_strcmp(name, "eglBindAPI") == 0) {
        angle_egl_bind_api = (egl_bind_api_fn)target;
        return (PVOID)angle_trace_egl_bind_api;
    }
    if (k32_strcmp(name, "eglCreateContext") == 0) {
        angle_egl_create_context = (egl_create_context_fn)target;
        return (PVOID)angle_trace_egl_create_context;
    }
    if (k32_strcmp(name, "eglCreatePbufferSurface") == 0) {
        angle_egl_create_pbuffer_surface =
            (egl_create_pbuffer_surface_fn)target;
        return (PVOID)angle_trace_egl_create_pbuffer_surface;
    }
    if (k32_strcmp(name, "eglCreateWindowSurface") == 0) {
        angle_egl_create_window_surface =
            (egl_create_window_surface_fn)target;
        return (PVOID)angle_trace_egl_create_window_surface;
    }
    if (k32_strcmp(name, "eglMakeCurrent") == 0) {
        angle_egl_make_current = (egl_make_current_fn)target;
        return (PVOID)angle_trace_egl_make_current;
    }
    if (k32_strcmp(name, "eglSwapBuffers") == 0) {
        angle_egl_swap_buffers = (egl_swap_buffers_fn)target;
        return (PVOID)angle_trace_egl_swap_buffers;
    }
    if (k32_strcmp(name, "eglDestroySurface") == 0) {
        angle_egl_destroy_surface = (egl_destroy_surface_fn)target;
        return (PVOID)angle_trace_egl_destroy_surface;
    }
    if (k32_strcmp(name, "eglGetSyncValuesCHROMIUM") == 0) {
        angle_egl_get_sync_values = (egl_get_sync_values_fn)target;
        return (PVOID)angle_trace_egl_get_sync_values;
    }
    return angle_trace_gl_export(name, target);
}

/* Chromium initializes both desktop GL and GLES entry points even when ANGLE
 * selected an ES backend. ANGLE exports the ES spelling for several desktop
 * calls, so adapt the few signatures that are not ABI-compatible and alias
 * the ones that are. Leaving these entries NULL makes Chromium's GL dispatch
 * table call address zero after eglInitialize succeeds. */
typedef void (WINAPI *angle_gl_depth_rangef_fn)(float near_value,
                                                float far_value);
typedef void (WINAPI *angle_gl_draw_buffers_fn)(int count,
                                                const DWORD *buffers);
typedef void (WINAPI *angle_gl_point_parameterf_fn)(DWORD pname, float value);

static angle_gl_depth_rangef_fn angle_gl_depth_rangef;
static angle_gl_draw_buffers_fn angle_gl_draw_buffers;
static angle_gl_point_parameterf_fn angle_gl_point_parameterf;

static void WINAPI angle_gl_depth_range_bridge(double near_value,
                                                double far_value)
{
    if (angle_gl_depth_rangef)
        angle_gl_depth_rangef((float)near_value, (float)far_value);
}

static void WINAPI angle_gl_draw_buffer_bridge(DWORD buffer)
{
    if (angle_gl_draw_buffers)
        angle_gl_draw_buffers(1, &buffer);
}

static void WINAPI angle_gl_point_parameteri_bridge(DWORD pname, int value)
{
    if (angle_gl_point_parameterf)
        angle_gl_point_parameterf(pname, (float)value);
}

static PVOID angle_gl_resolve_alias(PCSTR name)
{
    LOADED_MODULE *gles = dll_find_module("libglesv2.dll");
    return gles ? dll_resolve_export(gles, name, 0, FALSE) : NULL;
}

static PVOID angle_gl_compat_export(PCSTR name)
{
    PVOID target;
    PCSTR alias = NULL;

    if (!name || g_compat32_mode) return NULL;
    if (k32_strcmp(name, "glDepthRange") == 0) {
        target = angle_gl_resolve_alias("glDepthRangef");
        if (!target) return NULL;
        angle_gl_depth_rangef = (angle_gl_depth_rangef_fn)target;
        alias = "glDepthRangef";
        target = (PVOID)angle_gl_depth_range_bridge;
    } else if (k32_strcmp(name, "glDrawBuffer") == 0) {
        PVOID draw_buffers = angle_gl_resolve_alias("glDrawBuffers");
        if (!draw_buffers) return NULL;
        angle_gl_draw_buffers = (angle_gl_draw_buffers_fn)draw_buffers;
        alias = "glDrawBuffers";
        target = (PVOID)angle_gl_draw_buffer_bridge;
    } else if (k32_strcmp(name, "glGetQueryObjectiv") == 0) {
        alias = "glGetQueryObjectivEXT";
        target = angle_gl_resolve_alias(alias);
    } else if (k32_strcmp(name, "glMapBuffer") == 0) {
        alias = "glMapBufferOES";
        target = angle_gl_resolve_alias(alias);
    } else if (k32_strcmp(name, "glPointParameteri") == 0) {
        PVOID point_parameterf = angle_gl_resolve_alias("glPointParameterf");
        if (!point_parameterf) return NULL;
        angle_gl_point_parameterf =
            (angle_gl_point_parameterf_fn)point_parameterf;
        alias = "glPointParameterf";
        target = (PVOID)angle_gl_point_parameteri_bridge;
    } else if (k32_strcmp(name, "glPolygonMode") == 0) {
        alias = "glPolygonModeANGLE";
        target = angle_gl_resolve_alias(alias);
    } else {
        return NULL;
    }

    if (!target) return NULL;
    serial_puts("[ANGLE-GL] bridge ");
    serial_puts(name);
    serial_puts(" -> ");
    serial_puts(alias);
    serial_puts("\n");
    return target;
}

static ULONG angle_gpa_trace_count;

static BOOL angle_gpa_trace_begin(const LOADED_MODULE *mod, PCSTR name)
{
    if (!K32_VERBOSE_DIAGNOSTICS || !mod || !name ||
        angle_gpa_trace_count >= 512)
        return FALSE;
    if (k32_strcmp(mod->name, "libegl.dll") != 0 &&
        k32_strcmp(mod->name, "libglesv2.dll") != 0)
        return FALSE;

    angle_gpa_trace_count++;
    serial_puts("[ANGLE-GPA] ");
    serial_puts(mod->name);
    serial_puts("!");
    serial_puts(name);
    serial_puts(" request\n");
    return TRUE;
}

static void angle_gpa_trace_result(PCSTR name, PVOID result,
                                   PCSTR resolution)
{
    serial_puts("[ANGLE-GPA] ");
    serial_puts(name);
    serial_puts(" -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)result, 16);
    serial_puts(" (");
    serial_puts(resolution);
    serial_puts(")\n");
}

PVOID WINAPI GetProcAddress(HANDLE hModule, PCSTR lpProcName)
{
    if (!lpProcName) return NULL;
    ULONG_PTR proc_value = (ULONG_PTR)lpProcName;
    BOOL by_ordinal = proc_value <= 0xFFFF;
    USHORT ordinal = by_ordinal ? (USHORT)proc_value : 0;
    const char *proc_name = by_ordinal ? NULL : lpProcName;
    BOOL trace_angle_request = FALSE;
    LOADED_MODULE *loaded_module = hModule
        ? dll_find_module_by_base((PVOID)hModule) : NULL;
    BOOL trace_lwjgl_context = !by_ordinal &&
        k32_path_contains_ci(proc_name, "WindowsContextImplementation");
    BOOL trace_lwjgl_module = loaded_module &&
        k32_path_contains_ci(loaded_module->name, "lwjgl.dll");
    BOOL trace_lwjgl_gpa = trace_lwjgl_context || trace_lwjgl_module;

    if (trace_lwjgl_gpa) {
        serial_puts("[LWJGL-JNI-GPA] request ");
        if (by_ordinal)
            serial_putdec(ordinal);
        else
            serial_puts(proc_name);
        serial_puts(" hmod=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hModule, 8);
        serial_puts(" module=");
        serial_puts(loaded_module && loaded_module->name[0]
                        ? loaded_module->name : "<unknown>");
        serial_puts("\n");
    }

    /* If hModule is a loaded PE module, search its exports */
    if (loaded_module && loaded_module->synthetic_shim) {
        /* Shim handle — route to the correct shim DLL */
        const char *shim_dll = loaded_module->name;
        if (shim_dll) {
            BOOL trace_cef_delay = K32_VERBOSE_DIAGNOSTICS &&
                (k32_path_contains_ci(shim_dll, "user32.dll") ||
                 k32_path_contains_ci(shim_dll, "shell32.dll") ||
                 k32_path_contains_ci(shim_dll, "oleacc.dll")) &&
                __atomic_fetch_sub(&g_cef_delay_trace_budget, 1,
                                   __ATOMIC_RELAXED) > 0;
            if (trace_cef_delay) {
                serial_puts("[K32-DELAY] GetProcAddress ");
                serial_puts(shim_dll);
                serial_puts("!");
                if (by_ordinal)
                    serial_putdec(ordinal);
                else
                    serial_puts(proc_name);
                serial_puts("\n");
            }
            PVOID fn = dll_resolve_shim_export(shim_dll, proc_name, ordinal,
                                               by_ordinal);
            if (fn) {
                if (trace_cef_delay) {
                    serial_puts("[K32-DELAY] resolved 0x");
                    serial_puthex((uint64_t)(ULONG_PTR)fn, 16);
                    serial_puts("\n");
                }
                /* PE32 code calls this directly in compat32 mode.
                 * Must return a 32-bit thunk (INT 0x2E stub), not the
                 * raw 64-bit pointer — otherwise the REX prefixes in
                 * 64-bit code get misinterpreted as INC/DEC in 32-bit. */
                if (g_compat32_mode) {
                    /* Use the co-located ABI descriptor for the real argc +
                     * callconv (Phase 1), NOT a hardcoded 4. A wrong argc here
                     * makes the thunk's RET N over/under-clean the caller stack
                     * and corrupt its saved callee-saved registers (e.g.
                     * DirectDrawCreate is 3 args, not 4 — argc=4 over-cleaned 4
                     * bytes and clobbered UWindowsClient::Init's saved EBX). */
                    extern uint32_t compat32_make_thunk_ex(uint64_t, const char *,
                                                           uint8_t, uint8_t);
                    uint8_t nargs = 4, cc = 0 /* CC_STDCALL */;
                    const char *thunk_name = proc_name;
                    if (by_ordinal)
                        win32_abi_lookup_target(shim_dll, fn, &thunk_name,
                                                &nargs, &cc);
                    else
                        win32_abi_lookup(shim_dll, proc_name, &nargs, &cc);
                    uint32_t thunk = compat32_make_thunk_ex(
                        (uint64_t)(ULONG_PTR)fn,
                        thunk_name ? thunk_name : "ordinal", nargs, cc);
                    if (thunk)
                        return (PVOID)(ULONG_PTR)thunk;
                }
                return dll_get_shim_export_thunk(shim_dll, fn);
            }
            if (trace_cef_delay)
                serial_puts("[K32-DELAY] unresolved\n");
            SetLastError(127); /* ERROR_PROC_NOT_FOUND */
            return NULL;
        }
    } else if (hModule) {
        /* Regular PE module — search its export directory */
        LOADED_MODULE search_mod = {0};
        LOADED_MODULE *mod = loaded_module;
        if (!mod) {
            if ((ULONG_PTR)hModule != win32_current_image_base()) {
                SetLastError(6); /* ERROR_INVALID_HANDLE */
                return NULL;
            }
            search_mod.image.ImageBase = hModule;
            mod = &search_mod;
        }
        if (!by_ordinal)
            trace_angle_request = angle_gpa_trace_begin(mod, proc_name);
        PVOID fn = dll_resolve_export(mod, proc_name, ordinal, by_ordinal);
        if (fn) {
            PVOID result = by_ordinal ? fn : angle_trace_egl_export(
                proc_name, bridge_steamservice_export(mod, proc_name, fn));
            if (trace_lwjgl_gpa) {
                serial_puts("[LWJGL-JNI-GPA] resolved 0x");
                serial_puthex((uint64_t)(ULONG_PTR)result, 8);
                serial_puts("\n");
            }
            if (trace_angle_request)
                angle_gpa_trace_result(proc_name, result, "export");
            return result;
        }
        if (!by_ordinal) {
            /* Chromium asks ANGLE's EGL module for the GL dispatch table.
             * Resolve those entries from its paired GLES module explicitly;
             * the generic fallback searches built-in shims first and would
             * otherwise mix opengl32_shim functions into ANGLE's table. */
            if (k32_strcmp(mod->name, "libegl.dll") == 0 &&
                proc_name[0] == 'g' && proc_name[1] == 'l') {
                LOADED_MODULE *gles = dll_find_module("libglesv2.dll");
                PVOID gles_fn = gles
                    ? dll_resolve_export(gles, proc_name, 0, FALSE) : NULL;
                if (gles_fn) {
                    PVOID result = angle_trace_egl_export(proc_name, gles_fn);
                    if (trace_angle_request)
                        angle_gpa_trace_result(proc_name, result, "gles");
                    return result;
                }
            }
            PVOID compat = angle_gl_compat_export(proc_name);
            if (compat) {
                if (trace_angle_request)
                    angle_gpa_trace_result(proc_name, compat, "compat");
                return compat;
            }
        }
    }

    /* Fall back to searching all shims and modules */
    PVOID result = dll_resolve_import("", proc_name, ordinal, by_ordinal);
    if (trace_lwjgl_gpa) {
        serial_puts("[LWJGL-JNI-GPA] fallback 0x");
        serial_puthex((uint64_t)(ULONG_PTR)result, 8);
        serial_puts("\n");
    }
    if (!result && K32_VERBOSE_DIAGNOSTICS) {
        serial_puts("[GPA] UNRESOLVED: ");
        if (by_ordinal) {
            serial_puts("ordinal ");
            serial_putdec(ordinal);
        } else {
            serial_puts(proc_name);
        }
        serial_puts(" hMod=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hModule, 8);
        serial_puts("\n");
    }
    if (trace_angle_request)
        angle_gpa_trace_result(proc_name, result,
                               result ? "global" : "missing");
    return result;
}

HANDLE WINAPI GetModuleHandleA(PCSTR lpModuleName)
{
    if (!lpModuleName) {
        /* NULL → return main EXE module handle (= ImageBase) */
        ULONG_PTR image_base = win32_current_image_base();
        if (!image_base) {
            SetLastError(126); /* ERROR_MOD_NOT_FOUND */
            return NULL;
        }
        return (HANDLE)image_base;
    }
    /* Named module: try DLL lookup */
    PVOID h = dll_get_module_handle(lpModuleName, FALSE);
    if (!h)
        h = dll_get_shim_module_handle(lpModuleName, FALSE);
    if (!h)
        SetLastError(126); /* ERROR_MOD_NOT_FOUND */
    return (HANDLE)h;
}

HANDLE WINAPI GetModuleHandleW(PCWSTR lpModuleName)
{
    if (!lpModuleName) return GetModuleHandleA(NULL);
    /* Convert to ASCII and delegate */
    char narrow[260];
    int i = 0;
    while (lpModuleName[i] && i < 259) { narrow[i] = (char)lpModuleName[i]; i++; }
    narrow[i] = 0;
    return GetModuleHandleA(narrow);
}

static BOOL store_module_handle(PHANDLE output, HANDLE module)
{
    if (!output || !module) {
        SetLastError(output ? 126 : 87); /* MOD_NOT_FOUND / INVALID_PARAMETER */
        return FALSE;
    }
    if (g_compat32_mode)
        *(DWORD *)(void *)output = (DWORD)(ULONG_PTR)module;
    else
        *output = module;
    return TRUE;
}

#define K32_GET_MODULE_HANDLE_EX_FLAG_PIN                0x00000001U
#define K32_GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002U
#define K32_GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS       0x00000004U

static PVOID k32_main_image_from_address(PVOID address)
{
    BYTE *base = (BYTE *)win32_current_image_base();
    if (!base) return NULL;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < (LONG)sizeof(*dos) || dos->e_lfanew >= 0x1000)
        return NULL;

    BYTE *nt_base = base + dos->e_lfanew;
    if (*(ULONG *)nt_base != IMAGE_NT_SIGNATURE) return NULL;
    USHORT magic = *(USHORT *)(nt_base + sizeof(ULONG) +
                               sizeof(IMAGE_FILE_HEADER));
    ULONG size;
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        size = ((PIMAGE_NT_HEADERS32)nt_base)->OptionalHeader.SizeOfImage;
    else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        size = ((PIMAGE_NT_HEADERS64)nt_base)->OptionalHeader.SizeOfImage;
    else
        return NULL;

    ULONG_PTR value = (ULONG_PTR)address;
    ULONG_PTR start = (ULONG_PTR)base;
    return value >= start && value - start < size ? base : NULL;
}

static HANDLE k32_get_module_handle_ex_a(DWORD flags, PCSTR module_name)
{
    BOOL pin = (flags & K32_GET_MODULE_HANDLE_EX_FLAG_PIN) != 0;
    BOOL add_reference =
        (flags & (K32_GET_MODULE_HANDLE_EX_FLAG_PIN |
                  K32_GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) == 0;

    if (flags & K32_GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        PVOID address = (PVOID)(ULONG_PTR)module_name;
        PVOID found = dll_get_module_handle_by_address(
            address, add_reference, pin);
        if (!found)
            found = k32_main_image_from_address(address);
        return (HANDLE)found;
    }

    if (!module_name)
        return GetModuleHandleA(NULL);

    PVOID found = dll_get_module_handle_ex(module_name, add_reference, pin);
    if (!found)
        found = dll_get_shim_module_handle_ex(module_name, add_reference, pin);
    return (HANDLE)found;
}

BOOL WINAPI GetModuleHandleExA(DWORD flags, PCSTR module_name, PHANDLE module)
{
    if (!module || (flags & ~7U) || ((flags & 3U) == 3U) ||
        ((flags & K32_GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
         !module_name)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    return store_module_handle(module,
                               k32_get_module_handle_ex_a(flags, module_name));
}

BOOL WINAPI GetModuleHandleExW(DWORD flags, PCWSTR module_name, PHANDLE module)
{
    if (!module || (flags & ~7U) || ((flags & 3U) == 3U) ||
        ((flags & K32_GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
         !module_name)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    if ((flags & K32_GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) || !module_name)
        return store_module_handle(
            module, k32_get_module_handle_ex_a(flags, (PCSTR)module_name));

    char narrow[260];
    int i = 0;
    while (module_name[i] && i < 259) {
        narrow[i] = (char)module_name[i];
        i++;
    }
    narrow[i] = 0;
    return store_module_handle(module,
                               k32_get_module_handle_ex_a(flags, narrow));
}

static PVOID WINAPI RtlPcToFileHeader_k32(PVOID pc, PVOID *image_base)
{
    if (!image_base) return NULL;
    *image_base = NULL;

    LOADED_MODULE *module = dll_find_module_by_address(pc);
    if (module) return *image_base = module->image.ImageBase;

    return *image_base = k32_main_image_from_address(pc);
}

static BOOL WINAPI RtlAddFunctionTable_k32(PVOID function_table,
                                            DWORD entry_count,
                                            ULONGLONG base_address)
{
    (void)function_table;
    (void)entry_count;
    (void)base_address;
    return TRUE;
}

static BOOL WINAPI RtlDeleteFunctionTable_k32(PVOID function_table)
{
    (void)function_table;
    return TRUE;
}

static PVOID WINAPI RtlLookupFunctionEntry_k32(ULONGLONG control_pc,
                                                ULONGLONG *image_base,
                                                PVOID history_table)
{
    (void)control_pc;
    (void)history_table;
    if (image_base) *image_base = 0;
    return NULL;
}

/* ── File extended API ──────────────────────────────────────── */

DWORD WINAPI GetFileSize(HANDLE hFile, DWORD *lpFileSizeHigh)
{
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION info;

    NTSTATUS status = NtQueryInformationFile(hFile, &iosb, &info,
                                              sizeof(info),
                                              FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return (DWORD)-1;
    }

    DWORD result = (DWORD)(info.EndOfFile.QuadPart & 0xFFFFFFFF);

    /* Log file size for debugging localization file reads */
    static int gfs_log_count = 0;
    if (gfs_log_count < 20) {
        serial_puts("[GetFileSize] h=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hFile, 4);
        serial_puts(" size=");
        serial_putdec(result);
        serial_puts("\n");
        gfs_log_count++;
    }

    if (lpFileSizeHigh)
        *lpFileSizeHigh = (DWORD)(info.EndOfFile.QuadPart >> 32);

    return result;
}

static BOOL WINAPI GetFileSizeEx_k32(HANDLE hFile, PLARGE_INTEGER file_size)
{
    if (!file_size) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION info;
    NTSTATUS status = NtQueryInformationFile(hFile, &iosb, &info,
                                              sizeof(info),
                                              FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }

    file_size->QuadPart = info.EndOfFile.QuadPart;
    return TRUE;
}

#define FILE_BEGIN   0
#define FILE_CURRENT 1
#define FILE_END     2

static BOOL set_file_pointer_internal(HANDLE hFile, LONGLONG distance,
                                      DWORD move_method,
                                      PLARGE_INTEGER result)
{
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION pos_info;
    FILE_STANDARD_INFORMATION std_info;
    LONGLONG base;
    NTSTATUS status;

    switch (move_method) {
    case FILE_BEGIN:
        base = 0;
        break;

    case FILE_CURRENT:
        status = NtQueryInformationFile(hFile, &iosb, &pos_info,
                                        sizeof(pos_info),
                                        FilePositionInformation);
        if (!NT_SUCCESS(status)) {
            set_last_error_from_status(status);
            return FALSE;
        }
        base = pos_info.CurrentByteOffset.QuadPart;
        break;

    case FILE_END:
        status = NtQueryInformationFile(hFile, &iosb, &std_info,
                                        sizeof(std_info),
                                        FileStandardInformation);
        if (!NT_SUCCESS(status)) {
            set_last_error_from_status(status);
            return FALSE;
        }
        base = std_info.EndOfFile.QuadPart;
        break;

    default:
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    LONGLONG new_pos;
    if (__builtin_add_overflow(base, distance, &new_pos) || new_pos < 0) {
        SetLastError(131); /* ERROR_NEGATIVE_SEEK */
        return FALSE;
    }

    pos_info.CurrentByteOffset.QuadPart = new_pos;
    status = NtSetInformationFile(hFile, &iosb, &pos_info, sizeof(pos_info),
                                  FilePositionInformation);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }

    if (result)
        result->QuadPart = new_pos;
    return TRUE;
}

DWORD WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove,
                    LONG *lpDistanceToMoveHigh, DWORD dwMoveMethod)
{
    static volatile uint32_t set_pointer_trace_count;
    BOOL trace = __sync_fetch_and_add(&set_pointer_trace_count, 1) < 96;
    LARGE_INTEGER distance;
    LARGE_INTEGER result;
    distance.LowPart = (ULONG)lDistanceToMove;
    distance.HighPart = lpDistanceToMoveHigh ? *lpDistanceToMoveHigh
                                             : (lDistanceToMove < 0 ? -1 : 0);

    if (trace) {
        serial_puts("[K32-SEEK] handle=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hFile, 8);
        serial_puts(" distance=0x");
        serial_puthex((uint64_t)distance.QuadPart, 16);
        serial_puts(" method=");
        serial_putdec(dwMoveMethod);
        serial_puts(" high_ptr=0x");
        serial_puthex((uint64_t)(ULONG_PTR)lpDistanceToMoveHigh, 8);
        serial_puts("\n");
    }

    if (!set_file_pointer_internal(hFile, distance.QuadPart,
                                   dwMoveMethod, &result)) {
        if (trace) {
            serial_puts("[K32-SEEK] failed error=");
            serial_putdec(GetLastError());
            serial_puts("\n");
        }
        return (DWORD)-1; /* INVALID_SET_FILE_POINTER */
    }

    if (lpDistanceToMoveHigh)
        *lpDistanceToMoveHigh = result.HighPart;

    if (trace) {
        serial_puts("[K32-SEEK] result=0x");
        serial_puthex((uint64_t)result.QuadPart, 16);
        serial_puts("\n");
    }

    return result.LowPart;
}

/* PE32 passes LARGE_INTEGER by value as two 32-bit stack words. */
static BOOL WINAPI SetFilePointerEx_k32(HANDLE hFile, uint32_t distance_low,
                                         uint32_t distance_high,
                                         PLARGE_INTEGER new_position,
                                         DWORD move_method)
{
    LARGE_INTEGER distance;
    distance.LowPart = distance_low;
    distance.HighPart = (LONG)distance_high;

    return set_file_pointer_internal(hFile, distance.QuadPart, move_method,
                                     new_position);
}

static BOOL WINAPI SetFilePointerEx_k64(HANDLE hFile, LARGE_INTEGER distance,
                                         PLARGE_INTEGER new_position,
                                         DWORD move_method)
{
    return set_file_pointer_internal(hFile, distance.QuadPart, move_method,
                                     new_position);
}

/* ── File copy/delete/move (UT99/Steam) ─────────────────────── */

typedef DWORD (WINAPI *K32_COPY_PROGRESS_ROUTINE)(
    LARGE_INTEGER total_file_size, LARGE_INTEGER total_bytes_transferred,
    LARGE_INTEGER stream_size, LARGE_INTEGER stream_bytes_transferred,
    DWORD stream_number, DWORD callback_reason, HANDLE source_file,
    HANDLE destination_file, PVOID data);

static DWORD k32_copy_progress_call(PVOID routine, uint64_t total,
                                    uint64_t transferred, HANDLE source,
                                    HANDLE destination, PVOID data)
{
    if (g_compat32_mode) {
        uint32_t args[13] = {
            (uint32_t)total, (uint32_t)(total >> 32),
            (uint32_t)transferred, (uint32_t)(transferred >> 32),
            (uint32_t)total, (uint32_t)(total >> 32),
            (uint32_t)transferred, (uint32_t)(transferred >> 32),
            1, 0, (uint32_t)(ULONG_PTR)source,
            (uint32_t)(ULONG_PTR)destination, (uint32_t)(ULONG_PTR)data
        };
        return compat32_callback_args((uint32_t)(ULONG_PTR)routine, 13, args);
    }

    LARGE_INTEGER total_size;
    LARGE_INTEGER bytes_transferred;
    total_size.QuadPart = (LONGLONG)total;
    bytes_transferred.QuadPart = (LONGLONG)transferred;
    return ((K32_COPY_PROGRESS_ROUTINE)routine)(
        total_size, bytes_transferred, total_size, bytes_transferred,
        1, 0 /* CALLBACK_CHUNK_FINISHED */, source, destination, data);
}

static BOOL copy_file_common_a(PCSTR lpExistingFileName,
                               PCSTR lpNewFileName, BOOL bFailIfExists,
                               PVOID progress_routine, PVOID progress_data,
                               const BOOL *cancel)
{
    extern void *kmalloc(uint64_t size);
    extern void kfree(void *ptr);
    extern void *osfs2_find_ci(const char *name);
    static uint32_t trace_count;
    bool trace = trace_count < 32;
    HANDLE source = INVALID_HANDLE_VALUE;
    HANDLE destination = INVALID_HANDLE_VALUE;
    uint8_t *buffer = NULL;
    DWORD error = 0;
    uint64_t copied = 0;
    BOOL ok = FALSE;
    bool destination_existed = false;
    bool delete_partial = false;
    uint64_t total_size = 0;

    if (trace) {
        trace_count++;
        serial_puts("[K32-COPY] '");
        serial_puts(lpExistingFileName ? lpExistingFileName : "<null>");
        serial_puts("' -> '");
        serial_puts(lpNewFileName ? lpNewFileName : "<null>");
        serial_puts("' fail_if_exists=");
        serial_putdec(bFailIfExists != FALSE);
    }

    if (!lpExistingFileName || !lpNewFileName) {
        error = 87; /* ERROR_INVALID_PARAMETER */
        goto done;
    }
    if (cancel && *cancel) {
        error = 1235; /* ERROR_REQUEST_ABORTED */
        goto done;
    }

    char source_path[260], destination_path[260];
    if (!win32_normalize_path(lpExistingFileName, source_path) ||
        !win32_normalize_path(lpNewFileName, destination_path)) {
        error = 206; /* ERROR_FILENAME_EXCED_RANGE */
        goto done;
    }
    void *source_file = osfs2_find_ci(source_path);
    void *destination_file = osfs2_find_ci(destination_path);
    destination_existed = destination_file != NULL;
    if (!bFailIfExists && source_file && source_file == destination_file) {
        error = 32; /* ERROR_SHARING_VIOLATION */
        goto done;
    }

    source = CreateFileA(lpExistingFileName, GENERIC_READ, FILE_SHARE_READ,
                         NULL, 3 /* OPEN_EXISTING */, FILE_ATTRIBUTE_NORMAL,
                         NULL);
    if (source == INVALID_HANDLE_VALUE) {
        error = g_last_error;
        goto done;
    }
    LARGE_INTEGER source_size;
    if (!GetFileSizeEx_k32(source, &source_size)) {
        error = g_last_error;
        goto done;
    }
    total_size = (uint64_t)source_size.QuadPart;

    destination = CreateFileA(lpNewFileName, GENERIC_WRITE, 0, NULL,
                              bFailIfExists ? 1 /* CREATE_NEW */
                                            : 2 /* CREATE_ALWAYS */,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (destination == INVALID_HANDLE_VALUE) {
        error = g_last_error;
        goto done;
    }

    buffer = (uint8_t *)kmalloc(1024 * 1024);
    if (!buffer) {
        error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        goto done;
    }

    for (;;) {
        if (cancel && *cancel) {
            delete_partial = true;
            error = 1235; /* ERROR_REQUEST_ABORTED */
            goto done;
        }
        DWORD bytes_read = 0;
        if (!ReadFile(source, buffer, 1024 * 1024, &bytes_read, NULL)) {
            error = g_last_error;
            goto done;
        }
        if (!bytes_read) break;

        DWORD offset = 0;
        while (offset < bytes_read) {
            DWORD bytes_written = 0;
            if (!WriteFile(destination, buffer + offset, bytes_read - offset,
                           &bytes_written, NULL) || !bytes_written) {
                error = g_last_error ? g_last_error : 5; /* ERROR_ACCESS_DENIED */
                goto done;
            }
            offset += bytes_written;
        }
        copied += bytes_read;

        if (progress_routine) {
            DWORD action = k32_copy_progress_call(
                progress_routine, total_size, copied, source, destination,
                progress_data);
            if (action == 1 /* PROGRESS_CANCEL */) {
                delete_partial = true;
                error = 1235;
                goto done;
            }
            if (action == 2 /* PROGRESS_STOP */) {
                error = 1235;
                goto done;
            }
            if (action == 3 /* PROGRESS_QUIET */)
                progress_routine = NULL;
        }
    }
    ok = TRUE;

done:
    if (buffer) kfree(buffer);
    if (destination != INVALID_HANDLE_VALUE) CloseHandle(destination);
    if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
    if (!ok && destination != INVALID_HANDLE_VALUE &&
        (!destination_existed || delete_partial))
        DeleteFileA(lpNewFileName);

    g_last_error = ok ? 0 : error ? error : 5;
    sync_last_error();
    if (trace) {
        serial_puts(" result=");
        serial_putdec(ok);
        serial_puts(" bytes=");
        serial_putdec(copied);
        serial_puts(" error=");
        serial_putdec(g_last_error);
        serial_puts("\n");
    }
    return ok;
}

BOOL WINAPI CopyFileA(PCSTR lpExistingFileName, PCSTR lpNewFileName,
                      BOOL bFailIfExists)
{
    return copy_file_common_a(lpExistingFileName, lpNewFileName,
                              bFailIfExists, NULL, NULL, NULL);
}

BOOL WINAPI CopyFileW(PCWSTR lpExistingFileName, PCWSTR lpNewFileName, BOOL bFailIfExists)
{
    if (!lpExistingFileName || !lpNewFileName)
        return CopyFileA(NULL, NULL, bFailIfExists);

    char from[260], to[260];
    int i = 0;
    while (lpExistingFileName[i] && i < 259) {
        from[i] = (char)(lpExistingFileName[i] & 0xFF);
        i++;
    }
    from[i] = 0;
    i = 0;
    while (lpNewFileName[i] && i < 259) {
        to[i] = (char)(lpNewFileName[i] & 0xFF);
        i++;
    }
    to[i] = 0;
    return copy_file_common_a(from, to, bFailIfExists, NULL, NULL, NULL);
}

static BOOL WINAPI CopyFileExA_k32(PCSTR existing_name, PCSTR new_name,
                                    PVOID progress_routine, PVOID data,
                                    BOOL *cancel, DWORD flags)
{
    return copy_file_common_a(existing_name, new_name, (flags & 1) != 0,
                              progress_routine, data, cancel);
}

static BOOL WINAPI CopyFileExW_k32(PCWSTR existing_name, PCWSTR new_name,
                                    PVOID progress_routine, PVOID data,
                                    BOOL *cancel, DWORD flags)
{
    if (!existing_name || !new_name)
        return copy_file_common_a(NULL, NULL, (flags & 1) != 0,
                                  progress_routine, data, cancel);

    char from[260], to[260];
    int i = 0;
    while (existing_name[i] && i < 259) {
        from[i] = (char)(existing_name[i] & 0xFF);
        i++;
    }
    from[i] = 0;
    i = 0;
    while (new_name[i] && i < 259) {
        to[i] = (char)(new_name[i] & 0xFF);
        i++;
    }
    to[i] = 0;
    return copy_file_common_a(from, to, (flags & 1) != 0,
                              progress_routine, data, cancel);
}

static BOOL delete_osfs_file(PCSTR path)
{
    extern void *osfs2_find_ci(const char *name);
    extern const char *osfs2_file_name(void *file);
    extern int osfs2_delete(const char *name);

    if (!path) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        sync_last_error();
        return FALSE;
    }

    char fs_path[260];
    if (!win32_normalize_path(path, fs_path)) {
        g_last_error = 206; /* ERROR_FILENAME_EXCED_RANGE */
        sync_last_error();
        return FALSE;
    }
    path = fs_path;

    void *file = osfs2_find_ci(path);
    if (!file) {
        g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
        sync_last_error();
        return FALSE;
    }

    char stored_name[260];
    const char *stored = osfs2_file_name(file);
    int i = 0;
    while (stored && stored[i] && i < 259) {
        stored_name[i] = stored[i];
        i++;
    }
    if (!stored || stored[i]) {
        g_last_error = 206; /* ERROR_FILENAME_EXCED_RANGE */
        sync_last_error();
        return FALSE;
    }
    stored_name[i] = 0;

    int result = osfs2_delete(stored_name);
    g_last_error = result == 0 ? 0 : result == -2 ? 32 : 5;
    sync_last_error();
    return result == 0;
}

BOOL WINAPI DeleteFileA(PCSTR lpFileName)
{
    return delete_osfs_file(lpFileName);
}

BOOL WINAPI DeleteFileW(PCWSTR lpFileName)
{
    if (!lpFileName) return delete_osfs_file(NULL);
    char path[260];
    int i = 0;
    while (lpFileName[i] && i < 259) {
        path[i] = (char)(lpFileName[i] & 0xFF);
        i++;
    }
    path[i] = 0;
    return delete_osfs_file(path);
}

typedef struct {
    char from[K32_VIRTUAL_DIR_PATH];
    char to[K32_VIRTUAL_DIR_PATH];
} K32_DIRECTORY_FILE_MOVE;

static BOOL move_osfs_directory_normalized(const char *old_path,
                                            const char *new_path)
{
    extern void *kmalloc(uint64_t size);
    extern void kfree(void *ptr);
    extern int osfs2_rename(const char *from, const char *to, bool replace);

    if (!*old_path || !*new_path) {
        g_last_error = 5; /* ERROR_ACCESS_DENIED */
        sync_last_error();
        return FALSE;
    }
    if (k32_path_equal_ci(old_path, new_path)) {
        g_last_error = 0;
        sync_last_error();
        return TRUE;
    }
    if (k32_path_is_descendant(old_path, new_path)) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        sync_last_error();
        return FALSE;
    }
    if (!win32_directory_exists_normalized(old_path)) {
        g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
        sync_last_error();
        return FALSE;
    }
    if (k32_find_file_exact_ci(new_path) ||
        win32_directory_exists_normalized(new_path)) {
        g_last_error = 183; /* ERROR_ALREADY_EXISTS */
        sync_last_error();
        return FALSE;
    }
    if (!k32_directory_parent_exists(new_path)) {
        g_last_error = 3; /* ERROR_PATH_NOT_FOUND */
        sync_last_error();
        return FALSE;
    }

    /* OsitoFS3 has real directory inodes. Moving descendant files one by one
     * (the v2 fallback below) cannot work because the destination directory
     * does not exist until the directory itself is renamed. */
    if (osfs3_is_mounted()) {
        if (!k32_virtual_directory_move_preflight(old_path, new_path)) {
            g_last_error = 206; /* ERROR_FILENAME_EXCED_RANGE */
            sync_last_error();
            return FALSE;
        }
        int result = osfs2_rename(old_path, new_path, false);
        if (result == 0) {
            k32_virtual_directory_move_commit(old_path, new_path);
            g_last_error = 0;
            sync_last_error();
            return TRUE;
        }

        g_last_error = result == -2 ? 183 : result == -3 ? 32 : 5;
        sync_last_error();
        static uint32_t native_move_fail_logs;
        uint32_t log_index = __atomic_fetch_add(&native_move_fail_logs, 1,
                                                 __ATOMIC_RELAXED);
        if (log_index < 32) {
            serial_puts("[K32-DIR] native move failed '");
            serial_puts(old_path);
            serial_puts("' -> '");
            serial_puts(new_path);
            serial_puts("' result=");
            serial_putdec((uint64_t)(int64_t)result);
            serial_puts(" error=");
            serial_putdec(g_last_error);
            serial_puts("\n");
        }
        return FALSE;
    }

    /* Ensure even an inferred directory retains an empty record after move. */
    if (!k32_virtual_directory_insert(old_path)) {
        g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        sync_last_error();
        return FALSE;
    }
    if (!k32_virtual_directory_move_preflight(old_path, new_path)) {
        g_last_error = 206; /* ERROR_FILENAME_EXCED_RANGE */
        sync_last_error();
        return FALSE;
    }

    size_t old_length = strlen(old_path);
    size_t new_length = strlen(new_path);
    uint32_t capacity = osfs2_max_files();
    uint32_t count = 0;
    for (uint32_t i = 0; i < capacity; i++) {
        osfs2_file_t *file = osfs2_get_file((int)i);
        if (!file) continue;
        const char *stored = osfs2_file_name(file);
        if (!k32_path_is_descendant(old_path, stored)) continue;
        if (new_length + strlen(stored + old_length) >=
            K32_VIRTUAL_DIR_PATH) {
            g_last_error = 206;
            sync_last_error();
            return FALSE;
        }
        count++;
    }

    K32_DIRECTORY_FILE_MOVE *moves = NULL;
    if (count) {
        moves = (K32_DIRECTORY_FILE_MOVE *)kmalloc(
            (uint64_t)count * sizeof(*moves));
        if (!moves) {
            g_last_error = 8;
            sync_last_error();
            return FALSE;
        }
    }

    uint32_t filled = 0;
    for (uint32_t i = 0; i < capacity && filled < count; i++) {
        osfs2_file_t *file = osfs2_get_file((int)i);
        if (!file) continue;
        const char *stored = osfs2_file_name(file);
        if (!k32_path_is_descendant(old_path, stored)) continue;
        size_t from_length = strlen(stored);
        size_t suffix_length = from_length - old_length;
        memcpy(moves[filled].from, stored, from_length + 1);
        memcpy(moves[filled].to, new_path, new_length);
        memcpy(moves[filled].to + new_length, stored + old_length,
               suffix_length + 1);
        filled++;
    }
    count = filled;

    uint32_t moved = 0;
    for (; moved < count; moved++) {
        if (osfs2_rename(moves[moved].from, moves[moved].to, false) == 0)
            continue;

        while (moved) {
            moved--;
            osfs2_rename(moves[moved].to, moves[moved].from, false);
        }
        kfree(moves);
        g_last_error = 5; /* ERROR_ACCESS_DENIED */
        sync_last_error();
        return FALSE;
    }

    k32_virtual_directory_move_commit(old_path, new_path);
    if (moves) kfree(moves);
    g_last_error = 0;
    sync_last_error();
    if (K32_VERBOSE_DIAGNOSTICS) {
        serial_puts("[K32-DIR] moved '");
        serial_puts(old_path);
        serial_puts("' -> '");
        serial_puts(new_path);
        serial_puts("' files=");
        serial_putdec(count);
        serial_puts("\n");
    }
    return TRUE;
}

static BOOL move_osfs_file(PCSTR old_path, PCSTR new_path, bool replace)
{
    extern void *osfs2_find_ci(const char *name);
    extern const char *osfs2_file_name(void *file);
    extern int osfs2_rename(const char *from, const char *to, bool replace);
    static uint32_t trace_count;
    static uint32_t profile_move_logs;
    static uint32_t rebuild_move_logs;
    bool trace_rebuild =
        k32_path_contains_ci(old_path, "__tmp_for_rebuild") ||
        k32_path_contains_ci(new_path, "__tmp_for_rebuild");
    if (trace_rebuild) {
        uint32_t trace_index = __atomic_fetch_add(&rebuild_move_logs, 1,
                                                   __ATOMIC_RELAXED);
        trace_rebuild = trace_index < 128;
    }
    bool trace = trace_rebuild ||
                 (K32_VERBOSE_DIAGNOSTICS && trace_count < 32);
    if (K32_VERBOSE_DIAGNOSTICS && !trace &&
        (k32_is_profile_path(old_path) || k32_is_profile_path(new_path))) {
        uint32_t trace_index = __atomic_fetch_add(&profile_move_logs, 1,
                                                   __ATOMIC_RELAXED);
        trace = trace_index < 512;
    }

    if (trace) {
        if (trace_count < 32) trace_count++;
        serial_puts("[K32-MOVE] '");
        serial_puts(old_path ? old_path : "<null>");
        serial_puts("' -> '");
        serial_puts(new_path ? new_path : "<null>");
        serial_puts("' replace=");
        serial_putdec(replace);
    }

    if (!old_path || !new_path) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        sync_last_error();
        if (trace) serial_puts(" result=-1 error=87\n");
        return FALSE;
    }

    char old_name[260], new_name[260];
    if (!win32_normalize_path(old_path, old_name) ||
        !win32_normalize_path(new_path, new_name)) {
        g_last_error = 206; /* ERROR_FILENAME_EXCED_RANGE */
        sync_last_error();
        if (trace) serial_puts(" result=-1 error=206\n");
        return FALSE;
    }
    void *file = k32_find_file_exact_ci(old_name);
    if (!file) {
        BOOL moved_directory = move_osfs_directory_normalized(old_name,
                                                               new_name);
        if (trace) {
            serial_puts(" result=");
            serial_putdec(moved_directory ? 0 : (uint64_t)-1);
            serial_puts(" error=");
            serial_putdec(g_last_error);
            serial_puts("\n");
        }
        return moved_directory;
    }

    if (!*new_name || (!osfs3_is_mounted() && strlen(new_name) >= 128)) {
        g_last_error = 206; /* ERROR_FILENAME_EXCED_RANGE */
        sync_last_error();
        if (trace) serial_puts(" result=-1 error=206\n");
        return FALSE;
    }

    const char *stored_name = osfs2_file_name(file);
    int result = osfs2_rename(stored_name, new_name, replace);
    g_last_error = result == 0 ? 0 : result == -2 ? 183 :
                   result == -3 ? 32 : 5;
    sync_last_error();
    if (trace) {
        serial_puts(" result=");
        serial_putdec((uint64_t)(int64_t)result);
        serial_puts(" error=");
        serial_putdec(g_last_error);
        serial_puts("\n");
    }
    return result == 0;
}

BOOL WINAPI MoveFileA(PCSTR lpExistingFileName, PCSTR lpNewFileName)
{
    return move_osfs_file(lpExistingFileName, lpNewFileName, false);
}

static BOOL move_osfs_file_w(PCWSTR old_path, PCWSTR new_path, bool replace)
{
    if (!old_path || !new_path)
        return MoveFileA(NULL, NULL);

    char from[260], to[260];
    int i = 0;
    while (old_path[i] && i < 259) {
        from[i] = (char)(old_path[i] & 0xFF);
        i++;
    }
    from[i] = 0;
    i = 0;
    while (new_path[i] && i < 259) {
        to[i] = (char)(new_path[i] & 0xFF);
        i++;
    }
    to[i] = 0;
    return move_osfs_file(from, to, replace);
}

BOOL WINAPI MoveFileW(PCWSTR lpExistingFileName, PCWSTR lpNewFileName)
{
    return move_osfs_file_w(lpExistingFileName, lpNewFileName, false);
}

static BOOL WINAPI MoveFileExW_k32(PCWSTR existing_name, PCWSTR new_name,
                                    DWORD flags)
{
    return move_osfs_file_w(existing_name, new_name,
                            (flags & 1) != 0); /* MOVEFILE_REPLACE_EXISTING */
}

static BOOL WINAPI MoveFileTransactedW_k32(PCWSTR existing_name,
                                             PCWSTR new_name,
                                             PVOID progress_routine,
                                             PVOID progress_data,
                                             DWORD flags,
                                             HANDLE transaction)
{
    (void)progress_routine;
    (void)progress_data;
    (void)transaction;
    return MoveFileExW_k32(existing_name, new_name, flags);
}

static BOOL WINAPI ReplaceFileW_k32(PCWSTR replaced_name,
                                     PCWSTR replacement_name,
                                     PCWSTR backup_name,
                                     DWORD flags,
                                     PVOID exclude,
                                     PVOID reserved)
{
    (void)flags;
    (void)exclude;
    (void)reserved;
    if (backup_name && !CopyFileW(replaced_name, backup_name, FALSE))
        return FALSE;
    return move_osfs_file_w(replacement_name, replaced_name, true);
}

/* ponytail: transacted deletes commit immediately; add rollback when needed. */
static BOOL WINAPI DeleteFileTransactedW_k32(PCWSTR file_name,
                                               HANDLE transaction)
{
    (void)transaction;
    return DeleteFileW(file_name);
}

/* ── Handle duplication ─────────────────────────────────────── */

BOOL WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle,
                     HANDLE hTargetProcessHandle, PHANDLE lpTargetHandle,
                     DWORD dwDesiredAccess, BOOL bInheritHandle,
                     DWORD dwOptions)
{
    static ULONG trace_count;
    HANDLE_ENTRY *trace_entry = handle_get_entry(&g_handle_table,
                                                   hSourceHandle);
    BOOL trace = K32_VERBOSE_DIAGNOSTICS && trace_count < 64 &&
        ((dwDesiredAccess == 2 && dwOptions == 0) ||
         (trace_entry && trace_entry->type == OBJ_TYPE_PROCESS));
    if (trace) {
        HANDLE_ENTRY *entry = trace_entry;
        trace_count++;
        serial_puts("[K32-DUP] pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" ");
        serial_puts("[K32-DUP] src_proc=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hSourceProcessHandle, 16);
        serial_puts(" src=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hSourceHandle, 16);
        serial_puts(" type=0x");
        serial_puthex(entry ? (uint32_t)entry->type : 0, 8);
        serial_puts(" granted=0x");
        serial_puthex(entry ? entry->access : 0, 8);
        serial_puts(" refs=");
        serial_putdec(entry ? entry->refs : 0);
        serial_puts(" dst_proc=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hTargetProcessHandle, 16);
        serial_puts(" out=0x");
        serial_puthex((uint64_t)(ULONG_PTR)lpTargetHandle, 16);
        serial_puts(" access=0x");
        serial_puthex(dwDesiredAccess, 8);
        serial_puts(" inherit=");
        serial_putdec(bInheritHandle != FALSE);
        serial_puts(" options=0x");
        serial_puthex(dwOptions, 8);
        serial_puts(" caller=0x");
        serial_puthex((uint64_t)__builtin_return_address(0), 16);
        serial_puts("\n");
    }

    NTSTATUS status = NtDuplicateObject(hSourceProcessHandle, hSourceHandle,
                                         hTargetProcessHandle, lpTargetHandle,
                                         dwDesiredAccess, 0, dwOptions);
    if (trace) {
        serial_puts("[K32-DUP] status=0x");
        serial_puthex((uint32_t)status, 8);
        if (NT_SUCCESS(status) && lpTargetHandle) {
            serial_puts(" dst=0x");
            serial_puthex((uint64_t)(ULONG_PTR)*lpTargetHandle, 16);
        }
        serial_puts("\n");
    }
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

/* ── Memory protection ──────────────────────────────────────── */

BOOL WINAPI SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags)
{
    (void)dwMask;
    (void)dwFlags;
    if (!hObject || hObject == INVALID_HANDLE_VALUE) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    /* ponytail: handles are not inherited yet; store flags with process inheritance. */
    return TRUE;
}

BOOL WINAPI VirtualProtect(PVOID lpAddress, SIZE_T dwSize,
                    DWORD flNewProtect, DWORD *lpflOldProtect)
{
    PVOID base = lpAddress;
    SIZE_T size = dwSize;
    ULONG old_prot = 0;

    NTSTATUS status = NtProtectVirtualMemory(NT_CURRENT_PROCESS,
                                              &base, &size,
                                              flNewProtect, &old_prot);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }

    if (lpflOldProtect)
        *lpflOldProtect = old_prot;

    return TRUE;
}

static BOOL WINAPI VirtualProtectEx_k32(HANDLE process, PVOID address,
                                         SIZE_T size, DWORD new_protect,
                                         DWORD *old_protect)
{
    if (!k32_process_handle_valid(process)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    return VirtualProtect(address, size, new_protect, old_protect);
}

/* ── Memory query ──────────────────────────────────────────── */

typedef struct _MEMORY_BASIC_INFORMATION_K32 {
    PVOID       BaseAddress;
    PVOID       AllocationBase;
    ULONG       AllocationProtect;
    USHORT      PartitionId;
    USHORT      Padding0;
    SIZE_T      RegionSize;
    ULONG       State;
    ULONG       Protect;
    ULONG       Type;
    ULONG       Padding1;
} MEMORY_BASIC_INFORMATION_K32;

typedef struct _MEMORY_BASIC_INFORMATION32_K32 {
    uint32_t BaseAddress;
    uint32_t AllocationBase;
    ULONG    AllocationProtect;
    uint32_t RegionSize;
    ULONG    State;
    ULONG    Protect;
    ULONG    Type;
} MEMORY_BASIC_INFORMATION32_K32;

_Static_assert(sizeof(MEMORY_BASIC_INFORMATION_K32) == 48,
               "PE32+ MEMORY_BASIC_INFORMATION layout changed");
_Static_assert(sizeof(MEMORY_BASIC_INFORMATION32_K32) == 28,
               "PE32 MEMORY_BASIC_INFORMATION layout changed");

SIZE_T WINAPI VirtualQuery(PVOID lpAddress, PVOID lpBuffer, SIZE_T dwLength)
{
    SIZE_T required = g_compat32_mode
                      ? sizeof(MEMORY_BASIC_INFORMATION32_K32)
                      : sizeof(MEMORY_BASIC_INFORMATION_K32);
    if (!lpBuffer || dwLength < required)
        return 0;

    SIZE_T ret_len = 0;
    NTSTATUS status = NtQueryVirtualMemory(NT_CURRENT_PROCESS,
                                            lpAddress, 0 /* MemoryBasicInformation */,
                                            lpBuffer, dwLength, &ret_len);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return 0;
    }

    return ret_len;
}

static SIZE_T WINAPI VirtualQueryEx_k32(HANDLE process, PVOID address,
                                         PVOID buffer, SIZE_T length)
{
    if (!k32_process_handle_valid(process)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }
    return VirtualQuery(address, buffer, length);
}

/* ── String API (commonly needed by CRT) ────────────────────── */

int WINAPI lstrlenA(PCSTR lpString)
{
    if (!lpString) return 0;
    int len = 0;
    while (lpString[len]) len++;
    return len;
}

int WINAPI lstrlenW(PCWSTR lpString)
{
    if (!lpString) return 0;
    int len = 0;
    while (lpString[len]) len++;
    return len;
}

static PWSTR WINAPI lstrcpynW_k32(PWSTR dst, PCWSTR src, int max_chars)
{
    if (!dst || max_chars <= 0) return dst;
    int i = 0;
    if (src)
        while (i < max_chars - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}

static PSTR WINAPI lstrcpyA_k32(PSTR dst, PCSTR src)
{
    PSTR out = dst;
    if (!dst) return NULL;
    if (!src) { *dst = 0; return out; }
    while ((*dst++ = *src++)) {}
    return out;
}

static PWSTR WINAPI lstrcpyW_k32(PWSTR dst, PCWSTR src)
{
    PWSTR out = dst;
    if (!dst) return NULL;
    if (!src) { *dst = 0; return out; }
    while ((*dst++ = *src++)) {}
    return out;
}

static PWSTR WINAPI lstrcatW_k32(PWSTR dst, PCWSTR src)
{
    if (!dst) return NULL;
    PWSTR end = dst;
    while (*end) end++;
    lstrcpyW_k32(end, src);
    return dst;
}

static int WINAPI lstrcmpW_k32(PCWSTR a, PCWSTR b)
{
    if (!a || !b) return a ? 1 : b ? -1 : 0;
    while (*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}

static int WINAPI lstrcmpiA_k32(PCSTR a, PCSTR b)
{
    if (!a || !b) return a ? 1 : b ? -1 : 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return (int)ca - (int)cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int WINAPI lstrcmpiW_k32(PCWSTR a, PCWSTR b)
{
    if (!a || !b) return a ? 1 : b ? -1 : 0;
    while (*a && *b) {
        WCHAR ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return (int)ca - (int)cb;
    }
    return (int)*a - (int)*b;
}
/* ── Command line ────────────────────────────────────── */

/* Command line: omitting the map arg makes the engine fall through
 * to its DEFAULT URL (Entry.unr → main menu).  With "CityIntro.unr"
 * the engine attempts to load it but ends up trying to load package
 * "0" (some FName index resolves to empty/zero) and throws "Can't
 * find file for package '0'".  Bare exe → menu may work better. */
/* Keep the default command line bare. Map-specific repros should be injected
 * by launch scripts or image contents, not hardcoded into kernel32. */
PCSTR WINAPI GetCommandLineA(void)
{
    return win32_current_command_line();
}

PCWSTR WINAPI GetCommandLineW(void)
{
    const WCHAR *child_command = win32_current_command_line_w();
    if (child_command) return child_command;

    const char *current = win32_current_command_line();
    static WCHAR cmdline[4096];
    int i = 0;
    while (current[i] && i < 4095) {
        cmdline[i] = (WCHAR)(unsigned char)current[i];
        i++;
    }
    cmdline[i] = 0;
    return cmdline;
}

/* ── Environment (stub) ─────────────────────────────────────── */

PCSTR WINAPI GetEnvironmentStringsA(void)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        SIZE_T chars = kernel32_build_environment_block_w(
            GetCurrentProcessId(), NULL, 0);
        if (chars < 2) chars = 2;
        PWSTR wide = (PWSTR)HeapAlloc(GetProcessHeap(), 0,
                                      chars * sizeof(WCHAR));
        PSTR ansi = (PSTR)HeapAlloc(GetProcessHeap(), 0, chars);
        if (!wide || !ansi) {
            if (wide) HeapFree(GetProcessHeap(), 0, wide);
            if (ansi) HeapFree(GetProcessHeap(), 0, ansi);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        SIZE_T actual = kernel32_build_environment_block_w(
            GetCurrentProcessId(), wide, chars);
        if (actual <= chars) {
            for (SIZE_T i = 0; i < actual; i++)
                ansi[i] = (char)(wide[i] & 0xFF);
            HeapFree(GetProcessHeap(), 0, wide);
            SetLastError(0);
            return ansi;
        }
        HeapFree(GetProcessHeap(), 0, wide);
        HeapFree(GetProcessHeap(), 0, ansi);
    }
    SetLastError(8);
    return NULL;
}

BOOL WINAPI FreeEnvironmentStringsA(PCSTR lpszEnvironmentBlock)
{
    if (!lpszEnvironmentBlock) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    return HeapFree(GetProcessHeap(), 0, (PVOID)lpszEnvironmentBlock);
}

PCWSTR WINAPI GetEnvironmentStringsW(void)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        SIZE_T chars = kernel32_build_environment_block_w(
            GetCurrentProcessId(), NULL, 0);
        if (chars < 2) chars = 2;
        PWSTR block = (PWSTR)HeapAlloc(GetProcessHeap(), 0,
                                       chars * sizeof(WCHAR));
        if (!block) {
            SetLastError(8);
            return NULL;
        }
        SIZE_T actual = kernel32_build_environment_block_w(
            GetCurrentProcessId(), block, chars);
        if (actual <= chars) {
            SetLastError(0);
            return block;
        }
        HeapFree(GetProcessHeap(), 0, block);
    }
    SetLastError(8);
    return NULL;
}

BOOL WINAPI FreeEnvironmentStringsW(PCWSTR lpszEnvironmentBlock)
{
    if (!lpszEnvironmentBlock) {
        SetLastError(87);
        return FALSE;
    }
    return HeapFree(GetProcessHeap(), 0, (PVOID)lpszEnvironmentBlock);
}

/* ── Critical Section ───────────────────────────────────────── */
/*
 * Real spinlock-based critical sections for preemptive Win32 threads.
 * Uses OwningThread to track the owning thread ID for recursive locking.
 * Spins briefly then yields to the scheduler if the lock is held.
 */

extern DWORD WINAPI GetCurrentThreadId(void);

typedef struct {
    uint32_t DebugInfo;
    LONG     LockCount;
    LONG     RecursionCount;
    uint32_t OwningThread;
    uint32_t LockSemaphore;
    uint32_t SpinCount;
} CRITICAL_SECTION32;

_Static_assert(sizeof(CRITICAL_SECTION32) == 24,
               "Win32 CRITICAL_SECTION layout must be 24 bytes");

static inline LONG *critical_lock_count(LPCRITICAL_SECTION cs)
{
    return g_compat32_mode
        ? &((CRITICAL_SECTION32 *)(void *)cs)->LockCount
        : &cs->LockCount;
}

static inline LONG *critical_recursion_count(LPCRITICAL_SECTION cs)
{
    return g_compat32_mode
        ? &((CRITICAL_SECTION32 *)(void *)cs)->RecursionCount
        : &cs->RecursionCount;
}

static inline uint32_t critical_owner(LPCRITICAL_SECTION cs)
{
    return g_compat32_mode
        ? ((CRITICAL_SECTION32 *)(void *)cs)->OwningThread
        : (uint32_t)(ULONG_PTR)cs->OwningThread;
}

static inline void critical_set_owner(LPCRITICAL_SECTION cs, uint32_t owner)
{
    if (g_compat32_mode)
        ((CRITICAL_SECTION32 *)(void *)cs)->OwningThread = owner;
    else
        cs->OwningThread = (HANDLE)(ULONG_PTR)owner;
}

static inline DWORD critical_spin_count(LPCRITICAL_SECTION cs)
{
    return g_compat32_mode
        ? ((CRITICAL_SECTION32 *)(void *)cs)->SpinCount
        : (DWORD)cs->SpinCount;
}

static inline void critical_set_spin_count(LPCRITICAL_SECTION cs, DWORD spin)
{
    if (g_compat32_mode)
        ((CRITICAL_SECTION32 *)(void *)cs)->SpinCount = spin;
    else
        cs->SpinCount = spin;
}

void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION lpCS)
{
    if (!lpCS) return;
    if (g_compat32_mode) {
        CRITICAL_SECTION32 *cs = (CRITICAL_SECTION32 *)(void *)lpCS;
        cs->DebugInfo = 0;
        cs->LockCount = -1;
        cs->RecursionCount = 0;
        cs->OwningThread = 0;
        cs->LockSemaphore = 0;
        cs->SpinCount = 0;
        return;
    }
    lpCS->DebugInfo      = NULL;
    lpCS->LockCount      = -1;
    lpCS->RecursionCount = 0;
    lpCS->OwningThread   = NULL;
    lpCS->LockSemaphore  = NULL;
    lpCS->SpinCount      = 0;
}

BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCS,
                                                   DWORD dwSpinCount)
{
    if (!lpCS) return FALSE;
    InitializeCriticalSection(lpCS);
    critical_set_spin_count(lpCS, dwSpinCount);
    return TRUE;
}

DWORD WINAPI SetCriticalSectionSpinCount(LPCRITICAL_SECTION lpCS,
                                          DWORD dwSpinCount)
{
    if (!lpCS) return 0;
    DWORD previous = critical_spin_count(lpCS);
    critical_set_spin_count(lpCS, dwSpinCount);
    return previous;
}

static BOOL WINAPI InitializeCriticalSectionEx_k32(LPCRITICAL_SECTION lpCS,
                                                    DWORD dwSpinCount,
                                                    DWORD flags)
{
    (void)flags;
    return InitializeCriticalSectionAndSpinCount(lpCS, dwSpinCount);
}

void WINAPI EnterCriticalSection(LPCRITICAL_SECTION lpCS)
{
    if (!lpCS) return;
    extern void sched_yield(void);
    uint32_t me = GetCurrentThreadId();
    LONG *lock_count = critical_lock_count(lpCS);
    LONG *recursion_count = critical_recursion_count(lpCS);

    if (critical_owner(lpCS) == me) {
        (*recursion_count)++;
        (*lock_count)++;
        return;
    }

    for (;;) {
        LONG old = __sync_val_compare_and_swap(lock_count, -1, 0);
        if (old == -1) {
            critical_set_owner(lpCS, me);
            *recursion_count = 1;
            return;
        }
        DWORD spin_count = critical_spin_count(lpCS);
        for (DWORD spin = 0; spin < (spin_count ? spin_count : 100); spin++) {
            __asm__ volatile ("pause" ::: "memory");
            if (*lock_count == -1) break;
        }
        if (*lock_count == -1) continue;
        sched_yield();
    }
}

BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION lpCS)
{
    if (!lpCS) return FALSE;
    uint32_t me = GetCurrentThreadId();
    LONG *lock_count = critical_lock_count(lpCS);
    LONG *recursion_count = critical_recursion_count(lpCS);

    if (critical_owner(lpCS) == me) {
        (*recursion_count)++;
        (*lock_count)++;
        return TRUE;
    }

    LONG old = __sync_val_compare_and_swap(lock_count, -1, 0);
    if (old == -1) {
        critical_set_owner(lpCS, me);
        *recursion_count = 1;
        return TRUE;
    }
    return FALSE;
}

void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION lpCS)
{
    if (!lpCS) return;
    LONG *lock_count = critical_lock_count(lpCS);
    LONG *recursion_count = critical_recursion_count(lpCS);
    (*recursion_count)--;
    if (*recursion_count == 0) {
        critical_set_owner(lpCS, 0);
        __sync_lock_release(lock_count);
        *lock_count = -1;
    } else {
        (*lock_count)--;
    }
}

void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION lpCS)
{
    if (!lpCS) return;
    *critical_lock_count(lpCS) = -1;
    *critical_recursion_count(lpCS) = 0;
    critical_set_owner(lpCS, 0);
}
static BOOL srw_try_acquire(PVOID lock)
{
    if (g_compat32_mode)
        return __sync_val_compare_and_swap((volatile uint32_t *)lock,
                                           0U, 1U) == 0U;
    return __sync_val_compare_and_swap((volatile ULONG_PTR *)lock,
                                       0, 1) == 0;
}

static void WINAPI InitializeSRWLock_k32(PVOID lock)
{
    if (!lock) return;
    if (g_compat32_mode)
        *(volatile uint32_t *)lock = 0;
    else
        *(volatile ULONG_PTR *)lock = 0;
}

static void WINAPI AcquireSRWLockExclusive_k32(PVOID lock)
{
    if (!lock) return;
    extern void sched_yield(void);

    while (!srw_try_acquire(lock)) {
        for (int spin = 0; spin < 100; spin++)
            __asm__ volatile ("pause" ::: "memory");
        sched_yield();
    }
}

static BOOL WINAPI TryAcquireSRWLockExclusive_k32(PVOID lock)
{
    return lock && srw_try_acquire(lock);
}

static void WINAPI ReleaseSRWLockExclusive_k32(PVOID lock)
{
    if (!lock) return;
    if (g_compat32_mode)
        __sync_lock_release((volatile uint32_t *)lock);
    else
        __sync_lock_release((volatile ULONG_PTR *)lock);
}

static void WINAPI AcquireSRWLockShared_k32(PVOID lock)
{
    AcquireSRWLockExclusive_k32(lock);
}

static BOOL WINAPI TryAcquireSRWLockShared_k32(PVOID lock)
{
    return TryAcquireSRWLockExclusive_k32(lock);
}

static void WINAPI ReleaseSRWLockShared_k32(PVOID lock)
{
    ReleaseSRWLockExclusive_k32(lock);
}

static void WINAPI InitializeConditionVariable_k32(PVOID condition)
{
    if (!condition) return;
    if (g_compat32_mode)
        *(volatile uint32_t *)condition = 0;
    else
        *(volatile ULONG_PTR *)condition = 0;
}

static void WINAPI WakeAllConditionVariable_k32(PVOID condition)
{
    if (!condition) return;
    if (g_compat32_mode)
        __sync_add_and_fetch((volatile uint32_t *)condition, 1);
    else
        __sync_add_and_fetch((volatile ULONG_PTR *)condition, 1);
}

static BOOL WINAPI SleepConditionVariableSRW_k32(PVOID condition, PVOID lock,
                                                   DWORD milliseconds,
                                                   ULONG flags)
{
    /* ponytail: shared SRW waits need shared-lock support when first used. */
    if (!condition || !lock || flags != 0) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    ULONG_PTR generation = g_compat32_mode
        ? *(volatile uint32_t *)condition
        : *(volatile ULONG_PTR *)condition;
    DWORD start = GetTickCount();

    ReleaseSRWLockExclusive_k32(lock);
    while ((g_compat32_mode ? *(volatile uint32_t *)condition
                            : *(volatile ULONG_PTR *)condition) == generation) {
        if (milliseconds != 0xFFFFFFFFU &&
            (DWORD)(GetTickCount() - start) >= milliseconds) {
            AcquireSRWLockExclusive_k32(lock);
            SetLastError(1460); /* ERROR_TIMEOUT */
            return FALSE;
        }
        Sleep(milliseconds == 0 ? 0 : 1);
    }
    AcquireSRWLockExclusive_k32(lock);
    return TRUE;
}

static BOOL WINAPI SleepConditionVariableCS_k32(PVOID condition,
                                                  LPCRITICAL_SECTION cs,
                                                  DWORD milliseconds)
{
    if (!condition || !cs) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    ULONG_PTR generation = g_compat32_mode
        ? *(volatile uint32_t *)condition
        : *(volatile ULONG_PTR *)condition;
    DWORD start = GetTickCount();

    LeaveCriticalSection(cs);
    while ((g_compat32_mode ? *(volatile uint32_t *)condition
                            : *(volatile ULONG_PTR *)condition) == generation) {
        if (milliseconds != 0xFFFFFFFFU &&
            (DWORD)(GetTickCount() - start) >= milliseconds) {
            EnterCriticalSection(cs);
            SetLastError(1460); /* ERROR_TIMEOUT */
            return FALSE;
        }
        Sleep(milliseconds == 0 ? 0 : 1);
    }
    EnterCriticalSection(cs);
    return TRUE;
}
#define INIT_ONCE_PENDING  1ULL
#define INIT_ONCE_COMPLETE 2ULL
#define INIT_ONCE_FAILED   4U

static inline uint64_t init_once_load(PVOID once)
{
    return g_compat32_mode
        ? *(volatile uint32_t *)once
        : *(volatile ULONG_PTR *)once;
}

static inline BOOL init_once_cas(PVOID once, uint64_t old, uint64_t value)
{
    if (g_compat32_mode)
        return __sync_val_compare_and_swap((volatile uint32_t *)once,
                                           (uint32_t)old,
                                           (uint32_t)value) == (uint32_t)old;
    return __sync_val_compare_and_swap((volatile ULONG_PTR *)once,
                                       (ULONG_PTR)old,
                                       (ULONG_PTR)value) == (ULONG_PTR)old;
}

static inline void init_once_store_context(PVOID *out, uint64_t context)
{
    if (!out) return;
    if (g_compat32_mode)
        *(uint32_t *)(void *)out = (uint32_t)context;
    else
        *out = (PVOID)(ULONG_PTR)context;
}

static BOOL WINAPI InitOnceBeginInitialize_k32(PVOID once, DWORD flags,
                                                BOOL *pending, PVOID *context)
{
    /* ponytail: add CHECK_ONLY/ASYNC when a guest actually requests them. */
    if (!once || !pending || flags != 0) return FALSE;

    for (;;) {
        uint64_t state = init_once_load(once);
        if (state & INIT_ONCE_COMPLETE) {
            *pending = FALSE;
            init_once_store_context(context, state & ~3ULL);
            return TRUE;
        }
        if (state == 0 && init_once_cas(once, 0, INIT_ONCE_PENDING)) {
            *pending = TRUE;
            init_once_store_context(context, 0);
            return TRUE;
        }
        if (state != INIT_ONCE_PENDING) return FALSE;

        extern void sched_yield(void);
        sched_yield();
    }
}

static BOOL WINAPI InitOnceComplete_k32(PVOID once, DWORD flags, PVOID context)
{
    if (!once) return FALSE;
    if (flags == INIT_ONCE_FAILED)
        return init_once_cas(once, INIT_ONCE_PENDING, 0);
    if (flags != 0 || ((ULONG_PTR)context & 3U)) return FALSE;

    return init_once_cas(once, INIT_ONCE_PENDING,
                         (uint64_t)(ULONG_PTR)context | INIT_ONCE_COMPLETE);
}

static BOOL WINAPI InitOnceExecuteOnce_k32(PVOID once, PVOID init_fn,
                                            PVOID parameter, PVOID *context)
{
    BOOL pending = FALSE;
    PVOID callback_context = NULL;

    if (!init_fn ||
        !InitOnceBeginInitialize_k32(once, 0, &pending, &callback_context))
        return FALSE;
    if (!pending) {
        init_once_store_context(context,
                                (uint64_t)(ULONG_PTR)callback_context);
        return TRUE;
    }

    BOOL initialized;
    if (g_compat32_mode) {
        extern void *mem_alloc_pages(uint64_t count);
        extern void mem_free_pages(void *addr, uint64_t count);
        uint32_t *callback_context32 = (uint32_t *)mem_alloc_pages(1);
        if (!callback_context32 ||
            (uint64_t)(ULONG_PTR)callback_context32 > 0xFFFFFFFFULL) {
            if (callback_context32) mem_free_pages(callback_context32, 1);
            InitOnceComplete_k32(once, INIT_ONCE_FAILED, NULL);
            return FALSE;
        }

        *callback_context32 = 0;
        uint32_t args[3] = {
            (uint32_t)(ULONG_PTR)once,
            (uint32_t)(ULONG_PTR)parameter,
            (uint32_t)(ULONG_PTR)callback_context32
        };
        initialized = (BOOL)compat32_callback_args(
            (uint32_t)(ULONG_PTR)init_fn, 3, args);
        callback_context = (PVOID)(ULONG_PTR)*callback_context32;
        mem_free_pages(callback_context32, 1);
    } else {
        typedef BOOL (WINAPI *init_once_fn_t)(PVOID, PVOID, PVOID *);
        initialized = ((init_once_fn_t)init_fn)(once, parameter,
                                                &callback_context);
    }

    if (!initialized) {
        InitOnceComplete_k32(once, INIT_ONCE_FAILED, NULL);
        return FALSE;
    }
    if (!InitOnceComplete_k32(once, 0, callback_context)) return FALSE;

    init_once_store_context(context, (uint64_t)(ULONG_PTR)callback_context);
    return TRUE;
}
/* ── Thread Local Storage ──────────────────────────────────── */

#define TLS_MAX_SLOTS 1024
static uint32_t *tls_vector32;
static PVOID    *tls_vector64;
static PVOID     tls_expansion64[TLS_MAX_SLOTS - 64];

/* TLS indices are process-local on NT.  PE32 processes intentionally reuse
 * the same low virtual addresses for their PEB/TEB, so a PEB pointer is not a
 * stable cross-process identity. */
#define TLS_MAX_PROCESS_STATES 256
#define TLS_BITMAP_WORDS (TLS_MAX_SLOTS / 64)
typedef struct {
    volatile DWORD pid;
    volatile uint64_t used[TLS_BITMAP_WORDS];
} tls_process_state_t;

static tls_process_state_t tls_process_states[TLS_MAX_PROCESS_STATES];
static volatile int tls_process_states_lock;

#define MAX_STATIC_TLS_MODULES 64
typedef struct {
    DWORD    owner_pid;
    DWORD    index;
    uint32_t image_base;
    uint32_t raw_start;
    uint32_t raw_size;
    uint32_t total_size;
    uint32_t callbacks_addr;
    uint32_t callback_count;
} static_tls_module_t;

static static_tls_module_t static_tls_modules[MAX_STATIC_TLS_MODULES];
static int static_tls_module_count;
static void tls_clear_slot_process(DWORD index, DWORD process_id);
static BOOL win32_tls_seed_existing_threads(const static_tls_module_t *mod);
static void win64_tls_reset_root(void);

#define MAX_STATIC_TLS64_MODULES 128
typedef struct {
    DWORD    owner_pid;
    PPEB     owner;
    DWORD    index;
    PVOID    image_base;
    PVOID    raw_start;
    SIZE_T   raw_size;
    SIZE_T   total_size;
    PVOID    callbacks_addr;
    DWORD    callback_count;
} static_tls64_module_t;

static static_tls64_module_t static_tls64_modules[MAX_STATIC_TLS64_MODULES];
static int static_tls64_module_count;
static volatile int static_tls64_module_lock;

static void static_tls64_lock_acquire(void)
{
    while (__atomic_exchange_n(&static_tls64_module_lock, 1,
                               __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void static_tls64_lock_release(void)
{
    __atomic_store_n(&static_tls64_module_lock, 0, __ATOMIC_RELEASE);
}

static void tls_process_states_lock_acquire(void)
{
    while (__atomic_exchange_n(&tls_process_states_lock, 1,
                               __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void tls_process_states_lock_release(void)
{
    __atomic_store_n(&tls_process_states_lock, 0, __ATOMIC_RELEASE);
}

static tls_process_state_t *tls_process_state_get(DWORD pid, BOOL create)
{
    if (!pid) return NULL;

    for (int i = 0; i < TLS_MAX_PROCESS_STATES; i++) {
        if (__atomic_load_n(&tls_process_states[i].pid,
                            __ATOMIC_ACQUIRE) == pid)
            return &tls_process_states[i];
    }
    if (!create) return NULL;

    tls_process_states_lock_acquire();
    tls_process_state_t *free_state = NULL;
    for (int i = 0; i < TLS_MAX_PROCESS_STATES; i++) {
        DWORD owner = __atomic_load_n(&tls_process_states[i].pid,
                                      __ATOMIC_RELAXED);
        if (owner == pid) {
            tls_process_states_lock_release();
            return &tls_process_states[i];
        }
        if (!owner && !free_state)
            free_state = &tls_process_states[i];
    }
    if (free_state) {
        for (int i = 0; i < TLS_BITMAP_WORDS; i++)
            __atomic_store_n(&free_state->used[i], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&free_state->pid, pid, __ATOMIC_RELEASE);
    }
    tls_process_states_lock_release();
    return free_state;
}

static void tls_process_state_release(DWORD pid)
{
    if (!pid) return;
    tls_process_states_lock_acquire();
    for (int i = 0; i < TLS_MAX_PROCESS_STATES; i++) {
        tls_process_state_t *state = &tls_process_states[i];
        if (__atomic_load_n(&state->pid, __ATOMIC_RELAXED) != pid)
            continue;
        for (int j = 0; j < TLS_BITMAP_WORDS; j++)
            __atomic_store_n(&state->used[j], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&state->pid, 0, __ATOMIC_RELEASE);
        break;
    }
    tls_process_states_lock_release();
}

static BOOL tls_process_slot_used(DWORD pid, DWORD index)
{
    if (index >= TLS_MAX_SLOTS) return FALSE;
    tls_process_state_t *state = tls_process_state_get(pid, FALSE);
    if (!state) return FALSE;
    uint64_t mask = 1ULL << (index & 63);
    return (__atomic_load_n(&state->used[index >> 6],
                            __ATOMIC_ACQUIRE) & mask) != 0;
}

static DWORD tls_process_slot_alloc(DWORD pid)
{
    tls_process_state_t *state = tls_process_state_get(pid, TRUE);
    if (!state) return (DWORD)-1;

    for (DWORD index = 0; index < TLS_MAX_SLOTS; index++) {
        volatile uint64_t *word = &state->used[index >> 6];
        uint64_t mask = 1ULL << (index & 63);
        uint64_t old = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        while (!(old & mask)) {
            if (__atomic_compare_exchange_n(word, &old, old | mask, FALSE,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                return index;
        }
    }
    return (DWORD)-1;
}

static BOOL tls_process_slot_release(DWORD pid, DWORD index)
{
    if (index >= TLS_MAX_SLOTS) return FALSE;
    tls_process_state_t *state = tls_process_state_get(pid, FALSE);
    if (!state) return FALSE;
    uint64_t mask = 1ULL << (index & 63);
    uint64_t old = __atomic_fetch_and(&state->used[index >> 6], ~mask,
                                      __ATOMIC_ACQ_REL);
    return (old & mask) != 0;
}

static void tls_release_process_slots(DWORD pid)
{
    tls_process_state_t *state = tls_process_state_get(pid, FALSE);
    if (!state) return;
    for (DWORD index = 0; index < TLS_MAX_SLOTS; index++) {
        if (tls_process_slot_used(pid, index))
            tls_clear_slot_process(index, pid);
    }
    tls_process_state_release(pid);
}

static uint32_t *tls_current_vector32(void)
{
    TEB32 *teb = compat32_current_teb();
    return teb && teb->ThreadLocalStoragePointer
         ? (uint32_t *)(ULONG_PTR)teb->ThreadLocalStoragePointer
         : NULL;
}

static PVOID *tls_current_vector64(void)
{
    TEB *teb = win64_current_teb();
    return teb ? (PVOID *)teb->ThreadLocalStoragePointer : NULL;
}

static PVOID *tls_dynamic_slot64(TEB *teb, DWORD index)
{
    if (!teb || index >= TLS_MAX_SLOTS) return NULL;
    if (index < 64) return &teb->TlsSlots[index];
    if (!teb->TlsExpansionSlots) return NULL;
    return &teb->TlsExpansionSlots[index - 64];
}

static void tls_trace_slot42(const char *operation, DWORD index, PVOID value,
                             uint64_t caller)
{
    if (!K32_VERBOSE_DIAGNOSTICS || index != 42) return;
    serial_puts("[TLS42] ");
    serial_puts(operation);
    serial_puts(" mode=");
    serial_putdec((uint64_t)g_compat32_mode);
    serial_puts(" pid=");
    serial_putdec((uint64_t)win32_current_process_id());
    serial_puts(" tid=");
    serial_putdec((uint64_t)GetCurrentThreadId());
    serial_puts(" value=");
    serial_puthex((uint64_t)(ULONG_PTR)value, 16);
    serial_puts(" caller=");
    serial_puthex(caller, 16);
    serial_puts("\n");
}

static void tls_free_static_vector(uint32_t *vector, DWORD owner_pid)
{
    if (!vector || !owner_pid) return;

    for (int i = 0; i < static_tls_module_count; i++) {
        static_tls_module_t *mod = &static_tls_modules[i];
        if (mod->owner_pid != owner_pid) continue;
        if (!vector[mod->index]) continue;
        VirtualFree((void *)(ULONG_PTR)vector[mod->index], 0, MEM_RELEASE);
        vector[mod->index] = 0;
    }
}

void win32_tls_reset(void)
{
    win64_tls_reset_root();
    DWORD owner_pid = g_teb32.ClientId_UniqueProcess;
    if (!owner_pid) owner_pid = 1;
    tls_free_static_vector(tls_vector32, owner_pid);
    for (int i = 0; i < static_tls_module_count;) {
        if (static_tls_modules[i].owner_pid != owner_pid) {
            i++;
            continue;
        }
        for (int j = i + 1; j < static_tls_module_count; j++)
            static_tls_modules[j - 1] = static_tls_modules[j];
        static_tls_module_count--;
    }

    extern void *mem_alloc_pages(uint64_t count);
    if (!tls_vector32) {
        tls_vector32 = (uint32_t *)mem_alloc_pages(1);
        if ((uint64_t)(ULONG_PTR)tls_vector32 > 0xFFFFFFFFULL)
            tls_vector32 = NULL;
    }
    if (!tls_vector64)
        tls_vector64 = (PVOID *)mem_alloc_pages(2);

    for (DWORD i = 0; i < TLS_MAX_SLOTS; i++) {
        if (tls_vector32) tls_vector32[i] = 0;
        if (tls_vector64) tls_vector64[i] = NULL;
        if (i < 64) g_teb.TlsSlots[i] = NULL;
        else tls_expansion64[i - 64] = NULL;
    }
    tls_process_state_release(owner_pid);
    g_teb32.ThreadLocalStoragePointer =
        (uint32_t)(ULONG_PTR)tls_vector32;
    g_teb.ThreadLocalStoragePointer = tls_vector64;
    g_teb.TlsExpansionSlots = tls_expansion64;

    serial_puts("[TLS32] vector=0x");
    serial_puthex((uint64_t)(ULONG_PTR)tls_vector32, 8);
    serial_puts("\n");
    serial_puts("[TLS64] vector=0x");
    serial_puthex((uint64_t)(ULONG_PTR)tls_vector64, 16);
    serial_puts("\n");
}

DWORD WINAPI TlsAlloc(void)
{
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid) return (DWORD)-1;
    if (g_compat32_mode ? !tls_current_vector32() : !win64_current_teb())
        return (DWORD)-1;
    DWORD index = tls_process_slot_alloc(owner_pid);
    if (index != (DWORD)-1) {
        tls_clear_slot_process(index, owner_pid);
        tls_trace_slot42("alloc", index, NULL,
                         (uint64_t)__builtin_return_address(0));
        return index;
    }
    g_last_error = 87; /* ERROR_INVALID_PARAMETER */
    return (DWORD)-1; /* TLS_OUT_OF_INDEXES */
}

BOOL WINAPI TlsFree(DWORD dwTlsIndex)
{
    DWORD owner_pid = win32_current_process_id();
    if (dwTlsIndex >= TLS_MAX_SLOTS ||
        !tls_process_slot_used(owner_pid, dwTlsIndex))
        return FALSE;
    tls_trace_slot42("free", dwTlsIndex, NULL,
                     (uint64_t)__builtin_return_address(0));
    tls_clear_slot_process(dwTlsIndex, owner_pid);
    return tls_process_slot_release(owner_pid, dwTlsIndex);
}

PVOID WINAPI TlsGetValue(DWORD dwTlsIndex)
{
    DWORD owner_pid = win32_current_process_id();
    if (dwTlsIndex >= TLS_MAX_SLOTS ||
        !tls_process_slot_used(owner_pid, dwTlsIndex)) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        sync_last_error();
        return NULL;
    }
    g_last_error = 0;
    sync_last_error();
    if (g_compat32_mode) {
        uint32_t *vector = tls_current_vector32();
        return vector ? (PVOID)(ULONG_PTR)vector[dwTlsIndex] : NULL;
    }
    PVOID *slot = tls_dynamic_slot64(win64_current_teb(), dwTlsIndex);
    return slot ? *slot : NULL;
}

BOOL WINAPI TlsSetValue(DWORD dwTlsIndex, PVOID lpTlsValue)
{
    DWORD owner_pid = win32_current_process_id();
    if (dwTlsIndex >= TLS_MAX_SLOTS ||
        !tls_process_slot_used(owner_pid, dwTlsIndex))
        return FALSE;
    tls_trace_slot42("set", dwTlsIndex, lpTlsValue,
                     (uint64_t)__builtin_return_address(0));
    if (!g_compat32_mode) {
        PVOID *slot = tls_dynamic_slot64(win64_current_teb(), dwTlsIndex);
        if (!slot) return FALSE;
        *slot = lpTlsValue;
        return TRUE;
    }

    uint32_t *vector = tls_current_vector32();
    if (!vector) return FALSE;
    PVOID old_value = (PVOID)(ULONG_PTR)vector[dwTlsIndex];
    if (heap_is_vprof_ptr(lpTlsValue) || heap_is_vprof_ptr(old_value)) {
        extern uint32_t compat32_get_last_caller_eip(void);
        serial_puts("[VPROF-TLS] tid=");
        serial_putdec(GetCurrentThreadId());
        serial_puts(" slot=");
        serial_putdec(dwTlsIndex);
        serial_puts(" old=0x");
        serial_puthex((uint64_t)(ULONG_PTR)old_value, 8);
        serial_puts(" new=0x");
        serial_puthex((uint64_t)(ULONG_PTR)lpTlsValue, 8);
        serial_puts(" caller=0x");
        serial_puthex(compat32_get_last_caller_eip(), 8);
        serial_puts("\n");
    }
    vector[dwTlsIndex] = (uint32_t)(ULONG_PTR)lpTlsValue;
    return TRUE;
}

DWORD WINAPI FlsAlloc(PVOID lpCallback)
{
    (void)lpCallback;
    return TlsAlloc();
}

BOOL WINAPI FlsFree(DWORD dwFlsIndex)
{
    return TlsFree(dwFlsIndex);
}

PVOID WINAPI FlsGetValue(DWORD dwFlsIndex)
{
    return TlsGetValue(dwFlsIndex);
}

BOOL WINAPI FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData)
{
    return TlsSetValue(dwFlsIndex, lpFlsData);
}

int kernel32_tls_selftest(void)
{
    const DWORD pid_a = 0xFFFFFFF0U;
    const DWORD pid_b = 0xFFFFFFF1U;
    int checks = 0;
    int failures = 0;
#define TLS_CHECK(condition) do { checks++; if (!(condition)) failures++; } while (0)

    tls_process_state_release(pid_a);
    tls_process_state_release(pid_b);

    DWORD a0 = tls_process_slot_alloc(pid_a);
    DWORD b0 = tls_process_slot_alloc(pid_b);
    TLS_CHECK(a0 == 0);
    TLS_CHECK(b0 == 0);
    TLS_CHECK(tls_process_slot_used(pid_a, 0));
    TLS_CHECK(tls_process_slot_used(pid_b, 0));

    DWORD high = 0;
    for (DWORD i = 1; i <= 311; i++)
        high = tls_process_slot_alloc(pid_a);
    TLS_CHECK(high == 311);
    TLS_CHECK(tls_process_slot_used(pid_a, 311));

    tls_process_state_release(pid_a);
    TLS_CHECK(!tls_process_slot_used(pid_a, 0));
    TLS_CHECK(tls_process_slot_used(pid_b, 0));
    TLS_CHECK(tls_process_slot_alloc(pid_a) == 0);

    DWORD live = TlsAlloc();
    TLS_CHECK(live != (DWORD)-1);
    if (live != (DWORD)-1) {
        PVOID marker = (PVOID)(ULONG_PTR)0x12345000U;
        TLS_CHECK(TlsSetValue(live, marker));
        TLS_CHECK(TlsGetValue(live) == marker);
        TLS_CHECK(TlsFree(live));
        TLS_CHECK(!tls_process_slot_used(win32_current_process_id(), live));
    }

    tls_process_state_release(pid_a);
    tls_process_state_release(pid_b);
    serial_puts("[TLSTEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
#undef TLS_CHECK
    return failures;
}

/* Fiber API */

#define K32_MAX_FIBERS             256
#define K32_FIBER_FREE               0
#define K32_FIBER_INITIALIZING       1
#define K32_FIBER_READY              2
#define K32_FIBER_FLAG_FLOAT_SWITCH  1U
#define K32_FIBER_DEFAULT_STACK      (1024ULL * 1024ULL)
#define K32_FIBER_MIN_STACK          (64ULL * 1024ULL)
#define K32_FIBER_MAX_STACK          (64ULL * 1024ULL * 1024ULL)

typedef struct __attribute__((aligned(64))) {
    volatile int state;
    DWORD flags;

    /* The public pointer addresses this field so GetFiberData(), which
     * dereferences GetCurrentFiber(), sees the caller's parameter. */
    PVOID data;

    int owner_kernel_pid;
    BOOL converted_thread;
    BOOL context_valid;
    LPFIBER_START_ROUTINE start_address;
    PVOID stack_phys;
    SIZE_T stack_pages;
    PVOID stack_limit;
    PVOID stack_base;
    PVOID deallocation_stack;
    ULONG stack_guarantee;

    /* kern_setjmp layout: RBX, RBP, R12-R15, RSP, RIP, CR3. */
    uint64_t context[9];

    /* XCR0 is fixed to x87/SSE/AVX (mask 0x7) by cpu_features.c. */
    __attribute__((aligned(64))) BYTE xstate[1024];
} k32_fiber_t;

static k32_fiber_t k32_fibers[K32_MAX_FIBERS];
static volatile uint32_t k32_fiber_trace_count;

extern int kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
extern void kern_longjmp(uint64_t *buf, int value) __attribute__((noreturn));
extern int32_t proc_current_pid(void);
extern void proc_exit(int32_t code);

static PVOID k32_fiber_public(k32_fiber_t *fiber);

static void k32_fiber_trace(const char *event, k32_fiber_t *fiber,
                            k32_fiber_t *other)
{
    uint32_t trace = __atomic_fetch_add(&k32_fiber_trace_count, 1,
                                        __ATOMIC_RELAXED);
    if (trace >= 48) return;

    serial_puts("[K32-FIBER] ");
    serial_puts(event);
    serial_puts(" pid=");
    serial_putdec((uint64_t)(uint32_t)proc_current_pid());
    serial_puts(" fiber=0x");
    serial_puthex((uint64_t)(ULONG_PTR)k32_fiber_public(fiber), 16);
    if (other) {
        serial_puts(" other=0x");
        serial_puthex((uint64_t)(ULONG_PTR)k32_fiber_public(other), 16);
    }
    serial_puts("\n");
}

static PVOID k32_fiber_public(k32_fiber_t *fiber)
{
    return fiber ? (PVOID)&fiber->data : NULL;
}

static k32_fiber_t *k32_fiber_from_public(PVOID public_fiber)
{
    if (!public_fiber) return NULL;
    for (int i = 0; i < K32_MAX_FIBERS; i++) {
        k32_fiber_t *fiber = &k32_fibers[i];
        if (__atomic_load_n(&fiber->state, __ATOMIC_ACQUIRE) ==
                K32_FIBER_READY &&
            public_fiber == k32_fiber_public(fiber))
            return fiber;
    }
    return NULL;
}

static k32_fiber_t *k32_current_fiber(void)
{
    if (g_compat32_mode) return NULL;
    TEB *teb = win64_current_teb();
    return teb ? k32_fiber_from_public(teb->FiberData) : NULL;
}

static k32_fiber_t *k32_fiber_alloc_slot(void)
{
    for (int i = 0; i < K32_MAX_FIBERS; i++) {
        int expected = K32_FIBER_FREE;
        k32_fiber_t *fiber = &k32_fibers[i];
        if (!__atomic_compare_exchange_n(&fiber->state, &expected,
                                         K32_FIBER_INITIALIZING, FALSE,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            continue;

        memset(&fiber->flags, 0,
               sizeof(*fiber) - __builtin_offsetof(k32_fiber_t, flags));
        return fiber;
    }
    return NULL;
}

static void k32_fiber_publish(k32_fiber_t *fiber)
{
    __atomic_store_n(&fiber->state, K32_FIBER_READY, __ATOMIC_RELEASE);
}

static void k32_fiber_save_xstate(k32_fiber_t *fiber)
{
    __asm__ volatile ("xsave64 %0"
                      : "=m"(fiber->xstate)
                      : "a"(7U), "d"(0U)
                      : "memory");
}

static void k32_fiber_restore_xstate(k32_fiber_t *fiber)
{
    __asm__ volatile ("xrstor64 %0"
                      :
                      : "m"(fiber->xstate), "a"(7U), "d"(0U)
                      : "memory");
}

static void k32_fiber_release(k32_fiber_t *fiber)
{
    if (!fiber) return;
    if (fiber->stack_phys && fiber->stack_pages) {
        extern void mem_free_pages(void *addr, uint64_t count);
        mem_free_pages(fiber->stack_phys, fiber->stack_pages);
    }
    fiber->stack_phys = NULL;
    fiber->stack_pages = 0;
    fiber->context_valid = FALSE;
    __atomic_store_n(&fiber->state, K32_FIBER_FREE, __ATOMIC_RELEASE);
}

static void __attribute__((noreturn)) k32_fiber_entry(void)
{
    k32_fiber_t *fiber = k32_current_fiber();
    if (!fiber || !fiber->start_address) {
        serial_puts("[K32-FIBER] invalid entry context\n");
        proc_exit((int32_t)STATUS_INVALID_PARAMETER);
    }

    fiber->start_address(fiber->data);

    /* Windows exits the running thread when a fiber procedure returns. */
    proc_exit(0);
    for (;;) __asm__ volatile ("hlt");
}

PVOID WINAPI ConvertThreadToFiberEx(PVOID lpParameter, DWORD dwFlags)
{
    if (g_compat32_mode) {
        SetLastError(120); /* ERROR_CALL_NOT_IMPLEMENTED */
        return NULL;
    }
    if (dwFlags & ~K32_FIBER_FLAG_FLOAT_SWITCH) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }

    TEB *teb = win64_current_teb();
    if (!teb) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }
    if (k32_fiber_from_public(teb->FiberData)) {
        SetLastError(1280); /* ERROR_ALREADY_FIBER */
        return NULL;
    }

    if (!teb->StackBase || !teb->StackLimit) {
        extern int proc_get_kernel_stack_bounds(uint64_t *limit,
                                                uint64_t *base);
        uint64_t limit = 0;
        uint64_t base = 0;
        if (proc_get_kernel_stack_bounds(&limit, &base) == 0) {
            teb->StackLimit = (PVOID)(ULONG_PTR)limit;
            teb->StackBase = (PVOID)(ULONG_PTR)base;
            if (!teb->DeallocationStack)
                teb->DeallocationStack = (PVOID)(ULONG_PTR)(limit - 4096);
        }
    }

    k32_fiber_t *fiber = k32_fiber_alloc_slot();
    if (!fiber) {
        SetLastError(8);
        return NULL;
    }

    fiber->flags = dwFlags;
    fiber->data = lpParameter;
    fiber->owner_kernel_pid = proc_current_pid();
    fiber->converted_thread = TRUE;
    fiber->stack_limit = teb->StackLimit;
    fiber->stack_base = teb->StackBase;
    fiber->deallocation_stack = teb->DeallocationStack;
    fiber->stack_guarantee = teb->GuaranteedStackBytes;
    k32_fiber_save_xstate(fiber);
    k32_fiber_publish(fiber);

    teb->FiberData = k32_fiber_public(fiber);
    k32_fiber_trace("convert", fiber, NULL);
    SetLastError(0);
    return teb->FiberData;
}

PVOID WINAPI ConvertThreadToFiber(PVOID lpParameter)
{
    return ConvertThreadToFiberEx(lpParameter, 0);
}

BOOL WINAPI ConvertFiberToThread(void)
{
    if (g_compat32_mode) {
        SetLastError(120);
        return FALSE;
    }

    TEB *teb = win64_current_teb();
    k32_fiber_t *fiber = k32_current_fiber();
    if (!teb || !fiber || !fiber->converted_thread) {
        SetLastError(1281); /* ERROR_ALREADY_THREAD */
        return FALSE;
    }

    teb->FiberData = NULL;
    k32_fiber_release(fiber);
    SetLastError(0);
    return TRUE;
}

PVOID WINAPI CreateFiberEx(SIZE_T dwStackCommitSize,
                           SIZE_T dwStackReserveSize, DWORD dwFlags,
                           LPFIBER_START_ROUTINE lpStartAddress,
                           PVOID lpParameter)
{
    if (g_compat32_mode) {
        SetLastError(120);
        return NULL;
    }
    if (!lpStartAddress || (dwFlags & ~K32_FIBER_FLAG_FLOAT_SWITCH)) {
        SetLastError(87);
        return NULL;
    }

    SIZE_T stack_size = dwStackReserveSize;
    if (!stack_size) stack_size = K32_FIBER_DEFAULT_STACK;
    if (stack_size < dwStackCommitSize) stack_size = dwStackCommitSize;
    if (stack_size < K32_FIBER_MIN_STACK) stack_size = K32_FIBER_MIN_STACK;
    if (stack_size > K32_FIBER_MAX_STACK) {
        SetLastError(8);
        return NULL;
    }

    SIZE_T pages = (stack_size + 4095) / 4096;
    extern void *mem_alloc_pages(uint64_t count);
    PVOID stack_phys = mem_alloc_pages(pages);
    if (!stack_phys) {
        SetLastError(8);
        return NULL;
    }

    k32_fiber_t *fiber = k32_fiber_alloc_slot();
    if (!fiber) {
        extern void mem_free_pages(void *addr, uint64_t count);
        mem_free_pages(stack_phys, pages);
        SetLastError(8);
        return NULL;
    }

    BYTE *stack_limit = (BYTE *)PHYS_TO_VIRT(stack_phys);
    SIZE_T mapped_size = pages * 4096;
    memset(stack_limit, 0, mapped_size);

    fiber->flags = dwFlags;
    fiber->data = lpParameter;
    fiber->owner_kernel_pid = proc_current_pid();
    fiber->start_address = lpStartAddress;
    fiber->stack_phys = stack_phys;
    fiber->stack_pages = pages;
    fiber->stack_limit = stack_limit;
    fiber->stack_base = stack_limit + mapped_size;
    fiber->deallocation_stack = stack_limit;

    /* Enter the trampoline as if it had been called: a 16-byte-aligned
     * caller stack plus a dummy return address gives RSP % 16 == 8. */
    uint64_t stack_pointer = (uint64_t)(ULONG_PTR)fiber->stack_base;
    stack_pointer &= ~0xFULL;
    stack_pointer -= sizeof(uint64_t);
    *(uint64_t *)(ULONG_PTR)stack_pointer = 0;

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    fiber->context[6] = stack_pointer;
    fiber->context[7] = (uint64_t)(ULONG_PTR)k32_fiber_entry;
    fiber->context[8] = cr3;
    fiber->context_valid = TRUE;
    k32_fiber_save_xstate(fiber);
    k32_fiber_publish(fiber);

    k32_fiber_trace("create", fiber, NULL);
    SetLastError(0);
    return k32_fiber_public(fiber);
}

PVOID WINAPI CreateFiber(SIZE_T dwStackSize,
                         LPFIBER_START_ROUTINE lpStartAddress,
                         PVOID lpParameter)
{
    return CreateFiberEx(dwStackSize, 0, 0, lpStartAddress, lpParameter);
}

void WINAPI SwitchToFiber(PVOID lpFiber)
{
    if (g_compat32_mode) {
        SetLastError(120);
        return;
    }

    TEB *teb = win64_current_teb();
    k32_fiber_t *current = k32_current_fiber();
    k32_fiber_t *target = k32_fiber_from_public(lpFiber);
    if (!teb || !current || !target || !target->context_valid ||
        current->owner_kernel_pid != proc_current_pid() ||
        target->owner_kernel_pid != proc_current_pid()) {
        SetLastError(87);
        return;
    }
    if (target == current) return;

    int resumed = kern_setjmp(current->context);
    if (resumed != 0) return;

    current->context_valid = TRUE;
    current->stack_guarantee = teb->GuaranteedStackBytes;
    k32_fiber_save_xstate(current);
    k32_fiber_trace("switch", current, target);
    teb->FiberData = k32_fiber_public(target);
    teb->StackLimit = target->stack_limit;
    teb->StackBase = target->stack_base;
    teb->DeallocationStack = target->deallocation_stack;
    teb->GuaranteedStackBytes = target->stack_guarantee;
    k32_fiber_restore_xstate(target);
    kern_longjmp(target->context, 1);
}

void WINAPI DeleteFiber(PVOID lpFiber)
{
    if (g_compat32_mode) return;
    k32_fiber_t *fiber = k32_fiber_from_public(lpFiber);
    if (!fiber) return;

    if (fiber == k32_current_fiber()) {
        proc_exit(0);
        for (;;) __asm__ volatile ("hlt");
    }
    k32_fiber_release(fiber);
}

BOOL WINAPI IsThreadAFiber(void)
{
    return k32_current_fiber() != NULL;
}

BOOL win32_tls_register_static(DWORD index, uint32_t image_base,
                               uint32_t raw_start, uint32_t raw_size,
                               uint32_t total_size, uint32_t callbacks_addr,
                               uint32_t callback_count)
{
    DWORD owner_pid = win32_current_process_id();
    if (index >= TLS_MAX_SLOTS ||
        !owner_pid ||
        !tls_process_slot_used(owner_pid, index) ||
        static_tls_module_count >= MAX_STATIC_TLS_MODULES)
        return FALSE;

    static_tls_module_t mod = {
        .owner_pid = owner_pid,
        .index = index,
        .image_base = image_base,
        .raw_start = raw_start,
        .raw_size = raw_size,
        .total_size = total_size,
        .callbacks_addr = callbacks_addr,
        .callback_count = callback_count,
    };
    if (!win32_tls_seed_existing_threads(&mod))
        return FALSE;
    static_tls_modules[static_tls_module_count++] = mod;
    return TRUE;
}

static uint8_t *win32_tls_alloc_block(const static_tls_module_t *mod)
{
    uint64_t pages = (mod->total_size + 4095) / 4096;
    if (!pages) pages = 1;
    uint8_t *block = (uint8_t *)VirtualAlloc(
        NULL, pages * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!block || (uint64_t)(ULONG_PTR)block > UINT32_MAX) {
        if (block) VirtualFree(block, 0, MEM_RELEASE);
        return NULL;
    }
    memset(block, 0, pages * 4096);
    if (mod->raw_size)
        memcpy(block, (void *)(ULONG_PTR)mod->raw_start, mod->raw_size);
    return block;
}

static BOOL win32_tls_attach_thread(void)
{
    uint32_t *vector = tls_current_vector32();
    DWORD owner_pid = win32_current_process_id();
    extern uint32_t compat32_callback_args(uint32_t, int, const uint32_t *);
    if (!vector || !owner_pid) return FALSE;

    for (int i = 0; i < static_tls_module_count; i++) {
        static_tls_module_t *mod = &static_tls_modules[i];
        if (mod->owner_pid != owner_pid) continue;
        uint8_t *block = win32_tls_alloc_block(mod);
        if (!block) {
            tls_free_static_vector(vector, owner_pid);
            return FALSE;
        }
        vector[mod->index] = (uint32_t)(ULONG_PTR)block;
    }

    for (int i = 0; i < static_tls_module_count; i++) {
        static_tls_module_t *mod = &static_tls_modules[i];
        if (mod->owner_pid != owner_pid) continue;
        for (uint32_t j = 0; j < mod->callback_count; j++) {
            uint32_t callback = *(uint32_t *)(ULONG_PTR)
                (mod->callbacks_addr + j * sizeof(uint32_t));
            uint32_t args[3] = { mod->image_base, DLL_THREAD_ATTACH, 0 };
            compat32_callback_args(callback, 3, args);
        }
    }
    return TRUE;
}

static void win32_tls_detach_thread(void)
{
    uint32_t *vector = tls_current_vector32();
    DWORD owner_pid = win32_current_process_id();
    extern uint32_t compat32_callback_args(uint32_t, int, const uint32_t *);
    if (!vector || !owner_pid) return;

    for (int i = static_tls_module_count - 1; i >= 0; i--) {
        static_tls_module_t *mod = &static_tls_modules[i];
        if (mod->owner_pid != owner_pid) continue;
        for (uint32_t j = 0; j < mod->callback_count; j++) {
            uint32_t callback = *(uint32_t *)(ULONG_PTR)
                (mod->callbacks_addr + j * sizeof(uint32_t));
            uint32_t args[3] = { mod->image_base, DLL_THREAD_DETACH, 0 };
            compat32_callback_args(callback, 3, args);
        }
    }
    tls_free_static_vector(vector, owner_pid);
}

/* Thread API - Real preemptive Win32 threads */
/*
 * CreateThread creates REAL kernel threads via sched_spawn().
 * Each Win32 thread runs as its own preemptively-scheduled kernel process.
 *
 * Threading model:
 *   - sched_spawn() takes void(*entry)(void), no argument passing.
 *     We use a static context table indexed by slot to pass thread params.
 *   - compat32_callback_args() keeps callback state per scheduler slot, so
 *     workers may yield while another worker runs.
 *   - THREAD_OBJECT from ntprocess.c tracks NT thread state.
 *     We link it to the kernel PID so WaitForSingleObject works.
 */

static DWORD g_thread_id_counter = 1;

extern uint32_t compat32_callback_args(uint32_t func_addr, int nargs,
                                        const uint32_t *args);
extern void compat32_setup_teb(void *teb_addr);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern int   sched_spawn(const char *name, void (*entry)(void));
extern int   sched_alloc_compat_ist1(uint32_t pid);
extern void  sched_yield(void);
extern uint64_t idt_get_ticks(void);

static void k32_calendar_to_systemtime(const WINTIME_CALENDAR *calendar,
                                       SYSTEMTIME *system_time)
{
    system_time->wYear = calendar->year;
    system_time->wMonth = calendar->month;
    system_time->wDayOfWeek = calendar->day_of_week;
    system_time->wDay = calendar->day;
    system_time->wHour = calendar->hour;
    system_time->wMinute = calendar->minute;
    system_time->wSecond = calendar->second;
    system_time->wMilliseconds = calendar->millisecond;
}

static void k32_systemtime_to_calendar(const SYSTEMTIME *system_time,
                                       WINTIME_CALENDAR *calendar)
{
    calendar->year = system_time->wYear;
    calendar->month = system_time->wMonth;
    calendar->day_of_week = system_time->wDayOfWeek;
    calendar->day = system_time->wDay;
    calendar->hour = system_time->wHour;
    calendar->minute = system_time->wMinute;
    calendar->second = system_time->wSecond;
    calendar->millisecond = system_time->wMilliseconds;
    calendar->day_of_year = 0;
}

static BOOL k32_get_system_time(SYSTEMTIME *system_time)
{
    if (!system_time)
        return FALSE;
    uint64_t filetime = wintime_now_filetime();
    WINTIME_CALENDAR calendar;
    if (!filetime || wintime_filetime_to_calendar(filetime, &calendar) < 0) {
        memset(system_time, 0, sizeof(*system_time));
        return FALSE;
    }
    k32_calendar_to_systemtime(&calendar, system_time);
    return TRUE;
}

/* ── compat32 serialization lock ──────────────────────────────
 * Callback state is owned by each scheduler slot in compat32.c. */
/* ── Win32 thread context table ───────────────────────────────
 * sched_spawn only takes void(*)(void), so we pass context via a
 * static table. Each slot holds the function address, parameter, PE mode,
 * and bookkeeping for the thread handle and NT THREAD_OBJECT. */

#define WIN32_SCHEDULER_SLOT_RESERVE 32U
#define WIN32_THREAD_CAPACITY_MIN   224U
#define WIN32_THREAD_GRANULARITY     64U
#define WIN32_DEFAULT_THREAD_STACK (1024 * 1024)
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000

typedef struct {
    volatile int    active;        /* 1 = slot in use */
    ULONG_PTR       func_addr;     /* PE thread start routine */
    ULONG_PTR       param;         /* PE thread parameter */
    BOOL            compat32;      /* creator was running a PE32 image */
    DWORD           tid;           /* Win32 thread ID */
    HANDLE          handle;        /* handle into g_handle_table */
    PVOID           thread_object; /* stable identity across handle reuse */
    volatile int    terminated;    /* 1 = thread function returned */
    volatile int    reclaim_pending;
    volatile int    exit_jmp_ready; /* PE64 ExitThread may unwind safely */
    DWORD           exit_code;
    uint64_t        exit_jmpbuf[9]; /* scheduler-stack continuation */
    int             kernel_pid;    /* kernel PID from sched_spawn */
    DWORD           owner_pid;     /* immutable Win32 process identity */
    uint64_t        owner_cr3;     /* immutable scheduler address space */
    PPEB            owner_peb;     /* immutable process/TLS identity */
    int             suspended;     /* 1 = created suspended (CREATE_SUSPENDED) */
    PVOID           stack_allocation;
    PVOID           stack_limit;
    PVOID           stack_base;
    SIZE_T          stack_size;
    TEB32          *teb;           /* low per-thread FS:[0] state */
    uint32_t       *tls_vector;
    TEB             teb64;         /* per-thread GS:[0] state */
    PVOID           tls_vector64[TLS_MAX_SLOTS];
    PVOID           tls_expansion64[TLS_MAX_SLOTS - 64];
    WCHAR           description[64];
} win32_thread_ctx_t;

static win32_thread_ctx_t *g_win32_threads;
static volatile BYTE *g_win32_thread_slot_reserved;
static win32_thread_ctx_t **g_win32_threads_by_sched_slot;
static int g_win32_thread_capacity;
static int g_win32_thread_sched_capacity;
static volatile uint32_t g_win32_thread_table_state;
static spinlock_t g_win32_thread_stack_lock = SPINLOCK_INIT;
static WCHAR g_primary_thread_description[64];

static uint64_t win32_thread_stack_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&g_win32_thread_stack_lock);
    return flags;
}

static void win32_thread_stack_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&g_win32_thread_stack_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static BOOL win32_thread_table_ensure(void)
{
    uint32_t state = __atomic_load_n(&g_win32_thread_table_state,
                                     __ATOMIC_ACQUIRE);
    if (state == 2) return TRUE;
    if (state == 3) return FALSE;

    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&g_win32_thread_table_state, &expected, 1,
                                    FALSE, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        extern uint32_t sched_capacity_get(void);
        uint32_t scheduler_capacity = sched_capacity_get();
        uint32_t capacity = scheduler_capacity;
        if (capacity > WIN32_SCHEDULER_SLOT_RESERVE)
            capacity -= WIN32_SCHEDULER_SLOT_RESERVE;

        win32_thread_ctx_t *contexts = NULL;
        volatile BYTE *reserved = NULL;
        while (capacity >= WIN32_THREAD_CAPACITY_MIN) {
            contexts = (win32_thread_ctx_t *)kcalloc(
                capacity, sizeof(*contexts));
            reserved = (volatile BYTE *)kcalloc(capacity, sizeof(*reserved));
            if (contexts && reserved)
                break;
            kfree(contexts);
            kfree((void *)reserved);
            contexts = NULL;
            reserved = NULL;
            if (capacity == WIN32_THREAD_CAPACITY_MIN)
                break;
            capacity /= 2;
            capacity = (capacity / WIN32_THREAD_GRANULARITY) *
                       WIN32_THREAD_GRANULARITY;
            if (capacity < WIN32_THREAD_CAPACITY_MIN)
                capacity = WIN32_THREAD_CAPACITY_MIN;
        }
        if (!contexts || !reserved) {
            __atomic_store_n(&g_win32_thread_table_state, 3,
                             __ATOMIC_RELEASE);
            serial_puts("[K32] Failed to allocate Win32 thread table\n");
            return FALSE;
        }

        g_win32_threads = contexts;
        g_win32_thread_slot_reserved = reserved;
        g_win32_thread_capacity = (int)capacity;
        g_win32_threads_by_sched_slot = (win32_thread_ctx_t **)kcalloc(
            scheduler_capacity, sizeof(*g_win32_threads_by_sched_slot));
        g_win32_thread_sched_capacity = g_win32_threads_by_sched_slot
            ? (int)scheduler_capacity : 0;
        __atomic_store_n(&g_win32_thread_table_state, 2, __ATOMIC_RELEASE);
        serial_puts("[K32] Win32 thread capacity ");
        serial_putdec(capacity);
        serial_puts("\n");
        return TRUE;
    }

    while ((state = __atomic_load_n(&g_win32_thread_table_state,
                                     __ATOMIC_ACQUIRE)) == 1)
        __asm__ volatile ("pause" ::: "memory");
    return state == 2;
}

#define K32_EXECUTION_STATE_SLOTS 256
#define K32_ES_SYSTEM_REQUIRED    0x00000001U
#define K32_ES_DISPLAY_REQUIRED   0x00000002U
#define K32_ES_AWAYMODE_REQUIRED  0x00000040U
#define K32_ES_CONTINUOUS         0x80000000U

typedef struct {
    volatile uint64_t key;
    volatile DWORD state;
} K32_EXECUTION_STATE;

static K32_EXECUTION_STATE k32_execution_states[K32_EXECUTION_STATE_SLOTS];

static K32_EXECUTION_STATE *k32_execution_state_current(BOOL create)
{
    uint64_t key = ((uint64_t)win32_current_process_id() << 32) |
                   GetCurrentThreadId();

    for (int i = 0; i < K32_EXECUTION_STATE_SLOTS; i++) {
        if (__atomic_load_n(&k32_execution_states[i].key,
                            __ATOMIC_ACQUIRE) == key)
            return &k32_execution_states[i];
    }
    if (!create) return NULL;

    for (int i = 0; i < K32_EXECUTION_STATE_SLOTS; i++) {
        uint64_t empty = 0;
        if (!__atomic_compare_exchange_n(&k32_execution_states[i].key,
                                         &empty, key, FALSE,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            continue;
        __atomic_store_n(&k32_execution_states[i].state,
                         K32_ES_CONTINUOUS, __ATOMIC_RELEASE);
        return &k32_execution_states[i];
    }
    return NULL;
}

static void k32_execution_state_release_process(DWORD process_id)
{
    for (int i = 0; i < K32_EXECUTION_STATE_SLOTS; i++) {
        uint64_t key = __atomic_load_n(&k32_execution_states[i].key,
                                       __ATOMIC_ACQUIRE);
        if ((DWORD)(key >> 32) != process_id) continue;
        __atomic_store_n(&k32_execution_states[i].state, 0,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&k32_execution_states[i].key, 0,
                         __ATOMIC_RELEASE);
    }
}

static BOOL win32_tls_seed_existing_threads(const static_tls_module_t *mod)
{
    if (!mod || !mod->owner_pid || mod->index >= TLS_MAX_SLOTS)
        return FALSE;

    for (int i = 0; i < g_win32_thread_capacity; i++) {
        win32_thread_ctx_t *ctx = &g_win32_threads[i];
        if (!__atomic_load_n(&ctx->active, __ATOMIC_ACQUIRE) ||
            !ctx->compat32 || ctx->owner_pid != mod->owner_pid ||
            !ctx->tls_vector || ctx->tls_vector[mod->index])
            continue;
        uint8_t *block = win32_tls_alloc_block(mod);
        if (!block) {
            for (int j = 0; j < g_win32_thread_capacity; j++) {
                win32_thread_ctx_t *rollback = &g_win32_threads[j];
                if (!rollback->compat32 ||
                    rollback->owner_pid != mod->owner_pid ||
                    !rollback->tls_vector ||
                    !rollback->tls_vector[mod->index])
                    continue;
                VirtualFree((void *)(ULONG_PTR)
                                rollback->tls_vector[mod->index],
                            0, MEM_RELEASE);
                rollback->tls_vector[mod->index] = 0;
            }
            return FALSE;
        }
        ctx->tls_vector[mod->index] = (uint32_t)(ULONG_PTR)block;
    }
    return TRUE;
}

static PVOID win64_tls_alloc_block(const static_tls64_module_t *mod)
{
    extern void *mem_alloc_pages(uint64_t count);
    uint64_t pages = (mod->total_size + 4095) / 4096;
    if (!pages) pages = 1;
    void *phys = mem_alloc_pages(pages);
    BYTE *block = phys ? (BYTE *)PHYS_TO_VIRT(phys) : NULL;
    if (!block) return NULL;
    memset(block, 0, pages * 4096);
    if (mod->raw_size)
        memcpy(block, mod->raw_start, mod->raw_size);
    return block;
}

static void win64_tls_free_block(PVOID block, SIZE_T size)
{
    extern void mem_free_pages(void *addr, uint64_t count);
    uint64_t pages = (size + 4095) / 4096;
    if (!pages) pages = 1;
    if (block)
        mem_free_pages((void *)VIRT_TO_PHYS(block), pages);
}

static void win64_tls_free_vector_locked(DWORD owner_pid, PVOID *vector)
{
    if (!vector) return;
    for (int i = 0; i < static_tls64_module_count; i++) {
        static_tls64_module_t *mod = &static_tls64_modules[i];
        if (mod->owner_pid != owner_pid || !vector[mod->index]) continue;
        win64_tls_free_block(vector[mod->index], mod->total_size);
        vector[mod->index] = NULL;
    }
}

static void win64_tls_free_vector(DWORD owner_pid, PVOID *vector)
{
    static_tls64_lock_acquire();
    win64_tls_free_vector_locked(owner_pid, vector);
    static_tls64_lock_release();
}

static void win64_tls_reset_root(void)
{
    DWORD owner_pid = (DWORD)(ULONG_PTR)g_teb.ClientId.UniqueProcess;
    if (!owner_pid) owner_pid = 1;

    static_tls64_lock_acquire();
    for (int i = 0; i < static_tls64_module_count;) {
        static_tls64_module_t mod = static_tls64_modules[i];
        if (mod.owner_pid != owner_pid) {
            i++;
            continue;
        }
        if (tls_vector64 && tls_vector64[mod.index]) {
            win64_tls_free_block(tls_vector64[mod.index], mod.total_size);
            tls_vector64[mod.index] = NULL;
        }
        for (int j = 0; j < g_win32_thread_capacity; j++) {
            win32_thread_ctx_t *ctx = &g_win32_threads[j];
            if (!ctx->compat32 && ctx->owner_pid == owner_pid &&
                ctx->tls_vector64[mod.index]) {
                win64_tls_free_block(ctx->tls_vector64[mod.index],
                                     mod.total_size);
                ctx->tls_vector64[mod.index] = NULL;
            }
        }
        for (int j = i + 1; j < static_tls64_module_count; j++)
            static_tls64_modules[j - 1] = static_tls64_modules[j];
        static_tls64_module_count--;
    }
    static_tls64_lock_release();
}

static BOOL win64_tls_attach_thread(win32_thread_ctx_t *ctx)
{
    DWORD owner_pid = ctx->owner_pid;
    PPEB owner = ctx->owner_peb;
    static_tls64_module_t attached[MAX_STATIC_TLS64_MODULES];
    int attached_count = 0;

    static_tls64_lock_acquire();
    for (int i = 0; i < static_tls64_module_count; i++) {
        static_tls64_module_t *mod = &static_tls64_modules[i];
        if (mod->owner_pid != owner_pid || ctx->tls_vector64[mod->index])
            continue;
        ctx->tls_vector64[mod->index] = win64_tls_alloc_block(mod);
        if (!ctx->tls_vector64[mod->index]) {
            win64_tls_free_vector_locked(owner_pid, ctx->tls_vector64);
            static_tls64_lock_release();
            return FALSE;
        }
    }

    for (int i = 0; i < static_tls64_module_count; i++) {
        static_tls64_module_t *mod = &static_tls64_modules[i];
        if (mod->owner_pid == owner_pid)
            attached[attached_count++] = *mod;
    }
    static_tls64_lock_release();

    typedef void (WINAPI *tls_callback_fn)(PVOID, DWORD, PVOID);
    for (int i = 0; i < attached_count; i++) {
        static_tls64_module_t *mod = &attached[i];
        if ((uint64_t)(ULONG_PTR)mod->image_base == 0x180000000ULL) {
            serial_puts("[TLS64-CEF-ATTACH] owner=");
            serial_putdec(owner_pid);
            serial_puts(" tid=");
            serial_putdec(ctx->tid);
            serial_puts(" kpid=");
            serial_putdec((uint64_t)(uint32_t)ctx->kernel_pid);
            serial_puts(" peb=0x");
            serial_puthex((uint64_t)(ULONG_PTR)owner, 16);
            serial_puts(" slot=");
            serial_putdec(mod->index);
            serial_puts(" block=0x");
            serial_puthex((uint64_t)(ULONG_PTR)
                          ctx->tls_vector64[mod->index], 16);
            serial_puts("\n");
        }
        uint64_t *callbacks = (uint64_t *)mod->callbacks_addr;
        for (DWORD j = 0; j < mod->callback_count; j++)
            ((tls_callback_fn)(ULONG_PTR)callbacks[j])(
                mod->image_base, DLL_THREAD_ATTACH, NULL);
    }
    return TRUE;
}

static void win64_tls_detach_thread(win32_thread_ctx_t *ctx, BOOL callbacks)
{
    DWORD owner_pid = ctx->owner_pid;
    static_tls64_module_t attached[MAX_STATIC_TLS64_MODULES];
    int attached_count = 0;

    static_tls64_lock_acquire();
    for (int i = 0; i < static_tls64_module_count; i++) {
        if (static_tls64_modules[i].owner_pid == owner_pid)
            attached[attached_count++] = static_tls64_modules[i];
    }
    static_tls64_lock_release();

    if (callbacks) {
        typedef void (WINAPI *tls_callback_fn)(PVOID, DWORD, PVOID);
        for (int i = attached_count - 1; i >= 0; i--) {
            static_tls64_module_t mod = attached[i];
            uint64_t *entries = (uint64_t *)mod.callbacks_addr;
            for (DWORD j = 0; j < mod.callback_count; j++)
                ((tls_callback_fn)(ULONG_PTR)entries[j])(
                    mod.image_base, DLL_THREAD_DETACH, NULL);
        }
    }
    win64_tls_free_vector(owner_pid, ctx->tls_vector64);
}

DWORD win64_tls_alloc_static_index(void)
{
    TEB *teb = win64_current_teb();
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid && teb)
        owner_pid = (DWORD)(ULONG_PTR)teb->ClientId.UniqueProcess;
    if (!owner_pid || !teb || !teb->ProcessEnvironmentBlock ||
        !teb->ThreadLocalStoragePointer)
        return (DWORD)-1;

    static_tls64_lock_acquire();
    for (DWORD index = 0; index < TLS_MAX_SLOTS; index++) {
        BOOL used = FALSE;
        for (int i = 0; i < static_tls64_module_count; i++) {
            if (static_tls64_modules[i].owner_pid == owner_pid &&
                static_tls64_modules[i].index == index) {
                used = TRUE;
                break;
            }
        }
        if (!used) {
            static_tls64_lock_release();
            return index;
        }
    }
    static_tls64_lock_release();
    return (DWORD)-1;
}

BOOL win64_tls_register_static(DWORD index, PVOID image_base, PVOID raw_start,
                               SIZE_T raw_size, SIZE_T total_size,
                               PVOID callbacks_addr, DWORD callback_count)
{
    TEB *teb = win64_current_teb();
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid && teb)
        owner_pid = (DWORD)(ULONG_PTR)teb->ClientId.UniqueProcess;
    if (!owner_pid || !teb || !teb->ProcessEnvironmentBlock ||
        index >= TLS_MAX_SLOTS || !image_base)
        return FALSE;

    static_tls64_module_t mod = {
        .owner_pid = owner_pid,
        .owner = teb->ProcessEnvironmentBlock,
        .index = index,
        .image_base = image_base,
        .raw_start = raw_start,
        .raw_size = raw_size,
        .total_size = total_size,
        .callbacks_addr = callbacks_addr,
        .callback_count = callback_count,
    };

    static_tls64_lock_acquire();
    if (static_tls64_module_count >= MAX_STATIC_TLS64_MODULES) {
        static_tls64_lock_release();
        return FALSE;
    }
    for (int i = 0; i < static_tls64_module_count; i++) {
        static_tls64_module_t *existing = &static_tls64_modules[i];
        if (existing->owner_pid == mod.owner_pid &&
            (existing->index == index ||
             existing->image_base == image_base)) {
            static_tls64_lock_release();
            return FALSE;
        }
    }

    /* Publish the descriptor before walking existing threads. A thread that
     * starts during registration now either sees this entry in its attach
     * pass or is seeded by the loop below; there is no missing-slot window. */
    static_tls64_modules[static_tls64_module_count++] = mod;

    for (int i = 0; i < g_win32_thread_capacity; i++) {
        win32_thread_ctx_t *ctx = &g_win32_threads[i];
        if (!__atomic_load_n(&ctx->active, __ATOMIC_ACQUIRE) ||
            ctx->compat32 || ctx->owner_pid != mod.owner_pid ||
            ctx->tls_vector64[index])
            continue;
        ctx->tls_vector64[index] = win64_tls_alloc_block(&mod);
        if (!ctx->tls_vector64[index]) {
            for (int j = 0; j < g_win32_thread_capacity; j++) {
                win32_thread_ctx_t *rollback = &g_win32_threads[j];
                if (!rollback->compat32 &&
                    rollback->owner_pid == mod.owner_pid &&
                    rollback->tls_vector64[index]) {
                    win64_tls_free_block(rollback->tls_vector64[index],
                                         total_size);
                    rollback->tls_vector64[index] = NULL;
                }
            }
            static_tls64_module_count--;
            static_tls64_lock_release();
            return FALSE;
        }
    }
    static_tls64_lock_release();

    if ((uint64_t)(ULONG_PTR)image_base == 0x180000000ULL) {
        serial_puts("[TLS64-CEF-REGISTER] owner=");
        serial_putdec(owner_pid);
        serial_puts(" peb=0x");
        serial_puthex((uint64_t)(ULONG_PTR)mod.owner, 16);
        serial_puts(" slot=");
        serial_putdec(index);
        serial_puts("\n");
    }
    return TRUE;
}

void win64_tls_unregister_image(PVOID image_base)
{
    TEB *teb = win64_current_teb();
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid && teb)
        owner_pid = (DWORD)(ULONG_PTR)teb->ClientId.UniqueProcess;
    if (!owner_pid) return;

    BOOL removed = FALSE;
    static_tls64_lock_acquire();
    for (int i = 0; i < static_tls64_module_count;) {
        static_tls64_module_t mod = static_tls64_modules[i];
        if (mod.owner_pid != owner_pid || mod.image_base != image_base) {
            i++;
            continue;
        }
        PVOID *current = teb ? (PVOID *)teb->ThreadLocalStoragePointer : NULL;
        if (current && current[mod.index]) {
            win64_tls_free_block(current[mod.index], mod.total_size);
            current[mod.index] = NULL;
        }
        for (int j = 0; j < g_win32_thread_capacity; j++) {
            win32_thread_ctx_t *ctx = &g_win32_threads[j];
            if (ctx->owner_pid == owner_pid &&
                ctx->tls_vector64[mod.index] && ctx->tls_vector64 != current) {
                win64_tls_free_block(ctx->tls_vector64[mod.index],
                                     mod.total_size);
                ctx->tls_vector64[mod.index] = NULL;
            }
        }
        tls_trace_slot42("unregister", mod.index, NULL,
                         (uint64_t)__builtin_return_address(0));
        for (int j = i + 1; j < static_tls64_module_count; j++)
            static_tls64_modules[j - 1] = static_tls64_modules[j];
        static_tls64_module_count--;
        removed = TRUE;
    }
    static_tls64_lock_release();

    if (removed && (uint64_t)(ULONG_PTR)image_base == 0x180000000ULL) {
        serial_puts("[TLS64-CEF-UNREGISTER] owner=");
        serial_putdec(owner_pid);
        serial_puts("\n");
    }
}

void win64_tls_release_process(TEB *teb)
{
    if (!teb) return;
    DWORD owner_pid = (DWORD)(ULONG_PTR)teb->ClientId.UniqueProcess;
    if (!owner_pid) owner_pid = win32_current_process_id();
    if (!owner_pid) return;
    PVOID *vector = (PVOID *)teb->ThreadLocalStoragePointer;

    static_tls64_lock_acquire();
    for (int i = 0; i < static_tls64_module_count;) {
        static_tls64_module_t mod = static_tls64_modules[i];
        if (mod.owner_pid != owner_pid) {
            i++;
            continue;
        }
        if (vector && vector[mod.index]) {
            win64_tls_free_block(vector[mod.index], mod.total_size);
            vector[mod.index] = NULL;
        }
        for (int j = 0; j < g_win32_thread_capacity; j++) {
            win32_thread_ctx_t *ctx = &g_win32_threads[j];
            if (ctx->owner_pid == owner_pid &&
                ctx->tls_vector64[mod.index] && ctx->tls_vector64 != vector) {
                win64_tls_free_block(ctx->tls_vector64[mod.index],
                                     mod.total_size);
                ctx->tls_vector64[mod.index] = NULL;
            }
        }
        tls_trace_slot42("release", mod.index, NULL,
                         (uint64_t)__builtin_return_address(0));
        if ((uint64_t)(ULONG_PTR)mod.image_base == 0x180000000ULL) {
            serial_puts("[TLS64-CEF-RELEASE] owner=");
            serial_putdec(owner_pid);
            serial_puts("\n");
        }
        for (int j = i + 1; j < static_tls64_module_count; j++)
            static_tls64_modules[j - 1] = static_tls64_modules[j];
        static_tls64_module_count--;
    }
    static_tls64_lock_release();

    tls_release_process_slots(owner_pid);
}

void win32_tls_release_process32(TEB32 *teb)
{
    if (!teb || !teb->ProcessEnvironmentBlock) return;

    DWORD owner_pid = teb->ClientId_UniqueProcess;
    if (!owner_pid) owner_pid = win32_current_process_id();
    uint32_t *vector = teb->ThreadLocalStoragePointer
        ? (uint32_t *)(ULONG_PTR)teb->ThreadLocalStoragePointer : NULL;

    for (int i = 0; i < static_tls_module_count;) {
        static_tls_module_t mod = static_tls_modules[i];
        if (mod.owner_pid != owner_pid) {
            i++;
            continue;
        }

        if (vector && vector[mod.index]) {
            VirtualFree((void *)(ULONG_PTR)vector[mod.index], 0,
                        MEM_RELEASE);
            vector[mod.index] = 0;
        }
        for (int j = 0; j < g_win32_thread_capacity; j++) {
            win32_thread_ctx_t *ctx = &g_win32_threads[j];
            if (!ctx->compat32 || ctx->owner_pid != owner_pid || !ctx->teb ||
                !ctx->tls_vector || !ctx->tls_vector[mod.index] ||
                ctx->tls_vector == vector)
                continue;
            VirtualFree((void *)(ULONG_PTR)ctx->tls_vector[mod.index], 0,
                        MEM_RELEASE);
            ctx->tls_vector[mod.index] = 0;
        }

        for (int j = i + 1; j < static_tls_module_count; j++)
            static_tls_modules[j - 1] = static_tls_modules[j];
        static_tls_module_count--;
    }

    tls_release_process_slots(owner_pid);

    for (int i = 0; i < g_win32_thread_capacity; i++) {
        win32_thread_ctx_t *ctx = &g_win32_threads[i];
        if (!ctx->compat32 || ctx->owner_pid != owner_pid || !ctx->teb)
            continue;
        if (ctx->tls_vector)
            VirtualFree(ctx->tls_vector, 0, MEM_RELEASE);
        ctx->teb->ThreadLocalStoragePointer = 0;
        ctx->tls_vector = NULL;
        VirtualFree(ctx->teb, 0, MEM_RELEASE);
        ctx->teb = NULL;
    }
}

static void tls_clear_slot_process(DWORD index, DWORD process_id)
{
    if (!process_id || index >= TLS_MAX_SLOTS) return;

    if (win32_current_process_id() == process_id) {
        if (g_compat32_mode) {
            uint32_t *current32 = tls_current_vector32();
            if (current32) current32[index] = 0;
        } else {
            PVOID *slot = tls_dynamic_slot64(win64_current_teb(), index);
            if (slot) *slot = NULL;
        }
    }
    if (process_id == 1 && tls_vector32)
        tls_vector32[index] = 0;
    if (process_id == 1) {
        PVOID *slot = tls_dynamic_slot64(&g_teb, index);
        if (slot) *slot = NULL;
    }

    for (int i = 0; i < g_win32_thread_capacity; i++) {
        win32_thread_ctx_t *ctx = &g_win32_threads[i];
        if (!ctx->active) continue;
        if (ctx->owner_pid != process_id) continue;
        if (ctx->compat32 && ctx->teb && ctx->tls_vector)
            ctx->tls_vector[index] = 0;
        else if (!ctx->compat32) {
            PVOID *slot = tls_dynamic_slot64(&ctx->teb64, index);
            if (slot) *slot = NULL;
        }
    }
}

/* Look up context by kernel PID (called from thread entry) */
static win32_thread_ctx_t *find_ctx_by_pid(int pid)
{
    int current_slot = -1;
    if (g_win32_threads_by_sched_slot && pid == proc_current_pid()) {
        extern int sched_current_get(void);
        current_slot = sched_current_get();
        if (current_slot >= 0 &&
            current_slot < g_win32_thread_sched_capacity) {
            win32_thread_ctx_t *ctx = __atomic_load_n(
                &g_win32_threads_by_sched_slot[current_slot],
                __ATOMIC_ACQUIRE);
            if (ctx &&
                __atomic_load_n(&ctx->active, __ATOMIC_ACQUIRE) &&
                __atomic_load_n(&ctx->kernel_pid,
                                __ATOMIC_ACQUIRE) == pid)
                return ctx;
        }
    }

    for (int i = 0; i < g_win32_thread_capacity; i++) {
        if (__atomic_load_n(&g_win32_threads[i].active, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&g_win32_threads[i].kernel_pid,
                            __ATOMIC_ACQUIRE) == pid) {
            if (current_slot >= 0 &&
                current_slot < g_win32_thread_sched_capacity) {
                __atomic_store_n(
                    &g_win32_threads_by_sched_slot[current_slot],
                    &g_win32_threads[i], __ATOMIC_RELEASE);
            }
            return &g_win32_threads[i];
        }
    }
    return NULL;
}

DWORD win32_current_thread_process_id(void)
{
    win32_thread_ctx_t *ctx = find_ctx_by_pid(proc_current_pid());
    return ctx ? ctx->owner_pid : 0;
}

PVOID win32_current_thread_process_peb(void)
{
    win32_thread_ctx_t *ctx = find_ctx_by_pid(proc_current_pid());
    return ctx ? (PVOID)ctx->owner_peb : NULL;
}

PVOID win32_current_thread_object(void)
{
    extern int32_t proc_current_pid(void);
    extern PVOID win32_current_primary_thread_object(void);

    win32_thread_ctx_t *ctx = find_ctx_by_pid(proc_current_pid());
    if (ctx && ctx->thread_object)
        return ctx->thread_object;
    return win32_current_primary_thread_object();
}

/* Look up context by Win32 HANDLE */
static win32_thread_ctx_t *find_ctx_by_handle(HANDLE h)
{
    HANDLE_ENTRY *target = handle_get_entry(&g_handle_table, h);

    for (int i = 0; i < g_win32_thread_capacity; i++) {
        win32_thread_ctx_t *ctx = &g_win32_threads[i];
        if (!ctx->active)
            continue;
        if (target && target->type == OBJ_TYPE_THREAD &&
            ctx->thread_object == target->object)
            return ctx;
        if (!target && !ctx->thread_object && ctx->handle == h)
            return ctx;
    }
    return NULL;
}

static win32_thread_ctx_t *find_ctx_by_tid(DWORD tid)
{
    for (int i = 0; i < g_win32_thread_capacity; i++) {
        if (g_win32_threads[i].active && g_win32_threads[i].tid == tid)
            return &g_win32_threads[i];
    }
    return NULL;
}

/* Allocate a free slot */
static win32_thread_ctx_t *alloc_thread_ctx(void)
{
    if (!win32_thread_table_ensure())
        return NULL;

    for (int i = 0; i < g_win32_thread_capacity; i++) {
        win32_thread_ctx_t *ctx = &g_win32_threads[i];
        if (__atomic_load_n(&ctx->active, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&ctx->reclaim_pending, __ATOMIC_ACQUIRE))
            continue;

        BYTE expected = 0;
        if (!__atomic_compare_exchange_n(&g_win32_thread_slot_reserved[i],
                                         &expected, 1, FALSE,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            continue;

        if (__atomic_load_n(&ctx->active, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&ctx->reclaim_pending, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_win32_thread_slot_reserved[i], 0,
                             __ATOMIC_RELEASE);
            continue;
        }

        ctx->kernel_pid = 0;
        ctx->func_addr = 0;
        ctx->param = 0;
        ctx->compat32 = FALSE;
        ctx->tid = 0;
        ctx->handle = NULL;
        ctx->terminated = 0;
        ctx->reclaim_pending = 0;
        ctx->exit_jmp_ready = 0;
        ctx->exit_code = 0;
        memset(ctx->exit_jmpbuf, 0, sizeof(ctx->exit_jmpbuf));
        ctx->thread_object = NULL;
        ctx->owner_pid = 0;
        ctx->owner_peb = NULL;
        ctx->suspended = 0;
        ctx->stack_allocation = NULL;
        ctx->stack_limit = NULL;
        ctx->stack_base = NULL;
        ctx->stack_size = 0;
        ctx->owner_cr3 = 0;
        ctx->teb = NULL;
        ctx->tls_vector = NULL;
        ctx->description[0] = 0;
        __atomic_store_n(&ctx->active, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_win32_thread_slot_reserved[i], 0,
                         __ATOMIC_RELEASE);
        return ctx;
    }
    return NULL;
}

/* ── Thread entry points ──────────────────────────────────────
 * sched_spawn wants void(*)(void). We create per-slot entry functions
 * that find their context via the kernel PID of the running process. */

extern int32_t proc_current_pid(void);   /* from process.c */

static void win32_thread_release_stack(win32_thread_ctx_t *ctx)
{
    if (!ctx) return;

    uint64_t irq_flags = win32_thread_stack_lock_irqsave();
    if (!ctx->stack_allocation) {
        ctx->reclaim_pending = 0;
        win32_thread_stack_unlock_irqrestore(irq_flags);
        return;
    }

    ULONG owner_pid = ctx->owner_pid;
    PVOID allocation = ctx->stack_allocation;
    NTSTATUS status = nt_vm_free_stack_for_process(owner_pid, allocation);
    ctx->stack_allocation = NULL;
    ctx->stack_limit = NULL;
    ctx->stack_base = NULL;
    ctx->stack_size = 0;
    ctx->reclaim_pending = 0;
    win32_thread_stack_unlock_irqrestore(irq_flags);

    if (!NT_SUCCESS(status)) {
        serial_puts("[K32-THREAD] stack release failed status=0x");
        serial_puthex((uint32_t)status, 8);
        serial_puts(" owner=");
        serial_putdec(owner_pid);
        serial_puts(" allocation=0x");
        serial_puthex((ULONG_PTR)allocation, 16);
        serial_puts("\n");
    }
}

void kernel32_prepare_process_vm_release(DWORD process_id)
{
    if (!process_id ||
        __atomic_load_n(&g_win32_thread_table_state, __ATOMIC_ACQUIRE) != 2)
        return;

    uint32_t transferred = 0;
    uint32_t active = 0;
    uint64_t irq_flags = win32_thread_stack_lock_irqsave();
    for (int i = 0; i < g_win32_thread_capacity; i++) {
        win32_thread_ctx_t *ctx = &g_win32_threads[i];
        if (ctx->owner_pid != process_id)
            continue;
        if (__atomic_load_n(&ctx->active, __ATOMIC_ACQUIRE))
            active++;
        if (ctx->stack_allocation) {
            ctx->stack_allocation = NULL;
            ctx->stack_limit = NULL;
            ctx->stack_base = NULL;
            ctx->stack_size = 0;
            transferred++;
        }
        ctx->reclaim_pending = 0;
        ctx->teb = NULL;
        ctx->tls_vector = NULL;
    }
    win32_thread_stack_unlock_irqrestore(irq_flags);

    if (transferred) {
        serial_puts("[K32-THREAD] process VM owns pending stacks owner=");
        serial_putdec(process_id);
        serial_puts(" count=");
        serial_putdec(transferred);
        serial_puts("\n");
    }
    if (active) {
        serial_puts("[K32-THREAD] WARNING: process VM release with active threads owner=");
        serial_putdec(process_id);
        serial_puts(" count=");
        serial_putdec(active);
        serial_puts("\n");
    }
}

static void win32_thread_release_tls32_environment(win32_thread_ctx_t *ctx)
{
    if (!ctx) return;

    if (ctx->teb)
        ctx->teb->ThreadLocalStoragePointer = 0;
    if (ctx->tls_vector &&
        !VirtualFree(ctx->tls_vector, 0, MEM_RELEASE)) {
        serial_puts("[K32-THREAD] TLS32 vector release failed owner=");
        serial_putdec(ctx->owner_pid);
        serial_puts("\n");
    }
    ctx->tls_vector = NULL;
    if (ctx->teb && !VirtualFree(ctx->teb, 0, MEM_RELEASE)) {
        serial_puts("[K32-THREAD] TEB32 release failed owner=");
        serial_putdec(ctx->owner_pid);
        serial_puts("\n");
    }
    ctx->teb = NULL;
}

static void __attribute__((noinline))
win64_thread_dispatch(win32_thread_ctx_t *ctx)
{
    if (win64_tls_attach_thread(ctx)) {
        dll_notify_thread(DLL_THREAD_ATTACH);
        LPTHREAD_START_ROUTINE start =
            (LPTHREAD_START_ROUTINE)(ULONG_PTR)ctx->func_addr;
        ctx->exit_code = start((PVOID)ctx->param);
        dll_notify_thread(DLL_THREAD_DETACH);
        win64_tls_detach_thread(ctx, TRUE);
    } else {
        serial_puts("[K32-THREAD] PE64 TLS attach failed\n");
        ctx->exit_code = STATUS_NO_MEMORY;
    }
}

static void win64_thread_dispatch_on_stack(win32_thread_ctx_t *ctx)
{
#ifndef TEST_HARNESS
    PVOID stack_top = (BYTE *)ctx->stack_base - 64;
    void (*dispatch)(win32_thread_ctx_t *) = win64_thread_dispatch;
    __asm__ volatile (
        "mov %%rsp, %%r15\n"
        "mov %[top], %%rsp\n"
        "and $-16, %%rsp\n"
        "mov %[context], %%rdi\n"
        "call *%[dispatch]\n"
        "mov %%r15, %%rsp\n"
        :
        : [top] "r"(stack_top), [context] "r"(ctx),
          [dispatch] "r"(dispatch)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10",
          "r11", "r15", "cc", "memory"
    );
#else
    win64_thread_dispatch(ctx);
#endif
}

static void win32_thread_entry_common(void)
{
    int my_pid = proc_current_pid();
    win32_thread_ctx_t *ctx = find_ctx_by_pid(my_pid);

    if (!ctx) {
        serial_puts("[K32-THREAD] ERROR: no context for PID ");
        serial_putdec((uint64_t)my_pid);
        serial_puts("\n");
        return;
    }

    g_compat32_mode = ctx->compat32;

    /* If created suspended, spin until resumed */
    while (ctx->suspended) {
        sched_yield();
    }

#ifndef OK_QUIET
    serial_puts("[K32-THREAD] Running thread TID=");
    serial_putdec(ctx->tid);
    serial_puts(" func=0x");
    serial_puthex(ctx->func_addr, ctx->compat32 ? 8 : 16);
    serial_puts(" param=0x");
    serial_puthex(ctx->param, ctx->compat32 ? 8 : 16);
    serial_puts("\n");
#endif

    int exit_jump = 0;
    if (!ctx->compat32)
        exit_jump = kern_setjmp(ctx->exit_jmpbuf);

    /* kern_longjmp restores the scheduler stack saved above. Reacquire the
     * context instead of relying on optimized locals across returns_twice. */
    ctx = find_ctx_by_pid(my_pid);
    if (!ctx) {
        serial_puts("[K32-THREAD] ERROR: context lost during dispatch\n");
        return;
    }

    if (!ctx->compat32 && exit_jump != 0) {
        __atomic_store_n(&ctx->exit_jmp_ready, 0, __ATOMIC_RELEASE);
#ifndef OK_QUIET
        serial_puts("[K32-THREAD] ExitThread unwound TID=");
        serial_putdec(ctx->tid);
        serial_puts("\n");
#endif
    } else if (ctx->compat32) {
        compat32_setup_teb(ctx->teb);
        if (win32_tls_attach_thread()) {
            dll_notify_thread(DLL_THREAD_ATTACH);
            uint32_t arg = (uint32_t)ctx->param;
            ctx->exit_code = compat32_thread_entry_on_stack(
                (uint32_t)ctx->func_addr, 1, &arg,
                (uint32_t)(ULONG_PTR)((BYTE *)ctx->stack_base - 64));
            dll_notify_thread(DLL_THREAD_DETACH);
            win32_tls_detach_thread();
        } else {
            serial_puts("[K32-THREAD] static TLS attach failed\n");
        }
        win32_thread_release_tls32_environment(ctx);
    } else {
        __atomic_store_n(&ctx->exit_jmp_ready, 1, __ATOMIC_RELEASE);
        win64_thread_dispatch_on_stack(ctx);
        __atomic_store_n(&ctx->exit_jmp_ready, 0, __ATOMIC_RELEASE);
    }

#ifndef OK_QUIET
    serial_puts("[K32-THREAD] Thread TID=");
    serial_putdec(ctx->tid);
    serial_puts(" returned\n");
#endif

    /* We are back on the scheduler's kernel stack, so the user stack can now
     * be unmapped without invalidating the live return frame. */
    win32_thread_release_stack(ctx);

    /* A signaled thread must no longer depend on its user mappings. Otherwise
     * a waiter can tear down the process address space while this cleanup is
     * still using the thread stack. */
    iocp_release_thread(ctx->tid);
    ctx->terminated = 1;
    if (ctx->thread_object) {
        extern void win32_mark_thread_terminated(PVOID object,
                                                  NTSTATUS exit_status);
        win32_mark_thread_terminated(ctx->thread_object,
                                     (NTSTATUS)ctx->exit_code);
    }

    /* The NT object keeps the observable exit state; the execution context
     * can be reused once the callback has returned. */
    ctx->active = 0;

    /* Thread function returned — the sched_spawn wrapper will mark this
     * kernel process as ZOMBIE when we return, which is correct. */
}

HANDLE WINAPI CreateThread(PVOID lpThreadAttributes, SIZE_T dwStackSize,
                           LPTHREAD_START_ROUTINE lpStartAddress,
                           PVOID lpParameter, DWORD dwCreationFlags,
                           DWORD *lpThreadId)
{
    (void)lpThreadAttributes;

    DWORD tid = __atomic_add_fetch(&g_thread_id_counter, 1,
                                   __ATOMIC_RELAXED);
    if (lpThreadId) *lpThreadId = tid;

    BOOL compat32 = g_compat32_mode;
#ifndef OK_QUIET
    serial_puts("[K32] CreateThread: func=0x");
    serial_puthex((ULONG_PTR)lpStartAddress, compat32 ? 8 : 16);
    serial_puts(" param=0x");
    serial_puthex((ULONG_PTR)lpParameter, compat32 ? 8 : 16);
    serial_puts(" tid=");
    serial_putdec(tid);
#endif

    /* Allocate thread context slot */
    win32_thread_ctx_t *ctx = alloc_thread_ctx();
    if (!ctx) {
#ifdef OK_QUIET
        serial_puts("[K32] CreateThread FAILED (no free slot)\n");
#else
        serial_puts(" FAILED (no free slot)\n");
#endif
        g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        sync_last_error();
        return NULL;
    }

    ctx->func_addr = (ULONG_PTR)lpStartAddress;
    ctx->param     = (ULONG_PTR)lpParameter;
    ctx->compat32  = compat32;
    ctx->tid       = tid;
    ctx->owner_pid = win32_current_process_id();
    TEB *parent_teb = win64_current_teb();
    ctx->owner_peb = parent_teb ? parent_teb->ProcessEnvironmentBlock : NULL;

    SIZE_T stack_reserve = WIN32_DEFAULT_THREAD_STACK;
    if (dwStackSize &&
        ((dwCreationFlags & STACK_SIZE_PARAM_IS_A_RESERVATION) ||
         dwStackSize > stack_reserve))
        stack_reserve = dwStackSize;
    NTSTATUS status = nt_vm_allocate_stack(
        stack_reserve, &ctx->stack_allocation, &ctx->stack_limit,
        &ctx->stack_base);
    if (!NT_SUCCESS(status) ||
        (compat32 && (uint64_t)(ULONG_PTR)ctx->stack_base > UINT32_MAX)) {
        if (ctx->stack_allocation)
            (void)nt_vm_free_stack(ctx->stack_allocation);
        ctx->stack_allocation = NULL;
        win32_thread_release_stack(ctx);
        __atomic_store_n(&ctx->active, 0, __ATOMIC_RELEASE);
        g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        sync_last_error();
        return NULL;
    }
    ctx->stack_size = (uint64_t)(ULONG_PTR)ctx->stack_base -
                      (uint64_t)(ULONG_PTR)ctx->stack_allocation;

    if (compat32) {
        TEB32 *parent_teb32 = compat32_current_teb();
        if (!parent_teb32) {
            win32_thread_release_stack(ctx);
            __atomic_store_n(&ctx->active, 0, __ATOMIC_RELEASE);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        ctx->teb = (TEB32 *)VirtualAlloc(
            NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!ctx->teb || (uint64_t)(ULONG_PTR)ctx->teb > UINT32_MAX) {
            win32_thread_release_tls32_environment(ctx);
            win32_thread_release_stack(ctx);
            __atomic_store_n(&ctx->active, 0, __ATOMIC_RELEASE);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        *ctx->teb = *parent_teb32;
        ctx->teb->ExceptionList = 0xFFFFFFFF;
        ctx->teb->Self = (uint32_t)(ULONG_PTR)ctx->teb;
        ctx->teb->ClientId_UniqueThread = tid;
        ctx->teb->LastErrorValue = 0;
        ctx->teb->StackBase = (uint32_t)(ULONG_PTR)ctx->stack_base;
        ctx->teb->StackLimit = (uint32_t)(ULONG_PTR)ctx->stack_limit;
        ctx->tls_vector = (uint32_t *)VirtualAlloc(
            NULL, TLS_MAX_SLOTS * sizeof(uint32_t),
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!ctx->tls_vector ||
            (uint64_t)(ULONG_PTR)ctx->tls_vector > UINT32_MAX) {
            win32_thread_release_tls32_environment(ctx);
            win32_thread_release_stack(ctx);
            __atomic_store_n(&ctx->active, 0, __ATOMIC_RELEASE);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        memset(ctx->tls_vector, 0,
               TLS_MAX_SLOTS * sizeof(uint32_t));
        ctx->teb->ThreadLocalStoragePointer =
            (uint32_t)(ULONG_PTR)ctx->tls_vector;
    } else {
        if (!parent_teb) {
            win32_thread_release_stack(ctx);
            __atomic_store_n(&ctx->active, 0, __ATOMIC_RELEASE);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        ctx->teb64 = *parent_teb;
        ctx->teb64.StackBase = ctx->stack_base;
        ctx->teb64.StackLimit = ctx->stack_limit;
        ctx->teb64.DeallocationStack = ctx->stack_allocation;
        ctx->teb64.Self = &ctx->teb64;
        ctx->teb64.ClientId.UniqueThread = (HANDLE)(ULONG_PTR)tid;
        ctx->teb64.LastErrorValue = 0;
        ctx->teb64.LastStatusValue = STATUS_SUCCESS;
        memset(ctx->teb64.TlsSlots, 0, sizeof(ctx->teb64.TlsSlots));
        memset(ctx->tls_expansion64, 0, sizeof(ctx->tls_expansion64));
        ctx->teb64.TlsExpansionSlots = ctx->tls_expansion64;
        memset(ctx->tls_vector64, 0, sizeof(ctx->tls_vector64));
        ctx->teb64.ThreadLocalStoragePointer = ctx->tls_vector64;
    }

    if (dwCreationFlags & 0x4 /* CREATE_SUSPENDED */)
        ctx->suspended = 1;

    /* Create an NT THREAD_OBJECT and handle for WaitForSingleObject */
    extern NTSTATUS sys_NtCreateThread(ULONG_PTR *args);

    /* We build a minimal NtCreateThread call to get a handle.
     * The THREAD_OBJECT tracks state for wait operations. */
    HANDLE thread_handle = NULL;
    CLIENT_ID client_id = {0};
    INITIAL_TEB initial_teb = {
        .StackBase = ctx->stack_base,
        .StackLimit = ctx->stack_limit,
        .StackCommit = ctx->stack_limit,
        .StackCommitMax = ctx->stack_limit,
        .StackReserved = ctx->stack_allocation,
    };
    ULONG_PTR nt_args[8] = {
        (ULONG_PTR)&thread_handle,           /* ThreadHandle */
        (ULONG_PTR)GENERIC_ALL,              /* DesiredAccess */
        (ULONG_PTR)NULL,                     /* ObjectAttributes */
        (ULONG_PTR)NT_CURRENT_PROCESS,       /* ProcessHandle */
        (ULONG_PTR)&client_id,               /* ClientId */
        (ULONG_PTR)NULL,                     /* ThreadContext */
        (ULONG_PTR)&initial_teb,             /* InitialTeb */
        (ULONG_PTR)(ctx->suspended != 0)     /* CreateSuspended */
    };

    status = sys_NtCreateThread(nt_args);
    if (!NT_SUCCESS(status)) {
#ifdef OK_QUIET
        serial_puts("[K32] CreateThread: NT object allocation failed; using TID handle\n");
#else
        serial_puts(" (NT thread alloc failed, continuing with TID handle)\n");
#endif
        /* Fall back to using the TID as a pseudo-handle */
        thread_handle = (HANDLE)(ULONG_PTR)tid;
    }

    ctx->handle = thread_handle;
    HANDLE_ENTRY *thread_entry = handle_get_entry(&g_handle_table, thread_handle);
    ctx->thread_object = thread_entry && thread_entry->type == OBJ_TYPE_THREAD
                       ? thread_entry->object : NULL;

    extern uint64_t proc_current_cr3(void);
    extern int sched_spawn_in_address_space_blocked(
        const char *, void (*)(void), uint64_t, int);
    extern int proc_wake_pid(int pid);
    uint64_t owner_cr3 = proc_current_cr3();
    ctx->owner_cr3 = owner_cr3;
    int kpid = sched_spawn_in_address_space_blocked(
        "win32_thread", win32_thread_entry_common, owner_cr3, FALSE);
    if (kpid < 0) {
#ifdef OK_QUIET
        serial_puts("[K32] CreateThread FAILED (sched_spawn)\n");
#else
        serial_puts(" FAILED (sched_spawn)\n");
#endif
        win32_thread_release_tls32_environment(ctx);
        win32_thread_release_stack(ctx);
        __atomic_store_n(&ctx->active, 0, __ATOMIC_RELEASE);
        g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        sync_last_error();
        return NULL;
    }

    __atomic_store_n(&ctx->kernel_pid, kpid, __ATOMIC_RELEASE);
    {
        extern uint64_t proc_get_gs_base(void);
        extern void proc_set_gs_base_pid(int pid, uint64_t addr);
        uint64_t gs_base = compat32 ? proc_get_gs_base()
                                    : (uint64_t)(ULONG_PTR)&ctx->teb64;
        proc_set_gs_base_pid(kpid, gs_base);
    }
    if (sched_alloc_compat_ist1((uint32_t)kpid) < 0) {
        extern int proc_kill_pid(int pid);
        proc_kill_pid(kpid);
#ifdef OK_QUIET
        serial_puts("[K32] CreateThread FAILED (IST1 allocation)\n");
#else
        serial_puts(" FAILED (IST1 allocation)\n");
#endif
        win32_thread_release_tls32_environment(ctx);
        win32_thread_release_stack(ctx);
        __atomic_store_n(&ctx->active, 0, __ATOMIC_RELEASE);
        g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        sync_last_error();
        return NULL;
    }
    if (!ctx->suspended && proc_wake_pid(kpid) < 0) {
        extern int proc_kill_pid(int pid);
        proc_kill_pid(kpid);
        win32_thread_release_tls32_environment(ctx);
        win32_thread_release_stack(ctx);
        __atomic_store_n(&ctx->active, 0, __ATOMIC_RELEASE);
        g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        sync_last_error();
        serial_puts("[K32] CreateThread FAILED (sched wake)\n");
        return NULL;
    }

#if defined(K32_TRACE_THREAD_CREATE) && K32_TRACE_THREAD_CREATE
    serial_puts("[K32-THREAD-CREATE] kpid=");
    serial_putdec((uint64_t)kpid);
    serial_puts(" tid=");
    serial_putdec(tid);
    serial_puts(" func=0x");
    serial_puthex((ULONG_PTR)lpStartAddress, compat32 ? 8 : 16);
    serial_puts(" param=0x");
    serial_puthex((ULONG_PTR)lpParameter, compat32 ? 8 : 16);
    serial_puts(" owner=");
    serial_putdec(ctx->owner_pid);
    serial_puts(ctx->suspended ? " suspended\n" : "\n");
#endif

#ifndef OK_QUIET
    if (ctx->suspended)
        serial_puts(" (SUSPENDED)\n");
    else
        serial_puts(" (SCHEDULED)\n");

    serial_puts("[K32] Thread kernel PID=");
    serial_putdec((uint64_t)kpid);
    serial_puts(" handle=0x");
    serial_puthex((uint64_t)(ULONG_PTR)thread_handle, 8);
    serial_puts("\n");
#endif

    return thread_handle;
}

static HANDLE WINAPI CreateRemoteThread_k32(
    HANDLE process, PVOID thread_attributes, SIZE_T stack_size,
    LPTHREAD_START_ROUTINE start_address, PVOID parameter,
    DWORD creation_flags, DWORD *thread_id)
{
    DWORD process_id;

    if (process == NT_CURRENT_PROCESS) {
        return CreateThread(thread_attributes, stack_size, start_address,
                            parameter, creation_flags, thread_id);
    }

    extern BOOL nt_process_id(HANDLE handle, DWORD *process_id);
    if (!nt_process_id(process, &process_id)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return NULL;
    }

    if (process_id == win32_current_process_id()) {
        return CreateThread(thread_attributes, stack_size, start_address,
                            parameter, creation_flags, thread_id);
    }

    /* Remote address spaces do not yet have an attach path for a new
     * scheduler thread. Resolve the import and fail normally so optional
     * crash-dump injection remains best-effort instead of jumping through a
     * null IAT entry. */
    serial_puts("[K32] CreateRemoteThread deferred target_pid=");
    serial_putdec(process_id);
    serial_puts("\n");
    SetLastError(120); /* ERROR_CALL_NOT_IMPLEMENTED */
    return NULL;
}

static HANDLE WINAPI CreateRemoteThreadEx_k32(
    HANDLE process, PVOID thread_attributes, SIZE_T stack_size,
    LPTHREAD_START_ROUTINE start_address, PVOID parameter,
    DWORD creation_flags, PVOID attribute_list, DWORD *thread_id)
{
    (void)attribute_list;
    return CreateRemoteThread_k32(process, thread_attributes, stack_size,
                                  start_address, parameter, creation_flags,
                                  thread_id);
}

DWORD WINAPI GetCurrentThreadId(void)
{
    win32_thread_ctx_t *ctx = find_ctx_by_pid(proc_current_pid());
    if (ctx)
        return ctx->tid;
    return win32_current_process_thread_id();
}

static void WINAPI GetCurrentThreadStackLimits_k32(ULONG_PTR *low_limit,
                                                    ULONG_PTR *high_limit)
{
    if (!low_limit || !high_limit)
        return;

    if (g_compat32_mode) {
        TEB32 *teb = compat32_current_teb();
        *(uint32_t *)low_limit = teb ? teb->StackLimit : 0;
        *(uint32_t *)high_limit = teb ? teb->StackBase : 0;
        return;
    }

    TEB *teb = win64_current_teb();
    *low_limit = teb ? (ULONG_PTR)teb->StackLimit : 0;
    *high_limit = teb ? (ULONG_PTR)teb->StackBase : 0;
}

static BOOL WINAPI SetThreadStackGuarantee_k32(ULONG *stack_size)
{
    if (!stack_size) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    if (g_compat32_mode) {
        /* The compact PE32 TEB does not expose GuaranteedStackBytes. The
         * current 32-bit runtime has no grow-on-demand stack, so report the
         * fixed guarantee without claiming that the reserve was changed. */
        *stack_size = 0;
        SetLastError(0);
        return TRUE;
    }

    TEB *teb = win64_current_teb();
    if (!teb || !teb->StackBase || !teb->StackLimit) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    ULONG requested = *stack_size;
    ULONG previous = teb->GuaranteedStackBytes;
    *stack_size = previous;
    if (!requested || requested <= previous) {
        SetLastError(0);
        return TRUE;
    }

    ULONG_PTR allocation = teb->DeallocationStack
                         ? (ULONG_PTR)teb->DeallocationStack
                         : (ULONG_PTR)teb->StackLimit;
    ULONG_PTR reserve = (ULONG_PTR)teb->StackBase - allocation;
    ULONG_PTR rounded = ((ULONG_PTR)requested + 4095U) & ~(ULONG_PTR)4095U;
    if (allocation >= (ULONG_PTR)teb->StackBase || rounded > reserve) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    teb->GuaranteedStackBytes = (ULONG)rounded;
    SetLastError(0);
    return TRUE;
}

/* Snapshot of an orphaned win32 thread's identifying info, returned
 * to winexec_run for logging + reaping when the PE has exited but
 * the thread context table still references kernel processes that
 * would otherwise re-enter compat32 and re-mask the APIC LVT. */
typedef struct {
    int      kernel_pid;   /* sched_spawn'd kernel process to kill */
    uint32_t tid;          /* Win32 thread ID (for the log line) */
    ULONG_PTR func_addr;   /* PE entry point this thread was running */
} win32_orphan_info_t;

/* Collect (and immediately mark inactive) all non-terminated win32
 * threads. Caller is responsible for reaping the kernel PIDs via
 * proc_kill_pid. Marking them inactive frees the slot for the next
 * PE invocation. */
int win32_collect_orphan_threads(win32_orphan_info_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < g_win32_thread_capacity && n < max; i++) {
        win32_thread_ctx_t *t = &g_win32_threads[i];
        if (t->active && !t->terminated) {
            out[n].kernel_pid = t->kernel_pid;
            out[n].tid        = t->tid;
            out[n].func_addr  = t->func_addr;
            t->terminated = 1;
            iocp_release_thread(t->tid);
            t->reclaim_pending = t->stack_allocation != NULL;
            t->active     = 0;
            n++;
        }
    }
    return n;
}

/* Back-compat thin shim. Prefer win32_collect_orphan_threads which
 * also returns the TID + entry point for an informative reap log. */
int win32_get_thread_pids(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < g_win32_thread_capacity && n < max; i++) {
        if (g_win32_threads[i].active && !g_win32_threads[i].terminated) {
            out[n++] = g_win32_threads[i].kernel_pid;
            g_win32_threads[i].terminated = 1;
            iocp_release_thread(g_win32_threads[i].tid);
            g_win32_threads[i].reclaim_pending =
                g_win32_threads[i].stack_allocation != NULL;
            g_win32_threads[i].active = 0;
        }
    }
    return n;
}

HANDLE WINAPI GetCurrentThread(void) { return NT_CURRENT_THREAD; }

static BOOL WINAPI SwitchToThread_k32(void)
{
    sched_yield();
    return TRUE;
}

static HANDLE WINAPI OpenThread_k32(DWORD dwDesiredAccess,
                                    BOOL bInheritHandle, DWORD dwThreadId)
{
    win32_thread_ctx_t *ctx = find_ctx_by_tid(dwThreadId);
    HANDLE handle = NULL;

    if (!ctx) {
        if (dwThreadId == win32_current_process_thread_id()) {
            extern PVOID nt_process_primary_thread_object(ULONG tid);
            PVOID object = nt_process_primary_thread_object(dwThreadId);
            NTSTATUS status = object
                ? handle_alloc(&g_handle_table, OBJ_TYPE_THREAD,
                               dwDesiredAccess, object, &handle)
                : STATUS_INVALID_PARAMETER;
            if (NT_SUCCESS(status)) return handle;
            set_last_error_from_status(status);
            return NULL;
        }
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }

    if (ctx->thread_object) {
        NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_THREAD,
                                       dwDesiredAccess, ctx->thread_object,
                                       &handle);
        if (!NT_SUCCESS(status)) {
            set_last_error_from_status(status);
            return NULL;
        }
        return handle;
    }

    if (!DuplicateHandle(GetCurrentProcess(), ctx->handle,
                         GetCurrentProcess(), &handle, dwDesiredAccess,
                         bInheritHandle, 0))
        return NULL;
    return handle;
}

static BOOL WINAPI GetExitCodeThread_k32(HANDLE hThread, DWORD *lpExitCode)
{
    if (!lpExitCode) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (hThread == NT_CURRENT_THREAD) {
        *lpExitCode = 259; /* STILL_ACTIVE */
        return TRUE;
    }

    win32_thread_ctx_t *ctx = find_ctx_by_handle(hThread);
    if (ctx) {
        *lpExitCode = ctx->terminated ? ctx->exit_code : 259;
        return TRUE;
    }

    HANDLE_ENTRY *entry = handle_get_entry(&g_handle_table, hThread);
    extern BOOL nt_process_thread_exit_code(PVOID object, DWORD *exit_code);
    if (entry && entry->type == OBJ_TYPE_THREAD &&
        nt_process_thread_exit_code(entry->object, lpExitCode))
        return TRUE;

    SetLastError(6); /* ERROR_INVALID_HANDLE */
    return FALSE;
}

static BOOL WINAPI GetThreadContext_k32(HANDLE hThread, PVOID context)
{
    if (!context) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    (void)hThread;
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return FALSE;
}

static BOOL WINAPI GetThreadPriorityBoost_k32(HANDLE hThread,
                                               BOOL *disable_boost)
{
    (void)hThread;
    if (!disable_boost) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    *disable_boost = FALSE;
    return TRUE;
}

static BOOL WINAPI SetThreadPriorityBoost_k32(HANDLE hThread,
                                               BOOL disable_boost)
{
    (void)hThread;
    (void)disable_boost;
    return TRUE;
}

static DWORD WINAPI SetThreadAffinityMask_k32(HANDLE hThread, DWORD mask)
{
    if (!mask || (hThread != NT_CURRENT_THREAD && !find_ctx_by_handle(hThread))) {
        SetLastError(mask ? 6 : 87);
        return 0;
    }
    /* ponytail: affinity is advisory until the scheduler supports pinning. */
    return 1;
}

DWORD WINAPI SuspendThread(HANDLE hThread)
{
    win32_thread_ctx_t *ctx = find_ctx_by_handle(hThread);
    if (ctx) {
        ctx->suspended = 1;
        return 0; /* previous suspend count */
    }
    return (DWORD)-1;
}

BOOL win32_resume_thread_execution(PVOID thread_object)
{
    if (!thread_object) return FALSE;

    for (int i = 0; i < g_win32_thread_capacity; i++) {
        win32_thread_ctx_t *ctx = &g_win32_threads[i];
        if (!__atomic_load_n(&ctx->active, __ATOMIC_ACQUIRE) ||
            ctx->thread_object != thread_object)
            continue;

        __atomic_store_n(&ctx->suspended, 0, __ATOMIC_RELEASE);
        int kernel_pid = __atomic_load_n(&ctx->kernel_pid, __ATOMIC_ACQUIRE);
        if (kernel_pid <= 0)
            return FALSE;

        extern int proc_wake_pid(int pid);
        return proc_wake_pid(kernel_pid) == 0;
    }

    extern BOOL win32_resume_primary_thread_execution(PVOID thread_object);
    return win32_resume_primary_thread_execution(thread_object);
}

DWORD WINAPI ResumeThread(HANDLE hThread)
{
    extern NTSTATUS sys_NtResumeThread(ULONG_PTR *args);
    ULONG previous_count = 0;
    ULONG_PTR args[2] = {
        (ULONG_PTR)hThread, (ULONG_PTR)&previous_count
    };
    NTSTATUS status = sys_NtResumeThread(args);
    if (!NT_SUCCESS(status)) {
        /* Preserve the pseudo-handle fallback used when NT thread object
         * allocation fails after a scheduler context was already reserved. */
        win32_thread_ctx_t *ctx = find_ctx_by_handle(hThread);
        if (!ctx) {
            set_last_error_from_status(status);
            return (DWORD)-1;
        }
        previous_count = ctx->suspended ? 1 : 0;
        if (previous_count) {
            ctx->suspended = 0;
            extern int proc_wake_pid(int pid);
            if (proc_wake_pid(ctx->kernel_pid) < 0) {
                SetLastError(6); /* ERROR_INVALID_HANDLE */
                return (DWORD)-1;
            }
        }
    }

#ifndef OK_QUIET
    if (previous_count) {
        serial_puts("[K32] ResumeThread TID=");
        serial_putdec((uint64_t)(ULONG_PTR)hThread);
        serial_puts("\n");
    }
#endif
    return previous_count;
}

static void mark_thread_ctx_terminated(win32_thread_ctx_t *ctx,
                                       DWORD exit_code, BOOL release_tls)
{
    ctx->exit_code = exit_code;
    ctx->terminated = 1;
    if (ctx->thread_object) {
        extern void win32_mark_thread_terminated(PVOID object,
                                                  NTSTATUS exit_status);
        win32_mark_thread_terminated(ctx->thread_object,
                                     (NTSTATUS)exit_code);
    }
    if (release_tls && !ctx->compat32)
        win64_tls_detach_thread(ctx, FALSE);
    iocp_release_thread(ctx->tid);
    ctx->reclaim_pending = ctx->stack_allocation != NULL;
    ctx->active = 0;
}

/* process.c calls this before releasing a scheduler task. Normal Win32 thread
 * exits have already cleared active; a live match means the task died through
 * an exception and its Win32 handle/TLS state still needs retirement. */
void win32_kernel_thread_reaped(uint32_t kernel_pid, int exit_code)
{
    for (int i = 0; i < g_win32_thread_capacity; i++) {
        win32_thread_ctx_t *ctx = &g_win32_threads[i];
        if (ctx->kernel_pid != (int)kernel_pid ||
            (!ctx->active && !ctx->reclaim_pending))
            continue;
        if (ctx->active)
            mark_thread_ctx_terminated(ctx, (DWORD)exit_code, TRUE);
        win32_thread_release_stack(ctx);
        return;
    }
}

static BOOL terminate_thread_ctx(win32_thread_ctx_t *ctx, DWORD exit_code)
{
    extern int proc_kill_pid(int pid);
    extern void *proc_find_ptr(uint16_t pid);
    if (!ctx)
        return FALSE;

    /* A fault may already have zombified and reaped the scheduler task before
     * its owning PE process performs thread cleanup. In that case the Win32
     * context is still ours to retire; only reject a failed kill while the
     * kernel PID remains live (notably an attempt to kill the caller itself). */
    if (proc_kill_pid(ctx->kernel_pid) != 0) {
        if (ctx->kernel_pid > 0 &&
            proc_find_ptr((uint16_t)ctx->kernel_pid) != NULL)
            return FALSE;
        serial_puts("[WIN32-THREAD-STALE] retiring missing kernel_pid=");
        serial_putdec((uint64_t)ctx->kernel_pid);
        serial_puts(" tid=");
        serial_putdec(ctx->tid);
        serial_puts("\n");
    }

    /* Kernel stack/IST reclamation is deferred until the scheduler no longer
     * owns the task frame. Retire the observable Win32 thread state now. */
    mark_thread_ctx_terminated(ctx, exit_code, TRUE);
    return TRUE;
}

int win32_terminate_process_threads(PPEB owner, DWORD exit_code)
{
    if (!owner) return 0;

    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) :: "memory");
    int count = 0;
    for (int i = 0; i < g_win32_thread_capacity; i++) {
        win32_thread_ctx_t *ctx = &g_win32_threads[i];
        if (ctx->active && ctx->owner_peb == owner) {
            serial_puts("[WIN32-THREAD-STOP] kernel_pid=");
            serial_putdec((uint64_t)ctx->kernel_pid);
            serial_puts(" tid=");
            serial_putdec(ctx->tid);
            serial_puts(" entry=0x");
            serial_puthex(ctx->func_addr, 16);
            serial_puts(" owner=0x");
            serial_puthex((uint64_t)(ULONG_PTR)owner, 16);
            serial_puts("\n");
            if (terminate_thread_ctx(ctx, exit_code))
                count++;
        }
    }
    if (irq_flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
    return count;
}

BOOL WINAPI TerminateThread(HANDLE hThread, DWORD dwExitCode)
{
    win32_thread_ctx_t *ctx = find_ctx_by_handle(hThread);
    if (!ctx) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    DWORD tid = ctx->tid;

    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) :: "memory");
    BOOL killed = terminate_thread_ctx(ctx, dwExitCode);
    if (irq_flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");

    if (!killed) {
        SetLastError(5); /* ERROR_ACCESS_DENIED */
        return FALSE;
    }

    serial_puts("[K32] TerminateThread TID=");
    serial_putdec(tid);
    serial_puts("\n");
    return TRUE;
}

#define WAIT_OBJECT_0   0x00000000
#define WAIT_TIMEOUT    0x00000102
#define WAIT_FAILED     0xFFFFFFFF
#define INFINITE        0xFFFFFFFF
#define K32_MAXIMUM_WAIT_OBJECTS 64

enum {
    K32_SYNC_CREATE = 1,
    K32_SYNC_WAIT = 2,
    K32_SYNC_SET = 3,
    K32_SYNC_RESET = 4,
};

typedef struct {
    uint64_t seq;
    uint64_t tick;
    uint64_t caller;
    ULONG_PTR handle;
    uint32_t op;
    uint32_t winpid;
    int32_t kernel_pid;
    uint32_t arg;
    uint32_t result;
} K32_SYNC_TRACE_RECORD;

#define K32_SYNC_TRACE_CAPACITY 8192
static volatile K32_SYNC_TRACE_RECORD g_k32_sync_trace[K32_SYNC_TRACE_CAPACITY];
static volatile uint32_t g_k32_sync_trace_next;
static volatile uint32_t g_k32_sync_trace_winpid = 3;
static volatile uint32_t g_k32_wait_if0_trace_count;

__attribute__((noinline))
static void k32_wait_trace_if0(const char *phase, HANDLE handle,
                               DWORD milliseconds, uint64_t caller)
{
    extern uint64_t sched_current_frame_seq(void);
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) :: "memory");
    if (flags & (1ULL << 9))
        return;

    uint32_t index = __atomic_fetch_add(&g_k32_wait_if0_trace_count, 1,
                                        __ATOMIC_RELAXED);
    if (index >= 64)
        return;

    serial_puts("[K32-WAIT-IF0] phase=");
    serial_puts(phase);
    serial_puts(" proc=");
    serial_putdec(win32_current_process_id());
    serial_puts(" kpid=");
    serial_putdec((uint64_t)(uint32_t)proc_current_pid());
    serial_puts(" handle=0x");
    serial_puthex((ULONG_PTR)handle, 16);
    serial_puts(" timeout=");
    serial_putdec(milliseconds);
    serial_puts(" caller=0x");
    serial_puthex(caller, 16);
    serial_puts(" frame_seq=");
    serial_putdec(sched_current_frame_seq());
    serial_puts(" flags=0x");
    serial_puthex(flags, 16);
    serial_puts("\n");
}

static void k32_sync_trace(uint32_t op, HANDLE handle, uint32_t arg,
                           uint32_t result, uint64_t caller)
{
    DWORD winpid = win32_current_process_id();
    if (winpid != g_k32_sync_trace_winpid)
        return;

    uint32_t index = __atomic_fetch_add(&g_k32_sync_trace_next, 1,
                                        __ATOMIC_RELAXED);
    if (index >= K32_SYNC_TRACE_CAPACITY)
        return;

    volatile K32_SYNC_TRACE_RECORD *record = &g_k32_sync_trace[index];
    record->seq = index;
    record->tick = idt_get_ticks();
    record->caller = caller;
    record->handle = (ULONG_PTR)handle;
    record->op = op;
    record->winpid = winpid;
    record->kernel_pid = proc_current_pid();
    record->arg = arg;
    record->result = result;
}

static void log_null_sync_handle(const char *api)
{
    extern uint32_t compat32_get_last_caller_eip(void);
    extern uint32_t compat32_get_last_user_ebp(void);
    extern uint32_t compat32_get_last_user_esi(void);
    extern void compat32_dump_recent_calls(void);
    uint32_t ebp = compat32_get_last_user_ebp();
    uint32_t outer = 0;
    if (ebp >= 0x10000 && ebp < 0x7FFF0000)
        outer = *(volatile uint32_t *)(uintptr_t)(ebp + 4);
    serial_puts("[SYNC-NULL] "); serial_puts(api);
    serial_puts(" caller=0x"); serial_puthex(compat32_get_last_caller_eip(), 8);
    serial_puts(" outer=0x"); serial_puthex(outer, 8);
    serial_puts(" this=0x"); serial_puthex(compat32_get_last_user_esi(), 8);
    serial_puts(" ebp=0x"); serial_puthex(ebp, 8);
    serial_puts("\n");
    compat32_dump_recent_calls();
}

DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    uint64_t caller =
        (uint64_t)(ULONG_PTR)__builtin_return_address(0);
    k32_wait_trace_if0("entry", hHandle, dwMilliseconds, caller);
    k32_sync_trace(K32_SYNC_WAIT, hHandle, dwMilliseconds, 0,
                   caller);
    k32_wait_trace_if0("post-sync-trace", hHandle, dwMilliseconds, caller);

    if (!hHandle)
        log_null_sync_handle("WaitForSingleObject");

    /* Check if this handle is a Win32 thread — fast path using our table */
    win32_thread_ctx_t *tctx = find_ctx_by_handle(hHandle);
    k32_wait_trace_if0("post-thread-lookup", hHandle, dwMilliseconds, caller);
    if (tctx && !tctx->thread_object) {
        if (tctx->terminated)
            return WAIT_OBJECT_0;
        if (dwMilliseconds == 0)
            return WAIT_TIMEOUT;

        /* Wait for this Win32 thread to terminate */
        uint64_t start = idt_get_ticks();
        uint64_t timeout_ticks = 0;
        if (dwMilliseconds != INFINITE)
            timeout_ticks = ((uint64_t)dwMilliseconds + 9) / 10; /* 100Hz ticks */

        while (!tctx->terminated) {
            sched_yield();  /* give the thread CPU time */
            if (dwMilliseconds != INFINITE &&
                (idt_get_ticks() - start) >= timeout_ticks) {
                return WAIT_TIMEOUT;
            }
        }
        return WAIT_OBJECT_0;
    }

    /* Fall through to NT wait for events, mutexes, semaphores, etc. */
    LARGE_INTEGER timeout;
    PLARGE_INTEGER p_timeout = NULL;
    DWORD effective_timeout = dwMilliseconds;

    /* SteamChrome can need several seconds to publish a fresh IPC endpoint. */
    if (dwMilliseconds > 0 && dwMilliseconds <= 1000) {
        const char *name = steamipc_name_for_handle(hHandle);
        if (name &&
            k32_path_contains_ci(name, "steamchrome_masterstream_") &&
            k32_path_contains_ci(name, "_written"))
            effective_timeout = 10000;
    }
    k32_wait_trace_if0("post-name-lookup", hHandle, effective_timeout, caller);

    if (effective_timeout != INFINITE) {
        /* Convert milliseconds to 100ns units, negative = relative */
        timeout.QuadPart = -(LONGLONG)effective_timeout * 10000;
        p_timeout = &timeout;
    }

    steamipc_trace_handle("Wait1.begin", hHandle, effective_timeout,
                          0, caller);
    k32_wait_trace_if0("post-steamipc-trace", hHandle, effective_timeout,
                       caller);
    NTSTATUS status = NtWaitForSingleObject(hHandle, FALSE, p_timeout);

    DWORD result;
    if (status == STATUS_TIMEOUT) {
        result = WAIT_TIMEOUT;
    } else if (NT_SUCCESS(status)) {
        result = WAIT_OBJECT_0;
    } else {
        set_last_error_from_status(status);
        result = WAIT_FAILED;
    }

    steamipc_trace_handle("Wait1.end", hHandle, effective_timeout,
                          result, caller);
    return result;
}

static DWORD WINAPI WaitForSingleObjectEx_k32(HANDLE hHandle,
                                               DWORD dwMilliseconds,
                                               BOOL bAlertable)
{
    if (bAlertable && k32_dispatch_io_completions())
        return K32_WAIT_IO_COMPLETION;
    DWORD result = WaitForSingleObject(hHandle, dwMilliseconds);
    if (bAlertable && k32_dispatch_io_completions())
        return K32_WAIT_IO_COMPLETION;
    return result;
}

DWORD WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles,
                                     BOOL bWaitAll, DWORD dwMilliseconds)
{
    extern NTSTATUS sys_NtWaitForMultipleObjects(ULONG_PTR *args);

    if (!lpHandles || nCount == 0 || nCount > K32_MAXIMUM_WAIT_OBJECTS) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return WAIT_FAILED;
    }

    HANDLE handles[K32_MAXIMUM_WAIT_OBJECTS];
    for (DWORD i = 0; i < nCount; i++) {
        handles[i] = g_compat32_mode
            ? (HANDLE)(ULONG_PTR)((const uint32_t *)(const void *)lpHandles)[i]
            : lpHandles[i];
    }

    /* Quick check: if any handle is a Win32 thread, use polling path */
    uint64_t start = idt_get_ticks();
    uint64_t timeout_ticks = 0;
    if (dwMilliseconds != INFINITE)
        timeout_ticks = ((uint64_t)dwMilliseconds + 9) / 10;

    /* Check for Win32 thread handles — handle them specially */
    int has_pseudo_thread_handles = 0;
    for (DWORD i = 0; i < nCount; i++) {
        win32_thread_ctx_t *ctx = find_ctx_by_handle(handles[i]);
        if (ctx && !ctx->thread_object) {
            has_pseudo_thread_handles = 1;
            break;
        }
    }

    if (has_pseudo_thread_handles) {
        /* Polling path for mixed thread/sync objects */
        for (;;) {
            if (bWaitAll) {
                int all_done = 1;
                for (DWORD i = 0; i < nCount; i++) {
                    win32_thread_ctx_t *tc = find_ctx_by_handle(handles[i]);
                    if (tc && !tc->terminated) { all_done = 0; break; }
                }
                if (all_done) return WAIT_OBJECT_0;
            } else {
                for (DWORD i = 0; i < nCount; i++) {
                    win32_thread_ctx_t *tc = find_ctx_by_handle(handles[i]);
                    if (tc && tc->terminated) return WAIT_OBJECT_0 + i;
                }
            }

            if (dwMilliseconds != INFINITE && timeout_ticks > 0 &&
                (idt_get_ticks() - start) >= timeout_ticks) {
                return WAIT_TIMEOUT;
            }
            if (dwMilliseconds == 0) return WAIT_TIMEOUT;
            sched_yield();
        }
    }

    /* Standard NT path for sync objects */
    LARGE_INTEGER timeout;
    PLARGE_INTEGER p_timeout = NULL;

    if (dwMilliseconds != INFINITE) {
        timeout.QuadPart = -(LONGLONG)dwMilliseconds * 10000;
        p_timeout = &timeout;
    }

    ULONG_PTR args[5] = {
        (ULONG_PTR)nCount,
        (ULONG_PTR)handles,
        (ULONG_PTR)(bWaitAll ? 0 : 1),  /* NT: 0=WaitAll, 1=WaitAny */
        (ULONG_PTR)FALSE,               /* Alertable */
        (ULONG_PTR)p_timeout
    };

    uint64_t caller =
        (uint64_t)(ULONG_PTR)__builtin_return_address(0);
    for (DWORD i = 0; i < nCount; i++)
        steamipc_trace_handle("WaitN.begin", handles[i], i,
                              dwMilliseconds, caller);

    NTSTATUS status = sys_NtWaitForMultipleObjects(args);

    DWORD result;
    if (status == STATUS_TIMEOUT) {
        result = WAIT_TIMEOUT;
    } else if (NT_SUCCESS(status)) {
        result = (DWORD)status;
    } else {
        set_last_error_from_status(status);
        result = WAIT_FAILED;
    }

    for (DWORD i = 0; i < nCount; i++)
        steamipc_trace_handle("WaitN.end", handles[i], i,
                              result, caller);

    return result;
}

#define K32_MAX_REGISTERED_WAITS 256
#define K32_MAX_WAIT_DISPATCHERS 64
#define K32_WT_EXECUTEONLYONCE   0x00000008U

typedef void (WINAPI *K32_WAIT_CALLBACK)(PVOID context, BYTE timed_out);

typedef struct {
    volatile LONG allocated;
    volatile LONG canceled;
    volatile LONG callback_running;
    volatile LONG worker_done;
    volatile LONG worker_finalized;
    volatile LONG registered;
    volatile LONG cleanup_claimed;
    volatile LONG trace_seen;
    HANDLE token;
    HANDLE object;
    HANDLE completion_event;
    K32_WAIT_CALLBACK callback;
    PVOID context;
    DWORD milliseconds;
    DWORD flags;
    DWORD owner_pid;
    ULONGLONG deadline_ms;
    BOOL object_retained;
} K32_REGISTERED_WAIT;

typedef struct {
    volatile LONG state; /* 0=free, 1=initializing, 2=ready, 3=stopping */
    DWORD owner_pid;
    HANDLE worker_thread;
    int kernel_pid;
    volatile ULONGLONG poll_count;
    volatile ULONGLONG last_poll_ms;
} K32_WAIT_DISPATCHER;

static K32_REGISTERED_WAIT g_registered_waits[K32_MAX_REGISTERED_WAITS];
static K32_WAIT_DISPATCHER
    g_wait_dispatchers[K32_MAX_WAIT_DISPATCHERS];
static volatile ULONG_PTR next_wait_registration = 0xD1200000;
static volatile LONG g_registered_wait_trace_budget = 96;

static BOOL k32_registered_wait_trace(void)
{
    return __atomic_fetch_sub(&g_registered_wait_trace_budget, 1,
                              __ATOMIC_RELAXED) > 0;
}

static void k32_store_registered_wait_handle(HANDLE *out_wait, HANDLE value)
{
    if (g_compat32_mode)
        *(uint32_t *)(void *)out_wait = (uint32_t)(ULONG_PTR)value;
    else
        *out_wait = value;
}

static K32_REGISTERED_WAIT *k32_find_registered_wait(HANDLE token)
{
    for (int i = 0; i < K32_MAX_REGISTERED_WAITS; i++) {
        K32_REGISTERED_WAIT *wait = &g_registered_waits[i];
        if (__atomic_load_n(&wait->allocated, __ATOMIC_ACQUIRE) &&
            wait->token == token)
            return wait;
    }
    return NULL;
}

static void k32_registered_wait_try_cleanup(K32_REGISTERED_WAIT *wait)
{
    if (!wait ||
        !__atomic_load_n(&wait->canceled, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&wait->worker_finalized, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&wait->registered, __ATOMIC_ACQUIRE))
        return;

    LONG expected = 0;
    if (!__atomic_compare_exchange_n(&wait->cleanup_claimed, &expected, 1,
                                     FALSE, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;

    if (wait->object_retained) {
        NTSTATUS status = nt_close_handle_for_process(wait->object,
                                                       wait->owner_pid);
        if (!NT_SUCCESS(status)) {
            DWORD current_pid = win32_current_process_id();
            OBJECT_TYPE_ID type;
            ULONG refs;
            ULONG owner_refs;
            ULONG sole_owner;
            BOOL valid = handle_query_state(
                &g_handle_table, wait->object, wait->owner_pid, &type, &refs,
                &owner_refs, &sole_owner);
            serial_puts("[K32-WAITREG] release failed token=");
            serial_puthex((uint64_t)(ULONG_PTR)wait->token, 8);
            serial_puts(" object=");
            serial_puthex((uint64_t)(ULONG_PTR)wait->object, 8);
            serial_puts(" current_pid=");
            serial_putdec(current_pid);
            serial_puts(" owner_pid=");
            serial_putdec(wait->owner_pid);
            serial_puts(" status=0x");
            serial_puthex((uint32_t)status, 8);
            serial_puts(" valid=");
            serial_putdec(valid);
            serial_puts(" type=");
            serial_putdec(type);
            serial_puts(" refs=");
            serial_putdec(refs);
            serial_puts(" owner_refs=");
            serial_putdec(owner_refs);
            serial_puts(" sole_owner=");
            serial_putdec(sole_owner);
            serial_puts("\n");
        }
    }

    wait->token = NULL;
    wait->object = NULL;
    wait->completion_event = NULL;
    wait->callback = NULL;
    wait->context = NULL;
    wait->object_retained = FALSE;
    __atomic_store_n(&wait->allocated, 0, __ATOMIC_RELEASE);
}

static void k32_registered_wait_signal_completion(K32_REGISTERED_WAIT *wait)
{
    HANDLE completion = __atomic_exchange_n(&wait->completion_event, NULL,
                                             __ATOMIC_ACQ_REL);
    if (completion && completion != (HANDLE)(ULONG_PTR)-1)
        SetEvent(completion);
}

static void k32_registered_wait_finalize(K32_REGISTERED_WAIT *wait)
{
    if (__atomic_exchange_n(&wait->worker_done, 1,
                            __ATOMIC_ACQ_REL))
        return;

    HANDLE token = wait->token;
    serial_puts("[K32-WAITDISP] finalize token=");
    serial_puthex((uint64_t)(ULONG_PTR)token, 8);
    serial_puts("\n");

    k32_registered_wait_signal_completion(wait);
    __atomic_store_n(&wait->worker_finalized, 1, __ATOMIC_RELEASE);
    k32_registered_wait_try_cleanup(wait);
    serial_puts("[K32-WAITDISP] finalized token=");
    serial_puthex((uint64_t)(ULONG_PTR)token, 8);
    serial_puts("\n");
}

static BOOL k32_registered_wait_owner_has_live(DWORD owner_pid)
{
    for (int i = 0; i < K32_MAX_REGISTERED_WAITS; i++) {
        K32_REGISTERED_WAIT *wait = &g_registered_waits[i];
        if (__atomic_load_n(&wait->allocated, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&wait->registered, __ATOMIC_ACQUIRE) &&
            wait->owner_pid == owner_pid &&
            !__atomic_load_n(&wait->worker_finalized, __ATOMIC_ACQUIRE))
            return TRUE;
    }
    return FALSE;
}

static DWORD WINAPI k32_registered_wait_dispatcher(PVOID parameter)
{
    K32_WAIT_DISPATCHER *dispatcher = (K32_WAIT_DISPATCHER *)parameter;

    dispatcher->kernel_pid = proc_current_pid();
    serial_puts("[K32-WAITDISP] start owner=");
    serial_putdec(dispatcher->owner_pid);
    serial_puts(" kpid=");
    serial_putdec((uint64_t)(uint32_t)dispatcher->kernel_pid);
    serial_puts("\n");

    for (;;) {
        BOOL has_live_waits = FALSE;
        ULONGLONG now = GetTickCount64();
        __atomic_store_n(&dispatcher->last_poll_ms, now, __ATOMIC_RELEASE);
        __atomic_add_fetch(&dispatcher->poll_count, 1, __ATOMIC_RELAXED);

        for (int i = 0; i < K32_MAX_REGISTERED_WAITS; i++) {
            K32_REGISTERED_WAIT *wait = &g_registered_waits[i];
            if (!__atomic_load_n(&wait->allocated, __ATOMIC_ACQUIRE) ||
                !__atomic_load_n(&wait->registered, __ATOMIC_ACQUIRE) ||
                wait->owner_pid != dispatcher->owner_pid ||
                __atomic_load_n(&wait->worker_finalized,
                                __ATOMIC_ACQUIRE))
                continue;

            has_live_waits = TRUE;
            if (__atomic_load_n(&wait->canceled, __ATOMIC_ACQUIRE)) {
                k32_registered_wait_finalize(wait);
                continue;
            }

            DWORD result = WaitForSingleObject(wait->object, 0);
            BOOL timed_out = wait->milliseconds != INFINITE &&
                             now >= wait->deadline_ms;
            if (!__atomic_exchange_n(&wait->trace_seen, 1,
                                     __ATOMIC_ACQ_REL)) {
                serial_puts("[K32-WAITDISP] observe owner=");
                serial_putdec(dispatcher->owner_pid);
                serial_puts(" kpid=");
                serial_putdec((uint64_t)(uint32_t)proc_current_pid());
                serial_puts(" token=");
                serial_puthex((uint64_t)(ULONG_PTR)wait->token, 8);
                serial_puts(" object=");
                serial_puthex((uint64_t)(ULONG_PTR)wait->object, 8);
                serial_puts(" result=");
                serial_puthex(result, 8);
                serial_puts(" polls=");
                serial_putdec(__atomic_load_n(&dispatcher->poll_count,
                                               __ATOMIC_ACQUIRE));
                serial_puts("\n");
            }
            if (result == WAIT_FAILED) {
                serial_puts("[K32-WAITDISP] wait failed token=");
                serial_puthex((uint64_t)(ULONG_PTR)wait->token, 8);
                serial_puts(" error=");
                serial_putdec(GetLastError());
                serial_puts("\n");
                k32_registered_wait_finalize(wait);
                continue;
            }
            if (result != WAIT_OBJECT_0 && !timed_out)
                continue;

            __atomic_store_n(&wait->callback_running, 1,
                             __ATOMIC_RELEASE);
            if (!__atomic_load_n(&wait->canceled, __ATOMIC_ACQUIRE)) {
                if (k32_registered_wait_trace()) {
                    serial_puts("[K32-WAITREG] callback token=");
                    serial_puthex((uint64_t)(ULONG_PTR)wait->token, 8);
                    serial_puts(" timeout=");
                    serial_putdec(timed_out);
                    serial_puts("\n");
                }
                wait->callback(wait->context, (BYTE)timed_out);
                serial_puts("[K32-WAITDISP] callback returned token=");
                serial_puthex((uint64_t)(ULONG_PTR)wait->token, 8);
                serial_puts("\n");
            }
            __atomic_store_n(&wait->callback_running, 0,
                             __ATOMIC_RELEASE);

            if (__atomic_load_n(&wait->canceled, __ATOMIC_ACQUIRE) ||
                (wait->flags & K32_WT_EXECUTEONLYONCE)) {
                k32_registered_wait_finalize(wait);
            } else if (wait->milliseconds != INFINITE) {
                wait->deadline_ms = GetTickCount64() + wait->milliseconds;
            }
        }

        if (!has_live_waits) {
            LONG expected = 2;
            if (__atomic_compare_exchange_n(&dispatcher->state, &expected, 3,
                                             FALSE, __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE)) {
                /* Registrations publish registered before obtaining a
                 * dispatcher. Recheck after entering stopping so a concurrent
                 * registration either keeps this worker or waits for a new
                 * one after it exits. */
                if (k32_registered_wait_owner_has_live(
                        dispatcher->owner_pid)) {
                    __atomic_store_n(&dispatcher->state, 2,
                                     __ATOMIC_RELEASE);
                    continue;
                }
                serial_puts("[K32-WAITDISP] stop owner=");
                serial_putdec(dispatcher->owner_pid);
                serial_puts(" kpid=");
                serial_putdec((uint64_t)(uint32_t)dispatcher->kernel_pid);
                serial_puts("\n");
                return 0;
            }
        }

        Sleep(1);
    }
}

static BOOL k32_reap_wait_dispatcher(K32_WAIT_DISPATCHER *dispatcher)
{
    if (__atomic_load_n(&dispatcher->state, __ATOMIC_ACQUIRE) != 3)
        return FALSE;

    HANDLE worker = dispatcher->worker_thread;
    if (worker && WaitForSingleObject(worker, 0) != WAIT_OBJECT_0)
        return FALSE;

    LONG expected = 3;
    if (!__atomic_compare_exchange_n(&dispatcher->state, &expected, 1,
                                     FALSE, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return FALSE;

    if (worker)
        CloseHandle(worker);
    dispatcher->worker_thread = NULL;
    dispatcher->owner_pid = 0;
    dispatcher->kernel_pid = 0;
    dispatcher->poll_count = 0;
    dispatcher->last_poll_ms = 0;
    __atomic_store_n(&dispatcher->state, 0, __ATOMIC_RELEASE);
    return TRUE;
}

static BOOL k32_wait_dispatcher_owner_active(DWORD owner_pid)
{
    for (int i = 0; i < K32_MAX_WAIT_DISPATCHERS; i++) {
        K32_WAIT_DISPATCHER *dispatcher = &g_wait_dispatchers[i];
        if (__atomic_load_n(&dispatcher->state, __ATOMIC_ACQUIRE) &&
            dispatcher->owner_pid == owner_pid)
            return TRUE;
    }
    return FALSE;
}

static K32_WAIT_DISPATCHER *k32_get_wait_dispatcher(DWORD owner_pid)
{
    for (;;) {
        BOOL initializing = FALSE;
        BOOL owner_stopping = FALSE;

        for (int i = 0; i < K32_MAX_WAIT_DISPATCHERS; i++) {
            K32_WAIT_DISPATCHER *dispatcher = &g_wait_dispatchers[i];
            LONG state = __atomic_load_n(&dispatcher->state,
                                         __ATOMIC_ACQUIRE);
            if (state == 3) {
                if (k32_reap_wait_dispatcher(dispatcher))
                    continue;
                if (dispatcher->owner_pid == owner_pid)
                    owner_stopping = TRUE;
            } else if (state == 1) {
                initializing = TRUE;
            } else if (state == 2 &&
                       dispatcher->owner_pid == owner_pid) {
                return dispatcher;
            }
        }

        if (initializing || owner_stopping) {
            sched_yield();
            continue;
        }

        for (int i = 0; i < K32_MAX_WAIT_DISPATCHERS; i++) {
            K32_WAIT_DISPATCHER *dispatcher = &g_wait_dispatchers[i];
            LONG expected = 0;
            if (!__atomic_compare_exchange_n(&dispatcher->state, &expected, 1,
                                             FALSE, __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE))
                continue;

            dispatcher->owner_pid = owner_pid;
            dispatcher->worker_thread = NULL;
            dispatcher->kernel_pid = 0;
            dispatcher->poll_count = 0;
            dispatcher->last_poll_ms = 0;
            dispatcher->worker_thread = CreateThread(
                NULL, 0, k32_registered_wait_dispatcher, dispatcher, 0, NULL);
            if (!dispatcher->worker_thread) {
                dispatcher->owner_pid = 0;
                __atomic_store_n(&dispatcher->state, 0, __ATOMIC_RELEASE);
                return NULL;
            }
            __atomic_store_n(&dispatcher->state, 2, __ATOMIC_RELEASE);
            return dispatcher;
        }
        return NULL;
    }
}

static BOOL WINAPI RegisterWaitForSingleObject_k32(HANDLE *out_wait,
                                                    HANDLE object,
                                                    PVOID callback,
                                                    PVOID context,
                                                    DWORD milliseconds,
                                                    DWORD flags)
{
    if (!out_wait || !object || !callback) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    /* PE32 callback workers need a compat-mode kernel trampoline. Preserve
     * the previous behavior until that bridge exists. */
    if (g_compat32_mode) {
        HANDLE token = (HANDLE)__atomic_add_fetch(&next_wait_registration, 1,
                                                   __ATOMIC_RELAXED);
        k32_store_registered_wait_handle(out_wait, token);
        return TRUE;
    }

    K32_REGISTERED_WAIT *wait = NULL;
    for (int i = 0; i < K32_MAX_REGISTERED_WAITS; i++) {
        LONG expected = 0;
        if (__atomic_compare_exchange_n(&g_registered_waits[i].allocated,
                                        &expected, 1, FALSE,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            wait = &g_registered_waits[i];
            break;
        }
    }
    if (!wait) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    wait->canceled = 0;
    wait->callback_running = 0;
    wait->worker_done = 0;
    wait->worker_finalized = 0;
    wait->registered = 0;
    wait->cleanup_claimed = 0;
    wait->trace_seen = 0;
    wait->token = (HANDLE)__atomic_add_fetch(&next_wait_registration, 1,
                                              __ATOMIC_RELAXED);
    wait->object = object;
    wait->completion_event = NULL;
    wait->callback = (K32_WAIT_CALLBACK)callback;
    wait->context = context;
    wait->milliseconds = milliseconds;
    wait->flags = flags;
    wait->owner_pid = win32_current_process_id();
    wait->deadline_ms = milliseconds == INFINITE
                      ? 0 : GetTickCount64() + milliseconds;
    wait->object_retained = FALSE;
    k32_store_registered_wait_handle(out_wait, wait->token);

    if (!NT_SUCCESS(handle_retain_for_process(&g_handle_table, object,
                                               wait->owner_pid))) {
        k32_store_registered_wait_handle(out_wait, NULL);
        wait->allocated = 0;
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    wait->object_retained = TRUE;
    __atomic_store_n(&wait->registered, 1, __ATOMIC_RELEASE);

    K32_WAIT_DISPATCHER *dispatcher =
        k32_get_wait_dispatcher(wait->owner_pid);
    if (!dispatcher) {
        k32_store_registered_wait_handle(out_wait, NULL);
        __atomic_store_n(&wait->canceled, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&wait->worker_done, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&wait->worker_finalized, 1, __ATOMIC_RELEASE);
        k32_registered_wait_try_cleanup(wait);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    if (k32_registered_wait_trace()) {
        serial_puts("[K32-WAITREG] register pid=");
        serial_putdec(wait->owner_pid);
        serial_puts(" token=");
        serial_puthex((uint64_t)(ULONG_PTR)wait->token, 8);
        serial_puts(" object=");
        serial_puthex((uint64_t)(ULONG_PTR)object, 8);
        serial_puts(" callback=");
        serial_puthex((uint64_t)(ULONG_PTR)callback, 16);
        serial_puts(" timeout=");
        serial_puthex(milliseconds, 8);
        serial_puts(" flags=");
        serial_puthex(flags, 8);
        serial_puts(" dispatcher_kpid=");
        serial_putdec((uint64_t)(uint32_t)dispatcher->kernel_pid);
        serial_puts(" polls=");
        serial_putdec(__atomic_load_n(&dispatcher->poll_count,
                                      __ATOMIC_ACQUIRE));
        serial_puts(" age_ms=");
        ULONGLONG last_poll = __atomic_load_n(&dispatcher->last_poll_ms,
                                               __ATOMIC_ACQUIRE);
        ULONGLONG current_ms = GetTickCount64();
        serial_putdec(last_poll && current_ms >= last_poll
                    ? current_ms - last_poll : 0);
        serial_puts("\n");
    }

    SetLastError(0);
    return TRUE;
}

static BOOL k32_unregister_wait(HANDLE token, HANDLE completion_event)
{
    if (!token) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    K32_REGISTERED_WAIT *wait = k32_find_registered_wait(token);
    if (!wait) {
        /* PE32 registrations still use the legacy synthetic handles. */
        if (g_compat32_mode && (ULONG_PTR)token > 0xD1200000U)
            return TRUE;
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    __atomic_store_n(&wait->completion_event, completion_event,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&wait->canceled, 1, __ATOMIC_RELEASE);

    if (k32_registered_wait_trace()) {
        serial_puts("[K32-WAITREG] unregister current_pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" owner_pid=");
        serial_putdec(wait->owner_pid);
        serial_puts(" token=");
        serial_puthex((uint64_t)(ULONG_PTR)token, 8);
        serial_puts(" completion=");
        serial_puthex((uint64_t)(ULONG_PTR)completion_event, 16);
        serial_puts(" running=");
        serial_putdec(__atomic_load_n(&wait->callback_running,
                                      __ATOMIC_ACQUIRE));
        serial_puts("\n");
    }

    /* Publishing canceled prevents a dispatcher that has not started the
     * callback from entering it. If no callback is running now, completion
     * is already guaranteed even if the dispatcher itself is delayed. */
    if (!__atomic_load_n(&wait->callback_running, __ATOMIC_ACQUIRE))
        k32_registered_wait_signal_completion(wait);

    if (completion_event == (HANDLE)(ULONG_PTR)-1) {
        while (!__atomic_load_n(&wait->worker_finalized, __ATOMIC_ACQUIRE))
            Sleep(1);
    }

    k32_registered_wait_try_cleanup(wait);
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI UnregisterWait_k32(HANDLE wait)
{
    return k32_unregister_wait(wait, NULL);
}

static BOOL WINAPI UnregisterWaitEx_k32(HANDLE wait, HANDLE completion_event)
{
    return k32_unregister_wait(wait, completion_event);
}

BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority)
{
    (void)hThread;
    (void)nPriority;
    return TRUE;
}

static DWORD WINAPI SetThreadExecutionState_k32(DWORD flags)
{
    const DWORD valid_flags = K32_ES_SYSTEM_REQUIRED |
                              K32_ES_DISPLAY_REQUIRED |
                              K32_ES_AWAYMODE_REQUIRED |
                              K32_ES_CONTINUOUS;
    if (!flags || (flags & ~valid_flags)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    K32_EXECUTION_STATE *entry = k32_execution_state_current(TRUE);
    if (!entry) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }

    DWORD previous = __atomic_load_n(&entry->state, __ATOMIC_ACQUIRE);
    if (flags & K32_ES_CONTINUOUS) {
        __atomic_store_n(&entry->state, flags, __ATOMIC_RELEASE);
    }
    /* OsitoK currently has no automatic display/system suspend timer. Calls
     * without ES_CONTINUOUS therefore need no additional timer reset here. */
    SetLastError(0);
    return previous;
}

BOOL WINAPI SetPriorityClass(HANDLE hProcess, DWORD dwPriorityClass)
{
    (void)hProcess;
    (void)dwPriorityClass;
    SetLastError(0);
    return TRUE;
}

DWORD WINAPI GetPriorityClass(HANDLE hProcess)
{
    (void)hProcess;
    return 0x20; /* NORMAL_PRIORITY_CLASS */
}

static int WINAPI GetThreadPriority_k32(HANDLE hThread)
{
    (void)hThread;
    return 0; /* THREAD_PRIORITY_NORMAL; SetThreadPriority is advisory. */
}

static LONG WINAPI RoInitialize_k32(DWORD init_type)
{
    (void)init_type;
    return 0; /* S_OK; WinRT activation is not exposed yet. */
}

static void WINAPI RoUninitialize_k32(void)
{
}

static LONG WINAPI RoGetActivationFactory_k32(HANDLE class_id, LPCGUID iid,
                                               PVOID *factory)
{
    (void)class_id;
    (void)iid;
    if (factory) *factory = NULL;
    return (LONG)0x80040154; /* REGDB_E_CLASSNOTREG */
}

static LONG WINAPI RoActivateInstance_k32(HANDLE class_id, PVOID *instance)
{
    (void)class_id;
    if (instance) *instance = NULL;
    return (LONG)0x80040154; /* REGDB_E_CLASSNOTREG */
}

typedef struct {
    DWORD magic;
    DWORD length;
    PCWSTR buffer;
} K32_HSTRING;

#define K32_HSTRING_OWNED 0x48535452U
#define K32_HSTRING_REF   0x48535246U

static LONG WINAPI WindowsCreateString_k32(PCWSTR source, DWORD length,
                                            HANDLE *out_string)
{
    if (!out_string) return (LONG)0x80004003; /* E_POINTER */
    *out_string = NULL;
    if (!length) return 0;
    if (!source) return (LONG)0x80004003;

    SIZE_T bytes = sizeof(K32_HSTRING) +
                   ((SIZE_T)length + 1) * sizeof(WCHAR);
    K32_HSTRING *string = (K32_HSTRING *)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!string) return (LONG)0x8007000E; /* E_OUTOFMEMORY */

    WCHAR *buffer = (WCHAR *)(string + 1);
    for (DWORD i = 0; i < length; i++) buffer[i] = source[i];
    buffer[length] = 0;
    string->magic = K32_HSTRING_OWNED;
    string->length = length;
    string->buffer = buffer;
    *out_string = (HANDLE)string;
    return 0;
}

static LONG WINAPI WindowsCreateStringReference_k32(PCWSTR source, DWORD length,
                                                     PVOID header,
                                                     HANDLE *out_string)
{
    if (!header || !out_string) return (LONG)0x80004003;
    *out_string = NULL;
    if (!length) return 0;
    if (!source) return (LONG)0x80004003;

    K32_HSTRING *string = (K32_HSTRING *)header;
    string->magic = K32_HSTRING_REF;
    string->length = length;
    string->buffer = source;
    *out_string = (HANDLE)string;
    return 0;
}

static PCWSTR WINAPI WindowsGetStringRawBuffer_k32(HANDLE string,
                                                    DWORD *length)
{
    static const WCHAR empty[] = { 0 };
    if (!string) {
        if (length) *length = 0;
        return empty;
    }

    K32_HSTRING *value = (K32_HSTRING *)string;
    if (value->magic != K32_HSTRING_OWNED &&
        value->magic != K32_HSTRING_REF) {
        if (length) *length = 0;
        return empty;
    }
    if (length) *length = value->length;
    return value->buffer;
}

static LONG WINAPI WindowsDeleteString_k32(HANDLE string)
{
    if (!string) return 0;
    K32_HSTRING *value = (K32_HSTRING *)string;
    if (value->magic == K32_HSTRING_OWNED)
        HeapFree(GetProcessHeap(), 0, value);
    return 0;
}

static BOOL WINAPI SetThreadInformation_stub(HANDLE thread, DWORD info_class,
                                             PVOID info, DWORD info_size)
{
    (void)thread; (void)info_class;
    if (!info || !info_size) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    /* ponytail: scheduling hints are accepted until per-thread policy exists. */
    return TRUE;
}

/* ── Event API (Phase 21) ──────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

extern NTSTATUS sys_NtCreateSemaphore(ULONG_PTR *);
extern NTSTATUS sys_NtReleaseSemaphore(ULONG_PTR *);

#define EVENT_TYPE_NOTIFICATION_K32     0   /* manual-reset */
#define EVENT_TYPE_SYNCHRONIZATION_K32  1   /* auto-reset */

#define K32_MAX_NAMED_EVENTS K32_MAX_NAMED_OBJECTS
static K32_NAMED_OBJECT named_events[K32_MAX_NAMED_EVENTS];

typedef struct {
    USHORT length;
    USHORT maximum_length;
    ULONG buffer;
} K32_UNICODE_STRING32;

typedef struct {
    ULONG length;
    ULONG root_directory;
    ULONG object_name;
    ULONG attributes;
    ULONG security_descriptor;
    ULONG security_quality_of_service;
} K32_OBJECT_ATTRIBUTES32;

static NTSTATUS named_event_name_from_attributes(
    POBJECT_ATTRIBUTES attributes, char name[K32_OBJECT_NAME_MAX],
    BOOL *has_name)
{
    char raw[K32_OBJECT_NAME_MAX * 2];
    USHORT byte_length = 0;
    ULONG_PTR buffer = 0;

    if (!has_name) return STATUS_INVALID_PARAMETER;
    *has_name = FALSE;
    name[0] = 0;
    if (!attributes) return STATUS_SUCCESS;

    if (g_compat32_mode) {
        K32_OBJECT_ATTRIBUTES32 *attributes32 =
            (K32_OBJECT_ATTRIBUTES32 *)(ULONG_PTR)
                ((ULONG_PTR)attributes & 0xFFFFFFFFULL);
        if (!attributes32->object_name) return STATUS_SUCCESS;
        K32_UNICODE_STRING32 *string32 =
            (K32_UNICODE_STRING32 *)(ULONG_PTR)attributes32->object_name;
        byte_length = string32->length;
        buffer = (ULONG_PTR)string32->buffer;
    } else {
        if (!attributes->ObjectName) return STATUS_SUCCESS;
        byte_length = attributes->ObjectName->Length;
        buffer = (ULONG_PTR)attributes->ObjectName->Buffer;
    }

    if (byte_length & 1) return STATUS_OBJECT_NAME_INVALID;
    ULONG chars = byte_length / sizeof(WCHAR);
    if (!chars) return STATUS_SUCCESS;
    if (!buffer || chars >= sizeof(raw)) return STATUS_OBJECT_NAME_INVALID;

    const WCHAR *wide = (const WCHAR *)buffer;
    for (ULONG i = 0; i < chars; i++) {
        if (wide[i] > 0x7f) return STATUS_OBJECT_NAME_INVALID;
        raw[i] = (char)wide[i];
    }
    raw[chars] = 0;
    if (!named_object_canonicalize(raw, name))
        return STATUS_OBJECT_NAME_INVALID;
    *has_name = name[0] != 0;
    return STATUS_SUCCESS;
}

NTSTATUS kernel32_nt_open_named_event(POBJECT_ATTRIBUTES attributes,
                                      ACCESS_MASK desired_access,
                                      PHANDLE event_handle)
{
    if (!event_handle) return STATUS_INVALID_PARAMETER;
    *event_handle = NULL;

    char name[K32_OBJECT_NAME_MAX];
    BOOL has_name = FALSE;
    NTSTATUS status = named_event_name_from_attributes(
        attributes, name, &has_name);
    if (!NT_SUCCESS(status) || !has_name)
        return NT_SUCCESS(status) ? STATUS_OBJECT_NAME_NOT_FOUND : status;

    DWORD saved_error = g_last_error;
    HANDLE handle = named_object_open(
        named_events, K32_MAX_NAMED_EVENTS, OBJ_TYPE_EVENT,
        name, desired_access);
    g_last_error = saved_error;
    if (!handle) return STATUS_OBJECT_NAME_NOT_FOUND;

    *event_handle = handle;
    if (named_object_is_steamipc(name)) {
        serial_puts("[K32-NATIVE-EVENT] open pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" name='");
        serial_puts(name);
        serial_puts("' handle=0x");
        serial_puthex((ULONG_PTR)handle, 16);
        serial_puts("\n");
    }
    return STATUS_SUCCESS;
}

NTSTATUS kernel32_nt_publish_named_event(POBJECT_ATTRIBUTES attributes,
                                         ACCESS_MASK desired_access,
                                         HANDLE created_handle,
                                         PHANDLE event_handle,
                                         BOOL *already_exists)
{
    if (!created_handle || !event_handle || !already_exists)
        return STATUS_INVALID_PARAMETER;
    *event_handle = created_handle;
    *already_exists = FALSE;

    char name[K32_OBJECT_NAME_MAX];
    BOOL has_name = FALSE;
    NTSTATUS status = named_event_name_from_attributes(
        attributes, name, &has_name);
    if (!NT_SUCCESS(status) || !has_name)
        return status;

    DWORD saved_error = g_last_error;
    HANDLE published = named_object_publish_access(
        named_events, K32_MAX_NAMED_EVENTS, OBJ_TYPE_EVENT,
        name, created_handle, desired_access, already_exists);
    g_last_error = saved_error;
    if (!published) return STATUS_INSUFFICIENT_RESOURCES;

    *event_handle = published;
    if (named_object_is_steamipc(name)) {
        serial_puts("[K32-NATIVE-EVENT] publish pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" name='");
        serial_puts(name);
        serial_puts("' handle=0x");
        serial_puthex((ULONG_PTR)published, 16);
        serial_puts(*already_exists ? " existing=1\n" : " existing=0\n");
    }
    return STATUS_SUCCESS;
}

static HANDLE create_event_k32(BOOL manual_reset, BOOL initial_state,
                               const char *name, uint64_t caller)
{
    BOOL trace = K32_STEAMIPC_SERIAL_TRACE &&
                 named_object_is_steamchrome(name);
    if (trace) {
        serial_puts("[K32-STEAMIPC] CreateEvent pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" name='");
        serial_puts(name);
        serial_puts("' manual=");
        serial_putdec(manual_reset);
        serial_puts(" initial=");
        serial_putdec(initial_state);
        serial_puts("\n");
    }
    if (name && *name) {
        HANDLE existing = named_object_open(
            named_events, K32_MAX_NAMED_EVENTS, OBJ_TYPE_EVENT,
            name, GENERIC_ALL);
        if (existing) {
            SetLastError(183); /* ERROR_ALREADY_EXISTS */
            if (trace) {
                serial_puts("[K32-STEAMIPC] event existing handle=0x");
                serial_puthex((ULONG_PTR)existing, 16);
                serial_puts(" last_error=183\n");
            }
            return existing;
        }
    }

    HANDLE handle = NULL;
    ULONG type = manual_reset ? EVENT_TYPE_NOTIFICATION_K32
                              : EVENT_TYPE_SYNCHRONIZATION_K32;
    NTSTATUS status = NtCreateEvent(&handle, GENERIC_ALL, NULL, type,
                                    initial_state);
    if (!NT_SUCCESS(status)) {
        if (trace) {
            serial_puts("[K32-STEAMIPC] event create failed status=0x");
            serial_puthex((uint32_t)status, 8);
            serial_puts("\n");
        }
        set_last_error_from_status(status);
        return NULL;
    }

    if (name && *name) {
        BOOL already_exists = FALSE;
        HANDLE published = named_object_publish(
            named_events, K32_MAX_NAMED_EVENTS, OBJ_TYPE_EVENT,
            name, handle, &already_exists);
        if (!published) {
            CloseHandle(handle);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        if (already_exists) {
            CloseHandle(handle);
            SetLastError(183); /* ERROR_ALREADY_EXISTS */
            return published;
        }
        handle = published;
    }
    SetLastError(0);
    if (trace) {
        serial_puts("[K32-STEAMIPC] event created handle=0x");
        serial_puthex((ULONG_PTR)handle, 16);
        serial_puts(" last_error=0\n");
    }
    k32_sync_trace(K32_SYNC_CREATE, handle,
                   (manual_reset ? 1U : 0U) | (initial_state ? 2U : 0U),
                   NT_SUCCESS(status) ? 0U : (uint32_t)status, caller);
    return handle;
}

HANDLE WINAPI CreateEventA(PVOID lpEventAttributes, BOOL bManualReset,
                           BOOL bInitialState, PCSTR lpName)
{
    (void)lpEventAttributes;
    char name[K32_OBJECT_NAME_MAX];
    if (!named_object_name_a(lpName, name)) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NULL;
    }
    return create_event_k32(
        bManualReset, bInitialState, name,
        (uint64_t)(ULONG_PTR)__builtin_return_address(0));
}

HANDLE WINAPI CreateEventW(PVOID lpEventAttributes, BOOL bManualReset,
                           BOOL bInitialState, PCWSTR lpName)
{
    (void)lpEventAttributes;
    char name[K32_OBJECT_NAME_MAX];
    if (!named_object_name_w(lpName, name)) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NULL;
    }
    return create_event_k32(
        bManualReset, bInitialState, name,
        (uint64_t)(ULONG_PTR)__builtin_return_address(0));
}

static HANDLE create_semaphore_k32(LONG initial_count, LONG maximum_count)
{
    HANDLE handle = NULL;
    ULONG_PTR args[5] = {
        (ULONG_PTR)&handle, GENERIC_ALL, 0,
        (ULONG_PTR)initial_count, (ULONG_PTR)maximum_count
    };
    NTSTATUS status = sys_NtCreateSemaphore(args);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return NULL;
    }
    return handle;
}

static HANDLE WINAPI CreateSemaphoreA_k32(PVOID attributes, LONG initial_count,
                                           LONG maximum_count, PCSTR name)
{
    (void)attributes;
    (void)name;
    return create_semaphore_k32(initial_count, maximum_count);
}

static HANDLE WINAPI CreateSemaphoreW_k32(PVOID attributes, LONG initial_count,
                                           LONG maximum_count, PCWSTR name)
{
    (void)attributes;
    (void)name;
    return create_semaphore_k32(initial_count, maximum_count);
}

static BOOL WINAPI ReleaseSemaphore_k32(HANDLE semaphore, LONG release_count,
                                         LONG *previous_count)
{
    ULONG_PTR args[3] = {
        (ULONG_PTR)semaphore, (ULONG_PTR)release_count,
        (ULONG_PTR)previous_count
    };
    NTSTATUS status = sys_NtReleaseSemaphore(args);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI SetEvent(HANDLE hEvent)
{
    uint64_t caller =
        (uint64_t)(ULONG_PTR)__builtin_return_address(0);
    NTSTATUS status = NtSetEvent(hEvent, NULL);
    k32_sync_trace(K32_SYNC_SET, hEvent, 0, (uint32_t)status,
                   caller);
    steamipc_trace_handle("SetEvent", hEvent, 0, (uint32_t)status,
                          caller);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI ResetEvent(HANDLE hEvent)
{
    if (!hEvent)
        log_null_sync_handle("ResetEvent");
    uint64_t caller =
        (uint64_t)(ULONG_PTR)__builtin_return_address(0);
    NTSTATUS status = NtResetEvent(hEvent, NULL);
    k32_sync_trace(K32_SYNC_RESET, hEvent, 0, (uint32_t)status,
                   caller);
    steamipc_trace_handle("ResetEvent", hEvent, 0, (uint32_t)status,
                          caller);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI PulseEvent(HANDLE hEvent)
{
    serial_puts("[K32] PulseEvent\n");
    NTSTATUS status = NtPulseEvent(hEvent, NULL);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

HANDLE WINAPI OpenEventA(DWORD dwDesiredAccess, BOOL bInheritHandle, PCSTR lpName)
{
    (void)bInheritHandle;
    char name[K32_OBJECT_NAME_MAX];
    if (!lpName || !named_object_name_a(lpName, name)) {
        SetLastError(lpName ? 206 : 87);
        return NULL;
    }
    HANDLE handle = named_object_open(named_events, K32_MAX_NAMED_EVENTS,
                                      OBJ_TYPE_EVENT, name, dwDesiredAccess);
    if (K32_STEAMIPC_SERIAL_TRACE &&
        named_object_is_steamchrome(name)) {
        serial_puts("[K32-STEAMIPC] OpenEventA pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" name='");
        serial_puts(name);
        serial_puts("' handle=0x");
        serial_puthex((ULONG_PTR)handle, 16);
        serial_puts(" last_error=");
        serial_putdec(GetLastError());
        serial_puts("\n");
    }
    if (!handle) {
        serial_puts("[K32] OpenEventA: not found: ");
        serial_puts(name);
        serial_puts("\n");
    }
    return handle;
}

HANDLE WINAPI OpenEventW(DWORD dwDesiredAccess, BOOL bInheritHandle, PCWSTR lpName)
{
    (void)bInheritHandle;
    char name[K32_OBJECT_NAME_MAX];
    if (!lpName || !named_object_name_w(lpName, name)) {
        SetLastError(lpName ? 206 : 87);
        return NULL;
    }
    HANDLE handle = named_object_open(named_events, K32_MAX_NAMED_EVENTS,
                                      OBJ_TYPE_EVENT, name, dwDesiredAccess);
    if (K32_STEAMIPC_SERIAL_TRACE &&
        named_object_is_steamchrome(name)) {
        serial_puts("[K32-STEAMIPC] OpenEventW pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" name='");
        serial_puts(name);
        serial_puts("' handle=0x");
        serial_puthex((ULONG_PTR)handle, 16);
        serial_puts(" last_error=");
        serial_putdec(GetLastError());
        serial_puts("\n");
    }
    if (!handle) {
        serial_puts("[K32] OpenEventW: not found: ");
        serial_puts(name);
        serial_puts("\n");
    }
    return handle;
}

#define CREATE_WAITABLE_TIMER_MANUAL_RESET    0x00000001U
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002U
#define CREATE_WAITABLE_TIMER_VALID_FLAGS     0x00000003U
#define TIMER_ALL_ACCESS_K32                  0x001F0003U

#define K32_MAX_NAMED_TIMERS K32_MAX_NAMED_OBJECTS
static K32_NAMED_OBJECT named_timers[K32_MAX_NAMED_TIMERS];

static BOOL waitable_timer_name_a(PCSTR source,
                                  char name[K32_OBJECT_NAME_MAX])
{
    int i = 0;
    if (source)
        while (source[i] && i < K32_OBJECT_NAME_MAX - 1) {
            name[i] = source[i];
            i++;
        }
    name[i] = 0;
    return !source || !source[i];
}

static BOOL waitable_timer_name_w(PCWSTR source,
                                  char name[K32_OBJECT_NAME_MAX])
{
    int i = 0;
    if (source)
        while (source[i] && i < K32_OBJECT_NAME_MAX - 1) {
            name[i] = (char)source[i];
            i++;
        }
    name[i] = 0;
    return !source || !source[i];
}

static HANDLE create_waitable_timer_k32(PVOID attributes, const char *name,
                                        DWORD flags, DWORD desired_access)
{
    (void)attributes;
    if (flags & ~CREATE_WAITABLE_TIMER_VALID_FLAGS) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }

    if (name && *name) {
        HANDLE existing = named_object_open(
            named_timers, K32_MAX_NAMED_TIMERS, OBJ_TYPE_EVENT,
            name, desired_access);
        if (existing) {
            SetLastError(183); /* ERROR_ALREADY_EXISTS */
            return existing;
        }
    }

    HANDLE handle = NULL;
    ULONG type = (flags & CREATE_WAITABLE_TIMER_MANUAL_RESET)
                 ? EVENT_TYPE_NOTIFICATION_K32
                 : EVENT_TYPE_SYNCHRONIZATION_K32;
    NTSTATUS status = NtCreateEvent(&handle, desired_access, NULL, type, FALSE);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return NULL;
    }

    if (name && *name) {
        BOOL already_exists = FALSE;
        HANDLE published = named_object_publish_access(
            named_timers, K32_MAX_NAMED_TIMERS, OBJ_TYPE_EVENT,
            name, handle, desired_access, &already_exists);
        if (!published) {
            CloseHandle(handle);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        if (already_exists) {
            CloseHandle(handle);
            SetLastError(183); /* ERROR_ALREADY_EXISTS */
            return published;
        }
        handle = published;
    }

    /* The scheduler currently has a 10 ms tick.  HIGH_RESOLUTION is accepted
     * as a creation capability; deadlines are still rounded up by ntsync. */
    SetLastError(0);
    return handle;
}

static HANDLE WINAPI CreateWaitableTimerExA_k32(PVOID attributes, PCSTR name,
                                                 DWORD flags,
                                                 DWORD desired_access)
{
    char normalized[K32_OBJECT_NAME_MAX];
    if (!waitable_timer_name_a(name, normalized)) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NULL;
    }
    return create_waitable_timer_k32(attributes, normalized, flags,
                                     desired_access);
}

static HANDLE WINAPI CreateWaitableTimerExW_k32(PVOID attributes, PCWSTR name,
                                                 DWORD flags,
                                                 DWORD desired_access)
{
    char normalized[K32_OBJECT_NAME_MAX];
    if (!waitable_timer_name_w(name, normalized)) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NULL;
    }
    return create_waitable_timer_k32(attributes, normalized, flags,
                                     desired_access);
}

static HANDLE WINAPI CreateWaitableTimerA_k32(PVOID attributes,
                                               BOOL manual_reset,
                                               PCSTR name)
{
    return CreateWaitableTimerExA_k32(
        attributes, name,
        manual_reset ? CREATE_WAITABLE_TIMER_MANUAL_RESET : 0,
        TIMER_ALL_ACCESS_K32);
}

static HANDLE WINAPI CreateWaitableTimerW_k32(PVOID attributes,
                                               BOOL manual_reset,
                                               PCWSTR name)
{
    return CreateWaitableTimerExW_k32(
        attributes, name,
        manual_reset ? CREATE_WAITABLE_TIMER_MANUAL_RESET : 0,
        TIMER_ALL_ACCESS_K32);
}

static HANDLE open_waitable_timer_k32(const char *name, DWORD desired_access)
{
    HANDLE handle = named_object_open(
        named_timers, K32_MAX_NAMED_TIMERS, OBJ_TYPE_EVENT,
        name, desired_access);
    if (!handle && GetLastError() == 2)
        SetLastError(2); /* ERROR_FILE_NOT_FOUND */
    return handle;
}

static HANDLE WINAPI OpenWaitableTimerA_k32(DWORD desired_access,
                                             BOOL inherit_handle, PCSTR name)
{
    (void)inherit_handle;
    char normalized[K32_OBJECT_NAME_MAX];
    if (!name || !waitable_timer_name_a(name, normalized)) {
        SetLastError(name ? 206 : 87);
        return NULL;
    }
    return open_waitable_timer_k32(normalized, desired_access);
}

static HANDLE WINAPI OpenWaitableTimerW_k32(DWORD desired_access,
                                             BOOL inherit_handle, PCWSTR name)
{
    (void)inherit_handle;
    char normalized[K32_OBJECT_NAME_MAX];
    if (!name || !waitable_timer_name_w(name, normalized)) {
        SetLastError(name ? 206 : 87);
        return NULL;
    }
    return open_waitable_timer_k32(normalized, desired_access);
}

static BOOL WINAPI SetWaitableTimer_k32(HANDLE timer,
                                         PLARGE_INTEGER due_time,
                                         LONG period_ms,
                                         PVOID completion_routine,
                                         PVOID completion_arg,
                                         BOOL resume)
{
    (void)completion_arg;
    (void)resume;
    if (completion_routine) {
        SetLastError(50); /* ERROR_NOT_SUPPORTED */
        return FALSE;
    }

    extern NTSTATUS ntsync_set_waitable_timer(HANDLE, PLARGE_INTEGER, LONG);
    NTSTATUS status = ntsync_set_waitable_timer(timer, due_time, period_ms);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI SetWaitableTimerEx_k32(HANDLE timer,
                                           PLARGE_INTEGER due_time,
                                           LONG period_ms,
                                           PVOID completion_routine,
                                           PVOID completion_arg,
                                           PVOID wake_context,
                                           ULONG tolerable_delay_ms)
{
    (void)wake_context;
    (void)tolerable_delay_ms;
    return SetWaitableTimer_k32(timer, due_time, period_ms,
                                completion_routine, completion_arg, FALSE);
}

static BOOL WINAPI CancelWaitableTimer_k32(HANDLE timer)
{
    extern NTSTATUS ntsync_cancel_waitable_timer(HANDLE);
    NTSTATUS status = ntsync_cancel_waitable_timer(timer);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

/* ponytail: fixed queues; grow these only if a workload fills them. */
#define MAX_IOCP_PORTS        64
#define MAX_IOCP_PACKETS      256
#define MAX_IOCP_ASSOCIATIONS 512
#define MAX_PENDING_PIPE_READS 256
#define K32_PIPE_STAGE_SIZE    65536

typedef struct {
    DWORD bytes;
    ULONG_PTR key;
    PVOID overlapped;
    NTSTATUS completion_status;
    LONG pending_pipe_slot;
    DWORD pending_pipe_generation;
} IOCP_PACKET;

typedef struct {
    BOOL used;
    DWORD owner_pid;
    HANDLE handle;
    volatile LONG lock;
    DWORD concurrency_limit;
    DWORD active_count;
    DWORD *active_tids;
    DWORD head;
    DWORD tail;
    DWORD count;
    IOCP_PACKET packets[MAX_IOCP_PACKETS];
} IOCP_PORT;

typedef struct {
    BOOL used;
    DWORD owner_pid;
    HANDLE file;
    HANDLE port;
    ULONG_PTR key;
    BYTE modes;
} IOCP_ASSOCIATION;

typedef struct {
    BOOL active;
    BOOL servicing;
    BOOL completed;
    BOOL packet_queued;
    BOOL compat32;
    BOOL write;
    DWORD owner_pid;
    DWORD owner_tid;
    DWORD generation;
    uint64_t issue_sequence;
    HANDLE file;
    PVOID stream_identity;
    HANDLE event;
    PVOID buffer;
    DWORD length;
    DWORD progress;
    PVOID overlapped;
    NTSTATUS completion_status;
    DWORD completion_bytes;
    BYTE *write_stage;
    BOOL cancel_requested;
    BOOL abandoned;
    BYTE stage[K32_PIPE_STAGE_SIZE];
} K32_PENDING_PIPE_READ;

_Static_assert(sizeof(K32_PENDING_PIPE_READ) == 0x10078,
               "pending pipe layout changed");

static IOCP_PORT g_iocp_ports[MAX_IOCP_PORTS];
static IOCP_ASSOCIATION g_iocp_associations[MAX_IOCP_ASSOCIATIONS];
static K32_PENDING_PIPE_READ g_pending_pipe_reads[MAX_PENDING_PIPE_READS];
static volatile LONG g_iocp_association_lock;
static volatile LONG g_pending_pipe_read_lock;
static DWORD g_pending_pipe_generation;
static uint64_t g_pending_pipe_issue_sequence;
static volatile uint32_t g_iocp_mode_trace_count;
#if K32_IOCP_TRACE
static volatile uint32_t g_iocp_api_trace_count;
#endif

static void iocp_trace_notification_modes(HANDLE file, DWORD owner_pid,
                                          BYTE modes)
{
    if (__atomic_fetch_add(&g_iocp_mode_trace_count, 1,
                           __ATOMIC_RELAXED) >= 32)
        return;
    serial_puts("[IOCP-MODE] pid=");
    serial_putdec(owner_pid);
    serial_puts(" file=0x");
    serial_puthex((uint64_t)(ULONG_PTR)file, 8);
    serial_puts(" modes=0x");
    serial_puthex(modes, 2);
    serial_puts("\n");
}

static uint64_t iocp_irq_lock(volatile LONG *lock)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(lock, 1))
        __asm__ volatile ("pause" ::: "memory");
    return flags;
}

static void iocp_irq_unlock(volatile LONG *lock, uint64_t flags)
{
    __sync_lock_release(lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static uint64_t iocp_association_lock(void)
{
    return iocp_irq_lock(&g_iocp_association_lock);
}

static void iocp_association_unlock(uint64_t flags)
{
    iocp_irq_unlock(&g_iocp_association_lock, flags);
}

static uint64_t pending_pipe_read_lock(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&g_pending_pipe_read_lock, 1))
        __asm__ volatile ("pause");
    return flags;
}

static void pending_pipe_read_unlock(uint64_t flags)
{
    __sync_lock_release(&g_pending_pipe_read_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static uint64_t pending_pipe_next_sequence_locked(void)
{
    uint64_t sequence = ++g_pending_pipe_issue_sequence;
    if (!sequence)
        sequence = ++g_pending_pipe_issue_sequence;
    return sequence;
}

static BOOL pending_pipe_same_stream(const K32_PENDING_PIPE_READ *a,
                                     const K32_PENDING_PIPE_READ *b)
{
    if (a->stream_identity && b->stream_identity)
        return a->stream_identity == b->stream_identity;
    return a->owner_pid == b->owner_pid && a->file == b->file;
}

static BOOL pending_pipe_has_older_locked(int slot)
{
    const K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[slot];
    for (int i = 0; i < MAX_PENDING_PIPE_READS; i++) {
        const K32_PENDING_PIPE_READ *other = &g_pending_pipe_reads[i];
        if (i == slot || !other->active ||
            (other->completed && other->packet_queued) ||
            other->write != pending->write ||
            other->issue_sequence >= pending->issue_sequence)
            continue;
        if (pending_pipe_same_stream(other, pending))
            return TRUE;
    }
    return FALSE;
}

static IOCP_PORT *iocp_find_for_process(HANDLE handle, DWORD owner_pid)
{
    for (int i = 0; i < MAX_IOCP_PORTS; i++) {
        if (g_iocp_ports[i].used &&
            g_iocp_ports[i].owner_pid == owner_pid &&
            g_iocp_ports[i].handle == handle)
            return &g_iocp_ports[i];
    }
    return NULL;
}

static IOCP_PORT *iocp_find(HANDLE handle)
{
    return iocp_find_for_process(handle, win32_current_process_id());
}

static uint64_t iocp_lock(IOCP_PORT *port)
{
    return iocp_irq_lock(&port->lock);
}

static void iocp_unlock(IOCP_PORT *port, uint64_t flags)
{
    iocp_irq_unlock(&port->lock, flags);
}

static IOCP_PORT *iocp_create(HANDLE handle, DWORD concurrency_limit)
{
    if (!win32_thread_table_ensure())
        return NULL;

    DWORD owner_pid = win32_current_process_id();
    IOCP_PORT *port = iocp_find(handle);
    if (!port) {
        for (int i = 0; i < MAX_IOCP_PORTS; i++) {
            if (!g_iocp_ports[i].used) {
                port = &g_iocp_ports[i];
                break;
            }
        }
    }
    if (!port)
        return NULL;

    if (!port->active_tids) {
        port->active_tids = (DWORD *)kcalloc(g_win32_thread_capacity,
                                             sizeof(*port->active_tids));
        if (!port->active_tids)
            return NULL;
    }

    port->handle = handle;
    port->owner_pid = owner_pid;
    port->lock = 0;
    if (!concurrency_limit) {
        extern uint32_t smp_cpu_count(void);
        concurrency_limit = smp_cpu_count();
        if (!concurrency_limit)
            concurrency_limit = 1;
    }
    if (concurrency_limit > (DWORD)g_win32_thread_capacity)
        concurrency_limit = (DWORD)g_win32_thread_capacity;
    port->concurrency_limit = concurrency_limit;
    port->active_count = 0;
    for (int i = 0; i < g_win32_thread_capacity; i++)
        port->active_tids[i] = 0;
    port->head = 0;
    port->tail = 0;
    port->count = 0;
    port->used = TRUE;
    return port;
}

static int iocp_active_index_locked(IOCP_PORT *port, DWORD tid)
{
    for (DWORD i = 0; i < port->active_count; i++) {
        if (port->active_tids[i] == tid)
            return (int)i;
    }
    return -1;
}

static BOOL iocp_remove_active_locked(IOCP_PORT *port, DWORD tid)
{
    int index = iocp_active_index_locked(port, tid);
    if (index < 0)
        return FALSE;

    DWORD last = --port->active_count;
    port->active_tids[index] = port->active_tids[last];
    port->active_tids[last] = 0;
    return TRUE;
}

static void iocp_release_thread(DWORD tid)
{
    if (!tid)
        return;
    k32_io_completion_release_thread(tid);
    for (int i = 0; i < MAX_IOCP_PORTS; i++) {
        IOCP_PORT *port = &g_iocp_ports[i];
        if (!port->used)
            continue;
        uint64_t flags = iocp_lock(port);
        iocp_remove_active_locked(port, tid);
        iocp_unlock(port, flags);
    }
}

void k32_iocp_thread_blocking(void)
{
    win32_thread_ctx_t *ctx = find_ctx_by_pid(proc_current_pid());
    if (ctx)
        iocp_release_thread(ctx->tid);
}

static void iocp_activate_for_port(IOCP_PORT *target, DWORD tid)
{
    for (int i = 0; i < MAX_IOCP_PORTS; i++) {
        IOCP_PORT *port = &g_iocp_ports[i];
        if (!port->used || port == target)
            continue;
        uint64_t flags = iocp_lock(port);
        iocp_remove_active_locked(port, tid);
        iocp_unlock(port, flags);
    }
}

static void iocp_mark_waiting(IOCP_PORT *port, DWORD tid)
{
    uint64_t flags = iocp_lock(port);
    iocp_remove_active_locked(port, tid);
    iocp_unlock(port, flags);
}

static BOOL iocp_associate(HANDLE file, HANDLE port, ULONG_PTR key)
{
    DWORD owner_pid = win32_current_process_id();
    IOCP_ASSOCIATION *free_slot = NULL;

    uint64_t association_flags = iocp_association_lock();
    for (int i = 0; i < MAX_IOCP_ASSOCIATIONS; i++) {
        IOCP_ASSOCIATION *association = &g_iocp_associations[i];
        if (!association->used) {
            if (!free_slot)
                free_slot = association;
            continue;
        }
        if (association->owner_pid != owner_pid || association->file != file)
            continue;
        if (!association->port) {
            association->port = port;
            association->key = key;
            iocp_association_unlock(association_flags);
            SetLastError(0);
            return TRUE;
        }
        if (association->port != port) {
            iocp_association_unlock(association_flags);
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            return FALSE;
        }
        /* Windows keeps the completion key from the first association. */
        iocp_association_unlock(association_flags);
        SetLastError(0);
        return TRUE;
    }

    if (!free_slot) {
        iocp_association_unlock(association_flags);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    free_slot->used = TRUE;
    free_slot->owner_pid = owner_pid;
    free_slot->file = file;
    free_slot->port = port;
    free_slot->key = key;
    free_slot->modes = 0;
    iocp_association_unlock(association_flags);
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI SetFileCompletionNotificationModes(HANDLE file, BYTE flags)
{
    DWORD owner_pid = win32_current_process_id();
    IOCP_ASSOCIATION *free_slot = NULL;

    if (!file || file == INVALID_HANDLE_VALUE || (flags & ~3U)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    uint64_t association_flags = iocp_association_lock();
    for (int i = 0; i < MAX_IOCP_ASSOCIATIONS; i++) {
        IOCP_ASSOCIATION *association = &g_iocp_associations[i];
        if (!association->used) {
            if (!free_slot)
                free_slot = association;
            continue;
        }
        if (association->owner_pid == owner_pid && association->file == file) {
            association->modes |= flags;
            BYTE modes = association->modes;
            iocp_association_unlock(association_flags);
            iocp_trace_notification_modes(file, owner_pid, modes);
            SetLastError(0);
            return TRUE;
        }
    }

    if (!free_slot) {
        iocp_association_unlock(association_flags);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    free_slot->used = TRUE;
    free_slot->owner_pid = owner_pid;
    free_slot->file = file;
    free_slot->port = NULL;
    free_slot->key = 0;
    free_slot->modes = flags;
    iocp_association_unlock(association_flags);
    iocp_trace_notification_modes(file, owner_pid, flags);
    SetLastError(0);
    return TRUE;
}

void k32_iocp_forget_file(HANDLE file)
{
    DWORD owner_pid = win32_current_process_id();

    uint64_t association_flags = iocp_association_lock();
    for (int i = 0; i < MAX_IOCP_ASSOCIATIONS; i++) {
        IOCP_ASSOCIATION *association = &g_iocp_associations[i];
        if (association->used && association->owner_pid == owner_pid &&
            association->file == file)
            association->used = FALSE;
    }
    iocp_association_unlock(association_flags);
}

static void iocp_forget_handle(HANDLE handle)
{
    DWORD owner_pid = win32_current_process_id();
    IOCP_PORT *port = iocp_find(handle);

    uint64_t association_flags = iocp_association_lock();
    for (int i = 0; i < MAX_IOCP_ASSOCIATIONS; i++) {
        IOCP_ASSOCIATION *association = &g_iocp_associations[i];
        if (!association->used || association->owner_pid != owner_pid)
            continue;
        if (association->file == handle || association->port == handle)
            association->used = FALSE;
    }
    iocp_association_unlock(association_flags);
    if (port) {
        port->used = FALSE;
        port->owner_pid = 0;
    }
    for (int i = 0; i < MAX_PENDING_PIPE_READS; i++) {
        BYTE *write_stage = NULL;
        uint64_t pending_flags = pending_pipe_read_lock();
        K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[i];
        if (pending->active && pending->owner_pid == owner_pid &&
            pending->file == handle && !pending->completed) {
            if (pending->servicing) {
                pending->abandoned = TRUE;
                pending->cancel_requested = FALSE;
            } else {
                write_stage = pending->write_stage;
                pending->write_stage = NULL;
                pending->active = FALSE;
                pending->servicing = FALSE;
                pending->completed = FALSE;
                pending->packet_queued = FALSE;
                pending->cancel_requested = FALSE;
                pending->abandoned = FALSE;
            }
        }
        pending_pipe_read_unlock(pending_flags);
        if (write_stage)
            kfree(write_stage);
    }
}

static void iocp_reset_all(void)
{
    DWORD owner_pid = win32_current_process_id();

    uint64_t association_flags = iocp_association_lock();
    for (int i = 0; i < MAX_IOCP_ASSOCIATIONS; i++) {
        if (g_iocp_associations[i].used &&
            g_iocp_associations[i].owner_pid == owner_pid)
            g_iocp_associations[i].used = FALSE;
    }
    iocp_association_unlock(association_flags);
    for (int i = 0; i < MAX_IOCP_PORTS; i++) {
        if (g_iocp_ports[i].used && g_iocp_ports[i].owner_pid == owner_pid) {
            g_iocp_ports[i].used = FALSE;
            g_iocp_ports[i].owner_pid = 0;
            g_iocp_ports[i].lock = 0;
            kfree(g_iocp_ports[i].active_tids);
            g_iocp_ports[i].active_tids = NULL;
        }
    }
    for (int i = 0; i < MAX_PENDING_PIPE_READS; i++) {
        BYTE *write_stage = NULL;
        uint64_t pending_flags = pending_pipe_read_lock();
        K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[i];
        if (pending->active && pending->owner_pid == owner_pid) {
            if (pending->servicing) {
                pending->abandoned = TRUE;
                pending->cancel_requested = FALSE;
            } else {
                write_stage = pending->write_stage;
                pending->write_stage = NULL;
                pending->active = FALSE;
                pending->servicing = FALSE;
                pending->completed = FALSE;
                pending->packet_queued = FALSE;
                pending->cancel_requested = FALSE;
                pending->abandoned = FALSE;
            }
        }
        pending_pipe_read_unlock(pending_flags);
        if (write_stage)
            kfree(write_stage);
    }
}

#if K32_IOCP_TRACE
static BOOL iocp_api_trace_take(void)
{
    return __atomic_fetch_add(&g_iocp_api_trace_count, 1,
                              __ATOMIC_RELAXED) < K32_IOCP_TRACE_LIMIT;
}

static void iocp_api_trace_result(BOOL trace, const char *outcome,
                                  HANDLE result)
{
    if (!trace)
        return;
    serial_puts("[IOCP-API] ");
    serial_puts(outcome);
    serial_puts(" result=0x");
    serial_puthex((uint64_t)(ULONG_PTR)result, 8);
    serial_puts(" error=");
    serial_putdec(GetLastError());
    serial_puts("\n");
}
#endif

HANDLE WINAPI CreateIoCompletionPort(HANDLE hFile, HANDLE hExistingCompletionPort,
                                     ULONG_PTR completionKey,
                                     DWORD numberOfConcurrentThreads)
{
#if K32_IOCP_TRACE
    BOOL trace_iocp = iocp_api_trace_take();
    if (trace_iocp) {
        serial_puts("[IOCP-API] enter pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" tid=");
        serial_putdec(win32_current_process_thread_id());
        serial_puts(" compat32=");
        serial_putdec(g_compat32_mode ? 1 : 0);
        serial_puts(" image='");
        const char *image_name = win32_current_exe_name();
        serial_puts(image_name ? image_name : "<unknown>");
        serial_puts("' file=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hFile, 8);
        serial_puts(" port=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hExistingCompletionPort, 8);
        serial_puts(" key=0x");
        serial_puthex((uint64_t)completionKey, 16);
        serial_puts(" concurrency=");
        serial_putdec(numberOfConcurrentThreads);
        serial_puts("\n");
    }
#endif
    /* The generic PE32 gateway zero-extends DWORD arguments. Normalize the
     * Win32 -1 pseudo-handle before applying native-width HANDLE semantics. */
    if (g_compat32_mode &&
        (ULONG_PTR)hFile == (ULONG_PTR)0xFFFFFFFFULL)
        hFile = INVALID_HANDLE_VALUE;

    BOOL trace_mojo = k32_mojo_trace_current() &&
        k32_mojo_trace_take(&g_mojo_iocp_trace_count,
                            K32_MOJO_IO_TRACE_LIMIT);
    if (trace_mojo) {
        serial_puts("[MOJO-IOCP] enter pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" file=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hFile, 8);
        serial_puts(" port=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hExistingCompletionPort, 8);
        serial_puts(" key=0x");
        serial_puthex((uint64_t)completionKey, 16);
        serial_puts(" concurrency=");
        serial_putdec(numberOfConcurrentThreads);
        serial_puts("\n");
    }
#if K32_IOCP_TRACE
    if (win32_current_process_id() == 1) {
        serial_puts("[IOCP-TRACE] associate file=");
        serial_puthex((uint64_t)(ULONG_PTR)hFile, 8);
        serial_puts(" port=");
        serial_puthex((uint64_t)(ULONG_PTR)hExistingCompletionPort, 8);
        serial_puts(" key=");
        serial_puthex((uint64_t)completionKey, 16);
        serial_puts("\n");
    }
#endif

    if (hExistingCompletionPort) {
        if (!iocp_find(hExistingCompletionPort)) {
            if (trace_mojo)
                serial_puts("[MOJO-IOCP] reject unknown port\n");
            SetLastError(6); /* ERROR_INVALID_HANDLE */
#if K32_IOCP_TRACE
            iocp_api_trace_result(trace_iocp, "unknown-port", NULL);
#endif
            return NULL;
        }
        if (hFile == INVALID_HANDLE_VALUE || !hFile) {
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
#if K32_IOCP_TRACE
            iocp_api_trace_result(trace_iocp, "invalid-file", NULL);
#endif
            return NULL;
        }
        if (!iocp_associate(hFile, hExistingCompletionPort, completionKey)) {
#if K32_IOCP_TRACE
            iocp_api_trace_result(trace_iocp, "associate-failed", NULL);
#endif
            return NULL;
        }
        if (trace_mojo)
            serial_puts("[MOJO-IOCP] associated existing port\n");
#if K32_IOCP_TRACE
        iocp_api_trace_result(trace_iocp, "associated", hExistingCompletionPort);
#endif
        return hExistingCompletionPort;
    }

    if (K32_VERBOSE_DIAGNOSTICS)
        serial_puts("[K32] CreateIoCompletionPort\n");
    HANDLE handle = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!handle) {
#if K32_IOCP_TRACE
        iocp_api_trace_result(trace_iocp, "event-create-failed", NULL);
#endif
        return NULL;
    }
    IOCP_PORT *port = iocp_create(handle, numberOfConcurrentThreads);
    if (!port) {
        CloseHandle(handle);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
#if K32_IOCP_TRACE
        iocp_api_trace_result(trace_iocp, "port-table-full", NULL);
#endif
        return NULL;
    }
    if (trace_mojo) {
        serial_puts("[MOJO-IOCP] created port=0x");
        serial_puthex((uint64_t)(ULONG_PTR)handle, 8);
        serial_puts("\n");
    }
#if K32_IOCP_TRACE
    if (win32_current_process_id() == 1) {
        serial_puts("[IOCP-TRACE] create port=");
        serial_puthex((uint64_t)(ULONG_PTR)handle, 8);
        serial_puts(" concurrency=");
        serial_putdec(port->concurrency_limit);
        serial_puts("\n");
    }
#endif
    if (hFile != INVALID_HANDLE_VALUE && hFile) {
        if (!iocp_associate(hFile, handle, completionKey)) {
            CloseHandle(handle);
#if K32_IOCP_TRACE
            iocp_api_trace_result(trace_iocp, "initial-associate-failed", NULL);
#endif
            return NULL;
        }
    }
    SetLastError(0);
#if K32_IOCP_TRACE
    iocp_api_trace_result(trace_iocp, "created", handle);
#endif
    return handle;
}

static BOOL iocp_post_packet_status_ex(IOCP_PORT *port,
                                        DWORD dwNumberOfBytesTransferred,
                                        ULONG_PTR dwCompletionKey,
                                        PVOID lpOverlapped,
                                        NTSTATUS completion_status,
                                        LONG pending_pipe_slot,
                                        DWORD pending_pipe_generation)
{
#if K32_IOCP_TRACE
    if (port->owner_pid == 1 && dwNumberOfBytesTransferred == 0x233) {
        serial_puts("[IOCP-TRACE] post port=");
        serial_puthex((uint64_t)(ULONG_PTR)port->handle, 8);
        serial_puts(" bytes=");
        serial_puthex(dwNumberOfBytesTransferred, 8);
        serial_puts(" key=");
        serial_puthex((uint64_t)dwCompletionKey, 16);
        serial_puts(" ov=");
        serial_puthex((uint64_t)(uintptr_t)lpOverlapped, 16);
        serial_puts("\n");
    }
#endif

    uint64_t port_flags = iocp_lock(port);
    if (port->count == MAX_IOCP_PACKETS) {
        iocp_unlock(port, port_flags);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    IOCP_PACKET *packet = &port->packets[port->tail];
    packet->bytes = dwNumberOfBytesTransferred;
    packet->key = dwCompletionKey;
    packet->overlapped = lpOverlapped;
    packet->completion_status = completion_status;
    packet->pending_pipe_slot = pending_pipe_slot;
    packet->pending_pipe_generation = pending_pipe_generation;

    /* Publish the tracked slot while the port is still locked. A dequeuer
     * cannot observe this packet until packet_queued is visible, and another
     * producer cannot overtake it on the same completion port. */
    if (pending_pipe_slot >= 0) {
        if (pending_pipe_slot >= MAX_PENDING_PIPE_READS) {
            iocp_unlock(port, port_flags);
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            return FALSE;
        }
        uint64_t pending_flags = pending_pipe_read_lock();
        K32_PENDING_PIPE_READ *pending =
            &g_pending_pipe_reads[pending_pipe_slot];
        if (!pending->active || !pending->completed ||
            pending->packet_queued ||
            pending->generation != pending_pipe_generation ||
            pending->overlapped != lpOverlapped) {
            pending_pipe_read_unlock(pending_flags);
            iocp_unlock(port, port_flags);
            SetLastError(1168); /* ERROR_NOT_FOUND */
            return FALSE;
        }
        pending->packet_queued = TRUE;
        pending_pipe_read_unlock(pending_flags);
    }

    port->tail = (port->tail + 1) % MAX_IOCP_PACKETS;
    port->count++;
    extern NTSTATUS ntsync_set_event_for_process(HANDLE, ULONG, LONG *);
    (void)ntsync_set_event_for_process(port->handle, port->owner_pid, NULL);
    iocp_unlock(port, port_flags);
    return TRUE;
}

static BOOL iocp_post_packet_ex(IOCP_PORT *port,
                                 DWORD dwNumberOfBytesTransferred,
                                 ULONG_PTR dwCompletionKey,
                                 PVOID lpOverlapped,
                                 LONG pending_pipe_slot,
                                 DWORD pending_pipe_generation)
{
    return iocp_post_packet_status_ex(
        port, dwNumberOfBytesTransferred, dwCompletionKey, lpOverlapped,
        STATUS_SUCCESS, pending_pipe_slot, pending_pipe_generation);
}

static BOOL iocp_post_packet(IOCP_PORT *port,
                             DWORD dwNumberOfBytesTransferred,
                             ULONG_PTR dwCompletionKey,
                             PVOID lpOverlapped)
{
    return iocp_post_packet_ex(port, dwNumberOfBytesTransferred,
                               dwCompletionKey, lpOverlapped, -1, 0);
}

BOOL WINAPI PostQueuedCompletionStatus(HANDLE hCompletionPort,
                                       DWORD dwNumberOfBytesTransferred,
                                       ULONG_PTR dwCompletionKey,
                                       PVOID lpOverlapped)
{
    IOCP_PORT *port = iocp_find(hCompletionPort);
    if (!port) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    return iocp_post_packet(port, dwNumberOfBytesTransferred,
                            dwCompletionKey, lpOverlapped);
}

static BOOL iocp_try_dequeue(IOCP_PORT *port, DWORD tid,
                             IOCP_PACKET *packet,
                             BOOL *concurrency_blocked)
{
    BOOL found = FALSE;

    if (concurrency_blocked)
        *concurrency_blocked = FALSE;

    uint64_t port_flags = iocp_lock(port);
    if (port->count) {
        int active_index = iocp_active_index_locked(port, tid);
        if (active_index < 0 &&
            port->active_count >= port->concurrency_limit) {
            if (concurrency_blocked)
                *concurrency_blocked = TRUE;
        } else {
            if (active_index < 0)
                port->active_tids[port->active_count++] = tid;
            *packet = port->packets[port->head];
            port->head = (port->head + 1) % MAX_IOCP_PACKETS;
            port->count--;
            if (!port->count)
                ResetEvent(port->handle);
            found = TRUE;
        }
    } else {
        /* Readiness probes may wake a port before the servicing thread has
         * queued a completion packet.  Do not leave the manual-reset port
         * event signaled with an empty queue: GQCS would otherwise spin and
         * starve the process on a single CPU.  Posting is serialized by the
         * same lock, so resetting here cannot discard a queued completion. */
        ResetEvent(port->handle);
    }
    iocp_unlock(port, port_flags);
    if (found && k32_mojo_trace_current() &&
        k32_mojo_trace_take(&g_mojo_dequeue_trace_count,
                            K32_MOJO_DEQUEUE_TRACE_LIMIT)) {
        serial_puts("[MOJO-DEQUEUE] pid=");
        serial_putdec(port->owner_pid);
        serial_puts(" port=0x");
        serial_puthex((uint64_t)(ULONG_PTR)port->handle, 8);
        serial_puts(" bytes=");
        serial_putdec(packet->bytes);
        serial_puts(" key=0x");
        serial_puthex((uint64_t)packet->key, 16);
        serial_puts(" ov=0x");
        serial_puthex((uint64_t)(ULONG_PTR)packet->overlapped, 16);
        serial_puts(" slot=");
        serial_putdec((uint64_t)(int64_t)packet->pending_pipe_slot);
        serial_puts(" gen=");
        serial_putdec(packet->pending_pipe_generation);
        serial_puts("\n");
    }
#if K32_IOCP_TRACE
    if (found && port->owner_pid == 1 && packet->bytes == 0x233) {
        serial_puts("[IOCP-TRACE] dequeue port=");
        serial_puthex((uint64_t)(ULONG_PTR)port->handle, 8);
        serial_puts(" bytes=");
        serial_puthex(packet->bytes, 8);
        serial_puts(" key=");
        serial_puthex((uint64_t)packet->key, 16);
        serial_puts(" ov=");
        serial_puthex((uint64_t)(uintptr_t)packet->overlapped, 16);
        serial_puts("\n");
    }
#endif
    return found;
}

static void iocp_query_association(HANDLE file, DWORD owner_pid,
                                   HANDLE *port, ULONG_PTR *key, BYTE *modes)
{
    *port = NULL;
    *key = 0;
    *modes = 0;
    uint64_t association_flags = iocp_association_lock();
    for (int i = 0; i < MAX_IOCP_ASSOCIATIONS; i++) {
        IOCP_ASSOCIATION *association = &g_iocp_associations[i];
        if (association->used && association->owner_pid == owner_pid &&
            association->file == file) {
            *port = association->port;
            *key = association->key;
            *modes = association->modes;
            break;
        }
    }
    iocp_association_unlock(association_flags);
}

BOOL k32_iocp_wake_handle_for_owner(HANDLE file, DWORD owner_pid)
{
    HANDLE port = NULL;
    ULONG_PTR key = 0;
    BYTE modes = 0;

    iocp_query_association(file, owner_pid, &port, &key, &modes);

    (void)key;
    (void)modes;
    if (!port)
        return FALSE;

    IOCP_PORT *target = iocp_find_for_process(port, owner_pid);
    if (!target)
        return FALSE;

    extern NTSTATUS ntsync_set_event_for_process(HANDLE, ULONG, LONG *);
    return NT_SUCCESS(ntsync_set_event_for_process(target->handle, owner_pid,
                                                    NULL));
}

static BOOL k32_iocp_complete_handle_status_for_process(
    HANDLE file, DWORD bytes, PVOID overlapped, DWORD owner_pid,
    BOOL compat32, NTSTATUS completion_status, BOOL completed_async)
{
    HANDLE event = NULL;
    HANDLE port = NULL;
    ULONG_PTR key = 0;
    BYTE modes = 0;

    if (!overlapped)
        return FALSE;

    if (compat32) {
        volatile uint32_t *values = (volatile uint32_t *)overlapped;
        values[0] = (uint32_t)completion_status;
        values[1] = bytes;
        event = (HANDLE)(ULONG_PTR)values[4];
    } else {
        volatile ULONG_PTR *values = (volatile ULONG_PTR *)overlapped;
        values[0] = (ULONG_PTR)completion_status;
        values[1] = bytes;
        event = (HANDLE)values[3];
    }

    iocp_query_association(file, owner_pid, &port, &key, &modes);

#if K32_IOCP_TRACE
    if (owner_pid == 1 && bytes == 0x233) {
        serial_puts("[IOCP-TRACE] complete file=");
        serial_puthex((uint64_t)(ULONG_PTR)file, 8);
        serial_puts(" port=");
        serial_puthex((uint64_t)(ULONG_PTR)port, 8);
        serial_puts(" modes=");
        serial_puthex(modes, 2);
        serial_puts(" key=");
        serial_puthex((uint64_t)key, 16);
        serial_puts(" ov=");
        serial_puthex((uint64_t)(uintptr_t)overlapped, 16);
        serial_puts("\n");
    }
#endif

    extern NTSTATUS ntsync_set_event_for_process(HANDLE, ULONG, LONG *);
    ULONG_PTR event_value = (ULONG_PTR)event;
    if (event_value & 1U) {
        event_value &= ~(ULONG_PTR)1U;
        if (event_value)
            ntsync_set_event_for_process((HANDLE)event_value, owner_pid, NULL);
        return TRUE;
    }
    if (event)
        ntsync_set_event_for_process(event, owner_pid, NULL);
    /* FILE_SKIP_COMPLETION_PORT_ON_SUCCESS only applies when the operation
     * returned success synchronously. An operation that returned pending must
     * still enqueue its eventual completion packet. */
    if (!completed_async && NT_SUCCESS(completion_status) && (modes & 1U))
        return TRUE;
    if (port) {
        IOCP_PORT *target = iocp_find_for_process(port, owner_pid);
        if (!target)
            return FALSE;
        return iocp_post_packet_status_ex(
            target, bytes, key, overlapped, completion_status, -1, 0);
    }
    return TRUE;
}

BOOL k32_iocp_complete_handle(HANDLE file, DWORD bytes, PVOID overlapped)
{
    return k32_iocp_complete_handle_status_for_process(
        file, bytes, overlapped, win32_current_process_id(),
        g_compat32_mode, STATUS_SUCCESS, FALSE);
}

BOOL k32_iocp_complete_handle_for_owner(HANDLE file, DWORD bytes,
                                         PVOID overlapped, DWORD owner_pid,
                                         BOOL compat32)
{
    return k32_iocp_complete_handle_status_for_process(
        file, bytes, overlapped, owner_pid, compat32, STATUS_SUCCESS, TRUE);
}

BOOL k32_iocp_complete_handle_status_for_owner(
    HANDLE file, DWORD bytes, PVOID overlapped, DWORD owner_pid,
    BOOL compat32, NTSTATUS completion_status)
{
    return k32_iocp_complete_handle_status_for_process(
        file, bytes, overlapped, owner_pid, compat32, completion_status, TRUE);
}

static void k32_store_overlapped_status(PVOID overlapped, BOOL compat32,
                                        NTSTATUS status, ULONG_PTR bytes)
{
    if (compat32) {
        volatile uint32_t *values = (volatile uint32_t *)overlapped;
        values[0] = (uint32_t)status;
        values[1] = (uint32_t)bytes;
    } else {
        volatile ULONG_PTR *values = (volatile ULONG_PTR *)overlapped;
        values[0] = (ULONG_PTR)status;
        values[1] = bytes;
    }
}

static BOOL k32_copy_to_process(DWORD owner_pid, PVOID destination,
                                PCVOID source, SIZE_T size)
{
    extern BOOL win32_process_cr3(DWORD process_id, uint64_t *out_cr3);
    if (!size)
        return TRUE;
    if (!destination || !source)
        return FALSE;

    uint64_t destination_cr3;
    uint64_t source_cr3;
    uint64_t copied = 0;
    if (!win32_process_cr3(owner_pid, &destination_cr3))
        return FALSE;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(source_cr3));
    return paging_copy_between_cr3(
               destination_cr3, (uint64_t)(ULONG_PTR)destination,
               source_cr3, (uint64_t)(ULONG_PTR)source, size, &copied) == 0 &&
           copied == size;
}

static BOOL k32_store_overlapped_status_for_process(
    DWORD owner_pid, PVOID overlapped, BOOL compat32,
    NTSTATUS status, ULONG_PTR bytes)
{
    if (compat32) {
        uint32_t values[2] = { (uint32_t)status, (uint32_t)bytes };
        return k32_copy_to_process(owner_pid, overlapped, values,
                                   sizeof(values));
    }

    ULONG_PTR values[2] = { (ULONG_PTR)status, bytes };
    return k32_copy_to_process(owner_pid, overlapped, values, sizeof(values));
}

/* Complete a slot while servicing remains asserted. The data buffer and the
 * OVERLAPPED fields become visible in the owner's CR3 before completed is
 * published and before any event or IOCP port is signaled. */
static BOOL k32_pipe_complete_claimed(int slot, DWORD generation,
                                      NTSTATUS *completion_status,
                                      DWORD *completion_bytes)
{
    DWORD owner_pid;
    BOOL compat32;
    BOOL write;
    PVOID buffer;
    PVOID overlapped;
    DWORD length;
    BYTE *stage;
    BYTE *retired_write_stage = NULL;

    uint64_t pending_flags = pending_pipe_read_lock();
    K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[slot];
    if (!pending->active || !pending->servicing || pending->completed ||
        pending->generation != generation) {
        pending_pipe_read_unlock(pending_flags);
        return FALSE;
    }
    if (pending->abandoned) {
        retired_write_stage = pending->write_stage;
        pending->write_stage = NULL;
        pending->active = FALSE;
        pending->servicing = FALSE;
        pending->completed = FALSE;
        pending->packet_queued = FALSE;
        pending->cancel_requested = FALSE;
        pending->abandoned = FALSE;
        pending_pipe_read_unlock(pending_flags);
        if (retired_write_stage)
            kfree(retired_write_stage);
        return FALSE;
    }

    owner_pid = pending->owner_pid;
    compat32 = pending->compat32;
    write = pending->write;
    buffer = pending->buffer;
    overlapped = pending->overlapped;
    length = pending->length;
    stage = pending->stage;
    pending_pipe_read_unlock(pending_flags);

    NTSTATUS status = *completion_status;
    DWORD bytes = *completion_bytes;
    if (!write && NT_SUCCESS(status) && bytes) {
        if (bytes > length ||
            !k32_copy_to_process(owner_pid, buffer, stage, bytes)) {
            status = STATUS_ACCESS_VIOLATION;
            bytes = 0;
        }
    }
    if (!k32_store_overlapped_status_for_process(
            owner_pid, overlapped, compat32, status, bytes)) {
        status = STATUS_ACCESS_VIOLATION;
        bytes = 0;
    }

    pending_flags = pending_pipe_read_lock();
    pending = &g_pending_pipe_reads[slot];
    if (!pending->active || !pending->servicing || pending->completed ||
        pending->generation != generation) {
        pending_pipe_read_unlock(pending_flags);
        return FALSE;
    }
    if (pending->abandoned) {
        retired_write_stage = pending->write_stage;
        pending->write_stage = NULL;
        pending->active = FALSE;
        pending->servicing = FALSE;
        pending->completed = FALSE;
        pending->packet_queued = FALSE;
        pending->cancel_requested = FALSE;
        pending->abandoned = FALSE;
        pending_pipe_read_unlock(pending_flags);
        if (retired_write_stage)
            kfree(retired_write_stage);
        return FALSE;
    }

    pending->completion_status = status;
    pending->completion_bytes = bytes;
    pending->completed = TRUE;
    pending->servicing = FALSE;
    pending->cancel_requested = FALSE;
    pending_pipe_read_unlock(pending_flags);

    *completion_status = status;
    *completion_bytes = bytes;
    return TRUE;
}

static BOOL k32_pipe_retire_completed(int slot, DWORD generation)
{
    BYTE *write_stage = NULL;
    uint64_t pending_flags = pending_pipe_read_lock();
    K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[slot];
    if (!pending->active || !pending->completed || pending->packet_queued ||
        pending->generation != generation) {
        pending_pipe_read_unlock(pending_flags);
        return FALSE;
    }

    write_stage = pending->write_stage;
    pending->write_stage = NULL;
    pending->active = FALSE;
    pending->servicing = FALSE;
    pending->completed = FALSE;
    pending->packet_queued = FALSE;
    pending->cancel_requested = FALSE;
    pending->abandoned = FALSE;
    pending_pipe_read_unlock(pending_flags);

    if (write_stage)
        kfree(write_stage);
    return TRUE;
}

/* Publish a completed pipe operation exactly once. Operations without an
 * IOCP packet are retired after their event is signaled; tracked operations
 * remain alive until the packet is dequeued. */
static BOOL k32_pipe_publish_completed(int slot, DWORD generation)
{
    HANDLE file;
    HANDLE event;
    PVOID overlapped;
    DWORD owner_pid;
    DWORD bytes;
    NTSTATUS status;
    BOOL trace_mojo;

    uint64_t pending_flags = pending_pipe_read_lock();
    K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[slot];
    if (!pending->active || !pending->completed || pending->packet_queued ||
        pending->servicing || pending->generation != generation ||
        pending_pipe_has_older_locked(slot)) {
        pending_pipe_read_unlock(pending_flags);
        return FALSE;
    }
    pending->servicing = TRUE;
    file = pending->file;
    event = pending->event;
    overlapped = pending->overlapped;
    owner_pid = pending->owner_pid;
    bytes = pending->completion_bytes;
    status = pending->completion_status;
    trace_mojo = k32_mojo_trace_owner(owner_pid);
    pending_pipe_read_unlock(pending_flags);

    HANDLE port = NULL;
    ULONG_PTR key = 0;
    BYTE modes = 0;
    iocp_query_association(file, owner_pid, &port, &key, &modes);

    extern NTSTATUS ntsync_set_event_for_process(HANDLE, ULONG, LONG *);
    ULONG_PTR event_value = (ULONG_PTR)event;
    if (event_value & 1U) {
        event_value &= ~(ULONG_PTR)1U;
        if (event_value)
            (void)ntsync_set_event_for_process((HANDLE)event_value,
                                               owner_pid, NULL);
        return k32_pipe_retire_completed(slot, generation);
    }
    if (event)
        (void)ntsync_set_event_for_process(event, owner_pid, NULL);

    IOCP_PORT *target = port ? iocp_find_for_process(port, owner_pid) : NULL;
    if (!target)
        return k32_pipe_retire_completed(slot, generation);

    BOOL posted = iocp_post_packet_status_ex(
        target, bytes, key, overlapped, status, slot, generation);

    pending_flags = pending_pipe_read_lock();
    pending = &g_pending_pipe_reads[slot];
    if (pending->active && pending->generation == generation)
        pending->servicing = FALSE;
    pending_pipe_read_unlock(pending_flags);

    if (trace_mojo &&
        k32_mojo_trace_take(&g_mojo_xfer_trace_count,
                            K32_MOJO_XFER_TRACE_LIMIT)) {
        serial_puts("[MOJO-POST] pid=");
        serial_putdec(owner_pid);
        serial_puts(" port=0x");
        serial_puthex((uint64_t)(ULONG_PTR)port, 8);
        serial_puts(" file=0x");
        serial_puthex((uint64_t)(ULONG_PTR)file, 8);
        serial_puts(" ov=0x");
        serial_puthex((uint64_t)(ULONG_PTR)overlapped, 16);
        serial_puts(" posted=");
        serial_putdec(posted);
        serial_puts(" modes=0x");
        serial_puthex(modes, 2);
        serial_puts("\n");
    }
    return posted;
}

static BOOL k32_pipe_finalize_packet(IOCP_PACKET *packet,
                                     NTSTATUS *completion_status)
{
    LONG slot = packet->pending_pipe_slot;
    if (slot < 0 || slot >= MAX_PENDING_PIPE_READS)
        return FALSE;

    DWORD owner_pid = win32_current_process_id();
    uint64_t pending_flags = pending_pipe_read_lock();
    K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[slot];
    if (!pending->active || !pending->completed || !pending->packet_queued ||
        pending->owner_pid != owner_pid ||
        pending->generation != packet->pending_pipe_generation ||
        pending->overlapped != packet->overlapped) {
        pending_pipe_read_unlock(pending_flags);
        return FALSE;
    }

    NTSTATUS status = pending->completion_status;
    DWORD bytes = pending->completion_bytes;
    BOOL trace_mojo = k32_mojo_trace_owner(owner_pid);
    BOOL write = pending->write;
    HANDLE file = pending->file;
    BYTE *write_stage = pending->write_stage;
    pending->active = FALSE;
    pending->servicing = FALSE;
    pending->completed = FALSE;
    pending->packet_queued = FALSE;
    pending->write_stage = NULL;
    pending->cancel_requested = FALSE;
    pending->abandoned = FALSE;
    pending_pipe_read_unlock(pending_flags);

    if (trace_mojo &&
        k32_mojo_trace_take(&g_mojo_dequeue_trace_count,
                            K32_MOJO_DEQUEUE_TRACE_LIMIT)) {
        serial_puts("[MOJO-FINALIZE] pid=");
        serial_putdec(owner_pid);
        serial_puts(write ? " op=W" : " op=R");
        serial_puts(" file=0x");
        serial_puthex((uint64_t)(ULONG_PTR)file, 8);
        serial_puts(" bytes=");
        serial_putdec(bytes);
        serial_puts(" status=0x");
        serial_puthex((uint32_t)status, 8);
        serial_puts("\n");
    }

    if (write_stage)
        kfree(write_stage);

    packet->bytes = bytes;
    packet->pending_pipe_slot = -1;
    packet->pending_pipe_generation = 0;
    *completion_status = status;
    return TRUE;
}

void k32_pipe_service_pending(void)
{
    extern NTSTATUS nt_pipe_try_read(HANDLE, PVOID, ULONG, ULONG,
                                     PIO_STATUS_BLOCK);
    extern NTSTATUS nt_pipe_try_write(HANDLE, PCVOID, ULONG, ULONG,
                                      PIO_STATUS_BLOCK);

    /* A read can free space for a write, and that write can satisfy another
     * read. A few bounded passes drain the common pipe hand-off without
     * recursing through completion callbacks. */
    for (int pass = 0; pass < 4; pass++) {
        BOOL made_progress = FALSE;
        for (int i = 0; i < MAX_PENDING_PIPE_READS; i++) {
            HANDLE file = NULL;
            PVOID overlapped = NULL;
            PVOID io_buffer = NULL;
            DWORD owner_pid = 0;
            DWORD owner_tid = 0;
            DWORD generation = 0;
            DWORD length = 0;
            DWORD progress = 0;
            BOOL write = FALSE;
            BOOL trace_mojo = FALSE;
            BOOL claimed = FALSE;
            BOOL publish_completed = FALSE;
            uint64_t pending_flags;

            pending_flags = pending_pipe_read_lock();
            K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[i];
            if (pending->active && pending->completed &&
                !pending->packet_queued && !pending->servicing) {
                generation = pending->generation;
                publish_completed = TRUE;
            } else if (pending->active && !pending->servicing &&
                       !pending->completed &&
                       !pending_pipe_has_older_locked(i)) {
                pending->servicing = TRUE;
                file = pending->file;
                overlapped = pending->overlapped;
                owner_pid = pending->owner_pid;
                owner_tid = pending->owner_tid;
                generation = pending->generation;
                length = pending->length;
                progress = pending->progress;
                write = pending->write;
                trace_mojo = k32_mojo_trace_owner(owner_pid);
                io_buffer = write ? pending->write_stage : pending->stage;
                claimed = TRUE;
            }
            pending_pipe_read_unlock(pending_flags);
            if (publish_completed) {
                if (k32_pipe_publish_completed(i, generation))
                    made_progress = TRUE;
                continue;
            }
            if (!claimed)
                continue;

            IO_STATUS_BLOCK iosb = { STATUS_PENDING, 0 };
            NTSTATUS status;
            if (write) {
                if (!io_buffer) {
                    status = STATUS_INSUFFICIENT_RESOURCES;
                } else if (progress >= length) {
                    status = STATUS_SUCCESS;
                } else {
                    status = nt_pipe_try_write(
                        file, (const BYTE *)io_buffer + progress,
                        length - progress, owner_pid, &iosb);
                }
            } else {
                DWORD read_length = length;
                if (read_length > K32_PIPE_STAGE_SIZE)
                    read_length = K32_PIPE_STAGE_SIZE;
                status = nt_pipe_try_read(file, io_buffer, read_length,
                                          owner_pid, &iosb);
            }

            DWORD bytes = 0;
            pending_flags = pending_pipe_read_lock();
            pending = &g_pending_pipe_reads[i];
            if (!pending->active || pending->generation != generation) {
                pending_pipe_read_unlock(pending_flags);
                continue;
            }
            if (status == STATUS_PENDING) {
                if (write && iosb.Information) {
                    pending->progress += (DWORD)iosb.Information;
                    made_progress = TRUE;
                }
                if (!pending->cancel_requested && !pending->abandoned) {
                    pending->servicing = FALSE;
                    pending_pipe_read_unlock(pending_flags);
                    continue;
                }
                status = STATUS_CANCELLED;
                bytes = pending->progress;
            } else if (write && NT_SUCCESS(status)) {
                pending->progress += (DWORD)iosb.Information;
                bytes = pending->progress;
            } else if (!write && NT_SUCCESS(status)) {
                bytes = (DWORD)iosb.Information;
            }
            pending_pipe_read_unlock(pending_flags);

            if (!k32_pipe_complete_claimed(i, generation, &status, &bytes))
                continue;
            made_progress = TRUE;

            k32_mojo_ring_record(owner_pid, owner_tid, file, overlapped,
                                 write ? K32_MOJO_RING_WRITE
                                       : K32_MOJO_RING_READ,
                                 status, length, bytes, io_buffer,
                                 write ? length : bytes);

            if (trace_mojo &&
                k32_mojo_trace_take(&g_mojo_xfer_trace_count,
                                    K32_MOJO_XFER_TRACE_LIMIT)) {
                serial_puts("[MOJO-XFER] pid=");
                serial_putdec(owner_pid);
                serial_puts(write ? " op=W" : " op=R");
                serial_puts(" file=0x");
                serial_puthex((uint64_t)(ULONG_PTR)file, 8);
                serial_puts(" bytes=");
                serial_putdec(bytes);
                serial_puts(" status=0x");
                serial_puthex((uint32_t)status, 8);
                serial_puts(" data=");
                k32_mojo_trace_bytes(io_buffer, bytes);
                serial_puts("\n");
            }
            (void)k32_pipe_publish_completed(i, generation);
        }
        if (!made_progress)
            break;
    }
}

static BOOL k32_cancel_pipe_io(HANDLE file, PVOID target_overlapped,
                               BOOL current_thread_only)
{
    DWORD owner_pid = win32_current_process_id();
    DWORD owner_tid = GetCurrentThreadId();
    BOOL found = FALSE;

    for (int i = 0; i < MAX_PENDING_PIPE_READS; i++) {
        DWORD generation = 0;

        uint64_t pending_flags = pending_pipe_read_lock();
        K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[i];
        if (!pending->active || pending->completed ||
            pending->owner_pid != owner_pid || pending->file != file ||
            (target_overlapped &&
             pending->overlapped != target_overlapped) ||
            (current_thread_only && pending->owner_tid != owner_tid)) {
            pending_pipe_read_unlock(pending_flags);
            continue;
        }

        found = TRUE;
        if (pending->servicing) {
            /* The service routine owns stage/write_stage until it returns.
             * Let it publish cancellation only if the underlying try-I/O did
             * not already win the cancellation race. */
            pending->cancel_requested = TRUE;
            pending_pipe_read_unlock(pending_flags);
            continue;
        }

        pending->servicing = TRUE;
        pending->cancel_requested = FALSE;
        generation = pending->generation;
        pending_pipe_read_unlock(pending_flags);

        NTSTATUS status = STATUS_CANCELLED;
        DWORD bytes = 0;
        if (!k32_pipe_complete_claimed(i, generation, &status, &bytes))
            continue;
        (void)k32_pipe_publish_completed(i, generation);
    }
    return found;
}

static void k32_pipe_wait_quiescent(HANDLE file)
{
    DWORD owner_pid = win32_current_process_id();
    for (;;) {
        k32_pipe_service_pending();

        BOOL servicing = FALSE;
        uint64_t pending_flags = pending_pipe_read_lock();
        for (int i = 0; i < MAX_PENDING_PIPE_READS; i++) {
            K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[i];
            if (pending->active && pending->servicing &&
                pending->owner_pid == owner_pid && pending->file == file) {
                servicing = TRUE;
                break;
            }
        }
        pending_pipe_read_unlock(pending_flags);
        if (!servicing)
            return;
        sched_yield();
    }
}

static BOOL WINAPI CancelIo_k32(HANDLE file)
{
    if (!file || file == INVALID_HANDLE_VALUE) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    k32_cancel_pipe_io(file, NULL, TRUE);
    wsock_cancel_io((SOCKET)(ULONG_PTR)file, NULL, TRUE);
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI CancelIoEx_k32(HANDLE file, PVOID overlapped)
{
    if (!file || file == INVALID_HANDLE_VALUE) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    BOOL pipe_cancelled = k32_cancel_pipe_io(file, overlapped, FALSE);
    BOOL socket_cancelled = wsock_cancel_io(
        (SOCKET)(ULONG_PTR)file, overlapped, FALSE);
    if (!pipe_cancelled && !socket_cancelled) {
        SetLastError(1168); /* ERROR_NOT_FOUND */
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL k32_pipe_read_overlapped(HANDLE file, PVOID buffer, DWORD length,
                                     DWORD *bytes_read, PVOID overlapped)
{
    if (!overlapped || (!buffer && length)) {
        if (bytes_read)
            *bytes_read = 0;
        if (overlapped)
            k32_store_overlapped_status(overlapped, g_compat32_mode,
                                        STATUS_INVALID_PARAMETER, 0);
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD owner_pid = win32_current_process_id();
    DWORD owner_tid = GetCurrentThreadId();
    PVOID stream_identity = nt_pipe_stream_identity(file, owner_pid, FALSE);
    if (!stream_identity) {
        if (bytes_read)
            *bytes_read = 0;
        k32_store_overlapped_status(overlapped, g_compat32_mode,
                                    STATUS_INVALID_HANDLE, 0);
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    /* Queue even immediately satisfiable reads.  This gives all overlapped
     * reads one ordering point, so a later read cannot consume bytes ahead of
     * an earlier pending read on the same pipe stream. */
    int free_slot = -1;
    HANDLE event;
    if (g_compat32_mode) {
        volatile uint32_t *values = (volatile uint32_t *)overlapped;
        event = (HANDLE)(ULONG_PTR)values[4];
    } else {
        volatile ULONG_PTR *values = (volatile ULONG_PTR *)overlapped;
        event = (HANDLE)values[3];
    }
    uint64_t pending_flags = pending_pipe_read_lock();
    for (int i = 0; i < MAX_PENDING_PIPE_READS; i++) {
        if (!g_pending_pipe_reads[i].active) {
            free_slot = i;
            break;
        }
    }
    if (free_slot >= 0) {
        K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[free_slot];
        pending->active = TRUE;
        pending->servicing = FALSE;
        pending->completed = FALSE;
        pending->packet_queued = FALSE;
        pending->compat32 = g_compat32_mode;
        pending->write = FALSE;
        pending->owner_pid = owner_pid;
        pending->owner_tid = owner_tid;
        pending->generation = ++g_pending_pipe_generation;
        if (!pending->generation)
            pending->generation = ++g_pending_pipe_generation;
        pending->issue_sequence = pending_pipe_next_sequence_locked();
        pending->file = file;
        pending->stream_identity = stream_identity;
        pending->event = event;
        pending->buffer = buffer;
        pending->length = length;
        pending->progress = 0;
        pending->overlapped = overlapped;
        pending->completion_status = STATUS_PENDING;
        pending->completion_bytes = 0;
        pending->write_stage = NULL;
        pending->cancel_requested = FALSE;
        pending->abandoned = FALSE;
    }
    pending_pipe_read_unlock(pending_flags);

    if (free_slot < 0) {
        k32_store_overlapped_status(overlapped, g_compat32_mode,
                                    STATUS_INSUFFICIENT_RESOURCES, 0);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    if (bytes_read)
        *bytes_read = 0;
    k32_store_overlapped_status(overlapped, g_compat32_mode,
                                STATUS_PENDING, 0);
    k32_pipe_service_pending();
    SetLastError(997); /* ERROR_IO_PENDING */
    return FALSE;
}

static BOOL k32_pipe_write_overlapped(HANDLE file, PCVOID buffer, DWORD length,
                                      DWORD *bytes_written, PVOID overlapped)
{
    (void)k32_mojo_trace_current();
    if (!overlapped || (!buffer && length)) {
        if (bytes_written)
            *bytes_written = 0;
        if (overlapped)
            k32_store_overlapped_status(overlapped, g_compat32_mode,
                                        STATUS_INVALID_PARAMETER, 0);
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (!length) {
        if (bytes_written)
            *bytes_written = 0;
        k32_store_overlapped_status(overlapped, g_compat32_mode,
                                    STATUS_SUCCESS, 0);
        k32_iocp_complete_handle(file, 0, overlapped);
        SetLastError(0);
        return TRUE;
    }

    BYTE *write_stage = (BYTE *)kmalloc(length);
    if (!write_stage) {
        if (bytes_written)
            *bytes_written = 0;
        k32_store_overlapped_status(overlapped, g_compat32_mode,
                                    STATUS_INSUFFICIENT_RESOURCES, 0);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    const BYTE *src = (const BYTE *)buffer;
    for (DWORD i = 0; i < length; i++)
        write_stage[i] = src[i];

    HANDLE event;
    if (g_compat32_mode) {
        volatile uint32_t *values = (volatile uint32_t *)overlapped;
        event = (HANDLE)(ULONG_PTR)values[4];
    } else {
        volatile ULONG_PTR *values = (volatile ULONG_PTR *)overlapped;
        event = (HANDLE)values[3];
    }

    DWORD owner_pid = win32_current_process_id();
    DWORD owner_tid = GetCurrentThreadId();
    PVOID stream_identity = nt_pipe_stream_identity(file, owner_pid, TRUE);
    int free_slot = -1;
    uint64_t pending_flags = pending_pipe_read_lock();
    for (int i = 0; i < MAX_PENDING_PIPE_READS; i++) {
        if (!g_pending_pipe_reads[i].active) {
            free_slot = i;
            break;
        }
    }
    if (free_slot >= 0) {
        K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[free_slot];
        pending->active = TRUE;
        pending->servicing = FALSE;
        pending->completed = FALSE;
        pending->packet_queued = FALSE;
        pending->compat32 = g_compat32_mode;
        pending->write = TRUE;
        pending->owner_pid = owner_pid;
        pending->owner_tid = owner_tid;
        pending->generation = ++g_pending_pipe_generation;
        if (!pending->generation)
            pending->generation = ++g_pending_pipe_generation;
        pending->issue_sequence = pending_pipe_next_sequence_locked();
        pending->file = file;
        pending->stream_identity = stream_identity;
        pending->event = event;
        pending->buffer = NULL;
        pending->length = length;
        pending->progress = 0;
        pending->overlapped = overlapped;
        pending->completion_status = STATUS_PENDING;
        pending->completion_bytes = 0;
        pending->write_stage = write_stage;
        pending->cancel_requested = FALSE;
        pending->abandoned = FALSE;
    }
    pending_pipe_read_unlock(pending_flags);

    if (free_slot < 0) {
        kfree(write_stage);
        if (bytes_written)
            *bytes_written = 0;
        k32_store_overlapped_status(overlapped, g_compat32_mode,
                                    STATUS_INSUFFICIENT_RESOURCES, 0);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    if (bytes_written)
        *bytes_written = 0;
    k32_store_overlapped_status(overlapped, g_compat32_mode,
                                STATUS_PENDING, 0);
    k32_pipe_service_pending();
    SetLastError(997); /* ERROR_IO_PENDING */
    return FALSE;
}

static BOOL iocp_wait_packet(HANDLE hCompletionPort, IOCP_PORT *port,
                             DWORD tid, IOCP_PACKET *packet,
                             DWORD dwMilliseconds)
{
    iocp_activate_for_port(port, tid);

    uint64_t start = idt_get_ticks();
    DWORD remaining = dwMilliseconds;
    BOOL deadline_reached = FALSE;
    for (;;) {
        wsock_service_pending_io();

        BOOL concurrency_blocked = FALSE;
        if (iocp_try_dequeue(port, tid, packet,
                             &concurrency_blocked)) {
#if K32_IOCP_TRACE
            if (win32_current_process_id() == 1 && packet->bytes == 0x233) {
                serial_puts("[IOCP-TRACE] GQCS caller=");
                serial_puthex((uint64_t)(uintptr_t)
                              __builtin_return_address(0), 16);
                serial_puts(" tid=");
                serial_puthex(GetCurrentThreadId(), 8);
                serial_puts(" ov=");
                serial_puthex((uint64_t)(uintptr_t)packet->overlapped, 16);
                serial_puts("\n");
            }
#endif
            NTSTATUS completion_status = packet->completion_status;
            if (packet->pending_pipe_slot >= 0 &&
                !k32_pipe_finalize_packet(packet, &completion_status)) {
                /* Closing/reusing a handle can leave an old packet in the
                 * port.  Windows never exposes that stale OVERLAPPED to the
                 * new operation, so discard it and keep waiting. */
                continue;
            }
            packet->completion_status = completion_status;
            return TRUE;
        }

        if (deadline_reached || dwMilliseconds == 0) {
            SetLastError(258); /* WAIT_TIMEOUT */
            return FALSE;
        }

        /* A worker remains active while it handles a packet. It gives up its
         * concurrency slot only when it is about to block in GQCS again. */
        iocp_mark_waiting(port, tid);
        if (concurrency_blocked)
            sched_yield();

        /* Pending socket operations still need periodic service, but a 10 ms
         * slice wakes every idle IOCP worker 100 times per second. Packets
         * posted by another thread signal the port event immediately. */
        DWORD wait_slice = remaining;
        if (wait_slice == INFINITE || wait_slice > 100)
            wait_slice = 100;
        DWORD wait = WaitForSingleObject(hCompletionPort, wait_slice);
        if (wait == WAIT_FAILED)
            return FALSE;

        if (dwMilliseconds != INFINITE) {
            uint64_t elapsed_ms = (idt_get_ticks() - start) * 10;
            if (elapsed_ms >= dwMilliseconds) {
                deadline_reached = TRUE;
            } else {
                remaining = dwMilliseconds - (DWORD)elapsed_ms;
            }
            if (wait == WAIT_TIMEOUT && wait_slice >= remaining)
                deadline_reached = TRUE;
        }
    }
}

BOOL WINAPI GetQueuedCompletionStatus(HANDLE hCompletionPort,
                                      DWORD *lpNumberOfBytesTransferred,
                                      ULONG_PTR *lpCompletionKey,
                                      PVOID *lpOverlapped,
                                      DWORD dwMilliseconds)
{
    if (!lpNumberOfBytesTransferred || !lpCompletionKey || !lpOverlapped) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    *lpOverlapped = NULL;
    IOCP_PORT *port = iocp_find(hCompletionPort);
    if (!port) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    IOCP_PACKET packet;
    if (!iocp_wait_packet(hCompletionPort, port, GetCurrentThreadId(),
                          &packet, dwMilliseconds))
        return FALSE;

    *lpNumberOfBytesTransferred = packet.bytes;
    *lpCompletionKey = packet.key;
    *lpOverlapped = packet.overlapped;
    if (!NT_SUCCESS(packet.completion_status)) {
        set_last_error_from_status(packet.completion_status);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}
/* ── Mutex API (UT99) ──────────────────────────────────────── */

typedef struct {
    ULONG_PTR completion_key;
    PVOID overlapped;
    ULONG_PTR internal;
    DWORD bytes;
    DWORD padding;
} K32_OVERLAPPED_ENTRY64;

_Static_assert(sizeof(K32_OVERLAPPED_ENTRY64) == 32,
               "OVERLAPPED_ENTRY64 layout");

static void iocp_store_extended_entry(PVOID entries, ULONG index,
                                      const IOCP_PACKET *packet,
                                      NTSTATUS completion_status)
{
    if (g_compat32_mode) {
        uint32_t *entry = (uint32_t *)entries + index * 4;
        entry[0] = (uint32_t)packet->key;
        entry[1] = (uint32_t)(uintptr_t)packet->overlapped;
        entry[2] = (uint32_t)completion_status;
        entry[3] = packet->bytes;
    } else {
        K32_OVERLAPPED_ENTRY64 *entry =
            &((K32_OVERLAPPED_ENTRY64 *)entries)[index];
        entry->completion_key = packet->key;
        entry->overlapped = packet->overlapped;
        entry->internal = (ULONG_PTR)completion_status;
        entry->bytes = packet->bytes;
        entry->padding = 0;
    }
}

BOOL WINAPI GetQueuedCompletionStatusEx(HANDLE hCompletionPort,
                                        PVOID lpCompletionPortEntries,
                                        ULONG ulCount,
                                        ULONG *ulNumEntriesRemoved,
                                        DWORD dwMilliseconds,
                                        BOOL fAlertable)
{
    (void)fAlertable;
    if (!lpCompletionPortEntries || !ulNumEntriesRemoved || !ulCount) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    *ulNumEntriesRemoved = 0;

    IOCP_PORT *port = iocp_find(hCompletionPort);
    if (!port) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    DWORD tid = GetCurrentThreadId();
    IOCP_PACKET packet;
    if (!iocp_wait_packet(hCompletionPort, port, tid, &packet,
                           dwMilliseconds))
        return FALSE;

    iocp_store_extended_entry(lpCompletionPortEntries, 0, &packet,
                               packet.completion_status);
    ULONG removed = 1;
    while (removed < ulCount && iocp_try_dequeue(port, tid, &packet, NULL)) {
        NTSTATUS completion_status = packet.completion_status;
        if (packet.pending_pipe_slot >= 0 &&
            !k32_pipe_finalize_packet(&packet, &completion_status))
            continue;
        iocp_store_extended_entry(lpCompletionPortEntries, removed, &packet,
                                  completion_status);
        removed++;
    }
    *ulNumEntriesRemoved = removed;
    SetLastError(0);
    return TRUE;
}

static BOOL k32_pipe_pending_packet(HANDLE file, PVOID overlapped,
                                    IOCP_PACKET *packet, BOOL *completed)
{
    DWORD owner_pid = win32_current_process_id();
    BOOL found = FALSE;
    uint64_t pending_flags = pending_pipe_read_lock();
    for (int i = 0; i < MAX_PENDING_PIPE_READS; i++) {
        K32_PENDING_PIPE_READ *pending = &g_pending_pipe_reads[i];
        if (!pending->active || pending->owner_pid != owner_pid ||
            pending->file != file || pending->overlapped != overlapped)
            continue;
        packet->bytes = pending->completion_bytes;
        packet->key = 0;
        packet->overlapped = overlapped;
        packet->completion_status = pending->completion_status;
        packet->pending_pipe_slot = i;
        packet->pending_pipe_generation = pending->generation;
        *completed = pending->completed;
        found = TRUE;
        break;
    }
    pending_pipe_read_unlock(pending_flags);
    return found;
}

static BOOL k32_io_completion_query(const K32_IO_COMPLETION *completion,
                                    NTSTATUS *status, DWORD *bytes)
{
    if (completion->ready) {
        *status = completion->status;
        *bytes = completion->bytes;
        return TRUE;
    }

    IOCP_PACKET packet;
    BOOL completed = FALSE;
    if (k32_pipe_pending_packet(completion->file, completion->overlapped,
                                &packet, &completed)) {
        if (!completed)
            return FALSE;
        *status = packet.completion_status;
        *bytes = packet.bytes;
        return TRUE;
    }

    ULONG_PTR internal;
    ULONG_PTR internal_high;
    if (completion->compat32) {
        volatile uint32_t *values =
            (volatile uint32_t *)completion->overlapped;
        internal = values[0];
        internal_high = values[1];
    } else {
        volatile ULONG_PTR *values =
            (volatile ULONG_PTR *)completion->overlapped;
        internal = values[0];
        internal_high = values[1];
    }
    if ((NTSTATUS)internal == STATUS_PENDING)
        return FALSE;
    *status = (NTSTATUS)internal;
    *bytes = (DWORD)internal_high;
    return TRUE;
}

static DWORD k32_dispatch_io_completions(void)
{
    DWORD owner_pid = win32_current_process_id();
    DWORD owner_tid = GetCurrentThreadId();
    DWORD dispatched = 0;

    k32_pipe_service_pending();
    for (;;) {
        BOOL made_progress = FALSE;
        for (int i = 0; i < K32_MAX_IO_COMPLETIONS; i++) {
            K32_IO_COMPLETION completion;
            BOOL matches = FALSE;
            uint64_t flags = k32_io_completion_lock_irqsave();
            if (g_io_completions[i].used &&
                g_io_completions[i].owner_pid == owner_pid &&
                g_io_completions[i].owner_tid == owner_tid) {
                completion = g_io_completions[i];
                matches = TRUE;
            }
            k32_io_completion_unlock_irqrestore(flags);
            if (!matches)
                continue;

            NTSTATUS status;
            DWORD bytes;
            if (!k32_io_completion_query(&completion, &status, &bytes))
                continue;

            BOOL claimed = FALSE;
            flags = k32_io_completion_lock_irqsave();
            if (g_io_completions[i].used &&
                g_io_completions[i].generation == completion.generation &&
                g_io_completions[i].owner_pid == owner_pid &&
                g_io_completions[i].owner_tid == owner_tid) {
                memset(&g_io_completions[i], 0,
                       sizeof(g_io_completions[i]));
                claimed = TRUE;
            }
            k32_io_completion_unlock_irqrestore(flags);
            if (!claimed)
                continue;

            DWORD error = NT_SUCCESS(status)
                ? 0 : RtlNtStatusToDosError(status);
            uint32_t trace_index = __atomic_fetch_add(
                &g_io_completion_trace_count, 1, __ATOMIC_RELAXED);
            if (trace_index < 32) {
                serial_puts("[K32-APC] dispatch pid=");
                serial_putdec(owner_pid);
                serial_puts(" tid=");
                serial_putdec(owner_tid);
                serial_puts(" error=");
                serial_putdec(error);
                serial_puts(" bytes=");
                serial_putdec(bytes);
                serial_puts(" routine=0x");
                serial_puthex((uint64_t)(ULONG_PTR)completion.routine, 16);
                serial_puts("\n");
            }

            if (completion.compat32) {
                uint32_t args[3] = {
                    error, bytes,
                    (uint32_t)(ULONG_PTR)completion.overlapped
                };
                compat32_callback_args(
                    (uint32_t)(ULONG_PTR)completion.routine, 3, args);
            } else {
                typedef void (WINAPI *io_completion_routine_t)(
                    DWORD, DWORD, PVOID);
                ((io_completion_routine_t)completion.routine)(
                    error, bytes, completion.overlapped);
            }

            dispatched++;
            made_progress = TRUE;
            if (dispatched >= K32_MAX_IO_COMPLETIONS)
                return dispatched;
        }
        if (!made_progress)
            return dispatched;
        k32_pipe_service_pending();
    }
}

static BOOL WINAPI GetOverlappedResult_k32(HANDLE file, PVOID overlapped,
                                            DWORD *bytes_transferred,
                                            BOOL wait)
{
    if (!overlapped || !bytes_transferred) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    for (;;) {
        IOCP_PACKET packet;
        BOOL completed = FALSE;
        if (k32_pipe_pending_packet(file, overlapped, &packet, &completed)) {
            if (!completed) {
                if (!wait) {
                    SetLastError(996); /* ERROR_IO_INCOMPLETE */
                    return FALSE;
                }
                sched_yield();
                continue;
            }

            NTSTATUS status = packet.completion_status;
            *bytes_transferred = packet.bytes;
            if (!NT_SUCCESS(status)) {
                set_last_error_from_status(status);
                return FALSE;
            }
            SetLastError(0);
            return TRUE;
        }

        NTSTATUS status;
        ULONG_PTR bytes;
        if (g_compat32_mode) {
            volatile uint32_t *values = (volatile uint32_t *)overlapped;
            status = (NTSTATUS)values[0];
            bytes = values[1];
        } else {
            volatile ULONG_PTR *values = (volatile ULONG_PTR *)overlapped;
            status = (NTSTATUS)values[0];
            bytes = values[1];
        }
        if (status == STATUS_PENDING) {
            if (wait) {
                sched_yield();
                continue;
            }
            SetLastError(996); /* ERROR_IO_INCOMPLETE */
            return FALSE;
        }
        *bytes_transferred = (DWORD)bytes;
        if (!NT_SUCCESS(status)) {
            set_last_error_from_status(status);
            return FALSE;
        }
        SetLastError(0);
        return TRUE;
    }
}

extern NTSTATUS sys_NtCreateMutant(ULONG_PTR *);
extern NTSTATUS sys_NtReleaseMutant(ULONG_PTR *);

#define K32_MAX_NAMED_MUTEXES K32_MAX_NAMED_OBJECTS
static K32_NAMED_OBJECT named_mutexes[K32_MAX_NAMED_MUTEXES];

#define K32_STEAMIPC_SYNC_TRACE_LIMIT 2048
static volatile uint32_t g_steamipc_sync_trace_count;

static const char *steamipc_name_for_handle(HANDLE handle)
{
    const char *name = named_object_name_for_handle(
        named_events, K32_MAX_NAMED_EVENTS, OBJ_TYPE_EVENT, handle);
    if (!name)
        name = named_object_name_for_handle(
            named_mutexes, K32_MAX_NAMED_MUTEXES, OBJ_TYPE_MUTANT, handle);
    if (!name)
        name = named_object_name_for_handle(
            named_mappings, K32_MAX_NAMED_MAPPINGS, OBJ_TYPE_SECTION,
            handle);
    return named_object_is_steamipc(name) ? name : NULL;
}

static void steamipc_trace_handle(const char *operation, HANDLE handle,
                                  uint32_t argument, uint32_t result,
                                  uint64_t caller)
{
    const char *name = steamipc_name_for_handle(handle);
    if (!name)
        return;

    uint32_t sequence = __atomic_fetch_add(&g_steamipc_sync_trace_count, 1,
                                           __ATOMIC_RELAXED);
    if (!K32_STEAMIPC_SERIAL_TRACE ||
        sequence >= K32_STEAMIPC_SYNC_TRACE_LIMIT)
        return;

    serial_puts("[K32-STEAMSYNC] seq=");
    serial_putdec(sequence);
    serial_puts(" pid=");
    serial_putdec(win32_current_process_id());
    serial_puts(" kpid=");
    serial_putdec((uint32_t)proc_current_pid());
    serial_puts(" op=");
    serial_puts(operation);
    serial_puts(" handle=0x");
    serial_puthex((ULONG_PTR)handle, 16);
    serial_puts(" arg=0x");
    serial_puthex(argument, 8);
    serial_puts(" result=0x");
    serial_puthex(result, 8);
    serial_puts(" caller=0x");
    serial_puthex(caller, 16);
    serial_puts(" name='");
    serial_puts(name);
    serial_puts("'\n");
}

static HANDLE create_mutex_k32(BOOL initial_owner, const char *name)
{
    if (name && *name) {
        HANDLE existing = named_object_open(
            named_mutexes, K32_MAX_NAMED_MUTEXES, OBJ_TYPE_MUTANT,
            name, GENERIC_ALL);
        if (existing) {
            SetLastError(183); /* ERROR_ALREADY_EXISTS */
            steamipc_trace_handle("CreateMutex.old", existing,
                                  (uint32_t)initial_owner, 183, 0);
            return existing;
        }
    }

    HANDLE handle = NULL;
    ULONG_PTR args[4] = {
        (ULONG_PTR)&handle, GENERIC_ALL, 0, (ULONG_PTR)initial_owner
    };
    NTSTATUS status = sys_NtCreateMutant(args);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return NULL;
    }
    if (name && *name) {
        BOOL already_exists = FALSE;
        HANDLE published = named_object_publish(
            named_mutexes, K32_MAX_NAMED_MUTEXES, OBJ_TYPE_MUTANT,
            name, handle, &already_exists);
        if (!published) {
            CloseHandle(handle);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        if (already_exists) {
            CloseHandle(handle);
            SetLastError(183); /* ERROR_ALREADY_EXISTS */
            steamipc_trace_handle("CreateMutex.race", published,
                                  (uint32_t)initial_owner, 183, 0);
            return published;
        }
        handle = published;
    }
    SetLastError(0);
    steamipc_trace_handle("CreateMutex.new", handle,
                          (uint32_t)initial_owner, 0, 0);
    return handle;
}

HANDLE WINAPI CreateMutexA(PVOID lpMutexAttributes, BOOL bInitialOwner, PCSTR lpName)
{
    (void)lpMutexAttributes;
    char name[K32_OBJECT_NAME_MAX];
    if (!named_object_name_a(lpName, name)) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NULL;
    }
    return create_mutex_k32(bInitialOwner, name);
}

HANDLE WINAPI CreateMutexW(PVOID lpMutexAttributes, BOOL bInitialOwner, PCWSTR lpName)
{
    (void)lpMutexAttributes;
    char name[K32_OBJECT_NAME_MAX];
    if (!named_object_name_w(lpName, name)) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NULL;
    }
    return create_mutex_k32(bInitialOwner, name);
}

/* ── DLL / Module API ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

/* File reading for DLL loading */
extern void *osfs2_find(const char *name);
extern void *osfs2_find_ci(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t osfs2_file_size(void *file);

/* DLLs that are known to not exist — return NULL immediately to avoid
 * deep recursive searches or stack overflows in dll_load. */
static int is_unavailable_dll(const char *name)
{
    /* Extract basename */
    const char *bn = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') bn = p + 1;

    /* Most entries are exact DLL stems. Prefix matching here used to reject
     * hidapi.dll while trying to reject hid.dll. */
    static const char *skip_exact[] = {
        "RICHED32", "RICHED20", "COMCTL32", "HHCTRL", "VERSION",
        "RPCRT4", "SHLWAPI", "SETUPAPI", "CRYPT32", "WLDAP32",
        "SECUR32", "d3d8", "d3d9", "d3d11",
        "HID", "CFGMGR32",
        NULL
    };
    static const char *skip_prefix[] = {
        "dinput", "XINPUT", NULL
    };
    for (int i = 0; skip_exact[i]; i++) {
        const char *a = bn, *b = skip_exact[i];
        int match = 1;
        while (*b) {
            char ca = *a, cb = *b;
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) { match = 0; break; }
            a++; b++;
        }
        if (match && (*a == 0 ||
                      (a[0] == '.' &&
                       (a[1] == 'd' || a[1] == 'D') &&
                       (a[2] == 'l' || a[2] == 'L') &&
                       (a[3] == 'l' || a[3] == 'L') && a[4] == 0)))
            return 1;
    }
    for (int i = 0; skip_prefix[i]; i++) {
        const char *a = bn, *b = skip_prefix[i];
        int match = 1;
        while (*b) {
            char ca = *a, cb = *b;
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) { match = 0; break; }
            a++; b++;
        }
        if (match) return 1;
    }
    return 0;
}

static HANDLE load_library_a_flags(PCSTR lpLibFileName, DWORD search_flags)
{
    if (!lpLibFileName) return NULL;

    BOOL trace_java_library =
        k32_path_contains_ci(lpLibFileName, "java.dll") ||
        k32_path_contains_ci(lpLibFileName, "verify.dll");
    if (trace_java_library) {
        const char *image_path = win32_current_image_path();
        serial_puts("[K32-JAVA-LOAD] request='");
        serial_puts(lpLibFileName);
        serial_puts("' cwd='C:\\");
        serial_puts(kernel32_current_directory_relative());
        serial_puts("' image='");
        serial_puts(image_path ? image_path : "");
        serial_puts("' flags=0x");
        serial_puthex(search_flags, 8);
        serial_puts("\n");
    }

    BOOL trace_cef_delay = K32_VERBOSE_DIAGNOSTICS &&
        (k32_path_contains_ci(lpLibFileName, "user32.dll") ||
         k32_path_contains_ci(lpLibFileName, "oleacc.dll")) &&
        __atomic_fetch_sub(&g_cef_delay_trace_budget, 1,
                           __ATOMIC_RELAXED) > 0;
    if (trace_cef_delay) {
        serial_puts("[K32-DELAY] LoadLibrary '");
        serial_puts(lpLibFileName);
        serial_puts("'\n");
    }

    const char *command = win32_current_command_line();
    bool trace_angle_library = K32_VERBOSE_DIAGNOSTICS &&
        command && process_command_contains(command, "--type=gpu-process") &&
        (k32_path_contains_ci(lpLibFileName, "vulkan-1.dll") ||
         k32_path_contains_ci(lpLibFileName, "vk_swiftshader.dll") ||
         k32_path_contains_ci(lpLibFileName, "libegl.dll") ||
         k32_path_contains_ci(lpLibFileName, "libglesv2.dll"));
    if (trace_angle_library) {
        serial_puts("[K32-ANGLE] LoadLibrary request '");
        serial_puts(lpLibFileName);
        serial_puts("' cwd='C:\\");
        serial_puts(kernel32_current_directory_relative());
        serial_puts("'\n");
    }

    /* Fast-reject known-missing DLLs */
    if (is_unavailable_dll(lpLibFileName) && !dll_is_shim(lpLibFileName))
        return NULL;

    /* Reject purely-numeric basenames. UT99's native-binding loop
     * walks an internal package array past Transient — once past the
     * legit packages, slot N has its FName resolved as the decimal
     * representation of an uninitialised index, so the engine asks us
     * to LoadLibrary("C:\\System\\0"), "\\1", "\\2", … and (because we
     * used to return a sentinel handle for every request) treats each
     * one as a successfully loaded native, then later tries to find
     * package "0.u" / "1.u" / … on disk and throws PackageNotFound
     * from the localised error path. Returning NULL here tells the
     * engine the binding does not exist and the loop short-circuits. */
    {
        const char *bn = lpLibFileName;
        for (const char *p = lpLibFileName; *p; p++)
            if (*p == '\\' || *p == '/') bn = p + 1;
        bool all_digits = (*bn != 0);
        for (const char *p = bn; *p; p++) {
            if (*p < '0' || *p > '9') { all_digits = false; break; }
        }
        if (all_digits) {
            serial_puts("[K32] LoadLibraryA: rejecting numeric basename '");
            serial_puts(bn);
            serial_puts("' (uninitialised package slot)\n");
            return NULL;
        }
    }

#ifndef OK_QUIET
    serial_puts("[K32] LoadLibraryA: ");
    serial_puts(lpLibFileName);
    serial_puts("\n");
#endif

    /* Lookup and reference acquisition must be one loader-lock operation.
     * A separate probe followed by a second lookup can race FreeLibrary and
     * silently return NULL from this branch even though the first probe hit. */
    HANDLE loaded = (HANDLE)dll_get_module_handle(lpLibFileName, TRUE);
    if (loaded) {
        if (trace_cef_delay)
            serial_puts("[K32-DELAY] loaded PE module\n");
        if (trace_java_library) {
            serial_puts("[K32-JAVA-LOAD] reused base=0x");
            serial_puthex((ULONG_PTR)loaded, g_compat32_mode ? 8 : 16);
            serial_puts("\n");
        }
        return loaded;
    }

    /* Registered shims expose a process-local PE facade so applications may
     * inspect a real HMODULE before resolving exports. */
    {
        HANDLE h = (HANDLE)dll_get_shim_module_handle(lpLibFileName, TRUE);
        if (h) {
            if (trace_java_library) {
                serial_puts("[K32-JAVA-LOAD] shim base=0x");
                serial_puthex((ULONG_PTR)h, g_compat32_mode ? 8 : 16);
                serial_puts("\n");
            }
            if (trace_cef_delay) {
                serial_puts("[K32-DELAY] shim handle 0x");
                serial_puthex((uint64_t)(ULONG_PTR)h, 8);
                serial_puts("\n");
            }
#ifndef OK_QUIET
            serial_puts("[K32] LoadLibrary: shim found for ");
            serial_puts(lpLibFileName);
            serial_puts("\n");
#endif
            return h;
        }
    }

    /* Filesystem search, read, and publication are centralized under the
     * process loader lock so concurrent LoadLibrary calls cannot duplicate a
     * full image read. */
    SetLastError(0);
    PVOID base = dll_load_from_fs_ex(lpLibFileName, TRUE, search_flags);
    if (base) {
        if (trace_java_library) {
            serial_puts("[K32-JAVA-LOAD] loaded base=0x");
            serial_puthex((ULONG_PTR)base, g_compat32_mode ? 8 : 16);
            serial_puts("\n");
        }
        return (HANDLE)base;
    }

    DWORD load_error = GetLastError();
    if (trace_java_library) {
        serial_puts("[K32-JAVA-LOAD] failed error=");
        serial_putdec(load_error);
        serial_puts("\n");
    }

    /* Match LoadLibrary: optional DLL probes must be able to fail. */
#ifndef OK_QUIET
    serial_puts("[K32] DLL not found\n");
#endif
    if (GetLastError() != 193) /* Preserve ERROR_BAD_EXE_FORMAT. */
        SetLastError(126); /* ERROR_MOD_NOT_FOUND */
    if (trace_cef_delay)
        serial_puts("[K32-DELAY] module not found\n");
    if (trace_angle_library)
        serial_puts("[K32-ANGLE] LoadLibrary failed\n");
    return NULL;
}

HANDLE WINAPI LoadLibraryA(PCSTR lpLibFileName)
{
    return load_library_a_flags(lpLibFileName, 0);
}

static HANDLE load_library_w_flags(PCWSTR lpLibFileName, DWORD search_flags)
{
    if (!lpLibFileName) return NULL;

    /* Convert wide to ASCII */
    char name[260];
    int i;
    for (i = 0; i < 259 && lpLibFileName[i]; i++)
        name[i] = (char)(lpLibFileName[i] & 0xFF);
    name[i] = 0;

#ifndef OK_QUIET
    serial_puts("[K32] LoadLibraryW: ");
    serial_puts(name);
    serial_puts("\n");
#endif

    return load_library_a_flags(name, search_flags);
}

HANDLE WINAPI LoadLibraryW(PCWSTR lpLibFileName)
{
    return load_library_w_flags(lpLibFileName, 0);
}

HANDLE WINAPI LoadLibraryExA(PCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    (void)hFile;
    return load_library_a_flags(lpLibFileName, dwFlags);
}

HANDLE WINAPI LoadLibraryExW(PCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    (void)hFile;
    return load_library_w_flags(lpLibFileName, dwFlags);
}

BOOL WINAPI FreeLibrary(HANDLE hLibModule)
{
    if (!dll_release_module((PVOID)hLibModule)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI DisableThreadLibraryCalls(HANDLE hLibModule)
{
    if (!dll_disable_thread_notifications((PVOID)hLibModule)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static const char *k32_system_relative_path(const char *path)
{
    if (!path) return NULL;
    if (path[0] && path[1] == ':') path += 2;
    while (*path == '\\' || *path == '/') path++;

    const char *candidate = path;
    const char *prefix = "system";
    while (*candidate && *prefix &&
           k32_path_fold(*candidate) == *prefix) {
        candidate++;
        prefix++;
    }
    if (!*prefix && (*candidate == '\\' || *candidate == '/'))
        return candidate + 1;
    return path;
}

static BOOL k32_path_has_directory(const char *path)
{
    if (!path) return FALSE;
    for (; *path; path++)
        if (*path == '\\' || *path == '/') return TRUE;
    return FALSE;
}

static BOOL current_module_relative_path(HANDLE module, char path[260])
{
    const char *image_path =
        k32_system_relative_path(win32_current_image_path());
    if (!image_path || !*image_path)
        image_path = win32_current_exe_name();

    const char *module_name = NULL;
    const char *module_path = NULL;
    if (module && (ULONG_PTR)module != win32_current_image_base()) {
        LOADED_MODULE *loaded = dll_find_module_by_base((PVOID)module);
        if (!loaded) return FALSE;
        module_name = loaded->name;
        if (loaded->path[0] && k32_path_has_directory(loaded->path))
            module_path = k32_system_relative_path(loaded->path);
    }

    SIZE_T out = 0;
    if (module_path) {
        while (module_path[out] && out < 259) {
            char value = module_path[out];
            path[out] = value == '/' ? '\\' : value;
            out++;
        }
        if (module_path[out]) return FALSE;
    } else if (module_name) {
        const char *last_separator = NULL;
        for (const char *p = image_path; *p; p++)
            if (*p == '\\' || *p == '/') last_separator = p;
        if (last_separator) {
            while (image_path + out <= last_separator && out < 259) {
                path[out] = image_path[out];
                out++;
            }
        }
        for (SIZE_T i = 0; module_name[i] && out < 259; i++)
            path[out++] = module_name[i];
    } else {
        while (image_path[out] && out < 259) {
            char value = image_path[out];
            path[out] = value == '/' ? '\\' : value;
            out++;
        }
        if (image_path[out]) return FALSE;
    }
    path[out] = 0;
    return TRUE;
}

DWORD WINAPI GetModuleFileNameA(HANDLE hModule, PSTR lpFilename, DWORD nSize)
{
    if (!lpFilename || !nSize) {
        SetLastError(87);
        return 0;
    }
    /* Build full path: "C:\System\<exe_name>" so engine can derive install dir */
    char relative_path[260];
    if (!current_module_relative_path(hModule, relative_path)) {
        SetLastError(126); /* ERROR_MOD_NOT_FOUND */
        return 0;
    }
    static const char prefix[] = "C:\\System\\";
    DWORD pos = 0;

    for (int i = 0; prefix[i] && pos < nSize - 1; i++)
        lpFilename[pos++] = prefix[i];
    for (int i = 0; relative_path[i] && pos < nSize - 1; i++)
        lpFilename[pos++] = relative_path[i];
    lpFilename[pos] = 0;
    return pos;
}

DWORD WINAPI GetModuleFileNameW(HANDLE hModule, PWSTR lpFilename, DWORD nSize)
{
    if (!lpFilename || !nSize) {
        SetLastError(87);
        return 0;
    }
    /* Build wide path: "C:\System\<name>" matching GetModuleFileNameA */
    char relative_path[260];
    if (!current_module_relative_path(hModule, relative_path)) {
        SetLastError(126); /* ERROR_MOD_NOT_FOUND */
        return 0;
    }
    static const WCHAR prefix[] = {'C',':','\\','S','y','s','t','e','m','\\'};
    DWORD pos = 0;

    for (DWORD i = 0; i < 10 && pos < nSize - 1; i++)
        lpFilename[pos++] = prefix[i];
    for (int i = 0; relative_path[i] && pos < nSize - 1; i++)
        lpFilename[pos++] = (WCHAR)(unsigned char)relative_path[i];

    lpFilename[pos] = 0;

    const char *command = win32_current_command_line();
    if (K32_VERBOSE_DIAGNOSTICS && command &&
        process_command_contains(command, "--type=gpu-process")) {
        static uint32_t angle_module_logs;
        uint32_t index = __atomic_fetch_add(&angle_module_logs, 1,
                                             __ATOMIC_RELAXED);
        if (index < 24) {
            serial_puts("[K32-ANGLE] GetModuleFileNameW h=0x");
            serial_puthex((uint64_t)(ULONG_PTR)hModule, 16);
            serial_puts(" -> C:\\System\\");
            serial_puts(relative_path);
            serial_puts("\n");
        }
    }
    return pos;
}

#define K32_PROCESS_VM_READ                   0x00000010U
#define K32_PROCESS_QUERY_INFORMATION         0x00000400U
#define K32_PROCESS_QUERY_LIMITED_INFORMATION 0x00001000U

static BOOL k32_process_image_full_path(HANDLE process, HANDLE module,
                                        char path[384], DWORD *process_id)
{
    char image_path[320];
    PVOID image_base = NULL;
    DWORD pid;

    if (process == NT_CURRENT_PROCESS ||
        (ULONG_PTR)process == UINT32_MAX) {
        pid = win32_current_process_id();
        const char *current = win32_current_image_path();
        SIZE_T length = 0;
        if (!current) return FALSE;
        while (current[length] && length + 1 < sizeof(image_path)) {
            image_path[length] = current[length];
            length++;
        }
        if (current[length]) return FALSE;
        image_path[length] = 0;
        image_base = (PVOID)win32_current_image_base();
    } else {
        HANDLE_OBJECT_SNAPSHOT snapshot;
        NTSTATUS status = handle_snapshot_for_process(
            &g_handle_table, process, win32_current_process_id(), &snapshot);
        if (!NT_SUCCESS(status) || snapshot.type != OBJ_TYPE_PROCESS) {
            SetLastError(6); /* ERROR_INVALID_HANDLE */
            return FALSE;
        }

        ACCESS_MASK required = K32_PROCESS_QUERY_INFORMATION;
        if (module) required |= K32_PROCESS_VM_READ;
        BOOL query_limited = !module &&
            (snapshot.access & K32_PROCESS_QUERY_LIMITED_INFORMATION);
        if (!(snapshot.access & GENERIC_ALL) && !query_limited &&
            (snapshot.access & required) != required) {
            SetLastError(5); /* ERROR_ACCESS_DENIED */
            return FALSE;
        }

        extern BOOL nt_process_id(HANDLE, DWORD *);
        extern BOOL nt_process_image_path(HANDLE, char *, SIZE_T);
        extern BOOL nt_process_image_base(HANDLE, PVOID *);
        if (!nt_process_id(process, &pid) ||
            !nt_process_image_path(process, image_path, sizeof(image_path)) ||
            !nt_process_image_base(process, &image_base)) {
            SetLastError(6);
            return FALSE;
        }
    }

    const char *relative = k32_system_relative_path(image_path);

    static const char system_prefix[] = "C:\\System\\";
    SIZE_T out = 0;
    for (SIZE_T i = 0; system_prefix[i]; i++) path[out++] = system_prefix[i];
    while (*relative && out + 1 < 384) {
        char value = *relative++;
        path[out++] = value == '/' ? '\\' : value;
    }
    if (*relative) return FALSE;
    path[out] = 0;

    if (module && module != image_base) {
        LOADED_MODULE *loaded = dll_find_module_by_base((PVOID)module);
        if (!loaded || loaded->owner_pid != pid) {
            SetLastError(126); /* ERROR_MOD_NOT_FOUND */
            return FALSE;
        }

        BOOL have_stored_path = !loaded->synthetic_shim &&
            loaded->path[0] && k32_path_has_directory(loaded->path);
        if (loaded->synthetic_shim) {
            static const char shim_prefix[] = "C:\\Windows\\System32\\";
            out = 0;
            for (SIZE_T i = 0; shim_prefix[i]; i++)
                path[out++] = shim_prefix[i];
        } else if (have_stored_path) {
            static const char system_prefix[] = "C:\\System\\";
            relative = k32_system_relative_path(loaded->path);
            out = 0;
            for (SIZE_T i = 0; system_prefix[i]; i++)
                path[out++] = system_prefix[i];
            while (*relative && out + 1 < 384) {
                char value = *relative++;
                path[out++] = value == '/' ? '\\' : value;
            }
            if (*relative) return FALSE;
        } else {
            while (out && path[out - 1] != '\\') out--;
        }
        if (!have_stored_path) {
            for (SIZE_T i = 0; loaded->name[i] && out + 1 < 384; i++)
                path[out++] = loaded->name[i];
        }
        if (out + 1 >= 384) return FALSE;
        path[out] = 0;
    }

    if (process_id) *process_id = pid;
    return TRUE;
}

static DWORD k32_copy_process_path_a(const char *path, PSTR destination,
                                     DWORD capacity)
{
    DWORD length = 0;
    while (path[length]) length++;
    if (length >= capacity) {
        for (DWORD i = 0; i + 1 < capacity; i++) destination[i] = path[i];
        destination[capacity - 1] = 0;
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return capacity;
    }
    for (DWORD i = 0; i <= length; i++) destination[i] = path[i];
    SetLastError(0);
    return length;
}

static DWORD WINAPI K32GetModuleFileNameExA_k32(HANDLE process,
                                                 HANDLE module,
                                                 PSTR filename, DWORD size)
{
    if (!filename || !size) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    char path[384];
    DWORD pid = 0;
    if (!k32_process_image_full_path(process, module, path, &pid)) {
        if (!GetLastError()) SetLastError(6);
        return 0;
    }

    static uint32_t trace_count;
    if (__atomic_fetch_add(&trace_count, 1, __ATOMIC_RELAXED) < 16) {
        serial_puts("[K32-PROCESS-PATH] pid=");
        serial_putdec(pid);
        serial_puts(" module=0x");
        serial_puthex((uint64_t)(ULONG_PTR)module, 16);
        serial_puts(" path=");
        serial_puts(path);
        serial_puts("\n");
    }
    return k32_copy_process_path_a(path, filename, size);
}

static DWORD WINAPI K32GetModuleFileNameExW_k32(HANDLE process,
                                                 HANDLE module,
                                                 PWSTR filename, DWORD size)
{
    if (!filename || !size) {
        SetLastError(87);
        return 0;
    }

    char path[384];
    if (!k32_process_image_full_path(process, module, path, NULL)) {
        if (!GetLastError()) SetLastError(6);
        return 0;
    }

    DWORD length = 0;
    while (path[length]) length++;
    if (length >= size) {
        for (DWORD i = 0; i + 1 < size; i++)
            filename[i] = (WCHAR)(BYTE)path[i];
        filename[size - 1] = 0;
        SetLastError(122);
        return size;
    }
    for (DWORD i = 0; i <= length; i++)
        filename[i] = (WCHAR)(BYTE)path[i];
    SetLastError(0);
    return length;
}

static DWORD WINAPI K32GetMappedFileNameW_k32(HANDLE process, PVOID address,
                                               PWSTR filename, DWORD size)
{
    if (!k32_process_handle_valid(process) || !address || !filename || !size) {
        SetLastError(!k32_process_handle_valid(process) ? 6 : 87);
        return 0;
    }
    /* All PE mappings currently share one address space, so the active image
     * path is the best available mapped-file identity. */
    return GetModuleFileNameW(NULL, filename, size);
}

static BOOL WINAPI SetDefaultDllDirectories_k32(DWORD directory_flags)
{
    const DWORD allowed = 0x00000200U | /* APPLICATION_DIR */
                          0x00000400U | /* USER_DIRS */
                          0x00000800U | /* SYSTEM32 */
                          0x00001000U;  /* DEFAULT_DIRS */
    if (!directory_flags || (directory_flags & ~allowed)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (!dll_set_default_search_flags(directory_flags)) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetDllDirectoryA_k32(PCSTR path)
{
    if (!path) {
        if (!dll_set_search_directory(NULL, FALSE)) {
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return FALSE;
        }
        SetLastError(0);
        return TRUE;
    }
    if (strlen(path) >= 260) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return FALSE;
    }

    if (!path[0]) {
        if (!dll_set_search_directory(NULL, TRUE)) {
            SetLastError(8);
            return FALSE;
        }
        SetLastError(0);
        return TRUE;
    }

    char normalized[260];
    if (!win32_normalize_path(path, normalized)) {
        SetLastError(206);
        return FALSE;
    }
    if (!dll_set_search_directory(normalized, TRUE)) {
        SetLastError(8);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetDllDirectoryW_k32(PCWSTR path)
{
    if (!path)
        return SetDllDirectoryA_k32(NULL);

    char ascii_path[260];
    SIZE_T i = 0;
    while (path[i] && i < sizeof(ascii_path) - 1) {
        ascii_path[i] = (char)(path[i] & 0xFF);
        i++;
    }
    if (path[i]) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return FALSE;
    }
    ascii_path[i] = 0;
    return SetDllDirectoryA_k32(ascii_path);
}

static PVOID WINAPI AddDllDirectory_k32(PCWSTR new_directory)
{
    if (!new_directory || !new_directory[0]) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }

    char path[260];
    SIZE_T length = 0;
    while (new_directory[length] && length < sizeof(path) - 1) {
        WCHAR value = new_directory[length];
        if (value > 0x7F) {
            SetLastError(1113); /* ERROR_NO_UNICODE_TRANSLATION */
            return NULL;
        }
        path[length] = (char)value;
        length++;
    }
    if (new_directory[length]) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NULL;
    }
    path[length] = 0;

    const char *absolute = path;
    if (absolute[0] == '\\' && absolute[1] == '\\' &&
        absolute[2] == '?' && absolute[3] == '\\')
        absolute += 4;
    else if (absolute[0] == '\\' && absolute[1] == '?' &&
             absolute[2] == '?' && absolute[3] == '\\')
        absolute += 4;
    BOOL drive_absolute =
        ((absolute[0] >= 'A' && absolute[0] <= 'Z') ||
         (absolute[0] >= 'a' && absolute[0] <= 'z')) &&
        absolute[1] == ':' &&
        (absolute[2] == '\\' || absolute[2] == '/');
    if (!drive_absolute) {
        SetLastError(87); /* AddDllDirectory requires an absolute path. */
        return NULL;
    }

    char normalized[260];
    if (!win32_normalize_path(path, normalized)) {
        SetLastError(206);
        return NULL;
    }
    if (!win32_directory_exists_normalized(normalized)) {
        SetLastError(3); /* ERROR_PATH_NOT_FOUND */
        return NULL;
    }

    PVOID cookie = dll_add_search_directory(normalized);
    if (!cookie) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }
    SetLastError(0);
    return cookie;
}

static BOOL WINAPI RemoveDllDirectory_k32(PVOID cookie)
{
    if (!dll_remove_search_directory(cookie)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

/* ── Timing ────────────────────────────────────────────────── */

extern uint64_t idt_get_ticks(void);

DWORD WINAPI GetTickCount(void)
{
    /* APIC timer fires at 100Hz (1 tick = 10ms).
     * Windows GetTickCount returns milliseconds. */
    return (DWORD)(idt_get_ticks() * 10);
}

ULONGLONG WINAPI GetTickCount64(void)
{
    return (ULONGLONG)(idt_get_ticks() * 10);
}

void WINAPI GetSystemTimeAsFileTime(PVOID lpSystemTimeAsFileTime)
{
    if (!lpSystemTimeAsFileTime)
        return;
    ULONGLONG filetime = wintime_now_filetime();
    memcpy(lpSystemTimeAsFileTime, &filetime, sizeof(filetime));
}

static void WINAPI GetSystemTimePreciseAsFileTime_k32(PVOID file_time)
{
    GetSystemTimeAsFileTime(file_time);
}

static BOOL WINAPI GetFileTime_k32(HANDLE file, PVOID creation,
                                    PVOID access, PVOID write)
{
    HANDLE_ENTRY *entry = handle_get_entry(&g_handle_table, file);
    if (!entry || entry->type != OBJ_TYPE_FILE) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (creation) GetSystemTimeAsFileTime(creation);
    if (access) GetSystemTimeAsFileTime(access);
    if (write) GetSystemTimeAsFileTime(write);
    return TRUE;
}

BOOL WINAPI GetThreadTimes(HANDLE hThread, PVOID creation, PVOID exit,
                           PVOID kernel, PVOID user)
{
    (void)hThread;
    if (!creation || !exit || !kernel || !user) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    GetSystemTimeAsFileTime(creation);
    *(ULONGLONG *)exit = 0;
    *(ULONGLONG *)kernel = 0;
    *(ULONGLONG *)user = (ULONGLONG)idt_get_ticks() * 100000ULL;
    return TRUE;
}

static BOOL WINAPI QueryThreadCycleTime_k32(HANDLE thread,
                                             ULONGLONG *cycle_time)
{
    if (!cycle_time || !GetThreadId_k32(thread)) {
        if (!cycle_time) SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    *cycle_time = (ULONGLONG)idt_get_ticks() * 100000ULL;
    return TRUE;
}

static WORD WINAPI RtlCaptureStackBackTrace_stub(DWORD skip, DWORD capture,
                                                  PVOID *backtrace,
                                                  DWORD *hash)
{
    (void)skip; (void)capture; (void)backtrace;
    if (hash) *hash = 0;
    /* ponytail: return no frames until the Win64 user unwinder is shared here. */
    return 0;
}

/* ── System Info ───────────────────────────────────────────── */

static DWORD k32_processor_count(void)
{
    extern uint32_t smp_cpu_count(void);
    uint32_t count = smp_cpu_count();
    if (!count) count = 1;
    return count > 64 ? 64 : count;
}

static DWORD WINAPI GetCurrentProcessorNumber_k32(void)
{
    DWORD count = k32_processor_count();
    return count ? smp_current_cpu() % count : 0;
}

static DWORD WINAPI GetMaximumProcessorCount_k32(WORD group_number)
{
    if (group_number != 0 && group_number != 0xFFFFU) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    return k32_processor_count();
}

static WORD WINAPI GetMaximumProcessorGroupCount_k32(void)
{
    return 1;
}

static BOOL WINAPI GetProcessHandleCount_k32(HANDLE process,
                                              DWORD *handle_count)
{
    if (!handle_count || !k32_process_handle_valid(process)) {
        SetLastError(!handle_count ? 87 : 6);
        return FALSE;
    }
    *handle_count = g_handle_table.count;
    return TRUE;
}

static DWORD WINAPI WTSGetActiveConsoleSessionId_k32(void)
{
    return 1;
}

static DWORD WINAPI WerRegisterRuntimeExceptionModule_k32(
    PCWSTR module, PVOID context)
{
    (void)module;
    (void)context;
    return 0; /* S_OK */
}

static BOOL WINAPI MiniDumpWriteDump_k32(HANDLE process, DWORD process_id,
                                          HANDLE file, DWORD dump_type,
                                          PCVOID exception_params,
                                          PCVOID user_streams,
                                          PCVOID callback_info)
{
    (void)process;
    (void)process_id;
    (void)file;
    (void)dump_type;
    (void)exception_params;
    (void)user_streams;
    (void)callback_info;
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return FALSE;
}

static ULONG_PTR k32_pointer_cookie;

static PVOID WINAPI EncodePointer_k32(PVOID pointer)
{
    ULONG_PTR value = (ULONG_PTR)pointer;
    if (g_compat32_mode)
        value = (uint32_t)value ^ (uint32_t)k32_pointer_cookie;
    else
        value ^= k32_pointer_cookie;
    return (PVOID)value;
}

static PVOID WINAPI DecodePointer_k32(PVOID pointer)
{
    return EncodePointer_k32(pointer);
}

static BOOL WINAPI SetProcessShutdownParameters_k32(DWORD level, DWORD flags)
{
    (void)level;
    (void)flags;
    return TRUE;
}

#define K32_MAX_PROCESS_AFFINITY 128

typedef struct {
    BOOL used;
    DWORD process_id;
    uint64_t mask;
} K32_PROCESS_AFFINITY;

static K32_PROCESS_AFFINITY
    k32_process_affinity[K32_MAX_PROCESS_AFFINITY];
static spinlock_t k32_process_affinity_lock = SPINLOCK_INIT;

static uint64_t k32_process_affinity_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&k32_process_affinity_lock);
    return flags;
}

static void k32_process_affinity_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&k32_process_affinity_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static uint64_t k32_system_affinity_mask(void)
{
    DWORD count = k32_processor_count();
    return count >= 64 ? UINT64_MAX : ((1ULL << count) - 1);
}

static BOOL k32_affinity_process_id(HANDLE process, DWORD *process_id)
{
    ULONG_PTR handle = (ULONG_PTR)process;
    if (handle == (ULONG_PTR)NT_CURRENT_PROCESS || handle == UINT32_MAX) {
        *process_id = win32_current_process_id();
        return TRUE;
    }
    if (!process)
        return FALSE;
    extern BOOL nt_process_id(HANDLE handle, DWORD *process_id);
    return nt_process_id(process, process_id);
}

static BOOL WINAPI SetProcessAffinityMask_k32(HANDLE process,
                                               ULONG_PTR requested_mask)
{
    DWORD process_id;
    uint64_t system_mask = k32_system_affinity_mask();
    uint64_t mask = (uint64_t)requested_mask;
    if (!k32_affinity_process_id(process, &process_id)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (!mask || (mask & ~system_mask)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    uint64_t flags = k32_process_affinity_lock_irqsave();
    K32_PROCESS_AFFINITY *free_slot = NULL;
    for (int i = 0; i < K32_MAX_PROCESS_AFFINITY; i++) {
        K32_PROCESS_AFFINITY *entry = &k32_process_affinity[i];
        if (entry->used && entry->process_id == process_id) {
            entry->mask = mask;
            k32_process_affinity_unlock_irqrestore(flags);
            SetLastError(0);
            return TRUE;
        }
        if (!entry->used && !free_slot)
            free_slot = entry;
    }
    if (free_slot) {
        free_slot->used = TRUE;
        free_slot->process_id = process_id;
        free_slot->mask = mask;
    }
    k32_process_affinity_unlock_irqrestore(flags);
    if (!free_slot) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    /* The scheduler currently treats this as an allowed-mask hint. Keeping
     * the process-visible state coherent avoids lying on a subsequent query. */
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI GetProcessAffinityMask_k32(HANDLE process,
                                               PVOID process_mask,
                                               PVOID system_mask)
{
    DWORD process_id;
    if (!process_mask || !system_mask) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (!k32_affinity_process_id(process, &process_id)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    uint64_t system = k32_system_affinity_mask();
    uint64_t allowed = system;
    uint64_t flags = k32_process_affinity_lock_irqsave();
    for (int i = 0; i < K32_MAX_PROCESS_AFFINITY; i++) {
        if (k32_process_affinity[i].used &&
            k32_process_affinity[i].process_id == process_id) {
            allowed = k32_process_affinity[i].mask;
            break;
        }
    }
    k32_process_affinity_unlock_irqrestore(flags);

    if (g_compat32_mode) {
        *(uint32_t *)process_mask = (uint32_t)allowed;
        *(uint32_t *)system_mask = (uint32_t)system;
    } else {
        *(uint64_t *)process_mask = allowed;
        *(uint64_t *)system_mask = system;
    }
    SetLastError(0);
    return TRUE;
}

static void k32_process_affinity_release(DWORD process_id)
{
    uint64_t flags = k32_process_affinity_lock_irqsave();
    for (int i = 0; i < K32_MAX_PROCESS_AFFINITY; i++) {
        if (k32_process_affinity[i].used &&
            k32_process_affinity[i].process_id == process_id) {
            memset(&k32_process_affinity[i], 0,
                   sizeof(k32_process_affinity[i]));
            break;
        }
    }
    k32_process_affinity_unlock_irqrestore(flags);
}

void WINAPI GetSystemInfo(LPSYSTEM_INFO lpSystemInfo)
{
    if (!lpSystemInfo) return;

    /*
     * PE32 (i386) code allocates a 32-bit SYSTEM_INFO (36 bytes) on the
     * stack. Our 64-bit struct is 48 bytes (PVOID/ULONG_PTR are 8 bytes).
     * Writing 48 bytes to a 36-byte buffer overflows 12 bytes, corrupting
     * the caller's stack frame. Bytes 44-45 contain wProcessorLevel=6,
     * which lands on the saved SEH ExceptionList → value 6 in SEH chain.
     *
     * Fix: write using 32-bit struct layout (all fields 4 bytes or less).
     *
     * 32-bit SYSTEM_INFO layout (36 bytes):
     *   +0:  WORD  wProcessorArchitecture
     *   +2:  WORD  wReserved
     *   +4:  DWORD dwPageSize
     *   +8:  DWORD lpMinimumApplicationAddress  (4-byte ptr!)
     *   +12: DWORD lpMaximumApplicationAddress  (4-byte ptr!)
     *   +16: DWORD dwActiveProcessorMask        (4-byte!)
     *   +20: DWORD dwNumberOfProcessors
     *   +24: DWORD dwProcessorType
     *   +28: DWORD dwAllocationGranularity
     *   +32: WORD  wProcessorLevel
     *   +34: WORD  wProcessorRevision
     */
    if (g_compat32_mode) {
        DWORD count = k32_processor_count();
        if (count > 32) count = 32;
        uint8_t *p = (uint8_t *)lpSystemInfo;
        for (int i = 0; i < 36; i++) p[i] = 0;
        *(uint16_t *)(p + 0)  = 0;     /* PROCESSOR_ARCHITECTURE_INTEL (i386) */
        *(uint32_t *)(p + 4)  = 4096;  /* dwPageSize */
        *(uint32_t *)(p + 8)  = 0x10000;    /* lpMinimumApplicationAddress */
        *(uint32_t *)(p + 12) = 0x7FFEFFFF; /* lpMaximumApplicationAddress */
        *(uint32_t *)(p + 16) = count == 32 ? UINT32_MAX
                                             : ((1U << count) - 1);
        *(uint32_t *)(p + 20) = count;
        *(uint32_t *)(p + 24) = 586;   /* dwProcessorType (PROCESSOR_INTEL_PENTIUM) */
        *(uint32_t *)(p + 28) = 65536; /* dwAllocationGranularity */
        *(uint16_t *)(p + 32) = 6;     /* wProcessorLevel */
        *(uint16_t *)(p + 34) = 0;     /* wProcessorRevision */
        return;
    }

    lpSystemInfo->wProcessorArchitecture  = 9; /* PROCESSOR_ARCHITECTURE_AMD64 */
    lpSystemInfo->dwPageSize              = 4096;
    lpSystemInfo->lpMinimumApplicationAddress = (PVOID)(ULONG_PTR)0x10000;
    lpSystemInfo->lpMaximumApplicationAddress = (PVOID)(ULONG_PTR)0x7FFFFFFEFFFF;
    DWORD count = k32_processor_count();
    lpSystemInfo->dwActiveProcessorMask   = count == 64 ? UINT64_MAX
                                                        : ((1ULL << count) - 1);
    lpSystemInfo->dwNumberOfProcessors    = count;
    lpSystemInfo->dwProcessorType         = 8664; /* AMD64 */
    lpSystemInfo->dwAllocationGranularity = 65536;
    lpSystemInfo->wProcessorLevel         = 6;
    lpSystemInfo->wProcessorRevision      = 0;
}

#define K32_RELATION_PROCESSOR_CORE    0U
#define K32_RELATION_PROCESSOR_PACKAGE 3U
#define K32_RELATION_GROUP             4U
#define K32_RELATION_ALL               0xFFFFU

static BOOL WINAPI GetLogicalProcessorInformation_k32(
    PVOID buffer, DWORD *returned_length)
{
    if (!returned_length) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD required = g_compat32_mode ? 24 : 32;
    DWORD capacity = *returned_length;
    *returned_length = required;
    if (!buffer || capacity < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }

    DWORD count = k32_processor_count();
    if (g_compat32_mode && count > 32) count = 32;
    uint64_t mask = count == 64 ? UINT64_MAX : ((1ULL << count) - 1);
    memset(buffer, 0, required);
    if (g_compat32_mode) {
        *(uint32_t *)((BYTE *)buffer + 0) = (uint32_t)mask;
        *(DWORD *)((BYTE *)buffer + 4) = K32_RELATION_PROCESSOR_CORE;
        *((BYTE *)buffer + 8) = count > 1 ? 1 : 0; /* LTP_PC_SMT */
    } else {
        *(uint64_t *)((BYTE *)buffer + 0) = mask;
        *(DWORD *)((BYTE *)buffer + 8) = K32_RELATION_PROCESSOR_CORE;
        *((BYTE *)buffer + 16) = count > 1 ? 1 : 0;
    }
    return TRUE;
}

static BOOL WINAPI GetLogicalProcessorInformationEx_k32(
    DWORD relationship, PVOID buffer, DWORD *returned_length)
{
    if (!returned_length) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    BOOL processor = relationship == K32_RELATION_PROCESSOR_CORE ||
                     relationship == K32_RELATION_PROCESSOR_PACKAGE ||
                     relationship == K32_RELATION_ALL;
    BOOL group = relationship == K32_RELATION_GROUP ||
                 relationship == K32_RELATION_ALL;
    if (!processor && !group) {
        SetLastError(87);
        return FALSE;
    }

    DWORD processor_size = g_compat32_mode ? 44 : 48;
    DWORD group_size = g_compat32_mode ? 76 : 80;
    DWORD required = (processor ? processor_size : 0) +
                     (group ? group_size : 0);
    DWORD capacity = *returned_length;
    *returned_length = required;
    if (!buffer || capacity < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }

    DWORD count = k32_processor_count();
    if (g_compat32_mode && count > 32) count = 32;
    uint64_t mask = count == 64 ? UINT64_MAX : ((1ULL << count) - 1);
    BYTE *p = (BYTE *)buffer;
    memset(p, 0, required);

    if (processor) {
        *(DWORD *)(p + 0) = relationship == K32_RELATION_PROCESSOR_PACKAGE
                          ? K32_RELATION_PROCESSOR_PACKAGE
                          : K32_RELATION_PROCESSOR_CORE;
        *(DWORD *)(p + 4) = processor_size;
        p[8] = count > 1 ? 1 : 0; /* LTP_PC_SMT */
        *(WORD *)(p + 30) = 1;    /* GroupCount */
        if (g_compat32_mode)
            *(uint32_t *)(p + 32) = (uint32_t)mask;
        else
            *(uint64_t *)(p + 32) = mask;
        p += processor_size;
    }

    if (group) {
        *(DWORD *)(p + 0) = K32_RELATION_GROUP;
        *(DWORD *)(p + 4) = group_size;
        *(WORD *)(p + 8) = 1;  /* MaximumGroupCount */
        *(WORD *)(p + 10) = 1; /* ActiveGroupCount */
        p[32] = g_compat32_mode ? 32 : 64;
        p[33] = (BYTE)count;
        if (g_compat32_mode)
            *(uint32_t *)(p + 72) = (uint32_t)mask;
        else
            *(uint64_t *)(p + 72) = mask;
    }
    return TRUE;
}

#define WIN32_NT_MAJOR 10U
#define WIN32_NT_MINOR 0U
#define WIN32_NT_BUILD 19045U

BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA lpVersionInformation)
{
    if (!lpVersionInformation) return FALSE;
    /* Windows 10 22H2, still compatible with the legacy Win32 path. */
    lpVersionInformation->dwMajorVersion = WIN32_NT_MAJOR;
    lpVersionInformation->dwMinorVersion = WIN32_NT_MINOR;
    lpVersionInformation->dwBuildNumber  = WIN32_NT_BUILD;
    lpVersionInformation->dwPlatformId   = 2; /* VER_PLATFORM_WIN32_NT */
    for (int i = 0; i < 128; i++)
        lpVersionInformation->szCSDVersion[i] = 0;
    return TRUE;
}

BOOL WINAPI GetProductInfo(DWORD major, DWORD minor, DWORD sp_major,
                           DWORD sp_minor, DWORD *product_type)
{
    (void)major;
    (void)minor;
    (void)sp_major;
    (void)sp_minor;
    if (!product_type) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    *product_type = 0x30; /* PRODUCT_PROFESSIONAL */
    return TRUE;
}

/* ── Path / Directory ──────────────────────────────────────── */

DWORD WINAPI GetFullPathNameA(PCSTR lpFileName, DWORD nBufferLength,
                              PSTR lpBuffer, PSTR *lpFilePart)
{
    char path[260];
    if (!lpFileName || !win32_normalize_path(lpFileName, path))
        return 0;
    DWORD path_len = (DWORD)strlen(path);
    DWORD len = 3 + path_len;
    if (!lpBuffer) return nBufferLength == 0 ? len + 1 : 0;
    if (len >= nBufferLength) return len + 1;
    lpBuffer[0] = 'C'; lpBuffer[1] = ':'; lpBuffer[2] = '\\';
    memcpy(lpBuffer + 3, path, path_len + 1);
    if (lpFilePart) {
        *lpFilePart = lpBuffer;
        for (DWORD i = 0; i < len; i++) {
            if (lpBuffer[i] == '\\' || lpBuffer[i] == '/')
                *lpFilePart = lpBuffer + i + 1;
        }
    }
    return len;
}

DWORD WINAPI GetCurrentDirectoryA(DWORD nBufferLength, PSTR lpBuffer)
{
    const char *current_directory = kernel32_current_directory_relative();
    DWORD cwd_len = (DWORD)strlen(current_directory);
    DWORD len = 3 + cwd_len;
    if (!lpBuffer || nBufferLength <= len) return len + 1;
    lpBuffer[0] = 'C'; lpBuffer[1] = ':'; lpBuffer[2] = '\\';
    memcpy(lpBuffer + 3, current_directory, cwd_len + 1);
    return len;
}

DWORD WINAPI GetFileAttributesA(PCSTR lpFileName)
{
    static uint32_t profile_attr_logs;
    if (!lpFileName) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        sync_last_error();
        return (DWORD)-1;
    }

    char relative[260];
    if (!win32_normalize_path(lpFileName, relative)) {
        g_last_error = 206; /* ERROR_FILENAME_EXCED_RANGE */
        sync_last_error();
        return (DWORD)-1;
    }

    bool trace_profile = false;
    if (K32_VERBOSE_DIAGNOSTICS && k32_is_profile_path(relative)) {
        uint32_t trace_index = __atomic_fetch_add(&profile_attr_logs, 1,
                                                   __ATOMIC_RELAXED);
        trace_profile = trace_index < 512;
    }

    /* Match the same normalized path semantics used by CreateFile. */
    if (k32_find_file_exact_ci(relative)) {
        if (trace_profile) {
            serial_puts("[K32-PROFILE] attr file '");
            serial_puts(relative);
            serial_puts("'\n");
        }
        g_last_error = 0;
        sync_last_error();
        return 0x80; /* FILE_ATTRIBUTE_NORMAL */
    }
    if (win32_directory_exists_normalized(relative)) {
        if (trace_profile) {
            serial_puts("[K32-PROFILE] attr dir '");
            serial_puts(relative);
            serial_puts("'\n");
        }
        g_last_error = 0;
        sync_last_error();
        return 0x10; /* FILE_ATTRIBUTE_DIRECTORY */
    }

    if (trace_profile) {
        serial_puts("[K32-PROFILE] attr missing '");
        serial_puts(relative);
        serial_puts("'\n");
    }
    g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
    sync_last_error();
    return (DWORD)-1; /* INVALID_FILE_ATTRIBUTES */
}

BOOL WINAPI SetFileAttributesA(PCSTR lpFileName, DWORD dwFileAttributes)
{
    (void)lpFileName;
    (void)dwFileAttributes;
    return TRUE;
}

BOOL WINAPI CreateDirectoryA(PCSTR lpPathName, PVOID lpSecurityAttributes)
{
    static uint32_t create_log_count;
    static uint32_t profile_dir_logs;
    (void)lpSecurityAttributes;
    char path[260];
    if (!lpPathName) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        sync_last_error();
        return FALSE;
    }
    if (!win32_normalize_path(lpPathName, path) ||
        strlen(path) >= K32_VIRTUAL_DIR_PATH) {
        g_last_error = 206; /* ERROR_FILENAME_EXCED_RANGE */
        sync_last_error();
        return FALSE;
    }
    bool trace_profile = false;
    if (K32_VERBOSE_DIAGNOSTICS && k32_is_profile_path(path)) {
        uint32_t trace_index = __atomic_fetch_add(&profile_dir_logs, 1,
                                                   __ATOMIC_RELAXED);
        trace_profile = trace_index < 512;
    }
    DWORD error = win32_directory_create_normalized(path);
    if (error) {
        if (trace_profile) {
            serial_puts("[K32-PROFILE] mkdir '");
            serial_puts(path);
            serial_puts("' error=");
            serial_putdec(error);
            serial_puts("\n");
        }
        SetLastError(error);
        return FALSE;
    }
    if (trace_profile) {
        serial_puts("[K32-PROFILE] mkdir '");
        serial_puts(path);
        serial_puts("' error=0\n");
    }
    SetLastError(0);
    if (K32_VERBOSE_DIAGNOSTICS && create_log_count < 24) {
        serial_puts("[K32-DIR] created '");
        serial_puts(path);
        serial_puts("'\n");
    } else if (K32_VERBOSE_DIAGNOSTICS && create_log_count == 24) {
        serial_puts("[K32-DIR] further creation logs suppressed\n");
    }
    create_log_count++;
    return TRUE;
}

BOOL WINAPI RemoveDirectoryA(PCSTR lpPathName)
{
    char path[260];
    if (!lpPathName) {
        g_last_error = 87;
        sync_last_error();
        return FALSE;
    }
    if (!win32_normalize_path(lpPathName, path) ||
        strlen(path) >= K32_VIRTUAL_DIR_PATH) {
        g_last_error = 206;
        sync_last_error();
        return FALSE;
    }
    if (!*path) {
        g_last_error = 5; /* ERROR_ACCESS_DENIED */
        sync_last_error();
        return FALSE;
    }
    if (k32_find_file_exact_ci(path)) {
        g_last_error = 267; /* ERROR_DIRECTORY */
        sync_last_error();
        return FALSE;
    }
    if (!win32_directory_exists_normalized(path)) {
        g_last_error = 3; /* ERROR_PATH_NOT_FOUND */
        sync_last_error();
        return FALSE;
    }
    if (k32_directory_has_file_children(path) ||
        k32_directory_has_virtual_children(path)) {
        g_last_error = 145; /* ERROR_DIR_NOT_EMPTY */
        sync_last_error();
        return FALSE;
    }
    if (osfs3_is_mounted()) {
        int result = osfs3_rmdir(path);
        if (result == -2) {
            g_last_error = 145; /* ERROR_DIR_NOT_EMPTY */
            sync_last_error();
            return FALSE;
        }
        if (result < 0) {
            g_last_error = 5; /* ERROR_ACCESS_DENIED */
            sync_last_error();
            return FALSE;
        }
    }
    k32_virtual_directory_remove(path);
    g_last_error = 0;
    sync_last_error();
    if (K32_VERBOSE_DIAGNOSTICS) {
        serial_puts("[K32-DIR] removed '");
        serial_puts(path);
        serial_puts("'\n");
    }
    return TRUE;
}

BOOL WINAPI CreateDirectoryW(PCWSTR lpPathName, PVOID lpSecurityAttributes)
{
    if (!lpPathName) return CreateDirectoryA(NULL, lpSecurityAttributes);
    char path[260];
    int i = 0;
    while (lpPathName[i] && i < 259) {
        path[i] = (char)(lpPathName[i] & 0xFF);
        i++;
    }
    if (lpPathName[i]) {
        g_last_error = 206;
        sync_last_error();
        return FALSE;
    }
    path[i] = 0;
    return CreateDirectoryA(path, lpSecurityAttributes);
}

BOOL WINAPI RemoveDirectoryW(PCWSTR lpPathName)
{
    if (!lpPathName) return RemoveDirectoryA(NULL);
    char path[260];
    int i = 0;
    while (lpPathName[i] && i < 259) {
        path[i] = (char)(lpPathName[i] & 0xFF);
        i++;
    }
    if (lpPathName[i]) {
        g_last_error = 206;
        sync_last_error();
        return FALSE;
    }
    path[i] = 0;
    return RemoveDirectoryA(path);
}

BOOL WINAPI CreateSymbolicLinkW(PCWSTR lpSymlinkFileName,
                                PCWSTR lpTargetFileName, DWORD dwFlags)
{
    (void)dwFlags;
    if (!lpSymlinkFileName || !lpTargetFileName) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    /* OsitoFS3 can read symbolic-link inodes, but its live mutation API does
     * not create them yet. Resolve the Win32 entry point and report the
     * filesystem limitation instead of leaving a NULL IAT entry. */
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return FALSE;
}

DWORD WINAPI GetCurrentDirectoryW(DWORD nBufferLength, PWSTR lpBuffer)
{
    const char *current_directory = kernel32_current_directory_relative();
    DWORD cwd_len = (DWORD)strlen(current_directory);
    DWORD len = 3 + cwd_len;
    if (!lpBuffer || nBufferLength <= len) return len + 1;
    lpBuffer[0] = 'C'; lpBuffer[1] = ':'; lpBuffer[2] = '\\';
    for (DWORD i = 0; i <= cwd_len; i++)
        lpBuffer[i + 3] = (WCHAR)(unsigned char)current_directory[i];

    const char *command = win32_current_command_line();
    if (K32_VERBOSE_DIAGNOSTICS && command &&
        process_command_contains(command, "--type=gpu-process")) {
        static uint32_t angle_getcwd_logs;
        uint32_t index = __atomic_fetch_add(&angle_getcwd_logs, 1,
                                             __ATOMIC_RELAXED);
        if (index < 16) {
            serial_puts("[K32-ANGLE] GetCurrentDirectoryW -> C:\\");
            serial_puts(current_directory);
            serial_puts("\n");
        }
    }
    return len;
}

BOOL WINAPI SetCurrentDirectoryA(PCSTR lpPathName)
{
    char path[260];
    if (!win32_normalize_path(lpPathName, path) || strlen(path) >= 260) {
        serial_puts("[K32-CWD-FAIL] normalize input='");
        serial_puts(lpPathName ? lpPathName : "(null)");
        serial_puts("'\n");
        SetLastError(lpPathName ? 206 : 87);
        return FALSE;
    }
    if (!win32_directory_exists_normalized(path)) {
        serial_puts("[K32-CWD-FAIL] missing path='C:\\");
        serial_puts(path);
        serial_puts("'\n");
        SetLastError(3); /* ERROR_PATH_NOT_FOUND */
        return FALSE;
    }
    if (!win32_set_current_directory_override(path))
        strcpy(win32_current_directory, path);
    if (!win32_refresh_current_process_parameters()) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    serial_puts("[K32] CurrentDirectory='C:\\");
    serial_puts(path);
    serial_puts("'\n");
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI SetCurrentDirectoryW(PCWSTR lpPathName)
{
    if (!lpPathName) return SetCurrentDirectoryA(NULL);
    char path[260];
    int i = 0;
    while (lpPathName[i] && i < 259) {
        path[i] = (char)(lpPathName[i] & 0xFF);
        i++;
    }
    if (lpPathName[i]) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return FALSE;
    }
    path[i] = 0;
    return SetCurrentDirectoryA(path);
}

BOOL WINAPI SetFileAttributesW(PCWSTR lpFileName, DWORD dwFileAttributes)
{
    (void)lpFileName;
    (void)dwFileAttributes;
    return TRUE;
}

/* ── System / Windows Directory (UT99) ─────────────────────── */

DWORD WINAPI GetSystemDirectoryA(PSTR lpBuffer, DWORD uSize)
{
    const char *dir = "C:\\Windows\\System32";
    DWORD len = 0;
    while (dir[len]) len++;
    if (lpBuffer && uSize > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = dir[i];
    }
    return len;
}

DWORD WINAPI GetSystemDirectoryW(PWSTR lpBuffer, DWORD uSize)
{
    static const WCHAR dir[] = {'C',':','\\','W','i','n','d','o','w','s','\\',
                                 'S','y','s','t','e','m','3','2',0};
    DWORD len = 0;
    while (dir[len]) len++;
    if (lpBuffer && uSize > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = dir[i];
    }
    return len;
}

DWORD WINAPI GetWindowsDirectoryA(PSTR lpBuffer, DWORD uSize)
{
    const char *dir = "C:\\Windows";
    DWORD len = 0;
    while (dir[len]) len++;
    if (lpBuffer && uSize > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = dir[i];
    }
    return len;
}

DWORD WINAPI GetWindowsDirectoryW(PWSTR lpBuffer, DWORD uSize)
{
    static const WCHAR dir[] = {'C',':','\\','W','i','n','d','o','w','s',0};
    DWORD len = 0;
    while (dir[len]) len++;
    if (lpBuffer && uSize > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = dir[i];
    }
    return len;
}

/* ── Find File ─────────────────────────────────────────────── */

/* ── FindFirstFile/FindNextFile backed by OsitoFS ──────────── */

#define MAX_FIND_HANDLES 8
#define FIND_SEEN_SLOTS 32768
typedef struct {
    char directory[260];
    char pattern[260];
    int  next_idx;
    int  next_virtual_idx;
    uint64_t seen[FIND_SEEN_SLOTS];
    osfs3_dir_cursor_t osfs3_cursor;
    bool use_osfs3_index;
    bool in_use;
} find_handle_t;
static find_handle_t find_handles[MAX_FIND_HANDLES];

static bool find_is_sep(char c)
{
    return c == '\\' || c == '/';
}

static char find_fold(char c)
{
    if (find_is_sep(c)) return '\\';
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static bool find_wildcard_match(const char *pattern, const char *name)
{
    const char *star = NULL;
    const char *retry = NULL;
    while (*name) {
        if (*pattern == '?' ||
            (*pattern != '*' && find_fold(*pattern) == find_fold(*name))) {
            pattern++;
            name++;
        } else if (*pattern == '*') {
            star = pattern++;
            retry = name;
        } else if (star) {
            pattern = star + 1;
            name = ++retry;
        } else {
            return false;
        }
    }
    while (*pattern == '*') pattern++;
    return *pattern == '\0';
}

/* Return the immediate virtual child represented by a flat path key. */
static bool find_path_child(const char *directory, const char *stored,
                            char child[260], bool *is_directory)
{
    const char *p = stored;
    const char *d = directory;
    while (find_is_sep(*p)) p++;
    while (*d) {
        if (!*p || find_fold(*d) != find_fold(*p)) return false;
        d++;
        p++;
    }
    if (*directory) {
        if (!find_is_sep(*p)) return false;
        while (find_is_sep(*p)) p++;
    }
    if (!*p) return false;

    int n = 0;
    while (*p && !find_is_sep(*p) && n < 259) child[n++] = *p++;
    child[n] = '\0';
    if (!n || (*p && !find_is_sep(*p))) return false;
    *is_directory = find_is_sep(*p);
    return true;
}

static uint64_t find_name_hash(const char *name)
{
    uint64_t hash = 1469598103934665603ULL;
    while (*name) {
        hash ^= (uint8_t)find_fold(*name++);
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

static bool find_child_seen(find_handle_t *state, const char *child)
{
    uint64_t fingerprint = find_name_hash(child);
    uint32_t slot = (uint32_t)fingerprint & (FIND_SEEN_SLOTS - 1);
    while (state->seen[slot]) {
        if (state->seen[slot] == fingerprint) return true;
        slot = (slot + 1) & (FIND_SEEN_SLOTS - 1);
    }
    state->seen[slot] = fingerprint;
    return false;
}

static void fill_find_data_values(LPWIN32_FIND_DATAA fd, uint64_t size,
                                  const char *name, bool is_directory)
{
    memset(fd, 0, sizeof(*fd));
    fd->dwFileAttributes = is_directory ? 0x10 : 0x80;
    fd->nFileSizeLow = (uint32_t)size;
    fd->nFileSizeHigh = (uint32_t)(size >> 32);
    int i = 0;
    while (name[i] && i < 259) { fd->cFileName[i] = name[i]; i++; }
    fd->cFileName[i] = 0;
}

static void fill_find_data_a(LPWIN32_FIND_DATAA fd, osfs2_file_t *file,
                             const char *name, bool is_directory)
{
    uint64_t size = is_directory ? 0 : osfs2_file_size(file);
    fill_find_data_values(fd, size, name, is_directory);
}

static bool find_next_entry(find_handle_t *state, LPWIN32_FIND_DATAA data)
{
    if (state->use_osfs3_index) {
        for (;;) {
            char child[260];
            bool is_directory = false;
            uint64_t size = 0;
            int result = osfs3_dir_cursor_next(
                &state->osfs3_cursor, child, sizeof(child), &is_directory,
                &size, NULL);
            if (result <= 0) break;
            if (!find_wildcard_match(state->pattern, child) ||
                find_child_seen(state, child))
                continue;
            fill_find_data_values(data, size, child, is_directory);
            return true;
        }
    } else {
        int limit = (int)osfs2_max_files();
        for (int i = state->next_idx; i < limit; i++) {
            osfs2_file_t *file = osfs2_get_file(i);
            if (!file) continue;
            char child[260];
            bool is_directory;
            if (!find_path_child(state->directory, osfs2_file_name(file), child,
                                 &is_directory) ||
                !find_wildcard_match(state->pattern, child) ||
                find_child_seen(state, child))
                continue;
            state->next_idx = i + 1;
            fill_find_data_a(data, file, child, is_directory);
            return true;
        }
        state->next_idx = limit;
    }

    for (int i = state->next_virtual_idx; i < K32_MAX_VIRTUAL_DIRS; i++) {
        char path[K32_VIRTUAL_DIR_PATH];
        char child[260];
        bool ignored;
        if (!k32_virtual_directory_copy(i, path) ||
            !find_path_child(state->directory, path, child, &ignored) ||
            !find_wildcard_match(state->pattern, child) ||
            find_child_seen(state, child))
            continue;
        state->next_virtual_idx = i + 1;
        fill_find_data_a(data, NULL, child, true);
        return true;
    }
    state->next_virtual_idx = K32_MAX_VIRTUAL_DIRS;
    return false;
}

HANDLE WINAPI FindFirstFileA(PCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    static uint32_t trace_count;
    bool trace = trace_count < 32;
    if (trace_count == 32)
        serial_puts("[K32] further FindFirstFileA logs suppressed\n");
    trace_count++;

    if (!lpFileName || !lpFindFileData) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        return INVALID_HANDLE_VALUE;
    }

    char path[260];
    if (!win32_normalize_path(lpFileName, path)) {
        g_last_error = 206; /* ERROR_FILENAME_EXCED_RANGE */
        return INVALID_HANDLE_VALUE;
    }
    const char *last_sep = NULL;
    for (const char *p = path; *p; p++)
        if (find_is_sep(*p)) last_sep = p;
    const char *pattern = last_sep ? last_sep + 1 : path;
    uint64_t directory_len = last_sep ? (uint64_t)(last_sep - path) : 0;
    uint64_t pattern_len = strlen(pattern);
    if (directory_len >= sizeof(find_handles[0].directory) ||
        !pattern_len || pattern_len >= sizeof(find_handles[0].pattern)) {
        g_last_error = 206; /* ERROR_FILENAME_EXCED_RANGE */
        return INVALID_HANDLE_VALUE;
    }

    if (trace) {
        serial_puts("[K32] FindFirstFileA: '");
        serial_puts(lpFileName);
        serial_puts("' pattern='");
        serial_puts(pattern);
        serial_puts("'\n");
    }

    int slot = -1;
    for (int i = 0; i < MAX_FIND_HANDLES; i++) {
        if (!find_handles[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        g_last_error = 4; /* ERROR_TOO_MANY_OPEN_FILES */
        return INVALID_HANDLE_VALUE;
    }

    find_handles[slot].in_use = true;
    memcpy(find_handles[slot].directory, path, directory_len);
    find_handles[slot].directory[directory_len] = 0;
    memcpy(find_handles[slot].pattern, pattern, pattern_len + 1);
    find_handles[slot].next_idx = 0;
    find_handles[slot].next_virtual_idx = 0;
    memset(find_handles[slot].seen, 0, sizeof(find_handles[slot].seen));
    find_handles[slot].use_osfs3_index = osfs3_is_mounted();
    memset(&find_handles[slot].osfs3_cursor, 0,
           sizeof(find_handles[slot].osfs3_cursor));
    if (find_handles[slot].use_osfs3_index)
        osfs3_dir_cursor_open_ci(find_handles[slot].directory,
                                 &find_handles[slot].osfs3_cursor);
    if (!find_next_entry(&find_handles[slot], lpFindFileData)) {
        find_handles[slot].in_use = false;
        if (trace) serial_puts("[K32] FindFirstFileA: no match\n");
        g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
        return INVALID_HANDLE_VALUE;
    }

    if (trace) {
        serial_puts("[K32] FindFirst result: '");
        serial_puts(lpFindFileData->cFileName);
        serial_puts("'\n");
    }

    return (HANDLE)(ULONG_PTR)(slot + 0x100);  /* offset to avoid NULL */
}

BOOL WINAPI FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
{
    int slot = (int)(ULONG_PTR)hFindFile - 0x100;
    if (slot < 0 || slot >= MAX_FIND_HANDLES || !find_handles[slot].in_use) {
        g_last_error = 6; /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (!lpFindFileData) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (!find_next_entry(&find_handles[slot], lpFindFileData)) {
        g_last_error = 18; /* ERROR_NO_MORE_FILES */
        return FALSE;
    }

    /* Log .unr results to debug Entry.unr resolution */
    {
        const char *n = lpFindFileData->cFileName;
        int len = 0; while (n[len]) len++;
        if (len > 4 && n[len-4] == '.' &&
            (n[len-3]=='u'||n[len-3]=='U') &&
            (n[len-2]=='n'||n[len-2]=='N') &&
            (n[len-1]=='r'||n[len-1]=='R')) {
            serial_puts("[K32] FindNext .unr: '");
            serial_puts(n);
            serial_puts("'\n");
        }
    }

    return TRUE;
}

BOOL WINAPI FindClose(HANDLE hFindFile)
{
    int slot = (int)(ULONG_PTR)hFindFile - 0x100;
    if (slot >= 0 && slot < MAX_FIND_HANDLES)
        find_handles[slot].in_use = false;
    return TRUE;
}

HANDLE WINAPI FindFirstFileW(PCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData)
{
    /* Convert wide to ASCII and delegate */
    char narrow[260];
    int i = 0;
    while (lpFileName[i] && i < 259) { narrow[i] = (char)lpFileName[i]; i++; }
    narrow[i] = 0;

    WIN32_FIND_DATAA fdA;
    HANDLE h = FindFirstFileA(narrow, &fdA);
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    /* Convert result to wide */
    if (lpFindFileData) {
        memset(lpFindFileData, 0, sizeof(*lpFindFileData));
        lpFindFileData->dwFileAttributes = fdA.dwFileAttributes;
        lpFindFileData->nFileSizeLow = fdA.nFileSizeLow;
        lpFindFileData->nFileSizeHigh = fdA.nFileSizeHigh;
        for (int j = 0; fdA.cFileName[j] && j < 259; j++)
            lpFindFileData->cFileName[j] = (uint16_t)fdA.cFileName[j];
    }
    return h;
}

static HANDLE WINAPI FindFirstFileExW_k32(PCWSTR file_name, DWORD info_level,
                                           PVOID find_data, DWORD search_op,
                                           PVOID search_filter, DWORD flags)
{
    (void)info_level;
    (void)search_op;
    (void)search_filter;
    (void)flags;
    if (!file_name || !find_data) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return INVALID_HANDLE_VALUE;
    }
    return FindFirstFileW(file_name, (LPWIN32_FIND_DATAW)find_data);
}

BOOL WINAPI FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData)
{
    WIN32_FIND_DATAA fdA;
    BOOL ok = FindNextFileA(hFindFile, &fdA);
    if (!ok) return FALSE;
    if (lpFindFileData) {
        memset(lpFindFileData, 0, sizeof(*lpFindFileData));
        lpFindFileData->dwFileAttributes = fdA.dwFileAttributes;
        lpFindFileData->nFileSizeLow = fdA.nFileSizeLow;
        lpFindFileData->nFileSizeHigh = fdA.nFileSizeHigh;
        for (int j = 0; fdA.cFileName[j] && j < 259; j++)
            lpFindFileData->cFileName[j] = (uint16_t)fdA.cFileName[j];
    }
    return TRUE;
}

/* ── Startup / Debug ───────────────────────────────────────── */

void WINAPI GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo)
{
    if (!lpStartupInfo) return;

    /*
     * PE32 (i386) STARTUPINFOA is 68 bytes (4-byte pointers/handles).
     * Our 64-bit version is 104 bytes (8-byte pointers/handles).
     * Must write 32-bit layout to avoid 36-byte stack overflow.
     *
     * 32-bit layout (68 bytes):
     *   +0:  DWORD cb               +4:  LPSTR lpReserved
     *   +8:  LPSTR lpDesktop        +12: LPSTR lpTitle
     *   +16: DWORD dwX              +20: DWORD dwY
     *   +24: DWORD dwXSize          +28: DWORD dwYSize
     *   +32: DWORD dwXCountChars    +36: DWORD dwYCountChars
     *   +40: DWORD dwFillAttribute  +44: DWORD dwFlags
     *   +48: WORD wShowWindow       +50: WORD cbReserved2
     *   +52: LPBYTE lpReserved2     +56: HANDLE hStdInput
     *   +60: HANDLE hStdOutput      +64: HANDLE hStdError
     */
    if (g_compat32_mode) {
        uint8_t *p = (uint8_t *)lpStartupInfo;
        for (int i = 0; i < 68; i++) p[i] = 0;
        *(uint32_t *)(p + 0) = 68;  /* cb = 32-bit sizeof */
        return;
    }

    BYTE *p = (BYTE *)lpStartupInfo;
    for (SIZE_T i = 0; i < sizeof(STARTUPINFOA); i++) p[i] = 0;
    lpStartupInfo->cb = sizeof(STARTUPINFOA);
}

BOOL WINAPI IsDebuggerPresent(void) { return FALSE; }

static BOOL WINAPI CheckRemoteDebuggerPresent_k32(HANDLE process,
                                                   BOOL *present)
{
    if (!process || !present) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    *present = FALSE;
    return TRUE;
}

#define K32_MAX_DEBUG_ATTACHMENTS 32

typedef struct {
    BOOL used;
    DWORD debugger_process_id;
    DWORD target_process_id;
} K32_DEBUG_ATTACHMENT;

static K32_DEBUG_ATTACHMENT
    k32_debug_attachments[K32_MAX_DEBUG_ATTACHMENTS];
static spinlock_t k32_debug_attachment_lock = SPINLOCK_INIT;

static BOOL k32_process_id_exists(DWORD process_id)
{
    DWORD slots = win32_process_snapshot_capacity();
    for (DWORD slot = 0; slot < slots; slot++) {
        DWORD candidate = 0;
        if (win32_process_snapshot_slot(slot, &candidate, NULL, NULL, 0) &&
            candidate == process_id)
            return TRUE;
    }
    return FALSE;
}

static uint64_t k32_debug_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&k32_debug_attachment_lock);
    return flags;
}

static void k32_debug_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&k32_debug_attachment_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static BOOL WINAPI DebugActiveProcess_k32(DWORD process_id)
{
    DWORD debugger_process_id = win32_current_process_id();
    if (!process_id || process_id == debugger_process_id ||
        !k32_process_id_exists(process_id)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    int free_slot = -1;
    uint64_t flags = k32_debug_lock_irqsave();
    for (int i = 0; i < K32_MAX_DEBUG_ATTACHMENTS; i++) {
        K32_DEBUG_ATTACHMENT *attachment = &k32_debug_attachments[i];
        if (attachment->used &&
            attachment->target_process_id == process_id) {
            k32_debug_unlock_irqrestore(flags);
            SetLastError(5); /* ERROR_ACCESS_DENIED */
            return FALSE;
        }
        if (!attachment->used && free_slot < 0)
            free_slot = i;
    }

    if (free_slot < 0) {
        k32_debug_unlock_irqrestore(flags);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    k32_debug_attachments[free_slot].used = TRUE;
    k32_debug_attachments[free_slot].debugger_process_id =
        debugger_process_id;
    k32_debug_attachments[free_slot].target_process_id = process_id;
    k32_debug_unlock_irqrestore(flags);
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI DebugActiveProcessStop_k32(DWORD process_id)
{
    DWORD debugger_process_id = win32_current_process_id();
    uint64_t flags = k32_debug_lock_irqsave();
    for (int i = 0; i < K32_MAX_DEBUG_ATTACHMENTS; i++) {
        K32_DEBUG_ATTACHMENT *attachment = &k32_debug_attachments[i];
        if (attachment->used &&
            attachment->debugger_process_id == debugger_process_id &&
            attachment->target_process_id == process_id) {
            attachment->used = FALSE;
            k32_debug_unlock_irqrestore(flags);
            SetLastError(0);
            return TRUE;
        }
    }
    k32_debug_unlock_irqrestore(flags);
    SetLastError(87); /* ERROR_INVALID_PARAMETER */
    return FALSE;
}

#define K32_MAX_EXCEPTION_FILTER_STATES 128

typedef struct {
    BOOL used;
    DWORD process_id;
    PVOID filter;
} K32_EXCEPTION_FILTER_STATE;

static K32_EXCEPTION_FILTER_STATE
    k32_exception_filters[K32_MAX_EXCEPTION_FILTER_STATES];
static spinlock_t k32_exception_filter_lock = SPINLOCK_INIT;

static inline uint64_t k32_exception_filter_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&k32_exception_filter_lock);
    return flags;
}

static inline void k32_exception_filter_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&k32_exception_filter_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static DWORD k32_exception_filter_owner(void)
{
    DWORD process_id = win32_current_process_id();
    return process_id ? process_id : 1;
}

PVOID kernel32_get_unhandled_exception_filter(void)
{
    DWORD process_id = k32_exception_filter_owner();
    PVOID filter = NULL;
    uint64_t flags = k32_exception_filter_lock_irqsave();
    for (int i = 0; i < K32_MAX_EXCEPTION_FILTER_STATES; i++) {
        if (k32_exception_filters[i].used &&
            k32_exception_filters[i].process_id == process_id) {
            filter = k32_exception_filters[i].filter;
            break;
        }
    }
    k32_exception_filter_unlock_irqrestore(flags);
    return filter;
}

void kernel32_release_process_exception_state(DWORD process_id)
{
    if (!process_id)
        return;

    uint64_t flags = k32_exception_filter_lock_irqsave();
    for (int i = 0; i < K32_MAX_EXCEPTION_FILTER_STATES; i++) {
        if (k32_exception_filters[i].used &&
            k32_exception_filters[i].process_id == process_id)
            k32_exception_filters[i].used = FALSE;
    }
    k32_exception_filter_unlock_irqrestore(flags);

    flags = k32_debug_lock_irqsave();
    for (int i = 0; i < K32_MAX_DEBUG_ATTACHMENTS; i++) {
        if (k32_debug_attachments[i].used &&
            (k32_debug_attachments[i].debugger_process_id == process_id ||
             k32_debug_attachments[i].target_process_id == process_id))
            k32_debug_attachments[i].used = FALSE;
    }
    k32_debug_unlock_irqrestore(flags);
}

PVOID WINAPI SetUnhandledExceptionFilter(PVOID lpTopLevelExceptionFilter)
{
    static uint32_t full_logs;
    DWORD process_id = k32_exception_filter_owner();
    PVOID old = NULL;
    int slot = -1;
    int free_slot = -1;
    uint64_t flags = k32_exception_filter_lock_irqsave();

    for (int i = 0; i < K32_MAX_EXCEPTION_FILTER_STATES; i++) {
        if (k32_exception_filters[i].used &&
            k32_exception_filters[i].process_id == process_id) {
            slot = i;
            break;
        }
        if (!k32_exception_filters[i].used && free_slot < 0)
            free_slot = i;
    }

    if (slot >= 0) {
        old = k32_exception_filters[slot].filter;
        if (lpTopLevelExceptionFilter) {
            k32_exception_filters[slot].filter = lpTopLevelExceptionFilter;
        } else {
            k32_exception_filters[slot].used = FALSE;
        }
    } else if (lpTopLevelExceptionFilter && free_slot >= 0) {
        k32_exception_filters[free_slot].used = TRUE;
        k32_exception_filters[free_slot].process_id = process_id;
        k32_exception_filters[free_slot].filter = lpTopLevelExceptionFilter;
    } else if (lpTopLevelExceptionFilter &&
               __atomic_fetch_add(&full_logs, 1, __ATOMIC_RELAXED) < 4) {
        serial_puts("[SEH] process filter table full\n");
    }

    k32_exception_filter_unlock_irqrestore(flags);
    return old;
}

static PVOID WINAPI AddVectoredExceptionHandler_stub(ULONG first, PVOID handler)
{
    (void)first;
    return handler;
}

static ULONG WINAPI RemoveVectoredExceptionHandler_stub(PVOID handle)
{
    return handle ? 1 : 0;
}

void WINAPI OutputDebugStringA(PCSTR lpOutputString)
{
    if (lpOutputString) {
        static uint32_t libusb_log_count;
        static const char libusb_prefix[] = "Couldn't load libusb";
        BOOL is_libusb_log = TRUE;
        for (SIZE_T i = 0; libusb_prefix[i]; i++) {
            if (lpOutputString[i] != libusb_prefix[i]) {
                is_libusb_log = FALSE;
                break;
            }
        }
        if (is_libusb_log &&
            __atomic_fetch_add(&libusb_log_count, 1, __ATOMIC_RELAXED) != 0)
            return;

        serial_puts("[DEBUG] ");
        serial_puts(lpOutputString);
        serial_puts("\n");
        if (K32_VERBOSE_DIAGNOSTICS &&
            lpOutputString[0] == 's' && lpOutputString[1] == 'r' &&
            lpOutputString[2] == 'c' && lpOutputString[3] == '\\') {
            if (!g_compat32_mode) {
                uint64_t stack_pointer;
                ULONG_PTR image_base = win32_current_image_base();
                TEB *teb = win64_current_teb();
                __asm__ volatile ("mov %%rsp, %0" : "=r"(stack_pointer));

                SIZE_T entries = 0;
                ULONG_PTR stack_limit = teb
                    ? (ULONG_PTR)teb->StackLimit : 0;
                ULONG_PTR stack_base = teb
                    ? (ULONG_PTR)teb->StackBase : 0;
                if (stack_pointer >= stack_limit &&
                    stack_pointer < stack_base) {
                    entries = (stack_base - stack_pointer) /
                              sizeof(uint64_t);
                    if (entries > 1024) entries = 1024;
                }

                serial_puts("[ASSERT64-STACK]");
                for (SIZE_T i = 0; i < entries; i++) {
                    uint64_t value = *(volatile uint64_t *)(uintptr_t)(
                        stack_pointer + i * sizeof(uint64_t));
                    LOADED_MODULE *mod =
                        dll_find_module_by_address((PVOID)value);
                    BOOL in_exe = value >= image_base &&
                                  value - image_base < 0x10000000ULL;
                    if (mod || in_exe ||
                        (value >= 0x100000000ULL &&
                         value < 0x200000000ULL)) {
                        serial_puts(" +");
                        serial_puthex(i * sizeof(uint64_t), 4);
                        serial_puts("=0x"); serial_puthex(value, 16);
                        if (mod) {
                            serial_puts("("); serial_puts(mod->name);
                            serial_puts("+0x");
                            serial_puthex(
                                value - (ULONG_PTR)mod->image.ImageBase, 8);
                            serial_puts(")");
                        } else if (in_exe) {
                            serial_puts("(exe+0x");
                            serial_puthex(value - image_base, 8);
                            serial_puts(")");
                        }
                    }
                }
                serial_puts("\n");
            } else {
                extern uint32_t compat32_get_last_user_ebp(void);
                extern TEB32 *compat32_current_teb(void);
                extern void compat32_dump_recent_calls(void);
                TEB32 *teb = compat32_current_teb();
                uint32_t stack_limit = teb ? teb->StackLimit : 0;
                uint32_t stack_base = teb ? teb->StackBase : 0;
                BOOL stack_valid = teb && stack_limit < stack_base &&
                                   stack_base >= 8;
                uint32_t frame0 = compat32_get_last_user_ebp();
                uint32_t ret0 = 0, frame1 = 0, ret1 = 0;
                uint32_t frame2 = 0, ret2 = 0;

                if (stack_valid && frame0 >= stack_limit &&
                    frame0 <= stack_base - 8) {
                    frame1 = *(volatile uint32_t *)(uintptr_t)frame0;
                    ret0 = *(volatile uint32_t *)(uintptr_t)(frame0 + 4);
                }
                if (frame1 > frame0 && frame1 >= stack_limit &&
                    frame1 <= stack_base - 8) {
                    frame2 = *(volatile uint32_t *)(uintptr_t)frame1;
                    ret1 = *(volatile uint32_t *)(uintptr_t)(frame1 + 4);
                }
                if (frame2 > frame1 && frame2 >= stack_limit &&
                    frame2 <= stack_base - 8)
                    ret2 = *(volatile uint32_t *)(uintptr_t)(frame2 + 4);
                serial_puts("[ASSERT-SITE] f0=0x");
                serial_puthex(frame0, 8);
                serial_puts(" r0=0x"); serial_puthex(ret0, 8);
                serial_puts(" f1=0x"); serial_puthex(frame1, 8);
                serial_puts(" r1=0x"); serial_puthex(ret1, 8);
                serial_puts(" f2=0x"); serial_puthex(frame2, 8);
                serial_puts(" r2=0x"); serial_puthex(ret2, 8);
                serial_puts("\n");
                compat32_dump_recent_calls();
            }
        }
    }
}

/*
 * RaiseException — Win32 wrapper around RtlRaiseException.
 */
extern void RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);

#define K32_DELAYLOAD_MODULE_NOT_FOUND 0xC06D007EU
#define K32_DELAYLOAD_PROC_NOT_FOUND   0xC06D007FU

typedef struct {
    DWORD cb;
    DWORD reserved0;
    ULONG_PTR descriptor;
    ULONG_PTR iat_entry;
    ULONG_PTR dll_name;
    DWORD import_by_name;
    DWORD reserved1;
    ULONG_PTR import;
    ULONG_PTR module;
    ULONG_PTR proc;
    DWORD last_error;
    DWORD reserved2;
} K32_DELAY_LOAD_INFO64;

typedef struct {
    DWORD cb;
    uint32_t descriptor;
    uint32_t iat_entry;
    uint32_t dll_name;
    DWORD import_by_name;
    uint32_t import;
    uint32_t module;
    uint32_t proc;
    DWORD last_error;
} K32_DELAY_LOAD_INFO32;

static BOOL k32_range_readable(PCVOID pointer, SIZE_T size)
{
    if (!size)
        return TRUE;
    if (!pointer)
        return FALSE;

    uint64_t first = (uint64_t)(ULONG_PTR)pointer;
    uint64_t last = first + size - 1;
    if (last < first)
        return FALSE;

    uint64_t first_upper = first >> 48;
    uint64_t last_upper = last >> 48;
    if (first_upper != ((first & (1ULL << 47)) ? 0xFFFFULL : 0) ||
        last_upper != ((last & (1ULL << 47)) ? 0xFFFFULL : 0))
        return FALSE;

#ifdef TEST_HARNESS
    return TRUE;
#else
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t page = first & ~0xFFFULL;
    uint64_t last_page = last & ~0xFFFULL;
    for (;;) {
        if (paging_translate_in_cr3(cr3, page) == UINT64_MAX)
            return FALSE;
        if (page == last_page)
            break;
        page += 0x1000;
    }
    return TRUE;
#endif
}

static BOOL k32_copy_readable_ascii(char *output, SIZE_T capacity, PCSTR input)
{
    if (!output || !capacity)
        return FALSE;
    output[0] = '\0';
    if (!input)
        return FALSE;

    for (SIZE_T i = 0; i + 1 < capacity; i++) {
        if (!k32_range_readable(input + i, 1))
            return FALSE;
        output[i] = input[i];
        if (!output[i])
            return TRUE;
        output[i + 1] = '\0';
    }
    return TRUE;
}

static void k32_log_delay_load_exception(DWORD code, DWORD argument_count,
                                         const ULONG_PTR *arguments)
{
    serial_puts("[K32-DELAY-EXCEPTION] code=0x");
    serial_puthex(code, 8);

    SIZE_T argument_size = g_compat32_mode ? sizeof(uint32_t)
                                           : sizeof(*arguments);
    if (!argument_count ||
        !k32_range_readable(arguments, argument_size)) {
        serial_puts(" descriptor=unavailable\n");
        return;
    }

    K32_DELAY_LOAD_INFO64 info = {0};
    ULONG_PTR source_address;
    SIZE_T source_size;
    if (g_compat32_mode) {
        source_address = *(const uint32_t *)arguments;
        source_size = sizeof(K32_DELAY_LOAD_INFO32);
    } else {
        source_address = arguments[0];
        source_size = sizeof(K32_DELAY_LOAD_INFO64);
    }

    const void *source = (const void *)source_address;
    if (!k32_range_readable(source, source_size)) {
        serial_puts(" descriptor=unreadable addr=0x");
        serial_puthex((uint64_t)source_address, g_compat32_mode ? 8 : 16);
        serial_puts("\n");
        return;
    }

    if (g_compat32_mode) {
        const K32_DELAY_LOAD_INFO32 *source32 =
            (const K32_DELAY_LOAD_INFO32 *)source;
        info.cb = source32->cb;
        info.descriptor = source32->descriptor;
        info.iat_entry = source32->iat_entry;
        info.dll_name = source32->dll_name;
        info.import_by_name = source32->import_by_name;
        info.import = source32->import;
        info.module = source32->module;
        info.proc = source32->proc;
        info.last_error = source32->last_error;
    } else {
        info = *(const K32_DELAY_LOAD_INFO64 *)source;
    }
    char dll_name[128];
    char proc_name[128];
    BOOL have_dll = k32_copy_readable_ascii(
        dll_name, sizeof(dll_name), (PCSTR)(ULONG_PTR)info.dll_name);
    BOOL have_proc = info.import_by_name && k32_copy_readable_ascii(
        proc_name, sizeof(proc_name), (PCSTR)(ULONG_PTR)info.import);

    serial_puts(" dll='");
    serial_puts(have_dll ? dll_name : "<unreadable>");
    serial_puts("' proc=");
    if (info.import_by_name) {
        serial_puts("'");
        serial_puts(have_proc ? proc_name : "<unreadable>");
        serial_puts("'");
    } else {
        serial_puts("ordinal ");
        serial_putdec((DWORD)info.import);
    }
    serial_puts(" last_error=");
    serial_putdec(info.last_error);
    serial_puts(" hmod=0x");
    serial_puthex(info.module, 16);
    serial_puts(" iat=0x");
    serial_puthex(info.iat_entry, 16);
    serial_puts("\n");
}

void WINAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags,
                           DWORD nNumberOfArguments,
                           const ULONG_PTR *lpArguments)
{
    if (dwExceptionCode == 0x406D1388) {
        /* Legacy MSVC thread-name notification. A debugger consumes this
         * exception; it is not an application fault. */
        serial_puts("[K32] legacy thread-name exception consumed\n");
        return;
    }

    if (dwExceptionCode == K32_DELAYLOAD_MODULE_NOT_FOUND ||
        dwExceptionCode == K32_DELAYLOAD_PROC_NOT_FOUND) {
        k32_log_delay_load_exception(dwExceptionCode, nNumberOfArguments,
                                     lpArguments);
    }

    EXCEPTION_RECORD rec;
    BYTE *p = (BYTE *)&rec;
    for (SIZE_T i = 0; i < sizeof(EXCEPTION_RECORD); i++) p[i] = 0;

    rec.ExceptionCode    = dwExceptionCode;
    rec.ExceptionFlags   = dwExceptionFlags;
    rec.ExceptionRecord  = NULL;
    rec.ExceptionAddress = NULL;

    if (lpArguments && nNumberOfArguments > 0) {
        if (nNumberOfArguments > EXCEPTION_MAXIMUM_PARAMETERS)
            nNumberOfArguments = EXCEPTION_MAXIMUM_PARAMETERS;
        rec.NumberParameters = nNumberOfArguments;
        for (DWORD i = 0; i < nNumberOfArguments; i++)
            rec.ExceptionInformation[i] = lpArguments[i];
    }

    RtlRaiseException(&rec);
}

static void WINAPI DebugBreak_k32(void)
{
    RaiseException(0x80000003U /* EXCEPTION_BREAKPOINT */, 0, 0, NULL);
}

/*
 * UnhandledExceptionFilter — default top-level filter.
 * Returns EXCEPTION_EXECUTE_HANDLER to terminate the process.
 */
LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS ExceptionInfo)
{
    PEXCEPTION_RECORD record = ExceptionInfo ? ExceptionInfo->ExceptionRecord : NULL;
    if (g_compat32_mode && ExceptionInfo)
        record = (PEXCEPTION_RECORD)(ULONG_PTR)*(uint32_t *)ExceptionInfo;

    if (record) {
        ULONG_PTR exception_address = g_compat32_mode
            ? *(uint32_t *)((BYTE *)record + 12)
            : (ULONG_PTR)record->ExceptionAddress;
        serial_puts("[SEH] UnhandledExceptionFilter: mode=");
        serial_puts(g_compat32_mode ? "32" : "64");
        serial_puts(" code=0x");
        serial_puthex(record->ExceptionCode, 8);
        serial_puts(" address=0x");
        serial_puthex(exception_address, g_compat32_mode ? 8 : 16);
        serial_puts("\n");
        if (!g_compat32_mode && ExceptionInfo->ContextRecord) {
            ULONG_PTR rsp = *(ULONG_PTR *)((BYTE *)ExceptionInfo->ContextRecord + 0x98);
            serial_puts("[SEH64-STACK] rsp=0x");
            serial_puthex(rsp, 16);
            for (int i = 0; i < 256; i++) {
                ULONG_PTR value = ((ULONG_PTR *)rsp)[i];
                LOADED_MODULE *module = dll_find_module_by_address((PVOID)value);
                if (!module) continue;
                serial_puts(" +");
                serial_puthex((ULONG_PTR)i * sizeof(ULONG_PTR), 4);
                serial_puts("=");
                serial_puts(module->name);
                serial_puts("+0x");
                serial_puthex(value - (ULONG_PTR)module->image.ImageBase, 8);
            }
            serial_puts("\n");
            ULONG_PTR file = *(ULONG_PTR *)(rsp + 0x220);
            serial_puts("[SEH64-LOWIO] file=0x");
            serial_puthex(file, 16);
            for (int i = 0; i < 6; i++) {
                serial_puts(" +");
                serial_puthex((ULONG_PTR)i * sizeof(ULONG_PTR), 2);
                serial_puts("=0x");
                serial_puthex(((ULONG_PTR *)file)[i], 16);
            }
            serial_puts("\n");
        }
        if (g_compat32_mode) {
            extern uint32_t compat32_get_last_user_ebp(void);
            uint32_t frame = compat32_get_last_user_ebp();
            serial_puts("[GS-EBP]");
            for (uint32_t i = 0; i < 4; i++) {
                if (frame < 0x10000 || frame >= 0x7FFF0000) break;
                uint32_t parent = *(volatile uint32_t *)(uintptr_t)frame;
                uint32_t ret = *(volatile uint32_t *)(uintptr_t)(frame + 4);
                serial_puts(" f"); serial_putdec(i);
                serial_puts("=0x"); serial_puthex(frame, 8);
                serial_puts(" r=0x"); serial_puthex(ret, 8);
                if (parent <= frame || parent >= 0x7FFF0000) break;
                frame = parent;
            }
            serial_puts("\n");
        }
        extern void compat32_dump_recent_calls(void);
        compat32_dump_recent_calls();
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ── String Conversion ─────────────────────────────────────── */

#define CP_ACP  0
#define CP_UTF8 65001

int WINAPI MultiByteToWideChar(DWORD CodePage, DWORD dwFlags,
                               PCSTR lpMultiByteStr, int cbMultiByte,
                               PWSTR lpWideCharStr, int cchWideChar)
{
    (void)CodePage;
    (void)dwFlags;

    if (!lpMultiByteStr || cbMultiByte == 0 || cbMultiByte < -1 ||
        cchWideChar < 0 ||
        (lpWideCharStr &&
         (PCVOID)lpWideCharStr == (PCVOID)lpMultiByteStr)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    int len = 0;
    if (cbMultiByte == -1) {
        while (lpMultiByteStr[len]) len++;
        len++; /* include null */
    } else {
        len = cbMultiByte;
    }

    if (cchWideChar == 0) return len; /* query size */

    if (!lpWideCharStr || cchWideChar < len) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }

    for (int i = 0; i < len; i++)
        lpWideCharStr[i] = (WCHAR)(unsigned char)lpMultiByteStr[i];

    return len;
}

static int wide_char_to_multi_byte_impl(DWORD CodePage, DWORD dwFlags,
                                        PCWSTR lpWideCharStr, int cchWideChar,
                                        PSTR lpMultiByteStr, int cbMultiByte,
                                        PCSTR lpDefaultChar, BOOL *lpUsedDefaultChar)
{
    (void)CodePage;
    (void)dwFlags;
    (void)lpDefaultChar;
    if (!lpWideCharStr || cchWideChar == 0 || cchWideChar < -1 ||
        cbMultiByte < 0 ||
        (lpMultiByteStr &&
         (PCVOID)lpMultiByteStr == (PCVOID)lpWideCharStr)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (lpUsedDefaultChar) *lpUsedDefaultChar = FALSE;

    int len = 0;
    if (cchWideChar == -1) {
        while (lpWideCharStr[len]) len++;
        len++;
    } else {
        len = cchWideChar;
    }

    if (cbMultiByte == 0) return len;

    if (!lpMultiByteStr || cbMultiByte < len) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }

    for (int i = 0; i < len; i++)
        lpMultiByteStr[i] = (char)(lpWideCharStr[i] & 0xFF);

    return len;
}

int WINAPI WideCharToMultiByte(DWORD CodePage, DWORD dwFlags,
                               PCWSTR lpWideCharStr, int cchWideChar,
                               PSTR lpMultiByteStr, int cbMultiByte,
                               PCSTR lpDefaultChar, BOOL *lpUsedDefaultChar)
{
    return wide_char_to_multi_byte_impl(CodePage, dwFlags, lpWideCharStr,
                                        cchWideChar, lpMultiByteStr,
                                        cbMultiByte, lpDefaultChar,
                                        lpUsedDefaultChar);
}


/* ── Interlocked ───────────────────────────────────────────── */

LONG WINAPI InterlockedIncrement(volatile LONG *Addend)
{
    return __atomic_add_fetch(Addend, 1, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedDecrement(volatile LONG *Addend)
{
    return __atomic_sub_fetch(Addend, 1, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedExchange(volatile LONG *Target, LONG Value)
{
    return __atomic_exchange_n(Target, Value, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedCompareExchange(volatile LONG *Dest, LONG Exchange, LONG Comparand)
{
    __atomic_compare_exchange_n(Dest, &Comparand, Exchange, FALSE,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return Comparand;
}

typedef struct {
    uint64_t alignment;
    uint64_t region;
} K32_SLIST_HEADER64;

static spinlock_t k32_slist_lock = SPINLOCK_INIT;

static inline uint64_t k32_slist_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&k32_slist_lock);
    return flags;
}

static inline void k32_slist_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&k32_slist_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

void WINAPI InitializeSListHead(PVOID list_head)
{
    if (!list_head) return;

    uint64_t flags = k32_slist_lock_irqsave();
    if (g_compat32_mode) {
        *(uint64_t *)list_head = 0;
    } else {
        K32_SLIST_HEADER64 *head = (K32_SLIST_HEADER64 *)list_head;
        head->alignment = 0;
        head->region = 1; /* HeaderX64 */
    }
    k32_slist_unlock_irqrestore(flags);
}

PVOID WINAPI InterlockedPushEntrySList(PVOID list_head, PVOID list_entry)
{
    if (!list_head || !list_entry) return NULL;

    uint64_t flags = k32_slist_lock_irqsave();
    PVOID previous;

    if (g_compat32_mode) {
        uint64_t old = *(uint64_t *)list_head;
        uint16_t depth = (uint16_t)(old >> 32);
        uint16_t sequence = (uint16_t)(old >> 48);
        previous = (PVOID)(ULONG_PTR)(uint32_t)old;
        *(uint32_t *)list_entry = (uint32_t)(ULONG_PTR)previous;
        *(uint64_t *)list_head = (uint32_t)(ULONG_PTR)list_entry |
                                 ((uint64_t)(uint16_t)(depth + 1) << 32) |
                                 ((uint64_t)(uint16_t)(sequence + 1) << 48);
    } else {
        K32_SLIST_HEADER64 *head = (K32_SLIST_HEADER64 *)list_head;
        uint16_t depth = (uint16_t)head->alignment;
        uint64_t sequence = head->alignment >> 16;
        previous = (PVOID)(ULONG_PTR)(head->region & ~0xFULL);
        *(PVOID *)list_entry = previous;
        head->alignment = (((sequence + 1) & 0xFFFFFFFFFFFFULL) << 16) |
                          (uint16_t)(depth + 1);
        head->region = ((uint64_t)(ULONG_PTR)list_entry & ~0xFULL) | 1;
    }

    k32_slist_unlock_irqrestore(flags);
    return previous;
}

static PVOID WINAPI InterlockedPopEntrySList_k32(PVOID list_head)
{
    if (!list_head) return NULL;

    uint64_t flags = k32_slist_lock_irqsave();
    PVOID entry;

    if (g_compat32_mode) {
        uint64_t old = *(uint64_t *)list_head;
        uint16_t depth = (uint16_t)(old >> 32);
        uint16_t sequence = (uint16_t)(old >> 48);
        entry = (PVOID)(ULONG_PTR)(uint32_t)old;
        if (entry) {
            uint32_t next = *(uint32_t *)entry;
            *(uint64_t *)list_head = next |
                                     ((uint64_t)(uint16_t)(depth - 1) << 32) |
                                     ((uint64_t)(uint16_t)(sequence + 1) << 48);
        }
    } else {
        K32_SLIST_HEADER64 *head = (K32_SLIST_HEADER64 *)list_head;
        uint16_t depth = (uint16_t)head->alignment;
        uint64_t sequence = head->alignment >> 16;
        entry = (PVOID)(ULONG_PTR)(head->region & ~0xFULL);
        if (entry) {
            PVOID next = *(PVOID *)entry;
            head->alignment = (((sequence + 1) & 0xFFFFFFFFFFFFULL) << 16) |
                              (uint16_t)(depth - 1);
            head->region = ((uint64_t)(ULONG_PTR)next & ~0xFULL) | 1;
        }
    }

    k32_slist_unlock_irqrestore(flags);
    return entry;
}

PVOID WINAPI InterlockedFlushSList(PVOID list_head)
{
    if (!list_head) return NULL;

    uint64_t flags = k32_slist_lock_irqsave();
    PVOID first;

    if (g_compat32_mode) {
        uint64_t old = *(uint64_t *)list_head;
        uint16_t sequence = (uint16_t)(old >> 48);
        first = (PVOID)(ULONG_PTR)(uint32_t)old;
        *(uint64_t *)list_head = (uint64_t)(uint16_t)(sequence + 1) << 48;
    } else {
        K32_SLIST_HEADER64 *head = (K32_SLIST_HEADER64 *)list_head;
        uint64_t sequence = head->alignment >> 16;
        first = (PVOID)(ULONG_PTR)(head->region & ~0xFULL);
        head->alignment = ((sequence + 1) & 0xFFFFFFFFFFFFULL) << 16;
        head->region = 1;
    }

    k32_slist_unlock_irqrestore(flags);
    return first;
}

/* ── HeapSize / HeapReAlloc (CRT sometimes needs these) ────── */

SIZE_T WINAPI HeapSize(HANDLE hHeap, DWORD dwFlags, PCVOID lpMem)
{
    (void)hHeap;
    (void)dwFlags;
    if (!lpMem) return (SIZE_T)-1;
    heap_lock_acquire();
    /* Read size header from our bump allocator */
    SIZE_T available = 0;
    const BYTE *block = heap_current_block_from_ptr(lpMem, &available, NULL);
    if (!block || !heap_block_is_allocated(block, available)) {
        heap_trace_invalid("SIZE", (PVOID)lpMem, (BYTE *)block,
                           (uint64_t)__builtin_return_address(0));
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        sync_last_error();
        heap_lock_release();
        return (SIZE_T)-1;
    }
    SIZE_T size = ((const heap_header_t *)block)->size - HEAP_HEADER_SIZE;
    heap_lock_release();
    return size;
}

PVOID WINAPI HeapReAlloc(HANDLE hHeap, DWORD dwFlags, PVOID lpMem, SIZE_T dwBytes)
{
    (void)hHeap;
    if (!lpMem) return HeapAlloc(hHeap, dwFlags, dwBytes);

    /* Check if ptr is from our heap pool (has valid header) */
    heap_lock_acquire();
    SIZE_T available = 0;
    BYTE *block = heap_current_block_from_ptr(lpMem, &available, NULL);
    if (!block || !heap_block_is_allocated(block, available)) {
        heap_trace_invalid("REALLOC", lpMem, block,
                           (uint64_t)__builtin_return_address(0));
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        sync_last_error();
        heap_lock_release();
        return NULL;
    }
    BYTE *resolved = block + HEAP_HEADER_SIZE;
    SIZE_T old_size = ((heap_header_t *)block)->size - HEAP_HEADER_SIZE;
    heap_lock_release();

    PVOID new_mem = HeapAlloc(hHeap, dwFlags, dwBytes);
    if (!new_mem) return NULL;

    SIZE_T copy = old_size < dwBytes ? old_size : dwBytes;
    BYTE *d = (BYTE *)new_mem;
    BYTE *s = resolved;
    for (SIZE_T i = 0; i < copy; i++) d[i] = s[i];
    HeapFree(hHeap, 0, lpMem);

#ifndef OK_QUIET
    /* Diagnostic: log realloc details for FName array debugging */
    serial_puts("[HEAP-RA] 0x");
    serial_puthex((uint64_t)(ULONG_PTR)lpMem, 8);
    serial_puts(" -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)new_mem, 8);
    serial_puts(" old_sz=0x");
    serial_puthex(old_size, 8);
    serial_puts(" new_sz=0x");
    serial_puthex(dwBytes, 8);
    serial_puts(" flags=0x");
    serial_puthex(dwFlags, 8);
    serial_puts(" copy=0x");
    serial_puthex(copy, 8);
    serial_puts("\n");
#endif

    return new_mem;
}

/* ── UT99 stubs: Process, Memory, System, Console ─────────── */

typedef struct _MEMORYSTATUS {
    DWORD  dwLength;
    DWORD  dwMemoryLoad;
    SIZE_T dwTotalPhys;
    SIZE_T dwAvailPhys;
    SIZE_T dwTotalPageFile;
    SIZE_T dwAvailPageFile;
    SIZE_T dwTotalVirtual;
    SIZE_T dwAvailVirtual;
} MEMORYSTATUS;

typedef struct _MEMORYSTATUSEX32 {
    DWORD     dwLength;
    DWORD     dwMemoryLoad;
    ULONGLONG ullTotalPhys;
    ULONGLONG ullAvailPhys;
    ULONGLONG ullTotalPageFile;
    ULONGLONG ullAvailPageFile;
    ULONGLONG ullTotalVirtual;
    ULONGLONG ullAvailVirtual;
    ULONGLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX32;

_Static_assert(sizeof(MEMORYSTATUSEX32) == 64,
               "PE32 MEMORYSTATUSEX layout mismatch");

void WINAPI GlobalMemoryStatus(MEMORYSTATUS *lpBuffer)
{
    if (!lpBuffer) return;
    lpBuffer->dwLength         = sizeof(MEMORYSTATUS);
    lpBuffer->dwMemoryLoad     = 25;
    /* Values must fit in 32-bit SIZE_T (UT99 reads 4 bytes).
     * 4GB = 0x100000000 overflows to 0 → "Phys=0" → no rendering. */
    lpBuffer->dwTotalPhys      = 512 * 1024 * 1024;  /* 512 MB */
    lpBuffer->dwAvailPhys      = 384 * 1024 * 1024;
    lpBuffer->dwTotalPageFile  = 1024 * 1024 * 1024;  /* 1 GB */
    lpBuffer->dwAvailPageFile  = 768 * 1024 * 1024;
    lpBuffer->dwTotalVirtual   = 2047 * 1024 * 1024;  /* ~2 GB */
    lpBuffer->dwAvailVirtual   = 1536 * 1024 * 1024;
}

static BOOL WINAPI GlobalMemoryStatusEx_k32(MEMORYSTATUSEX32 *status)
{
    extern uint64_t mem_get_total(void);
    extern uint64_t mem_get_free(void);

    if (!status || status->dwLength != sizeof(*status)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    uint64_t total = mem_get_total();
    uint64_t available = mem_get_free();
    status->dwMemoryLoad = total ? (DWORD)((total - available) * 100 / total)
                                 : 0;
    status->ullTotalPhys = total;
    status->ullAvailPhys = available;
    status->ullTotalPageFile = total;
    status->ullAvailPageFile = available;
    status->ullTotalVirtual = 0x7FF00000ULL;
    status->ullAvailVirtual = 0x60000000ULL;
    status->ullAvailExtendedVirtual = 0;
    return TRUE;
}

BOOL WINAPI SetConsoleCtrlHandler(PVOID HandlerRoutine, BOOL Add)
{
    (void)HandlerRoutine; (void)Add;
    return TRUE;
}

static HANDLE WINAPI GetConsoleWindow_k32(void)
{
    /* Win32 GUI-subsystem processes do not own a console window. */
    return NULL;
}

BOOL WINAPI GetProcessWorkingSetSize(HANDLE hProcess, SIZE_T *lpMin, SIZE_T *lpMax)
{
    (void)hProcess;
    if (lpMin) *lpMin = 204800;
    if (lpMax) *lpMax = 1413120;
    return TRUE;
}

PVOID WINAPI GlobalAlloc(UINT uFlags, SIZE_T dwBytes)
{
    DWORD heap_flags = (uFlags & 0x40 /* GMEM_ZEROINIT */)
        ? 0x00000008 /* HEAP_ZERO_MEMORY */ : 0;
    return HeapAlloc(GetProcessHeap(), heap_flags, dwBytes);
}

static PVOID WINAPI GlobalLock_k32(PVOID memory)
{
    if (!memory) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return NULL;
    }
    SetLastError(0);
    return memory;
}

static BOOL WINAPI GlobalUnlock_k32(PVOID memory)
{
    if (!memory) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    /* GlobalAlloc currently returns fixed blocks. Fixed blocks have no lock
     * count, so Windows reports FALSE with NO_ERROR when they are unlocked. */
    SetLastError(0);
    return FALSE;
}

static PVOID WINAPI GlobalFree_k32(PVOID memory)
{
    return HeapFree(GetProcessHeap(), 0, memory) ? NULL : memory;
}

PVOID WINAPI LocalAlloc(UINT uFlags, SIZE_T dwBytes)
{
    DWORD heap_flags = (uFlags & 0x40) ? 0x00000008 : 0;
    return HeapAlloc(GetProcessHeap(), heap_flags, dwBytes);
}

PVOID WINAPI LocalFree(PVOID hMem)
{
    return HeapFree(GetProcessHeap(), 0, hMem) ? NULL : hMem;
}

static WCHAR *thread_description_slot(HANDLE thread)
{
    win32_thread_ctx_t *ctx = NULL;
    if (thread == NT_CURRENT_THREAD)
        ctx = find_ctx_by_pid(proc_current_pid());
    else
        ctx = find_ctx_by_handle(thread);
    if (ctx) return ctx->description;

    extern HANDLE win32_current_process_thread_handle(void);
    if (thread == NT_CURRENT_THREAD ||
        thread == win32_current_process_thread_handle())
        return g_primary_thread_description;
    return NULL;
}

static LONG WINAPI SetThreadDescription_k32(HANDLE thread,
                                             PCWSTR description)
{
    WCHAR *slot = thread_description_slot(thread);
    if (!description) return (LONG)0x80070057; /* E_INVALIDARG */
    if (!slot) return (LONG)0x80070006; /* HRESULT_FROM_WIN32(INVALID_HANDLE) */

    int i = 0;
    while (i < 63 && description[i]) {
        slot[i] = description[i];
        i++;
    }
    slot[i] = 0;
    return 0; /* S_OK */
}

static LONG WINAPI GetThreadDescription_k32(HANDLE thread,
                                             PVOID description_out)
{
    if (!description_out) return (LONG)0x80070057; /* E_INVALIDARG */
    if (g_compat32_mode)
        *(uint32_t *)description_out = 0;
    else
        *(PWSTR *)description_out = NULL;

    WCHAR *slot = thread_description_slot(thread);
    if (!slot) return (LONG)0x80070006; /* HRESULT_FROM_WIN32(INVALID_HANDLE) */

    SIZE_T length = 0;
    while (slot[length]) length++;
    PWSTR copy = (PWSTR)LocalAlloc(0, (length + 1) * sizeof(WCHAR));
    if (!copy) return (LONG)0x80070008; /* HRESULT_FROM_WIN32(NO_MEMORY) */
    for (SIZE_T i = 0; i <= length; i++) copy[i] = slot[i];

    if (g_compat32_mode)
        *(uint32_t *)description_out = (uint32_t)(ULONG_PTR)copy;
    else
        *(PWSTR *)description_out = copy;
    return 0; /* S_OK */
}

static char process_name_fold(char c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static BOOL process_command_contains(PCSTR command, PCSTR needle)
{
    if (!command || !needle || !*needle) return FALSE;
    for (; *command; command++) {
        int i = 0;
        while (needle[i] && command[i] == needle[i]) i++;
        if (!needle[i]) return TRUE;
    }
    return FALSE;
}

static PCSTR process_command_find_ci(PCSTR command, PCSTR needle)
{
    if (!command || !needle || !*needle) return NULL;
    for (; *command; command++) {
        SIZE_T i = 0;
        while (needle[i] && command[i] &&
               process_name_fold(command[i]) == process_name_fold(needle[i]))
            i++;
        if (!needle[i]) return command;
    }
    return NULL;
}

static BOOL process_image_is_current(PCSTR app, PCSTR cmd)
{
    BOOL from_cmd = !app || !*app;
    const char *start = from_cmd ? cmd : app;
    if (!start) return FALSE;

    while (*start == ' ' || *start == '\t') start++;
    char quote = from_cmd && (*start == '"' || *start == '\'') ? *start++ : 0;
    const char *end = start;
    while (*end &&
           (quote ? *end != quote
                  : (!from_cmd || (*end != ' ' && *end != '\t'))))
        end++;

    const char *base = start;
    for (const char *p = start; p < end; p++)
        if (*p == '\\' || *p == '/') base = p + 1;

    const char *current = win32_current_exe_name();
    for (const char *p = current; *p; p++)
        if (*p == '\\' || *p == '/') current = p + 1;

    while (base < end && *current &&
           process_name_fold(*base) == process_name_fold(*current)) {
        base++;
        current++;
    }
    return base == end && *current == 0;
}

typedef struct __attribute__((packed)) {
    uint32_t process;
    uint32_t thread;
    DWORD process_id;
    DWORD thread_id;
} PROCESS_INFORMATION32;

typedef struct {
    HANDLE process;
    HANDLE thread;
    DWORD process_id;
    DWORD thread_id;
} PROCESS_INFORMATION64;

static void write_process_information(PVOID information, HANDLE process,
                                      HANDLE thread, DWORD process_id,
                                      DWORD thread_id)
{
    if (g_compat32_mode) {
        PROCESS_INFORMATION32 *pi = (PROCESS_INFORMATION32 *)information;
        pi->process = (uint32_t)(ULONG_PTR)process;
        pi->thread = (uint32_t)(ULONG_PTR)thread;
        pi->process_id = process_id;
        pi->thread_id = thread_id;
    } else {
        PROCESS_INFORMATION64 *pi = (PROCESS_INFORMATION64 *)information;
        pi->process = process;
        pi->thread = thread;
        pi->process_id = process_id;
        pi->thread_id = thread_id;
    }
}

static BOOL process_extract_image(PCSTR app, PCSTR command, char path[260])
{
    char raw[260];
    int length = 0;
    if (app && *app) {
        while (app[length] && length < 259) {
            raw[length] = app[length];
            length++;
        }
        if (app[length]) return FALSE;
    } else {
        const char *p = command;
        if (!p) return FALSE;
        while (*p == ' ' || *p == '\t') p++;
        char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
        while (*p && length < 259 &&
               (quote ? *p != quote : (*p != ' ' && *p != '\t')))
            raw[length++] = *p++;
        if (!length || (length == 259 && *p && (!quote || *p != quote)))
            return FALSE;
    }
    raw[length] = 0;
    return win32_normalize_path(raw, path);
}

static BOOL process_path_basename_is(PCSTR path, PCSTR expected)
{
    if (!path || !expected) return FALSE;
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;

    while (*base && *expected &&
           process_name_fold(*base) == process_name_fold(*expected)) {
        base++;
        expected++;
    }
    return *base == 0 && *expected == 0;
}

static PCSTR process_apply_compat_flags(PCSTR image_path, PCSTR command,
                                        char adjusted[4096])
{
    static const char hang_flag[] = " --disable-hang-monitor";
    static const char gpu_watchdog_flag[] = " --disable-gpu-watchdog";
    static const char profiler_flag[] = " --disable-stack-profiler";
    static const char angle_key[] = "--use-angle=";
    static const char full_angle[] = "--use-angle=swiftshader";
    static const char angle_flag[] = " --use-angle=swiftshader";
    static const char netlog_flag[] =
        " --log-net-log=\"C:\\System\\Program Files\\Steam\\logs\\cef_netlog.json\"";
    static const char netlog_capture_flag[] =
        " --net-log-capture-mode=Everything";
    static const char profile_path[] =
        "C:\\Users\\osito\\AppData\\Local\\Steam\\htmlcache";
    static const char profile_suffix[] = "-fresh-probe-2";
    const char *angle_pos = NULL;
    const char *profile_pos = NULL;
    SIZE_T angle_span = 0;

    if (!command ||
        !process_path_basename_is(image_path, "steamwebhelper.exe"))
        return command;

    BOOL gpu_process =
        process_command_contains(command, "--type=gpu-process");
    BOOL browser_process = !process_command_contains(command, "--type=");
    profile_pos = process_command_find_ci(command, profile_path);
    if (gpu_process) {
        for (const char *p = command; *p; p++) {
            SIZE_T i = 0;
            while (angle_key[i] && p[i] == angle_key[i]) i++;
            if (!angle_key[i]) {
                angle_pos = p;
                while (p[angle_span] && p[angle_span] != ' ' &&
                       p[angle_span] != '\t' && p[angle_span] != '"')
                    angle_span++;
                break;
            }
        }
    }

    BOOL replace_angle = gpu_process && angle_pos &&
        (angle_span != sizeof(full_angle) - 1 ||
         !process_command_contains(angle_pos, full_angle));
    BOOL add_angle = gpu_process && !angle_pos;
    BOOL add_hang =
        !process_command_contains(command, "--disable-hang-monitor");
    BOOL add_gpu_watchdog = gpu_process &&
        !process_command_contains(command, "--disable-gpu-watchdog");
    BOOL add_profiler =
        !process_command_contains(command, "--disable-stack-profiler");
    BOOL add_netlog = browser_process &&
        !process_command_contains(command, "--log-net-log=");
    BOOL add_netlog_capture = browser_process &&
        !process_command_contains(command, "--net-log-capture-mode=");
    BOOL redirect_profile = profile_pos &&
        !process_command_find_ci(profile_pos + sizeof(profile_path) - 1,
                                 profile_suffix);
    if (!replace_angle && !add_angle && !add_hang && !add_gpu_watchdog &&
        !add_profiler &&
        !add_netlog && !add_netlog_capture && !redirect_profile)
        return command;

    SIZE_T command_len = 0;
    while (command[command_len]) command_len++;
    SIZE_T adjusted_len = command_len;
    if (replace_angle)
        adjusted_len = adjusted_len - angle_span + sizeof(full_angle) - 1;
    if (add_angle) adjusted_len += sizeof(angle_flag) - 1;
    if (add_hang) adjusted_len += sizeof(hang_flag) - 1;
    if (add_gpu_watchdog)
        adjusted_len += sizeof(gpu_watchdog_flag) - 1;
    if (add_profiler) adjusted_len += sizeof(profiler_flag) - 1;
    if (add_netlog) adjusted_len += sizeof(netlog_flag) - 1;
    if (add_netlog_capture)
        adjusted_len += sizeof(netlog_capture_flag) - 1;
    if (redirect_profile) adjusted_len += sizeof(profile_suffix) - 1;
    if (adjusted_len >= 4096) {
        serial_puts("[K32-COMPAT] Steam CEF flags skipped: command too long\n");
        return command;
    }

    SIZE_T out = 0;
    for (SIZE_T i = 0; i < command_len;) {
        if (redirect_profile && command + i == profile_pos) {
            for (SIZE_T j = 0; j < sizeof(profile_path) - 1; j++)
                adjusted[out++] = command[i + j];
            for (SIZE_T j = 0; j < sizeof(profile_suffix) - 1; j++)
                adjusted[out++] = profile_suffix[j];
            i += sizeof(profile_path) - 1;
        } else if (replace_angle && command + i == angle_pos) {
            for (SIZE_T j = 0; j < sizeof(full_angle) - 1; j++)
                adjusted[out++] = full_angle[j];
            i += angle_span;
        } else {
            adjusted[out++] = command[i++];
        }
    }
    if (add_angle) {
        for (SIZE_T i = 0; i < sizeof(angle_flag) - 1; i++)
            adjusted[out++] = angle_flag[i];
    }
    if (add_hang) {
        for (SIZE_T i = 0; i < sizeof(hang_flag) - 1; i++)
            adjusted[out++] = hang_flag[i];
    }
    if (add_gpu_watchdog) {
        for (SIZE_T i = 0; i < sizeof(gpu_watchdog_flag) - 1; i++)
            adjusted[out++] = gpu_watchdog_flag[i];
    }
    if (add_profiler) {
        for (SIZE_T i = 0; i < sizeof(profiler_flag) - 1; i++)
            adjusted[out++] = profiler_flag[i];
    }
    if (add_netlog) {
        for (SIZE_T i = 0; i < sizeof(netlog_flag) - 1; i++)
            adjusted[out++] = netlog_flag[i];
    }
    if (add_netlog_capture) {
        for (SIZE_T i = 0; i < sizeof(netlog_capture_flag) - 1; i++)
            adjusted[out++] = netlog_capture_flag[i];
    }
    adjusted[out] = 0;

    if (replace_angle || add_angle)
        serial_puts("[K32-COMPAT] Steam CEF GPU process forced to full SwiftShader\n");
    if (add_gpu_watchdog)
        serial_puts("[K32-COMPAT] disabled Steam CEF GPU watchdog\n");
    if (add_hang || add_profiler)
        serial_puts("[K32-COMPAT] disabled Steam CEF hang monitor/profiler\n");
    if (add_netlog || add_netlog_capture)
        serial_puts("[K32-COMPAT] enabled Steam CEF network log\n");
    if (redirect_profile)
        serial_puts("[K32-COMPAT] Steam CEF profile redirected to htmlcache-fresh-probe-2\n");
    return adjusted;
}

static BOOL create_process_common(PCSTR app, PCSTR command, BOOL inherit_handles,
                                  DWORD flags, PCVOID environment,
                                  PCSTR current_directory,
                                  PVOID startup_info, PVOID information)
{
    if (!information) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    char normalized_directory[260];
    PCSTR directory_to_spawn = NULL;
    if (current_directory) {
        if (!win32_normalize_path(current_directory, normalized_directory)) {
            SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
            return FALSE;
        }
        if (!win32_directory_exists_normalized(normalized_directory)) {
            SetLastError(267); /* ERROR_DIRECTORY */
            return FALSE;
        }
        directory_to_spawn = normalized_directory;
    }

    /* The child runner currently supports PE64. Keep the legacy in-process
     * relaunch only for PE32; PE64 self-spawns get real waitable children. */
    if (GetCurrentProcessId() == 1 && g_compat32_mode &&
        process_image_is_current(app, command)) {
        extern BOOL win32_request_relaunch(const char *application,
                                           const char *command_line);
        if (!win32_request_relaunch(app, command)) {
            SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
            return FALSE;
        }
        write_process_information(information, NT_CURRENT_PROCESS,
                                  NT_CURRENT_THREAD, 1, 1);
        serial_puts("[K32] CreateProcess -> self RE-EXEC requested\n");
        return TRUE;
    }
    if (process_command_contains(command, "--type=crashpad-handler")) {
        /* ponytail: crash reporting is omitted until nested PE loaders isolate state. */
        HANDLE process = create_event_k32(TRUE, TRUE, NULL, 0);
        HANDLE thread = create_event_k32(TRUE, TRUE, NULL, 0);
        if (!process || !thread) {
            if (process) CloseHandle(process);
            if (thread) CloseHandle(thread);
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return FALSE;
        }
        write_process_information(information, process, thread,
                                  GetCurrentProcessId(), GetCurrentThreadId());
        SetLastError(0);
        serial_puts("[K32] CreateProcess -> skipped crashpad handler\n");
        return TRUE;
    }
    char image_path[260];
    if (!process_extract_image(app, command, image_path)) {
        SetLastError((app || command) ? 206 : 2);
        return FALSE;
    }

    char adjusted_command[4096];
    PCSTR command_to_spawn = process_apply_compat_flags(
        image_path, command && *command ? command : app, adjusted_command);

    HANDLE retained[K32_MAX_INHERITED_HANDLES];
    DWORD retained_count = 0;
    if (!process_retain_inherited_handles(startup_info, flags,
                                          inherit_handles, retained,
                                          &retained_count))
        return FALSE;

    extern NTSTATUS win32_spawn_child(const char *, const char *, const char *,
                                      PCVOID, PHANDLE, PHANDLE, DWORD *, DWORD *,
                                      const HANDLE *, DWORD, DWORD);
    HANDLE process = NULL;
    HANDLE thread = NULL;
    DWORD process_id = 0;
    DWORD thread_id = 0;
    NTSTATUS status = win32_spawn_child(
        image_path, command_to_spawn, directory_to_spawn, environment,
        &process, &thread, &process_id, &thread_id,
        retained, retained_count, flags);
    if (!NT_SUCCESS(status)) {
        serial_puts("[K32] CreateProcess -> failed status=0x");
        serial_puthex((uint32_t)status, 8);
        serial_puts(" image=");
        serial_puts(image_path);
        serial_puts("\n");
        set_last_error_from_status(status);
        return FALSE;
    }

    write_process_information(information, process, thread,
                              process_id, thread_id);
    SetLastError(0);
    serial_puts("[K32] CreateProcess -> scheduled ");
    serial_puts(image_path);
    serial_puts(" pid=");
    serial_putdec(process_id);
    serial_puts("\n");
    return TRUE;
}

BOOL WINAPI CreateProcessA(PCSTR lpApp, PSTR lpCmd, PVOID a, PVOID b,
                            BOOL c, DWORD d, PVOID e, PCSTR f, PVOID g, PVOID h)
{
    (void)a; (void)b;
    serial_puts("[K32] CreateProcessA: app=");
    if (lpApp) serial_puts(lpApp);
    serial_puts(" cmd=");
    if (lpCmd) serial_puts(lpCmd);
    serial_puts("\n");
    return create_process_common(lpApp, lpCmd, c, d, e, f, g, h);
}

BOOL WINAPI CreateProcessW(PCWSTR lpApp, PWSTR lpCmd, PVOID a, PVOID b,
                            BOOL c, DWORD d, PVOID e, PCWSTR f, PVOID g, PVOID h)
{
    (void)a; (void)b;
    char app_ascii[260];
    char cmd_ascii[4096];
    char directory_ascii[260];
    int app_len = 0;
    int cmd_len = 0;
    int directory_len = 0;
    if (lpApp)
        while (lpApp[app_len] && app_len < 259) {
            app_ascii[app_len] = (char)(lpApp[app_len] & 0xFF);
            app_len++;
        }
    if (lpCmd)
        while (lpCmd[cmd_len] && cmd_len < 4095) {
            cmd_ascii[cmd_len] = (char)(lpCmd[cmd_len] & 0xFF);
            cmd_len++;
        }
    if (f)
        while (f[directory_len] && directory_len < 259) {
            directory_ascii[directory_len] =
                (char)(f[directory_len] & 0xFF);
            directory_len++;
        }
    app_ascii[app_len] = 0;
    cmd_ascii[cmd_len] = 0;
    directory_ascii[directory_len] = 0;

    if ((lpApp && lpApp[app_len]) || (lpCmd && lpCmd[cmd_len]) ||
        (f && f[directory_len])) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        serial_puts("[K32] CreateProcessW: command line too long\n");
        return FALSE;
    }

    serial_puts("[K32] CreateProcessW: app=");
    if (lpApp) serial_puts(app_ascii);
    serial_puts(" cmd=");
    if (lpCmd) serial_puts(cmd_ascii);
    serial_puts("\n");
    return create_process_common(lpApp ? app_ascii : NULL,
                                 lpCmd ? cmd_ascii : NULL, c, d,
                                 e, f ? directory_ascii : NULL, g, h);
}

#define K32_FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100U
#define K32_FORMAT_MESSAGE_FROM_STRING     0x00000400U
#define K32_FORMAT_MESSAGE_FROM_SYSTEM     0x00001000U

static PCSTR format_message_system_text(DWORD message_id)
{
    switch (message_id) {
    case 0:     return "The operation completed successfully.\r\n";
    case 2:     return "The system cannot find the file specified.\r\n";
    case 3:     return "The system cannot find the path specified.\r\n";
    case 5:     return "Access is denied.\r\n";
    case 6:     return "The handle is invalid.\r\n";
    case 8:     return "Not enough memory resources are available.\r\n";
    case 87:    return "The parameter is incorrect.\r\n";
    case 120:   return "This function is not supported on this system.\r\n";
    case 122:   return "The data area passed to a system call is too small.\r\n";
    case 183:   return "Cannot create a file when that file already exists.\r\n";
    case 995:   return "The I/O operation has been aborted.\r\n";
    case 996:   return "Overlapped I/O event is not in a signaled state.\r\n";
    case 997:   return "Overlapped I/O operation is in progress.\r\n";
    case 10014: return "The system detected an invalid pointer address.\r\n";
    case 10022: return "An invalid argument was supplied.\r\n";
    case 10024: return "Too many open sockets.\r\n";
    case 10035: return "A non-blocking socket operation could not be completed immediately.\r\n";
    case 10038: return "An operation was attempted on something that is not a socket.\r\n";
    case 10045: return "The attempted operation is not supported for this object.\r\n";
    case 10047: return "An address incompatible with the requested protocol was used.\r\n";
    case 10048: return "Only one usage of each socket address is normally permitted.\r\n";
    case 10054: return "An existing connection was forcibly closed by the remote host.\r\n";
    case 10055: return "No buffer space is available.\r\n";
    case 10057: return "A request was made on a socket that is not connected.\r\n";
    case 10060: return "A connection attempt failed because the connected party did not respond.\r\n";
    case 10061: return "No connection could be made because the target machine refused it.\r\n";
    case 10091: return "The network subsystem is unavailable.\r\n";
    case 10093: return "A successful WSAStartup call must occur before this operation.\r\n";
    case 10109: return "The specified class was not found.\r\n";
    case 11001: return "No such host is known.\r\n";
    case 11002: return "This is usually a temporary error during hostname resolution.\r\n";
    case 11003: return "A non-recoverable error occurred during a database lookup.\r\n";
    case 11004: return "The requested name is valid, but no data was found.\r\n";
    default:    return NULL;
    }
}

static DWORD format_message_ascii_length(PCSTR message)
{
    DWORD length = 0;
    while (message && message[length]) length++;
    return length;
}

static DWORD format_message_wide_length(PCWSTR message)
{
    DWORD length = 0;
    while (message && message[length]) length++;
    return length;
}

static PSTR format_message_buffer_a(PSTR buffer, DWORD flags, DWORD size,
                                    DWORD length)
{
    if (flags & K32_FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        PSTR *output = (PSTR *)(PVOID)buffer;
        DWORD capacity = size > length + 1 ? size : length + 1;
        if (!output) {
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            return NULL;
        }
        *output = (PSTR)LocalAlloc(0, capacity);
        if (!*output) {
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        return *output;
    }
    if (!buffer || size <= length) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return NULL;
    }
    return buffer;
}

static PWSTR format_message_buffer_w(PWSTR buffer, DWORD flags, DWORD size,
                                     DWORD length)
{
    if (flags & K32_FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        PWSTR *output = (PWSTR *)(PVOID)buffer;
        DWORD capacity = size > length + 1 ? size : length + 1;
        if (!output) {
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            return NULL;
        }
        *output = (PWSTR)LocalAlloc(0, (SIZE_T)capacity * sizeof(WCHAR));
        if (!*output) {
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        return *output;
    }
    if (!buffer || size <= length) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return NULL;
    }
    return buffer;
}

DWORD WINAPI FormatMessageA(DWORD dwFlags, PCVOID lpSource, DWORD dwMessageId,
                             DWORD dwLanguageId, PSTR lpBuffer, DWORD nSize,
                             PVOID Arguments)
{
    (void)dwLanguageId;
    (void)Arguments;
    PCSTR message = (dwFlags & K32_FORMAT_MESSAGE_FROM_STRING)
                        ? (PCSTR)lpSource
                        : ((dwFlags & K32_FORMAT_MESSAGE_FROM_SYSTEM)
                               ? format_message_system_text(dwMessageId)
                               : NULL);
    if (!message) {
        SetLastError(317); /* ERROR_MR_MID_NOT_FOUND */
        return 0;
    }

    DWORD length = format_message_ascii_length(message);
    PSTR output = format_message_buffer_a(lpBuffer, dwFlags, nSize, length);
    if (!output) return 0;
    for (DWORD i = 0; i <= length; i++) output[i] = message[i];
    SetLastError(0);
    return length;
}

DWORD WINAPI FormatMessageW(DWORD dwFlags, PCVOID lpSource, DWORD dwMessageId,
                              DWORD dwLanguageId, PWSTR lpBuffer, DWORD nSize,
                              PVOID Arguments)
{
    (void)dwLanguageId;
    (void)Arguments;
    PCWSTR wide_message = (dwFlags & K32_FORMAT_MESSAGE_FROM_STRING)
                              ? (PCWSTR)lpSource
                              : NULL;
    PCSTR ascii_message = !wide_message &&
                                  (dwFlags & K32_FORMAT_MESSAGE_FROM_SYSTEM)
                              ? format_message_system_text(dwMessageId)
                              : NULL;
    if (!wide_message && !ascii_message) {
        SetLastError(317); /* ERROR_MR_MID_NOT_FOUND */
        return 0;
    }

    DWORD length = wide_message ? format_message_wide_length(wide_message)
                                : format_message_ascii_length(ascii_message);
    PWSTR output = format_message_buffer_w(lpBuffer, dwFlags, nSize, length);
    if (!output) return 0;
    for (DWORD i = 0; i < length; i++)
        output[i] = wide_message ? wide_message[i]
                                 : (WCHAR)(unsigned char)ascii_message[i];
    output[length] = 0;
    SetLastError(0);
    return length;
}

BOOL WINAPI GetComputerNameA(PSTR lpBuffer, DWORD *nSize)
{
    const char *name = "OSITOK";
    DWORD len = 6;
    if (!lpBuffer || !nSize || *nSize <= len) {
        if (nSize) *nSize = len + 1;
        return FALSE;
    }
    for (DWORD i = 0; i <= len; i++) lpBuffer[i] = name[i];
    *nSize = len;
    return TRUE;
}

BOOL WINAPI GetComputerNameW(PWSTR lpBuffer, DWORD *nSize)
{
    static const WCHAR name[] = {'O','S','I','T','O','K',0};
    DWORD len = 6;
    if (!lpBuffer || !nSize || *nSize <= len) {
        if (nSize) *nSize = len + 1;
        return FALSE;
    }
    for (DWORD i = 0; i <= len; i++) lpBuffer[i] = name[i];
    *nSize = len;
    return TRUE;
}

BOOL WINAPI GetComputerNameExA(DWORD name_type, PSTR lpBuffer, DWORD *nSize)
{
    if (name_type >= 8 || !nSize) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (name_type == 2 || name_type == 6) {
        if (!lpBuffer || *nSize < 1) {
            *nSize = 1;
            SetLastError(234); /* ERROR_MORE_DATA */
            return FALSE;
        }
        lpBuffer[0] = 0;
        *nSize = 0;
        return TRUE;
    }
    if (!GetComputerNameA(lpBuffer, nSize)) {
        SetLastError(234);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI GetComputerNameExW(DWORD name_type, PWSTR lpBuffer, DWORD *nSize)
{
    if (name_type >= 8 || !nSize) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (name_type == 2 || name_type == 6) {
        if (!lpBuffer || *nSize < 1) {
            *nSize = 1;
            SetLastError(234); /* ERROR_MORE_DATA */
            return FALSE;
        }
        lpBuffer[0] = 0;
        *nSize = 0;
        return TRUE;
    }
    if (!GetComputerNameW(lpBuffer, nSize)) {
        SetLastError(234);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI GetExitCodeProcess(HANDLE hProcess, DWORD *lpExitCode)
{
    if (!lpExitCode) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (hProcess == NT_CURRENT_PROCESS) {
        *lpExitCode = 259; /* STILL_ACTIVE */
        return TRUE;
    }
    extern BOOL nt_process_exit_code(HANDLE handle, DWORD *exit_code);
    if (!nt_process_exit_code(hProcess, lpExitCode)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    return TRUE;
}

typedef struct _DYNAMIC_TIME_ZONE_INFORMATION {
    LONG Bias;
    WCHAR StandardName[32];
    SYSTEMTIME StandardDate;
    LONG StandardBias;
    WCHAR DaylightName[32];
    SYSTEMTIME DaylightDate;
    LONG DaylightBias;
    WCHAR TimeZoneKeyName[128];
    BYTE DynamicDaylightTimeDisabled;
} DYNAMIC_TIME_ZONE_INFORMATION;

_Static_assert(sizeof(DYNAMIC_TIME_ZONE_INFORMATION) == 432,
               "DYNAMIC_TIME_ZONE_INFORMATION must match the Win32 ABI");

void WINAPI GetLocalTime(SYSTEMTIME *lpSystemTime)
{
    /* The current OsitoK time-zone policy is UTC (zero bias), so local and
     * system time differ only once a configurable zone database is added. */
    k32_get_system_time(lpSystemTime);
}

static int k32_copy_wide_result(const WCHAR *value, PWSTR output,
                                int output_chars)
{
    int required = 1;
    while (value[required - 1]) required++;
    if (!output && output_chars == 0) return required;
    if (!output || output_chars < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    for (int i = 0; i < required; i++) output[i] = value[i];
    return required;
}

static int WINAPI GetDateFormatW_k32(DWORD locale, DWORD flags,
                                      const SYSTEMTIME *date, PCWSTR format,
                                      PWSTR output, int output_chars)
{
    static const WCHAR value[] = {
        '0','8','/','2','1','/','2','0','2','6',0
    };
    (void)locale;
    (void)flags;
    (void)date;
    (void)format;
    return k32_copy_wide_result(value, output, output_chars);
}

static int WINAPI GetTimeFormatW_k32(DWORD locale, DWORD flags,
                                      const SYSTEMTIME *time, PCWSTR format,
                                      PWSTR output, int output_chars)
{
    static const WCHAR value[] = {'1','2',':','0','0',':','0','0',0};
    (void)locale;
    (void)flags;
    (void)time;
    (void)format;
    return k32_copy_wide_result(value, output, output_chars);
}

static int WINAPI GetDateFormatEx_k32(PCWSTR locale_name, DWORD flags,
                                       const SYSTEMTIME *date, PCWSTR format,
                                       PWSTR output, int output_chars,
                                       PCWSTR calendar)
{
    (void)locale_name;
    (void)calendar;
    return GetDateFormatW_k32(0x0409, flags, date, format,
                              output, output_chars);
}

static int WINAPI GetTimeFormatEx_k32(PCWSTR locale_name, DWORD flags,
                                       const SYSTEMTIME *time, PCWSTR format,
                                       PWSTR output, int output_chars)
{
    (void)locale_name;
    return GetTimeFormatW_k32(0x0409, flags, time, format,
                              output, output_chars);
}

DWORD WINAPI GetVersion(void)
{
    /* Windows 10 22H2: major=10, minor=0, build=19045 */
    return (WIN32_NT_BUILD << 16) | (WIN32_NT_MINOR << 8) | WIN32_NT_MAJOR;
}

BOOL WINAPI GetVersionExW(PVOID lpVersionInformation)
{
    if (!lpVersionInformation) return FALSE;
    BYTE *p = (BYTE *)lpVersionInformation;
    /* Zero everything first (at least 276 bytes for OSVERSIONINFOW) */
    for (int i = 0; i < 276; i++) p[i] = 0;
    *(DWORD *)(p + 0)  = 276;  /* dwOSVersionInfoSize */
    *(DWORD *)(p + 4)  = WIN32_NT_MAJOR; /* dwMajorVersion */
    *(DWORD *)(p + 8)  = WIN32_NT_MINOR; /* dwMinorVersion */
    *(DWORD *)(p + 12) = WIN32_NT_BUILD; /* dwBuildNumber */
    *(DWORD *)(p + 16) = 2;    /* dwPlatformId = VER_PLATFORM_WIN32_NT */
    return TRUE;
}

#define K32_VER_EQUAL         1U
#define K32_VER_GREATER       2U
#define K32_VER_GREATER_EQUAL 3U
#define K32_VER_LESS          4U
#define K32_VER_LESS_EQUAL    5U
#define K32_VER_AND           6U
#define K32_VER_OR            7U

static BOOL version_field_matches(uint32_t current, uint32_t requested,
                                  uint32_t condition)
{
    switch (condition) {
    case K32_VER_EQUAL:         return current == requested;
    case K32_VER_GREATER:       return current > requested;
    case K32_VER_GREATER_EQUAL: return current >= requested;
    case K32_VER_LESS:          return current < requested;
    case K32_VER_LESS_EQUAL:    return current <= requested;
    case K32_VER_AND:           return (current & requested) == requested;
    case K32_VER_OR:            return (current & requested) != 0;
    default:                     return FALSE;
    }
}

static uint64_t WINAPI VerSetConditionMask_k32(uint32_t mask_lo,
                                               uint32_t mask_hi,
                                               DWORD type_mask,
                                               DWORD condition)
{
    uint64_t mask = ((uint64_t)mask_hi << 32) | mask_lo;
    condition &= 7U;
    for (uint32_t bit = 0; bit < 8; bit++) {
        if (type_mask & (1U << bit)) {
            uint32_t shift = bit * 3;
            mask = (mask & ~(7ULL << shift)) |
                   ((uint64_t)condition << shift);
        }
    }
    return mask;
}

static uint64_t WINAPI VerSetConditionMask_k64(uint64_t mask,
                                                DWORD type_mask,
                                                BYTE condition)
{
    return VerSetConditionMask_k32((uint32_t)mask, (uint32_t)(mask >> 32),
                                   type_mask, condition);
}

static BOOL verify_version_info(PVOID version_info, DWORD type_mask,
                                uint64_t mask, BOOL wide)
{
    if (!version_info || !type_mask || (type_mask & ~0xFFU)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    const BYTE *p = (const BYTE *)version_info;
    DWORD size = *(const DWORD *)p;
    DWORD extended_size = wide ? 284 : 156;
    if (size < 20 || ((type_mask & 0xF0U) && size < extended_size)) {
        SetLastError(87);
        return FALSE;
    }

    uint32_t current[8] = {
        WIN32_NT_MINOR, WIN32_NT_MAJOR, WIN32_NT_BUILD, 2,
        0, 0, 0, 1 /* VER_NT_WORKSTATION */
    };
    uint32_t requested[8] = {
        *(const DWORD *)(p + 8), *(const DWORD *)(p + 4),
        *(const DWORD *)(p + 12), *(const DWORD *)(p + 16),
        0, 0, 0, 0
    };
    if (size >= extended_size) {
        DWORD extended = wide ? 276 : 148;
        requested[4] = *(const WORD *)(p + extended + 2);
        requested[5] = *(const WORD *)(p + extended);
        requested[6] = *(const WORD *)(p + extended + 4);
        requested[7] = *(const BYTE *)(p + extended + 6);
    }

    for (uint32_t bit = 0; bit < 8; bit++) {
        if ((type_mask & (1U << bit)) &&
            !version_field_matches(current[bit], requested[bit],
                                   (uint32_t)((mask >> (bit * 3)) & 7U))) {
            SetLastError(1150); /* ERROR_OLD_WIN_VERSION */
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL WINAPI VerifyVersionInfoW_k32(PVOID version_info, DWORD type_mask,
                                           uint32_t mask_lo, uint32_t mask_hi)
{
    return verify_version_info(version_info, type_mask,
                               ((uint64_t)mask_hi << 32) | mask_lo, TRUE);
}

static BOOL WINAPI VerifyVersionInfoA_k32(PVOID version_info, DWORD type_mask,
                                           uint32_t mask_lo, uint32_t mask_hi)
{
    return verify_version_info(version_info, type_mask,
                               ((uint64_t)mask_hi << 32) | mask_lo, FALSE);
}

static BOOL WINAPI VerifyVersionInfoW_k64(PVOID version_info, DWORD type_mask,
                                           uint64_t mask)
{
    return verify_version_info(version_info, type_mask, mask, TRUE);
}

static BOOL WINAPI VerifyVersionInfoA_k64(PVOID version_info, DWORD type_mask,
                                           uint64_t mask)
{
    return verify_version_info(version_info, type_mask, mask, FALSE);
}
BOOL WINAPI TerminateProcess(HANDLE hProcess, UINT uExitCode)
{
    const char *command = win32_current_command_line();
    BOOL mojo_process = command &&
        process_command_contains(command, "--mojo-platform-channel-handle=");
    if (mojo_process) {
        serial_puts("[MOJO-EXIT] pid=");
        serial_putdec(win32_current_process_id());
        serial_puts(" process=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hProcess, 16);
        serial_puts(" code=");
        serial_putdec(uExitCode);
        serial_puts(" caller=0x");
        serial_puthex((uint64_t)__builtin_return_address(0), 16);
        serial_puts("\n");

        extern void proc_debug_dump_pid(uint32_t pid);
        extern void ntsync_debug_dump_process_events(ULONG owner_pid);
        DWORD owner_pid = win32_current_process_id();
        ntsync_debug_dump_process_events(owner_pid);
        proc_debug_dump_pid((uint32_t)proc_current_pid());
        for (int i = 0; i < g_win32_thread_capacity; i++) {
            win32_thread_ctx_t *ctx = &g_win32_threads[i];
            if (!ctx->active || ctx->terminated)
                continue;
            if (ctx->owner_pid != owner_pid)
                continue;
            serial_puts("[MOJO-THREAD] tid=");
            serial_putdec(ctx->tid);
            serial_puts(" kpid=");
            serial_putdec((uint64_t)(uint32_t)ctx->kernel_pid);
            serial_puts(" entry=0x");
            serial_puthex((uint64_t)ctx->func_addr, 16);
            serial_puts("\n");
            proc_debug_dump_pid((uint32_t)ctx->kernel_pid);
        }
    }
    if (hProcess == NT_CURRENT_PROCESS || !hProcess) {
        ExitProcess(uExitCode);
        return TRUE;
    }
    extern BOOL win32_terminate_child(HANDLE, NTSTATUS);
    if (win32_terminate_child(hProcess, (NTSTATUS)uExitCode)) return TRUE;
    SetLastError(6); /* ERROR_INVALID_HANDLE */
    return FALSE;
}

HANDLE WINAPI HeapCreate(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize)
{
    (void)flOptions; (void)dwInitialSize; (void)dwMaximumSize;
    return (HANDLE)(ULONG_PTR)0xBEEF0002;
}

BOOL WINAPI HeapDestroy(HANDLE hHeap)
{
    (void)hHeap;
    return TRUE;
}

BOOL WINAPI HeapValidate(HANDLE hHeap, DWORD dwFlags, PCVOID lpMem)
{
    (void)dwFlags;
    ULONG_PTR heap_value = (ULONG_PTR)hHeap;
    if (heap_value != 0xBEEF0001 && heap_value != 0xBEEF0002) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (!lpMem) return TRUE;

    heap_lock_acquire();
    SIZE_T available = 0;
    BYTE *block = heap_current_block_from_ptr(lpMem, &available, NULL);
    BOOL valid = block && heap_block_is_allocated(block, available);
    heap_lock_release();
    if (!valid)
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
    return valid;
}

static BOOL k32_heap_handle_valid(HANDLE heap)
{
    ULONG_PTR value = (ULONG_PTR)heap;
    return value == 0xBEEF0001 || value == 0xBEEF0002;
}

static DWORD WINAPI GetProcessHeaps_k32(DWORD capacity, PVOID heaps)
{
    if (capacity && !heaps) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (capacity) {
        HANDLE process_heap = GetProcessHeap();
        if (g_compat32_mode)
            *(uint32_t *)heaps = (uint32_t)(ULONG_PTR)process_heap;
        else
            *(HANDLE *)heaps = process_heap;
    }
    SetLastError(0);
    return 1;
}

static BOOL WINAPI HeapLock_k32(HANDLE heap)
{
    if (!k32_heap_handle_valid(heap)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    heap_lock_acquire();
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI HeapUnlock_k32(HANDLE heap)
{
    if (!k32_heap_handle_valid(heap)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (!heap_lock_owned_by_current()) {
        SetLastError(158); /* ERROR_NOT_LOCKED */
        return FALSE;
    }
    heap_lock_release();
    SetLastError(0);
    return TRUE;
}

static void k32_store_size_t(PVOID destination, SIZE_T value)
{
    if (!destination)
        return;
    if (g_compat32_mode)
        *(uint32_t *)destination = (uint32_t)value;
    else
        *(SIZE_T *)destination = value;
}

static BOOL WINAPI HeapQueryInformation_k32(HANDLE heap, DWORD info_class,
                                             PVOID info, SIZE_T info_size,
                                             PVOID return_size)
{
    if (heap && !k32_heap_handle_valid(heap)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    if (info_class == 0 /* HeapCompatibilityInformation */) {
        k32_store_size_t(return_size, sizeof(DWORD));
        if (!info || info_size < sizeof(DWORD)) {
            SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
            return FALSE;
        }
        *(DWORD *)info = 0; /* Standard heap, not LFH. */
        SetLastError(0);
        return TRUE;
    }
    if (info_class == 1 /* HeapEnableTerminationOnCorruption */) {
        k32_store_size_t(return_size, 0);
        SetLastError(0);
        return TRUE;
    }

    k32_store_size_t(return_size, 0);
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return FALSE;
}

static BOOL WINAPI HeapSetInformation_k32(HANDLE heap, DWORD info_class,
                                           PVOID info, SIZE_T info_size)
{
    if (heap && !k32_heap_handle_valid(heap)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (info_class == 1 /* HeapEnableTerminationOnCorruption */) {
        SetLastError(0);
        return TRUE;
    }
    if (info_class == 0 /* HeapCompatibilityInformation */ && info &&
        info_size >= sizeof(DWORD)) {
        DWORD mode = *(DWORD *)info;
        if (mode == 0 || mode == 2) {
            SetLastError(0);
            return TRUE;
        }
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (info_class == 3 /* HeapOptimizeResources */) {
        SetLastError(0);
        return TRUE;
    }
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return FALSE;
}

void WINAPI __attribute__((noreturn)) ExitThread(DWORD dwExitCode)
{
    int kernel_pid = proc_current_pid();
    win32_thread_ctx_t *ctx = find_ctx_by_pid(kernel_pid);

    if (!ctx) {
        /* The primary PE thread is not represented in g_win32_threads.  Until
         * it becomes a separately schedulable NT thread, exiting it must tear
         * down the process so worker threads cannot outlive their address
         * space. */
        ExitProcess(dwExitCode);
        __builtin_unreachable();
    }

    serial_puts("[K32-THREAD] ExitThread tid=");
    serial_putdec(ctx->tid);
    serial_puts(" kpid=");
    serial_putdec((uint64_t)(uint32_t)kernel_pid);
    serial_puts(" code=0x");
    serial_puthex(dwExitCode, 8);
    serial_puts("\n");

    /* Windows runs per-DLL thread detach notifications for ExitThread (but
     * not for forced TerminateThread).  Do this while the thread's TEB and
     * stack are still valid. */
    if (ctx->compat32) {
        dll_notify_thread(DLL_THREAD_DETACH);
        win32_tls_detach_thread();
        win32_thread_release_tls32_environment(ctx);
    } else {
        dll_notify_thread(DLL_THREAD_DETACH);
        win64_tls_detach_thread(ctx, TRUE);
    }
    ctx->exit_code = dwExitCode;

    /* A PE64 worker entered through win32_thread_entry_common has a saved
     * continuation on the scheduler stack. Unwind there so stack release and
     * NT object signaling follow the same order as a normal thread return. */
    if (!ctx->compat32 &&
        __atomic_load_n(&ctx->exit_jmp_ready, __ATOMIC_ACQUIRE)) {
        kern_longjmp(ctx->exit_jmpbuf, 1);
        __builtin_unreachable();
    }

    /* Compat32 still needs its callback gate to unwind before its PE stack can
     * be reclaimed. Keep the context active for the scheduler reaper rather
     * than publishing termination from a live INT 0x2E frame. */
    proc_exit((int32_t)dwExitCode);
    __builtin_unreachable();
}

static void WINAPI __attribute__((noreturn))
FreeLibraryAndExitThread_k32(HANDLE module, DWORD exit_code)
{
    FreeLibrary(module);
    ExitThread(exit_code);
    __builtin_unreachable();
}

BOOL WINAPI SetErrorMode(UINT uMode)
{
    (void)uMode;
    return 0;
}

UINT WINAPI SetHandleCount(UINT uNumber)
{
    return uNumber; /* no-op, return requested count */
}

BOOL WINAPI SetStdHandle(DWORD nStdHandle, HANDLE hHandle)
{
    (void)nStdHandle; (void)hHandle;
    return TRUE;
}

BOOL WINAPI FlushFileBuffers(HANDLE hFile)
{
    (void)hFile;
    return TRUE;
}

static BOOL WINAPI DeviceIoControl_k32(HANDLE device, DWORD control_code,
                                       PVOID input, DWORD input_size,
                                       PVOID output, DWORD output_size,
                                       DWORD *bytes_returned,
                                       PVOID overlapped)
{
    (void)control_code;
    (void)input;
    (void)input_size;
    (void)output;
    (void)output_size;
    (void)overlapped;
    if (bytes_returned) *bytes_returned = 0;
    SetLastError(device ? 1 : 6); /* ERROR_INVALID_FUNCTION / INVALID_HANDLE */
    return FALSE;
}

DWORD WINAPI GetFileType(HANDLE hFile)
{
    (void)hFile;
    return 1; /* FILE_TYPE_DISK */
}

typedef struct {
    PCSTR name;
    PCSTR value;
} K32_ENV_VALUE;

#define K32_MAX_DYNAMIC_ENV 512
#define K32_ENV_NAME_MAX     64
#define K32_ENV_VALUE_MAX  1024
#define K32_ENV_BLOCK_MAX_CHARS 32768

typedef struct {
    BOOL used;
    BOOL deleted;
    DWORD process_id;
    char name[K32_ENV_NAME_MAX];
    char value[K32_ENV_VALUE_MAX];
} K32_DYNAMIC_ENV_VALUE;

static K32_DYNAMIC_ENV_VALUE k32_dynamic_environment[K32_MAX_DYNAMIC_ENV];
static spinlock_t k32_environment_lock = SPINLOCK_INIT;

static inline uint64_t k32_environment_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&k32_environment_lock);
    return flags;
}

static inline void k32_environment_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&k32_environment_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static char k32_env_fold(char c)
{
    if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
    return c;
}

static BOOL k32_env_name_equal(PCSTR left, PCSTR right)
{
    while (*left && *right && k32_env_fold(*left) == k32_env_fold(*right)) {
        left++;
        right++;
    }
    return *left == 0 && *right == 0;
}

static BOOL k32_env_set_ascii(PCSTR name, PCSTR value)
{
    if (!name || !*name) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    SIZE_T name_len = 0;
    while (name[name_len] && name_len < K32_ENV_NAME_MAX) {
        /* The MSVCRT chdir helpers maintain per-drive CWDs in hidden
         * environment variables named "=C:", "=D:", and so on. */
        if (name[name_len] == '=' && name_len != 0) {
            SetLastError(87);
            return FALSE;
        }
        name_len++;
    }
    if (name_len == K32_ENV_NAME_MAX) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return FALSE;
    }

    SIZE_T value_len = 0;
    if (value) {
        while (value[value_len] && value_len < K32_ENV_VALUE_MAX) value_len++;
        if (value_len == K32_ENV_VALUE_MAX) {
            SetLastError(206);
            return FALSE;
        }
    }

    DWORD process_id = GetCurrentProcessId();
    int existing = -1;
    int free_slot = -1;
    uint64_t irq_flags = k32_environment_lock_irqsave();
    for (int i = 0; i < K32_MAX_DYNAMIC_ENV; i++) {
        if (!k32_dynamic_environment[i].used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (k32_dynamic_environment[i].process_id == process_id &&
            k32_env_name_equal(k32_dynamic_environment[i].name, name)) {
            existing = i;
            break;
        }
    }

    int slot = existing >= 0 ? existing : free_slot;
    if (slot < 0) {
        k32_environment_unlock_irqrestore(irq_flags);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    K32_DYNAMIC_ENV_VALUE *entry = &k32_dynamic_environment[slot];
    entry->used = FALSE;
    entry->process_id = process_id;
    for (SIZE_T i = 0; i <= name_len; i++) entry->name[i] = name[i];
    if (value) {
        for (SIZE_T i = 0; i <= value_len; i++) entry->value[i] = value[i];
        entry->deleted = FALSE;
    } else {
        entry->value[0] = 0;
        entry->deleted = TRUE;
    }
    entry->used = TRUE;
    k32_environment_unlock_irqrestore(irq_flags);

    if ((name_len >= 3 && k32_env_fold(name[0]) == 'V' &&
         k32_env_fold(name[1]) == 'K' && name[2] == '_') ||
        name[0] == '=') {
        serial_puts("[K32-ENV] ");
        serial_puts(name);
        serial_puts("=");
        serial_puts(value ? value : "(deleted)");
        serial_puts("\n");
    }
    if (!win32_refresh_current_process_parameters()) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI SetEnvironmentVariableA(PCSTR lpName, PCSTR lpValue)
{
    return k32_env_set_ascii(lpName, lpValue);
}

BOOL WINAPI SetEnvironmentVariableW(PCWSTR lpName, PCWSTR lpValue)
{
    char name[K32_ENV_NAME_MAX];
    char value[K32_ENV_VALUE_MAX];
    SIZE_T i = 0;
    if (!lpName) return k32_env_set_ascii(NULL, NULL);
    while (lpName[i] && i < K32_ENV_NAME_MAX) {
        name[i] = (char)(lpName[i] & 0xFF);
        i++;
    }
    if (i == K32_ENV_NAME_MAX) {
        SetLastError(206);
        return FALSE;
    }
    name[i] = 0;
    if (!lpValue) return k32_env_set_ascii(name, NULL);

    i = 0;
    while (lpValue[i] && i < K32_ENV_VALUE_MAX) {
        value[i] = (char)(lpValue[i] & 0xFF);
        i++;
    }
    if (i == K32_ENV_VALUE_MAX) {
        SetLastError(206);
        return FALSE;
    }
    value[i] = 0;
    return k32_env_set_ascii(name, value);
}

static const K32_ENV_VALUE k32_environment[] = {
    { "WINDIR",              "C:\\Windows" },
    { "SYSTEMROOT",          "C:\\Windows" },
    { "TEMP",                "C:\\Temp" },
    { "TMP",                 "C:\\Temp" },
    { "LOCALAPPDATA",        "C:\\Users\\osito\\AppData\\Local" },
    { "APPDATA",             "C:\\Users\\osito\\AppData\\Roaming" },
    { "USERPROFILE",         "C:\\Users\\osito" },
    { "PROGRAMFILES",        "C:\\System\\Program Files" },
    { "PROGRAMFILES(X86)",   "C:\\System\\Program Files (x86)" },
    { "PROGRAMDATA",         "C:\\ProgramData" },
    { "HOMEDRIVE",           "C:" },
    { "HOMEPATH",            "\\Users\\osito" },
    { "USERNAME",            "osito" },
    { "COMPUTERNAME",        "OSITOK" },
    { "PATH",                "C:\\Windows\\System32;C:\\Windows" },
    { NULL, NULL }
};

#define K32_BASE_ENV_COUNT \
    ((SIZE_T)(sizeof(k32_environment) / sizeof(k32_environment[0]) - 1))

_Static_assert(K32_BASE_ENV_COUNT <= 64,
               "base environment mask must fit in 64 bits");

typedef struct {
    char name[K32_ENV_NAME_MAX];
    char value[K32_ENV_VALUE_MAX];
} K32_PARSED_ENV_ENTRY;

static WCHAR k32_env_block_char(PCVOID environment, BOOL unicode,
                                SIZE_T index)
{
    return unicode ? ((const WCHAR *)environment)[index]
                   : (WCHAR)((const BYTE *)environment)[index];
}

static NTSTATUS k32_env_parse_block_entry(
    PCVOID environment, BOOL unicode, SIZE_T offset,
    K32_PARSED_ENV_ENTRY *entry, SIZE_T *next_offset, BOOL *at_end)
{
    if (!environment || !entry || !next_offset || !at_end ||
        offset >= K32_ENV_BLOCK_MAX_CHARS)
        return STATUS_INVALID_PARAMETER;

    if (!k32_env_block_char(environment, unicode, offset)) {
        *at_end = TRUE;
        *next_offset = offset;
        return STATUS_SUCCESS;
    }

    SIZE_T length = 0;
    while (offset + length < K32_ENV_BLOCK_MAX_CHARS &&
           k32_env_block_char(environment, unicode, offset + length))
        length++;
    if (offset + length >= K32_ENV_BLOCK_MAX_CHARS)
        return STATUS_INVALID_PARAMETER;

    SIZE_T separator = (SIZE_T)-1;
    for (SIZE_T i = 1; i < length; i++) {
        if (k32_env_block_char(environment, unicode, offset + i) == '=') {
            separator = i;
            break;
        }
    }
    if (separator == (SIZE_T)-1 || separator >= K32_ENV_NAME_MAX ||
        length - separator - 1 >= K32_ENV_VALUE_MAX)
        return STATUS_INVALID_PARAMETER;

    for (SIZE_T i = 0; i < separator; i++)
        entry->name[i] =
            (char)(k32_env_block_char(environment, unicode, offset + i) & 0xFF);
    entry->name[separator] = 0;

    SIZE_T value_length = length - separator - 1;
    for (SIZE_T i = 0; i < value_length; i++)
        entry->value[i] = (char)(k32_env_block_char(
            environment, unicode, offset + separator + 1 + i) & 0xFF);
    entry->value[value_length] = 0;

    *at_end = FALSE;
    *next_offset = offset + length + 1;
    return STATUS_SUCCESS;
}

static int k32_env_base_index(PCSTR name)
{
    for (SIZE_T i = 0; i < K32_BASE_ENV_COUNT; i++)
        if (k32_env_name_equal(name, k32_environment[i].name))
            return (int)i;
    return -1;
}

static int k32_env_find_process_entry_locked(DWORD process_id, PCSTR name)
{
    for (int i = 0; i < K32_MAX_DYNAMIC_ENV; i++) {
        K32_DYNAMIC_ENV_VALUE *entry = &k32_dynamic_environment[i];
        if (entry->used && entry->process_id == process_id &&
            k32_env_name_equal(entry->name, name))
            return i;
    }
    return -1;
}

static int k32_env_find_free_entry_locked(void)
{
    for (int i = 0; i < K32_MAX_DYNAMIC_ENV; i++)
        if (!k32_dynamic_environment[i].used)
            return i;
    return -1;
}

static BOOL k32_env_store_process_entry_locked(DWORD process_id, PCSTR name,
                                                PCSTR value, BOOL deleted)
{
    int slot = k32_env_find_process_entry_locked(process_id, name);
    if (slot < 0)
        slot = k32_env_find_free_entry_locked();
    if (slot < 0)
        return FALSE;

    K32_DYNAMIC_ENV_VALUE *entry = &k32_dynamic_environment[slot];
    entry->used = FALSE;
    entry->process_id = process_id;
    SIZE_T i = 0;
    while (name[i] && i < K32_ENV_NAME_MAX - 1) {
        entry->name[i] = name[i];
        i++;
    }
    entry->name[i] = 0;
    i = 0;
    if (value) {
        while (value[i] && i < K32_ENV_VALUE_MAX - 1) {
            entry->value[i] = value[i];
            i++;
        }
    }
    entry->value[i] = 0;
    entry->deleted = deleted;
    entry->used = TRUE;
    return TRUE;
}

NTSTATUS kernel32_set_process_environment_block(DWORD process_id,
                                                  PCVOID environment,
                                                  BOOL unicode)
{
    if (!process_id || !environment)
        return STATUS_INVALID_PARAMETER;

    SIZE_T offset = 0;
    SIZE_T required = 0;
    uint64_t seen_base = 0;
    K32_PARSED_ENV_ENTRY parsed;

    for (;;) {
        SIZE_T next = 0;
        BOOL at_end = FALSE;
        NTSTATUS status = k32_env_parse_block_entry(
            environment, unicode, offset, &parsed, &next, &at_end);
        if (!NT_SUCCESS(status))
            return status;
        if (at_end)
            break;

        int base = k32_env_base_index(parsed.name);
        if (base >= 0) {
            seen_base |= 1ULL << base;
            if (k32_strcmp(parsed.value, k32_environment[base].value) != 0)
                required++;
        } else {
            required++;
        }
        if (required > K32_MAX_DYNAMIC_ENV)
            return STATUS_INSUFFICIENT_RESOURCES;
        offset = next;
    }

    for (SIZE_T i = 0; i < K32_BASE_ENV_COUNT; i++)
        if (!(seen_base & (1ULL << i)))
            required++;
    if (required > K32_MAX_DYNAMIC_ENV)
        return STATUS_INSUFFICIENT_RESOURCES;

    uint64_t irq_flags = k32_environment_lock_irqsave();
    SIZE_T free_entries = 0;
    for (int i = 0; i < K32_MAX_DYNAMIC_ENV; i++) {
        K32_DYNAMIC_ENV_VALUE *entry = &k32_dynamic_environment[i];
        if (entry->used && entry->process_id == process_id)
            entry->used = FALSE;
        if (!entry->used)
            free_entries++;
    }
    if (free_entries < required) {
        k32_environment_unlock_irqrestore(irq_flags);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    offset = 0;
    for (;;) {
        SIZE_T next = 0;
        BOOL at_end = FALSE;
        NTSTATUS status = k32_env_parse_block_entry(
            environment, unicode, offset, &parsed, &next, &at_end);
        if (!NT_SUCCESS(status)) {
            k32_environment_unlock_irqrestore(irq_flags);
            return status;
        }
        if (at_end)
            break;

        int base = k32_env_base_index(parsed.name);
        if ((base < 0 ||
             k32_strcmp(parsed.value, k32_environment[base].value) != 0) &&
            !k32_env_store_process_entry_locked(
                process_id, parsed.name, parsed.value, FALSE)) {
            k32_environment_unlock_irqrestore(irq_flags);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        offset = next;
    }
    for (SIZE_T i = 0; i < K32_BASE_ENV_COUNT; i++) {
        if (!(seen_base & (1ULL << i)) &&
            !k32_env_store_process_entry_locked(
                process_id, k32_environment[i].name, NULL, TRUE)) {
            k32_environment_unlock_irqrestore(irq_flags);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    k32_environment_unlock_irqrestore(irq_flags);

    serial_puts("[K32-ENV] explicit child environment pid=");
    serial_putdec(process_id);
    serial_puts(" entries=");
    serial_putdec(required);
    serial_puts(unicode ? " unicode\n" : " ansi\n");
    return STATUS_SUCCESS;
}

static BOOL k32_env_name_matches_a(PCSTR name, DWORD chars, PCSTR expected)
{
    DWORD i = 0;
    while (i < chars && expected[i]) {
        char a = name[i];
        char b = expected[i];
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
        if (a != b) return FALSE;
        i++;
    }
    return i == chars && expected[i] == 0;
}

static BOOL k32_env_name_matches_w(PCWSTR name, DWORD chars, PCSTR expected)
{
    DWORD i = 0;
    while (i < chars && expected[i]) {
        WCHAR a = name[i];
        char b = expected[i];
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
        if (a != (WCHAR)(BYTE)b) return FALSE;
        i++;
    }
    return i == chars && expected[i] == 0;
}

static PCSTR k32_env_lookup_a(PCSTR name, DWORD chars)
{
    for (int i = 0; k32_environment[i].name; i++) {
        if (k32_env_name_matches_a(name, chars, k32_environment[i].name))
            return k32_environment[i].value;
    }
    return NULL;
}

static PCSTR k32_env_lookup_w(PCWSTR name, DWORD chars)
{
    for (int i = 0; k32_environment[i].name; i++) {
        if (k32_env_name_matches_w(name, chars, k32_environment[i].name))
            return k32_environment[i].value;
    }
    return NULL;
}

/* Returns 1 for a value, -1 for an explicit deletion, and 0 when no
 * process-local entry exists. */
static int k32_env_lookup_dynamic_a(PCSTR name, DWORD chars,
                                    char value[K32_ENV_VALUE_MAX])
{
    char key[K32_ENV_NAME_MAX];
    if (!name || !chars || chars >= K32_ENV_NAME_MAX) return FALSE;
    for (DWORD i = 0; i < chars; i++) key[i] = name[i];
    key[chars] = 0;

    int result = 0;
    DWORD process_id = GetCurrentProcessId();
    uint64_t irq_flags = k32_environment_lock_irqsave();
    for (int i = 0; i < K32_MAX_DYNAMIC_ENV; i++) {
        K32_DYNAMIC_ENV_VALUE *entry = &k32_dynamic_environment[i];
        if (!entry->used || entry->process_id != process_id ||
            !k32_env_name_equal(entry->name, key))
            continue;
        if (entry->deleted) {
            value[0] = 0;
            result = -1;
        } else {
            SIZE_T j = 0;
            while (entry->value[j] && j < K32_ENV_VALUE_MAX - 1) {
                value[j] = entry->value[j];
                j++;
            }
            value[j] = 0;
            result = 1;
        }
        break;
    }
    k32_environment_unlock_irqrestore(irq_flags);
    return result;
}

static int k32_env_lookup_dynamic_w(PCWSTR name, DWORD chars,
                                    char value[K32_ENV_VALUE_MAX])
{
    char key[K32_ENV_NAME_MAX];
    if (!name || !chars || chars >= K32_ENV_NAME_MAX) return FALSE;
    for (DWORD i = 0; i < chars; i++) key[i] = (char)(name[i] & 0xFF);
    key[chars] = 0;
    return k32_env_lookup_dynamic_a(key, chars, value);
}

DWORD WINAPI GetEnvironmentVariableA(PCSTR lpName, PSTR lpBuffer,
                                     DWORD nSize)
{
    if (!lpName) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    DWORD name_len = (DWORD)lstrlenA(lpName);
    char dynamic_value[K32_ENV_VALUE_MAX];
    int dynamic = k32_env_lookup_dynamic_a(lpName, name_len, dynamic_value);
    PCSTR value = dynamic > 0 ? dynamic_value
                  : dynamic < 0 ? NULL
                                : k32_env_lookup_a(lpName, name_len);
    if (!value) {
        if (lpBuffer && nSize) lpBuffer[0] = 0;
        SetLastError(203); /* ERROR_ENVVAR_NOT_FOUND */
        return 0;
    }

    DWORD len = (DWORD)lstrlenA(value);
    if (!lpBuffer || nSize <= len) {
        if (lpBuffer && nSize) lpBuffer[0] = 0;
        return len + 1;
    }
    for (DWORD i = 0; i <= len; i++) lpBuffer[i] = value[i];
    return len;
}

DWORD WINAPI GetEnvironmentVariableW(PCWSTR lpName, PWSTR lpBuffer,
                                     DWORD nSize)
{
    if (!lpName) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    DWORD name_len = (DWORD)lstrlenW(lpName);
    char dynamic_value[K32_ENV_VALUE_MAX];
    int dynamic = k32_env_lookup_dynamic_w(lpName, name_len, dynamic_value);
    PCSTR value = dynamic > 0 ? dynamic_value
                  : dynamic < 0 ? NULL
                                : k32_env_lookup_w(lpName, name_len);
    if (!value) {
        if (lpBuffer && nSize) lpBuffer[0] = 0;
        SetLastError(203); /* ERROR_ENVVAR_NOT_FOUND */
        return 0;
    }

    DWORD len = (DWORD)lstrlenA(value);
    if (!lpBuffer || nSize <= len) {
        if (lpBuffer && nSize) lpBuffer[0] = 0;
        return len + 1;
    }
    for (DWORD i = 0; i < len; i++) lpBuffer[i] = (WCHAR)(BYTE)value[i];
    lpBuffer[len] = 0;
    return len;
}

NTSTATUS kernel32_inherit_process_environment(DWORD parent_pid,
                                               DWORD child_pid)
{
    if (!parent_pid || !child_pid || parent_pid == child_pid)
        return STATUS_INVALID_PARAMETER;
    uint64_t irq_flags = k32_environment_lock_irqsave();
    SIZE_T required = 0;
    SIZE_T free_entries = 0;
    for (int i = 0; i < K32_MAX_DYNAMIC_ENV; i++) {
        K32_DYNAMIC_ENV_VALUE *entry = &k32_dynamic_environment[i];
        if (entry->used && entry->process_id == parent_pid)
            required++;
        if (!entry->used)
            free_entries++;
    }
    if (free_entries < required) {
        k32_environment_unlock_irqrestore(irq_flags);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (int i = 0; i < K32_MAX_DYNAMIC_ENV; i++) {
        K32_DYNAMIC_ENV_VALUE *source = &k32_dynamic_environment[i];
        if (!source->used || source->process_id != parent_pid) continue;
        int free_slot = -1;
        for (int j = 0; j < K32_MAX_DYNAMIC_ENV; j++) {
            if (!k32_dynamic_environment[j].used) {
                free_slot = j;
                break;
            }
        }
        if (free_slot < 0) {
            k32_environment_unlock_irqrestore(irq_flags);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        K32_DYNAMIC_ENV_VALUE *target = &k32_dynamic_environment[free_slot];
        *target = *source;
        target->process_id = child_pid;
    }
    k32_environment_unlock_irqrestore(irq_flags);
    return STATUS_SUCCESS;
}

void kernel32_release_process_environment(DWORD process_id)
{
    if (!process_id) return;
    uint64_t irq_flags = k32_environment_lock_irqsave();
    for (int i = 0; i < K32_MAX_DYNAMIC_ENV; i++) {
        if (k32_dynamic_environment[i].used &&
            k32_dynamic_environment[i].process_id == process_id)
            k32_dynamic_environment[i].used = FALSE;
    }
    k32_environment_unlock_irqrestore(irq_flags);
    k32_io_completion_release_process(process_id);
    k32_execution_state_release_process(process_id);
    heap_release_process_state(process_id);
    kernel32_release_process_exception_state(process_id);
}

typedef struct {
    PCSTR name;
    PCSTR value;
} K32_EFFECTIVE_ENV_VALUE;

static int k32_env_name_compare(PCSTR left, PCSTR right)
{
    while (*left && *right) {
        char a = k32_env_fold(*left);
        char b = k32_env_fold(*right);
        if (a != b) return (BYTE)a < (BYTE)b ? -1 : 1;
        left++;
        right++;
    }
    if (*left == *right) return 0;
    return *left ? 1 : -1;
}

SIZE_T kernel32_build_environment_block_w(DWORD process_id, PWSTR buffer,
                                           SIZE_T capacity)
{
    K32_EFFECTIVE_ENV_VALUE effective[
        K32_MAX_DYNAMIC_ENV +
        sizeof(k32_environment) / sizeof(k32_environment[0])];
    SIZE_T count = 0;
    if (!process_id) process_id = GetCurrentProcessId();

    uint64_t irq_flags = k32_environment_lock_irqsave();
    for (SIZE_T i = 0; k32_environment[i].name; i++) {
        BOOL overridden = FALSE;
        for (int j = 0; j < K32_MAX_DYNAMIC_ENV; j++) {
            K32_DYNAMIC_ENV_VALUE *entry = &k32_dynamic_environment[j];
            if (entry->used && entry->process_id == process_id &&
                k32_env_name_equal(entry->name, k32_environment[i].name)) {
                overridden = TRUE;
                break;
            }
        }
        if (!overridden) {
            effective[count].name = k32_environment[i].name;
            effective[count].value = k32_environment[i].value;
            count++;
        }
    }
    for (int i = 0; i < K32_MAX_DYNAMIC_ENV; i++) {
        K32_DYNAMIC_ENV_VALUE *entry = &k32_dynamic_environment[i];
        if (!entry->used || entry->deleted ||
            entry->process_id != process_id)
            continue;
        effective[count].name = entry->name;
        effective[count].value = entry->value;
        count++;
    }

    for (SIZE_T i = 1; i < count; i++) {
        K32_EFFECTIVE_ENV_VALUE value = effective[i];
        SIZE_T j = i;
        while (j && k32_env_name_compare(value.name,
                                          effective[j - 1].name) < 0) {
            effective[j] = effective[j - 1];
            j--;
        }
        effective[j] = value;
    }

    SIZE_T out = 0;
#define K32_ENV_EMIT(ch) do { \
        if (buffer && out < capacity) buffer[out] = (WCHAR)(BYTE)(ch); \
        out++; \
    } while (0)
    for (SIZE_T i = 0; i < count; i++) {
        for (SIZE_T j = 0; effective[i].name[j]; j++)
            K32_ENV_EMIT(effective[i].name[j]);
        K32_ENV_EMIT('=');
        for (SIZE_T j = 0; effective[i].value[j]; j++)
            K32_ENV_EMIT(effective[i].value[j]);
        K32_ENV_EMIT(0);
    }
    K32_ENV_EMIT(0);
    if (!count) K32_ENV_EMIT(0);
#undef K32_ENV_EMIT

    if (buffer && capacity && out > capacity)
        buffer[capacity - 1] = 0;
    k32_environment_unlock_irqrestore(irq_flags);
    return out;
}

static DWORD WINAPI ExpandEnvironmentStringsA_k32(PCSTR src, PSTR dst,
                                                   DWORD capacity)
{
    if (!src) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    DWORD out = 0;
    for (DWORD i = 0; src[i];) {
        DWORD end = i;
        PCSTR replacement = NULL;
        char dynamic_value[K32_ENV_VALUE_MAX];
        if (src[i] == '%') {
            end = i + 1;
            while (src[end] && src[end] != '%') end++;
            if (src[end] == '%') {
                int dynamic = k32_env_lookup_dynamic_a(
                    src + i + 1, end - i - 1, dynamic_value);
                replacement = dynamic > 0 ? dynamic_value
                              : dynamic < 0 ? NULL
                                            : k32_env_lookup_a(
                                                  src + i + 1,
                                                  end - i - 1);
            }
        }

        if (replacement) {
            for (DWORD j = 0; replacement[j]; j++, out++) {
                if (dst && out + 1 < capacity) dst[out] = replacement[j];
            }
            i = end + 1;
        } else {
            if (dst && out + 1 < capacity) dst[out] = src[i];
            out++;
            i++;
        }
    }
    if (dst && capacity) dst[out < capacity ? out : capacity - 1] = 0;
    return out + 1;
}

static DWORD WINAPI ExpandEnvironmentStringsW_k32(PCWSTR src, PWSTR dst,
                                                   DWORD capacity)
{
    if (!src) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    DWORD out = 0;
    for (DWORD i = 0; src[i];) {
        DWORD end = i;
        PCSTR replacement = NULL;
        char dynamic_value[K32_ENV_VALUE_MAX];
        if (src[i] == '%') {
            end = i + 1;
            while (src[end] && src[end] != '%') end++;
            if (src[end] == '%') {
                int dynamic = k32_env_lookup_dynamic_w(
                    src + i + 1, end - i - 1, dynamic_value);
                replacement = dynamic > 0 ? dynamic_value
                              : dynamic < 0 ? NULL
                                            : k32_env_lookup_w(
                                                  src + i + 1,
                                                  end - i - 1);
            }
        }

        if (replacement) {
            for (DWORD j = 0; replacement[j]; j++, out++) {
                if (dst && out + 1 < capacity)
                    dst[out] = (WCHAR)(BYTE)replacement[j];
            }
            i = end + 1;
        } else {
            if (dst && out + 1 < capacity) dst[out] = src[i];
            out++;
            i++;
        }
    }
    if (dst && capacity) dst[out < capacity ? out : capacity - 1] = 0;
    return out + 1;
}

PCSTR WINAPI GetEnvironmentStrings(void) { return GetEnvironmentStringsA(); }

void WINAPI Beep_stub(DWORD dwFreq, DWORD dwDuration)
{
    (void)dwFreq; (void)dwDuration;
}

/* Locale / codepage support */
BOOL WINAPI AreFileApisANSI(void) { return TRUE; }
UINT WINAPI GetACP(void) { return 1252; }
UINT WINAPI GetOEMCP(void) { return 437; }

typedef struct {
    UINT MaxCharSize;
    BYTE DefaultChar[2];
    BYTE LeadByte[12];
} K32_CPINFO;

_Static_assert(sizeof(K32_CPINFO) == 20, "Win32 CPINFO layout changed");

static UINT k32_resolve_code_page(UINT code_page)
{
    switch (code_page) {
    case 0: /* CP_ACP */
    case 3: /* CP_THREAD_ACP */
        return GetACP();
    case 1: /* CP_OEMCP */
        return GetOEMCP();
    default:
        return code_page;
    }
}

static void k32_cpinfo_add_range(K32_CPINFO *info, int pair,
                                 BYTE first, BYTE last)
{
    if (pair < 0 || pair >= 5) return;
    info->LeadByte[pair * 2] = first;
    info->LeadByte[pair * 2 + 1] = last;
}

static void k32_fill_cpinfo(UINT code_page, K32_CPINFO *info)
{
    memset(info, 0, sizeof(*info));
    info->MaxCharSize = 1;
    info->DefaultChar[0] = '?';

    switch (k32_resolve_code_page(code_page)) {
    case 932: /* Shift-JIS */
        info->MaxCharSize = 2;
        k32_cpinfo_add_range(info, 0, 0x81, 0x9F);
        k32_cpinfo_add_range(info, 1, 0xE0, 0xFC);
        break;
    case 936: /* GBK */
    case 949: /* Unified Hangul */
    case 950: /* Big5 */
        info->MaxCharSize = 2;
        k32_cpinfo_add_range(info, 0, 0x81, 0xFE);
        break;
    case 1361: /* Johab */
        info->MaxCharSize = 2;
        k32_cpinfo_add_range(info, 0, 0x84, 0xD3);
        k32_cpinfo_add_range(info, 1, 0xD8, 0xDE);
        k32_cpinfo_add_range(info, 2, 0xE0, 0xF9);
        break;
    case 54936: /* GB18030 */
    case 65001: /* UTF-8 */
        info->MaxCharSize = 4;
        break;
    case 65000: /* UTF-7 */
        info->MaxCharSize = 5;
        break;
    default:
        break;
    }
}

BOOL WINAPI GetCPInfo(UINT cp, PVOID info)
{
    if (!info) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    k32_fill_cpinfo(cp, (K32_CPINFO *)info);
    return TRUE;
}

BOOL WINAPI IsDBCSLeadByteEx(UINT code_page, BYTE test_char)
{
    K32_CPINFO info;
    k32_fill_cpinfo(code_page, &info);
    for (int pair = 0; pair < 6; pair++) {
        BYTE first = info.LeadByte[pair * 2];
        BYTE last = info.LeadByte[pair * 2 + 1];
        if (!first && !last) break;
        if (test_char >= first && test_char <= last) return TRUE;
    }
    return FALSE;
}

BOOL WINAPI IsDBCSLeadByte(BYTE test_char)
{
    return IsDBCSLeadByteEx(GetACP(), test_char);
}

DWORD WINAPI GetUserDefaultLCID(void) { return 0x0409; } /* en-US */
static DWORD WINAPI GetSystemDefaultLCID_k32(void) { return 0x0409; }
static DWORD WINAPI GetThreadLocale_k32(void) { return 0x0409; }
WORD WINAPI GetUserDefaultLangID(void) { return (WORD)GetUserDefaultLCID(); }
static WORD WINAPI GetUserDefaultUILanguage_k32(void) { return 0x0409; }
LONG WINAPI GetUserGeoID(DWORD geo_class)
{
    if (geo_class == 16) /* GEOCLASS_NATION */
        return 244;      /* United States */
    return -1;           /* GEOID_NOT_AVAILABLE */
}
static int WINAPI GetGeoInfoW_k32(LONG location, DWORD geo_type,
                                   PWSTR data, int data_chars, WORD lang_id)
{
    static const WCHAR iso2_us[] = {'U', 'S', 0};
    const int required = (int)(sizeof(iso2_us) / sizeof(iso2_us[0]));

    if (location != 244 || geo_type != 4 || lang_id != 0) {
        if (data && data_chars > 0) data[0] = 0;
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (data_chars == 0)
        return required;
    if (!data || data_chars < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    for (int i = 0; i < required; i++) data[i] = iso2_us[i];
    return required;
}
int WINAPI GetUserDefaultLocaleName(PWSTR name, int chars)
{
    static const WCHAR locale[] = { 'e', 'n', '-', 'U', 'S', 0 };
    if (!name || chars < (int)(sizeof(locale) / sizeof(locale[0]))) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    for (int i = 0; i < (int)(sizeof(locale) / sizeof(locale[0])); i++)
        name[i] = locale[i];
    return (int)(sizeof(locale) / sizeof(locale[0]));
}

static DWORD WINAPI LocaleNameToLCID_k32(PCWSTR name, DWORD flags)
{
    (void)flags;
    if (!name || !name[0]) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    return 0x0409; /* en-US */
}

static int WINAPI LCIDToLocaleName_k32(DWORD locale, PWSTR name,
                                        int chars, DWORD flags)
{
    static const WCHAR en_us[] = {'e','n','-','U','S',0};
    (void)flags;
    if (!locale) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    return k32_copy_wide_result(en_us, name, chars);
}

static BOOL WINAPI GetThreadPreferredUILanguages_k32(
    DWORD flags, ULONG *language_count, PWSTR languages, ULONG *buffer_chars)
{
    static const WCHAR name[] = {'e','n','-','U','S',0,0};
    static const WCHAR id[] = {'0','4','0','9',0,0};
    const WCHAR *value = (flags & 0x4) ? id : name; /* MUI_LANGUAGE_ID */
    ULONG required = (flags & 0x4) ? 6 : 7;

    if (!language_count || !buffer_chars ||
        ((flags & 0x4) && (flags & 0x8))) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    *language_count = 1;
    if (!languages) {
        if (*buffer_chars) {
            SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
            return FALSE;
        }
        *buffer_chars = required;
        return TRUE;
    }
    if (*buffer_chars < required) {
        *buffer_chars = required;
        SetLastError(122);
        return FALSE;
    }
    for (ULONG i = 0; i < required; i++) languages[i] = value[i];
    *buffer_chars = required;
    return TRUE;
}

BOOL WINAPI IsValidCodePage(UINT cp) { (void)cp; return TRUE; }
BOOL WINAPI IsValidLocale(DWORD lcid, DWORD flags) { (void)lcid; (void)flags; return TRUE; }
static BOOL WINAPI IsValidLocaleName_k32(PCWSTR name)
{
    return name && name[0];
}
BOOL WINAPI EnumSystemLocalesA(PVOID fn, DWORD flags) { (void)fn; (void)flags; return TRUE; }

static BOOL WINAPI EnumSystemLocalesW_k32(PVOID callback, DWORD flags)
{
    static WCHAR locale[] = {'0','4','0','9',0};
    (void)flags;
    if (!callback) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (g_compat32_mode)
        return TRUE;

    typedef BOOL (WINAPI *locale_callback_t)(PWSTR);
    return ((locale_callback_t)callback)(locale);
}

static BOOL WINAPI EnumSystemLocalesEx_k32(PVOID callback, DWORD flags,
                                            LONG_PTR parameter,
                                            PVOID reserved)
{
    static WCHAR locale[] = {'e','n','-','U','S',0};
    (void)flags;
    if (!callback || reserved) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (g_compat32_mode)
        return TRUE;

    typedef BOOL (WINAPI *locale_callback_t)(PWSTR, DWORD, LONG_PTR);
    return ((locale_callback_t)callback)(locale, 0x1, parameter);
}

static const char *k32_locale_info_en_us(DWORD locale_type)
{
    switch (locale_type & 0xFFFFU) {
    case 0x0001: return "0409";                    /* LOCALE_ILANGUAGE */
    case 0x0002: return "English (United States)"; /* LOCALE_SLANGUAGE */
    case 0x0003: return "ENU";                     /* LOCALE_SABBREVLANGNAME */
    case 0x0004: return "English (United States)"; /* LOCALE_SNATIVELANGNAME */
    case 0x0005: return "1";                       /* LOCALE_ICOUNTRY */
    case 0x0006: return "United States";           /* LOCALE_SCOUNTRY */
    case 0x0007: return "USA";                     /* LOCALE_SABBREVCTRYNAME */
    case 0x0008: return "United States";           /* LOCALE_SNATIVECTRYNAME */
    case 0x0009: return "0409";                    /* LOCALE_IDEFAULTLANGUAGE */
    case 0x000A: return "1";                       /* LOCALE_IDEFAULTCOUNTRY */
    case 0x000B: return "437";                     /* LOCALE_IDEFAULTCODEPAGE */
    case 0x000C: return ",";                       /* LOCALE_SLIST */
    case 0x000D: return "1";                       /* LOCALE_IMEASURE */
    case 0x000E: return ".";                       /* LOCALE_SDECIMAL */
    case 0x000F: return ",";                       /* LOCALE_STHOUSAND */
    case 0x0010: return "3;0";                     /* LOCALE_SGROUPING */
    case 0x0011: return "2";                       /* LOCALE_IDIGITS */
    case 0x0012: return "1";                       /* LOCALE_ILZERO */
    case 0x0013: return "0123456789";              /* LOCALE_SNATIVEDIGITS */
    case 0x0014: return "$";                       /* LOCALE_SCURRENCY */
    case 0x0015: return "USD";                     /* LOCALE_SINTLSYMBOL */
    case 0x0016: return ".";                       /* LOCALE_SMONDECIMALSEP */
    case 0x0017: return ",";                       /* LOCALE_SMONTHOUSANDSEP */
    case 0x0018: return "3;0";                     /* LOCALE_SMONGROUPING */
    case 0x0019: return "2";                       /* LOCALE_ICURRDIGITS */
    case 0x001A: return "2";                       /* LOCALE_IINTLCURRDIGITS */
    case 0x001B: return "0";                       /* LOCALE_ICURRENCY */
    case 0x001C: return "0";                       /* LOCALE_INEGCURR */
    case 0x001D: return "/";                       /* LOCALE_SDATE */
    case 0x001E: return ":";                       /* LOCALE_STIME */
    case 0x001F: return "M/d/yyyy";                /* LOCALE_SSHORTDATE */
    case 0x0020: return "dddd, MMMM d, yyyy";       /* LOCALE_SLONGDATE */
    case 0x0021: return "0";                       /* LOCALE_IDATE */
    case 0x0022: return "0";                       /* LOCALE_ILDATE */
    case 0x0023: return "0";                       /* LOCALE_ITIME */
    case 0x0028: return "AM";                      /* LOCALE_S1159 */
    case 0x0029: return "PM";                      /* LOCALE_S2359 */
    case 0x0050: return "+";                       /* LOCALE_SPOSITIVESIGN */
    case 0x0051: return "-";                       /* LOCALE_SNEGATIVESIGN */
    case 0x0059: return "en";                      /* LOCALE_SISO639LANGNAME */
    case 0x005A: return "US";                      /* LOCALE_SISO3166CTRYNAME */
    case 0x005B: return "244";                     /* LOCALE_IGEOID */
    case 0x005C: return "en-US";                   /* LOCALE_SNAME */
    case 0x0067: return "eng";                     /* LOCALE_SISO639LANGNAME2 */
    case 0x0068: return "USA";                     /* LOCALE_SISO3166CTRYNAME2 */
    case 0x1001: return "English (United States)"; /* LOCALE_SENGLANGUAGE */
    case 0x1002: return "United States";           /* LOCALE_SENGCOUNTRY */
    case 0x1003: return "h:mm:ss tt";              /* LOCALE_STIMEFORMAT */
    case 0x1004: return "1252";                    /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x1010: return "1";                       /* LOCALE_INEGNUMBER */
    case 0x1011: return "10000";                   /* LOCALE_IDEFAULTMACCODEPAGE */
    default: return NULL;
    }
}

static BOOL k32_locale_numeric_value(DWORD locale_type, DWORD *value)
{
    switch (locale_type & 0xFFFFU) {
    case 0x0001:
    case 0x0009:
        *value = 0x0409;
        return TRUE;
    case 0x0005:
    case 0x000A:
    case 0x000D:
    case 0x0012:
    case 0x1010:
        *value = 1;
        return TRUE;
    case 0x000B: *value = 437; return TRUE;
    case 0x0011:
    case 0x0019:
    case 0x001A:
        *value = 2;
        return TRUE;
    case 0x001B:
    case 0x001C:
    case 0x0021:
    case 0x0022:
    case 0x0023:
        *value = 0;
        return TRUE;
    case 0x005B: *value = 244; return TRUE;
    case 0x1004: *value = 1252; return TRUE;
    case 0x1011: *value = 10000; return TRUE;
    default: return FALSE;
    }
}

static int k32_locale_copy_a(const char *value, PSTR output, int chars)
{
    int required = 1;
    while (value[required - 1]) required++;
    if (chars == 0) return required;
    if (!output) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (chars < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    for (int i = 0; i < required; i++) output[i] = value[i];
    return required;
}

static int k32_locale_copy_w(const char *value, PWSTR output, int chars)
{
    int required = 1;
    while (value[required - 1]) required++;
    if (chars == 0) return required;
    if (!output) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (chars < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    for (int i = 0; i < required; i++)
        output[i] = (WCHAR)(BYTE)value[i];
    return required;
}

int WINAPI GetLocaleInfoA(DWORD Locale, DWORD LCType, PSTR lpLCData, int cchData)
{
    const DWORD return_number = 0x20000000U; /* LOCALE_RETURN_NUMBER */
    (void)Locale;
    if (cchData < 0) {
        SetLastError(87);
        return 0;
    }
    if (LCType & return_number) {
        DWORD number;
        const int required = (int)sizeof(number);
        if (!k32_locale_numeric_value(LCType, &number)) {
            SetLastError(1004); /* ERROR_INVALID_FLAGS */
            return 0;
        }
        if (cchData == 0) return required;
        if (!lpLCData) {
            SetLastError(87);
            return 0;
        }
        if (cchData < required) {
            SetLastError(122);
            return 0;
        }
        memcpy(lpLCData, &number, sizeof(number));
        return required;
    }
    const char *value = k32_locale_info_en_us(LCType);
    if (!value) {
        SetLastError(1004);
        return 0;
    }
    return k32_locale_copy_a(value, lpLCData, cchData);
}

int WINAPI GetLocaleInfoW(DWORD Locale, DWORD LCType, PWSTR lpLCData, int cchData)
{
    const DWORD return_number = 0x20000000U; /* LOCALE_RETURN_NUMBER */
    (void)Locale;
    if (cchData < 0) {
        SetLastError(87);
        return 0;
    }
    if (LCType & return_number) {
        DWORD number;
        const int required = (int)(sizeof(number) / sizeof(WCHAR));
        if (!k32_locale_numeric_value(LCType, &number)) {
            SetLastError(1004);
            return 0;
        }
        if (cchData == 0) return required;
        if (!lpLCData) {
            SetLastError(87);
            return 0;
        }
        if (cchData < required) {
            SetLastError(122);
            return 0;
        }
        memcpy(lpLCData, &number, sizeof(number));
        return required;
    }
    const char *value = k32_locale_info_en_us(LCType);
    if (!value) {
        SetLastError(1004);
        return 0;
    }
    return k32_locale_copy_w(value, lpLCData, cchData);
}

static int WINAPI GetLocaleInfoEx_k32(PCWSTR locale_name, DWORD locale_type,
                                      PWSTR data, int data_chars)
{
    (void)locale_name; /* NULL means LOCALE_NAME_USER_DEFAULT. */
    return GetLocaleInfoW(0x0409, locale_type, data, data_chars);
}

static WCHAR k32_ordinal_upper(WCHAR ch)
{
    if (ch >= 'a' && ch <= 'z') return (WCHAR)(ch - 0x20);

    /* Common one-to-one mappings from the invariant Unicode uppercase table. */
    if ((ch >= 0x00E0 && ch <= 0x00F6) ||
        (ch >= 0x00F8 && ch <= 0x00FE))
        return (WCHAR)(ch - 0x20);
    if (ch == 0x00FF) return 0x0178;
    if (ch == 0x0131) return 'I';
    if (ch == 0x017F) return 'S';

    if ((ch >= 0x0101 && ch <= 0x012F && (ch & 1)) ||
        (ch >= 0x0133 && ch <= 0x0137 && (ch & 1)) ||
        (ch >= 0x014B && ch <= 0x0177 && (ch & 1)))
        return (WCHAR)(ch - 1);
    if (ch >= 0x013A && ch <= 0x0148 && !(ch & 1))
        return (WCHAR)(ch - 1);

    if (ch >= 0x03B1 && ch <= 0x03C1) return (WCHAR)(ch - 0x20);
    if (ch >= 0x03C3 && ch <= 0x03CB) return (WCHAR)(ch - 0x20);
    if (ch == 0x03C2) return 0x03A3;
    if (ch >= 0x0430 && ch <= 0x044F) return (WCHAR)(ch - 0x20);
    if (ch >= 0x0450 && ch <= 0x045F) return (WCHAR)(ch - 0x50);
    if (ch >= 0x0561 && ch <= 0x0586) return (WCHAR)(ch - 0x30);

    return ch;
}

int WINAPI CompareStringOrdinal(PCWSTR string1, int count1,
                                PCWSTR string2, int count2,
                                BOOL ignore_case)
{
    if (!string1 || !string2 || count1 < -1 || count2 < -1 ||
        (ignore_case != FALSE && ignore_case != TRUE)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    if (count1 == -1) {
        count1 = 0;
        while (string1[count1]) count1++;
    }
    if (count2 == -1) {
        count2 = 0;
        while (string2[count2]) count2++;
    }

    int common = count1 < count2 ? count1 : count2;
    for (int i = 0; i < common; i++) {
        WCHAR ch1 = string1[i];
        WCHAR ch2 = string2[i];
        if (ignore_case) {
            ch1 = k32_ordinal_upper(ch1);
            ch2 = k32_ordinal_upper(ch2);
        }
        if (ch1 < ch2) return 1; /* CSTR_LESS_THAN */
        if (ch1 > ch2) return 3; /* CSTR_GREATER_THAN */
    }
    if (count1 < count2) return 1;
    if (count1 > count2) return 3;
    return 2; /* CSTR_EQUAL */
}

int WINAPI CompareStringA(DWORD loc, DWORD flags, PCSTR s1, int c1, PCSTR s2, int c2)
{
    (void)loc; (void)flags;
    if (c1 < 0) { int n=0; while(s1[n]) n++; c1 = n; }
    if (c2 < 0) { int n=0; while(s2[n]) n++; c2 = n; }
    int n = c1 < c2 ? c1 : c2;
    for (int i = 0; i < n; i++) {
        if ((unsigned char)s1[i] < (unsigned char)s2[i]) return 1; /* CSTR_LESS_THAN */
        if ((unsigned char)s1[i] > (unsigned char)s2[i]) return 3; /* CSTR_GREATER_THAN */
    }
    if (c1 < c2) return 1;
    if (c1 > c2) return 3;
    return 2; /* CSTR_EQUAL */
}
int WINAPI CompareStringW(DWORD loc, DWORD flags, PCWSTR s1, int c1, PCWSTR s2, int c2)
{
    (void)loc; (void)flags;
    if (c1 < 0) { int n=0; while(s1[n]) n++; c1 = n; }
    if (c2 < 0) { int n=0; while(s2[n]) n++; c2 = n; }
    int n = c1 < c2 ? c1 : c2;
    for (int i = 0; i < n; i++) {
        if (s1[i] < s2[i]) return 1;
        if (s1[i] > s2[i]) return 3;
    }
    if (c1 < c2) return 1;
    if (c1 > c2) return 3;
    return 2;
}

static int WINAPI CompareStringEx_k32(PCWSTR locale_name, DWORD flags,
                                       PCWSTR s1, int c1, PCWSTR s2, int c2,
                                       PVOID version, PVOID reserved,
                                       LONG_PTR sort_handle)
{
    (void)locale_name;
    (void)version;
    (void)reserved;
    (void)sort_handle;
    return CompareStringW(0x0409, flags, s1, c1, s2, c2);
}
static WORD nls_ascii_ctype1(uint32_t c)
{
    WORD type = c <= 0x7F ? 0x0200 : 0; /* C1_DEFINED */
    if (c < 0x20 || c == 0x7F) type |= 0x0020; /* C1_CNTRL */
    if (c == ' ' || (c >= '\t' && c <= '\r')) type |= 0x0008; /* C1_SPACE */
    if (c == ' ' || c == '\t') type |= 0x0040; /* C1_BLANK */
    if (c >= '0' && c <= '9') type |= 0x0004 | 0x0080;
    if (c >= 'A' && c <= 'Z') type |= 0x0001 | 0x0100;
    if (c >= 'a' && c <= 'z') type |= 0x0002 | 0x0100;
    if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) type |= 0x0080;
    if (c >= 0x21 && c <= 0x7E && !(type & (0x0100 | 0x0004)))
        type |= 0x0010; /* C1_PUNCT */
    return type;
}

BOOL WINAPI GetStringTypeA(DWORD Locale, DWORD dwInfoType, PCSTR lpSrcStr,
                           int cchSrc, WORD *lpCharType)
{
    (void)Locale;
    if (dwInfoType != 1 || !lpSrcStr || !lpCharType || cchSrc == 0)
        return FALSE;
    if (cchSrc < 0) {
        cchSrc = 1;
        while (lpSrcStr[cchSrc - 1]) cchSrc++;
    }
    for (int i = 0; i < cchSrc; i++)
        lpCharType[i] = nls_ascii_ctype1((BYTE)lpSrcStr[i]);
    return TRUE;
}
BOOL WINAPI GetStringTypeW(DWORD dwInfoType, PCWSTR lpSrcStr, int cchSrc, WORD *lpCharType)
{
    if (dwInfoType != 1 || !lpSrcStr || !lpCharType || cchSrc == 0)
        return FALSE;
    if (cchSrc < 0) {
        cchSrc = 1;
        while (lpSrcStr[cchSrc - 1]) cchSrc++;
    }
    for (int i = 0; i < cchSrc; i++)
        lpCharType[i] = nls_ascii_ctype1(lpSrcStr[i]);
    return TRUE;
}
int WINAPI LCMapStringA(DWORD loc, DWORD flags, PCSTR src, int srclen, PSTR dst, int dstlen)
{
    (void)loc; (void)flags;
    if (srclen < 0) { int n=0; while(src[n]) n++; srclen = n+1; }
    if (dstlen == 0) return srclen;
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) dst[i] = src[i];
    return n;
}
int WINAPI LCMapStringW(DWORD loc, DWORD flags, PCWSTR src, int srclen, PWSTR dst, int dstlen)
{
    (void)loc; (void)flags;
    if (srclen < 0) { int n=0; while(src[n]) n++; srclen = n+1; }
    if (dstlen == 0) return srclen;
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) dst[i] = src[i];
    return n;
}

static int WINAPI LCMapStringEx_k32(PCWSTR locale_name, DWORD flags,
                                     PCWSTR src, int src_chars,
                                     PWSTR dst, int dst_chars,
                                     PVOID version, PVOID reserved,
                                     LONG_PTR sort_handle)
{
    (void)locale_name;
    (void)version;
    (void)reserved;
    (void)sort_handle;
    return LCMapStringW(0x0409, flags, src, src_chars, dst, dst_chars);
}
BOOL WINAPI IsBadReadPtr(PCVOID lp, SIZE_T ucb) { (void)lp; (void)ucb; return FALSE; }
BOOL WINAPI IsBadWritePtr(PVOID lp, SIZE_T ucb) { (void)lp; (void)ucb; return FALSE; }
BOOL WINAPI IsBadCodePtr(PVOID lpfn) { (void)lpfn; return FALSE; }

#define K32_PATHCCH_MAX_CCH                     0x8000ULL
#define K32_PATHCCH_ALLOW_LONG_PATHS            0x00000001U
#define K32_PATHCCH_FORCE_ENABLE_LONG_NAME      0x00000002U
#define K32_PATHCCH_FORCE_DISABLE_LONG_NAME     0x00000004U
#define K32_PATHCCH_DO_NOT_NORMALIZE_SEGMENTS   0x00000008U
#define K32_PATHCCH_ENSURE_EXTENDED             0x00000010U
#define K32_PATHCCH_ENSURE_TRAILING_SLASH       0x00000020U
#define K32_PATHCCH_VALID_FLAGS                  0x0000003FU
#define K32_PATHCCH_E_INVALIDARG                 ((LONG)0x80070057U)
#define K32_PATHCCH_E_OUTOFMEMORY                ((LONG)0x8007000EU)
#define K32_PATHCCH_E_INSUFFICIENT_BUFFER        ((LONG)0x8007007AU)
#define K32_PATHCCH_E_FILENAME_TOO_LONG          ((LONG)0x800700CEU)

static volatile uint32_t g_pathcch_trace_count;

static BOOL k32_pathcch_is_alpha(WCHAR ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static WCHAR k32_pathcch_ascii_lower(WCHAR ch)
{
    return ch >= 'A' && ch <= 'Z' ? (WCHAR)(ch + ('a' - 'A')) : ch;
}

static BOOL k32_pathcch_prefix_i(PCWSTR path, const char *prefix)
{
    if (!path || !prefix) return FALSE;
    while (*prefix) {
        if (!*path || k32_pathcch_ascii_lower(*path) !=
                          k32_pathcch_ascii_lower((WCHAR)(BYTE)*prefix))
            return FALSE;
        path++;
        prefix++;
    }
    return TRUE;
}

static BOOL k32_pathcch_drive(PCWSTR path)
{
    return path && k32_pathcch_is_alpha(path[0]) && path[1] == ':';
}

static BOOL k32_pathcch_prefixed_drive(PCWSTR path)
{
    return path && path[0] == '\\' && path[1] == '\\' &&
           path[2] == '?' && path[3] == '\\' &&
           k32_pathcch_drive(path + 4);
}

static BOOL k32_pathcch_prefixed_unc(PCWSTR path)
{
    return path && path[0] == '\\' && path[1] == '\\' &&
           path[2] == '?' && path[3] == '\\' &&
           k32_pathcch_prefix_i(path + 4, "UNC\\");
}

static BOOL k32_pathcch_prefixed_volume(PCWSTR path)
{
    if (!path || !k32_pathcch_prefix_i(path, "\\\\?\\Volume"))
        return FALSE;

    PCWSTR guid = path + 10;
    for (int i = 0; i <= 37; i++) {
        WCHAR ch = guid[i];
        if (i == 0) {
            if (ch != '{') return FALSE;
        } else if (i == 9 || i == 14 || i == 19 || i == 24) {
            if (ch != '-') return FALSE;
        } else if (i == 37) {
            if (ch != '}') return FALSE;
        } else if (!((ch >= '0' && ch <= '9') ||
                     (ch >= 'a' && ch <= 'f') ||
                     (ch >= 'A' && ch <= 'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

static PCWSTR k32_pathcch_next_segment(PCWSTR path)
{
    while (*path && *path != '\\') path++;
    return *path == '\\' ? path + 1 : path;
}

/* Return the first character after the root, matching PathCchSkipRoot. */
static PCWSTR k32_pathcch_root_end(PCWSTR path)
{
    PCWSTR end;

    if (k32_pathcch_prefixed_volume(path))
        return path + (path[48] == '\\' ? 49 : 48);
    if (k32_pathcch_prefixed_unc(path)) {
        end = k32_pathcch_next_segment(path + 8);
        return k32_pathcch_next_segment(end);
    }
    if (k32_pathcch_prefixed_drive(path))
        return path + (path[6] == '\\' ? 7 : 6);
    if (path[0] == '\\' && path[1] == '\\') {
        end = k32_pathcch_next_segment(path + 2);
        return *end == '\\' ? end : k32_pathcch_next_segment(end);
    }
    if (path[0] == '\\')
        return path + 1;
    if (k32_pathcch_drive(path))
        return path + (path[2] == '\\' ? 3 : 2);
    return NULL;
}

LONG WINAPI PathCchSkipRoot(PCWSTR path, PCWSTR *root_end)
{
    if (!path || !path[0] || !root_end)
        return K32_PATHCCH_E_INVALIDARG;

    /* A \\? prefix is valid only for a drive, UNC path, or volume GUID. */
    if (path[0] == '\\' && path[1] == '\\' && path[2] == '?' &&
        !k32_pathcch_prefixed_volume(path) &&
        !k32_pathcch_prefixed_unc(path) &&
        !k32_pathcch_prefixed_drive(path))
        return K32_PATHCCH_E_INVALIDARG;

    *root_end = k32_pathcch_root_end(path);
    if (!*root_end)
        return K32_PATHCCH_E_INVALIDARG;

    uint32_t trace = __atomic_fetch_add(&g_pathcch_trace_count, 1,
                                         __ATOMIC_RELAXED);
    if (trace < 12) {
        serial_puts("[K32-PATHCCH] SkipRoot offset=");
        serial_putdec((uint64_t)(*root_end - path));
        serial_puts("\n");
    }
    return 0;
}

static SIZE_T k32_pathcch_wcsnlen(PCWSTR path, SIZE_T limit)
{
    SIZE_T length = 0;
    if (!path) return 0;
    while (length < limit && path[length]) length++;
    return length;
}

static void k32_pathcch_copy(PWSTR dst, PCWSTR src, SIZE_T count)
{
    for (SIZE_T i = 0; i < count; i++) dst[i] = src[i];
}

static void k32_pathcch_move(PWSTR dst, PCWSTR src, SIZE_T count)
{
    if (dst < src) {
        for (SIZE_T i = 0; i < count; i++) dst[i] = src[i];
    } else if (dst > src) {
        while (count) {
            count--;
            dst[count] = src[count];
        }
    }
}

static SIZE_T k32_pathcch_strip_prefix(PWSTR path, SIZE_T length)
{
    if (k32_pathcch_prefixed_unc(path)) {
        path[0] = '\\';
        path[1] = '\\';
        k32_pathcch_move(path + 2, path + 8, length - 7);
        return length - 6;
    }
    if (k32_pathcch_prefixed_drive(path)) {
        k32_pathcch_move(path, path + 4, length - 3);
        return length - 4;
    }
    return length;
}

static void k32_pathcch_pop_component(PWSTR path, SIZE_T *length,
                                       SIZE_T root_length)
{
    SIZE_T n = *length;
    if (n <= root_length) return;
    if (path[n - 1] == '\\' && n - 1 >= root_length) n--;
    while (n > root_length && path[n - 1] != '\\') n--;
    *length = n;
}

static LONG k32_pathcch_canonicalize(PCWSTR input, ULONG flags,
                                      PWSTR *result_out, SIZE_T *length_out)
{
    SIZE_T input_length = k32_pathcch_wcsnlen(input, K32_PATHCCH_MAX_CCH);
    if (input_length == K32_PATHCCH_MAX_CCH)
        return K32_PATHCCH_E_FILENAME_TOO_LONG;
    if ((flags & ~K32_PATHCCH_VALID_FLAGS) ||
        ((flags & K32_PATHCCH_FORCE_ENABLE_LONG_NAME) &&
         (flags & K32_PATHCCH_FORCE_DISABLE_LONG_NAME)) ||
        ((flags & (K32_PATHCCH_FORCE_ENABLE_LONG_NAME |
                   K32_PATHCCH_FORCE_DISABLE_LONG_NAME)) &&
         !(flags & K32_PATHCCH_ALLOW_LONG_PATHS)) ||
        ((flags & K32_PATHCCH_ENSURE_EXTENDED) &&
         (flags & K32_PATHCCH_ALLOW_LONG_PATHS)))
        return K32_PATHCCH_E_INVALIDARG;
    if (input_length + 1 > 260 &&
        !(flags & (K32_PATHCCH_ALLOW_LONG_PATHS |
                   K32_PATHCCH_ENSURE_EXTENDED)))
        return K32_PATHCCH_E_FILENAME_TOO_LONG;

    SIZE_T capacity = input_length + 8;
    PWSTR result = (PWSTR)kmalloc(capacity * sizeof(WCHAR));
    if (!result) return K32_PATHCCH_E_OUTOFMEMORY;

    PCWSTR root_end = k32_pathcch_root_end(input);
    SIZE_T root_length = root_end ? (SIZE_T)(root_end - input) : 0;
    SIZE_T src = root_length;
    SIZE_T dst = 0;
    if (root_length) {
        k32_pathcch_copy(result, input, root_length);
        dst = root_length;
    }

    while (src < input_length) {
        SIZE_T segment = src;
        while (src < input_length && input[src] != '\\') src++;
        SIZE_T segment_length = src - segment;
        BOOL separator = src < input_length && input[src] == '\\';
        BOOL dot = segment_length == 1 && input[segment] == '.';
        BOOL dotdot = segment_length == 2 && input[segment] == '.' &&
                      input[segment + 1] == '.';

        if (dotdot) {
            k32_pathcch_pop_component(result, &dst, root_length);
        } else if (!dot) {
            k32_pathcch_copy(result + dst, input + segment, segment_length);
            dst += segment_length;
            if (separator) result[dst++] = '\\';
        }
        if ((dot || dotdot) && separator && dst == 2 &&
            root_length == 2 && k32_pathcch_drive(result)) {
            result[dst++] = '\\';
            root_length = dst;
        }
        if (separator) src++;
    }

    if (!(flags & K32_PATHCCH_DO_NOT_NORMALIZE_SEGMENTS)) {
        while (dst > root_length && result[dst - 1] == '.') {
            if (dst - 1 > root_length && result[dst - 2] == '*') break;
            dst--;
        }
    }
    if (!dst) result[dst++] = '\\';
    if (dst == 2 && k32_pathcch_drive(result)) result[dst++] = '\\';
    result[dst] = 0;

    BOOL extend = (flags & K32_PATHCCH_ENSURE_EXTENDED) != 0;
    if (!extend && dst + 1 > 260 &&
        (flags & K32_PATHCCH_ALLOW_LONG_PATHS) &&
        !(flags & K32_PATHCCH_FORCE_ENABLE_LONG_NAME))
        extend = TRUE;

    if (extend && k32_pathcch_drive(result)) {
        k32_pathcch_move(result + 4, result, dst + 1);
        result[0] = '\\'; result[1] = '\\'; result[2] = '?'; result[3] = '\\';
        dst += 4;
    } else if (extend && result[0] == '\\' && result[1] == '\\' &&
               result[2] != '?' && result[2] != '.') {
        k32_pathcch_move(result + 8, result + 2, dst - 1);
        result[0] = '\\'; result[1] = '\\'; result[2] = '?'; result[3] = '\\';
        result[4] = 'U'; result[5] = 'N'; result[6] = 'C'; result[7] = '\\';
        dst += 6;
    }

    if ((flags & K32_PATHCCH_ENSURE_TRAILING_SLASH) &&
        result[dst - 1] != '\\') {
        result[dst++] = '\\';
        result[dst] = 0;
    }
    if (dst + 1 > K32_PATHCCH_MAX_CCH) {
        kfree(result);
        return K32_PATHCCH_E_FILENAME_TOO_LONG;
    }

    *result_out = result;
    *length_out = dst;
    return 0;
}

LONG WINAPI PathCchCombineEx(PWSTR output, SIZE_T output_chars,
                              PCWSTR path, PCWSTR more, ULONG flags)
{
    static const WCHAR empty[] = {0};

    if (!output || !output_chars || output_chars > K32_PATHCCH_MAX_CCH)
        return K32_PATHCCH_E_INVALIDARG;

    SIZE_T path_length = k32_pathcch_wcsnlen(path, K32_PATHCCH_MAX_CCH);
    SIZE_T more_length = k32_pathcch_wcsnlen(more, K32_PATHCCH_MAX_CCH);
    if ((path && path_length == K32_PATHCCH_MAX_CCH) ||
        (more && more_length == K32_PATHCCH_MAX_CCH)) {
        output[0] = 0;
        return K32_PATHCCH_E_FILENAME_TOO_LONG;
    }

    PCWSTR base = path ? path : empty;
    PCWSTR tail = more ? more : empty;
    BOOL tail_is_full = k32_pathcch_drive(tail) ||
                        (tail[0] == '\\' && tail[1] == '\\');
    if (tail_is_full) {
        base = tail;
        path_length = more_length;
        tail = empty;
        more_length = 0;
    }

    if (path_length + more_length + 3 > K32_PATHCCH_MAX_CCH) {
        output[0] = 0;
        return K32_PATHCCH_E_FILENAME_TOO_LONG;
    }
    SIZE_T combined_capacity = path_length + more_length + 4;
    PWSTR combined = (PWSTR)kmalloc(combined_capacity * sizeof(WCHAR));
    if (!combined) {
        output[0] = 0;
        return K32_PATHCCH_E_OUTOFMEMORY;
    }
    k32_pathcch_copy(combined, base, path_length);
    combined[path_length] = 0;
    path_length = k32_pathcch_strip_prefix(combined, path_length);

    if (tail[0] == '\\' && tail[1] != '\\') {
        PCWSTR root = k32_pathcch_root_end(combined);
        if (root) {
            path_length = (SIZE_T)(root - combined);
            combined[path_length] = 0;
        }
        tail++;
        more_length--;
    }
    if (more_length) {
        if (path_length && combined[path_length - 1] != '\\')
            combined[path_length++] = '\\';
        k32_pathcch_copy(combined + path_length, tail, more_length);
        path_length += more_length;
        combined[path_length] = 0;
    } else if (tail_is_full && path_length == 2 &&
               k32_pathcch_drive(combined)) {
        combined[path_length++] = '\\';
        combined[path_length] = 0;
    }

    PWSTR normalized = NULL;
    SIZE_T normalized_length = 0;
    LONG status = k32_pathcch_canonicalize(combined, flags, &normalized,
                                            &normalized_length);
    kfree(combined);
    if (status < 0) {
        output[0] = 0;
        return status;
    }
    if (normalized_length + 1 > output_chars) {
        output[0] = 0;
        kfree(normalized);
        return K32_PATHCCH_E_INSUFFICIENT_BUFFER;
    }
    k32_pathcch_copy(output, normalized, normalized_length + 1);
    kfree(normalized);

    uint32_t trace = __atomic_fetch_add(&g_pathcch_trace_count, 1,
                                         __ATOMIC_RELAXED);
    if (trace < 12) {
        serial_puts("[K32-PATHCCH] Combine length=");
        serial_putdec(normalized_length);
        serial_puts(" flags=0x");
        serial_puthex(flags, 8);
        serial_puts("\n");
    }
    return 0;
}

DWORD WINAPI GetFullPathNameW(PCWSTR lpFileName, DWORD nBufferLength,
                               PWSTR lpBuffer, PWSTR *lpFilePart)
{
    if (!lpFileName) return 0;
    char input[260], path[260];
    int input_len = 0;
    while (lpFileName[input_len] && input_len < 259) {
        input[input_len] = (char)(lpFileName[input_len] & 0xFF);
        input_len++;
    }
    input[input_len] = 0;
    if (!win32_normalize_path(input, path)) return 0;
    DWORD path_len = (DWORD)strlen(path);
    DWORD len = 3 + path_len;
    if (!lpBuffer) return nBufferLength == 0 ? len + 1 : 0;
    if (len >= nBufferLength) return len + 1;
    lpBuffer[0] = 'C'; lpBuffer[1] = ':'; lpBuffer[2] = '\\';
    for (DWORD i = 0; i <= path_len; i++)
        lpBuffer[i + 3] = (WCHAR)(unsigned char)path[i];
    if (lpFilePart) {
        *lpFilePart = lpBuffer;
        for (DWORD i = 0; i < len; i++)
            if (lpBuffer[i] == '\\' || lpBuffer[i] == '/')
                *lpFilePart = lpBuffer + i + 1;
    }
    return len;
}

static DWORD WINAPI GetLongPathNameW_k32(PCWSTR short_path, PWSTR long_path,
                                          DWORD buffer_chars)
{
    if (!short_path) return 0;
    DWORD len = (DWORD)lstrlenW(short_path);
    if (!long_path || buffer_chars <= len) return len + 1;
    lstrcpynW_k32(long_path, short_path, (int)buffer_chars);
    return len;
}

static BOOL WINAPI GetVolumePathNameW_k32(PCWSTR file_name, PWSTR volume_path,
                                           DWORD buffer_chars)
{
    if (!file_name || !volume_path) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (buffer_chars < 4) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }
    volume_path[0] = 'C';
    volume_path[1] = ':';
    volume_path[2] = '\\';
    volume_path[3] = 0;
    return TRUE;
}

static BOOL WINAPI GetVolumeNameForVolumeMountPointW_k32(
    PCWSTR mount_point, PWSTR volume_name, DWORD buffer_chars)
{
    static const WCHAR name[] = {
        '\\','\\','?','\\','V','o','l','u','m','e','{',
        '0','0','0','0','0','0','0','0','-','0','0','0','0','-',
        '0','0','0','0','-','0','0','0','0','-',
        '0','0','0','0','0','0','0','0','0','0','0','1','}','\\',0
    };
    if (!mount_point || !volume_name) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    DWORD required = (DWORD)(sizeof(name) / sizeof(name[0]));
    if (buffer_chars < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }
    for (DWORD i = 0; i < required; i++) volume_name[i] = name[i];
    return TRUE;
}

static BOOL WINAPI GetVolumePathNamesForVolumeNameW_k32(
    PCWSTR volume_name, PWSTR volume_paths, DWORD buffer_chars,
    DWORD *required_chars)
{
    const DWORD required = 5; /* "C:\\" plus the MULTI_SZ double NUL */
    if (!volume_name || !required_chars) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    *required_chars = required;
    if (!volume_paths || buffer_chars < required) {
        SetLastError(234); /* ERROR_MORE_DATA */
        return FALSE;
    }
    volume_paths[0] = 'C';
    volume_paths[1] = ':';
    volume_paths[2] = '\\';
    volume_paths[3] = 0;
    volume_paths[4] = 0;
    return TRUE;
}

static BOOL WINAPI GetVolumeInformationW_k32(
    PCWSTR root_path, PWSTR volume_name, DWORD volume_name_chars,
    DWORD *volume_serial, DWORD *max_component_chars, DWORD *filesystem_flags,
    PWSTR filesystem_name, DWORD filesystem_name_chars)
{
    extern const char *osfs2_label(void);
    static const WCHAR filesystem_label[] = { 'O','S','I','T','O','F','S',0 };
    const char *volume_label = osfs2_label();
    if (!volume_label || !volume_label[0]) volume_label = "OSITO";
    const DWORD volume_required = (DWORD)strlen(volume_label) + 1;
    const DWORD filesystem_required =
        (DWORD)(sizeof(filesystem_label) / sizeof(filesystem_label[0]));

    (void)root_path; /* NULL selects the current volume on Windows. */

    if ((volume_name && volume_name_chars < volume_required) ||
        (filesystem_name && filesystem_name_chars < filesystem_required)) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }

    if (volume_name) {
        for (DWORD i = 0; i < volume_required; i++)
            volume_name[i] = (WCHAR)(unsigned char)volume_label[i];
    }
    if (filesystem_name) {
        for (DWORD i = 0; i < filesystem_required; i++)
            filesystem_name[i] = filesystem_label[i];
    }
    if (volume_serial) *volume_serial = 0x4F534954; /* OSIT */
    if (max_component_chars) *max_component_chars = 255;
    if (filesystem_flags) {
        *filesystem_flags = 0x00000002 | /* FILE_CASE_PRESERVED_NAMES */
                            0x00000004;  /* FILE_UNICODE_ON_DISK */
    }

    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI GetVolumeInformationA_k32(
    PCSTR root_path, PSTR volume_name, DWORD volume_name_chars,
    DWORD *volume_serial, DWORD *max_component_chars, DWORD *filesystem_flags,
    PSTR filesystem_name, DWORD filesystem_name_chars)
{
    extern const char *osfs2_label(void);
    static const char filesystem_label[] = "OSITOFS";
    const char *volume_label = osfs2_label();
    if (!volume_label || !volume_label[0]) volume_label = "OSITO";
    DWORD volume_required = (DWORD)strlen(volume_label) + 1;
    DWORD filesystem_required = (DWORD)sizeof(filesystem_label);

    (void)root_path; /* NULL selects the current volume on Windows. */

    if ((volume_name && volume_name_chars < volume_required) ||
        (filesystem_name && filesystem_name_chars < filesystem_required)) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }

    if (volume_name)
        memcpy(volume_name, volume_label, volume_required);
    if (filesystem_name)
        memcpy(filesystem_name, filesystem_label, filesystem_required);
    if (volume_serial) *volume_serial = 0x4F534954; /* OSIT */
    if (max_component_chars) *max_component_chars = 255;
    if (filesystem_flags) {
        *filesystem_flags = 0x00000002 | /* FILE_CASE_PRESERVED_NAMES */
                            0x00000004;  /* FILE_UNICODE_ON_DISK */
    }

    SetLastError(0);
    return TRUE;
}

DWORD WINAPI GetFileAttributesW(PCWSTR lpFileName)
{
    if (!lpFileName) return (DWORD)-1;
    /* Convert wide to narrow and delegate */
    char narrow[260];
    int i = 0;
    for (; lpFileName[i] && i < 259; i++)
        narrow[i] = (char)(lpFileName[i] & 0xFF);
    narrow[i] = 0;
    return GetFileAttributesA(narrow);
}

typedef struct __attribute__((packed)) _WIN32_FILE_ATTRIBUTE_DATA32 {
    DWORD dwFileAttributes;
    DWORD ftCreationTimeLo, ftCreationTimeHi;
    DWORD ftLastAccessTimeLo, ftLastAccessTimeHi;
    DWORD ftLastWriteTimeLo, ftLastWriteTimeHi;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA32;

_Static_assert(sizeof(WIN32_FILE_ATTRIBUTE_DATA32) == 36,
               "WIN32_FILE_ATTRIBUTE_DATA must match the Win32 ABI");

static BOOL WINAPI GetFileAttributesExW_k32(
    PCWSTR file_name, DWORD info_level, WIN32_FILE_ATTRIBUTE_DATA32 *data)
{
    if (!file_name || !data || info_level != 0) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD attributes = GetFileAttributesW(file_name);
    if (attributes == (DWORD)-1)
        return FALSE;

    memset(data, 0, sizeof(*data));
    data->dwFileAttributes = attributes;
    if (!(attributes & 0x10)) { /* FILE_ATTRIBUTE_DIRECTORY */
        HANDLE file = CreateFileW(file_name, GENERIC_READ, 3, NULL, 3, 0, NULL);
        LARGE_INTEGER size;
        if (file == INVALID_HANDLE_VALUE)
            return FALSE;
        BOOL ok = GetFileSizeEx_k32(file, &size);
        CloseHandle(file);
        if (!ok)
            return FALSE;
        data->nFileSizeHigh = size.HighPart;
        data->nFileSizeLow = size.LowPart;
    }
    return TRUE;
}

BOOL WINAPI FileTimeToLocalFileTime(PCVOID lpFileTime, PVOID lpLocalFileTime)
{
    if (!lpFileTime || !lpLocalFileTime) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    memcpy(lpLocalFileTime, lpFileTime, sizeof(ULONGLONG));
    return TRUE;
}

BOOL WINAPI FileTimeToSystemTime(PCVOID lpFileTime, SYSTEMTIME *lpSystemTime)
{
    if (!lpFileTime || !lpSystemTime) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    ULONGLONG filetime;
    WINTIME_CALENDAR calendar;
    memcpy(&filetime, lpFileTime, sizeof(filetime));
    if (wintime_filetime_to_calendar(filetime, &calendar) < 0) {
        SetLastError(87);
        return FALSE;
    }
    k32_calendar_to_systemtime(&calendar, lpSystemTime);
    return TRUE;
}

static BOOL WINAPI SystemTimeToTzSpecificLocalTime_k32(
    PCVOID timezone, const SYSTEMTIME *utc, SYSTEMTIME *local)
{
    (void)timezone;
    if (!utc || !local) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    *local = *utc; /* Osito currently exposes a zero-bias UTC time zone. */
    return TRUE;
}

static BOOL WINAPI TzSpecificLocalTimeToSystemTime_k32(
    PCVOID timezone, const SYSTEMTIME *local, SYSTEMTIME *utc)
{
    return SystemTimeToTzSpecificLocalTime_k32(timezone, local, utc);
}

DWORD WINAPI GetTimeZoneInformation(PVOID lpTimeZoneInformation)
{
    if (lpTimeZoneInformation) {
        BYTE *p = (BYTE *)lpTimeZoneInformation;
        for (int i = 0; i < 172; i++) p[i] = 0;
    }
    return 0; /* TIME_ZONE_ID_UNKNOWN */
}

DWORD WINAPI GetDynamicTimeZoneInformation(
    DYNAMIC_TIME_ZONE_INFORMATION *timezone)
{
    if (!timezone) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0xFFFFFFFFU; /* TIME_ZONE_ID_INVALID */
    }

    memset(timezone, 0, sizeof(*timezone));
    timezone->StandardName[0] = 'U';
    timezone->StandardName[1] = 'T';
    timezone->StandardName[2] = 'C';
    timezone->TimeZoneKeyName[0] = 'U';
    timezone->TimeZoneKeyName[1] = 'T';
    timezone->TimeZoneKeyName[2] = 'C';
    timezone->DynamicDaylightTimeDisabled = TRUE;
    return 0; /* TIME_ZONE_ID_UNKNOWN */
}

DWORD WINAPI GetLogicalDrives(void) { return 0x04; } /* C: drive */

static DWORD WINAPI GetLogicalDriveStringsW_k32(DWORD capacity,
                                                 PWSTR buffer)
{
    static const WCHAR drives[] = {'C',':','\x5C',0,0};
    DWORD required = (DWORD)(sizeof(drives) / sizeof(drives[0]));
    if (!buffer || capacity < required)
        return required;
    for (DWORD i = 0; i < required; i++) buffer[i] = drives[i];
    return required - 1; /* Excludes the final list terminator. */
}

static DWORD WINAPI GetLogicalDriveStringsA_k32(DWORD capacity,
                                                 PSTR buffer)
{
    static const char drives[] = {'C',':','\\',0,0};
    DWORD required = (DWORD)sizeof(drives);
    if (!buffer || capacity < required)
        return required;
    memcpy(buffer, drives, required);
    return required - 1; /* Excludes the final list terminator. */
}

UINT WINAPI GetDriveTypeA(PCSTR lpRootPathName) { (void)lpRootPathName; return 3; } /* DRIVE_FIXED */
UINT WINAPI GetDriveTypeW(PCWSTR lpRootPathName) { (void)lpRootPathName; return 3; }

BOOL WINAPI GetDiskFreeSpaceA(PCSTR lpRoot, DWORD *lpSPC, DWORD *lpBPS,
                                DWORD *lpFC, DWORD *lpTC)
{
    (void)lpRoot;
    extern uint32_t osfs2_free_blocks(void);
    extern uint32_t osfs2_get_block_size(void);
    extern uint32_t osfs2_total_blocks(void);
    uint32_t block_size = osfs2_get_block_size();
    uint32_t total_blocks = osfs2_total_blocks();
    if (!block_size || !total_blocks) return FALSE;

    if (lpSPC) *lpSPC = block_size / 512;
    if (lpBPS) *lpBPS = 512;
    if (lpFC)  *lpFC  = osfs2_free_blocks();
    if (lpTC)  *lpTC  = total_blocks;
    return TRUE;
}

static BOOL k32_get_disk_free_space_ex(PULARGE_INTEGER lpFreeAvailable,
                                       PULARGE_INTEGER lpTotalBytes,
                                       PULARGE_INTEGER lpTotalFree)
{
    extern uint32_t osfs2_free_blocks(void);
    extern uint32_t osfs2_get_block_size(void);
    extern uint32_t osfs2_total_blocks(void);
    uint64_t block_size = osfs2_get_block_size();
    uint64_t total_blocks = osfs2_total_blocks();
    if (!block_size || !total_blocks) return FALSE;

    uint64_t free_bytes = (uint64_t)osfs2_free_blocks() * block_size;
    uint64_t total_bytes = total_blocks * block_size;
    if (lpFreeAvailable) lpFreeAvailable->QuadPart = free_bytes;
    if (lpTotalBytes)    lpTotalBytes->QuadPart = total_bytes;
    if (lpTotalFree)     lpTotalFree->QuadPart = free_bytes;
    return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceExA(PCSTR lpRoot, PULARGE_INTEGER lpFreeAvailable,
                                 PULARGE_INTEGER lpTotalBytes,
                                 PULARGE_INTEGER lpTotalFree)
{
    (void)lpRoot;
    return k32_get_disk_free_space_ex(lpFreeAvailable, lpTotalBytes,
                                      lpTotalFree);
}

BOOL WINAPI GetDiskFreeSpaceExW(PCWSTR lpRoot, PULARGE_INTEGER lpFreeAvailable,
                                 PULARGE_INTEGER lpTotalBytes,
                                 PULARGE_INTEGER lpTotalFree)
{
    (void)lpRoot;
    return k32_get_disk_free_space_ex(lpFreeAvailable, lpTotalBytes,
                                      lpTotalFree);
}

/* ── INI File (Private Profile) ────────────────────────────── */
/*
 * In-memory INI store. UT99 reads UnrealTournament.ini, User.ini, etc.
 * We keep a flat array of section+key→value entries.
 * File parameter is ignored (all INI data is in one global store).
 */

#define INI_MAX_ENTRIES 1024
#define INI_MAX_SECTION  64
#define INI_MAX_KEY      64
#define INI_MAX_VALUE   512

typedef struct {
    char section[INI_MAX_SECTION];
    char key[INI_MAX_KEY];
    char value[INI_MAX_VALUE];
} INI_ENTRY;

static INI_ENTRY g_ini_store[INI_MAX_ENTRIES];
static int       g_ini_count = 0;

/* Track which INI files have been loaded from OsitoFS */
#define INI_FILES_MAX 8
static char ini_loaded_files[INI_FILES_MAX][260];
static int  ini_loaded_count = 0;

static int ini_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void ini_strcpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; src[i] && i < max - 1; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static int ini_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static INI_ENTRY *ini_find(const char *section, const char *key)
{
    for (int i = 0; i < g_ini_count; i++) {
        if (ini_stricmp(g_ini_store[i].section, section) == 0 &&
            ini_stricmp(g_ini_store[i].key, key) == 0)
            return &g_ini_store[i];
    }
    return NULL;
}

/* Add INI entry (allows duplicates — UT99 uses multi-value keys like Paths=) */
static void ini_add(const char *section, const char *key, const char *value)
{
    if (g_ini_count >= INI_MAX_ENTRIES) return;
    INI_ENTRY *e = &g_ini_store[g_ini_count++];
    ini_strcpy(e->section, section, INI_MAX_SECTION);
    ini_strcpy(e->key, key, INI_MAX_KEY);
    ini_strcpy(e->value, value, INI_MAX_VALUE);
}

/* Parse and load an INI file from OsitoFS into the INI store */
static void ini_load_from_osfs(const char *filename)
{
    if (!filename || !*filename) return;

    /* Check if already loaded */
    const char *base = filename;
    for (const char *p = filename; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    for (int i = 0; i < ini_loaded_count; i++) {
        if (ini_stricmp(ini_loaded_files[i], filename) == 0) return;
    }

    void *f = osfs2_find_ci(filename);
    if (!f && base != filename) f = osfs2_find_ci(base);
    if (!f) return;

    uint64_t fsize = osfs2_file_size(f);
    if (fsize == 0 || fsize > 64 * 1024) return; /* sanity limit */

    /* Allocate temp buffer and read */
    extern void *kmalloc(uint64_t size);
    extern void kfree(void *ptr);
    char *buf = (char *)kmalloc(fsize + 1);
    if (!buf) return;
    osfs2_read(f, 0, buf, fsize);
    buf[fsize] = 0;

    serial_puts("[INI] Loading ");
    serial_puts(base);
    serial_puts(" (");
    serial_putdec(fsize);
    serial_puts(" bytes)\n");

    /* Track as loaded */
    if (ini_loaded_count < INI_FILES_MAX)
        ini_strcpy(ini_loaded_files[ini_loaded_count++], filename, 260);

    /* Parse: [Section] and Key=Value lines */
    char cur_section[INI_MAX_SECTION] = "";
    char *p = buf;
    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '[') {
            /* Section header */
            p++;
            char *start = p;
            while (*p && *p != ']' && *p != '\r' && *p != '\n') p++;
            int len = (int)(p - start);
            if (len >= INI_MAX_SECTION) len = INI_MAX_SECTION - 1;
            for (int i = 0; i < len; i++) cur_section[i] = start[i];
            cur_section[len] = 0;
            if (*p == ']') p++;
        } else if (*p == ';' || *p == '#' || *p == '\r' || *p == '\n') {
            /* Comment or empty line — skip */
        } else if (cur_section[0]) {
            /* Key=Value */
            char key[INI_MAX_KEY] = "";
            char val[INI_MAX_VALUE] = "";
            char *start = p;
            while (*p && *p != '=' && *p != '\r' && *p != '\n') p++;
            if (*p == '=') {
                int klen = (int)(p - start);
                if (klen >= INI_MAX_KEY) klen = INI_MAX_KEY - 1;
                for (int i = 0; i < klen; i++) key[i] = start[i];
                key[klen] = 0;
                p++; /* skip '=' */
                start = p;
                while (*p && *p != '\r' && *p != '\n') p++;
                int vlen = (int)(p - start);
                if (vlen >= INI_MAX_VALUE) vlen = INI_MAX_VALUE - 1;
                for (int i = 0; i < vlen; i++) val[i] = start[i];
                val[vlen] = 0;
                ini_add(cur_section, key, val);
            }
        }
        /* Skip to end of line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    serial_puts("[INI] Loaded ");
    serial_putdec(g_ini_count);
    serial_puts(" total entries\n");
    kfree(buf);
}

DWORD WINAPI GetPrivateProfileStringA(PCSTR lpAppName, PCSTR lpKeyName,
                                       PCSTR lpDefault, PSTR lpReturnedString,
                                       DWORD nSize, PCSTR lpFileName)
{
    /* Auto-load INI file from OsitoFS on first access */
    if (lpFileName)
        ini_load_from_osfs(lpFileName);

    if (!lpReturnedString || nSize == 0)
        return 0;

    /* If section is NULL, enumerate section names */
    if (!lpAppName) {
        if (lpReturnedString && nSize > 0)
            lpReturnedString[0] = 0;
        return 0;
    }

    /* If key is NULL, enumerate keys in section */
    if (!lpKeyName) {
        if (lpReturnedString && nSize > 0)
            lpReturnedString[0] = 0;
        return 0;
    }

    const char *result = lpDefault ? lpDefault : "";

    /* Filter out ServerActors — UT99 loads IpDrv/IpServer/UWeb DLLs
     * which don't exist on OsitoK, causing ExecWarning + appError. */
    if (lpKeyName && ini_stricmp(lpKeyName, "ServerActors") == 0) {
        if (lpReturnedString && nSize > 0) lpReturnedString[0] = 0;
        return 0;
    }

    INI_ENTRY *entry = ini_find(lpAppName, lpKeyName);
    if (entry)
        result = entry->value;

    int len = ini_strlen(result);
    if ((DWORD)len >= nSize) len = (int)nSize - 1;
    for (int i = 0; i < len; i++)
        lpReturnedString[i] = result[i];
    lpReturnedString[len] = 0;
    return (DWORD)len;
}

static void ini_wide_to_ansi(PCWSTR source, char *destination,
                             DWORD destination_size)
{
    DWORD i = 0;
    if (!destination_size) return;
    if (source) {
        while (source[i] && i + 1 < destination_size) {
            destination[i] = source[i] <= 0xFF ? (char)source[i] : '?';
            i++;
        }
    }
    destination[i] = 0;
}

static DWORD WINAPI GetPrivateProfileStringW_k32(
    PCWSTR app_name, PCWSTR key_name, PCWSTR default_value,
    PWSTR returned_string, DWORD size, PCWSTR file_name)
{
    if (!returned_string || size == 0)
        return 0;

    char app[INI_MAX_SECTION];
    char key[INI_MAX_KEY];
    char fallback[INI_MAX_VALUE];
    char file[260];
    if (app_name) ini_wide_to_ansi(app_name, app, sizeof(app));
    if (key_name) ini_wide_to_ansi(key_name, key, sizeof(key));
    if (default_value)
        ini_wide_to_ansi(default_value, fallback, sizeof(fallback));
    if (file_name) ini_wide_to_ansi(file_name, file, sizeof(file));

    extern void *kmalloc(uint64_t bytes);
    extern void kfree(void *memory);
    char *narrow = kmalloc(size);
    if (!narrow) {
        returned_string[0] = 0;
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }

    DWORD length = GetPrivateProfileStringA(
        app_name ? app : NULL, key_name ? key : NULL,
        default_value ? fallback : NULL, narrow, size,
        file_name ? file : NULL);
    for (DWORD i = 0; i < length; i++)
        returned_string[i] = (BYTE)narrow[i];
    returned_string[length] = 0;
    kfree(narrow);
    return length;
}

BOOL WINAPI WritePrivateProfileStringA(PCSTR lpAppName, PCSTR lpKeyName,
                                        PCSTR lpString, PCSTR lpFileName)
{
    (void)lpFileName;

    if (!lpAppName) return FALSE;

    /* Delete key if lpString is NULL */
    if (!lpKeyName || !lpString) return TRUE;

    INI_ENTRY *entry = ini_find(lpAppName, lpKeyName);
    if (entry) {
        ini_strcpy(entry->value, lpString, INI_MAX_VALUE);
        return TRUE;
    }

    if (g_ini_count >= INI_MAX_ENTRIES) return FALSE;

    entry = &g_ini_store[g_ini_count++];
    ini_strcpy(entry->section, lpAppName, INI_MAX_SECTION);
    ini_strcpy(entry->key, lpKeyName, INI_MAX_KEY);
    ini_strcpy(entry->value, lpString, INI_MAX_VALUE);
    return TRUE;
}

UINT WINAPI GetPrivateProfileIntA(PCSTR lpAppName, PCSTR lpKeyName,
                                   int nDefault, PCSTR lpFileName)
{
    char buf[32];
    DWORD len = GetPrivateProfileStringA(lpAppName, lpKeyName, NULL, buf, 32, lpFileName);
    if (len == 0) return (UINT)nDefault;

    /* Simple atoi */
    int result = 0, sign = 1, i = 0;
    if (buf[0] == '-') { sign = -1; i = 1; }
    for (; buf[i] >= '0' && buf[i] <= '9'; i++)
        result = result * 10 + (buf[i] - '0');
    return (UINT)(result * sign);
}

DWORD WINAPI GetPrivateProfileSectionNamesA(PSTR lpszReturnBuffer,
                                             DWORD nSize, PCSTR lpFileName)
{
    (void)lpFileName;
    /* Return empty double-null-terminated buffer */
    if (lpszReturnBuffer && nSize >= 2) {
        lpszReturnBuffer[0] = 0;
        lpszReturnBuffer[1] = 0;
    }
    return 0;
}

/* ── Stubs for MSVCRT.dll CRT init dependencies ───────────── */

/* These are called by the real MSVCRT.dll during CRT initialization.
 * They need to exist as stubs to prevent NULL IAT entries → crashes. */

static BOOL WINAPI HeapCompact_stub(HANDLE hHeap, DWORD dwFlags)
{
    (void)hHeap; (void)dwFlags;
    return 1;  /* report success */
}

static BOOL WINAPI HeapWalk_stub(HANDLE hHeap, void *lpEntry)
{
    (void)hHeap; (void)lpEntry;
    g_last_error = 0x12; /* ERROR_NO_MORE_ITEMS */
    sync_last_error();
    return FALSE;
}

static BOOL WINAPI ReadConsoleA_stub(HANDLE h, void *buf, DWORD n, DWORD *read, void *r)
{
    (void)h; (void)buf; (void)n; (void)r;
    if (read) *read = 0;
    return FALSE;
}

typedef struct {
    int16_t X;
    int16_t Y;
} K32_COORD;

typedef struct {
    int16_t Left;
    int16_t Top;
    int16_t Right;
    int16_t Bottom;
} K32_SMALL_RECT;

typedef struct {
    K32_COORD dwSize;
    K32_COORD dwCursorPosition;
    WORD wAttributes;
    K32_SMALL_RECT srWindow;
    K32_COORD dwMaximumWindowSize;
} K32_CONSOLE_SCREEN_BUFFER_INFO;

static DWORD g_console_input_mode = 0x0007;  /* processed | line | echo */
static DWORD g_console_output_mode = 0x0003; /* processed | wrap-at-EOL */
#define K32_CONSOLE_TITLE_MAX 32768
static WCHAR g_console_title[K32_CONSOLE_TITLE_MAX] = {
    'O', 's', 'i', 't', 'o', 'K', 0
};

static BOOL WINAPI SetConsoleTitleW_k32(PCWSTR title)
{
    if (!title) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    SIZE_T length = 0;
    while (title[length] && length < K32_CONSOLE_TITLE_MAX - 1)
        length++;
    if (title[length]) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return FALSE;
    }
    for (SIZE_T i = 0; i <= length; i++)
        g_console_title[i] = title[i];
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetConsoleTitleA_k32(PCSTR title)
{
    if (!title) {
        SetLastError(87);
        return FALSE;
    }
    SIZE_T length = 0;
    while (title[length] && length < K32_CONSOLE_TITLE_MAX - 1)
        length++;
    if (title[length]) {
        SetLastError(206);
        return FALSE;
    }
    for (SIZE_T i = 0; i < length; i++)
        g_console_title[i] = (WCHAR)(BYTE)title[i];
    g_console_title[length] = 0;
    SetLastError(0);
    return TRUE;
}

static DWORD WINAPI GetConsoleTitleW_k32(PWSTR title, DWORD capacity)
{
    if (!title || !capacity) {
        SetLastError(87);
        return 0;
    }
    DWORD length = 0;
    while (g_console_title[length])
        length++;
    DWORD copied = length < capacity - 1 ? length : capacity - 1;
    for (DWORD i = 0; i < copied; i++)
        title[i] = g_console_title[i];
    title[copied] = 0;
    SetLastError(0);
    return copied;
}

static DWORD WINAPI GetConsoleTitleA_k32(PSTR title, DWORD capacity)
{
    if (!title || !capacity) {
        SetLastError(87);
        return 0;
    }
    DWORD length = 0;
    while (g_console_title[length])
        length++;
    DWORD copied = length < capacity - 1 ? length : capacity - 1;
    for (DWORD i = 0; i < copied; i++)
        title[i] = (char)(g_console_title[i] & 0xFF);
    title[copied] = 0;
    SetLastError(0);
    return copied;
}

static BOOL k32_is_console_input(HANDLE handle)
{
    return handle == console_handle(WIN32_STD_INPUT_HANDLE);
}

static BOOL k32_is_console_output(HANDLE handle)
{
    return handle == console_handle(WIN32_STD_OUTPUT_HANDLE) ||
           handle == console_handle(WIN32_STD_ERROR_HANDLE);
}

static UINT WINAPI GetConsoleCP_k32(void)
{
    return GetOEMCP();
}

static UINT WINAPI GetConsoleOutputCP_stub(void)
{
    return GetOEMCP();
}

static BOOL WINAPI GetConsoleScreenBufferInfo_stub(
    HANDLE output, K32_CONSOLE_SCREEN_BUFFER_INFO *info)
{
    if (!info || (output != console_handle(WIN32_STD_OUTPUT_HANDLE) &&
                  output != console_handle(WIN32_STD_ERROR_HANDLE))) {
        SetLastError(!info ? 87 : 6); /* INVALID_PARAMETER / INVALID_HANDLE */
        return FALSE;
    }

    info->dwSize.X = 80;
    info->dwSize.Y = 25;
    info->dwCursorPosition.X = 0;
    info->dwCursorPosition.Y = 0;
    info->wAttributes = 7;
    info->srWindow.Left = 0;
    info->srWindow.Top = 0;
    info->srWindow.Right = 79;
    info->srWindow.Bottom = 24;
    info->dwMaximumWindowSize = info->dwSize;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetConsoleTextAttribute_stub(HANDLE output,
                                                 WORD attributes)
{
    (void)attributes;
    if (output != console_handle(WIN32_STD_OUTPUT_HANDLE) &&
        output != console_handle(WIN32_STD_ERROR_HANDLE)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI ReadConsoleW_stub(HANDLE input, PVOID buffer, DWORD count,
                                     DWORD *read, PVOID reserved)
{
    (void)input;
    (void)buffer;
    (void)count;
    (void)reserved;
    if (read) *read = 0;
    SetLastError(6); /* No interactive Win32 console input yet. */
    return FALSE;
}

static BOOL WINAPI WriteConsoleW_stub(HANDLE output, PCVOID buffer,
                                      DWORD count, DWORD *written,
                                      PVOID reserved)
{
    (void)reserved;
    if (written) *written = 0;
    if (!buffer) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    const WCHAR *wide = (const WCHAR *)buffer;
    DWORD total = 0;
    while (total < count) {
        char text[128];
        DWORD chunk = count - total;
        if (chunk > sizeof(text)) chunk = sizeof(text);
        for (DWORD i = 0; i < chunk; i++) {
            WCHAR ch = wide[total + i];
            text[i] = ch <= 0x7f ? (char)ch : '?';
        }

        DWORD bytes = 0;
        if (!WriteFile(output, text, chunk, &bytes, NULL))
            return FALSE;
        total += bytes;
        if (bytes != chunk)
            break;
    }

    if (written) *written = total;
    return total == count;
}

static BOOL WINAPI SetConsoleMode_stub(HANDLE h, DWORD mode)
{
    if (k32_is_console_input(h)) {
        g_console_input_mode = mode;
    } else if (k32_is_console_output(h)) {
        g_console_output_mode = mode;
    } else {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI GetConsoleMode_stub(HANDLE h, DWORD *mode)
{
    if (!mode) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (k32_is_console_input(h)) {
        *mode = g_console_input_mode;
    } else if (k32_is_console_output(h)) {
        *mode = g_console_output_mode;
    } else {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetEndOfFile_stub(HANDLE h)
{
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION position;
    NTSTATUS status = NtQueryInformationFile(h, &iosb, &position,
                                              sizeof(position),
                                              FilePositionInformation);
    if (NT_SUCCESS(status))
        status = NtSetInformationFile(h, &iosb, &position.CurrentByteOffset,
                                      sizeof(position.CurrentByteOffset),
                                      FileEndOfFileInformation);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI ReadDirectoryChangesW_k32(HANDLE directory, PVOID buffer,
                                               DWORD buffer_size,
                                               BOOL watch_subtree,
                                               DWORD notify_filter,
                                               DWORD *bytes_returned,
                                               PVOID overlapped,
                                               PVOID completion_routine)
{
    (void)watch_subtree;
    (void)notify_filter;
    (void)overlapped;
    (void)completion_routine;
    if (bytes_returned) *bytes_returned = 0;
    if (!buffer || !buffer_size) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    FILE_OBJECT *file = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, directory, OBJ_TYPE_FILE,
                                    (PVOID *)&file);
    if (!NT_SUCCESS(status) || !file ||
        !(file->flags & FILE_OBJ_DIRECTORY)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    static bool logged;
    if (!logged) {
        serial_puts("[K32] ReadDirectoryChangesW: change notifications "
                    "unsupported; watcher disabled\n");
        logged = true;
    }
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return FALSE;
}

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    DWORD ftCreationTimeLo, ftCreationTimeHi;
    DWORD ftLastAccessTimeLo, ftLastAccessTimeHi;
    DWORD ftLastWriteTimeLo, ftLastWriteTimeHi;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION;
_Static_assert(sizeof(BY_HANDLE_FILE_INFORMATION) == 52,
               "BY_HANDLE_FILE_INFORMATION must match the Win32 ABI");

static BOOL WINAPI GetFileInformationByHandle_stub(HANDLE h, BY_HANDLE_FILE_INFORMATION *info)
{
    if (!info) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    IO_STATUS_BLOCK iosb;
    FILE_BASIC_INFORMATION basic;
    FILE_STANDARD_INFORMATION standard;
    NTSTATUS status = NtQueryInformationFile(h, &iosb, &basic,
                                              sizeof(basic),
                                              FileBasicInformation);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    status = NtQueryInformationFile(h, &iosb, &standard, sizeof(standard),
                                    FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }

    memset(info, 0, sizeof(*info));
    info->dwFileAttributes = basic.FileAttributes;
    info->ftCreationTimeLo = basic.CreationTime.LowPart;
    info->ftCreationTimeHi = basic.CreationTime.HighPart;
    info->ftLastAccessTimeLo = basic.LastAccessTime.LowPart;
    info->ftLastAccessTimeHi = basic.LastAccessTime.HighPart;
    info->ftLastWriteTimeLo = basic.LastWriteTime.LowPart;
    info->ftLastWriteTimeHi = basic.LastWriteTime.HighPart;
    info->nFileSizeHigh = standard.EndOfFile.HighPart;
    info->nFileSizeLow = standard.EndOfFile.LowPart;
    info->nNumberOfLinks = standard.NumberOfLinks;
    return TRUE;
}

typedef struct {
    DWORD FileNameLength;
    WCHAR FileName[1];
} K32_FILE_NAME_INFO;

typedef struct {
    DWORD FileAttributes;
    DWORD ReparseTag;
} K32_FILE_ATTRIBUTE_TAG_INFO;

typedef struct {
    ULONGLONG VolumeSerialNumber;
    BYTE FileId[16];
} K32_FILE_ID_INFO;

static BOOL WINAPI GetFileInformationByHandleEx_k32(HANDLE file_handle,
                                                      DWORD info_class,
                                                      PVOID info,
                                                      DWORD info_size)
{
    if (!info) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    IO_STATUS_BLOCK iosb;
    NTSTATUS status;
    if (info_class == 0) { /* FileBasicInfo */
        if (info_size < sizeof(FILE_BASIC_INFORMATION)) {
            SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
            return FALSE;
        }
        status = NtQueryInformationFile(file_handle, &iosb, info, info_size,
                                        FileBasicInformation);
    } else if (info_class == 1) { /* FileStandardInfo */
        if (info_size < sizeof(FILE_STANDARD_INFORMATION)) {
            SetLastError(122);
            return FALSE;
        }
        status = NtQueryInformationFile(file_handle, &iosb, info, info_size,
                                        FileStandardInformation);
    } else {
        PVOID object = NULL;
        status = handle_lookup(&g_handle_table, file_handle, OBJ_TYPE_FILE,
                               &object);
        if (!NT_SUCCESS(status)) {
            set_last_error_from_status(status);
            return FALSE;
        }
        FILE_OBJECT *file = (FILE_OBJECT *)object;
        DWORD attributes = (file->flags & FILE_OBJ_DIRECTORY)
            ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

        if (info_class == 2) { /* FileNameInfo */
            DWORD name_chars = 0;
            while (file->name[name_chars]) name_chars++;
            DWORD name_bytes = name_chars * sizeof(WCHAR);
            if (info_size < sizeof(DWORD) + name_bytes) {
                SetLastError(122);
                return FALSE;
            }
            K32_FILE_NAME_INFO *name = (K32_FILE_NAME_INFO *)info;
            name->FileNameLength = name_bytes;
            memcpy(name->FileName, file->name, name_bytes);
        } else if (info_class == 9) { /* FileAttributeTagInfo */
            if (info_size < sizeof(K32_FILE_ATTRIBUTE_TAG_INFO)) {
                SetLastError(122);
                return FALSE;
            }
            K32_FILE_ATTRIBUTE_TAG_INFO *tag =
                (K32_FILE_ATTRIBUTE_TAG_INFO *)info;
            tag->FileAttributes = attributes;
            tag->ReparseTag = 0;
        } else if (info_class == 17) { /* FileAlignmentInfo */
            if (info_size < sizeof(DWORD)) {
                SetLastError(122);
                return FALSE;
            }
            *(DWORD *)info = 0; /* byte-aligned */
        } else if (info_class == 18) { /* FileIdInfo */
            if (info_size < sizeof(K32_FILE_ID_INFO)) {
                SetLastError(122);
                return FALSE;
            }
            K32_FILE_ID_INFO *id = (K32_FILE_ID_INFO *)info;
            memset(id, 0, sizeof(*id));
            id->VolumeSerialNumber = 0x4F534954ULL; /* OSIT */
            uint64_t hash = 1469598103934665603ULL;
            for (DWORD i = 0; file->name[i]; i++) {
                hash ^= (BYTE)k32_path_fold((char)file->name[i]);
                hash *= 1099511628211ULL;
            }
            memcpy(id->FileId, &hash, sizeof(hash));
        } else {
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            return FALSE;
        }
        SetLastError(0);
        return TRUE;
    }

    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL k32_file_object_path_ascii(const FILE_OBJECT *file,
                                       char path[260])
{
    if (!file) return FALSE;
    int i = 0;
    while (file->name[i] && i < 259) {
        path[i] = (char)(file->name[i] & 0xFF);
        i++;
    }
    if (file->name[i]) return FALSE;
    path[i] = 0;
    return i != 0;
}

static BOOL k32_make_absolute_path(const char *normalized, char path[264])
{
    size_t length = strlen(normalized);
    if (length + 4 > 264) return FALSE;
    path[0] = 'C';
    path[1] = ':';
    path[2] = '\\';
    memcpy(path + 3, normalized, length + 1);
    return TRUE;
}

static BOOL k32_path_is_absolute(const char *path)
{
    if (!path || !*path) return FALSE;
    if (path[0] == '\\' || path[0] == '/') return TRUE;
    return path[0] && path[1] == ':' &&
           (path[2] == '\\' || path[2] == '/');
}

static BOOL k32_file_info_path(HANDLE root_handle, const WCHAR *wide_name,
                               DWORD name_bytes, char path[260])
{
    if (!wide_name || (name_bytes & 1) || name_bytes / sizeof(WCHAR) >= 260) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD chars = name_bytes / sizeof(WCHAR);
    char raw[260];
    for (DWORD i = 0; i < chars; i++)
        raw[i] = (char)(wide_name[i] & 0xFF);
    raw[chars] = 0;

    if (root_handle && !k32_path_is_absolute(raw)) {
        FILE_OBJECT *root = NULL;
        NTSTATUS status = handle_lookup(&g_handle_table, root_handle,
                                        OBJ_TYPE_FILE, (PVOID *)&root);
        if (!NT_SUCCESS(status) || !root ||
            !(root->flags & FILE_OBJ_DIRECTORY)) {
            SetLastError(6); /* ERROR_INVALID_HANDLE */
            return FALSE;
        }

        char joined[524];
        int pos = 0;
        joined[pos++] = 'C';
        joined[pos++] = ':';
        joined[pos++] = '\\';
        int i = 0;
        while (root->name[i] && pos < (int)sizeof(joined) - 1)
            joined[pos++] = (char)(root->name[i++] & 0xFF);
        if (root->name[i]) {
            SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
            return FALSE;
        }
        if (pos > 3 && joined[pos - 1] != '\\' && joined[pos - 1] != '/')
            joined[pos++] = '\\';
        for (DWORD j = 0; j < chars; j++) {
            if (pos >= (int)sizeof(joined) - 1) {
                SetLastError(206);
                return FALSE;
            }
            joined[pos++] = raw[j];
        }
        joined[pos] = 0;
        if (!win32_normalize_path(joined, path)) {
            SetLastError(206);
            return FALSE;
        }
        return TRUE;
    }

    if (!win32_normalize_path(raw, path)) {
        SetLastError(206);
        return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI SetFileInformationByHandle_k32(HANDLE file_handle,
                                                    DWORD info_class,
                                                    PVOID info,
                                                    DWORD info_size)
{
    if (!info) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    FILE_OBJECT *file = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, file_handle,
                                    OBJ_TYPE_FILE, (PVOID *)&file);
    if (!NT_SUCCESS(status) || !file) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    static uint32_t trace_count;
    if (trace_count < 24) {
        serial_puts("[K32-FILEINFO] set class=");
        serial_putdec(info_class);
        serial_puts(" handle=0x");
        serial_puthex((uint64_t)(ULONG_PTR)file_handle, 8);
        serial_puts("\n");
    } else if (trace_count == 24) {
        serial_puts("[K32-FILEINFO] further set logs suppressed\n");
    }
    trace_count++;

    if (info_class == 0) { /* FileBasicInfo */
        if (info_size < sizeof(FILE_BASIC_INFORMATION)) {
            SetLastError(87);
            return FALSE;
        }
        SetLastError(0);
        return TRUE;
    }

    if (info_class == 3 || info_class == 22) { /* Rename / RenameEx */
        BYTE *bytes = (BYTE *)info;
        DWORD root_offset = g_compat32_mode ? 4 : 8;
        DWORD length_offset = g_compat32_mode ? 8 : 16;
        DWORD name_offset = g_compat32_mode ? 12 : 20;
        if (info_size < name_offset) {
            SetLastError(87);
            return FALSE;
        }

        HANDLE root = NULL;
        if (g_compat32_mode) {
            DWORD root32;
            memcpy(&root32, bytes + root_offset, sizeof(root32));
            root = (HANDLE)(ULONG_PTR)root32;
        } else {
            memcpy(&root, bytes + root_offset, sizeof(root));
        }
        DWORD name_bytes;
        memcpy(&name_bytes, bytes + length_offset, sizeof(name_bytes));
        if ((name_bytes & 1) || name_bytes > info_size - name_offset) {
            SetLastError(87);
            return FALSE;
        }

        BOOL replace;
        if (info_class == 22) {
            DWORD flags;
            memcpy(&flags, bytes, sizeof(flags));
            replace = (flags & 1) != 0; /* FILE_RENAME_REPLACE_IF_EXISTS */
        } else {
            replace = bytes[0] != 0;
        }

        char source[260], destination[260];
        char source_absolute[264], destination_absolute[264];
        if (!k32_file_object_path_ascii(file, source) ||
            !k32_file_info_path(root, (const WCHAR *)(bytes + name_offset),
                                name_bytes, destination) ||
            !k32_make_absolute_path(source, source_absolute) ||
            !k32_make_absolute_path(destination, destination_absolute)) {
            if (!GetLastError()) SetLastError(206);
            return FALSE;
        }
        if (!move_osfs_file(source_absolute, destination_absolute, replace))
            return FALSE;

        int i = 0;
        while (destination[i] && i < 259) {
            file->name[i] = (WCHAR)(unsigned char)destination[i];
            i++;
        }
        file->name[i] = 0;
        if (file->flags & FILE_OBJ_DISK_FILE)
            file->osfs_file = k32_find_file_exact_ci(destination);
        SetLastError(0);
        return TRUE;
    }

    if (info_class == 4 || info_class == 21) { /* Disposition / Ex */
        if (info_size < sizeof(DWORD)) {
            SetLastError(87);
            return FALSE;
        }
        DWORD flags;
        memcpy(&flags, info, sizeof(flags));
        if (!(flags & 1)) {
            SetLastError(0);
            return TRUE;
        }

        char source[260], source_absolute[264];
        if (!k32_file_object_path_ascii(file, source) ||
            !k32_make_absolute_path(source, source_absolute)) {
            SetLastError(206);
            return FALSE;
        }
        BOOL removed = (file->flags & FILE_OBJ_DIRECTORY)
            ? RemoveDirectoryA(source_absolute)
            : DeleteFileA(source_absolute);
        if (removed && (file->flags & FILE_OBJ_DISK_FILE)) {
            file->osfs_file = NULL;
            file->size = 0;
            file->position = 0;
        }
        return removed;
    }

    if (info_class == 5) { /* FileAllocationInfo */
        if (info_size < sizeof(LARGE_INTEGER) ||
            ((PLARGE_INTEGER)info)->QuadPart < 0) {
            SetLastError(87);
            return FALSE;
        }
        SetLastError(0);
        return TRUE;
    }

    if (info_class == 6) { /* FileEndOfFileInfo */
        if (info_size < sizeof(LARGE_INTEGER)) {
            SetLastError(87);
            return FALSE;
        }
        IO_STATUS_BLOCK iosb;
        status = NtSetInformationFile(file_handle, &iosb, info,
                                      sizeof(LARGE_INTEGER),
                                      FileEndOfFileInformation);
        if (!NT_SUCCESS(status)) {
            set_last_error_from_status(status);
            return FALSE;
        }
        SetLastError(0);
        return TRUE;
    }

    SetLastError(87); /* ERROR_INVALID_PARAMETER */
    return FALSE;
}

static DWORD WINAPI GetFinalPathNameByHandleW_k32(HANDLE file_handle,
                                                   PWSTR path,
                                                   DWORD path_chars,
                                                   DWORD flags)
{
    DWORD volume_mode = flags & 0x7;
    if ((flags & ~0xF) || (volume_mode != 0 && volume_mode != 4)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    PVOID object = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, file_handle,
                                    OBJ_TYPE_FILE, &object);
    FILE_OBJECT *file = (FILE_OBJECT *)object;
    if (!NT_SUCCESS(status) || !file) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }

    const char *disk_name = NULL;
    const WCHAR *object_name = NULL;
    if ((file->flags & FILE_OBJ_DISK_FILE) && file->osfs_file)
        disk_name = osfs2_file_name(file->osfs_file);
    else if (file->flags & FILE_OBJ_DIRECTORY)
        object_name = file->name;
    else {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }

    if ((!disk_name || !disk_name[0]) &&
        (!object_name || !object_name[0])) {
        SetLastError(2); /* ERROR_FILE_NOT_FOUND */
        return 0;
    }

    static const WCHAR dos_prefix[] = { '\\', '\\', '?', '\\',
                                         'C', ':', '\\', 0 };
    DWORD prefix_chars = volume_mode == 4 ? 0 : 7;
    DWORD name_chars = 0;
    if (object_name) {
        while (object_name[name_chars]) name_chars++;
    } else {
        while (disk_name[name_chars]) name_chars++;
    }
    DWORD result_chars = prefix_chars + name_chars;

    if (!path || path_chars <= result_chars) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return result_chars + 1;
    }

    for (DWORD i = 0; i < prefix_chars; i++) path[i] = dos_prefix[i];
    for (DWORD i = 0; i < name_chars; i++) {
        WCHAR c = object_name ? object_name[i] : (WCHAR)disk_name[i];
        path[prefix_chars + i] = c == '/' ? '\\' : c;
    }
    path[result_chars] = 0;
    return result_chars;
}

typedef struct {
    ULONG Characteristics;
    ULONG TimeDateStamp;
    USHORT MajorVersion;
    USHORT MinorVersion;
    USHORT NumberOfNamedEntries;
    USHORT NumberOfIdEntries;
} K32_RESOURCE_DIRECTORY;

typedef struct {
    ULONG Name;
    ULONG OffsetToData;
} K32_RESOURCE_ENTRY;

typedef struct {
    ULONG OffsetToData;
    ULONG Size;
    ULONG CodePage;
    ULONG Reserved;
} K32_RESOURCE_DATA_ENTRY;

typedef struct {
    BYTE *image;
    ULONG image_size;
    BYTE *resource;
    ULONG resource_size;
} K32_RESOURCE_VIEW;

#define K32_RESOURCE_NAME_IS_STRING 0x80000000U
#define K32_RESOURCE_DATA_IS_DIR    0x80000000U

static BOOL k32_resource_range(const K32_RESOURCE_VIEW *view, ULONG offset,
                               ULONG size)
{
    return offset <= view->resource_size &&
           size <= view->resource_size - offset;
}

static BOOL k32_resource_view(HANDLE module, K32_RESOURCE_VIEW *view)
{
    BYTE *image = module ? (BYTE *)module
                         : (BYTE *)(ULONG_PTR)win32_current_image_base();
    if (!image || !view) return FALSE;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)image;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 0x100000)
        return FALSE;

    BYTE *nt_base = image + dos->e_lfanew;
    if (*(ULONG *)nt_base != IMAGE_NT_SIGNATURE) return FALSE;
    USHORT magic = *(USHORT *)(nt_base + sizeof(ULONG) +
                               sizeof(IMAGE_FILE_HEADER));
    IMAGE_DATA_DIRECTORY *directory = NULL;
    ULONG image_size = 0;
    ULONG directory_count = 0;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)nt_base;
        image_size = nt->OptionalHeader.SizeOfImage;
        directory_count = nt->OptionalHeader.NumberOfRvaAndSizes;
        directory = nt->OptionalHeader.DataDirectory;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        PIMAGE_NT_HEADERS32 nt = (PIMAGE_NT_HEADERS32)nt_base;
        image_size = nt->OptionalHeader.SizeOfImage;
        directory_count = nt->OptionalHeader.NumberOfRvaAndSizes;
        directory = nt->OptionalHeader.DataDirectory;
    } else {
        return FALSE;
    }

    if (directory_count <= IMAGE_DIRECTORY_ENTRY_RESOURCE) return FALSE;
    IMAGE_DATA_DIRECTORY *resources =
        &directory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    if (!resources->VirtualAddress ||
        resources->VirtualAddress > image_size ||
        resources->Size > image_size - resources->VirtualAddress)
        return FALSE;

    view->image = image;
    view->image_size = image_size;
    view->resource = image + resources->VirtualAddress;
    view->resource_size = resources->Size;
    return resources->Size >= sizeof(K32_RESOURCE_DIRECTORY);
}

static K32_RESOURCE_DIRECTORY *k32_resource_directory(
    const K32_RESOURCE_VIEW *view, ULONG offset)
{
    if (!k32_resource_range(view, offset, sizeof(K32_RESOURCE_DIRECTORY)))
        return NULL;
    K32_RESOURCE_DIRECTORY *directory =
        (K32_RESOURCE_DIRECTORY *)(view->resource + offset);
    ULONG count = (ULONG)directory->NumberOfNamedEntries +
                  directory->NumberOfIdEntries;
    ULONG bytes = sizeof(*directory) + count * sizeof(K32_RESOURCE_ENTRY);
    if (bytes < sizeof(*directory) || !k32_resource_range(view, offset, bytes))
        return NULL;
    return directory;
}

static BOOL k32_resource_name_matches(const K32_RESOURCE_VIEW *view,
                                      const K32_RESOURCE_ENTRY *entry,
                                      PCWSTR requested)
{
    ULONG_PTR value = (ULONG_PTR)requested;
    if ((value >> 16) == 0)
        return !(entry->Name & K32_RESOURCE_NAME_IS_STRING) &&
               (entry->Name & 0xFFFF) == (value & 0xFFFF);
    if (!(entry->Name & K32_RESOURCE_NAME_IS_STRING)) return FALSE;

    ULONG offset = entry->Name & ~K32_RESOURCE_NAME_IS_STRING;
    if (!k32_resource_range(view, offset, sizeof(USHORT))) return FALSE;
    USHORT length = *(USHORT *)(view->resource + offset);
    ULONG bytes = sizeof(USHORT) + (ULONG)length * sizeof(WCHAR);
    if (!k32_resource_range(view, offset, bytes)) return FALSE;
    PCWSTR stored = (PCWSTR)(view->resource + offset + sizeof(USHORT));
    for (USHORT i = 0; i < length; i++) {
        WCHAR a = stored[i];
        WCHAR b = requested[i];
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
        if (!b || a != b) return FALSE;
    }
    return requested[length] == 0;
}

static K32_RESOURCE_ENTRY *k32_resource_find(
    const K32_RESOURCE_VIEW *view, ULONG directory_offset, PCWSTR name)
{
    K32_RESOURCE_DIRECTORY *directory =
        k32_resource_directory(view, directory_offset);
    if (!directory) return NULL;
    ULONG count = (ULONG)directory->NumberOfNamedEntries +
                  directory->NumberOfIdEntries;
    K32_RESOURCE_ENTRY *entries = (K32_RESOURCE_ENTRY *)(directory + 1);
    for (ULONG i = 0; i < count; i++)
        if (k32_resource_name_matches(view, &entries[i], name))
            return &entries[i];
    return NULL;
}

typedef BOOL (WINAPI *K32_ENUM_RESOURCE_NAME_PROC)(
    HANDLE module, PCWSTR type, PCWSTR name, LONG_PTR context);

static BOOL WINAPI EnumResourceNamesW_k32(
    HANDLE module, PCWSTR type, K32_ENUM_RESOURCE_NAME_PROC callback,
    LONG_PTR context)
{
    K32_RESOURCE_VIEW view;
    if (!callback) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (!k32_resource_view(module, &view)) {
        SetLastError(1812); /* ERROR_RESOURCE_DATA_NOT_FOUND */
        return FALSE;
    }

    K32_RESOURCE_ENTRY *type_entry = k32_resource_find(&view, 0, type);
    if (!type_entry || !(type_entry->OffsetToData & K32_RESOURCE_DATA_IS_DIR)) {
        SetLastError(1813); /* ERROR_RESOURCE_TYPE_NOT_FOUND */
        return FALSE;
    }
    ULONG type_dir = type_entry->OffsetToData & ~K32_RESOURCE_DATA_IS_DIR;
    K32_RESOURCE_DIRECTORY *directory =
        k32_resource_directory(&view, type_dir);
    if (!directory) {
        SetLastError(1812);
        return FALSE;
    }

    ULONG count = (ULONG)directory->NumberOfNamedEntries +
                  directory->NumberOfIdEntries;
    K32_RESOURCE_ENTRY *entries = (K32_RESOURCE_ENTRY *)(directory + 1);
    for (ULONG i = 0; i < count; i++) {
        PCWSTR name;
        WCHAR name_buffer[256];
        if (entries[i].Name & K32_RESOURCE_NAME_IS_STRING) {
            ULONG offset = entries[i].Name & ~K32_RESOURCE_NAME_IS_STRING;
            if (!k32_resource_range(&view, offset, sizeof(USHORT)))
                continue;
            USHORT length = *(USHORT *)(view.resource + offset);
            ULONG bytes = sizeof(USHORT) + (ULONG)length * sizeof(WCHAR);
            if (length >= 256 || !k32_resource_range(&view, offset, bytes))
                continue;
            PCWSTR stored =
                (PCWSTR)(view.resource + offset + sizeof(USHORT));
            for (USHORT j = 0; j < length; j++)
                name_buffer[j] = stored[j];
            name_buffer[length] = 0;
            name = name_buffer;
        } else {
            name = (PCWSTR)(ULONG_PTR)(entries[i].Name & 0xFFFF);
        }

        BOOL keep_going;
        if (g_compat32_mode) {
            uint32_t args[4] = {
                (uint32_t)(ULONG_PTR)module,
                (uint32_t)(ULONG_PTR)type,
                (uint32_t)(ULONG_PTR)name,
                (uint32_t)context
            };
            keep_going = (BOOL)compat32_callback_args(
                (uint32_t)(ULONG_PTR)callback, 4, args);
        } else {
            keep_going = callback(module, type, name, context);
        }
        if (!keep_going)
            return FALSE;
    }
    return TRUE;
}

static BOOL k32_resource_ansi_identifier(PCSTR value, WCHAR *buffer,
                                         PCWSTR *wide_value, DWORD error)
{
    if ((ULONG_PTR)value <= 0xFFFF) {
        *wide_value = (PCWSTR)(ULONG_PTR)value;
        return TRUE;
    }

    SIZE_T length = 0;
    while (value[length] && length < 255) {
        buffer[length] = (WCHAR)(BYTE)value[length];
        length++;
    }
    if (value[length]) {
        SetLastError(error);
        return FALSE;
    }
    buffer[length] = 0;
    *wide_value = buffer;
    return TRUE;
}

static HANDLE WINAPI FindResourceW_k32(HANDLE module, PCWSTR name,
                                        PCWSTR type)
{
    K32_RESOURCE_VIEW view;
    if (!k32_resource_view(module, &view)) {
        SetLastError(1812); /* ERROR_RESOURCE_DATA_NOT_FOUND */
        return NULL;
    }

    K32_RESOURCE_ENTRY *type_entry = k32_resource_find(&view, 0, type);
    if (!type_entry || !(type_entry->OffsetToData & K32_RESOURCE_DATA_IS_DIR)) {
        SetLastError(1813); /* ERROR_RESOURCE_TYPE_NOT_FOUND */
        return NULL;
    }
    ULONG type_dir = type_entry->OffsetToData & ~K32_RESOURCE_DATA_IS_DIR;
    K32_RESOURCE_ENTRY *name_entry = k32_resource_find(&view, type_dir, name);
    if (!name_entry || !(name_entry->OffsetToData & K32_RESOURCE_DATA_IS_DIR)) {
        SetLastError(1814); /* ERROR_RESOURCE_NAME_NOT_FOUND */
        return NULL;
    }

    ULONG language_dir = name_entry->OffsetToData & ~K32_RESOURCE_DATA_IS_DIR;
    K32_RESOURCE_DIRECTORY *languages =
        k32_resource_directory(&view, language_dir);
    if (!languages ||
        !(languages->NumberOfNamedEntries + languages->NumberOfIdEntries)) {
        SetLastError(1815); /* ERROR_RESOURCE_LANG_NOT_FOUND */
        return NULL;
    }
    K32_RESOURCE_ENTRY *language = (K32_RESOURCE_ENTRY *)(languages + 1);
    if (language->OffsetToData & K32_RESOURCE_DATA_IS_DIR) {
        SetLastError(1812);
        return NULL;
    }
    ULONG data_offset = language->OffsetToData;
    if (!k32_resource_range(&view, data_offset,
                            sizeof(K32_RESOURCE_DATA_ENTRY))) {
        SetLastError(1812);
        return NULL;
    }
    return (HANDLE)(view.resource + data_offset);
}

static HANDLE WINAPI FindResourceA_k32(HANDLE module, PCSTR name, PCSTR type)
{
    WCHAR name_buffer[256];
    WCHAR type_buffer[256];
    PCWSTR wide_name;
    PCWSTR wide_type;
    if (!k32_resource_ansi_identifier(name, name_buffer, &wide_name,
                                      1814 /* ERROR_RESOURCE_NAME_NOT_FOUND */) ||
        !k32_resource_ansi_identifier(type, type_buffer, &wide_type,
                                      1813 /* ERROR_RESOURCE_TYPE_NOT_FOUND */))
        return NULL;
    return FindResourceW_k32(module, wide_name, wide_type);
}

static PVOID WINAPI LoadResource_k32(HANDLE module, HANDLE resource)
{
    K32_RESOURCE_VIEW view;
    if (!resource || !k32_resource_view(module, &view)) {
        SetLastError(1812);
        return NULL;
    }
    ULONG_PTR offset = (ULONG_PTR)resource - (ULONG_PTR)view.resource;
    if (offset > 0xFFFFFFFFU ||
        !k32_resource_range(&view, (ULONG)offset,
                            sizeof(K32_RESOURCE_DATA_ENTRY))) {
        SetLastError(1812);
        return NULL;
    }
    K32_RESOURCE_DATA_ENTRY *data = (K32_RESOURCE_DATA_ENTRY *)resource;
    if (data->OffsetToData > view.image_size ||
        data->Size > view.image_size - data->OffsetToData) {
        SetLastError(1812);
        return NULL;
    }
    return view.image + data->OffsetToData;
}

static PVOID WINAPI LockResource_k32(HANDLE resource_data)
{
    return (PVOID)resource_data;
}

static DWORD WINAPI SizeofResource_k32(HANDLE module, HANDLE resource)
{
    K32_RESOURCE_VIEW view;
    if (!resource || !k32_resource_view(module, &view)) {
        SetLastError(1812);
        return 0;
    }
    ULONG_PTR offset = (ULONG_PTR)resource - (ULONG_PTR)view.resource;
    if (offset > 0xFFFFFFFFU ||
        !k32_resource_range(&view, (ULONG)offset,
                            sizeof(K32_RESOURCE_DATA_ENTRY))) {
        SetLastError(1812);
        return 0;
    }
    return ((K32_RESOURCE_DATA_ENTRY *)resource)->Size;
}

static BOOL WINAPI PeekNamedPipe_k32(HANDLE h, void *buf, DWORD sz,
                                      DWORD *read, DWORD *avail, DWORD *left)
{
    extern NTSTATUS nt_pipe_peek(HANDLE handle, PVOID buffer, ULONG length,
                                 ULONG *bytes_read, ULONG *bytes_available,
                                 ULONG *bytes_left);
    NTSTATUS status = nt_pipe_peek(h, buf, sz, read, avail, left);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

typedef struct _INPUT_RECORD { WORD EventType; char pad[18]; } INPUT_RECORD;

static BOOL WINAPI ReadConsoleInputA_stub(HANDLE h, INPUT_RECORD *buf, DWORD len, DWORD *read)
{
    (void)h; (void)buf; (void)len;
    if (read) *read = 0;
    return FALSE;
}

static BOOL WINAPI PeekConsoleInputA_stub(HANDLE h, INPUT_RECORD *buf, DWORD len, DWORD *read)
{
    (void)buf; (void)len;
    if (!k32_is_console_input(h)) {
        if (read) *read = 0;
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (read) *read = 0;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI GetNumberOfConsoleInputEvents_stub(HANDLE h, DWORD *num)
{
    if (!num) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (!k32_is_console_input(h)) {
        *num = 0;
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    *num = 0;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI LockFile_stub(HANDLE h, DWORD lo, DWORD hi, DWORD nlo, DWORD nhi)
{
    (void)h; (void)lo; (void)hi; (void)nlo; (void)nhi;
    return TRUE;
}

static BOOL WINAPI UnlockFile_stub(HANDLE h, DWORD lo, DWORD hi, DWORD nlo, DWORD nhi)
{
    (void)h; (void)lo; (void)hi; (void)nlo; (void)nhi;
    return TRUE;
}

static BOOL WINAPI LockFileEx_stub(HANDLE h, DWORD flags, DWORD reserved,
                                   DWORD nlo, DWORD nhi, PVOID overlapped)
{
    (void)flags; (void)reserved; (void)overlapped;
    return LockFile_stub(h, 0, 0, nlo, nhi);
}

static BOOL WINAPI UnlockFileEx_stub(HANDLE h, DWORD reserved,
                                     DWORD nlo, DWORD nhi, PVOID overlapped)
{
    (void)reserved; (void)overlapped;
    return UnlockFile_stub(h, 0, 0, nlo, nhi);
}

static BOOL WINAPI CreatePipe_k32(HANDLE *hRead, HANDLE *hWrite,
                                   void *lpAttr, DWORD nSize)
{
    (void)lpAttr; (void)nSize;
    if (!hRead || !hWrite) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    HANDLE read_handle = NULL;
    HANDLE write_handle = NULL;
    extern NTSTATUS nt_pipe_create(PHANDLE read_handle, PHANDLE write_handle);
    NTSTATUS status = nt_pipe_create(&read_handle, &write_handle);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    if (g_compat32_mode) {
        *(DWORD *)(void *)hRead = (DWORD)(ULONG_PTR)read_handle;
        *(DWORD *)(void *)hWrite = (DWORD)(ULONG_PTR)write_handle;
    } else {
        *hRead = read_handle;
        *hWrite = write_handle;
    }
    return TRUE;
}

static HANDLE WINAPI CreateNamedPipeW_k32(const WCHAR *name, DWORD open_mode,
                                           DWORD pipe_mode, DWORD max_instances,
                                           DWORD out_size, DWORD in_size,
                                           DWORD timeout, void *security_attributes)
{
    (void)open_mode; (void)pipe_mode; (void)max_instances;
    (void)out_size; (void)in_size; (void)timeout; (void)security_attributes;
    if (!name) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return INVALID_HANDLE_VALUE;
    }

    char ascii_name[260];
    int name_len = 0;
    while (name_len < 259 && name[name_len]) {
        ascii_name[name_len] = (char)(name[name_len] & 0xFF);
        name_len++;
    }
    if (name[name_len]) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return INVALID_HANDLE_VALUE;
    }
    ascii_name[name_len] = 0;

    int free_slot = -1;
    for (int i = 0; i < K32_MAX_PENDING_NAMED_PIPES; i++) {
        K32_PENDING_NAMED_PIPE *pipe = &g_pending_named_pipes[i];
        if (!pipe->used) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (named_pipe_name_equal(pipe->name, ascii_name)) {
            SetLastError(231); /* ERROR_PIPE_BUSY */
            return INVALID_HANDLE_VALUE;
        }
    }
    if (free_slot < 0) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return INVALID_HANDLE_VALUE;
    }

    HANDLE server = NULL;
    HANDLE client = NULL;
    extern NTSTATUS nt_pipe_create_duplex(PHANDLE server_handle,
                                           PHANDLE client_handle);
    NTSTATUS status = nt_pipe_create_duplex(&server, &client);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return INVALID_HANDLE_VALUE;
    }

    K32_PENDING_NAMED_PIPE *pending = &g_pending_named_pipes[free_slot];
    pending->used = TRUE;
    pending->server = server;
    pending->client = client;
    memcpy(pending->name, ascii_name, (SIZE_T)name_len + 1);
    serial_puts("[K32-PIPE] created slot=");
    serial_putdec((uint64_t)free_slot);
    serial_puts(" server=0x");
    serial_puthex((uint64_t)(ULONG_PTR)server, 8);
    serial_puts(" client=0x");
    serial_puthex((uint64_t)(ULONG_PTR)client, 8);
    serial_puts("\n");
    SetLastError(0);
    return server;
}

static BOOL WINAPI ConnectNamedPipe_k32(HANDLE pipe, void *overlapped)
{
    (void)overlapped;
    if (!PeekNamedPipe_k32(pipe, NULL, 0, NULL, NULL, NULL))
        return FALSE;

    SetLastError(535); /* ERROR_PIPE_CONNECTED */
    return FALSE;
}

static BOOL WINAPI DisconnectNamedPipe_k32(HANDLE pipe)
{
    /* Named pipes currently share the anonymous-pipe transport.  There is no
     * persistent connection state to reset, but Windows still requires this
     * call to reject non-server handles. */
    if (!PeekNamedPipe_k32(pipe, NULL, 0, NULL, NULL, NULL))
        return FALSE;

    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetNamedPipeHandleState_stub(HANDLE pipe, DWORD *mode,
                                                 DWORD *max_collection,
                                                 DWORD *collect_timeout)
{
    (void)mode; (void)max_collection; (void)collect_timeout;
    if (!pipe || pipe == INVALID_HANDLE_VALUE) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI TransactNamedPipe_stub(HANDLE pipe, void *input,
                                           DWORD input_size, void *output,
                                           DWORD output_size, DWORD *bytes_read,
                                           void *overlapped)
{
    (void)input; (void)input_size; (void)overlapped;
    if (!pipe || pipe == INVALID_HANDLE_VALUE || !output || output_size != 12) {
        SetLastError(50); /* ERROR_NOT_SUPPORTED */
        return FALSE;
    }
    /* ponytail: Crashpad's startup ping only checks for its 12-byte reply. */
    memset(output, 0, output_size);
    if (bytes_read) *bytes_read = output_size;
    return TRUE;
}

static BOOL WINAPI WaitNamedPipeW_stub(const WCHAR *name, DWORD timeout)
{
    (void)timeout;
    if (!name) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    char ascii_name[260];
    int i = 0;
    while (i < 259 && name[i]) {
        ascii_name[i] = (char)(name[i] & 0xFF);
        i++;
    }
    ascii_name[i] = 0;
    BOOL available = named_pipe_has_pending(ascii_name);
    SetLastError(available ? 0 : 2); /* ERROR_FILE_NOT_FOUND */
    return available;
}

static BOOL WINAPI SetFileTime_stub(HANDLE h, const void *c, const void *a, const void *w)
{
    (void)h; (void)c; (void)a; (void)w;
    return TRUE;
}

static BOOL WINAPI LocalFileTimeToFileTime_stub(const void *local, void *utc)
{
    if (!local || !utc) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    memcpy(utc, local, sizeof(ULONGLONG));
    return TRUE;
}

static BOOL WINAPI SystemTimeToFileTime_stub(const void *st, void *ft)
{
    if (!st || !ft) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    WINTIME_CALENDAR calendar;
    ULONGLONG filetime;
    k32_systemtime_to_calendar((const SYSTEMTIME *)st, &calendar);
    if (wintime_calendar_to_filetime(&calendar, &filetime) < 0) {
        SetLastError(87);
        return FALSE;
    }
    memcpy(ft, &filetime, sizeof(filetime));
    return TRUE;
}

static void WINAPI GetSystemTime_stub(SYSTEMTIME *st)
{
    k32_get_system_time(st);
}

static BOOL WINAPI SetLocalTime_stub(const SYSTEMTIME *st)
{
    (void)st;
    return TRUE;
}

static BOOL WINAPI ReleaseMutex_stub(HANDLE h)
{
    ULONG_PTR args[2] = { (ULONG_PTR)h, 0 };
    NTSTATUS status = sys_NtReleaseMutant(args);
    steamipc_trace_handle(
        "ReleaseMutex", h, 0, (uint32_t)status,
        (uint64_t)(ULONG_PTR)__builtin_return_address(0));
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

static void WINAPI OutputDebugStringW_stub(const WCHAR *s)
{
    (void)s;
    /* Silent */
}

static DWORD WINAPI GlobalAddAtomW_stub(const WCHAR *s)
{
    (void)s;
    return 0xC000;  /* fake atom */
}

/* ── Path and file attribute APIs ────────────────────────────── */

static DWORD WINAPI GetTempPathA_k32(DWORD nBufferLength, PSTR lpBuffer)
{
    const char *tmp = "C:\\Temp\\";
    DWORD len = 8;
    if (lpBuffer && nBufferLength > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = tmp[i];
    }
    return len;
}

static DWORD WINAPI GetTempPathW_k32(DWORD nBufferLength, PWSTR lpBuffer)
{
    static const WCHAR tmp[] = {0x43, 0x3A, 0x5C, 0x54, 0x65, 0x6D, 0x70, 0x5C, 0};
    DWORD len = 8;
    if (lpBuffer && nBufferLength > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = tmp[i];
    }
    return len;
}

static UINT WINAPI GetTempFileNameW_k32(PCWSTR path, PCWSTR prefix,
                                            UINT unique, PWSTR output)
{
    if (!path || !output) return 0;

    DWORD path_len = lstrlenW(path);
    if (path_len > 247) return 0;

    static UINT sequence = 1;
    for (UINT attempt = 0; attempt < 256; attempt++) {
        UINT value = unique ? unique : ((GetTickCount() + sequence++) & 0xFFFF);
        if (!value) value = 1;

        DWORD pos = 0;
        while (path[pos]) { output[pos] = path[pos]; pos++; }
        if (pos && output[pos - 1] != 0x5C && output[pos - 1] != '/')
            output[pos++] = 0x5C;
        for (DWORD i = 0; prefix && i < 3 && prefix[i]; i++) output[pos++] = prefix[i];
        for (int shift = 12; shift >= 0; shift -= 4) {
            UINT digit = (value >> shift) & 0xF;
            output[pos++] = (WCHAR)(digit < 10 ? '0' + digit : 'A' + digit - 10);
        }
        output[pos++] = '.'; output[pos++] = 't'; output[pos++] = 'm'; output[pos++] = 'p';
        output[pos] = 0;

        if (unique) return value;
        HANDLE file = CreateFileW(output, GENERIC_WRITE, 0, NULL, 1,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            return value;
        }
    }
    return 0;
}

static DWORD WINAPI GetSystemDirectoryA_k32(PSTR lpBuffer, DWORD uSize)
{
    const char *dir = "System";
    DWORD len = 6;
    if (lpBuffer && uSize > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = dir[i];
    }
    return len;
}

static DWORD WINAPI GetWindowsDirectoryA_k32(PSTR lpBuffer, DWORD uSize)
{
    return GetSystemDirectoryA_k32(lpBuffer, uSize);
}

static const GUID k32_ports_class_guid = {
    0x4D36E978, 0xE325, 0x11CE,
    { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 }
};
static const GUID k32_modem_class_guid = {
    0x4D36E96D, 0xE325, 0x11CE,
    { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 }
};
static const GUID k32_hid_interface_guid = {
    0x4D1E55B2, 0xF16F, 0x11CF,
    { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 }
};

typedef struct {
    uint32_t magic;
} K32_DEVICE_INFO_SET;

#define K32_DEVICE_INFO_SET_MAGIC 0x49445644U
static K32_DEVICE_INFO_SET k32_empty_device_info_set = {
    K32_DEVICE_INFO_SET_MAGIC
};

static int setupapi_class_name_a(PCSTR name)
{
    if (!name) return 0;
    char folded[8];
    int i = 0;
    while (name[i] && i < 7) {
        char c = name[i];
        folded[i++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    folded[i] = 0;
    if (!strcmp(folded, "PORTS")) return 1;
    if (!strcmp(folded, "MODEM")) return 2;
    return 0;
}

static int setupapi_class_name_w(PCWSTR name)
{
    if (!name) return 0;
    char ascii[8];
    int i = 0;
    while (name[i] && i < 7) {
        WCHAR value = name[i];
        char c = value <= 0x7F ? (char)value : 0;
        ascii[i++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    ascii[i] = 0;
    return setupapi_class_name_a(ascii);
}

static BOOL setupapi_class_guids(int class_id, GUID *class_guids,
                                 DWORD class_guid_count,
                                 DWORD *required_count)
{
    if (!required_count) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (!class_id) {
        *required_count = 0;
        SetLastError(1411); /* ERROR_CLASS_DOES_NOT_EXIST */
        return FALSE;
    }

    *required_count = 1;
    if (!class_guids || class_guid_count < 1) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }
    class_guids[0] = class_id == 1 ? k32_ports_class_guid
                                   : k32_modem_class_guid;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetupDiClassGuidsFromNameA_k32(PCSTR class_name,
                                                   GUID *class_guids,
                                                   DWORD class_guid_count,
                                                   DWORD *required_count)
{
    return setupapi_class_guids(setupapi_class_name_a(class_name), class_guids,
                                class_guid_count, required_count);
}

static BOOL WINAPI SetupDiClassGuidsFromNameW_k32(PCWSTR class_name,
                                                   GUID *class_guids,
                                                   DWORD class_guid_count,
                                                   DWORD *required_count)
{
    return setupapi_class_guids(setupapi_class_name_w(class_name), class_guids,
                                class_guid_count, required_count);
}

static PVOID WINAPI SetupDiGetClassDevsA_k32(PCVOID class_guid,
                                              PCSTR enumerator, PVOID parent,
                                              DWORD flags)
{
    (void)class_guid; (void)enumerator; (void)parent; (void)flags;
    SetLastError(0);
    return &k32_empty_device_info_set;
}

static PVOID WINAPI SetupDiGetClassDevsW_k32(PCVOID class_guid,
                                              PCWSTR enumerator, PVOID parent,
                                              DWORD flags)
{
    (void)class_guid; (void)enumerator; (void)parent; (void)flags;
    SetLastError(0);
    return &k32_empty_device_info_set;
}

static PVOID WINAPI SetupDiCreateDeviceInfoList_k32(PCVOID class_guid,
                                                     PVOID parent)
{
    (void)class_guid;
    (void)parent;
    SetLastError(0);
    return &k32_empty_device_info_set;
}

static BOOL WINAPI SetupDiEnumDeviceInfo_k32(PVOID device_info_set,
                                              DWORD member_index,
                                              PVOID device_info_data)
{
    if (device_info_set != &k32_empty_device_info_set || !device_info_data) {
        SetLastError(87);
        return FALSE;
    }
    (void)member_index;
    SetLastError(259); /* ERROR_NO_MORE_ITEMS */
    return FALSE;
}

static BOOL WINAPI SetupDiEnumDeviceInterfaces_k32(
    PVOID device_info_set, PVOID device_info_data, PCVOID interface_class_guid,
    DWORD member_index, PVOID device_interface_data)
{
    (void)device_info_data;
    (void)interface_class_guid;
    (void)member_index;
    if (device_info_set != &k32_empty_device_info_set ||
        !device_interface_data) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    SetLastError(259); /* ERROR_NO_MORE_ITEMS */
    return FALSE;
}

static BOOL WINAPI SetupDiGetDeviceInterfaceDetailW_k32(
    PVOID device_info_set, PVOID device_interface_data,
    PVOID device_interface_detail_data, DWORD detail_data_size,
    DWORD *required_size, PVOID device_info_data)
{
    (void)device_interface_data;
    (void)device_interface_detail_data;
    (void)detail_data_size;
    (void)device_info_data;
    if (required_size) *required_size = 0;
    if (device_info_set != &k32_empty_device_info_set) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    SetLastError(259); /* ERROR_NO_MORE_ITEMS */
    return FALSE;
}

static BOOL WINAPI SetupDiGetDevicePropertyW_k32(
    PVOID device_info_set, PVOID device_info_data, PCVOID property_key,
    DWORD *property_type, BYTE *property_buffer, DWORD property_buffer_size,
    DWORD *required_size, DWORD flags)
{
    (void)device_info_data;
    (void)property_key;
    (void)property_type;
    (void)property_buffer;
    (void)property_buffer_size;
    (void)flags;
    if (required_size) *required_size = 0;
    if (device_info_set != &k32_empty_device_info_set) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    SetLastError(1168); /* ERROR_NOT_FOUND */
    return FALSE;
}

static BOOL WINAPI SetupDiOpenDeviceInfoW_k32(
    PVOID device_info_set, PCWSTR device_instance_id, PVOID parent,
    DWORD open_flags, PVOID device_info_data)
{
    (void)device_instance_id;
    (void)parent;
    (void)open_flags;
    (void)device_info_data;
    if (device_info_set != &k32_empty_device_info_set) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    SetLastError(433); /* ERROR_NO_SUCH_DEVINST */
    return FALSE;
}

static BOOL WINAPI SetupDiOpenDeviceInterfaceW_k32(
    PVOID device_info_set, PCWSTR device_path, DWORD open_flags,
    PVOID device_interface_data)
{
    (void)device_path;
    (void)open_flags;
    (void)device_interface_data;
    if (device_info_set != &k32_empty_device_info_set) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    SetLastError(1168); /* ERROR_NOT_FOUND */
    return FALSE;
}

static BOOL WINAPI SetupDiGetDeviceInstanceIdW_k32(
    PVOID device_info_set, PVOID device_info_data, PWSTR device_instance_id,
    DWORD device_instance_id_size, DWORD *required_size)
{
    (void)device_info_set;
    (void)device_info_data;
    (void)device_instance_id;
    (void)device_instance_id_size;
    if (required_size) *required_size = 0;
    SetLastError(433); /* ERROR_NO_SUCH_DEVINST */
    return FALSE;
}

static BOOL WINAPI SetupDiGetDeviceInstanceIdA_k32(
    PVOID device_info_set, PVOID device_info_data, PSTR device_instance_id,
    DWORD device_instance_id_size, DWORD *required_size)
{
    (void)device_info_set;
    (void)device_info_data;
    if (device_instance_id && device_instance_id_size)
        device_instance_id[0] = 0;
    if (required_size) *required_size = 0;
    SetLastError(433); /* ERROR_NO_SUCH_DEVINST */
    return FALSE;
}

static HANDLE WINAPI SetupDiOpenDevRegKey_k32(
    PVOID device_info_set, PVOID device_info_data, DWORD scope,
    DWORD hardware_profile, DWORD key_type, DWORD access)
{
    (void)device_info_set;
    (void)device_info_data;
    (void)scope;
    (void)hardware_profile;
    (void)key_type;
    (void)access;
    SetLastError(433);
    return INVALID_HANDLE_VALUE;
}

static BOOL WINAPI SetupDiGetDeviceRegistryPropertyW_k32(
    PVOID device_info_set, PVOID device_info_data, DWORD property,
    DWORD *property_reg_data_type, BYTE *property_buffer,
    DWORD property_buffer_size, DWORD *required_size)
{
    (void)device_info_set; (void)device_info_data; (void)property;
    (void)property_reg_data_type; (void)property_buffer;
    (void)property_buffer_size;
    if (required_size) *required_size = 0;
    SetLastError(13); /* ERROR_INVALID_DATA */
    return FALSE;
}

static BOOL WINAPI SetupDiDestroyDeviceInfoList_k32(PVOID device_info_set)
{
    if (device_info_set != &k32_empty_device_info_set) {
        SetLastError(87);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static DWORD WINAPI CM_Get_Parent_k32(DWORD *parent, DWORD device_instance,
                                       DWORD flags)
{
    (void)device_instance;
    (void)flags;
    if (parent) *parent = 0;
    return 0x0000000D; /* CR_NO_SUCH_DEVNODE */
}

static DWORD WINAPI CM_Get_Device_IDW_k32(DWORD device_instance,
                                           PWSTR buffer, DWORD buffer_length,
                                           DWORD flags)
{
    (void)device_instance;
    (void)flags;
    if (buffer && buffer_length) buffer[0] = 0;
    return 0x0000000D; /* CR_NO_SUCH_DEVNODE */
}

static DWORD WINAPI CM_Locate_DevNodeW_k32(DWORD *device_instance,
                                            PWSTR device_id, DWORD flags)
{
    (void)flags;
    if (!device_instance) return 0x00000003; /* CR_INVALID_POINTER */
    if (!device_id || !device_id[0]) {
        *device_instance = 1; /* root devnode */
        return 0; /* CR_SUCCESS */
    }
    *device_instance = 0;
    return 0x0000000D; /* CR_NO_SUCH_DEVNODE */
}

static DWORD WINAPI CM_Locate_DevNodeA_k32(DWORD *device_instance,
                                            PSTR device_id, DWORD flags)
{
    (void)flags;
    if (!device_instance) return 0x00000003; /* CR_INVALID_POINTER */
    if (!device_id || !device_id[0]) {
        *device_instance = 1;
        return 0; /* CR_SUCCESS */
    }
    *device_instance = 0;
    return 0x0000000D; /* CR_NO_SUCH_DEVNODE */
}

static BOOL k32_root_devnode_enabled = TRUE;

static DWORD WINAPI CM_Get_DevNode_Status_k32(DWORD *status, DWORD *problem,
                                               DWORD device_instance,
                                               DWORD flags)
{
    (void)flags;
    if (!status || !problem) return 0x00000003; /* CR_INVALID_POINTER */
    if (device_instance != 1) return 0x0000000D; /* CR_NO_SUCH_DEVNODE */
    *status = 0x00000001U; /* DN_ROOT_ENUMERATED */
    if (k32_root_devnode_enabled)
        *status |= 0x00000002U | 0x00000008U; /* driver loaded, started */
    *problem = 0;
    return 0; /* CR_SUCCESS */
}

static DWORD WINAPI CM_Enable_DevNode_k32(DWORD device_instance, DWORD flags)
{
    (void)flags;
    if (device_instance != 1) return 0x0000000D; /* CR_NO_SUCH_DEVNODE */
    k32_root_devnode_enabled = TRUE;
    return 0;
}

static DWORD WINAPI CM_Disable_DevNode_k32(DWORD device_instance, DWORD flags)
{
    (void)flags;
    if (device_instance != 1) return 0x0000000D; /* CR_NO_SUCH_DEVNODE */
    k32_root_devnode_enabled = FALSE;
    return 0;
}

static DWORD WINAPI CM_Get_DevNode_PropertyW_k32(
    DWORD device_instance, PCVOID property_key, DWORD *property_type,
    BYTE *property_buffer, DWORD *property_buffer_size, DWORD flags)
{
    (void)device_instance;
    (void)property_key;
    (void)property_type;
    (void)property_buffer;
    (void)flags;
    if (property_buffer_size) *property_buffer_size = 0;
    return 0x0000000D; /* CR_NO_SUCH_DEVNODE */
}

static DWORD WINAPI CM_Get_Device_Interface_PropertyW_k32(
    PCWSTR device_interface, PCVOID property_key, DWORD *property_type,
    BYTE *property_buffer, DWORD *property_buffer_size, DWORD flags)
{
    (void)device_interface;
    (void)property_key;
    (void)property_type;
    (void)property_buffer;
    (void)flags;
    if (property_buffer_size) *property_buffer_size = 0;
    return 0x00000025; /* CR_NO_SUCH_VALUE */
}

static DWORD WINAPI CM_Get_Device_Interface_List_SizeW_k32(
    DWORD *character_count, PCVOID interface_class_guid, PCWSTR device_id,
    DWORD flags)
{
    (void)interface_class_guid;
    (void)device_id;
    (void)flags;
    if (!character_count) return 0x00000003; /* CR_INVALID_POINTER */
    *character_count = 1; /* Empty MULTI_SZ. */
    return 0; /* CR_SUCCESS */
}

static DWORD WINAPI CM_Get_Device_Interface_ListW_k32(
    PCVOID interface_class_guid, PCWSTR device_id, PWSTR buffer,
    DWORD buffer_character_count, DWORD flags)
{
    (void)interface_class_guid;
    (void)device_id;
    (void)flags;
    if (!buffer) return 0x00000003; /* CR_INVALID_POINTER */
    if (buffer_character_count < 1) return 0x0000001A; /* CR_BUFFER_SMALL */
    buffer[0] = 0;
    return 0; /* CR_SUCCESS */
}

static void WINAPI HidD_GetHidGuid_k32(GUID *hid_guid)
{
    if (hid_guid) *hid_guid = k32_hid_interface_guid;
}

static BOOL hid_no_device_k32(void)
{
    SetLastError(6); /* ERROR_INVALID_HANDLE */
    return FALSE;
}

static BOOL WINAPI HidD_GetAttributes_k32(HANDLE device, PVOID attributes)
{
    (void)device; (void)attributes;
    return hid_no_device_k32();
}

static BOOL WINAPI HidD_GetString_k32(HANDLE device, PVOID buffer,
                                       DWORD buffer_size)
{
    (void)device; (void)buffer; (void)buffer_size;
    return hid_no_device_k32();
}

static BOOL WINAPI HidD_TransferReport_k32(HANDLE device, PVOID report,
                                            DWORD report_size)
{
    (void)device; (void)report; (void)report_size;
    return hid_no_device_k32();
}

static BOOL WINAPI HidD_GetIndexedString_k32(HANDLE device,
                                              DWORD string_index,
                                              PVOID buffer,
                                              DWORD buffer_size)
{
    (void)device; (void)string_index; (void)buffer; (void)buffer_size;
    return hid_no_device_k32();
}

static BOOL WINAPI HidD_GetPreparsedData_k32(HANDLE device,
                                              PVOID *preparsed_data)
{
    (void)device;
    if (preparsed_data) *preparsed_data = NULL;
    return hid_no_device_k32();
}

static BOOL WINAPI HidD_FreePreparsedData_k32(PVOID preparsed_data)
{
    (void)preparsed_data;
    SetLastError(87); /* ERROR_INVALID_PARAMETER */
    return FALSE;
}

static NTSTATUS WINAPI HidP_GetCaps_k32(PVOID preparsed_data, PVOID caps)
{
    (void)preparsed_data; (void)caps;
    return (NTSTATUS)0xC0110001; /* HIDP_STATUS_INVALID_PREPARSED_DATA */
}

static BOOL WINAPI HidD_SetNumInputBuffers_k32(HANDLE device,
                                                DWORD buffer_count)
{
    (void)device; (void)buffer_count;
    return hid_no_device_k32();
}

static DWORD WINAPI CM_MapCrToWin32Err_k32(DWORD config_ret,
                                            DWORD default_error)
{
    if (config_ret == 0) return 0;
    if (config_ret == 0x0000000D) return 1168; /* ERROR_NOT_FOUND */
    return default_error;
}

typedef struct {
    BYTE ac_line_status, battery_flag, battery_percent, system_status_flag;
    DWORD battery_life_time, battery_full_life_time;
} SYSTEM_POWER_STATUS_K32;

#define K32_POWER_REQUEST_MAGIC          0x5257504BU /* KPWR */
#define K32_POWER_REQUEST_TYPE_COUNT     4U
#define K32_POWER_REQUEST_CONTEXT_SIMPLE 0x00000001U
#define K32_POWER_REQUEST_CONTEXT_DETAIL 0x00000002U

typedef struct {
    ULONG version;
    DWORD flags;
} K32_POWER_REQUEST_CONTEXT;

typedef struct {
    ULONG magic;
    DWORD reason_flags;
    volatile ULONG active[K32_POWER_REQUEST_TYPE_COUNT];
} K32_POWER_REQUEST;

static volatile ULONG g_k32_power_request_totals[K32_POWER_REQUEST_TYPE_COUNT];

_Static_assert(sizeof(SYSTEM_POWER_STATUS_K32) == 12,
               "SYSTEM_POWER_STATUS layout");

static BOOL WINAPI GetSystemPowerStatus_k32(SYSTEM_POWER_STATUS_K32 *status)
{
    if (!status) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    status->ac_line_status = 1;
    status->battery_flag = 128; /* no system battery */
    status->battery_percent = 255;
    status->system_status_flag = 0;
    status->battery_life_time = 0xFFFFFFFFU;
    status->battery_full_life_time = 0xFFFFFFFFU;
    return TRUE;
}

static K32_POWER_REQUEST *power_request_lookup_k32(HANDLE handle)
{
    K32_POWER_REQUEST *request = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, handle,
                                    OBJ_TYPE_POWER_REQUEST,
                                    (PVOID *)&request);
    if (!NT_SUCCESS(status) || !request ||
        request->magic != K32_POWER_REQUEST_MAGIC) {
        set_last_error_from_status(NT_SUCCESS(status) ?
                                   STATUS_INVALID_HANDLE : status);
        return NULL;
    }
    return request;
}

static HANDLE WINAPI PowerCreateRequest_k32(
    const K32_POWER_REQUEST_CONTEXT *context)
{
    if (!context || context->version != 0 ||
        (context->flags != K32_POWER_REQUEST_CONTEXT_SIMPLE &&
         context->flags != K32_POWER_REQUEST_CONTEXT_DETAIL)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return INVALID_HANDLE_VALUE;
    }

    K32_POWER_REQUEST *request = kmalloc(sizeof(*request));
    if (!request) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return INVALID_HANDLE_VALUE;
    }
    memset(request, 0, sizeof(*request));
    request->magic = K32_POWER_REQUEST_MAGIC;
    request->reason_flags = context->flags;

    HANDLE handle = INVALID_HANDLE_VALUE;
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_POWER_REQUEST,
                                   GENERIC_ALL, request, &handle);
    if (!NT_SUCCESS(status)) {
        request->magic = 0;
        kfree(request);
        set_last_error_from_status(status);
        return INVALID_HANDLE_VALUE;
    }

    serial_puts("[K32-POWER] create handle=0x");
    serial_puthex((uint64_t)(ULONG_PTR)handle, 8);
    serial_puts(" flags=0x");
    serial_puthex(context->flags, 8);
    serial_puts("\n");
    SetLastError(0);
    return handle;
}

static BOOL WINAPI PowerSetRequest_k32(HANDLE handle, DWORD request_type)
{
    if (request_type >= K32_POWER_REQUEST_TYPE_COUNT) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    K32_POWER_REQUEST *request = power_request_lookup_k32(handle);
    if (!request)
        return FALSE;

    volatile ULONG *counter = &request->active[request_type];
    ULONG current = __atomic_load_n(counter, __ATOMIC_RELAXED);
    do {
        if (current == 0xFFFFFFFFU) {
            SetLastError(298); /* ERROR_TOO_MANY_POSTS */
            return FALSE;
        }
    } while (!__atomic_compare_exchange_n(counter, &current, current + 1,
                                           FALSE, __ATOMIC_ACQ_REL,
                                           __ATOMIC_RELAXED));
    __atomic_add_fetch(&g_k32_power_request_totals[request_type], 1,
                       __ATOMIC_RELAXED);
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI PowerClearRequest_k32(HANDLE handle, DWORD request_type)
{
    if (request_type >= K32_POWER_REQUEST_TYPE_COUNT) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    K32_POWER_REQUEST *request = power_request_lookup_k32(handle);
    if (!request)
        return FALSE;

    volatile ULONG *counter = &request->active[request_type];
    ULONG current = __atomic_load_n(counter, __ATOMIC_RELAXED);
    do {
        if (current == 0) {
            SetLastError(87); /* no matching PowerSetRequest */
            return FALSE;
        }
    } while (!__atomic_compare_exchange_n(counter, &current, current - 1,
                                           FALSE, __ATOMIC_ACQ_REL,
                                           __ATOMIC_RELAXED));
    __atomic_sub_fetch(&g_k32_power_request_totals[request_type], 1,
                       __ATOMIC_RELAXED);
    SetLastError(0);
    return TRUE;
}

void k32_power_request_release(PVOID object)
{
    K32_POWER_REQUEST *request = (K32_POWER_REQUEST *)object;
    if (!request || request->magic != K32_POWER_REQUEST_MAGIC)
        return;

    request->magic = 0;
    for (DWORD type = 0; type < K32_POWER_REQUEST_TYPE_COUNT; type++) {
        ULONG active = __atomic_exchange_n(&request->active[type], 0,
                                           __ATOMIC_ACQ_REL);
        if (active)
            __atomic_sub_fetch(&g_k32_power_request_totals[type], active,
                               __ATOMIC_RELAXED);
    }
    kfree(request);
}

static NTSTATUS WINAPI CallNtPowerInformation_k32(DWORD information_level,
                                                   PVOID input_buffer,
                                                   DWORD input_length,
                                                   PVOID output_buffer,
                                                   DWORD output_length)
{
    (void)information_level;
    (void)input_buffer;
    (void)input_length;
    (void)output_buffer;
    (void)output_length;
    return (NTSTATUS)0xC0000002; /* STATUS_NOT_IMPLEMENTED */
}

static DWORD WINAPI PowerDeterminePlatformRoleEx_k32(DWORD version)
{
    (void)version;
    return 1; /* PlatformRoleDesktop */
}

static DWORD WINAPI PowerGetActiveScheme_k32(HANDLE root_key, PVOID *scheme)
{
    (void)root_key;
    if (scheme) *scheme = NULL;
    return 50; /* ERROR_NOT_SUPPORTED */
}

static DWORD power_read_value_k32(HANDLE root_key, PCVOID scheme,
                                  PCVOID subgroup, PCVOID setting,
                                  DWORD *type, BYTE *buffer,
                                  DWORD *buffer_size)
{
    (void)root_key;
    (void)scheme;
    (void)subgroup;
    (void)setting;
    (void)buffer;
    if (type) *type = 0;
    if (buffer_size) *buffer_size = 0;
    return 50; /* ERROR_NOT_SUPPORTED */
}

static DWORD WINAPI PowerReadACValue_k32(HANDLE root_key, PCVOID scheme,
                                          PCVOID subgroup, PCVOID setting,
                                          DWORD *type, BYTE *buffer,
                                          DWORD *buffer_size)
{
    return power_read_value_k32(root_key, scheme, subgroup, setting,
                                type, buffer, buffer_size);
}

static DWORD WINAPI PowerReadDCValue_k32(HANDLE root_key, PCVOID scheme,
                                          PCVOID subgroup, PCVOID setting,
                                          DWORD *type, BYTE *buffer,
                                          DWORD *buffer_size)
{
    return power_read_value_k32(root_key, scheme, subgroup, setting,
                                type, buffer, buffer_size);
}

/* Media Foundation and D3D video are optional for CEF. Returning E_NOTIMPL
 * lets winh264 disable hardware decoding instead of calling an empty IAT. */
static LONG WINAPI MediaFoundationUnavailable_k32(void)
{
    return (LONG)0x80004001U; /* E_NOTIMPL */
}

/* ── Export resolution table ────────────────────────────────── */

typedef struct {
    const char *name;
    PVOID       func;
    uint8_t     argc;
    uint8_t     cc;
} K32_EXPORT;

static const K32_EXPORT k32_exports[] = {
    { "CreateFileA",             (PVOID)CreateFileA,             7, CC_STDCALL },
    { "CreateFileW",             (PVOID)CreateFileW,             7, CC_STDCALL },
    { "ReadFile",                (PVOID)ReadFile,                5, CC_STDCALL },
    { "ReadFileEx",              (PVOID)ReadFileEx_k32,          5, CC_STDCALL },
    { "WriteFile",               (PVOID)WriteFile,               5, CC_STDCALL },
    { "WriteFileEx",             (PVOID)WriteFileEx_k32,         5, CC_STDCALL },
    { "ClearCommBreak",          (PVOID)ClearCommBreak_k32,      1, CC_STDCALL },
    { "ClearCommError",          (PVOID)ClearCommError_k32,      3, CC_STDCALL },
    { "SetupComm",               (PVOID)SetupComm_k32,           3, CC_STDCALL },
    { "EscapeCommFunction",      (PVOID)EscapeCommFunction_k32,  2, CC_STDCALL },
    { "GetCommModemStatus",      (PVOID)GetCommModemStatus_k32,  2, CC_STDCALL },
    { "GetCommState",            (PVOID)GetCommState_k32,        2, CC_STDCALL },
    { "GetCommTimeouts",         (PVOID)GetCommTimeouts_k32,     2, CC_STDCALL },
    { "PurgeComm",               (PVOID)PurgeComm_k32,           2, CC_STDCALL },
    { "SetCommBreak",            (PVOID)SetCommBreak_k32,        1, CC_STDCALL },
    { "SetCommMask",             (PVOID)SetCommMask_k32,         2, CC_STDCALL },
    { "SetCommState",            (PVOID)SetCommState_k32,        2, CC_STDCALL },
    { "SetCommTimeouts",         (PVOID)SetCommTimeouts_k32,     2, CC_STDCALL },
    { "WaitCommEvent",           (PVOID)WaitCommEvent_k32,       3, CC_STDCALL },
    { "CloseHandle",             (PVOID)CloseHandle,             1, CC_STDCALL },
    { "GetStdHandle",            (PVOID)GetStdHandle,            1, CC_STDCALL },
    { "WriteConsoleA",           (PVOID)WriteConsoleA,           5, CC_STDCALL },
    { "ExitProcess",             (PVOID)ExitProcess,             1, CC_STDCALL },
    { "GetCurrentProcess",       (PVOID)GetCurrentProcess,       0, CC_STDCALL },
    { "OpenProcess",             (PVOID)OpenProcess,             3, CC_STDCALL },
    { "CreateJobObjectW",        (PVOID)CreateJobObjectW,        2, CC_STDCALL },
    { "SetInformationJobObject", (PVOID)SetInformationJobObject, 4, CC_STDCALL },
    { "AssignProcessToJobObject",(PVOID)AssignProcessToJobObject,2, CC_STDCALL },
    { "QueryInformationJobObject",(PVOID)QueryInformationJobObject,5, CC_STDCALL },
    { "TerminateJobObject",      (PVOID)TerminateJobObject,      2, CC_STDCALL },
    { "InitializeProcThreadAttributeList", (PVOID)InitializeProcThreadAttributeList, 4, CC_STDCALL },
    { "UpdateProcThreadAttribute", (PVOID)UpdateProcThreadAttribute, 7, CC_STDCALL },
    { "DeleteProcThreadAttributeList", (PVOID)DeleteProcThreadAttributeList, 1, CC_STDCALL },
    { "GetProcessMitigationPolicy", (PVOID)GetProcessMitigationPolicy_k32, 4, CC_STDCALL },
    { "SetProcessMitigationPolicy", (PVOID)SetProcessMitigationPolicy_k32, 3, CC_STDCALL },
    { "IsWow64Process",          (PVOID)IsWow64Process_k32,      2, CC_STDCALL },
    { "IsWow64Process2",         (PVOID)IsWow64Process2_k32,     3, CC_STDCALL },
    { "IsProcessorFeaturePresent", (PVOID)IsProcessorFeaturePresent_k32, 1, CC_STDCALL },
    { "GetCurrentProcessId",     (PVOID)GetCurrentProcessId,     0, CC_STDCALL },
    { "EnumProcessModules",      (PVOID)EnumProcessModules_psapi, 4, CC_STDCALL },
    { "GetModuleBaseNameA",      (PVOID)GetModuleBaseNameA_psapi, 4, CC_STDCALL },
    { "GetModuleBaseNameW",      (PVOID)GetModuleBaseNameW_psapi, 4, CC_STDCALL },
    { "GetModuleInformation",    (PVOID)GetModuleInformation_psapi, 4, CC_STDCALL },
    { "GetProcessMemoryInfo",    (PVOID)GetProcessMemoryInfo_psapi, 3, CC_STDCALL },
    { "GetPerformanceInfo",      (PVOID)GetPerformanceInfo_psapi, 2, CC_STDCALL },
    { "QueryWorkingSetEx",       (PVOID)QueryWorkingSetEx_psapi, 3, CC_STDCALL },
    { "K32EnumProcessModules",   (PVOID)EnumProcessModules_psapi, 4, CC_STDCALL },
    { "K32GetModuleBaseNameA",   (PVOID)GetModuleBaseNameA_psapi, 4, CC_STDCALL },
    { "K32GetModuleBaseNameW",   (PVOID)GetModuleBaseNameW_psapi, 4, CC_STDCALL },
    { "K32GetModuleInformation", (PVOID)GetModuleInformation_psapi, 4, CC_STDCALL },
    { "K32GetProcessMemoryInfo", (PVOID)GetProcessMemoryInfo_psapi, 3, CC_STDCALL },
    { "K32GetPerformanceInfo",   (PVOID)GetPerformanceInfo_psapi, 2, CC_STDCALL },
    { "K32QueryWorkingSetEx",    (PVOID)QueryWorkingSetEx_psapi, 3, CC_STDCALL },
    { "K32EnumProcesses",        (PVOID)K32EnumProcesses_k32,    3, CC_STDCALL },
    { "EnumProcesses",           (PVOID)K32EnumProcesses_k32,    3, CC_STDCALL },
    { "K32GetMappedFileNameW",   (PVOID)K32GetMappedFileNameW_k32, 4, CC_STDCALL },
    { "GetProcessId",            (PVOID)GetProcessId_k32,        1, CC_STDCALL },
    { "GetProcessHandleCount",   (PVOID)GetProcessHandleCount_k32, 2, CC_STDCALL },
    { "ProcessIdToSessionId",    (PVOID)ProcessIdToSessionId_k32,  2, CC_STDCALL },
    { "WTSGetActiveConsoleSessionId", (PVOID)WTSGetActiveConsoleSessionId_k32, 0, CC_STDCALL },
    { "CreateToolhelp32Snapshot",(PVOID)CreateToolhelp32Snapshot_k32, 2, CC_STDCALL },
    { "Process32First",          (PVOID)Process32First_k32,      2, CC_STDCALL },
    { "Process32Next",           (PVOID)Process32Next_k32,       2, CC_STDCALL },
    { "Process32FirstA",         (PVOID)Process32First_k32,      2, CC_STDCALL },
    { "Process32NextA",          (PVOID)Process32Next_k32,       2, CC_STDCALL },
    { "Process32FirstW",         (PVOID)Process32FirstW_k32,     2, CC_STDCALL },
    { "Process32NextW",          (PVOID)Process32NextW_k32,      2, CC_STDCALL },
    { "VirtualAlloc",            (PVOID)VirtualAlloc,            4, CC_STDCALL },
    { "VirtualAllocEx",          (PVOID)VirtualAllocEx_k32,      5, CC_STDCALL },
    { "VirtualFree",             (PVOID)VirtualFree,             3, CC_STDCALL },
    { "VirtualFreeEx",           (PVOID)VirtualFreeEx_k32,       4, CC_STDCALL },
    { "ReadProcessMemory",       (PVOID)ReadProcessMemory_k32,    5, CC_STDCALL },
    { "WriteProcessMemory",      (PVOID)WriteProcessMemory_k32,   5, CC_STDCALL },
    { "FlushInstructionCache",  (PVOID)FlushInstructionCache_k32, 3, CC_STDCALL },
    { "DiscardVirtualMemory",    (PVOID)DiscardVirtualMemory_k32, 2, CC_STDCALL },
    { "PrefetchVirtualMemory",   (PVOID)PrefetchVirtualMemory_k32, 4, CC_STDCALL },
    { "GetProcessHeap",          (PVOID)GetProcessHeap,          0, CC_STDCALL },
    { "HeapAlloc",               (PVOID)HeapAlloc,               3, CC_STDCALL },
    { "HeapFree",                (PVOID)HeapFree,                3, CC_STDCALL },
    { "GetLastError",            (PVOID)GetLastError,            0, CC_STDCALL },
    { "SetLastError",            (PVOID)SetLastError,            1, CC_STDCALL },
    { "GetSystemPowerStatus",    (PVOID)GetSystemPowerStatus_k32, 1, CC_STDCALL },
    { "CallNtPowerInformation",  (PVOID)CallNtPowerInformation_k32, 5, CC_STDCALL },
    { "PowerCreateRequest",      (PVOID)PowerCreateRequest_k32,   1, CC_STDCALL },
    { "PowerSetRequest",         (PVOID)PowerSetRequest_k32,      2, CC_STDCALL },
    { "PowerClearRequest",       (PVOID)PowerClearRequest_k32,    2, CC_STDCALL },
    { "PowerDeterminePlatformRoleEx", (PVOID)PowerDeterminePlatformRoleEx_k32, 1, CC_STDCALL },
    { "PowerGetActiveScheme",    (PVOID)PowerGetActiveScheme_k32, 2, CC_STDCALL },
    { "PowerReadACValue",        (PVOID)PowerReadACValue_k32,     7, CC_STDCALL },
    { "PowerReadDCValue",        (PVOID)PowerReadDCValue_k32,     7, CC_STDCALL },
    { "SetupDiClassGuidsFromNameA", (PVOID)SetupDiClassGuidsFromNameA_k32, 4, CC_STDCALL },
    { "SetupDiClassGuidsFromNameW", (PVOID)SetupDiClassGuidsFromNameW_k32, 4, CC_STDCALL },
    { "SetupDiGetClassDevsA",    (PVOID)SetupDiGetClassDevsA_k32, 4, CC_STDCALL },
    { "SetupDiGetClassDevsW",    (PVOID)SetupDiGetClassDevsW_k32, 4, CC_STDCALL },
    { "SetupDiCreateDeviceInfoList",
                                  (PVOID)SetupDiCreateDeviceInfoList_k32,
                                                                       2, CC_STDCALL },
    { "SetupDiEnumDeviceInfo",   (PVOID)SetupDiEnumDeviceInfo_k32, 3, CC_STDCALL },
    { "SetupDiEnumDeviceInterfaces",
                                  (PVOID)SetupDiEnumDeviceInterfaces_k32,
                                                                       5, CC_STDCALL },
    { "SetupDiGetDeviceInterfaceDetailW",
                                  (PVOID)SetupDiGetDeviceInterfaceDetailW_k32,
                                                                       6, CC_STDCALL },
    { "SetupDiGetDevicePropertyW",
                                  (PVOID)SetupDiGetDevicePropertyW_k32,
                                                                       8, CC_STDCALL },
    { "SetupDiOpenDeviceInfoW",  (PVOID)SetupDiOpenDeviceInfoW_k32,    5, CC_STDCALL },
    { "SetupDiOpenDeviceInterfaceW",
                                  (PVOID)SetupDiOpenDeviceInterfaceW_k32,
                                                                       4, CC_STDCALL },
    { "SetupDiGetDeviceRegistryPropertyW",
                                  (PVOID)SetupDiGetDeviceRegistryPropertyW_k32,
                                                                       7, CC_STDCALL },
    { "SetupDiGetDeviceInstanceIdW",
                                  (PVOID)SetupDiGetDeviceInstanceIdW_k32,
                                                                       5, CC_STDCALL },
    { "SetupDiGetDeviceInstanceIdA",
                                  (PVOID)SetupDiGetDeviceInstanceIdA_k32,
                                                                       5, CC_STDCALL },
    { "SetupDiOpenDevRegKey",    (PVOID)SetupDiOpenDevRegKey_k32,      6, CC_STDCALL },
    { "SetupDiDestroyDeviceInfoList",
                                  (PVOID)SetupDiDestroyDeviceInfoList_k32,
                                                                       1, CC_STDCALL },
    { "CM_Get_Parent",           (PVOID)CM_Get_Parent_k32,             3, CC_STDCALL },
    { "CM_Get_Device_IDW",       (PVOID)CM_Get_Device_IDW_k32,         4, CC_STDCALL },
    { "CM_Locate_DevNodeW",      (PVOID)CM_Locate_DevNodeW_k32,        3, CC_STDCALL },
    { "CM_Locate_DevNodeA",      (PVOID)CM_Locate_DevNodeA_k32,        3, CC_STDCALL },
    { "CM_Get_DevNode_Status",   (PVOID)CM_Get_DevNode_Status_k32,     4, CC_STDCALL },
    { "CM_Enable_DevNode",       (PVOID)CM_Enable_DevNode_k32,         2, CC_STDCALL },
    { "CM_Disable_DevNode",      (PVOID)CM_Disable_DevNode_k32,        2, CC_STDCALL },
    { "CM_Get_DevNode_PropertyW", (PVOID)CM_Get_DevNode_PropertyW_k32, 6, CC_STDCALL },
    { "CM_Get_Device_Interface_PropertyW",
                                  (PVOID)CM_Get_Device_Interface_PropertyW_k32,
                                                                       6, CC_STDCALL },
    { "CM_Get_Device_Interface_List_SizeW",
                                  (PVOID)CM_Get_Device_Interface_List_SizeW_k32,
                                                                       4, CC_STDCALL },
    { "CM_Get_Device_Interface_ListW",
                                  (PVOID)CM_Get_Device_Interface_ListW_k32,
                                                                       5, CC_STDCALL },
    { "CM_MapCrToWin32Err",      (PVOID)CM_MapCrToWin32Err_k32,        2, CC_STDCALL },
    { "HidD_GetHidGuid",         (PVOID)HidD_GetHidGuid_k32,           1, CC_STDCALL },
    { "HidD_GetAttributes",      (PVOID)HidD_GetAttributes_k32,        2, CC_STDCALL },
    { "HidD_GetSerialNumberString", (PVOID)HidD_GetString_k32,         3, CC_STDCALL },
    { "HidD_GetManufacturerString", (PVOID)HidD_GetString_k32,         3, CC_STDCALL },
    { "HidD_GetProductString",   (PVOID)HidD_GetString_k32,            3, CC_STDCALL },
    { "HidD_SetFeature",         (PVOID)HidD_TransferReport_k32,       3, CC_STDCALL },
    { "HidD_GetFeature",         (PVOID)HidD_TransferReport_k32,       3, CC_STDCALL },
    { "HidD_GetInputReport",     (PVOID)HidD_TransferReport_k32,       3, CC_STDCALL },
    { "HidD_GetIndexedString",   (PVOID)HidD_GetIndexedString_k32,     4, CC_STDCALL },
    { "HidD_GetPreparsedData",   (PVOID)HidD_GetPreparsedData_k32,     2, CC_STDCALL },
    { "HidD_FreePreparsedData",  (PVOID)HidD_FreePreparsedData_k32,    1, CC_STDCALL },
    { "HidP_GetCaps",            (PVOID)HidP_GetCaps_k32,              2, CC_STDCALL },
    { "HidD_SetNumInputBuffers", (PVOID)HidD_SetNumInputBuffers_k32,   2, CC_STDCALL },
    { "MFStartup",               (PVOID)MediaFoundationUnavailable_k32, 2, CC_STDCALL },
    { "MFShutdown",              (PVOID)MediaFoundationUnavailable_k32, 0, CC_STDCALL },
    { "MFCreateMediaType",       (PVOID)MediaFoundationUnavailable_k32, 1, CC_STDCALL },
    { "MFCreateStreamDescriptor",(PVOID)MediaFoundationUnavailable_k32, 4, CC_STDCALL },
    { "MFCreateSample",          (PVOID)MediaFoundationUnavailable_k32, 1, CC_STDCALL },
    { "MFCreatePresentationDescriptor", (PVOID)MediaFoundationUnavailable_k32, 3, CC_STDCALL },
    { "MFCreateAttributes",      (PVOID)MediaFoundationUnavailable_k32, 2, CC_STDCALL },
    { "MFCreateMemoryBuffer",    (PVOID)MediaFoundationUnavailable_k32, 2, CC_STDCALL },
    { "MFCreateEventQueue",      (PVOID)MediaFoundationUnavailable_k32, 1, CC_STDCALL },
    { "MFGetService",            (PVOID)MediaFoundationUnavailable_k32, 4, CC_STDCALL },
    { "MFCreateSourceReaderFromMediaSource", (PVOID)MediaFoundationUnavailable_k32, 3, CC_STDCALL },
    { "Direct3DCreate9Ex",       (PVOID)MediaFoundationUnavailable_k32, 2, CC_STDCALL },
    { "DXVA2CreateDirect3DDeviceManager9", (PVOID)MediaFoundationUnavailable_k32, 2, CC_STDCALL },
    { "MulDiv",                  (PVOID)MulDiv_k32,               3, CC_STDCALL },
    { "Sleep",                   (PVOID)Sleep,                   1, CC_STDCALL },
    { "QueryPerformanceCounter", (PVOID)QueryPerformanceCounter, 1, CC_STDCALL },
    { "QueryPerformanceFrequency",(PVOID)QueryPerformanceFrequency,1, CC_STDCALL },
    { "QueryUnbiasedInterruptTimePrecise",
      (PVOID)QueryUnbiasedInterruptTimePrecise_k32, 1, CC_STDCALL },
    { "GetProcAddress",          (PVOID)GetProcAddress,          2, CC_STDCALL },
    { "GetModuleHandleA",        (PVOID)GetModuleHandleA,        1, CC_STDCALL },
    { "GetModuleHandleW",        (PVOID)GetModuleHandleW,        1, CC_STDCALL },
    { "GetModuleHandleExA",      (PVOID)GetModuleHandleExA,      3, CC_STDCALL },
    { "GetModuleHandleExW",      (PVOID)GetModuleHandleExW,      3, CC_STDCALL },
    { "RtlPcToFileHeader",       (PVOID)RtlPcToFileHeader_k32,   2, CC_STDCALL },
    { "RtlAddFunctionTable",     (PVOID)RtlAddFunctionTable_k32, 3, CC_STDCALL },
    { "RtlDeleteFunctionTable",  (PVOID)RtlDeleteFunctionTable_k32, 1, CC_STDCALL },
    { "RtlLookupFunctionEntry",  (PVOID)RtlLookupFunctionEntry_k32, 3, CC_STDCALL },
    { "GetFileSize",             (PVOID)GetFileSize,             2, CC_STDCALL },
    { "GetFileSizeEx",           (PVOID)GetFileSizeEx_k32,       2, CC_STDCALL },
    { "SetFilePointer",          (PVOID)SetFilePointer,          4, CC_STDCALL },
    { "SetFilePointerEx",        (PVOID)SetFilePointerEx_k32,     5, CC_STDCALL },
    { "DuplicateHandle",         (PVOID)DuplicateHandle,         7, CC_STDCALL },
    { "SetHandleInformation",    (PVOID)SetHandleInformation,    3, CC_STDCALL },
    { "VirtualProtect",          (PVOID)VirtualProtect,          4, CC_STDCALL },
    { "VirtualProtectEx",        (PVOID)VirtualProtectEx_k32,    5, CC_STDCALL },
    { "VirtualQuery",            (PVOID)VirtualQuery,            3, CC_STDCALL },
    { "VirtualQueryEx",          (PVOID)VirtualQueryEx_k32,      4, CC_STDCALL },
    { "CreateFileMappingA",      (PVOID)CreateFileMappingA,      6, CC_STDCALL },
    { "CreateFileMappingW",      (PVOID)CreateFileMappingW,      6, CC_STDCALL },
    { "OpenFileMappingA",        (PVOID)OpenFileMappingA,        3, CC_STDCALL },
    { "OpenFileMappingW",        (PVOID)OpenFileMappingW,        3, CC_STDCALL },
    { "MapViewOfFile",           (PVOID)MapViewOfFile,           5, CC_STDCALL },
    { "UnmapViewOfFile",         (PVOID)UnmapViewOfFile,         1, CC_STDCALL },
    { "FlushViewOfFile",         (PVOID)FlushViewOfFile_k32,     2, CC_STDCALL },
    { "lstrlenA",                (PVOID)lstrlenA,                1, CC_STDCALL },
    { "lstrlenW",                (PVOID)lstrlenW,                1, CC_STDCALL },
    { "lstrcpynW",               (PVOID)lstrcpynW_k32,           3, CC_STDCALL },
    { "lstrcpyA",                (PVOID)lstrcpyA_k32,            2, CC_STDCALL },
    { "lstrcpyW",                (PVOID)lstrcpyW_k32,            2, CC_STDCALL },
    { "lstrcatW",                (PVOID)lstrcatW_k32,            2, CC_STDCALL },
    { "lstrcmpW",                (PVOID)lstrcmpW_k32,            2, CC_STDCALL },
    { "lstrcmpiA",               (PVOID)lstrcmpiA_k32,           2, CC_STDCALL },
    { "lstrcmpiW",               (PVOID)lstrcmpiW_k32,           2, CC_STDCALL },
    { "GetCommandLineA",         (PVOID)GetCommandLineA,         0, CC_STDCALL },
    { "GetCommandLineW",         (PVOID)GetCommandLineW,         0, CC_STDCALL },
    { "GetEnvironmentStringsA",  (PVOID)GetEnvironmentStringsA,  0, CC_STDCALL },
    { "FreeEnvironmentStringsA", (PVOID)FreeEnvironmentStringsA, 1, CC_STDCALL },
    /* Phase 11: Critical Section */
    { "InitializeCriticalSection",          (PVOID)InitializeCriticalSection,          1, CC_STDCALL },
    { "InitializeCriticalSectionAndSpinCount",(PVOID)InitializeCriticalSectionAndSpinCount,2, CC_STDCALL },
    { "SetCriticalSectionSpinCount",          (PVOID)SetCriticalSectionSpinCount,       2, CC_STDCALL },
    { "InitializeCriticalSectionEx",          (PVOID)InitializeCriticalSectionEx_k32,   3, CC_STDCALL },
    { "EnterCriticalSection",               (PVOID)EnterCriticalSection,               1, CC_STDCALL },
    { "TryEnterCriticalSection",            (PVOID)TryEnterCriticalSection,            1, CC_STDCALL },
    { "LeaveCriticalSection",               (PVOID)LeaveCriticalSection,               1, CC_STDCALL },
    { "DeleteCriticalSection",              (PVOID)DeleteCriticalSection,              1, CC_STDCALL },
    { "InitializeSRWLock",                  (PVOID)InitializeSRWLock_k32,               1, CC_STDCALL },
    { "AcquireSRWLockExclusive",            (PVOID)AcquireSRWLockExclusive_k32,         1, CC_STDCALL },
    { "TryAcquireSRWLockExclusive",         (PVOID)TryAcquireSRWLockExclusive_k32,      1, CC_STDCALL },
    { "ReleaseSRWLockExclusive",            (PVOID)ReleaseSRWLockExclusive_k32,         1, CC_STDCALL },
    { "AcquireSRWLockShared",               (PVOID)AcquireSRWLockShared_k32,            1, CC_STDCALL },
    { "TryAcquireSRWLockShared",            (PVOID)TryAcquireSRWLockShared_k32,         1, CC_STDCALL },
    { "ReleaseSRWLockShared",               (PVOID)ReleaseSRWLockShared_k32,            1, CC_STDCALL },
    { "SleepConditionVariableSRW",          (PVOID)SleepConditionVariableSRW_k32,       4, CC_STDCALL },
    { "InitializeConditionVariable",       (PVOID)InitializeConditionVariable_k32,     1, CC_STDCALL },
    { "SleepConditionVariableCS",          (PVOID)SleepConditionVariableCS_k32,        3, CC_STDCALL },
    { "WakeConditionVariable",              (PVOID)WakeAllConditionVariable_k32,        1, CC_STDCALL },
    { "WakeAllConditionVariable",           (PVOID)WakeAllConditionVariable_k32,        1, CC_STDCALL },
    { "InitOnceBeginInitialize",           (PVOID)InitOnceBeginInitialize_k32,       4, CC_STDCALL },
    { "InitOnceComplete",                  (PVOID)InitOnceComplete_k32,              3, CC_STDCALL },
    { "InitOnceExecuteOnce",               (PVOID)InitOnceExecuteOnce_k32,           4, CC_STDCALL },
    /* TLS */
    { "TlsAlloc",               (PVOID)TlsAlloc,                0, CC_STDCALL },
    { "TlsFree",                (PVOID)TlsFree,                 1, CC_STDCALL },
    { "TlsGetValue",            (PVOID)TlsGetValue,             1, CC_STDCALL },
    { "TlsSetValue",            (PVOID)TlsSetValue,             2, CC_STDCALL },
    { "FlsAlloc",               (PVOID)FlsAlloc,                1, CC_STDCALL },
    { "FlsFree",                (PVOID)FlsFree,                 1, CC_STDCALL },
    { "FlsGetValue",            (PVOID)FlsGetValue,             1, CC_STDCALL },
    { "FlsGetValue2",           (PVOID)FlsGetValue,             1, CC_STDCALL },
    { "FlsSetValue",            (PVOID)FlsSetValue,             2, CC_STDCALL },
    /* Fibers */
    { "ConvertThreadToFiber",   (PVOID)ConvertThreadToFiber,    1, CC_STDCALL },
    { "ConvertThreadToFiberEx", (PVOID)ConvertThreadToFiberEx,  2, CC_STDCALL },
    { "ConvertFiberToThread",   (PVOID)ConvertFiberToThread,    0, CC_STDCALL },
    { "CreateFiber",            (PVOID)CreateFiber,             3, CC_STDCALL },
    { "CreateFiberEx",          (PVOID)CreateFiberEx,           5, CC_STDCALL },
    { "DeleteFiber",            (PVOID)DeleteFiber,             1, CC_STDCALL },
    { "SwitchToFiber",          (PVOID)SwitchToFiber,           1, CC_STDCALL },
    { "IsThreadAFiber",         (PVOID)IsThreadAFiber,          0, CC_STDCALL },
    /* Thread */
    { "CreateThread",            (PVOID)CreateThread,            6, CC_STDCALL },
    { "CreateRemoteThread",      (PVOID)CreateRemoteThread_k32,  7, CC_STDCALL },
    { "CreateRemoteThreadEx",    (PVOID)CreateRemoteThreadEx_k32, 8, CC_STDCALL },
    { "GetCurrentThreadId",      (PVOID)GetCurrentThreadId,      0, CC_STDCALL },
    { "GetCurrentThread",        (PVOID)GetCurrentThread,        0, CC_STDCALL },
    { "GetCurrentThreadStackLimits", (PVOID)GetCurrentThreadStackLimits_k32,
                                                               2, CC_STDCALL },
    { "GetThreadId",             (PVOID)GetThreadId_k32,         1, CC_STDCALL },
    { "SetThreadDescription",    (PVOID)SetThreadDescription_k32, 2, CC_STDCALL },
    { "GetThreadDescription",    (PVOID)GetThreadDescription_k32, 2, CC_STDCALL },
    { "OpenThread",              (PVOID)OpenThread_k32,          3, CC_STDCALL },
    { "GetExitCodeThread",       (PVOID)GetExitCodeThread_k32,   2, CC_STDCALL },
    { "GetThreadContext",        (PVOID)GetThreadContext_k32,    2, CC_STDCALL },
    { "GetThreadPriorityBoost",  (PVOID)GetThreadPriorityBoost_k32, 2, CC_STDCALL },
    { "SetThreadPriorityBoost",  (PVOID)SetThreadPriorityBoost_k32, 2, CC_STDCALL },
    { "SetThreadStackGuarantee", (PVOID)SetThreadStackGuarantee_k32,
                                                               1, CC_STDCALL },
    { "SetThreadAffinityMask",   (PVOID)SetThreadAffinityMask_k32, 2, CC_STDCALL },
    { "SwitchToThread",          (PVOID)SwitchToThread_k32,      0, CC_STDCALL },
    { "SuspendThread",           (PVOID)SuspendThread,           1, CC_STDCALL },
    { "ResumeThread",            (PVOID)ResumeThread,            1, CC_STDCALL },
    { "TerminateThread",         (PVOID)TerminateThread,         2, CC_STDCALL },
    { "QueryThreadCycleTime",    (PVOID)QueryThreadCycleTime_k32, 2, CC_STDCALL },
    { "SleepEx",                 (PVOID)SleepEx_k32,              2, CC_STDCALL },
    { "WaitForSingleObject",     (PVOID)WaitForSingleObject,     2, CC_STDCALL },
    { "WaitForSingleObjectEx",   (PVOID)WaitForSingleObjectEx_k32, 3, CC_STDCALL },
    { "WaitForMultipleObjects",  (PVOID)WaitForMultipleObjects,  4, CC_STDCALL },
    { "RegisterWaitForSingleObject", (PVOID)RegisterWaitForSingleObject_k32, 6, CC_STDCALL },
    { "UnregisterWait",          (PVOID)UnregisterWait_k32,      1, CC_STDCALL },
    { "UnregisterWaitEx",        (PVOID)UnregisterWaitEx_k32,    2, CC_STDCALL },
    /* DLL / Module */
    { "LoadLibraryA",            (PVOID)LoadLibraryA,            1, CC_STDCALL },
    { "LoadLibraryW",            (PVOID)LoadLibraryW,            1, CC_STDCALL },
    { "LoadLibraryExA",          (PVOID)LoadLibraryExA,          3, CC_STDCALL },
    { "LoadLibraryExW",          (PVOID)LoadLibraryExW,          3, CC_STDCALL },
    { "FreeLibrary",             (PVOID)FreeLibrary,             1, CC_STDCALL },
    { "FreeLibraryAndExitThread", (PVOID)FreeLibraryAndExitThread_k32, 2, CC_STDCALL },
    { "DisableThreadLibraryCalls", (PVOID)DisableThreadLibraryCalls, 1, CC_STDCALL },
    { "GetModuleFileNameA",      (PVOID)GetModuleFileNameA,      3, CC_STDCALL },
    { "GetModuleFileNameExA",    (PVOID)K32GetModuleFileNameExA_k32, 4, CC_STDCALL },
    { "GetModuleFileNameExW",    (PVOID)K32GetModuleFileNameExW_k32, 4, CC_STDCALL },
    { "K32GetModuleFileNameExA", (PVOID)K32GetModuleFileNameExA_k32, 4, CC_STDCALL },
    { "K32GetModuleFileNameExW", (PVOID)K32GetModuleFileNameExW_k32, 4, CC_STDCALL },
    { "SetDefaultDllDirectories", (PVOID)SetDefaultDllDirectories_k32, 1, CC_STDCALL },
    { "AddDllDirectory",         (PVOID)AddDllDirectory_k32,        1, CC_STDCALL },
    { "RemoveDllDirectory",      (PVOID)RemoveDllDirectory_k32,     1, CC_STDCALL },
    { "SetDllDirectoryA",        (PVOID)SetDllDirectoryA_k32,    1, CC_STDCALL },
    { "SetDllDirectoryW",        (PVOID)SetDllDirectoryW_k32,    1, CC_STDCALL },
    /* Timing */
    { "GetTickCount",            (PVOID)GetTickCount,            0, CC_STDCALL },
    { "GetTickCount64",          (PVOID)GetTickCount64,          0, CC_STDCALL },
    { "GetSystemTimeAsFileTime", (PVOID)GetSystemTimeAsFileTime, 1, CC_STDCALL },
    { "GetSystemTimePreciseAsFileTime", (PVOID)GetSystemTimePreciseAsFileTime_k32, 1, CC_STDCALL },
    { "GetFileTime",             (PVOID)GetFileTime_k32,         4, CC_STDCALL },
    { "GetThreadTimes",          (PVOID)GetThreadTimes,          5, CC_STDCALL },
    { "GetProcessTimes",         (PVOID)GetThreadTimes,          5, CC_STDCALL },
    { "RtlCaptureStackBackTrace",(PVOID)RtlCaptureStackBackTrace_stub, 4, CC_STDCALL },
    /* System */
    { "GetSystemInfo",           (PVOID)GetSystemInfo,           1, CC_STDCALL },
    { "GetNativeSystemInfo",     (PVOID)GetSystemInfo,           1, CC_STDCALL },
    { "GetProcessAffinityMask",  (PVOID)GetProcessAffinityMask_k32, 3, CC_STDCALL },
    { "SetProcessAffinityMask",  (PVOID)SetProcessAffinityMask_k32, 2, CC_STDCALL },
    { "GetCurrentProcessorNumber", (PVOID)GetCurrentProcessorNumber_k32, 0, CC_STDCALL },
    { "GetMaximumProcessorCount", (PVOID)GetMaximumProcessorCount_k32, 1, CC_STDCALL },
    { "GetMaximumProcessorGroupCount", (PVOID)GetMaximumProcessorGroupCount_k32, 0, CC_STDCALL },
    { "EncodePointer",           (PVOID)EncodePointer_k32,       1, CC_STDCALL },
    { "DecodePointer",           (PVOID)DecodePointer_k32,       1, CC_STDCALL },
    { "SetProcessShutdownParameters", (PVOID)SetProcessShutdownParameters_k32, 2, CC_STDCALL },
    { "GetLogicalProcessorInformation", (PVOID)GetLogicalProcessorInformation_k32, 2, CC_STDCALL },
    { "GetLogicalProcessorInformationEx", (PVOID)GetLogicalProcessorInformationEx_k32, 3, CC_STDCALL },
    { "WerRegisterRuntimeExceptionModule", (PVOID)WerRegisterRuntimeExceptionModule_k32, 2, CC_STDCALL },
    { "MiniDumpWriteDump",      (PVOID)MiniDumpWriteDump_k32,   7, CC_STDCALL },
    { "GetVersionExA",           (PVOID)GetVersionExA,           1, CC_STDCALL },
    { "GetProductInfo",          (PVOID)GetProductInfo,          5, CC_STDCALL },
    /* Path / Dir */
    { "GetFullPathNameA",        (PVOID)GetFullPathNameA,        4, CC_STDCALL },
    { "GetCurrentDirectoryA",    (PVOID)GetCurrentDirectoryA,    2, CC_STDCALL },
    { "GetFileAttributesA",      (PVOID)GetFileAttributesA,      1, CC_STDCALL },
    { "SetFileAttributesA",      (PVOID)SetFileAttributesA,      2, CC_STDCALL },
    { "CreateDirectoryA",        (PVOID)CreateDirectoryA,        2, CC_STDCALL },
    { "RemoveDirectoryA",        (PVOID)RemoveDirectoryA,        1, CC_STDCALL },
    /* Find File */
    { "FindFirstFileA",          (PVOID)FindFirstFileA,          2, CC_STDCALL },
    { "FindNextFileA",           (PVOID)FindNextFileA,           2, CC_STDCALL },
    { "FindClose",               (PVOID)FindClose,               1, CC_STDCALL },
    /* Startup / Debug */
    { "GetStartupInfoA",         (PVOID)GetStartupInfoA,         1, CC_STDCALL },
    { "GetStartupInfoW",         (PVOID)GetStartupInfoA,         1, CC_STDCALL },
    { "IsDebuggerPresent",       (PVOID)IsDebuggerPresent,       0, CC_STDCALL },
    { "CheckRemoteDebuggerPresent", (PVOID)CheckRemoteDebuggerPresent_k32, 2, CC_STDCALL },
    { "DebugActiveProcess",      (PVOID)DebugActiveProcess_k32,  1, CC_STDCALL },
    { "DebugActiveProcessStop",  (PVOID)DebugActiveProcessStop_k32, 1, CC_STDCALL },
    { "SetUnhandledExceptionFilter",(PVOID)SetUnhandledExceptionFilter,1, CC_STDCALL },
    { "AddVectoredExceptionHandler",(PVOID)AddVectoredExceptionHandler_stub,2, CC_STDCALL },
    { "RemoveVectoredExceptionHandler",(PVOID)RemoveVectoredExceptionHandler_stub,1, CC_STDCALL },
    { "UnhandledExceptionFilter",(PVOID)UnhandledExceptionFilter,1, CC_STDCALL },
    { "RaiseException",          (PVOID)RaiseException,          4, CC_STDCALL },
    { "DebugBreak",              (PVOID)DebugBreak_k32,          0, CC_STDCALL },
    { "RtlUnwind",               (PVOID)RtlUnwind,               4, CC_STDCALL },
    { "OutputDebugStringA",      (PVOID)OutputDebugStringA,      1, CC_STDCALL },
    /* String Conversion */
    { "MultiByteToWideChar",     (PVOID)MultiByteToWideChar,     6, CC_STDCALL },
    { "WideCharToMultiByte",     (PVOID)WideCharToMultiByte,     8, CC_STDCALL },
    /* Interlocked */
    { "InterlockedIncrement",    (PVOID)InterlockedIncrement,    1, CC_STDCALL },
    { "InterlockedDecrement",    (PVOID)InterlockedDecrement,    1, CC_STDCALL },
    { "InterlockedExchange",     (PVOID)InterlockedExchange,     2, CC_STDCALL },
    { "InterlockedCompareExchange",(PVOID)InterlockedCompareExchange,3, CC_STDCALL },
    { "InitializeSListHead",     (PVOID)InitializeSListHead,     1, CC_STDCALL },
    { "InterlockedPushEntrySList",(PVOID)InterlockedPushEntrySList,2, CC_STDCALL },
    { "InterlockedPopEntrySList",(PVOID)InterlockedPopEntrySList_k32, 1, CC_STDCALL },
    { "InterlockedFlushSList",   (PVOID)InterlockedFlushSList,   1, CC_STDCALL },
    /* Heap extended */
    { "HeapSize",                (PVOID)HeapSize,                3, CC_STDCALL },
    { "HeapReAlloc",             (PVOID)HeapReAlloc,             4, CC_STDCALL },
    /* UT99: File copy/delete/move */
    { "CopyFileA",               (PVOID)CopyFileA,               3, CC_STDCALL },
    { "CopyFileW",               (PVOID)CopyFileW,               3, CC_STDCALL },
    { "CopyFileExA",             (PVOID)CopyFileExA_k32,         6, CC_STDCALL },
    { "CopyFileExW",             (PVOID)CopyFileExW_k32,         6, CC_STDCALL },
    { "DeleteFileA",             (PVOID)DeleteFileA,             1, CC_STDCALL },
    { "DeleteFileW",             (PVOID)DeleteFileW,             1, CC_STDCALL },
    { "MoveFileA",               (PVOID)MoveFileA,               2, CC_STDCALL },
    { "MoveFileW",               (PVOID)MoveFileW,               2, CC_STDCALL },
    { "MoveFileExW",             (PVOID)MoveFileExW_k32,         3, CC_STDCALL },
    { "MoveFileTransactedW",     (PVOID)MoveFileTransactedW_k32, 6, CC_STDCALL },
    { "ReplaceFileW",            (PVOID)ReplaceFileW_k32,        6, CC_STDCALL },
    { "DeleteFileTransactedW",   (PVOID)DeleteFileTransactedW_k32, 2, CC_STDCALL },
    /* UT99: Directory W variants */
    { "CreateDirectoryW",        (PVOID)CreateDirectoryW,        2, CC_STDCALL },
    { "RemoveDirectoryW",        (PVOID)RemoveDirectoryW,        1, CC_STDCALL },
    { "GetCurrentDirectoryW",    (PVOID)GetCurrentDirectoryW,    2, CC_STDCALL },
    { "CreateSymbolicLinkW",     (PVOID)CreateSymbolicLinkW,     3, CC_STDCALL },
    { "SetCurrentDirectoryA",    (PVOID)SetCurrentDirectoryA,    1, CC_STDCALL },
    { "SetCurrentDirectoryW",    (PVOID)SetCurrentDirectoryW,    1, CC_STDCALL },
    { "SetFileAttributesW",      (PVOID)SetFileAttributesW,      2, CC_STDCALL },
    /* UT99: System/Windows directory */
    { "GetSystemDirectoryA",     (PVOID)GetSystemDirectoryA,     2, CC_STDCALL },
    { "GetSystemDirectoryW",     (PVOID)GetSystemDirectoryW,     2, CC_STDCALL },
    { "GetWindowsDirectoryA",    (PVOID)GetWindowsDirectoryA,    2, CC_STDCALL },
    { "GetWindowsDirectoryW",    (PVOID)GetWindowsDirectoryW,    2, CC_STDCALL },
    /* UT99: Find file W variants */
    { "FindFirstFileW",          (PVOID)FindFirstFileW,          2, CC_STDCALL },
    { "FindFirstFileExW",        (PVOID)FindFirstFileExW_k32,    6, CC_STDCALL },
    { "FindNextFileW",           (PVOID)FindNextFileW,           2, CC_STDCALL },
    /* UT99: Module filename W */
    { "GetModuleFileNameW",      (PVOID)GetModuleFileNameW,      3, CC_STDCALL },
    /* UT99: Mutex */
    { "CreateMutexA",            (PVOID)CreateMutexA,            3, CC_STDCALL },
    { "CreateMutexW",            (PVOID)CreateMutexW,            3, CC_STDCALL },
    /* UT99: Thread priority */
    { "GetThreadPriority",       (PVOID)GetThreadPriority_k32,   1, CC_STDCALL },
    { "SetThreadPriority",       (PVOID)SetThreadPriority,       2, CC_STDCALL },
    { "SetThreadExecutionState", (PVOID)SetThreadExecutionState_k32, 1, CC_STDCALL },
    { "SetPriorityClass",        (PVOID)SetPriorityClass,        2, CC_STDCALL },
    { "GetPriorityClass",        (PVOID)GetPriorityClass,        1, CC_STDCALL },
    { "SetThreadInformation",    (PVOID)SetThreadInformation_stub, 4, CC_STDCALL },
    { "RoInitialize",            (PVOID)RoInitialize_k32,        1, CC_STDCALL },
    { "RoUninitialize",          (PVOID)RoUninitialize_k32,      0, CC_STDCALL },
    { "RoGetActivationFactory",  (PVOID)RoGetActivationFactory_k32, 3, CC_STDCALL },
    { "RoActivateInstance",      (PVOID)RoActivateInstance_k32,  2, CC_STDCALL },
    { "WindowsCreateString",     (PVOID)WindowsCreateString_k32, 3, CC_STDCALL },
    { "WindowsCreateStringReference", (PVOID)WindowsCreateStringReference_k32, 4, CC_STDCALL },
    { "WindowsGetStringRawBuffer", (PVOID)WindowsGetStringRawBuffer_k32, 2, CC_STDCALL },
    { "WindowsDeleteString",     (PVOID)WindowsDeleteString_k32, 1, CC_STDCALL },
    /* Phase 21: Event objects */
    { "CreateEventA",            (PVOID)CreateEventA,            4, CC_STDCALL },
    { "CreateEventW",            (PVOID)CreateEventW,            4, CC_STDCALL },
    { "CreateSemaphoreA",        (PVOID)CreateSemaphoreA_k32,    4, CC_STDCALL },
    { "CreateSemaphoreW",        (PVOID)CreateSemaphoreW_k32,    4, CC_STDCALL },
    { "ReleaseSemaphore",        (PVOID)ReleaseSemaphore_k32,    3, CC_STDCALL },
    { "SetEvent",                (PVOID)SetEvent,                1, CC_STDCALL },
    { "ResetEvent",              (PVOID)ResetEvent,              1, CC_STDCALL },
    { "PulseEvent",              (PVOID)PulseEvent,              1, CC_STDCALL },
    { "OpenEventA",              (PVOID)OpenEventA,              3, CC_STDCALL },
    { "OpenEventW",              (PVOID)OpenEventW,              3, CC_STDCALL },
    { "CreateIoCompletionPort",  (PVOID)CreateIoCompletionPort,  4, CC_STDCALL },
    { "PostQueuedCompletionStatus", (PVOID)PostQueuedCompletionStatus, 4, CC_STDCALL },
    { "GetQueuedCompletionStatus", (PVOID)GetQueuedCompletionStatus, 5, CC_STDCALL },
    { "GetQueuedCompletionStatusEx", (PVOID)GetQueuedCompletionStatusEx, 6, CC_STDCALL },
    { "GetOverlappedResult", (PVOID)GetOverlappedResult_k32, 4, CC_STDCALL },
    { "CancelIo",                (PVOID)CancelIo_k32,           1, CC_STDCALL },
    { "CancelIoEx",              (PVOID)CancelIoEx_k32,         2, CC_STDCALL },
    { "SetFileCompletionNotificationModes", (PVOID)SetFileCompletionNotificationModes, 2, CC_STDCALL },
    { "CreateWaitableTimerA",    (PVOID)CreateWaitableTimerA_k32,    3, CC_STDCALL },
    { "CreateWaitableTimerW",    (PVOID)CreateWaitableTimerW_k32,    3, CC_STDCALL },
    { "CreateWaitableTimerExA",  (PVOID)CreateWaitableTimerExA_k32,  4, CC_STDCALL },
    { "CreateWaitableTimerExW",  (PVOID)CreateWaitableTimerExW_k32,  4, CC_STDCALL },
    { "OpenWaitableTimerA",      (PVOID)OpenWaitableTimerA_k32,      3, CC_STDCALL },
    { "OpenWaitableTimerW",      (PVOID)OpenWaitableTimerW_k32,      3, CC_STDCALL },
    { "SetWaitableTimer",        (PVOID)SetWaitableTimer_k32,        6, CC_STDCALL },
    { "SetWaitableTimerEx",      (PVOID)SetWaitableTimerEx_k32,      7, CC_STDCALL },
    { "CancelWaitableTimer",     (PVOID)CancelWaitableTimer_k32,     1, CC_STDCALL },
    /* INI file (Private Profile) */
    { "GetPrivateProfileStringA",      (PVOID)GetPrivateProfileStringA,      6, CC_STDCALL },
    { "GetPrivateProfileStringW",      (PVOID)GetPrivateProfileStringW_k32,  6, CC_STDCALL },
    { "WritePrivateProfileStringA",    (PVOID)WritePrivateProfileStringA,    4, CC_STDCALL },
    { "GetPrivateProfileIntA",         (PVOID)GetPrivateProfileIntA,         4, CC_STDCALL },
    { "GetPrivateProfileSectionNamesA",(PVOID)GetPrivateProfileSectionNamesA,3, CC_STDCALL },
    /* UT99: Process/Memory/System */
    { "GlobalMemoryStatus",      (PVOID)GlobalMemoryStatus,      1, CC_STDCALL },
    { "GlobalMemoryStatusEx",    (PVOID)GlobalMemoryStatusEx_k32,1, CC_STDCALL },
    { "SetConsoleCtrlHandler",   (PVOID)SetConsoleCtrlHandler,   2, CC_STDCALL },
    { "GetConsoleWindow",        (PVOID)GetConsoleWindow_k32,    0, CC_STDCALL },
    { "SetConsoleTitleA",        (PVOID)SetConsoleTitleA_k32,     1, CC_STDCALL },
    { "SetConsoleTitleW",        (PVOID)SetConsoleTitleW_k32,     1, CC_STDCALL },
    { "GetConsoleTitleA",        (PVOID)GetConsoleTitleA_k32,     2, CC_STDCALL },
    { "GetConsoleTitleW",        (PVOID)GetConsoleTitleW_k32,     2, CC_STDCALL },
    { "GetProcessWorkingSetSize",(PVOID)GetProcessWorkingSetSize,3, CC_STDCALL },
    { "GlobalAlloc",             (PVOID)GlobalAlloc,             2, CC_STDCALL },
    { "GlobalLock",              (PVOID)GlobalLock_k32,          1, CC_STDCALL },
    { "GlobalUnlock",            (PVOID)GlobalUnlock_k32,        1, CC_STDCALL },
    { "LocalAlloc",              (PVOID)LocalAlloc,              2, CC_STDCALL },
    { "LocalFree",               (PVOID)LocalFree,               1, CC_STDCALL },
    { "CreateProcessA",          (PVOID)CreateProcessA,          10, CC_STDCALL },
    { "CreateProcessW",          (PVOID)CreateProcessW,          10, CC_STDCALL },
    { "FormatMessageA",          (PVOID)FormatMessageA,          7, CC_STDCALL },
    { "FormatMessageW",          (PVOID)FormatMessageW,          7, CC_STDCALL },
    { "GetComputerNameA",        (PVOID)GetComputerNameA,        2, CC_STDCALL },
    { "GetComputerNameW",        (PVOID)GetComputerNameW,        2, CC_STDCALL },
    { "GetComputerNameExA",      (PVOID)GetComputerNameExA,      3, CC_STDCALL },
    { "GetComputerNameExW",      (PVOID)GetComputerNameExW,      3, CC_STDCALL },
    { "GetExitCodeProcess",      (PVOID)GetExitCodeProcess,      2, CC_STDCALL },
    { "GetLocalTime",            (PVOID)GetLocalTime,            1, CC_STDCALL },
    { "GetDateFormatW",          (PVOID)GetDateFormatW_k32,      6, CC_STDCALL },
    { "GetDateFormatEx",         (PVOID)GetDateFormatEx_k32,     7, CC_STDCALL },
    { "GetTimeFormatW",          (PVOID)GetTimeFormatW_k32,      6, CC_STDCALL },
    { "GetTimeFormatEx",         (PVOID)GetTimeFormatEx_k32,     6, CC_STDCALL },
    { "GetVersion",              (PVOID)GetVersion,              0, CC_STDCALL },
    { "GetVersionExW",           (PVOID)GetVersionExW,           1, CC_STDCALL },
    { "VerSetConditionMask",     (PVOID)VerSetConditionMask_k32, 4, CC_STDCALL },
    { "VerifyVersionInfoA",      (PVOID)VerifyVersionInfoA_k32,  4, CC_STDCALL },
    { "VerifyVersionInfoW",      (PVOID)VerifyVersionInfoW_k32,  4, CC_STDCALL },
    { "TerminateProcess",        (PVOID)TerminateProcess,        2, CC_STDCALL },
    { "HeapCreate",              (PVOID)HeapCreate,              3, CC_STDCALL },
    { "HeapDestroy",             (PVOID)HeapDestroy,             1, CC_STDCALL },
    { "HeapValidate",            (PVOID)HeapValidate,            3, CC_STDCALL },
    { "GetProcessHeaps",         (PVOID)GetProcessHeaps_k32,     2, CC_STDCALL },
    { "HeapLock",                (PVOID)HeapLock_k32,            1, CC_STDCALL },
    { "HeapUnlock",              (PVOID)HeapUnlock_k32,          1, CC_STDCALL },
    { "HeapQueryInformation",    (PVOID)HeapQueryInformation_k32, 5, CC_STDCALL },
    { "HeapSetInformation",      (PVOID)HeapSetInformation_k32, 4, CC_STDCALL },
    { "ExitThread",              (PVOID)ExitThread,              1, CC_STDCALL },
    { "SetErrorMode",            (PVOID)SetErrorMode,            1, CC_STDCALL },
    { "SetHandleCount",          (PVOID)SetHandleCount,          1, CC_STDCALL },
    { "SetStdHandle",            (PVOID)SetStdHandle,            2, CC_STDCALL },
    { "FlushFileBuffers",        (PVOID)FlushFileBuffers,        1, CC_STDCALL },
    { "DeviceIoControl",         (PVOID)DeviceIoControl_k32,     8, CC_STDCALL },
    { "GetFileType",             (PVOID)GetFileType,             1, CC_STDCALL },
    { "SetEnvironmentVariableA", (PVOID)SetEnvironmentVariableA, 2, CC_STDCALL },
    { "SetEnvironmentVariableW", (PVOID)SetEnvironmentVariableW, 2, CC_STDCALL },
    { "GetEnvironmentVariableA", (PVOID)GetEnvironmentVariableA, 3, CC_STDCALL },
    { "GetEnvironmentVariableW", (PVOID)GetEnvironmentVariableW, 3, CC_STDCALL },
    { "ExpandEnvironmentStringsA", (PVOID)ExpandEnvironmentStringsA_k32, 3, CC_STDCALL },
    { "ExpandEnvironmentStringsW", (PVOID)ExpandEnvironmentStringsW_k32, 3, CC_STDCALL },
    { "GetEnvironmentStrings",   (PVOID)GetEnvironmentStrings,   0, CC_STDCALL },
    { "GetEnvironmentStringsW",  (PVOID)GetEnvironmentStringsW,  0, CC_STDCALL },
    { "FreeEnvironmentStringsW", (PVOID)FreeEnvironmentStringsW, 1, CC_STDCALL },
    { "Beep",                    (PVOID)Beep_stub,               2, CC_STDCALL },
    { "AreFileApisANSI",         (PVOID)AreFileApisANSI,         0, CC_STDCALL },
    { "GetACP",                  (PVOID)GetACP,                  0, CC_STDCALL },
    { "GetOEMCP",                (PVOID)GetOEMCP,                0, CC_STDCALL },
    { "GetCPInfo",               (PVOID)GetCPInfo,               2, CC_STDCALL },
    { "IsDBCSLeadByte",          (PVOID)IsDBCSLeadByte,          1, CC_STDCALL },
    { "IsDBCSLeadByteEx",        (PVOID)IsDBCSLeadByteEx,        2, CC_STDCALL },
    { "GetUserDefaultLCID",      (PVOID)GetUserDefaultLCID,      0, CC_STDCALL },
    { "GetSystemDefaultLCID",    (PVOID)GetSystemDefaultLCID_k32, 0, CC_STDCALL },
    { "GetThreadLocale",         (PVOID)GetThreadLocale_k32,     0, CC_STDCALL },
    { "GetUserDefaultLangID",    (PVOID)GetUserDefaultLangID,    0, CC_STDCALL },
    { "GetUserDefaultUILanguage", (PVOID)GetUserDefaultUILanguage_k32, 0, CC_STDCALL },
    { "GetUserGeoID",            (PVOID)GetUserGeoID,            1, CC_STDCALL },
    { "GetGeoInfoW",             (PVOID)GetGeoInfoW_k32,         5, CC_STDCALL },
    { "GetUserDefaultLocaleName", (PVOID)GetUserDefaultLocaleName, 2, CC_STDCALL },
    { "LocaleNameToLCID",        (PVOID)LocaleNameToLCID_k32,    2, CC_STDCALL },
    { "LCIDToLocaleName",        (PVOID)LCIDToLocaleName_k32,    4, CC_STDCALL },
    { "GetThreadPreferredUILanguages", (PVOID)GetThreadPreferredUILanguages_k32, 4, CC_STDCALL },
    { "IsValidCodePage",         (PVOID)IsValidCodePage,         1, CC_STDCALL },
    { "IsValidLocale",           (PVOID)IsValidLocale,           2, CC_STDCALL },
    { "IsValidLocaleName",       (PVOID)IsValidLocaleName_k32,   1, CC_STDCALL },
    { "EnumSystemLocalesA",      (PVOID)EnumSystemLocalesA,      2, CC_STDCALL },
    { "EnumSystemLocalesW",      (PVOID)EnumSystemLocalesW_k32,  2, CC_STDCALL },
    { "EnumSystemLocalesEx",     (PVOID)EnumSystemLocalesEx_k32, 4, CC_STDCALL },
    { "GetLocaleInfoA",          (PVOID)GetLocaleInfoA,          4, CC_STDCALL },
    { "GetLocaleInfoW",          (PVOID)GetLocaleInfoW,          4, CC_STDCALL },
    { "GetLocaleInfoEx",         (PVOID)GetLocaleInfoEx_k32,     4, CC_STDCALL },
    { "CompareStringA",          (PVOID)CompareStringA,          6, CC_STDCALL },
    { "CompareStringW",          (PVOID)CompareStringW,          6, CC_STDCALL },
    { "CompareStringEx",         (PVOID)CompareStringEx_k32,     9, CC_STDCALL },
    { "CompareStringOrdinal",    (PVOID)CompareStringOrdinal,    5, CC_STDCALL },
    { "GetStringTypeA",          (PVOID)GetStringTypeA,          5, CC_STDCALL },
    { "GetStringTypeW",          (PVOID)GetStringTypeW,          4, CC_STDCALL },
    { "LCMapStringA",            (PVOID)LCMapStringA,            6, CC_STDCALL },
    { "LCMapStringW",            (PVOID)LCMapStringW,            6, CC_STDCALL },
    { "LCMapStringEx",           (PVOID)LCMapStringEx_k32,       9, CC_STDCALL },
    { "IsBadReadPtr",            (PVOID)IsBadReadPtr,            2, CC_STDCALL },
    { "IsBadWritePtr",           (PVOID)IsBadWritePtr,           2, CC_STDCALL },
    { "IsBadCodePtr",            (PVOID)IsBadCodePtr,            1, CC_STDCALL },
    { "GetFullPathNameW",        (PVOID)GetFullPathNameW,        4, CC_STDCALL },
    { "PathCchSkipRoot",         (PVOID)PathCchSkipRoot,         2, CC_STDCALL },
    { "PathCchCombineEx",        (PVOID)PathCchCombineEx,        5, CC_STDCALL },
    { "GetLongPathNameW",        (PVOID)GetLongPathNameW_k32,    3, CC_STDCALL },
    { "GetVolumePathNameW",      (PVOID)GetVolumePathNameW_k32,  3, CC_STDCALL },
    { "GetVolumeNameForVolumeMountPointW", (PVOID)GetVolumeNameForVolumeMountPointW_k32, 3, CC_STDCALL },
    { "GetVolumePathNamesForVolumeNameW", (PVOID)GetVolumePathNamesForVolumeNameW_k32, 4, CC_STDCALL },
    { "GetVolumeInformationA",   (PVOID)GetVolumeInformationA_k32, 8, CC_STDCALL },
    { "GetVolumeInformationW",   (PVOID)GetVolumeInformationW_k32, 8, CC_STDCALL },
    { "GetFileAttributesW",      (PVOID)GetFileAttributesW,      1, CC_STDCALL },
    { "GetFileAttributesExW",    (PVOID)GetFileAttributesExW_k32,3, CC_STDCALL },
    { "FileTimeToLocalFileTime", (PVOID)FileTimeToLocalFileTime, 2, CC_STDCALL },
    { "FileTimeToSystemTime",    (PVOID)FileTimeToSystemTime,    2, CC_STDCALL },
    { "GetTimeZoneInformation",  (PVOID)GetTimeZoneInformation,  1, CC_STDCALL },
    { "GetDynamicTimeZoneInformation", (PVOID)GetDynamicTimeZoneInformation, 1, CC_STDCALL },
    { "GetLogicalDrives",        (PVOID)GetLogicalDrives,        0, CC_STDCALL },
    { "GetLogicalDriveStringsA", (PVOID)GetLogicalDriveStringsA_k32, 2, CC_STDCALL },
    { "GetLogicalDriveStringsW", (PVOID)GetLogicalDriveStringsW_k32, 2, CC_STDCALL },
    { "GetDriveTypeA",           (PVOID)GetDriveTypeA,           1, CC_STDCALL },
    { "GetDriveTypeW",           (PVOID)GetDriveTypeW,           1, CC_STDCALL },
    { "GetDiskFreeSpaceA",       (PVOID)GetDiskFreeSpaceA,       5, CC_STDCALL },
    { "GetDiskFreeSpaceExA",     (PVOID)GetDiskFreeSpaceExA,     4, CC_STDCALL },
    { "GetDiskFreeSpaceExW",     (PVOID)GetDiskFreeSpaceExW,     4, CC_STDCALL },
    /* MSVCRT CRT init stubs */
    { "HeapCompact",             (PVOID)HeapCompact_stub,        2, CC_STDCALL },
    { "HeapWalk",                (PVOID)HeapWalk_stub,           2, CC_STDCALL },
    { "ReadConsoleA",            (PVOID)ReadConsoleA_stub,       5, CC_STDCALL },
    { "ReadConsoleW",            (PVOID)ReadConsoleW_stub,       5, CC_STDCALL },
    { "WriteConsoleW",           (PVOID)WriteConsoleW_stub,      5, CC_STDCALL },
    { "GetConsoleCP",            (PVOID)GetConsoleCP_k32,        0, CC_STDCALL },
    { "GetConsoleOutputCP",      (PVOID)GetConsoleOutputCP_stub, 0, CC_STDCALL },
    { "GetConsoleScreenBufferInfo", (PVOID)GetConsoleScreenBufferInfo_stub, 2, CC_STDCALL },
    { "SetConsoleTextAttribute", (PVOID)SetConsoleTextAttribute_stub, 2, CC_STDCALL },
    { "SetConsoleMode",          (PVOID)SetConsoleMode_stub,     2, CC_STDCALL },
    { "GetConsoleMode",          (PVOID)GetConsoleMode_stub,     2, CC_STDCALL },
    { "SetEndOfFile",            (PVOID)SetEndOfFile_stub,       1, CC_STDCALL },
    { "ReadDirectoryChangesW",   (PVOID)ReadDirectoryChangesW_k32, 8, CC_STDCALL },
    { "GetFileInformationByHandle",(PVOID)GetFileInformationByHandle_stub,2, CC_STDCALL },
    { "GetFileInformationByHandleEx",(PVOID)GetFileInformationByHandleEx_k32,4, CC_STDCALL },
    { "SetFileInformationByHandle",(PVOID)SetFileInformationByHandle_k32,4, CC_STDCALL },
    { "GetFinalPathNameByHandleW", (PVOID)GetFinalPathNameByHandleW_k32, 4, CC_STDCALL },
    { "FindResourceA",          (PVOID)FindResourceA_k32,       3, CC_STDCALL },
    { "FindResourceW",          (PVOID)FindResourceW_k32,       3, CC_STDCALL },
    { "EnumResourceNamesW",     (PVOID)EnumResourceNamesW_k32,  4, CC_STDCALL },
    { "LoadResource",           (PVOID)LoadResource_k32,        2, CC_STDCALL },
    { "LockResource",           (PVOID)LockResource_k32,        1, CC_STDCALL },
    { "SizeofResource",         (PVOID)SizeofResource_k32,      2, CC_STDCALL },
    { "PeekNamedPipe",           (PVOID)PeekNamedPipe_k32,      6, CC_STDCALL },
    { "ReadConsoleInputA",       (PVOID)ReadConsoleInputA_stub,  4, CC_STDCALL },
    { "PeekConsoleInputA",       (PVOID)PeekConsoleInputA_stub,  4, CC_STDCALL },
    { "GetNumberOfConsoleInputEvents",(PVOID)GetNumberOfConsoleInputEvents_stub,2, CC_STDCALL },
    { "LockFile",                (PVOID)LockFile_stub,           5, CC_STDCALL },
    { "UnlockFile",              (PVOID)UnlockFile_stub,         5, CC_STDCALL },
    { "LockFileEx",              (PVOID)LockFileEx_stub,         6, CC_STDCALL },
    { "UnlockFileEx",            (PVOID)UnlockFileEx_stub,       5, CC_STDCALL },
    { "CreatePipe",              (PVOID)CreatePipe_k32,         4, CC_STDCALL },
    { "CreateNamedPipeW",        (PVOID)CreateNamedPipeW_k32,    8, CC_STDCALL },
    { "ConnectNamedPipe",        (PVOID)ConnectNamedPipe_k32,    2, CC_STDCALL },
    { "DisconnectNamedPipe",     (PVOID)DisconnectNamedPipe_k32, 1, CC_STDCALL },
    { "SetNamedPipeHandleState", (PVOID)SetNamedPipeHandleState_stub, 4, CC_STDCALL },
    { "TransactNamedPipe",       (PVOID)TransactNamedPipe_stub,  7, CC_STDCALL },
    { "WaitNamedPipeW",          (PVOID)WaitNamedPipeW_stub,     2, CC_STDCALL },
    { "SetFileTime",             (PVOID)SetFileTime_stub,        4, CC_STDCALL },
    { "LocalFileTimeToFileTime", (PVOID)LocalFileTimeToFileTime_stub,2, CC_STDCALL },
    { "SystemTimeToFileTime",    (PVOID)SystemTimeToFileTime_stub,2, CC_STDCALL },
    { "SystemTimeToTzSpecificLocalTime", (PVOID)SystemTimeToTzSpecificLocalTime_k32,3, CC_STDCALL },
    { "TzSpecificLocalTimeToSystemTime", (PVOID)TzSpecificLocalTimeToSystemTime_k32,3, CC_STDCALL },
    { "GetSystemTime",           (PVOID)GetSystemTime_stub,      1, CC_STDCALL },
    { "SetLocalTime",            (PVOID)SetLocalTime_stub,       1, CC_STDCALL },
    { "GlobalFree",              (PVOID)GlobalFree_k32,          1, CC_STDCALL },
    { "ReleaseMutex",            (PVOID)ReleaseMutex_stub,       1, CC_STDCALL },
    { "OutputDebugStringW",      (PVOID)OutputDebugStringW_stub, 1, CC_STDCALL },
    { "GlobalAddAtomW",          (PVOID)GlobalAddAtomW_stub,     1, CC_STDCALL },
    /* ── Path/attribute APIs (UT99 needs these) ── */
    { "GetTempPathA",            (PVOID)GetTempPathA_k32,        2, CC_STDCALL },
    { "GetTempPathW",            (PVOID)GetTempPathW_k32,        2, CC_STDCALL },
    { "GetTempFileNameW",        (PVOID)GetTempFileNameW_k32,    4, CC_STDCALL },
    { "GetSystemDirectoryA",     (PVOID)GetSystemDirectoryA_k32, 2, CC_STDCALL },
    { "GetWindowsDirectoryA",    (PVOID)GetWindowsDirectoryA_k32,2, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *kernel32_abi_table(int *count) {
    *count = (int)(sizeof(k32_exports) / sizeof(k32_exports[0]));
    return (const WIN32_EXPORT *)k32_exports;
}

static int k32_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void k32_module_test_expect(BOOL condition, const char *name,
                                   int *checks, int *failures)
{
    (*checks)++;
    if (condition) return;
    (*failures)++;
    serial_puts("[K32MODTEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

static BOOL k32_module_test_path_ends_with(const char *path,
                                           const char *suffix)
{
    SIZE_T path_len = 0, suffix_len = 0;
    while (path[path_len]) path_len++;
    while (suffix[suffix_len]) suffix_len++;
    if (suffix_len > path_len) return FALSE;
    return k32_strcmp(path + path_len - suffix_len, suffix) == 0;
}

int kernel32_iocp_selftest(void)
{
    static const char thunk_name[] = "CreateIoCompletionPort";
    uint32_t create_iocp_thunk = 0;
    const DWORD packet_bytes = 0x233;
    const ULONG_PTR packet_key = (ULONG_PTR)0x11223344U;
    PVOID const packet_overlapped = (PVOID)(ULONG_PTR)0x12345000U;
    HANDLE port = NULL;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    PVOID overlapped = NULL;
    int checks = 0, failures = 0;
    int saved_mode = g_compat32_mode;

    g_compat32_mode = 0;
    port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 2);
    k32_module_test_expect(port != NULL && GetLastError() == 0,
                           "native create", &checks, &failures);
    if (port) {
        IOCP_PORT *test_port = iocp_find(port);
        uint64_t saved_flags, observed_flags;
        __asm__ volatile ("pushfq; popq %0; cli"
                          : "=r"(saved_flags) :: "memory");
        uint64_t lock_flags = iocp_lock(test_port);
        iocp_unlock(test_port, lock_flags);
        __asm__ volatile ("pushfq; popq %0"
                          : "=r"(observed_flags) :: "memory");
        if (saved_flags & (1ULL << 9))
            __asm__ volatile ("sti" ::: "memory");
        k32_module_test_expect(!(observed_flags & (1ULL << 9)),
                               "irq-off port lock preserves IF",
                               &checks, &failures);

        BOOL posted = PostQueuedCompletionStatus(
            port, packet_bytes, packet_key, packet_overlapped);
        k32_module_test_expect(posted, "post packet", &checks, &failures);

        BOOL dequeued = GetQueuedCompletionStatus(
            port, &bytes, &key, &overlapped, 0);
        k32_module_test_expect(dequeued && bytes == packet_bytes &&
                               key == packet_key &&
                               overlapped == packet_overlapped,
                               "dequeue packet", &checks, &failures);

        overlapped = packet_overlapped;
        BOOL empty = GetQueuedCompletionStatus(
            port, &bytes, &key, &overlapped, 0);
        k32_module_test_expect(!empty && GetLastError() == 258 &&
                               overlapped == NULL,
                               "empty queue timeout", &checks, &failures);

        HANDLE closed_port = port;
        BOOL closed = CloseHandle(port);
        port = NULL;
        k32_module_test_expect(closed, "close port", &checks, &failures);
        BOOL posted_after_close = PostQueuedCompletionStatus(
            closed_port, packet_bytes, packet_key, packet_overlapped);
        k32_module_test_expect(!posted_after_close && GetLastError() == 6,
                               "closed port rejected", &checks, &failures);
    }

    /* Exercise the PE32 pseudo-handle representation directly. It must create
     * an unassociated port, exactly like native INVALID_HANDLE_VALUE. */
    g_compat32_mode = 1;
    port = CreateIoCompletionPort((HANDLE)(ULONG_PTR)0xFFFFFFFFU,
                                  NULL, 0, 1);
    k32_module_test_expect(port != NULL, "PE32 pseudo-handle create",
                           &checks, &failures);
    if (port) {
        HANDLE associated_port = NULL;
        ULONG_PTR associated_key = 0;
        BYTE modes = 0;
        iocp_query_association((HANDLE)(ULONG_PTR)0xFFFFFFFFU,
                               win32_current_process_id(),
                               &associated_port, &associated_key, &modes);
        k32_module_test_expect(associated_port == NULL,
                               "PE32 pseudo-handle is not associated",
                               &checks, &failures);
        k32_module_test_expect(CloseHandle(port), "close PE32 direct port",
                               &checks, &failures);
        port = NULL;
    }

    /* Cross the real 32-bit thunk and INT 0x2E gateway, not just the native
     * implementation with g_compat32_mode forced. */
    g_compat32_mode = 0;
    if (!compat32_is_initialized())
        compat32_init();
    if (compat32_is_initialized()) {
        create_iocp_thunk = compat32_make_thunk_ex(
            (uint64_t)(ULONG_PTR)CreateIoCompletionPort,
            thunk_name, 4, CC_STDCALL);
    }
    k32_module_test_expect(create_iocp_thunk != 0,
                           "PE32 thunk available", &checks, &failures);
    if (create_iocp_thunk) {
        const uint32_t args[4] = { 0xFFFFFFFFU, 0, 0, 1 };
        uint32_t result = compat32_callback_args(create_iocp_thunk, 4, args);
        port = (HANDLE)(ULONG_PTR)result;
        k32_module_test_expect(port != NULL, "PE32 thunk create",
                               &checks, &failures);
        if (port) {
            k32_module_test_expect(CloseHandle(port), "close PE32 thunk port",
                                   &checks, &failures);
            port = NULL;
        }
    }

    /* Closing a port must release its fixed bookkeeping slot. Iterate past
     * MAX_IOCP_PORTS to catch lifecycle leaks deterministically. */
    BOOL recycled = TRUE;
    for (int i = 0; i < MAX_IOCP_PORTS + 8; i++) {
        port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
        if (!port || !CloseHandle(port)) {
            recycled = FALSE;
            break;
        }
        port = NULL;
    }
    k32_module_test_expect(recycled, "port slots recycle",
                           &checks, &failures);

    if (port) {
        CloseHandle(port);
        port = NULL;
    }

    /* A batched dequeue must retire every tracked pipe slot, including the
     * first packet returned by the blocking wait and later packets drained by
     * the nonblocking loop. */
    port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 2);
    k32_module_test_expect(port != NULL, "tracked batch port",
                           &checks, &failures);
    if (port) {
        int tracked_slots[2] = { -1, -1 };
        DWORD tracked_generations[2] = { 0, 0 };
        PVOID tracked_overlapped[2] = {
            (PVOID)(ULONG_PTR)0x12346000U,
            (PVOID)(ULONG_PTR)0x12347000U,
        };
        DWORD owner_pid = win32_current_process_id();
        DWORD owner_tid = GetCurrentThreadId();

        uint64_t pending_flags = pending_pipe_read_lock();
        for (int n = 0; n < 2; n++) {
            for (int i = 0; i < MAX_PENDING_PIPE_READS; i++) {
                if (!g_pending_pipe_reads[i].active &&
                    i != tracked_slots[0]) {
                    tracked_slots[n] = i;
                    break;
                }
            }
            if (tracked_slots[n] < 0)
                continue;

            K32_PENDING_PIPE_READ *pending =
                &g_pending_pipe_reads[tracked_slots[n]];
            pending->active = TRUE;
            pending->servicing = FALSE;
            pending->completed = TRUE;
            pending->packet_queued = FALSE;
            pending->compat32 = FALSE;
            pending->write = FALSE;
            pending->owner_pid = owner_pid;
            pending->owner_tid = owner_tid;
            pending->generation = ++g_pending_pipe_generation;
            if (!pending->generation)
                pending->generation = ++g_pending_pipe_generation;
            tracked_generations[n] = pending->generation;
            pending->issue_sequence = pending_pipe_next_sequence_locked();
            pending->file = (HANDLE)(ULONG_PTR)(0x7A000000U + (DWORD)n);
            pending->stream_identity = (PVOID)(ULONG_PTR)0x7A001000U;
            pending->event = NULL;
            pending->buffer = NULL;
            pending->length = 0;
            pending->progress = 0;
            pending->overlapped = tracked_overlapped[n];
            pending->completion_status = STATUS_SUCCESS;
            pending->completion_bytes = packet_bytes + (DWORD)n;
            pending->write_stage = NULL;
            pending->cancel_requested = FALSE;
            pending->abandoned = FALSE;
        }
        pending_pipe_read_unlock(pending_flags);

        k32_module_test_expect(tracked_slots[0] >= 0 && tracked_slots[1] >= 0,
                               "tracked batch slots", &checks, &failures);
        IOCP_PORT *tracked_port = iocp_find(port);
        BOOL posted_first = tracked_port && tracked_slots[0] >= 0 &&
            iocp_post_packet_status_ex(
                tracked_port, packet_bytes, packet_key,
                tracked_overlapped[0], STATUS_SUCCESS, tracked_slots[0],
                tracked_generations[0]);
        BOOL posted_second = tracked_port && tracked_slots[1] >= 0 &&
            iocp_post_packet_status_ex(
                tracked_port, packet_bytes + 1, packet_key + 1,
                tracked_overlapped[1], STATUS_SUCCESS, tracked_slots[1],
                tracked_generations[1]);
        k32_module_test_expect(posted_first && posted_second,
                               "tracked batch publish", &checks, &failures);

        K32_OVERLAPPED_ENTRY64 entries[2];
        ULONG removed = 0;
        BOOL dequeued = GetQueuedCompletionStatusEx(
            port, entries, 2, &removed, 0, FALSE);
        k32_module_test_expect(
            dequeued && removed == 2 &&
            entries[0].overlapped == tracked_overlapped[0] &&
            entries[1].overlapped == tracked_overlapped[1],
            "tracked batch dequeue", &checks, &failures);

        pending_flags = pending_pipe_read_lock();
        BOOL retired = TRUE;
        for (int n = 0; n < 2; n++) {
            if (tracked_slots[n] >= 0 &&
                g_pending_pipe_reads[tracked_slots[n]].active)
                retired = FALSE;
        }
        pending_pipe_read_unlock(pending_flags);
        k32_module_test_expect(retired, "tracked batch retires all slots",
                               &checks, &failures);

        CloseHandle(port);
        port = NULL;
        pending_flags = pending_pipe_read_lock();
        for (int n = 0; n < 2; n++) {
            if (tracked_slots[n] < 0)
                continue;
            K32_PENDING_PIPE_READ *pending =
                &g_pending_pipe_reads[tracked_slots[n]];
            if (pending->generation == tracked_generations[n]) {
                pending->active = FALSE;
                pending->servicing = FALSE;
                pending->completed = FALSE;
                pending->packet_queued = FALSE;
                pending->write_stage = NULL;
                pending->cancel_requested = FALSE;
                pending->abandoned = FALSE;
            }
        }
        pending_pipe_read_unlock(pending_flags);
    }

    g_compat32_mode = saved_mode;
#if K32_IOCP_TRACE
    __atomic_store_n(&g_iocp_api_trace_count, 0, __ATOMIC_RELAXED);
#endif
    serial_puts("[IOCPTEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

static volatile LONG g_k32_wait_test_callbacks;

static void WINAPI k32_wait_test_callback(PVOID context, BYTE timed_out)
{
    (void)context;
    (void)timed_out;
    __atomic_add_fetch(&g_k32_wait_test_callbacks, 1, __ATOMIC_RELAXED);
}

int kernel32_wait_selftest(void)
{
    HANDLE source = NULL;
    HANDLE completion = NULL;
    HANDLE token = NULL;
    HANDLE timer = NULL;
    HANDLE named_timer = NULL;
    HANDLE duplicate_timer = NULL;
    HANDLE opened_timer = NULL;
    static const WCHAR timer_name[] = {
        'O', 's', 'i', 't', 'o', 'K', 'W', 'a', 'i', 't', 'T', 'e', 's', 't', 0
    };
    int checks = 0, failures = 0;
    int saved_mode = g_compat32_mode;

    g_compat32_mode = 0;
    __atomic_store_n(&g_k32_wait_test_callbacks, 0, __ATOMIC_RELAXED);

    timer = CreateWaitableTimerExW_k32(
        NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS_K32);
    k32_module_test_expect(timer != NULL, "high-resolution timer creation",
                           &checks, &failures);
    if (timer) {
        LARGE_INTEGER due_time;
        due_time.QuadPart = -500000; /* 50 ms, in 100 ns units */
        k32_module_test_expect(
            SetWaitableTimerEx_k32(timer, &due_time, 0, NULL, NULL, NULL, 0),
            "extended timer activation", &checks, &failures);
        k32_module_test_expect(
            WaitForSingleObject(timer, 0) == WAIT_TIMEOUT,
            "timer remains unsignaled before deadline", &checks, &failures);
        k32_module_test_expect(
            WaitForSingleObject(timer, 250) == WAIT_OBJECT_0,
            "timer reaches deadline", &checks, &failures);
        k32_module_test_expect(
            WaitForSingleObject(timer, 0) == WAIT_TIMEOUT,
            "synchronization timer auto-resets", &checks, &failures);
    }

    SetLastError(0);
    HANDLE invalid_timer = CreateWaitableTimerExW_k32(
        NULL, NULL, 0x80000000U, TIMER_ALL_ACCESS_K32);
    k32_module_test_expect(
        invalid_timer == NULL && GetLastError() == 87,
        "invalid timer flags rejected", &checks, &failures);
    if (invalid_timer)
        CloseHandle(invalid_timer);

    named_timer = CreateWaitableTimerExW_k32(
        NULL, timer_name, CREATE_WAITABLE_TIMER_MANUAL_RESET,
        TIMER_ALL_ACCESS_K32);
    k32_module_test_expect(named_timer != NULL, "named timer creation",
                           &checks, &failures);
    if (named_timer) {
        SetLastError(0);
        duplicate_timer = CreateWaitableTimerExW_k32(
            NULL, timer_name, 0, TIMER_ALL_ACCESS_K32);
        k32_module_test_expect(
            duplicate_timer != NULL && GetLastError() == 183,
            "named timer create reopens existing object", &checks, &failures);
        opened_timer = OpenWaitableTimerW_k32(
            TIMER_ALL_ACCESS_K32, FALSE, timer_name);
        k32_module_test_expect(opened_timer != NULL, "named timer open",
                               &checks, &failures);

        LARGE_INTEGER due_time;
        due_time.QuadPart = -100000; /* 10 ms */
        k32_module_test_expect(
            SetWaitableTimer_k32(named_timer, &due_time, 0,
                                 NULL, NULL, FALSE),
            "named manual timer activation", &checks, &failures);
        if (duplicate_timer)
            k32_module_test_expect(
                WaitForSingleObject(duplicate_timer, 250) == WAIT_OBJECT_0,
                "named timer shares state", &checks, &failures);
        if (opened_timer)
            k32_module_test_expect(
                WaitForSingleObject(opened_timer, 0) == WAIT_OBJECT_0,
                "manual timer remains signaled", &checks, &failures);
    }

    source = CreateEventA(NULL, TRUE, FALSE, NULL);
    completion = CreateEventA(NULL, TRUE, FALSE, NULL);
    k32_module_test_expect(source != NULL && completion != NULL,
                           "registered-wait events", &checks, &failures);

    if (source && completion) {
        BOOL registered = RegisterWaitForSingleObject_k32(
            &token, source, (PVOID)k32_wait_test_callback, NULL, INFINITE, 0);
        k32_module_test_expect(registered && token != NULL,
                               "register unsignaled wait",
                               &checks, &failures);
        if (registered) {
            BOOL unregistered = UnregisterWaitEx_k32(token, completion);
            k32_module_test_expect(unregistered,
                                   "unregister with completion event",
                                   &checks, &failures);
            k32_module_test_expect(
                WaitForSingleObject(completion, 0) == WAIT_OBJECT_0,
                "completion signaled synchronously",
                &checks, &failures);
            k32_module_test_expect(
                __atomic_load_n(&g_k32_wait_test_callbacks,
                                __ATOMIC_ACQUIRE) == 0,
                "canceled callback not invoked", &checks, &failures);

            for (int i = 0; i < 100 && k32_find_registered_wait(token); i++)
                Sleep(1);
            k32_module_test_expect(k32_find_registered_wait(token) == NULL,
                                   "registration slot reclaimed",
                                   &checks, &failures);

            DWORD owner_pid = win32_current_process_id();
            for (int i = 0;
                 i < 100 && k32_wait_dispatcher_owner_active(owner_pid);
                 i++) {
                for (int j = 0; j < K32_MAX_WAIT_DISPATCHERS; j++)
                    k32_reap_wait_dispatcher(&g_wait_dispatchers[j]);
                Sleep(1);
            }
            k32_module_test_expect(
                !k32_wait_dispatcher_owner_active(owner_pid),
                "dispatcher stopped and reaped", &checks, &failures);
        }
    }

    if (completion)
        CloseHandle(completion);
    if (source)
        CloseHandle(source);
    if (opened_timer)
        CloseHandle(opened_timer);
    if (duplicate_timer)
        CloseHandle(duplicate_timer);
    if (named_timer)
        CloseHandle(named_timer);
    if (timer)
        CloseHandle(timer);
    g_compat32_mode = saved_mode;

    serial_puts("[K32WAITTEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

int kernel32_module_selftest(void)
{
    static const char module_name[] =
        "api-ms-win-power-setting-l1-1-0.dll";
    MEMORY_BASIC_INFORMATION_K32 mbi;
    MEMORY_BASIC_INFORMATION32_K32 mbi32;
    char module_path[260];
    int checks = 0, failures = 0;
    int saved_mode = g_compat32_mode;

    /* The shell can run this before the first winexec invocation. Bootstrap
     * only the loader pieces exercised here; winexec_run will perform its
     * normal full reset and registration before launching an application. */
    if (!dll_is_shim(module_name)) {
        int abi_count = 0;
        dll_loader_init();
        dll_register_shim(module_name, kernel32_resolve);
        win32_abi_register(module_name,
                           kernel32_abi_table(&abi_count), abi_count);
    }
    if (!compat32_is_initialized())
        compat32_init();

    g_compat32_mode = 0;
    WCHAR conversion_wide[4] = {0};
    char conversion_narrow[4] = {0};
    SetLastError(0);
    k32_module_test_expect(
        MultiByteToWideChar(CP_UTF8, 0, NULL, -1,
                            conversion_wide, 4) == 0 &&
        GetLastError() == 87,
        "MultiByteToWideChar rejects null input", &checks, &failures);
    SetLastError(0);
    k32_module_test_expect(
        MultiByteToWideChar(CP_UTF8, 0, "abc", -1,
                            conversion_wide, 3) == 0 &&
        GetLastError() == 122,
        "MultiByteToWideChar rejects short output", &checks, &failures);
    k32_module_test_expect(
        MultiByteToWideChar(CP_UTF8, 0, "abc", -1,
                            conversion_wide, 4) == 4 &&
        conversion_wide[0] == 'a' && conversion_wide[3] == 0,
        "MultiByteToWideChar converts complete input", &checks, &failures);
    SetLastError(0);
    k32_module_test_expect(
        WideCharToMultiByte(CP_UTF8, 0, NULL, -1,
                            conversion_narrow, 4, NULL, NULL) == 0 &&
        GetLastError() == 87,
        "WideCharToMultiByte rejects null input", &checks, &failures);
    SetLastError(0);
    k32_module_test_expect(
        WideCharToMultiByte(CP_UTF8, 0, conversion_wide, -1,
                            conversion_narrow, 3, NULL, NULL) == 0 &&
        GetLastError() == 122,
        "WideCharToMultiByte rejects short output", &checks, &failures);
    k32_module_test_expect(
        WideCharToMultiByte(CP_UTF8, 0, conversion_wide, -1,
                            conversion_narrow, 4, NULL, NULL) == 4 &&
        conversion_narrow[0] == 'a' && conversion_narrow[3] == 0,
        "WideCharToMultiByte converts complete input", &checks, &failures);

    char locale_cp[8] = {0};
    WCHAR locale_name[8] = {0};
    k32_module_test_expect(
        GetLocaleInfoA(0x0409, 0x1004, locale_cp,
                       (int)sizeof(locale_cp)) == 5 &&
        k32_strcmp(locale_cp, "1252") == 0,
        "GetLocaleInfoA ANSI code page", &checks, &failures);
    k32_module_test_expect(
        GetLocaleInfoW(0x0409, 0x005C, locale_name,
                       (int)(sizeof(locale_name) / sizeof(locale_name[0]))) == 6 &&
        locale_name[0] == 'e' && locale_name[2] == '-' &&
        locale_name[5] == 0,
        "GetLocaleInfoW locale name", &checks, &failures);

    char compat_command[4096];
    static const char gpu_command[] =
        "steamwebhelper.exe --type=gpu-process --use-angle=gl";
    PCSTR adjusted_gpu_command = process_apply_compat_flags(
        "C:\\Steam\\steamwebhelper.exe", gpu_command, compat_command);
    k32_module_test_expect(
        adjusted_gpu_command &&
        process_command_contains(adjusted_gpu_command,
                                 "--disable-gpu-watchdog"),
        "Steam GPU child disables GPU watchdog", &checks, &failures);

    static const char browser_command[] = "steamwebhelper.exe";
    PCSTR adjusted_browser_command = process_apply_compat_flags(
        "C:\\Steam\\steamwebhelper.exe", browser_command, compat_command);
    k32_module_test_expect(
        adjusted_browser_command &&
        !process_command_contains(adjusted_browser_command,
                                  "--disable-gpu-watchdog"),
        "Steam browser process keeps GPU watchdog", &checks, &failures);

    static const char complete_gpu_command[] =
        "steamwebhelper.exe --type=gpu-process --use-angle=swiftshader "
        "--disable-hang-monitor --disable-gpu-watchdog "
        "--disable-stack-profiler";
    k32_module_test_expect(
        process_apply_compat_flags("C:\\Steam\\steamwebhelper.exe",
                                   complete_gpu_command,
                                   compat_command) == complete_gpu_command,
        "Steam GPU flags are idempotent", &checks, &failures);

    k32_module_test_expect(dll_export_lookup_selftest() == 0,
                           "PE export hint/binary search", &checks,
                           &failures);
    k32_module_test_expect(dll_is_shim(module_name),
                           "shim registry initialized", &checks, &failures);

    HANDLE module = GetModuleHandleA(module_name);
    k32_module_test_expect(module != NULL, "GetModuleHandle creates facade",
                           &checks, &failures);
    if (!module) goto done;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)module;
    BOOL dos_valid = dos->e_magic == IMAGE_DOS_SIGNATURE &&
                     dos->e_lfanew >= (LONG)sizeof(*dos) &&
                     dos->e_lfanew < 0x1000;
    k32_module_test_expect(dos_valid, "DOS header", &checks, &failures);

    PIMAGE_NT_HEADERS64 nt = dos_valid
        ? (PIMAGE_NT_HEADERS64)((BYTE *)module + dos->e_lfanew) : NULL;
    k32_module_test_expect(nt && nt->Signature == IMAGE_NT_SIGNATURE &&
                           nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                           (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) &&
                           nt->OptionalHeader.Magic ==
                               IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                           nt->OptionalHeader.ImageBase ==
                               (ULONGLONG)(ULONG_PTR)module &&
                           nt->OptionalHeader.SizeOfImage ==
                               DLL_SYNTHETIC_SHIM_IMAGE_SIZE,
                           "PE32+ module header", &checks, &failures);

    LOADED_MODULE *record = dll_find_module_by_base((PVOID)module);
    k32_module_test_expect(record && record->synthetic_shim &&
                           record->owner_pid == win32_current_process_id(),
                           "owner-aware loader record", &checks, &failures);
    int initial_refs = record ? record->ref_count : 0;

    HANDLE referenced = NULL;
    BOOL referenced_ok = GetModuleHandleExA(0, module_name, &referenced);
    k32_module_test_expect(referenced_ok && referenced == module && record &&
                           record->ref_count == initial_refs + 1,
                           "GetModuleHandleEx takes reference",
                           &checks, &failures);
    k32_module_test_expect(referenced && FreeLibrary(referenced) && record &&
                           record->ref_count == initial_refs,
                           "GetModuleHandleEx reference balance",
                           &checks, &failures);

    HANDLE unchanged = NULL;
    BOOL unchanged_ok = GetModuleHandleExA(
        K32_GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        module_name, &unchanged);
    k32_module_test_expect(unchanged_ok && unchanged == module && record &&
                           record->ref_count == initial_refs,
                           "GetModuleHandleEx unchanged refcount",
                           &checks, &failures);

    HANDLE from_address = NULL;
    BOOL from_address_ok = GetModuleHandleExA(
        K32_GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (PCSTR)((BYTE *)module + 0x100), &from_address);
    k32_module_test_expect(from_address_ok && from_address == module &&
                           record && record->ref_count == initial_refs + 1,
                           "GetModuleHandleEx from address",
                           &checks, &failures);
    k32_module_test_expect(from_address && FreeLibrary(from_address) &&
                           record && record->ref_count == initial_refs,
                           "from-address reference balance",
                           &checks, &failures);

    HANDLE invalid_address = NULL;
    SetLastError(0);
    k32_module_test_expect(
        !GetModuleHandleExA(K32_GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            (PCSTR)(ULONG_PTR)0x1234, &invalid_address) &&
        GetLastError() == 126,
        "from-address rejects unrelated address", &checks, &failures);

    HANDLE loaded = LoadLibraryA(module_name);
    k32_module_test_expect(loaded == module && record &&
                           record->ref_count == initial_refs + 1,
                           "LoadLibrary reference", &checks, &failures);

    typedef DWORD (WINAPI *get_pid_fn)(void);
    get_pid_fn get_pid = loaded
        ? (get_pid_fn)GetProcAddress(loaded, "GetCurrentProcessId") : NULL;
    k32_module_test_expect(get_pid &&
                           get_pid() == win32_current_process_id(),
                           "named shim export", &checks, &failures);

    typedef BOOL (WINAPI *get_volume_information_a_fn)(
        PCSTR, PSTR, DWORD, DWORD *, DWORD *, DWORD *, PSTR, DWORD);
    get_volume_information_a_fn get_volume_information_a = loaded
        ? (get_volume_information_a_fn)GetProcAddress(
              loaded, "GetVolumeInformationA") : NULL;
    char volume_name[64] = {0};
    char filesystem_name[16] = {0};
    DWORD volume_serial = 0;
    DWORD max_component = 0;
    DWORD filesystem_flags = 0;
    BOOL volume_ok = get_volume_information_a &&
        get_volume_information_a("C:\\", volume_name,
                                 sizeof(volume_name), &volume_serial,
                                 &max_component, &filesystem_flags,
                                 filesystem_name, sizeof(filesystem_name));
    k32_module_test_expect(volume_ok && volume_name[0] &&
                           k32_strcmp(filesystem_name, "OSITOFS") == 0 &&
                           volume_serial == 0x4F534954 &&
                           max_component == 255 &&
                           (filesystem_flags & 0x6) == 0x6,
                           "GetVolumeInformationA contract", &checks,
                           &failures);

    char short_volume_name[1];
    SetLastError(0);
    k32_module_test_expect(
        get_volume_information_a &&
        !get_volume_information_a(NULL, short_volume_name,
                                  sizeof(short_volume_name), NULL, NULL,
                                  NULL, NULL, 0) &&
        GetLastError() == 122,
        "GetVolumeInformationA short buffer", &checks, &failures);

    typedef BOOL (WINAPI *get_disk_free_space_ex_a_fn)(
        PCSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER);
    get_disk_free_space_ex_a_fn get_disk_free_space_ex_a = loaded
        ? (get_disk_free_space_ex_a_fn)GetProcAddress(
              loaded, "GetDiskFreeSpaceExA") : NULL;
    ULARGE_INTEGER free_available = {0};
    ULARGE_INTEGER total_bytes = {0};
    ULARGE_INTEGER total_free = {0};
    BOOL disk_space_ok = get_disk_free_space_ex_a &&
        get_disk_free_space_ex_a("C:\\", &free_available, &total_bytes,
                                 &total_free);
    k32_module_test_expect(disk_space_ok && total_bytes.QuadPart > 0 &&
                           total_free.QuadPart <= total_bytes.QuadPart &&
                           free_available.QuadPart == total_free.QuadPart,
                           "GetDiskFreeSpaceExA contract", &checks,
                           &failures);

    SIZE_T query_size = VirtualQuery(module, &mbi, sizeof(mbi));
    k32_module_test_expect(query_size == sizeof(mbi) &&
                           mbi.AllocationBase == module &&
                           mbi.State == MEM_COMMIT &&
                           mbi.Type == 0x01000000UL,
                           "VirtualQuery MEM_IMAGE", &checks, &failures);

    DWORD path_len = GetModuleFileNameA(module, module_path,
                                        sizeof(module_path));
    k32_module_test_expect(path_len > 0 && path_len < sizeof(module_path) &&
                           k32_module_test_path_ends_with(module_path,
                                                          module_name),
                           "module filename", &checks, &failures);

    BOOL freed = loaded ? FreeLibrary(loaded) : FALSE;
    k32_module_test_expect(freed && record &&
                           dll_find_module_by_base((PVOID)module) == record &&
                           record->ref_count == initial_refs,
                           "FreeLibrary reference balance",
                           &checks, &failures);

    g_compat32_mode = 1;
    HANDLE module32 = GetModuleHandleA(module_name);
    k32_module_test_expect(module32 && module32 != module &&
                           (ULONG_PTR)module32 <= 0xFFFFFFFFULL,
                           "PE32 facade is distinct and addressable",
                           &checks, &failures);

    PIMAGE_DOS_HEADER dos32 = (PIMAGE_DOS_HEADER)module32;
    BOOL dos32_valid = module32 &&
                       dos32->e_magic == IMAGE_DOS_SIGNATURE &&
                       dos32->e_lfanew >= (LONG)sizeof(*dos32) &&
                       dos32->e_lfanew < 0x1000;
    PIMAGE_NT_HEADERS32 nt32 = dos32_valid
        ? (PIMAGE_NT_HEADERS32)((BYTE *)module32 + dos32->e_lfanew) : NULL;
    k32_module_test_expect(nt32 && nt32->Signature == IMAGE_NT_SIGNATURE &&
                           nt32->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
                           nt32->OptionalHeader.Magic ==
                               IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
                           nt32->OptionalHeader.ImageBase ==
                               (DWORD)(ULONG_PTR)module32,
                           "PE32 module header", &checks, &failures);

    LOADED_MODULE *record32 = dll_find_module_by_base((PVOID)module32);
    k32_module_test_expect(record32 && record32->synthetic_shim &&
                           record32->image.Is32Bit &&
                           record32->owner_pid == win32_current_process_id(),
                           "PE32 owner-aware loader record",
                           &checks, &failures);

    RtlZeroMemory(&mbi32, sizeof(mbi32));
    SIZE_T query_size32 = VirtualQuery(module32, &mbi32, sizeof(mbi32));
    k32_module_test_expect(
        query_size32 == sizeof(mbi32) &&
        mbi32.AllocationBase == (uint32_t)(ULONG_PTR)module32 &&
        mbi32.State == MEM_COMMIT &&
        mbi32.Type == 0x01000000UL,
        "PE32 VirtualQuery layout", &checks, &failures);

    HANDLE module32_again = GetModuleHandleA(module_name);
    k32_module_test_expect(module32_again == module32,
                           "PE32 facade lookup is stable",
                           &checks, &failures);

    PVOID get_pid32 = GetProcAddress(module32, "GetCurrentProcessId");
    k32_module_test_expect(get_pid32 &&
                           (ULONG_PTR)get_pid32 <= 0xFFFFFFFFULL,
                           "PE32 shim export uses a thunk",
                           &checks, &failures);

    int initial_refs32 = record32 ? record32->ref_count : 0;
    HANDLE loaded32 = LoadLibraryA(module_name);
    k32_module_test_expect(loaded32 == module32 && record32 &&
                           record32->ref_count == initial_refs32 + 1 &&
                           record && record->ref_count == initial_refs,
                           "PE32 reference is ABI-local",
                           &checks, &failures);
    BOOL freed32 = loaded32 ? FreeLibrary(loaded32) : FALSE;
    k32_module_test_expect(freed32 && record32 &&
                           record32->ref_count == initial_refs32,
                           "PE32 FreeLibrary reference balance",
                           &checks, &failures);

    g_compat32_mode = 0;
    k32_module_test_expect(GetModuleHandleA(module_name) == module,
                           "PE32+ facade remains selected",
                           &checks, &failures);

done:
    g_compat32_mode = saved_mode;
    serial_puts("[K32MODTEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

PVOID kernel32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;
    if (!g_compat32_mode &&
        k32_strcmp(func_name, "SetFilePointerEx") == 0)
        return (PVOID)SetFilePointerEx_k64;
    if (!g_compat32_mode &&
        k32_strcmp(func_name, "VerSetConditionMask") == 0)
        return (PVOID)VerSetConditionMask_k64;
    if (!g_compat32_mode &&
        k32_strcmp(func_name, "VerifyVersionInfoA") == 0)
        return (PVOID)VerifyVersionInfoA_k64;
    if (!g_compat32_mode &&
        k32_strcmp(func_name, "VerifyVersionInfoW") == 0)
        return (PVOID)VerifyVersionInfoW_k64;

    for (int i = 0; k32_exports[i].name; i++) {
        if (k32_strcmp(func_name, k32_exports[i].name) == 0)
            return k32_exports[i].func;
    }

    return NULL;
}

PVOID kernel32_shim_init(void)
{
    (void)win32_thread_table_ensure();

    /* Re-exec reset: don't leak the previous run's last-error into the fresh
     * process (real NT starts a process with LastError = 0). The heap_pool is
     * intentionally KEPT — it's a kmalloc arena reused across runs. */
    g_last_error = 0;
    kernel32_release_process_exception_state(k32_exception_filter_owner());
    /* Named kernel objects form a global namespace shared by child processes.
     * Their entries are reclaimed when the last endpoint closes. */
    memset(k32_jobs, 0, sizeof(k32_jobs));
    g_primary_thread_description[0] = 0;
    {
        extern void random_get_bytes(void *buf, uint32_t len);
        random_get_bytes(&k32_pointer_cookie, sizeof(k32_pointer_cookie));
        if (!k32_pointer_cookie)
            k32_pointer_cookie = (ULONG_PTR)0x9E3779B97F4A7C15ULL;
    }
    iocp_reset_all();
    win32_reset_current_directory();
    sync_last_error();
    return (PVOID)k32_exports;
}
