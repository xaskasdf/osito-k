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
 *   - Waiters leave the runnable queue until an object or timeout wakes them
 *   - Events: manual-reset or auto-reset
 *   - Mutants: recursive (like Windows KMUTANT/CreateMutex)
 *   - Semaphores: counted signaling
 */

#include "ntsyscall.h"
#include "handle.h"
#include "kernel32_shim.h"
#include "../kernel/smp.h"

#ifndef NTSYNC_TRACE_DIAGNOSTICS
#define NTSYNC_TRACE_DIAGNOSTICS 0
#endif

/* ── External ───────────────────────────────────────────────── */

extern void     serial_puts(const char *s);
extern void     serial_puthex(uint64_t val, int digits);
extern void     serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);
extern void     sched_yield(void);
extern int      sched_current_get(void);
extern int      sched_block_current(void);
extern void     sched_unblock(int proc_idx);
extern uint32_t sched_capacity_get(void);
extern void    *kcalloc(uint64_t count, uint64_t size);
extern HANDLE_TABLE g_handle_table;
extern int32_t  proc_current_pid(void);
extern DWORD    win32_current_process_id(void);
extern NTSTATUS nt_close_handle_for_process(HANDLE handle, ULONG owner_pid);
extern uint32_t compat32_get_last_caller_eip(void);

#define STATUS_TIMEOUT              ((NTSTATUS)0x00000102)
#define STATUS_WAIT_0               ((NTSTATUS)0x00000000)
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED ((NTSTATUS)0xC0000046)
#define MAXIMUM_WAIT_OBJECTS        64
#define THREAD_STATE_TERMINATED     4

/* ── Event object ───────────────────────────────────────────── */

#define EVENT_TYPE_NOTIFICATION  0   /* manual-reset */
#define EVENT_TYPE_SYNCHRONIZATION 1 /* auto-reset */

typedef struct _EVENT_OBJECT {
    BOOL    allocated;
    ULONG   type;       /* EVENT_TYPE_* */
    BOOL    signaled;
    BOOL    timer_active;
    uint64_t timer_due_tick;
    uint64_t timer_period_ticks;
    uint64_t trace_id;
} EVENT_OBJECT;

#define MAX_EVENTS  MAX_HANDLES
static EVENT_OBJECT g_events[MAX_EVENTS];

/* ── Mutant object ──────────────────────────────────────────── */

typedef struct _MUTANT_OBJECT {
    BOOL    allocated;
    BOOL    owned;
    ULONG   owner_tid;      /* owning thread ID (0 = unowned) */
    ULONG   recursion_count;
    uint64_t identity;
} MUTANT_OBJECT;

#define MAX_MUTANTS MAX_HANDLES
static MUTANT_OBJECT g_mutants[MAX_MUTANTS];

/* ── Semaphore object ───────────────────────────────────────── */

typedef struct _SEMAPHORE_OBJECT {
    BOOL    allocated;
    LONG    count;          /* current count (signaled if > 0) */
    LONG    max_count;
    uint64_t identity;
} SEMAPHORE_OBJECT;

#define MAX_SEMAPHORES MAX_HANDLES
static SEMAPHORE_OBJECT g_semaphores[MAX_SEMAPHORES];

typedef struct _NTSYNC_WAITER {
    BOOL            active;
    BOOL            timed_out;
    BOOL            completed;
    int             proc_idx;
    ULONG           count;
    ULONG           wait_type;
    ULONG           owner_pid;
    ULONG           owner_tid;
    BOOL            has_deadline;
    uint64_t        deadline;
    NTSTATUS        completion_status;
    OBJECT_TYPE_ID  types[MAXIMUM_WAIT_OBJECTS];
    PVOID           objects[MAXIMUM_WAIT_OBJECTS];
} NTSYNC_WAITER;

static NTSYNC_WAITER *g_waiters;
static int g_waiter_capacity;
static volatile ULONG g_waiter_count;
#if NTSYNC_TRACE_DIAGNOSTICS
static volatile ULONG g_waiter_trace_count;
#endif
static volatile ULONG g_wait_irq_trace_count;
#if NTSYNC_TRACE_DIAGNOSTICS
static volatile ULONG g_event_lifecycle_trace_count;
#endif
static spinlock_t g_ntsync_lock = SPINLOCK_INIT;
static volatile uint64_t g_sync_object_identity;

__attribute__((noinline)) static void ntsync_if0_probe(void)
{
    __asm__ volatile ("" ::: "memory");
}

#if NTSYNC_TRACE_DIAGNOSTICS

#define NTSYNC_EVENT_TRACE_SLOTS 1024

typedef struct {
    BOOL used;
    ULONG owner_pid;
    EVENT_OBJECT *event;
    uint64_t event_id;
    uint64_t first_tick;
    uint64_t last_tick;
    uint64_t last_deadline;
    ULONG wait_blocks;
    ULONG wait_hits;
    ULONG wait_timeouts;
    ULONG sets;
    ULONG wakes;
    ULONG resets;
    ULONG acquires;
    ULONG last_actor_pid;
    int last_waiter_slot;
} NTSYNC_EVENT_TRACE;

static NTSYNC_EVENT_TRACE g_event_traces[NTSYNC_EVENT_TRACE_SLOTS];

static NTSYNC_EVENT_TRACE *ntsync_event_trace_find(ULONG owner_pid,
                                                    EVENT_OBJECT *event)
{
    if (!event)
        return NULL;
    if (!owner_pid)
        owner_pid = 1;
    for (int i = 0; i < NTSYNC_EVENT_TRACE_SLOTS; i++) {
        NTSYNC_EVENT_TRACE *trace = &g_event_traces[i];
        if (trace->used && trace->owner_pid == owner_pid &&
            trace->event == event && trace->event_id == event->trace_id)
            return trace;
    }
    return NULL;
}

