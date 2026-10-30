/* Job lifetime and private-commit limits. Win32 processes run on the BSP;
 * interrupts protect short metadata transactions, never guest memory or exit. */
#include "ntjob.h"
#include "ntprocess.h"
#include "ntsyscall.h"
#include "kernel32_shim.h"
#include "handle.h"

extern void *kcalloc(uint64_t count, uint64_t size);
extern void kfree(void *pointer);
extern DWORD win32_current_process_id(void);
extern BOOL win32_terminate_process_id(DWORD pid, NTSTATUS status);
extern HANDLE_TABLE g_handle_table;

#define JOB_ACTIVE_LIMIT 0x0008U
#define JOB_PROCESS_MEMORY 0x0100U
#define JOB_TOTAL_MEMORY 0x0200U
#define JOB_DIE_ON_UNHANDLED 0x0400U
#define JOB_BREAKAWAY 0x0800U
#define JOB_SILENT_BREAKAWAY 0x1000U
#define JOB_KILL_ON_CLOSE 0x2000U
#define JOB_SUPPORTED (JOB_ACTIVE_LIMIT | JOB_PROCESS_MEMORY | JOB_TOTAL_MEMORY | \
                       JOB_DIE_ON_UNHANDLED | \
                       JOB_BREAKAWAY | JOB_SILENT_BREAKAWAY | JOB_KILL_ON_CLOSE)
#define JOB_UI_SUPPORTED (NT_JOB_UI_HANDLES | NT_JOB_UI_READCLIPBOARD | NT_JOB_UI_WRITECLIPBOARD | \
                          NT_JOB_UI_SYSTEMPARAMETERS | NT_JOB_UI_DISPLAYSETTINGS | \
                          NT_JOB_UI_GLOBALATOMS | NT_JOB_UI_DESKTOP | NT_JOB_UI_EXITWINDOWS)
/* DIE_ON_UNHANDLED uses the existing PE32/PE64 exception backend: handlers
 * run first, then only the faulting process exits with its exception status.
 * There is no WER/error-dialog backend to suppress. Keep the flag as job state;
 * it must not bypass SEH/VEH, kill peers, or change the process error-mode bits. */

typedef struct job_user_grant {
    struct job_user_grant *next;
    ULONG owner;
    UINT kind;
    ULONG_PTR value;
} JOB_USER_GRANT;

typedef struct job_object {
    struct job_object *next;
    JOB_USER_GRANT *user_grants;
    ULONG references, handles;
    DWORD flags, active_limit, active, total, terminated;
    DWORD ui_limits;
    ULONGLONG atom_namespace;
    SIZE_T process_limit, job_limit, committed, peak_process, peak_job;
    BOOL terminating;
    PVOID completion_port;
    HANDLE completion_reference;
    ULONG_PTR completion_key;
} JOB_OBJECT;

typedef struct job_process {
    struct job_process *next;
    ULONG pid;
    SIZE_T committed;
    JOB_OBJECT *job;
} JOB_PROCESS;

static JOB_PROCESS *job_processes;
static JOB_OBJECT *job_objects;
static ULONG g_job_live_objects;
static ULONG g_job_user_grants;
static ULONGLONG next_job_atom_namespace = 1;

