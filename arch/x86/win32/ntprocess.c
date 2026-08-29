/*
 * OsitoK Windows Compatibility Layer — Process & Thread Management
 *
 * NT-style process and thread creation for Windows PE executables.
 *
 * Implements:
 *   NtCreateProcess / NtCreateProcessEx
 *   NtCreateThread
 *   NtTerminateThread
 *   NtResumeThread
 *   NtWaitForSingleObject
 *   NtQueryInformationProcess
 *
 * Design:
 *   - Process = loaded PE image + handle table + PEB
 *   - Thread  = execution context (register state + stack)
 *   - Uses OsitoK's proc_* infrastructure underneath
 *   - Single-address-space model (identity-mapped)
 */

#include "ntsyscall.h"
#include "handle.h"
#include "pe.h"

/* ── External kernel interfaces ─────────────────────────────── */

extern void  serial_puts(const char *s);
extern void  serial_puthex(uint64_t val, int digits);
extern void  serial_putdec(uint64_t val);
extern void  proc_exit(int32_t code);
extern uint64_t idt_get_ticks(void);

/* ── Thread state ───────────────────────────────────────────── */

#define MAX_THREADS     192

typedef enum _THREAD_STATE {
    THREAD_FREE     = 0,
    THREAD_READY    = 1,
    THREAD_RUNNING  = 2,
    THREAD_SUSPENDED = 3,
    THREAD_TERMINATED = 4,
} THREAD_STATE;

typedef struct _THREAD_OBJECT {
    ULONG           tid;
    THREAD_STATE    state;
    NTSTATUS        exit_status;
    PVOID           stack_allocation;
    PVOID           stack_limit;
    PVOID           stack_base;
    SIZE_T          stack_size;
    PVOID           start_address;  /* entry point */
    PVOID           parameter;      /* arg to entry */
    ULONG           suspend_count;
    PVOID           impersonation_token;
    TEB             teb;            /* per-thread TEB */
    /* Saved register context (for context switch) */
    uint64_t        saved_rsp;
    uint64_t        saved_rip;
} THREAD_OBJECT, *PTHREAD_OBJECT;

/* ── Process state ──────────────────────────────────────────── */

#define MAX_NT_PROCESSES  32
#define NT_PROCESS_IMAGE_PATH_CAP 320

typedef struct _PROCESS_OBJECT {
    ULONG           pid;
    BOOL            active;
    NTSTATUS        exit_status;
    HANDLE_TABLE    handles;
    PEB             peb;
    PE_IMAGE_INFO   image;
    char            image_path[NT_PROCESS_IMAGE_PATH_CAP];
    THREAD_OBJECT   threads[MAX_THREADS];
    ULONG           thread_count;
    ULONG           next_tid;
} PROCESS_OBJECT, *PPROCESS_OBJECT;

static PROCESS_OBJECT g_processes[MAX_NT_PROCESSES];
static PROCESS_OBJECT *g_current_process = NULL;
static ULONG g_next_pid = 1;
/* ponytail: one live primary-thread object until processes own native main threads. */
static THREAD_OBJECT g_primary_thread;

PVOID nt_process_primary_thread_object(ULONG tid)
{
    if (!tid) return NULL;
    g_primary_thread.tid = tid;
    g_primary_thread.state = THREAD_RUNNING;
    g_primary_thread.exit_status = STATUS_SUCCESS;
    return &g_primary_thread;
}

PVOID nt_thread_get_impersonation_token(PVOID thread_object)
{
    THREAD_OBJECT *thread = (THREAD_OBJECT *)thread_object;
    if (!thread || thread->state == THREAD_FREE)
        return NULL;
    return __atomic_load_n(&thread->impersonation_token, __ATOMIC_ACQUIRE);
}

BOOL nt_thread_set_impersonation_token(PVOID thread_object,
                                       PVOID token_object)
{
    THREAD_OBJECT *thread = (THREAD_OBJECT *)thread_object;
    if (!thread || thread->state == THREAD_FREE)
        return FALSE;
    __atomic_store_n(&thread->impersonation_token, token_object,
                     __ATOMIC_RELEASE);
    return TRUE;
}

/* ── External handle table (from ntsyscall.c) ───────────────── */
extern HANDLE_TABLE g_handle_table;
extern void ntsync_notify_object(OBJECT_TYPE_ID type, PVOID object);
extern DWORD win32_current_process_id(void);
extern BOOL win32_resume_thread_execution(PVOID thread_object);
extern void ntdll_release_process_inspection(ULONG process_id);
NTSTATUS nt_process_open_for_process(ULONG pid, ACCESS_MASK access,
                                     ULONG owner_pid, PHANDLE out_handle);

