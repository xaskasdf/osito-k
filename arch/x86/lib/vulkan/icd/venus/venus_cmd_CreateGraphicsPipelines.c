#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkCreateGraphicsPipelines 65u
#define VN_CMD_GENERATE_REPLY 1u
#define VENUS_PIPELINE_CMD_CAP (256u * 1024u)

extern void *malloc(unsigned long);
extern void free(void *);
extern int printf(const char *, ...);

static void vcw_wr_float(struct venus_cmd_writer *wr, float v) {
    vcw_wr_bytes(wr, &v, 4);
}

static uint32_t venus_cstr_size(const char *s) {
    uint32_t n = 0;
    if (!s) return 0;
    while (n < 4095u && s[n])
        n++;
    return n + 1u;
}

static const VkPipelineRenderingCreateInfo *
venus_find_pipeline_rendering(const void *pnext) {
    const VkBaseInStructure *base = (const VkBaseInStructure *)pnext;
    while (base) {
        if (base->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
            return (const VkPipelineRenderingCreateInfo *)base;
        base = base->pNext;
    }
    return 0;
}

static void venus_write_shader_stage(struct venus_cmd_writer *wr,
                                     const VkPipelineShaderStageCreateInfo *st,
                                     uint64_t shader_id) {
    const char *name = st->pName ? st->pName : "main";
    uint32_t name_size = venus_cstr_size(name);

    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u32(wr, st->flags);
    vcw_wr_i32(wr, (int32_t)st->stage);
    vcw_wr_u64(wr, shader_id);
    vcw_wr_array_size(wr, name_size);
    vcw_wr_blob_array(wr, name, name_size);
    if (st->pSpecializationInfo) {
        const VkSpecializationInfo *sp = st->pSpecializationInfo;
        vcw_wr_u64(wr, 1);
        vcw_wr_u32(wr, sp->mapEntryCount);
        vcw_wr_array_size(wr, sp->mapEntryCount);
        for (uint32_t i = 0; i < sp->mapEntryCount; i++) {
            const VkSpecializationMapEntry *e = &sp->pMapEntries[i];
            vcw_wr_u32(wr, e->constantID);
            vcw_wr_u32(wr, e->offset);
            vcw_wr_u64(wr, (uint64_t)e->size);
        }
        vcw_wr_u64(wr, (uint64_t)sp->dataSize);
        if (sp->pData && sp->dataSize) {
            if ((uint64_t)sp->dataSize > 0xffffffffu) {
                wr->err = -12;
                return;
            }
            vcw_wr_array_size(wr, (uint64_t)sp->dataSize);
            vcw_wr_blob_array(wr, sp->pData, (uint32_t)sp->dataSize);
        } else {
            vcw_wr_array_size(wr, 0);
        }
    } else {
        vcw_wr_u64(wr, 0);
    }
}

static void venus_write_vertex_input(struct venus_cmd_writer *wr,
                                     const VkPipelineVertexInputStateCreateInfo *vi) {
    vcw_wr_u64(wr, 1);
    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u32(wr, vi->flags);
    vcw_wr_u32(wr, vi->vertexBindingDescriptionCount);
    vcw_wr_array_size(wr, vi->vertexBindingDescriptionCount);
    for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++) {
        const VkVertexInputBindingDescription *b = &vi->pVertexBindingDescriptions[i];
        vcw_wr_u32(wr, b->binding);
        vcw_wr_u32(wr, b->stride);
        vcw_wr_i32(wr, (int32_t)b->inputRate);
    }
    vcw_wr_u32(wr, vi->vertexAttributeDescriptionCount);
    vcw_wr_array_size(wr, vi->vertexAttributeDescriptionCount);
    for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
        const VkVertexInputAttributeDescription *a = &vi->pVertexAttributeDescriptions[i];
        vcw_wr_u32(wr, a->location);
        vcw_wr_u32(wr, a->binding);
        vcw_wr_i32(wr, (int32_t)a->format);
        vcw_wr_u32(wr, a->offset);
    }
}

static void venus_write_input_assembly(struct venus_cmd_writer *wr,
                                       const VkPipelineInputAssemblyStateCreateInfo *ia) {
    vcw_wr_u64(wr, 1);
    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u32(wr, ia->flags);
    vcw_wr_i32(wr, (int32_t)ia->topology);
    vcw_wr_u32(wr, ia->primitiveRestartEnable);
}

