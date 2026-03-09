/*
 * OsitoK Windows Compatibility Layer — Synchronization Primitives
 *
 * Implements:
 *   NtCreateEvent / NtSetEvent / NtResetEvent / NtPulseEvent
 *   NtCreateMutant / NtReleaseMutant
 *   NtCreateSemaphore / NtReleaseSemaphore
 *   NtWaitForMultipleObjects
 *
 * Design:
 *   - Non-preemptive kernel → simple polling-based wait
 *   - Events: manual-reset or auto-reset
 *   - Mutants: recursive (like Windows KMUTANT/CreateMutex)
 *   - Semaphores: counted signaling
 */

#include "ntsyscall.h"
#include "handle.h"

/* ── External ───────────────────────────────────────────────── */

extern void     serial_puts(const char *s);
extern void     serial_puthex(uint64_t val, int digits);
extern uint64_t idt_get_ticks(void);
extern HANDLE_TABLE g_handle_table;

/* ── Event object ───────────────────────────────────────────── */

#define EVENT_TYPE_NOTIFICATION  0   /* manual-reset */
#define EVENT_TYPE_SYNCHRONIZATION 1 /* auto-reset */

typedef struct _EVENT_OBJECT {
    ULONG   type;       /* EVENT_TYPE_* */
    BOOL    signaled;
} EVENT_OBJECT;

#define MAX_EVENTS  32
static EVENT_OBJECT g_events[MAX_EVENTS];
static int g_event_next = 0;

/* ── Mutant object ──────────────────────────────────────────── */

typedef struct _MUTANT_OBJECT {
    BOOL    owned;
    ULONG   owner_tid;      /* owning thread ID (0 = unowned) */
    ULONG   recursion_count;
} MUTANT_OBJECT;

#define MAX_MUTANTS 16
static MUTANT_OBJECT g_mutants[MAX_MUTANTS];
static int g_mutant_next = 0;

/* ── Semaphore object ───────────────────────────────────────── */

typedef struct _SEMAPHORE_OBJECT {
    LONG    count;          /* current count (signaled if > 0) */
    LONG    max_count;
} SEMAPHORE_OBJECT;

#define MAX_SEMAPHORES 16
static SEMAPHORE_OBJECT g_semaphores[MAX_SEMAPHORES];
static int g_semaphore_next = 0;

/* ── NtCreateEvent ──────────────────────────────────────────── */

NTSTATUS sys_NtCreateEvent(ULONG_PTR *args)
{
    PHANDLE         EventHandle     = (PHANDLE)args[0];
    ACCESS_MASK     DesiredAccess   = (ACCESS_MASK)args[1];
    /* POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)args[2]; */
    ULONG           EventType       = (ULONG)args[3];
    BOOL            InitialState    = (BOOL)args[4];

    if (!EventHandle || g_event_next >= MAX_EVENTS)
        return STATUS_INSUFFICIENT_RESOURCES;

    EVENT_OBJECT *evt = &g_events[g_event_next++];
    evt->type     = EventType;
    evt->signaled = InitialState;

    return handle_alloc(&g_handle_table, OBJ_TYPE_EVENT,
                        DesiredAccess, evt, EventHandle);
}

/* ── NtSetEvent ─────────────────────────────────────────────── */

NTSTATUS sys_NtSetEvent(ULONG_PTR *args)
{
    HANDLE  EventHandle    = (HANDLE)args[0];
    LONG   *PreviousState  = (LONG *)args[1];

    EVENT_OBJECT *evt = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, EventHandle,
                                    OBJ_TYPE_EVENT, (PVOID *)&evt);
    if (!NT_SUCCESS(status))
        return status;

    LONG prev = evt->signaled;
    evt->signaled = TRUE;

    if (PreviousState)
        *PreviousState = prev;

    return STATUS_SUCCESS;
}

/* ── NtResetEvent ───────────────────────────────────────────── */