static NTSYNC_EVENT_TRACE *ntsync_event_trace_get(ULONG owner_pid,
                                                   EVENT_OBJECT *event)
{
    NTSYNC_EVENT_TRACE *trace = ntsync_event_trace_find(owner_pid, event);
    if (trace)
        return trace;
    if (!event)
        return NULL;
    if (!owner_pid)
        owner_pid = 1;

    NTSYNC_EVENT_TRACE *available = NULL;
    for (int i = 0; i < NTSYNC_EVENT_TRACE_SLOTS; i++) {
        trace = &g_event_traces[i];
        if (trace->used && trace->event && trace->event->allocated &&
            trace->event->trace_id == trace->event_id)
            continue;
        available = trace;
        break;
    }
    if (!available)
        return NULL;

    available->used = TRUE;
    available->owner_pid = owner_pid;
    available->event = event;
    available->event_id = event->trace_id;
    available->first_tick = idt_get_ticks();
    available->last_tick = available->first_tick;
    available->last_deadline = 0;
    available->wait_blocks = 0;
    available->wait_hits = 0;
    available->wait_timeouts = 0;
    available->sets = 0;
    available->wakes = 0;
    available->resets = 0;
    available->acquires = 0;
    available->last_actor_pid = owner_pid;
    available->last_waiter_slot = -1;
    return available;
}

static void ntsync_event_trace_signal(EVENT_OBJECT *event, ULONG actor_pid,
                                      BOOL wake)
{
    if (!event)
        return;
    uint64_t now = idt_get_ticks();
    for (int i = 0; i < NTSYNC_EVENT_TRACE_SLOTS; i++) {
        NTSYNC_EVENT_TRACE *trace = &g_event_traces[i];
        if (!trace->used || trace->event != event ||
            trace->event_id != event->trace_id)
            continue;
        if (wake)
            trace->wakes++;
        else
            trace->sets++;
        trace->last_actor_pid = actor_pid;
        trace->last_tick = now;
    }
}

static void ntsync_event_trace_wait(ULONG owner_pid, EVENT_OBJECT *event,
                                    int proc_idx, uint64_t deadline,
                                    ULONG operation)
{
    NTSYNC_EVENT_TRACE *trace = ntsync_event_trace_get(owner_pid, event);
    if (!trace)
        return;
    if (operation == 0)
        trace->wait_blocks++;
    else if (operation == 1)
        trace->wait_hits++;
    else
        trace->wait_timeouts++;
    trace->last_waiter_slot = proc_idx;
    trace->last_deadline = deadline;
    trace->last_tick = idt_get_ticks();
}

static void ntsync_event_trace_acquire(ULONG owner_pid, EVENT_OBJECT *event)
{
    NTSYNC_EVENT_TRACE *trace = ntsync_event_trace_find(owner_pid, event);
    if (!trace)
        return;
    trace->acquires++;
    trace->last_tick = idt_get_ticks();
}

static void ntsync_event_trace_reset(EVENT_OBJECT *event, ULONG actor_pid)
{
    uint64_t now = idt_get_ticks();
    for (int i = 0; i < NTSYNC_EVENT_TRACE_SLOTS; i++) {
        NTSYNC_EVENT_TRACE *trace = &g_event_traces[i];
        if (!trace->used || trace->event != event ||
            trace->event_id != event->trace_id)
            continue;
        trace->resets++;
        trace->last_actor_pid = actor_pid;
        trace->last_tick = now;
    }
}

void ntsync_debug_dump_process_events(ULONG owner_pid)
{
    serial_puts("[NTEVENT-SUMMARY] pid=");
    serial_putdec(owner_pid);
    serial_puts("\n");
    for (int i = 0; i < NTSYNC_EVENT_TRACE_SLOTS; i++) {
        NTSYNC_EVENT_TRACE *trace = &g_event_traces[i];
        if (!trace->used || trace->owner_pid != owner_pid)
            continue;
        EVENT_OBJECT *event = trace->event;
        BOOL live = event && event->allocated &&
                    event->trace_id == trace->event_id;
        serial_puts("[NTEVENT] obj=0x");
        serial_puthex((uint64_t)(ULONG_PTR)event, 16);
        serial_puts(" id=");
        serial_putdec(trace->event_id);
        serial_puts(" type=");
        serial_putdec(live ? event->type : 0xFFFFFFFFU);
        serial_puts(" sig=");
        serial_putdec(live ? event->signaled : 0);
        serial_puts(" block=");
        serial_putdec(trace->wait_blocks);
        serial_puts(" hit=");
        serial_putdec(trace->wait_hits);
        serial_puts(" timeout=");
        serial_putdec(trace->wait_timeouts);
        serial_puts(" set=");
        serial_putdec(trace->sets);
        serial_puts(" wake=");
        serial_putdec(trace->wakes);
        serial_puts(" acquire=");
        serial_putdec(trace->acquires);
        serial_puts(" reset=");
        serial_putdec(trace->resets);
        serial_puts(" actor=");
        serial_putdec(trace->last_actor_pid);
        serial_puts(" slot=");
        serial_putdec((uint64_t)(uint32_t)trace->last_waiter_slot);
        serial_puts(" first=0x");
        serial_puthex(trace->first_tick, 16);
        serial_puts(" last=0x");
        serial_puthex(trace->last_tick, 16);
        serial_puts(" deadline=0x");
        serial_puthex(trace->last_deadline, 16);
        serial_puts("\n");
    }
}

#else

static inline void ntsync_event_trace_signal(EVENT_OBJECT *event,
                                              ULONG actor_pid, BOOL wake)
{
    (void)event;
    (void)actor_pid;
    (void)wake;
}

static inline void ntsync_event_trace_wait(ULONG owner_pid,
                                            EVENT_OBJECT *event,
                                            int proc_idx, uint64_t deadline,
                                            ULONG operation)
{
    (void)owner_pid;
    (void)event;
    (void)proc_idx;
    (void)deadline;
    (void)operation;
}

static inline void ntsync_event_trace_acquire(ULONG owner_pid,
                                               EVENT_OBJECT *event)
{
    (void)owner_pid;
    (void)event;
}

static inline void ntsync_event_trace_reset(EVENT_OBJECT *event,
                                             ULONG actor_pid)
{
    (void)event;
    (void)actor_pid;
}

void ntsync_debug_dump_process_events(ULONG owner_pid)
{
    (void)owner_pid;
}

#endif

