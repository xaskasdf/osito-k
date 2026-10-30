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
#include "ntprocess.h"
#include "ntjob.h"
#include "advapi32_shim.h"
#include "compat32.h"
#include "../include/proc_cycles.h"
#include "../drivers/intel_gpu_memory.h"
#include "../drivers/intel_gpu_kmt.h"
#include "../drivers/intel_gpu_cpu_view.h"
#include "../drivers/intel_gpu_service.h"
#include "../drivers/intel_gpu_capture.h"
#include "../drivers/intel_gpu_jobs.h"
#include "../drivers/intel_gpu_executor.h"
#include "../drivers/intel_gfx_capture.h"
#include "d3dkmt_cpu.h"
#include "d3dkmt.h"
#include "../drivers/intel_gpu_umd_abi.h"
#include "../include/paging.h"
#include "../kernel/smp_work.h"

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
    uint32_t        cpu_kernel_pid;
    uint64_t        cpu_cycle_base;
    uint64_t        cpu_cycles;
    BOOL            fs_redirection_disabled;
    ULONG           sync_io_state; /* 0 idle, 1 pending, 2 cancel requested */
    BOOL            primary_parked; /* signaled, but teardown still owns slot */
} THREAD_OBJECT, *PTHREAD_OBJECT;

/* ── Process state ──────────────────────────────────────────── */

#define MAX_NT_PROCESSES  32
#define NT_PROCESS_IMAGE_PATH_CAP 320

typedef struct _PROCESS_OBJECT {
    ULONG           pid;
    BOOL            active;
    BOOL            vm_closing;
    uint64_t        gpu_epoch;
    NTSTATUS        exit_status;
    HANDLE_TABLE    handles;
    PEB             peb;
    PE_IMAGE_INFO   image;
    char            image_path[NT_PROCESS_IMAGE_PATH_CAP];
    THREAD_OBJECT   threads[MAX_THREADS];
    ULONG           thread_count;
    uint64_t        retired_cpu_cycles;
    PVOID           primary_token;
} PROCESS_OBJECT, *PPROCESS_OBJECT;

static PROCESS_OBJECT g_processes[MAX_NT_PROCESSES];
static PROCESS_OBJECT *g_current_process = NULL;
static ULONG g_next_pid = 1;
static uint64_t g_next_gpu_epoch = 1;
/* TID 1 belongs to the legacy shell-launched main thread. Worker and child
 * primary IDs share this namespace; never recycle one while a handle lives. */
static ULONG g_next_tid = 2;
/* ponytail: one live primary-thread object until processes own native main threads. */
static THREAD_OBJECT g_primary_thread;
static void nt_thread_stop_cpu_accounting(THREAD_OBJECT *thread);
static uint64_t nt_cpu_lock(void);
static void nt_cpu_unlock(uint64_t flags);

static void nt_thread_clear_token(THREAD_OBJECT *thread)
{
    if (thread)
        advapi32_token_release(__atomic_exchange_n(&thread->impersonation_token,
                                                   NULL, __ATOMIC_ACQ_REL));
}

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
    uint64_t flags = nt_cpu_lock();
    PVOID token = thread && thread->state != THREAD_FREE
        ? __atomic_load_n(&thread->impersonation_token, __ATOMIC_ACQUIRE) : NULL;
    advapi32_token_retain(token);
    nt_cpu_unlock(flags);
    return token;
}

BOOL nt_thread_set_impersonation_token(PVOID thread_object,
                                       PVOID token_object)
{
    THREAD_OBJECT *thread = (THREAD_OBJECT *)thread_object;
    uint64_t flags = nt_cpu_lock();
    if (!thread || thread->state == THREAD_FREE || thread->state == THREAD_TERMINATED) {
        nt_cpu_unlock(flags);
        return FALSE;
    }
    advapi32_token_retain(token_object);
    PVOID previous = __atomic_exchange_n(&thread->impersonation_token, token_object,
                                          __ATOMIC_ACQ_REL);
    nt_cpu_unlock(flags);
    advapi32_token_release(previous);
    return TRUE;
}

/* ── External handle table (from ntsyscall.c) ───────────────── */
extern HANDLE_TABLE g_handle_table;
extern void ntsync_notify_object(OBJECT_TYPE_ID type, PVOID object);
extern DWORD win32_current_process_id(void);
extern PVOID win32_current_thread_object(void);
extern int g_compat32_mode;
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
        if (proc->threads[i].tid &&
            handle_object_referenced(&g_handle_table, OBJ_TYPE_THREAD,
                                     &proc->threads[i]))
            return TRUE;
    return FALSE;
}

/* ── Process allocation ─────────────────────────────────────── */

static PROCESS_OBJECT *alloc_process(PCVOID primary_source)
{
    PVOID primary_token = advapi32_token_create_primary(primary_source);
    if (!primary_token) return NULL;
    for (int i = 0; i < MAX_NT_PROCESSES; i++) {
        if (!g_processes[i].active &&
            !process_object_referenced(&g_processes[i])) {
            PROCESS_OBJECT *proc = &g_processes[i];
            if (proc->pid)
                ntdll_release_process_inspection(proc->pid);
            advapi32_token_release(proc->primary_token);
            nt_proc_memset(proc, 0, sizeof(PROCESS_OBJECT));
            proc->primary_token = primary_token;
            proc->pid = g_next_pid++;
            if (!nt_job_process_create(proc->pid)) {
                advapi32_token_release(proc->primary_token);
                proc->primary_token = NULL;
                return NULL;
            }
            proc->active = TRUE;
            handle_table_init(&proc->handles);
            return proc;
        }
    }
    advapi32_token_release(primary_token);
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
        g_current_process = alloc_process(NULL);
        if (g_current_process)
            g_current_process->peb.ProcessHeap = (PVOID)(ULONG_PTR)0xBEEF0001;
    }
    return g_current_process;
}

