#include "venus_real.h"

extern void *malloc(unsigned long);
extern void free(void *);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern unsigned long strlen(const char *);
extern int strcmp(const char *, const char *);
extern int printf(const char *, ...);

#define VN_CMD_CREATE_DEVICE 11u
#define VN_CMD_DESTROY_DEVICE 12u
#define VN_CMD_GET_DEVICE_QUEUE_2 155u
#define VN_CMD_GET_CALIBRATED_TIMESTAMPS_EXT 236u
#define VN_COMMAND_GENERATE_REPLY 1u
#define VN_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA 1000384005u

struct device_encoder {
    uint8_t *data;
    uint32_t capacity;
    uint32_t length;
    int failed;
};

static void encode_bytes(struct device_encoder *enc, const void *data,
                         uint32_t size)
{
    if (enc->length > enc->capacity || size > enc->capacity - enc->length) {
        enc->failed = 1;
        return;
    }
    if (enc->data && size)
        memcpy(enc->data + enc->length, data, size);
    enc->length += size;
}

static void encode_u32(struct device_encoder *enc, uint32_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static void encode_u64(struct device_encoder *enc, uint64_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static int encode_names(struct device_encoder *enc, uint32_t count,
                        const char *const *names)
{
    if (count && !names)
        return -1;
    encode_u64(enc, names ? count : 0);
    for (uint32_t i = 0; i < count; i++) {
        if (!names[i])
            return -1;
        uint64_t length = strlen(names[i]) + 1;
        encode_u64(enc, length);
        encode_bytes(enc, names[i], (uint32_t)length);
        uint32_t padded = ((uint32_t)length + 3u) & ~3u;
        if (padded != length) {
            static const uint32_t zero;
            encode_bytes(enc, &zero, padded - (uint32_t)length);
        }
    }
    return enc->failed ? -1 : 0;
}

static int encode_device_extension_names(
    struct device_encoder *enc, uint32_t count, const char *const *names)
{
    if (count && !names)
        return -1;
    uint32_t host_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!names[i])
            return -1;
        if (strcmp(names[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) != 0)
            host_count++;
    }
    encode_u32(enc, host_count);
    encode_u64(enc, host_count);
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(names[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
            continue;
        uint64_t length = strlen(names[i]) + 1;
        encode_u64(enc, length);
        encode_bytes(enc, names[i], (uint32_t)length);
        uint32_t padded = ((uint32_t)length + 3u) & ~3u;
        if (padded != length) {
            static const uint32_t zero;
            encode_bytes(enc, &zero, padded - (uint32_t)length);
        }
    }
    return enc->failed ? -1 : 0;
}

#define FEATURE_RANGE(type, value, first, last) do { \
    const type *typed = (const type *)(value); \
    *body = &typed->first; \
    *body_size = (uint32_t)((const uint8_t *)&typed->last + \
                            sizeof(typed->last) - \
                            (const uint8_t *)&typed->first); \
} while (0)

static int get_feature_body(const VkBaseInStructure *feature,
                            const void **body, uint32_t *body_size)
{
    switch (feature->sType) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
        FEATURE_RANGE(VkPhysicalDeviceFeatures2, feature,
                      features, features);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
        FEATURE_RANGE(VkPhysicalDeviceVulkan11Features, feature,
                      storageBuffer16BitAccess, shaderDrawParameters);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        FEATURE_RANGE(VkPhysicalDeviceVulkan12Features, feature,
                      samplerMirrorClampToEdge, subgroupBroadcastDynamicId);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
        FEATURE_RANGE(VkPhysicalDeviceVulkan13Features, feature,
                      robustImageAccess, maintenance4);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT,
                      feature, attachmentFeedbackLoopLayout,
                      attachmentFeedbackLoopLayout);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT:
        FEATURE_RANGE(
            VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT,
            feature, attachmentFeedbackLoopDynamicState,
            attachmentFeedbackLoopDynamicState);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDevice4444FormatsFeaturesEXT, feature,
                      formatA4R4G4B4, formatA4B4G4R4);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceBorderColorSwizzleFeaturesEXT, feature,
                      borderColorSwizzle, borderColorSwizzleFromImage);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceColorWriteEnableFeaturesEXT, feature,
                      colorWriteEnable, colorWriteEnable);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceCustomBorderColorFeaturesEXT, feature,
                      customBorderColors, customBorderColorWithoutFormat);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceDepthClipEnableFeaturesEXT, feature,
                      depthClipEnable, depthClipEnable);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceDepthClipControlFeaturesEXT, feature,
                      depthClipControl, depthClipControl);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceDepthBiasControlFeaturesEXT, feature,
                      depthBiasControl, depthBiasExact);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, feature,
                      extendedDynamicState3TessellationDomainOrigin,
                      extendedDynamicState3ShadingRateImageEnable);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT,
                      feature, fragmentShaderSampleInterlock,
                      fragmentShaderShadingRateInterlock);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES:
        FEATURE_RANGE(VkPhysicalDeviceDynamicRenderingLocalReadFeatures,
                      feature, dynamicRenderingLocalRead,
                      dynamicRenderingLocalRead);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT:
        FEATURE_RANGE(
            VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT,
            feature, dynamicRenderingUnusedAttachments,
            dynamicRenderingUnusedAttachments);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT,
                      feature, graphicsPipelineLibrary,
                      graphicsPipelineLibrary);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES:
        FEATURE_RANGE(VkPhysicalDeviceHostImageCopyFeatures, feature,
                      hostImageCopy, hostImageCopy);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT, feature,
                      image2DViewOf3D, sampler2DViewOf3D);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES:
        FEATURE_RANGE(VkPhysicalDeviceLineRasterizationFeatures, feature,
                      rectangularLines, stippledSmoothLines);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceMemoryPriorityFeaturesEXT, feature,
                      memoryPriority, memoryPriority);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES:
        FEATURE_RANGE(VkPhysicalDeviceMaintenance6Features, feature,
                      maintenance6, maintenance6);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR:
        FEATURE_RANGE(VkPhysicalDeviceMaintenance7FeaturesKHR, feature,
                      maintenance7, maintenance7);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_FEATURES_EXT:
        FEATURE_RANGE(
            VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT,
            feature, multisampledRenderToSingleSampled,
            multisampledRenderToSingleSampled);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT, feature,
                      nonSeamlessCubeMap, nonSeamlessCubeMap);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT,
                      feature, primitivesGeneratedQuery,
                      primitivesGeneratedQueryWithNonZeroStreams);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceRobustness2FeaturesEXT, feature,
                      robustBufferAccess2, nullDescriptor);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT,
                      feature, shaderModuleIdentifier, shaderModuleIdentifier);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT, feature,
                      shaderBufferFloat32Atomics,
                      sparseImageFloat32AtomicAdd);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT,
                      feature, swapchainMaintenance1, swapchainMaintenance1);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceTransformFeedbackFeaturesEXT, feature,
                      transformFeedback, geometryStreams);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT,
                      feature, vertexAttributeInstanceRateDivisor,
                      vertexAttributeInstanceRateZeroDivisor);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_FEATURES_EXT:
        FEATURE_RANGE(VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT,
                      feature, legacyVertexAttributes,
                      legacyVertexAttributes);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR:
        FEATURE_RANGE(
            VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR,
            feature, workgroupMemoryExplicitLayout,
            workgroupMemoryExplicitLayout16BitAccess);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES:
        FEATURE_RANGE(VkPhysicalDeviceMaintenance5Features, feature,
                      maintenance5, maintenance5);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR:
        FEATURE_RANGE(VkPhysicalDevicePresentIdFeaturesKHR, feature,
                      presentId, presentId);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR:
        FEATURE_RANGE(VkPhysicalDevicePresentWaitFeaturesKHR, feature,
                      presentWait, presentWait);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_POOL_OVERALLOCATION_FEATURES_NV:
        FEATURE_RANGE(VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV,
                      feature, descriptorPoolOverallocation,
                      descriptorPoolOverallocation);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAW_ACCESS_CHAINS_FEATURES_NV:
        FEATURE_RANGE(VkPhysicalDeviceRawAccessChainsFeaturesNV, feature,
                      shaderRawAccessChains, shaderRawAccessChains);
        break;
    default:
        printf("[VENUS] create device: unsupported feature sType=%u\n",
               (uint32_t)feature->sType);
        return -1;
    }
    return 0;
}