void ntsync_debug_dump_waiter(int proc_idx)
{
    if (!g_waiters || proc_idx < 0 || proc_idx >= g_waiter_capacity)
        return;

    NTSYNC_WAITER *waiter = &g_waiters[proc_idx];
    serial_puts("[NTWAIT-SNAP] slot=");
    serial_putdec((uint64_t)proc_idx);
    serial_puts(" active=");
    serial_putdec(waiter->active);
    serial_puts(" timeout=");
    serial_putdec(waiter->timed_out);
    serial_puts(" completed=");
    serial_putdec(waiter->completed);
    serial_puts(" status=0x");
    serial_puthex((uint32_t)waiter->completion_status, 8);
    serial_puts(" count=");
    serial_putdec(waiter->count);
    serial_puts(" type=");
    serial_putdec(waiter->wait_type);
    serial_puts(" deadline=0x");
    serial_puthex(waiter->has_deadline ? waiter->deadline : 0, 16);
    for (ULONG i = 0; i < waiter->count && i < 4; i++) {
        serial_puts(" obj");
        serial_putdec(i);
        serial_puts("=");
        serial_putdec(waiter->types[i]);
        serial_puts("@0x");
        serial_puthex((uint64_t)(ULONG_PTR)waiter->objects[i], 16);
        if (waiter->types[i] == OBJ_TYPE_EVENT && waiter->objects[i]) {
            EVENT_OBJECT *event = (EVENT_OBJECT *)waiter->objects[i];
            serial_puts("(type=");
            serial_putdec(event->type);
            serial_puts(" sig=");
            serial_putdec(event->signaled);
            serial_puts(" timer=");
            serial_putdec(event->timer_active);
            serial_puts(")");
        }
    }
    serial_puts("\n");
}

static inline uint64_t ntsync_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&g_ntsync_lock);
    return flags;
}

