/*
 * Encoder for vkCreateRenderPass over venus.
 *
 * W3b.4 acceptance: single-subpass, single color attachment. More
 * attachments / subpasses → VK_ERROR_INITIALIZATION_FAILED.
 *
 * Request payload (flattened VkRenderPassCreateInfo — W3b.4 subset):
 *   dev_id                   (u64)
 *   flags                    (u32)
 *   pad                      (u32)
 *   attachmentCount          (u32)   must be 1
 *   subpassCount             (u32)   must be 1
 *   dependencyCount          (u32)
 *   pad2                     (u32)
 *   attachment[0] flattened  (9 * u32 + pad = 40 bytes)
 *     flags, format, samples, loadOp, storeOp,
 *     stencilLoadOp, stencilStoreOp, initialLayout, finalLayout, pad
 *   subpass[0] flattened     (40 bytes = 10 * u32)
 *     flags, pipelineBindPoint, inputAttachmentCount,
 *     colorAttachmentCount (must be 1),
 *     colorAttachment[0] attachment/layout (2 * u32),
 *     depthStencilAttachment_present (u32), ds_attachment, ds_layout,
 *     preserveAttachmentCount (u32)
 *   dependencies[] flattened (28 bytes each)
 *     srcSubpass, dstSubpass, srcStage, dstStage, srcAccess, dstAccess, flags
 *   pAllocator_present       (u32)  always 0
 *   pRenderPass_present      (u32)  always 1
 *
 * Reply:
 *   VkResult                 (u32)
 *   pad                      (u32)
 *   host VkRenderPass id     (u64)
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

#define VENUS_RP_MAX_DEPS 4u

int venus_cmd_encode_CreateRenderPass(
        struct venus_wire *w, uint64_t dev_id,
        const VkRenderPassCreateInfo *pCreateInfo,
        uint64_t *out_rp_id) {
    if (!w || !pCreateInfo || !out_rp_id) return -22;

    /* W3b.4 subset. */
    if (pCreateInfo->attachmentCount != 1 || !pCreateInfo->pAttachments)
        return 3; /* VK_ERROR_INITIALIZATION_FAILED */
    if (pCreateInfo->subpassCount != 1 || !pCreateInfo->pSubpasses)
        return 3;

    uint32_t dep_count = pCreateInfo->dependencyCount;
    if (dep_count > VENUS_RP_MAX_DEPS) dep_count = VENUS_RP_MAX_DEPS;

    const VkAttachmentDescription *att = &pCreateInfo->pAttachments[0];
    const VkSubpassDescription    *sp  = &pCreateInfo->pSubpasses[0];
    if (sp->colorAttachmentCount != 1 || !sp->pColorAttachments)
        return 3;

    const uint32_t att_block = 40u;   /* 10 u32 */
    const uint32_t sp_block  = 40u;   /* see layout below */
    const uint32_t dep_block = 28u;   /* 7 u32 */

    uint32_t payload = 8u + 4u + 4u + 4u + 4u + 4u + 4u
                     + att_block + sp_block
                     + dep_count * dep_block
                     + 4u + 4u;
    /* pad to 8 */
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCreateRenderPass,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            payload, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;                                  off += 8;
    *(uint32_t *)(p + off) = pCreateInfo->flags;                      off += 4;
    *(uint32_t *)(p + off) = 0u;                                      off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->attachmentCount;            off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->subpassCount;               off += 4;
    *(uint32_t *)(p + off) = dep_count;                               off += 4;
    *(uint32_t *)(p + off) = 0u;                                      off += 4;

    /* attachment[0] — 10 u32 = 40 bytes. */
    *(uint32_t *)(p + off) = att->flags;             off += 4;
    *(uint32_t *)(p + off) = (uint32_t)att->format;  off += 4;
    *(uint32_t *)(p + off) = (uint32_t)att->samples; off += 4;
    *(uint32_t *)(p + off) = (uint32_t)att->loadOp;  off += 4;
    *(uint32_t *)(p + off) = (uint32_t)att->storeOp; off += 4;
    *(uint32_t *)(p + off) = (uint32_t)att->stencilLoadOp;  off += 4;
    *(uint32_t *)(p + off) = (uint32_t)att->stencilStoreOp; off += 4;
    *(uint32_t *)(p + off) = (uint32_t)att->initialLayout;  off += 4;
    *(uint32_t *)(p + off) = (uint32_t)att->finalLayout;    off += 4;
    *(uint32_t *)(p + off) = 0u;                                      off += 4;

    /* subpass[0] — 10 u32 = 40 bytes. */
    *(uint32_t *)(p + off) = sp->flags;                         off += 4;
    *(uint32_t *)(p + off) = (uint32_t)sp->pipelineBindPoint;   off += 4;
    *(uint32_t *)(p + off) = sp->inputAttachmentCount;          off += 4;
    *(uint32_t *)(p + off) = sp->colorAttachmentCount;          off += 4;
    *(uint32_t *)(p + off) = sp->pColorAttachments[0].attachment; off += 4;
    *(uint32_t *)(p + off) = (uint32_t)sp->pColorAttachments[0].layout; off += 4;
    uint32_t ds_present = (sp->pDepthStencilAttachment != (const VkAttachmentReference *)0);
    *(uint32_t *)(p + off) = ds_present;                        off += 4;
    *(uint32_t *)(p + off) = ds_present ? sp->pDepthStencilAttachment->attachment : 0u; off += 4;
    *(uint32_t *)(p + off) = ds_present ? (uint32_t)sp->pDepthStencilAttachment->layout : 0u; off += 4;
    *(uint32_t *)(p + off) = sp->preserveAttachmentCount;       off += 4;

    /* dependencies */
    for (uint32_t i = 0; i < dep_count; i++) {
        const VkSubpassDependency *d = &pCreateInfo->pDependencies[i];
        *(uint32_t *)(p + off) = d->srcSubpass;    off += 4;
        *(uint32_t *)(p + off) = d->dstSubpass;    off += 4;
        *(uint32_t *)(p + off) = d->srcStageMask;  off += 4;
        *(uint32_t *)(p + off) = d->dstStageMask;  off += 4;
        *(uint32_t *)(p + off) = d->srcAccessMask; off += 4;
        *(uint32_t *)(p + off) = d->dstAccessMask; off += 4;
        *(uint32_t *)(p + off) = d->dependencyFlags; off += 4;
    }
    *(uint32_t *)(p + off) = 0u; off += 4; /* pAllocator null */
    *(uint32_t *)(p + off) = 1u; off += 4; /* pRenderPass present */
    /* Tail pad already zeroed by ring. */

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    *out_rp_id = *(uint64_t *)(reply + 8);
    return (int)vk_result;
}
