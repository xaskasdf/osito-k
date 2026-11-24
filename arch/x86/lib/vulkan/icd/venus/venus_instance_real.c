#include "venus_real.h"

extern void *malloc(unsigned long);
extern void free(void *);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern unsigned long strlen(const char *);
extern int strcmp(const char *, const char *);
extern long __syscall1(long, long);

#define VN_CMD_CREATE_INSTANCE 0u
#define VN_CMD_DESTROY_INSTANCE 1u
#define VN_CMD_ENUMERATE_PHYSICAL_DEVICES 2u
#define VN_CMD_ENUMERATE_INSTANCE_VERSION 137u
#define VN_COMMAND_GENERATE_REPLY 1u

struct wire_encoder {
    uint8_t *data;
    uint32_t capacity;
    uint32_t length;
    int failed;
};

static void encode_bytes(struct wire_encoder *enc, const void *data,
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

static void encode_u32(struct wire_encoder *enc, uint32_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static void encode_u64(struct wire_encoder *enc, uint64_t value)
{
    encode_bytes(enc, &value, sizeof(value));
}

static void encode_string(struct wire_encoder *enc, const char *string)
{
    uint64_t size = string ? strlen(string) + 1 : 0;
    encode_u64(enc, size);
    if (!size)
        return;
    uint32_t padded = ((uint32_t)size + 3u) & ~3u;
    encode_bytes(enc, string, (uint32_t)size);
    if (padded != size) {
        static const uint32_t zero;
        encode_bytes(enc, &zero, padded - (uint32_t)size);
    }
}

static int encode_name_array(struct wire_encoder *enc, uint32_t count,
                             const char *const *names)
{
    if (count && !names)
        return -1;
    encode_u64(enc, names ? count : 0);
    for (uint32_t i = 0; i < count; i++) {
        if (!names[i])
            return -1;
        encode_string(enc, names[i]);
    }
    return enc->failed ? -1 : 0;
}

static int encode_create_instance(struct wire_encoder *enc,
                                  const VkInstanceCreateInfo *info,
                                  uint64_t object_id)
{
    if (!info || info->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO)
        return -1;
    const VkApplicationInfo *app = info->pApplicationInfo;
    if (app && app->sType != VK_STRUCTURE_TYPE_APPLICATION_INFO)
        return -1;

    encode_u32(enc, VN_CMD_CREATE_INSTANCE);
    encode_u32(enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(enc, 1); /* pCreateInfo */
    encode_u32(enc, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    encode_u64(enc, 0); /* unsupported pNext entries are ignored by protocol */
    encode_u32(enc, info->flags);
    encode_u64(enc, app ? 1 : 0);
    if (app) {
        encode_u32(enc, VK_STRUCTURE_TYPE_APPLICATION_INFO);
        encode_u64(enc, 0);
        encode_string(enc, app->pApplicationName);
        encode_u32(enc, app->applicationVersion);
        encode_string(enc, app->pEngineName);
        encode_u32(enc, app->engineVersion);
        encode_u32(enc, app->apiVersion);
    }
    encode_u32(enc, info->enabledLayerCount);
    if (encode_name_array(enc, info->enabledLayerCount,
                          info->ppEnabledLayerNames) < 0)
        return -1;
    encode_u32(enc, info->enabledExtensionCount);
    if (encode_name_array(enc, info->enabledExtensionCount,
                          info->ppEnabledExtensionNames) < 0)
        return -1;
    encode_u64(enc, 0); /* pAllocator is guest-local */
    encode_u64(enc, 1); /* pInstance */
    encode_u64(enc, object_id);
    return enc->failed ? -1 : 0;
}

static VkResult create_host_instance(struct venus_instance_real *self,
                                     const VkInstanceCreateInfo *info)
{
    struct wire_encoder size_enc = { .capacity = UINT32_MAX };
    if (encode_create_instance(&size_enc, info, self->object_id) < 0)
        return VK_ERROR_INITIALIZATION_FAILED;

    uint8_t *command = malloc(size_enc.length);
    if (!command)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct wire_encoder enc = {
        .data = command,
        .capacity = size_enc.length,
    };
    if (encode_create_instance(&enc, info, self->object_id) < 0) {
        free(command);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    uint8_t reply[24];
    memset(reply, 0, sizeof(reply));
    int wire_result = venus_wire_call(self->wire, command, enc.length,
                                      reply, sizeof(reply));
    free(command);
    if (wire_result < 0)
        return VK_ERROR_DEVICE_LOST;

    uint32_t command_type;
    int32_t result;
    uint64_t present;
    uint64_t returned_id;
    memcpy(&command_type, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&present, reply + 8, 8);
    memcpy(&returned_id, reply + 16, 8);
    if (command_type != VN_CMD_CREATE_INSTANCE)
        return VK_ERROR_DEVICE_LOST;
    if (result == VK_SUCCESS && (!present || returned_id != self->object_id))
        return VK_ERROR_DEVICE_LOST;
    return (VkResult)result;
}

static VkResult enumerate_host_physical_devices(
    struct venus_instance_real *self, uint32_t *count,
    struct venus_physical_device_real *devices)
{
    uint32_t capacity = devices ? *count : 0;
    uint32_t command_size = 36u + capacity * 8u;
    uint32_t reply_size = 28u + capacity * 8u;
    uint8_t *command = malloc(command_size);
    uint8_t *reply = malloc(reply_size);
    if (!command || !reply) {
        free(command);
        free(reply);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    struct wire_encoder enc = {
        .data = command,
        .capacity = command_size,
    };
    encode_u32(&enc, VN_CMD_ENUMERATE_PHYSICAL_DEVICES);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, self->object_id);
    encode_u64(&enc, 1); /* pPhysicalDeviceCount */
    encode_u32(&enc, capacity);
    encode_u64(&enc, capacity);
    for (uint32_t i = 0; i < capacity; i++)
        encode_u64(&enc, devices[i].object_id);
    if (enc.failed || enc.length != command_size) {
        free(command);
        free(reply);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    memset(reply, 0, reply_size);
    if (venus_wire_call(self->wire, command, command_size, reply,
                        reply_size) < 0) {
        free(command);
        free(reply);
        return VK_ERROR_DEVICE_LOST;
    }
    free(command);

    uint32_t command_type;
    int32_t result;
    uint64_t count_present;
    uint32_t returned_count;
    uint64_t returned_array_size;
    memcpy(&command_type, reply, 4);
    memcpy(&result, reply + 4, 4);
    memcpy(&count_present, reply + 8, 8);
    memcpy(&returned_count, reply + 16, 4);
    memcpy(&returned_array_size, reply + 20, 8);
    if (command_type != VN_CMD_ENUMERATE_PHYSICAL_DEVICES ||
        !count_present || returned_array_size > capacity ||
        returned_array_size != (devices ? returned_count : 0)) {
        free(reply);
        return VK_ERROR_DEVICE_LOST;
    }
    for (uint32_t i = 0; i < returned_array_size; i++) {
        uint64_t returned_id;
        memcpy(&returned_id, reply + 28u + i * 8u, 8);
        if (returned_id != devices[i].object_id) {
            free(reply);
            return VK_ERROR_DEVICE_LOST;
        }
    }
    free(reply);
    *count = returned_count;
    return (VkResult)result;
}

static VkResult init_physical_devices(struct venus_instance_real *self)
{
    if (self->physical_devices_initialized)
        return VK_SUCCESS;

    uint32_t count = 0;
    VkResult result = enumerate_host_physical_devices(self, &count, 0);
    if (result != VK_SUCCESS)
        return result;
    if (!count) {
        self->physical_devices_initialized = 1;
        return VK_SUCCESS;
    }

    struct venus_physical_device_real *devices =
        malloc((unsigned long)count * sizeof(*devices));
    if (!devices)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(devices, 0, (unsigned long)count * sizeof(*devices));
    for (uint32_t i = 0; i < count; i++) {
        set_loader_magic_value(&devices[i]);
        devices[i].object_id = (uint64_t)(uintptr_t)&devices[i];
        devices[i].instance = self;
    }

    uint32_t returned_count = count;
    result = enumerate_host_physical_devices(self, &returned_count, devices);
    if (result != VK_SUCCESS || returned_count > count) {
        free(devices);
        return result == VK_SUCCESS ? VK_ERROR_DEVICE_LOST : result;
    }

    self->physical_devices = devices;
    self->physical_device_count = returned_count;
    self->physical_devices_initialized = 1;
    return VK_SUCCESS;
}

static VkResult open_temporary_wire(int32_t *ctx_id, struct venus_wire **wire)
{
    uint32_t caps = 0;
    if (__syscall1(VENUS_SYS_GPU_CAPS, (long)&caps) < 0 ||
        !(caps & VENUS_GPU_CAP_READY))
        return VK_ERROR_INITIALIZATION_FAILED;
    long id = __syscall1(VENUS_SYS_GPU_CTX_CREATE, VENUS_GPU_CTX);
    if (id <= 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    *ctx_id = (int32_t)id;
    *wire = venus_wire_open(*ctx_id);
    if (!*wire) {
        (void)__syscall1(VENUS_SYS_GPU_CTX_DESTROY, *ctx_id);
        *ctx_id = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

static int is_guest_instance_extension(const char *name)
{
    return strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME) == 0 ||
           strcmp(name, VK_OSITOK_COMPOSITOR_SURFACE_EXTENSION_NAME) == 0;
}

static VkResult create_filtered_host_instance(
    struct venus_instance_real *self, const VkInstanceCreateInfo *create_info)
{
    VkInstanceCreateInfo host_info = *create_info;
    const char **host_extensions = 0;
    if (create_info->enabledExtensionCount) {
        host_extensions = malloc(
            (unsigned long)create_info->enabledExtensionCount *
            sizeof(*host_extensions));
        if (!host_extensions)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    host_info.enabledExtensionCount = 0;
    for (uint32_t i = 0; i < create_info->enabledExtensionCount; i++) {
        const char *name = create_info->ppEnabledExtensionNames[i];
        if (!name) {
            free(host_extensions);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!is_guest_instance_extension(name))
            host_extensions[host_info.enabledExtensionCount++] = name;
    }
    host_info.ppEnabledExtensionNames = host_info.enabledExtensionCount
        ? host_extensions : 0;
    VkResult result = create_host_instance(self, &host_info);
    free(host_extensions);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_EnumerateInstanceVersion(uint32_t *api_version)
{
    if (!api_version)
        return VK_ERROR_INITIALIZATION_FAILED;
    int32_t ctx_id = 0;
    struct venus_wire *wire = 0;
    VkResult result = open_temporary_wire(&ctx_id, &wire);
    if (result != VK_SUCCESS)
        return result;

    uint8_t command[16];
    struct wire_encoder enc = { .data = command, .capacity = sizeof(command) };
    encode_u32(&enc, VN_CMD_ENUMERATE_INSTANCE_VERSION);
    encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
    encode_u64(&enc, 1);

    uint8_t reply[20];
    memset(reply, 0, sizeof(reply));
    if (venus_wire_call(wire, command, sizeof(command), reply,
                        sizeof(reply)) < 0) {
        result = VK_ERROR_DEVICE_LOST;
    } else {
        uint32_t command_type;
        int32_t host_result;
        uint64_t present;
        memcpy(&command_type, reply, 4);
        memcpy(&host_result, reply + 4, 4);
        memcpy(&present, reply + 8, 8);
        memcpy(api_version, reply + 16, 4);
        if (command_type != VN_CMD_ENUMERATE_INSTANCE_VERSION || !present)
            result = VK_ERROR_DEVICE_LOST;
        else
            result = (VkResult)host_result;
    }

    venus_wire_close(wire);
    (void)__syscall1(VENUS_SYS_GPU_CTX_DESTROY, ctx_id);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateInstance(const VkInstanceCreateInfo *create_info,
                          const VkAllocationCallbacks *allocator,
                          VkInstance *instance)
{
    if (!create_info || !instance || allocator)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_instance_real *self = malloc(sizeof(*self));
    if (!self)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(self, 0, sizeof(*self));
    set_loader_magic_value(self);
    self->object_id = (uint64_t)(uintptr_t)self;

    VkResult result = open_temporary_wire(&self->ctx_id, &self->wire);
    if (result == VK_SUCCESS)
        result = create_filtered_host_instance(self, create_info);
    if (result != VK_SUCCESS) {
        if (self->wire)
            venus_wire_close(self->wire);
        if (self->ctx_id > 0)
            (void)__syscall1(VENUS_SYS_GPU_CTX_DESTROY, self->ctx_id);
        free(self);
        return result;
    }

    *instance = (VkInstance)self;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyInstance(VkInstance instance,
                           const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!instance)
        return;
    struct venus_instance_real *self = (struct venus_instance_real *)instance;
    for (uint32_t i = 0; i < self->physical_device_count; i++) {
        free(self->physical_devices[i].queue_family_properties);
        free(self->physical_devices[i].extension_properties);
    }
    free(self->physical_devices);
    if (self->wire) {
        uint8_t command[24];
        struct wire_encoder enc = {
            .data = command,
            .capacity = sizeof(command),
        };
        encode_u32(&enc, VN_CMD_DESTROY_INSTANCE);
        encode_u32(&enc, VN_COMMAND_GENERATE_REPLY);
        encode_u64(&enc, self->object_id);
        encode_u64(&enc, 0); /* pAllocator */
        uint8_t reply[4];
        (void)venus_wire_call(self->wire, command, sizeof(command),
                              reply, sizeof(reply));
        venus_wire_close(self->wire);
    }
    if (self->ctx_id > 0)
        (void)__syscall1(VENUS_SYS_GPU_CTX_DESTROY, self->ctx_id);
    free(self);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_EnumeratePhysicalDevices(VkInstance instance,
                                    uint32_t *physical_device_count,
                                    VkPhysicalDevice *physical_devices)
{
    if (!instance || !physical_device_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_instance_real *self = (struct venus_instance_real *)instance;
    VkResult result = init_physical_devices(self);
    if (result != VK_SUCCESS)
        return result;

    uint32_t available = self->physical_device_count;
    if (!physical_devices) {
        *physical_device_count = available;
        return VK_SUCCESS;
    }

    uint32_t written = *physical_device_count < available
        ? *physical_device_count : available;
    for (uint32_t i = 0; i < written; i++)
        physical_devices[i] = (VkPhysicalDevice)&self->physical_devices[i];
    *physical_device_count = written;
    return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}