static inline void ntsync_irq_restore(uint64_t flags)
{
    spin_unlock(&g_ntsync_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static void ntsync_trace_event_operation(const char *operation, HANDLE handle,
                                         const EVENT_OBJECT *event,
                                         LONG previous_state,
                                         ULONG owner_pid)
{
#if !NTSYNC_TRACE_DIAGNOSTICS || (defined(OK_QUIET) && OK_QUIET)
    (void)operation;
    (void)handle;
    (void)event;
    (void)previous_state;
    (void)owner_pid;
#else
    ULONG trace_index = __atomic_fetch_add(&g_event_lifecycle_trace_count, 1,
                                           __ATOMIC_RELAXED);
    if (trace_index >= 128 || !event)
        return;

    serial_puts("[NTSYNC-EVENT] op=");
    serial_puts(operation);
    serial_puts(" handle=0x");
    serial_puthex((uint64_t)(ULONG_PTR)handle, 8);
    serial_puts(" id=");
    serial_putdec(event->trace_id);
    serial_puts(" type=");
    serial_putdec(event->type);
    serial_puts(" state=");
    serial_putdec(event->signaled);
    serial_puts(" previous=");
    serial_putdec((uint32_t)previous_state);
    serial_puts(" actor=");
    serial_putdec(win32_current_process_id());
    serial_puts(" owner=");
    serial_putdec(owner_pid);
    serial_puts(" caller=0x");
    serial_puthex(compat32_get_last_caller_eip(), 8);
    serial_puts(" object=0x");
    serial_puthex((uint64_t)(ULONG_PTR)event, 16);
    serial_puts("\n");
#endif
}

static uint64_t ntsync_next_object_identity_locked(void)
{
    uint64_t identity = ++g_sync_object_identity;
    if (!identity)
        identity = ++g_sync_object_identity;
    return identity;
}

uint64_t ntsync_object_identity(OBJECT_TYPE_ID type, PVOID object)
{
    if (!object)
        return 0;

    uint64_t flags = ntsync_irq_save();
    uint64_t identity = 0;
    if (type == OBJ_TYPE_EVENT) {
        EVENT_OBJECT *event = (EVENT_OBJECT *)object;
        if (event->allocated)
            identity = event->trace_id;
    } else if (type == OBJ_TYPE_MUTANT) {
        MUTANT_OBJECT *mutant = (MUTANT_OBJECT *)object;
        if (mutant->allocated)
            identity = mutant->identity;
    } else if (type == OBJ_TYPE_SEMAPHORE) {
        SEMAPHORE_OBJECT *semaphore = (SEMAPHORE_OBJECT *)object;
        if (semaphore->allocated)
            identity = semaphore->identity;
    }
    ntsync_irq_restore(flags);
    return identity;
}

void ntsync_cancel_waiter(int proc_idx)
{
    if (!g_waiters || proc_idx < 0 || proc_idx >= g_waiter_capacity)
        return;

    uint64_t flags = ntsync_irq_save();
    NTSYNC_WAITER *waiter = &g_waiters[proc_idx];
    if (waiter->active) {
        waiter->active = FALSE;
        if (g_waiter_count)
            g_waiter_count--;
    }
    waiter->timed_out = FALSE;
    waiter->completed = FALSE;
    ntsync_irq_restore(flags);
}

static BOOL ntsync_waiter_contains(const NTSYNC_WAITER *waiter,
                                   OBJECT_TYPE_ID type, PVOID object)
{
    for (ULONG i = 0; i < waiter->count; i++) {
        if (waiter->types[i] == type && waiter->objects[i] == object)
            return TRUE;
    }
    return FALSE;
}

static BOOL ntsync_update_event_timer(EVENT_OBJECT *evt, uint64_t now);

static BOOL ntsync_object_signaled_locked(OBJECT_TYPE_ID type, PVOID object,
                                          ULONG owner_tid, uint64_t now)
{
    switch (type) {
    case OBJ_TYPE_EVENT:
        return ntsync_update_event_timer((EVENT_OBJECT *)object, now);
    case OBJ_TYPE_MUTANT: {
        MUTANT_OBJECT *mut = (MUTANT_OBJECT *)object;
        return !mut->owned || mut->owner_tid == owner_tid;
    }
    case OBJ_TYPE_SEMAPHORE:
        return ((SEMAPHORE_OBJECT *)object)->count > 0;
    case OBJ_TYPE_THREAD: {
        int *state_ptr = (int *)((char *)object + 4);
        return *state_ptr == THREAD_STATE_TERMINATED;
    }
    case OBJ_TYPE_PROCESS: {
        extern BOOL nt_process_object_signaled(PVOID object);
        return nt_process_object_signaled(object);
    }
    default:
        return FALSE;
    }
}

static void ntsync_acquire_raw_locked(OBJECT_TYPE_ID type, PVOID object,
                                      ULONG owner_pid, ULONG owner_tid)
{
    switch (type) {
    case OBJ_TYPE_EVENT: {
        EVENT_OBJECT *evt = (EVENT_OBJECT *)object;
        ntsync_event_trace_acquire(owner_pid, evt);
        if (evt->type == EVENT_TYPE_SYNCHRONIZATION)
            evt->signaled = FALSE;
        break;
    }
    case OBJ_TYPE_MUTANT: {
        MUTANT_OBJECT *mut = (MUTANT_OBJECT *)object;
        mut->owned = TRUE;
        mut->owner_tid = owner_tid;
        mut->recursion_count++;
        break;
    }
    case OBJ_TYPE_SEMAPHORE:
        ((SEMAPHORE_OBJECT *)object)->count--;
        break;
    case OBJ_TYPE_THREAD:
    case OBJ_TYPE_PROCESS:
    default:
        break;
    }
}

static BOOL ntsync_waiter_ready_locked(const NTSYNC_WAITER *waiter,
                                       uint64_t now, ULONG *selected_index)
{
    if (waiter->wait_type == 1) {
        for (ULONG i = 0; i < waiter->count; i++) {
            if (ntsync_object_signaled_locked(waiter->types[i],
                                              waiter->objects[i],
                                              waiter->owner_tid, now)) {
                *selected_index = i;
                return TRUE;
            }
        }
        return FALSE;
    }

    for (ULONG i = 0; i < waiter->count; i++) {
        if (!ntsync_object_signaled_locked(waiter->types[i],
                                           waiter->objects[i],
                                           waiter->owner_tid, now))
            return FALSE;
    }
    *selected_index = 0;
    return TRUE;
}

static void ntsync_complete_waiter_locked(NTSYNC_WAITER *waiter,
                                          NTSTATUS status, BOOL timed_out)
{
    waiter->active = FALSE;
    waiter->timed_out = timed_out;
    waiter->completed = TRUE;
    waiter->completion_status = status;
    if (g_waiter_count)
        g_waiter_count--;
    sched_unblock(waiter->proc_idx);
}

static void ntsync_wake_object_locked(OBJECT_TYPE_ID type, PVOID object)
{
    BOOL woke = FALSE;
    uint64_t now = idt_get_ticks();
    for (int i = 0; i < g_waiter_capacity; i++) {
        NTSYNC_WAITER *waiter = &g_waiters[i];
        if (!waiter->active ||
            !ntsync_waiter_contains(waiter, type, object))
            continue;

        ULONG selected_index;
        if (!ntsync_waiter_ready_locked(waiter, now, &selected_index))
            continue;

        if (waiter->wait_type == 1) {
            ntsync_acquire_raw_locked(waiter->types[selected_index],
                                      waiter->objects[selected_index],
                                      waiter->owner_pid, waiter->owner_tid);
        } else {
            for (ULONG j = 0; j < waiter->count; j++)
                ntsync_acquire_raw_locked(waiter->types[j],
                                          waiter->objects[j],
                                          waiter->owner_pid,
                                          waiter->owner_tid);
        }

        NTSTATUS status = waiter->wait_type == 1
                        ? STATUS_WAIT_0 + selected_index : STATUS_WAIT_0;
        ntsync_complete_waiter_locked(waiter, status, FALSE);
        woke = TRUE;
    }
    if (woke && type == OBJ_TYPE_EVENT)
        ntsync_event_trace_signal((EVENT_OBJECT *)object,
                                  win32_current_process_id(), TRUE);
}

void ntsync_notify_object(OBJECT_TYPE_ID type, PVOID object)
{
    if (!object)
        return;
    uint64_t flags = ntsync_irq_save();
    ntsync_wake_object_locked(type, object);
    ntsync_irq_restore(flags);
}

/* ── NtCreateEvent ──────────────────────────────────────────── */

NTSTATUS sys_NtCreateEvent(ULONG_PTR *args)
{
    PHANDLE         EventHandle     = (PHANDLE)args[0];
    ACCESS_MASK     DesiredAccess   = (ACCESS_MASK)args[1];
    POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)args[2];
    ULONG           EventType       = (ULONG)args[3];
    BOOL            InitialState    = (BOOL)args[4];

    if (!EventHandle)
        return STATUS_INVALID_PARAMETER;

    *EventHandle = NULL;
    if (ObjectAttributes) {
        NTSTATUS open_status = kernel32_nt_open_named_event(
            ObjectAttributes, DesiredAccess, EventHandle);
        if (NT_SUCCESS(open_status))
            return STATUS_OBJECT_NAME_EXISTS;
        if (open_status != STATUS_OBJECT_NAME_NOT_FOUND)
            return open_status;
    }

    EVENT_OBJECT *evt = NULL;
    uint64_t flags = ntsync_irq_save();
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (!g_events[i].allocated) {
            evt = &g_events[i];
            evt->allocated = TRUE;
            break;
        }
    }
    if (!evt) {
        ntsync_irq_restore(flags);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    evt->type     = EventType;
    evt->signaled = InitialState;
    evt->timer_active = FALSE;
    evt->timer_due_tick = 0;
    evt->timer_period_ticks = 0;
    evt->trace_id = ntsync_next_object_identity_locked();
    ntsync_irq_restore(flags);

    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_EVENT,
                                   DesiredAccess, evt, EventHandle);
    if (!NT_SUCCESS(status)) {
        flags = ntsync_irq_save();
        evt->allocated = FALSE;
        ntsync_irq_restore(flags);
    }
    if (NT_SUCCESS(status) && ObjectAttributes) {
        HANDLE created_handle = *EventHandle;
        HANDLE effective_handle = created_handle;
        BOOL already_exists = FALSE;
        status = kernel32_nt_publish_named_event(
            ObjectAttributes, DesiredAccess, created_handle,
            &effective_handle, &already_exists);
        if (!NT_SUCCESS(status) || already_exists) {
            ULONG owner_pid = win32_current_process_id();
            if (!owner_pid) owner_pid = 1;
            nt_close_handle_for_process(created_handle, owner_pid);
        }
        if (!NT_SUCCESS(status)) {
            *EventHandle = NULL;
            return status;
        }
        *EventHandle = effective_handle;
        if (already_exists)
            return STATUS_OBJECT_NAME_EXISTS;
    }
    if (NT_SUCCESS(status)) {
        ULONG owner_pid = win32_current_process_id();
        if (!owner_pid) owner_pid = 1;
        ntsync_trace_event_operation("create", *EventHandle, evt,
                                     InitialState, owner_pid);
    }
    return status;
}

