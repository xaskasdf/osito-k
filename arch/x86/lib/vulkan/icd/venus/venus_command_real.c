#include "venus_real.h"

extern void *malloc(unsigned long);
extern void free(void *);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern int printf(const char *, ...);
extern void okgl_diag3(const char *message, uint64_t a, uint64_t b,
                       uint64_t c);

#ifndef OSITO_VN_SYNC_DIAGNOSTICS
#define OSITO_VN_SYNC_DIAGNOSTICS 0
#endif

#define VN_CMD_CREATE_FENCE 35u
#define VN_CMD_DESTROY_FENCE 36u
#define VN_CMD_RESET_FENCES 37u
#define VN_CMD_WAIT_FOR_FENCES 39u
#define VN_CMD_CREATE_SEMAPHORE 40u
#define VN_CMD_DESTROY_SEMAPHORE 41u
#define VN_CMD_GET_SEMAPHORE_COUNTER_VALUE 172u
#define VN_CMD_WAIT_SEMAPHORES 173u
#define VN_CMD_SIGNAL_SEMAPHORE 174u
#define VN_CMD_CREATE_COMMAND_POOL 85u
#define VN_CMD_DESTROY_COMMAND_POOL 86u
#define VN_CMD_RESET_COMMAND_POOL 87u
#define VN_CMD_ALLOCATE_COMMAND_BUFFERS 88u
#define VN_CMD_FREE_COMMAND_BUFFERS 89u
#define VN_CMD_BEGIN_COMMAND_BUFFER 90u
#define VN_CMD_END_COMMAND_BUFFER 91u
#define VN_CMD_RESET_COMMAND_BUFFER 92u
#define VN_CMD_QUEUE_WAIT_IDLE 19u
#define VN_CMD_QUEUE_SUBMIT_2 206u
#define VN_CMD_COPY_BUFFER_2 207u
#define VN_CMD_COPY_BUFFER_TO_IMAGE_2 209u
#define VN_CMD_COPY_IMAGE_TO_BUFFER_2 210u
#define VN_CMD_PIPELINE_BARRIER_2 204u
#define VN_CMD_CLEAR_COLOR_IMAGE 119u
#define VN_CMD_CLEAR_DEPTH_STENCIL_IMAGE 120u
#define VN_CMD_BEGIN_RENDERING 213u
#define VN_CMD_END_RENDERING 214u
#define VN_CMD_BIND_VERTEX_BUFFERS_2 220u
#define VN_CMD_BIND_INDEX_BUFFER_2 279u
#define VN_CMD_BIND_PIPELINE 93u
#define VN_CMD_SET_VIEWPORT 94u
#define VN_CMD_SET_SCISSOR 95u
#define VN_CMD_SET_LINE_WIDTH 96u
#define VN_CMD_SET_DEPTH_BIAS 97u
#define VN_CMD_SET_ATTACHMENT_FEEDBACK_LOOP_ENABLE 329u
#define VN_CMD_SET_BLEND_CONSTANTS 98u
#define VN_CMD_SET_DEPTH_BOUNDS 99u
#define VN_CMD_SET_STENCIL_COMPARE_MASK 100u
#define VN_CMD_SET_STENCIL_WRITE_MASK 101u
#define VN_CMD_SET_STENCIL_REFERENCE 102u
#define VN_CMD_DRAW 106u
#define VN_CMD_BIND_DESCRIPTOR_SETS 103u
#define VN_CMD_BIND_INDEX_BUFFER 104u
#define VN_CMD_BIND_VERTEX_BUFFERS 105u
#define VN_CMD_DRAW_INDEXED 107u
#define VN_CMD_PUSH_DESCRIPTOR_SET 249u
#define VN_CMD_PUSH_CONSTANTS 132u
#define VN_CMD_SET_CULL_MODE 215u
#define VN_CMD_SET_FRONT_FACE 216u
#define VN_CMD_SET_PRIMITIVE_TOPOLOGY 217u
#define VN_CMD_SET_VIEWPORT_WITH_COUNT 218u
#define VN_CMD_SET_SCISSOR_WITH_COUNT 219u
#define VN_COMMAND_GENERATE_REPLY 1u

static volatile uint32_t sync_submit_log_count;
static volatile uint32_t sync_submit_call_log_count;
static volatile uint32_t sync_wait_log_count;
static volatile uint32_t sync_counter_log_count;

static int sync_log_take(volatile uint32_t *counter, uint32_t limit)
{
    if (!OSITO_VN_SYNC_DIAGNOSTICS)
        return 0;
    return __sync_fetch_and_add(counter, 1) < limit;
}

struct venus_command_pool_real;

struct venus_command_buffer_real {
    VK_LOADER_DATA loader_data;
    uint64_t object_id;
    struct venus_command_pool_real *pool;
    struct venus_command_buffer_real *next;
};

struct venus_command_pool_real {
    uint64_t object_id;
    struct venus_device_real *device;
    struct venus_command_buffer_real *command_buffers;
};

struct command_encoder {
    uint8_t *data;
    uint32_t capacity;
    uint32_t length;
    int failed;
};

