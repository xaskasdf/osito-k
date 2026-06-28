#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkAllocateMemory 21u
#define VN_CMD_GENERATE_REPLY 1u

extern int printf(const char *, ...);

int venus_cmd_encode_AllocateMemory(
        struct venus_wire *w, uint64_t dev_id,
        uint64_t allocation_size, uint32_t memory_type_index,
        uint64_t *out_memory_id) {
    if (!w || !out_memory_id) return -22;

    extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);

    uint8_t cmd[256];
    uint8_t reply[24];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };
    uint64_t memory_id = venus_wire_alloc_object_id(w);
    if (!memory_id) return -12;

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkAllocateMemory);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);

    vcw_wr_u64(&wr, 1);                                      /* pAllocateInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    vcw_wr_u64(&wr, 0);                                      /* pNext */
    vcw_wr_u64(&wr, allocation_size);
    vcw_wr_u32(&wr, memory_type_index);
    vcw_wr_u64(&wr, 0);                                      /* pAllocator */
    vcw_wr_u64(&wr, 1);                                      /* pMemory */
    vcw_wr_u64(&wr, memory_id);
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t present   = *(uint64_t *)(reply + 8);
    uint64_t mem_reply = *(uint64_t *)(reply + 16);
    static uint32_t log_count;
    if (log_count < 16u) {
        log_count++;
        printf("[VAM] reply cmd=%u vk=%u present=%llu mem=%llu guest=%llu size=%llu type=%u bytes=%u\n",
               reply_cmd, vk_result, (unsigned long long)present,
               (unsigned long long)mem_reply,
               (unsigned long long)memory_id,
               (unsigned long long)allocation_size,
               memory_type_index, wr.off);
    }
    if (reply_cmd != VN_CMD_TYPE_vkAllocateMemory)
        return -5;
    if (vk_result != VK_SUCCESS)
        return (int)vk_result;
    if (!present || !mem_reply)
        return -5;

    *out_memory_id = memory_id;
    return VK_SUCCESS;
}
