#include "venus_real.h"

extern void *malloc(unsigned long);
extern void free(void *);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern int printf(const char *, ...);

#define VN_CMD_BIND_BUFFER_MEMORY 28u
#define VN_CMD_GET_BUFFER_MEMORY_REQUIREMENTS 30u
#define VN_CMD_BIND_IMAGE_MEMORY 29u
#define VN_CMD_GET_IMAGE_MEMORY_REQUIREMENTS 31u
#define VN_CMD_CREATE_BUFFER 50u
#define VN_CMD_DESTROY_BUFFER 51u
#define VN_CMD_CREATE_BUFFER_VIEW 52u
#define VN_CMD_DESTROY_BUFFER_VIEW 53u
#define VN_CMD_CREATE_IMAGE 54u
#define VN_CMD_DESTROY_IMAGE 55u
#define VN_CMD_CREATE_IMAGE_VIEW 57u
#define VN_CMD_DESTROY_IMAGE_VIEW 58u
#define VN_CMD_CREATE_SHADER_MODULE 59u
#define VN_CMD_DESTROY_SHADER_MODULE 60u
#define VN_CMD_CREATE_SAMPLER 70u
#define VN_CMD_DESTROY_SAMPLER 71u
#define VN_CMD_GET_BUFFER_MEMORY_REQUIREMENTS_2 145u
#define VN_CMD_GET_IMAGE_MEMORY_REQUIREMENTS_2 144u
#define VN_CMD_ALLOCATE_MEMORY 21u
#define VN_CMD_FREE_MEMORY 22u
#define VN_COMMAND_GENERATE_REPLY 1u

struct venus_memory_real {
    uint64_t object_id;
    struct venus_device_real *device;
    uint32_t resource_id;
    void *mapping;
    uint64_t size;
    int mapped;
};

struct resource_encoder {
    uint8_t *data;
    uint32_t capacity;
    uint32_t length;
    int failed;
};

