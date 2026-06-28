#include "venus_wire.h"
#include "venus.h"   /* for VENUS_SYS_* and GPU_RES_* constants */

/* Minimum libc surface. */
extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);
extern long  __syscall2(long, long, long);
extern long  __syscall1(long, long);
extern int   printf(const char *, ...);

/* From gpu_syscalls.h — duplicated here because that header is in the
 * kernel tree and not visible from user builds. */
#define VENUS_SYS_GPU_RES_CREATE   603L
#define VENUS_SYS_GPU_RES_MAP      604L
#define VENUS_SYS_GPU_SUBMIT       605L
#define VENUS_SYS_GPU_FENCE_WAIT   606L

#define GPU_RES_KIND_BUFFER         0u
#define GPU_RES_FLAG_HOST_COHERENT (1u << 0)

#define VN_CMD_TYPE_vkSetReplyCommandStreamMESA 178u
#define VN_CMD_TYPE_vkSeekReplyCommandStreamMESA 179u
#define VN_CMD_FLAG_GENERATE_REPLY 1u
#define VENUS_REPLY_STREAM_OFFSET \
    ((uint32_t)(sizeof(struct venus_ring_header) + VENUS_RING_CMD_BYTES))
#define VENUS_REPLY_PROBE_SPINS 10000000u
#define VENUS_REPLY_WAIT_SPINS 200000000u

struct gpu_res_create_args {
    uint32_t kind;
    uint32_t flags;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t size;
};

struct gpu_submit_args {
    uint32_t   ctx_id;
    uint32_t   reserved;
    const void *cmd_bytes;
    uint64_t   cmd_len;
    uint64_t  *out_fence;
};

struct venus_wire {
    int32_t                   ctx_id;
    uint32_t                  res_id;
    struct venus_ring_header *hdr;       /* maps the whole ring */
    uint8_t                  *cmd_area;
    uint8_t                  *reply_area;
    uint64_t                  next_reply_id;
    uint64_t                  next_object_id;
    uint32_t                  reply_stream_ready;
    volatile uint32_t         reply_lock;
    /* bookkeeping for an in-flight but unsubmitted command */
    uint32_t                  pending_head_advance;
};

static uint8_t *venus_put_u32(uint8_t *p, uint32_t v) {
    *(uint32_t *)p = v;
    return p + 4;
}

static uint8_t *venus_put_u64(uint8_t *p, uint64_t v) {
    *(uint64_t *)p = v;
    return p + 8;
}

static void venus_wire_lock_reply(struct venus_wire *w) {
    while (__sync_lock_test_and_set(&w->reply_lock, 1u)) {
        __asm__ volatile("pause" ::: "memory");
    }
}

static void venus_wire_unlock_reply(struct venus_wire *w) {
    __sync_lock_release(&w->reply_lock);
}

int venus_wire_submit_raw(struct venus_wire *w, const void *cmd,
                          uint32_t cmd_size) {
    if (!w || !cmd || cmd_size == 0) return -22;

    uint32_t cmd_type = *(const uint32_t *)cmd;
    uint64_t fence = 0;
    struct gpu_submit_args sa = {
        .ctx_id = (uint32_t)w->ctx_id,
        .cmd_bytes = cmd,
        .cmd_len = cmd_size,
        .out_fence = &fence,
    };
    long rc = __syscall1(VENUS_SYS_GPU_SUBMIT, (long)&sa);
    if (rc < 0) {
        printf("[VWIRE] submit_raw type=%u rc=%ld ctx=%d size=%u\n",
               cmd_type, rc, w->ctx_id, cmd_size);
        return (int)rc;
    }
    printf("[VWIRE] submit_raw type=%u rc=%ld fence=%llu ctx=%d size=%u\n",
           cmd_type, rc, (unsigned long long)fence, w->ctx_id, cmd_size);
    if (fence != 0) {
        long wrc = __syscall2(VENUS_SYS_GPU_FENCE_WAIT,
                              (long)fence, 1000000000L);
        printf("[VWIRE] fence_wait type=%u rc=%ld fence=%llu ctx=%d\n",
               cmd_type, wrc, (unsigned long long)fence, w->ctx_id);
        if (wrc < 0) {
            return (int)wrc;
        }
    }
    return 0;
}

