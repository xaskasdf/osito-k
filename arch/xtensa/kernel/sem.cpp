/*
 * OsitoK - Counting semaphores and mutexes
 *
 * Semaphore wait queue is a simple FIFO array (MAX_TASKS = 8,
 * so no need for linked lists). Tasks that block go to
 * TASK_STATE_BLOCKED and are skipped by the scheduler.
 *
 * All critical sections use irq_save/irq_restore to prevent
 * races between task context and timer ISR.
 */

#include "kernel/sem.h"
#include "kernel/task.h"

extern "C" {

/* ====== Counting Semaphore ====== */

void sem_init(sem_t *s, int32_t initial_count)
{
    s->count = initial_count;
    s->num_waiters = 0;
}

void sem_wait(sem_t *s)
{
    uint32_t ps = irq_save();

    if (s->count > 0) {
        /* Resource available — take it immediately */
        s->count--;
        irq_restore(ps);
        return;
    }

    /* No resource available — block this task */
    s->waiters[s->num_waiters++] = current_task->id;
    current_task->state = TASK_STATE_BLOCKED;

    irq_restore(ps);

    /* Yield to scheduler. Since we're BLOCKED, the scheduler
     * will skip us. We resume here when sem_post wakes us. */
    task_yield();
}

int sem_trywait(sem_t *s)
{
    uint32_t ps = irq_save();

    if (s->count > 0) {
        s->count--;
        irq_restore(ps);
        return 0;
    }

    irq_restore(ps);
    return -1;
}

void sem_post(sem_t *s)
{
    uint32_t ps = irq_save();

    if (s->num_waiters > 0) {
        /* Wake the first waiter (FIFO order) */
        uint8_t tid = s->waiters[0];

        /* Shift the wait queue down */
        for (uint8_t i = 1; i < s->num_waiters; i++) {
            s->waiters[i - 1] = s->waiters[i];
        }
        s->num_waiters--;

        /* Wake the task */
        task_tcb_t *pool = sched_get_task_pool();
        pool[tid].state = TASK_STATE_READY;

        /* Don't increment count — the resource goes directly
         * from poster to waiter (classic semaphore semantics). */
    } else {
        /* No waiters — increment the count */
        s->count++;
    }

    irq_restore(ps);
}

int32_t sem_getcount(sem_t *s)
{
    return s->count;
}

/* ====== Binary Mutex ====== */

void mutex_init(mutex_t *m)
{
    sem_init(&m->sem, 1);
}

void mutex_lock(mutex_t *m)
{
    sem_wait(&m->sem);
}

int mutex_trylock(mutex_t *m)
{
    return sem_trywait(&m->sem);
}

void mutex_unlock(mutex_t *m)
{
    sem_post(&m->sem);
}

} /* extern "C" */
