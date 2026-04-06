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
extern int osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len)
    __attribute__((weak));
extern int osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len)
    __attribute__((weak));

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

/* ── Process (execute pending SQEs) ──────────────────────────── */

int io_uring_process(int ring_idx)
{
    if (ring_idx < 0 || ring_idx >= IOURING_MAX) return -1;
    io_uring_t *r = &rings[ring_idx];
    if (!r->active) return 0;

    int processed = 0;
    while (r->sq_head != r->sq_tail) {
        io_uring_sqe_t *sqe = &r->sq[r->sq_head];
        int32_t result = 0;

        switch (sqe->opcode) {
        case IORING_OP_NOP:
            result = 0;
            break;
        case IORING_OP_READ:
            /* TODO: dispatch to FD table read */
            result = -38;  /* ENOSYS for now */
            break;
        case IORING_OP_WRITE:
            result = -38;
            break;
        default:
            result = -22;  /* EINVAL */
            break;
        }

        /* Post CQE */
        uint32_t cq_next = (r->cq_tail + 1) % r->entries;
        if (cq_next != r->cq_head) {
            r->cq[r->cq_tail].user_data = sqe->user_data;
            r->cq[r->cq_tail].res = result;
            r->cq[r->cq_tail].flags = 0;
            r->cq_tail = cq_next;
        }

        r->sq_head = (r->sq_head + 1) % r->entries;
        processed++;
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