/* ── Helpers ────────────────────────────────────────────────── */

static void nt_proc_log(const char *msg)
{
#if defined(OK_QUIET) && OK_QUIET
    (void)msg;
#else
    serial_puts("[NT-PROC] ");
    serial_puts(msg);
    serial_puts("\n");
#endif
}

static void nt_proc_log_hex(const char *prefix, ULONGLONG val)
{
#if defined(OK_QUIET) && OK_QUIET
    (void)prefix;
    (void)val;
#else
    serial_puts("[NT-PROC] ");
    serial_puts(prefix);
    serial_puthex(val, 16);
    serial_puts("\n");
#endif
}

static inline void nt_proc_memset(void *s, int c, SIZE_T n)
{
    BYTE *p = (BYTE *)s;
    while (n--) *p++ = (BYTE)c;
}

static BOOL process_object_referenced(PROCESS_OBJECT *proc)
{
    if (handle_object_referenced(&g_handle_table, OBJ_TYPE_PROCESS, proc))
        return TRUE;
    for (int i = 0; i < MAX_THREADS; i++)
        if (handle_object_referenced(&g_handle_table, OBJ_TYPE_THREAD,
                                     &proc->threads[i]))
            return TRUE;
    return FALSE;
}

/* ── Process allocation ─────────────────────────────────────── */

static PROCESS_OBJECT *alloc_process(void)
{
    for (int i = 0; i < MAX_NT_PROCESSES; i++) {
        if (!g_processes[i].active &&
            !process_object_referenced(&g_processes[i])) {
            PROCESS_OBJECT *proc = &g_processes[i];
            if (proc->pid)
                ntdll_release_process_inspection(proc->pid);
            nt_proc_memset(proc, 0, sizeof(PROCESS_OBJECT));
            proc->pid = g_next_pid++;
            proc->active = TRUE;
            proc->next_tid = 1;
            handle_table_init(&proc->handles);
            return proc;
        }
    }
    serial_puts("[NT-PROC] process object table full\n");
    return NULL;
}

static PROCESS_OBJECT *current_process(void)
{
    ULONG pid = win32_current_process_id();
    for (int i = 0; i < MAX_NT_PROCESSES; i++) {
        if (g_processes[i].pid == pid)
            return &g_processes[i];
    }

    /* Only the initial process is created lazily. Child objects exist before
     * their first instruction runs, so silently creating one here is wrong. */
    if (pid != 1)
        return NULL;

    if (!g_current_process) {
        g_current_process = alloc_process();
        if (g_current_process)
            g_current_process->peb.ProcessHeap = (PVOID)(ULONG_PTR)0xBEEF0001;
    }
    return g_current_process;
}

BOOL nt_process_set_image_base(PVOID process_object, PVOID image_base)
{
    PROCESS_OBJECT *proc = process_object
                         ? (PROCESS_OBJECT *)process_object
                         : current_process();
    if (!proc) return FALSE;
    __atomic_store_n(&proc->peb.ImageBaseAddress, image_base,
                     __ATOMIC_RELEASE);
    return TRUE;
}

BOOL nt_process_set_peb(PVOID process_object, const PEB *source)
{
    PROCESS_OBJECT *proc = process_object
                         ? (PROCESS_OBJECT *)process_object
                         : current_process();
    if (!proc || !source) return FALSE;
    proc->peb = *source;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return TRUE;
}

BOOL nt_process_set_image_path(PVOID process_object, const char *source)
{
    PROCESS_OBJECT *proc = process_object
                         ? (PROCESS_OBJECT *)process_object
                         : current_process();
    if (!proc || !source) return FALSE;

    SIZE_T length = 0;
    while (source[length] && length + 1 < NT_PROCESS_IMAGE_PATH_CAP) {
        proc->image_path[length] = source[length];
        length++;
    }
    if (source[length]) return FALSE;
    proc->image_path[length] = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return TRUE;
}

/* ── Thread allocation ──────────────────────────────────────── */