static int encode_feature_chain(struct device_encoder *enc,
                                const void *chain)
{
    if (!chain) {
        encode_u64(enc, 0);
        return enc->failed ? -1 : 0;
    }
    const VkBaseInStructure *feature =
        (const VkBaseInStructure *)chain;
    const void *body = 0;
    uint32_t body_size = 0;
    if (get_feature_body(feature, &body, &body_size) < 0)
        return -1;
    encode_u64(enc, 1);
    encode_u32(enc, (uint32_t)feature->sType);
    if (encode_feature_chain(enc, feature->pNext) < 0)
        return -1;
    encode_bytes(enc, body, body_size);
    return enc->failed ? -1 : 0;
}

#undef FEATURE_RANGE

static int encode_device_create(struct device_encoder *enc,
                                struct venus_physical_device_real *physical,
                                const VkDeviceCreateInfo *info,
                                uint64_t device_id)
{
    if (!info || info->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO ||
        !info->queueCreateInfoCount ||
        !info->pQueueCreateInfos)
        return -1;
    encode_u32(enc, VN_CMD_CREATE_DEVICE);
    encode_u32(enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(enc, physical->object_id);
    encode_u64(enc, 1); /* pCreateInfo */
    encode_u32(enc, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    if (encode_feature_chain(enc, info->pNext) < 0)
        return -1;
    encode_u32(enc, info->flags);
    encode_u32(enc, info->queueCreateInfoCount);
    encode_u64(enc, info->queueCreateInfoCount);
    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
        const VkDeviceQueueCreateInfo *queue = &info->pQueueCreateInfos[i];
        if (queue->sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO ||
            queue->pNext || !queue->queueCount || !queue->pQueuePriorities) {
            printf("[VENUS] create device: invalid queue[%u] sType=%u pNext=%p count=%u priorities=%p\n",
                   i, (uint32_t)queue->sType, queue->pNext,
                   queue->queueCount, queue->pQueuePriorities);
            return -1;
        }
        encode_u32(enc, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        encode_u64(enc, 0); /* pNext */
        encode_u32(enc, queue->flags);
        encode_u32(enc, queue->queueFamilyIndex);
        encode_u32(enc, queue->queueCount);
        encode_u64(enc, queue->queueCount);
        encode_bytes(enc, queue->pQueuePriorities,
                     queue->queueCount * sizeof(float));
    }
    encode_u32(enc, info->enabledLayerCount);
    if (encode_names(enc, info->enabledLayerCount,
                     info->ppEnabledLayerNames) < 0)
        return -1;
    if (encode_device_extension_names(enc, info->enabledExtensionCount,
                                      info->ppEnabledExtensionNames) < 0)
        return -1;
    encode_u64(enc, info->pEnabledFeatures ? 1 : 0);
    if (info->pEnabledFeatures)
        encode_bytes(enc, info->pEnabledFeatures,
                     sizeof(*info->pEnabledFeatures));
    encode_u64(enc, 0); /* pAllocator */
    encode_u64(enc, 1); /* pDevice */
    encode_u64(enc, device_id);
    return enc->failed ? -1 : 0;
}

static uint32_t acquire_ring_index(struct venus_instance_real *instance)
{
    for (uint32_t index = 1; index < 32; index++) {
        uint32_t bit = 1u << index;
        if (!(instance->queue_ring_mask & bit)) {
            instance->queue_ring_mask |= bit;
            return index;
        }
    }
    return 0;
}

static void release_ring_index(struct venus_instance_real *instance,
                               uint32_t index)
{
    if (index && index < 32)
        instance->queue_ring_mask &= ~(1u << index);
}

static int init_host_queue(struct venus_queue_real *queue)
{
    uint8_t command[80];
    struct device_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_GET_DEVICE_QUEUE_2);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, queue->device->object_id);
    encode_u64(&enc, 1); /* pQueueInfo */
    encode_u32(&enc, VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2);
    encode_u64(&enc, 1); /* pNext */
    encode_u32(&enc, VN_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA);
    encode_u64(&enc, 0); /* timeline pNext */
    encode_u32(&enc, queue->ring_index);
    encode_u32(&enc, 0); /* flags */
    encode_u32(&enc, queue->family_index);
    encode_u32(&enc, queue->queue_index);
    encode_u64(&enc, 1); /* pQueue */
    encode_u64(&enc, queue->object_id);
    if (enc.failed || enc.length != sizeof(command))
        return -1;

    uint8_t reply[20];
    memset(reply, 0, sizeof(reply));
    struct venus_instance_real *instance =
        queue->device->physical_device->instance;
    if (venus_wire_call(instance->wire, command, sizeof(command), reply,
                        sizeof(reply)) < 0)
        return -1;
    uint32_t command_type;
    uint64_t present;
    uint64_t object_id;
    memcpy(&command_type, reply, 4);
    memcpy(&present, reply + 4, 8);
    memcpy(&object_id, reply + 12, 8);
    return command_type == VN_CMD_GET_DEVICE_QUEUE_2 && present &&
           object_id == queue->object_id ? 0 : -1;
}

