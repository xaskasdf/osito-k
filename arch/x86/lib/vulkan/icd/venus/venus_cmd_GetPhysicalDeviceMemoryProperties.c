/*
 * Encoder for vkGetPhysicalDeviceMemoryProperties over venus.
 *
 * Request payload:
 *   pd_id                     (u64)
 *   pMemoryProperties_present (u32, always 1)
 *   pad                       (u32)
 *
 * Reply: flattened VkPhysicalDeviceMemoryProperties
 *   (memoryTypeCount u32 + 32*VkMemoryType + memoryHeapCount u32 +
 *    16*VkMemoryHeap = 520 bytes on LP64). */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

extern void *memcpy(void *, const void *, unsigned long);

int venus_cmd_encode_GetPhysicalDeviceMemoryProperties(
        struct venus_wire *w, uint64_t pd_id,
        VkPhysicalDeviceMemoryProperties *out) {
    if (!w || !out) return -22;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkGetPhysicalDeviceMemoryProperties,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            8u + 4u + 4u,
            &reply_id);
    if (!p) return -12;

    ((uint64_t *)p)[0]        = pd_id;
    ((uint32_t *)(p + 8))[0]  = 1u;
    ((uint32_t *)(p + 12))[0] = 0u;

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    int got = venus_wire_wait_reply(w, reply_id, out,
                                    (uint32_t)sizeof(VkPhysicalDeviceMemoryProperties));
    if (got < (int)sizeof(VkPhysicalDeviceMemoryProperties)) return -5;
    return 0;
}
