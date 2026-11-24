/*
 * Encoder for vkMapMemory over venus.
 *
 * Request payload:
 *   dev_id          (u64)
 *   mem_id          (u64)
 *   offset          (u64)
 *   size            (u64)   VK_WHOLE_SIZE == UINT64_MAX
 *   flags           (u32)
 *   pad             (u32)
 *   ppData_present  (u32)   always 1
 *   pad2            (u32)
 *
 * Reply:
 *   VkResult        (u32)
 *   pad             (u32)
 *   host-side mapped pointer (u64) — opaque handle from guest POV; in
 *                   W3b.3 the guest does NOT dereference this. Callers
 *                   use the guest-local backing buffer instead (see
 *                   venus_device.c::venus_MapMemory). The roundtrip
 *                   happens for protocol fidelity only.
 *
 * Important: W3b.3 does NOT provide the guest with a real writable
 * pointer to host memory; that requires a host-coherent mapping which
 * is not wired through SYS_GPU_RES_* for arbitrary host allocations
 * yet. Landing in W3b.4. See master plan §W3b.3 T6 for details. */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_MapMemory(
        struct venus_wire *w, uint64_t dev_id, uint64_t mem_id,
        uint64_t offset, uint64_t size, uint32_t flags,
        uint64_t *out_host_ptr) {
    if (!w) return -22;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkMapMemory,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            8u + 8u + 8u + 8u + 4u + 4u + 4u + 4u,
            &reply_id);
    if (!p) return -12;

    *(uint64_t *)(p + 0)  = dev_id;
    *(uint64_t *)(p + 8)  = mem_id;
    *(uint64_t *)(p + 16) = offset;
    *(uint64_t *)(p + 24) = size;
    *(uint32_t *)(p + 32) = flags;
    *(uint32_t *)(p + 36) = 0u;
    *(uint32_t *)(p + 40) = 1u; /* ppData present */
    *(uint32_t *)(p + 44) = 0u;

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    if (out_host_ptr) *out_host_ptr = *(uint64_t *)(reply + 8);
    return (int)vk_result;
}
