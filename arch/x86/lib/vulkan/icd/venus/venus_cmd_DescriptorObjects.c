#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkCreateSampler          70u
#define VN_CMD_TYPE_vkDestroySampler         71u
#define VN_CMD_TYPE_vkCreateDescriptorPool   74u
#define VN_CMD_TYPE_vkDestroyDescriptorPool  75u
#define VN_CMD_TYPE_vkAllocateDescriptorSets 77u
#define VN_CMD_TYPE_vkFreeDescriptorSets     78u
#define VN_CMD_TYPE_vkUpdateDescriptorSets   79u
#define VN_CMD_TYPE_vkCmdBindDescriptorSets  103u
#define VN_CMD_GENERATE_REPLY                1u

#define VENUS_LOCAL_SLOT_MASK 0x0fffull
#define LOCAL_SLOT(h) ((int)(((uint64_t)(h) >> 48) & VENUS_LOCAL_SLOT_MASK))

extern void *malloc(unsigned long);
extern void free(void *);
extern int printf(const char *, ...);
extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);

static void wr_float(struct venus_cmd_writer *wr, float v) {
    vcw_wr_bytes(wr, &v, 4);
}

static uint64_t host_sampler(struct venus_device *dev, VkSampler sampler) {
    int slot = sampler ? LOCAL_SLOT(sampler) : -1;
    if (slot < 0 || slot >= (int)VENUS_MAX_SAMPLER_OBJECTS)
        return 0;
    if (!dev->samplers[slot].in_use)
        return 0;
    return dev->samplers[slot].host_id;
}

static uint64_t host_image_view(struct venus_device *dev, VkImageView view) {
    int slot = view ? LOCAL_SLOT(view) : -1;
    if (slot < 0 || slot >= (int)VENUS_MAX_IMAGE_VIEW_OBJECTS)
        return 0;
    if (!dev->image_views[slot].in_use)
        return 0;
    return dev->image_views[slot].host_id;
}

static uint64_t host_buffer(struct venus_device *dev, VkBuffer buffer) {
    int slot = buffer ? LOCAL_SLOT(buffer) : -1;
    if (slot < 0 || slot >= (int)VENUS_MAX_BUF_OBJECTS)
        return 0;
    if (!dev->buffers[slot].in_use)
        return 0;
    return dev->buffers[slot].host_id;
}

static uint64_t host_buffer_view(struct venus_device *dev, VkBufferView view) {
    int slot = view ? LOCAL_SLOT(view) : -1;
    if (slot < 0 || slot >= (int)VENUS_MAX_BUFFER_VIEW_OBJECTS)
        return 0;
    if (!dev->buffer_views[slot].in_use)
        return 0;
    return dev->buffer_views[slot].host_id;
}

static uint64_t host_desc_set(struct venus_device *dev, VkDescriptorSet set) {
    int slot = set ? LOCAL_SLOT(set) : -1;
    if (slot < 0 || slot >= (int)VENUS_MAX_DESC_SET_OBJECTS)
        return 0;
    if (!dev->desc_sets[slot].in_use)
        return 0;
    return dev->desc_sets[slot].host_id;
}

static uint64_t host_pl_layout(struct venus_device *dev, VkPipelineLayout layout) {
    int slot = layout ? LOCAL_SLOT(layout) : -1;
    if (slot < 0 || slot >= (int)VENUS_MAX_PL_LAYOUT_OBJECTS)
        return 0;
    if (!dev->pl_layouts[slot].in_use)
        return 0;
    return dev->pl_layouts[slot].host_id;
}

static int desc_type_needs_image(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}

