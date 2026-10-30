/* Legacy Win32 timer queues. Deadlines are scheduled independently of guest
 * callbacks; bounded persistent workers may execute the same periodic timer
 * concurrently. Tokens, callbacks and internal handles belong to one process.
 * All queue metadata is protected by tq_lock. No guest callback or blocking
 * wait runs under that lock. Event signaling only wakes scheduler waiters. */
#include "timer_queue.h"
#include "kernel32_shim.h"
#include "ntsyscall.h"
#include "handle.h"
#include "compat32.h"
#include "../kernel/smp.h"

extern DWORD win32_current_process_id(void);
extern HANDLE_TABLE g_handle_table;

#define TQ_QUEUES 64
#define INFINITE 0xFFFFFFFFU
#define WAIT_OBJECT_0 0U
#define TQ_TIMERS 512
#define TQ_WORKERS 4
#define TQ_STARTING 1
#define TQ_READY 2
#define TQ_CLOSING 3
#define TQ_ONLY_ONCE 0x08U
#define TQ_TIMER_THREAD 0x20U
#define TQ_TRANSFER_TOKEN 0x100U
#define TQ_ALLOWED_FLAGS (0xffff0000U | 0x1U | 0x8U | 0x10U | 0x20U | 0x80U | 0x100U)

typedef struct {
    unsigned state;
    DWORD owner;
    HANDLE token;
    BOOL is_default;
    BOOL compat32;
    unsigned threads; /* Includes a creator reference until initialization ends. */
    unsigned next_timer[TQ_WORKERS + 1];
    DWORD callback_tid[TQ_WORKERS + 1];
    HANDLE callback_timer[TQ_WORKERS + 1];
    HANDLE changed, work, done, completion;
} TIMER_QUEUE;

typedef struct {
    BOOL used, canceled, armed;
    BOOL threadpool, set;
    TIMER_QUEUE *queue;
    HANDLE token, done, completion;
    K32_TIMER_CALLBACK callback;
    K32_POOL_TIMER_CALLBACK pool_callback;
    HANDLE idle;
    PVOID context;
    DWORD flags, period;
    ULONGLONG deadline, pending;
    unsigned running;
    unsigned waiters;
} QUEUE_TIMER;

static spinlock_t tq_lock = SPINLOCK_INIT;
static TIMER_QUEUE queues[TQ_QUEUES];
static QUEUE_TIMER timers[TQ_TIMERS];
static ULONG next_token = 0xD2200000U;
static uint32_t worker_thunk32;

static uint64_t tq_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&tq_lock);
    return flags;
}

static void tq_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&tq_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static HANDLE tq_token_locked(void)
{
    /* Never recycle a token, even when its array slot is reused. */
    if (next_token >= 0xD2FFFFFCU)
        return NULL;
    next_token += 4;
    return (HANDLE)(ULONG_PTR)next_token;
}

static TIMER_QUEUE *tq_lookup_locked(HANDLE token, DWORD owner)
{
    for (unsigned i = 0; i < TQ_QUEUES; i++) {
        TIMER_QUEUE *queue = &queues[i];
        if (queue->state && queue->owner == owner &&
            (token ? queue->token == token : queue->is_default))
            return queue;
    }
    return NULL;
}

static QUEUE_TIMER *tq_timer_locked(TIMER_QUEUE *queue, HANDLE token)
{
    if (!queue || queue->state != TQ_READY)
        return NULL;
    for (unsigned i = 0; i < TQ_TIMERS; i++) {
        QUEUE_TIMER *timer = &timers[i];
        if (timer->used && !timer->canceled && timer->queue == queue &&
            timer->token == token)
            return timer;
    }
    return NULL;
}

static BOOL tq_sync_completion(HANDLE completion)
{
    return g_compat32_mode ? (ULONG_PTR)completion == UINT32_MAX :
                            completion == (HANDLE)(ULONG_PTR)-1;
}

static void tq_close(HANDLE handle, DWORD owner)
{
    if (handle)
        nt_force_close_handle_for_process(handle, owner);
}