static int venus_wire_seek_reply_stream(struct venus_wire *w, uint64_t pos) {
    uint8_t cmd[16];
    uint8_t *p = cmd;
    p = venus_put_u32(p, VN_CMD_TYPE_vkSeekReplyCommandStreamMESA);
    p = venus_put_u32(p, 0);
    p = venus_put_u64(p, pos);
    return venus_wire_submit_raw(w, cmd, (uint32_t)(p - cmd));
}

static int venus_wire_set_reply_stream(struct venus_wire *w) {
    uint8_t cmd[40];
    uint8_t *p = cmd;
    p = venus_put_u32(p, VN_CMD_TYPE_vkSetReplyCommandStreamMESA);
    p = venus_put_u32(p, 0);
    p = venus_put_u64(p, 1);                         /* pStream present */
    p = venus_put_u32(p, w->res_id);
    p = venus_put_u64(p, VENUS_REPLY_STREAM_OFFSET);
    p = venus_put_u64(p, VENUS_RING_REPLY_BYTES);
    int rc = venus_wire_submit_raw(w, cmd, (uint32_t)(p - cmd));
    if (rc == 0) {
        w->reply_stream_ready = 1;
        printf("[VWIRE] reply stream res=%u off=0x%x size=0x%x\n",
               w->res_id, VENUS_REPLY_STREAM_OFFSET, VENUS_RING_REPLY_BYTES);
    }
    return rc;
}

static int venus_wire_probe_reply_stream(struct venus_wire *w) {
    uint8_t cmd[16];
    uint8_t *p = cmd;
    uint8_t *reply = w->reply_area;
    for (uint32_t i = 0; i < 16; i++)
        reply[i] = 0xCD;

    p = venus_put_u32(p, VN_CMD_TYPE_vkSeekReplyCommandStreamMESA);
    p = venus_put_u32(p, VN_CMD_FLAG_GENERATE_REPLY);
    p = venus_put_u64(p, 0);

    int rc = venus_wire_submit_raw(w, cmd, (uint32_t)(p - cmd));
    if (rc < 0) {
        printf("[VWIRE] reply probe submit rc=%d\n", rc);
        return rc;
    }

    for (uint32_t attempt = 0; attempt < VENUS_REPLY_PROBE_SPINS; attempt++) {
        __asm__ volatile("mfence" ::: "memory");
        if (*(volatile uint32_t *)reply != 0xCDCDCDCDu)
            break;
        __asm__ volatile("pause" ::: "memory");
        if (attempt == VENUS_REPLY_PROBE_SPINS - 1u) {
            printf("[VWIRE] reply probe timeout raw=%02x %02x %02x %02x\n",
                   reply[0], reply[1], reply[2], reply[3]);
            return -110;
        }
    }

    printf("[VWIRE] reply probe raw=%02x %02x %02x %02x\n",
           reply[0], reply[1], reply[2], reply[3]);
    return (*(uint32_t *)reply == VN_CMD_TYPE_vkSeekReplyCommandStreamMESA) ? 0 : -5;
}

struct venus_wire *venus_wire_open(int32_t ctx_id) {
    if (ctx_id <= 0) return 0;
    struct venus_wire *w = malloc(sizeof(*w));
    if (!w) return 0;
    memset(w, 0, sizeof(*w));
    w->ctx_id = ctx_id;
    w->next_reply_id = 1;
    w->next_object_id = 1;

    struct gpu_res_create_args args = (struct gpu_res_create_args){
        .kind = GPU_RES_KIND_BUFFER,
        .flags = GPU_RES_FLAG_HOST_COHERENT,
        .size = VENUS_RING_TOTAL_BYTES,
    };
    long rid = __syscall2(VENUS_SYS_GPU_RES_CREATE,
                          (long)(uint32_t)ctx_id, (long)&args);
    if (rid <= 0) { free(w); return 0; }
    w->res_id = (uint32_t)rid;

