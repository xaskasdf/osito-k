/*
 * Encoder for vkCmdSetViewport over venus.
 *
 * W3b.6 — dynamic viewport state. The CPU-fallback rasterizer ignores
 * the recorded viewport (it always paints over the full framebuffer),
 * but the wire path serializes the data in case a real host picks it up.
 *
 * Request payload:
 *   dev_id            (u64)
 *   cb_id             (u64)
 *   firstViewport     (u32)
 *   viewportCount     (u32)
 *   viewports[viewportCount] of 6 floats each: { x, y, w, h, minDepth, maxDepth }
 *
 * No reply expected.
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

extern void *memcpy(void *, const void *, unsigned long);

#define VENUS_VP_MAX 4u

int venus_cmd_encode_CmdSetViewport(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint32_t first_viewport, uint32_t viewport_count,
        const VkViewport *viewports) {
    if (!w) return -22;
    if (viewport_count > VENUS_VP_MAX) viewport_count = VENUS_VP_MAX;

    uint32_t payload = 8u + 8u + 4u + 4u
                     + viewport_count * (uint32_t)sizeof(VkViewport);
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCmdSetViewport,
            0u /* no reply */,
            payload, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;          off += 8;
    *(uint64_t *)(p + off) = cb_id;           off += 8;
    *(uint32_t *)(p + off) = first_viewport;  off += 4;
    *(uint32_t *)(p + off) = viewport_count;  off += 4;
    if (viewport_count && viewports)
        memcpy(p + off, viewports, viewport_count * sizeof(VkViewport));

    return venus_wire_submit(w);
}