static BOOL tq_retain_completion(HANDLE completion, DWORD owner)
{
    if (!completion || tq_sync_completion(completion))
        return TRUE;
    NTSTATUS status = handle_retain_for_process(&g_handle_table, completion,
                                                owner);
    if (!NT_SUCCESS(status)) {
        SetLastError(6);
        return FALSE;
    }
    HANDLE_ENTRY *entry = handle_get_entry(&g_handle_table, completion);
    DWORD error = !entry || entry->type != OBJ_TYPE_EVENT ? 6 :
                  !(entry->access & (GENERIC_ALL | GENERIC_WRITE | 2U)) ? 5 : 0;
    if (error) {
        tq_close(completion, owner);
        SetLastError(error);
        return FALSE;
    }
    return TRUE;
}

static void tq_finish_timer_locked(QUEUE_TIMER *timer)
{
    if (!timer->canceled || timer->pending || timer->running || timer->waiters)
        return;
    DWORD owner = timer->queue->owner;
    SetEvent(timer->done);
    if (timer->completion)
        SetEvent(timer->completion);
    tq_close(timer->done, owner);
    tq_close(timer->completion, owner);
    tq_close(timer->idle, owner);
    memset(timer, 0, sizeof(*timer));
}

static void tq_finish_queue_locked(TIMER_QUEUE *queue)
{
    if (queue->state != TQ_CLOSING || queue->threads)
        return;
    if (queue->done)
        SetEvent(queue->done);
    if (queue->completion)
        SetEvent(queue->completion);
    tq_close(queue->changed, queue->owner);
    tq_close(queue->work, queue->owner);
    tq_close(queue->done, queue->owner);
    tq_close(queue->completion, queue->owner);
    memset(queue, 0, sizeof(*queue));
}

static QUEUE_TIMER *tq_take_work_locked(TIMER_QUEUE *queue, unsigned worker)
{
    for (unsigned i = 0; i < TQ_TIMERS; i++) {
        unsigned slot = (queue->next_timer[worker] + i) % TQ_TIMERS;
        QUEUE_TIMER *timer = &timers[slot];
        if (!timer->used || timer->queue != queue || !timer->pending ||
            ((timer->flags & TQ_TIMER_THREAD) != 0) != (worker == 0))
            continue;
        timer->pending--;
        timer->running++;
        queue->next_timer[worker] = (slot + 1) % TQ_TIMERS;
        queue->callback_tid[worker] = GetCurrentThreadId();
        queue->callback_timer[worker] = timer->token;
        return timer;
    }
    return NULL;
}

static DWORD tq_schedule_locked(TIMER_QUEUE *queue)
{
    ULONGLONG now = GetTickCount64();
    DWORD delay = INFINITE;
    for (unsigned i = 0; i < TQ_TIMERS; i++) {
        QUEUE_TIMER *timer = &timers[i];
        if (!timer->used || timer->queue != queue || timer->canceled ||
            !timer->armed)
            continue;
        if (now >= timer->deadline) {
            ULONGLONG elapsed = 1;
            if (timer->period) {
                elapsed += (now - timer->deadline) / timer->period;
                timer->deadline += elapsed * timer->period;
            } else {
                timer->armed = FALSE;
            }
            timer->pending += elapsed;
            if (timer->threadpool)
                ResetEvent(timer->idle);
            if (!(timer->flags & TQ_TIMER_THREAD))
                SetEvent(queue->work);
        }
        if (timer->armed) {
            ULONGLONG remaining = timer->deadline - now;
            DWORD bounded = remaining >= INFINITE ? INFINITE - 1 : (DWORD)remaining;
            if (bounded < delay)
                delay = bounded;
        }
    }
    return delay;
}