NTSTATUS nt_thread_open_current_for_process(ACCESS_MASK access,
                                            ULONG owner_pid,
                                            PHANDLE out_handle)
{
    extern DWORD WINAPI GetCurrentThreadId(void);
    extern PVOID win32_current_thread_object(void);

    if (!owner_pid || !out_handle)
        return STATUS_INVALID_PARAMETER;

    PROCESS_OBJECT *proc = current_process();
    if (!proc)
        return STATUS_INVALID_HANDLE;

    THREAD_OBJECT *thread = (THREAD_OBJECT *)win32_current_thread_object();

    /* The initial process predates native primary-thread objects. */
    if (!thread && proc->pid == 1)
        thread = (THREAD_OBJECT *)nt_process_primary_thread_object(
            GetCurrentThreadId());
    if (!thread)
        return STATUS_INVALID_HANDLE;

    return handle_alloc_for_process(&g_handle_table, OBJ_TYPE_THREAD, access,
                                    thread, owner_pid, out_handle);
}

NTSTATUS nt_process_open(ULONG pid, ACCESS_MASK access, PHANDLE out_handle)
{
    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    return nt_process_open_for_process(pid, access, owner_pid, out_handle);
}

NTSTATUS nt_process_open_for_process(ULONG pid, ACCESS_MASK access,
                                     ULONG owner_pid, PHANDLE out_handle)
{
    if (!out_handle || !owner_pid)
        return STATUS_INVALID_PARAMETER;

    if (!current_process()) return STATUS_INSUFFICIENT_RESOURCES;
    PROCESS_OBJECT *proc = NULL;
    for (int i = 0; i < MAX_NT_PROCESSES; i++) {
        if (g_processes[i].pid == pid &&
            (g_processes[i].active ||
             process_object_referenced(&g_processes[i]))) {
            proc = &g_processes[i];
            break;
        }
    }
    if (!proc) return STATUS_INVALID_PARAMETER;

    return handle_alloc_for_process(&g_handle_table, OBJ_TYPE_PROCESS, access,
                                    proc, owner_pid, out_handle);
}

static THREAD_OBJECT *alloc_thread(PROCESS_OBJECT *proc)
{
    if (!proc || proc->thread_count >= MAX_THREADS)
        return NULL;

    for (int i = 0; i < MAX_THREADS; i++) {
        if (proc->threads[i].state == THREAD_FREE) {
            THREAD_OBJECT *thread = &proc->threads[i];
            nt_proc_memset(thread, 0, sizeof(THREAD_OBJECT));
            thread->tid = proc->next_tid++;
            thread->state = THREAD_READY;
            proc->thread_count++;
            return thread;
        }
    }
    return NULL;
}

NTSTATUS nt_process_create_child(PHANDLE process_handle,
                                 PHANDLE thread_handle,
                                 ULONG *process_id, ULONG *thread_id,
                                 PVOID *process_object, PVOID *thread_object,
                                 BOOL create_suspended)
{
    if (!process_handle || !thread_handle)
        return STATUS_INVALID_PARAMETER;

    PROCESS_OBJECT *proc = alloc_process();
    if (!proc) return STATUS_INSUFFICIENT_RESOURCES;
    proc->peb.ProcessHeap = (PVOID)(ULONG_PTR)0xBEEF0001;

    THREAD_OBJECT *thread = alloc_thread(proc);
    if (!thread) {
        proc->active = FALSE;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    thread->state = create_suspended ? THREAD_SUSPENDED : THREAD_RUNNING;
    thread->suspend_count = create_suspended ? 1 : 0;

    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_PROCESS,
                                   GENERIC_ALL, proc, process_handle);
    if (!NT_SUCCESS(status)) goto fail;
    status = handle_alloc(&g_handle_table, OBJ_TYPE_THREAD,
                          GENERIC_ALL, thread, thread_handle);
    if (!NT_SUCCESS(status)) {
        handle_close(&g_handle_table, *process_handle);
        goto fail;
    }

    serial_puts("[NT-PROC-CHILD] creator=");
    serial_putdec(win32_current_process_id());
    serial_puts(" child=");
    serial_putdec(proc->pid);
    serial_puts(" process_handle=0x");
    serial_puthex((uint64_t)(ULONG_PTR)*process_handle, 8);
    serial_puts(" thread_handle=0x");
    serial_puthex((uint64_t)(ULONG_PTR)*thread_handle, 8);
    serial_puts("\n");

    if (process_id) *process_id = proc->pid;
    if (thread_id) *thread_id = thread->tid;
    if (process_object) *process_object = proc;
    if (thread_object) *thread_object = thread;
    return STATUS_SUCCESS;

fail:
    thread->state = THREAD_FREE;
    proc->thread_count = 0;
    proc->active = FALSE;
    return status;
}

