/*
 * venus_wire_test_shim.c — test-only stand-in for venus_wire.c.
 *
 * Provides venus_wire_test_shim_open() and venus_wire_test_shim_ring_bytes()
 * that drive an in-process memory buffer instead of a real kernel GPU
 * resource. This file must NOT be added to libvulkan.a — it is linked
 * ONLY by the venus-wire-test test app, which needs the byte-level view
 * of the ring without standing up a real virtio-gpu context.
 *
 * STRUCT-LAYOUT COUPLING: the `struct venus_wire` definition below MUST
 * match byte-for-byte the private struct in venus_wire.c. If a field is
 * added to either file, mirror it here or the test will crash.
 */
#include "venus_wire.h"

extern void *malloc(unsigned long);
extern void *memset(void *, int, unsigned long);

/* Private to the test. A real venus_wire has ctx_id + res_id + mapped
 * ring; here we fabricate a local buffer. */
struct venus_wire {
    int32_t                   ctx_id;
    uint32_t                  res_id;
    struct venus_ring_header *hdr;
    uint8_t                  *cmd_area;
    uint8_t                  *reply_area;
    uint64_t                  next_reply_id;
    uint32_t                  pending_head_advance;
};

struct venus_wire *venus_wire_test_shim_open(void) {
    uint8_t *buf = malloc(VENUS_RING_TOTAL_BYTES);
    if (!buf) return 0;
    memset(buf, 0, VENUS_RING_TOTAL_BYTES);
    struct venus_wire *w = malloc(sizeof(*w));
    if (!w) return 0;
    memset(w, 0, sizeof(*w));
    w->ctx_id = 1;
    w->res_id = 1;
    w->hdr = (struct venus_ring_header *)buf;
    w->cmd_area   = buf + sizeof(*w->hdr);
    w->reply_area = w->cmd_area + VENUS_RING_CMD_BYTES;
    w->next_reply_id = 1;
    w->hdr->magic = VENUS_RING_MAGIC;
    w->hdr->version = VENUS_RING_VERSION;
    w->hdr->capacity_bytes = VENUS_RING_CMD_BYTES;
    return w;
}

const uint8_t *venus_wire_test_shim_ring_bytes(struct venus_wire *w,
                                               uint32_t *out_len) {
    if (out_len) *out_len = w->hdr->head;
    return w->cmd_area;
}