static void venus_write_viewport_state(struct venus_cmd_writer *wr,
                                       const VkPipelineViewportStateCreateInfo *vp) {
    vcw_wr_u64(wr, 1);
    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u32(wr, vp->flags);
    vcw_wr_u32(wr, vp->viewportCount);
    if (vp->pViewports) {
        vcw_wr_array_size(wr, vp->viewportCount);
        for (uint32_t i = 0; i < vp->viewportCount; i++) {
            const VkViewport *v = &vp->pViewports[i];
            vcw_wr_float(wr, v->x);
            vcw_wr_float(wr, v->y);
            vcw_wr_float(wr, v->width);
            vcw_wr_float(wr, v->height);
            vcw_wr_float(wr, v->minDepth);
            vcw_wr_float(wr, v->maxDepth);
        }
    } else {
        vcw_wr_array_size(wr, 0);
    }
    vcw_wr_u32(wr, vp->scissorCount);
    if (vp->pScissors) {
        vcw_wr_array_size(wr, vp->scissorCount);
        for (uint32_t i = 0; i < vp->scissorCount; i++) {
            const VkRect2D *s = &vp->pScissors[i];
            vcw_wr_i32(wr, s->offset.x);
            vcw_wr_i32(wr, s->offset.y);
            vcw_wr_u32(wr, s->extent.width);
            vcw_wr_u32(wr, s->extent.height);
        }
    } else {
        vcw_wr_array_size(wr, 0);
    }
}

static void venus_write_rasterization(struct venus_cmd_writer *wr,
                                      const VkPipelineRasterizationStateCreateInfo *rs) {
    vcw_wr_u64(wr, 1);
    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u32(wr, rs->flags);
    vcw_wr_u32(wr, rs->depthClampEnable);
    vcw_wr_u32(wr, rs->rasterizerDiscardEnable);
    vcw_wr_i32(wr, (int32_t)rs->polygonMode);
    vcw_wr_u32(wr, rs->cullMode);
    vcw_wr_i32(wr, (int32_t)rs->frontFace);
    vcw_wr_u32(wr, rs->depthBiasEnable);
    vcw_wr_float(wr, rs->depthBiasConstantFactor);
    vcw_wr_float(wr, rs->depthBiasClamp);
    vcw_wr_float(wr, rs->depthBiasSlopeFactor);
    vcw_wr_float(wr, rs->lineWidth);
}

static void venus_write_multisample(struct venus_cmd_writer *wr,
                                    const VkPipelineMultisampleStateCreateInfo *ms) {
    vcw_wr_u64(wr, 1);
    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u32(wr, ms->flags);
    vcw_wr_i32(wr, (int32_t)ms->rasterizationSamples);
    vcw_wr_u32(wr, ms->sampleShadingEnable);
    vcw_wr_float(wr, ms->minSampleShading);
    if (ms->pSampleMask) {
        uint32_t sample_words = ((uint32_t)ms->rasterizationSamples + 31u) / 32u;
        vcw_wr_array_size(wr, sample_words);
        for (uint32_t i = 0; i < sample_words; i++)
            vcw_wr_u32(wr, ms->pSampleMask[i]);
    } else {
        vcw_wr_array_size(wr, 0);
    }
    vcw_wr_u32(wr, ms->alphaToCoverageEnable);
    vcw_wr_u32(wr, ms->alphaToOneEnable);
}

static void venus_write_stencil(struct venus_cmd_writer *wr,
                                const VkStencilOpState *s) {
    vcw_wr_i32(wr, (int32_t)s->failOp);
    vcw_wr_i32(wr, (int32_t)s->passOp);
    vcw_wr_i32(wr, (int32_t)s->depthFailOp);
    vcw_wr_i32(wr, (int32_t)s->compareOp);
    vcw_wr_u32(wr, s->compareMask);
    vcw_wr_u32(wr, s->writeMask);
    vcw_wr_u32(wr, s->reference);
}

static void venus_write_depth_stencil(struct venus_cmd_writer *wr,
                                      const VkPipelineDepthStencilStateCreateInfo *ds) {
    if (!ds) {
        vcw_wr_u64(wr, 0);
        return;
    }
    vcw_wr_u64(wr, 1);
    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u32(wr, ds->flags);
    vcw_wr_u32(wr, ds->depthTestEnable);
    vcw_wr_u32(wr, ds->depthWriteEnable);
    vcw_wr_i32(wr, (int32_t)ds->depthCompareOp);
    vcw_wr_u32(wr, ds->depthBoundsTestEnable);
    vcw_wr_u32(wr, ds->stencilTestEnable);
    venus_write_stencil(wr, &ds->front);
    venus_write_stencil(wr, &ds->back);
    vcw_wr_float(wr, ds->minDepthBounds);
    vcw_wr_float(wr, ds->maxDepthBounds);
}

