/*
 * OsitoK - Software timers
 *
 * Simple timer registry: up to SWTIMER_MAX active timers.
 * swtimer_tick() is called from the FRC1 ISR every 10ms.
 * Callbacks execute in ISR context — keep them fast!
 */

#include "kernel/timer_sw.h"
#include "kernel/task.h"

extern "C" {

/* Registry of active timers */
static swtimer_t *timer_list[SWTIMER_MAX];
static int timer_count = 0;

void swtimer_init(swtimer_t *t, swtimer_cb_t cb, void *arg)
{
    t->callback = cb;
    t->arg = arg;
    t->interval = 0;
    t->expire_tick = 0;
    t->mode = SWTIMER_ONESHOT;
    t->active = 0;
}

void swtimer_register(swtimer_t *t)
{
    /* Check if already registered */
    for (int i = 0; i < timer_count; i++) {
        if (timer_list[i] == t) return;
    }
    if (timer_count < SWTIMER_MAX) {
        timer_list[timer_count++] = t;
    }
}

void swtimer_unregister(swtimer_t *t)
{
    for (int i = 0; i < timer_count; i++) {
        if (timer_list[i] == t) {
            /* Shift down */
            for (int j = i; j < timer_count - 1; j++) {
                timer_list[j] = timer_list[j + 1];
            }
            timer_count--;
            return;
        }
    }
}

void swtimer_start(swtimer_t *t, uint32_t ticks, uint8_t mode)
{
    uint32_t ps = irq_save();
    t->interval = ticks;
    t->mode = mode;
    t->expire_tick = tick_count + ticks;
    t->active = 1;
    swtimer_register(t);
    irq_restore(ps);
}

void swtimer_stop(swtimer_t *t)
{
    uint32_t ps = irq_save();
    t->active = 0;
    swtimer_unregister(t);
    irq_restore(ps);
}

/*
 * swtimer_tick - process all active timers
 *
 * Called from the FRC1 ISR (timer_tick.c) every tick.
 * Runs in exception context with interrupts masked.
 */
void swtimer_tick(void)
{
    for (int i = 0; i < timer_count; ) {
        swtimer_t *t = timer_list[i];

        if (!t->active) {
            i++;
            continue;
        }

        if ((int32_t)(tick_count - t->expire_tick) >= 0) {
            /* Timer expired — fire callback */
            t->callback(t->arg);

            if (t->mode == SWTIMER_PERIODIC) {
                /* Reload for next period */
                t->expire_tick = tick_count + t->interval;
                i++;
            } else {
                /* One-shot: deactivate and remove */
                t->active = 0;
                swtimer_unregister(t);
                /* Don't increment i — list shifted down */
            }
        } else {
            i++;
        }
    }
}

int swtimer_active_count(void)
{
    return timer_count;
}

} /* extern "C" */