/* ── NtSetEvent ─────────────────────────────────────────────── */

NTSTATUS ntsync_set_event_for_process(HANDLE EventHandle, ULONG owner_pid,
                                      LONG *PreviousState)
{
    EVENT_OBJECT *evt = NULL;
    NTSTATUS status = handle_lookup_for_process(
        &g_handle_table, EventHandle, owner_pid, OBJ_TYPE_EVENT,
        (PVOID *)&evt);
    if (!NT_SUCCESS(status))
        return status;

    uint64_t flags = ntsync_irq_save();
    LONG prev = evt->signaled;
    evt->signaled = TRUE;
    ntsync_event_trace_signal(evt, owner_pid, FALSE);
    ntsync_wake_object_locked(OBJ_TYPE_EVENT, evt);
    ntsync_irq_restore(flags);

    ntsync_trace_event_operation("set", EventHandle, evt, prev, owner_pid);

    if (PreviousState)
        *PreviousState = prev;

    return STATUS_SUCCESS;
}

NTSTATUS sys_NtSetEvent(ULONG_PTR *args)
{
    HANDLE EventHandle = (HANDLE)args[0];
    LONG *PreviousState = (LONG *)args[1];
    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    return ntsync_set_event_for_process(EventHandle, owner_pid,
                                        PreviousState);
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

    uint64_t flags = ntsync_irq_save();
    LONG prev = evt->signaled;
    evt->signaled = FALSE;
    ntsync_event_trace_reset(evt, win32_current_process_id());
    ntsync_irq_restore(flags);

    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    ntsync_trace_event_operation("reset", EventHandle, evt, prev, owner_pid);

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

    uint64_t flags = ntsync_irq_save();
    LONG prev = evt->signaled;
    evt->signaled = TRUE;
    ntsync_event_trace_signal(evt, win32_current_process_id(), FALSE);
    ntsync_wake_object_locked(OBJ_TYPE_EVENT, evt);
    evt->signaled = FALSE;
    ntsync_irq_restore(flags);

    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;
    ntsync_trace_event_operation("pulse", EventHandle, evt, prev, owner_pid);

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

    if (!MutantHandle)
        return STATUS_INVALID_PARAMETER;

    MUTANT_OBJECT *mut = NULL;
    uint64_t flags = ntsync_irq_save();
    for (int i = 0; i < MAX_MUTANTS; i++) {
        if (!g_mutants[i].allocated) {
            mut = &g_mutants[i];
            mut->allocated = TRUE;
            break;
        }
    }
    if (!mut) {
        ntsync_irq_restore(flags);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    mut->owned = InitialOwner;
    mut->owner_tid = InitialOwner ? (ULONG)proc_current_pid() : 0;
    mut->recursion_count = InitialOwner ? 1 : 0;
    mut->identity = ntsync_next_object_identity_locked();
    ntsync_irq_restore(flags);

    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_MUTANT,
                                   DesiredAccess, mut, MutantHandle);
    if (!NT_SUCCESS(status)) {
        flags = ntsync_irq_save();
        mut->allocated = FALSE;
        ntsync_irq_restore(flags);
    }
    return status;
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

    uint64_t flags = ntsync_irq_save();
    if (!mut->owned || mut->owner_tid != (ULONG)proc_current_pid()) {
        ntsync_irq_restore(flags);
        return STATUS_UNSUCCESSFUL;  /* not owned */
    }

    if (PreviousCount)
        *PreviousCount = (LONG)mut->recursion_count;

    mut->recursion_count--;
    if (mut->recursion_count == 0) {
        mut->owned = FALSE;
        mut->owner_tid = 0;
        ntsync_wake_object_locked(OBJ_TYPE_MUTANT, mut);
    }
    ntsync_irq_restore(flags);

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

    if (!SemaphoreHandle || InitialCount < 0 || MaximumCount <= 0 ||
        InitialCount > MaximumCount)
        return STATUS_INVALID_PARAMETER;

    SEMAPHORE_OBJECT *sem = NULL;
    uint64_t flags = ntsync_irq_save();
    for (int i = 0; i < MAX_SEMAPHORES; i++) {
        if (!g_semaphores[i].allocated) {
            sem = &g_semaphores[i];
            sem->allocated = TRUE;
            break;
        }
    }
    if (!sem) {
        ntsync_irq_restore(flags);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    sem->count = InitialCount;
    sem->max_count = MaximumCount;
    sem->identity = ntsync_next_object_identity_locked();
    ntsync_irq_restore(flags);

    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_SEMAPHORE,
                                   DesiredAccess, sem, SemaphoreHandle);
    if (!NT_SUCCESS(status)) {
        flags = ntsync_irq_save();
        sem->allocated = FALSE;
        ntsync_irq_restore(flags);
    }
    return status;
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

    uint64_t flags = ntsync_irq_save();
    if (PreviousCount)
        *PreviousCount = sem->count;

    if (ReleaseCount <= 0 || sem->count + ReleaseCount > sem->max_count) {
        ntsync_irq_restore(flags);
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }

    sem->count += ReleaseCount;
    ntsync_wake_object_locked(OBJ_TYPE_SEMAPHORE, sem);
    ntsync_irq_restore(flags);
    return STATUS_SUCCESS;
}

void ntsync_release_object(OBJECT_TYPE_ID type, PVOID object)
{
    if (!object)
        return;

    uint64_t flags = ntsync_irq_save();
    if (type == OBJ_TYPE_EVENT) {
        ((EVENT_OBJECT *)object)->timer_active = FALSE;
        ((EVENT_OBJECT *)object)->allocated = FALSE;
    } else if (type == OBJ_TYPE_MUTANT)
        ((MUTANT_OBJECT *)object)->allocated = FALSE;
    else if (type == OBJ_TYPE_SEMAPHORE)
        ((SEMAPHORE_OBJECT *)object)->allocated = FALSE;
    ntsync_irq_restore(flags);
}

