#include "venus_real.h"

extern void *malloc(unsigned long);
extern void free(void *);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);

#define VN_CMD_CREATE_DESCRIPTOR_SET_LAYOUT 72u
#define VN_CMD_DESTROY_DESCRIPTOR_SET_LAYOUT 73u
#define VN_CMD_CREATE_PIPELINE_LAYOUT 68u
#define VN_CMD_DESTROY_PIPELINE_LAYOUT 69u
#define VN_CMD_CREATE_DESCRIPTOR_UPDATE_TEMPLATE 158u
#define VN_CMD_DESTROY_DESCRIPTOR_UPDATE_TEMPLATE 159u
#define VN_CMD_CREATE_DESCRIPTOR_POOL 74u
#define VN_CMD_DESTROY_DESCRIPTOR_POOL 75u
#define VN_CMD_RESET_DESCRIPTOR_POOL 76u
#define VN_CMD_ALLOCATE_DESCRIPTOR_SETS 77u
#define VN_CMD_UPDATE_DESCRIPTOR_SETS 79u
#define VN_COMMAND_GENERATE_REPLY 1u

struct descriptor_encoder {
    uint8_t *data;
    uint32_t capacity;
    uint32_t length;
    int failed;
};

static void encode_bytes(struct descriptor_encoder *enc, const void *src,
                         uint32_t size)
{
    if (enc->length > enc->capacity || size > enc->capacity - enc->length) {
        enc->failed = 1;
        return;
    }
    if (enc->data && size)
        memcpy(enc->data + enc->length, src, size);
    enc->length += size;
}

