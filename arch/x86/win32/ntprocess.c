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
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);
extern void  proc_exit(int32_t code);
extern uint64_t idt_get_ticks(void);

/* ── Thread state ───────────────────────────────────────────── */

#define MAX_THREADS     16
#define THREAD_STACK_SIZE  (256 * 1024)  /* 256KB per thread */

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
    PVOID           stack_base;     /* allocated stack */
    SIZE_T          stack_size;
    PVOID           start_address;  /* entry point */
    PVOID           parameter;      /* arg to entry */
    ULONG           suspend_count;
    TEB             teb;            /* per-thread TEB */
    /* Saved register context (for context switch) */
    uint64_t        saved_rsp;
    uint64_t        saved_rip;
} THREAD_OBJECT, *PTHREAD_OBJECT;

/* ── Process state ──────────────────────────────────────────── */

#define MAX_NT_PROCESSES  8

typedef struct _PROCESS_OBJECT {
    ULONG           pid;
    BOOL            active;
    NTSTATUS        exit_status;
    HANDLE_TABLE    handles;
    PEB             peb;
    PE_IMAGE_INFO   image;
    THREAD_OBJECT   threads[MAX_THREADS];
    ULONG           thread_count;
    ULONG           next_tid;
} PROCESS_OBJECT, *PPROCESS_OBJECT;

static PROCESS_OBJECT g_processes[MAX_NT_PROCESSES];
static PROCESS_OBJECT *g_current_process = NULL;
static ULONG g_next_pid = 1;

/* ── External handle table (from ntsyscall.c) ───────────────── */
extern HANDLE_TABLE g_handle_table;

/* ── Helpers ────────────────────────────────────────────────── */

static void nt_proc_log(const char *msg)
{
    serial_puts("[NT-PROC] ");
    serial_puts(msg);
    serial_puts("\n");
}

static void nt_proc_log_hex(const char *prefix, ULONGLONG val)
{
    serial_puts("[NT-PROC] ");
    serial_puts(prefix);
    serial_puthex(val, 16);
    serial_puts("\n");
}

static inline void nt_proc_memset(void *s, int c, SIZE_T n)
{
    BYTE *p = (BYTE *)s;
    while (n--) *p++ = (BYTE)c;
}

/* ── Process allocation ─────────────────────────────────────── */

static PROCESS_OBJECT *alloc_process(void)
{
    for (int i = 0; i < MAX_NT_PROCESSES; i++) {
        if (!g_processes[i].active) {
            PROCESS_OBJECT *proc = &g_processes[i];
            nt_proc_memset(proc, 0, sizeof(PROCESS_OBJECT));
            proc->pid = g_next_pid++;
            proc->active = TRUE;
            proc->next_tid = 1;
            handle_table_init(&proc->handles);
            return proc;
        }
    }
    return NULL;
}

/* ── Thread allocation ──────────────────────────────────────── */

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
    /* PINITIAL_TEB  InitialTeb       = (PINITIAL_TEB)args[6]; */
    BOOL            CreateSuspended  = (BOOL)args[7];

    if (!ThreadHandle)
        return STATUS_INVALID_PARAMETER;

    /* Look up process */
    PROCESS_OBJECT *proc = NULL;

    if (ProcessHandle == NT_CURRENT_PROCESS) {
        proc = g_current_process;
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

    /* Allocate stack */
    uint64_t stack_pages = THREAD_STACK_SIZE / 4096;
    thread->stack_base = mem_alloc_pages(stack_pages);
    if (!thread->stack_base) {
        thread->state = THREAD_FREE;
        proc->thread_count--;
        return STATUS_NO_MEMORY;
    }
    thread->stack_size = THREAD_STACK_SIZE;

    if (CreateSuspended)
        thread->state = THREAD_SUSPENDED;

    /* Set up TEB */
    thread->teb.Self = &thread->teb;
    thread->teb.ProcessEnvironmentBlock = &proc->peb;
    thread->teb.ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)proc->pid;
    thread->teb.ClientId.UniqueThread  = (HANDLE)(ULONG_PTR)thread->tid;
    thread->teb.StackBase  = (BYTE *)thread->stack_base + thread->stack_size;
    thread->teb.StackLimit = thread->stack_base;

    /* Return client ID */
    if (ClientId) {
        ClientId->UniqueProcess = (HANDLE)(ULONG_PTR)proc->pid;
        ClientId->UniqueThread  = (HANDLE)(ULONG_PTR)thread->tid;
    }

    /* Allocate handle */
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_THREAD,
                                   DesiredAccess, thread, ThreadHandle);
    if (!NT_SUCCESS(status)) {
        mem_free_pages(thread->stack_base, stack_pages);
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

    ULONG prev = thread->suspend_count;
    if (PreviousSuspendCount)
        *PreviousSuspendCount = prev;

    if (thread->suspend_count > 0) {
        thread->suspend_count--;
        if (thread->suspend_count == 0 && thread->state == THREAD_SUSPENDED)
            thread->state = THREAD_READY;
    }

    return STATUS_SUCCESS;
}

/* ── NtWaitForSingleObject ──────────────────────────────────── */

NTSTATUS sys_NtWaitForSingleObject(ULONG_PTR *args)
{
    HANDLE          Handle       = (HANDLE)args[0];
    /* BOOLEAN      Alertable    = (BOOLEAN)args[1]; */
    PLARGE_INTEGER  Timeout      = (PLARGE_INTEGER)args[2];

    HANDLE_ENTRY *entry = handle_get_entry(&g_handle_table, Handle);
    if (!entry)
        return STATUS_INVALID_HANDLE;

    /* Determine what we're waiting on */
    switch (entry->type) {
    case OBJ_TYPE_PROCESS: {
        PROCESS_OBJECT *proc = (PROCESS_OBJECT *)entry->object;
        /* Poll until process exits */
        uint64_t start = idt_get_ticks();
        uint64_t timeout_ticks = 0;
        if (Timeout) {
            LONGLONG t = Timeout->QuadPart;
            if (t < 0) t = -t;
            timeout_ticks = (uint64_t)(t / 100000);
        }

        while (proc->active) {
            __asm__ volatile("sti; hlt; cli");
            if (Timeout && timeout_ticks > 0 &&
                (idt_get_ticks() - start) >= timeout_ticks) {
                return STATUS_TIMEOUT;
            }
        }
        return STATUS_SUCCESS;
    }

    case OBJ_TYPE_THREAD: {
        THREAD_OBJECT *thread = (THREAD_OBJECT *)entry->object;
        uint64_t start = idt_get_ticks();
        uint64_t timeout_ticks = 0;
        if (Timeout) {
            LONGLONG t = Timeout->QuadPart;
            if (t < 0) t = -t;
            timeout_ticks = (uint64_t)(t / 100000);
        }

        while (thread->state != THREAD_TERMINATED) {
            __asm__ volatile("sti; hlt; cli");
            if (Timeout && timeout_ticks > 0 &&
                (idt_get_ticks() - start) >= timeout_ticks) {
                return STATUS_TIMEOUT;
            }
        }
        return STATUS_SUCCESS;
    }

    case OBJ_TYPE_EVENT: {
        /* Events: poll the signaled flag */
        /* TODO: implement when events are added */
        return STATUS_NOT_IMPLEMENTED;
    }

    default:
        return STATUS_NOT_IMPLEMENTED;
    }
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
        proc = g_current_process;
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
