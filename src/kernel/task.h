/*
 * OsitoK - Task Control Block (TCB) and related definitions
 */
#ifndef OSITO_TASK_H
#define OSITO_TASK_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Task states */
typedef enum {
    TASK_STATE_FREE    = 0,   /* Slot is unused */
    TASK_STATE_READY   = 1,   /* Ready to run */
    TASK_STATE_RUNNING = 2,   /* Currently executing */
    TASK_STATE_BLOCKED = 3,   /* Waiting (for future use) */
    TASK_STATE_DEAD    = 4    /* Terminated */
} task_state_t;

/* Task function prototype */
typedef void (*task_func_t)(void *arg);

/*
 * Task Control Block
 *
 * The 'sp' field MUST be first — the context switch assembly
 * accesses it at offset 0 from the TCB pointer.
 */
typedef struct task_tcb {
    uint32_t    sp;                     /* Saved stack pointer (offset 0) */
    task_state_t state;                 /* Current state */
    uint8_t     id;                     /* Task ID (0..MAX_TASKS-1) */
    uint8_t     priority;               /* Priority (0=lowest, unused for now) */
    uint8_t     _pad;
    uint32_t    ticks_run;              /* Number of ticks this task has run */
    uint32_t    stack_base;             /* Bottom of stack allocation */
    uint32_t    stack_size;             /* Stack size in bytes */
    const char *name;                   /* Human-readable name */
} task_tcb_t;

/*
 * Context frame layout on the task's stack (pushed by context_switch.S)
 *
 * Offset  Register
 * ------  --------
 *  0x00   a0       (return address)
 *  0x04   a1       (stack pointer — saved for completeness)
 *  0x08   a2
 *  0x0C   a3
 *  0x10   a4
 *  0x14   a5
 *  0x18   a6
 *  0x1C   a7
 *  0x20   a8
 *  0x24   a9
 *  0x28   a10
 *  0x2C   a11
 *  0x30   a12
 *  0x34   a13
 *  0x38   a14
 *  0x3C   a15
 *  0x40   PS       (processor state)
 *  0x44   SAR      (shift amount register)
 *  0x48   EPC1     (exception PC — resume address)
 *  0x4C   (pad to 80 bytes)
 */

/* Context frame offsets (must match context_switch.S) */
#define CTX_A0    0x00
#define CTX_A1    0x04
#define CTX_A2    0x08
#define CTX_A3    0x0C
#define CTX_A4    0x10
#define CTX_A5    0x14
#define CTX_A6    0x18
#define CTX_A7    0x1C
#define CTX_A8    0x20
#define CTX_A9    0x24
#define CTX_A10   0x28
#define CTX_A11   0x2C
#define CTX_A12   0x30
#define CTX_A13   0x34
#define CTX_A14   0x38
#define CTX_A15   0x3C
#define CTX_PS    0x40
#define CTX_SAR   0x44
#define CTX_EPC1  0x48
#define CTX_PAD   0x4C

/* Total context frame size = 20 * 4 = 80 bytes */
#define CTX_SIZE  80

/* ====== Timer API (implemented in timer_tick.c) ====== */

/* Initialize FRC1 timer for periodic tick interrupts */
void timer_init(void);

/* C exception/interrupt dispatcher (called from context_switch.S) */
void os_exception_handler(void);

/* ====== Scheduler API (implemented in sched.cpp) ====== */

/* Initialize the scheduler */
void sched_init(void);

/* Create a new task. Returns task ID or -1 on failure. */
int task_create(const char *name, task_func_t func, void *arg, uint8_t priority);

/* Voluntarily yield the CPU */
void task_yield(void);

/* Called from timer ISR to select next task */
void schedule(void);

/* Start the scheduler (loads first task, never returns) */
void sched_start(void) __attribute__((noreturn));

/* Get current tick count */
uint32_t get_tick_count(void);

/* Simple delay (busy-wait in tick increments) */
void task_delay_ticks(uint32_t ticks);

/* Get pointer to current task TCB */
task_tcb_t *sched_current_task(void);

/* Get task pool (for ps command) */
task_tcb_t *sched_get_task_pool(void);

/* Assembly entry point: load context of current_task and start running */
extern void _xt_restore_context_and_rfe(void);

/* Assembly trampoline for new task entry */
extern void _task_entry_trampoline(void);

/* Called from asm trampoline when a task function returns */
void _task_exit_handler(void);

/* Global current task pointer (used by context_switch.S) */
extern task_tcb_t *current_task;

/* Global tick counter */
extern volatile uint32_t tick_count;

/* ISR stack top (defined in linker script) */
extern uint32_t _isr_stack_top;

#ifdef __cplusplus
}
#endif

#endif /* OSITO_TASK_H */
