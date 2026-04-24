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
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_CreateImageView(
        struct venus_wire *w, uint64_t dev_id, uint64_t image_id,
        const VkImageViewCreateInfo *pCreateInfo,
        uint64_t *out_view_id) {
    if (!w || !pCreateInfo || !out_view_id) return -22;

    uint32_t payload =
          8u
        + 4u + 4u
        + 8u
        + 4u + 4u
        + 4u*4u
        + 4u*5u
        + 4u
        + 4u + 4u;
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCreateImageView,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            payload, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;                                off += 8;
    *(uint32_t *)(p + off) = pCreateInfo->flags;                    off += 4;
    *(uint32_t *)(p + off) = 0u;                                    off += 4;
    *(uint64_t *)(p + off) = image_id;                              off += 8;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->viewType;       off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->format;         off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->components.r;   off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->components.g;   off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->components.b;   off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->components.a;   off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->subresourceRange.aspectMask;     off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->subresourceRange.baseMipLevel;   off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->subresourceRange.levelCount;     off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->subresourceRange.baseArrayLayer; off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->subresourceRange.layerCount;     off += 4;
    *(uint32_t *)(p + off) = 0u; off += 4;
    *(uint32_t *)(p + off) = 0u; off += 4;
    *(uint32_t *)(p + off) = 1u; off += 4;

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    *out_view_id = *(uint64_t *)(reply + 8);
    return (int)vk_result;
}