static void destroy_host_device(struct venus_device_real *device)
{
    uint8_t command[24];
    struct device_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_DESTROY_DEVICE);
    encode_u32(&enc, 0);
    encode_u64(&enc, device->object_id);
    encode_u64(&enc, 0); /* pAllocator */
    struct venus_instance_real *instance =
        device->physical_device->instance;
    (void)venus_wire_submit_async(instance->wire, command, sizeof(command));
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateDevice(VkPhysicalDevice physical_device,
                        const VkDeviceCreateInfo *create_info,
                        const VkAllocationCallbacks *allocator,
                        VkDevice *device)
{
    if (!physical_device || !create_info || !device || allocator ||
        !create_info->pQueueCreateInfos)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t queue_count = 0;
    for (uint32_t i = 0; i < create_info->queueCreateInfoCount; i++) {
        if (UINT32_MAX - queue_count <
            create_info->pQueueCreateInfos[i].queueCount)
            return VK_ERROR_INITIALIZATION_FAILED;
        queue_count += create_info->pQueueCreateInfos[i].queueCount;
    }
    if (!queue_count)
        return VK_ERROR_INITIALIZATION_FAILED;

    struct venus_device_real *self = malloc(sizeof(*self));
    if (!self)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(self, 0, sizeof(*self));
    set_loader_magic_value(self);
    self->object_id = (uint64_t)(uintptr_t)self;
    self->physical_device =
        (struct venus_physical_device_real *)physical_device;
    self->queues = malloc((unsigned long)queue_count * sizeof(*self->queues));
    if (!self->queues) {
        free(self);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(self->queues, 0,
           (unsigned long)queue_count * sizeof(*self->queues));
    self->queue_count = queue_count;

    struct venus_instance_real *instance = self->physical_device->instance;
    uint32_t queue_pos = 0;
    for (uint32_t i = 0; i < create_info->queueCreateInfoCount; i++) {
        const VkDeviceQueueCreateInfo *info =
            &create_info->pQueueCreateInfos[i];
        for (uint32_t j = 0; j < info->queueCount; j++) {
            struct venus_queue_real *queue = &self->queues[queue_pos++];
            set_loader_magic_value(queue);
            queue->object_id = (uint64_t)(uintptr_t)queue;
            queue->device = self;
            queue->family_index = info->queueFamilyIndex;
            queue->queue_index = j;
            queue->ring_index = acquire_ring_index(instance);
            if (!queue->ring_index)
                goto fail_guest;
        }
    }

    struct device_encoder size_enc = { .capacity = UINT32_MAX };
    if (encode_device_create(&size_enc, self->physical_device, create_info,
                             self->object_id) < 0 || !size_enc.length) {
        printf("[VENUS] create device: sizing encode failed length=%u\n",
               size_enc.length);
        goto fail_guest;
    }
    uint8_t *command = malloc(size_enc.length);
    if (!command)
        goto fail_guest;
    struct device_encoder enc = {
        .data = command,
        .capacity = size_enc.length,
    };
    if (encode_device_create(&enc, self->physical_device, create_info,
                             self->object_id) < 0) {
        printf("[VENUS] create device: final encode failed length=%u capacity=%u\n",
               enc.length, enc.capacity);
        free(command);
        goto fail_guest;
    }
    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = venus_wire_call(instance->wire, command, enc.length,
                                      reply, sizeof(reply));
    free(command);
    if (wire_result < 0) {
        printf("[VENUS] create device: wire call failed rc=%d\n",
               wire_result);
        goto fail_guest;
    }
    uint32_t command_type;
    int32_t result;
    uint64_t present;
    uint64_t returned_id;
    memcpy(&command_type, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&present, reply + 8, 8);
    memcpy(&returned_id, reply + 16, 8);
    if (command_type != VN_CMD_CREATE_DEVICE) {
        printf("[VENUS] create device: bad reply command=%u\n",
               command_type);
        goto fail_guest;
    }
    if (result != VK_SUCCESS) {
        printf("[VENUS] create device: host result=%d\n", result);
        for (uint32_t i = 0; i < self->queue_count; i++)
            release_ring_index(instance, self->queues[i].ring_index);
        free(self->queues);
        free(self);
        return (VkResult)result;
    }
    if (!present || returned_id != self->object_id) {
        printf("[VENUS] create device: bad handle present=%llu returned=%p expected=%p\n",
               (unsigned long long)present, (void *)(uintptr_t)returned_id,
               (void *)(uintptr_t)self->object_id);
        destroy_host_device(self);
        goto fail_guest;
    }

    for (uint32_t i = 0; i < self->queue_count; i++) {
        if (init_host_queue(&self->queues[i]) < 0) {
            printf("[VENUS] create device: queue init failed index=%u family=%u queue=%u ring=%u\n",
                   i, self->queues[i].family_index,
                   self->queues[i].queue_index,
                   self->queues[i].ring_index);
            destroy_host_device(self);
            goto fail_guest;
        }
    }
    *device = (VkDevice)self;
    return VK_SUCCESS;

fail_guest:
    for (uint32_t i = 0; i < self->queue_count; i++)
        release_ring_index(instance, self->queues[i].ring_index);
    free(self->queues);
    free(self);
    return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyDevice(VkDevice device,
                         const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device)
        return;
    struct venus_device_real *self = (struct venus_device_real *)device;
    struct venus_instance_real *instance = self->physical_device->instance;
    for (uint32_t i = 0; i < self->queue_count; i++) {
        if (self->queues[i].wait_fence)
            venus_real_DestroyFence(
                device, self->queues[i].wait_fence, 0);
    }
    destroy_host_device(self);
    for (uint32_t i = 0; i < self->queue_count; i++)
        release_ring_index(instance, self->queues[i].ring_index);
    free(self->queues);
    free(self);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_GetDeviceQueue(VkDevice device, uint32_t queue_family_index,
                          uint32_t queue_index, VkQueue *queue)
{
    if (!device || !queue)
        return;
    *queue = VK_NULL_HANDLE;
    struct venus_device_real *self = (struct venus_device_real *)device;
    for (uint32_t i = 0; i < self->queue_count; i++) {
        struct venus_queue_real *candidate = &self->queues[i];
        if (candidate->family_index == queue_family_index &&
            candidate->queue_index == queue_index) {
            *queue = (VkQueue)candidate;
            return;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_DeviceWaitIdle(VkDevice device)
{
    if (!device)
        return VK_ERROR_DEVICE_LOST;
    struct venus_device_real *self = (struct venus_device_real *)device;
    for (uint32_t i = 0; i < self->queue_count; i++) {
        const VkFenceCreateInfo create_info = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };
        VkFence fence = VK_NULL_HANDLE;
        VkResult result = venus_real_CreateFence(
            device, &create_info, 0, &fence);
        if (result == VK_SUCCESS)
            result = venus_real_QueueSubmit2(
                (VkQueue)&self->queues[i], 0, 0, fence);
        if (result == VK_SUCCESS)
            result = venus_real_WaitForFences(
                device, 1, &fence, VK_TRUE, UINT64_MAX);
        if (fence)
            venus_real_DestroyFence(device, fence, 0);
        if (result != VK_SUCCESS) {
            printf("[VN] DeviceWaitIdle queue %u failed %d\n", i, result);
            return result;
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_GetCalibratedTimestampsEXT(
    VkDevice device, uint32_t timestamp_count,
    const VkCalibratedTimestampInfoKHR *timestamp_infos,
    uint64_t *timestamps, uint64_t *max_deviation)
{
    if (!device || (timestamp_count && !timestamp_infos))
        return VK_ERROR_INITIALIZATION_FAILED;

    uint64_t command_size_64 = 44u + (uint64_t)timestamp_count * 16u;
    uint64_t reply_size_64 = 24u +
        (timestamps ? (uint64_t)timestamp_count * 8u : 0u) +
        (max_deviation ? 8u : 0u);
    if (command_size_64 > UINT32_MAX || reply_size_64 > 65536u)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    uint32_t command_size = (uint32_t)command_size_64;
    uint32_t reply_size = (uint32_t)reply_size_64;
    uint8_t *command = malloc(command_size);
    uint8_t *reply = malloc(reply_size);
    if (!command || !reply) {
        free(command);
        free(reply);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    struct venus_device_real *self = (struct venus_device_real *)device;
    struct device_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    encode_u32(&enc, VN_CMD_GET_CALIBRATED_TIMESTAMPS_EXT);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, timestamp_count);
    encode_u64(&enc, timestamp_infos ? timestamp_count : 0);
    for (uint32_t i = 0; i < timestamp_count; i++) {
        encode_u32(&enc,
                   VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT);
        encode_u64(&enc, 0); /* no supported pNext structures */
        encode_u32(&enc, (uint32_t)timestamp_infos[i].timeDomain);
    }
    encode_u64(&enc, timestamps ? timestamp_count : 0);
    encode_u64(&enc, max_deviation ? 1 : 0);
    if (enc.failed || enc.length != command_size) {
        free(command);
        free(reply);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    memset(reply, 0, reply_size);
    struct venus_instance_real *instance = self->physical_device->instance;
    int wire_result = venus_wire_call(instance->wire, command, command_size,
                                      reply, reply_size);
    free(command);
    if (wire_result < 0) {
        free(reply);
        return VK_ERROR_DEVICE_LOST;
    }

    uint32_t offset = 0;
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t array_size = 0;
    uint64_t max_deviation_present = 0;
    memcpy(&returned_command, reply + offset, 4);
    offset += 4;
    memcpy(&result, reply + offset, 4);
    offset += 4;
    memcpy(&array_size, reply + offset, 8);
    offset += 8;
    if (returned_command != VN_CMD_GET_CALIBRATED_TIMESTAMPS_EXT ||
        array_size != (timestamps ? timestamp_count : 0)) {
        free(reply);
        return VK_ERROR_DEVICE_LOST;
    }
    if (array_size) {
        memcpy(timestamps, reply + offset,
               (unsigned long)array_size * sizeof(*timestamps));
        offset += (uint32_t)array_size * sizeof(*timestamps);
    }
    memcpy(&max_deviation_present, reply + offset, 8);
    offset += 8;
    if (!!max_deviation_present != !!max_deviation) {
        free(reply);
        return VK_ERROR_DEVICE_LOST;
    }
    if (max_deviation_present)
        memcpy(max_deviation, reply + offset, sizeof(*max_deviation));
    free(reply);
    return (VkResult)result;
}