static int desc_type_needs_sampler(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_SAMPLER ||
           type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

static int desc_type_needs_buffer(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static int desc_type_needs_texel_view(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}

int venus_cmd_encode_CreateSampler(struct venus_wire *w, uint64_t dev_id,
                                   const VkSamplerCreateInfo *ci,
                                   uint64_t *out_sampler_id) {
    if (!w || !ci || !out_sampler_id) return -22;

    uint8_t cmd[256];
    uint8_t reply[24];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };
    uint64_t sampler_id = venus_wire_alloc_object_id(w);
    if (!sampler_id) return -12;

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCreateSampler);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, 1); /* pCreateInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    vcw_wr_u64(&wr, 0); /* pNext */
    vcw_wr_u32(&wr, ci->flags);
    vcw_wr_i32(&wr, (int32_t)ci->magFilter);
    vcw_wr_i32(&wr, (int32_t)ci->minFilter);
    vcw_wr_i32(&wr, (int32_t)ci->mipmapMode);
    vcw_wr_i32(&wr, (int32_t)ci->addressModeU);
    vcw_wr_i32(&wr, (int32_t)ci->addressModeV);
    vcw_wr_i32(&wr, (int32_t)ci->addressModeW);
    wr_float(&wr, ci->mipLodBias);
    vcw_wr_u32(&wr, ci->anisotropyEnable);
    wr_float(&wr, ci->maxAnisotropy);
    vcw_wr_u32(&wr, ci->compareEnable);
    vcw_wr_i32(&wr, (int32_t)ci->compareOp);
    wr_float(&wr, ci->minLod);
    wr_float(&wr, ci->maxLod);
    vcw_wr_i32(&wr, (int32_t)ci->borderColor);
    vcw_wr_u32(&wr, ci->unnormalizedCoordinates);
    vcw_wr_u64(&wr, 0); /* pAllocator */
    vcw_wr_u64(&wr, 1); /* pSampler */
    vcw_wr_u64(&wr, sampler_id);
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++) reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t present = *(uint64_t *)(reply + 8);
    uint64_t sampler_reply = *(uint64_t *)(reply + 16);
    static uint32_t log_count;
    if (log_count < 32u) {
        log_count++;
        printf("[VSAMP] reply cmd=%u vk=%u present=%llu sampler=%llu guest=%llu bytes=%u\n",
               reply_cmd, vk_result, (unsigned long long)present,
               (unsigned long long)sampler_reply,
               (unsigned long long)sampler_id, wr.off);
    }
    if (reply_cmd != VN_CMD_TYPE_vkCreateSampler) return -5;
    if (vk_result != VK_SUCCESS) return (int)vk_result;
    if (!present || !sampler_reply) return -5;

    *out_sampler_id = sampler_id;
    return VK_SUCCESS;
}

int venus_cmd_encode_DestroySampler(struct venus_wire *w, uint64_t dev_id,
                                    uint64_t sampler_id) {
    if (!w || !sampler_id) return -22;
    uint8_t cmd[32];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkDestroySampler);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, sampler_id);
    vcw_wr_u64(&wr, 0); /* pAllocator */
    if (wr.err) return wr.err;
    return venus_wire_submit_raw(w, cmd, wr.off);
}

int venus_cmd_encode_CreateDescriptorPool(struct venus_wire *w, uint64_t dev_id,
                                          const VkDescriptorPoolCreateInfo *ci,
                                          uint64_t *out_pool_id) {
    if (!w || !ci || !out_pool_id) return -22;

    uint32_t cap = 128u + ci->poolSizeCount * 16u;
    uint8_t *cmd = (uint8_t *)malloc(cap);
    uint8_t reply[24];
    if (!cmd) return -12;

    struct venus_cmd_writer wr = { cmd, 0, cap, 0 };
    uint64_t pool_id = venus_wire_alloc_object_id(w);
    if (!pool_id) {
        free(cmd);
        return -12;
    }

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCreateDescriptorPool);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, 1); /* pCreateInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    vcw_wr_u64(&wr, 0); /* pNext */
    vcw_wr_u32(&wr, ci->flags);
    vcw_wr_u32(&wr, ci->maxSets);
    vcw_wr_u32(&wr, ci->poolSizeCount);
    vcw_wr_array_size(&wr, ci->poolSizeCount);
    for (uint32_t i = 0; i < ci->poolSizeCount; i++) {
        vcw_wr_i32(&wr, (int32_t)ci->pPoolSizes[i].type);
        vcw_wr_u32(&wr, ci->pPoolSizes[i].descriptorCount);
    }
    vcw_wr_u64(&wr, 0); /* pAllocator */
    vcw_wr_u64(&wr, 1); /* pDescriptorPool */
    vcw_wr_u64(&wr, pool_id);
    if (wr.err) {
        int err = wr.err;
        free(cmd);
        return err;
    }

    for (uint32_t i = 0; i < sizeof(reply); i++) reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    free(cmd);
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t present = *(uint64_t *)(reply + 8);
    uint64_t pool_reply = *(uint64_t *)(reply + 16);
    static uint32_t log_count;
    if (log_count < 32u) {
        log_count++;
        printf("[VDPOOL] reply cmd=%u vk=%u present=%llu pool=%llu guest=%llu sizes=%u bytes=%u\n",
               reply_cmd, vk_result, (unsigned long long)present,
               (unsigned long long)pool_reply,
               (unsigned long long)pool_id, ci->poolSizeCount, cap);
    }
    if (reply_cmd != VN_CMD_TYPE_vkCreateDescriptorPool) return -5;
    if (vk_result != VK_SUCCESS) return (int)vk_result;
    if (!present || !pool_reply) return -5;

    *out_pool_id = pool_id;
    return VK_SUCCESS;
}