static DWORD WINAPI tq_worker(PVOID parameter)
{
    ULONG_PTR encoded = (ULONG_PTR)parameter;
    unsigned slot = (unsigned)(encoded & 255U);
    unsigned worker = (unsigned)(encoded >> 8);
    if (!slot || slot > TQ_QUEUES || worker > TQ_WORKERS)
        return 87;
    TIMER_QUEUE *queue = &queues[slot - 1];
    for (;;) {
        uint64_t flags = tq_lock_irqsave();
        if (queue->state == TQ_STARTING) {
            tq_unlock_irqrestore(flags);
            Sleep(1);
            continue;
        }
        DWORD delay = worker || queue->state == TQ_CLOSING ? INFINITE :
                      tq_schedule_locked(queue);
        QUEUE_TIMER *timer = tq_take_work_locked(queue, worker);
        if (!timer && queue->state == TQ_CLOSING) {
            queue->threads--;
            tq_finish_queue_locked(queue);
            tq_unlock_irqrestore(flags);
            return 0;
        }
        if (!timer) {
            HANDLE wake = worker ? queue->work : queue->changed;
            if (worker)
                ResetEvent(wake);
            tq_unlock_irqrestore(flags);
            WaitForSingleObject(wake, delay);
            continue;
        }
        K32_TIMER_CALLBACK callback = timer->callback;
        K32_POOL_TIMER_CALLBACK pool_callback = timer->pool_callback;
        BOOL threadpool = timer->threadpool;
        HANDLE token = timer->token;
        /* The opaque instance identifies this worker's active callback. */
        PVOID instance = (PVOID)(ULONG_PTR)(0xD3200000U +
                         (((unsigned)(queue - queues) * (TQ_WORKERS + 1) + worker) * 4));
        PVOID context = timer->context;
        BOOL compat32 = queue->compat32;
        tq_unlock_irqrestore(flags);

        if (threadpool && compat32) {
            uint32_t args[3] = { (uint32_t)(ULONG_PTR)instance,
                (uint32_t)(ULONG_PTR)context, (uint32_t)(ULONG_PTR)token };
            compat32_callback_args((uint32_t)(ULONG_PTR)pool_callback, 3, args);
        } else if (threadpool) {
            pool_callback(instance, context, token);
        } else if (compat32) {
            uint32_t args[2] = { (uint32_t)(ULONG_PTR)context, 1 };
            compat32_callback_args((uint32_t)(ULONG_PTR)callback, 2, args);
        } else {
            callback(context, TRUE);
        }

        flags = tq_lock_irqsave();
        queue->callback_tid[worker] = 0;
        queue->callback_timer[worker] = NULL;
        timer->running--;
        if (timer->threadpool && !timer->pending && !timer->running)
            SetEvent(timer->idle);
        tq_finish_timer_locked(timer);
        tq_unlock_irqrestore(flags);
    }
}

