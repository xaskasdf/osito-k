#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkCreateDescriptorSetLayout 72u
#define VN_CMD_GENERATE_REPLY 1u

extern int printf(const char *, ...);

static const VkDescriptorSetLayoutBindingFlagsCreateInfo *
venus_find_binding_flags(const void *pnext) {
    const VkBaseInStructure *base = (const VkBaseInStructure *)pnext;
    while (base) {
        if (base->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)
            return (const VkDescriptorSetLayoutBindingFlagsCreateInfo *)base;
        base = base->pNext;
    }
    return 0;
}

static void venus_write_dsl_pnext(struct venus_cmd_writer *wr,
                                  const VkDescriptorSetLayoutCreateInfo *ci) {
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *bf =
        venus_find_binding_flags(ci->pNext);
    if (!bf) {
        vcw_wr_u64(wr, 0);
        return;
    }

    vcw_wr_u64(wr, 1);
    vcw_wr_i32(wr, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    vcw_wr_u64(wr, 0); /* pNext */
    vcw_wr_u32(wr, bf->bindingCount);
    vcw_wr_array_size(wr, bf->bindingCount);
    for (uint32_t i = 0; i < bf->bindingCount; i++) {
        uint32_t flags = bf->pBindingFlags ? bf->pBindingFlags[i] : 0u;
        vcw_wr_u32(wr, flags);
    }
}

int venus_cmd_encode_CreateDescriptorSetLayout(
        struct venus_wire *w, uint64_t dev_id,
        const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
        uint64_t *out_layout_id) {
    if (!w || !pCreateInfo || !out_layout_id) return -22;

    extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);

    uint8_t cmd[8192];
    uint8_t reply[24];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };
    uint64_t layout_id = venus_wire_alloc_object_id(w);
    if (!layout_id) return -12;

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCreateDescriptorSetLayout);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);

    vcw_wr_u64(&wr, 1); /* pCreateInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    venus_write_dsl_pnext(&wr, pCreateInfo);
    vcw_wr_u32(&wr, pCreateInfo->flags);
    vcw_wr_u32(&wr, pCreateInfo->bindingCount);
    vcw_wr_array_size(&wr, pCreateInfo->bindingCount);
    for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *b = &pCreateInfo->pBindings[i];
        vcw_wr_u32(&wr, b->binding);
        vcw_wr_i32(&wr, (int32_t)b->descriptorType);
        vcw_wr_u32(&wr, b->descriptorCount);
        vcw_wr_u32(&wr, b->stageFlags);
        if (b->pImmutableSamplers && b->descriptorCount) {
            vcw_wr_array_size(&wr, b->descriptorCount);
            for (uint32_t j = 0; j < b->descriptorCount; j++)
                vcw_wr_u64(&wr, 0); /* sampler host ids are not wired yet */
        } else {
            vcw_wr_array_size(&wr, 0);
        }
    }

    vcw_wr_u64(&wr, 0); /* pAllocator */
    vcw_wr_u64(&wr, 1); /* pSetLayout */
    vcw_wr_u64(&wr, layout_id);
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t present = *(uint64_t *)(reply + 8);
    uint64_t layout_reply = *(uint64_t *)(reply + 16);
    static uint32_t log_count;
    if (log_count < 32u) {
        log_count++;
        printf("[VDSL] reply cmd=%u vk=%u present=%llu dsl=%llu guest=%llu bindings=%u bytes=%u\n",
               reply_cmd, vk_result, (unsigned long long)present,
               (unsigned long long)layout_reply,
               (unsigned long long)layout_id,
               pCreateInfo->bindingCount, wr.off);
    }
    if (reply_cmd != VN_CMD_TYPE_vkCreateDescriptorSetLayout)
        return -5;
    if (vk_result != VK_SUCCESS)
        return (int)vk_result;
    if (!present || !layout_reply)
        return -5;

    *out_layout_id = layout_id;
    return VK_SUCCESS;
}