NTSTATUS ntsync_set_waitable_timer(HANDLE handle, PLARGE_INTEGER due_time,
                                    LONG period_ms)
{
    if (!due_time || period_ms < 0)
        return STATUS_INVALID_PARAMETER;

    EVENT_OBJECT *evt = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, handle, OBJ_TYPE_EVENT,
                                    (PVOID *)&evt);
    if (!NT_SUCCESS(status))
        return status;

    uint64_t now = idt_get_ticks();
    LONGLONG due = due_time->QuadPart;
    uint64_t delay_ticks;
    if (due < 0) {
        uint64_t relative = (uint64_t)(-(due + 1)) + 1;
        delay_ticks = (relative + 99999) / 100000;
    } else {
        uint64_t now_filetime = 133500000000000000ULL + now * 10000ULL;
        uint64_t absolute = (uint64_t)due;
        delay_ticks = absolute > now_filetime
            ? (absolute - now_filetime + 99999) / 100000
            : 0;
    }

    uint64_t flags = ntsync_irq_save();
    evt->signaled = FALSE;
    evt->timer_due_tick = now + delay_ticks;
    evt->timer_period_ticks = period_ms
        ? ((uint64_t)period_ms + 9) / 10
        : 0;
    evt->timer_active = TRUE;
    ntsync_irq_restore(flags);
    return STATUS_SUCCESS;
}

NTSTATUS ntsync_cancel_waitable_timer(HANDLE handle)
{
    EVENT_OBJECT *evt = NULL;
    NTSTATUS status = handle_lookup(&g_handle_table, handle, OBJ_TYPE_EVENT,
                                    (PVOID *)&evt);
    if (!NT_SUCCESS(status))
        return status;

    uint64_t flags = ntsync_irq_save();
    evt->timer_active = FALSE;
    ntsync_irq_restore(flags);
    return STATUS_SUCCESS;
}

/* ── Wait helper: check if object is signaled ───────────────── */
/*
 * These are exported (non-static) so ntprocess.c can use them for
 * NtWaitForSingleObject on event/mutant/semaphore handles.
 */

static BOOL ntsync_update_event_timer(EVENT_OBJECT *evt, uint64_t now)
{
    if (!evt->timer_active || now < evt->timer_due_tick)
        return evt->signaled;

    evt->signaled = TRUE;
    if (evt->timer_period_ticks)
        evt->timer_due_tick = now + evt->timer_period_ticks;
    else
        evt->timer_active = FALSE;
    return TRUE;
}

BOOL ntsync_is_signaled(HANDLE_ENTRY *entry)
{
    return ntsync_object_signaled_locked(entry->type, entry->object,
                                         (ULONG)proc_current_pid(),
                                         idt_get_ticks());
}

/* Called from the BSP scheduler tick. It handles deadlines and waitable
 * timers, and also provides a fallback wakeup for terminated objects. */
void ntsync_poll_waiters(uint64_t now)
{
    if (!g_waiter_count)
        return;

    uint64_t flags = ntsync_irq_save();
    for (int i = 0; i < g_waiter_capacity; i++) {
        NTSYNC_WAITER *waiter = &g_waiters[i];
        if (!waiter->active)
            continue;

        if (waiter->has_deadline && now >= waiter->deadline) {
            ntsync_complete_waiter_locked(waiter, STATUS_TIMEOUT, TRUE);
            continue;
        }

        for (ULONG j = 0; j < waiter->count; j++) {
            if (ntsync_object_signaled_locked(waiter->types[j],
                                               waiter->objects[j],
                                               waiter->owner_tid, now)) {
                ntsync_wake_object_locked(waiter->types[j],
                                          waiter->objects[j]);
                break;
            }
        }
    }
    ntsync_irq_restore(flags);
}

/* Acquire a signaled object (consume the signal) */
void ntsync_acquire_object(HANDLE_ENTRY *entry)
{
    switch (entry->type) {
    case OBJ_TYPE_EVENT: {
        EVENT_OBJECT *evt = (EVENT_OBJECT *)entry->object;
        ntsync_event_trace_acquire(win32_current_process_id(), evt);
        if (evt->type == EVENT_TYPE_SYNCHRONIZATION)
            evt->signaled = FALSE;  /* auto-reset */
        break;
    }
    case OBJ_TYPE_MUTANT: {
        MUTANT_OBJECT *mut = (MUTANT_OBJECT *)entry->object;
        mut->owned = TRUE;
        mut->owner_tid = (ULONG)proc_current_pid();
        mut->recursion_count++;
        break;
    }
    case OBJ_TYPE_SEMAPHORE: {
        SEMAPHORE_OBJECT *sem = (SEMAPHORE_OBJECT *)entry->object;
        sem->count--;
        break;
    }
    case OBJ_TYPE_THREAD:
    case OBJ_TYPE_PROCESS:
        /* Nothing to consume for a thread — signaled = terminated */
        break;
    default:
        break;
    }
}

/* ── NtWaitForMultipleObjects ───────────────────────────────── */

static uint64_t ntsync_timeout_deadline(PLARGE_INTEGER timeout, uint64_t now)
{
    LONGLONG value = timeout->QuadPart;
    uint64_t units_100ns;

    if (value < 0) {
        units_100ns = (uint64_t)(-(value + 1)) + 1;
    } else {
        /* Keep this clock source aligned with GetSystemTimeAsFileTime. */
        uint64_t now_filetime = 133500000000000000ULL + now * 10000ULL;
        uint64_t absolute = (uint64_t)value;
        units_100ns = absolute > now_filetime
                    ? absolute - now_filetime : 0;
    }

    uint64_t ticks = (units_100ns + 99999ULL) / 100000ULL;
    return ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
}

extern NTSTATUS sys_NtClose(ULONG_PTR *args);

static void ntsync_release_wait_handles(HANDLE *handles, ULONG count)
{
    for (ULONG i = 0; i < count; i++) {
        ULONG_PTR close_args[1] = { (ULONG_PTR)handles[i] };
        (void)sys_NtClose(close_args);
    }
}

