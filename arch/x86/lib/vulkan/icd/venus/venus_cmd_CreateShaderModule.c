#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkCreateShaderModule 59u
#define VN_CMD_GENERATE_REPLY 1u
#define VENUS_SUBMIT_MAX_BYTES (1u << 20)

extern void *malloc(unsigned long);
extern void free(void *);
extern int printf(const char *, ...);

int venus_cmd_encode_CreateShaderModule(
        struct venus_wire *w, uint64_t dev_id,
        const VkShaderModuleCreateInfo *pCreateInfo,
        uint64_t *out_shader_id) {
    if (!w || !pCreateInfo || !out_shader_id) return -22;

    extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);

    uint64_t code_words = pCreateInfo->codeSize / 4u;
    if (!pCreateInfo->pCode || (pCreateInfo->codeSize & 3u))
        return VK_ERROR_INITIALIZATION_FAILED;

    uint64_t cmd_size64 = 4u + 4u + 8u + 8u + 4u + 8u + 4u + 8u +
                          8u + code_words * 4u + 8u + 8u + 8u;
    if (cmd_size64 > VENUS_SUBMIT_MAX_BYTES)
        return VK_ERROR_INITIALIZATION_FAILED;

    uint8_t *cmd = malloc((unsigned long)cmd_size64);
    if (!cmd) return -12;

    uint8_t reply[24];
    struct venus_cmd_writer wr = { cmd, 0, (uint32_t)cmd_size64, 0 };
    uint64_t shader_id = venus_wire_alloc_object_id(w);
    if (!shader_id) {
        free(cmd);
        return -12;
    }

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCreateShaderModule);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);

    vcw_wr_u64(&wr, 1);                                      /* pCreateInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    vcw_wr_u64(&wr, 0);                                      /* pNext */
    vcw_wr_u32(&wr, pCreateInfo->flags);
    vcw_wr_u64(&wr, pCreateInfo->codeSize);
    vcw_wr_array_size(&wr, code_words);
    for (uint64_t i = 0; i < code_words; i++)
        vcw_wr_u32(&wr, pCreateInfo->pCode[i]);

    vcw_wr_u64(&wr, 0);                                      /* pAllocator */
    vcw_wr_u64(&wr, 1);                                      /* pShaderModule */
    vcw_wr_u64(&wr, shader_id);
    if (wr.err) {
        free(cmd);
        return wr.err;
    }

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    free(cmd);
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t present   = *(uint64_t *)(reply + 8);
    uint64_t shader_reply = *(uint64_t *)(reply + 16);
    static uint32_t log_count;
    if (log_count < 32u) {
        log_count++;
        printf("[VSHADER] reply cmd=%u vk=%u present=%llu shader=%llu guest=%llu bytes=%u code=%llu\n",
               reply_cmd, vk_result, (unsigned long long)present,
               (unsigned long long)shader_reply,
               (unsigned long long)shader_id, wr.off,
               (unsigned long long)pCreateInfo->codeSize);
    }
    if (reply_cmd != VN_CMD_TYPE_vkCreateShaderModule)
        return -5;
    if (vk_result != VK_SUCCESS)
        return (int)vk_result;
    if (!present || !shader_reply)
        return -5;

    *out_shader_id = shader_id;
    return VK_SUCCESS;
}
