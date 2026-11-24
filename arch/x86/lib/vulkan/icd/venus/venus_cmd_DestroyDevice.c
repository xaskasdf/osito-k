/*
 * Encoder for vkDestroyDevice over venus.
 *
 * Request payload:
 *   dev_id                 (u64)  host-assigned VkDevice handle
 *   pAllocator_present     (u32)  always 0 in W3b.3
 *   pad                    (u32)
 *
 * No reply expected. See master plan §W3b.3 T3. */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_DestroyDevice(struct venus_wire *w, uint64_t dev_id) {
    if (!w) return -22;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkDestroyDevice,
            0u /* no reply */,
            8u + 4u + 4u,
            &reply_id);
    if (!p) return -12;

    *(uint64_t *)p        = dev_id;
    *(uint32_t *)(p + 8)  = 0u;
    *(uint32_t *)(p + 12) = 0u;

    return venus_wire_submit(w);
}
