/*
 * Encoder for vkCmdSetScissor over venus.
 *
 * W3b.6 — dynamic scissor state. The CPU-fallback rasterizer clips to
 * the framebuffer extent, not the recorded scissor; this encoder is
 * still serialized so a real host can act on it later.
 *
 * Request payload:
 *   dev_id           (u64)
 *   cb_id            (u64)
 *   firstScissor     (u32)
 *   scissorCount     (u32)
 *   scissors[scissorCount] each: { offset.x i32, offset.y i32, ext.w u32, ext.h u32 }
 *
 * No reply expected.
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

extern void *memcpy(void *, const void *, unsigned long);

#define VENUS_SC_MAX 4u

int venus_cmd_encode_CmdSetScissor(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint32_t first_scissor, uint32_t scissor_count,
        const VkRect2D *scissors) {
    if (!w) return -22;
    if (scissor_count > VENUS_SC_MAX) scissor_count = VENUS_SC_MAX;

    uint32_t payload = 8u + 8u + 4u + 4u
                     + scissor_count * (uint32_t)sizeof(VkRect2D);
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCmdSetScissor,
            0u /* no reply */,
            payload, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;         off += 8;
    *(uint64_t *)(p + off) = cb_id;          off += 8;
    *(uint32_t *)(p + off) = first_scissor;  off += 4;
    *(uint32_t *)(p + off) = scissor_count;  off += 4;
    if (scissor_count && scissors)
        memcpy(p + off, scissors, scissor_count * sizeof(VkRect2D));

    return venus_wire_submit(w);
}