static uint64_t job_lock(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void job_unlock(uint64_t flags)
{
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

static JOB_PROCESS *job_process(ULONG pid)
{
    for (JOB_PROCESS *p = job_processes; p; p = p->next)
        if (p->pid == pid) return p;
    return NULL;
}

static void job_retain(JOB_OBJECT *job)
{
    __atomic_add_fetch(&job->references, 1, __ATOMIC_RELAXED);
}

/* Metadata callers hold job_lock, so the association cannot be detached while
 * a notification uses its retained port. Delivery is best-effort, as on NT. */
static void job_notify(JOB_OBJECT *job, DWORD message, ULONG pid)
{
    if (job->completion_port)
        k32_iocp_post_job(job->completion_port, message, job->completion_key, pid);
}

static void job_release(JOB_OBJECT *job)
{
    if (job && !__atomic_sub_fetch(&job->references, 1, __ATOMIC_ACQ_REL)) {
        uint64_t flags = job_lock();
        JOB_OBJECT **link = &job_objects;
        while (*link && *link != job) link = &(*link)->next;
        if (*link) *link = job->next;
        JOB_USER_GRANT *grants = job->user_grants;
        for (JOB_USER_GRANT *g = grants; g; g = g->next) g_job_user_grants--;
        job_unlock(flags);
        while (grants) {
            JOB_USER_GRANT *next = grants->next;
            kfree(grants);
            grants = next;
        }
        k32_iocp_release_job(job->completion_reference);
        k32_atom_release_job(job->atom_namespace);
        __atomic_sub_fetch(&g_job_live_objects, 1, __ATOMIC_RELAXED);
        kfree(job);
    }
}

void nt_job_handle_retain(PVOID object)
{
    JOB_OBJECT *job = object;
    job_retain(job);
    __atomic_add_fetch(&job->handles, 1, __ATOMIC_RELAXED);
}

static BOOL job_terminate(JOB_OBJECT *job, DWORD code)
{
    ULONG previous = 0;
    BOOL success = TRUE;
    uint64_t flags = job_lock();
    job->terminating = job->active != 0;
    job_unlock(flags);
    /* PIDs are monotonic. Each request can yield or complete teardown, so do
     * not keep member pointers or a metadata lock across the call. */
    for (;;) {
        ULONG pid = 0;
        flags = job_lock();
        for (JOB_PROCESS *p = job_processes; p; p = p->next)
            if (p->job == job && p->pid > previous && (!pid || p->pid < pid))
                pid = p->pid;
        job_unlock(flags);
        if (!pid) break;
        previous = pid;
        if (!win32_terminate_process_id(pid, (NTSTATUS)code)) success = FALSE;
    }
    return success;
}

void nt_job_handle_release(PVOID object)
{
    JOB_OBJECT *job = object;
    uint64_t flags = job_lock();
    BOOL kill = !__atomic_sub_fetch(&job->handles, 1, __ATOMIC_ACQ_REL) &&
                (job->flags & JOB_KILL_ON_CLOSE) && !job->terminating;
    job_unlock(flags);
    if (kill) (void)job_terminate(job, 0);
    job_release(job);
}

BOOL nt_job_process_create(ULONG pid)
{
    JOB_PROCESS *p = kcalloc(1, sizeof(*p));
    if (!p) return FALSE;
    p->pid = pid;
    uint64_t flags = job_lock();
    if (job_process(pid)) {
        job_unlock(flags);
        kfree(p);
        return TRUE;
    }
    p->next = job_processes;
    job_processes = p;
    job_unlock(flags);
    return TRUE;
}

static BOOL job_abnormal_exit(NTSTATUS status)
{
    switch ((ULONG)status) {
    case 0x80000001U: case 0x80000002U: case 0x80000003U: case 0x80000004U:
    case 0xc0000005U: case 0xc0000006U: case 0xc000001dU:
    case 0xc0000025U: case 0xc0000026U:
    case 0xc000008cU: case 0xc000008dU: case 0xc000008eU: case 0xc000008fU:
    case 0xc0000090U: case 0xc0000091U: case 0xc0000092U: case 0xc0000093U:
    case 0xc0000094U: case 0xc0000095U: case 0xc0000096U:
    case 0xc00000fdU: case 0xc000013aU: case 0xc00002b4U: case 0xc00002b5U:
    case 0xc00002c9U:
        return TRUE;
    default:
        return FALSE;
    }
}

void nt_job_process_exit(ULONG pid, NTSTATUS status)
{
    uint64_t flags = job_lock();
    JOB_PROCESS **link = &job_processes;
    while (*link && (*link)->pid != pid) link = &(*link)->next;
    JOB_PROCESS *p = *link;
    JOB_OBJECT *job = p ? p->job : NULL;
    if (p) *link = p->next;
    if (job) {
        job->committed -= p->committed;
        job_notify(job, job_abnormal_exit(status) ? 8 : 7, pid);
        if (!--job->active) {
            job->terminating = FALSE;
            job_notify(job, 4, 0); /* ACTIVE_PROCESS_ZERO */
        }
    }
    job_unlock(flags);
    job_release(job);
    kfree(p);
}

static DWORD job_attach(JOB_OBJECT *job, JOB_PROCESS *p)
{
    if (p->job) return p->job == job ? 5 : 50; /* Nested jobs not implemented. */
    if (job->terminating) return 5;
    if ((job->flags & JOB_ACTIVE_LIMIT) && job->active >= job->active_limit) {
        job->terminated++;
        job_notify(job, 3, 0); /* ACTIVE_PROCESS_LIMIT */
        return 1816; /* ERROR_NOT_ENOUGH_QUOTA; caller terminates the target. */
    }
    p->job = job;
    job_retain(job);
    job->active++;
    job->total++;
    job->committed += p->committed;
    if (p->committed > job->peak_process) job->peak_process = p->committed;
    if (job->committed > job->peak_job) job->peak_job = job->committed;
    job_notify(job, 6, p->pid); /* NEW_PROCESS */
    return 0;
}

NTSTATUS nt_job_inherit(ULONG parent, ULONG child, DWORD creation_flags)
{
    uint64_t flags = job_lock();
    JOB_PROCESS *source = job_process(parent), *target = job_process(child);
    JOB_OBJECT *job = source ? source->job : NULL;
    DWORD error = 0;
    if (job) {
        if (job->flags & JOB_SILENT_BREAKAWAY) {
            /* Every new child is outside this job. */
        } else if (creation_flags & 0x01000000U) {
            if (!(job->flags & JOB_BREAKAWAY)) error = 5;
        } else {
            error = target ? job_attach(job, target) : 8;
        }
    }
    job_unlock(flags);
    return error ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}

BOOL nt_job_charge_commit(ULONG pid, SIZE_T bytes)
{
    uint64_t flags = job_lock();
    JOB_PROCESS *p = job_process(pid);
    JOB_OBJECT *job = p ? p->job : NULL;
    BOOL allowed = TRUE;
    if (p) {
        allowed = bytes <= (SIZE_T)-1 - p->committed;
        if (job && allowed) {
            if ((job->flags & JOB_PROCESS_MEMORY) &&
                p->committed + bytes > job->process_limit) {
                job_notify(job, 9, pid); /* PROCESS_MEMORY_LIMIT */
                allowed = FALSE;
            } else if (bytes > (SIZE_T)-1 - job->committed) {
                allowed = FALSE;
            } else if ((job->flags & JOB_TOTAL_MEMORY) &&
                       job->committed + bytes > job->job_limit) {
                job_notify(job, 10, pid); /* JOB_MEMORY_LIMIT */
                allowed = FALSE;
            }
        }
        if (allowed) {
            p->committed += bytes;
            if (job) {
                job->committed += bytes;
                if (p->committed > job->peak_process) job->peak_process = p->committed;
                if (job->committed > job->peak_job) job->peak_job = job->committed;
            }
        }
    }
    job_unlock(flags);
    return allowed;
}

void nt_job_return_commit(ULONG pid, SIZE_T bytes)
{
    uint64_t flags = job_lock();
    JOB_PROCESS *p = job_process(pid);
    if (p) {
        if (bytes > p->committed) bytes = p->committed;
        p->committed -= bytes;
        if (p->job) p->job->committed -= bytes;
    }
    job_unlock(flags);
}

DWORD nt_job_ui_restrictions(ULONG pid)
{
    uint64_t flags = job_lock();
    JOB_PROCESS *p = job_process(pid);
    DWORD restrictions = p && p->job ? p->job->ui_limits : 0;
    job_unlock(flags);
    return restrictions;
}

/* Broadcasts/hooks use membership only; an explicit object grant does not
 * extend a job's broadcast domain. Callers protect the USER object's lifetime. */
static BOOL job_user_same_domain_locked(ULONG caller, ULONG owner)
{
    JOB_PROCESS *source = job_process(caller), *target = job_process(owner);
    JOB_OBJECT *job = source ? source->job : NULL;
    return !job || !(job->ui_limits & NT_JOB_UI_HANDLES) || caller == owner ||
           (target && target->job == job);
}

BOOL nt_job_user_same_domain(ULONG caller, ULONG owner)
{
    uint64_t flags = job_lock();
    BOOL allowed = job_user_same_domain_locked(caller, owner);
    job_unlock(flags);
    return allowed;
}

BOOL nt_job_user_access(ULONG caller, ULONG owner, UINT kind, ULONG_PTR value)
{
    uint64_t flags = job_lock();
    BOOL allowed = !owner || job_user_same_domain_locked(caller, owner);
    JOB_PROCESS *source = allowed ? NULL : job_process(caller);
    if (source && source->job)
        for (JOB_USER_GRANT *g = source->job->user_grants; g; g = g->next)
            if (g->kind == kind && g->value == value && g->owner == owner) {
                allowed = TRUE;
                break;
            }
    job_unlock(flags);
    return allowed;
}

void nt_job_user_destroy(UINT kind, ULONG_PTR value)
{
    JOB_USER_GRANT *retired = NULL;
    uint64_t flags = job_lock();
    for (JOB_OBJECT *job = job_objects; job; job = job->next) {
        JOB_USER_GRANT **link = &job->user_grants;
        while (*link) {
            JOB_USER_GRANT *g = *link;
            if (g->kind != kind || g->value != value) { link = &g->next; continue; }
            *link = g->next;
            g->next = retired;
            retired = g;
            g_job_user_grants--;
        }
    }
    job_unlock(flags);
    while (retired) {
        JOB_USER_GRANT *next = retired->next;
        kfree(retired);
        retired = next;
    }
}

ULONGLONG nt_job_global_atom_namespace(ULONG pid)
{
    uint64_t flags = job_lock();
    JOB_PROCESS *p = job_process(pid);
    JOB_OBJECT *job = p ? p->job : NULL;
    ULONGLONG id = job && (job->ui_limits & NT_JOB_UI_GLOBALATOMS)
                 ? job->atom_namespace : 0;
    job_unlock(flags);
    return id;
}

static JOB_OBJECT *job_reference(HANDLE handle, DWORD access, BOOL current)
{
    JOB_OBJECT *job = NULL;
    DWORD error = 6;
    uint64_t flags = job_lock();
    if (!handle && current) {
        JOB_PROCESS *p = job_process(win32_current_process_id());
        if (p) job = p->job;
        error = 5;
    } else {
        ACCESS_MASK granted = 0;
        if (NT_SUCCESS(handle_lookup_access_for_process(&g_handle_table, handle,
                win32_current_process_id(), OBJ_TYPE_JOB, (PVOID *)&job, &granted)) &&
            !(granted & GENERIC_ALL) && (granted & access) != access) {
            job = NULL;
            error = 5;
        }
    }
    if (job) job_retain(job);
    job_unlock(flags);
    if (!job) SetLastError(error);
    return job;
}

static BOOL job_result(JOB_OBJECT *job, DWORD error)
{
    job_release(job);
    SetLastError(error);
    return error == 0;
}

BOOL nt_job_user_grant(HANDLE handle, ULONG owner, UINT kind, ULONG_PTR value,
                       BOOL grant)
{
    DWORD previous_error = GetLastError();
    JOB_OBJECT *job = job_reference(handle, 2, FALSE); /* JOB_OBJECT_SET_ATTRIBUTES */
    if (!job) { SetLastError(87); return FALSE; }
    JOB_USER_GRANT *spare = grant ? kcalloc(1, sizeof(*spare)) : NULL;
    JOB_USER_GRANT *retired = NULL;
    DWORD error = 0;
    uint64_t flags = job_lock();
    if (!(job->ui_limits & NT_JOB_UI_HANDLES) || !value || !kind) error = 87;
    else {
        JOB_USER_GRANT **link = &job->user_grants;
        while (*link && ((*link)->kind != kind || (*link)->value != value ||
                        (*link)->owner != owner)) link = &(*link)->next;
        if (grant && !*link) {
            if (!spare) error = 8;
            else {
                spare->owner = owner;
                spare->kind = kind;
                spare->value = value;
                *link = spare;
                spare = NULL;
                g_job_user_grants++;
            }
        } else if (!grant && *link) {
            retired = *link;
            *link = retired->next;
            g_job_user_grants--;
        }
    }
    job_unlock(flags);
    kfree(spare);
    kfree(retired);
    job_release(job);
    SetLastError(error ? error : previous_error);
    return error == 0;
}

static BOOL job_buffer(PCVOID buffer, SIZE_T size, BOOL write)
{
    return buffer && nt_vm_user_range_accessible((ULONG_PTR)buffer, size, write);
}

HANDLE WINAPI CreateJobObjectW(PVOID attributes, PCWSTR name)
{
    /* Do not silently discard requested security or object namespace state. */
    if (name) { SetLastError(50); return NULL; }
    BOOL inherit = FALSE;
    if (attributes) {
        DWORD size = g_compat32_mode ? 12 : 24;
        if (!job_buffer(attributes, size, FALSE)) { SetLastError(998); return NULL; }
        const BYTE *a = attributes;
        ULONG_PTR descriptor = g_compat32_mode ? *(const DWORD *)(a + 4)
                                               : *(const ULONGLONG *)(a + 8);
        if (descriptor) { SetLastError(50); return NULL; }
        inherit = *(const DWORD *)(a + (g_compat32_mode ? 8 : 16)) != 0;
    }
    JOB_OBJECT *job = kcalloc(1, sizeof(*job));
    if (!job) { SetLastError(8); return NULL; }
    job->references = 1;
    uint64_t flags = job_lock();
    job->next = job_objects;
    job_objects = job;
    job_unlock(flags);
    __atomic_add_fetch(&g_job_live_objects, 1, __ATOMIC_RELAXED);
    HANDLE handle = NULL;
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_JOB, GENERIC_ALL, job, &handle);
    if (NT_SUCCESS(status) && inherit)
        handle_update_flags_for_process(&g_handle_table, handle, win32_current_process_id(),
            HANDLE_USER_FLAG_INHERIT, HANDLE_USER_FLAG_INHERIT);
    job_release(job);
    SetLastError(NT_SUCCESS(status) ? 0 : 8);
    return handle;
}

HANDLE WINAPI CreateJobObjectA(PVOID attributes, PCSTR name)
{
    if (name) { SetLastError(50); return NULL; }
    return CreateJobObjectW(attributes, NULL);
}

BOOL WINAPI SetInformationJobObject(HANDLE handle, DWORD cls, PCVOID info, DWORD size)
{
    DWORD previous_error = GetLastError();
    JOB_OBJECT *job = job_reference(handle, 2, FALSE);
    if (!job) return FALSE;
    if (cls == 7) { /* JobObjectAssociateCompletionPortInformation */
        DWORD bytes = g_compat32_mode ? 8 : 16;
        if (g_compat32_mode ? size < bytes : size != bytes) return job_result(job, 24);
        if (!job_buffer(info, bytes, FALSE)) return job_result(job, 998);
        const BYTE *input = info;
        ULONG_PTR key = g_compat32_mode ? *(const DWORD *)input : *(const ULONGLONG *)input;
        HANDLE handle_port = (HANDLE)(g_compat32_mode ? (ULONG_PTR)*(const DWORD *)(input + 4)
                                                     : *(const ULONGLONG *)(input + 8));
        PVOID port = NULL;
        HANDLE reference = NULL, previous = NULL;
        NTSTATUS status = handle_port ? k32_iocp_reference_job(handle_port, &port, &reference)
                                     : STATUS_SUCCESS;
        if (!NT_SUCCESS(status))
            return job_result(job, status == STATUS_ACCESS_DENIED ? 5 :
                                   status == STATUS_INSUFFICIENT_RESOURCES ? 8 : 6);
        uint64_t flags = job_lock();
        DWORD error = 0;
        if (port && job->completion_port) error = 87;
        else {
            previous = job->completion_reference;
            job->completion_port = port;
            job->completion_reference = reference;
            job->completion_key = key;
            reference = NULL;
            if (port)
                for (JOB_PROCESS *p = job_processes; p; p = p->next)
                    if (p->job == job) job_notify(job, 6, p->pid);
        }
        job_unlock(flags);
        k32_iocp_release_job(previous);
        k32_iocp_release_job(reference);
        BOOL result = job_result(job, error);
        if (result) SetLastError(previous_error);
        return result;
    }
    DWORD required = cls == 4 ? 4 : cls == 2 ? (g_compat32_mode ? 48 : 64) :
                     cls == 9 ? (g_compat32_mode ? 112 : 144) : 0;
    if (!required) return job_result(job, 50);
    if (size != required) return job_result(job, 24);
    if (!job_buffer(info, required, FALSE)) return job_result(job, 998);
    BYTE copy[144] __attribute__((aligned(8)));
    for (DWORD i = 0; i < required; i++) copy[i] = ((const BYTE *)info)[i];
    if (cls == 4) {
        DWORD mask = *(DWORD *)copy;
        if (mask & ~0xffU) return job_result(job, 87);
        /* Every advertised bit has a USER enforcement path. */
        if (mask & ~JOB_UI_SUPPORTED) return job_result(job, 50);
        uint64_t flags = job_lock();
        if ((mask & NT_JOB_UI_GLOBALATOMS) && !job->atom_namespace) {
            if (!next_job_atom_namespace) {
                job_unlock(flags);
                return job_result(job, 8);
            }
            job->atom_namespace = next_job_atom_namespace++;
        }
        job->ui_limits = mask;
        job_unlock(flags);
        job_release(job);
        SetLastError(previous_error);
        return TRUE;
    }
    DWORD limits = *(DWORD *)(copy + 16);
    if (limits & ~0x7ffffU) return job_result(job, 87);
    if (limits & ~JOB_SUPPORTED) return job_result(job, 50);
    if (cls == 2 && (limits & ~JOB_ACTIVE_LIMIT)) return job_result(job, 87);
    DWORD active = *(DWORD *)(copy + (g_compat32_mode ? 28 : 40));
    SIZE_T process_memory = 0, job_memory = 0;
    if (cls == 9) {
        process_memory = g_compat32_mode ? *(DWORD *)(copy + 96) : *(ULONGLONG *)(copy + 112);
        job_memory = g_compat32_mode ? *(DWORD *)(copy + 100) : *(ULONGLONG *)(copy + 120);
    }
    if (((limits & JOB_PROCESS_MEMORY) && !process_memory) ||
        ((limits & JOB_TOTAL_MEMORY) && !job_memory)) return job_result(job, 87);
    uint64_t flags = job_lock();
    job->flags = cls == 2 ? (job->flags & ~0xffU) | limits : limits;
    job->active_limit = (limits & JOB_ACTIVE_LIMIT) ? active : 0;
    if (cls == 9) {
        job->process_limit = (limits & JOB_PROCESS_MEMORY) ? process_memory : 0;
        job->job_limit = (limits & JOB_TOTAL_MEMORY) ? job_memory : 0;
    }
    job_unlock(flags);
    return job_result(job, 0);
}

BOOL WINAPI QueryInformationJobObject(HANDLE handle, DWORD cls, PVOID info,
                                      DWORD size, DWORD *returned)
{
    DWORD previous_error = GetLastError();
    JOB_OBJECT *job = job_reference(handle, 4, TRUE);
    if (!job) return FALSE;
    if (cls == 7) return job_result(job, 87); /* Set-only information class. */
    DWORD required = cls == 1 ? 48 : cls == 4 ? 4 :
        cls == 2 ? (g_compat32_mode ? 48 : 64) : cls == 9 ? (g_compat32_mode ? 112 : 144) : 0;
    if (!required) return job_result(job, 50);
    if (size < required) return job_result(job, 24);
    if (!job_buffer(info, required, TRUE) ||
        (returned && !job_buffer(returned, 4, TRUE))) return job_result(job, 998);
    BYTE copy[144] __attribute__((aligned(8))) = {0};
    uint64_t flags = job_lock();
    if (cls == 1) {
        *(DWORD *)(copy + 36) = job->total;
        *(DWORD *)(copy + 40) = job->active;
        *(DWORD *)(copy + 44) = job->terminated;
    } else if (cls == 4) {
        *(DWORD *)copy = job->ui_limits;
    } else if (cls == 2 || cls == 9) {
        *(DWORD *)(copy + 16) = job->flags;
        *(DWORD *)(copy + (g_compat32_mode ? 28 : 40)) = job->active_limit;
        if (cls == 9) {
            if (g_compat32_mode) {
                *(DWORD *)(copy + 96) = (DWORD)job->process_limit;
                *(DWORD *)(copy + 100) = (DWORD)job->job_limit;
                *(DWORD *)(copy + 104) = (DWORD)job->peak_process;
                *(DWORD *)(copy + 108) = (DWORD)job->peak_job;
            } else {
                *(ULONGLONG *)(copy + 112) = job->process_limit;
                *(ULONGLONG *)(copy + 120) = job->job_limit;
                *(ULONGLONG *)(copy + 128) = job->peak_process;
                *(ULONGLONG *)(copy + 136) = job->peak_job;
            }
        }
    }
    job_unlock(flags);
    for (DWORD i = 0; i < required; i++) ((BYTE *)info)[i] = copy[i];
    if (returned) *returned = required;
    BOOL result = job_result(job, 0);
    if (cls == 4) SetLastError(previous_error);
    return result;
}

BOOL WINAPI AssignProcessToJobObject(HANDLE handle, HANDLE process)
{
    JOB_OBJECT *job = job_reference(handle, 1, FALSE);
    if (!job) return FALSE;
    ULONG pid;
    NTSTATUS status = nt_process_resolve_job_pid(process, 0x0101U, &pid);
    if (!NT_SUCCESS(status)) return job_result(job, status == STATUS_ACCESS_DENIED ? 5 : 6);
    uint64_t flags = job_lock();
    JOB_PROCESS *p = job_process(pid);
    DWORD error = p ? job_attach(job, p) : 5;
    job_unlock(flags);
    if (error == 1816) (void)win32_terminate_process_id(pid, (NTSTATUS)0xc0000044U);
    return job_result(job, error);
}

BOOL WINAPI IsProcessInJob(HANDLE process, HANDLE handle, BOOL *result)
{
    if (!job_buffer(result, sizeof(*result), TRUE)) { SetLastError(998); return FALSE; }
    JOB_OBJECT *job = handle ? job_reference(handle, 4, FALSE) : NULL;
    if (handle && !job) return FALSE;
    ULONG pid;
    NTSTATUS status = nt_process_resolve_job_pid(process, 0x0400U, &pid);
    if (!NT_SUCCESS(status)) return job_result(job, status == STATUS_ACCESS_DENIED ? 5 : 6);
    uint64_t flags = job_lock();
    JOB_PROCESS *p = job_process(pid);
    BOOL member = p && p->job && (!job || p->job == job);
    job_unlock(flags);
    *result = member;
    return job_result(job, 0);
}

BOOL WINAPI TerminateJobObject(HANDLE handle, UINT code)
{
    JOB_OBJECT *job = job_reference(handle, 8, FALSE);
    if (!job) return FALSE;
    BOOL result = job_terminate(job, code);
    return job_result(job, result ? 0 : 5);
}