static void encode_bytes(struct resource_encoder *enc, const void *data,
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

static void encode_u32(struct resource_encoder *enc, uint32_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static void encode_u64(struct resource_encoder *enc, uint64_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static void encode_float(struct resource_encoder *enc, float value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static VkResult result_call(struct venus_device_real *device,
                            const void *command, uint32_t command_size,
                            uint32_t expected_command)
{
    uint8_t reply[8];
    memset(reply, 0, sizeof(reply));
    if (venus_wire_call(device->physical_device->instance->wire,
                        command, command_size, reply, sizeof(reply)) < 0)
        return VK_ERROR_DEVICE_LOST;
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    memcpy(&returned_command, reply, sizeof(returned_command));
    memcpy(&result, reply + 4, sizeof(result));
    return returned_command == expected_command
        ? (VkResult)result : VK_ERROR_DEVICE_LOST;
}

static const VkBaseInStructure *
memory_allocate_next_supported(const VkBaseInStructure *next)
{
    while (next) {
        switch (next->sType) {
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
        case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO:
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO:
        case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO:
            return next;
        default:
            next = next->pNext;
            break;
        }
    }
    return 0;
}

static void
encode_memory_allocate_pnext(struct resource_encoder *enc,
                             const VkBaseInStructure *next)
{
    next = memory_allocate_next_supported(next);
    if (!next) {
        encode_u64(enc, 0);
        return;
    }

    encode_u64(enc, 1);
    encode_u32(enc, (uint32_t)next->sType);
    encode_memory_allocate_pnext(enc, next->pNext);

    switch (next->sType) {
    case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO: {
        const VkExportMemoryAllocateInfo *info =
            (const VkExportMemoryAllocateInfo *)next;
        encode_u32(enc, info->handleTypes);
        break;
    }
    case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO: {
        const VkMemoryAllocateFlagsInfo *info =
            (const VkMemoryAllocateFlagsInfo *)next;
        encode_u32(enc, info->flags);
        encode_u32(enc, info->deviceMask);
        break;
    }
    case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO: {
        const VkMemoryDedicatedAllocateInfo *info =
            (const VkMemoryDedicatedAllocateInfo *)next;
        encode_u64(enc, (uint64_t)info->image);
        encode_u64(enc, (uint64_t)info->buffer);
        break;
    }
    case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO: {
        const VkMemoryOpaqueCaptureAddressAllocateInfo *info =
            (const VkMemoryOpaqueCaptureAddressAllocateInfo *)next;
        encode_u64(enc, info->opaqueCaptureAddress);
        break;
    }
    default:
        enc->failed = 1;
        break;
    }
}

static void
encode_allocate_memory_command(struct resource_encoder *enc,
                               const struct venus_device_real *device,
                               const VkMemoryAllocateInfo *allocate_info,
                               uint64_t object_id)
{
    encode_u32(enc, VN_CMD_ALLOCATE_MEMORY);
    encode_u32(enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(enc, device->object_id);
    encode_u64(enc, 1); /* pAllocateInfo */
    encode_u32(enc, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    encode_memory_allocate_pnext(
        enc, (const VkBaseInStructure *)allocate_info->pNext);
    encode_u64(enc, allocate_info->allocationSize);
    encode_u32(enc, allocate_info->memoryTypeIndex);
    encode_u64(enc, 0); /* pAllocator */
    encode_u64(enc, 1); /* pMemory */
    encode_u64(enc, object_id);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateBuffer(VkDevice device,
                        const VkBufferCreateInfo *create_info,
                        const VkAllocationCallbacks *allocator,
                        VkBuffer *buffer)
{
    if (!device || !create_info || !buffer || allocator ||
        create_info->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO ||
        !create_info->size ||
        (create_info->sharingMode == VK_SHARING_MODE_CONCURRENT &&
         (!create_info->queueFamilyIndexCount ||
          !create_info->pQueueFamilyIndices)))
        return VK_ERROR_INITIALIZATION_FAILED;

    void *object = malloc(1);
    if (!object)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    uint32_t family_count = create_info->sharingMode ==
            VK_SHARING_MODE_CONCURRENT
        ? create_info->queueFamilyIndexCount : 0;
    if (family_count > (UINT32_MAX - 108u) / 4u) {
        free(object);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint32_t command_size = 108u + family_count * 4u;
    uint8_t *command = malloc(command_size);
    if (!command) {
        free(object);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    struct resource_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_CREATE_BUFFER);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pCreateInfo */
    encode_u32(&enc, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    encode_u64(&enc, 1); /* pNext */
    encode_u32(&enc, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
    encode_u64(&enc, 0); /* external-memory pNext */
    encode_u32(&enc, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    encode_u32(&enc, create_info->flags);
    encode_u64(&enc, create_info->size);
    encode_u32(&enc, create_info->usage);
    encode_u32(&enc, (uint32_t)create_info->sharingMode);
    encode_u32(&enc, create_info->queueFamilyIndexCount);
    encode_u64(&enc, family_count);
    for (uint32_t i = 0; i < family_count; i++)
        encode_u32(&enc, create_info->pQueueFamilyIndices[i]);
    encode_u64(&enc, 0); /* pAllocator */
    encode_u64(&enc, 1); /* pBuffer */
    encode_u64(&enc, object_id);

    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = enc.failed ? -1 :
        venus_wire_call(self->physical_device->instance->wire,
                        command, command_size, reply, sizeof(reply));
    free(command);
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t present = 0;
    uint64_t returned_id = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&present, reply + 8, 8);
    memcpy(&returned_id, reply + 16, 8);
    if (wire_result < 0 || returned_command != VN_CMD_CREATE_BUFFER ||
        (result == VK_SUCCESS && (!present || returned_id != object_id)))
        result = VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        free(object);
        return (VkResult)result;
    }
    *buffer = (VkBuffer)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyBuffer(VkDevice device, VkBuffer buffer,
                         const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !buffer)
        return;
    uint8_t command[32];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_DESTROY_BUFFER);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)buffer);
    encode_u64(&enc, 0); /* pAllocator */
    if (!enc.failed)
        (void)venus_wire_submit_async(self->physical_device->instance->wire,
                                      command, sizeof(command));
    free((void *)(uintptr_t)buffer);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateBufferView(VkDevice device,
                            const VkBufferViewCreateInfo *create_info,
                            const VkAllocationCallbacks *allocator,
                            VkBufferView *view)
{
    if (!device || !create_info || !view || allocator ||
        create_info->sType != VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO ||
        !create_info->buffer || create_info->format == VK_FORMAT_UNDEFINED)
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkBufferUsageFlags2CreateInfoKHR *usage_info = 0;
    for (const VkBaseInStructure *next =
             (const VkBaseInStructure *)create_info->pNext;
         next; next = next->pNext) {
        if (next->sType ==
            VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR) {
            usage_info = (const VkBufferUsageFlags2CreateInfoKHR *)next;
            break;
        }
    }

    uint32_t command_size = usage_info ? 112u : 92u;
    void *object = malloc(1);
    uint8_t *command = malloc(command_size);
    if (!object || !command) {
        free(command);
        free(object);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    uint64_t object_id = (uint64_t)(uintptr_t)object;
    struct resource_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_CREATE_BUFFER_VIEW);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pCreateInfo */
    encode_u32(&enc, VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
    if (usage_info) {
        encode_u64(&enc, 1);
        encode_u32(&enc,
                   VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR);
        encode_u64(&enc, 0);
        encode_u64(&enc, usage_info->usage);
    } else {
        encode_u64(&enc, 0);
    }
    encode_u32(&enc, create_info->flags);
    encode_u64(&enc, (uint64_t)create_info->buffer);
    encode_u32(&enc, (uint32_t)create_info->format);
    encode_u64(&enc, create_info->offset);
    encode_u64(&enc, create_info->range);
    encode_u64(&enc, 0); /* pAllocator */
    encode_u64(&enc, 1); /* pView */
    encode_u64(&enc, object_id);

    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = enc.failed ? -1 :
        venus_wire_call(self->physical_device->instance->wire,
                        command, command_size, reply, sizeof(reply));
    free(command);

    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t present = 0;
    uint64_t returned_id = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&present, reply + 8, 8);
    memcpy(&returned_id, reply + 16, 8);
    if (wire_result < 0 ||
        returned_command != VN_CMD_CREATE_BUFFER_VIEW ||
        (result == VK_SUCCESS && (!present || returned_id != object_id)))
        result = VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        free(object);
        return (VkResult)result;
    }

    *view = (VkBufferView)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyBufferView(VkDevice device, VkBufferView view,
                             const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !view)
        return;
    uint8_t command[32];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_DESTROY_BUFFER_VIEW);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)view);
    encode_u64(&enc, 0); /* pAllocator */
    if (!enc.failed)
        (void)venus_wire_submit_async(self->physical_device->instance->wire,
                                      command, sizeof(command));
    free((void *)(uintptr_t)view);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_GetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                       VkMemoryRequirements *requirements)
{
    if (!device || !buffer || !requirements)
        return;
    struct venus_device_real *self = (struct venus_device_real *)device;
    uint8_t command[32];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_GET_BUFFER_MEMORY_REQUIREMENTS);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)buffer);
    encode_u64(&enc, 1); /* pMemoryRequirements */
    uint8_t reply[32];
    memset(reply, 0, sizeof(reply));
    if (enc.failed ||
        venus_wire_call(self->physical_device->instance->wire,
                        command, sizeof(command), reply, sizeof(reply)) < 0)
        return;
    uint32_t returned_command = 0;
    uint64_t present = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&present, reply + 4, 8);
    if (returned_command != VN_CMD_GET_BUFFER_MEMORY_REQUIREMENTS || !present)
        return;
    memcpy(&requirements->size, reply + 12, 8);
    memcpy(&requirements->alignment, reply + 20, 8);
    memcpy(&requirements->memoryTypeBits, reply + 28, 4);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_GetBufferMemoryRequirements2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements)
{
    if (!device || !info || !requirements ||
        info->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 ||
        requirements->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2)
        return;
    struct venus_device_real *self = (struct venus_device_real *)device;
    uint8_t command[64];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_GET_BUFFER_MEMORY_REQUIREMENTS_2);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pInfo */
    encode_u32(&enc, VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2);
    encode_u64(&enc, 0); /* info pNext */
    encode_u64(&enc, (uint64_t)info->buffer);
    encode_u64(&enc, 1); /* pMemoryRequirements */
    encode_u32(&enc, VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2);
    encode_u64(&enc, 0); /* output pNext */
    uint8_t reply[44];
    memset(reply, 0, sizeof(reply));
    if (!enc.failed &&
        venus_wire_call(self->physical_device->instance->wire,
                        command, sizeof(command), reply, sizeof(reply)) == 0) {
        uint32_t returned_command = 0;
        uint64_t present = 0;
        uint32_t returned_type = 0;
        uint64_t returned_pnext = 0;
        memcpy(&returned_command, reply, 4);
        memcpy(&present, reply + 4, 8);
        memcpy(&returned_type, reply + 12, 4);
        memcpy(&returned_pnext, reply + 16, 8);
        if (returned_command == VN_CMD_GET_BUFFER_MEMORY_REQUIREMENTS_2 &&
            present && returned_type == VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 &&
            !returned_pnext) {
            memcpy(&requirements->memoryRequirements.size, reply + 24, 8);
            memcpy(&requirements->memoryRequirements.alignment, reply + 32, 8);
            memcpy(&requirements->memoryRequirements.memoryTypeBits,
                   reply + 40, 4);
        }
    }
    for (VkBaseOutStructure *next = (VkBaseOutStructure *)requirements->pNext;
         next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            VkMemoryDedicatedRequirements *dedicated =
                (VkMemoryDedicatedRequirements *)next;
            dedicated->prefersDedicatedAllocation = VK_FALSE;
            dedicated->requiresDedicatedAllocation = VK_FALSE;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_BindBufferMemory(VkDevice device, VkBuffer buffer,
                            VkDeviceMemory memory, VkDeviceSize memory_offset)
{
    if (!device || !buffer || !memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t command[40];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_BIND_BUFFER_MEMORY);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)buffer);
    encode_u64(&enc, (uint64_t)memory);
    encode_u64(&enc, memory_offset);
    return enc.failed ? VK_ERROR_INITIALIZATION_FAILED
        : result_call(self, command, sizeof(command),
                      VN_CMD_BIND_BUFFER_MEMORY);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_AllocateMemory(VkDevice device,
                          const VkMemoryAllocateInfo *allocate_info,
                          const VkAllocationCallbacks *allocator,
                          VkDeviceMemory *memory)
{
    if (!device || !allocate_info || !memory || allocator ||
        allocate_info->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO ||
        !allocate_info->allocationSize)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device_real *self = (struct venus_device_real *)device;
    struct venus_memory_real *object = malloc(sizeof(*object));
    if (!object)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(object, 0, sizeof(*object));
    object->object_id = (uint64_t)(uintptr_t)object;
    object->device = self;
    object->size = allocate_info->allocationSize;

    struct resource_encoder size_enc = {
        .capacity = UINT32_MAX,
    };
    encode_allocate_memory_command(&size_enc, self, allocate_info,
                                   object->object_id);
    uint8_t *command = size_enc.failed ? 0 : malloc(size_enc.length);
    if (!command) {
        free(object);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    struct resource_encoder enc = {
        .data = command,
        .capacity = size_enc.length,
    };
    encode_allocate_memory_command(&enc, self, allocate_info,
                                   object->object_id);

    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = enc.failed ? -1 :
        venus_wire_call(self->physical_device->instance->wire,
                        command, enc.length, reply, sizeof(reply));
    free(command);
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t present = 0;
    uint64_t returned_id = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&present, reply + 8, 8);
    memcpy(&returned_id, reply + 16, 8);
    if (wire_result < 0 || returned_command != VN_CMD_ALLOCATE_MEMORY ||
        (result == VK_SUCCESS &&
         (!present || returned_id != object->object_id)))
        result = VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        free(object);
        return (VkResult)result;
    }

    if (venus_wire_resource_create(self->physical_device->instance->wire,
                                   object->size, object->object_id,
                                   &object->resource_id,
                                   &object->mapping) < 0) {
        uint8_t free_command[32];
        struct resource_encoder free_enc = {
            .data = free_command,
            .capacity = sizeof(free_command),
        };
        encode_u32(&free_enc, VN_CMD_FREE_MEMORY);
        encode_u32(&free_enc, 0);
        encode_u64(&free_enc, self->object_id);
        encode_u64(&free_enc, object->object_id);
        encode_u64(&free_enc, 0); /* pAllocator */
        if (!free_enc.failed)
            (void)venus_wire_submit_async(
                self->physical_device->instance->wire,
                free_command, sizeof(free_command));
        free(object);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    memset(object->mapping, 0, object->size);
    __asm__ volatile("mfence" ::: "memory");
    *memory = (VkDeviceMemory)object->object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_FreeMemory(VkDevice device, VkDeviceMemory memory,
                      const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !memory)
        return;
    struct venus_memory_real *object =
        (struct venus_memory_real *)(uintptr_t)memory;
    uint8_t command[32];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_FREE_MEMORY);
    encode_u32(&enc, 0);
    encode_u64(&enc, object->device->object_id);
    encode_u64(&enc, object->object_id);
    encode_u64(&enc, 0); /* pAllocator */
    int submitted = -1;
    if (!enc.failed)
        submitted = venus_wire_submit_async(
            object->device->physical_device->instance->wire,
            command, sizeof(command));
    if (submitted == 0) {
        (void)venus_wire_resource_destroy(
            object->device->physical_device->instance->wire,
            object->resource_id);
    }
    free(object);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_MapMemory(VkDevice device, VkDeviceMemory memory,
                     VkDeviceSize offset, VkDeviceSize size,
                     VkMemoryMapFlags flags, void **data)
{
    if (!device || !memory || !data || flags)
        return VK_ERROR_MEMORY_MAP_FAILED;
    struct venus_memory_real *object =
        (struct venus_memory_real *)(uintptr_t)memory;
    if (object->device != (struct venus_device_real *)device ||
        offset > object->size ||
        (size != VK_WHOLE_SIZE && size > object->size - offset))
        return VK_ERROR_MEMORY_MAP_FAILED;
    object->mapped = 1;
    *data = (uint8_t *)object->mapping + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_UnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    if (!device || !memory)
        return;
    struct venus_memory_real *object =
        (struct venus_memory_real *)(uintptr_t)memory;
    if (object->device == (struct venus_device_real *)device) {
        __asm__ volatile("mfence" ::: "memory");
        object->mapped = 0;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateImage(VkDevice device,
                       const VkImageCreateInfo *create_info,
                       const VkAllocationCallbacks *allocator,
                       VkImage *image)
{
    if (!device || !create_info || !image || allocator ||
        create_info->sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO ||
        !create_info->extent.width || !create_info->extent.height ||
        !create_info->extent.depth || !create_info->mipLevels ||
        !create_info->arrayLayers ||
        (create_info->sharingMode == VK_SHARING_MODE_CONCURRENT &&
         (!create_info->queueFamilyIndexCount ||
          !create_info->pQueueFamilyIndices)))
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkImageFormatListCreateInfo *format_list = 0;
    for (const VkBaseInStructure *next =
             (const VkBaseInStructure *)create_info->pNext;
         next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            format_list = (const VkImageFormatListCreateInfo *)next;
        } else if (next->sType !=
                   VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) {
            printf("[VN image] unsupported pNext sType=%u\n",
                   (uint32_t)next->sType);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    if (format_list && format_list->viewFormatCount &&
        !format_list->pViewFormats)
        return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t family_count = create_info->sharingMode ==
            VK_SHARING_MODE_CONCURRENT
        ? create_info->queueFamilyIndexCount : 0;
    uint64_t pnext_size = format_list
        ? 48u + (uint64_t)format_list->viewFormatCount * 4u : 24u;
    uint64_t size = 116u + pnext_size + (uint64_t)family_count * 4u;
    if (size > UINT32_MAX)
        return VK_ERROR_INITIALIZATION_FAILED;
    void *object = malloc(1);
    uint8_t *command = malloc((uint32_t)size);
    if (!object || !command) {
        free(command);
        free(object);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    struct resource_encoder enc = {
        .data = command,
        .capacity = (uint32_t)size,
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_CREATE_IMAGE);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pCreateInfo */
    encode_u32(&enc, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    encode_u64(&enc, 1); /* external-memory pNext */
    encode_u32(&enc, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    if (format_list) {
        encode_u64(&enc, 1);
        encode_u32(&enc, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
        encode_u64(&enc, 0);
        encode_u32(&enc, format_list->viewFormatCount);
        encode_u64(&enc, format_list->viewFormatCount);
        for (uint32_t i = 0; i < format_list->viewFormatCount; i++)
            encode_u32(&enc, (uint32_t)format_list->pViewFormats[i]);
    } else {
        encode_u64(&enc, 0);
    }
    encode_u32(&enc, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    encode_u32(&enc, create_info->flags);
    encode_u32(&enc, (uint32_t)create_info->imageType);
    encode_u32(&enc, (uint32_t)create_info->format);
    encode_u32(&enc, create_info->extent.width);
    encode_u32(&enc, create_info->extent.height);
    encode_u32(&enc, create_info->extent.depth);
    encode_u32(&enc, create_info->mipLevels);
    encode_u32(&enc, create_info->arrayLayers);
    encode_u32(&enc, (uint32_t)create_info->samples);
    encode_u32(&enc, (uint32_t)create_info->tiling);
    encode_u32(&enc, create_info->usage);
    encode_u32(&enc, (uint32_t)create_info->sharingMode);
    encode_u32(&enc, create_info->queueFamilyIndexCount);
    encode_u64(&enc, family_count);
    for (uint32_t i = 0; i < family_count; i++)
        encode_u32(&enc, create_info->pQueueFamilyIndices[i]);
    encode_u32(&enc, (uint32_t)create_info->initialLayout);
    encode_u64(&enc, 0); /* pAllocator */
    encode_u64(&enc, 1); /* pImage */
    encode_u64(&enc, object_id);

    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = enc.failed ? -1 :
        venus_wire_call(self->physical_device->instance->wire,
                        command, (uint32_t)size, reply, sizeof(reply));
    free(command);
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t present = 0;
    uint64_t returned_id = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&present, reply + 8, 8);
    memcpy(&returned_id, reply + 16, 8);
    if (wire_result < 0 || returned_command != VN_CMD_CREATE_IMAGE ||
        (result == VK_SUCCESS && (!present || returned_id != object_id)))
        result = VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        printf("[VN image] create failed wire=%d reply_cmd=%u result=%d "
               "present=%llu fmt=%u extent=%ux%ux%u usage=0x%x "
               "tiling=%u flags=0x%x pnext=%u bytes=%u encoded=%u\n",
               wire_result, returned_command, result,
               (unsigned long long)present, (uint32_t)create_info->format,
               create_info->extent.width, create_info->extent.height,
               create_info->extent.depth, create_info->usage,
               (uint32_t)create_info->tiling, create_info->flags,
               format_list ? (uint32_t)format_list->sType : 0u,
               (uint32_t)size, enc.length);
        free(object);
        return (VkResult)result;
    }
    *image = (VkImage)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyImage(VkDevice device, VkImage image,
                        const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !image)
        return;
    uint8_t command[32];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_DESTROY_IMAGE);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)image);
    encode_u64(&enc, 0);
    if (!enc.failed)
        (void)venus_wire_submit_async(self->physical_device->instance->wire,
                                      command, sizeof(command));
    free((void *)(uintptr_t)image);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_GetImageMemoryRequirements(VkDevice device, VkImage image,
                                      VkMemoryRequirements *requirements)
{
    if (!device || !image || !requirements)
        return;
    struct venus_device_real *self = (struct venus_device_real *)device;
    uint8_t command[32];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_GET_IMAGE_MEMORY_REQUIREMENTS);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)image);
    encode_u64(&enc, 1);
    uint8_t reply[32];
    memset(reply, 0, sizeof(reply));
    if (enc.failed ||
        venus_wire_call(self->physical_device->instance->wire,
                        command, sizeof(command), reply, sizeof(reply)) < 0)
        return;
    uint32_t returned_command = 0;
    uint64_t present = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&present, reply + 4, 8);
    if (returned_command != VN_CMD_GET_IMAGE_MEMORY_REQUIREMENTS || !present)
        return;
    memcpy(&requirements->size, reply + 12, 8);
    memcpy(&requirements->alignment, reply + 20, 8);
    memcpy(&requirements->memoryTypeBits, reply + 28, 4);
}

