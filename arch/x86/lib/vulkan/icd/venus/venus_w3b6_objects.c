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
extern int printf(const char *, ...);
extern void *memcpy(void *, const void *, unsigned long);

/* Slot decode (matches W3b.4 / W3b.5 invariants). */
#define VENUS_H_SLOT_MASK_W3B6   0x0FFFull

#define VENUS_W3B6_MAX_BINDINGS  8u

static uint32_t venus_w3b6_logged_copy_like;
static uint32_t venus_w3b6_logged_copy_buffer;
static uint32_t venus_w3b6_logged_copy_b2i;
static uint32_t venus_w3b6_logged_clear_att;
static uint32_t venus_w3b6_logged_draw_indirect_count;

static uint32_t venus_w3b6_pack_clear_bgra(const VkClearValue *clear) {
    if (!clear) return 0;
    float r = clear->color.float32[0];
    float g = clear->color.float32[1];
    float b = clear->color.float32[2];
    float a = clear->color.float32[3];
    if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
    if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
    uint32_t bb = (uint32_t)(b * 255.0f + 0.5f);
    uint32_t bg = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t br = (uint32_t)(r * 255.0f + 0.5f);
    uint32_t ba = (uint32_t)(a * 255.0f + 0.5f);
    return bb | (bg << 8) | (br << 16) | (ba << 24);
}

static void venus_w3b6_copy_buffer_now(struct venus_device *dev,
                                       VkBuffer srcBuffer,
                                       VkBuffer dstBuffer,
                                       uint32_t regionCount,
                                       const VkBufferCopy *pRegions,
                                       const char *op_name) {
    if (!dev || !srcBuffer || !dstBuffer || !pRegions || regionCount == 0)
        return;
    int src_slot = (int)(((uint64_t)srcBuffer >> 48) & VENUS_H_SLOT_MASK_W3B6);
    int dst_slot = (int)(((uint64_t)dstBuffer >> 48) & VENUS_H_SLOT_MASK_W3B6);
    if (src_slot < 0 || src_slot >= (int)VENUS_MAX_BUF_OBJECTS) return;
    if (dst_slot < 0 || dst_slot >= (int)VENUS_MAX_BUF_OBJECTS) return;
    struct venus_buffer *src = &dev->buffers[src_slot];
    struct venus_buffer *dst = &dev->buffers[dst_slot];
    if (!src->in_use || !dst->in_use) return;
    if (src->bound_mem_slot < 0 || src->bound_mem_slot >= (int)VENUS_MAX_MEM_OBJECTS)
        return;
    if (dst->bound_mem_slot < 0 || dst->bound_mem_slot >= (int)VENUS_MAX_MEM_OBJECTS)
        return;
    struct venus_memory *src_m = &dev->memories[src->bound_mem_slot];
    struct venus_memory *dst_m = &dev->memories[dst->bound_mem_slot];
    if (!src_m->in_use || !dst_m->in_use) return;
    if (!src_m->local_ptr || !dst_m->local_ptr) return;

    if (!venus_w3b6_logged_copy_buffer) {
        venus_w3b6_logged_copy_buffer = 1u;
        printf("[VCOPY] record %s srcbuf=%d dstbuf=%d\n",
               op_name ? op_name : "CopyBuffer", src_slot, dst_slot);
    }

    for (uint32_t i = 0; i < regionCount; i++) {
        uint64_t src_off = src->bound_offset + (uint64_t)pRegions[i].srcOffset;
        uint64_t dst_off = dst->bound_offset + (uint64_t)pRegions[i].dstOffset;
        uint64_t size = (uint64_t)pRegions[i].size;
        if (src_off >= src_m->size || dst_off >= dst_m->size) continue;
        if (size > src_m->size - src_off) size = src_m->size - src_off;
        if (size > dst_m->size - dst_off) size = dst_m->size - dst_off;
        if (size == 0) continue;
        memcpy((uint8_t *)dst_m->local_ptr + dst_off,
               (const uint8_t *)src_m->local_ptr + src_off,
               (unsigned long)size);
    }
}

