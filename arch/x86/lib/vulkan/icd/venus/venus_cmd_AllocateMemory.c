/*
 * Encoder for vkAllocateMemory over venus.
 *
 * Request payload (flattened VkMemoryAllocateInfo — W3b.3 subset, no pNext
 * chain: import/export descriptors, dedicated-allocation info, etc. are
 * deferred):
 *   dev_id                 (u64)
 *   allocationSize         (u64)
 *   memoryTypeIndex        (u32)
 *   pad                    (u32)
 *   pAllocator_present     (u32)   always 0
 *   pMemory_present        (u32)   always 1
 *
 * Reply payload:
 *   VkResult               (u32)
 *   pad                    (u32)
 *   host VkDeviceMemory id (u64)
 *
 * See master plan §W3b.3 T4. */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_AllocateMemory(
        struct venus_wire *w, uint64_t dev_id,
        uint64_t allocation_size, uint32_t memory_type_index,
        uint64_t *out_memory_id) {
    if (!w || !out_memory_id) return -22;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkAllocateMemory,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            8u + 8u + 4u + 4u + 4u + 4u,
            &reply_id);
    if (!p) return -12;

    *(uint64_t *)(p + 0)  = dev_id;
    *(uint64_t *)(p + 8)  = allocation_size;
    *(uint32_t *)(p + 16) = memory_type_index;
    *(uint32_t *)(p + 20) = 0u;
    *(uint32_t *)(p + 24) = 0u; /* pAllocator null */
    *(uint32_t *)(p + 28) = 1u; /* pMemory present */

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    *out_memory_id = *(uint64_t *)(reply + 8);
    return (int)vk_result;
}
