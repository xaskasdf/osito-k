/*
 * Encoder for vkCreateImage over venus.
 *
 * Request payload (flattened VkImageCreateInfo — W3b.4 subset):
 *   dev_id               (u64)
 *   flags                (u32)
 *   imageType            (u32)
 *   format               (u32)
 *   pad                  (u32)
 *   extent.width         (u32)
 *   extent.height        (u32)
 *   extent.depth         (u32)
 *   mipLevels            (u32)
 *   arrayLayers          (u32)
 *   samples              (u32)
 *   tiling               (u32)
 *   pad2                 (u32)
 *   usage                (u32)
 *   sharingMode          (u32)
 *   queueFamilyIndexCount(u32)
 *   pad3                 (u32)
 *   qfi[] * u32          (variable)
 *   initialLayout        (u32)
 *   pAllocator_present   (u32)  always 0
 *   pImage_present       (u32)  always 1
 *   pad4                 (u32)
 *
 * Reply:
 *   VkResult             (u32)
 *   pad                  (u32)
 *   host VkImage id      (u64)
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

#define VENUS_CI_MAX_QFI 8u

int venus_cmd_encode_CreateImage(
        struct venus_wire *w, uint64_t dev_id,
        const VkImageCreateInfo *pCreateInfo,
        uint64_t *out_image_id) {
    if (!w || !pCreateInfo || !out_image_id) return -22;

    uint32_t qfi_count = pCreateInfo->queueFamilyIndexCount;
    if (qfi_count > VENUS_CI_MAX_QFI) qfi_count = VENUS_CI_MAX_QFI;

    uint32_t payload =
          8u                      /* dev_id */
        + 4u + 4u + 4u + 4u       /* flags/type/format/pad */
        + 4u + 4u + 4u            /* extent */
        + 4u + 4u + 4u + 4u + 4u  /* mip/layers/samples/tiling/pad */
        + 4u + 4u + 4u + 4u       /* usage/sharing/qfi_count/pad */
        + qfi_count * 4u
        + 4u + 4u + 4u + 4u;      /* initialLayout/pAlloc/pImage/pad */
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCreateImage,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            payload, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;                                  off += 8;
    *(uint32_t *)(p + off) = pCreateInfo->flags;                      off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->imageType;        off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->format;           off += 4;
    *(uint32_t *)(p + off) = 0u;                                      off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->extent.width;               off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->extent.height;              off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->extent.depth;               off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->mipLevels;                  off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->arrayLayers;                off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->samples;          off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->tiling;           off += 4;
    *(uint32_t *)(p + off) = 0u;                                      off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->usage;                      off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->sharingMode;      off += 4;
    *(uint32_t *)(p + off) = qfi_count;                               off += 4;
    *(uint32_t *)(p + off) = 0u;                                      off += 4;
    for (uint32_t i = 0; i < qfi_count; i++) {
        uint32_t idx = pCreateInfo->pQueueFamilyIndices
                       ? pCreateInfo->pQueueFamilyIndices[i] : 0u;
        *(uint32_t *)(p + off) = idx;                                 off += 4;
    }
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->initialLayout;    off += 4;
    *(uint32_t *)(p + off) = 0u; off += 4;  /* pAllocator null */
    *(uint32_t *)(p + off) = 1u; off += 4;  /* pImage present */
    *(uint32_t *)(p + off) = 0u; off += 4;  /* pad */

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    *out_image_id = *(uint64_t *)(reply + 8);
    return (int)vk_result;
}