static void encode_bytes(struct command_encoder *enc, const void *src,
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

static void encode_u32(struct command_encoder *enc, uint32_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static void encode_u64(struct command_encoder *enc, uint64_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static void encode_write_descriptor_pnext(struct command_encoder *enc,
                                          const void *chain)
{
    if (!chain) {
        encode_u64(enc, 0);
        return;
    }
    const VkBaseInStructure *base = (const VkBaseInStructure *)chain;
    encode_u64(enc, 1);
    encode_u32(enc, (uint32_t)base->sType);
    switch (base->sType) {
    case VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK: {
        const VkWriteDescriptorSetInlineUniformBlock *inline_block =
            (const VkWriteDescriptorSetInlineUniformBlock *)chain;
        encode_write_descriptor_pnext(enc, inline_block->pNext);
        encode_u32(enc, inline_block->dataSize);
        encode_u64(enc, inline_block->pData ? inline_block->dataSize : 0);
        if (inline_block->pData)
            encode_bytes(enc, inline_block->pData, inline_block->dataSize);
        break;
    }
    case VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR: {
        const VkWriteDescriptorSetAccelerationStructureKHR *accel =
            (const VkWriteDescriptorSetAccelerationStructureKHR *)chain;
        encode_write_descriptor_pnext(enc, accel->pNext);
        encode_u32(enc, accel->accelerationStructureCount);
        encode_u64(enc, accel->pAccelerationStructures
            ? accel->accelerationStructureCount : 0);
        if (accel->pAccelerationStructures) {
            for (uint32_t i = 0; i < accel->accelerationStructureCount; i++) {
                encode_u64(enc,
                    (uint64_t)accel->pAccelerationStructures[i]);
            }
        }
        break;
    }
    default:
        enc->failed = 1;
        break;
    }
}

static void encode_write_descriptor_set(struct command_encoder *enc,
                                        const VkWriteDescriptorSet *write)
{
    if (!write || write->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET) {
        enc->failed = 1;
        return;
    }
    encode_u32(enc, (uint32_t)write->sType);
    encode_write_descriptor_pnext(enc, write->pNext);
    encode_u64(enc, (uint64_t)write->dstSet);
    encode_u32(enc, write->dstBinding);
    encode_u32(enc, write->dstArrayElement);
    encode_u32(enc, write->descriptorCount);
    encode_u32(enc, (uint32_t)write->descriptorType);

    encode_u64(enc, write->pImageInfo ? write->descriptorCount : 0);
    if (write->pImageInfo) {
        for (uint32_t i = 0; i < write->descriptorCount; i++) {
            encode_u64(enc, (uint64_t)write->pImageInfo[i].sampler);
            encode_u64(enc, (uint64_t)write->pImageInfo[i].imageView);
            encode_u32(enc, (uint32_t)write->pImageInfo[i].imageLayout);
        }
    }

    encode_u64(enc, write->pBufferInfo ? write->descriptorCount : 0);
    if (write->pBufferInfo) {
        for (uint32_t i = 0; i < write->descriptorCount; i++) {
            encode_u64(enc, (uint64_t)write->pBufferInfo[i].buffer);
            encode_u64(enc, write->pBufferInfo[i].offset);
            encode_u64(enc, write->pBufferInfo[i].range);
        }
    }

    encode_u64(enc, write->pTexelBufferView ? write->descriptorCount : 0);
    if (write->pTexelBufferView) {
        for (uint32_t i = 0; i < write->descriptorCount; i++)
            encode_u64(enc, (uint64_t)write->pTexelBufferView[i]);
    }
}

static VkResult decode_object_reply(const uint8_t reply[24],
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

static VkResult create_simple_object(
    struct venus_device_real *device, uint32_t command_type,
    VkStructureType structure_type, uint32_t flags,
    const uint32_t *extra, uint64_t object_id)
{
    uint8_t command[68];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, command_type);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, device->object_id);
    encode_u64(&enc, 1); /* pCreateInfo */
    encode_u32(&enc, (uint32_t)structure_type);
    encode_u64(&enc, 0); /* pNext */
    encode_u32(&enc, flags);
    if (extra)
        encode_u32(&enc, *extra);
    encode_u64(&enc, 0); /* pAllocator */
    encode_u64(&enc, 1); /* output pointer */
    encode_u64(&enc, object_id);
    if (enc.failed)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    if (venus_wire_call(device->physical_device->instance->wire,
                        command, enc.length, reply, sizeof(reply)) < 0)
        return VK_ERROR_DEVICE_LOST;
    return decode_object_reply(reply, command_type, object_id);
}

static void destroy_simple_object(struct venus_device_real *device,
                                  uint32_t command_type, uint64_t object_id)
{
    uint8_t command[32];
    struct command_encoder enc = {
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
}

static void encode_semaphore_create_chain(struct command_encoder *enc,
                                          const void *chain)
{
    const VkBaseInStructure *next = chain;
    while (next) {
        if (next->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
            const VkSemaphoreTypeCreateInfo *type_info =
                (const VkSemaphoreTypeCreateInfo *)next;
            encode_u64(enc, 1);
            encode_u32(enc, (uint32_t)next->sType);
            encode_semaphore_create_chain(enc, next->pNext);
            encode_u32(enc, (uint32_t)type_info->semaphoreType);
            encode_u64(enc, type_info->initialValue);
            return;
        }
        if (next->sType == VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO) {
            const VkExportSemaphoreCreateInfo *export_info =
                (const VkExportSemaphoreCreateInfo *)next;
            encode_u64(enc, 1);
            encode_u32(enc, (uint32_t)next->sType);
            encode_semaphore_create_chain(enc, next->pNext);
            encode_u32(enc, export_info->handleTypes);
            return;
        }
        next = next->pNext;
    }
    encode_u64(enc, 0);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateSemaphore(VkDevice device,
                           const VkSemaphoreCreateInfo *create_info,
                           const VkAllocationCallbacks *allocator,
                           VkSemaphore *semaphore)
{
    if (!device || !create_info || !semaphore || allocator ||
        create_info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO)
        return VK_ERROR_INITIALIZATION_FAILED;
    void *object = malloc(1);
    if (!object)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    struct venus_device_real *self = (struct venus_device_real *)device;
    uint8_t command[128];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_CREATE_SEMAPHORE);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pCreateInfo */
    encode_u32(&enc, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    encode_semaphore_create_chain(&enc, create_info->pNext);
    encode_u32(&enc, create_info->flags);
    encode_u64(&enc, 0); /* pAllocator */
    encode_u64(&enc, 1); /* pSemaphore */
    encode_u64(&enc, object_id);
    if (enc.failed) {
        free(object);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    VkResult result = VK_ERROR_DEVICE_LOST;
    if (venus_wire_call(self->physical_device->instance->wire,
                        command, enc.length, reply, sizeof(reply)) == 0)
        result = decode_object_reply(reply, VN_CMD_CREATE_SEMAPHORE,
                                     object_id);
    if (result != VK_SUCCESS) {
        free(object);
        return result;
    }
    *semaphore = (VkSemaphore)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroySemaphore(VkDevice device, VkSemaphore semaphore,
                            const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !semaphore)
        return;
    destroy_simple_object((struct venus_device_real *)device,
                          VN_CMD_DESTROY_SEMAPHORE, (uint64_t)semaphore);
    free((void *)(uintptr_t)semaphore);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_GetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore,
                                    uint64_t *value)
{
    if (!device || !semaphore || !value)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device_real *self = (struct venus_device_real *)device;
    uint8_t command[32];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_GET_SEMAPHORE_COUNTER_VALUE);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)semaphore);
    encode_u64(&enc, 1); /* pValue */
    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    if (enc.failed ||
        venus_wire_call(self->physical_device->instance->wire,
                        command, sizeof(command), reply, sizeof(reply)) < 0)
        return VK_ERROR_DEVICE_LOST;
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t present = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&present, reply + 8, 8);
    if (returned_command != VN_CMD_GET_SEMAPHORE_COUNTER_VALUE || !present)
        return VK_ERROR_DEVICE_LOST;
    memcpy(value, reply + 16, sizeof(*value));
    if (sync_log_take(&sync_counter_log_count, 64u))
    {
        printf("[VN sync] counter sem=%p value=%llu result=%d\n",
               (void *)(uintptr_t)semaphore,
               (unsigned long long)*value, result);
        okgl_diag3("[VN-COUNTER]", (uint64_t)semaphore, *value,
                   (uint64_t)(uint32_t)result);
    }
    return (VkResult)result;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_WaitSemaphores(VkDevice device,
                          const VkSemaphoreWaitInfo *wait_info,
                          uint64_t timeout)
{
    if (!device || !wait_info ||
        wait_info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO ||
        (wait_info->semaphoreCount &&
         (!wait_info->pSemaphores || !wait_info->pValues)) ||
        wait_info->semaphoreCount > (UINT32_MAX - 68u) / 16u)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t command_size = 68u + wait_info->semaphoreCount * 16u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_device_real *self = (struct venus_device_real *)device;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    encode_u32(&enc, VN_CMD_WAIT_SEMAPHORES);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pWaitInfo */
    encode_u32(&enc, VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
    encode_u64(&enc, 0); /* unsupported pNext */
    encode_u32(&enc, wait_info->flags);
    encode_u32(&enc, wait_info->semaphoreCount);
    encode_u64(&enc, wait_info->semaphoreCount);
    for (uint32_t i = 0; i < wait_info->semaphoreCount; i++)
        encode_u64(&enc, (uint64_t)wait_info->pSemaphores[i]);
    encode_u64(&enc, wait_info->semaphoreCount);
    for (uint32_t i = 0; i < wait_info->semaphoreCount; i++)
        encode_u64(&enc, wait_info->pValues[i]);
    encode_u64(&enc, timeout);
    uint8_t reply[8];
    memset(reply, 0, sizeof(reply));
    int wire_result = enc.failed ? -1 :
        venus_wire_call(self->physical_device->instance->wire,
                        command, enc.length, reply, sizeof(reply));
    free(command);
    if (wire_result < 0)
        return VK_ERROR_DEVICE_LOST;
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    if (sync_log_take(&sync_wait_log_count, 128u)) {
        uint64_t first_value = wait_info->semaphoreCount
            ? wait_info->pValues[0] : 0;
        printf("[VN sync] wait count=%u first=%llu timeout=%llu "
               "reply=%u result=%d\n",
               wait_info->semaphoreCount,
               (unsigned long long)first_value,
               (unsigned long long)timeout,
               returned_command, result);
        okgl_diag3("[VN-WAIT]", wait_info->semaphoreCount, first_value,
                   (uint64_t)(uint32_t)result);
    }
    return returned_command == VN_CMD_WAIT_SEMAPHORES
        ? (VkResult)result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_SignalSemaphore(VkDevice device,
                           const VkSemaphoreSignalInfo *signal_info)
{
    if (!device || !signal_info || !signal_info->semaphore ||
        signal_info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device_real *self = (struct venus_device_real *)device;
    uint8_t command[52];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_SIGNAL_SEMAPHORE);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pSignalInfo */
    encode_u32(&enc, VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO);
    encode_u64(&enc, 0); /* unsupported pNext */
    encode_u64(&enc, (uint64_t)signal_info->semaphore);
    encode_u64(&enc, signal_info->value);
    uint8_t reply[8];
    memset(reply, 0, sizeof(reply));
    if (enc.failed ||
        venus_wire_call(self->physical_device->instance->wire,
                        command, sizeof(command), reply, sizeof(reply)) < 0)
        return VK_ERROR_DEVICE_LOST;
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    return returned_command == VN_CMD_SIGNAL_SEMAPHORE
        ? (VkResult)result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateFence(VkDevice device,
                       const VkFenceCreateInfo *create_info,
                       const VkAllocationCallbacks *allocator,
                       VkFence *fence)
{
    if (!device || !create_info || !fence || allocator ||
        create_info->sType != VK_STRUCTURE_TYPE_FENCE_CREATE_INFO ||
        create_info->pNext)
        return VK_ERROR_INITIALIZATION_FAILED;
    void *object = malloc(1);
    if (!object)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    VkResult result = create_simple_object(
        (struct venus_device_real *)device, VN_CMD_CREATE_FENCE,
        create_info->sType, create_info->flags, 0, object_id);
    if (result != VK_SUCCESS) {
        free(object);
        return result;
    }
    *fence = (VkFence)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyFence(VkDevice device, VkFence fence,
                        const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !fence)
        return;
    destroy_simple_object((struct venus_device_real *)device,
                          VN_CMD_DESTROY_FENCE, (uint64_t)fence);
    free((void *)(uintptr_t)fence);
}

static VkResult fence_array_call(struct venus_device_real *device,
                                 uint32_t command_type, uint32_t fence_count,
                                 const VkFence *fences, VkBool32 wait_all,
                                 uint64_t timeout)
{
    if (!device || !fence_count || !fences ||
        fence_count > (UINT32_MAX - 40u) / 8u)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t command_size = 28u + fence_count * 8u;
    if (command_type == VN_CMD_WAIT_FOR_FENCES)
        command_size += 12u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    encode_u32(&enc, command_type);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, device->object_id);
    encode_u32(&enc, fence_count);
    encode_u64(&enc, fence_count);
    for (uint32_t i = 0; i < fence_count; i++) {
        if (!fences[i])
            enc.failed = 1;
        encode_u64(&enc, (uint64_t)fences[i]);
    }
    if (command_type == VN_CMD_WAIT_FOR_FENCES) {
        encode_u32(&enc, wait_all);
        encode_u64(&enc, timeout);
    }

    uint8_t reply[8];
    memset(reply, 0, sizeof(reply));
    int wire_result = enc.failed ? -1 :
        venus_wire_call(device->physical_device->instance->wire,
                        command, command_size, reply, sizeof(reply));
    free(command);
    if (wire_result < 0) {
        printf("[VN] fence command %u wire failure %d\n",
               command_type, wire_result);
        return VK_ERROR_DEVICE_LOST;
    }
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    memcpy(&returned_command, reply, sizeof(returned_command));
    memcpy(&result, reply + 4, sizeof(result));
    if (returned_command != command_type || result != VK_SUCCESS)
        printf("[VN] fence command %u reply command=%u result=%d\n",
               command_type, returned_command, result);
    return returned_command == command_type
        ? (VkResult)result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_ResetFences(VkDevice device, uint32_t fence_count,
                       const VkFence *fences)
{
    return fence_array_call((struct venus_device_real *)device,
                            VN_CMD_RESET_FENCES, fence_count, fences,
                            VK_FALSE, 0);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_WaitForFences(VkDevice device, uint32_t fence_count,
                         const VkFence *fences, VkBool32 wait_all,
                         uint64_t timeout)
{
    return fence_array_call((struct venus_device_real *)device,
                            VN_CMD_WAIT_FOR_FENCES, fence_count, fences,
                            wait_all, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateCommandPool(VkDevice device,
                             const VkCommandPoolCreateInfo *create_info,
                             const VkAllocationCallbacks *allocator,
                             VkCommandPool *command_pool)
{
    if (!device || !create_info || !command_pool || allocator ||
        create_info->sType != VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO ||
        create_info->pNext)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_command_pool_real *pool = malloc(sizeof(*pool));
    if (!pool)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(pool, 0, sizeof(*pool));
    pool->object_id = (uint64_t)(uintptr_t)pool;
    pool->device = (struct venus_device_real *)device;
    VkResult result = create_simple_object(
        pool->device, VN_CMD_CREATE_COMMAND_POOL, create_info->sType,
        create_info->flags, &create_info->queueFamilyIndex, pool->object_id);
    if (result != VK_SUCCESS) {
        free(pool);
        return result;
    }
    *command_pool = (VkCommandPool)(uintptr_t)pool;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyCommandPool(VkDevice device, VkCommandPool command_pool,
                              const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !command_pool)
        return;
    struct venus_command_pool_real *pool =
        (struct venus_command_pool_real *)(uintptr_t)command_pool;
    destroy_simple_object((struct venus_device_real *)device,
                          VN_CMD_DESTROY_COMMAND_POOL, pool->object_id);
    struct venus_command_buffer_real *command_buffer = pool->command_buffers;
    while (command_buffer) {
        struct venus_command_buffer_real *next = command_buffer->next;
        free(command_buffer);
        command_buffer = next;
    }
    free(pool);
}

static VkResult command_result_call(struct venus_device_real *device,
                                    const uint8_t *command,
                                    uint32_t command_size,
                                    uint32_t expected_command)
{
    uint8_t reply[8];
    memset(reply, 0, sizeof(reply));
    int wire_result = venus_wire_call(
        device->physical_device->instance->wire,
        command, command_size, reply, sizeof(reply));
    if (wire_result < 0) {
        printf("[VN] command %u wire failure %d\n",
               expected_command, wire_result);
        return VK_ERROR_DEVICE_LOST;
    }
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    if (returned_command != expected_command || result != VK_SUCCESS)
        printf("[VN] command %u reply command=%u result=%d\n",
               expected_command, returned_command, result);
    return returned_command == expected_command
        ? (VkResult)result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_ResetCommandPool(VkDevice device, VkCommandPool command_pool,
                            VkCommandPoolResetFlags flags)
{
    if (!device || !command_pool)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_command_pool_real *pool =
        (struct venus_command_pool_real *)(uintptr_t)command_pool;
    uint8_t command[28];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_RESET_COMMAND_POOL);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, pool->device->object_id);
    encode_u64(&enc, pool->object_id);
    encode_u32(&enc, flags);
    return enc.failed ? VK_ERROR_INITIALIZATION_FAILED
        : command_result_call(pool->device, command, sizeof(command),
                              VN_CMD_RESET_COMMAND_POOL);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_AllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *allocate_info,
    VkCommandBuffer *command_buffers)
{
    if (!device || !allocate_info || !command_buffers ||
        allocate_info->sType !=
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO ||
        allocate_info->pNext || !allocate_info->commandPool ||
        !allocate_info->commandBufferCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_command_pool_real *pool =
        (struct venus_command_pool_real *)(uintptr_t)allocate_info->commandPool;
    uint32_t count = allocate_info->commandBufferCount;
    struct venus_command_buffer_real **objects =
        malloc((unsigned long)count * sizeof(*objects));
    if (!objects)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(objects, 0, (unsigned long)count * sizeof(*objects));
    for (uint32_t i = 0; i < count; i++) {
        objects[i] = malloc(sizeof(*objects[i]));
        if (!objects[i]) {
            for (uint32_t j = 0; j < i; j++)
                free(objects[j]);
            free(objects);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        memset(objects[i], 0, sizeof(*objects[i]));
        set_loader_magic_value(objects[i]);
        objects[i]->object_id = (uint64_t)(uintptr_t)objects[i];
        objects[i]->pool = pool;
    }
    uint32_t command_size = 60u + count * 8u;
    uint8_t *command = malloc(command_size);
    if (!command) {
        for (uint32_t i = 0; i < count; i++)
            free(objects[i]);
        free(objects);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    encode_u32(&enc, VN_CMD_ALLOCATE_COMMAND_BUFFERS);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, pool->device->object_id);
    encode_u64(&enc, 1); /* pAllocateInfo */
    encode_u32(&enc, (uint32_t)allocate_info->sType);
    encode_u64(&enc, 0); /* pNext */
    encode_u64(&enc, pool->object_id);
    encode_u32(&enc, (uint32_t)allocate_info->level);
    encode_u32(&enc, count);
    encode_u64(&enc, count);
    for (uint32_t i = 0; i < count; i++)
        encode_u64(&enc, objects[i]->object_id);
    uint32_t reply_size = 16u + count * 8u;
    uint8_t *reply = malloc(reply_size);
    if (enc.failed || !reply) {
        free(command);
        free(reply);
        for (uint32_t i = 0; i < count; i++)
            free(objects[i]);
        free(objects);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(reply, 0, reply_size);
    int wire_result = venus_wire_call(pool->device->physical_device->instance->wire,
                                      command, command_size,
                                      reply, reply_size);
    free(command);
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t returned_count = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&returned_count, reply + 8, 8);
    if (wire_result < 0 || returned_command != VN_CMD_ALLOCATE_COMMAND_BUFFERS ||
        returned_count != count)
        result = VK_ERROR_DEVICE_LOST;
    for (uint32_t i = 0; i < count && result == VK_SUCCESS; i++) {
        uint64_t returned_id = 0;
        memcpy(&returned_id, reply + 16u + i * 8u, 8);
        if (returned_id != objects[i]->object_id)
            result = VK_ERROR_DEVICE_LOST;
    }
    free(reply);
    if (result != VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++)
            free(objects[i]);
        free(objects);
        return (VkResult)result;
    }
    for (uint32_t i = 0; i < count; i++) {
        objects[i]->next = pool->command_buffers;
        pool->command_buffers = objects[i];
        command_buffers[i] = (VkCommandBuffer)objects[i];
    }
    free(objects);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_FreeCommandBuffers(VkDevice device, VkCommandPool command_pool,
                              uint32_t count,
                              const VkCommandBuffer *command_buffers)
{
    if (!device || !command_pool || !count || !command_buffers)
        return;
    struct venus_command_pool_real *pool =
        (struct venus_command_pool_real *)(uintptr_t)command_pool;
    uint32_t command_size = 36u + count * 8u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    encode_u32(&enc, VN_CMD_FREE_COMMAND_BUFFERS);
    encode_u32(&enc, 0);
    encode_u64(&enc, pool->device->object_id);
    encode_u64(&enc, pool->object_id);
    encode_u32(&enc, count);
    encode_u64(&enc, count);
    for (uint32_t i = 0; i < count; i++) {
        struct venus_command_buffer_real *command_buffer =
            (struct venus_command_buffer_real *)command_buffers[i];
        encode_u64(&enc, command_buffer->object_id);
    }
    if (!enc.failed)
        (void)venus_wire_submit_async(pool->device->physical_device->instance->wire,
                                      command, command_size);
    free(command);
    for (uint32_t i = 0; i < count; i++) {
        struct venus_command_buffer_real *target =
            (struct venus_command_buffer_real *)command_buffers[i];
        struct venus_command_buffer_real **link = &pool->command_buffers;
        while (*link && *link != target)
            link = &(*link)->next;
        if (*link) {
            *link = target->next;
            free(target);
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_BeginCommandBuffer(VkCommandBuffer command_buffer,
                              const VkCommandBufferBeginInfo *begin_info)
{
    if (!command_buffer || !begin_info ||
        begin_info->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO ||
        begin_info->pNext || begin_info->pInheritanceInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    uint8_t command[48];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_BEGIN_COMMAND_BUFFER);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pBeginInfo */
    encode_u32(&enc, (uint32_t)begin_info->sType);
    encode_u64(&enc, 0); /* pNext */
    encode_u32(&enc, begin_info->flags);
    encode_u64(&enc, 0); /* pInheritanceInfo */
    return enc.failed ? VK_ERROR_INITIALIZATION_FAILED
        : command_result_call(self->pool->device, command, sizeof(command),
                              VN_CMD_BEGIN_COMMAND_BUFFER);
}

static VkResult command_buffer_call(VkCommandBuffer command_buffer,
                                    uint32_t command_type,
                                    const uint32_t *flags)
{
    if (!command_buffer)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    uint8_t command[20];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, command_type);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    if (flags)
        encode_u32(&enc, *flags);
    return enc.failed ? VK_ERROR_INITIALIZATION_FAILED
        : command_result_call(self->pool->device, command, enc.length,
                              command_type);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_EndCommandBuffer(VkCommandBuffer command_buffer)
{
    return command_buffer_call(command_buffer, VN_CMD_END_COMMAND_BUFFER, 0);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_ResetCommandBuffer(VkCommandBuffer command_buffer,
                              VkCommandBufferResetFlags flags)
{
    return command_buffer_call(command_buffer, VN_CMD_RESET_COMMAND_BUFFER,
                               &flags);
}

static void encode_semaphore_submit(struct command_encoder *enc,
                                    const VkSemaphoreSubmitInfo *info)
{
    if (!info || info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO ||
        !info->semaphore) {
        enc->failed = 1;
        return;
    }
    encode_u32(enc, VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    encode_u64(enc, 0); /* unsupported pNext */
    encode_u64(enc, (uint64_t)info->semaphore);
    encode_u64(enc, info->value);
    encode_u64(enc, info->stageMask);
    encode_u32(enc, info->deviceIndex);
}

static void encode_command_buffer_submit(
    struct command_encoder *enc, const VkCommandBufferSubmitInfo *info)
{
    if (!info ||
        info->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO ||
        !info->commandBuffer) {
        enc->failed = 1;
        return;
    }
    struct venus_command_buffer_real *command_buffer =
        (struct venus_command_buffer_real *)info->commandBuffer;
    encode_u32(enc, VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
    encode_u64(enc, 0); /* unsupported pNext */
    encode_u64(enc, command_buffer->object_id);
    encode_u32(enc, info->deviceMask);
}

static void encode_submit_2(struct command_encoder *enc,
                            const VkSubmitInfo2 *submit)
{
    if (!submit || submit->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO_2 ||
        (submit->waitSemaphoreInfoCount && !submit->pWaitSemaphoreInfos) ||
        (submit->commandBufferInfoCount && !submit->pCommandBufferInfos) ||
        (submit->signalSemaphoreInfoCount &&
         !submit->pSignalSemaphoreInfos)) {
        enc->failed = 1;
        return;
    }

    encode_u32(enc, VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
    encode_u64(enc, 0); /* unsupported pNext */
    encode_u32(enc, submit->flags);

    encode_u32(enc, submit->waitSemaphoreInfoCount);
    encode_u64(enc, submit->pWaitSemaphoreInfos
                        ? submit->waitSemaphoreInfoCount : 0);
    for (uint32_t i = 0; i < submit->waitSemaphoreInfoCount; i++)
        encode_semaphore_submit(enc, &submit->pWaitSemaphoreInfos[i]);

    encode_u32(enc, submit->commandBufferInfoCount);
    encode_u64(enc, submit->pCommandBufferInfos
                        ? submit->commandBufferInfoCount : 0);
    for (uint32_t i = 0; i < submit->commandBufferInfoCount; i++)
        encode_command_buffer_submit(enc, &submit->pCommandBufferInfos[i]);

    encode_u32(enc, submit->signalSemaphoreInfoCount);
    encode_u64(enc, submit->pSignalSemaphoreInfos
                        ? submit->signalSemaphoreInfoCount : 0);
    for (uint32_t i = 0; i < submit->signalSemaphoreInfoCount; i++)
        encode_semaphore_submit(enc, &submit->pSignalSemaphoreInfos[i]);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_QueueWaitIdle(VkQueue queue)
{
    if (!queue)
        return VK_ERROR_INITIALIZATION_FAILED;

    struct venus_queue_real *self = (struct venus_queue_real *)queue;
    VkDevice device = (VkDevice)self->device;
    if (!self->wait_fence) {
        const VkFenceCreateInfo create_info = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };
        VkResult result = venus_real_CreateFence(
            device, &create_info, 0, &self->wait_fence);
        if (result != VK_SUCCESS)
            return result;
    }

    VkResult result = venus_real_QueueSubmit2(
        queue, 0, 0, self->wait_fence);
    if (result != VK_SUCCESS)
        return result;

    result = venus_real_WaitForFences(
        device, 1, &self->wait_fence, VK_TRUE, UINT64_MAX);
    VkResult reset_result = venus_real_ResetFences(
        device, 1, &self->wait_fence);
    return result == VK_SUCCESS ? reset_result : result;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_QueueSubmit2(VkQueue queue, uint32_t submit_count,
                        const VkSubmitInfo2 *submits, VkFence fence)
{
    if (!queue || (submit_count && !submits))
        return VK_ERROR_INITIALIZATION_FAILED;

    struct venus_queue_real *self = (struct venus_queue_real *)queue;
    if (sync_log_take(&sync_submit_call_log_count, 96u)) {
        uint32_t signal_count = submit_count
            ? submits[submit_count - 1].signalSemaphoreInfoCount : 0;
        uint64_t last_signal = signal_count
            ? submits[submit_count - 1]
                .pSignalSemaphoreInfos[signal_count - 1].value : 0;
        printf("[VN sync] submit2 enter count=%u last_signals=%u "
               "last=%llu fence=%p\n",
               submit_count, signal_count,
               (unsigned long long)last_signal,
               (void *)(uintptr_t)fence);
        okgl_diag3("[VN-SUBMIT2]", submit_count, signal_count, last_signal);
    }
    for (uint32_t i = 0; i < submit_count; i++) {
        const VkSubmitInfo2 *submit = &submits[i];
        for (uint32_t j = 0; j < submit->signalSemaphoreInfoCount; j++) {
            const VkSemaphoreSubmitInfo *signal =
                &submit->pSignalSemaphoreInfos[j];
            if (signal->value &&
                sync_log_take(&sync_submit_log_count, 128u)) {
                printf("[VN sync] submit queue=%p item=%u cmds=%u waits=%u "
                       "sem=%p value=%llu fence=%p\n",
                       (void *)(uintptr_t)self->object_id, i,
                       submit->commandBufferInfoCount,
                       submit->waitSemaphoreInfoCount,
                       (void *)(uintptr_t)signal->semaphore,
                       (unsigned long long)signal->value,
                       (void *)(uintptr_t)fence);
            }
        }
    }
    struct command_encoder enc = { .capacity = UINT32_MAX };
    encode_u32(&enc, VN_CMD_QUEUE_SUBMIT_2);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, submit_count);
    encode_u64(&enc, submits ? submit_count : 0);
    for (uint32_t i = 0; i < submit_count; i++)
        encode_submit_2(&enc, &submits[i]);
    encode_u64(&enc, (uint64_t)fence);
    if (enc.failed)
        return VK_ERROR_INITIALIZATION_FAILED;

    uint8_t *command = malloc(enc.length);
    if (!command)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t command_size = enc.length;
    memset(&enc, 0, sizeof(enc));
    enc.data = command;
    enc.capacity = command_size;
    encode_u32(&enc, VN_CMD_QUEUE_SUBMIT_2);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, submit_count);
    encode_u64(&enc, submits ? submit_count : 0);
    for (uint32_t i = 0; i < submit_count; i++)
        encode_submit_2(&enc, &submits[i]);
    encode_u64(&enc, (uint64_t)fence);

    uint8_t reply[8];
    memset(reply, 0, sizeof(reply));
    struct venus_instance_real *instance =
        self->device->physical_device->instance;
    int wire_result = enc.failed ? -1 :
        venus_wire_call(instance->wire, command, command_size,
                        reply, sizeof(reply));
    free(command);
    if (wire_result < 0) {
        printf("[VN] QueueSubmit2 wire failure %d\n", wire_result);
        return VK_ERROR_DEVICE_LOST;
    }

    uint32_t command_type = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    memcpy(&command_type, reply, sizeof(command_type));
    memcpy(&result, reply + 4, sizeof(result));
    if (command_type != VN_CMD_QUEUE_SUBMIT_2 || result != VK_SUCCESS)
        printf("[VN] QueueSubmit2 reply command=%u result=%d\n",
               command_type, result);
    return command_type == VN_CMD_QUEUE_SUBMIT_2
        ? (VkResult)result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdCopyBuffer2(VkCommandBuffer command_buffer,
                          const VkCopyBufferInfo2 *copy_info)
{
    if (!command_buffer || !copy_info ||
        copy_info->sType != VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2 ||
        !copy_info->srcBuffer || !copy_info->dstBuffer ||
        !copy_info->regionCount || !copy_info->pRegions ||
        copy_info->regionCount > (UINT32_MAX - 64u) / 36u)
        return;
    for (uint32_t i = 0; i < copy_info->regionCount; i++) {
        if (copy_info->pRegions[i].sType !=
            VK_STRUCTURE_TYPE_BUFFER_COPY_2)
            return;
    }
    uint32_t command_size = 64u + copy_info->regionCount * 36u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_COPY_BUFFER_2);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pCopyBufferInfo */
    encode_u32(&enc, VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2);
    encode_u64(&enc, 0); /* info pNext */
    encode_u64(&enc, (uint64_t)copy_info->srcBuffer);
    encode_u64(&enc, (uint64_t)copy_info->dstBuffer);
    encode_u32(&enc, copy_info->regionCount);
    encode_u64(&enc, copy_info->regionCount);
    for (uint32_t i = 0; i < copy_info->regionCount; i++) {
        const VkBufferCopy2 *region = &copy_info->pRegions[i];
        encode_u32(&enc, VK_STRUCTURE_TYPE_BUFFER_COPY_2);
        encode_u64(&enc, 0); /* region pNext */
        encode_u64(&enc, region->srcOffset);
        encode_u64(&enc, region->dstOffset);
        encode_u64(&enc, region->size);
    }
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, command_size);
    free(command);
}

static void encode_memory_barrier_2(struct command_encoder *enc,
                                    const VkMemoryBarrier2 *barrier)
{
    encode_u32(enc, VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    encode_u64(enc, 0);
    encode_u64(enc, barrier->srcStageMask);
    encode_u64(enc, barrier->srcAccessMask);
    encode_u64(enc, barrier->dstStageMask);
    encode_u64(enc, barrier->dstAccessMask);
}

static void encode_buffer_barrier_2(struct command_encoder *enc,
                                    const VkBufferMemoryBarrier2 *barrier)
{
    encode_u32(enc, VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2);
    encode_u64(enc, 0);
    encode_u64(enc, barrier->srcStageMask);
    encode_u64(enc, barrier->srcAccessMask);
    encode_u64(enc, barrier->dstStageMask);
    encode_u64(enc, barrier->dstAccessMask);
    encode_u32(enc, barrier->srcQueueFamilyIndex);
    encode_u32(enc, barrier->dstQueueFamilyIndex);
    encode_u64(enc, (uint64_t)barrier->buffer);
    encode_u64(enc, barrier->offset);
    encode_u64(enc, barrier->size);
}

static void encode_image_barrier_2(struct command_encoder *enc,
                                   const VkImageMemoryBarrier2 *barrier)
{
    encode_u32(enc, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    encode_u64(enc, 0);
    encode_u64(enc, barrier->srcStageMask);
    encode_u64(enc, barrier->srcAccessMask);
    encode_u64(enc, barrier->dstStageMask);
    encode_u64(enc, barrier->dstAccessMask);
    encode_u32(enc, (uint32_t)barrier->oldLayout);
    encode_u32(enc, (uint32_t)barrier->newLayout);
    encode_u32(enc, barrier->srcQueueFamilyIndex);
    encode_u32(enc, barrier->dstQueueFamilyIndex);
    encode_u64(enc, (uint64_t)barrier->image);
    encode_u32(enc, barrier->subresourceRange.aspectMask);
    encode_u32(enc, barrier->subresourceRange.baseMipLevel);
    encode_u32(enc, barrier->subresourceRange.levelCount);
    encode_u32(enc, barrier->subresourceRange.baseArrayLayer);
    encode_u32(enc, barrier->subresourceRange.layerCount);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdPipelineBarrier2(VkCommandBuffer command_buffer,
                               const VkDependencyInfo *dependency_info)
{
    if (!command_buffer || !dependency_info || dependency_info->pNext ||
        dependency_info->sType != VK_STRUCTURE_TYPE_DEPENDENCY_INFO ||
        (dependency_info->memoryBarrierCount &&
         !dependency_info->pMemoryBarriers) ||
        (dependency_info->bufferMemoryBarrierCount &&
         !dependency_info->pBufferMemoryBarriers) ||
        (dependency_info->imageMemoryBarrierCount &&
         !dependency_info->pImageMemoryBarriers))
        return;
    for (uint32_t i = 0; i < dependency_info->memoryBarrierCount; i++)
        if (dependency_info->pMemoryBarriers[i].sType !=
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 ||
            dependency_info->pMemoryBarriers[i].pNext)
            return;
    for (uint32_t i = 0; i < dependency_info->bufferMemoryBarrierCount; i++)
        if (dependency_info->pBufferMemoryBarriers[i].sType !=
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 ||
            dependency_info->pBufferMemoryBarriers[i].pNext ||
            !dependency_info->pBufferMemoryBarriers[i].buffer)
            return;
    for (uint32_t i = 0; i < dependency_info->imageMemoryBarrierCount; i++)
        if (dependency_info->pImageMemoryBarriers[i].sType !=
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 ||
            dependency_info->pImageMemoryBarriers[i].pNext ||
            !dependency_info->pImageMemoryBarriers[i].image)
            return;

    uint64_t size = 76u +
        (uint64_t)dependency_info->memoryBarrierCount * 44u +
        (uint64_t)dependency_info->bufferMemoryBarrierCount * 76u +
        (uint64_t)dependency_info->imageMemoryBarrierCount * 88u;
    if (size > UINT32_MAX)
        return;
    uint8_t *command = malloc((uint32_t)size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = (uint32_t)size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_PIPELINE_BARRIER_2);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pDependencyInfo */
    encode_u32(&enc, VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    encode_u64(&enc, 0);
    encode_u32(&enc, dependency_info->dependencyFlags);
    encode_u32(&enc, dependency_info->memoryBarrierCount);
    encode_u64(&enc, dependency_info->memoryBarrierCount);
    for (uint32_t i = 0; i < dependency_info->memoryBarrierCount; i++)
        encode_memory_barrier_2(&enc, &dependency_info->pMemoryBarriers[i]);
    encode_u32(&enc, dependency_info->bufferMemoryBarrierCount);
    encode_u64(&enc, dependency_info->bufferMemoryBarrierCount);
    for (uint32_t i = 0; i < dependency_info->bufferMemoryBarrierCount; i++)
        encode_buffer_barrier_2(
            &enc, &dependency_info->pBufferMemoryBarriers[i]);
    encode_u32(&enc, dependency_info->imageMemoryBarrierCount);
    encode_u64(&enc, dependency_info->imageMemoryBarrierCount);
    for (uint32_t i = 0; i < dependency_info->imageMemoryBarrierCount; i++)
        encode_image_barrier_2(
            &enc, &dependency_info->pImageMemoryBarriers[i]);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, (uint32_t)size);
    free(command);
}

static void encode_buffer_image_copy_2(struct command_encoder *enc,
                                       const VkBufferImageCopy2 *region)
{
    encode_u32(enc, VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2);
    encode_u64(enc, 0);
    encode_u64(enc, region->bufferOffset);
    encode_u32(enc, region->bufferRowLength);
    encode_u32(enc, region->bufferImageHeight);
    encode_u32(enc, region->imageSubresource.aspectMask);
    encode_u32(enc, region->imageSubresource.mipLevel);
    encode_u32(enc, region->imageSubresource.baseArrayLayer);
    encode_u32(enc, region->imageSubresource.layerCount);
    encode_u32(enc, (uint32_t)region->imageOffset.x);
    encode_u32(enc, (uint32_t)region->imageOffset.y);
    encode_u32(enc, (uint32_t)region->imageOffset.z);
    encode_u32(enc, region->imageExtent.width);
    encode_u32(enc, region->imageExtent.height);
    encode_u32(enc, region->imageExtent.depth);
}

static void command_copy_buffer_image(VkCommandBuffer command_buffer,
                                      uint32_t command_type,
                                      VkBuffer buffer, VkImage image,
                                      VkImageLayout image_layout,
                                      uint32_t region_count,
                                      const VkBufferImageCopy2 *regions)
{
    if (!command_buffer || !buffer || !image || !region_count || !regions ||
        region_count > (UINT32_MAX - 68u) / 68u)
        return;
    for (uint32_t i = 0; i < region_count; i++)
        if (regions[i].sType != VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 ||
            regions[i].pNext)
            return;
    uint32_t command_size = 68u + region_count * 68u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, command_type);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1);
    encode_u32(&enc, command_type == VN_CMD_COPY_BUFFER_TO_IMAGE_2
        ? VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2
        : VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2);
    encode_u64(&enc, 0);
    if (command_type == VN_CMD_COPY_BUFFER_TO_IMAGE_2) {
        encode_u64(&enc, (uint64_t)buffer);
        encode_u64(&enc, (uint64_t)image);
        encode_u32(&enc, (uint32_t)image_layout);
    } else {
        encode_u64(&enc, (uint64_t)image);
        encode_u32(&enc, (uint32_t)image_layout);
        encode_u64(&enc, (uint64_t)buffer);
    }
    encode_u32(&enc, region_count);
    encode_u64(&enc, region_count);
    for (uint32_t i = 0; i < region_count; i++)
        encode_buffer_image_copy_2(&enc, &regions[i]);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, command_size);
    free(command);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdCopyBufferToImage2(
    VkCommandBuffer command_buffer,
    const VkCopyBufferToImageInfo2 *copy_info)
{
    if (!copy_info || copy_info->pNext ||
        copy_info->sType != VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2)
        return;
    command_copy_buffer_image(command_buffer,
                              VN_CMD_COPY_BUFFER_TO_IMAGE_2,
                              copy_info->srcBuffer, copy_info->dstImage,
                              copy_info->dstImageLayout,
                              copy_info->regionCount, copy_info->pRegions);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdCopyImageToBuffer2(
    VkCommandBuffer command_buffer,
    const VkCopyImageToBufferInfo2 *copy_info)
{
    if (!copy_info || copy_info->pNext ||
        copy_info->sType != VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2)
        return;
    command_copy_buffer_image(command_buffer,
                              VN_CMD_COPY_IMAGE_TO_BUFFER_2,
                              copy_info->dstBuffer, copy_info->srcImage,
                              copy_info->srcImageLayout,
                              copy_info->regionCount, copy_info->pRegions);
}

static void encode_subresource_range(struct command_encoder *enc,
                                     const VkImageSubresourceRange *range)
{
    encode_u32(enc, range->aspectMask);
    encode_u32(enc, range->baseMipLevel);
    encode_u32(enc, range->levelCount);
    encode_u32(enc, range->baseArrayLayer);
    encode_u32(enc, range->layerCount);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdClearColorImage(
    VkCommandBuffer command_buffer, VkImage image, VkImageLayout image_layout,
    const VkClearColorValue *color, uint32_t range_count,
    const VkImageSubresourceRange *ranges)
{
    if (!command_buffer || !image || !color || !range_count || !ranges ||
        range_count > (UINT32_MAX - 76u) / 20u)
        return;
    uint32_t command_size = 76u + range_count * 20u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_CLEAR_COLOR_IMAGE);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)image);
    encode_u32(&enc, (uint32_t)image_layout);
    encode_u64(&enc, 1);
    encode_u32(&enc, 2); /* VkClearColorValue union tag: uint32 */
    encode_u64(&enc, 4);
    encode_bytes(&enc, color, sizeof(*color));
    encode_u32(&enc, range_count);
    encode_u64(&enc, range_count);
    for (uint32_t i = 0; i < range_count; i++)
        encode_subresource_range(&enc, &ranges[i]);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, command_size);
    free(command);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdClearDepthStencilImage(
    VkCommandBuffer command_buffer, VkImage image, VkImageLayout image_layout,
    const VkClearDepthStencilValue *depth_stencil, uint32_t range_count,
    const VkImageSubresourceRange *ranges)
{
    if (!command_buffer || !image || !depth_stencil || !range_count ||
        !ranges || range_count > (UINT32_MAX - 56u) / 20u)
        return;
    uint32_t command_size = 56u + range_count * 20u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_CLEAR_DEPTH_STENCIL_IMAGE);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)image);
    encode_u32(&enc, (uint32_t)image_layout);
    encode_u64(&enc, 1);
    encode_bytes(&enc, depth_stencil, sizeof(*depth_stencil));
    encode_u32(&enc, range_count);
    encode_u64(&enc, range_count);
    for (uint32_t i = 0; i < range_count; i++)
        encode_subresource_range(&enc, &ranges[i]);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, command_size);
    free(command);
}

static void encode_clear_value(struct command_encoder *enc,
                               const VkClearValue *value)
{
    encode_u32(enc, 0); /* VkClearValue union tag: color */
    encode_u32(enc, 2); /* VkClearColorValue union tag: uint32 */
    encode_u64(enc, 4);
    encode_bytes(enc, &value->color, sizeof(value->color));
}

static void encode_rendering_attachment(
    struct command_encoder *enc, const VkRenderingAttachmentInfo *attachment)
{
    encode_u32(enc, VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    encode_u64(enc, 0);
    encode_u64(enc, (uint64_t)attachment->imageView);
    encode_u32(enc, (uint32_t)attachment->imageLayout);
    encode_u32(enc, (uint32_t)attachment->resolveMode);
    encode_u64(enc, (uint64_t)attachment->resolveImageView);
    encode_u32(enc, (uint32_t)attachment->resolveImageLayout);
    encode_u32(enc, (uint32_t)attachment->loadOp);
    encode_u32(enc, (uint32_t)attachment->storeOp);
    encode_clear_value(enc, &attachment->clearValue);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBeginRendering(VkCommandBuffer command_buffer,
                             const VkRenderingInfo *rendering_info)
{
    if (!command_buffer || !rendering_info ||
        rendering_info->sType != VK_STRUCTURE_TYPE_RENDERING_INFO ||
        rendering_info->pNext ||
        (rendering_info->colorAttachmentCount &&
         !rendering_info->pColorAttachments) ||
        rendering_info->colorAttachmentCount > 8)
        return;
    uint32_t attachment_count = rendering_info->colorAttachmentCount +
        (rendering_info->pDepthAttachment ? 1u : 0u) +
        (rendering_info->pStencilAttachment ? 1u : 0u);
    uint32_t command_size = 92u + attachment_count * 80u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_BEGIN_RENDERING);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1);
    encode_u32(&enc, VK_STRUCTURE_TYPE_RENDERING_INFO);
    encode_u64(&enc, 0);
    encode_u32(&enc, rendering_info->flags);
    encode_u32(&enc, (uint32_t)rendering_info->renderArea.offset.x);
    encode_u32(&enc, (uint32_t)rendering_info->renderArea.offset.y);
    encode_u32(&enc, rendering_info->renderArea.extent.width);
    encode_u32(&enc, rendering_info->renderArea.extent.height);
    encode_u32(&enc, rendering_info->layerCount);
    encode_u32(&enc, rendering_info->viewMask);
    encode_u32(&enc, rendering_info->colorAttachmentCount);
    encode_u64(&enc, rendering_info->colorAttachmentCount);
    for (uint32_t i = 0; i < rendering_info->colorAttachmentCount; i++)
        encode_rendering_attachment(
            &enc, &rendering_info->pColorAttachments[i]);
    encode_u64(&enc, rendering_info->pDepthAttachment ? 1u : 0u);
    if (rendering_info->pDepthAttachment)
        encode_rendering_attachment(&enc,
                                    rendering_info->pDepthAttachment);
    encode_u64(&enc, rendering_info->pStencilAttachment ? 1u : 0u);
    if (rendering_info->pStencilAttachment)
        encode_rendering_attachment(&enc,
                                    rendering_info->pStencilAttachment);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, enc.length);
    free(command);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdEndRendering(VkCommandBuffer command_buffer)
{
    if (!command_buffer)
        return;
    uint8_t command[16];
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_END_RENDERING);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBindVertexBuffers2(
    VkCommandBuffer command_buffer, uint32_t first_binding,
    uint32_t binding_count, const VkBuffer *buffers,
    const VkDeviceSize *offsets, const VkDeviceSize *sizes,
    const VkDeviceSize *strides)
{
    if (!command_buffer || !binding_count || !buffers || !offsets ||
        binding_count > (UINT32_MAX - 56u) / 32u)
        return;
    uint32_t payload_arrays = 2u + (sizes ? 1u : 0u) +
        (strides ? 1u : 0u);
    uint32_t command_size = 56u + binding_count * 8u * payload_arrays;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_BIND_VERTEX_BUFFERS_2);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, first_binding);
    encode_u32(&enc, binding_count);
    encode_u64(&enc, binding_count);
    for (uint32_t i = 0; i < binding_count; i++)
        encode_u64(&enc, (uint64_t)buffers[i]);
    encode_u64(&enc, binding_count);
    for (uint32_t i = 0; i < binding_count; i++)
        encode_u64(&enc, offsets[i]);
    encode_u64(&enc, sizes ? binding_count : 0u);
    if (sizes)
        for (uint32_t i = 0; i < binding_count; i++)
            encode_u64(&enc, sizes[i]);
    encode_u64(&enc, strides ? binding_count : 0u);
    if (strides)
        for (uint32_t i = 0; i < binding_count; i++)
            encode_u64(&enc, strides[i]);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, command_size);
    free(command);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBindVertexBuffers(
    VkCommandBuffer command_buffer, uint32_t first_binding,
    uint32_t binding_count, const VkBuffer *buffers,
    const VkDeviceSize *offsets)
{
    if (!command_buffer || !binding_count || !buffers || !offsets ||
        binding_count > (UINT32_MAX - 40u) / 16u)
        return;
    uint32_t command_size = 40u + binding_count * 16u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_BIND_VERTEX_BUFFERS);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, first_binding);
    encode_u32(&enc, binding_count);
    encode_u64(&enc, binding_count);
    for (uint32_t i = 0; i < binding_count; i++)
        encode_u64(&enc, (uint64_t)buffers[i]);
    encode_u64(&enc, binding_count);
    for (uint32_t i = 0; i < binding_count; i++)
        encode_u64(&enc, offsets[i]);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, enc.length);
    free(command);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBindIndexBuffer(VkCommandBuffer command_buffer, VkBuffer buffer,
                              VkDeviceSize offset, VkIndexType index_type)
{
    if (!command_buffer || !buffer)
        return;
    uint8_t command[36];
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_BIND_INDEX_BUFFER);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)buffer);
    encode_u64(&enc, offset);
    encode_u32(&enc, (uint32_t)index_type);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBindIndexBuffer2(VkCommandBuffer command_buffer, VkBuffer buffer,
                               VkDeviceSize offset, VkDeviceSize size,
                               VkIndexType index_type)
{
    if (!command_buffer || !buffer)
        return;
    uint8_t command[44];
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_BIND_INDEX_BUFFER_2);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)buffer);
    encode_u64(&enc, offset);
    encode_u64(&enc, size);
    encode_u32(&enc, (uint32_t)index_type);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

static void submit_u32_command(VkCommandBuffer command_buffer,
                               uint32_t command_type, uint32_t value)
{
    if (!command_buffer)
        return;
    uint8_t command[20];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, command_type);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, value);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBindPipeline(VkCommandBuffer command_buffer,
                           VkPipelineBindPoint bind_point,
                           VkPipeline pipeline)
{
    if (!command_buffer || !pipeline)
        return;
    uint8_t command[28];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_BIND_PIPELINE);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, (uint32_t)bind_point);
    encode_u64(&enc, (uint64_t)pipeline);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetAttachmentFeedbackLoopEnableEXT(
    VkCommandBuffer command_buffer, VkImageAspectFlags aspect_mask)
{
    submit_u32_command(command_buffer,
                       VN_CMD_SET_ATTACHMENT_FEEDBACK_LOOP_ENABLE,
                       aspect_mask);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetCullMode(VkCommandBuffer command_buffer,
                          VkCullModeFlags cull_mode)
{
    submit_u32_command(command_buffer, VN_CMD_SET_CULL_MODE, cull_mode);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetFrontFace(VkCommandBuffer command_buffer,
                           VkFrontFace front_face)
{
    submit_u32_command(command_buffer, VN_CMD_SET_FRONT_FACE,
                       (uint32_t)front_face);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetPrimitiveTopology(VkCommandBuffer command_buffer,
                                   VkPrimitiveTopology topology)
{
    submit_u32_command(command_buffer, VN_CMD_SET_PRIMITIVE_TOPOLOGY,
                       (uint32_t)topology);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetBlendConstants(VkCommandBuffer command_buffer,
                                const float blend_constants[4])
{
    if (!command_buffer || !blend_constants)
        return;
    uint8_t command[40];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_SET_BLEND_CONSTANTS);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 4);
    encode_bytes(&enc, blend_constants, sizeof(float) * 4);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetViewport(VkCommandBuffer command_buffer,
                          uint32_t first_viewport,
                          uint32_t viewport_count,
                          const VkViewport *viewports)
{
    if (!command_buffer || !viewport_count || !viewports ||
        viewport_count > (UINT32_MAX - 32u) / 24u)
        return;
    uint32_t command_size = 32u + viewport_count * 24u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_SET_VIEWPORT);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, first_viewport);
    encode_u32(&enc, viewport_count);
    encode_u64(&enc, viewport_count);
    for (uint32_t i = 0; i < viewport_count; i++) {
        encode_bytes(&enc, &viewports[i].x, sizeof(float));
        encode_bytes(&enc, &viewports[i].y, sizeof(float));
        encode_bytes(&enc, &viewports[i].width, sizeof(float));
        encode_bytes(&enc, &viewports[i].height, sizeof(float));
        encode_bytes(&enc, &viewports[i].minDepth, sizeof(float));
        encode_bytes(&enc, &viewports[i].maxDepth, sizeof(float));
    }
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, enc.length);
    free(command);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetScissor(VkCommandBuffer command_buffer,
                         uint32_t first_scissor,
                         uint32_t scissor_count,
                         const VkRect2D *scissors)
{
    if (!command_buffer || !scissor_count || !scissors ||
        scissor_count > (UINT32_MAX - 32u) / 16u)
        return;
    uint32_t command_size = 32u + scissor_count * 16u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_SET_SCISSOR);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, first_scissor);
    encode_u32(&enc, scissor_count);
    encode_u64(&enc, scissor_count);
    for (uint32_t i = 0; i < scissor_count; i++) {
        encode_u32(&enc, (uint32_t)scissors[i].offset.x);
        encode_u32(&enc, (uint32_t)scissors[i].offset.y);
        encode_u32(&enc, scissors[i].extent.width);
        encode_u32(&enc, scissors[i].extent.height);
    }
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, enc.length);
    free(command);
}

