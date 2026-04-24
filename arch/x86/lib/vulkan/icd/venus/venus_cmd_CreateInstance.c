/*
 * Encoder for vkCreateInstance over venus.
 * Packet layout (per Mesa venus-protocol):
 *   1. pCreateInfo: non-null marker (1 uint32), then flattened struct:
 *        flags (uint32), appInfo_present (uint32), then appInfo if present:
 *          apiVersion (uint32), applicationVersion (uint32),
 *          engineVersion (uint32), pApplicationName_len (uint32),
 *          applicationName bytes (padded to 8), pEngineName_len,
 *          engineName bytes.
 *        enabledLayerCount (uint32), enabledExtensionCount (uint32),
 *        … (W3b.1 assumes 0/0 for simplicity).
 *   2. pAllocator: 0 (null marker).
 *   3. pInstance: 0 (null marker; host will fill in reply).
 *
 * Reply:
 *   VkResult (uint32) + VkInstance handle-id (uint64).
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

extern unsigned long strlen(const char *);
extern void *memcpy(void *, const void *, unsigned long);

/* ── packet writer helpers ─────────────────────────────────── */

static uint8_t *put_u32(uint8_t *p, uint32_t v) {
    ((uint32_t *)p)[0] = v;
    return p + 4;
}

__attribute__((unused))
static uint8_t *put_u64(uint8_t *p, uint64_t v) {
    ((uint64_t *)p)[0] = v;
    return p + 8;
}

static uint8_t *put_str(uint8_t *p, const char *s) {
    uint32_t n = s ? (uint32_t)strlen(s) + 1 : 0;
    /* Field total: 4-byte length + n bytes + pad so (4 + n) rounds to 8. */
    uint32_t field_total = (4u + n + 7u) & ~7u;
    uint32_t pad = field_total - 4u - n;
    ((uint32_t *)p)[0] = n;
    p += 4;
    if (n) { memcpy(p, s, n); p += n; }
    /* Zero the pad bytes so we don't leak stack garbage into the wire. */
    for (uint32_t i = 0; i < pad; i++) p[i] = 0;
    return p + pad;
}

static uint32_t str_wire_len(const char *s) {
    uint32_t n = s ? (uint32_t)strlen(s) + 1 : 0;
    uint32_t t = 4 + n;
    return (t + 7) & ~7u;
}

/* ── public encoder entry ───────────────────────────────────── */

int venus_cmd_encode_CreateInstance(struct venus_wire *w,
                                    const VkInstanceCreateInfo *pCreateInfo,
                                    uint64_t *out_host_handle) {
    if (!w || !pCreateInfo || !out_host_handle) return -22;
    const VkApplicationInfo *app = pCreateInfo->pApplicationInfo;

    /* Compute payload size. */
    uint32_t sz = 4;         /* pCreateInfo_present */
    sz += 4;                 /* flags */
    sz += 4;                 /* appInfo_present */
    if (app) {
        sz += 4 * 3;         /* apiVersion + applicationVersion + engineVersion */
        sz += str_wire_len(app->pApplicationName);
        sz += str_wire_len(app->pEngineName);
    }
    sz += 4;                 /* enabledLayerCount (0 in W3b.1) */
    sz += 4;                 /* enabledExtensionCount (0) */
    sz += 4;                 /* pAllocator_present (0) */
    sz += 4;                 /* pInstance_present (0, host fills reply) */

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(w, VN_CMD_vkCreateInstance,
                                      VENUS_CMD_FLAG_REPLY_EXPECTED,
                                      sz, &reply_id);
    if (!p) return -12 /* ENOMEM */;

    uint8_t *c = p;
    c = put_u32(c, 1);                                 /* pCreateInfo present */
    c = put_u32(c, pCreateInfo->flags);
    c = put_u32(c, app ? 1 : 0);                       /* appInfo present */
    if (app) {
        c = put_u32(c, app->apiVersion);
        c = put_u32(c, app->applicationVersion);
        c = put_u32(c, app->engineVersion);
        c = put_str(c, app->pApplicationName);
        c = put_str(c, app->pEngineName);
    }
    c = put_u32(c, 0);                                 /* enabledLayerCount */
    c = put_u32(c, 0);                                 /* enabledExtensionCount */
    c = put_u32(c, 0);                                 /* pAllocator null */
    c = put_u32(c, 0);                                 /* pInstance null (reply fills) */

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    rc = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (rc < 12) return -5 /* EIO */;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    uint64_t handle_id = *(uint64_t *)&reply[4];
    *out_host_handle = handle_id;
    return (int)vk_result;
}