int venus_cmd_encode_DestroyDescriptorPool(struct venus_wire *w, uint64_t dev_id,
                                           uint64_t pool_id) {
    if (!w || !pool_id) return -22;
    uint8_t cmd[32];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkDestroyDescriptorPool);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, pool_id);
    vcw_wr_u64(&wr, 0); /* pAllocator */
    if (wr.err) return wr.err;
    return venus_wire_submit_raw(w, cmd, wr.off);
}

int venus_cmd_encode_AllocateDescriptorSets(
        struct venus_wire *w, uint64_t dev_id, uint64_t pool_id,
        uint32_t count, const uint64_t *layout_ids, uint64_t *set_ids) {
    if (!w || !pool_id || !layout_ids || !set_ids || count == 0) return -22;

    uint32_t cap = 96u + count * 16u;
    uint32_t reply_cap = 16u + count * 8u;
    uint8_t *cmd = (uint8_t *)malloc(cap);
    uint8_t *reply = (uint8_t *)malloc(reply_cap);
    if (!cmd || !reply) {
        if (cmd) free(cmd);
        if (reply) free(reply);
        return -12;
    }

    for (uint32_t i = 0; i < count; i++) {
        set_ids[i] = venus_wire_alloc_object_id(w);
        if (!set_ids[i]) {
            free(cmd);
            free(reply);
            return -12;
        }
    }

    struct venus_cmd_writer wr = { cmd, 0, cap, 0 };
    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkAllocateDescriptorSets);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, 1); /* pAllocateInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    vcw_wr_u64(&wr, 0); /* pNext */
    vcw_wr_u64(&wr, pool_id);
    vcw_wr_u32(&wr, count);
    vcw_wr_array_size(&wr, count);
    for (uint32_t i = 0; i < count; i++)
        vcw_wr_u64(&wr, layout_ids[i]);
    vcw_wr_array_size(&wr, count); /* pDescriptorSets */
    for (uint32_t i = 0; i < count; i++)
        vcw_wr_u64(&wr, set_ids[i]);
    if (wr.err) {
        int err = wr.err;
        free(cmd);
        free(reply);
        return err;
    }

    for (uint32_t i = 0; i < reply_cap; i++) reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, reply_cap);
    if (rc < (int)reply_cap) {
        free(cmd);
        free(reply);
        return rc < 0 ? rc : -5;
    }

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t reply_count = *(uint64_t *)(reply + 8);
    static uint32_t log_count;
    if (log_count < 32u) {
        log_count++;
        printf("[VDSET] alloc reply cmd=%u vk=%u count=%llu req=%u first=%llu bytes=%u\n",
               reply_cmd, vk_result, (unsigned long long)reply_count,
               count, (unsigned long long)set_ids[0], wr.off);
    }

    free(cmd);
    free(reply);
    if (reply_cmd != VN_CMD_TYPE_vkAllocateDescriptorSets) return -5;
    if (vk_result != VK_SUCCESS) return (int)vk_result;
    if (reply_count != count) return -5;
    return VK_SUCCESS;
}

int venus_cmd_encode_FreeDescriptorSets(struct venus_wire *w, uint64_t dev_id,
                                        uint64_t pool_id, uint32_t count,
                                        const uint64_t *set_ids) {
    if (!w || !pool_id || (count && !set_ids)) return -22;
    uint32_t cap = 40u + count * 8u;
    uint8_t *cmd = (uint8_t *)malloc(cap);
    if (!cmd) return -12;
    struct venus_cmd_writer wr = { cmd, 0, cap, 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkFreeDescriptorSets);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, pool_id);
    vcw_wr_u32(&wr, count);
    vcw_wr_array_size(&wr, count);
    for (uint32_t i = 0; i < count; i++)
        vcw_wr_u64(&wr, set_ids[i]);
    if (wr.err) {
        int err = wr.err;
        free(cmd);
        return err;
    }
    int rc = venus_wire_submit_raw(w, cmd, wr.off);
    free(cmd);
    return rc;
}