static void submit_float_command(VkCommandBuffer command_buffer,
                                 uint32_t command_type, float value)
{
    if (!command_buffer)
        return;
    uint8_t command[20];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, command_type);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_bytes(&enc, &value, sizeof(value));
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

static void submit_two_u32_command(VkCommandBuffer command_buffer,
                                   uint32_t command_type,
                                   uint32_t first, uint32_t second)
{
    if (!command_buffer)
        return;
    uint8_t command[24];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, command_type);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, first);
    encode_u32(&enc, second);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetLineWidth(VkCommandBuffer command_buffer, float line_width)
{
    submit_float_command(command_buffer, VN_CMD_SET_LINE_WIDTH, line_width);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetDepthBias(VkCommandBuffer command_buffer,
                           float constant_factor, float clamp,
                           float slope_factor)
{
    if (!command_buffer)
        return;
    uint8_t command[28];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_SET_DEPTH_BIAS);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_bytes(&enc, &constant_factor, sizeof(constant_factor));
    encode_bytes(&enc, &clamp, sizeof(clamp));
    encode_bytes(&enc, &slope_factor, sizeof(slope_factor));
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetDepthBounds(VkCommandBuffer command_buffer,
                             float min_depth_bounds,
                             float max_depth_bounds)
{
    if (!command_buffer)
        return;
    uint8_t command[24];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_SET_DEPTH_BOUNDS);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_bytes(&enc, &min_depth_bounds, sizeof(min_depth_bounds));
    encode_bytes(&enc, &max_depth_bounds, sizeof(max_depth_bounds));
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetStencilCompareMask(VkCommandBuffer command_buffer,
                                    VkStencilFaceFlags face_mask,
                                    uint32_t compare_mask)
{
    submit_two_u32_command(command_buffer, VN_CMD_SET_STENCIL_COMPARE_MASK,
                           face_mask, compare_mask);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetStencilWriteMask(VkCommandBuffer command_buffer,
                                  VkStencilFaceFlags face_mask,
                                  uint32_t write_mask)
{
    submit_two_u32_command(command_buffer, VN_CMD_SET_STENCIL_WRITE_MASK,
                           face_mask, write_mask);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetStencilReference(VkCommandBuffer command_buffer,
                                  VkStencilFaceFlags face_mask,
                                  uint32_t reference)
{
    submit_two_u32_command(command_buffer, VN_CMD_SET_STENCIL_REFERENCE,
                           face_mask, reference);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetViewportWithCount(VkCommandBuffer command_buffer,
                                   uint32_t viewport_count,
                                   const VkViewport *viewports)
{
    if (!command_buffer || !viewport_count || !viewports ||
        viewport_count > (UINT32_MAX - 28u) / 24u)
        return;
    uint32_t command_size = 28u + viewport_count * 24u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_SET_VIEWPORT_WITH_COUNT);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, viewport_count);
    encode_u64(&enc, viewport_count);
    for (uint32_t i = 0; i < viewport_count; i++) {
        encode_bytes(&enc, &viewports[i].x, sizeof(float));
        encode_bytes(&enc, &viewports[i].y, sizeof(float));
        encode_bytes(&enc, &viewports[i].width, sizeof(float));
        encode_bytes(&enc, &viewports[i].height, sizeof(float));
        encode_bytes(&enc, &viewports[i].minDepth, sizeof(float));
        encode_bytes(&enc, &viewports[i].maxDepth, sizeof(float));
    }
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, command_size);
    free(command);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetScissorWithCount(VkCommandBuffer command_buffer,
                                  uint32_t scissor_count,
                                  const VkRect2D *scissors)
{
    if (!command_buffer || !scissor_count || !scissors ||
        scissor_count > (UINT32_MAX - 28u) / 16u)
        return;
    uint32_t command_size = 28u + scissor_count * 16u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_SET_SCISSOR_WITH_COUNT);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, scissor_count);
    encode_u64(&enc, scissor_count);
    for (uint32_t i = 0; i < scissor_count; i++) {
        encode_u32(&enc, (uint32_t)scissors[i].offset.x);
        encode_u32(&enc, (uint32_t)scissors[i].offset.y);
        encode_u32(&enc, scissors[i].extent.width);
        encode_u32(&enc, scissors[i].extent.height);
    }
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, command_size);
    free(command);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdDraw(VkCommandBuffer command_buffer, uint32_t vertex_count,
                   uint32_t instance_count, uint32_t first_vertex,
                   uint32_t first_instance)
{
    if (!command_buffer)
        return;
    uint8_t command[32];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_DRAW);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, vertex_count);
    encode_u32(&enc, instance_count);
    encode_u32(&enc, first_vertex);
    encode_u32(&enc, first_instance);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdDrawIndexed(VkCommandBuffer command_buffer,
                          uint32_t index_count, uint32_t instance_count,
                          uint32_t first_index, int32_t vertex_offset,
                          uint32_t first_instance)
{
    if (!command_buffer)
        return;
    uint8_t command[36];
    struct command_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_DRAW_INDEXED);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, index_count);
    encode_u32(&enc, instance_count);
    encode_u32(&enc, first_index);
    encode_u32(&enc, (uint32_t)vertex_offset);
    encode_u32(&enc, first_instance);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, sizeof(command));
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBindDescriptorSets(
    VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
    VkPipelineLayout layout, uint32_t first_set,
    uint32_t descriptor_set_count, const VkDescriptorSet *descriptor_sets,
    uint32_t dynamic_offset_count, const uint32_t *dynamic_offsets)
{
    if (!command_buffer || !layout ||
        (descriptor_set_count && !descriptor_sets) ||
        (dynamic_offset_count && !dynamic_offsets) ||
        descriptor_set_count > 32 || dynamic_offset_count > 256)
        return;
    uint32_t command_size = 56u + descriptor_set_count * 8u +
        dynamic_offset_count * 4u;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_BIND_DESCRIPTOR_SETS);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, (uint32_t)bind_point);
    encode_u64(&enc, (uint64_t)layout);
    encode_u32(&enc, first_set);
    encode_u32(&enc, descriptor_set_count);
    encode_u64(&enc, descriptor_set_count);
    for (uint32_t i = 0; i < descriptor_set_count; i++)
        encode_u64(&enc, (uint64_t)descriptor_sets[i]);
    encode_u32(&enc, dynamic_offset_count);
    encode_u64(&enc, dynamic_offset_count);
    for (uint32_t i = 0; i < dynamic_offset_count; i++)
        encode_u32(&enc, dynamic_offsets[i]);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, command_size);
    free(command);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdPushConstants(VkCommandBuffer command_buffer,
                            VkPipelineLayout layout,
                            VkShaderStageFlags stage_flags,
                            uint32_t offset, uint32_t size,
                            const void *values)
{
    if (!command_buffer || !layout || !size || !values ||
        size > UINT32_MAX - 44u)
        return;
    uint32_t command_size = 44u + size;
    uint8_t *command = malloc(command_size);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;
    encode_u32(&enc, VN_CMD_PUSH_CONSTANTS);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)layout);
    encode_u32(&enc, stage_flags);
    encode_u32(&enc, offset);
    encode_u32(&enc, size);
    encode_u64(&enc, size);
    encode_bytes(&enc, values, size);
    if (!enc.failed)
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, enc.length);
    free(command);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_CmdPushDescriptorSet(
    VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
    VkPipelineLayout layout, uint32_t set, uint32_t write_count,
    const VkWriteDescriptorSet *writes)
{
    if (!command_buffer || !layout || (write_count && !writes))
        return;
    struct venus_command_buffer_real *self =
        (struct venus_command_buffer_real *)command_buffer;

    struct command_encoder size_enc = { .capacity = UINT32_MAX };
    encode_u32(&size_enc, VN_CMD_PUSH_DESCRIPTOR_SET);
    encode_u32(&size_enc, 0);
    encode_u64(&size_enc, self->object_id);
    encode_u32(&size_enc, (uint32_t)bind_point);
    encode_u64(&size_enc, (uint64_t)layout);
    encode_u32(&size_enc, set);
    encode_u32(&size_enc, write_count);
    encode_u64(&size_enc, write_count);
    for (uint32_t i = 0; i < write_count; i++)
        encode_write_descriptor_set(&size_enc, &writes[i]);
    if (size_enc.failed || size_enc.length > (16u << 20))
        return;

    uint8_t *command = malloc(size_enc.length);
    if (!command)
        return;
    struct command_encoder enc = {
        .data = command,
        .capacity = size_enc.length,
    };
    encode_u32(&enc, VN_CMD_PUSH_DESCRIPTOR_SET);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u32(&enc, (uint32_t)bind_point);
    encode_u64(&enc, (uint64_t)layout);
    encode_u32(&enc, set);
    encode_u32(&enc, write_count);
    encode_u64(&enc, write_count);
    for (uint32_t i = 0; i < write_count; i++)
        encode_write_descriptor_set(&enc, &writes[i]);
    if (!enc.failed) {
        (void)venus_wire_submit_async(
            self->pool->device->physical_device->instance->wire,
            command, enc.length);
    }
    free(command);
}
