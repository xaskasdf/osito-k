/*
 * Encoder for vkCmdBindVertexBuffers over venus.
 *
 * W3b.6 — added so the hello-triangle smoke can record vertex bindings
 * for the CPU-fallback rasterizer at present time.
 *
 * Request payload (variable):
 *   dev_id          (u64)
 *   cb_id           (u64)
 *   firstBinding    (u32)
 *   bindingCount    (u32)
 *   buffer_ids[bindingCount]   (u64 each)
 *   offsets[bindingCount]      (u64 each)
 *
 * No reply expected.
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

#define VENUS_VB_MAX_BINDINGS 8u

int venus_cmd_encode_CmdBindVertexBuffers(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint32_t first_binding, uint32_t binding_count,
        const uint64_t *buffer_ids, const uint64_t *offsets) {
    if (!w) return -22;
    if (binding_count > VENUS_VB_MAX_BINDINGS)
        binding_count = VENUS_VB_MAX_BINDINGS;

    uint32_t payload = 8u + 8u + 4u + 4u
                     + binding_count * 8u
                     + binding_count * 8u;
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCmdBindVertexBuffers,
            0u /* no reply */,
            payload, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;        off += 8;
    *(uint64_t *)(p + off) = cb_id;         off += 8;
    *(uint32_t *)(p + off) = first_binding; off += 4;
    *(uint32_t *)(p + off) = binding_count; off += 4;
    for (uint32_t i = 0; i < binding_count; i++) {
        *(uint64_t *)(p + off) = buffer_ids ? buffer_ids[i] : 0ull;
        off += 8;
    }
    for (uint32_t i = 0; i < binding_count; i++) {
        *(uint64_t *)(p + off) = offsets ? offsets[i] : 0ull;
        off += 8;
    }

    return venus_wire_submit(w);
}
