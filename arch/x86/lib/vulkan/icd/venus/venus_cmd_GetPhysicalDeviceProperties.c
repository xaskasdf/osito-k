/*
 * Encoder for vkGetPhysicalDeviceProperties over venus.
 *
 * Request payload:
 *   pd_id            (u64)  host-assigned physical-device handle
 *   pProperties_present (u32, always 1)
 *   pad              (u32)
 *
 * Reply payload:
 *   Flattened VkPhysicalDeviceProperties (sizeof(...) bytes).
 *
 * Mesa venus serializes this struct field-by-field; for W3b.2 we trust
 * the host to return the native C layout because both guest and host
 * use the same vulkan.h version. If the layout diverges, swap to a
 * real per-field deserializer. */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

extern void *memcpy(void *, const void *, unsigned long);

int venus_cmd_encode_GetPhysicalDeviceProperties(
        struct venus_wire *w, uint64_t pd_id,
        VkPhysicalDeviceProperties *out) {
    if (!w || !out) return -22;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkGetPhysicalDeviceProperties,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            8u + 4u + 4u /* pd_id + present + pad */,
            &reply_id);
    if (!p) return -12 /* ENOMEM */;

    ((uint64_t *)p)[0]         = pd_id;
    ((uint32_t *)(p + 8))[0]   = 1u;   /* pProperties present */
    ((uint32_t *)(p + 12))[0]  = 0u;   /* pad */

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    int got = venus_wire_wait_reply(w, reply_id, out,
                                    (uint32_t)sizeof(VkPhysicalDeviceProperties));
    if (got < (int)sizeof(VkPhysicalDeviceProperties)) return -5 /* EIO */;
    return 0;
}