VKAPI_ATTR void VKAPI_CALL
venus_real_GetImageMemoryRequirements2(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements)
{
    if (!device || !info || !requirements ||
        info->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 ||
        info->pNext ||
        requirements->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2)
        return;
    struct venus_device_real *self = (struct venus_device_real *)device;
    uint8_t command[76];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    encode_u32(&enc, VN_CMD_GET_IMAGE_MEMORY_REQUIREMENTS_2);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pInfo */
    encode_u32(&enc, VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2);
    encode_u64(&enc, 0);
    encode_u64(&enc, (uint64_t)info->image);
    encode_u64(&enc, 1); /* pMemoryRequirements */
    encode_u32(&enc, VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2);
    encode_u64(&enc, 1); /* dedicated pNext */
    encode_u32(&enc, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
    encode_u64(&enc, 0);
    uint8_t reply[64];
    memset(reply, 0, sizeof(reply));
    if (enc.failed ||
        venus_wire_call(self->physical_device->instance->wire,
                        command, sizeof(command), reply, sizeof(reply)) < 0)
        return;
    uint32_t returned_command = 0;
    uint64_t present = 0;
    uint32_t returned_type = 0;
    uint64_t dedicated_present = 0;
    uint32_t dedicated_type = 0;
    uint64_t dedicated_next = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&present, reply + 4, 8);
    memcpy(&returned_type, reply + 12, 4);
    memcpy(&dedicated_present, reply + 16, 8);
    memcpy(&dedicated_type, reply + 24, 4);
    memcpy(&dedicated_next, reply + 28, 8);
    if (returned_command != VN_CMD_GET_IMAGE_MEMORY_REQUIREMENTS_2 ||
        !present || returned_type != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 ||
        !dedicated_present ||
        dedicated_type != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS ||
        dedicated_next)
        return;
    uint32_t prefers = 0;
    uint32_t requires = 0;
    memcpy(&prefers, reply + 36, 4);
    memcpy(&requires, reply + 40, 4);
    memcpy(&requirements->memoryRequirements.size, reply + 44, 8);
    memcpy(&requirements->memoryRequirements.alignment, reply + 52, 8);
    memcpy(&requirements->memoryRequirements.memoryTypeBits, reply + 60, 4);
    for (VkBaseOutStructure *next = (VkBaseOutStructure *)requirements->pNext;
         next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            VkMemoryDedicatedRequirements *dedicated =
                (VkMemoryDedicatedRequirements *)next;
            dedicated->prefersDedicatedAllocation = prefers;
            dedicated->requiresDedicatedAllocation = requires;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_BindImageMemory(VkDevice device, VkImage image,
                           VkDeviceMemory memory,
                           VkDeviceSize memory_offset)
{
    if (!device || !image || !memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t command[40];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_BIND_IMAGE_MEMORY);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)image);
    encode_u64(&enc, (uint64_t)memory);
    encode_u64(&enc, memory_offset);
    return enc.failed ? VK_ERROR_INITIALIZATION_FAILED
        : result_call(self, command, sizeof(command), VN_CMD_BIND_IMAGE_MEMORY);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateImageView(VkDevice device,
                           const VkImageViewCreateInfo *create_info,
                           const VkAllocationCallbacks *allocator,
                           VkImageView *view)
{
    if (!device || !create_info || !view || allocator ||
        create_info->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO ||
        !create_info->image)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkImageViewUsageCreateInfo *usage_info = 0;
    for (const VkBaseInStructure *next =
             (const VkBaseInStructure *)create_info->pNext;
         next; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO)
            return VK_ERROR_INITIALIZATION_FAILED;
        usage_info = (const VkImageViewUsageCreateInfo *)next;
    }
    uint32_t command_size = usage_info ? 132u : 116u;
    void *object = malloc(1);
    uint8_t *command = malloc(command_size);
    if (!object || !command) {
        free(command);
        free(object);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    struct resource_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_CREATE_IMAGE_VIEW);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1);
    encode_u32(&enc, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    if (usage_info) {
        encode_u64(&enc, 1);
        encode_u32(&enc, VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO);
        encode_u64(&enc, 0);
        encode_u32(&enc, usage_info->usage);
    } else {
        encode_u64(&enc, 0);
    }
    encode_u32(&enc, create_info->flags);
    encode_u64(&enc, (uint64_t)create_info->image);
    encode_u32(&enc, (uint32_t)create_info->viewType);
    encode_u32(&enc, (uint32_t)create_info->format);
    encode_u32(&enc, (uint32_t)create_info->components.r);
    encode_u32(&enc, (uint32_t)create_info->components.g);
    encode_u32(&enc, (uint32_t)create_info->components.b);
    encode_u32(&enc, (uint32_t)create_info->components.a);
    encode_u32(&enc, create_info->subresourceRange.aspectMask);
    encode_u32(&enc, create_info->subresourceRange.baseMipLevel);
    encode_u32(&enc, create_info->subresourceRange.levelCount);
    encode_u32(&enc, create_info->subresourceRange.baseArrayLayer);
    encode_u32(&enc, create_info->subresourceRange.layerCount);
    encode_u64(&enc, 0);
    encode_u64(&enc, 1);
    encode_u64(&enc, object_id);
    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = enc.failed ? -1 :
        venus_wire_call(self->physical_device->instance->wire,
                        command, command_size, reply, sizeof(reply));
    free(command);
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t present = 0;
    uint64_t returned_id = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&present, reply + 8, 8);
    memcpy(&returned_id, reply + 16, 8);
    if (wire_result < 0 || returned_command != VN_CMD_CREATE_IMAGE_VIEW ||
        (result == VK_SUCCESS && (!present || returned_id != object_id)))
        result = VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        free(object);
        return (VkResult)result;
    }
    *view = (VkImageView)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyImageView(VkDevice device, VkImageView view,
                            const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !view)
        return;
    uint8_t command[32];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_DESTROY_IMAGE_VIEW);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)view);
    encode_u64(&enc, 0);
    if (!enc.failed)
        (void)venus_wire_submit_async(self->physical_device->instance->wire,
                                      command, sizeof(command));
    free((void *)(uintptr_t)view);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateSampler(VkDevice device,
                         const VkSamplerCreateInfo *create_info,
                         const VkAllocationCallbacks *allocator,
                         VkSampler *sampler)
{
    if (!device || !create_info || !sampler || allocator ||
        create_info->sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO ||
        create_info->pNext)
        return VK_ERROR_INITIALIZATION_FAILED;
    void *object = malloc(1);
    if (!object)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    uint8_t command[124];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_CREATE_SAMPLER);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1);
    encode_u32(&enc, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    encode_u64(&enc, 0);
    encode_u32(&enc, create_info->flags);
    encode_u32(&enc, (uint32_t)create_info->magFilter);
    encode_u32(&enc, (uint32_t)create_info->minFilter);
    encode_u32(&enc, (uint32_t)create_info->mipmapMode);
    encode_u32(&enc, (uint32_t)create_info->addressModeU);
    encode_u32(&enc, (uint32_t)create_info->addressModeV);
    encode_u32(&enc, (uint32_t)create_info->addressModeW);
    encode_float(&enc, create_info->mipLodBias);
    encode_u32(&enc, create_info->anisotropyEnable);
    encode_float(&enc, create_info->maxAnisotropy);
    encode_u32(&enc, create_info->compareEnable);
    encode_u32(&enc, (uint32_t)create_info->compareOp);
    encode_float(&enc, create_info->minLod);
    encode_float(&enc, create_info->maxLod);
    encode_u32(&enc, (uint32_t)create_info->borderColor);
    encode_u32(&enc, create_info->unnormalizedCoordinates);
    encode_u64(&enc, 0);
    encode_u64(&enc, 1);
    encode_u64(&enc, object_id);
    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = enc.failed ? -1 :
        venus_wire_call(self->physical_device->instance->wire,
                        command, sizeof(command), reply, sizeof(reply));
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t present = 0;
    uint64_t returned_id = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&present, reply + 8, 8);
    memcpy(&returned_id, reply + 16, 8);
    if (wire_result < 0 || returned_command != VN_CMD_CREATE_SAMPLER ||
        (result == VK_SUCCESS && (!present || returned_id != object_id)))
        result = VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        free(object);
        return (VkResult)result;
    }
    *sampler = (VkSampler)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroySampler(VkDevice device, VkSampler sampler,
                          const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !sampler)
        return;
    uint8_t command[32];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_DESTROY_SAMPLER);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)sampler);
    encode_u64(&enc, 0);
    if (!enc.failed)
        (void)venus_wire_submit_async(self->physical_device->instance->wire,
                                      command, sizeof(command));
    free((void *)(uintptr_t)sampler);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkShaderModule *shader_module)
{
    if (!device || !create_info || !shader_module || allocator ||
        create_info->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
        create_info->pNext || !create_info->pCode ||
        !create_info->codeSize || (create_info->codeSize & 3u) ||
        create_info->codeSize > UINT32_MAX - 80u)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t command_size = 80u + (uint32_t)create_info->codeSize;
    uint8_t *command = malloc(command_size);
    void *object = malloc(1);
    if (!command || !object) {
        free(command);
        free(object);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint64_t object_id = (uint64_t)(uintptr_t)object;
    struct resource_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_CREATE_SHADER_MODULE);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1);
    encode_u32(&enc, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    encode_u64(&enc, 0);
    encode_u32(&enc, create_info->flags);
    encode_u64(&enc, create_info->codeSize);
    encode_u64(&enc, create_info->codeSize / 4u);
    encode_bytes(&enc, create_info->pCode, (uint32_t)create_info->codeSize);
    encode_u64(&enc, 0);
    encode_u64(&enc, 1);
    encode_u64(&enc, object_id);
    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = enc.failed ? -1 :
        venus_wire_call(self->physical_device->instance->wire,
                        command, command_size, reply, sizeof(reply));
    free(command);
    uint32_t returned_command = 0;
    int32_t result = VK_ERROR_DEVICE_LOST;
    uint64_t present = 0;
    uint64_t returned_id = 0;
    memcpy(&returned_command, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&present, reply + 8, 8);
    memcpy(&returned_id, reply + 16, 8);
    if (wire_result < 0 ||
        returned_command != VN_CMD_CREATE_SHADER_MODULE ||
        (result == VK_SUCCESS && (!present || returned_id != object_id)))
        result = VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        free(object);
        return (VkResult)result;
    }
    *shader_module = (VkShaderModule)object_id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyShaderModule(VkDevice device,
                               VkShaderModule shader_module,
                               const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !shader_module)
        return;
    uint8_t command[32];
    struct resource_encoder enc = {
        .data = command,
        .capacity = sizeof(command),
    };
    struct venus_device_real *self = (struct venus_device_real *)device;
    encode_u32(&enc, VN_CMD_DESTROY_SHADER_MODULE);
    encode_u32(&enc, 0);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, (uint64_t)shader_module);
    encode_u64(&enc, 0);
    if (!enc.failed)
        (void)venus_wire_submit_async(self->physical_device->instance->wire,
                                      command, sizeof(command));
    free((void *)(uintptr_t)shader_module);
}