static int write_has_valid_hosts(struct venus_device *dev,
                                 const VkWriteDescriptorSet *write) {
    if (!write || !host_desc_set(dev, write->dstSet) || write->descriptorCount == 0)
        return 0;

    if (desc_type_needs_image(write->descriptorType)) {
        if (!write->pImageInfo) return 0;
        for (uint32_t i = 0; i < write->descriptorCount; i++)
            if (!host_image_view(dev, write->pImageInfo[i].imageView))
                return 0;
    }
    if (desc_type_needs_sampler(write->descriptorType)) {
        if (!write->pImageInfo) return 0;
        for (uint32_t i = 0; i < write->descriptorCount; i++)
            if (write->pImageInfo[i].sampler &&
                !host_sampler(dev, write->pImageInfo[i].sampler))
                return 0;
    }
    if (desc_type_needs_buffer(write->descriptorType)) {
        if (!write->pBufferInfo) return 0;
        for (uint32_t i = 0; i < write->descriptorCount; i++)
            if (!host_buffer(dev, write->pBufferInfo[i].buffer))
                return 0;
    }
    if (desc_type_needs_texel_view(write->descriptorType)) {
        if (!write->pTexelBufferView) return 0;
        for (uint32_t i = 0; i < write->descriptorCount; i++)
            if (!host_buffer_view(dev, write->pTexelBufferView[i]))
                return 0;
    }
    return 1;
}

int venus_cmd_encode_UpdateDescriptorSets(
        struct venus_wire *w, struct venus_device *dev,
        uint32_t write_count, const VkWriteDescriptorSet *writes,
        uint32_t copy_count, const VkCopyDescriptorSet *copies) {
    if (!w || !dev) return -22;

    uint32_t valid_writes = 0;
    uint32_t valid_copies = 0;
    uint64_t cap = 32u;

    for (uint32_t i = 0; i < write_count; i++) {
        const VkWriteDescriptorSet *wr = &writes[i];
        if (!write_has_valid_hosts(dev, wr))
            continue;
        valid_writes++;
        cap += 64u;
        cap += wr->pImageInfo ? (8u + (uint64_t)wr->descriptorCount * 24u) : 8u;
        cap += wr->pBufferInfo ? (8u + (uint64_t)wr->descriptorCount * 24u) : 8u;
        cap += wr->pTexelBufferView ? (8u + (uint64_t)wr->descriptorCount * 8u) : 8u;
    }
    for (uint32_t i = 0; i < copy_count; i++) {
        if (copies && host_desc_set(dev, copies[i].srcSet) &&
            host_desc_set(dev, copies[i].dstSet)) {
            valid_copies++;
            cap += 64u;
        }
    }

    if (!valid_writes && !valid_copies)
        return VK_SUCCESS;
    if (cap > 1024u * 1024u)
        return -12;

    uint8_t *cmd = (uint8_t *)malloc((unsigned long)cap);
    if (!cmd) return -12;
    struct venus_cmd_writer wr = { cmd, 0, (uint32_t)cap, 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkUpdateDescriptorSets);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, dev->host_handle);
    vcw_wr_u32(&wr, valid_writes);
    vcw_wr_array_size(&wr, valid_writes);
    for (uint32_t i = 0; i < write_count; i++) {
        const VkWriteDescriptorSet *src = &writes[i];
        if (!write_has_valid_hosts(dev, src))
            continue;

        vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        vcw_wr_u64(&wr, 0); /* pNext */
        vcw_wr_u64(&wr, host_desc_set(dev, src->dstSet));
        vcw_wr_u32(&wr, src->dstBinding);
        vcw_wr_u32(&wr, src->dstArrayElement);
        vcw_wr_u32(&wr, src->descriptorCount);
        vcw_wr_i32(&wr, (int32_t)src->descriptorType);

        if (src->pImageInfo) {
            vcw_wr_array_size(&wr, src->descriptorCount);
            for (uint32_t j = 0; j < src->descriptorCount; j++) {
                vcw_wr_u64(&wr, host_sampler(dev, src->pImageInfo[j].sampler));
                vcw_wr_u64(&wr, host_image_view(dev, src->pImageInfo[j].imageView));
                vcw_wr_i32(&wr, (int32_t)src->pImageInfo[j].imageLayout);
            }
        } else {
            vcw_wr_array_size(&wr, 0);
        }

        if (src->pBufferInfo) {
            vcw_wr_array_size(&wr, src->descriptorCount);
            for (uint32_t j = 0; j < src->descriptorCount; j++) {
                vcw_wr_u64(&wr, host_buffer(dev, src->pBufferInfo[j].buffer));
                vcw_wr_u64(&wr, src->pBufferInfo[j].offset);
                vcw_wr_u64(&wr, src->pBufferInfo[j].range);
            }
        } else {
            vcw_wr_array_size(&wr, 0);
        }

        if (src->pTexelBufferView) {
            vcw_wr_array_size(&wr, src->descriptorCount);
            for (uint32_t j = 0; j < src->descriptorCount; j++)
                vcw_wr_u64(&wr, host_buffer_view(dev, src->pTexelBufferView[j]));
        } else {
            vcw_wr_array_size(&wr, 0);
        }
    }

    vcw_wr_u32(&wr, valid_copies);
    vcw_wr_array_size(&wr, valid_copies);
    for (uint32_t i = 0; i < copy_count; i++) {
        if (!copies || !host_desc_set(dev, copies[i].srcSet) ||
            !host_desc_set(dev, copies[i].dstSet))
            continue;
        vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET);
        vcw_wr_u64(&wr, 0); /* pNext */
        vcw_wr_u64(&wr, host_desc_set(dev, copies[i].srcSet));
        vcw_wr_u32(&wr, copies[i].srcBinding);
        vcw_wr_u32(&wr, copies[i].srcArrayElement);
        vcw_wr_u64(&wr, host_desc_set(dev, copies[i].dstSet));
        vcw_wr_u32(&wr, copies[i].dstBinding);
        vcw_wr_u32(&wr, copies[i].dstArrayElement);
        vcw_wr_u32(&wr, copies[i].descriptorCount);
    }

    if (wr.err) {
        int err = wr.err;
        free(cmd);
        return err;
    }
    static uint32_t log_count;
    if (log_count < 64u) {
        log_count++;
        printf("[VDESC-H] update writes=%u/%u copies=%u/%u bytes=%u\n",
               valid_writes, write_count, valid_copies, copy_count, wr.off);
    }
    int rc = venus_wire_submit_raw(w, cmd, wr.off);
    free(cmd);
    return rc;
}

