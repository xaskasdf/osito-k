/*
 * Encoder for vkGetPhysicalDeviceQueueFamilyProperties over venus.
 *
 * Vulkan call idiom: two-pass (NULL to size, then array). We do a
 * single wire round-trip: the host always reports count_out plus as
 * many entries as count_in permits; guest caller decides which pass
 * they are in based on their own args.
 *
 * Request payload:
 *   pd_id                  (u64)
 *   pCount_present         (u32, always 1 — we always want the count back)
 *   count_in               (u32, 0 = size-query, >0 = fill up to that many)
 *   pQueueFamilies_present (u32, 1 if caller buffer non-NULL)
 *   pad                    (u32)
 *
 * Reply:
 *   count_out              (u32)
 *   pad                    (u32)
 *   array of VkQueueFamilyProperties — min(count_in, count_out) entries. */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

extern void *memcpy(void *, const void *, unsigned long);

int venus_cmd_encode_GetPhysicalDeviceQueueFamilyProperties(
        struct venus_wire *w, uint64_t pd_id,
        uint32_t *pCount, VkQueueFamilyProperties *pQueueFamilies) {
    if (!w || !pCount) return -22;

    uint32_t count_in = pQueueFamilies ? *pCount : 0u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkGetPhysicalDeviceQueueFamilyProperties,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            8u + 4u + 4u + 4u + 4u,
            &reply_id);
    if (!p) return -12;

    ((uint64_t *)p)[0]        = pd_id;
    ((uint32_t *)(p + 8))[0]  = 1u;                                /* pCount present */
    ((uint32_t *)(p + 12))[0] = count_in;
    ((uint32_t *)(p + 16))[0] = pQueueFamilies ? 1u : 0u;          /* array present */
    ((uint32_t *)(p + 20))[0] = 0u;                                /* pad */

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    /* Worst-case reply: 8B header + 32 entries * 24B each = 776B. The
     * ring reply area is 8KiB so we can hold whatever host returns for
     * a sane device. We allocate a conservative stack buffer. */
    uint8_t reply[1024] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 8) return -5;

    uint32_t count_out = ((uint32_t *)reply)[0];
    *pCount = count_out;

    if (pQueueFamilies && count_in > 0) {
        uint32_t n = (count_out < count_in) ? count_out : count_in;
        uint32_t bytes = n * (uint32_t)sizeof(VkQueueFamilyProperties);
        if ((int)(8u + bytes) > got) {
            /* Host didn't send enough — treat as IO error. */
            return -5;
        }
        memcpy(pQueueFamilies, reply + 8, bytes);
    }
    return 0;
}
