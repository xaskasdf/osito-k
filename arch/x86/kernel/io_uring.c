/*
 * OsitoK x86-64 — io_uring (Async I/O)
 *
 * Linux-compatible io_uring for high-performance async I/O.
 * Submission Queue (SQ) + Completion Queue (CQ) in shared memory.
 * Supports: IORING_OP_READ, IORING_OP_WRITE, IORING_OP_NOP.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);

/* Dispatch through the main syscall handler for FD-aware I/O */
extern int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5,
                                uint64_t a6);
#define SYS_READ_NR   0
#define SYS_WRITE_NR  1
#define SYS_PREAD_NR  17
#define SYS_PWRITE_NR 18

/* ── SQE / CQE Structures (Linux ABI) ───────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t ioprio;
    int32_t  fd;
    uint64_t off;
    uint64_t addr;       /* Buffer address */
    uint32_t len;
    uint32_t rw_flags;
    uint64_t user_data;  /* Returned in CQE for correlation */
    uint8_t  pad[24];
} io_uring_sqe_t;

typedef struct __attribute__((packed)) {
    uint64_t user_data;
    int32_t  res;        /* Result (bytes or -errno) */
    uint32_t flags;
} io_uring_cqe_t;

#define IORING_OP_NOP    0
#define IORING_OP_READV  1
#define IORING_OP_WRITEV 2
#define IORING_OP_READ  22
#define IORING_OP_WRITE 23

/* ── io_uring Instance ───────────────────────────────────────── */

#define IOURING_MAX       4
#define IOURING_ENTRIES  64

/* In-flight SQE slot for deferred/retried operations */
#define IOURING_MAX_INFLIGHT 8

typedef struct {
    bool            active;
    io_uring_sqe_t  sqe;
    uint8_t         retries;
} io_uring_inflight_t;

typedef struct {
    bool             active;
    /* Submission queue */
    io_uring_sqe_t  *sq;
    uint32_t         sq_head;
    uint32_t         sq_tail;
    /* Completion queue */
    io_uring_cqe_t  *cq;
    uint32_t         cq_head;
    uint32_t         cq_tail;
    uint32_t         entries;
    /* In-flight: SQEs deferred due to -EAGAIN */
    io_uring_inflight_t inflight[IOURING_MAX_INFLIGHT];
} io_uring_t;

static io_uring_t rings[IOURING_MAX];

/* ── Setup ───────────────────────────────────────────────────── */

int io_uring_setup(uint32_t entries)
{
    if (entries == 0) entries = IOURING_ENTRIES;
    if (entries > IOURING_ENTRIES) entries = IOURING_ENTRIES;

    for (int i = 0; i < IOURING_MAX; i++) {
        if (!rings[i].active) {
            io_uring_t *r = &rings[i];
            r->sq = (io_uring_sqe_t *)mem_alloc_aligned(
                entries * sizeof(io_uring_sqe_t), 4096);
            r->cq = (io_uring_cqe_t *)mem_alloc_aligned(
                entries * sizeof(io_uring_cqe_t), 4096);
            if (!r->sq || !r->cq) return -1;
            memset(r->sq, 0, entries * sizeof(io_uring_sqe_t));
            memset(r->cq, 0, entries * sizeof(io_uring_cqe_t));
            r->sq_head = r->sq_tail = 0;
            r->cq_head = r->cq_tail = 0;
            r->entries = entries;
            r->active = true;
            serial_puts("[IOURING] Created ring ");
            serial_putdec((uint64_t)i);
            serial_puts(" (");
            serial_putdec(entries);
            serial_puts(" entries)\n");
            return i;
        }
    }
    return -1;
}

/* ── Submit ──────────────────────────────────────────────────── */

int io_uring_submit(int ring_idx, const io_uring_sqe_t *sqe)
{
    if (ring_idx < 0 || ring_idx >= IOURING_MAX) return -1;
    io_uring_t *r = &rings[ring_idx];
    if (!r->active) return -1;

    uint32_t next = (r->sq_tail + 1) % r->entries;
    if (next == r->sq_head) return -1;  /* Full */

    r->sq[r->sq_tail] = *sqe;
    r->sq_tail = next;
    return 0;
}

/* ── Execute one SQE, return result ─────────────────────────── */

