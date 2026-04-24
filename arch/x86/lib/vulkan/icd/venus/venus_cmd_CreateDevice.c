/*
 * Encoder for vkCreateDevice over venus.
 *
 * Request payload (flattened VkDeviceCreateInfo — W3b.3 subset; extensions,
 * layers, and pEnabledFeatures are skipped but their "present" flags are
 * emitted for protocol alignment):
 *   pd_id                    (u64)  host-assigned physical-device handle
 *   flags                    (u32)
 *   queueCreateInfoCount     (u32)
 *   for each queue create info:
 *     queue_flags            (u32)
 *     queueFamilyIndex       (u32)
 *     queueCount             (u32)
 *     priorities_len         (u32)
 *     priorities_len * f32   (variable)
 *   enabledLayerCount        (u32)   always 0
 *   enabledExtensionCount    (u32)   always 0
 *   pEnabledFeatures_present (u32)   always 0 (null)
 *   pAllocator_present       (u32)   always 0
 *   pDevice_present          (u32)   always 1
 *
 * Reply payload:
 *   VkResult                 (u32)
 *   pad                      (u32)
 *   host VkDevice id         (u64)
 *
 * See master plan §W3b.3 T2. */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

extern void *memcpy(void *, const void *, unsigned long);

/* Guard against pathological queueCreateInfoCount — our stack buffer caps
 * the payload at 512 bytes which is plenty for any sane Vulkan device. */
#define VENUS_CD_MAX_QCI 8u
#define VENUS_CD_MAX_PRIO 16u

int venus_cmd_encode_CreateDevice(
        struct venus_wire *w, uint64_t pd_id,
        const VkDeviceCreateInfo *pCreateInfo,
        uint64_t *out_device_id) {
    if (!w || !pCreateInfo || !out_device_id) return -22;

    uint32_t qci_count = pCreateInfo->queueCreateInfoCount;
    if (qci_count > VENUS_CD_MAX_QCI) qci_count = VENUS_CD_MAX_QCI;

    /* Compute payload size up front. */
    uint32_t size = 8u + 4u + 4u; /* pd_id + flags + qci_count */
    for (uint32_t i = 0; i < qci_count; i++) {
        const VkDeviceQueueCreateInfo *qi = &pCreateInfo->pQueueCreateInfos[i];
        uint32_t plen = qi->queueCount;
        if (plen > VENUS_CD_MAX_PRIO) plen = VENUS_CD_MAX_PRIO;
        size += 4u + 4u + 4u + 4u + plen * 4u;
    }
    size += 4u + 4u + 4u + 4u + 4u; /* layerCount + extCount + 3 present flags */

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCreateDevice,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            size, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = pd_id;                  off += 8;
    *(uint32_t *)(p + off) = pCreateInfo->flags;     off += 4;
    *(uint32_t *)(p + off) = qci_count;              off += 4;
    for (uint32_t i = 0; i < qci_count; i++) {
        const VkDeviceQueueCreateInfo *qi = &pCreateInfo->pQueueCreateInfos[i];
        uint32_t plen = qi->queueCount;
        if (plen > VENUS_CD_MAX_PRIO) plen = VENUS_CD_MAX_PRIO;
        *(uint32_t *)(p + off) = qi->flags;              off += 4;
        *(uint32_t *)(p + off) = qi->queueFamilyIndex;   off += 4;
        *(uint32_t *)(p + off) = qi->queueCount;         off += 4;
        *(uint32_t *)(p + off) = plen;                   off += 4;
        for (uint32_t j = 0; j < plen; j++) {
            float pr = qi->pQueuePriorities ? qi->pQueuePriorities[j] : 0.0f;
            *(float *)(p + off) = pr;                    off += 4;
        }
    }
    *(uint32_t *)(p + off) = 0u; off += 4; /* enabledLayerCount */
    *(uint32_t *)(p + off) = 0u; off += 4; /* enabledExtensionCount */
    *(uint32_t *)(p + off) = 0u; off += 4; /* pEnabledFeatures null */
    *(uint32_t *)(p + off) = 0u; off += 4; /* pAllocator null */
    *(uint32_t *)(p + off) = 1u; off += 4; /* pDevice present */

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    uint64_t dev_id    = *(uint64_t *)(reply + 8);
    *out_device_id = dev_id;
    return (int)vk_result;
}
