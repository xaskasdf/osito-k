/*
 * OsitoK x86-64 — Thread-Level Speculation (TLS)
 *
 * Automatically parallelizes detected loops across AP cores.
 * Current scope: read-only loops (no stores to shared memory).
 * The AP executes a chunk of the loop's iteration space using
 * the process's CR3 (shared address space, read-only access).
 *
 * For write loops, a shadow CR3 with COW pages would be needed
 * (future work — requires AP-safe #PF handling).
 *
 * Integration: spec_analyze.c detects loops at load time.
 * sched_tick can trigger TLS for hot loops (profiled at runtime).
 * Or userspace can hint via a syscall.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

extern int smp_submit(int ap_idx, void (*func)(void*, void*), void *arg, void *result);
extern void smp_wait(int ap_idx);
extern int ap_worker_count;

/* ── Loop chunk descriptor ──────────────────────────────────── */

typedef struct {
    /* Loop iteration bounds for this chunk */
    int64_t  iter_start;
    int64_t  iter_end;
    int64_t  iter_step;

    /* Function to execute (the loop body, extracted or wrapped) */
    void   (*body)(int64_t iter, void *ctx);
    void    *ctx;         /* User context passed to body */

    /* Result */
    volatile int done;
    int64_t      result;  /* Optional reduction value */
} tls_chunk_t;

/* ── Chunk worker (runs on AP) ──────────────────────────────── */

static void tls_chunk_worker(void *arg, void *result_buf)
{
    (void)result_buf;
    tls_chunk_t *chunk = (tls_chunk_t *)arg;

    /* Execute the loop body for our iteration range */
    for (int64_t i = chunk->iter_start; i < chunk->iter_end; i += chunk->iter_step) {
        if (chunk->body)
            chunk->body(i, chunk->ctx);
    }

    chunk->done = 1;
}

/* ── Public API: parallel_for ───────────────────────────────── */

/* Distribute a loop across APs + BSP.
 * `body(iter, ctx)` is called for each iteration.
 * Iterations must be independent (no cross-iteration deps).
 *
 * Returns 0 if parallelized, -1 if ran sequentially (fallback). */
int tls_parallel_for(int64_t start, int64_t end, int64_t step,
                     void (*body)(int64_t iter, void *ctx), void *ctx)
{
    if (!body || end <= start || step <= 0)
        return -1;

    int64_t total = (end - start + step - 1) / step;
    int workers = ap_worker_count;

    /* Not worth parallelizing small loops or if no APs */
    if (workers <= 0 || total < 64) {
        for (int64_t i = start; i < end; i += step)
            body(i, ctx);
        return -1;
    }

    if (workers > 3) workers = 3;
    int parts = workers + 1;  /* workers + BSP */
    int64_t chunk_iters = total / parts;
    if (chunk_iters < 1) chunk_iters = 1;

    /* Allocate chunk descriptors */
    tls_chunk_t chunks[3];  /* max 3 APs */

    /* Submit chunks to APs */
    for (int i = 0; i < workers; i++) {
        chunks[i].iter_start = start + (int64_t)i * chunk_iters * step;
        chunks[i].iter_end   = start + (int64_t)(i + 1) * chunk_iters * step;
        if (chunks[i].iter_end > end) chunks[i].iter_end = end;
        chunks[i].iter_step  = step;
        chunks[i].body       = body;
        chunks[i].ctx        = ctx;
        chunks[i].done       = 0;
        chunks[i].result     = 0;

        smp_submit(i, tls_chunk_worker, &chunks[i], NULL);
    }

    /* BSP does the last chunk */
    int64_t bsp_start = start + (int64_t)workers * chunk_iters * step;
    for (int64_t i = bsp_start; i < end; i += step)
        body(i, ctx);

    /* Wait for APs */
    for (int i = 0; i < workers; i++)
        smp_wait(i);

    return 0;
}

/* ── Parallel reduce (parallel_for + accumulation) ──────────── */

typedef struct {
    void   (*body)(int64_t iter, void *ctx, int64_t *accum);
    void    *ctx;
    int64_t  iter_start, iter_end, iter_step;
    int64_t  partial_result;
    volatile int done;
} tls_reduce_chunk_t;

static void tls_reduce_worker(void *arg, void *result_buf)
{
    (void)result_buf;
    tls_reduce_chunk_t *chunk = (tls_reduce_chunk_t *)arg;
    int64_t accum = 0;

    for (int64_t i = chunk->iter_start; i < chunk->iter_end; i += chunk->iter_step) {
        if (chunk->body)
            chunk->body(i, chunk->ctx, &accum);
    }

    chunk->partial_result = accum;
    chunk->done = 1;
}

int64_t tls_parallel_reduce(int64_t start, int64_t end, int64_t step,
                            void (*body)(int64_t iter, void *ctx, int64_t *accum),
                            void *ctx)
{
    if (!body || end <= start || step <= 0) return 0;

    int64_t total = (end - start + step - 1) / step;
    int workers = ap_worker_count;

    /* Fallback: sequential */
    if (workers <= 0 || total < 64) {
        int64_t accum = 0;
        for (int64_t i = start; i < end; i += step)
            body(i, ctx, &accum);
        return accum;
    }

    if (workers > 3) workers = 3;
    int64_t chunk_iters = total / (workers + 1);
    if (chunk_iters < 1) chunk_iters = 1;

    tls_reduce_chunk_t chunks[3];

    for (int i = 0; i < workers; i++) {
        chunks[i].iter_start = start + (int64_t)i * chunk_iters * step;
        chunks[i].iter_end   = start + (int64_t)(i + 1) * chunk_iters * step;
        if (chunks[i].iter_end > end) chunks[i].iter_end = end;
        chunks[i].iter_step  = step;
        chunks[i].body       = body;
        chunks[i].ctx        = ctx;
        chunks[i].partial_result = 0;
        chunks[i].done       = 0;

        smp_submit(i, tls_reduce_worker, &chunks[i], NULL);
    }

    /* BSP chunk */
    int64_t bsp_accum = 0;
    int64_t bsp_start = start + (int64_t)workers * chunk_iters * step;
    for (int64_t i = bsp_start; i < end; i += step)
        body(i, ctx, &bsp_accum);

    /* Collect partial results */
    int64_t total_result = bsp_accum;
    for (int i = 0; i < workers; i++) {
        smp_wait(i);
        total_result += chunks[i].partial_result;
    }

    return total_result;
}