NTSTATUS sys_NtResetEvent(ULONG_PTR *args)
{
    HANDLE  EventHandle    = (HANDLE)args[0];
    LONG   *PreviousState  = (LONG *)args[1];

    EVENT_OBJECT *evt = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, EventHandle,
                                    OBJ_TYPE_EVENT, (PVOID *)&evt);
    if (!NT_SUCCESS(status))
        return status;

    LONG prev = evt->signaled;
    evt->signaled = FALSE;

    if (PreviousState)
        *PreviousState = prev;

    return STATUS_SUCCESS;
}

/* ── NtPulseEvent ───────────────────────────────────────────── */

NTSTATUS sys_NtPulseEvent(ULONG_PTR *args)
{
    HANDLE  EventHandle    = (HANDLE)args[0];
    LONG   *PreviousState  = (LONG *)args[1];

    EVENT_OBJECT *evt = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, EventHandle,
                                    OBJ_TYPE_EVENT, (PVOID *)&evt);
    if (!NT_SUCCESS(status))
        return status;

    LONG prev = evt->signaled;

    /* Signal then immediately reset */
    evt->signaled = TRUE;
    evt->signaled = FALSE;

    if (PreviousState)
        *PreviousState = prev;

    return STATUS_SUCCESS;
}

/* ── NtCreateMutant ─────────────────────────────────────────── */

NTSTATUS sys_NtCreateMutant(ULONG_PTR *args)
{
    PHANDLE         MutantHandle    = (PHANDLE)args[0];
    ACCESS_MASK     DesiredAccess   = (ACCESS_MASK)args[1];
    /* POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)args[2]; */
    BOOL            InitialOwner    = (BOOL)args[3];

    if (!MutantHandle || g_mutant_next >= MAX_MUTANTS)
        return STATUS_INSUFFICIENT_RESOURCES;

    MUTANT_OBJECT *mut = &g_mutants[g_mutant_next++];
    mut->owned = InitialOwner;
    mut->owner_tid = InitialOwner ? 1 : 0;  /* current thread = 1 */
    mut->recursion_count = InitialOwner ? 1 : 0;

    return handle_alloc(&g_handle_table, OBJ_TYPE_MUTANT,
                        DesiredAccess, mut, MutantHandle);
}

/* ── NtReleaseMutant ────────────────────────────────────────── */

NTSTATUS sys_NtReleaseMutant(ULONG_PTR *args)
{
    HANDLE  MutantHandle   = (HANDLE)args[0];
    LONG   *PreviousCount  = (LONG *)args[1];

    MUTANT_OBJECT *mut = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, MutantHandle,
                                    OBJ_TYPE_MUTANT, (PVOID *)&mut);
    if (!NT_SUCCESS(status))
        return status;

    if (!mut->owned)
        return STATUS_UNSUCCESSFUL;  /* not owned */

    if (PreviousCount)
        *PreviousCount = (LONG)mut->recursion_count;

    mut->recursion_count--;
    if (mut->recursion_count == 0) {
        mut->owned = FALSE;
        mut->owner_tid = 0;
    }

    return STATUS_SUCCESS;
}

/* ── NtCreateSemaphore ──────────────────────────────────────── */

NTSTATUS sys_NtCreateSemaphore(ULONG_PTR *args)
{
    PHANDLE         SemaphoreHandle = (PHANDLE)args[0];
    ACCESS_MASK     DesiredAccess   = (ACCESS_MASK)args[1];
    /* POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)args[2]; */
    LONG            InitialCount    = (LONG)args[3];
    LONG            MaximumCount    = (LONG)args[4];

    if (!SemaphoreHandle || g_semaphore_next >= MAX_SEMAPHORES)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (InitialCount < 0 || MaximumCount <= 0 || InitialCount > MaximumCount)
        return STATUS_INVALID_PARAMETER;

    SEMAPHORE_OBJECT *sem = &g_semaphores[g_semaphore_next++];
    sem->count = InitialCount;
    sem->max_count = MaximumCount;

    return handle_alloc(&g_handle_table, OBJ_TYPE_SEMAPHORE,
                        DesiredAccess, sem, SemaphoreHandle);
}

