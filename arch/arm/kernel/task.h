#ifndef OSITO_ARM_TASK_H
#define OSITO_ARM_TASK_H

#include "../include/types.h"

#define MAX_TASKS       16
#define TASK_STACK_SIZE 8192

typedef enum {
    TASK_STATE_FREE    = 0,
    TASK_STATE_READY   = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3,
    TASK_STATE_DEAD    = 4,
} task_state_t;

/* Task Control Block — sp MUST be at offset 0 (used by asm) */
typedef struct {
    uint64_t        sp;         /* Offset 0: saved stack pointer */
    task_state_t    state;
    uint8_t         id;
    uint8_t         priority;
    uint8_t         _pad;
    uint32_t        ticks_run;
    uint64_t        wake_tick;
    uint8_t        *stack_base;
    uint32_t        stack_size;
    const char     *name;
} task_tcb_t;

extern task_tcb_t  task_pool[MAX_TASKS];
extern task_tcb_t *current_task;
extern volatile int need_schedule;

void schedule(void);
void task_exit_handler(void);

#endif
