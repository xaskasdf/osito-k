/* Official Mesa Venus ring transport over OsitoK GPU syscalls. */
#ifndef OSITOK_VENUS_WIRE_H
#define OSITOK_VENUS_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define VENUS_RING_MAGIC    0x53554E56u   /* 'VNUS' LE */
#define VENUS_RING_VERSION  1u

/* 16 KiB total ring, 8 KiB cmds + 8 KiB replies. Power of two. */
#define VENUS_RING_TOTAL_BYTES  (8u * 1024u * 1024u)
#define VENUS_RING_CMD_BYTES    (VENUS_RING_TOTAL_BYTES / 2u - sizeof(struct venus_ring_header))
#define VENUS_RING_REPLY_BYTES  (VENUS_RING_TOTAL_BYTES / 2u)

struct venus_ring_header {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity_bytes;   /* of each half */
    uint32_t head;             /* cmd area producer (guest writes) */
    uint32_t tail;             /* cmd area consumer (host writes) */
    uint32_t reply_head;       /* reply area producer (host writes) */
    uint32_t reply_tail;       /* reply area consumer (guest writes) */
    uint32_t flags;
} __attribute__((aligned(64)));

struct venus_cmd_header {
    uint32_t cmd_id;
    uint16_t flags;            /* bit0=REPLY_EXPECTED, bit1=FENCE */
    uint16_t _reserved;
    uint32_t payload_size;
    uint64_t reply_id;         /* guest-assigned; host echoes in reply */
} __attribute__((packed));

#define VENUS_CMD_FLAG_REPLY_EXPECTED (1u << 0)
#define VENUS_CMD_FLAG_FENCE          (1u << 1)

/* Public wire API (called by per-command encoders). */

/* Initialize the wire on behalf of the current venus_instance.
 * Allocates a HOST_COHERENT GPU resource (SYS_GPU_RES_CREATE), maps it
 * into the caller's address space (SYS_GPU_RES_MAP), zero-fills the
 * header, and returns the ring base pointer. ctx_id must be a live
 * kernel GPU context (from SYS_GPU_CTX_CREATE(GPU_CTX_VENUS)). */
struct venus_wire *venus_wire_open(int32_t ctx_id);
void venus_wire_close(struct venus_wire *w);

/* Reserve `payload_size` bytes in the command area, return a writable
 * pointer to the payload. Fills the cmd header in-place. Returns NULL
 * if the ring is full (caller must retry or drain). */
void *venus_wire_alloc_cmd(struct venus_wire *w,
                           uint32_t cmd_id, uint16_t flags,
                           uint32_t payload_size, uint64_t *out_reply_id);

/* Publish the pending command and issue SYS_GPU_SUBMIT to notify the
 * host. Blocks nothing — reply still needs to be awaited. */
int venus_wire_submit(struct venus_wire *w);

/* Submit a real venus-protocol command stream directly through SUBMIT_3D.
 * The reply variant points the renderer at this wire's reply stream, seeks it
 * to zero, submits `cmd`, then copies `reply_size` bytes from the stream.
 * This bootstraps the real protocol before the async ring path exists. */
int venus_wire_submit_raw(struct venus_wire *w, const void *cmd,
                          uint32_t cmd_size);
int venus_wire_submit_reply(struct venus_wire *w, const void *cmd,
                            uint32_t cmd_size, void *reply,
                            uint32_t reply_size);
uint64_t venus_wire_alloc_object_id(struct venus_wire *w);

/* Wait for the host's reply keyed by reply_id. Copies reply payload
 * into `out_buf` up to `buf_size` bytes. Returns bytes written, or
 * negative errno on error (ETIMEDOUT, EIO). */
int venus_wire_wait_reply(struct venus_wire *w, uint64_t reply_id,
                          void *out_buf, uint32_t buf_size);

#endif
