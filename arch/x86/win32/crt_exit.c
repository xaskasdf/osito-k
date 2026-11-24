#include "crt_exit.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);
extern void sched_yield(void);
extern DWORD win32_current_process_id(void);
extern void compat32_callback(uint32_t function);

typedef struct CRT_EXIT_CALLBACK {
    struct CRT_EXIT_CALLBACK *next;
    PVOID function;
    DWORD pid;
    BOOL compat32;
} CRT_EXIT_CALLBACK;
static CRT_EXIT_CALLBACK *crt_exit_callbacks;
static volatile uint32_t crt_exit_mutex;

static void crt_exit_callbacks_lock(void)
{
    while (__sync_lock_test_and_set(&crt_exit_mutex, 1)) {
        for (unsigned i = 0; i < 100; ++i) __asm__ volatile("pause" ::: "memory");
        sched_yield();
    }
}

int crt_exit_callbacks_register(PVOID function)
{
    if (!function || (g_compat32_mode && (ULONG_PTR)function > UINT32_MAX)) return -1;
    CRT_EXIT_CALLBACK *entry = kmalloc(sizeof(*entry));
    if (!entry) return -1;
    entry->function = function;
    entry->pid = win32_current_process_id();
    if (!entry->pid) entry->pid = 1;
    entry->compat32 = !!g_compat32_mode;
    crt_exit_callbacks_lock();
    entry->next = crt_exit_callbacks;
    crt_exit_callbacks = entry;
    __sync_lock_release(&crt_exit_mutex);
    return 0;
}

void crt_exit_callbacks_run(void)
{
    DWORD pid = win32_current_process_id();
    if (!pid) pid = 1;
    for (;;) {
        crt_exit_callbacks_lock();
        CRT_EXIT_CALLBACK **link = &crt_exit_callbacks;
        while (*link && (*link)->pid != pid) link = &(*link)->next;
        CRT_EXIT_CALLBACK *entry = *link;
        if (entry) *link = entry->next;
        __sync_lock_release(&crt_exit_mutex);
        if (!entry) return;
        PVOID function = entry->function;
        BOOL compat32 = entry->compat32;
        kfree(entry);
        /* Remove before invoking: callbacks can register another callback,
         * reenter exit, or terminate the process without returning. */
        if (compat32) compat32_callback((uint32_t)(ULONG_PTR)function);
        else ((void (WINAPI *)(void))function)();
    }
}

void crt_exit_callbacks_release(DWORD pid)
{
    CRT_EXIT_CALLBACK *released = NULL;
    crt_exit_callbacks_lock();
    CRT_EXIT_CALLBACK **link = &crt_exit_callbacks;
    while (*link) {
        CRT_EXIT_CALLBACK *entry = *link;
        if (entry->pid == pid) {
            *link = entry->next;
            entry->next = released;
            released = entry;
        } else link = &entry->next;
    }
    __sync_lock_release(&crt_exit_mutex);
    while (released) {
        CRT_EXIT_CALLBACK *next = released->next;
        kfree(released);
        released = next;
    }
}