static HANDLE tq_create(BOOL is_default)
{
    DWORD owner = win32_current_process_id();
    BOOL compat32 = g_compat32_mode;
    for (;;) {
        uint64_t flags = tq_lock_irqsave();
        TIMER_QUEUE *queue = is_default ? tq_lookup_locked(NULL, owner) : NULL;
        if (queue) {
            HANDLE token = queue->token;
            unsigned state = queue->state;
            tq_unlock_irqrestore(flags);
            if (state == TQ_READY)
                return token;
            if (state == TQ_CLOSING) {
                SetLastError(6);
                return NULL;
            }
            Sleep(1);
            continue;
        }
        for (unsigned i = 0; i < TQ_QUEUES; i++) {
            if (!queues[i].state) {
                queue = &queues[i];
                break;
            }
        }
        HANDLE token = queue ? tq_token_locked() : NULL;
        if (!token) {
            tq_unlock_irqrestore(flags);
            SetLastError(8);
            return NULL;
        }
        memset(queue, 0, sizeof(*queue));
        queue->state = TQ_STARTING;
        queue->owner = owner;
        queue->compat32 = compat32;
        queue->is_default = is_default;
        queue->token = token;
        queue->threads = 1;
        tq_unlock_irqrestore(flags);

        queue->changed = CreateEventA(NULL, FALSE, FALSE, NULL);
        queue->work = CreateEventA(NULL, TRUE, FALSE, NULL);
        queue->done = CreateEventA(NULL, TRUE, FALSE, NULL);
        BOOL ok = queue->changed && queue->work && queue->done;
        LPTHREAD_START_ROUTINE start = tq_worker;
        if (ok && compat32) {
            uint32_t thunk = __atomic_load_n(&worker_thunk32, __ATOMIC_ACQUIRE);
            if (!thunk) {
                thunk = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)tq_worker,
                                                "kernel32!timer_queue", 1,
                                                CC_STDCALL);
                if (thunk)
                    __atomic_store_n(&worker_thunk32, thunk, __ATOMIC_RELEASE);
            }
            ok = thunk != 0;
            start = (LPTHREAD_START_ROUTINE)(ULONG_PTR)thunk;
        }
        for (unsigned worker = 0; ok && worker <= TQ_WORKERS; worker++) {
            flags = tq_lock_irqsave();
            queue->threads++;
            tq_unlock_irqrestore(flags);
            ULONG_PTR encoded = (unsigned)(queue - queues) + 1 + (worker << 8);
            HANDLE thread = CreateThread(NULL, 0, start, (PVOID)encoded, 0, NULL);
            if (thread) {
                CloseHandle(thread);
            } else {
                flags = tq_lock_irqsave();
                queue->threads--;
                tq_unlock_irqrestore(flags);
                ok = FALSE;
            }
        }
        flags = tq_lock_irqsave();
        queue->state = ok ? TQ_READY : TQ_CLOSING;
        queue->threads--;
        tq_finish_queue_locked(queue);
        tq_unlock_irqrestore(flags);
        if (!ok)
            SetLastError(8);
        return ok ? token : NULL;
    }
}

HANDLE WINAPI k32_CreateTimerQueue(void)
{
    return tq_create(FALSE);
}

BOOL WINAPI k32_CreateTimerQueueTimer(HANDLE *out, HANDLE queue_handle,
                                       K32_TIMER_CALLBACK callback,
                                       PVOID context, DWORD due, DWORD period,
                                       ULONG timer_flags)
{
    if (!out || !callback || (timer_flags & ~TQ_ALLOWED_FLAGS) ||
        ((timer_flags & TQ_ONLY_ONCE) && period)) {
        SetLastError(87);
        return FALSE;
    }
    /* Token transfer needs an owned impersonation-token snapshot. Do not
     * silently execute security-sensitive callbacks with the wrong identity. */
    if (timer_flags & TQ_TRANSFER_TOKEN) {
        SetLastError(50);
        return FALSE;
    }
    if (!queue_handle && !tq_create(TRUE))
        return FALSE;
    DWORD owner = win32_current_process_id();
    HANDLE done = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!done)
        return FALSE;
    uint64_t flags = tq_lock_irqsave();
    TIMER_QUEUE *queue = tq_lookup_locked(queue_handle, owner);
    QUEUE_TIMER *timer = NULL;
    if (queue && queue->state == TQ_READY) {
        for (unsigned i = 0; i < TQ_TIMERS; i++) {
            if (!timers[i].used) {
                timer = &timers[i];
                break;
            }
        }
    }
    HANDLE token = timer ? tq_token_locked() : NULL;
    if (!token) {
        DWORD error = !queue || queue->state != TQ_READY ? 6 : 8;
        tq_unlock_irqrestore(flags);
        tq_close(done, owner);
        SetLastError(error);
        return FALSE;
    }
    memset(timer, 0, sizeof(*timer));
    timer->used = TRUE;
    timer->queue = queue;
    timer->token = token;
    timer->done = done;
    timer->callback = callback;
    timer->context = context;
    timer->flags = timer_flags;
    timer->period = period;
    timer->armed = due != INFINITE;
    timer->deadline = GetTickCount64() + due;
    /* PHANDLE is four bytes in PE32; the next guest stack field is not ours. */
    if (queue->compat32)
        *(uint32_t *)(void *)out = (uint32_t)(ULONG_PTR)token;
    else
        *out = token;
    SetEvent(queue->changed);
    tq_unlock_irqrestore(flags);
    return TRUE;
}

