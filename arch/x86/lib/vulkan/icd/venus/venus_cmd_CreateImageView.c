/*
 * Encoder for vkCreateImageView over venus.
 *
 * Request payload (flattened VkImageViewCreateInfo):
 *   dev_id                (u64)
 *   flags                 (u32)
 *   pad                   (u32)
 *   image_id              (u64)
 *   viewType              (u32)
 *   format                (u32)
 *   components r,g,b,a    (4 * u32)
 *   subresourceRange      (aspectMask, baseMipLevel, levelCount,
 *                          baseArrayLayer, layerCount) = 5 * u32
 *   pad2                  (u32)
 *   pAllocator_present    (u32)  always 0
 *   pImageView_present    (u32)  always 1
 *
 * Reply:
 *   VkResult              (u32)
 *   pad                   (u32)
 *   host VkImageView id   (u64)
 */
#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkCreateImageView 57u
#define VN_CMD_GENERATE_REPLY 1u

extern int printf(const char *, ...);

int venus_cmd_encode_CreateImageView(
        struct venus_wire *w, uint64_t dev_id, uint64_t image_id,
        const VkImageViewCreateInfo *pCreateInfo,
        uint64_t *out_view_id) {
    if (!w || !pCreateInfo || !out_view_id) return -22;

    extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);

    uint8_t cmd[256];
    uint8_t reply[24];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };
    uint64_t view_id = venus_wire_alloc_object_id(w);
    if (!view_id) return -12;

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCreateImageView);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);

    vcw_wr_u64(&wr, 1);                                      /* pCreateInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    vcw_wr_u64(&wr, 0);                                      /* pNext */
    vcw_wr_u32(&wr, pCreateInfo->flags);
    vcw_wr_u64(&wr, image_id);
    vcw_wr_i32(&wr, (int32_t)pCreateInfo->viewType);
    vcw_wr_i32(&wr, (int32_t)pCreateInfo->format);
    vcw_wr_i32(&wr, (int32_t)pCreateInfo->components.r);
    vcw_wr_i32(&wr, (int32_t)pCreateInfo->components.g);
    vcw_wr_i32(&wr, (int32_t)pCreateInfo->components.b);
    vcw_wr_i32(&wr, (int32_t)pCreateInfo->components.a);
    vcw_wr_u32(&wr, pCreateInfo->subresourceRange.aspectMask);
    vcw_wr_u32(&wr, pCreateInfo->subresourceRange.baseMipLevel);
    vcw_wr_u32(&wr, pCreateInfo->subresourceRange.levelCount);
    vcw_wr_u32(&wr, pCreateInfo->subresourceRange.baseArrayLayer);
    vcw_wr_u32(&wr, pCreateInfo->subresourceRange.layerCount);
    vcw_wr_u64(&wr, 0);                                      /* pAllocator */
    vcw_wr_u64(&wr, 1);                                      /* pView */
    vcw_wr_u64(&wr, view_id);
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t present   = *(uint64_t *)(reply + 8);
    uint64_t view_reply = *(uint64_t *)(reply + 16);
    static uint32_t log_count;
    if (log_count < 64u) {
        log_count++;
        printf("[VIV] reply cmd=%u vk=%u present=%llu view=%llu guest=%llu image=%llu bytes=%u\n",
               reply_cmd, vk_result, (unsigned long long)present,
               (unsigned long long)view_reply,
               (unsigned long long)view_id,
               (unsigned long long)image_id, wr.off);
    }
    if (reply_cmd != VN_CMD_TYPE_vkCreateImageView)
        return -5;
    if (vk_result != VK_SUCCESS)
        return (int)vk_result;
    if (!present || !view_reply)
        return -5;

    *out_view_id = view_id;
    return VK_SUCCESS;
}
