/*
 * arch/x86/include/pred_sched.h — Markov-chain scheduler predictor
 *
 * Records transitions between processes in a 64-bucket, 4-way Markov
 * table hashed by (prev_pid, trigger). On each context switch, predicts
 * the next-next process and prefetches its kernel_rsp + FPU state into
 * L1d so the upcoming context switch sees warm cache lines.
 *
 * Compile-time cost: ~200 bytes/entry × 256 entries ≈ 50 KB.
 * Runtime cost: a few cache-line loads per switch; no MSR ops.
 */
#ifndef OSITOK_PRED_SCHED_H
#define OSITOK_PRED_SCHED_H

#include "types.h"
#include "stdint.h"

#define PRED_TRIGGER_QUANTUM 0
#define PRED_TRIGGER_YIELD   1
#define PRED_TRIGGER_BLOCK   2
#define PRED_TRIGGER_EXIT    3
#define PRED_TRIGGER_EXEC    4

/* Record a transition from `from_pid` to `to_pid` driven by `trigger`. */
void pred_record(uint16_t from_pid, uint16_t to_pid, uint8_t trigger);

/* Predict next PID after `cur_pid` experiences `trigger`.
 * Returns 0 if confidence is below threshold. */
uint16_t pred_next(uint16_t cur_pid, uint8_t trigger);

/* Prefetch kernel_rsp + fpu_state of `pid` so context switch is cache-warm. */
void pred_prewarm(uint16_t pid);

/* Shell: show top transitions and confidence. */
void pred_stats(void);

/* Reset the Markov table. */
void pred_reset(void);

/* Global counters for diagnostics. */
extern uint64_t pred_total_predictions;
extern uint64_t pred_correct_predictions;

#endif /* OSITOK_PRED_SCHED_H */
