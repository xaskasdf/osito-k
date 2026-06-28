#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

extern int printf(const char *, ...);

#define VN_CMD_TYPE_vkCmdBeginRendering 213u
#define VENUS_RENDERING_MAX_COLOR_ATTACHMENTS 8u
#define VENUS_H_SLOT_MASK_RENDERING 0x0fffull

static int vcw_slot_from_handle(uint64_t h) {
    return (int)((h >> 48) & VENUS_H_SLOT_MASK_RENDERING);
}

static uint64_t vcw_image_view_host(struct venus_device *dev,
                                    VkImageView view) {
    if (!dev || !view) return 0;
    int slot = vcw_slot_from_handle((uint64_t)view);
    if (slot < 0 || slot >= (int)VENUS_MAX_IMAGE_VIEW_OBJECTS)
        return 0;
    if (!dev->image_views[slot].in_use)
        return 0;
    return dev->image_views[slot].host_id;
}

static void vcw_wr_clear_color(struct venus_cmd_writer *wr,
                               const VkClearValue *clear) {
    vcw_wr_u32(wr, 0); /* VkClearValue tag: color */
    vcw_wr_u32(wr, 2); /* VkClearColorValue tag: uint32[4] */
    vcw_wr_array_size(wr, 4);
    vcw_wr_u32(wr, clear->color.uint32[0]);
    vcw_wr_u32(wr, clear->color.uint32[1]);
    vcw_wr_u32(wr, clear->color.uint32[2]);
    vcw_wr_u32(wr, clear->color.uint32[3]);
}

static void vcw_wr_clear_depth(struct venus_cmd_writer *wr,
                               const VkClearValue *clear) {
    vcw_wr_u32(wr, 1); /* VkClearValue tag: depth/stencil */
    vcw_wr_bytes(wr, &clear->depthStencil.depth, 4);
    vcw_wr_u32(wr, clear->depthStencil.stencil);
}

static void vcw_wr_rendering_attachment(struct venus_cmd_writer *wr,
                                        struct venus_device *dev,
                                        const VkRenderingAttachmentInfo *att,
                                        int depth_stencil) {
    uint64_t view = att ? vcw_image_view_host(dev, att->imageView) : 0;
    uint64_t resolve_view = att ?
        vcw_image_view_host(dev, att->resolveImageView) : 0;
    const VkClearValue zero_clear = { { { 0, 0, 0, 0 } } };
    const VkClearValue *clear = att ? &att->clearValue : &zero_clear;

    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u64(wr, view);
    vcw_wr_i32(wr, att ? (int32_t)att->imageLayout : 0);
    vcw_wr_i32(wr, att ? (int32_t)att->resolveMode : 0);
    vcw_wr_u64(wr, resolve_view);
    vcw_wr_i32(wr, att ? (int32_t)att->resolveImageLayout : 0);
    vcw_wr_i32(wr, att ? (int32_t)att->loadOp : 0);
    vcw_wr_i32(wr, att ? (int32_t)att->storeOp : 0);
    if (depth_stencil)
        vcw_wr_clear_depth(wr, clear);
    else
        vcw_wr_clear_color(wr, clear);
}

int venus_cmd_encode_CmdBeginRendering(
        struct venus_wire *w, struct venus_device *dev, uint64_t cb_id,
        const VkRenderingInfo *info) {
    if (!w || !dev || !info) return -22;

    uint32_t color_count = info->colorAttachmentCount;
    if (!info->pColorAttachments)
        color_count = 0;
    if (color_count > VENUS_RENDERING_MAX_COLOR_ATTACHMENTS)
        color_count = VENUS_RENDERING_MAX_COLOR_ATTACHMENTS;

    static uint32_t begin_rendering_log_count;
    if (begin_rendering_log_count < 96u) {
        begin_rendering_log_count++;
        printf("[VCBR] #%u cb=%llu flags=0x%x area=%dx%d+%d,%d layers=%u viewMask=0x%x colors=%u pNext=%p depth=%p stencil=%p\n",
               begin_rendering_log_count, (unsigned long long)cb_id,
               (unsigned)info->flags,
               (int)info->renderArea.extent.width,
               (int)info->renderArea.extent.height,
               (int)info->renderArea.offset.x,
               (int)info->renderArea.offset.y,
               (unsigned)info->layerCount,
               (unsigned)info->viewMask,
               (unsigned)color_count,
               info->pNext,
               (const void *)info->pDepthAttachment,
               (const void *)info->pStencilAttachment);
        for (uint32_t i = 0; i < color_count; i++) {
            const VkRenderingAttachmentInfo *att = &info->pColorAttachments[i];
            printf("[VCBR]   color[%u] view=0x%llx host=%llu layout=%d resolve=0x%llx rlayout=%d load=%d store=%d rpNext=%p\n",
                   (unsigned)i,
                   (unsigned long long)(uint64_t)att->imageView,
                   (unsigned long long)vcw_image_view_host(dev, att->imageView),
                   (int)att->imageLayout,
                   (unsigned long long)(uint64_t)att->resolveImageView,
                   (int)att->resolveImageLayout,
                   (int)att->loadOp,
                   (int)att->storeOp,
                   att->pNext);
        }
    }

    uint8_t cmd[1024];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCmdBeginRendering);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, cb_id);
    vcw_wr_u64(&wr, 1); /* pRenderingInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_RENDERING_INFO);
    vcw_wr_u64(&wr, 0); /* pNext */
    vcw_wr_u32(&wr, info->flags);
    vcw_wr_i32(&wr, info->renderArea.offset.x);
    vcw_wr_i32(&wr, info->renderArea.offset.y);
    vcw_wr_u32(&wr, info->renderArea.extent.width);
    vcw_wr_u32(&wr, info->renderArea.extent.height);
    vcw_wr_u32(&wr, info->layerCount);
    vcw_wr_u32(&wr, info->viewMask);
    vcw_wr_u32(&wr, color_count);
    vcw_wr_array_size(&wr, color_count);
    for (uint32_t i = 0; i < color_count; i++) {
        vcw_wr_rendering_attachment(&wr, dev,
                                    &info->pColorAttachments[i], 0);
    }
    if (info->pDepthAttachment) {
        vcw_wr_u64(&wr, 1);
        vcw_wr_rendering_attachment(&wr, dev, info->pDepthAttachment, 1);
    } else {
        vcw_wr_u64(&wr, 0);
    }
    if (info->pStencilAttachment) {
        vcw_wr_u64(&wr, 1);
        vcw_wr_rendering_attachment(&wr, dev, info->pStencilAttachment, 1);
    } else {
        vcw_wr_u64(&wr, 0);
    }

    if (wr.err) return wr.err;
    return venus_wire_submit_raw(w, cmd, wr.off);
}
