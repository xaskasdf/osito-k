/*
 * OsitoK - Software timers
 *
 * One-shot and periodic timers built on the 100Hz system tick.
 * Timer callbacks run in ISR context (keep them short!).
 *
 * Usage:
 *   swtimer_t t;
 *   swtimer_init(&t, my_callback, my_arg);
 *   swtimer_start(&t, 100, SWTIMER_PERIODIC);  // 1 second periodic
 *   swtimer_stop(&t);
 */
#ifndef OSITO_TIMER_SW_H
#define OSITO_TIMER_SW_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWTIMER_MAX  8

/* Timer modes */
#define SWTIMER_ONESHOT   0
#define SWTIMER_PERIODIC  1

/* Timer callback type (runs in ISR context!) */
typedef void (*swtimer_cb_t)(void *arg);

typedef struct {
    swtimer_cb_t callback;          /* Function to call on expiry */
    void        *arg;               /* Argument passed to callback */
    uint32_t     interval;          /* Period in ticks (for periodic) */
    uint32_t     expire_tick;       /* Tick at which timer fires */
    uint8_t      mode;              /* SWTIMER_ONESHOT or SWTIMER_PERIODIC */
    uint8_t      active;            /* 1 = running, 0 = stopped */
} swtimer_t;

/* Initialize a software timer (does not start it) */
void swtimer_init(swtimer_t *t, swtimer_cb_t cb, void *arg);

/* Start or restart timer. ticks = delay in system ticks. */
void swtimer_start(swtimer_t *t, uint32_t ticks, uint8_t mode);

/* Stop a running timer */
void swtimer_stop(swtimer_t *t);

/* Register a timer with the system (called automatically by start) */
void swtimer_register(swtimer_t *t);

/* Unregister a timer from the system */
void swtimer_unregister(swtimer_t *t);

/* Tick all active timers — called from FRC1 ISR */
void swtimer_tick(void);

/* Number of active timers (for shell/diagnostics) */
int swtimer_active_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_TIMER_SW_H */