    long va = __syscall1(VENUS_SYS_GPU_RES_MAP, (long)w->res_id);
    if (va == 0) { free(w); return 0; }

    w->hdr        = (struct venus_ring_header *)va;
    w->cmd_area   = (uint8_t *)(va + sizeof(struct venus_ring_header));
    w->reply_area = w->cmd_area + VENUS_RING_CMD_BYTES;

    /* Zero-fill + install the magic. Host sees this as the "ring ready"
     * signal when it later polls on any cmd. */
    memset(w->hdr, 0, sizeof(*w->hdr));
    w->hdr->magic          = VENUS_RING_MAGIC;
    w->hdr->version        = VENUS_RING_VERSION;
    w->hdr->capacity_bytes = VENUS_RING_CMD_BYTES;
    if (venus_wire_set_reply_stream(w) < 0) {
        free(w);
        return 0;
    }
    (void)venus_wire_probe_reply_stream(w);
    return w;
}

void venus_wire_close(struct venus_wire *w) {
    if (!w) return;
    /* Kernel-side resource cleanup happens at process teardown
     * (vg3d_cleanup_process); we don't call SYS_GPU_RES_DESTROY in
     * Wave 1's syscall set. Just release guest struct. */
    free(w);
}

void *venus_wire_alloc_cmd(struct venus_wire *w,
                           uint32_t cmd_id, uint16_t flags,
                           uint32_t payload_size, uint64_t *out_reply_id) {
    if (!w) return 0;
    /* Single-in-flight contract: if a prior alloc_cmd wasn't followed by
     * submit, refuse to start a new one. Prevents silently overwriting
     * the previous command's payload. */
    if (w->pending_head_advance != 0) return 0;
    /* Round payload to 8 bytes for wire alignment. */
    uint32_t aligned = (payload_size + 7) & ~7u;
    uint32_t total = sizeof(struct venus_cmd_header) + aligned;

    /* Simple bumped head — no wraparound in W3b.1. Ring must not fill. */
    uint32_t head = w->hdr->head;
    if (head + total > VENUS_RING_CMD_BYTES) return 0;

    struct venus_cmd_header *h = (struct venus_cmd_header *)(w->cmd_area + head);
    h->cmd_id = cmd_id;
    h->flags  = flags;
    h->_reserved = 0;
    h->payload_size = payload_size;
    h->reply_id = w->next_reply_id++;
    if (out_reply_id) *out_reply_id = h->reply_id;

    w->pending_head_advance = total;
    /* Caller writes payload directly after the header. */
    return (void *)(w->cmd_area + head + sizeof(*h));
}

int venus_wire_submit(struct venus_wire *w) {
    if (!w || w->pending_head_advance == 0) return -22 /* EINVAL */;

    /* Make the command visible to the host BEFORE bumping head. */
    __asm__ volatile("mfence" ::: "memory");
    w->hdr->head += w->pending_head_advance;
    __asm__ volatile("mfence" ::: "memory");
    w->pending_head_advance = 0;

    /* Send a single-byte "ping" via SYS_GPU_SUBMIT so the kernel pokes
     * the virtio-gpu virtqueue. Actual command bytes live in the ring;
     * this submission just tells the host to drain. */
    uint64_t fence = 0;
    uint8_t ping = 0;
    struct gpu_submit_args sa = {
        .ctx_id = (uint32_t)w->ctx_id,
        .cmd_bytes = &ping,
        .cmd_len = 1,
        .out_fence = &fence,
    };
    long rc = __syscall1(VENUS_SYS_GPU_SUBMIT, (long)&sa);
    if (rc < 0) return (int)rc;
    /* Fence wait is deferred — the *reply* is how we know the command
     * was consumed. We don't need to fence-wait here. */
    return 0;
}

