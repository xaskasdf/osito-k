/*
 * Native Win32 interrupt-state audit.
 *
 * Build with WIN32_IF_AUDIT=1. Compiler hooks observe only calls whose
 * immediate caller is a lower-half PE address. This catches a shim that
 * enters with IF set and returns to PE code with IF clear, without reporting
 * internal helpers that intentionally run inside irq-save regions.
 */

#include <stdint.h>

#define IF_MASK                 (1ULL << 9)
#define IF_AUDIT_TASKS          256
#define IF_AUDIT_NESTING        32
#define LOWER_CANONICAL_LIMIT   0x0000800000000000ULL

#define NO_INSTRUMENT __attribute__((no_instrument_function))

extern int sched_current_get(void);
extern int32_t proc_current_pid(void);
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

typedef struct {
    uint64_t function;
    uint64_t caller;
    uint64_t entry_flags;
    uint8_t report_boundary;
} if_audit_frame_t;

typedef struct {
    uint8_t depth;
    if_audit_frame_t frames[IF_AUDIT_NESTING];
} if_audit_task_t;

static if_audit_task_t if_audit_tasks[IF_AUDIT_TASKS];
static volatile uint32_t if_audit_log_count;

static NO_INSTRUMENT uint64_t if_audit_read_flags(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) :: "memory");
    return flags;
}

static NO_INSTRUMENT int if_audit_is_pe_caller(uint64_t caller)
{
    return caller >= 0x10000ULL && caller < LOWER_CANONICAL_LIMIT;
}

static NO_INSTRUMENT void if_audit_log(const char *event,
                                       const if_audit_frame_t *frame,
                                       uint64_t exit_flags)
{
    if (__atomic_fetch_add(&if_audit_log_count, 1, __ATOMIC_RELAXED) >= 64)
        return;

    uint64_t rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    serial_puts(event);
    serial_puts(" kpid=");
    serial_putdec((uint64_t)(uint32_t)proc_current_pid());
    serial_puts(" fn=0x");
    serial_puthex(frame->function, 16);
    serial_puts(" caller=0x");
    serial_puthex(frame->caller, 16);
    serial_puts(" entry=0x");
    serial_puthex(frame->entry_flags, 16);
    serial_puts(" exit=0x");
    serial_puthex(exit_flags, 16);
    serial_puts(" rsp=0x");
    serial_puthex(rsp, 16);
    serial_puts("\n");
}

NO_INSTRUMENT void __cyg_profile_func_enter(void *this_fn, void *call_site)
{
    uint64_t caller = (uint64_t)(uintptr_t)call_site;
    if (!if_audit_is_pe_caller(caller))
        return;

    int task = sched_current_get();
    if (task < 0 || task >= IF_AUDIT_TASKS)
        return;

    if_audit_task_t *state = &if_audit_tasks[task];
    if (state->depth >= IF_AUDIT_NESTING)
        state->depth = 0;

    uint8_t report_boundary = state->depth == 0 ||
        state->frames[state->depth - 1].caller != caller;
    if_audit_frame_t *frame = &state->frames[state->depth++];
    frame->function = (uint64_t)(uintptr_t)this_fn;
    frame->caller = caller;
    frame->entry_flags = if_audit_read_flags();
    frame->report_boundary = report_boundary;

    if (frame->report_boundary && !(frame->entry_flags & IF_MASK))
        if_audit_log("[WIN32-IF-ENTRY]", frame, frame->entry_flags);
}

NO_INSTRUMENT void __cyg_profile_func_exit(void *this_fn, void *call_site)
{
    uint64_t caller = (uint64_t)(uintptr_t)call_site;
    if (!if_audit_is_pe_caller(caller))
        return;

    int task = sched_current_get();
    if (task < 0 || task >= IF_AUDIT_TASKS)
        return;

    if_audit_task_t *state = &if_audit_tasks[task];
    if (!state->depth)
        return;

    if_audit_frame_t *frame = &state->frames[--state->depth];
    if (frame->function != (uint64_t)(uintptr_t)this_fn ||
        frame->caller != caller) {
        state->depth = 0;
        return;
    }

    uint64_t exit_flags = if_audit_read_flags();
    if (frame->report_boundary &&
        (frame->entry_flags & IF_MASK) && !(exit_flags & IF_MASK))
        if_audit_log("[WIN32-IF-LEAK]", frame, exit_flags);
}
