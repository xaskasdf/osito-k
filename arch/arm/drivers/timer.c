/*
 * timer.c -- ARM Generic Timer for SM8350
 *
 * Tested on ASUS ROG Phone 5, 2026-03-03/04.
 * Frequency: 19.2 MHz (confirmed via CNTFRQ_EL0 = 0x124F800).
 *
 * Uses the non-secure physical timer (EL1).
 * For scheduler: configure CNTP_TVAL_EL0 + enable CNTP_CTL_EL0,
 * handle PPI 30 (TIMER_NS_PHYS_PPI) via GICv3.
 */

#include "sm8350.h"
#include "aarch64.h"

uint64_t timer_get_ticks(void) {
    return read_cntpct_el0();
}

uint32_t timer_get_freq(void) {
    return read_cntfrq_el0();
}

/* Milliseconds since boot (wraps after ~30 years at 19.2 MHz) */
uint64_t timer_ms(void) {
    return read_cntpct_el0() / (TIMER_FREQ_HZ / 1000);
}

void udelay(uint32_t us) {
    uint64_t start = read_cntpct_el0();
    uint64_t target = (uint64_t)us * (TIMER_FREQ_HZ / 1000000);
    while ((read_cntpct_el0() - start) < target)
        ;
}

void mdelay(uint32_t ms) {
    uint64_t start = read_cntpct_el0();
    uint64_t target = (uint64_t)ms * (TIMER_FREQ_HZ / 1000);
    while ((read_cntpct_el0() - start) < target)
        ;
}

/*
 * Set timer to fire after `ticks` counts.
 * Used by scheduler for preemptive tick (e.g., 10ms = 192000 ticks).
 */
void timer_set_oneshot(uint32_t ticks) {
    write_cntp_tval_el0(ticks);
    write_cntp_ctl_el0(1);  /* ENABLE=1, IMASK=0 */
}

/* Disable timer interrupt */
void timer_disable(void) {
    write_cntp_ctl_el0(0);
}

/* Acknowledge timer (clear condition by setting next interval) */
void timer_ack(uint32_t next_ticks) {
    write_cntp_tval_el0(next_ticks);
}
