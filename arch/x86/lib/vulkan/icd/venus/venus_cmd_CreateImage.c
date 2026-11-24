#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VENUS_CI_MAX_QFI 8u
#define VN_CMD_TYPE_vkCreateImage 54u
#define VN_CMD_GENERATE_REPLY 1u

extern int printf(const char *, ...);

int venus_cmd_encode_CreateImage(
        struct venus_wire *w, uint64_t dev_id,
        const VkImageCreateInfo *pCreateInfo,
        uint64_t *out_image_id) {
    if (!w || !pCreateInfo || !out_image_id) return -22;

    extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);

    uint8_t cmd[768];
    uint8_t reply[24];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };
    uint64_t image_id = venus_wire_alloc_object_id(w);
    if (!image_id) return -12;

    uint32_t qfi_count = pCreateInfo->queueFamilyIndexCount;
    if (qfi_count > VENUS_CI_MAX_QFI) qfi_count = VENUS_CI_MAX_QFI;

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCreateImage);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);

    vcw_wr_u64(&wr, 1);                                      /* pCreateInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    vcw_wr_u64(&wr, 0);                                      /* pNext */
    vcw_wr_u32(&wr, pCreateInfo->flags);
    vcw_wr_i32(&wr, (int32_t)pCreateInfo->imageType);
    vcw_wr_i32(&wr, (int32_t)pCreateInfo->format);
    vcw_wr_u32(&wr, pCreateInfo->extent.width);
    vcw_wr_u32(&wr, pCreateInfo->extent.height);
    vcw_wr_u32(&wr, pCreateInfo->extent.depth);
    vcw_wr_u32(&wr, pCreateInfo->mipLevels);
    vcw_wr_u32(&wr, pCreateInfo->arrayLayers);
    vcw_wr_i32(&wr, (int32_t)pCreateInfo->samples);
    vcw_wr_i32(&wr, (int32_t)pCreateInfo->tiling);
    vcw_wr_u32(&wr, pCreateInfo->usage);
    vcw_wr_i32(&wr, (int32_t)pCreateInfo->sharingMode);
    vcw_wr_u32(&wr, qfi_count);
    vcw_wr_array_size(&wr, qfi_count);
    for (uint32_t i = 0; i < qfi_count; i++) {
        uint32_t idx = pCreateInfo->pQueueFamilyIndices
                       ? pCreateInfo->pQueueFamilyIndices[i] : 0u;
        vcw_wr_u32(&wr, idx);
    }

    vcw_wr_i32(&wr, (int32_t)pCreateInfo->initialLayout);
    vcw_wr_u64(&wr, 0);                                      /* pAllocator */
    vcw_wr_u64(&wr, 1);                                      /* pImage */
    vcw_wr_u64(&wr, image_id);
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t present   = *(uint64_t *)(reply + 8);
    uint64_t img_reply = *(uint64_t *)(reply + 16);
    static uint32_t log_count;
    if (log_count < 16u) {
        log_count++;
        printf("[VCI] reply cmd=%u vk=%u present=%llu img=%llu guest=%llu bytes=%u %ux%u fmt=%u usage=0x%x\n",
               reply_cmd, vk_result, (unsigned long long)present,
               (unsigned long long)img_reply,
               (unsigned long long)image_id, wr.off,
               pCreateInfo->extent.width, pCreateInfo->extent.height,
               (uint32_t)pCreateInfo->format, pCreateInfo->usage);
    }
    if (reply_cmd != VN_CMD_TYPE_vkCreateImage)
        return -5;
    if (vk_result != VK_SUCCESS)
        return (int)vk_result;
    if (!present || !img_reply)
        return -5;

    *out_image_id = image_id;
    return VK_SUCCESS;
}
