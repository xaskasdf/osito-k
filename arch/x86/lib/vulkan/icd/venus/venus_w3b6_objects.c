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

/* W4.8 — vkCmdClearColorImage entry point.
 *
 * Records the clear color + target image slot on the cmd buffer. Actual
 * SHM fill happens in QueueSubmit (so we don't burn time on cmd buffers
 * that are recorded but never submitted, and so we honor cmd buffer
 * ordering: the SHM is filled when the GPU "executes" the cmd, not when
 * the app records it).
 *
 * VkClearColorValue.float32 is RGBA 0..1; pack into BGRA8 byte order
 * (compositor expects little-endian u32 with bytes [B,G,R,A]). */
VKAPI_ATTR void VKAPI_CALL
venus_CmdClearColorImage(VkCommandBuffer cb, VkImage image,
                         VkImageLayout imageLayout,
                         const VkClearColorValue *pColor,
                         uint32_t rangeCount,
                         const VkImageSubresourceRange *pRanges) {
    (void)imageLayout; (void)rangeCount; (void)pRanges;
    if (!cb || !image || !pColor) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
    struct venus_device *dev = vcb->owner;
    if (!dev) return;
    int islot = (int)(((uint64_t)image >> 48) & VENUS_H_SLOT_MASK_W3B6);
    if (islot < 0 || islot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    if (!dev->images[islot].in_use) return;

    float r = pColor->float32[0];
    float g = pColor->float32[1];
    float b = pColor->float32[2];
    float a = pColor->float32[3];
    if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
    if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
    uint32_t br = (uint32_t)(b * 255.0f + 0.5f);
    uint32_t bg = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t bb = (uint32_t)(r * 255.0f + 0.5f);  /* red byte at byte[2] */
    uint32_t ba = (uint32_t)(a * 255.0f + 0.5f);
    vcb->recorded_clear_color      = br | (bg << 8) | (bb << 16) | (ba << 24);
    vcb->recorded_has_clear        = 1u;
    vcb->recorded_clear_image_slot = islot;
}