static void venus_w3b6_record_copy_buffer_to_image(
        VkCommandBuffer cb, VkBuffer srcBuffer, VkImage dstImage,
        const VkBufferImageCopy *region, const char *op_name) {
    if (!cb || !srcBuffer || !dstImage) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
    struct venus_device *dev = vcb->owner;
    if (!dev) return;

    int src_slot = (int)(((uint64_t)srcBuffer >> 48) & VENUS_H_SLOT_MASK_W3B6);
    int dst_slot = (int)(((uint64_t)dstImage >> 48) & VENUS_H_SLOT_MASK_W3B6);
    if (src_slot < 0 || src_slot >= (int)VENUS_MAX_BUF_OBJECTS) return;
    if (dst_slot < 0 || dst_slot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    if (!dev->buffers[src_slot].in_use || !dev->images[dst_slot].in_use) return;

    vcb->recorded_has_copy_buffer_to_image = 1u;
    vcb->recorded_copy_src_buffer_slot = src_slot;
    vcb->recorded_copy_buffer_dst_image_slot = dst_slot;
    vcb->recorded_copy_buffer_offset = region ? (uint64_t)region->bufferOffset : 0u;
    vcb->recorded_copy_buffer_width =
        region ? region->imageExtent.width : dev->images[dst_slot].width;
    vcb->recorded_copy_buffer_height =
        region ? region->imageExtent.height : dev->images[dst_slot].height;
    vcb->recorded_copy_buffer_row_length = region ? region->bufferRowLength : 0u;

    if (!venus_w3b6_logged_copy_b2i) {
        venus_w3b6_logged_copy_b2i = 1u;
        printf("[VCOPY] record %s srcbuf=%d dstimg=%d %ux%u\n",
               op_name ? op_name : "CopyBufferToImage", src_slot, dst_slot,
               vcb->recorded_copy_buffer_width,
               vcb->recorded_copy_buffer_height);
    }
}

static void venus_w3b6_record_copy_like(VkCommandBuffer cb, VkImage srcImage,
                                        VkImage dstImage,
                                        const char *op_name) {
    if (!cb || !srcImage || !dstImage) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
    struct venus_device *dev = vcb->owner;
    if (!dev) return;

    int src_slot = (int)(((uint64_t)srcImage >> 48) & VENUS_H_SLOT_MASK_W3B6);
    int dst_slot = (int)(((uint64_t)dstImage >> 48) & VENUS_H_SLOT_MASK_W3B6);
    if (src_slot < 0 || src_slot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    if (dst_slot < 0 || dst_slot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    if (!dev->images[src_slot].in_use || !dev->images[dst_slot].in_use) return;

    vcb->recorded_has_copy_image = 1u;
    vcb->recorded_copy_src_image_slot = src_slot;
    vcb->recorded_copy_dst_image_slot = dst_slot;

    if (!venus_w3b6_logged_copy_like) {
        venus_w3b6_logged_copy_like = 1u;
        printf("[VCOPY] record %s src=%d dst=%d\n",
               op_name ? op_name : "copy-like", src_slot, dst_slot);
    }
}

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
venus_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t firstBinding,
                            uint32_t bindingCount, const VkBuffer *pBuffers,
                            const VkDeviceSize *pOffsets,
                            const VkDeviceSize *pSizes,
                            const VkDeviceSize *pStrides) {
    (void)pSizes;
    venus_CmdBindVertexBuffers(cb, firstBinding, bindingCount,
                               pBuffers, pOffsets);
    if (!cb || !pStrides || firstBinding > 0 || bindingCount == 0) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
    if (pStrides[0] > 0 && pStrides[0] <= 4096u)
        vcb->recorded_vb_stride = (uint32_t)pStrides[0];
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
venus_CmdSetViewportWithCount(VkCommandBuffer cb, uint32_t viewportCount,
                              const VkViewport *pViewports) {
    venus_CmdSetViewport(cb, 0, viewportCount, pViewports);
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

VKAPI_ATTR void VKAPI_CALL
venus_CmdSetScissorWithCount(VkCommandBuffer cb, uint32_t scissorCount,
                             const VkRect2D *pScissors) {
    venus_CmdSetScissor(cb, 0, scissorCount, pScissors);
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

VKAPI_ATTR void VKAPI_CALL
venus_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage image,
                                VkImageLayout imageLayout,
                                const VkClearDepthStencilValue *pDepthStencil,
                                uint32_t rangeCount,
                                const VkImageSubresourceRange *pRanges) {
    (void)cb; (void)image; (void)imageLayout;
    (void)pDepthStencil; (void)rangeCount; (void)pRanges;
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdClearAttachments(VkCommandBuffer cb, uint32_t attachmentCount,
                          const VkClearAttachment *pAttachments,
                          uint32_t rectCount, const VkClearRect *pRects) {
    (void)rectCount; (void)pRects;
    if (!cb || !pAttachments || attachmentCount == 0) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
    if (vcb->last_drawn_image_slot < 0) return;
    for (uint32_t i = 0; i < attachmentCount; i++) {
        if (!(pAttachments[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT))
            continue;
        vcb->recorded_clear_color =
            venus_w3b6_pack_clear_bgra(&pAttachments[i].clearValue);
        vcb->recorded_has_clear = 1u;
        vcb->recorded_clear_image_slot = vcb->last_drawn_image_slot;
        if (!venus_w3b6_logged_clear_att) {
            venus_w3b6_logged_clear_att = 1u;
            printf("[VCLEAR] ClearAttachments image=%d color=0x%x\n",
                   vcb->recorded_clear_image_slot,
                   vcb->recorded_clear_color);
        }
        return;
    }
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer srcBuffer,
                    VkBuffer dstBuffer, uint32_t regionCount,
                    const VkBufferCopy *pRegions) {
    if (!cb) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
    venus_w3b6_copy_buffer_now(vcb->owner, srcBuffer, dstBuffer,
                               regionCount, pRegions, "CopyBuffer");
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyBuffer2(VkCommandBuffer cb, const VkCopyBufferInfo2 *pInfo) {
    if (!cb || !pInfo || !pInfo->pRegions || pInfo->regionCount == 0)
        return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
#define VENUS_W3B6_COPY_BUFFER_MAX 16u
    VkBufferCopy locals[VENUS_W3B6_COPY_BUFFER_MAX];
    uint32_t n = pInfo->regionCount;
    if (n > VENUS_W3B6_COPY_BUFFER_MAX) n = VENUS_W3B6_COPY_BUFFER_MAX;
    for (uint32_t i = 0; i < n; i++) {
        locals[i].srcOffset = pInfo->pRegions[i].srcOffset;
        locals[i].dstOffset = pInfo->pRegions[i].dstOffset;
        locals[i].size = pInfo->pRegions[i].size;
    }
    venus_w3b6_copy_buffer_now(vcb->owner, pInfo->srcBuffer,
                               pInfo->dstBuffer, n, locals, "CopyBuffer2");
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer srcBuffer,
                           VkImage dstImage, VkImageLayout dstImageLayout,
                           uint32_t regionCount,
                           const VkBufferImageCopy *pRegions) {
    (void)dstImageLayout;
    if (regionCount == 0) return;
    venus_w3b6_record_copy_buffer_to_image(
            cb, srcBuffer, dstImage, pRegions, "CopyBufferToImage");
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyBufferToImage2(VkCommandBuffer cb,
                            const VkCopyBufferToImageInfo2 *pInfo) {
    if (!pInfo || pInfo->regionCount == 0 || !pInfo->pRegions) return;
    VkBufferImageCopy region;
    region.bufferOffset = pInfo->pRegions[0].bufferOffset;
    region.bufferRowLength = pInfo->pRegions[0].bufferRowLength;
    region.bufferImageHeight = pInfo->pRegions[0].bufferImageHeight;
    region.imageSubresource = pInfo->pRegions[0].imageSubresource;
    region.imageOffset = pInfo->pRegions[0].imageOffset;
    region.imageExtent = pInfo->pRegions[0].imageExtent;
    venus_w3b6_record_copy_buffer_to_image(
            cb, pInfo->srcBuffer, pInfo->dstImage, &region,
            "CopyBufferToImage2");
}

/* Guest-local vkCmdCopyImage.
 *
 * DXVK often renders/clears an intermediate image and then copies it into the
 * swapchain image before present. Until the Venus wire path executes real GPU
 * work, record the last image-to-image copy so QueueSubmit can mirror CPU
 * backing storage into the SHM-backed swapchain image. */
VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyImage(VkCommandBuffer cb, VkImage srcImage,
                   VkImageLayout srcImageLayout, VkImage dstImage,
                   VkImageLayout dstImageLayout, uint32_t regionCount,
                   const VkImageCopy *pRegions) {
    (void)srcImageLayout;
    (void)dstImageLayout;
    (void)pRegions;
    if (regionCount == 0) return;
    venus_w3b6_record_copy_like(cb, srcImage, dstImage, "CopyImage");
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdBlitImage(VkCommandBuffer cb, VkImage srcImage,
                   VkImageLayout srcImageLayout, VkImage dstImage,
                   VkImageLayout dstImageLayout, uint32_t regionCount,
                   const VkImageBlit *pRegions, VkFilter filter) {
    (void)srcImageLayout;
    (void)dstImageLayout;
    (void)pRegions;
    (void)filter;
    if (regionCount == 0) return;
    venus_w3b6_record_copy_like(cb, srcImage, dstImage, "BlitImage");
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdResolveImage(VkCommandBuffer cb, VkImage srcImage,
                      VkImageLayout srcImageLayout, VkImage dstImage,
                      VkImageLayout dstImageLayout, uint32_t regionCount,
                      const VkImageResolve *pRegions) {
    (void)srcImageLayout;
    (void)dstImageLayout;
    (void)pRegions;
    if (regionCount == 0) return;
    venus_w3b6_record_copy_like(cb, srcImage, dstImage, "ResolveImage");
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyImage2(VkCommandBuffer cb, const VkCopyImageInfo2 *pInfo) {
    if (!pInfo || pInfo->regionCount == 0) return;
    venus_w3b6_record_copy_like(cb, pInfo->srcImage, pInfo->dstImage,
                                "CopyImage2");
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdBlitImage2(VkCommandBuffer cb, const VkBlitImageInfo2 *pInfo) {
    if (!pInfo || pInfo->regionCount == 0) return;
    venus_w3b6_record_copy_like(cb, pInfo->srcImage, pInfo->dstImage,
                                "BlitImage2");
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdResolveImage2(VkCommandBuffer cb,
                       const VkResolveImageInfo2 *pInfo) {
    if (!pInfo || pInfo->regionCount == 0) return;
    venus_w3b6_record_copy_like(cb, pInfo->srcImage, pInfo->dstImage,
                                "ResolveImage2");
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage srcImage,
                           VkImageLayout srcImageLayout, VkBuffer dstBuffer,
                           uint32_t regionCount,
                           const VkBufferImageCopy *pRegions) {
    (void)cb; (void)srcImage; (void)srcImageLayout;
    (void)dstBuffer; (void)regionCount; (void)pRegions;
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyImageToBuffer2(VkCommandBuffer cb,
                            const VkCopyImageToBufferInfo2 *pInfo) {
    (void)cb; (void)pInfo;
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdPipelineBarrier(VkCommandBuffer cb,
                         VkPipelineStageFlags srcStageMask,
                         VkPipelineStageFlags dstStageMask,
                         VkDependencyFlags dependencyFlags,
                         uint32_t memoryBarrierCount,
                         const VkMemoryBarrier *pMemoryBarriers,
                         uint32_t bufferMemoryBarrierCount,
                         const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                         uint32_t imageMemoryBarrierCount,
                         const VkImageMemoryBarrier *pImageMemoryBarriers) {
    (void)cb; (void)srcStageMask; (void)dstStageMask; (void)dependencyFlags;
    (void)memoryBarrierCount; (void)pMemoryBarriers;
    (void)bufferMemoryBarrierCount; (void)pBufferMemoryBarriers;
    (void)imageMemoryBarrierCount; (void)pImageMemoryBarriers;
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdPipelineBarrier2(VkCommandBuffer cb,
                          const VkDependencyInfo *pDependencyInfo) {
    (void)cb; (void)pDependencyInfo;
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout,
                       VkShaderStageFlags stageFlags, uint32_t offset,
                       uint32_t size, const void *pValues) {
    (void)cb; (void)layout; (void)stageFlags;
    (void)offset; (void)size; (void)pValues;
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer buffer,
                           VkDeviceSize offset, VkBuffer countBuffer,
                           VkDeviceSize countBufferOffset,
                           uint32_t maxDrawCount, uint32_t stride) {
    (void)buffer; (void)offset; (void)countBuffer;
    (void)countBufferOffset; (void)stride;
    if (!cb) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
    vcb->recorded_vertex_count = maxDrawCount ? 3u : 0u;
    vcb->recorded_first_vertex = 0u;
    vcb->drew_flag = maxDrawCount ? 1u : 0u;
    if (!venus_w3b6_logged_draw_indirect_count) {
        venus_w3b6_logged_draw_indirect_count = 1u;
        printf("[VDRAW] DrawIndirectCount max=%u image=%d\n",
               maxDrawCount, vcb->last_drawn_image_slot);
    }
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer buffer,
                                  VkDeviceSize offset, VkBuffer countBuffer,
                                  VkDeviceSize countBufferOffset,
                                  uint32_t maxDrawCount, uint32_t stride) {
    venus_CmdDrawIndirectCount(cb, buffer, offset, countBuffer,
                               countBufferOffset, maxDrawCount, stride);
}
