#ifndef OSITOK_X86_TIMER_DEADLINE_H
#define OSITOK_X86_TIMER_DEADLINE_H

#include "../include/types.h"

extern uint64_t sched_get_next_deadline_us(void);
extern uint64_t audio_get_next_deadline_us(void);

static inline uint64_t idt_next_deadline_us(bool native_dos_active)
{
    /* Native DOS defers sched_tick(), including expired-sleeper removal.
     * Asking those sleepers for a deadline would rearm at 1 us forever.
     * Keep the periodic backend's 100 Hz device service cadence instead;
     * this also prevents tickless idle from starving the virtual PIT/input. */
    uint64_t next_us = native_dos_active ? 10000ULL
                                       : sched_get_next_deadline_us();
    uint64_t audio_us = audio_get_next_deadline_us();
    if (audio_us && (!next_us || audio_us < next_us))
        next_us = audio_us;
    return next_us;
}

#endif
