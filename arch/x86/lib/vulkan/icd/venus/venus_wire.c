#include "venus_wire.h"

extern void *malloc(unsigned long);
extern void free(void *);
extern void *memset(void *, int, unsigned long);
extern long  __syscall2(long, long, long);
extern long  __syscall1(long, long);
extern int   printf(const char *, ...);

#define SYS_GPU_RES_CREATE 603L
#define SYS_GPU_RES_MAP 604L
#define SYS_GPU_SUBMIT 605L
#define SYS_GPU_RES_DESTROY 608L
#define SYS_NANOSLEEP 35L

#define GPU_RES_KIND_BUFFER 0u
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
    uint64_t blob_id;
};

struct gpu_submit_args {
    uint32_t ctx_id;
    uint32_t reserved;
    const void *cmd_bytes;
    uint64_t cmd_len;
    uint64_t *out_fence;
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

static void wire_unlock(struct venus_wire *w)
{
    __sync_lock_release(&w->lock);
}

static void put_u32(uint8_t **dst, uint32_t value)
{
    memcpy(*dst, &value, sizeof(value));
    *dst += sizeof(value);
}

static void put_u64(uint8_t **dst, uint64_t value)
{
    memcpy(*dst, &value, sizeof(value));
    *dst += sizeof(value);
}

static int submit_direct(struct venus_wire *w, const void *bytes, uint32_t size)
{
    uint64_t fence = 0;
    struct gpu_submit_args args = {
        .ctx_id = w->ctx_id,
        .cmd_bytes = bytes,
        .cmd_len = size,
        .out_fence = &fence,
    };
    return (int)__syscall1(SYS_GPU_SUBMIT, (long)&args);
}

static int create_resource(uint32_t ctx_id, uint64_t size,
                           uint32_t *resource_id, uint8_t **mapping)
{
    struct gpu_res_create_args args = {
        .kind = GPU_RES_KIND_BUFFER,
        .flags = GPU_RES_FLAG_HOST_COHERENT,
        .size = size,
    };
    long id = __syscall2(SYS_GPU_RES_CREATE, ctx_id, (long)&args);
    if (id <= 0)
        return -1;
    long va = __syscall1(SYS_GPU_RES_MAP, id);
    /* Osito ELF processes currently execute in ring 0 with the kernel's
     * shared upper-half PML4, so a valid mapped blob is a negative signed
     * long (0xFFFF...).  Only a null mapping is failure here. */
    if (va == 0)
        return -1;
    *resource_id = (uint32_t)id;
    *mapping = (uint8_t *)va;
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
    *resource_id = (uint32_t)id;
    mapped = (uint8_t *)va;
    int result = 0;
    *mapping = mapped;
    return result;
}

int venus_wire_resource_destroy(struct venus_wire *w, uint32_t resource_id)
{
    if (!w || !resource_id)
        return -1;
    return (int)__syscall1(SYS_GPU_RES_DESTROY, resource_id);
}

static void ring_copy(uint8_t *ring, uint32_t position,
                      const void *source, uint32_t size)
{
    const uint8_t *src = source;
    uint32_t offset = position % VN_RING_BUFFER_SIZE;
    uint32_t first = VN_RING_BUFFER_SIZE - offset;
    if (first > size)
        first = size;
    memcpy(ring + VN_RING_BUFFER_OFFSET + offset, src, first);
    if (first < size)
        memcpy(ring + VN_RING_BUFFER_OFFSET, src + first, size - first);
}

static int notify_ring(struct venus_wire *w)
{
    uint8_t command[24];
    uint8_t *p = command;
    put_u32(&p, VN_CMD_NOTIFY_RING);
    put_u32(&p, 0);
    put_u64(&p, w->ring_id);
    put_u32(&p, ++w->seqno);
    put_u32(&p, 0);
    return submit_direct(w, command, sizeof(command));
}

static int wait_consumed(struct venus_wire *w, uint32_t tail)
{
    volatile uint32_t *head = (volatile uint32_t *)(w->ring + VN_RING_HEAD_OFFSET);
    volatile uint32_t *status =
        (volatile uint32_t *)(w->ring + VN_RING_STATUS_OFFSET);
    for (uint32_t attempt = 0; attempt < 100000u; attempt++) {
        __asm__ volatile("lfence" ::: "memory");
        if (*head == tail)
            return 0;
        if (*status & 2u)
            return -1;
        __asm__ volatile("pause" ::: "memory");
    }

    struct {
        long seconds;
        long nanoseconds;
    } delay = {0, 1000000};
    for (uint32_t attempt = 0; attempt < 30000u; attempt++) {
        __asm__ volatile("lfence" ::: "memory");
        if (*head == tail)
            return 0;
        if (*status & 2u)
            return -1;
        (void)__syscall2(SYS_NANOSLEEP, (long)&delay, 0);
    }
    return -1;
}

struct venus_wire *venus_wire_open(int32_t ctx_id)
{
    if (ctx_id <= 0)
        return 0;
    struct venus_wire *w = malloc(sizeof(*w));
    if (!w)
        return 0;
    memset(w, 0, sizeof(*w));
    w->ctx_id = (uint32_t)ctx_id;
    w->ring_id = 0x4f5349544f000000ull | (uint32_t)ctx_id;