/* ── NtReleaseSemaphore ─────────────────────────────────────── */

NTSTATUS sys_NtReleaseSemaphore(ULONG_PTR *args)
{
    HANDLE  SemaphoreHandle = (HANDLE)args[0];
    LONG    ReleaseCount    = (LONG)args[1];
    LONG   *PreviousCount   = (LONG *)args[2];

    SEMAPHORE_OBJECT *sem = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, SemaphoreHandle,
                                    OBJ_TYPE_SEMAPHORE, (PVOID *)&sem);
    if (!NT_SUCCESS(status))
        return status;

    if (PreviousCount)
        *PreviousCount = sem->count;

    if (sem->count + ReleaseCount > sem->max_count)
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;

    sem->count += ReleaseCount;
    return STATUS_SUCCESS;
}

/* ── Wait helper: check if object is signaled ───────────────── */

static BOOL is_signaled(HANDLE_ENTRY *entry)
{
    switch (entry->type) {
    case OBJ_TYPE_EVENT: {
        EVENT_OBJECT *evt = (EVENT_OBJECT *)entry->object;
        return evt->signaled;
    }
    case OBJ_TYPE_MUTANT: {
        MUTANT_OBJECT *mut = (MUTANT_OBJECT *)entry->object;
        return !mut->owned || mut->owner_tid == 1; /* can acquire if unowned or we own it */
    }
    case OBJ_TYPE_SEMAPHORE: {
        SEMAPHORE_OBJECT *sem = (SEMAPHORE_OBJECT *)entry->object;
        return sem->count > 0;
    }
    default:
        return FALSE;
    }
}

/* Acquire a signaled object (consume the signal) */
static void acquire_object(HANDLE_ENTRY *entry)
{
    switch (entry->type) {
    case OBJ_TYPE_EVENT: {
        EVENT_OBJECT *evt = (EVENT_OBJECT *)entry->object;
        if (evt->type == EVENT_TYPE_SYNCHRONIZATION)
            evt->signaled = FALSE;  /* auto-reset */
        break;
    }
    case OBJ_TYPE_MUTANT: {
        MUTANT_OBJECT *mut = (MUTANT_OBJECT *)entry->object;
        mut->owned = TRUE;
        mut->owner_tid = 1;
        mut->recursion_count++;
        break;
    }
    case OBJ_TYPE_SEMAPHORE: {
        SEMAPHORE_OBJECT *sem = (SEMAPHORE_OBJECT *)entry->object;
        sem->count--;
        break;
    }
    default:
        break;
    }
}

/* ── NtWaitForMultipleObjects ───────────────────────────────── */

#define STATUS_TIMEOUT              ((NTSTATUS)0x00000102)
#define STATUS_WAIT_0               ((NTSTATUS)0x00000000)
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED ((NTSTATUS)0xC0000046)
#define MAXIMUM_WAIT_OBJECTS        64

