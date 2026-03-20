/*
 * timer.c -- ARM Generic Timer with IRQ-based tick for scheduler
 */
#include "../include/hal.h"
#include "../include/aarch64.h"

static uint32_t timer_freq;
static uint32_t tick_interval;
static volatile uint64_t tick_count;

#define TIMER_PPI   30

uint64_t timer_get_ticks(void) { return read_cntpct_el0(); }
uint32_t timer_get_freq(void)  { return timer_freq; }
uint64_t timer_ms(void)        { return read_cntpct_el0() / (timer_freq / 1000); }
uint64_t timer_get_tick_count(void) { return tick_count; }

void udelay(uint32_t us) {
    uint64_t start = read_cntpct_el0();
    uint64_t target = (uint64_t)us * (timer_freq / 1000000);
    while ((read_cntpct_el0() - start) < target);
}

void mdelay(uint32_t ms) {
    uint64_t start = read_cntpct_el0();
    uint64_t target = (uint64_t)ms * (timer_freq / 1000);
    while ((read_cntpct_el0() - start) < target);
}

void timer_init(uint32_t hz) {
    timer_freq = read_cntfrq_el0();
    tick_interval = timer_freq / hz;
    tick_count = 0;

    serial_puts("[TMR ] Freq: ");
    serial_putdec(timer_freq);
    serial_puts(" Hz, interval: ");
    serial_putdec(tick_interval);
    serial_puts(" (");
    serial_putdec(hz);
    serial_puts(" Hz tick)\n");

    write_cntp_tval_el0(tick_interval);
    write_cntp_ctl_el0(1);  /* ENABLE=1, IMASK=0 */
}

void timer_tick_handler(void) {
    tick_count++;
    write_cntp_tval_el0(tick_interval);  /* Reload for next tick */
}