void nt_process_complete_child(PVOID process_object, PVOID thread_object,
                               NTSTATUS exit_status)
{
    PROCESS_OBJECT *proc = (PROCESS_OBJECT *)process_object;
    THREAD_OBJECT *thread = (THREAD_OBJECT *)thread_object;
    if (thread) {
        thread->exit_status = exit_status;
        thread->state = THREAD_TERMINATED;
        ntsync_notify_object(OBJ_TYPE_THREAD, thread);
    }
    if (proc) {
        proc->exit_status = exit_status;
        proc->active = FALSE;
        ntsync_notify_object(OBJ_TYPE_PROCESS, proc);
    }
}

BOOL nt_process_exit_code(HANDLE handle, DWORD *exit_code)
{
    PROCESS_OBJECT *proc = NULL;
    if (!exit_code ||
        !NT_SUCCESS(handle_lookup(&g_handle_table, handle, OBJ_TYPE_PROCESS,
                                  (PVOID *)&proc)))
        return FALSE;
    *exit_code = proc->active ? 259 : (DWORD)proc->exit_status;
    return TRUE;
}

BOOL nt_process_id(HANDLE handle, DWORD *process_id)
{
    PROCESS_OBJECT *proc = NULL;
    if (!process_id ||
        !NT_SUCCESS(handle_lookup(&g_handle_table, handle, OBJ_TYPE_PROCESS,
                                  (PVOID *)&proc)))
        return FALSE;
    *process_id = proc->pid;
    return TRUE;
}

BOOL nt_process_image_path(HANDLE handle, char *destination,
                           SIZE_T capacity)
{
    PROCESS_OBJECT *proc = NULL;
    if (!destination || !capacity ||
        !NT_SUCCESS(handle_lookup(&g_handle_table, handle, OBJ_TYPE_PROCESS,
                                  (PVOID *)&proc)) ||
        !proc || !proc->image_path[0])
        return FALSE;

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    SIZE_T length = 0;
    while (proc->image_path[length] && length + 1 < capacity) {
        destination[length] = proc->image_path[length];
        length++;
    }
    if (proc->image_path[length]) return FALSE;
    destination[length] = 0;
    return TRUE;
}

BOOL nt_process_image_base(HANDLE handle, PVOID *image_base)
{
    PROCESS_OBJECT *proc = NULL;
    if (!image_base ||
        !NT_SUCCESS(handle_lookup(&g_handle_table, handle, OBJ_TYPE_PROCESS,
                                  (PVOID *)&proc)) || !proc)
        return FALSE;
    *image_base = __atomic_load_n(&proc->peb.ImageBaseAddress,
                                  __ATOMIC_ACQUIRE);
    return TRUE;
}

BOOL nt_thread_id(HANDLE handle, DWORD *thread_id)
{
    THREAD_OBJECT *thread = NULL;
    if (!thread_id ||
        !NT_SUCCESS(handle_lookup(&g_handle_table, handle, OBJ_TYPE_THREAD,
                                  (PVOID *)&thread)))
        return FALSE;
    *thread_id = thread->tid;
    return TRUE;
}

BOOL nt_process_object_signaled(PVOID object)
{
    PROCESS_OBJECT *proc = (PROCESS_OBJECT *)object;
    return proc && !proc->active;
}

static PROCESS_OBJECT *thread_owner(THREAD_OBJECT *thread)
{
    for (int p = 0; p < MAX_NT_PROCESSES; p++) {
        for (int t = 0; t < MAX_THREADS; t++) {
            if (&g_processes[p].threads[t] == thread)
                return &g_processes[p];
        }
    }
    return NULL;
}

void nt_process_release_thread_object(PVOID object)
{
    THREAD_OBJECT *thread = (THREAD_OBJECT *)object;
    if (thread == &g_primary_thread) return;
    if (!thread || thread->state != THREAD_TERMINATED ||
        handle_object_referenced(&g_handle_table, OBJ_TYPE_THREAD, object))
        return;

    PROCESS_OBJECT *proc = thread_owner(thread);
    nt_proc_memset(thread, 0, sizeof(*thread));
    if (proc && proc->thread_count)
        proc->thread_count--;
}

BOOL nt_process_thread_exit_code(PVOID object, DWORD *exit_code)
{
    THREAD_OBJECT *thread = (THREAD_OBJECT *)object;
    if (!thread || !exit_code || thread->state == THREAD_FREE)
        return FALSE;
    *exit_code = thread->state == THREAD_TERMINATED
               ? (DWORD)thread->exit_status : 259; /* STILL_ACTIVE */
    return TRUE;
}

