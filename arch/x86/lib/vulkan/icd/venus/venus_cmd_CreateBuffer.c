/*
 * Encoder for vkCreateBuffer over venus.
 *
 * Request payload (flattened VkBufferCreateInfo — W3b.3 subset, no pNext
 * chain, queue family array capped):
 *   dev_id                        (u64)
 *   flags                         (u32)
 *   pad                           (u32)
 *   size                          (u64)
 *   usage                         (u32)
 *   sharingMode                   (u32)
 *   queueFamilyIndexCount         (u32)
 *   pad2                          (u32)
 *   queueFamilyIndexCount * u32   (variable)
 *   pAllocator_present            (u32)  always 0
 *   pBuffer_present               (u32)  always 1
 *
 * Reply:
 *   VkResult                      (u32)
 *   pad                           (u32)
 *   host VkBuffer id              (u64)
 *
 * See master plan §W3b.3 T8. */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

#define VENUS_CB_MAX_QFI 8u

int venus_cmd_encode_CreateBuffer(
        struct venus_wire *w, uint64_t dev_id,
        const VkBufferCreateInfo *pCreateInfo,
        uint64_t *out_buffer_id) {
    if (!w || !pCreateInfo || !out_buffer_id) return -22;

    uint32_t qfi_count = pCreateInfo->queueFamilyIndexCount;
    if (qfi_count > VENUS_CB_MAX_QFI) qfi_count = VENUS_CB_MAX_QFI;

    uint32_t size = 8u + 4u + 4u + 8u + 4u + 4u + 4u + 4u
                    + qfi_count * 4u + 4u + 4u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCreateBuffer,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            size, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;                     off += 8;
    *(uint32_t *)(p + off) = pCreateInfo->flags;         off += 4;
    *(uint32_t *)(p + off) = 0u;                         off += 4;
    *(uint64_t *)(p + off) = pCreateInfo->size;          off += 8;
    *(uint32_t *)(p + off) = pCreateInfo->usage;         off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->sharingMode; off += 4;
    *(uint32_t *)(p + off) = qfi_count;                  off += 4;
    *(uint32_t *)(p + off) = 0u;                         off += 4;
    for (uint32_t i = 0; i < qfi_count; i++) {
        uint32_t idx = pCreateInfo->pQueueFamilyIndices
                       ? pCreateInfo->pQueueFamilyIndices[i] : 0u;
        *(uint32_t *)(p + off) = idx;                    off += 4;
    }
    *(uint32_t *)(p + off) = 0u; off += 4; /* pAllocator */
    *(uint32_t *)(p + off) = 1u; off += 4; /* pBuffer present */

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    *out_buffer_id = *(uint64_t *)(reply + 8);
    return (int)vk_result;
}