static void venus_write_color_blend(struct venus_cmd_writer *wr,
                                    const VkPipelineColorBlendStateCreateInfo *cb) {
    vcw_wr_u64(wr, 1);
    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u32(wr, cb->flags);
    vcw_wr_u32(wr, cb->logicOpEnable);
    vcw_wr_i32(wr, (int32_t)cb->logicOp);
    vcw_wr_u32(wr, cb->attachmentCount);
    vcw_wr_array_size(wr, cb->attachmentCount);
    for (uint32_t i = 0; i < cb->attachmentCount; i++) {
        const VkPipelineColorBlendAttachmentState *a = &cb->pAttachments[i];
        vcw_wr_u32(wr, a->blendEnable);
        vcw_wr_i32(wr, (int32_t)a->srcColorBlendFactor);
        vcw_wr_i32(wr, (int32_t)a->dstColorBlendFactor);
        vcw_wr_i32(wr, (int32_t)a->colorBlendOp);
        vcw_wr_i32(wr, (int32_t)a->srcAlphaBlendFactor);
        vcw_wr_i32(wr, (int32_t)a->dstAlphaBlendFactor);
        vcw_wr_i32(wr, (int32_t)a->alphaBlendOp);
        vcw_wr_u32(wr, a->colorWriteMask);
    }
    vcw_wr_array_size(wr, 4);
    for (uint32_t i = 0; i < 4; i++)
        vcw_wr_float(wr, cb->blendConstants[i]);
}

static void venus_write_dynamic_state(struct venus_cmd_writer *wr,
                                      const VkPipelineDynamicStateCreateInfo *ds) {
    if (!ds) {
        vcw_wr_u64(wr, 0);
        return;
    }
    vcw_wr_u64(wr, 1);
    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u32(wr, ds->flags);
    vcw_wr_u32(wr, ds->dynamicStateCount);
    vcw_wr_array_size(wr, ds->dynamicStateCount);
    for (uint32_t i = 0; i < ds->dynamicStateCount; i++)
        vcw_wr_i32(wr, (int32_t)ds->pDynamicStates[i]);
}

static void venus_write_rendering_pnext(struct venus_cmd_writer *wr,
                                        const VkPipelineRenderingCreateInfo *ri) {
    if (!ri) {
        vcw_wr_u64(wr, 0);
        return;
    }
    vcw_wr_u64(wr, 1);
    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u32(wr, ri->viewMask);
    vcw_wr_u32(wr, ri->colorAttachmentCount);
    if (ri->pColorAttachmentFormats) {
        vcw_wr_array_size(wr, ri->colorAttachmentCount);
        for (uint32_t i = 0; i < ri->colorAttachmentCount; i++)
            vcw_wr_i32(wr, (int32_t)ri->pColorAttachmentFormats[i]);
    } else {
        vcw_wr_array_size(wr, 0);
    }
    vcw_wr_i32(wr, (int32_t)ri->depthAttachmentFormat);
    vcw_wr_i32(wr, (int32_t)ri->stencilAttachmentFormat);
}

