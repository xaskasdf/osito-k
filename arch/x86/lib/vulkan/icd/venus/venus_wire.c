#include "venus_wire.h"
#include "venus.h"   /* for VENUS_SYS_* and GPU_RES_* constants */

/* Minimum libc surface. */
extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);
extern long  __syscall2(long, long, long);
extern long  __syscall1(long, long);

/* From gpu_syscalls.h — duplicated here because that header is in the
 * kernel tree and not visible from user builds. */
#define VENUS_SYS_GPU_RES_CREATE   603L
#define VENUS_SYS_GPU_RES_MAP      604L
#define VENUS_SYS_GPU_SUBMIT       605L
#define VENUS_SYS_GPU_FENCE_WAIT   606L

#define GPU_RES_KIND_BUFFER         0u
#define GPU_RES_FLAG_HOST_COHERENT (1u << 0)

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
    /* bookkeeping for an in-flight but unsubmitted command */
    uint32_t                  pending_head_advance;
};

struct venus_wire *venus_wire_open(int32_t ctx_id) {
    if (ctx_id <= 0) return 0;
    struct venus_wire *w = malloc(sizeof(*w));
    if (!w) return 0;
    memset(w, 0, sizeof(*w));
    w->ctx_id = ctx_id;
    w->next_reply_id = 1;

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
    return w;
}

void venus_wire_close(struct venus_wire *w) {
    if (!w) return;
    /* Kernel-side resource cleanup happens at process teardown
     * (vg3d_cleanup_process); we don't call SYS_GPU_RES_DESTROY in
     * Wave 1's syscall set. Just release guest struct. */
    free(w);
}
