/* Bootstrap encoder for vkCreateInstance over the real Mesa venus-protocol.
 * The renderer decodes a stream of:
 *   VkCommandTypeEXT, VkCommandFlagsEXT, generated command arguments...
 * Replies are written to the stream selected by vkSetReplyCommandStreamMESA.
 */
#include "venus_wire.h"
#include "venus.h"

extern unsigned long strlen(const char *);
extern int printf(const char *, ...);

#define VN_CMD_TYPE_vkCreateInstance 0u
#define VN_CMD_GENERATE_REPLY 1u

struct vn_writer {
    uint8_t *buf;
    uint32_t off;
    uint32_t cap;
    int err;
};

static void wr_bytes(struct vn_writer *w, const void *src, uint32_t n) {
    if (w->err) return;
    if (w->off + n > w->cap || w->off + n < w->off) {
        w->err = -12;
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        w->buf[w->off + i] = ((const uint8_t *)src)[i];
    w->off += n;
}

static void wr_u32(struct vn_writer *w, uint32_t v) {
    wr_bytes(w, &v, 4);
}

static void wr_i32(struct vn_writer *w, int32_t v) {
    wr_bytes(w, &v, 4);
}

static void wr_u64(struct vn_writer *w, uint64_t v) {
    wr_bytes(w, &v, 8);
}

static void wr_str(struct vn_writer *w, const char *s) {
    uint64_t n = s ? (uint64_t)strlen(s) + 1u : 0u;
    wr_u64(w, n);
    if (!n) return;
    wr_bytes(w, s, (uint32_t)n);
    while (w->off & 3u) {
        uint8_t z = 0;
        wr_bytes(w, &z, 1);
    }
}

int venus_cmd_encode_CreateInstance(struct venus_wire *w,
                                    const VkInstanceCreateInfo *pCreateInfo,
                                    uint64_t *out_host_handle) {
    if (!w || !pCreateInfo || !out_host_handle) return -22;
    const VkApplicationInfo *app = pCreateInfo->pApplicationInfo;

    uint8_t cmd[2048];
    struct vn_writer wr = { cmd, 0, sizeof(cmd), 0 };
    uint64_t instance_id = venus_wire_alloc_object_id(w);
    if (!instance_id) return -12;

    wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCreateInstance);
    wr_u32(&wr, VN_CMD_GENERATE_REPLY);

    wr_u64(&wr, 1);                                      /* pCreateInfo */
    wr_i32(&wr, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    wr_u64(&wr, 0);                                      /* pNext */
    wr_u32(&wr, pCreateInfo->flags);
    if (app) {
        wr_u64(&wr, 1);                                  /* pApplicationInfo */
        wr_i32(&wr, VK_STRUCTURE_TYPE_APPLICATION_INFO);
        wr_u64(&wr, 0);                                  /* pNext */
        wr_str(&wr, app->pApplicationName);
        wr_u32(&wr, app->applicationVersion);
        wr_str(&wr, app->pEngineName);
        wr_u32(&wr, app->engineVersion);
        wr_u32(&wr, app->apiVersion);
    } else {
        wr_u64(&wr, 0);
    }

    wr_u32(&wr, 0);                                      /* enabledLayerCount */
    wr_u64(&wr, 0);                                      /* ppEnabledLayerNames */
    wr_u32(&wr, 0);                                      /* enabledExtensionCount */
    wr_u64(&wr, 0);                                      /* ppEnabledExtensionNames */
    wr_u64(&wr, 0);                                      /* pAllocator */
    wr_u64(&wr, 1);                                      /* pInstance */
    wr_u64(&wr, instance_id);
    if (wr.err) return wr.err;

    uint8_t reply[24] = {0};
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t present   = *(uint64_t *)(reply + 8);
    uint64_t handle_id = *(uint64_t *)(reply + 16);
    printf("[VCI] reply cmd=%u vk=%u present=%llu handle=%llu guestid=%llu bytes=%u\n",
           reply_cmd, vk_result,
           (unsigned long long)present,
           (unsigned long long)handle_id,
           (unsigned long long)instance_id, wr.off);
    if (!present || !handle_id) {
        printf("[VCI] reply raw %02x %02x %02x %02x %02x %02x %02x %02x "
               "%02x %02x %02x %02x %02x %02x %02x %02x "
               "%02x %02x %02x %02x %02x %02x %02x %02x\n",
               reply[0], reply[1], reply[2], reply[3],
               reply[4], reply[5], reply[6], reply[7],
               reply[8], reply[9], reply[10], reply[11],
               reply[12], reply[13], reply[14], reply[15],
               reply[16], reply[17], reply[18], reply[19],
               reply[20], reply[21], reply[22], reply[23]);
    }
    if (reply_cmd != VN_CMD_TYPE_vkCreateInstance) return -5;
    if (vk_result != VK_SUCCESS) return (int)vk_result;
    if (!present || !handle_id) return -5;
    /* Venus object references in later commands are guest-allocated object
     * ids, not the native VkInstance handle returned for diagnostics. */
    *out_host_handle = instance_id;
    return VK_SUCCESS;
}