BOOL WINAPI k32_ChangeTimerQueueTimer(HANDLE queue_handle, HANDLE timer_handle,
                                       ULONG due, ULONG period)
{
    uint64_t flags = tq_lock_irqsave();
    TIMER_QUEUE *queue = tq_lookup_locked(queue_handle, win32_current_process_id());
    QUEUE_TIMER *timer = tq_timer_locked(queue, timer_handle);
    if (!timer || ((timer->flags & TQ_ONLY_ONCE) && period)) {
        tq_unlock_irqrestore(flags);
        SetLastError(timer ? 87 : 6);
        return FALSE;
    }
    /* An expired one-shot cannot be rearmed by ChangeTimerQueueTimer. */
    if (timer->armed || timer->deadline > GetTickCount64() || timer->period) {
        timer->deadline = GetTickCount64() + due;
        timer->period = period;
        timer->armed = due != INFINITE;
        SetEvent(queue->changed);
    }
    tq_unlock_irqrestore(flags);
    return TRUE;
}

static BOOL tq_callback_is_current(TIMER_QUEUE *queue, HANDLE timer)
{
    DWORD tid = GetCurrentThreadId();
    for (unsigned i = 0; i <= TQ_WORKERS; i++) {
        if (queue->callback_tid[i] == tid &&
            (!timer || queue->callback_timer[i] == timer))
            return TRUE;
    }
    return FALSE;
}