NTSTATUS sys_NtWaitForMultipleObjects(ULONG_PTR *args)
{
    ULONG           Count       = (ULONG)args[0];
    HANDLE         *Handles     = (HANDLE *)args[1];
    ULONG           WaitType    = (ULONG)args[2];  /* 0=WaitAll, 1=WaitAny */
    /* BOOLEAN      Alertable   = (BOOLEAN)args[3]; */
    PLARGE_INTEGER  Timeout     = (PLARGE_INTEGER)args[4];

    if (Count == 0 || Count > MAXIMUM_WAIT_OBJECTS || !Handles ||
        WaitType > 1)
        return STATUS_INVALID_PARAMETER;

    {
        extern void win32_main_termination_checkpoint(void);
        win32_main_termination_checkpoint();
    }

    /* Keep every handle entry and object alive while the waiter stores raw
     * pointers to them. Another thread may close the application's reference
     * while this thread is blocked. */
    HANDLE_ENTRY *entries[MAXIMUM_WAIT_OBJECTS];
    ULONG owner_pid = win32_current_process_id();
    if (!owner_pid)
        owner_pid = 1;
    ULONG retained_count = 0;
    for (ULONG i = 0; i < Count; i++) {
        NTSTATUS status = handle_retain_for_process(
            &g_handle_table, Handles[i], owner_pid);
        if (!NT_SUCCESS(status)) {
            ntsync_release_wait_handles(Handles, retained_count);
            return status;
        }
        retained_count++;
        entries[i] = handle_get_entry(&g_handle_table, Handles[i]);
        if (!entries[i]) {
            ntsync_release_wait_handles(Handles, retained_count);
            return STATUS_INVALID_HANDLE;
        }
    }

    uint64_t now = idt_get_ticks();
    BOOL has_deadline = Timeout != NULL;
    uint64_t deadline = has_deadline
                      ? ntsync_timeout_deadline(Timeout, now) : 0;

    int initial_proc_idx = sched_current_get();
    if (initial_proc_idx >= 0 && initial_proc_idx < g_waiter_capacity)
        ntsync_cancel_waiter(initial_proc_idx);

    for (;;) {
        extern BOOL win32_main_termination_pending(void);
        extern void win32_main_termination_checkpoint(void);
        if (win32_main_termination_pending()) {
            ntsync_release_wait_handles(Handles, retained_count);
            win32_main_termination_checkpoint();
            __builtin_unreachable();
        }
        uint64_t flags = ntsync_irq_save();
        uint64_t entry_flags = flags;
        if (!(flags & (1ULL << 9)) && g_wait_irq_trace_count++ < 32) {
            ntsync_if0_probe();
            serial_puts("[NTWAIT-IF0] phase=entry proc=");
            serial_putdec((uint64_t)(uint32_t)sched_current_get());
            serial_puts(" pid=");
            serial_putdec((uint64_t)(uint32_t)proc_current_pid());
            serial_puts(" compat32=");
            serial_putdec((uint64_t)(uint32_t)g_compat32_mode);
            serial_puts(" flags=0x");
            serial_puthex(flags, 16);
            serial_puts("\n");
        }
        int completion_idx = sched_current_get();
        if (completion_idx >= 0 && completion_idx < g_waiter_capacity) {
            NTSYNC_WAITER *completed = &g_waiters[completion_idx];
            if (completed->completed) {
                NTSTATUS completion_status = completed->completion_status;
                completed->completed = FALSE;
                completed->timed_out = FALSE;
                ntsync_irq_restore(flags);
                if (entries[0]->type == OBJ_TYPE_EVENT) {
                    ntsync_trace_event_operation(
                        "wait-complete", Handles[0],
                        (EVENT_OBJECT *)entries[0]->object, 0, owner_pid);
                }
                ntsync_release_wait_handles(Handles, retained_count);
                return completion_status;
            }
        }

        NTSTATUS wait_status = STATUS_TIMEOUT;
        BOOL satisfied = FALSE;
        ULONG satisfied_index = 0;

        if (WaitType == 1) {
            /* WaitAny: return when ANY object is signaled */
            for (ULONG i = 0; i < Count; i++) {
                if (ntsync_is_signaled(entries[i])) {
                    ntsync_acquire_object(entries[i]);
                    wait_status = STATUS_WAIT_0 + i;
                    satisfied = TRUE;
                    satisfied_index = i;
                    break;
                }
            }
        } else {
            /* WaitAll: return when ALL objects are signaled */
            BOOL all_signaled = TRUE;
            for (ULONG i = 0; i < Count; i++) {
                if (!ntsync_is_signaled(entries[i])) {
                    all_signaled = FALSE;
                    break;
                }
            }
            if (all_signaled) {
                for (ULONG i = 0; i < Count; i++)
                    ntsync_acquire_object(entries[i]);
                wait_status = STATUS_WAIT_0;
                satisfied = TRUE;
            }
        }

        if (satisfied) {
            if (entries[satisfied_index]->type == OBJ_TYPE_EVENT)
                ntsync_event_trace_wait(win32_current_process_id(),
                    (EVENT_OBJECT *)entries[satisfied_index]->object,
                    sched_current_get(), deadline, 1);
            ntsync_irq_restore(flags);
            if (entries[satisfied_index]->type == OBJ_TYPE_EVENT) {
                ntsync_trace_event_operation(
                    "wait-hit", Handles[satisfied_index],
                    (EVENT_OBJECT *)entries[satisfied_index]->object,
                    1, owner_pid);
            }
            ntsync_release_wait_handles(Handles, retained_count);
            return wait_status;
        }

        now = idt_get_ticks();
        if (has_deadline && now >= deadline) {
            for (ULONG i = 0; i < Count; i++) {
                if (entries[i]->type == OBJ_TYPE_EVENT)
                    ntsync_event_trace_wait(win32_current_process_id(),
                        (EVENT_OBJECT *)entries[i]->object,
                        sched_current_get(), deadline, 2);
            }
            ntsync_irq_restore(flags);
            if (entries[0]->type == OBJ_TYPE_EVENT) {
                ntsync_trace_event_operation(
                    "wait-timeout", Handles[0],
                    (EVENT_OBJECT *)entries[0]->object, 0, owner_pid);
            }
            ntsync_release_wait_handles(Handles, retained_count);
            return STATUS_TIMEOUT;
        }

        int proc_idx = sched_current_get();
        if (!g_waiters || proc_idx < 0 || proc_idx >= g_waiter_capacity) {
            /* The bootstrap shell is not represented by a scheduler slot.
             * It can still wait correctly by polling object state and the
             * absolute deadline while yielding to managed tasks. */
            ntsync_irq_restore(flags);
            sched_yield();
            continue;
        }

        NTSYNC_WAITER *waiter = &g_waiters[proc_idx];
        /* A runnable task cannot still own a registered waiter. This can
         * happen when an external teardown/wakeup makes the scheduler slot
         * READY, or when a reaped slot is reused. Retire the stale record
         * instead of yielding forever on another task's wait state. */
        if (waiter->active) {
            waiter->active = FALSE;
            waiter->timed_out = FALSE;
            if (g_waiter_count)
                g_waiter_count--;
        }
        waiter->timed_out = FALSE;
        waiter->completed = FALSE;
        waiter->proc_idx = proc_idx;
        waiter->count = Count;
        waiter->wait_type = WaitType;
        waiter->owner_pid = owner_pid;
        waiter->owner_tid = (ULONG)proc_current_pid();
        waiter->has_deadline = has_deadline;
        waiter->deadline = deadline;
        waiter->completion_status = STATUS_TIMEOUT;
        for (ULONG i = 0; i < Count; i++) {
            waiter->types[i] = entries[i]->type;
            waiter->objects[i] = entries[i]->object;
            if (entries[i]->type == OBJ_TYPE_EVENT)
                ntsync_event_trace_wait(win32_current_process_id(),
                    (EVENT_OBJECT *)entries[i]->object, proc_idx, deadline, 0);
        }
        waiter->active = TRUE;
        g_waiter_count++;

        /* sched_block_current() releases this thread's IOCP activity and can
         * take IOCP locks. Drop the sync lock first, then reconcile a signal
         * that raced between waiter registration and the state transition. */
        ntsync_irq_restore(flags);
        if (entries[0]->type == OBJ_TYPE_EVENT) {
            ntsync_trace_event_operation(
                "wait-block", Handles[0],
                (EVENT_OBJECT *)entries[0]->object, 0, owner_pid);
        }
        int blocked_idx = sched_block_current();

        flags = ntsync_irq_save();
        if (!(flags & (1ULL << 9)) && g_wait_irq_trace_count++ < 32) {
            serial_puts("[NTWAIT-IF0] phase=post-block proc=");
            serial_putdec((uint64_t)(uint32_t)proc_idx);
            serial_puts(" pid=");
            serial_putdec((uint64_t)(uint32_t)proc_current_pid());
            serial_puts(" compat32=");
            serial_putdec((uint64_t)(uint32_t)g_compat32_mode);
            serial_puts(" entry=0x");
            serial_puthex(entry_flags, 16);
            serial_puts(" flags=0x");
            serial_puthex(flags, 16);
            serial_puts("\n");
        }
        if (blocked_idx != proc_idx) {
            if (waiter->active) {
                waiter->active = FALSE;
                if (g_waiter_count)
                    g_waiter_count--;
            }
        } else if (!waiter->active) {
            sched_unblock(proc_idx);
        }
#if NTSYNC_TRACE_DIAGNOSTICS
        BOOL trace_block = blocked_idx == proc_idx && waiter->active &&
                           g_waiter_trace_count++ < 48;
        ULONG active_waiters = g_waiter_count;
#endif
        ntsync_irq_restore(flags);

#if NTSYNC_TRACE_DIAGNOSTICS
        if (trace_block) {
#if !defined(OK_QUIET) || !OK_QUIET
            serial_puts("[NTWAIT-BLOCK] proc=");
            serial_putdec((uint64_t)proc_idx);
            serial_puts(" objects=");
            serial_putdec(Count);
            serial_puts(" active=");
            serial_putdec(active_waiters);
            serial_puts(" first_handle=0x");
            serial_puthex((uint64_t)(ULONG_PTR)Handles[0], 8);
            serial_puts(" first_type=");
            serial_putdec(entries[0]->type);
            serial_puts(" first_object=0x");
            serial_puthex((uint64_t)(ULONG_PTR)entries[0]->object, 16);
            if (entries[0]->type == OBJ_TYPE_EVENT) {
                EVENT_OBJECT *event = (EVENT_OBJECT *)entries[0]->object;
                serial_puts(" event_id=");
                serial_putdec(event->trace_id);
                serial_puts(" event_type=");
                serial_putdec(event->type);
                serial_puts(" event_state=");
                serial_putdec(event->signaled);
                serial_puts(" event_timer=");
                serial_putdec(event->timer_active);
            }
            serial_puts(" caller=0x");
            serial_puthex(compat32_get_last_caller_eip(), 8);
            serial_puts("\n");
#else
            (void)active_waiters;
#endif
        }
#endif

        sched_yield();
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
    g_waiter_count = 0;
#if NTSYNC_TRACE_DIAGNOSTICS
    g_waiter_trace_count = 0;
    g_event_lifecycle_trace_count = 0;
#endif

    if (!g_waiters) {
        g_waiter_capacity = (int)sched_capacity_get();
        g_waiters = (NTSYNC_WAITER *)kcalloc(
            (uint64_t)g_waiter_capacity, sizeof(*g_waiters));
        if (!g_waiters) {
            g_waiter_capacity = 0;
            serial_puts("[NTSYNC] Failed to allocate waiter table\n");
        } else {
            serial_puts("[NTSYNC] Waiter capacity ");
            serial_putdec((uint64_t)g_waiter_capacity);
            serial_puts("\n");
        }
    }

    for (int i = 0; i < g_waiter_capacity; i++) {
        g_waiters[i].active = FALSE;
        g_waiters[i].completed = FALSE;
    }
    for (int i = 0; i < MAX_EVENTS; i++)
        g_events[i].allocated = FALSE;
#if NTSYNC_TRACE_DIAGNOSTICS
    for (int i = 0; i < NTSYNC_EVENT_TRACE_SLOTS; i++)
        g_event_traces[i].used = FALSE;
#endif
    for (int i = 0; i < MAX_MUTANTS; i++)
        g_mutants[i].allocated = FALSE;
    for (int i = 0; i < MAX_SEMAPHORES; i++)
        g_semaphores[i].allocated = FALSE;

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