/* ── NtCreateProcess ────────────────────────────────────────── */

NTSTATUS sys_NtCreateProcess(ULONG_PTR *args)
{
    PHANDLE             ProcessHandle    = (PHANDLE)args[0];
    ACCESS_MASK         DesiredAccess    = (ACCESS_MASK)args[1];
    /* POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)args[2]; */
    /* HANDLE           ParentProcess    = (HANDLE)args[3]; */
    /* BOOLEAN          InheritObjectTable = (BOOLEAN)args[4]; */
    /* HANDLE           SectionHandle    = (HANDLE)args[5]; */
    /* HANDLE           DebugPort        = (HANDLE)args[6]; */
    /* HANDLE           ExceptionPort    = (HANDLE)args[7]; */

    if (!ProcessHandle)
        return STATUS_INVALID_PARAMETER;

    PROCESS_OBJECT *proc = alloc_process();
    if (!proc)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Set up minimal PEB */
    proc->peb.BeingDebugged = 0;
    proc->peb.ProcessHeap = (PVOID)(ULONG_PTR)0xBEEF0001;

    /* Allocate handle in caller's table */
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_PROCESS,
                                   DesiredAccess, proc, ProcessHandle);
    if (!NT_SUCCESS(status)) {
        proc->active = FALSE;
        return status;
    }

    nt_proc_log_hex("NtCreateProcess: pid=", proc->pid);
    return STATUS_SUCCESS;
}

/* ── NtCreateThread ─────────────────────────────────────────── */

NTSTATUS sys_NtCreateThread(ULONG_PTR *args)
{
    PHANDLE         ThreadHandle     = (PHANDLE)args[0];
    ACCESS_MASK     DesiredAccess    = (ACCESS_MASK)args[1];
    /* POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)args[2]; */
    HANDLE          ProcessHandle    = (HANDLE)args[3];
    PCLIENT_ID      ClientId         = (PCLIENT_ID)args[4];
    /* PCONTEXT      ThreadContext    = (PCONTEXT)args[5]; */
    PINITIAL_TEB    InitialTeb       = (PINITIAL_TEB)args[6];
    BOOL            CreateSuspended  = (BOOL)args[7];

    if (!ThreadHandle)
        return STATUS_INVALID_PARAMETER;

    /* Look up process */
    PROCESS_OBJECT *proc = NULL;

    if (ProcessHandle == NT_CURRENT_PROCESS) {
        proc = current_process();
    } else {
        NTSTATUS status = handle_lookup(&g_handle_table, ProcessHandle,
                                        OBJ_TYPE_PROCESS, (PVOID *)&proc);
        if (!NT_SUCCESS(status))
            return status;
    }

    if (!proc)
        return STATUS_INVALID_HANDLE;

    /* Allocate thread */
    THREAD_OBJECT *thread = alloc_thread(proc);
    if (!thread)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (InitialTeb) {
        uint64_t stack_base = (uint64_t)(ULONG_PTR)InitialTeb->StackBase;
        uint64_t stack_limit = (uint64_t)(ULONG_PTR)InitialTeb->StackLimit;
        uint64_t allocation = (uint64_t)(ULONG_PTR)InitialTeb->StackReserved;
        if (!stack_base || stack_limit >= stack_base ||
            allocation > stack_limit) {
            thread->state = THREAD_FREE;
            proc->thread_count--;
            return STATUS_INVALID_PARAMETER;
        }
        thread->stack_allocation = (PVOID)(ULONG_PTR)allocation;
        thread->stack_limit = (PVOID)(ULONG_PTR)stack_limit;
        thread->stack_base = (PVOID)(ULONG_PTR)stack_base;
        thread->stack_size = stack_base - allocation;
    }

    if (CreateSuspended) {
        thread->state = THREAD_SUSPENDED;
        thread->suspend_count = 1;
    }

    /* Set up TEB */
    thread->teb.Self = &thread->teb;
    thread->teb.ProcessEnvironmentBlock = &proc->peb;
    thread->teb.ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)proc->pid;
    thread->teb.ClientId.UniqueThread  = (HANDLE)(ULONG_PTR)thread->tid;
    thread->teb.StackBase  = thread->stack_base;
    thread->teb.StackLimit = thread->stack_limit;
    thread->teb.DeallocationStack = thread->stack_allocation;

    /* Return client ID */
    if (ClientId) {
        ClientId->UniqueProcess = (HANDLE)(ULONG_PTR)proc->pid;
        ClientId->UniqueThread  = (HANDLE)(ULONG_PTR)thread->tid;
    }

    /* Allocate handle */
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_THREAD,
                                   DesiredAccess, thread, ThreadHandle);
    if (!NT_SUCCESS(status)) {
        thread->state = THREAD_FREE;
        proc->thread_count--;
        return status;
    }

    nt_proc_log_hex("NtCreateThread: tid=", thread->tid);
    return STATUS_SUCCESS;
}

