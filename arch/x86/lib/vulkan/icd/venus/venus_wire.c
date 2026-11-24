#include "venus_wire.h"

extern void *malloc(unsigned long);
extern void free(void *);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern long __syscall1(long, long);
extern long __syscall2(long, long, long);
extern int printf(const char *, ...);

#define SYS_GPU_RES_CREATE 603L
#define SYS_GPU_RES_MAP 604L
#define SYS_GPU_SUBMIT 605L
#define SYS_GPU_RES_DESTROY 608L
#define SYS_NANOSLEEP 35L

#define GPU_RES_KIND_BUFFER 0u
#define GPU_RES_FLAG_HOST_COHERENT (1u << 0)

#define VN_CMD_SET_REPLY_STREAM 178u
#define VN_CMD_CREATE_RING 188u
#define VN_CMD_DESTROY_RING 189u
#define VN_CMD_NOTIFY_RING 190u
#define VN_STRUCTURE_TYPE_RING_CREATE_INFO 1000384000u

#define VN_RING_HEAD_OFFSET 0u
#define VN_RING_TAIL_OFFSET 64u
#define VN_RING_STATUS_OFFSET 128u
#define VN_RING_BUFFER_OFFSET 192u
#define VN_RING_BUFFER_SIZE (16u * 1024u * 1024u)
#define VN_RING_EXTRA_OFFSET (VN_RING_BUFFER_OFFSET + VN_RING_BUFFER_SIZE)
#define VN_RING_EXTRA_SIZE 4u
#define VN_RING_SIZE (VN_RING_EXTRA_OFFSET + VN_RING_EXTRA_SIZE)
#define VN_REPLY_SIZE 65536u

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
    uint32_t ctx_id;
    uint32_t ring_resource_id;
    uint32_t reply_resource_id;
    uint32_t seqno;
    uint64_t ring_id;
    uint8_t *ring;
    uint8_t *reply;
    volatile uint32_t lock;
};

static void wire_lock(struct venus_wire *w)
{
    while (__sync_lock_test_and_set(&w->lock, 1))
        __asm__ volatile("pause" ::: "memory");
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

int venus_wire_resource_create(struct venus_wire *w, uint64_t size,
                               uint64_t blob_id,
                               uint32_t *resource_id, void **mapping)
{
    if (!w || !resource_id || !mapping)
        return -1;
    uint8_t *mapped = 0;
    struct gpu_res_create_args args = {
        .kind = GPU_RES_KIND_BUFFER,
        .flags = GPU_RES_FLAG_HOST_COHERENT,
        .size = size,
        .blob_id = blob_id,
    };
    long id = __syscall2(SYS_GPU_RES_CREATE, w->ctx_id, (long)&args);
    if (id <= 0)
        return -1;
    long va = __syscall1(SYS_GPU_RES_MAP, id);
    if (!va) {
        (void)__syscall1(SYS_GPU_RES_DESTROY, id);
        return -1;
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
    uint32_t command_type = 0;
    if (command && command_size >= sizeof(command_type))
        memcpy(&command_type, command, sizeof(command_type));
    wire_lock(w);
    uint32_t tail = 0;
    int rc = submit_ring(w, 0, 0, command, command_size, &tail, 1);
    if (rc < 0) {
        printf("[VN wire async] command=%u bytes=%u head=%u tail=%u "
               "status=0x%x\n", command_type, command_size,
               *(volatile uint32_t *)(w->ring + VN_RING_HEAD_OFFSET),
               *(volatile uint32_t *)(w->ring + VN_RING_TAIL_OFFSET),
               *(volatile uint32_t *)(w->ring + VN_RING_STATUS_OFFSET));
    }
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