    if (create_resource(w->ctx_id, VN_RING_SIZE, &w->ring_resource_id,
                        &w->ring) < 0 ||
        create_resource(w->ctx_id, VN_REPLY_SIZE, &w->reply_resource_id,
                        &w->reply) < 0) {
        free(w);
        return 0;
    }
    memset(w->ring, 0, VN_RING_SIZE);
    memset(w->reply, 0, VN_REPLY_SIZE);

    uint8_t command[124];
    uint8_t *p = command;
    put_u32(&p, VN_CMD_CREATE_RING);
    put_u32(&p, 0);
    put_u64(&p, w->ring_id);
    put_u64(&p, 1);
    put_u32(&p, VN_STRUCTURE_TYPE_RING_CREATE_INFO);
    put_u64(&p, 0);
    put_u32(&p, 0);
    put_u32(&p, w->ring_resource_id);
    put_u64(&p, 0);
    put_u64(&p, VN_RING_SIZE);
    put_u64(&p, 1000000);
    put_u64(&p, VN_RING_HEAD_OFFSET);
    put_u64(&p, VN_RING_TAIL_OFFSET);
    put_u64(&p, VN_RING_STATUS_OFFSET);
    put_u64(&p, VN_RING_BUFFER_OFFSET);
    put_u64(&p, VN_RING_BUFFER_SIZE);
    put_u64(&p, VN_RING_EXTRA_OFFSET);
    put_u64(&p, VN_RING_EXTRA_SIZE);
    if ((uint32_t)(p - command) != sizeof(command) ||
        submit_direct(w, command, sizeof(command)) < 0) {
        free(w);
        return 0;
    }
    return w;
}

static int submit_ring(struct venus_wire *w, const void *prefix,
                       uint32_t prefix_size, const void *command,
                       uint32_t command_size, uint32_t *new_tail,
                       int wait)
{
    volatile uint32_t *head = (volatile uint32_t *)(w->ring + VN_RING_HEAD_OFFSET);
    volatile uint32_t *tail = (volatile uint32_t *)(w->ring + VN_RING_TAIL_OFFSET);
    uint32_t total = prefix_size + command_size;
    if (!command || !command_size || total > VN_RING_BUFFER_SIZE)
        return -1;

    uint32_t current_tail = *tail;
    for (uint32_t attempt = 0; attempt < 100000000u; attempt++) {
        __asm__ volatile("lfence" ::: "memory");
        if (current_tail - *head <= VN_RING_BUFFER_SIZE - total)
            break;
        if (attempt == 99999999u)
            return -1;
        __asm__ volatile("pause" ::: "memory");
    }

    if (prefix_size)
        ring_copy(w->ring, current_tail, prefix, prefix_size);
    ring_copy(w->ring, current_tail + prefix_size, command, command_size);
    *new_tail = current_tail + total;
    __asm__ volatile("mfence" ::: "memory");
    *tail = *new_tail;
    __asm__ volatile("mfence" ::: "memory");
    if (notify_ring(w) < 0)
        return -1;
    return wait ? wait_consumed(w, *new_tail) : 0;
}

int venus_wire_call(struct venus_wire *w, const void *command,
                    uint32_t command_size, void *reply,
                    uint32_t reply_size)
{
    if (!w || !reply || !reply_size || reply_size > VN_REPLY_SIZE)
        return -1;
    wire_lock(w);
    memset(w->reply, 0, reply_size);
    __asm__ volatile("mfence" ::: "memory");

    uint8_t set_reply[36];
    uint8_t *p = set_reply;
    put_u32(&p, VN_CMD_SET_REPLY_STREAM);
    put_u32(&p, 0);
    put_u64(&p, 1);
    put_u32(&p, w->reply_resource_id);
    put_u64(&p, 0);
    put_u64(&p, reply_size);

    uint32_t tail = 0;
    int rc = submit_ring(w, set_reply, sizeof(set_reply), command,
                         command_size, &tail, 1);
    if (rc == 0) {
        __asm__ volatile("mfence" ::: "memory");
        memcpy(reply, w->reply, reply_size);
        if (*(volatile uint32_t *)(w->ring + VN_RING_STATUS_OFFSET) & 2u)
            rc = -1;
    }
    if (rc < 0) {
        uint32_t command_type = 0;
        memcpy(&command_type, command, sizeof(command_type));
        printf("[VN wire] command=%u bytes=%u capacity=%u head=%u tail=%u "
               "status=0x%x\n", command_type, command_size,
               VN_RING_BUFFER_SIZE,
               *(volatile uint32_t *)(w->ring + VN_RING_HEAD_OFFSET),
               *(volatile uint32_t *)(w->ring + VN_RING_TAIL_OFFSET),
               *(volatile uint32_t *)(w->ring + VN_RING_STATUS_OFFSET));
    }
    wire_unlock(w);
    return rc;
}

int venus_wire_submit_async(struct venus_wire *w, const void *command,
                            uint32_t command_size)
{
    if (!w)
        return -1;
    wire_lock(w);
    uint32_t tail = 0;
    int rc = submit_ring(w, 0, 0, command, command_size, &tail, 0);
    wire_unlock(w);
    return rc;
}

void venus_wire_close(struct venus_wire *w)
{
    if (!w)
        return;
    uint8_t command[16];
    uint8_t *p = command;
    put_u32(&p, VN_CMD_DESTROY_RING);
    put_u32(&p, 0);
    put_u64(&p, w->ring_id);
    (void)submit_direct(w, command, sizeof(command));
    free(w);
}
