/*
 * venus_w3b6_objects.c — W3b.6 public entry points (vertex input + dynamic
 * state).
 *
 * Behavior: same dual-path pattern as W3b.4 / W3b.5. Each entry sends a
 * wire command (no-op when wire=NULL) and records guest-local
 * bookkeeping onto the cmd buffer so the CPU-fallback rasterizer in
 * vkQueuePresentKHR can paint a triangle when no real GPU work runs.
 *
 * Entry points implemented here:
 *   vkCmdBindVertexBuffers
 *   vkCmdSetViewport
 *   vkCmdSetScissor
 *
 * The CPU rasterizer lives in venus_cmd_present_software_raster.c.
 * The integration site lives in venus_cmd_QueuePresentKHR.c (the encoder).
 */
#include "venus.h"
#include "venus_wire.h"
#include "venus_proto_core.h"

extern int venus_cmd_encode_CmdBindVertexBuffers(
        struct venus_wire *, uint64_t, uint64_t,
        uint32_t, uint32_t, const uint64_t *, const uint64_t *);
extern int venus_cmd_encode_CmdSetViewport(
        struct venus_wire *, uint64_t, uint64_t,
        uint32_t, uint32_t, const VkViewport *);
extern int venus_cmd_encode_CmdSetScissor(
        struct venus_wire *, uint64_t, uint64_t,
        uint32_t, uint32_t, const VkRect2D *);

/* Slot decode (matches W3b.4 / W3b.5 invariants). */
#define VENUS_H_SLOT_MASK_W3B6   0x0FFFull

#define VENUS_W3B6_MAX_BINDINGS  8u

VKAPI_ATTR void VKAPI_CALL
venus_CmdBindVertexBuffers(VkCommandBuffer cb,
                           uint32_t firstBinding, uint32_t bindingCount,
                           const VkBuffer *pBuffers,
                           const VkDeviceSize *pOffsets) {
    if (!cb || !pBuffers) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
    struct venus_device *dev = vcb->owner;
    if (!dev) return;

    /* Resolve the LOWEST binding's buffer slot for the rasterizer. The
     * CPU fallback only consumes one stream of vec3 vertices today, so
     * we record the first binding only. */
    uint64_t buf_ids[VENUS_W3B6_MAX_BINDINGS];
    uint64_t offs[VENUS_W3B6_MAX_BINDINGS];
    uint32_t n = bindingCount;
    if (n > VENUS_W3B6_MAX_BINDINGS) n = VENUS_W3B6_MAX_BINDINGS;
    for (uint32_t i = 0; i < n; i++) {
        VkBuffer h = pBuffers[i];
        int bslot = (int)(((uint64_t)h >> 48) & VENUS_H_SLOT_MASK_W3B6);
        uint64_t host_id = 0;
        if (bslot >= 0 && bslot < (int)VENUS_MAX_BUF_OBJECTS &&
            dev->buffers[bslot].in_use)
            host_id = dev->buffers[bslot].host_id;
        buf_ids[i] = host_id;
        offs[i]    = pOffsets ? (uint64_t)pOffsets[i] : 0ull;
        if (firstBinding + i == 0) {
            /* Only record valid slots; -1 sentinel means "rasterizer skip". */
            if (bslot >= 0 && bslot < (int)VENUS_MAX_BUF_OBJECTS &&
                dev->buffers[bslot].in_use) {
                vcb->recorded_vb_slot   = bslot;
                vcb->recorded_vb_offset = offs[i];
                /* Default stride to vec3 (12 B). A future wave will plumb
                 * pipeline VI binding stride here. */
                vcb->recorded_vb_stride = 12u;
            }
        }
    }

    if (dev->parent && dev->parent->wire && vcb->host_id != 0)
        (void)venus_cmd_encode_CmdBindVertexBuffers(
                dev->parent->wire, dev->host_handle, vcb->host_id,
                firstBinding, n, buf_ids, offs);
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdSetViewport(VkCommandBuffer cb, uint32_t firstViewport,
                     uint32_t viewportCount, const VkViewport *pViewports) {
    if (!cb) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
    struct venus_device *dev = vcb->owner;
    if (!dev) return;
    if (dev->parent && dev->parent->wire && vcb->host_id != 0)
        (void)venus_cmd_encode_CmdSetViewport(
                dev->parent->wire, dev->host_handle, vcb->host_id,
                firstViewport, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdSetScissor(VkCommandBuffer cb, uint32_t firstScissor,
                    uint32_t scissorCount, const VkRect2D *pScissors) {
    if (!cb) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
    struct venus_device *dev = vcb->owner;
    if (!dev) return;
    if (dev->parent && dev->parent->wire && vcb->host_id != 0)
        (void)venus_cmd_encode_CmdSetScissor(
                dev->parent->wire, dev->host_handle, vcb->host_id,
                firstScissor, scissorCount, pScissors);
}
