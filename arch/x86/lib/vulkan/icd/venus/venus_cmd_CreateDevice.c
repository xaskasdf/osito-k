/*
 * Encoder for vkCreateDevice over the real Mesa venus-protocol stream.
 *
 * The renderer requires a registered VkPhysicalDevice object id from
 * vkEnumeratePhysicalDevices, then a generated VkDeviceCreateInfo payload.
 * Queues, extension names, layer names, pEnabledFeatures, and the Vulkan 1.3
 * device feature chain are forwarded exactly enough for host vkCreateDevice.
 */
#include "venus_wire.h"
#include "venus.h"

extern unsigned long strlen(const char *);
extern int printf(const char *, ...);

#define VN_CMD_TYPE_vkCreateDevice 11u
#define VN_CMD_GENERATE_REPLY 1u

struct vcd_writer {
    uint8_t *buf;
    uint32_t off;
    uint32_t cap;
    int err;
};

static void vcd_wr_bytes(struct vcd_writer *w, const void *src, uint32_t n) {
    if (w->err) return;
    if (w->off + n > w->cap || w->off + n < w->off) {
        w->err = -12;
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        w->buf[w->off + i] = ((const uint8_t *)src)[i];
    w->off += n;
}

static void vcd_wr_u32(struct vcd_writer *w, uint32_t v) {
    vcd_wr_bytes(w, &v, 4);
}

static void vcd_wr_i32(struct vcd_writer *w, int32_t v) {
    vcd_wr_bytes(w, &v, 4);
}

static void vcd_wr_u64(struct vcd_writer *w, uint64_t v) {
    vcd_wr_bytes(w, &v, 8);
}

static void vcd_wr_str(struct vcd_writer *w, const char *s) {
    uint64_t n = s ? (uint64_t)strlen(s) + 1u : 0u;
    vcd_wr_u64(w, n);
    if (!n) return;
    vcd_wr_bytes(w, s, (uint32_t)n);
    while (w->off & 3u) {
        uint8_t z = 0;
        vcd_wr_bytes(w, &z, 1);
    }
}

static void vcd_wr_queue_info(struct vcd_writer *w,
                              const VkDeviceQueueCreateInfo *q) {
    uint32_t queue_count = q ? q->queueCount : 0u;

    vcd_wr_i32(w, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    vcd_wr_u64(w, 0);                                /* pNext */
    vcd_wr_u32(w, q ? q->flags : 0u);
    vcd_wr_u32(w, q ? q->queueFamilyIndex : 0u);
    vcd_wr_u32(w, queue_count);
    vcd_wr_u64(w, queue_count);                      /* pQueuePriorities */
    for (uint32_t i = 0; i < queue_count; i++) {
        float prio = q->pQueuePriorities ? q->pQueuePriorities[i] : 0.0f;
        vcd_wr_bytes(w, &prio, 4);
    }
}

static void vcd_wr_string_array(struct vcd_writer *w, uint32_t count,
                                const char *const *strings) {
    if (!strings)
        count = 0;
    vcd_wr_u64(w, count);
    for (uint32_t i = 0; i < count; i++)
        vcd_wr_str(w, strings[i]);
}

static int vcd_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int vcd_extension_goes_to_host(const char *name) {
    /* The loader advertises swapchain support because OsitoK implements WSI
     * locally.  The Venus render server has no native swapchain surface, so
     * passing this extension to host vkCreateDevice makes Mesa reject the
     * device with VK_ERROR_EXTENSION_NOT_PRESENT. */
    if (vcd_streq(name, "VK_KHR_swapchain"))
        return 0;
    /* These are currently loader-injected so DXVK enables its code paths and
     * queries feature bits.  Until the Venus encoder forwards the matching
     * pNext feature structs, keep them guest-visible but do not require host
     * device-extension enablement. */
    if (vcd_streq(name, "VK_EXT_robustness2") ||
        vcd_streq(name, "VK_EXT_transform_feedback"))
        return 0;
    return 1;
}

static const VkPhysicalDeviceVulkan13Features *
vcd_find_vulkan13_features(const VkDeviceCreateInfo *info) {
    const VkBaseInStructure *pnext;

    if (!info)
        return 0;

    pnext = (const VkBaseInStructure *)info->pNext;
    while (pnext) {
        if (pnext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
            return (const VkPhysicalDeviceVulkan13Features *)pnext;
        pnext = pnext->pNext;
    }

    return 0;
}

static void vcd_wr_bool32(struct vcd_writer *w, VkBool32 v) {
    vcd_wr_u32(w, (uint32_t)v);
}

static void vcd_wr_vulkan13_features_pnext(
        struct vcd_writer *w,
        const VkPhysicalDeviceVulkan13Features *f) {
    if (!f) {
        vcd_wr_u64(w, 0);
        return;
    }

    vcd_wr_u64(w, 1);
    vcd_wr_i32(w, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    vcd_wr_u64(w, 0);                                /* nested pNext */
    vcd_wr_bool32(w, f->robustImageAccess);
    vcd_wr_bool32(w, f->inlineUniformBlock);
    vcd_wr_bool32(w, f->descriptorBindingInlineUniformBlockUpdateAfterBind);
    vcd_wr_bool32(w, f->pipelineCreationCacheControl);
    vcd_wr_bool32(w, f->privateData);
    vcd_wr_bool32(w, f->shaderDemoteToHelperInvocation);
    vcd_wr_bool32(w, f->shaderTerminateInvocation);
    vcd_wr_bool32(w, f->subgroupSizeControl);
    vcd_wr_bool32(w, f->computeFullSubgroups);
    vcd_wr_bool32(w, f->synchronization2);
    vcd_wr_bool32(w, f->textureCompressionASTC_HDR);
    vcd_wr_bool32(w, f->shaderZeroInitializeWorkgroupMemory);
    vcd_wr_bool32(w, f->dynamicRendering);
    vcd_wr_bool32(w, f->shaderIntegerDotProduct);
    vcd_wr_bool32(w, f->maintenance4);
}

static uint32_t vcd_count_host_extensions(uint32_t count,
                                          const char *const *strings) {
    uint32_t out = 0;
    if (!strings) return 0;
    for (uint32_t i = 0; i < count; i++)
        if (vcd_extension_goes_to_host(strings[i]))
            out++;
    return out;
}

static void vcd_wr_host_extension_array(struct vcd_writer *w, uint32_t count,
                                        const char *const *strings) {
    uint32_t host_count = vcd_count_host_extensions(count, strings);
    vcd_wr_u64(w, host_count);
    if (!strings) return;
    for (uint32_t i = 0; i < count; i++) {
        if (vcd_extension_goes_to_host(strings[i]))
            vcd_wr_str(w, strings[i]);
    }
}

int venus_cmd_encode_CreateDevice(
        struct venus_wire *w, uint64_t physical_device_id,
        const VkDeviceCreateInfo *pCreateInfo,
        uint64_t *out_device_id) {
    if (!w || !physical_device_id || !pCreateInfo || !out_device_id)
        return -22;

    extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);

    uint8_t cmd[8192];
    uint8_t reply[24];
    struct vcd_writer wr = { cmd, 0, sizeof(cmd), 0 };
    uint64_t device_id = venus_wire_alloc_object_id(w);
    if (!device_id) return -12;

    uint32_t qci_count = pCreateInfo->pQueueCreateInfos ?
                         pCreateInfo->queueCreateInfoCount : 0u;
    uint32_t layer_count = pCreateInfo->ppEnabledLayerNames ?
                           pCreateInfo->enabledLayerCount : 0u;
    uint32_t ext_count = pCreateInfo->ppEnabledExtensionNames ?
                         pCreateInfo->enabledExtensionCount : 0u;
    uint32_t host_ext_count =
        vcd_count_host_extensions(ext_count,
            (const char *const *)pCreateInfo->ppEnabledExtensionNames);
    const VkPhysicalDeviceVulkan13Features *vk13 =
        vcd_find_vulkan13_features(pCreateInfo);

    vcd_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCreateDevice);
    vcd_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcd_wr_u64(&wr, physical_device_id);

    vcd_wr_u64(&wr, 1);                               /* pCreateInfo */
    vcd_wr_i32(&wr, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    vcd_wr_vulkan13_features_pnext(&wr, vk13);
    vcd_wr_u32(&wr, pCreateInfo->flags);
    vcd_wr_u32(&wr, qci_count);
    vcd_wr_u64(&wr, qci_count);                       /* pQueueCreateInfos */
    for (uint32_t i = 0; i < qci_count; i++)
        vcd_wr_queue_info(&wr, &pCreateInfo->pQueueCreateInfos[i]);

    vcd_wr_u32(&wr, layer_count);
    vcd_wr_string_array(&wr, layer_count,
                        (const char *const *)pCreateInfo->ppEnabledLayerNames);
    vcd_wr_u32(&wr, host_ext_count);
    vcd_wr_host_extension_array(&wr, ext_count,
        (const char *const *)pCreateInfo->ppEnabledExtensionNames);

    if (pCreateInfo->pEnabledFeatures) {
        vcd_wr_u64(&wr, 1);                           /* pEnabledFeatures */
        vcd_wr_bytes(&wr, pCreateInfo->pEnabledFeatures,
                     (uint32_t)sizeof(*pCreateInfo->pEnabledFeatures));
    } else {
        vcd_wr_u64(&wr, 0);
    }

    vcd_wr_u64(&wr, 0);                               /* pAllocator */
    vcd_wr_u64(&wr, 1);                               /* pDevice */
    vcd_wr_u64(&wr, device_id);
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t present   = *(uint64_t *)(reply + 8);
    uint64_t dev_reply = *(uint64_t *)(reply + 16);
    printf("[VCD] reply cmd=%u vk=%u present=%llu dev=%llu guest=%llu bytes=%u qci=%u ext=%u/%u vk13=%u sync2=%u dyn=%u maint4=%u\n",
           reply_cmd, vk_result, (unsigned long long)present,
           (unsigned long long)dev_reply, (unsigned long long)device_id,
           wr.off, qci_count, host_ext_count, ext_count, vk13 ? 1u : 0u,
           vk13 ? (uint32_t)vk13->synchronization2 : 0u,
           vk13 ? (uint32_t)vk13->dynamicRendering : 0u,
           vk13 ? (uint32_t)vk13->maintenance4 : 0u);

    if (reply_cmd != VN_CMD_TYPE_vkCreateDevice)
        return -5;
    if (vk_result != VK_SUCCESS)
        return (int)vk_result;
    if (!present || !dev_reply)
        return -5;

    /* Later command streams refer to the guest object id registered above.
     * dev_reply is useful diagnostics, but may be the native host handle. */
    *out_device_id = device_id;
    return VK_SUCCESS;
}
