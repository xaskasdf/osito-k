/*
 * Encoder for vkCreateFramebuffer over venus.
 *
 * Request payload (flattened VkFramebufferCreateInfo):
 *   dev_id                  (u64)
 *   flags                   (u32)
 *   pad                     (u32)
 *   rp_id                   (u64)
 *   attachmentCount         (u32)
 *   pad2                    (u32)
 *   attachments[] * u64     (variable, one view_id per attachment)
 *   width                   (u32)
 *   height                  (u32)
 *   layers                  (u32)
 *   pad3                    (u32)
 *   pAllocator_present      (u32)  always 0
 *   pFramebuffer_present    (u32)  always 1
 *
 * Reply:
 *   VkResult                (u32)
 *   pad                     (u32)
 *   host VkFramebuffer id   (u64)
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

#define VENUS_FB_MAX_ATTS 8u

int venus_cmd_encode_CreateFramebuffer(
        struct venus_wire *w, uint64_t dev_id, uint64_t rp_id,
        const VkFramebufferCreateInfo *pCreateInfo,
        const uint64_t *view_ids, uint32_t view_count,
        uint64_t *out_fb_id) {
    if (!w || !pCreateInfo || !out_fb_id) return -22;
    if (view_count > VENUS_FB_MAX_ATTS) view_count = VENUS_FB_MAX_ATTS;

    uint32_t payload = 8u + 4u + 4u + 8u + 4u + 4u
                     + view_count * 8u
                     + 4u + 4u + 4u + 4u + 4u + 4u;
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCreateFramebuffer,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            payload, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;                       off += 8;
    *(uint32_t *)(p + off) = pCreateInfo->flags;           off += 4;
    *(uint32_t *)(p + off) = 0u;                           off += 4;
    *(uint64_t *)(p + off) = rp_id;                        off += 8;
    *(uint32_t *)(p + off) = view_count;                   off += 4;
    *(uint32_t *)(p + off) = 0u;                           off += 4;
    for (uint32_t i = 0; i < view_count; i++) {
        uint64_t vid = view_ids ? view_ids[i] : 0ull;
        *(uint64_t *)(p + off) = vid;                      off += 8;
    }
    *(uint32_t *)(p + off) = pCreateInfo->width;           off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->height;          off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->layers;          off += 4;
    *(uint32_t *)(p + off) = 0u;                           off += 4;
    *(uint32_t *)(p + off) = 0u;                           off += 4;
    *(uint32_t *)(p + off) = 1u;                           off += 4;

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    *out_fb_id = *(uint64_t *)(reply + 8);
    return (int)vk_result;
}
