#include <vulkan/vulkan.h>

extern int printf(const char *format, ...);

static const uint32_t empty_compute_shader[] = {
    0x07230203, 0x00010000, 0x00000000, 0x00000006, 0x00000000,
    0x00020011, 0x00000001,
    0x0003000e, 0x00000000, 0x00000001,
    0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001,
    0x00000001,
    0x00020013, 0x00000001,
    0x00030021, 0x00000002, 0x00000001,
    0x00050036, 0x00000001, 0x00000004, 0x00000000, 0x00000002,
    0x000200f8, 0x00000005,
    0x000100fd,
    0x00010038,
};

int main(void)
{
    uint32_t version = 0;
    VkResult result = vkEnumerateInstanceVersion(&version);
    if (result != VK_SUCCESS || !version) {
        printf("VENUS-USER-ROUNDTRIP: version failed (%d)\n", result);
        return 1;
    }

    const VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "hello_vk",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "OsitoK",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };
    VkInstance instance = VK_NULL_HANDLE;
    result = vkCreateInstance(&create_info, 0, &instance);
    if (result != VK_SUCCESS || !instance) {
        printf("VENUS-USER-ROUNDTRIP: create failed (%d)\n", result);
        return 2;
    }

    uint32_t physical_device_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &physical_device_count, 0);
    if (result != VK_SUCCESS || !physical_device_count) {
        printf("VENUS-USER-ROUNDTRIP: enumerate count failed (%d, %u)\n",
               result, physical_device_count);
        vkDestroyInstance(instance, 0);
        return 3;
    }

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32_t requested_count = 1;
    result = vkEnumeratePhysicalDevices(instance, &requested_count,
                                        &physical_device);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
        requested_count != 1 || !physical_device) {
        printf("VENUS-USER-ROUNDTRIP: enumerate handle failed (%d, %u)\n",
               result, requested_count);
        vkDestroyInstance(instance, 0);
        return 4;
    }

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    if (!properties.apiVersion || !properties.deviceName[0]) {
        printf("VENUS-USER-ROUNDTRIP: properties failed\n");
        vkDestroyInstance(instance, 0);
        return 5;
    }

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                             &queue_family_count, 0);
    if (!queue_family_count || queue_family_count > 32) {
        printf("VENUS-USER-ROUNDTRIP: queue count failed (%u)\n",
               queue_family_count);
        vkDestroyInstance(instance, 0);
        return 6;
    }
    VkQueueFamilyProperties queue_families[32];
    uint32_t returned_queue_count = queue_family_count;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                             &returned_queue_count,
                                             queue_families);
    uint32_t graphics_queue = UINT32_MAX;
    for (uint32_t i = 0; i < returned_queue_count; i++) {
        if (queue_families[i].queueCount &&
            (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            graphics_queue = i;
            break;
        }
    }
    if (graphics_queue == UINT32_MAX) {
        printf("VENUS-USER-ROUNDTRIP: no graphics queue\n");
        vkDestroyInstance(instance, 0);
        return 7;
    }

    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical_device, &features);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    if (!memory_properties.memoryTypeCount ||
        !memory_properties.memoryHeapCount ||
        !memory_properties.memoryHeaps[0].size) {
        printf("VENUS-USER-ROUNDTRIP: memory properties failed\n");
        vkDestroyInstance(instance, 0);
        return 8;
    }

    uint32_t extension_count = 0;
    result = vkEnumerateDeviceExtensionProperties(
        physical_device, 0, &extension_count, 0);
    if (result != VK_SUCCESS || !extension_count || extension_count > 256) {
        printf("VENUS-USER-ROUNDTRIP: extension count failed (%d, %u)\n",
               result, extension_count);
        vkDestroyInstance(instance, 0);
        return 9;
    }
    VkExtensionProperties extensions[256];
    uint32_t returned_extension_count = extension_count;
    result = vkEnumerateDeviceExtensionProperties(
        physical_device, 0, &returned_extension_count, extensions);
    if (result != VK_SUCCESS || returned_extension_count != extension_count ||
        !extensions[0].extensionName[0]) {
        printf("VENUS-USER-ROUNDTRIP: extensions failed (%d, %u)\n",
               result, returned_extension_count);
        vkDestroyInstance(instance, 0);
        return 10;
    }

    const float queue_priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = graphics_queue,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
    };
    VkDevice device = VK_NULL_HANDLE;
    result = vkCreateDevice(physical_device, &device_info, 0, &device);
    if (result != VK_SUCCESS || !device) {
        printf("VENUS-USER-ROUNDTRIP: create device failed (%d)\n", result);
        vkDestroyInstance(instance, 0);
        return 11;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, graphics_queue, 0, &queue);
    if (!queue) {
        printf("VENUS-USER-ROUNDTRIP: get queue failed\n");
        vkDestroyDevice(device, 0);
        vkDestroyInstance(instance, 0);
        return 12;
    }
    VkQueue same_queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, graphics_queue, 0, &same_queue);
    if (same_queue != queue) {
        printf("VENUS-USER-ROUNDTRIP: queue handle unstable\n");
        vkDestroyDevice(device, 0);
        vkDestroyInstance(instance, 0);
        return 13;
    }

    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkSemaphore semaphore = VK_NULL_HANDLE;
    result = vkCreateSemaphore(device, &semaphore_info, 0, &semaphore);
    if (result != VK_SUCCESS || !semaphore) {
        printf("VENUS-USER-ROUNDTRIP: create semaphore failed (%d)\n",
               result);
        vkDestroyDevice(device, 0);
        vkDestroyInstance(instance, 0);
        return 14;
    }
    vkDestroySemaphore(device, semaphore, 0);

    const VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(empty_compute_shader),
        .pCode = empty_compute_shader,
    };
    VkShaderModule shader_module = VK_NULL_HANDLE;
    result = vkCreateShaderModule(device, &shader_info, 0, &shader_module);
    if (result != VK_SUCCESS || !shader_module) {
        printf("VENUS-USER-ROUNDTRIP: create shader failed (%d)\n", result);
        vkDestroyDevice(device, 0);
        vkDestroyInstance(instance, 0);
        return 15;
    }
    vkDestroyShaderModule(device, shader_module, 0);
    vkDestroyDevice(device, 0);

    vkDestroyInstance(instance, 0);
    printf("VENUS-USER-ROUNDTRIP: OK version=0x%x devices=%u gpu=%s api=0x%x queues=%u heaps=%u extensions=%u anisotropy=%u shader=1\n",
           version, physical_device_count, properties.deviceName,
           properties.apiVersion, returned_queue_count,
           memory_properties.memoryHeapCount, returned_extension_count,
           features.samplerAnisotropy);
    return 0;
}
