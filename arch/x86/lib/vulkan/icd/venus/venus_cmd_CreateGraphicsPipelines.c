/*
 * Encoder for vkCreateGraphicsPipelines over venus.
 *
 * W3b.4 acceptance (enforced — returns VK_ERROR_INITIALIZATION_FAILED
 * otherwise):
 *   - exactly 1 pipeline per call (createInfoCount == 1)
 *   - exactly 2 stages: VERTEX + FRAGMENT
 *   - vertex input: 1 binding + 1 attribute
 *   - single viewport + single scissor
 *   - single color blend attachment
 *   - dynamic state: VIEWPORT + SCISSOR only (optional)
 *
 * Anything outside these bounds is rejected. This is a Wave-4 limitation
 * documented in the plan G5 acceptance bullet.
 *
 * Request payload (flattened — see inline comments for structure). This
 * is the biggest encoder in the wave; each Vk sub-struct is serialized
 * as a mini-flat record with 8-byte alignment between sub-records.
 *
 * Reply:
 *   VkResult                 (u32)
 *   pad                      (u32)
 *   host VkPipeline id       (u64)
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

extern void *memset(void *, int, unsigned long);

int venus_cmd_encode_CreateGraphicsPipelines(
        struct venus_wire *w, uint64_t dev_id,
        uint64_t pipeline_cache_id,
        uint32_t create_info_count,
        const VkGraphicsPipelineCreateInfo *pCreateInfo,
        uint64_t shader_vs_id, uint64_t shader_fs_id,
        uint64_t layout_id, uint64_t rp_id,
        uint64_t *out_pipeline_id) {
    if (!w || !pCreateInfo || !out_pipeline_id) return -22;
    if (create_info_count != 1) return 3;

    /* Subset enforcement. */
    if (pCreateInfo->stageCount != 2) return 3;
    const VkPipelineShaderStageCreateInfo *s0 = &pCreateInfo->pStages[0];
    const VkPipelineShaderStageCreateInfo *s1 = &pCreateInfo->pStages[1];
    int vs_found = 0, fs_found = 0;
    if (s0->stage == VK_SHADER_STAGE_VERTEX_BIT)   vs_found = 1;
    if (s0->stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs_found = 1;
    if (s1->stage == VK_SHADER_STAGE_VERTEX_BIT)   vs_found = 1;
    if (s1->stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs_found = 1;
    if (!vs_found || !fs_found) return 3;

    const VkPipelineVertexInputStateCreateInfo *vi = pCreateInfo->pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo *ia = pCreateInfo->pInputAssemblyState;
    const VkPipelineViewportStateCreateInfo *vp = pCreateInfo->pViewportState;
    const VkPipelineRasterizationStateCreateInfo *rs = pCreateInfo->pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo *ms = pCreateInfo->pMultisampleState;
    const VkPipelineColorBlendStateCreateInfo *cb = pCreateInfo->pColorBlendState;
    const VkPipelineDynamicStateCreateInfo *ds = pCreateInfo->pDynamicState;

    if (!vi || !ia || !vp || !rs || !ms || !cb) return 3;
    if (vi->vertexBindingDescriptionCount != 1)   return 3;
    if (vi->vertexAttributeDescriptionCount != 1) return 3;
    if (vp->viewportCount != 1) return 3;
    if (vp->scissorCount  != 1) return 3;
    if (cb->attachmentCount != 1) return 3;

    uint32_t dyn_count = ds ? ds->dynamicStateCount : 0u;
    if (dyn_count > 8u) dyn_count = 8u;

    /* Payload layout (all u32 unless annotated):
     *   dev_id (u64)
     *   cache_id (u64)
     *   createInfoCount (u32)
     *   pipeline_flags (u32)
     *   stageCount (u32) pad
     *   stage[0]: stage(u32), shader_id(u64), pNameLen(u32)=0, pad
     *   stage[1]: same layout
     *   layout_id (u64), rp_id (u64), subpass (u32), basePipelineIndex (u32)
     *   basePipeline_id (u64)
     *   VI: flags(u32) pad, bindingCount(u32) pad,
     *       bind[0]: binding, stride, rate, pad
     *       attrCount(u32) pad
     *       attr[0]: location, binding, format, offset
     *   IA: flags(u32), topology(u32), primitiveRestart(u32), pad
     *   VP: flags(u32), viewportCount(u32), scissorCount(u32), pad
     *       viewport[0]: x,y,w,h,minDepth,maxDepth (6*f32=24B)
     *       scissor[0]: offsetX(i32), offsetY(i32), extentW(u32), extentH(u32)
     *   RS: flags, depthClampEnable, rasterizerDiscardEnable, polygonMode,
     *       cullMode, frontFace, depthBiasEnable, (7 u32)
     *       depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor,
     *       lineWidth (4 f32)
     *   MS: flags, rasterizationSamples, sampleShadingEnable, minSampleShading,
     *       pSampleMask_present, alphaToCoverageEnable, alphaToOneEnable, pad
     *   CB: flags, logicOpEnable, logicOp, attachmentCount, (4 u32)
     *       attachment[0]: blendEnable, srcColorBF, dstColorBF, colorBlendOp,
     *                      srcAlphaBF, dstAlphaBF, alphaBlendOp, colorWriteMask,
     *                      (8 u32)
     *       blendConstants[4] (4 f32)
     *   DS: flags, dynamicStateCount, dyn[...] u32
     *   pAllocator_present (u32)=0
     *   pPipeline_present (u32)=1
     */
    uint32_t payload =
          8u + 8u + 4u + 4u + 4u + 4u
        + (4u + 8u + 4u + 4u) * 2u   /* stages */
        + 8u + 8u + 4u + 4u + 8u     /* layout + rp + subpass + baseIndex + basePipeline */
        + 4u + 4u + 4u + 4u          /* VI header */
        + 16u                         /* binding[0] */
        + 16u                         /* attr[0] */
        + 16u                         /* IA */
        + 16u                         /* VP header */
        + 24u                         /* viewport[0] */
        + 16u                         /* scissor[0] */
        + 28u + 4u + 16u              /* RS (7 u32 + 4 f32 = 44, padded 48) */
        + 16u + 16u                   /* MS */
        + 16u + 32u + 16u             /* CB */
        + 4u + 4u + dyn_count * 4u    /* DS */
        + 4u + 4u;
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCreateGraphicsPipelines,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            payload, &reply_id);
    if (!p) return -12;
    memset(p, 0, payload);

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;                                  off += 8;
    *(uint64_t *)(p + off) = pipeline_cache_id;                       off += 8;
    *(uint32_t *)(p + off) = create_info_count;                       off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->flags;                      off += 4;
    *(uint32_t *)(p + off) = pCreateInfo->stageCount;                 off += 4;
    *(uint32_t *)(p + off) = 0u;                                      off += 4;

    /* stage[0], stage[1] — use s0, s1 in order. Host resolves shader_vs_id /
     * shader_fs_id via the stage bit on each record. */
    uint64_t ids_by_stage[2] = {
        (s0->stage == VK_SHADER_STAGE_VERTEX_BIT) ? shader_vs_id : shader_fs_id,
        (s1->stage == VK_SHADER_STAGE_VERTEX_BIT) ? shader_vs_id : shader_fs_id
    };
    const VkPipelineShaderStageCreateInfo *stages[2] = { s0, s1 };
    for (int i = 0; i < 2; i++) {
        *(uint32_t *)(p + off) = (uint32_t)stages[i]->stage;           off += 4;
        *(uint64_t *)(p + off) = ids_by_stage[i];                      off += 8;
        *(uint32_t *)(p + off) = 0u;                                   off += 4;
        *(uint32_t *)(p + off) = 0u;                                   off += 4;
    }

    *(uint64_t *)(p + off) = layout_id;                               off += 8;
    *(uint64_t *)(p + off) = rp_id;                                   off += 8;
    *(uint32_t *)(p + off) = pCreateInfo->subpass;                    off += 4;
    *(uint32_t *)(p + off) = (uint32_t)pCreateInfo->basePipelineIndex; off += 4;
    *(uint64_t *)(p + off) = 0ull; /* basePipelineHandle as id */     off += 8;

    /* VI */
    *(uint32_t *)(p + off) = vi->flags;                                off += 4;
    *(uint32_t *)(p + off) = 0u;                                       off += 4;
    *(uint32_t *)(p + off) = 1u;                                       off += 4;
    *(uint32_t *)(p + off) = 0u;                                       off += 4;
    {
        const VkVertexInputBindingDescription *b = &vi->pVertexBindingDescriptions[0];
        *(uint32_t *)(p + off) = b->binding;           off += 4;
        *(uint32_t *)(p + off) = b->stride;            off += 4;
        *(uint32_t *)(p + off) = (uint32_t)b->inputRate; off += 4;
        *(uint32_t *)(p + off) = 0u;                   off += 4;
    }
    /* attrCount: embedded above with VI header, now attr[0]. */
    {
        const VkVertexInputAttributeDescription *a = &vi->pVertexAttributeDescriptions[0];
        *(uint32_t *)(p + off) = a->location;          off += 4;
        *(uint32_t *)(p + off) = a->binding;           off += 4;
        *(uint32_t *)(p + off) = (uint32_t)a->format;  off += 4;
        *(uint32_t *)(p + off) = a->offset;            off += 4;
    }

    /* IA */
    *(uint32_t *)(p + off) = ia->flags;                                off += 4;
    *(uint32_t *)(p + off) = (uint32_t)ia->topology;                   off += 4;
    *(uint32_t *)(p + off) = ia->primitiveRestartEnable;               off += 4;
    *(uint32_t *)(p + off) = 0u;                                       off += 4;

    /* VP */
    *(uint32_t *)(p + off) = vp->flags;                                off += 4;
    *(uint32_t *)(p + off) = vp->viewportCount;                        off += 4;
    *(uint32_t *)(p + off) = vp->scissorCount;                         off += 4;
    *(uint32_t *)(p + off) = 0u;                                       off += 4;
    if (vp->pViewports) {
        const VkViewport *vpv = &vp->pViewports[0];
        *(float *)(p + off) = vpv->x;             off += 4;
        *(float *)(p + off) = vpv->y;             off += 4;
        *(float *)(p + off) = vpv->width;         off += 4;
        *(float *)(p + off) = vpv->height;        off += 4;
        *(float *)(p + off) = vpv->minDepth;      off += 4;
        *(float *)(p + off) = vpv->maxDepth;      off += 4;
    } else {
        off += 24;
    }
    if (vp->pScissors) {
        const VkRect2D *sc = &vp->pScissors[0];
        *(int32_t *)(p + off)  = sc->offset.x;    off += 4;
        *(int32_t *)(p + off)  = sc->offset.y;    off += 4;
        *(uint32_t *)(p + off) = sc->extent.width;  off += 4;
        *(uint32_t *)(p + off) = sc->extent.height; off += 4;
    } else {
        off += 16;
    }

    /* RS */
    *(uint32_t *)(p + off) = rs->flags;                    off += 4;
    *(uint32_t *)(p + off) = rs->depthClampEnable;         off += 4;
    *(uint32_t *)(p + off) = rs->rasterizerDiscardEnable;  off += 4;
    *(uint32_t *)(p + off) = (uint32_t)rs->polygonMode;    off += 4;
    *(uint32_t *)(p + off) = rs->cullMode;                 off += 4;
    *(uint32_t *)(p + off) = (uint32_t)rs->frontFace;      off += 4;
    *(uint32_t *)(p + off) = rs->depthBiasEnable;          off += 4;
    *(uint32_t *)(p + off) = 0u;                           off += 4;
    *(float *)(p + off) = rs->depthBiasConstantFactor;     off += 4;
    *(float *)(p + off) = rs->depthBiasClamp;              off += 4;
    *(float *)(p + off) = rs->depthBiasSlopeFactor;        off += 4;
    *(float *)(p + off) = rs->lineWidth;                   off += 4;

    /* MS */
    *(uint32_t *)(p + off) = ms->flags;                         off += 4;
    *(uint32_t *)(p + off) = (uint32_t)ms->rasterizationSamples; off += 4;
    *(uint32_t *)(p + off) = ms->sampleShadingEnable;           off += 4;
    *(float *)   (p + off) = ms->minSampleShading;              off += 4;
    *(uint32_t *)(p + off) = ms->pSampleMask ? 1u : 0u;         off += 4;
    *(uint32_t *)(p + off) = ms->alphaToCoverageEnable;         off += 4;
    *(uint32_t *)(p + off) = ms->alphaToOneEnable;              off += 4;
    *(uint32_t *)(p + off) = 0u;                                off += 4;

    /* CB */
    *(uint32_t *)(p + off) = cb->flags;                    off += 4;
    *(uint32_t *)(p + off) = cb->logicOpEnable;            off += 4;
    *(uint32_t *)(p + off) = (uint32_t)cb->logicOp;        off += 4;
    *(uint32_t *)(p + off) = cb->attachmentCount;          off += 4;
    {
        const VkPipelineColorBlendAttachmentState *a = &cb->pAttachments[0];
        *(uint32_t *)(p + off) = a->blendEnable;           off += 4;
        *(uint32_t *)(p + off) = (uint32_t)a->srcColorBlendFactor; off += 4;
        *(uint32_t *)(p + off) = (uint32_t)a->dstColorBlendFactor; off += 4;
        *(uint32_t *)(p + off) = (uint32_t)a->colorBlendOp;         off += 4;
        *(uint32_t *)(p + off) = (uint32_t)a->srcAlphaBlendFactor;  off += 4;
        *(uint32_t *)(p + off) = (uint32_t)a->dstAlphaBlendFactor;  off += 4;
        *(uint32_t *)(p + off) = (uint32_t)a->alphaBlendOp;         off += 4;
        *(uint32_t *)(p + off) = a->colorWriteMask;        off += 4;
    }
    *(float *)(p + off) = cb->blendConstants[0]; off += 4;
    *(float *)(p + off) = cb->blendConstants[1]; off += 4;
    *(float *)(p + off) = cb->blendConstants[2]; off += 4;
    *(float *)(p + off) = cb->blendConstants[3]; off += 4;

    /* DS */
    *(uint32_t *)(p + off) = ds ? ds->flags : 0u; off += 4;
    *(uint32_t *)(p + off) = dyn_count;           off += 4;
    for (uint32_t i = 0; i < dyn_count; i++) {
        *(uint32_t *)(p + off) = (uint32_t)ds->pDynamicStates[i];
        off += 4;
    }

    *(uint32_t *)(p + off) = 0u; off += 4;
    *(uint32_t *)(p + off) = 1u; off += 4;

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    *out_pipeline_id = *(uint64_t *)(reply + 8);
    return (int)vk_result;
}
