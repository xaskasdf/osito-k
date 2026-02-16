/*
 * OsitoK - Counting semaphores and mutexes
 *
 * sem_t:   Counting semaphore with FIFO wait queue.
 * mutex_t: Binary mutex (sem initialized to 1).
 *
 * All operations are safe to call from task context.
 * Do NOT call from ISR context (they may yield).
 */
#ifndef OSITO_SEM_H
#define OSITO_SEM_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====== Counting Semaphore ====== */

typedef struct {
    volatile int32_t count;             /* >0: available, <=0: tasks waiting */
    uint8_t  waiters[MAX_TASKS];        /* FIFO of blocked task IDs */
    uint8_t  num_waiters;               /* Number of tasks in wait queue */
} sem_t;

/* Initialize semaphore with given count (e.g. 0 for sync, N for resources) */
void sem_init(sem_t *s, int32_t initial_count);

/* Decrement count. Blocks if count <= 0 until another task posts. */
void sem_wait(sem_t *s);

/* Non-blocking wait. Returns 0 on success, -1 if would block. */
int sem_trywait(sem_t *s);

/* Increment count. Wakes one waiting task if any. */
void sem_post(sem_t *s);

/* Read current count (informational, may change immediately) */
int32_t sem_getcount(sem_t *s);

/* ====== Binary Mutex ====== */

typedef struct {
    sem_t sem;
} mutex_t;

/* Initialize mutex (unlocked) */
void mutex_init(mutex_t *m);

/* Acquire mutex. Blocks if already held by another task. */
void mutex_lock(mutex_t *m);

/* Non-blocking lock. Returns 0 on success, -1 if held. */
int mutex_trylock(mutex_t *m);

/* Release mutex. Wakes one waiting task if any. */
void mutex_unlock(mutex_t *m);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_SEM_H */