int venus_cmd_encode_CmdBindDescriptorSets(
        struct venus_wire *w, struct venus_device *dev, uint64_t cb_id,
        uint32_t bind_point, VkPipelineLayout layout, uint32_t first_set,
        uint32_t set_count, const VkDescriptorSet *sets,
        uint32_t dynamic_count, const uint32_t *dynamic_offsets) {
    if (!w || !dev || !cb_id || !sets) return -22;

    uint64_t layout_id = host_pl_layout(dev, layout);
    if (!layout_id) return -22;

    uint32_t valid = 0;
    for (uint32_t i = 0; i < set_count; i++) {
        if (!host_desc_set(dev, sets[i]))
            return -22;
        valid++;
    }
    if (valid != set_count)
        return -22;

    uint32_t cap = 64u + set_count * 8u + dynamic_count * 4u;
    uint8_t *cmd = (uint8_t *)malloc(cap);
    if (!cmd) return -12;
    struct venus_cmd_writer wr = { cmd, 0, cap, 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCmdBindDescriptorSets);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, cb_id);
    vcw_wr_i32(&wr, (int32_t)bind_point);
    vcw_wr_u64(&wr, layout_id);
    vcw_wr_u32(&wr, first_set);
    vcw_wr_u32(&wr, set_count);
    vcw_wr_array_size(&wr, set_count);
    for (uint32_t i = 0; i < set_count; i++)
        vcw_wr_u64(&wr, host_desc_set(dev, sets[i]));
    vcw_wr_u32(&wr, dynamic_count);
    if (dynamic_offsets && dynamic_count) {
        vcw_wr_array_size(&wr, dynamic_count);
        for (uint32_t i = 0; i < dynamic_count; i++)
            vcw_wr_u32(&wr, dynamic_offsets[i]);
    } else {
        vcw_wr_array_size(&wr, 0);
    }
    if (wr.err) {
        int err = wr.err;
        free(cmd);
        return err;
    }
    static uint32_t log_count;
    if (log_count < 64u) {
        log_count++;
        printf("[VDBIND-H] cb=%llu layout=%llu first=%u sets=%u dyn=%u\n",
               (unsigned long long)cb_id, (unsigned long long)layout_id,
               first_set, set_count, dynamic_count);
    }
    int rc = venus_wire_submit_raw(w, cmd, wr.off);
    free(cmd);
    return rc;
}