int venus_cmd_encode_CreateGraphicsPipelines(
        struct venus_wire *w, uint64_t dev_id,
        uint64_t pipeline_cache_id,
        uint32_t create_info_count,
        const VkGraphicsPipelineCreateInfo *pCreateInfo,
        uint64_t shader_vs_id, uint64_t shader_fs_id,
        uint64_t layout_id, uint64_t rp_id,
        uint64_t *out_pipeline_id) {
    if (!w || !pCreateInfo || !out_pipeline_id) return -22;
    if (create_info_count != 1) return VK_ERROR_INITIALIZATION_FAILED;
    if (!shader_vs_id || !shader_fs_id || !layout_id)
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkPipelineVertexInputStateCreateInfo *vi = pCreateInfo->pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo *ia = pCreateInfo->pInputAssemblyState;
    const VkPipelineViewportStateCreateInfo *vp = pCreateInfo->pViewportState;
    const VkPipelineRasterizationStateCreateInfo *rs = pCreateInfo->pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo *ms = pCreateInfo->pMultisampleState;
    const VkPipelineColorBlendStateCreateInfo *cb = pCreateInfo->pColorBlendState;
    const VkPipelineRenderingCreateInfo *ri =
        venus_find_pipeline_rendering(pCreateInfo->pNext);

    if (!vi || !ia || !vp || !rs || !ms || !cb || !pCreateInfo->pStages)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (vi->vertexBindingDescriptionCount && !vi->pVertexBindingDescriptions)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (vi->vertexAttributeDescriptionCount && !vi->pVertexAttributeDescriptions)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (cb->attachmentCount && !cb->pAttachments)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->stageCount > 8u ||
        vi->vertexBindingDescriptionCount > 32u ||
        vi->vertexAttributeDescriptionCount > 64u ||
        vp->viewportCount > 16u || vp->scissorCount > 16u ||
        cb->attachmentCount > 16u)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (ri && ri->colorAttachmentCount && !ri->pColorAttachmentFormats)
        return VK_ERROR_INITIALIZATION_FAILED;

    extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);

    uint8_t *cmd = malloc(VENUS_PIPELINE_CMD_CAP);
    if (!cmd) return -12;

    uint8_t reply[24];
    struct venus_cmd_writer wr = { cmd, 0, VENUS_PIPELINE_CMD_CAP, 0 };
    uint64_t pipeline_id = venus_wire_alloc_object_id(w);
    if (!pipeline_id) {
        free(cmd);
        return -12;
    }

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCreateGraphicsPipelines);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, pipeline_cache_id);
    vcw_wr_u32(&wr, create_info_count);
    vcw_wr_array_size(&wr, create_info_count);

    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    venus_write_rendering_pnext(&wr, ri);
    vcw_wr_u32(&wr, pCreateInfo->flags);
    vcw_wr_u32(&wr, pCreateInfo->stageCount);
    vcw_wr_array_size(&wr, pCreateInfo->stageCount);
    for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
        const VkPipelineShaderStageCreateInfo *st = &pCreateInfo->pStages[i];
        uint64_t shader_id =
            (st->stage == VK_SHADER_STAGE_VERTEX_BIT) ? shader_vs_id :
            (st->stage == VK_SHADER_STAGE_FRAGMENT_BIT) ? shader_fs_id : 0;
        if (!shader_id) {
            free(cmd);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        venus_write_shader_stage(&wr, st, shader_id);
    }

    venus_write_vertex_input(&wr, vi);
    venus_write_input_assembly(&wr, ia);
    vcw_wr_u64(&wr, 0); /* pTessellationState */
    venus_write_viewport_state(&wr, vp);
    venus_write_rasterization(&wr, rs);
    venus_write_multisample(&wr, ms);
    venus_write_depth_stencil(&wr, pCreateInfo->pDepthStencilState);
    venus_write_color_blend(&wr, cb);
    venus_write_dynamic_state(&wr, pCreateInfo->pDynamicState);
    vcw_wr_u64(&wr, layout_id);
    vcw_wr_u64(&wr, rp_id);
    vcw_wr_u32(&wr, pCreateInfo->subpass);
    vcw_wr_u64(&wr, 0); /* basePipelineHandle */
    vcw_wr_i32(&wr, pCreateInfo->basePipelineIndex);

    vcw_wr_u64(&wr, 0); /* pAllocator */
    vcw_wr_array_size(&wr, 1);
    vcw_wr_u64(&wr, pipeline_id);
    if (wr.err) {
        free(cmd);
        return wr.err;
    }

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    uint32_t bytes = wr.off;
    free(cmd);
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t count = *(uint64_t *)(reply + 8);
    uint64_t pipeline_reply = *(uint64_t *)(reply + 16);
    static uint32_t log_count;
    if (log_count < 32u) {
        log_count++;
        printf("[VPIPE] raw reply cmd=%u vk=%u count=%llu pipe=%llu guest=%llu bytes=%u stages=%u layout=%llu rp=%llu dyn=%u\n",
               reply_cmd, vk_result, (unsigned long long)count,
               (unsigned long long)pipeline_reply,
               (unsigned long long)pipeline_id, bytes,
               pCreateInfo->stageCount,
               (unsigned long long)layout_id,
               (unsigned long long)rp_id,
               ri ? 1u : 0u);
    }
    if (reply_cmd != VN_CMD_TYPE_vkCreateGraphicsPipelines)
        return -5;
    if (vk_result != VK_SUCCESS)
        return (int)vk_result;
    if (count != 1 || !pipeline_reply)
        return -5;

    *out_pipeline_id = pipeline_id;
    return VK_SUCCESS;
}
