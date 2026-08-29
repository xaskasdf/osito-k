#include "venus_real.h"

extern void *malloc(unsigned long);
extern void free(void *);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern unsigned long strlen(const char *);
extern int printf(const char *, ...);

#define VN_CMD_CREATE_GRAPHICS_PIPELINES 65u
#define VN_CMD_CREATE_COMPUTE_PIPELINES 66u
#define VN_CMD_DESTROY_PIPELINE 67u
#define VN_COMMAND_GENERATE_REPLY 1u
#define PIPELINE_COMMAND_CAPACITY (1u << 20)

struct pipeline_encoder {
    uint8_t *data;
    uint32_t capacity;
    uint32_t offset;
    int failed;
};

static void encode_bytes(struct pipeline_encoder *enc,
                         const void *value, uint32_t size)
{
    if (enc->failed || size > enc->capacity - enc->offset) {
        enc->failed = 1;
        return;
    }
    memcpy(enc->data + enc->offset, value, size);
    enc->offset += size;
}

static void encode_u32(struct pipeline_encoder *enc, uint32_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static void encode_u64(struct pipeline_encoder *enc, uint64_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static void encode_float(struct pipeline_encoder *enc, float value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static void encode_blob(struct pipeline_encoder *enc,
                        const void *value, uint32_t size)
{
    static const uint8_t zero[3] = { 0, 0, 0 };
    encode_bytes(enc, value, size);
    uint32_t padding = (4u - (size & 3u)) & 3u;
    if (padding)
        encode_bytes(enc, zero, padding);
}

static void encode_specialization(struct pipeline_encoder *enc,
                                  const VkSpecializationInfo *info)
{
    encode_u32(enc, info->mapEntryCount);
    encode_u64(enc, info->pMapEntries ? info->mapEntryCount : 0u);
    if (info->pMapEntries) {
        for (uint32_t i = 0; i < info->mapEntryCount; i++) {
            encode_u32(enc, info->pMapEntries[i].constantID);
            encode_u32(enc, info->pMapEntries[i].offset);
            encode_u64(enc, info->pMapEntries[i].size);
        }
    }
    encode_u64(enc, info->dataSize);
    encode_u64(enc, info->pData ? info->dataSize : 0u);
    if (info->pData && info->dataSize <= UINT32_MAX)
        encode_blob(enc, info->pData, (uint32_t)info->dataSize);
    else if (info->pData)
        enc->failed = 1;
}

static void encode_shader_stage(struct pipeline_encoder *enc,
                                const VkPipelineShaderStageCreateInfo *info)
{
    encode_u32(enc, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    encode_u64(enc, 0);
    encode_u32(enc, info->flags);
    encode_u32(enc, (uint32_t)info->stage);
    encode_u64(enc, (uint64_t)info->module);
    if (info->pName) {
        size_t length = strlen(info->pName) + 1u;
        encode_u64(enc, length);
        if (length <= UINT32_MAX)
            encode_blob(enc, info->pName, (uint32_t)length);
        else
            enc->failed = 1;
    } else {
        encode_u64(enc, 0);
    }
    encode_u64(enc, info->pSpecializationInfo ? 1u : 0u);
    if (info->pSpecializationInfo)
        encode_specialization(enc, info->pSpecializationInfo);
}

static void encode_vertex_input(
    struct pipeline_encoder *enc,
    const VkPipelineVertexInputStateCreateInfo *info)
{
    encode_u32(enc, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    encode_u64(enc, 0);
    encode_u32(enc, info->flags);
    encode_u32(enc, info->vertexBindingDescriptionCount);
    encode_u64(enc, info->pVertexBindingDescriptions
                    ? info->vertexBindingDescriptionCount : 0u);
    if (info->pVertexBindingDescriptions) {
        for (uint32_t i = 0; i < info->vertexBindingDescriptionCount; i++) {
            const VkVertexInputBindingDescription *binding =
                &info->pVertexBindingDescriptions[i];
            encode_u32(enc, binding->binding);
            encode_u32(enc, binding->stride);
            encode_u32(enc, (uint32_t)binding->inputRate);
        }
    }
    encode_u32(enc, info->vertexAttributeDescriptionCount);
    encode_u64(enc, info->pVertexAttributeDescriptions
                    ? info->vertexAttributeDescriptionCount : 0u);
    if (info->pVertexAttributeDescriptions) {
        for (uint32_t i = 0; i < info->vertexAttributeDescriptionCount; i++) {
            const VkVertexInputAttributeDescription *attribute =
                &info->pVertexAttributeDescriptions[i];
            encode_u32(enc, attribute->location);
            encode_u32(enc, attribute->binding);
            encode_u32(enc, (uint32_t)attribute->format);
            encode_u32(enc, attribute->offset);
        }
    }
}

static void encode_input_assembly(
    struct pipeline_encoder *enc,
    const VkPipelineInputAssemblyStateCreateInfo *info)
{
    encode_u32(enc,
               VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    encode_u64(enc, 0);
    encode_u32(enc, info->flags);
    encode_u32(enc, (uint32_t)info->topology);
    encode_u32(enc, info->primitiveRestartEnable);
}

static void encode_viewport_state(
    struct pipeline_encoder *enc,
    const VkPipelineViewportStateCreateInfo *info)
{
    encode_u32(enc, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    encode_u64(enc, 0);
    encode_u32(enc, info->flags);
    encode_u32(enc, info->viewportCount);
    encode_u64(enc, info->pViewports ? info->viewportCount : 0u);
    if (info->pViewports) {
        for (uint32_t i = 0; i < info->viewportCount; i++) {
            encode_float(enc, info->pViewports[i].x);
            encode_float(enc, info->pViewports[i].y);
            encode_float(enc, info->pViewports[i].width);
            encode_float(enc, info->pViewports[i].height);
            encode_float(enc, info->pViewports[i].minDepth);
            encode_float(enc, info->pViewports[i].maxDepth);
        }
    }
    encode_u32(enc, info->scissorCount);
    encode_u64(enc, info->pScissors ? info->scissorCount : 0u);
    if (info->pScissors) {
        for (uint32_t i = 0; i < info->scissorCount; i++) {
            encode_u32(enc, (uint32_t)info->pScissors[i].offset.x);
            encode_u32(enc, (uint32_t)info->pScissors[i].offset.y);
            encode_u32(enc, info->pScissors[i].extent.width);
            encode_u32(enc, info->pScissors[i].extent.height);
        }
    }
}

static void encode_rasterization(
    struct pipeline_encoder *enc,
    const VkPipelineRasterizationStateCreateInfo *info)
{
    const VkPipelineRasterizationDepthClipStateCreateInfoEXT *depth_clip = 0;
    for (const VkBaseInStructure *next =
             (const VkBaseInStructure *)info->pNext;
         next; next = next->pNext) {
        if (next->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT) {
            enc->failed = 1;
            return;
        }
        depth_clip =
            (const VkPipelineRasterizationDepthClipStateCreateInfoEXT *)next;
    }
    encode_u32(enc, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    if (depth_clip) {
        encode_u64(enc, 1);
        encode_u32(enc,
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT);
        encode_u64(enc, 0);
        encode_u32(enc, depth_clip->flags);
        encode_u32(enc, depth_clip->depthClipEnable);
    } else {
        encode_u64(enc, 0);
    }
    encode_u32(enc, info->flags);
    encode_u32(enc, info->depthClampEnable);
    encode_u32(enc, info->rasterizerDiscardEnable);
    encode_u32(enc, (uint32_t)info->polygonMode);
    encode_u32(enc, info->cullMode);
    encode_u32(enc, (uint32_t)info->frontFace);
    encode_u32(enc, info->depthBiasEnable);
    encode_float(enc, info->depthBiasConstantFactor);
    encode_float(enc, info->depthBiasClamp);
    encode_float(enc, info->depthBiasSlopeFactor);
    encode_float(enc, info->lineWidth);
}

static void encode_multisample(
    struct pipeline_encoder *enc,
    const VkPipelineMultisampleStateCreateInfo *info)
{
    uint32_t mask_count = (info->rasterizationSamples + 31u) / 32u;
    encode_u32(enc, VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    encode_u64(enc, 0);
    encode_u32(enc, info->flags);
    encode_u32(enc, (uint32_t)info->rasterizationSamples);
    encode_u32(enc, info->sampleShadingEnable);
    encode_float(enc, info->minSampleShading);
    encode_u64(enc, info->pSampleMask ? mask_count : 0u);
    if (info->pSampleMask)
        for (uint32_t i = 0; i < mask_count; i++)
            encode_u32(enc, info->pSampleMask[i]);
    encode_u32(enc, info->alphaToCoverageEnable);
    encode_u32(enc, info->alphaToOneEnable);
}

static void encode_stencil_state(struct pipeline_encoder *enc,
                                 const VkStencilOpState *state)
{
    encode_u32(enc, (uint32_t)state->failOp);
    encode_u32(enc, (uint32_t)state->passOp);
    encode_u32(enc, (uint32_t)state->depthFailOp);
    encode_u32(enc, (uint32_t)state->compareOp);
    encode_u32(enc, state->compareMask);
    encode_u32(enc, state->writeMask);
    encode_u32(enc, state->reference);
}

static void encode_depth_stencil(
    struct pipeline_encoder *enc,
    const VkPipelineDepthStencilStateCreateInfo *info)
{
    encode_u32(enc,
               VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
    encode_u64(enc, 0);
    encode_u32(enc, info->flags);
    encode_u32(enc, info->depthTestEnable);
    encode_u32(enc, info->depthWriteEnable);
    encode_u32(enc, (uint32_t)info->depthCompareOp);
    encode_u32(enc, info->depthBoundsTestEnable);
    encode_u32(enc, info->stencilTestEnable);
    encode_stencil_state(enc, &info->front);
    encode_stencil_state(enc, &info->back);
    encode_float(enc, info->minDepthBounds);
    encode_float(enc, info->maxDepthBounds);
}

static void encode_color_blend(
    struct pipeline_encoder *enc,
    const VkPipelineColorBlendStateCreateInfo *info)
{
    encode_u32(enc, VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    encode_u64(enc, 0);
    encode_u32(enc, info->flags);
    encode_u32(enc, info->logicOpEnable);
    encode_u32(enc, (uint32_t)info->logicOp);
    encode_u32(enc, info->attachmentCount);
    encode_u64(enc, info->pAttachments ? info->attachmentCount : 0u);
    if (info->pAttachments) {
        for (uint32_t i = 0; i < info->attachmentCount; i++) {
            const VkPipelineColorBlendAttachmentState *attachment =
                &info->pAttachments[i];
            encode_u32(enc, attachment->blendEnable);
            encode_u32(enc, (uint32_t)attachment->srcColorBlendFactor);
            encode_u32(enc, (uint32_t)attachment->dstColorBlendFactor);
            encode_u32(enc, (uint32_t)attachment->colorBlendOp);
            encode_u32(enc, (uint32_t)attachment->srcAlphaBlendFactor);
            encode_u32(enc, (uint32_t)attachment->dstAlphaBlendFactor);
            encode_u32(enc, (uint32_t)attachment->alphaBlendOp);
            encode_u32(enc, attachment->colorWriteMask);
        }
    }
    encode_u64(enc, 4);
    for (uint32_t i = 0; i < 4; i++)
        encode_float(enc, info->blendConstants[i]);
}

static void encode_dynamic_state(
    struct pipeline_encoder *enc,
    const VkPipelineDynamicStateCreateInfo *info)
{
    encode_u32(enc, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    encode_u64(enc, 0);
    encode_u32(enc, info->flags);
    encode_u32(enc, info->dynamicStateCount);
    encode_u64(enc, info->pDynamicStates ? info->dynamicStateCount : 0u);
    if (info->pDynamicStates)
        for (uint32_t i = 0; i < info->dynamicStateCount; i++)
            encode_u32(enc, (uint32_t)info->pDynamicStates[i]);
}

static void encode_pipeline_rendering(
    struct pipeline_encoder *enc,
    const VkPipelineRenderingCreateInfo *info)
{
    encode_u64(enc, 1);
    encode_u32(enc, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    encode_u64(enc, 0);
    encode_u32(enc, info->viewMask);
    encode_u32(enc, info->colorAttachmentCount);
    encode_u64(enc, info->pColorAttachmentFormats
                    ? info->colorAttachmentCount : 0u);
    if (info->pColorAttachmentFormats)
        for (uint32_t i = 0; i < info->colorAttachmentCount; i++)
            encode_u32(enc, (uint32_t)info->pColorAttachmentFormats[i]);
    encode_u32(enc, (uint32_t)info->depthAttachmentFormat);
    encode_u32(enc, (uint32_t)info->stencilAttachmentFormat);
}

static int encode_graphics_pipeline(
    struct pipeline_encoder *enc,
    const VkGraphicsPipelineCreateInfo *info)
{
    const VkPipelineRenderingCreateInfo *rendering = 0;
    for (const VkBaseInStructure *next =
             (const VkBaseInStructure *)info->pNext;
         next; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            printf("[VN pipeline] unsupported create pNext=%u\n",
                   (uint32_t)next->sType);
            return -1;
        }
        rendering = (const VkPipelineRenderingCreateInfo *)next;
    }
    if (!rendering || !info->stageCount || !info->pStages ||
        info->stageCount > 6 || !info->pVertexInputState ||
        !info->pInputAssemblyState || !info->pViewportState ||
        !info->pRasterizationState || !info->pMultisampleState ||
        !info->pDepthStencilState || !info->pColorBlendState ||
        !info->pDynamicState) {
        printf("[VN pipeline] missing state stages=%u vi=%u ia=%u vp=%u rs=%u ms=%u ds=%u cb=%u dy=%u\n",
               info->stageCount, !!info->pVertexInputState,
               !!info->pInputAssemblyState, !!info->pViewportState,
               !!info->pRasterizationState, !!info->pMultisampleState,
               !!info->pDepthStencilState, !!info->pColorBlendState,
               !!info->pDynamicState);
        return -1;
    }
    for (uint32_t i = 0; i < info->stageCount; i++)
        if (info->pStages[i].pNext) {
            printf("[VN pipeline] unsupported stage pNext=%u\n",
                   (uint32_t)((const VkBaseInStructure *)
                       info->pStages[i].pNext)->sType);
            return -1;
        }
    if (info->pVertexInputState->pNext ||
        info->pInputAssemblyState->pNext ||
        info->pViewportState->pNext ||
        info->pMultisampleState->pNext ||
        info->pDepthStencilState->pNext ||
        info->pColorBlendState->pNext ||
        info->pDynamicState->pNext) {
        printf("[VN pipeline] unsupported state pNext vi=%u ia=%u vp=%u ms=%u ds=%u cb=%u dy=%u\n",
               !!info->pVertexInputState->pNext,
               !!info->pInputAssemblyState->pNext,
               !!info->pViewportState->pNext,
               !!info->pMultisampleState->pNext,
               !!info->pDepthStencilState->pNext,
               !!info->pColorBlendState->pNext,
               !!info->pDynamicState->pNext);
        return -1;
    }
    encode_u32(enc, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    encode_pipeline_rendering(enc, rendering);
    encode_u32(enc, info->flags);
    encode_u32(enc, info->stageCount);
    encode_u64(enc, info->stageCount);
    for (uint32_t i = 0; i < info->stageCount; i++)
        encode_shader_stage(enc, &info->pStages[i]);
    encode_u64(enc, 1);
    encode_vertex_input(enc, info->pVertexInputState);
    encode_u64(enc, 1);
    encode_input_assembly(enc, info->pInputAssemblyState);
    encode_u64(enc, info->pTessellationState ? 1u : 0u);
    if (info->pTessellationState) {
        printf("[VN pipeline] tessellation state not supported\n");
        return -1;
    }
    encode_u64(enc, 1);
    encode_viewport_state(enc, info->pViewportState);
    encode_u64(enc, 1);
    encode_rasterization(enc, info->pRasterizationState);
    encode_u64(enc, 1);
    encode_multisample(enc, info->pMultisampleState);
    encode_u64(enc, 1);
    encode_depth_stencil(enc, info->pDepthStencilState);
    encode_u64(enc, 1);
    encode_color_blend(enc, info->pColorBlendState);
    encode_u64(enc, 1);
    encode_dynamic_state(enc, info->pDynamicState);
    encode_u64(enc, (uint64_t)info->layout);
    encode_u64(enc, (uint64_t)info->renderPass);
    encode_u32(enc, info->subpass);
    encode_u64(enc, (uint64_t)info->basePipelineHandle);
    encode_u32(enc, (uint32_t)info->basePipelineIndex);
    return enc->failed ? -1 : 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipeline_cache,
    uint32_t create_info_count,
    const VkGraphicsPipelineCreateInfo *create_infos,
    const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
    if (!device || pipeline_cache || create_info_count != 1 ||
        !create_infos || allocator || !pipelines)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t *command = malloc(PIPELINE_COMMAND_CAPACITY);
    void *object = malloc(1);
    if (!command || !object) {
        free(command);
        free(object);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    struct pipeline_encoder enc = {
        .data = command,
        .capacity = PIPELINE_COMMAND_CAPACITY,
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_CREATE_GRAPHICS_PIPELINES);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 0); /* pipelineCache */
    encode_u32(&enc, 1);
    encode_u64(&enc, 1);
    if (encode_graphics_pipeline(&enc, &create_infos[0]) < 0) {
        free(command);
        free(object);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    encode_u64(&enc, 0); /* pAllocator */
    encode_u64(&enc, 1);
    encode_u64(&enc, object_id);
    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = enc.failed ? -1 :
        venus_wire_call(self->physical_device->instance->wire,
                        command, enc.offset, reply, sizeof(reply));
    free(command);
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t returned_count = 0;
    uint64_t returned_id = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&returned_count, reply + 8, 8);
    memcpy(&returned_id, reply + 16, 8);
    if (wire_result < 0 ||
        returned_command != VN_CMD_CREATE_GRAPHICS_PIPELINES ||
        (result == VK_SUCCESS &&
         (returned_count != 1 || returned_id != object_id)))
        result = VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        printf("[VN pipeline] host create failed wire=%d reply=%u result=%d count=%llu\n",
               wire_result, returned_command, result,
               (unsigned long long)returned_count);
        free(object);
        return (VkResult)result;
    }
    pipelines[0] = (VkPipeline)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateComputePipelines(
    VkDevice device, VkPipelineCache pipeline_cache,
    uint32_t create_info_count,
    const VkComputePipelineCreateInfo *create_infos,
    const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
    if (!device || pipeline_cache || create_info_count != 1 ||
        !create_infos || allocator || !pipelines ||
        create_infos[0].pNext || create_infos[0].stage.pNext ||
        !create_infos[0].stage.module || !create_infos[0].layout)
        return VK_ERROR_INITIALIZATION_FAILED;

    uint8_t command[512];
    void *object = malloc(1);
    if (!object)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    struct pipeline_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    const VkComputePipelineCreateInfo *info = &create_infos[0];
    encode_u32(&enc, VN_CMD_CREATE_COMPUTE_PIPELINES);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 0); /* pipelineCache */
    encode_u32(&enc, 1);
    encode_u64(&enc, 1);
    encode_u32(&enc, VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    encode_u64(&enc, 0); /* pNext */
    encode_u32(&enc, info->flags);
    encode_shader_stage(&enc, &info->stage);
    encode_u64(&enc, (uint64_t)info->layout);
    encode_u64(&enc, (uint64_t)info->basePipelineHandle);
    encode_u32(&enc, (uint32_t)info->basePipelineIndex);
    encode_u64(&enc, 0); /* pAllocator */
    encode_u64(&enc, 1);
    encode_u64(&enc, object_id);

    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = enc.failed ? -1 :
        venus_wire_call(self->physical_device->instance->wire,
                        command, enc.offset, reply, sizeof(reply));
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t returned_count = 0;
    uint64_t returned_id = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&returned_count, reply + 8, 8);
    memcpy(&returned_id, reply + 16, 8);
    if (wire_result < 0 ||
        returned_command != VN_CMD_CREATE_COMPUTE_PIPELINES ||
        (result == VK_SUCCESS &&
         (returned_count != 1 || returned_id != object_id)))
        result = VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        printf("[VN compute pipeline] host create failed wire=%d reply=%u result=%d count=%llu\n",
               wire_result, returned_command, result,
               (unsigned long long)returned_count);
        free(object);
        return (VkResult)result;
    }
    pipelines[0] = (VkPipeline)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyPipeline(VkDevice device, VkPipeline pipeline,
                           const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !pipeline)
        return;
    uint8_t command[32];
    struct pipeline_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_DESTROY_PIPELINE);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)pipeline);
    encode_u64(&enc, 0);
    if (!enc.failed)
        (void)venus_wire_submit_async(self->physical_device->instance->wire,
                                      command, sizeof(command));
    free((void *)(uintptr_t)pipeline);
}