int venus_wire_submit_reply(struct venus_wire *w, const void *cmd,
                            uint32_t cmd_size, void *reply,
                            uint32_t reply_size) {
    if (!w || !cmd || !reply || reply_size == 0) return -22;
    uint32_t cmd_type = *(const uint32_t *)cmd;
    int ret = -110;
    venus_wire_lock_reply(w);
    if (!w->reply_stream_ready) {
        int rc = venus_wire_set_reply_stream(w);
        if (rc < 0) {
            ret = rc;
            goto out_unlock;
        }
    }

    uint32_t clear = reply_size;
    if (clear > VENUS_RING_REPLY_BYTES) clear = VENUS_RING_REPLY_BYTES;
    for (uint32_t i = 0; i < clear; i++)
        w->reply_area[i] = 0xCD;

    int rc = venus_wire_seek_reply_stream(w, 0);
    if (rc < 0) {
        ret = rc;
        goto out_unlock;
    }
    rc = venus_wire_submit_raw(w, cmd, cmd_size);
    if (rc < 0) {
        ret = rc;
        goto out_unlock;
    }

    for (uint32_t attempt = 0; attempt < VENUS_REPLY_WAIT_SPINS; attempt++) {
        __asm__ volatile("mfence" ::: "memory");
        uint32_t first = *(volatile uint32_t *)w->reply_area;
        uint32_t second = reply_size > 4 ?
                          *(volatile uint32_t *)(w->reply_area + 4) : 0u;
        if (first != 0xCDCDCDCDu &&
            (reply_size <= 4 || second != 0xCDCDCDCDu))
            break;
        __asm__ volatile("pause" ::: "memory");
        if (attempt == VENUS_REPLY_WAIT_SPINS - 1u) {
            printf("[VWIRE] reply wait timeout ctx=%d type=%u cmd_size=%u reply=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                   w->ctx_id, cmd_type, cmd_size,
                   w->reply_area[0], w->reply_area[1],
                   w->reply_area[2], w->reply_area[3],
                   w->reply_area[4], w->reply_area[5],
                   w->reply_area[6], w->reply_area[7]);
            ret = -110;
            goto out_unlock;
        }
    }

    __asm__ volatile("mfence" ::: "memory");
    for (uint32_t i = 0; i < reply_size; i++)
        ((uint8_t *)reply)[i] = w->reply_area[i];
    ret = (int)reply_size;

out_unlock:
    venus_wire_unlock_reply(w);
    return ret;
}

uint64_t venus_wire_alloc_object_id(struct venus_wire *w) {
    if (!w) return 0;
    return w->next_object_id++;
}

int venus_wire_wait_reply(struct venus_wire *w, uint64_t reply_id,
                          void *out_buf, uint32_t buf_size) {
    if (!w) return -22;

    /* Poll reply_head for up to ~1 second of busy-spin. Real driver
     * should back off; W3b.1 keeps it simple. */
    for (int attempt = 0; attempt < 100000000; attempt++) {
        __asm__ volatile("lfence" ::: "memory");
        if (w->hdr->reply_head > w->hdr->reply_tail) {
            struct venus_cmd_header *rh =
                (struct venus_cmd_header *)(w->reply_area + w->hdr->reply_tail);
            if (rh->reply_id != reply_id) {
                /* Drop the mismatched reply to unstick the ring. Caller's
                 * contract is single-in-flight in W3b.1, so this path
                 * means something went wrong (host reply to a destroyed
                 * instance, host out-of-sequence). */
                uint32_t aligned = (rh->payload_size + 7) & ~7u;
                w->hdr->reply_tail += sizeof(*rh) + aligned;
                return -5 /* EIO */;
            }
            uint32_t copy = rh->payload_size;
            if (copy > buf_size) copy = buf_size;
            const uint8_t *src = (const uint8_t *)(rh + 1);
            for (uint32_t i = 0; i < copy; i++)
                ((uint8_t *)out_buf)[i] = src[i];
            uint32_t aligned = (rh->payload_size + 7) & ~7u;
            w->hdr->reply_tail += sizeof(*rh) + aligned;
            return (int)copy;
        }
    }
    return -110 /* ETIMEDOUT */;
}
