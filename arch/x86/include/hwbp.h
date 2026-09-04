/*
 * arch/x86/include/hwbp.h — DR0-DR3 hardware breakpoint/watchpoint manager
 *
 * x86 has 4 debug address registers (DR0-DR3) plus DR6 (status) / DR7
 * (control). Each can break on execute, write, read+write, or I/O at
 * any address — without modifying code. Perfect for debugging data
 * races: `watch sched_switch_rsp write 8` prints a stack trace every
 * time that global is written, so you can see who corrupted it.
 */
#ifndef OSITOK_HWBP_H
#define OSITOK_HWBP_H

#include "types.h"
#include "stdint.h"
#include "interrupt.h"

typedef enum {
    HWBP_EXECUTE    = 0,   /* Break on instruction fetch at addr */
    HWBP_WRITE      = 1,   /* Break on data write to addr */
    HWBP_IO         = 2,   /* Break on I/O port access (needs DR7.DE) */
    HWBP_READWRITE  = 3,   /* Break on any data access to addr */
} hwbp_cond_t;

typedef enum {
    HWBP_LEN_1 = 0,
    HWBP_LEN_2 = 1,
    HWBP_LEN_8 = 2,
    HWBP_LEN_4 = 3,
} hwbp_len_t;

typedef struct {
    uint64_t    addr;
    hwbp_cond_t cond;
    hwbp_len_t  len;
    bool        active;
    uint64_t    hit_count;
    char        name[32];
} hwbp_t;

/* Slots 0-3 correspond to DR0-DR3. */
extern hwbp_t hwbps[4];

/* Configure slot. name is optional (nullable). Returns 0 on success. */
int  hwbp_set(int slot, uint64_t addr, hwbp_cond_t cond, hwbp_len_t len,
              const char *name);
int  hwbp_clear(int slot);
void hwbp_clear_all(void);

/* Called from #DB handler. Returns true if the exception was a HWBP
 * hit (and was handled). */
bool hwbp_dispatch(x86_interrupt_frame_t *frame);

/* Shell helpers. */
void hwbp_list(void);

#endif /* OSITOK_HWBP_H */