/* ── NtTerminateThread ──────────────────────────────────────── */

NTSTATUS sys_NtTerminateThread(ULONG_PTR *args)
{
    HANDLE   ThreadHandle = (HANDLE)args[0];
    NTSTATUS ExitStatus   = (NTSTATUS)args[1];

    if (ThreadHandle == NT_CURRENT_THREAD || ThreadHandle == NULL) {
        /* Terminate current thread — for now, same as terminate process */
        nt_proc_log_hex("NtTerminateThread(current): status=", (ULONGLONG)ExitStatus);
        proc_exit((int32_t)ExitStatus);
        return STATUS_SUCCESS;
    }

    THREAD_OBJECT *thread = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, ThreadHandle,
                                    OBJ_TYPE_THREAD, (PVOID *)&thread);
    if (!NT_SUCCESS(status))
        return status;

    thread->state = THREAD_TERMINATED;
    thread->exit_status = ExitStatus;
    ntsync_notify_object(OBJ_TYPE_THREAD, thread);

    nt_proc_log_hex("NtTerminateThread: tid=", thread->tid);
    return STATUS_SUCCESS;
}

/* ── NtResumeThread ─────────────────────────────────────────── */

NTSTATUS sys_NtResumeThread(ULONG_PTR *args)
{
    HANDLE  ThreadHandle    = (HANDLE)args[0];
    ULONG  *PreviousSuspendCount = (ULONG *)args[1];

    THREAD_OBJECT *thread = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, ThreadHandle,
                                    OBJ_TYPE_THREAD, (PVOID *)&thread);
    if (!NT_SUCCESS(status))
        return status;

    ULONG prev;
    for (;;) {
        prev = __atomic_load_n(&thread->suspend_count, __ATOMIC_ACQUIRE);
        if (!prev)
            break;

        ULONG expected = prev;
        if (__atomic_compare_exchange_n(&thread->suspend_count, &expected,
                                        prev - 1, FALSE,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            break;
    }
    if (PreviousSuspendCount)
        *PreviousSuspendCount = prev;

    if (prev == 1) {
        if (thread->state == THREAD_SUSPENDED)
            thread->state = THREAD_READY;
        win32_resume_thread_execution(thread);
    }

    return STATUS_SUCCESS;
}

/* ── NtWaitForSingleObject ──────────────────────────────────── */

extern NTSTATUS sys_NtWaitForMultipleObjects(ULONG_PTR *args);

NTSTATUS sys_NtWaitForSingleObject(ULONG_PTR *args)
{
    HANDLE          Handle       = (HANDLE)args[0];
    BOOL            Alertable    = (BOOL)args[1];
    PLARGE_INTEGER  Timeout      = (PLARGE_INTEGER)args[2];

    ULONG_PTR wait_args[5] = {
        1,
        (ULONG_PTR)&Handle,
        1, /* WaitAny */
        (ULONG_PTR)Alertable,
        (ULONG_PTR)Timeout
    };
    return sys_NtWaitForMultipleObjects(wait_args);
}

/* ── NtQueryInformationProcess ──────────────────────────────── */

NTSTATUS sys_NtQueryInformationProcess(ULONG_PTR *args)
{
    HANDLE              ProcessHandle  = (HANDLE)args[0];
    PROCESSINFOCLASS    InfoClass      = (PROCESSINFOCLASS)args[1];
    PVOID               ProcessInfo    = (PVOID)args[2];
    ULONG               ProcessInfoLen = (ULONG)args[3];
    ULONG              *ReturnLength   = (ULONG *)args[4];

    PROCESS_OBJECT *proc = NULL;

    if (ProcessHandle == NT_CURRENT_PROCESS) {
        proc = current_process();
    } else {
        NTSTATUS status = handle_lookup(&g_handle_table, ProcessHandle,
                                        OBJ_TYPE_PROCESS, (PVOID *)&proc);
        if (!NT_SUCCESS(status))
            return status;
    }

    switch (InfoClass) {
    case ProcessBasicInformation: {
        if (ProcessInfoLen < sizeof(PROCESS_BASIC_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;

        PROCESS_BASIC_INFORMATION *pbi = (PROCESS_BASIC_INFORMATION *)ProcessInfo;
        pbi->ExitStatus = proc ? proc->exit_status : STATUS_SUCCESS;
        pbi->PebBaseAddress = proc ? &proc->peb : NULL;
        pbi->AffinityMask = 1;
        pbi->BasePriority = 8;
        pbi->UniqueProcessId = proc ? proc->pid : 1;
        pbi->InheritedFromUniqueProcessId = 0;

        if (ReturnLength)
            *ReturnLength = sizeof(PROCESS_BASIC_INFORMATION);

        return STATUS_SUCCESS;
    }

    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ── SSDT registration helper ───────────────────────────────── */
/*
 * Call from nt_syscall_init() to register process/thread handlers.
 * We provide this as a separate init function to keep ntsyscall.c clean.
 */

static void nt_process_test_expect(BOOL condition, const char *name,
                                   int *checks, int *failures)
{
    (*checks)++;
    serial_puts(condition ? "[PROCTEST] PASS " : "[PROCTEST] FAIL ");
    serial_puts(name);
    serial_puts("\n");
    if (!condition) (*failures)++;
}

int nt_process_inspection_selftest(void)
{
    extern NTSTATUS NTAPI NtQueryInformationProcess(
        HANDLE process, PROCESSINFOCLASS info_class, PVOID info,
        ULONG info_len, ULONG *return_len);

    int checks = 0;
    int failures = 0;
    HANDLE process_a = NULL, thread_a = NULL;
    HANDLE process_b = NULL, thread_b = NULL;
    PVOID object_a = NULL, thread_object_a = NULL;
    PVOID object_b = NULL, thread_object_b = NULL;
    ULONG pid_a = 0, tid_a = 0, pid_b = 0, tid_b = 0;
    PROCESS_BASIC_INFORMATION info_a = {0};
    PROCESS_BASIC_INFORMATION info_b = {0};
    PROCESS_BASIC_INFORMATION info_a_again = {0};
    RTL_USER_PROCESS_PARAMETERS parameters_a = {0};
    RTL_USER_PROCESS_PARAMETERS parameters_b = {0};
    PEB peb_a = {0};
    PEB peb_b = {0};
    ULONG returned = 0;
    const ULONG_PTR image_a = 0x0000000140000000ULL;
    const ULONG_PTR image_b = 0x0000000180000000ULL;

    serial_puts("[PROCTEST] starting process identity contract test\n");
    NTSTATUS status_a = nt_process_create_child(
        &process_a, &thread_a, &pid_a, &tid_a, &object_a, &thread_object_a,
        FALSE);
    NTSTATUS status_b = nt_process_create_child(
        &process_b, &thread_b, &pid_b, &tid_b, &object_b, &thread_object_b,
        FALSE);
    nt_process_test_expect(NT_SUCCESS(status_a) && NT_SUCCESS(status_b),
                           "create independent process objects",
                           &checks, &failures);
    if (!NT_SUCCESS(status_a) || !NT_SUCCESS(status_b)) goto cleanup;

    parameters_a.Flags = RTL_USER_PROC_PARAMS_NORMALIZED;
    parameters_b.Flags = RTL_USER_PROC_PARAMS_NORMALIZED;
    peb_a.ImageBaseAddress = (PVOID)image_a;
    peb_a.ProcessParameters = &parameters_a;
    peb_a.ProcessHeap = (PVOID)(ULONG_PTR)0xBEEF0001;
    peb_a.NumberOfProcessors = 4;
    peb_b.ImageBaseAddress = (PVOID)image_b;
    peb_b.ProcessParameters = &parameters_b;
    peb_b.ProcessHeap = (PVOID)(ULONG_PTR)0xBEEF0001;
    peb_b.NumberOfProcessors = 8;
    nt_process_test_expect(
        nt_process_set_peb(object_a, &peb_a) &&
        nt_process_set_peb(object_b, &peb_b),
        "publish complete per-process PEB state", &checks, &failures);

    status_a = NtQueryInformationProcess(
        process_a, ProcessBasicInformation, &info_a, sizeof(info_a),
        &returned);
    status_b = NtQueryInformationProcess(
        process_b, ProcessBasicInformation, &info_b, sizeof(info_b),
        &returned);
    nt_process_test_expect(
        NT_SUCCESS(status_a) && NT_SUCCESS(status_b) &&
        info_a.UniqueProcessId == pid_a && info_b.UniqueProcessId == pid_b,
        "query target process identities", &checks, &failures);

    nt_process_test_expect(
        info_a.PebBaseAddress && info_b.PebBaseAddress &&
        info_a.PebBaseAddress != info_b.PebBaseAddress,
        "allocate stable PEB per PID", &checks, &failures);
    if (info_a.PebBaseAddress && info_b.PebBaseAddress) {
        PPEB inspected_a = (PPEB)info_a.PebBaseAddress;
        PPEB inspected_b = (PPEB)info_b.PebBaseAddress;
        ULONGLONG observed_a = *(ULONGLONG *)(
            (BYTE *)info_a.PebBaseAddress + 16);
        ULONGLONG observed_b = *(ULONGLONG *)(
            (BYTE *)info_b.PebBaseAddress + 16);
        nt_process_test_expect(observed_a == image_a && observed_b == image_b,
                               "preserve target image bases",
                               &checks, &failures);
        nt_process_test_expect(
            inspected_a->ProcessParameters == &parameters_a &&
            inspected_b->ProcessParameters == &parameters_b &&
            inspected_a->ProcessParameters->Flags ==
                RTL_USER_PROC_PARAMS_NORMALIZED &&
            inspected_b->ProcessParameters->Flags ==
                RTL_USER_PROC_PARAMS_NORMALIZED,
            "preserve target process parameters", &checks, &failures);
        nt_process_test_expect(
            inspected_a->NumberOfProcessors == 4 &&
            inspected_b->NumberOfProcessors == 8 &&
            inspected_a->GdiSharedHandleTable &&
            inspected_b->GdiSharedHandleTable,
            "preserve extended PEB fields", &checks, &failures);
    }

    status_a = NtQueryInformationProcess(
        process_a, ProcessBasicInformation, &info_a_again,
        sizeof(info_a_again), &returned);
    nt_process_test_expect(
        NT_SUCCESS(status_a) &&
        info_a_again.PebBaseAddress == info_a.PebBaseAddress &&
        *(ULONGLONG *)((BYTE *)info_a_again.PebBaseAddress + 16) == image_a &&
        ((PPEB)info_a_again.PebBaseAddress)->ProcessParameters == &parameters_a,
        "requery does not inherit another process state",
        &checks, &failures);

cleanup:
    if (object_a)
        nt_process_complete_child(object_a, thread_object_a, STATUS_SUCCESS);
    if (object_b)
        nt_process_complete_child(object_b, thread_object_b, STATUS_SUCCESS);
    if (thread_a) (void)handle_close(&g_handle_table, thread_a);
    if (process_a) (void)handle_close(&g_handle_table, process_a);
    if (thread_b) (void)handle_close(&g_handle_table, thread_b);
    if (process_b) (void)handle_close(&g_handle_table, process_b);
    serial_puts("[PROCTEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

void nt_process_register_syscalls(NT_SERVICE_TABLE *table)
{
    table->handlers[NTSYS_CreateProcess]  = sys_NtCreateProcess;
    table->arg_counts[NTSYS_CreateProcess] = 8;

    table->handlers[NTSYS_CreateThread]   = sys_NtCreateThread;
    table->arg_counts[NTSYS_CreateThread] = 8;

    table->handlers[NTSYS_TerminateThread] = sys_NtTerminateThread;
    table->arg_counts[NTSYS_TerminateThread] = 2;

    table->handlers[NTSYS_ResumeThread]   = sys_NtResumeThread;
    table->arg_counts[NTSYS_ResumeThread] = 2;

    table->handlers[NTSYS_WaitForSingleObject] = sys_NtWaitForSingleObject;
    table->arg_counts[NTSYS_WaitForSingleObject] = 3;

    table->handlers[NTSYS_QueryInformationProcess] = sys_NtQueryInformationProcess;
    table->arg_counts[NTSYS_QueryInformationProcess] = 5;
}

/* Helper for kernel32 shim: mark a thread object as terminated. */
void win32_mark_thread_terminated(PVOID object, NTSTATUS exit_status)
{
    if (object) {
        THREAD_OBJECT *th = (THREAD_OBJECT *)object;
        th->state = THREAD_TERMINATED;
        th->exit_status = exit_status;
        ntsync_notify_object(OBJ_TYPE_THREAD, th);
        nt_process_release_thread_object(object);
    }
}