static void encode_u32(struct descriptor_encoder *enc, uint32_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static void encode_u64(struct descriptor_encoder *enc, uint64_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static int encode_set_layout_create(
    struct descriptor_encoder *enc, struct venus_device_real *device,
    const VkDescriptorSetLayoutCreateInfo *info, uint64_t object_id)
{
    if (!info || info->sType !=
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO ||
        info->pNext || (info->bindingCount && !info->pBindings))
        return -1;
    encode_u32(enc, VN_CMD_CREATE_DESCRIPTOR_SET_LAYOUT);
    encode_u32(enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(enc, device->object_id);
    encode_u64(enc, 1); /* pCreateInfo */
    encode_u32(enc, (uint32_t)info->sType);
    encode_u64(enc, 0); /* pNext */
    encode_u32(enc, info->flags);
    encode_u32(enc, info->bindingCount);
    encode_u64(enc, info->pBindings ? info->bindingCount : 0);
    for (uint32_t i = 0; i < info->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *binding = &info->pBindings[i];
        encode_u32(enc, binding->binding);
        encode_u32(enc, (uint32_t)binding->descriptorType);
        encode_u32(enc, binding->descriptorCount);
        encode_u32(enc, binding->stageFlags);
        encode_u64(enc, binding->pImmutableSamplers
            ? binding->descriptorCount : 0);
        for (uint32_t j = 0;
             binding->pImmutableSamplers && j < binding->descriptorCount;
             j++) {
            encode_u64(enc, (uint64_t)binding->pImmutableSamplers[j]);
        }
    }
    encode_u64(enc, 0); /* pAllocator */
    encode_u64(enc, 1); /* pSetLayout */
    encode_u64(enc, object_id);
    return enc->failed ? -1 : 0;
}

static VkResult decode_create_reply(const uint8_t reply[24],
                                    uint32_t expected_command,
                                    uint64_t expected_object)
{
    uint32_t command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t present = 0;
    uint64_t object_id = 0;
    memcpy(&command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&present, reply + 8, 8);
    memcpy(&object_id, reply + 16, 8);
    if (command != expected_command)
        return VK_ERROR_DEVICE_LOST;
    if (result == VK_SUCCESS && (!present || object_id != expected_object))
        return VK_ERROR_DEVICE_LOST;
    return (VkResult)result;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDescriptorSetLayout *set_layout)
{
    if (!device || !create_info || !set_layout || allocator)
        return VK_ERROR_INITIALIZATION_FAILED;
    void *object = malloc(1);
    if (!object)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    struct venus_device_real *self = (struct venus_device_real *)device;
    struct descriptor_encoder size_enc = { .capacity = UINT32_MAX };
    if (encode_set_layout_create(&size_enc, self, create_info, object_id) < 0) {
        free(object);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint8_t *command = malloc(size_enc.length);
    if (!command) {
        free(object);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    struct descriptor_encoder enc = {
        .data = command,
        .capacity = size_enc.length,
    };
    if (encode_set_layout_create(&enc, self, create_info, object_id) < 0) {
        free(command);
        free(object);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = venus_wire_call(self->physical_device->instance->wire,
                                      command, enc.length,
                                      reply, sizeof(reply));
    free(command);
    if (wire_result < 0) {
        free(object);
        return VK_ERROR_DEVICE_LOST;
    }
    VkResult result = decode_create_reply(
        reply, VN_CMD_CREATE_DESCRIPTOR_SET_LAYOUT, object_id);
    if (result != VK_SUCCESS) {
        free(object);
        return result;
    }
    *set_layout = (VkDescriptorSetLayout)object_id;
    return VK_SUCCESS;
}

static void destroy_object(struct venus_device_real *device,
                           uint32_t command_type, uint64_t object_id)
{
    uint8_t command[32];
    struct descriptor_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, command_type);
    encode_u32(&enc, 0);
    encode_u64(&enc, device->object_id);
    encode_u64(&enc, object_id);
    encode_u64(&enc, 0); /* pAllocator */
    if (!enc.failed)
        (void)venus_wire_submit_async(device->physical_device->instance->wire,
                                      command, sizeof(command));
    free((void *)(uintptr_t)object_id);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyDescriptorSetLayout(
    VkDevice device, VkDescriptorSetLayout set_layout,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (device && set_layout)
        destroy_object((struct venus_device_real *)device,
                       VN_CMD_DESTROY_DESCRIPTOR_SET_LAYOUT,
                       (uint64_t)set_layout);
}

static int encode_update_template_create(
    struct descriptor_encoder *enc, struct venus_device_real *device,
    const VkDescriptorUpdateTemplateCreateInfo *info, uint64_t object_id)
{
    if (!info || info->sType !=
            VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO ||
        info->pNext ||
        (info->descriptorUpdateEntryCount &&
         !info->pDescriptorUpdateEntries))
        return -1;
    encode_u32(enc, VN_CMD_CREATE_DESCRIPTOR_UPDATE_TEMPLATE);
    encode_u32(enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(enc, device->object_id);
    encode_u64(enc, 1); /* pCreateInfo */
    encode_u32(enc, (uint32_t)info->sType);
    encode_u64(enc, 0); /* pNext */
    encode_u32(enc, info->flags);
    encode_u32(enc, info->descriptorUpdateEntryCount);
    encode_u64(enc, info->pDescriptorUpdateEntries
        ? info->descriptorUpdateEntryCount : 0);
    for (uint32_t i = 0; i < info->descriptorUpdateEntryCount; i++) {
        const VkDescriptorUpdateTemplateEntry *entry =
            &info->pDescriptorUpdateEntries[i];
        encode_u32(enc, entry->dstBinding);
        encode_u32(enc, entry->dstArrayElement);
        encode_u32(enc, entry->descriptorCount);
        encode_u32(enc, (uint32_t)entry->descriptorType);
        encode_u64(enc, (uint64_t)entry->offset);
        encode_u64(enc, (uint64_t)entry->stride);
    }
    encode_u32(enc, (uint32_t)info->templateType);
    encode_u64(enc, (uint64_t)info->descriptorSetLayout);
    encode_u32(enc, (uint32_t)info->pipelineBindPoint);
    encode_u64(enc, (uint64_t)info->pipelineLayout);
    encode_u32(enc, info->set);
    encode_u64(enc, 0); /* pAllocator */
    encode_u64(enc, 1); /* pDescriptorUpdateTemplate */
    encode_u64(enc, object_id);
    return enc->failed ? -1 : 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateDescriptorUpdateTemplate(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDescriptorUpdateTemplate *update_template)
{
    if (!device || !create_info || !update_template || allocator)
        return VK_ERROR_INITIALIZATION_FAILED;
    void *object = malloc(1);
    if (!object)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    struct venus_device_real *self = (struct venus_device_real *)device;
    struct descriptor_encoder size_enc = { .capacity = UINT32_MAX };
    if (encode_update_template_create(
            &size_enc, self, create_info, object_id) < 0) {
        free(object);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint8_t *command = malloc(size_enc.length);
    if (!command) {
        free(object);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    struct descriptor_encoder enc = {
        .data = command,
        .capacity = size_enc.length,
    };
    if (encode_update_template_create(&enc, self, create_info, object_id) < 0) {
        free(command);
        free(object);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = venus_wire_call(self->physical_device->instance->wire,
                                      command, enc.length,
                                      reply, sizeof(reply));
    free(command);
    if (wire_result < 0) {
        free(object);
        return VK_ERROR_DEVICE_LOST;
    }
    VkResult result = decode_create_reply(
        reply, VN_CMD_CREATE_DESCRIPTOR_UPDATE_TEMPLATE, object_id);
    if (result != VK_SUCCESS) {
        free(object);
        return result;
    }
    *update_template = (VkDescriptorUpdateTemplate)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyDescriptorUpdateTemplate(
    VkDevice device, VkDescriptorUpdateTemplate update_template,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (device && update_template)
        destroy_object((struct venus_device_real *)device,
                       VN_CMD_DESTROY_DESCRIPTOR_UPDATE_TEMPLATE,
                       (uint64_t)update_template);
}

static int encode_pipeline_layout_create(
    struct descriptor_encoder *enc, struct venus_device_real *device,
    const VkPipelineLayoutCreateInfo *info, uint64_t object_id)
{
    if (!info || info->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO ||
        info->pNext || (info->setLayoutCount && !info->pSetLayouts) ||
        (info->pushConstantRangeCount && !info->pPushConstantRanges))
        return -1;
    encode_u32(enc, VN_CMD_CREATE_PIPELINE_LAYOUT);
    encode_u32(enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(enc, device->object_id);
    encode_u64(enc, 1); /* pCreateInfo */
    encode_u32(enc, (uint32_t)info->sType);
    encode_u64(enc, 0); /* pNext */
    encode_u32(enc, info->flags);
    encode_u32(enc, info->setLayoutCount);
    encode_u64(enc, info->pSetLayouts ? info->setLayoutCount : 0);
    for (uint32_t i = 0; i < info->setLayoutCount; i++)
        encode_u64(enc, (uint64_t)info->pSetLayouts[i]);
    encode_u32(enc, info->pushConstantRangeCount);
    encode_u64(enc, info->pPushConstantRanges
        ? info->pushConstantRangeCount : 0);
    for (uint32_t i = 0; i < info->pushConstantRangeCount; i++) {
        encode_u32(enc, info->pPushConstantRanges[i].stageFlags);
        encode_u32(enc, info->pPushConstantRanges[i].offset);
        encode_u32(enc, info->pPushConstantRanges[i].size);
    }
    encode_u64(enc, 0); /* pAllocator */
    encode_u64(enc, 1); /* pPipelineLayout */
    encode_u64(enc, object_id);
    return enc->failed ? -1 : 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreatePipelineLayout(
    VkDevice device, const VkPipelineLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkPipelineLayout *pipeline_layout)
{
    if (!device || !create_info || !pipeline_layout || allocator)
        return VK_ERROR_INITIALIZATION_FAILED;
    void *object = malloc(1);
    if (!object)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    struct venus_device_real *self = (struct venus_device_real *)device;
    struct descriptor_encoder size_enc = { .capacity = UINT32_MAX };
    if (encode_pipeline_layout_create(
            &size_enc, self, create_info, object_id) < 0) {
        free(object);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint8_t *command = malloc(size_enc.length);
    if (!command) {
        free(object);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    struct descriptor_encoder enc = {
        .data = command,
        .capacity = size_enc.length,
    };
    if (encode_pipeline_layout_create(&enc, self, create_info, object_id) < 0) {
        free(command);
        free(object);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = venus_wire_call(self->physical_device->instance->wire,
                                      command, enc.length,
                                      reply, sizeof(reply));
    free(command);
    if (wire_result < 0) {
        free(object);
        return VK_ERROR_DEVICE_LOST;
    }
    VkResult result = decode_create_reply(
        reply, VN_CMD_CREATE_PIPELINE_LAYOUT, object_id);
    if (result != VK_SUCCESS) {
        free(object);
        return result;
    }
    *pipeline_layout = (VkPipelineLayout)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyPipelineLayout(
    VkDevice device, VkPipelineLayout pipeline_layout,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (device && pipeline_layout)
        destroy_object((struct venus_device_real *)device,
                       VN_CMD_DESTROY_PIPELINE_LAYOUT,
                       (uint64_t)pipeline_layout);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkDescriptorPool *pool)
{
    if (!device || !create_info || !pool || allocator ||
        create_info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO ||
        create_info->pNext ||
        (create_info->poolSizeCount && !create_info->pPoolSizes) ||
        create_info->poolSizeCount > 64)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t command_size = 80u + create_info->poolSizeCount * 8u;
    uint8_t *command = malloc(command_size);
    void *object = malloc(1);
    if (!command || !object) {
        free(command);
        free(object);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    struct descriptor_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_CREATE_DESCRIPTOR_POOL);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1);
    encode_u32(&enc, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    encode_u64(&enc, 0);
    encode_u32(&enc, create_info->flags);
    encode_u32(&enc, create_info->maxSets);
    encode_u32(&enc, create_info->poolSizeCount);
    encode_u64(&enc, create_info->poolSizeCount);
    for (uint32_t i = 0; i < create_info->poolSizeCount; i++) {
        encode_u32(&enc, (uint32_t)create_info->pPoolSizes[i].type);
        encode_u32(&enc, create_info->pPoolSizes[i].descriptorCount);
    }
    encode_u64(&enc, 0);
    encode_u64(&enc, 1);
    encode_u64(&enc, object_id);
    int wire_result = enc.failed ? -1 :
        venus_wire_submit_async(self->physical_device->instance->wire,
                                command, command_size);
    free(command);
    if (wire_result < 0) {
        free(object);
        return VK_ERROR_DEVICE_LOST;
    }
    *pool = (VkDescriptorPool)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyDescriptorPool(
    VkDevice device, VkDescriptorPool pool,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (device && pool)
        destroy_object((struct venus_device_real *)device,
                       VN_CMD_DESTROY_DESCRIPTOR_POOL, (uint64_t)pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_ResetDescriptorPool(VkDevice device, VkDescriptorPool pool,
                               VkDescriptorPoolResetFlags flags)
{
    if (!device || !pool)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t command[28];
    struct descriptor_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_RESET_DESCRIPTOR_POOL);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)pool);
    encode_u32(&enc, flags);
    return !enc.failed &&
        venus_wire_submit_async(self->physical_device->instance->wire,
                                command, sizeof(command)) == 0
        ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_AllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *allocate_info,
    VkDescriptorSet *sets)
{
    if (!device || !allocate_info || !sets ||
        allocate_info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO ||
        allocate_info->pNext || !allocate_info->descriptorPool ||
        !allocate_info->descriptorSetCount || !allocate_info->pSetLayouts ||
        allocate_info->descriptorSetCount > 32)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count = allocate_info->descriptorSetCount;
    void *objects[32];
    memset(objects, 0, sizeof(objects));
    for (uint32_t i = 0; i < count; i++) {
        objects[i] = malloc(1);
        if (!objects[i]) {
            for (uint32_t j = 0; j < i; j++)
                free(objects[j]);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    uint32_t command_size = 64u + count * 16u;
    uint8_t *command = malloc(command_size);
    if (!command) {
        for (uint32_t i = 0; i < count; i++)
            free(objects[i]);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    struct descriptor_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_ALLOCATE_DESCRIPTOR_SETS);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1);
    encode_u32(&enc, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    encode_u64(&enc, 0);
    encode_u64(&enc, (uint64_t)allocate_info->descriptorPool);
    encode_u32(&enc, count);
    encode_u64(&enc, count);
    for (uint32_t i = 0; i < count; i++)
        encode_u64(&enc, (uint64_t)allocate_info->pSetLayouts[i]);
    encode_u64(&enc, count);
    for (uint32_t i = 0; i < count; i++)
        encode_u64(&enc, (uint64_t)(uintptr_t)objects[i]);
    int wire_result = enc.failed ? -1 :
        venus_wire_submit_async(self->physical_device->instance->wire,
                                command, command_size);
    free(command);
    if (wire_result < 0) {
        for (uint32_t i = 0; i < count; i++)
            free(objects[i]);
        return VK_ERROR_DEVICE_LOST;
    }
    for (uint32_t i = 0; i < count; i++)
        sets[i] = (VkDescriptorSet)(uintptr_t)objects[i];
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_UpdateDescriptorSets(
    VkDevice device, uint32_t write_count,
    const VkWriteDescriptorSet *writes, uint32_t copy_count,
    const VkCopyDescriptorSet *copies)
{
    if (!device || (write_count && !writes) || (copy_count && !copies) ||
        write_count > 128 || copy_count)
        return;
    uint8_t *command = malloc(1u << 20);
    if (!command)
        return;
    struct descriptor_encoder enc = {
        .data = command,
        .capacity = 1u << 20,
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_UPDATE_DESCRIPTOR_SETS);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, write_count);
    encode_u64(&enc, write_count);
    for (uint32_t i = 0; i < write_count; i++) {
        const VkWriteDescriptorSet *write = &writes[i];
        if (write->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET ||
            write->pNext) {
            enc.failed = 1;
            break;
        }
        encode_u32(&enc, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        encode_u64(&enc, 0);
        encode_u64(&enc, (uint64_t)write->dstSet);
        encode_u32(&enc, write->dstBinding);
        encode_u32(&enc, write->dstArrayElement);
        encode_u32(&enc, write->descriptorCount);
        encode_u32(&enc, (uint32_t)write->descriptorType);
        int image_info = write->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
            write->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
            write->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
            write->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
            write->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        int buffer_info = write->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            write->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            write->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            write->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        int texel_view = write->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
            write->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        if ((image_info && !write->pImageInfo) ||
            (buffer_info && !write->pBufferInfo) ||
            (texel_view && !write->pTexelBufferView)) {
            enc.failed = 1;
            break;
        }
        encode_u64(&enc, image_info ? write->descriptorCount : 0u);
        if (image_info) {
            for (uint32_t j = 0; j < write->descriptorCount; j++) {
                encode_u64(&enc, (uint64_t)write->pImageInfo[j].sampler);
                encode_u64(&enc, (uint64_t)write->pImageInfo[j].imageView);
                encode_u32(&enc,
                           (uint32_t)write->pImageInfo[j].imageLayout);
            }
        }
        encode_u64(&enc, buffer_info ? write->descriptorCount : 0u);
        if (buffer_info) {
            for (uint32_t j = 0; j < write->descriptorCount; j++) {
                encode_u64(&enc, (uint64_t)write->pBufferInfo[j].buffer);
                encode_u64(&enc, write->pBufferInfo[j].offset);
                encode_u64(&enc, write->pBufferInfo[j].range);
            }
        }
        encode_u64(&enc, texel_view ? write->descriptorCount : 0u);
        if (texel_view)
            for (uint32_t j = 0; j < write->descriptorCount; j++)
                encode_u64(&enc, (uint64_t)write->pTexelBufferView[j]);
    }
    encode_u32(&enc, 0);
    encode_u64(&enc, 0);
    if (!enc.failed)
        (void)venus_wire_submit_async(self->physical_device->instance->wire,
                                      command, enc.length);
    free(command);
}
