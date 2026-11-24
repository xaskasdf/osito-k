/*
 * Encoder for vkGetDeviceQueue over venus.
 *
 * Queue slot allocator.  The handle returned to the app is still a
 * dispatchable pointer into venus_device->queues[slot], but when the
 * wire is live we also initialize the renderer-side queue object with
 * vkGetDeviceQueue2 so later QueueSubmit/QueueSubmit2 can reference a
 * real host queue id.
 *
 * Assignment rule: the (queue_family_index, queue_index) pair maps
 * 1:1 to a slot; duplicate Get calls for the same pair return the
 * same slot. See master plan §W3b.5 T2.
 */
#include "venus.h"
#include "venus_cmd_writer.h"
#include "venus_wire.h"

extern void *memset(void *, int, unsigned long);
extern int printf(const char *, ...);

#define VN_CMD_TYPE_vkGetDeviceQueue2 155u
#define VN_CMD_GENERATE_REPLY 1u
#define VK_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA_OSITOK \
    ((VkStructureType)1000384005)

static int venus_cmd_init_host_queue(struct venus_device *dev,
                                     struct venus_queue *q) {
    if (!dev || !q) return -22;
    if (q->host_id != 0) return 0;
    if (!dev->parent || !dev->parent->wire || dev->host_handle == 0)
        return 0;

    uint64_t queue_id = venus_wire_alloc_object_id(dev->parent->wire);
    if (!queue_id) return -12;

    uint8_t cmd[96];
    uint8_t reply[20];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkGetDeviceQueue2);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev->host_handle);
    vcw_wr_u64(&wr, 1); /* pQueueInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2);
    vcw_wr_u64(&wr, 1); /* VkDeviceQueueTimelineInfoMESA pNext */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA_OSITOK);
    vcw_wr_u64(&wr, 0); /* pNext */
    vcw_wr_u32(&wr, q->queue_index + 1u); /* ringIdx 0 is invalid */
    vcw_wr_u32(&wr, 0); /* flags */
    vcw_wr_u32(&wr, q->queue_family_index);
    vcw_wr_u32(&wr, q->queue_index);
    vcw_wr_u64(&wr, 1); /* pQueue */
    vcw_wr_u64(&wr, queue_id);
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(dev->parent->wire, cmd, wr.off,
                                     reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint64_t present = *(uint64_t *)(reply + 4);
    uint64_t queue_reply = *(uint64_t *)(reply + 12);
    static uint32_t log_count;
    if (log_count < 16u) {
        log_count++;
        printf("[VGQ2] reply cmd=%u present=%llu queue=%llu guest=%llu bytes=%u\n",
               reply_cmd, (unsigned long long)present,
               (unsigned long long)queue_reply,
               (unsigned long long)queue_id, wr.off);
    }
    if (reply_cmd != VN_CMD_TYPE_vkGetDeviceQueue2 || !present)
        return -5;

    q->host_id = queue_id;
    return 0;
}

int venus_cmd_encode_GetDeviceQueue(
        struct venus_device *dev,
        uint32_t queue_family_index, uint32_t queue_index,
        struct venus_queue **out_q) {
    if (!dev || !out_q) return -22;

    /* Return existing slot if the pair was already requested. */
    for (uint32_t i = 0; i < VENUS_MAX_QUEUE_OBJECTS; i++) {
        struct venus_queue *q = &dev->queues[i];
        if (q->in_use && q->queue_family_index == queue_family_index &&
            q->queue_index == queue_index) {
            (void)venus_cmd_init_host_queue(dev, q);
            *out_q = q;
            return 0;
        }
    }
    /* Allocate. */
    for (uint32_t i = 0; i < VENUS_MAX_QUEUE_OBJECTS; i++) {
        struct venus_queue *q = &dev->queues[i];
        if (!q->in_use) {
            memset(q, 0, sizeof(*q));
            set_loader_magic_value(&q->loader_data);
            q->owner = dev;
            q->queue_family_index = queue_family_index;
            q->queue_index = queue_index;
            q->in_use = 1;
            (void)venus_cmd_init_host_queue(dev, q);
            *out_q = q;
            return 0;
        }
    }
    return -12;
}