BOOL WINAPI k32_DeleteTimerQueueTimer(HANDLE queue_handle, HANDLE timer_handle,
                                       HANDLE completion)
{
    DWORD owner = win32_current_process_id();
    BOOL synchronous = tq_sync_completion(completion);
    if (!tq_retain_completion(completion, owner))
        return FALSE;
    uint64_t flags = tq_lock_irqsave();
    TIMER_QUEUE *queue = tq_lookup_locked(queue_handle, owner);
    QUEUE_TIMER *timer = tq_timer_locked(queue, timer_handle);
    DWORD error = !timer ? 6 : synchronous &&
                  tq_callback_is_current(queue, timer_handle) ? 1131 : 0;
    if (error) {
        tq_unlock_irqrestore(flags);
        if (!synchronous)
            tq_close(completion, owner);
        SetLastError(error);
        return FALSE;
    }
    HANDLE done = timer->done;
    if (synchronous && !NT_SUCCESS(handle_retain_for_process(
                                     &g_handle_table, done, owner))) {
        tq_unlock_irqrestore(flags);
        SetLastError(8);
        return FALSE;
    }
    BOOL pending = timer->pending || timer->running;
    timer->completion = synchronous ? NULL : completion;
    timer->canceled = TRUE;
    timer->armed = FALSE;
    tq_finish_timer_locked(timer);
    SetEvent(queue->changed);
    tq_unlock_irqrestore(flags);
    if (synchronous) {
        DWORD result = WaitForSingleObject(done, INFINITE);
        tq_close(done, owner);
        return result == WAIT_OBJECT_0;
    }
    if (!completion && pending) {
        SetLastError(997); /* ERROR_IO_PENDING: deletion is already accepted. */
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI k32_DeleteTimerQueueEx(HANDLE queue_handle, HANDLE completion)
{
    DWORD owner = win32_current_process_id();
    BOOL synchronous = tq_sync_completion(completion);
    if (!tq_retain_completion(completion, owner))
        return FALSE;
    uint64_t flags = tq_lock_irqsave();
    TIMER_QUEUE *queue = queue_handle ? tq_lookup_locked(queue_handle, owner) : NULL;
    DWORD error = !queue || queue->state != TQ_READY ? 6 : synchronous &&
                  tq_callback_is_current(queue, NULL) ? 1131 : 0;
    if (error) {
        tq_unlock_irqrestore(flags);
        if (!synchronous)
            tq_close(completion, owner);
        SetLastError(error);
        return FALSE;
    }
    HANDLE done = queue->done;
    if (synchronous && !NT_SUCCESS(handle_retain_for_process(
                                     &g_handle_table, done, owner))) {
        tq_unlock_irqrestore(flags);
        SetLastError(8);
        return FALSE;
    }
    queue->state = TQ_CLOSING;
    queue->completion = synchronous ? NULL : completion;
    for (unsigned i = 0; i < TQ_TIMERS; i++) {
        QUEUE_TIMER *timer = &timers[i];
        if (timer->used && timer->queue == queue) {
            timer->canceled = TRUE;
            timer->armed = FALSE;
            tq_finish_timer_locked(timer);
        }
    }
    SetEvent(queue->changed);
    SetEvent(queue->work);
    tq_unlock_irqrestore(flags);
    if (synchronous) {
        DWORD result = WaitForSingleObject(done, INFINITE);
        tq_close(done, owner);
        return result == WAIT_OBJECT_0;
    }
    return TRUE;
}

BOOL WINAPI k32_DeleteTimerQueue(HANDLE queue)
{
    return k32_DeleteTimerQueueEx(queue, NULL);
}

static QUEUE_TIMER *tq_pool_lookup_locked(HANDLE token)
{
    TIMER_QUEUE *queue = tq_lookup_locked(NULL, win32_current_process_id());
    QUEUE_TIMER *timer = tq_timer_locked(queue, token);
    return timer && timer->threadpool ? timer : NULL;
}

static BOOL tq_pool_environment(PCVOID environment)
{
    if (!environment) return TRUE;
    extern BOOL win32_user_range_readable(PCVOID, SIZE_T, BOOL);
    BOOL compat = g_compat32_mode;
    if (!win32_user_range_readable(environment, 4, compat)) {
        SetLastError(998); return FALSE;
    }
    const BYTE *bytes = environment;
    DWORD version = *(const DWORD *)bytes;
    SIZE_T length = version == 1 ? (compat ? 32 : 64) : (compat ? 40 : 72);
    if ((version != 1 && version != 3) ||
        !win32_user_range_readable(environment, length, compat)) {
        SetLastError(87); return FALSE;
    }
    /* A custom pool, cleanup group, activation/DLL reference or finalizer
     * needs its own lifetime protocol. Never silently discard the request. */
    SIZE_T first = compat ? 4 : 8, stride = compat ? 4 : 8;
    for (unsigned i = 0; i < 6; i++) {
        ULONG_PTR field = compat ? *(const DWORD *)(bytes + first + i * stride) :
                                  *(const ULONG_PTR *)(bytes + first + i * stride);
        if (field) { SetLastError(50); return FALSE; }
    }
    SIZE_T flags_offset = first + 6 * stride;
    if (*(const DWORD *)(bytes + flags_offset) ||
        (version == 3 && *(const DWORD *)(bytes + flags_offset + 4) != 1)) {
        SetLastError(50); return FALSE;
    }
    if (version == 3 && *(const DWORD *)(bytes + flags_offset + 8) != length) {
        SetLastError(87); return FALSE;
    }
    return TRUE;
}

HANDLE WINAPI k32_CreateThreadpoolTimer(K32_POOL_TIMER_CALLBACK callback,
                                        PVOID context, PCVOID environment)
{
    if (!callback) { SetLastError(87); return NULL; }
    if (!tq_pool_environment(environment)) return NULL;
    HANDLE idle = CreateEventA(NULL, TRUE, TRUE, NULL);
    if (!idle) return NULL;
    HANDLE token = NULL;
    /* Disarmed until modern callback metadata is published. No worker can
     * invoke the legacy signature, and the token is not exposed yet. */
    if (!k32_CreateTimerQueueTimer(&token, NULL, (K32_TIMER_CALLBACK)callback,
                                    context, INFINITE, 0, 0)) {
        DWORD error = GetLastError();
        tq_close(idle, win32_current_process_id());
        SetLastError(error); return NULL;
    }
    uint64_t flags = tq_lock_irqsave();
    TIMER_QUEUE *queue = tq_lookup_locked(NULL, win32_current_process_id());
    QUEUE_TIMER *timer = tq_timer_locked(queue, token);
    timer->threadpool = TRUE;
    timer->pool_callback = callback;
    timer->idle = idle;
    tq_unlock_irqrestore(flags);
    return token;
}

void WINAPI k32_SetThreadpoolTimer(HANDLE token, PCVOID due_pointer,
                                   DWORD period, DWORD window)
{
    /* Exact deadlines are within every allowed coalescing window. */
    (void)window;
    ULONGLONG delay = 0;
    if (due_pointer) {
        LONGLONG due;
        memcpy(&due, due_pointer, sizeof(due));
        ULONGLONG ticks;
        if (due < 0) {
            ticks = (ULONGLONG)(-(due + 1)) + 1;
        } else {
            ULONGLONG now;
            GetSystemTimeAsFileTime(&now);
            ticks = (ULONGLONG)due > now ? (ULONGLONG)due - now : 0;
        }
        delay = (ticks + 9999) / 10000;
    }
    uint64_t flags = tq_lock_irqsave();
    QUEUE_TIMER *timer = tq_pool_lookup_locked(token);
    if (!timer) { tq_unlock_irqrestore(flags); SetLastError(87); return; }
    timer->set = timer->armed = due_pointer != NULL;
    timer->period = period;
    ULONGLONG now = GetTickCount64();
    timer->deadline = delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
    SetEvent(timer->queue->changed);
    tq_unlock_irqrestore(flags);
}

BOOL WINAPI k32_IsThreadpoolTimerSet(HANDLE token)
{
    uint64_t flags = tq_lock_irqsave();
    QUEUE_TIMER *timer = tq_pool_lookup_locked(token);
    BOOL set = timer && timer->set;
    tq_unlock_irqrestore(flags);
    return set;
}

void WINAPI k32_WaitForThreadpoolTimerCallbacks(HANDLE token, BOOL cancel_pending)
{
    uint64_t flags = tq_lock_irqsave();
    QUEUE_TIMER *timer = tq_pool_lookup_locked(token);
    if (!timer) { tq_unlock_irqrestore(flags); SetLastError(87); return; }
    if (cancel_pending) timer->pending = 0;
    if (!timer->pending && !timer->running) SetEvent(timer->idle);
    HANDLE idle = timer->idle;
    timer->waiters++;
    tq_unlock_irqrestore(flags);
    WaitForSingleObject(idle, INFINITE);
    flags = tq_lock_irqsave();
    timer->waiters--;
    tq_finish_timer_locked(timer);
    tq_unlock_irqrestore(flags);
}

void WINAPI k32_CloseThreadpoolTimer(HANDLE token)
{
    uint64_t flags = tq_lock_irqsave();
    QUEUE_TIMER *timer = tq_pool_lookup_locked(token);
    if (timer) {
        timer->canceled = TRUE;
        timer->armed = timer->set = FALSE;
        TIMER_QUEUE *queue = timer->queue;
        tq_finish_timer_locked(timer);
        SetEvent(queue->changed);
    }
    tq_unlock_irqrestore(flags);
}

void k32_timer_queues_release_process(DWORD owner)
{
    /* Called only after all of the process's Win32 workers were terminated. */
    uint64_t flags = tq_lock_irqsave();
    for (unsigned i = 0; i < TQ_TIMERS; i++) {
        QUEUE_TIMER *timer = &timers[i];
        if (timer->used && timer->queue->owner == owner) {
            tq_close(timer->done, owner);
            tq_close(timer->completion, owner);
            tq_close(timer->idle, owner);
            memset(timer, 0, sizeof(*timer));
        }
    }
    for (unsigned i = 0; i < TQ_QUEUES; i++) {
        TIMER_QUEUE *queue = &queues[i];
        if (queue->state && queue->owner == owner) {
            tq_close(queue->changed, owner);
            tq_close(queue->work, owner);
            tq_close(queue->done, owner);
            tq_close(queue->completion, owner);
            memset(queue, 0, sizeof(*queue));
        }
    }
    tq_unlock_irqrestore(flags);
}