static uint64_t nt_cpu_lock(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void nt_cpu_unlock(uint64_t flags)
{
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

static void nt_process_clear_primary_token(PROCESS_OBJECT *proc)
{
    if (!proc) return;
    uint64_t flags = nt_cpu_lock();
    PVOID previous = proc->primary_token;
    proc->primary_token = NULL;
    nt_cpu_unlock(flags);
    advapi32_token_release(previous);
}

static NTSTATUS nt_process_reference_token(HANDLE handle, ACCESS_MASK required, PVOID *token)
{
    if (!token) return STATUS_INVALID_PARAMETER;
    *token = NULL;
    PROCESS_OBJECT *proc = NULL;
    uint64_t flags;
    if (handle == NT_CURRENT_PROCESS ||
        (g_compat32_mode && (ULONG_PTR)handle == 0xffffffffU)) {
        proc = current_process();
        flags = nt_cpu_lock();
    } else {
        /* Win32 process slots are BSP-owned. Pin before another thread can
         * close the process handle and recycle its slot. */
        flags = nt_cpu_lock();
        ACCESS_MASK access;
        NTSTATUS status = handle_lookup_access_for_process(&g_handle_table, handle,
            win32_current_process_id(), OBJ_TYPE_PROCESS, (PVOID *)&proc, &access);
        if (!NT_SUCCESS(status)) { nt_cpu_unlock(flags); return status; }
        if (!(access & (required | GENERIC_ALL))) {
            nt_cpu_unlock(flags);
            return STATUS_ACCESS_DENIED;
        }
    }
    if (!proc) { nt_cpu_unlock(flags); return STATUS_INVALID_HANDLE; }
    *token = proc->primary_token;
    advapi32_token_retain(*token);
    nt_cpu_unlock(flags);
    return *token ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

NTSTATUS nt_process_reference_primary_token(HANDLE handle, PVOID *token)
{
    return nt_process_reference_token(handle, 0x0400U | 0x1000U, token);
}

NTSTATUS nt_thread_reference_impersonation_token(HANDLE handle, PVOID *token)
{
    if (!token) return STATUS_INVALID_PARAMETER;
    *token = NULL;
    THREAD_OBJECT *thread = NULL;
    uint64_t flags = nt_cpu_lock();
    NTSTATUS status = STATUS_SUCCESS;
    if (handle == NT_CURRENT_THREAD ||
        (g_compat32_mode && (ULONG_PTR)handle == 0xfffffffeU)) {
        thread = win32_current_thread_object();
    } else {
        ACCESS_MASK access = 0;
        status = handle_lookup_access_for_process(&g_handle_table, handle,
            win32_current_process_id(), OBJ_TYPE_THREAD, (PVOID *)&thread, &access);
        if (NT_SUCCESS(status) && !(access & (0x0040U | 0x0800U | GENERIC_ALL)))
            status = STATUS_ACCESS_DENIED;
    }
    if (NT_SUCCESS(status)) {
        if (!thread || thread->state == THREAD_FREE) status = STATUS_INVALID_HANDLE;
        else {
            /* Pin while the source handle and thread slot are still protected. */
            *token = thread->impersonation_token;
            advapi32_token_retain(*token);
            if (!*token) status = STATUS_NO_TOKEN;
        }
    }
    nt_cpu_unlock(flags);
    return status;
}

NTSTATUS nt_thread_assign_token_handle(HANDLE handle, HANDLE token_handle)
{
    ULONG_PTR value = (ULONG_PTR)handle;
    if (g_compat32_mode && value == 0xfffffffeU) handle = NT_CURRENT_THREAD;
    if (handle == NT_CURRENT_PROCESS ||
        (g_compat32_mode && value == 0xffffffffU)) return STATUS_OBJECT_TYPE_MISMATCH;
    if (handle != NT_CURRENT_THREAD && value > 0xffffffffU) return STATUS_INVALID_HANDLE;

    THREAD_OBJECT *thread = NULL;
    PVOID token = NULL, previous = NULL;
    uint64_t flags = nt_cpu_lock();
    NTSTATUS status = STATUS_SUCCESS;
    if (handle == NT_CURRENT_THREAD) thread = win32_current_thread_object();
    else {
        ACCESS_MASK access = 0;
        status = handle_lookup_access_for_process(&g_handle_table, handle,
            win32_current_process_id(), OBJ_TYPE_THREAD, (PVOID *)&thread, &access);
        if (NT_SUCCESS(status) && !(access & (0x0080U | GENERIC_ALL)))
            status = STATUS_ACCESS_DENIED; /* THREAD_SET_THREAD_TOKEN */
    }
    if (NT_SUCCESS(status) && (!thread || thread->state == THREAD_FREE))
        status = STATUS_INVALID_HANDLE;
    if (NT_SUCCESS(status) && token_handle)
        status = advapi32_reference_impersonation_token(token_handle, &token);
    if (NT_SUCCESS(status)) {
        /* Transfer the pinned token to the thread. Even a terminated thread
         * remains an assignable object while an open handle keeps it alive. */
        previous = __atomic_exchange_n(&thread->impersonation_token, token,
                                         __ATOMIC_ACQ_REL);
    }
    nt_cpu_unlock(flags);
    advapi32_token_release(previous);
    return status;
}

static NTSTATUS nt_process_resolve_pid(HANDLE handle, ACCESS_MASK required,
                                       ULONG *pid, BOOL memory_access)
{
    PROCESS_OBJECT *proc = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    uint64_t flags = nt_cpu_lock();
    if (handle == NT_CURRENT_PROCESS || (DWORD)(ULONG_PTR)handle == 0xffffffffU) {
        proc = current_process();
    } else {
        ACCESS_MASK access = 0;
        status = handle_lookup_access_for_process(&g_handle_table, handle,
            win32_current_process_id(), OBJ_TYPE_PROCESS, (PVOID *)&proc, &access);
        if (NT_SUCCESS(status) && !(access & GENERIC_ALL) &&
            (access & required) != required) status = STATUS_ACCESS_DENIED;
    }
    if (NT_SUCCESS(status)) {
        if (!proc) status = STATUS_INVALID_HANDLE;
        else if (memory_access && (!proc->active || proc->vm_closing))
            status = STATUS_PROCESS_IS_TERMINATING;
        else *pid = proc->pid;
    }
    nt_cpu_unlock(flags);
    return status;
}

NTSTATUS nt_process_resolve_job_pid(HANDLE handle, ACCESS_MASK required, ULONG *pid)
{
    return nt_process_resolve_pid(handle, required, pid, FALSE);
}

NTSTATUS nt_process_resolve_vm_pid(HANDLE handle, ACCESS_MASK required, ULONG *pid)
{
    return nt_process_resolve_pid(handle, required, pid, TRUE);
}

void nt_process_begin_vm_teardown(ULONG pid)
{
    uint64_t flags = nt_cpu_lock();
    for (int i = 0; i < MAX_NT_PROCESSES; i++)
        if (g_processes[i].pid == pid) g_processes[i].vm_closing = TRUE;
    nt_cpu_unlock(flags);
}

static NTSTATUS nt_process_gpu_device_admit(ULONG pid, struct intel_gpu_memory *memory,
                                           ULONG *device, BOOL retained)
{
    if (!pid || !memory || !device) return STATUS_INVALID_PARAMETER;
    extern uint32_t smp_current_cpu(void);
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
    /* All scheduled NT tasks live on the BSP. Keep process validation and
     * device publication within the same exclusion as begin_vm_teardown.
     * The driver operation only changes metadata: no allocation/MMIO/wait. */
    uint64_t flags=nt_cpu_lock();
    PROCESS_OBJECT *proc=NULL;
    for (unsigned i=0;i<MAX_NT_PROCESSES;i++)
        if (g_processes[i].pid==pid) { proc=&g_processes[i]; break; }
    NTSTATUS status=STATUS_INVALID_CID;
    if (proc) {
        status=STATUS_PROCESS_IS_TERMINATING;
        if (proc->active && !proc->vm_closing) {
            if (!proc->gpu_epoch && g_next_gpu_epoch!=UINT64_MAX)
                proc->gpu_epoch=g_next_gpu_epoch++;
            enum intel_gpu_buffer_status result=proc->gpu_epoch ?
                intel_gpu_memory_open_device_epoch(memory,pid,proc->gpu_epoch,device,retained) :
                INTEL_GPU_BUFFER_NO_MEMORY;
            status=result==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS :
                result==INTEL_GPU_BUFFER_NO_MEMORY ? STATUS_INSUFFICIENT_RESOURCES : STATUS_INVALID_PARAMETER;
        }
    }
    nt_cpu_unlock(flags);
    return status;
}

NTSTATUS nt_process_open_gpu_device(ULONG pid, struct intel_gpu_memory *memory, ULONG *device)
{ return nt_process_gpu_device_admit(pid,memory,device,FALSE); }

NTSTATUS nt_process_open_gpu_device_retained(ULONG pid, struct intel_gpu_memory *memory, ULONG *device)
{ return nt_process_gpu_device_admit(pid,memory,device,TRUE); }

void nt_process_release_process_object(PVOID object)
{
    PROCESS_OBJECT *proc = object;
    uint64_t flags = nt_cpu_lock();
    PVOID token = NULL;
    if (proc && !proc->active && !process_object_referenced(proc)) {
        token = proc->primary_token;
        proc->primary_token = NULL;
        nt_job_process_exit(proc->pid, proc->exit_status);
    }
    nt_cpu_unlock(flags);
    advapi32_token_release(token);
}

static uint64_t nt_thread_cpu_cycles(THREAD_OBJECT *thread)
{
    uint64_t total;
    if (thread->cpu_kernel_pid &&
        proc_query_cpu_cycles(thread->cpu_kernel_pid, &total) &&
        total >= thread->cpu_cycle_base)
        thread->cpu_cycles = total - thread->cpu_cycle_base;
    return thread->cpu_cycles;
}

void nt_thread_bind_cpu_accounting(PVOID object, uint32_t kernel_pid)
{
    THREAD_OBJECT *thread = (THREAD_OBJECT *)object;
    if (!thread || !kernel_pid) return;
    uint64_t flags = nt_cpu_lock();
    uint64_t base;
    if (!thread->cpu_kernel_pid && proc_query_cpu_cycles(kernel_pid, &base)) {
        thread->cpu_cycle_base = base;
        thread->cpu_cycles = 0;
        thread->cpu_kernel_pid = kernel_pid;
    }
    nt_cpu_unlock(flags);
}

static void nt_thread_stop_cpu_accounting(THREAD_OBJECT *thread)
{
    if (!thread) return;
    uint64_t flags = nt_cpu_lock();
    (void)nt_thread_cpu_cycles(thread);
    thread->cpu_kernel_pid = 0;
    nt_cpu_unlock(flags);
}

void nt_process_begin_main_cpu_accounting(uint32_t kernel_pid)
{
    PROCESS_OBJECT *proc = current_process();
    if (!proc) return;
    (void)nt_job_process_create(proc->pid);
    /* Consecutive shell launches reuse the root NT object, not its identity. */
    nt_process_clear_primary_token(proc);
    proc->primary_token = advapi32_token_create_primary(NULL);
    uint64_t flags = nt_cpu_lock();
    /* winexec synchronously finishes the old root's VM/GDI exit notification
     * before this point. Deferred GPU collectors retain exact device/context
     * IDs; the next launch must not inherit the old admission identity. */
    proc->gpu_epoch = 0;
    proc->vm_closing = FALSE;
    (void)nt_process_primary_thread_object(1);
    /* The legacy primary NT process object spans consecutive shell launches.
     * Keep its total monotonic, but never charge the intervening shell idle. */
    nt_thread_stop_cpu_accounting(&g_primary_thread);
    proc->retired_cpu_cycles += g_primary_thread.cpu_cycles;
    g_primary_thread.cpu_cycles = 0;
    g_primary_thread.fs_redirection_disabled = FALSE;
    __atomic_store_n(&g_primary_thread.sync_io_state, 0, __ATOMIC_RELEASE);
    nt_thread_clear_token(&g_primary_thread);
    nt_thread_bind_cpu_accounting(&g_primary_thread, kernel_pid);
    nt_cpu_unlock(flags);
}

BOOL nt_thread_fs_redirection(int disabled)
{
    THREAD_OBJECT *thread = win32_current_thread_object();
    if (!thread && win32_current_process_id() == 1) thread = &g_primary_thread;
    if (!thread) return FALSE;
    BOOL previous = thread->fs_redirection_disabled;
    if (disabled >= 0) thread->fs_redirection_disabled = disabled != 0;
    return previous;
}

void nt_process_end_main_cpu_accounting(void)
{
    __atomic_store_n(&g_primary_thread.sync_io_state, 0, __ATOMIC_RELEASE);
    if (g_current_process) nt_job_process_exit(g_current_process->pid, g_current_process->exit_status);
    nt_thread_stop_cpu_accounting(&g_primary_thread);
    nt_thread_clear_token(&g_primary_thread);
    nt_process_clear_primary_token(g_current_process);
}

BOOL nt_process_set_main_admin(void)
{
    PROCESS_OBJECT *proc = current_process();
    if (!proc || proc != g_current_process || proc->pid != 1) return FALSE;
    PVOID token = advapi32_token_create_admin_primary();
    if (!token) return FALSE;
    uint64_t flags = nt_cpu_lock();
    PVOID previous = proc->primary_token;
    proc->primary_token = token;
    nt_cpu_unlock(flags);
    advapi32_token_release(previous);
    return TRUE;
}

NTSTATUS nt_process_query_cycles(HANDLE handle, ULONGLONG *cycles)
{
    PROCESS_OBJECT *proc = NULL;
    if (handle == NT_CURRENT_PROCESS) {
        proc = current_process();
    } else {
        ACCESS_MASK access;
        NTSTATUS status = handle_lookup_access_for_process(
            &g_handle_table, handle, win32_current_process_id(),
            OBJ_TYPE_PROCESS, (PVOID *)&proc, &access);
        if (!NT_SUCCESS(status)) return status;
        if (!(access & (0x0400U | 0x1000U | GENERIC_ALL)))
            return STATUS_ACCESS_DENIED;
    }
    if (!proc) return STATUS_INVALID_HANDLE;
    uint64_t flags = nt_cpu_lock();
    uint64_t total = proc->retired_cpu_cycles;
    for (unsigned i = 0; i < MAX_THREADS; i++)
        if (proc->threads[i].state != THREAD_FREE)
            total += nt_thread_cpu_cycles(&proc->threads[i]);
    if (proc->pid == 1)
        total += nt_thread_cpu_cycles(&g_primary_thread);
    nt_cpu_unlock(flags);
    *cycles = total;
    return STATUS_SUCCESS;
}

NTSTATUS nt_thread_query_cycles(HANDLE handle, ULONGLONG *cycles)
{
    THREAD_OBJECT *thread = NULL;
    if (handle == NT_CURRENT_THREAD) {
        extern PVOID win32_current_thread_object(void);
        thread = (THREAD_OBJECT *)win32_current_thread_object();
        if (!thread && win32_current_process_id() == 1)
            thread = &g_primary_thread;
    } else {
        ACCESS_MASK access;
        NTSTATUS status = handle_lookup_access_for_process(
            &g_handle_table, handle, win32_current_process_id(),
            OBJ_TYPE_THREAD, (PVOID *)&thread, &access);
        if (!NT_SUCCESS(status)) return status;
        if (!(access & (0x0040U | 0x0800U | GENERIC_ALL)))
            return STATUS_ACCESS_DENIED;
    }
    if (!thread) return STATUS_INVALID_HANDLE;
    uint64_t flags = nt_cpu_lock();
    uint64_t total = nt_thread_cpu_cycles(thread);
    nt_cpu_unlock(flags);
    *cycles = total;
    return STATUS_SUCCESS;
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

static PROCESS_OBJECT *nt_process_find_openable(ULONG pid)
{
    for (int i = 0; i < MAX_NT_PROCESSES; i++) {
        if (g_processes[i].pid == pid &&
            (g_processes[i].active ||
             process_object_referenced(&g_processes[i])))
            return &g_processes[i];
    }
    return NULL;
}

NTSTATUS nt_process_open_for_process(ULONG pid, ACCESS_MASK access,
                                     ULONG owner_pid, PHANDLE out_handle)
{
    if (!out_handle || !owner_pid) return STATUS_INVALID_PARAMETER;
    if (!current_process()) return STATUS_INSUFFICIENT_RESOURCES;
    uint64_t flags = nt_cpu_lock();
    PROCESS_OBJECT *proc = nt_process_find_openable(pid);
    /* Internal pseudo-handle duplication already determined the granted rights. */
    NTSTATUS status = proc ? handle_alloc_for_process(&g_handle_table,
        OBJ_TYPE_PROCESS, access, proc, owner_pid, out_handle)
        : STATUS_INVALID_PARAMETER;
    nt_cpu_unlock(flags);
    return status;
}

NTSTATUS nt_process_open_client(ULONG_PTR pid, ULONG_PTR tid, ACCESS_MASK access,
                                BOOL inherit, PHANDLE out_handle)
{
    if (!out_handle) return STATUS_INVALID_PARAMETER;
    if (!pid || pid > UINT32_MAX || tid > UINT32_MAX)
        return STATUS_INVALID_CID;
    if (!current_process()) return STATUS_INSUFFICIENT_RESOURCES;

    uint64_t flags = nt_cpu_lock();
    PROCESS_OBJECT *proc = nt_process_find_openable((ULONG)pid);
    NTSTATUS status = STATUS_INVALID_CID;
    if (!proc) goto done;
    if (tid) {
        THREAD_OBJECT *thread = NULL;
        if (pid == 1 && g_primary_thread.tid == tid &&
            g_primary_thread.state != THREAD_FREE)
            thread = &g_primary_thread;
        for (unsigned t = 0; !thread && t < MAX_THREADS; t++) {
            THREAD_OBJECT *candidate = &proc->threads[t];
            if (candidate->state != THREAD_FREE && candidate->tid == tid)
                thread = candidate;
        }
        if (!thread || (thread->state == THREAD_TERMINATED &&
            !handle_object_referenced(&g_handle_table, OBJ_TYPE_THREAD, thread)))
            goto done;
    }

    /* As with thread opens, map the process generic rights before publishing.
     * Object DACL enforcement is not provided by this compatibility model. */
    ACCESS_MASK granted = access & ~(GENERIC_READ | GENERIC_WRITE |
        GENERIC_EXECUTE | GENERIC_ALL | 0x02000000U);
    if (access & GENERIC_READ) granted |= 0x00020410U;
    if (access & GENERIC_WRITE) granted |= 0x00021BEAU;
    if (access & GENERIC_EXECUTE) granted |= 0x00121001U;
    if (access & (GENERIC_ALL | 0x02000000U)) granted |= 0x001FFFFFU;
    if (granted & 0x01000000U) {
        status = STATUS_PRIVILEGE_NOT_HELD;
        goto done;
    }
    if (!granted || (granted & ~0x001FFFFFU)) {
        status = STATUS_ACCESS_DENIED;
        goto done;
    }
    if (granted & 0x0400U) granted |= 0x1000U;
    if (granted & 0x0200U) granted |= 0x2000U;
    status = handle_alloc(&g_handle_table, OBJ_TYPE_PROCESS, granted,
                           proc, out_handle);
    if (NT_SUCCESS(status) && inherit)
        (void)handle_update_flags_for_process(&g_handle_table, *out_handle,
            win32_current_process_id(), HANDLE_USER_FLAG_INHERIT,
            HANDLE_USER_FLAG_INHERIT);
done:
    nt_cpu_unlock(flags);
    return status;
}

NTSTATUS nt_thread_open(ULONG_PTR pid, ULONG_PTR tid, ACCESS_MASK access,
                        BOOL inherit, PHANDLE out_handle)
{
    if (!out_handle) return STATUS_INVALID_PARAMETER;
    if (!tid || tid > UINT32_MAX || pid > UINT32_MAX)
        return STATUS_INVALID_CID;
    if (!current_process()) return STATUS_INSUFFICIENT_RESOURCES;

    uint64_t flags = nt_cpu_lock();
    THREAD_OBJECT *thread = NULL;
    if ((!pid || pid == 1) && g_primary_thread.tid == tid &&
        g_primary_thread.state != THREAD_FREE)
        thread = &g_primary_thread;
    for (unsigned p = 0; !thread && p < MAX_NT_PROCESSES; p++) {
        PROCESS_OBJECT *proc = &g_processes[p];
        if (pid && proc->pid != pid) continue;
        for (unsigned t = 0; t < MAX_THREADS; t++) {
            THREAD_OBJECT *candidate = &proc->threads[t];
            if (candidate->state != THREAD_FREE && candidate->tid == tid) {
                thread = candidate;
                break;
            }
        }
    }
    NTSTATUS status = STATUS_INVALID_CID;
    if (!thread) goto done;
    if (thread->state == THREAD_TERMINATED &&
        !handle_object_referenced(&g_handle_table, OBJ_TYPE_THREAD, thread))
        goto done;

    /* Map generic rights before publishing the handle. Object DACL checks
     * remain outside this compatibility model; this is not a security boundary. */
    ACCESS_MASK granted = access & ~(GENERIC_READ | GENERIC_WRITE |
        GENERIC_EXECUTE | GENERIC_ALL | 0x02000000U);
    if (access & GENERIC_READ) granted |= 0x00020048U;
    if (access & GENERIC_WRITE) granted |= 0x00020037U;
    if (access & GENERIC_EXECUTE) granted |= 0x00120000U;
    if (access & (GENERIC_ALL | 0x02000000U)) granted |= 0x001FFFFFU;
    if (granted & 0x01000000U) {
        status = STATUS_PRIVILEGE_NOT_HELD;
        goto done;
    }
    if (!granted || (granted & ~0x001FFFFFU)) {
        status = STATUS_ACCESS_DENIED;
        goto done;
    }
    if (granted & 0x0040U) granted |= 0x0800U;
    if (granted & 0x0020U) granted |= 0x0400U;
    if (granted & 0x0002U) granted |= 0x1000U;
    if (access & GENERIC_EXECUTE) granted |= 0x1800U;

    status = handle_alloc(&g_handle_table, OBJ_TYPE_THREAD, granted,
                           thread, out_handle);
    if (NT_SUCCESS(status) && inherit)
        (void)handle_update_flags_for_process(&g_handle_table, *out_handle,
            win32_current_process_id(), HANDLE_USER_FLAG_INHERIT,
            HANDLE_USER_FLAG_INHERIT);
done:
    nt_cpu_unlock(flags);
    return status;
}

ULONG nt_thread_live_owner(ULONG tid)
{
    if (!tid) return 0;
    uint64_t flags = nt_cpu_lock();
    ULONG owner = 0;
    if (g_primary_thread.tid == tid && g_primary_thread.state != THREAD_FREE &&
        g_primary_thread.state != THREAD_TERMINATED) owner = 1;
    for (unsigned p = 0; !owner && p < MAX_NT_PROCESSES; p++) {
        PROCESS_OBJECT *process = &g_processes[p];
        if (!process->active) continue;
        for (unsigned t = 0; t < MAX_THREADS; t++) {
            THREAD_OBJECT *thread = &process->threads[t];
            if (thread->tid == tid && thread->state != THREAD_FREE &&
                thread->state != THREAD_TERMINATED) { owner = process->pid; break; }
        }
    }
    nt_cpu_unlock(flags);
    return owner;
}

PVOID nt_thread_sync_io_begin(void)
{
    THREAD_OBJECT *thread = win32_current_thread_object();
    if (!thread) return NULL;
    ULONG idle = 0;
    if (!__atomic_compare_exchange_n(&thread->sync_io_state, &idle, 1, FALSE,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return NULL;
    return thread;
}

BOOL nt_thread_sync_io_cancelled(PVOID token)
{
    THREAD_OBJECT *thread = token;
    return thread && __atomic_load_n(&thread->sync_io_state,
                                      __ATOMIC_ACQUIRE) == 2;
}

void nt_thread_sync_io_end(PVOID token)
{
    THREAD_OBJECT *thread = token;
    if (thread)
        __atomic_store_n(&thread->sync_io_state, 0, __ATOMIC_RELEASE);
}

DWORD nt_thread_cancel_sync_io(HANDLE handle)
{
    THREAD_OBJECT *thread = NULL;
    ACCESS_MASK access = 0;
    DWORD error = 0;
    uint64_t flags = nt_cpu_lock();
    if (handle == NT_CURRENT_THREAD ||
        (ULONG_PTR)handle == (ULONG_PTR)0xfffffffeU) {
        thread = win32_current_thread_object();
    } else {
        NTSTATUS status = handle_lookup_access_for_process(&g_handle_table,
            handle, win32_current_process_id(), OBJ_TYPE_THREAD,
            (PVOID *)&thread, &access);
        if (!NT_SUCCESS(status)) error = 6; /* ERROR_INVALID_HANDLE */
        else if (!(access & (0x0001U | GENERIC_ALL)))
            error = 5; /* THREAD_TERMINATE / ERROR_ACCESS_DENIED */
    }
    if (!error && (!thread || thread->state == THREAD_FREE)) error = 6;
    if (!error) {
        error = 1168; /* ERROR_NOT_FOUND: never cancel a future request. */
        if (thread->state != THREAD_TERMINATED) {
            ULONG pending = __atomic_load_n(&thread->sync_io_state,
                                             __ATOMIC_ACQUIRE);
            while (pending) {
                if (__atomic_compare_exchange_n(&thread->sync_io_state,
                        &pending, 2, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    error = 0;
                    break;
                }
            }
        }
    }
    nt_cpu_unlock(flags);
    return error;
}

NTSTATUS nt_thread_query_identity(HANDLE handle, ULONG *tid, DWORD *exit_code)
{
    THREAD_OBJECT *thread;
    ACCESS_MASK access;
    uint64_t flags = nt_cpu_lock();
    NTSTATUS status = handle_lookup_access_for_process(&g_handle_table, handle,
        win32_current_process_id(), OBJ_TYPE_THREAD, (PVOID *)&thread, &access);
    if (NT_SUCCESS(status)) {
        if (!(access & (0x0840U | GENERIC_ALL))) {
            status = STATUS_ACCESS_DENIED;
        } else {
            if (tid) *tid = thread->tid;
            if (exit_code) *exit_code = thread->state == THREAD_TERMINATED
                ? (DWORD)thread->exit_status : 259;
        }
    }
    nt_cpu_unlock(flags);
    return status;
}

static THREAD_OBJECT *alloc_thread(PROCESS_OBJECT *proc)
{
    if (!proc || proc->thread_count >= MAX_THREADS)
        return NULL;

    uint64_t flags = nt_cpu_lock();
    if (!g_next_tid) {
        nt_cpu_unlock(flags);
        return NULL;
    }
    for (int i = 0; i < MAX_THREADS; i++) {
        if (proc->threads[i].state == THREAD_FREE) {
            THREAD_OBJECT *thread = &proc->threads[i];
            nt_proc_memset(thread, 0, sizeof(THREAD_OBJECT));
            thread->tid = g_next_tid++;
            thread->state = THREAD_READY;
            proc->thread_count++;
            nt_cpu_unlock(flags);
            return thread;
        }
    }
    nt_cpu_unlock(flags);
    return NULL;
}

NTSTATUS nt_process_create_child(PHANDLE process_handle,
                                 PHANDLE thread_handle,
                                 ULONG *process_id, ULONG *thread_id,
                                 PVOID *process_object, PVOID *thread_object,
                                 BOOL create_suspended, PCVOID primary_token)
{
    if (!process_handle || !thread_handle)
        return STATUS_INVALID_PARAMETER;

    PVOID parent_token = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    if (!primary_token) {
        status = nt_process_reference_primary_token(NT_CURRENT_PROCESS, &parent_token);
        if (!NT_SUCCESS(status)) return status;
        primary_token = parent_token;
    }
    PROCESS_OBJECT *proc = alloc_process(primary_token);
    advapi32_token_release(parent_token);
    if (!proc) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    proc->peb.ProcessHeap = (PVOID)(ULONG_PTR)0xBEEF0001;

    THREAD_OBJECT *thread = alloc_thread(proc);
    if (!thread) {
        proc->active = FALSE;
        nt_job_process_exit(proc->pid, proc->exit_status);
        nt_process_clear_primary_token(proc);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    thread->state = create_suspended ? THREAD_SUSPENDED : THREAD_RUNNING;
    thread->suspend_count = create_suspended ? 1 : 0;

    status = handle_alloc(&g_handle_table, OBJ_TYPE_PROCESS,
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
    nt_job_process_exit(proc->pid, proc->exit_status);
    nt_process_clear_primary_token(proc);
    return status;
}

void nt_process_complete_child(PVOID process_object, PVOID thread_object,
                               NTSTATUS exit_status)
{
    PROCESS_OBJECT *proc = (PROCESS_OBJECT *)process_object;
    THREAD_OBJECT *thread = (THREAD_OBJECT *)thread_object;
    if (thread) {
        thread->primary_parked = FALSE;
        nt_thread_stop_cpu_accounting(thread);
        thread->exit_status = exit_status;
        thread->state = THREAD_TERMINATED;
        nt_thread_clear_token(thread);
        ntsync_notify_object(OBJ_TYPE_THREAD, thread);
    }
    if (proc) {
        proc->exit_status = exit_status;
        proc->active = FALSE;
        nt_job_process_exit(proc->pid, exit_status);
        ntsync_notify_object(OBJ_TYPE_PROCESS, proc);
        nt_process_release_process_object(proc);
    }
}

NTSTATUS nt_process_query_identity(HANDLE handle, DWORD *pid, DWORD *exit_code)
{
    PROCESS_OBJECT *proc = NULL;
    ACCESS_MASK access;
    uint64_t flags = nt_cpu_lock();
    NTSTATUS status = handle_lookup_access_for_process(&g_handle_table, handle,
        win32_current_process_id(), OBJ_TYPE_PROCESS, (PVOID *)&proc, &access);
    if (NT_SUCCESS(status)) {
        if (!(access & (0x1400U | GENERIC_ALL))) {
            status = STATUS_ACCESS_DENIED;
        } else {
            if (pid) *pid = proc->pid;
            if (exit_code) *exit_code = proc->active ? 259 : (DWORD)proc->exit_status;
        }
    }
    nt_cpu_unlock(flags);
    return status;
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

void nt_process_token_handle_closed(OBJECT_TYPE_ID type, PVOID object)
{
    PROCESS_OBJECT *proc = type == OBJ_TYPE_PROCESS ? object : thread_owner(object);
    nt_process_release_process_object(proc);
}

void nt_process_release_thread_object(PVOID object)
{
    THREAD_OBJECT *thread = (THREAD_OBJECT *)object;
    if (thread == &g_primary_thread) return;
    uint64_t flags = nt_cpu_lock();
    if (!thread || thread->primary_parked || thread->state != THREAD_TERMINATED ||
        handle_object_referenced(&g_handle_table, OBJ_TYPE_THREAD, object)) {
        nt_cpu_unlock(flags);
        return;
    }

    PROCESS_OBJECT *proc = thread_owner(thread);
    if (proc) proc->retired_cpu_cycles += nt_thread_cpu_cycles(thread);
    nt_thread_clear_token(thread);
    nt_proc_memset(thread, 0, sizeof(*thread));
    if (proc && proc->thread_count)
        proc->thread_count--;
    nt_cpu_unlock(flags);
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
    HANDLE              ParentProcess    = (HANDLE)args[3];
    /* BOOLEAN          InheritObjectTable = (BOOLEAN)args[4]; */
    /* HANDLE           SectionHandle    = (HANDLE)args[5]; */
    /* HANDLE           DebugPort        = (HANDLE)args[6]; */
    /* HANDLE           ExceptionPort    = (HANDLE)args[7]; */

    if (!ProcessHandle)
        return STATUS_INVALID_PARAMETER;

    PVOID parent_token = NULL;
    NTSTATUS status = nt_process_reference_token(ParentProcess, 0x0080U, &parent_token);
    if (!NT_SUCCESS(status)) return status;
    PROCESS_OBJECT *proc = alloc_process(parent_token);
    advapi32_token_release(parent_token);
    if (!proc) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Set up minimal PEB */
    proc->peb.BeingDebugged = 0;
    proc->peb.ProcessHeap = (PVOID)(ULONG_PTR)0xBEEF0001;

    /* Allocate handle in caller's table */
    status = handle_alloc(&g_handle_table, OBJ_TYPE_PROCESS,
                                   DesiredAccess, proc, ProcessHandle);
    if (!NT_SUCCESS(status)) {
        proc->active = FALSE;
        nt_process_clear_primary_token(proc);
        nt_job_process_exit(proc->pid, proc->exit_status);
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

    nt_thread_stop_cpu_accounting(thread);
    thread->state = THREAD_TERMINATED;
    nt_thread_clear_token(thread);
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
    ACCESS_MASK access;
    NTSTATUS status = handle_lookup_access_for_process(&g_handle_table,
        ThreadHandle, win32_current_process_id(), OBJ_TYPE_THREAD,
        (PVOID *)&thread, &access);
    if (!NT_SUCCESS(status))
        return status;
    if (!(access & (0x1002U | GENERIC_ALL))) return STATUS_ACCESS_DENIED;

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

static NTSTATUS nt_process_handle_snapshot(HANDLE handle, ULONG *values,
                                            ULONG capacity, ULONG *count)
{
    ULONG_PTR value = (ULONG_PTR)handle;
    if (g_compat32_mode && value == UINT32_MAX) handle = NT_CURRENT_PROCESS;
    if (handle != NT_CURRENT_PROCESS && value > UINT32_MAX)
        return STATUS_INVALID_HANDLE;

    /* NT processes and their ownership sets are mutated on the BSP. Keep the
     * process resolution and locked table snapshot in the same IRQ exclusion. */
    uint64_t flags = nt_cpu_lock();
    PROCESS_OBJECT *proc = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    if (handle == NT_CURRENT_PROCESS) proc = current_process();
    else {
        ACCESS_MASK access = 0;
        status = handle_lookup_access_for_process(&g_handle_table, handle,
            win32_current_process_id(), OBJ_TYPE_PROCESS, (PVOID *)&proc, &access);
        if (NT_SUCCESS(status) && !(access & GENERIC_ALL)) {
            BOOL allowed = values ? (access & 0x0440U) == 0x0440U
                                  : (access & 0x1400U) != 0;
            if (!allowed) status = STATUS_ACCESS_DENIED;
        }
    }
    if (NT_SUCCESS(status)) {
        if (!proc) status = STATUS_INVALID_HANDLE;
        else if (!proc->active || proc->vm_closing) {
            *count = 0;
            if (values) status = STATUS_PROCESS_IS_TERMINATING;
        } else {
            *count = handle_enumerate_for_process(&g_handle_table, proc->pid,
                                                  values, capacity);
        }
    }
    nt_cpu_unlock(flags);
    return status;
}

NTSTATUS nt_process_query_handle_count(HANDLE handle, ULONG *count)
{
    return nt_process_handle_snapshot(handle, NULL, 0, count);
}

static NTSTATUS nt_process_query_handle_table(HANDLE process, PVOID info,
                                               ULONG size, ULONG *returned)
{
    if (size < sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
    if ((ULONG_PTR)info & (sizeof(ULONG) - 1))
        return STATUS_DATATYPE_MISALIGNMENT;
    if (!win32_user_range_writable(info, size, g_compat32_mode) ||
        (returned && !win32_user_range_writable(returned, sizeof(*returned),
                                               g_compat32_mode)))
        return STATUS_ACCESS_VIOLATION;

    ULONG capacity = size / sizeof(ULONG);
    if (capacity > MAX_HANDLES) capacity = MAX_HANDLES;
    extern void *kmalloc(uint64_t size);
    extern void kfree(void *pointer);
    ULONG *snapshot = kmalloc(capacity * sizeof(ULONG));
    if (!snapshot) return STATUS_INSUFFICIENT_RESOURCES;
    ULONG count = 0;
    NTSTATUS status = nt_process_handle_snapshot(process, snapshot, capacity, &count);
    if (NT_SUCCESS(status)) {
        if (count > capacity) count = capacity;
        /* Never fault on a guest output while holding the handle-table lock. */
        for (ULONG i = 0; i < count; i++) ((ULONG *)info)[i] = snapshot[i];
        if (returned) *returned = count * sizeof(ULONG);
    } else if (status == STATUS_PROCESS_IS_TERMINATING && returned) {
        *returned = 0;
    }
    kfree(snapshot);
    return status;
}

static NTSTATUS nt_process_query_wow64(HANDLE handle, PVOID info,
                                        ULONG size, ULONG *returned)
{
    ULONG width = g_compat32_mode ? 4 : 8;
    /* Native rejects oversized buffers too, preserving both outputs. The
     * WOW64 thunk accepts unaligned output; native NT requires DWORD alignment. */
    if (size != width) return STATUS_INFO_LENGTH_MISMATCH;
    if (!g_compat32_mode && ((ULONG_PTR)info & 3U))
        return STATUS_DATATYPE_MISALIGNMENT;
    if (!win32_user_range_writable(info, width, g_compat32_mode) ||
        (returned && !win32_user_range_writable(returned, sizeof(*returned),
                                               g_compat32_mode)))
        return STATUS_ACCESS_VIOLATION;

    ULONG_PTR value = (ULONG_PTR)handle;
    if (g_compat32_mode && value == UINT32_MAX) handle = NT_CURRENT_PROCESS;
    if (handle != NT_CURRENT_PROCESS && value > UINT32_MAX)
        return STATUS_INVALID_HANDLE;
    uint64_t flags = nt_cpu_lock();
    PROCESS_OBJECT *proc = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    if (handle == NT_CURRENT_PROCESS) proc = current_process();
    else {
        ACCESS_MASK access = 0;
        status = handle_lookup_access_for_process(&g_handle_table, handle,
            win32_current_process_id(), OBJ_TYPE_PROCESS, (PVOID *)&proc, &access);
        if (NT_SUCCESS(status) && !(access & (0x1400U | GENERIC_ALL)))
            status = STATUS_ACCESS_DENIED;
    }
    uint64_t peb = 0;
    if (NT_SUCCESS(status)) {
        if (!proc) status = STATUS_INVALID_HANDLE;
        else {
            extern uint32_t win32_process_peb32_address(DWORD process_id);
            /* Snapshot the target's guest address while its process slot is
             * pinned. A native target has no PEB32, independent of our ABI. */
            peb = win32_process_peb32_address(proc->pid);
        }
    }
    nt_cpu_unlock(flags);
    if (!NT_SUCCESS(status)) return status;
    for (ULONG i = 0; i < width; i++) ((BYTE *)info)[i] = (BYTE)(peb >> (i * 8));
    if (returned) {
        for (ULONG i = 0; i < sizeof(*returned); i++)
            ((BYTE *)returned)[i] = (BYTE)(width >> (i * 8));
    }
    return STATUS_SUCCESS;
}

NTSTATUS sys_NtQueryInformationProcess(ULONG_PTR *args)
{
    HANDLE              ProcessHandle  = (HANDLE)args[0];
    PROCESSINFOCLASS    InfoClass      = (PROCESSINFOCLASS)args[1];
    PVOID               ProcessInfo    = (PVOID)args[2];
    ULONG               ProcessInfoLen = (ULONG)args[3];
    ULONG              *ReturnLength   = (ULONG *)args[4];

    if (InfoClass == ProcessHandleTable)
        return nt_process_query_handle_table(ProcessHandle, ProcessInfo,
                                               ProcessInfoLen, ReturnLength);
    if (InfoClass == ProcessWow64Information)
        return nt_process_query_wow64(ProcessHandle, ProcessInfo,
                                      ProcessInfoLen, ReturnLength);

    PROCESS_OBJECT *proc = NULL;

    if (ProcessHandle == NT_CURRENT_PROCESS) {
        proc = current_process();
    } else {
        ACCESS_MASK access;
        NTSTATUS status = handle_lookup_access_for_process(&g_handle_table,
            ProcessHandle, win32_current_process_id(), OBJ_TYPE_PROCESS,
            (PVOID *)&proc, &access);
        if (!NT_SUCCESS(status))
            return status;
        if (!(access & (0x1400U | GENERIC_ALL)))
            return STATUS_ACCESS_DENIED;
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

/* CPU-only resource lifetime test over actual NT objects and PMM storage.
 * A temporary KMT entry exercises the native lifetime provider with no
 * render/display capabilities. No GPU command or cache/DMA qualification. */
static struct {
    uint64_t phys;
    uint32_t pages, allocations, releases, wakes;
} nt_gpu_test_ram;
static bool nt_gpu_test_allocate(void *context, uint32_t pages, uint64_t *dma, void **cpu)
{
    extern void *mem_alloc_pages(uint64_t);
    if (context!=&nt_gpu_test_ram || nt_gpu_test_ram.phys || !pages || pages>2) return false;
    uint64_t phys=(uint64_t)mem_alloc_pages(pages);
    if (!phys) return false;
    nt_gpu_test_ram.phys=phys; nt_gpu_test_ram.pages=pages; nt_gpu_test_ram.allocations++;
    *dma=phys; *cpu=PHYS_TO_VIRT(phys);
    return true;
}
static bool nt_gpu_test_validate(void *context, uint64_t dma, void *cpu, uint32_t pages)
{
    return context==&nt_gpu_test_ram && dma && dma==nt_gpu_test_ram.phys &&
        cpu==PHYS_TO_VIRT(dma) && pages==nt_gpu_test_ram.pages;
}
static bool nt_gpu_test_sync(void *context, uint64_t dma, void *cpu, uint32_t pages)
{
    return nt_gpu_test_validate(context,dma,cpu,pages);
}
static bool nt_gpu_test_cpu_physical(void *context,uint64_t dma,void *cpu,
    uint32_t pages,uint64_t *physical)
{
    if (!physical || !nt_gpu_test_validate(context,dma,cpu,pages)) return false;
    *physical=nt_gpu_test_ram.phys;
    return true;
}
static bool nt_gpu_test_release(void *context, uint64_t dma, void *cpu, uint32_t pages)
{
    extern void mem_free_pages(void *,uint64_t);
    if (!nt_gpu_test_validate(context,dma,cpu,pages)) return false;
    mem_free_pages((void *)dma,pages);
    nt_gpu_test_ram.phys=0; nt_gpu_test_ram.pages=0; nt_gpu_test_ram.releases++;
    return true;
}
static void nt_gpu_test_wake(void *context)
{
    if (context==&nt_gpu_test_ram) nt_gpu_test_ram.wakes++;
}

/* Persistent KMT CPU owner over real PMM and current-process VM mappings.
 * The shared PMM fixture is serialized by nt_process_graphics_selftest. */
static int nt_gpu_cpu_view_selftest(void)
{
    static intel_gpu_memory_t memory;
    static intel_gpu_kmt_t provider;
    static intel_gpu_cpu_views_t views;
    int checks=0,failures=0;
    if (memory.registered || memory.pages || nt_gpu_test_ram.phys ||
        (views.self && !intel_gpu_cpu_views_empty(&views))) return 1;
    memset(&memory,0,sizeof(memory)); memset(&provider,0,sizeof(provider));
    memset(&views,0,sizeof(views)); memset(&nt_gpu_test_ram,0,sizeof(nt_gpu_test_ram));
    OKMT_LUID luid={0}; OKMT_HANDLE adapter=0,device=0,allocation=0;
    BOOL registered=FALSE;
    uint64_t ticket=0,address=0;
    ULONG pid=1;
#define VIEW_EXPECT(c,label) nt_process_test_expect((c),(label),&checks,&failures)
    VIEW_EXPECT(intel_gpu_cpu_views_init(&views,nt_gpu_test_wake,&nt_gpu_test_ram),
        "persistent CPU view owner initializes");
    NTSTATUS admission=nt_process_resolve_vm_pid(NT_CURRENT_PROCESS,0,&pid);
    if (admission!=STATUS_SUCCESS) {
        VIEW_EXPECT(intel_gpu_cpu_view_acquire(&views,pid,1,1,FALSE,TRUE,&ticket,&address)==admission &&
            !ticket && !address,"CPU view rejects a closed root before KMT access");
        goto cleanup;
    }
    const intel_gpu_allocator_t allocator={nt_gpu_test_allocate,nt_gpu_test_validate,
        nt_gpu_test_release,nt_gpu_test_sync,nt_gpu_test_cpu_physical};
    VIEW_EXPECT(intel_gpu_memory_init(&memory,&allocator,&nt_gpu_test_ram,2,2),"CPU view native memory init");
    VIEW_EXPECT(intel_gpu_memory_publish(&memory)==INTEL_GPU_BUFFER_OK,"CPU view native exit registration");
    VIEW_EXPECT(intel_gpu_kmt_provider(&provider,&memory,nt_gpu_test_wake,&nt_gpu_test_ram),"CPU view native KMT provider");
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
    NTSTATUS status=d3dkmt_register_memory_driver(&description,&provider.device,&provider.allocation,&luid);
    VIEW_EXPECT(status==STATUS_SUCCESS,"CPU view temporary non-rendering adapter");
    if (status!=STATUS_SUCCESS) goto cleanup;
    registered=TRUE;
    VIEW_EXPECT(d3dkmt_open_adapter(pid,luid,&adapter)==0,"CPU view adapter opened for real NT owner");
    VIEW_EXPECT(d3dkmt_create_device(pid,adapter,0,&device)==0,"CPU view device admitted for real NT owner");
    D3DKMT_ALLOCATION_DESCRIPTION desc={5000};
    status=d3dkmt_create_allocations(pid,device,1,&desc,&allocation);
    VIEW_EXPECT(status==0 && memory.pages==2,"CPU view allocation owns two real PMM pages");
    if (status!=0) goto cleanup;
    /* Exercise the permanent detached BSP worker with real process PTEs. The
     * diagnostic sync callback validates RAM only; this is not GPU coherence. */
    status=d3dkmt_cpu_acquire(pid,device,allocation,FALSE,FALSE,&ticket,&address);
    VIEW_EXPECT(status==0 && ticket && address,"kernel CPU service maps retained native allocation");
    if (status!=0) goto cleanup;
    VIEW_EXPECT(nt_vm_user_range_accessible(address,8192,FALSE) &&
        !nt_vm_user_range_accessible(address,8192,TRUE),"read-only CPU service view denies process writes");
    memory.resources[0].busy=true;
    VIEW_EXPECT(d3dkmt_cpu_unlock(pid,device,1,&allocation)==D3DKMT_STATUS_DEVICE_BUSY,
        "busy native release remains owned by permanent CPU service");
    VIEW_EXPECT(!nt_vm_user_range_accessible(address,8192,FALSE) && memory.resources[0].cpu_holder,
        "deferred service release revokes CPU VA before releasing backing");
    memory.resources[0].busy=false;
    {
        extern void sched_yield(void);
        uint64_t deadline=idt_get_ticks()+200;
        while ((memory.resources[0].cpu_holder || !d3dkmt_cpu_idle()) && idt_get_ticks()<deadline)
            sched_yield();
        VIEW_EXPECT(!memory.resources[0].cpu_holder && d3dkmt_cpu_idle(),
            "detached CPU worker drains without another app call and sleeps");
    }
    if (memory.resources[0].cpu_holder) goto cleanup;
    VIEW_EXPECT(d3dkmt_cpu_release(pid,ticket)==STATUS_INVALID_PARAMETER,
        "worker-retired public service ticket is stale");
    ticket=address=0;
    for (unsigned bits=0;bits<2;bits++) {
        ticket=address=0;
        status=intel_gpu_cpu_view_acquire(&views,pid,device,allocation,bits!=0,TRUE,&ticket,&address);
        VIEW_EXPECT(status==0 && ticket && address && (!bits || address<0x80000000ULL),
            "native allocation mapped into requested CPU VA window");
        if (status!=0) goto cleanup;
        volatile BYTE *cpu=(volatile BYTE *)(uintptr_t)address;
        cpu[0]=(BYTE)(0x41+bits); cpu[4999]=(BYTE)(0x81+bits);
        BYTE *physical=PHYS_TO_VIRT(nt_gpu_test_ram.phys);
        VIEW_EXPECT(physical[0]==0x41+bits && physical[4999]==0x81+bits && !cpu[8191],
            "mapped native bytes alias PMM and preserve zero page padding");
        VIEW_EXPECT(nt_vm_user_range_accessible(address,8192,TRUE),"native mapped CPU range passes process ownership validation");
        intel_gpu_memory_lease_t denied={0};
        intel_gpu_cpu_view_t *record=&views.views[0];
        VIEW_EXPECT(intel_gpu_memory_acquire(&memory,pid,record->cpu.access.device,
            record->cpu.access.resource,&denied)==INTEL_GPU_BUFFER_BUSY && !denied.self,
            "mapped CPU ownership excludes GPU-capable pins");
        if (!bits) {
            VIEW_EXPECT(intel_gpu_cpu_view_release(&views,pid,ticket)==0,
                "explicit native unlock revokes VM before consuming CPU ownership");
        } else {
            VIEW_EXPECT(d3dkmt_destroy_allocations(pid,device,1,&allocation)==0,
                "logical allocation close retains mapped CPU bytes");
            allocation=0;
            VIEW_EXPECT(memory.pages==2 && nt_gpu_test_ram.phys,
                "CPU mapping retains backing after public allocation close");
            VIEW_EXPECT(nt_vm_unmap_driver_view(&record->vm)==0 && record->cpu.access.self &&
                record->closing && record->vm_done,"VM-initiated retirement queues persistent native cleanup");
            VIEW_EXPECT(intel_gpu_cpu_views_collect(&views)==0,
                "collector consumes CPU ownership after VM retirement");
        }
        VIEW_EXPECT(!record->ticket && !record->cpu.access.self &&
            !nt_vm_user_range_accessible(address,8192,FALSE),"retired native CPU view no longer belongs to process VM");
        VIEW_EXPECT(intel_gpu_cpu_view_release(&views,pid,ticket)==STATUS_INVALID_PARAMETER,
            "retired CPU ticket cannot be reused");
        ticket=0;
    }
cleanup:
    if (views.self) {
        intel_gpu_cpu_views_shutdown(&views);
        for (unsigned i=0;i<INTEL_GPU_CPU_VIEWS;i++) (void)intel_gpu_cpu_views_collect(&views);
        VIEW_EXPECT(intel_gpu_cpu_views_empty(&views),"persistent CPU view owner drained");
    }
    if (allocation) (void)d3dkmt_destroy_allocations(pid,device,1,&allocation);
    if (device) (void)d3dkmt_destroy_device(pid,device);
    if (adapter) (void)d3dkmt_close_adapter(pid,adapter);
    (void)d3dkmt_collect_devices();
    if (registered) VIEW_EXPECT(d3dkmt_unregister_adapter(luid)==0,"CPU view temporary adapter removed");
    if (memory.initialized) {
        intel_gpu_memory_shutdown(&memory);
        for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) (void)intel_gpu_memory_collect(&memory);
        if (memory.registered) VIEW_EXPECT(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_OK,
            "CPU view native memory owner unpublished after cleanup");
    }
    VIEW_EXPECT(!memory.pages && !nt_gpu_test_ram.phys && nt_gpu_test_ram.allocations==nt_gpu_test_ram.releases,
        "CPU view test reclaims all physical backing");
    serial_puts("[GPUVIEW] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef VIEW_EXPECT
    return failures;
}
/* Native command staging over actual PMM and process PTEs. This temporary
 * provider neither submits GPU commands nor certifies DMA/cache behavior. */
static int nt_gpu_context_selftest(void)
{
    extern void sched_yield(void);
    static intel_gpu_memory_t memory;
    static intel_gpu_kmt_t provider;
    static intel_gpu_contexts_t contexts;
    static intel_gpu_kmt_context_lease_t lease;
    static uint64_t service_ticket;
    int checks=0,failures=0;
    if (service_ticket || memory.registered || memory.pages || nt_gpu_test_ram.phys || lease.reference.self ||
        lease.native.self || (contexts.self && !intel_gpu_contexts_empty(&contexts))) return 1;
    ULONG pid=0;
    NTSTATUS admission=nt_process_resolve_vm_pid(NT_CURRENT_PROCESS,0,&pid);
#define CTX_EXPECT(c,label) nt_process_test_expect((c),(label),&checks,&failures)
    if (admission!=STATUS_SUCCESS) {
        CTX_EXPECT(admission==STATUS_PROCESS_IS_TERMINATING,"context test rejects the retired root");
        CTX_EXPECT(intel_gpu_service_idle(),"permanent driver service survives PE root retirement");
        goto report;
    }
    memset(&memory,0,sizeof(memory)); memset(&provider,0,sizeof(provider));
    memset(&contexts,0,sizeof(contexts)); memset(&nt_gpu_test_ram,0,sizeof(nt_gpu_test_ram));
    OKMT_LUID luid={0};
    OKMT_HANDLE adapter=0,device=0,context=0;
    BOOL registered=FALSE,blocked_cpu=FALSE;
    const intel_gpu_allocator_t allocator={nt_gpu_test_allocate,nt_gpu_test_validate,
        nt_gpu_test_release,nt_gpu_test_sync,nt_gpu_test_cpu_physical};
    CTX_EXPECT(intel_gpu_memory_init(&memory,&allocator,&nt_gpu_test_ram,2,2),"context PMM owner initializes");
    CTX_EXPECT(intel_gpu_memory_publish(&memory)==INTEL_GPU_BUFFER_OK,"context memory exit registration");
    CTX_EXPECT(intel_gpu_kmt_provider(&provider,&memory,intel_gpu_service_notify,NULL),"context KMT memory provider");
    CTX_EXPECT(intel_gpu_contexts_init(&contexts,&memory,intel_gpu_service_notify,NULL),"native context owner initializes");
    CTX_EXPECT(intel_gpu_kmt_context_provider(&provider,&contexts),"native context callbacks attach");
    if (failures) goto cleanup;
    CTX_EXPECT(intel_gpu_service_attach(&memory,&contexts,&service_ticket)==0 && service_ticket,
        "permanent kernel service retains the driver owner before KMT publication");
    if (failures) goto cleanup;
    CTX_EXPECT(intel_gpu_service_detach(service_ticket)==D3DKMT_STATUS_DEVICE_BUSY,
        "service refuses to detach an admitted driver owner");
    D3DKMT_ADAPTER_DESCRIPTION adapter_description={.driver_version=3200};
    NTSTATUS status=d3dkmt_register_context_driver(&adapter_description,&provider.device,
        &provider.allocation,&provider.context,&luid);
    CTX_EXPECT(status==0,"temporary non-rendering context adapter");
    if (status!=0) goto cleanup;
    registered=TRUE;
    CTX_EXPECT(d3dkmt_open_adapter(pid,luid,&adapter)==0,"context adapter opens for real process");
    CTX_EXPECT(d3dkmt_create_device(pid,adapter,0,&device)==0,"context device opens for real process");
    if (failures) goto cleanup;
    uint64_t previous=0;
    for (unsigned bits=0;bits<2;bits++) {
        const intel_umd_context_description_t layout={INTEL_UMD_CONTEXT_MAGIC,
            INTEL_UMD_CONTEXT_VERSION,4096,64,64,0};
        D3DKMT_CONTEXT_DESCRIPTION description={.private_bytes=sizeof(layout),.compat32=bits!=0};
        D3DKMT_CONTEXT_BUFFERS buffers={0};
        memcpy(description.private_data,&layout,sizeof(layout));
        status=d3dkmt_create_context(pid,device,&description,&context,&buffers);
        CTX_EXPECT(status==0 && context && memory.pages==2,"context owns two real PMM pages");
        if (status!=0) goto cleanup;
        CTX_EXPECT(buffers.commands && (!bits || buffers.commands<0x80000000ULL) &&
            buffers.allocations==buffers.commands+4096 && buffers.patches==buffers.commands+4608 &&
            buffers.command_bytes==4096 && buffers.allocation_count==64 && buffers.patch_count==64 &&
            !buffers.gpu_commands,"context publishes CPU staging addresses and bounded list sizes");
        BOOL writable=nt_vm_user_range_accessible(buffers.commands,8192,TRUE);
        CTX_EXPECT(writable,"context buffers belong to the process writable VM");
        if (!writable) goto cleanup;
        status=intel_gpu_kmt_acquire_context(pid,context,&lease);
        CTX_EXPECT(status==0,"typed context lease retains native kernel aliases");
        if (status!=0) goto cleanup;
        intel_gpu_command_context_t *record=&contexts.contexts[0];
        CTX_EXPECT(lease.native.id>previous && record->id==lease.native.id,
            "context reuse has a new native cookie");
        previous=lease.native.id;
        volatile BYTE *cpu=(volatile BYTE *)(uintptr_t)buffers.commands;
        CTX_EXPECT(!cpu[0] && !cpu[4095] && !cpu[4608] && !cpu[8191],"new staging bytes and page padding are zero");
        cpu[0]=0x41; cpu[4095]=0x42; cpu[4096]=0x43; cpu[4608]=0x44;
        CTX_EXPECT(((BYTE *)lease.native.commands)[0]==0x41 && ((BYTE *)lease.native.commands)[4095]==0x42 &&
            ((BYTE *)lease.native.allocations)[0]==0x43 && ((BYTE *)lease.native.patches)[0]==0x44,
            "process staging writes alias retained native PMM bytes");
        if (bits) {
            CTX_EXPECT(nt_vm_unmap_driver_view(&record->vm)==0 && record->vm_done && record->retained,
                "VM retirement retains the external context cookie");
            uint64_t deadline=idt_get_ticks()+200;
            while (!intel_gpu_service_idle() && idt_get_ticks()<deadline) sched_yield();
            CTX_EXPECT(intel_gpu_service_idle() && memory.pages==2 && record->cpu.self,
                "capture lease holds backing after process VA retirement");
        } else {
            CTX_EXPECT(d3dkmt_destroy_context(pid,context)==0,"context closes while capture retains its owner");
            context=0;
            CTX_EXPECT(memory.pages==2 && !nt_gpu_test_ram.releases,"logical context close preserves captured bytes");
        }
        CTX_EXPECT(((BYTE *)lease.native.commands)[4095]==0x42,"retained capture remains valid after close");
        if (!bits) { memory.resources[0].busy=true; blocked_cpu=TRUE; }
        CTX_EXPECT(intel_gpu_kmt_release_context(&lease)==0,"context capture returns both native and KMT leases");
        if (!bits) {
            uint64_t deadline=idt_get_ticks()+200;
            while (!record->vm_done && idt_get_ticks()<deadline) sched_yield();
            CTX_EXPECT(record->vm_done && record->cpu.self && memory.pages==2 &&
                !nt_vm_user_range_accessible(buffers.commands,8192,FALSE),
                "service revokes process VA while busy native CPU release stays retained");
            memory.resources[0].busy=false; blocked_cpu=FALSE;
            /* No notify: its timer retry must finish without another app call. */
        }
        uint64_t deadline=idt_get_ticks()+200;
        while ((memory.pages || !intel_gpu_service_idle()) && idt_get_ticks()<deadline) sched_yield();
        CTX_EXPECT(!memory.pages && intel_gpu_service_idle(),"detached driver worker retires CPU ownership and sleeps");
        CTX_EXPECT(!nt_vm_user_range_accessible(buffers.commands,8192,FALSE) && !record->cpu.self && !record->resource,
            "context VA is revoked before backing reclamation");
        if (bits) {
            CTX_EXPECT(record->id==previous && record->retained && !intel_gpu_contexts_need_retry(&contexts),
                "VM-retired context keeps its cookie without polling");
            CTX_EXPECT(d3dkmt_destroy_context(pid,context)==0,"KMT returns VM-retired context cookie");
            context=0;
            deadline=idt_get_ticks()+200;
            while ((record->id || !intel_gpu_service_idle()) && idt_get_ticks()<deadline) sched_yield();
            CTX_EXPECT(!record->id && intel_gpu_service_idle(),"worker retires returned context tombstone without app collection");
        }
        CTX_EXPECT(!record->id,"native context slot can be reused only after ownership drains");
        CTX_EXPECT(!memory.pages && !nt_gpu_test_ram.phys && nt_gpu_test_ram.allocations==nt_gpu_test_ram.releases,
            "native memory collector reclaims context PMM pages");
        if (failures) goto cleanup;
    }
cleanup:
    if (blocked_cpu) memory.resources[0].busy=false;
    if (context) (void)d3dkmt_destroy_context(pid,context);
    if (lease.native.self) (void)intel_gpu_kmt_release_context(&lease);
    if (device) (void)d3dkmt_destroy_device(pid,device);
    if (adapter) (void)d3dkmt_close_adapter(pid,adapter);
    if (contexts.self) {
        intel_gpu_contexts_shutdown(&contexts);
        if (!service_ticket)
            for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) (void)intel_gpu_contexts_collect(&contexts);
    }
    (void)d3dkmt_collect_devices();
    if (registered) CTX_EXPECT(d3dkmt_unregister_adapter(luid)==0,"temporary context adapter removed");
    if (memory.initialized) {
        intel_gpu_memory_shutdown(&memory);
        if (service_ticket) {
            intel_gpu_service_notify(NULL);
            uint64_t deadline=idt_get_ticks()+200;
            while ((!intel_gpu_contexts_empty(&contexts) || memory.pages || !intel_gpu_service_idle()) &&
                idt_get_ticks()<deadline) sched_yield();
        } else {
            for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) (void)intel_gpu_memory_collect(&memory);
        }
        if (memory.registered) CTX_EXPECT(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_OK,
            "context native memory owner unregisters after cleanup");
    }
    if (contexts.self) CTX_EXPECT(intel_gpu_contexts_empty(&contexts),"native command context owner drains completely");
    if (service_ticket) {
        NTSTATUS detached=intel_gpu_service_detach(service_ticket);
        CTX_EXPECT(detached==0,"service returns driver storage only after VM and native memory retire");
        if (detached==0) service_ticket=0;
    }
    CTX_EXPECT(!memory.pages && !nt_gpu_test_ram.phys && nt_gpu_test_ram.allocations==nt_gpu_test_ram.releases,
        "context test retains no physical backing");
report:
    serial_puts("[GPUCTX] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef CTX_EXPECT
    return failures;
}
/* Separate four-allocation RAM fixture: a staging context plus two resources.
 * Sync certifies these test PMM spans only; it does not qualify GPU DMA. */
static struct {
    uint64_t physical[4];
    uint32_t pages[4],allocations,releases;
} nt_gpu_capture_ram;
static bool nt_gpu_capture_allocate(void *opaque,uint32_t pages,uint64_t *dma,void **cpu)
{
    extern void *mem_alloc_pages(uint64_t);
    if (opaque!=&nt_gpu_capture_ram || !pages || pages>2) return false;
    for (unsigned i=0;i<4;i++) if (!nt_gpu_capture_ram.physical[i]) {
        uint64_t physical=(uint64_t)mem_alloc_pages(pages);
        if (!physical) return false;
        nt_gpu_capture_ram.physical[i]=physical; nt_gpu_capture_ram.pages[i]=pages;
        nt_gpu_capture_ram.allocations++; *dma=physical; *cpu=PHYS_TO_VIRT(physical);
        return true;
    }
    return false;
}
static bool nt_gpu_capture_validate(void *opaque,uint64_t dma,void *cpu,uint32_t pages)
{
    if (opaque!=&nt_gpu_capture_ram || !dma || cpu!=PHYS_TO_VIRT(dma)) return false;
    for (unsigned i=0;i<4;i++)
        if (nt_gpu_capture_ram.physical[i]==dma && nt_gpu_capture_ram.pages[i]==pages) return true;
    return false;
}
static bool nt_gpu_capture_release(void *opaque,uint64_t dma,void *cpu,uint32_t pages)
{
    extern void mem_free_pages(void *,uint64_t);
    if (!nt_gpu_capture_validate(opaque,dma,cpu,pages)) return false;
    for (unsigned i=0;i<4;i++) if (nt_gpu_capture_ram.physical[i]==dma) {
        mem_free_pages((void *)dma,pages);
        nt_gpu_capture_ram.physical[i]=0; nt_gpu_capture_ram.pages[i]=0; nt_gpu_capture_ram.releases++;
        return true;
    }
    return false;
}
static bool nt_gpu_capture_physical(void *opaque,uint64_t dma,void *cpu,uint32_t pages,uint64_t *physical)
{
    if (!physical || !nt_gpu_capture_validate(opaque,dma,cpu,pages)) return false;
    *physical=dma; return true;
}
static struct {
    unsigned ready,done;
    uint64_t id;
    DWORD pid;
    NTSTATUS result;
} nt_gpu_job_probe;
static void nt_gpu_job_ready(void *opaque)
{
    if (opaque==&nt_gpu_job_probe) __atomic_add_fetch(&nt_gpu_job_probe.ready,1,__ATOMIC_RELAXED);
}
static void nt_gpu_job_done(void *opaque,uint64_t id,DWORD pid,NTSTATUS result)
{
    if (opaque!=&nt_gpu_job_probe) return;
    nt_gpu_job_probe.id=id; nt_gpu_job_probe.pid=pid; nt_gpu_job_probe.result=result;
    __atomic_add_fetch(&nt_gpu_job_probe.done,1,__ATOMIC_RELEASE);
}
static int nt_gpu_capture_selftest(void)
{
    extern void sched_yield(void);
    static intel_gpu_memory_t memory;
    static intel_gpu_kmt_t provider;
    static intel_gpu_contexts_t contexts;
    static intel_gpu_capture_t capture;
    static intel_gpu_jobs_t jobs;
    static intel_gpu_job_lease_t job;
    static intel_gpu_kmt_cpu_lease_t cpu;
    static uint64_t ticket;
    int checks=0,failures=0;
    if (ticket || memory.registered || memory.pages || capture.self || cpu.reference.self || job.self ||
        (jobs.self && !intel_gpu_jobs_empty(&jobs)) ||
        nt_gpu_capture_ram.allocations!=nt_gpu_capture_ram.releases) return 1;
    ULONG pid=0;
    NTSTATUS admission=nt_process_resolve_vm_pid(NT_CURRENT_PROCESS,0,&pid);
#define CAP_EXPECT(c,label) nt_process_test_expect((c),(label),&checks,&failures)
    if (admission!=STATUS_SUCCESS) {
        CAP_EXPECT(admission==STATUS_PROCESS_IS_TERMINATING && intel_gpu_executor_idle() &&
            intel_gpu_executor_quiescent(),"capture test refuses retired root; detached executor survives");
        goto report;
    }
    CAP_EXPECT(intel_gpu_executor_start()==0,"native executor starts detached in kernel address space");
    intel_gpu_executor_notify(NULL);
    uint64_t executor_until=idt_get_ticks()+200;
    while (!intel_gpu_executor_idle() && idt_get_ticks()<executor_until) sched_yield();
    CAP_EXPECT(intel_gpu_executor_idle() && intel_gpu_executor_quiescent(),
        "native executor consumes retained hints and parks without an owner");
    memset(&memory,0,sizeof(memory)); memset(&provider,0,sizeof(provider));
    memset(&contexts,0,sizeof(contexts)); memset(&nt_gpu_capture_ram,0,sizeof(nt_gpu_capture_ram));
    memset(&jobs,0,sizeof(jobs)); memset(&nt_gpu_job_probe,0,sizeof(nt_gpu_job_probe));
    uint64_t job_id=0;
    OKMT_LUID luid={0}; OKMT_HANDLE adapter=0,device=0,context=0,resources[2]={0};
    BOOL registered=FALSE;
    D3DKMT_CONTEXT_BUFFERS buffers={0};
    const intel_gpu_allocator_t allocator={nt_gpu_capture_allocate,nt_gpu_capture_validate,
        nt_gpu_capture_release,nt_gpu_capture_validate,nt_gpu_capture_physical};
    CAP_EXPECT(intel_gpu_memory_init(&memory,&allocator,&nt_gpu_capture_ram,4,4),"capture PMM manager initializes");
    CAP_EXPECT(intel_gpu_memory_publish(&memory)==INTEL_GPU_BUFFER_OK,"capture PMM manager exit registration");
    CAP_EXPECT(intel_gpu_kmt_provider(&provider,&memory,intel_gpu_service_notify,NULL),"capture native KMT provider");
    CAP_EXPECT(intel_gpu_contexts_init(&contexts,&memory,intel_gpu_service_notify,NULL),"capture native context owner");
    CAP_EXPECT(intel_gpu_kmt_context_provider(&provider,&contexts),"capture context callbacks attach");
    CAP_EXPECT(intel_gpu_jobs_init(&jobs,&contexts,nt_gpu_job_ready,nt_gpu_job_done,&nt_gpu_job_probe),
        "persistent job owner retains native captures independently of submitter");
    CAP_EXPECT(intel_gpu_service_attach_jobs(&memory,&contexts,&jobs,&ticket)==0,"capture job owner attaches to permanent service");
    if (failures) goto cleanup;
    D3DKMT_ADAPTER_DESCRIPTION adapter_description={.driver_version=3200};
    NTSTATUS status=d3dkmt_register_context_driver(&adapter_description,&provider.device,&provider.allocation,&provider.context,&luid);
    CAP_EXPECT(status==0,"temporary capture adapter registers without rendering capability");
    if (status!=0) goto cleanup;
    registered=TRUE;
    CAP_EXPECT(d3dkmt_open_adapter(pid,luid,&adapter)==0,"capture adapter opens for real process");
    CAP_EXPECT(d3dkmt_create_device(pid,adapter,0,&device)==0,"capture device opens for real process");
    const intel_umd_context_description_t layout={INTEL_UMD_CONTEXT_MAGIC,INTEL_UMD_CONTEXT_VERSION,4096,64,64,0};
    D3DKMT_CONTEXT_DESCRIPTION description={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(layout),.compat32=TRUE};
    memcpy(description.private_data,&layout,sizeof(layout));
    status=d3dkmt_create_context(pid,device,&description,&context,&buffers);
    CAP_EXPECT(status==0,"capture command staging maps actual low process VA");
    if (status!=0) goto cleanup;
    D3DKMT_ALLOCATION_DESCRIPTION allocations[2]={{1000},{1000}};
    status=d3dkmt_create_allocations(pid,device,2,allocations,resources);
    CAP_EXPECT(status==0 && memory.pages==4,"capture resources own distinct PMM allocations");
    if (status!=0) goto cleanup;
    BOOL writable=nt_vm_user_range_accessible(buffers.commands,8192,TRUE);
    CAP_EXPECT(writable,"capture staging bytes belong to process writable VM");
    if (!writable) goto cleanup;
    intel_umd_copy32_t command={INTEL_UMD_COMMAND_MAGIC,INTEL_UMD_COMMAND_VERSION,
        INTEL_UMD_COPY32,sizeof(command),16,8,128,128,0,0,{0}};
    OKMT_ALLOCATION_LIST list[2]={{resources[0],0},{resources[1],1}};
    OKMT_PATCH_LOCATION patches[2]={{0,0,0,0,160,0},{1,1,0,0,168,0}};
    memcpy((BYTE *)(uintptr_t)buffers.commands+128,&command,sizeof(command));
    memcpy((void *)(uintptr_t)buffers.allocations,list,sizeof(list));
    memcpy((void *)(uintptr_t)buffers.patches,patches,sizeof(patches));
    status=intel_gpu_capture_begin(pid,context,128,sizeof(command),2,2,&capture);
    CAP_EXPECT(status==0 && capture.valid,"kernel captures and validates process command and resource lists");
    if (status!=0) goto cleanup;
    CAP_EXPECT(capture.resources[0].native.buffer!=capture.resources[1].native.buffer &&
        capture.resources[0].native.buffer->bytes==1000 && capture.resources[1].native.buffer->bytes==1000,
        "capture retains exact owned resource sizes rather than rounded PMM size");
    status=intel_gpu_jobs_submit(&jobs,pid,context,128,sizeof(command),2,2,&job_id);
    CAP_EXPECT(status==0 && job_id && __atomic_load_n(&nt_gpu_job_probe.ready,__ATOMIC_ACQUIRE),
        "job publication captures real process staging and notifies native executor");
    if (status!=0) goto cleanup;
    status=intel_gpu_jobs_take(&jobs,&job);
    CAP_EXPECT(status==0 && job.id==job_id && job.capture->valid,
        "native executor claims stable captured work after KMT admission");
    if (status!=0) goto cleanup;
    CAP_EXPECT(intel_gpu_jobs_cancel(&jobs,pid,job_id)==D3DKMT_STATUS_DEVICE_BUSY &&
        intel_gpu_jobs_cancelled(&job),"cancellation preserves claimed work until trusted retirement");
    memset((BYTE *)(uintptr_t)buffers.commands+128,0xff,sizeof(command));
    memset((void *)(uintptr_t)buffers.allocations,0xff,sizeof(list));
    memset((void *)(uintptr_t)buffers.patches,0xff,sizeof(patches));
    CAP_EXPECT(capture.command.width==16 && capture.command.height==8 && !capture.command.source &&
        capture.allocations[0].allocation==resources[0],"capture is independent of later process staging writes");
    status=intel_gpu_kmt_acquire_cpu(pid,device,resources[0],&cpu);
    CAP_EXPECT(status==0,"captured resource residency permits CPU access before GPU execution");
    if (status!=0) goto cleanup;
    ((BYTE *)cpu.access.cpu)[0]=0x73;
    CAP_EXPECT(intel_gpu_kmt_release_cpu(&cpu)==0,"CPU ownership returns without losing capture residency");
    CAP_EXPECT(d3dkmt_destroy_context(pid,context)==0,"logical context close preserves captured work");
    context=0;
    CAP_EXPECT(d3dkmt_destroy_device(pid,device)==0,"logical device close preserves captured resources");
    device=0;
    uint64_t deadline=idt_get_ticks()+200;
    while (!intel_gpu_service_idle() && idt_get_ticks()<deadline) sched_yield();
    CAP_EXPECT(intel_gpu_service_idle() && memory.pages==4 && !nt_gpu_capture_ram.releases &&
        ((BYTE *)capture.resources[0].native.buffer->cpu)[0]==0x73,
        "permanent service retains closed resources until capture ownership returns");
    CAP_EXPECT(intel_gpu_capture_release(&capture)==0,"capture returns context and both native resource references");
    deadline=idt_get_ticks()+200;
    while (!intel_gpu_service_idle() && idt_get_ticks()<deadline) sched_yield();
    CAP_EXPECT(memory.pages==4 && !__atomic_load_n(&nt_gpu_job_probe.done,__ATOMIC_ACQUIRE) && job.capture->valid,
        "persistent claimed job retains all backing after device close and caller capture release");
    /* This fixture never submitted GPU work, so retirement is proven here. */
    CAP_EXPECT(intel_gpu_jobs_retire(&job,STATUS_CANCELLED)==0 && !job.self,
        "trusted no-submission retirement returns executor ownership");
    deadline=idt_get_ticks()+200;
    while ((memory.pages || !intel_gpu_service_idle()) && idt_get_ticks()<deadline) sched_yield();
    CAP_EXPECT(!memory.pages && nt_gpu_capture_ram.releases==3 &&
        !nt_vm_user_range_accessible(buffers.commands,8192,FALSE),"service revokes staging and reclaims all captured PMM backing");
    CAP_EXPECT(__atomic_load_n(&nt_gpu_job_probe.done,__ATOMIC_ACQUIRE)==1 &&
        nt_gpu_job_probe.id==job_id && nt_gpu_job_probe.pid==pid && nt_gpu_job_probe.result==STATUS_CANCELLED,
        "permanent service delivers one completion after capture retirement");
cleanup:
    if (jobs.self) intel_gpu_jobs_shutdown(&jobs);
    if (job.self) (void)intel_gpu_jobs_retire(&job,STATUS_CANCELLED); /* no GPU work in this test */
    if (cpu.reference.self) (void)intel_gpu_kmt_release_cpu(&cpu);
    if (capture.self) (void)intel_gpu_capture_release(&capture);
    if (context) (void)d3dkmt_destroy_context(pid,context);
    if (device) (void)d3dkmt_destroy_device(pid,device);
    if (adapter) (void)d3dkmt_close_adapter(pid,adapter);
    if (registered) {
        NTSTATUS removed=d3dkmt_unregister_adapter(luid);
        CAP_EXPECT(removed==0,"temporary capture adapter removed");
        if (removed==0) registered=FALSE;
    }
    if (contexts.self) intel_gpu_contexts_shutdown(&contexts);
    if (memory.initialized) intel_gpu_memory_shutdown(&memory);
    if (ticket) {
        intel_gpu_service_notify(NULL);
        uint64_t deadline=idt_get_ticks()+200;
        while ((!intel_gpu_contexts_empty(&contexts) || !intel_gpu_jobs_empty(&jobs) || memory.pages || !intel_gpu_service_idle()) &&
            idt_get_ticks()<deadline) sched_yield();
    } else {
        if (jobs.self)
            for (unsigned i=0;i<INTEL_GPU_JOBS;i++) (void)intel_gpu_jobs_collect(&jobs);
        if (contexts.self)
            for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) (void)intel_gpu_contexts_collect(&contexts);
        if (memory.initialized)
            for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) (void)intel_gpu_memory_collect(&memory);
    }
    if (memory.registered) CAP_EXPECT(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_OK,
        "capture memory manager unpublishes after complete retirement");
    if (ticket && !registered) {
        NTSTATUS detached=intel_gpu_service_detach(ticket);
        CAP_EXPECT(detached==0,"capture owner detaches after native cleanup");
        if (detached==0) ticket=0;
    }
    CAP_EXPECT(!ticket && !memory.pages && !capture.self && !cpu.reference.self && !job.self &&
        (!jobs.self || intel_gpu_jobs_empty(&jobs)) &&
        nt_gpu_capture_ram.allocations==nt_gpu_capture_ram.releases,"capture fixture leaves no retained backing or leases");
report:
    serial_puts("[GPUCAP] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef CAP_EXPECT
    return failures;
}
static int nt_gpu_execution_selftest(void)
{
    extern void sched_yield(void);
    static intel_gpu_memory_t memory;
    static intel_gpu_contexts_t contexts;
    static intel_gpu_kmt_t provider;
    static intel_gpu_jobs_t jobs;
    static intel_gpu_kmt_cpu_lease_t cpu[2];
    static uint64_t collector;
    ULONG pid=0;
    if (!intel_gfx_capture_available() || nt_process_resolve_vm_pid(NT_CURRENT_PROCESS,0,&pid)!=0) {
        serial_puts("[GPUHW] unavailable: no stopped physical engine or live process root\n");
        return 0;
    }
    if (collector || memory.registered || memory.pages || cpu[0].reference.self ||
        cpu[1].reference.self || (jobs.self && !intel_gpu_jobs_empty(&jobs))) return 1;
    memset(&memory,0,sizeof(memory)); memset(&contexts,0,sizeof(contexts));
    memset(&provider,0,sizeof(provider)); memset(&jobs,0,sizeof(jobs));
    memset(&nt_gpu_job_probe,0,sizeof(nt_gpu_job_probe));
    int checks=0,failures=0;
    unsigned copies=0,verified_bytes=0;
    uint64_t submitted=0;
    BOOL registered=FALSE,opened=FALSE;
    OKMT_LUID luid={0}; OKMT_HANDLE adapter=0,device=0,context=0,resources[2]={0};
    D3DKMT_CONTEXT_BUFFERS buffers={0};
#define HW_EXPECT(c,label) nt_process_test_expect((c),(label),&checks,&failures)
    HW_EXPECT(intel_gfx_capture_memory_init(&memory),"physical DMA allocator initializes owned PMM manager");
    HW_EXPECT(intel_gpu_memory_publish(&memory)==INTEL_GPU_BUFFER_OK,"physical resources publish for process exit");
    HW_EXPECT(intel_gpu_contexts_init(&contexts,&memory,intel_gpu_service_notify,NULL),"physical staging owner initializes");
    HW_EXPECT(intel_gpu_kmt_provider(&provider,&memory,intel_gpu_service_notify,NULL) &&
        intel_gpu_kmt_context_provider(&provider,&contexts),"physical native provider uses typed KMT resources");
    HW_EXPECT(intel_gpu_jobs_init(&jobs,&contexts,intel_gpu_executor_notify,nt_gpu_job_done,&nt_gpu_job_probe),
        "physical captured job pool uses permanent executor");
    HW_EXPECT(intel_gpu_kmt_render_provider(&provider,&jobs),"physical captured jobs expose native Render provider");
    HW_EXPECT(intel_gpu_service_attach_jobs(&memory,&contexts,&jobs,&collector)==0,"physical cleanup remains on permanent BSP service");
    if (failures) goto cleanup;
    opened=TRUE; /* close owns partial initialization too */
    HW_EXPECT(intel_gfx_capture_open(&jobs)==0,"physical executor starts before any application context or surface");
    if (failures) goto cleanup;
    D3DKMT_ADAPTER_DESCRIPTION adapter_description={.driver_version=3200};
    registered=d3dkmt_register_render_driver(&adapter_description,&provider.device,&provider.allocation,&provider.context,&provider.render,&luid)==0;
    HW_EXPECT(registered,"physical probe registers temporary adapter without render capabilities");
    if (!registered) goto cleanup;
    HW_EXPECT(d3dkmt_open_adapter(pid,luid,&adapter)==0 && d3dkmt_create_device(pid,adapter,0,&device)==0,
        "physical test owns real process KMT device");
    const intel_umd_context_description_t layout={INTEL_UMD_CONTEXT_MAGIC,INTEL_UMD_CONTEXT_VERSION,4096,64,64,0};
    D3DKMT_CONTEXT_DESCRIPTION description={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(layout),.compat32=TRUE};
    memcpy(description.private_data,&layout,sizeof(layout));
    D3DKMT_ALLOCATION_DESCRIPTION sizes[2]={{1000},{1000}};
    for (unsigned round=0;round<4;round++) {
        serial_puts("[GPUHW] copy round="); serial_putdec(round); serial_puts(" preparing\n");
        if (round==0 || round==2) {
            if (context) HW_EXPECT(d3dkmt_destroy_context(pid,context)==0,"old logical context closes while engine remains live");
            context=0;
            HW_EXPECT(d3dkmt_create_context(pid,device,&description,&context,&buffers)==0 &&
                nt_vm_user_range_accessible(buffers.commands,8192,TRUE),"new physical command context maps process staging after engine start");
            if (failures) goto cleanup;
            OKMT_HANDLE old[2]={resources[0],resources[1]};
            HW_EXPECT(d3dkmt_create_allocations(pid,device,2,sizes,resources)==0,"physical source and destination allocate");
            if (failures) goto cleanup;
            if (round==2) HW_EXPECT(d3dkmt_destroy_allocations(pid,device,2,old)==0,"old physical surfaces close before live rebind");
        }
        intel_umd_copy32_t command={INTEL_UMD_COMMAND_MAGIC,INTEL_UMD_COMMAND_VERSION,
            INTEL_UMD_COPY32,sizeof(command),13,7,80,96,0,0,{0}};
        OKMT_ALLOCATION_LIST list[2]={{resources[0],0},{resources[1],1}};
        OKMT_PATCH_LOCATION patches[2]={{0,0,0,0,160,0},{1,1,0,0,168,0}};
        memcpy((BYTE *)(uintptr_t)buffers.commands+128,&command,sizeof(command));
        memcpy((void *)(uintptr_t)buffers.allocations,list,sizeof(list));
        memcpy((void *)(uintptr_t)buffers.patches,patches,sizeof(patches));
        for (unsigned role=0;role<2;role++) {
            HW_EXPECT(intel_gpu_kmt_acquire_cpu(pid,device,resources[role],&cpu[role])==0,
                "physical CPU fill owns resource before GPU execution");
            if (failures) goto cleanup;
            BYTE *bytes=cpu[role].access.cpu;
            for (unsigned i=0;i<1000;i++) bytes[i]=role ? 0xa6 : (BYTE)(i*13+round*7);
        }
        HW_EXPECT(intel_gpu_kmt_release_cpu(&cpu[0])==0,"source CPU cache ownership returns before dispatch");
        BYTE *render_argument=(BYTE *)(uintptr_t)buffers.commands+512;
        BOOL compat=(round&1)==0;
        memset(render_argument,0,sizeof(OKMT_RENDER));
        DWORD *narrow=(DWORD *)render_argument;
        narrow[0]=context; narrow[1]=128; narrow[2]=sizeof(command); narrow[3]=2; narrow[4]=2;
        if (compat) narrow[11]=7; /* decline resize, return existing capacities */
        else ((OKMT_RENDER *)render_argument)->flags=7;
        submitted=nt_gpu_job_probe.id;
        int saved_compat=g_compat32_mode; g_compat32_mode=compat;
        NTSTATUS rendered=D3DKMTRender(render_argument);
        g_compat32_mode=saved_compat;
        HW_EXPECT(rendered==0,"public PE32/64 Render admits captured native command");
        if (compat) {
            HW_EXPECT(narrow[5]==buffers.commands && narrow[6]==buffers.command_bytes &&
                narrow[7]==buffers.allocations && narrow[8]==buffers.allocation_count &&
                narrow[9]==buffers.patches && narrow[10]==buffers.patch_count && narrow[79]==1,
                "PE32 Render returns fixed staging buffers and one pending job");
        } else {
            OKMT_RENDER *returned=(OKMT_RENDER *)render_argument;
            HW_EXPECT((uint64_t)returned->commands==buffers.commands && returned->command_bytes==buffers.command_bytes &&
                (uint64_t)returned->allocations==buffers.allocations && returned->allocation_capacity==buffers.allocation_count &&
                (uint64_t)returned->patches==buffers.patches && returned->patch_capacity==buffers.patch_count && returned->queued==1,
                "PE64 Render returns fixed staging buffers and one pending job");
        }
        if (failures) goto cleanup;
        /* Captured job must be independent of all subsequent staging writes. */
        memset((BYTE *)(uintptr_t)buffers.commands+128,0xff,sizeof(command));
        serial_puts("[GPUHW] submitted, CPU destination held\n");
        uint64_t until=idt_get_ticks()+10;
        while (idt_get_ticks()<until) sched_yield();
        HW_EXPECT(__atomic_load_n(&nt_gpu_job_probe.done,__ATOMIC_ACQUIRE)==round,
            "held CPU destination prevents physical GPU completion");
        HW_EXPECT(intel_gpu_kmt_release_cpu(&cpu[1])==0,"destination CPU release permits native execution");
        serial_puts("[GPUHW] CPU destination released\n");
        until=idt_get_ticks()+300;
        while (__atomic_load_n(&nt_gpu_job_probe.done,__ATOMIC_ACQUIRE)<round+1 && idt_get_ticks()<until) sched_yield();
        HW_EXPECT(__atomic_load_n(&nt_gpu_job_probe.done,__ATOMIC_ACQUIRE)==round+1 &&
            nt_gpu_job_probe.id>submitted && nt_gpu_job_probe.pid==pid && nt_gpu_job_probe.result==0,
            "physical native completion retires captured job successfully");
        if (failures) goto cleanup;
        HW_EXPECT(intel_gpu_kmt_acquire_cpu(pid,device,resources[1],&cpu[1])==0,
            "completed GPU destination returns to exclusive CPU ownership");
        if (failures) goto cleanup;
        BYTE *actual=cpu[1].access.cpu;
        BOOL equal=TRUE;
        for (unsigned i=0;i<1000;i++) {
            unsigned row=i/96,column=i%96;
            BYTE expected=row<7 && column<52 ? (BYTE)((row*80+column)*13+round*7) : 0xa6;
            if (actual[i]!=expected) equal=FALSE;
        }
        HW_EXPECT(equal,"GPU copy matches captured dimensions and preserves row gaps and trailing bytes");
        HW_EXPECT(intel_gpu_kmt_release_cpu(&cpu[1])==0,"verified physical destination releases CPU lease");
        if (failures) goto cleanup;
        copies++; verified_bytes+=1000;
        serial_puts("[GPUHW] copy verified\n");
    }
cleanup:
    serial_puts("[GPUHW] cleanup\n");
    for (unsigned i=0;i<2;i++) if (cpu[i].reference.self)
        HW_EXPECT(intel_gpu_kmt_release_cpu(&cpu[i])==0,"physical cleanup releases CPU leases");
    if (opened) {
        NTSTATUS closed;
        uint64_t until=idt_get_ticks()+500;
        do { closed=intel_gfx_capture_close(); if (closed!=0) sched_yield(); } while (closed!=0 && idt_get_ticks()<until);
        HW_EXPECT(closed==0,"physical executor joins and retires before returning structural mappings");
        if (closed!=0) goto report; /* hardware may still own DMA; preserve every manager */
    }
    if (jobs.self) intel_gpu_jobs_shutdown(&jobs);
    if (context) (void)d3dkmt_destroy_context(pid,context);
    if (device) (void)d3dkmt_destroy_device(pid,device);
    if (adapter) (void)d3dkmt_close_adapter(pid,adapter);
    if (registered) {
        NTSTATUS status=d3dkmt_unregister_adapter(luid);
        uint64_t until=idt_get_ticks()+200;
        while (status==D3DKMT_STATUS_DEVICE_BUSY && idt_get_ticks()<until) {
            sched_yield(); status=d3dkmt_unregister_adapter(luid);
        }
        HW_EXPECT(status==0,"physical temporary adapter withdraws");
    }
    if (contexts.self) intel_gpu_contexts_shutdown(&contexts);
    if (memory.initialized) intel_gpu_memory_shutdown(&memory);
    if (collector) {
        intel_gpu_service_notify(NULL);
        uint64_t until=idt_get_ticks()+300;
        while ((memory.pages || !intel_gpu_contexts_empty(&contexts) || !intel_gpu_jobs_empty(&jobs) ||
                !intel_gpu_service_idle()) && idt_get_ticks()<until) sched_yield();
    } else if (memory.initialized) {
        for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) {
            if (jobs.self) (void)intel_gpu_jobs_collect(&jobs);
            if (contexts.self) (void)intel_gpu_contexts_collect(&contexts);
            (void)intel_gpu_memory_collect(&memory);
        }
    }
    if (memory.registered) HW_EXPECT(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_OK,"physical resource manager drains");
    if (collector) {
        NTSTATUS status=intel_gpu_service_detach(collector);
        HW_EXPECT(status==0,"physical cleanup service detaches after all callbacks");
        if (status==0) collector=0;
    }
    HW_EXPECT(!collector && !memory.pages && intel_gfx_capture_clean(),"physical engine and KMT allocations reclaim every guarded PMM page");
report:
    serial_puts("[GPUHW] checks="); serial_putdec(checks); serial_puts(" failures="); serial_putdec(failures);
    serial_puts(" copies="); serial_putdec(copies); serial_puts(" verified_bytes="); serial_putdec(verified_bytes);
    serial_puts(" clean="); serial_putdec(intel_gfx_capture_clean()); serial_puts("\n");
#undef HW_EXPECT
    return failures;
}

typedef struct {
    intel_gpu_memory_t *memory;
    ULONG pid, device;
    NTSTATUS status;
    uint32_t cpu, done;
} NT_GPU_TEST_WORK;
static void nt_gpu_test_ap(void *argument, void *result)
{
    (void)result;
    extern uint32_t smp_current_cpu(void);
    NT_GPU_TEST_WORK *work=argument;
    work->cpu=smp_current_cpu();
    work->status=nt_process_open_gpu_device(work->pid,work->memory,&work->device);
    __atomic_store_n(&work->done,1,__ATOMIC_RELEASE);
}

static int nt_process_graphics_selftest(void)
{
    static unsigned busy;
    static intel_gpu_memory_t memory;
    static intel_gpu_kmt_t kmt;
    if (__atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE)) return 1;
    int view_failures=nt_gpu_cpu_view_selftest();
    if (view_failures) return view_failures; /* Preserve failed fixture ownership. */
    int context_failures=nt_gpu_context_selftest();
    if (context_failures) return context_failures;
    int capture_failures=nt_gpu_capture_selftest();
    if (capture_failures) return capture_failures;
    int execution_failures=nt_gpu_execution_selftest();
    if (execution_failures) return execution_failures;
    int checks=0,failures=0;
    HANDLE process_a=NULL,thread_a=NULL,process_b=NULL,thread_b=NULL;
    PVOID object_a=NULL,thread_object_a=NULL,object_b=NULL,thread_object_b=NULL;
    ULONG pid_a=0,pid_b=0,device_a=0,device_b=0,resource=0;
    OKMT_LUID kmt_luid={0};
    OKMT_HANDLE adapter_a=0,adapter_b=0,kmt_a=0,kmt_b=0;
    OKMT_HANDLE kmt_allocation=0,kmt_group_a=0,kmt_group_b=0;
    ULONG native_kmt_a=0;
    BOOL kmt_registered=FALSE;
    PVOID test_token=NULL;
    intel_gpu_memory_lease_t lease={0};
    intel_gpu_kmt_device_lease_t device_lease={0};
    intel_gpu_kmt_cpu_lease_t cpu_access={0},denied_cpu={0};
    D3DKMT_ALLOCATION_LEASE allocation_lease={0},denied_allocation={0};
    const intel_gpu_allocator_t allocator={nt_gpu_test_allocate,nt_gpu_test_validate,
        nt_gpu_test_release,nt_gpu_test_sync,nt_gpu_test_cpu_physical};
    memset(&memory,0,sizeof(memory)); memset(&kmt,0,sizeof(kmt));
    memset(&nt_gpu_test_ram,0,sizeof(nt_gpu_test_ram));
#define GPU_EXPECT(c,label) nt_process_test_expect((c),(label),&checks,&failures)
    serial_puts("[GPUPROC] starting NT device admission and exit test\n");
    /* The shell's previous PE token may already have been retired. Keep the
     * test independent of it, and reserve the lazy root identity first. */
    if (current_process()) test_token=advapi32_token_create_primary(NULL);
    GPU_EXPECT(test_token!=NULL,"private token for test process creation");
    if (!test_token) goto cleanup;
    NTSTATUS a=nt_process_create_child(&process_a,&thread_a,&pid_a,NULL,
        &object_a,&thread_object_a,FALSE,test_token);
    NTSTATUS b=nt_process_create_child(&process_b,&thread_b,&pid_b,NULL,
        &object_b,&thread_object_b,FALSE,test_token);
    advapi32_token_release(test_token); test_token=NULL;
    GPU_EXPECT(NT_SUCCESS(a) && NT_SUCCESS(b),"GPU admission has real NT process objects");
    if (!NT_SUCCESS(a) || !NT_SUCCESS(b)) goto cleanup;
    GPU_EXPECT(intel_gpu_memory_init(&memory,&allocator,&nt_gpu_test_ram,4,2),"GPU manager init");
    GPU_EXPECT(intel_gpu_memory_publish(&memory)==INTEL_GPU_BUFFER_OK,"GPU manager exit registration");
    if (!memory.registered) goto cleanup;
    ULONG rejected=0xaabbccdd;
    GPU_EXPECT(nt_process_open_gpu_device(0,&memory,&rejected)==STATUS_INVALID_PARAMETER &&
        rejected==0xaabbccdd,"zero process cannot open GPU device");
    GPU_EXPECT(nt_process_open_gpu_device(UINT32_MAX,&memory,&rejected)==STATUS_INVALID_CID &&
        rejected==0xaabbccdd,"missing process cannot open GPU device");
    uint64_t before,after;
    __asm__ volatile("pushfq; popq %0" : "=r"(before));
    a=nt_process_open_gpu_device(pid_a,&memory,&device_a);
    __asm__ volatile("pushfq; popq %0" : "=r"(after));
    GPU_EXPECT(NT_SUCCESS(a) && device_a && !((before^after)&(1ULL<<9)),
        "live process admitted with caller IRQ state preserved");
    GPU_EXPECT(nt_process_open_gpu_device(pid_b,&memory,&device_b)==STATUS_SUCCESS &&
        device_b!=device_a,"second process gets independent device");
    if (!device_a || !device_b) goto cleanup;
    if (ws_cpu_count>1) {
        NT_GPU_TEST_WORK work={.memory=&memory,.pid=pid_a,.device=0xaabbccdd};
        int task=smp_submit_any(nt_gpu_test_ap,&work,NULL);
        GPU_EXPECT(task>=0,"AP admission probe submitted");
        if (task>=0) {
            for (unsigned i=0;i<1000000;i++) {
                if (__atomic_load_n(&work.done,__ATOMIC_ACQUIRE)) break;
                __asm__ volatile("pause");
            }
            smp_wait(task);
            GPU_EXPECT(work.cpu!=0 && work.status==STATUS_NOT_SUPPORTED &&
                work.device==0xaabbccdd,"AP cannot inspect NT ownership or publish device");
            serial_puts("[GPUPROC] admission_probe_cpu="); serial_putdec(work.cpu); serial_puts("\n");
        }
    }
    GPU_EXPECT(intel_gpu_kmt_provider(&kmt,&memory,nt_gpu_test_wake,&nt_gpu_test_ram),
        "native KMT provider retains the published manager");
    if (!kmt.self) goto cleanup;
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
    kmt_registered=d3dkmt_register_memory_driver(&description,&kmt.device,&kmt.allocation,&kmt_luid)==STATUS_SUCCESS;
    GPU_EXPECT(kmt_registered,"temporary KMT lifetime entry without render capabilities");
    if (!kmt_registered) goto cleanup;
    GPU_EXPECT(d3dkmt_open_adapter(pid_a,kmt_luid,&adapter_a)==STATUS_SUCCESS &&
        d3dkmt_open_adapter(pid_b,kmt_luid,&adapter_b)==STATUS_SUCCESS,"process-owned KMT adapter handles");
    GPU_EXPECT(d3dkmt_create_device(pid_a,adapter_a,0,&kmt_a)==STATUS_SUCCESS &&
        d3dkmt_create_device(pid_b,adapter_b,0,&kmt_b)==STATUS_SUCCESS && kmt_a && kmt_b && kmt_a!=kmt_b,
        "KMT devices admitted by real NT ownership gate");
    if (!kmt_a || !kmt_b) goto cleanup;
    GPU_EXPECT(intel_gpu_kmt_acquire_device(pid_a,kmt_a,&device_lease)==STATUS_SUCCESS &&
        device_lease.memory==&memory,"retain native operation from process-owned KMT handle");
    native_kmt_a=device_lease.device;
    GPU_EXPECT(native_kmt_a!=0,"KMT cookie owns a retained native device");
    if (!native_kmt_a) goto cleanup;
    GPU_EXPECT(d3dkmt_destroy_device(pid_b,kmt_a)==STATUS_INVALID_PARAMETER,"KMT owner isolation");
    GPU_EXPECT(d3dkmt_close_adapter(pid_a,adapter_a)==STATUS_SUCCESS,"KMT device survives originating adapter close");
    intel_umd_buffer_description_t allocation_description={INTEL_UMD_BUFFER_MAGIC,INTEL_UMD_BUFFER_VERSION,5000,0,0};
    D3DKMT_PRIVATE_ALLOCATION private_allocation={.bytes=sizeof(allocation_description)};
    for (unsigned invalid=0;invalid<4;invalid++) {
        intel_umd_buffer_description_t malformed=allocation_description;
        if (invalid==0) malformed.magic=0;
        if (invalid==1) malformed.bytes=0;
        if (invalid==2) malformed.flags=1;
        if (invalid==3) malformed.bytes=INTEL_GPU_BUFFER_MAX_BYTES+1;
        memcpy(private_allocation.data,&malformed,sizeof(malformed));
        OKMT_HANDLE output=0xfeed;
        GPU_EXPECT(d3dkmt_create_private_allocations(pid_a,kmt_a,1,&private_allocation,&output)==
            (invalid ? STATUS_INVALID_PARAMETER : STATUS_NOT_SUPPORTED) && output==0xfeed && !memory.pages,
            "invalid native UMD buffer description cannot allocate or publish");
    }
    memcpy(private_allocation.data,&allocation_description,sizeof(allocation_description));
    GPU_EXPECT(d3dkmt_create_private_resource_allocations(pid_a,kmt_a,0,TRUE,0,NULL,&kmt_group_a,NULL)==STATUS_SUCCESS &&
        kmt_group_a && !memory.pages,"empty KMT resource retains device without allocating PMM pages");
    GPU_EXPECT(d3dkmt_create_private_resource_allocations(pid_b,kmt_b,0,TRUE,0,NULL,&kmt_group_b,NULL)==STATUS_SUCCESS &&
        kmt_group_b && kmt_group_b!=kmt_group_a,"independent device-local empty resource");
    if (!kmt_group_a || !kmt_group_b) goto cleanup;
    GPU_EXPECT(d3dkmt_destroy_resource(pid_b,kmt_a,kmt_group_a)==STATUS_INVALID_PARAMETER,
        "resource group enforces NT process ownership");
    GPU_EXPECT(d3dkmt_close_adapter(pid_a,kmt_group_a)==STATUS_INVALID_PARAMETER,
        "resource handle cannot alias an adapter");
    OKMT_HANDLE published_group=0;
    GPU_EXPECT(d3dkmt_create_private_resource_allocations(pid_a,kmt_a,kmt_group_a,FALSE,1,
        &private_allocation,&published_group,&kmt_allocation)==STATUS_SUCCESS && published_group==kmt_group_a,
        "native UMD buffer description appends two PMM pages to the empty KMT resource");
    if (!kmt_allocation) goto cleanup;
    GPU_EXPECT(d3dkmt_acquire_allocation(pid_b,kmt_a,kmt_allocation,&denied_allocation)==STATUS_INVALID_PARAMETER,
        "allocation handle rejects a different process");
    GPU_EXPECT(d3dkmt_acquire_allocation(pid_a,kmt_b,kmt_allocation,&denied_allocation)==STATUS_INVALID_PARAMETER,
        "allocation handle rejects a different device");
    GPU_EXPECT(d3dkmt_acquire_allocation(pid_a,kmt_a,kmt_allocation,&allocation_lease)==STATUS_SUCCESS &&
        allocation_lease.device_cookie==native_kmt_a && allocation_lease.cookie && allocation_lease.cookie<=UINT32_MAX,
        "retain allocation and its parent through KMT ownership");
    resource=(ULONG)allocation_lease.cookie;
    if (!resource) goto cleanup;
    GPU_EXPECT(intel_gpu_kmt_acquire_cpu(pid_a,kmt_a,kmt_allocation,&cpu_access)==STATUS_SUCCESS &&
        cpu_access.access.cpu && cpu_access.access.bytes==5000,"KMT CPU access owns real PMM backing");
    if (!cpu_access.access.self) goto cleanup;
    GPU_EXPECT(intel_gpu_kmt_acquire_cpu(pid_a,kmt_a,kmt_allocation,&denied_cpu)==D3DKMT_STATUS_DEVICE_BUSY &&
        !denied_cpu.reference.self && !denied_cpu.access.self,"second CPU access cannot share exclusive backing");
    GPU_EXPECT(intel_gpu_memory_acquire(&memory,pid_a,native_kmt_a,resource,&lease)==INTEL_GPU_BUFFER_BUSY &&
        !lease.self,"CPU ownership rejects GPU-capable pins");
    ((BYTE *)cpu_access.access.cpu)[0]=0x6d; ((BYTE *)cpu_access.access.cpu)[4999]=0x7a;
    GPU_EXPECT(intel_gpu_kmt_release_cpu(&cpu_access)==STATUS_SUCCESS && !cpu_access.access.self &&
        !cpu_access.reference.self && nt_gpu_test_ram.wakes==1,"release CPU access flushes cache and signals owner");
    unsigned cpu_wakes=nt_gpu_test_ram.wakes;
    GPU_EXPECT(intel_gpu_memory_acquire(&memory,pid_a,native_kmt_a,resource,&lease)==INTEL_GPU_BUFFER_OK,
        "pin allocation through process exit");
    if (!lease.self) goto cleanup;
    GPU_EXPECT(((BYTE *)lease.buffer->cpu)[0]==0x6d && ((BYTE *)lease.buffer->cpu)[4999]==0x7a,
        "native pin observes bytes written through exclusive CPU access");
    GPU_EXPECT(intel_gpu_kmt_acquire_cpu(pid_a,kmt_a,kmt_allocation,&denied_cpu)==D3DKMT_STATUS_DEVICE_BUSY &&
        !denied_cpu.reference.self && !denied_cpu.access.self,"GPU-capable pin excludes new CPU access");
    GPU_EXPECT(d3dkmt_destroy_resource(pid_a,kmt_a,kmt_group_a)==STATUS_SUCCESS &&
        memory.pages==2 && nt_gpu_test_ram.wakes==cpu_wakes,"resource close retains pinned native allocation and its group");
    OKMT_HANDLE rejected_allocation=0xfeed;
    published_group=0xfeed;
    GPU_EXPECT(d3dkmt_create_private_resource_allocations(pid_a,kmt_a,kmt_group_a,FALSE,1,
        &private_allocation,&published_group,&rejected_allocation)==STATUS_INVALID_PARAMETER &&
        published_group==0xfeed && rejected_allocation==0xfeed,"closing resource rejects append without publishing");
    /* Exercise the common teardown entry, not just its graphics notification.
     * It closes admission before GDI and native manager resource teardown. */
    nt_vm_release_process(pid_a);
    GPU_EXPECT(d3dkmt_destroy_device(pid_a,kmt_a)==STATUS_INVALID_PARAMETER && nt_gpu_test_ram.wakes==cpu_wakes &&
        d3dkmt_collect_devices()==D3DKMT_STATUS_DEVICE_BUSY,
        "VM exit denies new KMT work while retaining the active operation");
    GPU_EXPECT(nt_process_open_gpu_device(pid_a,&memory,&rejected)==STATUS_PROCESS_IS_TERMINATING &&
        rejected==0xaabbccdd,"VM teardown rejects new GPU devices");
    GPU_EXPECT(intel_gpu_memory_create(&memory,pid_a,device_a,1,&rejected)==INTEL_GPU_BUFFER_INVALID &&
        rejected==0xaabbccdd,"VM teardown closes existing device admission");
    GPU_EXPECT(intel_gpu_memory_create(&memory,pid_a,native_kmt_a,1,&rejected)==INTEL_GPU_BUFFER_INVALID &&
        rejected==0xaabbccdd,"VM teardown closes native KMT device admission");
    GPU_EXPECT(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_BUSY &&
        memory.pages==2 && !nt_gpu_test_ram.releases,"VM exit retains pinned GPU allocation");
    GPU_EXPECT(intel_gpu_memory_release(&lease)==INTEL_GPU_BUFFER_OK,"release post-exit lease");
    GPU_EXPECT(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_BUSY && memory.pages==2 &&
        !nt_gpu_test_ram.releases,"native cookie stays valid until retained KMT allocation is released");
    GPU_EXPECT(d3dkmt_release_allocation(&allocation_lease)==STATUS_SUCCESS && nt_gpu_test_ram.wakes==cpu_wakes+1,
        "last KMT allocation operation retires native ownership and requests cleanup");
    GPU_EXPECT(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_OK && !memory.pages &&
        nt_gpu_test_ram.releases==1,"worker reclaims exited allocation and quota");
    GPU_EXPECT(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_BUSY,
        "empty native device remains retained by its KMT operation");
    GPU_EXPECT(intel_gpu_kmt_release_device(&device_lease)==STATUS_SUCCESS && nt_gpu_test_ram.wakes==cpu_wakes+2,
        "last KMT operation retires native reference and requests cleanup");
    GPU_EXPECT(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_OK,
        "native device reclaimed after operation completion");
    GPU_EXPECT(nt_process_open_gpu_device(pid_a,&memory,&rejected)==STATUS_PROCESS_IS_TERMINATING &&
        rejected==0xaabbccdd,"reclaimed owner cannot reopen during NT teardown");
    GPU_EXPECT(d3dkmt_open_adapter(pid_a,kmt_luid,&adapter_a)==STATUS_SUCCESS &&
        d3dkmt_create_device(pid_a,adapter_a,0,&rejected)==STATUS_PROCESS_IS_TERMINATING &&
        rejected==0xaabbccdd,"KMT cannot reopen an exited NT process");
    GPU_EXPECT(d3dkmt_close_adapter(pid_a,adapter_a)==STATUS_SUCCESS,"close rejected KMT reopen reference");
    GPU_EXPECT(d3dkmt_destroy_device(pid_b,kmt_b)==STATUS_SUCCESS && nt_gpu_test_ram.wakes==cpu_wakes+3,
        "explicit KMT close requests independent native cleanup");
    ULONG extra=0;
    GPU_EXPECT(nt_process_open_gpu_device(pid_b,&memory,&extra)==STATUS_SUCCESS,
        "unrelated live NT owner still admitted");
    nt_process_complete_child(object_b,thread_object_b,STATUS_SUCCESS);
    object_b=NULL; thread_object_b=NULL;
    GPU_EXPECT(nt_process_open_gpu_device(pid_b,&memory,&rejected)==STATUS_PROCESS_IS_TERMINATING &&
        rejected==0xaabbccdd,"inactive NT object cannot reopen GPU device");
cleanup:
    advapi32_token_release(test_token);
    if (pid_a) nt_vm_release_process(pid_a);
    if (pid_b) nt_vm_release_process(pid_b);
    if (cpu_access.access.self)
        GPU_EXPECT(intel_gpu_kmt_release_cpu(&cpu_access)==STATUS_SUCCESS,"cleanup retained CPU access");
    if (denied_cpu.access.self)
        GPU_EXPECT(intel_gpu_kmt_release_cpu(&denied_cpu)==STATUS_SUCCESS,"cleanup unexpected CPU access");
    if (lease.self) GPU_EXPECT(intel_gpu_memory_release(&lease)==INTEL_GPU_BUFFER_OK,"cleanup retained lease");
    if (allocation_lease.self)
        GPU_EXPECT(d3dkmt_release_allocation(&allocation_lease)==STATUS_SUCCESS,"cleanup retained KMT allocation");
    if (device_lease.reference.self)
        GPU_EXPECT(intel_gpu_kmt_release_device(&device_lease)==STATUS_SUCCESS,"cleanup retained KMT operation");
    if (object_a) nt_process_complete_child(object_a,thread_object_a,STATUS_SUCCESS);
    if (object_b) nt_process_complete_child(object_b,thread_object_b,STATUS_SUCCESS);
    if (thread_a) (void)handle_close(&g_handle_table,thread_a);
    if (process_a) (void)handle_close(&g_handle_table,process_a);
    if (thread_b) (void)handle_close(&g_handle_table,thread_b);
    if (process_b) (void)handle_close(&g_handle_table,process_b);
    if (kmt_registered) {
        NTSTATUS removed=d3dkmt_unregister_adapter(kmt_luid);
        GPU_EXPECT(removed==STATUS_SUCCESS,"unregister KMT lifetime provider after callbacks");
        if (removed==STATUS_SUCCESS) kmt_registered=FALSE;
    }
    if (memory.initialized) {
        intel_gpu_memory_shutdown(&memory);
        for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
            if (intel_gpu_memory_collect(&memory)!=INTEL_GPU_BUFFER_OK) { failures++; break; }
        if (memory.registered)
            GPU_EXPECT(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_OK,"unregister retired test manager");
    }
    GPU_EXPECT(!memory.pages && !nt_gpu_test_ram.phys &&
        nt_gpu_test_ram.allocations==nt_gpu_test_ram.releases,"all NT test allocations reclaimed");
    serial_puts("[GPUPROC] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
    if (!kmt_registered && !memory.registered && !memory.pages && !nt_gpu_test_ram.phys)
        __atomic_store_n(&busy,0,__ATOMIC_RELEASE);
#undef GPU_EXPECT
    return failures;
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
    PVOID test_token = current_process() ? advapi32_token_create_primary(NULL) : NULL;

    serial_puts("[PROCTEST] starting process identity contract test\n");
    nt_process_test_expect(test_token != NULL, "private process-test token",
                           &checks, &failures);
    if (!test_token) goto cleanup;
    NTSTATUS status_a = nt_process_create_child(
        &process_a, &thread_a, &pid_a, &tid_a, &object_a, &thread_object_a,
        FALSE, test_token);
    NTSTATUS status_b = nt_process_create_child(
        &process_b, &thread_b, &pid_b, &tid_b, &object_b, &thread_object_b,
        FALSE, test_token);
    advapi32_token_release(test_token); test_token = NULL;
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
    advapi32_token_release(test_token);
    if (object_a)
        nt_process_complete_child(object_a, thread_object_a, STATUS_SUCCESS);
    if (object_b)
        nt_process_complete_child(object_b, thread_object_b, STATUS_SUCCESS);
    if (thread_a) (void)handle_close(&g_handle_table, thread_a);
    if (process_a) (void)handle_close(&g_handle_table, process_a);
    if (thread_b) (void)handle_close(&g_handle_table, thread_b);
    if (process_b) (void)handle_close(&g_handle_table, process_b);
    failures += nt_process_graphics_selftest();
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
void nt_thread_park_primary(PVOID object, NTSTATUS exit_status)
{
    THREAD_OBJECT *thread = object;
    if (!thread) return;
    thread->primary_parked = TRUE;
    nt_thread_stop_cpu_accounting(thread);
    thread->exit_status = exit_status;
    nt_thread_clear_token(thread);
    __atomic_store_n(&thread->state, THREAD_TERMINATED, __ATOMIC_RELEASE);
    ntsync_notify_object(OBJ_TYPE_THREAD, thread);
}

void win32_mark_thread_terminated(PVOID object, NTSTATUS exit_status)
{
    if (object) {
        THREAD_OBJECT *th = (THREAD_OBJECT *)object;
        nt_thread_stop_cpu_accounting(th);
        th->state = THREAD_TERMINATED;
        nt_thread_clear_token(th);
        th->exit_status = exit_status;
        ntsync_notify_object(OBJ_TYPE_THREAD, th);
        nt_process_release_thread_object(object);
    }
}
