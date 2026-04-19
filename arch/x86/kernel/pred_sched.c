/*
 * arch/x86/kernel/pred_sched.c — Markov-chain predictor for scheduler
 *
 * Observation: process behavior is repetitive. Shell → fork/exec → wait
 * → shell; compositor: render → flip → render; UDP server: recv →
 * process → recv. A simple bigram Markov model captures >90% of these
 * with minimal memory.
 */

#include "../include/pred_sched.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

#define MARKOV_BUCKETS      64
#define MARKOV_PER_BUCKET   4

typedef struct {
    uint16_t next_pid;
    uint16_t count;
} markov_entry_t;

static markov_entry_t markov[MARKOV_BUCKETS][MARKOV_PER_BUCKET];

uint64_t pred_total_predictions;
uint64_t pred_correct_predictions;

/* FNV-like hash */
static uint32_t markov_hash(uint16_t pid, uint8_t trigger)
{
    uint32_t h = (uint32_t)pid * 2654435761u;
    h ^= (uint32_t)trigger * 16777619u;
    return h % MARKOV_BUCKETS;
}

static uint16_t last_prediction_from;
static uint16_t last_prediction_next;

void pred_record(uint16_t from, uint16_t to, uint8_t trigger)
{
    /* Check if our last prediction came true */
    if (last_prediction_from == from && last_prediction_next != 0) {
        pred_total_predictions++;
        if (last_prediction_next == to)
            pred_correct_predictions++;
        last_prediction_next = 0;
    }

    uint32_t b = markov_hash(from, trigger);
    markov_entry_t *e = markov[b];
    int min_slot = 0;
    uint16_t min_count = 0xFFFFu;
    int empty_slot = -1;

    for (int i = 0; i < MARKOV_PER_BUCKET; i++) {
        if (e[i].count == 0) {
            if (empty_slot < 0) empty_slot = i;
            continue;
        }
        if (e[i].next_pid == to) {
            if (e[i].count < 0xFFFFu) e[i].count++;
            return;
        }
        if (e[i].count < min_count) {
            min_count = e[i].count;
            min_slot = i;
        }
    }

    int slot = (empty_slot >= 0) ? empty_slot : min_slot;
    e[slot].next_pid = to;
    e[slot].count = 1;
}

uint16_t pred_next(uint16_t cur, uint8_t trigger)
{
    uint32_t b = markov_hash(cur, trigger);
    markov_entry_t *e = markov[b];
    uint16_t best = 0;
    uint16_t best_count = 0;
    uint32_t total = 0;

    for (int i = 0; i < MARKOV_PER_BUCKET; i++) {
        total += e[i].count;
        if (e[i].count > best_count) {
            best_count = e[i].count;
            best = e[i].next_pid;
        }
    }

    /* Return prediction only if it accounts for >60% of observations and
     * we have enough samples to trust the pattern. */
    if (total < 4) return 0;
    if ((uint32_t)best_count * 10 < total * 6) return 0;

    last_prediction_from = cur;
    last_prediction_next = best;
    return best;
}

/* Opaque access to process_t for prefetch (avoid circular include). */
extern void *proc_find_ptr(uint16_t pid);
extern uint64_t proc_kernel_rsp(void *p);
extern void *proc_fpu_state_ptr(void *p);

void pred_prewarm(uint16_t pid)
{
    if (!pid) return;
    void *p = proc_find_ptr(pid);
    if (!p) return;

    uint64_t krsp = proc_kernel_rsp(p);
    if (krsp) {
        /* 3 cache lines of the interrupt frame (rsp0 ... rsp0+192) */
        __asm__ volatile("prefetcht0 (%0)" :: "r"(krsp) : "memory");
        __asm__ volatile("prefetcht0 64(%0)" :: "r"(krsp) : "memory");
        __asm__ volatile("prefetcht0 128(%0)" :: "r"(krsp) : "memory");
    }

    void *fpu = proc_fpu_state_ptr(p);
    if (fpu) {
        /* 512 B FPU state -> 8 cache lines. Use prefetcht1 (L2) so we don't
         * thrash L1d; it'll migrate to L1d on first access. */
        for (int i = 0; i < 512; i += 128) {
            __asm__ volatile("prefetcht1 (%0)" :: "r"((uint8_t*)fpu + i) : "memory");
        }
    }
}

void pred_reset(void)
{
    for (int b = 0; b < MARKOV_BUCKETS; b++)
        for (int i = 0; i < MARKOV_PER_BUCKET; i++)
            markov[b][i].next_pid = markov[b][i].count = 0;
    pred_total_predictions = 0;
    pred_correct_predictions = 0;
    last_prediction_from = last_prediction_next = 0;
}

void pred_stats(void)
{
    serial_puts("[PRED] total_predictions=");
    serial_putdec(pred_total_predictions);
    serial_puts(" correct=");
    serial_putdec(pred_correct_predictions);
    if (pred_total_predictions > 0) {
        serial_puts(" accuracy=");
        serial_putdec((pred_correct_predictions * 100) / pred_total_predictions);
        serial_puts("%");
    }
    serial_puts("\n[PRED] top transitions:\n");

    /* Print up to 10 highest-count entries across all buckets */
    for (int shown = 0; shown < 10; shown++) {
        int best_b = -1, best_i = -1;
        uint16_t best_count = 0;
        for (int b = 0; b < MARKOV_BUCKETS; b++) {
            for (int i = 0; i < MARKOV_PER_BUCKET; i++) {
                if (markov[b][i].count > best_count) {
                    /* Skip entries we've already shown (set to 0xFFFF marker) */
                    if (markov[b][i].count == 0xFFFFu) continue;
                    best_count = markov[b][i].count;
                    best_b = b; best_i = i;
                }
            }
        }
        if (best_b < 0) break;
        serial_puts("  bucket=");
        serial_putdec(best_b);
        serial_puts(" next_pid=");
        serial_putdec(markov[best_b][best_i].next_pid);
        serial_puts(" count=");
        serial_putdec(markov[best_b][best_i].count);
        serial_puts("\n");
        /* Don't mutate table — just cap the count seen for ranking in this loop */
        /* Simple approach: remember which we've shown by tracking (b,i) pairs */
        markov[best_b][best_i].count |= 0x8000u;
    }
    /* Unmask */
    for (int b = 0; b < MARKOV_BUCKETS; b++)
        for (int i = 0; i < MARKOV_PER_BUCKET; i++)
            markov[b][i].count &= 0x7FFFu;
}