static int32_t io_uring_exec_sqe(const io_uring_sqe_t *sqe)
{
    switch (sqe->opcode) {
    case IORING_OP_NOP:
        return 0;
    case IORING_OP_READ:
        if (sqe->off != 0 && sqe->off != (uint64_t)-1)
            return (int32_t)syscall_dispatch(SYS_PREAD_NR, (uint64_t)sqe->fd,
                                             sqe->addr, sqe->len, sqe->off, 0, 0);
        return (int32_t)syscall_dispatch(SYS_READ_NR, (uint64_t)sqe->fd,
                                         sqe->addr, sqe->len, 0, 0, 0);
    case IORING_OP_WRITE:
        if (sqe->off != 0 && sqe->off != (uint64_t)-1)
            return (int32_t)syscall_dispatch(SYS_PWRITE_NR, (uint64_t)sqe->fd,
                                             sqe->addr, sqe->len, sqe->off, 0, 0);
        return (int32_t)syscall_dispatch(SYS_WRITE_NR, (uint64_t)sqe->fd,
                                         sqe->addr, sqe->len, 0, 0, 0);
    default:
        return -22;  /* EINVAL */
    }
}

/* Post a CQE; returns 0 on success, -1 if CQ is full */
static int io_uring_post_cqe(io_uring_t *r, uint64_t user_data, int32_t res)
{
    uint32_t cq_next = (r->cq_tail + 1) % r->entries;
    if (cq_next == r->cq_head) return -1;  /* CQ full */
    r->cq[r->cq_tail].user_data = user_data;
    r->cq[r->cq_tail].res = res;
    r->cq[r->cq_tail].flags = 0;
    r->cq_tail = cq_next;
    return 0;
}

/* ── Process (execute pending SQEs) ──────────────────────────── */

#define EAGAIN 11
#define IOURING_BATCH_LIMIT 8  /* max SQEs per call to avoid long blocking */
#define IOURING_MAX_RETRIES 16

int io_uring_process(int ring_idx)
{
    if (ring_idx < 0 || ring_idx >= IOURING_MAX) return -1;
    io_uring_t *r = &rings[ring_idx];
    if (!r->active) return 0;

    int processed = 0;

    /* Phase 1: retry deferred in-flight SQEs */
    for (int i = 0; i < IOURING_MAX_INFLIGHT; i++) {
        io_uring_inflight_t *inf = &r->inflight[i];
        if (!inf->active) continue;

        int32_t result = io_uring_exec_sqe(&inf->sqe);
        if (result == -EAGAIN && inf->retries < IOURING_MAX_RETRIES) {
            inf->retries++;
            continue;  /* still blocked, retry next time */
        }
        io_uring_post_cqe(r, inf->sqe.user_data, result);
        inf->active = false;
        processed++;
    }

    /* Phase 2: process new SQEs from submission queue */
    int batch = 0;
    while (r->sq_head != r->sq_tail && batch < IOURING_BATCH_LIMIT) {
        io_uring_sqe_t *sqe = &r->sq[r->sq_head];
        int32_t result = io_uring_exec_sqe(sqe);

        if (result == -EAGAIN) {
            /* Defer: find a free inflight slot */
            int slot = -1;
            for (int i = 0; i < IOURING_MAX_INFLIGHT; i++) {
                if (!r->inflight[i].active) { slot = i; break; }
            }
            if (slot >= 0) {
                r->inflight[slot].sqe = *sqe;
                r->inflight[slot].retries = 0;
                r->inflight[slot].active = true;
            } else {
                /* No inflight slots — return EAGAIN to caller */
                io_uring_post_cqe(r, sqe->user_data, -EAGAIN);
            }
        } else {
            io_uring_post_cqe(r, sqe->user_data, result);
        }

        r->sq_head = (r->sq_head + 1) % r->entries;
        processed++;
        batch++;
    }

    return processed;
}

/* ── Reap completions ────────────────────────────────────────── */

int io_uring_peek_cqe(int ring_idx, io_uring_cqe_t *out)
{
    if (ring_idx < 0 || ring_idx >= IOURING_MAX) return -1;
    io_uring_t *r = &rings[ring_idx];
    if (!r->active || r->cq_head == r->cq_tail) return -1;

    *out = r->cq[r->cq_head];
    r->cq_head = (r->cq_head + 1) % r->entries;
    return 0;
}

void io_uring_destroy(int ring_idx)
{
    if (ring_idx >= 0 && ring_idx < IOURING_MAX)
        rings[ring_idx].active = false;
}