NTSTATUS sys_NtWaitForMultipleObjects(ULONG_PTR *args)
{
    ULONG           Count       = (ULONG)args[0];
    HANDLE         *Handles     = (HANDLE *)args[1];
    ULONG           WaitType    = (ULONG)args[2];  /* 0=WaitAll, 1=WaitAny */
    /* BOOLEAN      Alertable   = (BOOLEAN)args[3]; */
    PLARGE_INTEGER  Timeout     = (PLARGE_INTEGER)args[4];

    if (Count == 0 || Count > MAXIMUM_WAIT_OBJECTS || !Handles)
        return STATUS_INVALID_PARAMETER;

    /* Resolve all handles */
    HANDLE_ENTRY *entries[MAXIMUM_WAIT_OBJECTS];
    for (ULONG i = 0; i < Count; i++) {
        entries[i] = handle_get_entry(&g_handle_table, Handles[i]);
        if (!entries[i])
            return STATUS_INVALID_HANDLE;
    }

    uint64_t start = idt_get_ticks();
    uint64_t timeout_ticks = 0;
    if (Timeout) {
        LONGLONG t = Timeout->QuadPart;
        if (t < 0) t = -t;
        timeout_ticks = (uint64_t)(t / 100000);
        if (timeout_ticks == 0 && t > 0) timeout_ticks = 1;
    }

    for (;;) {
        if (WaitType == 1) {
            /* WaitAny: return when ANY object is signaled */
            for (ULONG i = 0; i < Count; i++) {
                if (is_signaled(entries[i])) {
                    acquire_object(entries[i]);
                    return STATUS_WAIT_0 + i;
                }
            }
        } else {
            /* WaitAll: return when ALL objects are signaled */
            BOOL all_signaled = TRUE;
            for (ULONG i = 0; i < Count; i++) {
                if (!is_signaled(entries[i])) {
                    all_signaled = FALSE;
                    break;
                }
            }
            if (all_signaled) {
                for (ULONG i = 0; i < Count; i++)
                    acquire_object(entries[i]);
                return STATUS_WAIT_0;
            }
        }

        /* Check timeout */
        if (Timeout && timeout_ticks > 0 &&
            (idt_get_ticks() - start) >= timeout_ticks) {
            return STATUS_TIMEOUT;
        }

        /* Zero timeout = poll once and return */
        if (Timeout && Timeout->QuadPart == 0)
            return STATUS_TIMEOUT;

        __asm__ volatile("sti; hlt; cli");
    }
}

/* ── SSDT registration ──────────────────────────────────────── */

/* Syscall numbers for sync primitives (WS2003 SP1 amd64) */
#define NTSYS_CreateEvent       137
#define NTSYS_SetEvent          253
#define NTSYS_ResetEvent        242
#define NTSYS_PulseEvent        234
#define NTSYS_CreateMutant      140
#define NTSYS_ReleaseMutant     241
#define NTSYS_CreateSemaphore   155
#define NTSYS_ReleaseSemaphore  244
#define NTSYS_WaitForMultipleObjects 274

void nt_sync_register_syscalls(NT_SERVICE_TABLE *table)
{
    table->handlers[NTSYS_CreateEvent]    = sys_NtCreateEvent;
    table->arg_counts[NTSYS_CreateEvent]  = 5;

    table->handlers[NTSYS_SetEvent]       = sys_NtSetEvent;
    table->arg_counts[NTSYS_SetEvent]     = 2;

    table->handlers[NTSYS_ResetEvent]     = sys_NtResetEvent;
    table->arg_counts[NTSYS_ResetEvent]   = 2;

    table->handlers[NTSYS_PulseEvent]     = sys_NtPulseEvent;
    table->arg_counts[NTSYS_PulseEvent]   = 2;

    table->handlers[NTSYS_CreateMutant]   = sys_NtCreateMutant;
    table->arg_counts[NTSYS_CreateMutant] = 4;

    table->handlers[NTSYS_ReleaseMutant]  = sys_NtReleaseMutant;
    table->arg_counts[NTSYS_ReleaseMutant] = 2;

    table->handlers[NTSYS_CreateSemaphore] = sys_NtCreateSemaphore;
    table->arg_counts[NTSYS_CreateSemaphore] = 5;

    table->handlers[NTSYS_ReleaseSemaphore] = sys_NtReleaseSemaphore;
    table->arg_counts[NTSYS_ReleaseSemaphore] = 3;

    table->handlers[NTSYS_WaitForMultipleObjects] = sys_NtWaitForMultipleObjects;
    table->arg_counts[NTSYS_WaitForMultipleObjects] = 5;
}
