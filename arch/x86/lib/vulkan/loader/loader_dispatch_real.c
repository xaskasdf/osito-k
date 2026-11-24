#include "loader.h"
#include <vulkan/vulkan_ositok.h>

extern int strcmp(const char *, const char *);
extern int printf(const char *, ...);
extern void okgl_diag3(const char *message, uint64_t a, uint64_t b,
                       uint64_t c);
extern void okgl_trace(const char *message);

#ifndef OSITO_VK_RENDER_DIAGNOSTICS
#define OSITO_VK_RENDER_DIAGNOSTICS 0
#endif

static volatile uint32_t queue_submit1_trace_count;
static volatile uint32_t queue_submit2_trace_count;

static int
loader_trace_take(volatile uint32_t *counter, uint32_t limit)
{
    if (!OSITO_VK_RENDER_DIAGNOSTICS)
        return 0;
    return __sync_fetch_and_add(counter, 1) < limit;
}

static volatile uint32_t descriptor_update_trace_count;
static volatile uint32_t descriptor_draw_trace_count;
static volatile uint32_t image_upload_trace_count;
static volatile uint32_t vertex_draw_trace_count;
static volatile uint32_t image_bind_trace_count;
static volatile uint32_t submit_draw_trace_count;
static volatile uint32_t submit_copy_trace_count;
static volatile uint32_t descriptor_idle_check_trace_count;
static volatile uint32_t descriptor_inflight_mutation_trace_count;
static volatile uint32_t dashboard_readback_state;
static volatile uint32_t image_trace_lock;
static struct osito_image *image_trace_head;

static uint32_t
loader_descriptor_generation_load(const struct osito_descriptor_set *set)
{
    return __atomic_load_n(&set->image_generation, __ATOMIC_ACQUIRE);
}

static uint32_t
loader_descriptor_generation_next(struct osito_descriptor_set *set)
{
    return __atomic_add_fetch(&set->image_generation, 1u,
                              __ATOMIC_RELEASE);
}

static void
loader_trace_image_lock(void)
{
    while (__sync_lock_test_and_set(&image_trace_lock, 1))
        __asm__ volatile("pause");
}

static void
loader_trace_image_unlock(void)
{
    __sync_lock_release(&image_trace_lock);
}

static void
loader_trace_image_add(struct osito_image *image)
{
    if (!OSITO_VK_RENDER_DIAGNOSTICS)
        return;
    loader_trace_image_lock();
    image->trace_next = image_trace_head;
    image_trace_head = image;
    loader_trace_image_unlock();
}

static void
loader_trace_image_remove(struct osito_image *image)
{
    if (!OSITO_VK_RENDER_DIAGNOSTICS)
        return;
    loader_trace_image_lock();
    struct osito_image **link = &image_trace_head;
    while (*link && *link != image)
        link = &(*link)->trace_next;
    if (*link)
        *link = image->trace_next;
    loader_trace_image_unlock();
}

static void
loader_trace_image_bind(struct osito_image *image,
                        struct osito_memory *memory,
                        VkDeviceSize offset)
{
    image->memory = memory;
    image->memory_offset = offset;
    if (!OSITO_VK_RENDER_DIAGNOSTICS)
        return;

    loader_trace_image_lock();

    if (loader_trace_take(&image_bind_trace_count, 512)) {
        okgl_diag3("[VK-IMG-BIND]", (uint64_t)image,
                   (uint64_t)memory, offset);
        okgl_diag3("[VK-IMG-SPAN]", (uint64_t)image,
                   image->memory_size,
                   ((uint64_t)image->width << 32) | image->height);
        okgl_diag3("[VK-IMG-REAL]", (uint64_t)image->real,
                   (uint64_t)memory->real, (uint64_t)image->format);
    }

    if (image->memory_size && offset <= memory->allocation_size &&
        image->memory_size <= memory->allocation_size - offset) {
        VkDeviceSize end = offset + image->memory_size;
        for (struct osito_image *other = image_trace_head;
             other; other = other->trace_next) {
            if (other == image || other->memory != memory ||
                !other->memory_size ||
                other->memory_offset > memory->allocation_size ||
                other->memory_size >
                    memory->allocation_size - other->memory_offset)
                continue;
            VkDeviceSize other_end = other->memory_offset +
                                      other->memory_size;
            if (offset < other_end && other->memory_offset < end) {
                okgl_diag3("[VK-IMG-ALIAS]", (uint64_t)image,
                           (uint64_t)other, (uint64_t)memory);
                okgl_diag3("[VK-IMG-RANGE-A]", offset,
                           image->memory_size, other->memory_offset);
                okgl_diag3("[VK-IMG-RANGE-B]", other->memory_size,
                           ((uint64_t)image->width << 32) | image->height,
                           ((uint64_t)other->width << 32) | other->height);
            }
        }
    }
    loader_trace_image_unlock();
}

static const uint8_t *
loader_trace_buffer_data(struct osito_buffer *buffer, VkDeviceSize offset,
                         VkDeviceSize size)
{
    if (!buffer || offset > buffer->size || size > buffer->size - offset)
        return 0;
    if (buffer->memory && buffer->memory->mapped_data) {
        VkDeviceSize source_offset = buffer->memory_offset + offset;
        VkDeviceSize mapped_end = buffer->memory->mapped_offset +
                                   buffer->memory->mapped_size;
        if (source_offset >= buffer->memory->mapped_offset &&
            source_offset <= mapped_end && size <= mapped_end - source_offset)
            return (const uint8_t *)buffer->memory->mapped_data +
                (size_t)(source_offset - buffer->memory->mapped_offset);
    }
    if (buffer->trace_shadow && offset <= buffer->trace_shadow_size &&
        size <= buffer->trace_shadow_size - offset)
        return buffer->trace_shadow + (size_t)offset;
    return 0;
}

static uint64_t
loader_trace_hash_bytes(uint64_t hash, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t
loader_trace_draw_hash(const struct osito_trace_draw *sample)
{
    uint32_t count = sample->element_count;
    if (!count || count > 512)
        return 0;

    uint64_t hash = 1469598103934665603ull;
    const uint8_t *indices = 0;
    VkDeviceSize index_size = 0;
    if (sample->indexed) {
        if (!sample->index_buffer)
            return 0;
        if (sample->index_type == VK_INDEX_TYPE_UINT32)
            index_size = 4;
        else if (sample->index_type == VK_INDEX_TYPE_UINT8_EXT)
            index_size = 1;
        else
            index_size = 2;
        VkDeviceSize index_offset = sample->index_offset +
            (VkDeviceSize)sample->first_element * index_size;
        indices = loader_trace_buffer_data(sample->index_buffer,
                                           index_offset,
                                           (VkDeviceSize)count * index_size);
        if (!indices)
            return 0;
        hash = loader_trace_hash_bytes(hash, indices,
                                       (size_t)count * index_size);
    }

    for (uint32_t i = 0; i < count; i++) {
        int64_t vertex;
        if (sample->indexed) {
            uint32_t index;
            if (index_size == 4)
                memcpy(&index, indices + (size_t)i * 4u, 4u);
            else if (index_size == 2) {
                uint16_t value;
                memcpy(&value, indices + (size_t)i * 2u, 2u);
                index = value;
            } else {
                index = indices[i];
            }
            vertex = (int64_t)sample->vertex_offset + index;
        } else {
            vertex = (int64_t)sample->first_element + i;
        }
        if (vertex < 0 || !sample->vertex_buffers[0])
            return 0;

        VkDeviceSize xy_stride = sample->vertex_strides[0]
            ? sample->vertex_strides[0] : 16u;
        VkDeviceSize xy_offset = sample->vertex_offsets[0] +
            (VkDeviceSize)vertex * xy_stride;
        size_t xy_size = sample->vertex_buffers[1] ? 8u : 16u;
        const uint8_t *xy = loader_trace_buffer_data(
            sample->vertex_buffers[0], xy_offset, xy_size);
        if (!xy)
            return 0;
        hash = loader_trace_hash_bytes(hash, xy, xy_size);

        if (sample->vertex_buffers[1]) {
            VkDeviceSize uv_stride = sample->vertex_strides[1]
                ? sample->vertex_strides[1] : xy_stride;
            VkDeviceSize uv_offset = sample->vertex_offsets[1] +
                (VkDeviceSize)vertex * uv_stride;
            const uint8_t *uv = loader_trace_buffer_data(
                sample->vertex_buffers[1], uv_offset, 8u);
            if (!uv)
                return 0;
            hash = loader_trace_hash_bytes(hash, uv, 8u);
        }
    }
    return hash;
}

static void
loader_trace_store_draw(struct osito_cmd_buffer *command_buffer,
                        struct osito_descriptor_set *set,
                        uint32_t sequence, uint32_t element_count,
                        int indexed, uint32_t first_element,
                        int32_t vertex_offset)
{
    if (!set->trace_image || !set->trace_image->trace_dashboard)
        return;

    uint32_t slot = command_buffer->trace_dashboard_draw_count++ & 3u;
    struct osito_trace_draw *sample =
        &command_buffer->trace_draw_samples[slot];
    memset(sample, 0, sizeof(*sample));
    sample->vertex_buffers[0] = command_buffer->trace_vertex_buffers[0];
    sample->vertex_buffers[1] = command_buffer->trace_vertex_buffers[1];
    sample->vertex_offsets[0] = command_buffer->trace_vertex_offsets[0];
    sample->vertex_offsets[1] = command_buffer->trace_vertex_offsets[1];
    sample->vertex_strides[0] = command_buffer->trace_vertex_strides[0];
    sample->vertex_strides[1] = command_buffer->trace_vertex_strides[1];
    sample->index_buffer = command_buffer->trace_index_buffer;
    sample->descriptor_set = set;
    sample->index_offset = command_buffer->trace_index_offset;
    sample->index_type = command_buffer->trace_index_type;
    sample->image = __atomic_load_n(&set->image, __ATOMIC_ACQUIRE);
    sample->sequence = sequence;
    sample->element_count = element_count;
    sample->first_element = first_element;
    sample->descriptor_generation =
        loader_descriptor_generation_load(set);
    sample->vertex_offset = vertex_offset;
    sample->indexed = !!indexed;
    sample->valid = 1;
    sample->record_hash = loader_trace_draw_hash(sample);
}

static void
loader_trace_submit_draws(struct osito_cmd_buffer *command_buffer)
{
    for (uint32_t slot = 0; slot < 4; slot++) {
        struct osito_trace_draw *sample =
            &command_buffer->trace_draw_samples[slot];
        if (!sample->valid)
            continue;
        struct osito_descriptor_set *set = sample->descriptor_set;
        if (set) {
            sample->submit_descriptor_generation =
                loader_descriptor_generation_load(set);
            sample->submit_image =
                __atomic_load_n(&set->image, __ATOMIC_ACQUIRE);
        }
        if (!loader_trace_take(&submit_draw_trace_count, 256))
            continue;
        uint64_t submit_hash = loader_trace_draw_hash(sample);
        okgl_diag3("[VK-SUBMIT-DRAW]", (uint64_t)command_buffer->real,
                   ((uint64_t)slot << 32) | sample->sequence,
                   (uint64_t)sample->image);
        okgl_diag3("[VK-SUBMIT-HASH]", sample->record_hash, submit_hash,
                   ((uint64_t)sample->element_count << 32) |
                       sample->first_element);
        if (set) {
            uint32_t generation = sample->submit_descriptor_generation;
            okgl_diag3("[VK-SUBMIT-DGEN]", (uint64_t)set->real,
                       sample->descriptor_generation,
                       generation);
            if (sample->descriptor_generation != generation ||
                sample->image != sample->submit_image)
                okgl_diag3("[VK-SUBMIT-DESC-MUTATE]",
                           (uint64_t)set->real, (uint64_t)sample->image,
                           (uint64_t)sample->submit_image);
        }
        if (!sample->record_hash || !submit_hash)
            okgl_diag3("[VK-SUBMIT-NODATA]", slot,
                       sample->record_hash, submit_hash);
        else if (sample->record_hash != submit_hash)
            okgl_diag3("[VK-SUBMIT-MUTATE]", slot,
                       sample->record_hash, submit_hash);
    }
}

static uint32_t
loader_trace_post_idle_descriptors(struct osito_cmd_buffer *command_buffer)
{
    uint32_t mutations = 0;
    for (uint32_t slot = 0; slot < 4; slot++) {
        struct osito_trace_draw *sample =
            &command_buffer->trace_draw_samples[slot];
        struct osito_descriptor_set *set = sample->descriptor_set;
        if (!sample->valid || !set)
            continue;
        uint64_t post_idle_hash = loader_trace_draw_hash(sample);
        if (loader_trace_take(&descriptor_inflight_mutation_trace_count,
                              128)) {
            okgl_diag3("[VK-POST-HASH]", sample->record_hash,
                       post_idle_hash,
                       ((uint64_t)sample->sequence << 32) |
                           sample->first_element);
            if (!post_idle_hash ||
                post_idle_hash != sample->record_hash)
                okgl_diag3("[VK-POST-MUTATE]", sample->sequence,
                           sample->record_hash, post_idle_hash);
        }
        uint32_t generation = loader_descriptor_generation_load(set);
        VkImage image = __atomic_load_n(&set->image, __ATOMIC_ACQUIRE);
        if (generation == sample->submit_descriptor_generation &&
            image == sample->submit_image)
            continue;
        mutations++;
        if (loader_trace_take(&descriptor_inflight_mutation_trace_count,
                              128)) {
            okgl_diag3("[VK-DESC-INFLIGHT-MUTATE]", (uint64_t)set->real,
                       sample->submit_descriptor_generation, generation);
            okgl_diag3("[VK-DESC-INFLIGHT-IMAGE]", (uint64_t)set->real,
                       (uint64_t)sample->submit_image, (uint64_t)image);
        }
    }
    return mutations;
}

static uint64_t
loader_trace_load_u64(const void *data)
{
    uint64_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

static void
loader_trace_vertex_draw(struct osito_cmd_buffer *command_buffer,
                         uint32_t sequence, int indexed,
                         uint32_t first_element, int32_t vertex_offset)
{
    if (!loader_trace_take(&vertex_draw_trace_count, 120))
        return;

    struct osito_buffer *xy_buffer = command_buffer->trace_vertex_buffers[0];
    struct osito_buffer *uv_buffer = command_buffer->trace_vertex_buffers[1];
    VkDeviceSize xy_stride = command_buffer->trace_vertex_strides[0]
        ? command_buffer->trace_vertex_strides[0] : 16u;
    VkDeviceSize uv_stride = command_buffer->trace_vertex_strides[1]
        ? command_buffer->trace_vertex_strides[1] : xy_stride;
    int64_t base_vertex = indexed ? vertex_offset : (int64_t)first_element;

    okgl_diag3("[VK-VTX-DRAW]", (uint64_t)command_buffer->real,
               ((uint64_t)sequence << 32) | first_element,
               ((uint64_t)(uint32_t)vertex_offset << 32) |
                   (uint32_t)xy_stride);

    if (indexed && command_buffer->trace_index_buffer) {
        VkDeviceSize index_size = command_buffer->trace_index_type ==
            VK_INDEX_TYPE_UINT32 ? 4u : 2u;
        const uint8_t *indices = loader_trace_buffer_data(
            command_buffer->trace_index_buffer,
            command_buffer->trace_index_offset +
                (VkDeviceSize)first_element * index_size,
            index_size * 6u);
        if (indices) {
            uint32_t decoded[6];
            for (uint32_t i = 0; i < 6; i++) {
                if (index_size == 4u)
                    memcpy(&decoded[i], indices + i * 4u, 4u);
                else {
                    uint16_t value;
                    memcpy(&value, indices + i * 2u, 2u);
                    decoded[i] = value;
                }
            }
            okgl_diag3("[VK-IDX-6]", sequence,
                       ((uint64_t)decoded[0] << 48) |
                           ((uint64_t)decoded[1] << 32) |
                           ((uint64_t)decoded[2] << 16) | decoded[3],
                       ((uint64_t)decoded[4] << 32) | decoded[5]);
        }
    }

    if (!xy_buffer || base_vertex < 0) {
        okgl_diag3("[VK-VTX-MISS]", sequence, (uint64_t)xy_buffer,
                   (uint64_t)base_vertex);
        return;
    }

    static const char *const labels[] = {
        "[VK-VTX-0]", "[VK-VTX-1]", "[VK-VTX-2]", "[VK-VTX-3]"
    };
    for (uint32_t i = 0; i < 4; i++) {
        VkDeviceSize xy_offset = command_buffer->trace_vertex_offsets[0] +
            (VkDeviceSize)(base_vertex + i) * xy_stride;
        const uint8_t *xy = loader_trace_buffer_data(xy_buffer, xy_offset,
                                                     uv_buffer ? 8u : 16u);
        const uint8_t *uv;
        if (uv_buffer) {
            VkDeviceSize uv_offset = command_buffer->trace_vertex_offsets[1] +
                (VkDeviceSize)(base_vertex + i) * uv_stride;
            uv = loader_trace_buffer_data(uv_buffer, uv_offset, 8u);
        } else {
            uv = xy ? xy + 8u : 0;
        }
        if (!xy || !uv) {
            okgl_diag3("[VK-VTX-MISS]", sequence, (uint64_t)xy_buffer,
                       xy_offset);
            return;
        }
        okgl_diag3(labels[i], sequence, loader_trace_load_u64(xy),
                   loader_trace_load_u64(uv));
    }
}

static uint64_t
loader_trace_hash_rows(const uint8_t *data, uint32_t width, uint32_t height,
                       size_t row_stride)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = data + (size_t)y * row_stride;
        for (size_t i = 0; i < (size_t)width * 4u; i++) {
            hash ^= row[i];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

static void
loader_trace_dashboard_readback(struct osito_queue *queue,
                                struct osito_image *image,
                                VkImageLayout original_layout)
{
    if (!queue || !image || !image->trace_dashboard ||
        image->owner != queue->owner)
        return;
    if (__sync_val_compare_and_swap(&dashboard_readback_state, 0, 1) != 0)
        return;

    struct osito_device *device = queue->owner;
    const struct osito_icd_entry *icd = device->owner->icd;
    VkInstance instance = device->owner->handle;
    PFN_vkCreateBuffer create_buffer =
        (PFN_vkCreateBuffer)icd->get_proc_addr(instance, "vkCreateBuffer");
    PFN_vkDestroyBuffer destroy_buffer =
        (PFN_vkDestroyBuffer)icd->get_proc_addr(instance, "vkDestroyBuffer");
    PFN_vkGetBufferMemoryRequirements get_requirements =
        (PFN_vkGetBufferMemoryRequirements)icd->get_proc_addr(
            instance, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory allocate_memory =
        (PFN_vkAllocateMemory)icd->get_proc_addr(instance,
                                                 "vkAllocateMemory");
    PFN_vkFreeMemory free_memory =
        (PFN_vkFreeMemory)icd->get_proc_addr(instance, "vkFreeMemory");
    PFN_vkBindBufferMemory bind_memory =
        (PFN_vkBindBufferMemory)icd->get_proc_addr(instance,
                                                  "vkBindBufferMemory");
    PFN_vkMapMemory map_memory =
        (PFN_vkMapMemory)icd->get_proc_addr(instance, "vkMapMemory");
    PFN_vkUnmapMemory unmap_memory =
        (PFN_vkUnmapMemory)icd->get_proc_addr(instance, "vkUnmapMemory");
    PFN_vkInvalidateMappedMemoryRanges invalidate_memory =
        (PFN_vkInvalidateMappedMemoryRanges)icd->get_proc_addr(
            instance, "vkInvalidateMappedMemoryRanges");
    PFN_vkCreateCommandPool create_pool =
        (PFN_vkCreateCommandPool)icd->get_proc_addr(
            instance, "vkCreateCommandPool");
    PFN_vkDestroyCommandPool destroy_pool =
        (PFN_vkDestroyCommandPool)icd->get_proc_addr(
            instance, "vkDestroyCommandPool");
    PFN_vkAllocateCommandBuffers allocate_commands =
        (PFN_vkAllocateCommandBuffers)icd->get_proc_addr(
            instance, "vkAllocateCommandBuffers");
    PFN_vkFreeCommandBuffers free_commands =
        (PFN_vkFreeCommandBuffers)icd->get_proc_addr(
            instance, "vkFreeCommandBuffers");
    PFN_vkBeginCommandBuffer begin_command =
        (PFN_vkBeginCommandBuffer)icd->get_proc_addr(
            instance, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end_command =
        (PFN_vkEndCommandBuffer)icd->get_proc_addr(
            instance, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier2 barrier =
        (PFN_vkCmdPipelineBarrier2)icd->get_proc_addr(
            instance, "vkCmdPipelineBarrier2");
    if (!barrier)
        barrier = (PFN_vkCmdPipelineBarrier2)icd->get_proc_addr(
            instance, "vkCmdPipelineBarrier2KHR");
    PFN_vkCmdCopyImageToBuffer2 copy_image =
        (PFN_vkCmdCopyImageToBuffer2)icd->get_proc_addr(
            instance, "vkCmdCopyImageToBuffer2");
    if (!copy_image)
        copy_image = (PFN_vkCmdCopyImageToBuffer2)icd->get_proc_addr(
            instance, "vkCmdCopyImageToBuffer2KHR");
    PFN_vkQueueSubmit2 submit =
        (PFN_vkQueueSubmit2)icd->get_proc_addr(instance, "vkQueueSubmit2");
    if (!submit)
        submit = (PFN_vkQueueSubmit2)icd->get_proc_addr(
            instance, "vkQueueSubmit2KHR");
    PFN_vkQueueWaitIdle wait_idle =
        (PFN_vkQueueWaitIdle)icd->get_proc_addr(instance,
                                                "vkQueueWaitIdle");

    uint64_t missing = 0;
    missing |= !create_buffer ? 1ull << 0 : 0;
    missing |= !destroy_buffer ? 1ull << 1 : 0;
    missing |= !get_requirements ? 1ull << 2 : 0;
    missing |= !allocate_memory ? 1ull << 3 : 0;
    missing |= !free_memory ? 1ull << 4 : 0;
    missing |= !bind_memory ? 1ull << 5 : 0;
    missing |= !map_memory ? 1ull << 6 : 0;
    missing |= !unmap_memory ? 1ull << 7 : 0;
    missing |= !create_pool ? 1ull << 9 : 0;
    missing |= !destroy_pool ? 1ull << 10 : 0;
    missing |= !allocate_commands ? 1ull << 11 : 0;
    missing |= !free_commands ? 1ull << 12 : 0;
    missing |= !begin_command ? 1ull << 13 : 0;
    missing |= !end_command ? 1ull << 14 : 0;
    missing |= !barrier ? 1ull << 15 : 0;
    missing |= !copy_image ? 1ull << 16 : 0;
    missing |= !submit ? 1ull << 17 : 0;
    missing |= !wait_idle ? 1ull << 18 : 0;
    if (missing) {
        okgl_diag3("[VK-IMAGE-RB-ERR]", 1,
                   (uint64_t)(uint32_t)VK_ERROR_EXTENSION_NOT_PRESENT,
                   missing);
        return;
    }

    if (!(image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        original_layout == VK_IMAGE_LAYOUT_UNDEFINED ||
        image->trace_staging_memory_type >= 32u) {
        okgl_diag3("[VK-IMAGE-RB-ERR]", 2,
                   ((uint64_t)(uint32_t)image->usage << 32) |
                       (uint32_t)original_layout,
                   image->trace_staging_memory_type);
        return;
    }

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    void *mapped = 0;
    VkResult result = VK_SUCCESS;
    VkMemoryRequirements requirements;
    uint32_t stage = 0;
    int submitted = 0;
    int completed = 0;
    memset(&requirements, 0, sizeof(requirements));

    VkBufferCreateInfo buffer_info;
    memset(&buffer_info, 0, sizeof(buffer_info));
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = 128u * 32u * 4u;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    stage = 3;
    result = create_buffer(device->real, &buffer_info, 0, &buffer);
    if (result != VK_SUCCESS)
        goto fail;

    get_requirements(device->real, buffer, &requirements);
    uint32_t memory_type = image->trace_staging_memory_type;
    if (!(requirements.memoryTypeBits & (1u << memory_type))) {
        stage = 4;
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto fail;
    }

    VkMemoryAllocateInfo memory_info;
    memset(&memory_info, 0, sizeof(memory_info));
    memory_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory_info.allocationSize = requirements.size;
    memory_info.memoryTypeIndex = memory_type;
    stage = 5;
    result = allocate_memory(device->real, &memory_info, 0, &memory);
    if (result != VK_SUCCESS)
        goto fail;

    stage = 6;
    result = bind_memory(device->real, buffer, memory, 0);
    if (result != VK_SUCCESS)
        goto fail;

    stage = 7;
    result = map_memory(device->real, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS)
        goto fail;

    VkCommandPoolCreateInfo pool_info;
    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue->family_index;
    stage = 8;
    result = create_pool(device->real, &pool_info, 0, &pool);
    if (result != VK_SUCCESS)
        goto fail;

    VkCommandBufferAllocateInfo command_info;
    memset(&command_info, 0, sizeof(command_info));
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    stage = 9;
    result = allocate_commands(device->real, &command_info, &command);
    if (result != VK_SUCCESS)
        goto fail;

    VkCommandBufferBeginInfo begin_info;
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    stage = 10;
    result = begin_command(command, &begin_info);
    if (result != VK_SUCCESS)
        goto fail;

    VkImageMemoryBarrier2 image_barrier;
    memset(&image_barrier, 0, sizeof(image_barrier));
    image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    image_barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    image_barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT |
                                  VK_ACCESS_2_MEMORY_WRITE_BIT;
    image_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    image_barrier.oldLayout = original_layout;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = image->real;
    image_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_barrier.subresourceRange.levelCount = 1;
    image_barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency;
    memset(&dependency, 0, sizeof(dependency));
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &image_barrier;
    barrier(command, &dependency);

    VkBufferImageCopy2 region;
    memset(&region, 0, sizeof(region));
    region.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = 128;
    region.imageExtent.height = 32;
    region.imageExtent.depth = 1;

    VkCopyImageToBufferInfo2 copy_info;
    memset(&copy_info, 0, sizeof(copy_info));
    copy_info.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
    copy_info.srcImage = image->real;
    copy_info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copy_info.dstBuffer = buffer;
    copy_info.regionCount = 1;
    copy_info.pRegions = &region;
    copy_image(command, &copy_info);

    image_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    image_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    image_barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image_barrier.newLayout = original_layout;
    barrier(command, &dependency);

    stage = 11;
    result = end_command(command);
    if (result != VK_SUCCESS)
        goto fail;

    VkCommandBufferSubmitInfo command_submit;
    memset(&command_submit, 0, sizeof(command_submit));
    command_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_submit.commandBuffer = command;
    command_submit.deviceMask = 1;

    VkSubmitInfo2 submit_info;
    memset(&submit_info, 0, sizeof(submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit_info.commandBufferInfoCount = 1;
    submit_info.pCommandBufferInfos = &command_submit;
    stage = 12;
    result = submit(queue->real, 1, &submit_info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS)
        goto fail;
    submitted = 1;

    stage = 13;
    result = wait_idle(queue->real);
    if (result != VK_SUCCESS)
        goto fail;
    completed = 1;

    if (invalidate_memory) {
        VkMappedMemoryRange mapped_range;
        memset(&mapped_range, 0, sizeof(mapped_range));
        mapped_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mapped_range.memory = memory;
        mapped_range.offset = 0;
        mapped_range.size = VK_WHOLE_SIZE;
        stage = 14;
        result = invalidate_memory(device->real, 1, &mapped_range);
        if (result != VK_SUCCESS)
            goto fail;
    } else {
        __sync_synchronize();
        okgl_diag3("[VK-IMAGE-RB-COHERENT]", memory_type,
                   requirements.size, (uint64_t)image->real);
    }

    const uint8_t *pixels = (const uint8_t *)mapped;
    const size_t row_stride = 128u * 4u;
    uint64_t q0 = loader_trace_hash_rows(pixels, 32, 32, row_stride);
    uint64_t q1 = loader_trace_hash_rows(pixels + 32u * 4u,
                                         32, 32, row_stride);
    uint64_t q2 = loader_trace_hash_rows(pixels + 64u * 4u,
                                         32, 32, row_stride);
    uint64_t q3 = loader_trace_hash_rows(pixels + 96u * 4u,
                                         32, 32, row_stride);
    okgl_diag3("[VK-IMAGE-RB-Q01]", (uint64_t)image->real, q0, q1);
    okgl_diag3("[VK-IMAGE-RB-Q23]", (uint64_t)image->real, q2, q3);
    okgl_diag3("[VK-IMAGE-RB-MATCH]",
               q0 == 0xcf8fcb37d7881d19ull &&
                   q1 == 0xc5107c8d018e36e3ull,
               q2 == 0xbcd521dd441052c6ull &&
                   q3 == 0x5f6121b202fd2393ull,
               ((uint64_t)(uint32_t)image->format << 32) |
                   (uint32_t)original_layout);
    goto cleanup;

fail:
    okgl_diag3("[VK-IMAGE-RB-ERR]", stage,
               (uint64_t)(uint32_t)result,
               ((uint64_t)requirements.memoryTypeBits << 32) |
                   image->trace_staging_memory_type);
    if (submitted && !completed)
        return;

cleanup:
    if (mapped)
        unmap_memory(device->real, memory);
    if (command && pool)
        free_commands(device->real, pool, 1, &command);
    if (pool)
        destroy_pool(device->real, pool, 0);
    if (buffer)
        destroy_buffer(device->real, buffer, 0);
    if (memory)
        free_memory(device->real, memory, 0);
}

static int
loader_trace_dashboard_hashes(struct osito_buffer *buffer,
                              VkDeviceSize buffer_offset,
                              uint32_t row_length,
                              uint64_t hashes[4])
{
    size_t row_stride = (size_t)row_length * 4u;
    VkDeviceSize required = (VkDeviceSize)row_stride * 32u;
    const uint8_t *source = loader_trace_buffer_data(
        buffer, buffer_offset, required);
    if (!source)
        return 0;

    for (uint32_t i = 0; i < 4; i++)
        hashes[i] = loader_trace_hash_rows(source + (size_t)i * 32u * 4u,
                                           32, 32, row_stride);
    return 1;
}

static void
loader_trace_record_image_copy(struct osito_cmd_buffer *command_buffer,
                               struct osito_buffer *buffer,
                               struct osito_image *image,
                               const VkCopyBufferToImageInfo2 *copy_info)
{
    if (image->width != 128 || image->height != 32 ||
        !copy_info->regionCount || !copy_info->pRegions)
        return;

    const VkBufferImageCopy2 *region = &copy_info->pRegions[0];
    if (region->imageOffset.x || region->imageOffset.y ||
        region->imageExtent.width != 128 ||
        region->imageExtent.height != 32)
        return;

    struct osito_trace_image_copy *sample = 0;
    for (uint32_t i = 0; i < 2; i++) {
        if (!command_buffer->trace_image_copies[i].valid) {
            sample = &command_buffer->trace_image_copies[i];
            break;
        }
    }
    if (!sample)
        return;

    sample->buffer = buffer;
    sample->image = image;
    sample->buffer_offset = region->bufferOffset;
    sample->row_length = region->bufferRowLength
        ? region->bufferRowLength : region->imageExtent.width;
    sample->valid = loader_trace_dashboard_hashes(
        buffer, sample->buffer_offset, sample->row_length,
        sample->record_hashes);
}

static int
loader_trace_submit_image_copies(struct osito_cmd_buffer *command_buffer)
{
    int found = 0;
    for (uint32_t i = 0; i < 2; i++) {
        struct osito_trace_image_copy *sample =
            &command_buffer->trace_image_copies[i];
        if (!sample->valid)
            continue;
        found = 1;

        uint64_t submit_hashes[4] = { 0, 0, 0, 0 };
        int readable = loader_trace_dashboard_hashes(
            sample->buffer, sample->buffer_offset, sample->row_length,
            submit_hashes);
        if (!loader_trace_take(&submit_copy_trace_count, 16))
            continue;

        okgl_diag3("[VK-SUBMIT-COPY]", (uint64_t)command_buffer->real,
                   (uint64_t)sample->image->real,
                   (uint64_t)sample->buffer->real);
        okgl_diag3("[VK-COPY-REC01]", sample->record_hashes[0],
                   sample->record_hashes[1], sample->buffer_offset);
        okgl_diag3("[VK-COPY-REC23]", sample->record_hashes[2],
                   sample->record_hashes[3], sample->row_length);
        okgl_diag3("[VK-COPY-SUB01]", submit_hashes[0],
                   submit_hashes[1], readable);
        okgl_diag3("[VK-COPY-SUB23]", submit_hashes[2],
                   submit_hashes[3], readable);
        int mutated = !readable;
        for (uint32_t j = 0; j < 4; j++)
            mutated |= sample->record_hashes[j] != submit_hashes[j];
        if (mutated)
            okgl_diag3("[VK-COPY-MUTATE]", (uint64_t)sample->image->real,
                       readable, sample->buffer_offset);
    }
    return found;
}

static void
loader_trace_image_upload(struct osito_buffer *buffer,
                          struct osito_image *image,
                          const VkCopyBufferToImageInfo2 *copy_info)
{
    if (image->width != 128 || image->height != 32 ||
        !buffer->memory || !buffer->memory->mapped_data ||
        !copy_info->regionCount || !copy_info->pRegions ||
        !loader_trace_take(&image_upload_trace_count, 32))
        return;

    const VkBufferImageCopy2 *region = &copy_info->pRegions[0];
    uint32_t row_length = region->bufferRowLength
        ? region->bufferRowLength : region->imageExtent.width;
    VkDeviceSize source_offset = buffer->memory_offset +
                                  region->bufferOffset;
    VkDeviceSize mapped_end = buffer->memory->mapped_offset +
                               buffer->memory->mapped_size;
    size_t row_stride = (size_t)row_length * 4u;
    VkDeviceSize required = (VkDeviceSize)row_stride * 32u;

    okgl_diag3("[VK-COPY-IMG]", (uint64_t)image,
               ((uint64_t)(uint32_t)image->format << 32) |
                   copy_info->regionCount,
               ((uint64_t)region->imageExtent.width << 32) |
                   region->imageExtent.height);

    if (region->imageOffset.x != 0 || region->imageOffset.y != 0 ||
        region->imageExtent.width != 128 ||
        region->imageExtent.height != 32 ||
        source_offset < buffer->memory->mapped_offset ||
        source_offset > mapped_end ||
        required > mapped_end - source_offset) {
        okgl_diag3("[VK-COPY-MISS]", (uint64_t)buffer,
                   source_offset, mapped_end);
        return;
    }

    const uint8_t *source = (const uint8_t *)buffer->memory->mapped_data +
        (size_t)(source_offset - buffer->memory->mapped_offset);
    uint64_t q0 = loader_trace_hash_rows(source, 32, 32, row_stride);
    uint64_t q1 = loader_trace_hash_rows(source + 32u * 4u,
                                         32, 32, row_stride);
    uint64_t q2 = loader_trace_hash_rows(source + 64u * 4u,
                                         32, 32, row_stride);
    uint64_t q3 = loader_trace_hash_rows(source + 96u * 4u,
                                         32, 32, row_stride);
    image->trace_dashboard =
        q0 == 0xcf8fcb37d7881d19ull &&
        q1 == 0xc5107c8d018e36e3ull &&
        q2 == 0xbcd521dd441052c6ull &&
        q3 == 0x5f6121b202fd2393ull;
    if (image->trace_dashboard && buffer->memory)
        image->trace_staging_memory_type =
            buffer->memory->memory_type_index;
    okgl_diag3("[VK-COPY-Q01]", (uint64_t)image, q0, q1);
    okgl_diag3("[VK-COPY-Q23]", (uint64_t)image, q2, q3);
}

static int
loader_trace_image_dimensions(uint32_t width, uint32_t height)
{
    return (width == 128 && height == 32) ||
           (width >= 1024 && height <= 64);
}

static void
loader_trace_descriptor_draw(struct osito_cmd_buffer *command_buffer,
                             uint32_t element_count, int indexed,
                             uint32_t first_element, int32_t vertex_offset)
{
    struct osito_descriptor_set *set =
        command_buffer->trace_graphics_set2;
    uint32_t sequence = ++command_buffer->trace_draw_sequence;
    if (!set || !loader_trace_image_dimensions(set->image_width,
                                                set->image_height))
        return;

    loader_trace_store_draw(command_buffer, set, sequence, element_count,
                            indexed, first_element, vertex_offset);
    if (!loader_trace_take(&descriptor_draw_trace_count, 480))
        return;

    okgl_diag3("[VK-DRAW-IMG]", (uint64_t)command_buffer->real,
               ((uint64_t)sequence << 32) | element_count,
               (uint64_t)set->image);
    okgl_diag3("[VK-DRAW-SET]", (uint64_t)set->real,
               (uint64_t)set->image_view,
               ((uint64_t)set->image_width << 32) |
                   ((uint64_t)set->image_height << 1) | !!indexed);
    loader_trace_vertex_draw(command_buffer, sequence, indexed,
                             first_element, vertex_offset);
}

static int
loader_supports_device_extension(const char *name)
{
    /* Do not expose an ICD extension until every command needed by clients
     * can cross the loader's wrapped device/command-buffer boundary. */
    if (strcmp(name, VK_EXT_SHADER_OBJECT_EXTENSION_NAME) == 0) {
        okgl_trace("[OKGL-VK] loader hides EXT_shader_object\n");
        return 0;
    }
    return 1;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceVersion(uint32_t *api_version)
{
    if (!api_version)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (unsigned i = 0; i < osito_icd_count; i++) {
        PFN_vkEnumerateInstanceVersion enumerate =
            (PFN_vkEnumerateInstanceVersion)osito_icd_table[i].get_proc_addr(
                VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
        if (!enumerate)
            continue;
        VkResult result = enumerate(api_version);
        if (result == VK_SUCCESS)
            return result;
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char *layer_name,
                                       uint32_t *property_count,
                                       VkExtensionProperties *properties)
{
    if (!property_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (layer_name)
        return VK_ERROR_LAYER_NOT_PRESENT;
    static const VkExtensionProperties extensions[] = {
        { VK_KHR_SURFACE_EXTENSION_NAME, 25 },
        { VK_OSITOK_COMPOSITOR_SURFACE_EXTENSION_NAME,
          VK_OSITOK_COMPOSITOR_SURFACE_SPEC_VERSION },
    };
    uint32_t total = sizeof(extensions) / sizeof(extensions[0]);
    if (!properties) {
        *property_count = total;
        return VK_SUCCESS;
    }
    uint32_t written = *property_count < total ? *property_count : total;
    memcpy(properties, extensions, sizeof(extensions[0]) * written);
    *property_count = written;
    return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t *property_count,
                                   VkLayerProperties *properties)
{
    if (!property_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    *property_count = 0;
    (void)properties;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!device || !name)
        return 0;
    if (strcmp(name, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)vkDestroyDevice;
    if (strcmp(name, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceQueue;
    if (strcmp(name, "vkDeviceWaitIdle") == 0)
        return (PFN_vkVoidFunction)vkDeviceWaitIdle;
    if (strcmp(name, "vkQueueWaitIdle") == 0)
        return (PFN_vkVoidFunction)vkQueueWaitIdle;
    if (strcmp(name, "vkGetCalibratedTimestampsEXT") == 0)
        return (PFN_vkVoidFunction)vkGetCalibratedTimestampsEXT;
    if (strcmp(name, "vkCreateDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorSetLayout;
    if (strcmp(name, "vkDestroyDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorSetLayout;
    if (strcmp(name, "vkCreateDescriptorUpdateTemplate") == 0 ||
        strcmp(name, "vkCreateDescriptorUpdateTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorUpdateTemplate;
    if (strcmp(name, "vkDestroyDescriptorUpdateTemplate") == 0 ||
        strcmp(name, "vkDestroyDescriptorUpdateTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorUpdateTemplate;
    if (strcmp(name, "vkUpdateDescriptorSetWithTemplate") == 0 ||
        strcmp(name, "vkUpdateDescriptorSetWithTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkUpdateDescriptorSetWithTemplate;
    if (strcmp(name, "vkCmdPushDescriptorSet") == 0 ||
        strcmp(name, "vkCmdPushDescriptorSetKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdPushDescriptorSetKHR;
    if (strcmp(name, "vkCmdPushDescriptorSetWithTemplate") == 0 ||
        strcmp(name, "vkCmdPushDescriptorSetWithTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdPushDescriptorSetWithTemplateKHR;
    if (strcmp(name, "vkCreatePipelineLayout") == 0)
        return (PFN_vkVoidFunction)vkCreatePipelineLayout;
    if (strcmp(name, "vkDestroyPipelineLayout") == 0)
        return (PFN_vkVoidFunction)vkDestroyPipelineLayout;
    if (strcmp(name, "vkCreateSemaphore") == 0)
        return (PFN_vkVoidFunction)vkCreateSemaphore;
    if (strcmp(name, "vkDestroySemaphore") == 0)
        return (PFN_vkVoidFunction)vkDestroySemaphore;
    if (strcmp(name, "vkGetSemaphoreCounterValue") == 0 ||
        strcmp(name, "vkGetSemaphoreCounterValueKHR") == 0)
        return (PFN_vkVoidFunction)vkGetSemaphoreCounterValue;
    if (strcmp(name, "vkWaitSemaphores") == 0 ||
        strcmp(name, "vkWaitSemaphoresKHR") == 0)
        return (PFN_vkVoidFunction)vkWaitSemaphores;
    if (strcmp(name, "vkSignalSemaphore") == 0 ||
        strcmp(name, "vkSignalSemaphoreKHR") == 0)
        return (PFN_vkVoidFunction)vkSignalSemaphore;
    if (strcmp(name, "vkCreateFence") == 0)
        return (PFN_vkVoidFunction)vkCreateFence;
    if (strcmp(name, "vkDestroyFence") == 0)
        return (PFN_vkVoidFunction)vkDestroyFence;
    if (strcmp(name, "vkResetFences") == 0)
        return (PFN_vkVoidFunction)vkResetFences;
    if (strcmp(name, "vkWaitForFences") == 0)
        return (PFN_vkVoidFunction)vkWaitForFences;
    if (strcmp(name, "vkCreateCommandPool") == 0)
        return (PFN_vkVoidFunction)vkCreateCommandPool;
    if (strcmp(name, "vkDestroyCommandPool") == 0)
        return (PFN_vkVoidFunction)vkDestroyCommandPool;
    if (strcmp(name, "vkResetCommandPool") == 0)
        return (PFN_vkVoidFunction)vkResetCommandPool;
    if (strcmp(name, "vkAllocateCommandBuffers") == 0)
        return (PFN_vkVoidFunction)vkAllocateCommandBuffers;
    if (strcmp(name, "vkFreeCommandBuffers") == 0)
        return (PFN_vkVoidFunction)vkFreeCommandBuffers;
    if (strcmp(name, "vkBeginCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkBeginCommandBuffer;
    if (strcmp(name, "vkEndCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkEndCommandBuffer;
    if (strcmp(name, "vkResetCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkResetCommandBuffer;
    if (strcmp(name, "vkQueueSubmit") == 0)
        return (PFN_vkVoidFunction)vkQueueSubmit;
    if (strcmp(name, "vkQueueSubmit2") == 0 ||
        strcmp(name, "vkQueueSubmit2KHR") == 0)
        return (PFN_vkVoidFunction)vkQueueSubmit2;
    if (strcmp(name, "vkCreateBuffer") == 0)
        return (PFN_vkVoidFunction)vkCreateBuffer;
    if (strcmp(name, "vkDestroyBuffer") == 0)
        return (PFN_vkVoidFunction)vkDestroyBuffer;
    if (strcmp(name, "vkCreateBufferView") == 0)
        return (PFN_vkVoidFunction)vkCreateBufferView;
    if (strcmp(name, "vkDestroyBufferView") == 0)
        return (PFN_vkVoidFunction)vkDestroyBufferView;
    if (strcmp(name, "vkGetBufferMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)vkGetBufferMemoryRequirements;
    if (strcmp(name, "vkGetBufferMemoryRequirements2") == 0 ||
        strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetBufferMemoryRequirements2;
    if (strcmp(name, "vkBindBufferMemory") == 0)
        return (PFN_vkVoidFunction)vkBindBufferMemory;
    if (strcmp(name, "vkAllocateMemory") == 0)
        return (PFN_vkVoidFunction)vkAllocateMemory;
    if (strcmp(name, "vkFreeMemory") == 0)
        return (PFN_vkVoidFunction)vkFreeMemory;
    if (strcmp(name, "vkMapMemory") == 0)
        return (PFN_vkVoidFunction)vkMapMemory;
    if (strcmp(name, "vkUnmapMemory") == 0)
        return (PFN_vkVoidFunction)vkUnmapMemory;
    if (strcmp(name, "vkCmdCopyBuffer") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBuffer;
    if (strcmp(name, "vkCmdPipelineBarrier") == 0)
        return (PFN_vkVoidFunction)vkCmdPipelineBarrier;
    if (strcmp(name, "vkCmdCopyBufferToImage") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBufferToImage;
    if (strcmp(name, "vkCmdCopyImageToBuffer") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyImageToBuffer;
    if (strcmp(name, "vkCmdCopyBuffer2") == 0 ||
        strcmp(name, "vkCmdCopyBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBuffer2;
    if (strcmp(name, "vkCmdPipelineBarrier2") == 0 ||
        strcmp(name, "vkCmdPipelineBarrier2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdPipelineBarrier2;
    if (strcmp(name, "vkCmdCopyBufferToImage2") == 0 ||
        strcmp(name, "vkCmdCopyBufferToImage2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBufferToImage2;
    if (strcmp(name, "vkCmdCopyImageToBuffer2") == 0 ||
        strcmp(name, "vkCmdCopyImageToBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyImageToBuffer2;
    if (strcmp(name, "vkCmdClearColorImage") == 0)
        return (PFN_vkVoidFunction)vkCmdClearColorImage;
    if (strcmp(name, "vkCmdClearDepthStencilImage") == 0)
        return (PFN_vkVoidFunction)vkCmdClearDepthStencilImage;
    if (strcmp(name, "vkCmdClearAttachments") == 0)
        return (PFN_vkVoidFunction)vkCmdClearAttachments;
    if (strcmp(name, "vkCmdBeginRendering") == 0 ||
        strcmp(name, "vkCmdBeginRenderingKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdBeginRendering;
    if (strcmp(name, "vkCmdEndRendering") == 0 ||
        strcmp(name, "vkCmdEndRenderingKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdEndRendering;
    if (strcmp(name, "vkCmdBindVertexBuffers") == 0)
        return (PFN_vkVoidFunction)vkCmdBindVertexBuffers;
    if (strcmp(name, "vkCmdBindVertexBuffers2") == 0 ||
        strcmp(name, "vkCmdBindVertexBuffers2EXT") == 0)
        return (PFN_vkVoidFunction)vkCmdBindVertexBuffers2;
    if (strcmp(name, "vkCmdBindIndexBuffer") == 0)
        return (PFN_vkVoidFunction)vkCmdBindIndexBuffer;
    if (strcmp(name, "vkCmdBindIndexBuffer2") == 0 ||
        strcmp(name, "vkCmdBindIndexBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdBindIndexBuffer2KHR;
    if (strcmp(name, "vkCmdBindPipeline") == 0)
        return (PFN_vkVoidFunction)vkCmdBindPipeline;
    if (strcmp(name, "vkCmdSetAttachmentFeedbackLoopEnableEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetAttachmentFeedbackLoopEnableEXT;
    if (strcmp(name, "vkCmdSetCullMode") == 0 ||
        strcmp(name, "vkCmdSetCullModeEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetCullMode;
    if (strcmp(name, "vkCmdSetFrontFace") == 0 ||
        strcmp(name, "vkCmdSetFrontFaceEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetFrontFace;
    if (strcmp(name, "vkCmdSetPrimitiveTopology") == 0 ||
        strcmp(name, "vkCmdSetPrimitiveTopologyEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetPrimitiveTopology;
    if (strcmp(name, "vkCmdSetViewport") == 0)
        return (PFN_vkVoidFunction)vkCmdSetViewport;
    if (strcmp(name, "vkCmdSetScissor") == 0)
        return (PFN_vkVoidFunction)vkCmdSetScissor;
    if (strcmp(name, "vkCmdSetLineWidth") == 0)
        return (PFN_vkVoidFunction)vkCmdSetLineWidth;
    if (strcmp(name, "vkCmdSetDepthBias") == 0)
        return (PFN_vkVoidFunction)vkCmdSetDepthBias;
    if (strcmp(name, "vkCmdSetBlendConstants") == 0)
        return (PFN_vkVoidFunction)vkCmdSetBlendConstants;
    if (strcmp(name, "vkCmdSetDepthBounds") == 0)
        return (PFN_vkVoidFunction)vkCmdSetDepthBounds;
    if (strcmp(name, "vkCmdSetStencilCompareMask") == 0)
        return (PFN_vkVoidFunction)vkCmdSetStencilCompareMask;
    if (strcmp(name, "vkCmdSetStencilWriteMask") == 0)
        return (PFN_vkVoidFunction)vkCmdSetStencilWriteMask;
    if (strcmp(name, "vkCmdSetStencilReference") == 0)
        return (PFN_vkVoidFunction)vkCmdSetStencilReference;
    if (strcmp(name, "vkCmdSetViewportWithCount") == 0 ||
        strcmp(name, "vkCmdSetViewportWithCountEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetViewportWithCount;
    if (strcmp(name, "vkCmdSetScissorWithCount") == 0 ||
        strcmp(name, "vkCmdSetScissorWithCountEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetScissorWithCount;
    if (strcmp(name, "vkCmdDraw") == 0)
        return (PFN_vkVoidFunction)vkCmdDraw;
    if (strcmp(name, "vkCmdDrawIndexed") == 0)
        return (PFN_vkVoidFunction)vkCmdDrawIndexed;
    if (strcmp(name, "vkCmdBindDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkCmdBindDescriptorSets;
    if (strcmp(name, "vkCmdPushConstants") == 0)
        return (PFN_vkVoidFunction)vkCmdPushConstants;
    if (strcmp(name, "vkCreateImage") == 0)
        return (PFN_vkVoidFunction)vkCreateImage;
    if (strcmp(name, "vkDestroyImage") == 0)
        return (PFN_vkVoidFunction)vkDestroyImage;
    if (strcmp(name, "vkGetImageMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)vkGetImageMemoryRequirements;
    if (strcmp(name, "vkGetImageMemoryRequirements2") == 0 ||
        strcmp(name, "vkGetImageMemoryRequirements2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetImageMemoryRequirements2;
    if (strcmp(name, "vkBindImageMemory") == 0)
        return (PFN_vkVoidFunction)vkBindImageMemory;
    if (strcmp(name, "vkCreateImageView") == 0)
        return (PFN_vkVoidFunction)vkCreateImageView;
    if (strcmp(name, "vkDestroyImageView") == 0)
        return (PFN_vkVoidFunction)vkDestroyImageView;
    if (strcmp(name, "vkCreateSampler") == 0)
        return (PFN_vkVoidFunction)vkCreateSampler;
    if (strcmp(name, "vkDestroySampler") == 0)
        return (PFN_vkVoidFunction)vkDestroySampler;
    if (strcmp(name, "vkCreateShaderModule") == 0)
        return (PFN_vkVoidFunction)vkCreateShaderModule;
    if (strcmp(name, "vkDestroyShaderModule") == 0)
        return (PFN_vkVoidFunction)vkDestroyShaderModule;
    if (strcmp(name, "vkCreateGraphicsPipelines") == 0)
        return (PFN_vkVoidFunction)vkCreateGraphicsPipelines;
    if (strcmp(name, "vkCreateComputePipelines") == 0)
        return (PFN_vkVoidFunction)vkCreateComputePipelines;
    if (strcmp(name, "vkDestroyPipeline") == 0)
        return (PFN_vkVoidFunction)vkDestroyPipeline;
    if (strcmp(name, "vkCreateDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorPool;
    if (strcmp(name, "vkDestroyDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorPool;
    if (strcmp(name, "vkResetDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkResetDescriptorPool;
    if (strcmp(name, "vkAllocateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkAllocateDescriptorSets;
    if (strcmp(name, "vkUpdateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkUpdateDescriptorSets;
    return 0;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties(VkPhysicalDevice physical_device,
                              VkPhysicalDeviceProperties *properties)
{
    if (!physical_device || !properties)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceProperties get_properties =
        (PFN_vkGetPhysicalDeviceProperties)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkGetPhysicalDeviceProperties");
    if (get_properties)
        get_properties(self->real, properties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physical_device, uint32_t *property_count,
    VkQueueFamilyProperties *properties)
{
    if (!physical_device || !property_count)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_properties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceQueueFamilyProperties");
    if (get_properties)
        get_properties(self->real, property_count, properties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures(VkPhysicalDevice physical_device,
                            VkPhysicalDeviceFeatures *features)
{
    if (!physical_device || !features)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceFeatures get_features =
        (PFN_vkGetPhysicalDeviceFeatures)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkGetPhysicalDeviceFeatures");
    if (get_features)
        get_features(self->real, features);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties *properties)
{
    if (!physical_device || !properties)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceMemoryProperties get_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceMemoryProperties");
    if (get_properties)
        get_properties(self->real, properties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physical_device, VkFormat format,
    VkFormatProperties *properties)
{
    if (!physical_device || !properties)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceFormatProperties get_properties =
        (PFN_vkGetPhysicalDeviceFormatProperties)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceFormatProperties");
    if (get_properties) {
        get_properties(self->real, format, properties);
        return;
    }

    PFN_vkGetPhysicalDeviceFormatProperties2 get_properties2 =
        (PFN_vkGetPhysicalDeviceFormatProperties2)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceFormatProperties2");
    VkFormatProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
    };
    if (get_properties2)
        get_properties2(self->real, format, &properties2);
    *properties = properties2.formatProperties;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physical_device, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage,
    VkImageCreateFlags flags, VkImageFormatProperties *properties)
{
    if (!physical_device || !properties)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceImageFormatProperties get_properties =
        (PFN_vkGetPhysicalDeviceImageFormatProperties)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceImageFormatProperties");
    if (get_properties)
        return get_properties(self->real, format, type, tiling, usage,
                              flags, properties);

    PFN_vkGetPhysicalDeviceImageFormatProperties2 get_properties2 =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceImageFormatProperties2");
    if (!get_properties2)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = format,
        .type = type,
        .tiling = tiling,
        .usage = usage,
        .flags = flags,
    };
    VkImageFormatProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    VkResult result = get_properties2(self->real, &info, &properties2);
    if (result == VK_SUCCESS)
        *properties = properties2.imageFormatProperties;
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(
    VkPhysicalDevice physical_device, uint32_t *time_domain_count,
    VkTimeDomainKHR *time_domains)
{
    if (!physical_device || !time_domain_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT get_domains =
        (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
    return get_domains
        ? get_domains(self->real, time_domain_count, time_domains)
        : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceLayerProperties(VkPhysicalDevice physical_device,
                                 uint32_t *property_count,
                                 VkLayerProperties *properties)
{
    (void)physical_device;
    (void)properties;
    if (!property_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    *property_count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice physical_device, VkFormat format, VkImageType type,
    VkSampleCountFlagBits samples, VkImageUsageFlags usage,
    VkImageTiling tiling, uint32_t *property_count,
    VkSparseImageFormatProperties *properties)
{
    if (!physical_device || !property_count)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties get_properties =
        (PFN_vkGetPhysicalDeviceSparseImageFormatProperties)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceSparseImageFormatProperties");
    if (get_properties) {
        get_properties(self->real, format, type, samples, usage, tiling,
                       property_count, properties);
        return;
    }
    *property_count = 0;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2(VkPhysicalDevice physical_device,
                               VkPhysicalDeviceProperties2 *properties)
{
    if (!physical_device || !properties)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceProperties2 get_properties =
        (PFN_vkGetPhysicalDeviceProperties2)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkGetPhysicalDeviceProperties2");
    if (get_properties)
        get_properties(self->real, properties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physical_device,
                             VkPhysicalDeviceFeatures2 *features)
{
    if (!physical_device || !features)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceFeatures2 get_features =
        (PFN_vkGetPhysicalDeviceFeatures2)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkGetPhysicalDeviceFeatures2");
    if (get_features)
        get_features(self->real, features);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties2 *properties)
{
    if (!physical_device || !properties)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceMemoryProperties2 get_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties2)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceMemoryProperties2");
    if (get_properties)
        get_properties(self->real, properties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice physical_device, uint32_t *property_count,
    VkQueueFamilyProperties2 *properties)
{
    if (!physical_device || !property_count)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 get_properties2 =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceQueueFamilyProperties2");
    if (get_properties2) {
        get_properties2(self->real, property_count, properties);
        return;
    }
    if (!properties) {
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                                 property_count, NULL);
        return;
    }

    uint32_t capacity = *property_count;
    VkQueueFamilyProperties *legacy =
        malloc(sizeof(*legacy) * capacity);
    if (!legacy) {
        *property_count = 0;
        return;
    }
    uint32_t count = capacity;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count,
                                             legacy);
    uint32_t written = count < capacity ? count : capacity;
    for (uint32_t i = 0; i < written; i++)
        properties[i].queueFamilyProperties = legacy[i];
    free(legacy);
    *property_count = count;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceSparseImageFormatInfo2 *info,
    uint32_t *property_count, VkSparseImageFormatProperties2 *properties)
{
    if (!physical_device || !info || !property_count)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties2 get_properties2 =
        (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceSparseImageFormatProperties2");
    if (get_properties2) {
        get_properties2(self->real, info, property_count, properties);
        return;
    }
    if (!properties) {
        vkGetPhysicalDeviceSparseImageFormatProperties(
            physical_device, info->format, info->type, info->samples,
            info->usage, info->tiling, property_count, NULL);
        return;
    }

    uint32_t capacity = *property_count;
    VkSparseImageFormatProperties *legacy =
        malloc(sizeof(*legacy) * capacity);
    if (!legacy) {
        *property_count = 0;
        return;
    }
    uint32_t count = capacity;
    vkGetPhysicalDeviceSparseImageFormatProperties(
        physical_device, info->format, info->type, info->samples,
        info->usage, info->tiling, &count, legacy);
    uint32_t written = count < capacity ? count : capacity;
    for (uint32_t i = 0; i < written; i++)
        properties[i].properties = legacy[i];
    free(legacy);
    *property_count = count;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physical_device, VkFormat format,
    VkFormatProperties2 *properties)
{
    if (!physical_device || !properties)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceFormatProperties2 get_properties =
        (PFN_vkGetPhysicalDeviceFormatProperties2)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceFormatProperties2");
    if (get_properties)
        get_properties(self->real, format, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceImageFormatInfo2 *info,
    VkImageFormatProperties2 *properties)
{
    if (!physical_device || !info || !properties)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceImageFormatProperties2 get_properties =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceImageFormatProperties2");
    if (!get_properties)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return get_properties(self->real, info, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char *layer_name,
    uint32_t *property_count, VkExtensionProperties *properties)
{
    if (!physical_device || !property_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkEnumerateDeviceExtensionProperties enumerate =
        (PFN_vkEnumerateDeviceExtensionProperties)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkEnumerateDeviceExtensionProperties");
    if (!enumerate)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    uint32_t real_count = 0;
    VkResult result = enumerate(self->real, layer_name, &real_count, NULL);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        return result;
    if (real_count == 0) {
        *property_count = 0;
        return VK_SUCCESS;
    }
    if (real_count > (size_t)-1 / sizeof(VkExtensionProperties))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    VkExtensionProperties *real_properties =
        malloc((size_t)real_count * sizeof(*real_properties));
    if (!real_properties)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    uint32_t fetched = real_count;
    result = enumerate(self->real, layer_name, &fetched, real_properties);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        free(real_properties);
        return result;
    }

    uint32_t supported_count = 0;
    for (uint32_t i = 0; i < fetched; i++) {
        if (loader_supports_device_extension(
                real_properties[i].extensionName))
            supported_count++;
    }

    if (!properties) {
        *property_count = supported_count;
        free(real_properties);
        return VK_SUCCESS;
    }

    uint32_t capacity = *property_count;
    uint32_t written = 0;
    for (uint32_t i = 0; i < fetched && written < capacity; i++) {
        if (!loader_supports_device_extension(
                real_properties[i].extensionName))
            continue;
        properties[written++] = real_properties[i];
    }
    free(real_properties);
    *property_count = written;
    return written < supported_count ? VK_INCOMPLETE : VK_SUCCESS;
}

typedef VkResult (VKAPI_PTR *PFN_ositoCreateSurface)(
    VkInstance,
    const VkOsitoCompositorSurfaceCreateInfoOSITOK *,
    const VkAllocationCallbacks *, VkSurfaceKHR *);

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateOsitokCompositorSurfaceKHR(
    VkInstance instance,
    const VkOsitoCompositorSurfaceCreateInfoOSITOK *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    if (!instance || !create_info || !surface)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_instance *self = osito_instance_from(instance);
    if (!self->icd_instance_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_icd_inst *owner = &self->icd_instances[0];
    PFN_ositoCreateSurface create =
        (PFN_ositoCreateSurface)owner->icd->get_proc_addr(
            owner->handle, "vkCreateOsitokCompositorSurfaceKHR");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSurfaceKHR real = VK_NULL_HANDLE;
    VkResult result = create(owner->handle, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_surface *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroySurfaceKHR destroy =
            (PFN_vkDestroySurfaceKHR)owner->icd->get_proc_addr(
                owner->handle, "vkDestroySurfaceKHR");
        if (destroy)
            destroy(owner->handle, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(wrapper, 0, sizeof(*wrapper));
    wrapper->owner_inst = self;
    wrapper->owner_icd = owner;
    wrapper->real = real;
    *surface = (VkSurfaceKHR)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                    const VkAllocationCallbacks *allocator)
{
    if (!instance || !surface)
        return;
    struct osito_surface *wrapper =
        (struct osito_surface *)(uintptr_t)surface;
    PFN_vkDestroySurfaceKHR destroy =
        (PFN_vkDestroySurfaceKHR)wrapper->owner_icd->icd->get_proc_addr(
            wrapper->owner_icd->handle, "vkDestroySurfaceKHR");
    if (destroy)
        destroy(wrapper->owner_icd->handle, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physical_device,
               const VkDeviceCreateInfo *create_info,
               const VkAllocationCallbacks *allocator,
               VkDevice *device)
{
    if (!physical_device || !create_info || !device)
        return VK_ERROR_INITIALIZATION_FAILED;
    printf("[VKLOADER] create device: queues=%u extensions=%u pNext=%p features=%p\n",
           create_info->queueCreateInfoCount,
           create_info->enabledExtensionCount, create_info->pNext,
           create_info->pEnabledFeatures);
    for (uint32_t i = 0; i < create_info->queueCreateInfoCount; i++) {
        const VkDeviceQueueCreateInfo *queue =
            &create_info->pQueueCreateInfos[i];
        printf("[VKLOADER] create device queue[%u]: family=%u count=%u flags=0x%x pNext=%p\n",
               i, queue->queueFamilyIndex, queue->queueCount, queue->flags,
               queue->pNext);
    }
    for (uint32_t i = 0; i < create_info->enabledExtensionCount; i++)
        printf("[VKLOADER] create device extension[%u]=%s\n", i,
               create_info->ppEnabledExtensionNames[i]);
    const VkBaseInStructure *feature =
        (const VkBaseInStructure *)create_info->pNext;
    for (uint32_t i = 0; feature && i < 64; i++) {
        printf("[VKLOADER] create device feature[%u] sType=%u\n", i,
               (uint32_t)feature->sType);
        feature = feature->pNext;
    }
    struct osito_phys_device *physical = osito_phys_from(physical_device);
    PFN_vkCreateDevice create =
        (PFN_vkCreateDevice)physical->owner->icd->get_proc_addr(
            physical->owner->handle, "vkCreateDevice");
    if (!create) {
        printf("[VKLOADER] create device: ICD proc missing\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkDevice real = VK_NULL_HANDLE;
    VkResult result = create(physical->real, create_info, allocator, &real);
    printf("[VKLOADER] create device: ICD result=%d real=%p\n", result,
           (void *)real);
    if (result != VK_SUCCESS)
        return result;

    struct osito_device *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyDevice destroy =
            (PFN_vkDestroyDevice)physical->owner->icd->get_proc_addr(
                physical->owner->handle, "vkDestroyDevice");
        if (destroy)
            destroy(real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(wrapper, 0, sizeof(*wrapper));
    set_loader_magic_value(wrapper);
    wrapper->owner = physical->owner;
    wrapper->real = real;
    *device = osito_device_to(wrapper);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator)
{
    if (!device)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_queue *queue = self->queues;
    while (queue) {
        struct osito_queue *next = queue->next;
        free(queue);
        queue = next;
    }
    PFN_vkDestroyDevice destroy =
        (PFN_vkDestroyDevice)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyDevice");
    if (destroy)
        destroy(self->real, allocator);
    free(self);
}

VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue(VkDevice device, uint32_t queue_family_index,
                 uint32_t queue_index, VkQueue *queue)
{
    if (!device || !queue)
        return;
    *queue = VK_NULL_HANDLE;
    struct osito_device *self = osito_device_from(device);
    PFN_vkGetDeviceQueue get_queue =
        (PFN_vkGetDeviceQueue)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkGetDeviceQueue");
    if (!get_queue)
        return;
    VkQueue real = VK_NULL_HANDLE;
    get_queue(self->real, queue_family_index, queue_index, &real);
    if (!real)
        return;
    for (struct osito_queue *existing = self->queues; existing;
         existing = existing->next) {
        if (existing->real == real) {
            *queue = (VkQueue)existing;
            return;
        }
    }
    struct osito_queue *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper)
        return;
    memset(wrapper, 0, sizeof(*wrapper));
    set_loader_magic_value(wrapper);
    wrapper->owner = self;
    wrapper->real = real;
    wrapper->family_index = queue_family_index;
    wrapper->next = self->queues;
    self->queues = wrapper;
    *queue = (VkQueue)wrapper;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkDeviceWaitIdle(VkDevice device)
{
    if (!device)
        return VK_ERROR_DEVICE_LOST;
    struct osito_device *self = osito_device_from(device);
    PFN_vkDeviceWaitIdle wait_idle =
        (PFN_vkDeviceWaitIdle)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDeviceWaitIdle");
    return wait_idle ? wait_idle(self->real) : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueWaitIdle(VkQueue queue)
{
    if (!queue)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_queue *wrapper = (struct osito_queue *)queue;
    struct osito_device *device = wrapper->owner;
    PFN_vkQueueWaitIdle wait_idle =
        (PFN_vkQueueWaitIdle)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkQueueWaitIdle");
    return wait_idle ? wait_idle(wrapper->real)
                     : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetCalibratedTimestampsEXT(
    VkDevice device, uint32_t timestamp_count,
    const VkCalibratedTimestampInfoKHR *timestamp_infos,
    uint64_t *timestamps, uint64_t *max_deviation)
{
    if (!device)
        return VK_ERROR_DEVICE_LOST;
    struct osito_device *self = osito_device_from(device);
    PFN_vkGetCalibratedTimestampsEXT get_timestamps =
        (PFN_vkGetCalibratedTimestampsEXT)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkGetCalibratedTimestampsEXT");
    return get_timestamps
        ? get_timestamps(self->real, timestamp_count, timestamp_infos,
                         timestamps, max_deviation)
        : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDescriptorSetLayout *set_layout)
{
    if (!device || !create_info || !set_layout)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateDescriptorSetLayout create =
        (PFN_vkCreateDescriptorSetLayout)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateDescriptorSetLayout");
    return create ? create(self->real, create_info, allocator, set_layout)
                  : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDescriptorSetLayout(
    VkDevice device, VkDescriptorSetLayout set_layout,
    const VkAllocationCallbacks *allocator)
{
    if (!device || !set_layout)
        return;
    struct osito_device *self = osito_device_from(device);
    PFN_vkDestroyDescriptorSetLayout destroy =
        (PFN_vkDestroyDescriptorSetLayout)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyDescriptorSetLayout");
    if (destroy)
        destroy(self->real, set_layout, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorUpdateTemplate(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDescriptorUpdateTemplate *update_template)
{
    if (!device || !create_info || !update_template ||
        (create_info->descriptorUpdateEntryCount &&
         !create_info->pDescriptorUpdateEntries))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateDescriptorUpdateTemplate create =
        (PFN_vkCreateDescriptorUpdateTemplate)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkCreateDescriptorUpdateTemplate");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDescriptorUpdateTemplateCreateInfo real_info = *create_info;
    /* Descriptor set layouts are native ICD handles in this loader. */
    if (create_info->pipelineLayout) {
        struct osito_pipeline_layout *layout =
            (struct osito_pipeline_layout *)(uintptr_t)
                create_info->pipelineLayout;
        if (layout->owner != self)
            return VK_ERROR_INITIALIZATION_FAILED;
        real_info.pipelineLayout = layout->real;
    }
    VkDescriptorUpdateTemplate real = VK_NULL_HANDLE;
    VkResult result = create(self->real, &real_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;

    size_t wrapper_size = sizeof(struct osito_descriptor_update_template) +
        (size_t)create_info->descriptorUpdateEntryCount *
            sizeof(VkDescriptorUpdateTemplateEntry);
    struct osito_descriptor_update_template *wrapper = malloc(wrapper_size);
    if (!wrapper) {
        PFN_vkDestroyDescriptorUpdateTemplate destroy =
            (PFN_vkDestroyDescriptorUpdateTemplate)
                self->owner->icd->get_proc_addr(
                    self->owner->handle, "vkDestroyDescriptorUpdateTemplate");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    wrapper->template_type = create_info->templateType;
    wrapper->pipeline_bind_point = create_info->pipelineBindPoint;
    wrapper->entry_count = create_info->descriptorUpdateEntryCount;
    if (wrapper->entry_count) {
        memcpy(wrapper->entries, create_info->pDescriptorUpdateEntries,
               (size_t)wrapper->entry_count * sizeof(wrapper->entries[0]));
    }
    *update_template =
        (VkDescriptorUpdateTemplate)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDescriptorUpdateTemplate(
    VkDevice device, VkDescriptorUpdateTemplate update_template,
    const VkAllocationCallbacks *allocator)
{
    if (!device || !update_template)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_descriptor_update_template *wrapper =
        (struct osito_descriptor_update_template *)(uintptr_t)
            update_template;
    if (wrapper->owner != self)
        return;
    PFN_vkDestroyDescriptorUpdateTemplate destroy =
        (PFN_vkDestroyDescriptorUpdateTemplate)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyDescriptorUpdateTemplate");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreatePipelineLayout(
    VkDevice device, const VkPipelineLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkPipelineLayout *pipeline_layout)
{
    if (!device || !create_info || !pipeline_layout)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreatePipelineLayout create =
        (PFN_vkCreatePipelineLayout)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreatePipelineLayout");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkPipelineLayout real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_pipeline_layout *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyPipelineLayout destroy =
            (PFN_vkDestroyPipelineLayout)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyPipelineLayout");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *pipeline_layout = (VkPipelineLayout)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyPipelineLayout(
    VkDevice device, VkPipelineLayout pipeline_layout,
    const VkAllocationCallbacks *allocator)
{
    if (!device || !pipeline_layout)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_pipeline_layout *wrapper =
        (struct osito_pipeline_layout *)(uintptr_t)pipeline_layout;
    PFN_vkDestroyPipelineLayout destroy =
        (PFN_vkDestroyPipelineLayout)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyPipelineLayout");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSemaphore(VkDevice device,
                  const VkSemaphoreCreateInfo *create_info,
                  const VkAllocationCallbacks *allocator,
                  VkSemaphore *semaphore)
{
    if (!device || !create_info || !semaphore)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateSemaphore create =
        (PFN_vkCreateSemaphore)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateSemaphore");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSemaphore real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_semaphore *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroySemaphore destroy =
            (PFN_vkDestroySemaphore)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroySemaphore");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *semaphore = (VkSemaphore)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySemaphore(VkDevice device, VkSemaphore semaphore,
                   const VkAllocationCallbacks *allocator)
{
    if (!device || !semaphore)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_semaphore *wrapper =
        (struct osito_semaphore *)(uintptr_t)semaphore;
    PFN_vkDestroySemaphore destroy =
        (PFN_vkDestroySemaphore)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroySemaphore");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore,
                           uint64_t *value)
{
    if (!device || !semaphore || !value)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_semaphore *wrapper =
        (struct osito_semaphore *)(uintptr_t)semaphore;
    if (wrapper->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetSemaphoreCounterValue get_value =
        (PFN_vkGetSemaphoreCounterValue)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkGetSemaphoreCounterValue");
    return get_value ? get_value(self->real, wrapper->real, value)
                     : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *wait_info,
                 uint64_t timeout)
{
    if (!device || !wait_info ||
        (wait_info->semaphoreCount && !wait_info->pSemaphores))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkWaitSemaphores wait =
        (PFN_vkWaitSemaphores)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkWaitSemaphores");
    if (!wait)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    VkSemaphoreWaitInfo real_info = *wait_info;
    VkSemaphore *real_semaphores = 0;
    if (wait_info->semaphoreCount) {
        real_semaphores = malloc(
            (size_t)wait_info->semaphoreCount * sizeof(*real_semaphores));
        if (!real_semaphores)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < wait_info->semaphoreCount; i++) {
            struct osito_semaphore *wrapper =
                (struct osito_semaphore *)(uintptr_t)
                    wait_info->pSemaphores[i];
            if (!wrapper || wrapper->owner != self) {
                free(real_semaphores);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            real_semaphores[i] = wrapper->real;
        }
        real_info.pSemaphores = real_semaphores;
    }
    VkResult result = wait(self->real, &real_info, timeout);
    free(real_semaphores);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkSignalSemaphore(VkDevice device,
                  const VkSemaphoreSignalInfo *signal_info)
{
    if (!device || !signal_info || !signal_info->semaphore)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_semaphore *wrapper =
        (struct osito_semaphore *)(uintptr_t)signal_info->semaphore;
    if (wrapper->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkSignalSemaphore signal =
        (PFN_vkSignalSemaphore)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkSignalSemaphore");
    if (!signal)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSemaphoreSignalInfo real_info = *signal_info;
    real_info.semaphore = wrapper->real;
    return signal(self->real, &real_info);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFence(VkDevice device, const VkFenceCreateInfo *create_info,
              const VkAllocationCallbacks *allocator, VkFence *fence)
{
    if (!device || !create_info || !fence)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateFence create =
        (PFN_vkCreateFence)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateFence");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkFence real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_fence *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyFence destroy =
            (PFN_vkDestroyFence)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyFence");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *fence = (VkFence)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyFence(VkDevice device, VkFence fence,
               const VkAllocationCallbacks *allocator)
{
    if (!device || !fence)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_fence *wrapper = (struct osito_fence *)(uintptr_t)fence;
    PFN_vkDestroyFence destroy =
        (PFN_vkDestroyFence)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyFence");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkResetFences(VkDevice device, uint32_t fence_count, const VkFence *fences)
{
    if (!device || !fence_count || !fences)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    VkFence *real = malloc((size_t)fence_count * sizeof(*real));
    if (!real)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < fence_count; i++) {
        struct osito_fence *wrapper =
            (struct osito_fence *)(uintptr_t)fences[i];
        if (!wrapper || wrapper->owner != self) {
            free(real);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        real[i] = wrapper->real;
    }
    PFN_vkResetFences reset =
        (PFN_vkResetFences)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkResetFences");
    VkResult result = reset
        ? reset(self->real, fence_count, real)
        : VK_ERROR_EXTENSION_NOT_PRESENT;
    free(real);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkWaitForFences(VkDevice device, uint32_t fence_count,
                const VkFence *fences, VkBool32 wait_all, uint64_t timeout)
{
    if (!device || !fence_count || !fences)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    VkFence *real = malloc((size_t)fence_count * sizeof(*real));
    if (!real)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < fence_count; i++) {
        struct osito_fence *wrapper =
            (struct osito_fence *)(uintptr_t)fences[i];
        if (!wrapper || wrapper->owner != self) {
            free(real);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        real[i] = wrapper->real;
    }
    PFN_vkWaitForFences wait =
        (PFN_vkWaitForFences)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkWaitForFences");
    VkResult result = wait
        ? wait(self->real, fence_count, real, wait_all, timeout)
        : VK_ERROR_EXTENSION_NOT_PRESENT;
    free(real);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *allocate_info,
                 const VkAllocationCallbacks *allocator,
                 VkDeviceMemory *memory)
{
    if (!device || !allocate_info || !memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkAllocateMemory allocate =
        (PFN_vkAllocateMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkAllocateMemory");
    if (!allocate)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDeviceMemory real = VK_NULL_HANDLE;
    VkResult result = allocate(
        self->real, allocate_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_memory *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkFreeMemory release =
            (PFN_vkFreeMemory)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkFreeMemory");
        if (release)
            release(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    wrapper->allocation_size = allocate_info->allocationSize;
    wrapper->memory_type_index = allocate_info->memoryTypeIndex;
    wrapper->mapped_data = 0;
    wrapper->mapped_offset = 0;
    wrapper->mapped_size = 0;
    *memory = (VkDeviceMemory)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkFreeMemory(VkDevice device, VkDeviceMemory memory,
             const VkAllocationCallbacks *allocator)
{
    if (!device || !memory)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_memory *wrapper =
        (struct osito_memory *)(uintptr_t)memory;
    PFN_vkFreeMemory release =
        (PFN_vkFreeMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkFreeMemory");
    if (release)
        release(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
            VkDeviceSize size, VkMemoryMapFlags flags, void **data)
{
    if (!device || !memory || !data)
        return VK_ERROR_MEMORY_MAP_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_memory *wrapper =
        (struct osito_memory *)(uintptr_t)memory;
    if (wrapper->owner != self)
        return VK_ERROR_MEMORY_MAP_FAILED;
    PFN_vkMapMemory map =
        (PFN_vkMapMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkMapMemory");
    if (!map)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult result = map(self->real, wrapper->real, offset, size, flags,
                          data);
    if (result == VK_SUCCESS) {
        wrapper->mapped_data = *data;
        wrapper->mapped_offset = offset;
        wrapper->mapped_size = size == VK_WHOLE_SIZE
            ? wrapper->allocation_size - offset : size;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL
vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    if (!device || !memory)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_memory *wrapper =
        (struct osito_memory *)(uintptr_t)memory;
    if (wrapper->owner != self)
        return;
    PFN_vkUnmapMemory unmap =
        (PFN_vkUnmapMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkUnmapMemory");
    if (unmap)
        unmap(self->real, wrapper->real);
    wrapper->mapped_data = 0;
    wrapper->mapped_offset = 0;
    wrapper->mapped_size = 0;
}

static void *
allocate_legacy_regions(uint32_t count, size_t element_size)
{
    if (!count || !element_size ||
        (size_t)count > (size_t)-1 / element_size)
        return 0;
    return malloc((size_t)count * element_size);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBuffer(VkCommandBuffer command_buffer, VkBuffer source,
                VkBuffer destination, uint32_t region_count,
                const VkBufferCopy *legacy_regions)
{
    if (!command_buffer || !source || !destination || !region_count ||
        !legacy_regions)
        return;
    VkBufferCopy2 *regions = allocate_legacy_regions(region_count,
                                                      sizeof(*regions));
    if (!regions)
        return;
    for (uint32_t i = 0; i < region_count; i++) {
        regions[i] = (VkBufferCopy2) {
            .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
            .srcOffset = legacy_regions[i].srcOffset,
            .dstOffset = legacy_regions[i].dstOffset,
            .size = legacy_regions[i].size,
        };
    }
    VkCopyBufferInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .srcBuffer = source,
        .dstBuffer = destination,
        .regionCount = region_count,
        .pRegions = regions,
    };
    vkCmdCopyBuffer2(command_buffer, &info);
    free(regions);
}

static void
convert_buffer_image_region(VkBufferImageCopy2 *region,
                            const VkBufferImageCopy *legacy)
{
    *region = (VkBufferImageCopy2) {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = legacy->bufferOffset,
        .bufferRowLength = legacy->bufferRowLength,
        .bufferImageHeight = legacy->bufferImageHeight,
        .imageSubresource = legacy->imageSubresource,
        .imageOffset = legacy->imageOffset,
        .imageExtent = legacy->imageExtent,
    };
}

VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBufferToImage(VkCommandBuffer command_buffer, VkBuffer source,
                       VkImage destination,
                       VkImageLayout destination_layout,
                       uint32_t region_count,
                       const VkBufferImageCopy *legacy_regions)
{
    if (!command_buffer || !source || !destination || !region_count ||
        !legacy_regions)
        return;
    VkBufferImageCopy2 *regions = allocate_legacy_regions(region_count,
                                                           sizeof(*regions));
    if (!regions)
        return;
    for (uint32_t i = 0; i < region_count; i++)
        convert_buffer_image_region(&regions[i], &legacy_regions[i]);
    VkCopyBufferToImageInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = source,
        .dstImage = destination,
        .dstImageLayout = destination_layout,
        .regionCount = region_count,
        .pRegions = regions,
    };
    vkCmdCopyBufferToImage2(command_buffer, &info);
    free(regions);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdCopyImageToBuffer(VkCommandBuffer command_buffer, VkImage source,
                       VkImageLayout source_layout, VkBuffer destination,
                       uint32_t region_count,
                       const VkBufferImageCopy *legacy_regions)
{
    if (!command_buffer || !source || !destination || !region_count ||
        !legacy_regions)
        return;
    VkBufferImageCopy2 *regions = allocate_legacy_regions(region_count,
                                                           sizeof(*regions));
    if (!regions)
        return;
    for (uint32_t i = 0; i < region_count; i++)
        convert_buffer_image_region(&regions[i], &legacy_regions[i]);
    VkCopyImageToBufferInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .srcImage = source,
        .srcImageLayout = source_layout,
        .dstBuffer = destination,
        .regionCount = region_count,
        .pRegions = regions,
    };
    vkCmdCopyImageToBuffer2(command_buffer, &info);
    free(regions);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier(VkCommandBuffer command_buffer,
                     VkPipelineStageFlags source_stage_mask,
                     VkPipelineStageFlags destination_stage_mask,
                     VkDependencyFlags dependency_flags,
                     uint32_t memory_barrier_count,
                     const VkMemoryBarrier *legacy_memory_barriers,
                     uint32_t buffer_barrier_count,
                     const VkBufferMemoryBarrier *legacy_buffer_barriers,
                     uint32_t image_barrier_count,
                     const VkImageMemoryBarrier *legacy_image_barriers)
{
    if (!command_buffer ||
        (memory_barrier_count && !legacy_memory_barriers) ||
        (buffer_barrier_count && !legacy_buffer_barriers) ||
        (image_barrier_count && !legacy_image_barriers))
        return;

    VkMemoryBarrier2 *memory_barriers = 0;
    VkBufferMemoryBarrier2 *buffer_barriers = 0;
    VkImageMemoryBarrier2 *image_barriers = 0;
    if (memory_barrier_count) {
        memory_barriers = allocate_legacy_regions(memory_barrier_count,
                                                   sizeof(*memory_barriers));
        if (!memory_barriers)
            goto out;
        for (uint32_t i = 0; i < memory_barrier_count; i++) {
            memory_barriers[i] = (VkMemoryBarrier2) {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = source_stage_mask,
                .srcAccessMask = legacy_memory_barriers[i].srcAccessMask,
                .dstStageMask = destination_stage_mask,
                .dstAccessMask = legacy_memory_barriers[i].dstAccessMask,
            };
        }
    }
    if (buffer_barrier_count) {
        buffer_barriers = allocate_legacy_regions(buffer_barrier_count,
                                                   sizeof(*buffer_barriers));
        if (!buffer_barriers)
            goto out;
        for (uint32_t i = 0; i < buffer_barrier_count; i++) {
            const VkBufferMemoryBarrier *legacy = &legacy_buffer_barriers[i];
            buffer_barriers[i] = (VkBufferMemoryBarrier2) {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = source_stage_mask,
                .srcAccessMask = legacy->srcAccessMask,
                .dstStageMask = destination_stage_mask,
                .dstAccessMask = legacy->dstAccessMask,
                .srcQueueFamilyIndex = legacy->srcQueueFamilyIndex,
                .dstQueueFamilyIndex = legacy->dstQueueFamilyIndex,
                .buffer = legacy->buffer,
                .offset = legacy->offset,
                .size = legacy->size,
            };
        }
    }
    if (image_barrier_count) {
        image_barriers = allocate_legacy_regions(image_barrier_count,
                                                  sizeof(*image_barriers));
        if (!image_barriers)
            goto out;
        for (uint32_t i = 0; i < image_barrier_count; i++) {
            const VkImageMemoryBarrier *legacy = &legacy_image_barriers[i];
            image_barriers[i] = (VkImageMemoryBarrier2) {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = source_stage_mask,
                .srcAccessMask = legacy->srcAccessMask,
                .dstStageMask = destination_stage_mask,
                .dstAccessMask = legacy->dstAccessMask,
                .oldLayout = legacy->oldLayout,
                .newLayout = legacy->newLayout,
                .srcQueueFamilyIndex = legacy->srcQueueFamilyIndex,
                .dstQueueFamilyIndex = legacy->dstQueueFamilyIndex,
                .image = legacy->image,
                .subresourceRange = legacy->subresourceRange,
            };
        }
    }

    VkDependencyInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .dependencyFlags = dependency_flags,
        .memoryBarrierCount = memory_barrier_count,
        .pMemoryBarriers = memory_barriers,
        .bufferMemoryBarrierCount = buffer_barrier_count,
        .pBufferMemoryBarriers = buffer_barriers,
        .imageMemoryBarrierCount = image_barrier_count,
        .pImageMemoryBarriers = image_barriers,
    };
    vkCmdPipelineBarrier2(command_buffer, &info);

out:
    free(image_barriers);
    free(buffer_barriers);
    free(memory_barriers);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBuffer2(VkCommandBuffer command_buffer,
                 const VkCopyBufferInfo2 *copy_info)
{
    if (!command_buffer || !copy_info ||
        !copy_info->srcBuffer || !copy_info->dstBuffer)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_buffer *source =
        (struct osito_buffer *)(uintptr_t)copy_info->srcBuffer;
    struct osito_buffer *destination =
        (struct osito_buffer *)(uintptr_t)copy_info->dstBuffer;
    if (source->owner != device || destination->owner != device)
        return;
    VkCopyBufferInfo2 real_info = *copy_info;
    real_info.srcBuffer = source->real;
    real_info.dstBuffer = destination->real;
    if (OSITO_VK_RENDER_DIAGNOSTICS && !destination->trace_shadow &&
        destination->size <= 4u * 1024u * 1024u) {
        destination->trace_shadow = calloc(1, (size_t)destination->size);
        if (destination->trace_shadow)
            destination->trace_shadow_size = destination->size;
    }
    if (destination->trace_shadow && copy_info->pRegions) {
        for (uint32_t i = 0; i < copy_info->regionCount; i++) {
            const VkBufferCopy2 *region = &copy_info->pRegions[i];
            const uint8_t *data = loader_trace_buffer_data(
                source, region->srcOffset, region->size);
            if (data && region->dstOffset <= destination->trace_shadow_size &&
                region->size <= destination->trace_shadow_size -
                                    region->dstOffset)
                memcpy(destination->trace_shadow + (size_t)region->dstOffset,
                       data, (size_t)region->size);
        }
    }
    PFN_vkCmdCopyBuffer2 copy =
        (PFN_vkCmdCopyBuffer2)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdCopyBuffer2");
    if (copy)
        copy(command_wrapper->real, &real_info);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier2(VkCommandBuffer command_buffer,
                      const VkDependencyInfo *dependency_info)
{
    if (!command_buffer || !dependency_info)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    VkDependencyInfo real_info = *dependency_info;
    VkBufferMemoryBarrier2 *buffer_barriers = 0;
    VkImageMemoryBarrier2 *image_barriers = 0;

    if (dependency_info->bufferMemoryBarrierCount) {
        if (!dependency_info->pBufferMemoryBarriers)
            return;
        buffer_barriers = malloc(
            (size_t)dependency_info->bufferMemoryBarrierCount *
            sizeof(*buffer_barriers));
        if (!buffer_barriers)
            return;
        memcpy(buffer_barriers, dependency_info->pBufferMemoryBarriers,
               (size_t)dependency_info->bufferMemoryBarrierCount *
               sizeof(*buffer_barriers));
        real_info.pBufferMemoryBarriers = buffer_barriers;
        for (uint32_t i = 0; i < dependency_info->bufferMemoryBarrierCount;
             i++) {
            struct osito_buffer *wrapper =
                (struct osito_buffer *)(uintptr_t)buffer_barriers[i].buffer;
            if (!wrapper || wrapper->owner != device) {
                free(buffer_barriers);
                return;
            }
            buffer_barriers[i].buffer = wrapper->real;
        }
    }

    if (dependency_info->imageMemoryBarrierCount) {
        if (!dependency_info->pImageMemoryBarriers) {
            free(buffer_barriers);
            return;
        }
        image_barriers = malloc(
            (size_t)dependency_info->imageMemoryBarrierCount *
            sizeof(*image_barriers));
        if (!image_barriers) {
            free(buffer_barriers);
            return;
        }
        memcpy(image_barriers, dependency_info->pImageMemoryBarriers,
               (size_t)dependency_info->imageMemoryBarrierCount *
               sizeof(*image_barriers));
        real_info.pImageMemoryBarriers = image_barriers;
        for (uint32_t i = 0; i < dependency_info->imageMemoryBarrierCount;
             i++) {
            struct osito_image *wrapper =
                (struct osito_image *)(uintptr_t)image_barriers[i].image;
            if (!wrapper || wrapper->owner != device) {
                free(image_barriers);
                free(buffer_barriers);
                return;
            }
            image_barriers[i].image = wrapper->real;
        }
    }

    PFN_vkCmdPipelineBarrier2 barrier =
        (PFN_vkCmdPipelineBarrier2)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdPipelineBarrier2");
    if (barrier)
        barrier(command_wrapper->real, &real_info);
    free(image_barriers);
    free(buffer_barriers);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBufferToImage2(VkCommandBuffer command_buffer,
                        const VkCopyBufferToImageInfo2 *copy_info)
{
    if (!command_buffer || !copy_info ||
        !copy_info->srcBuffer || !copy_info->dstImage)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_buffer *buffer =
        (struct osito_buffer *)(uintptr_t)copy_info->srcBuffer;
    struct osito_image *image =
        (struct osito_image *)(uintptr_t)copy_info->dstImage;
    if (buffer->owner != device || image->owner != device)
        return;
    VkCopyBufferToImageInfo2 real_info = *copy_info;
    real_info.srcBuffer = buffer->real;
    real_info.dstImage = image->real;
    loader_trace_image_upload(buffer, image, copy_info);
    loader_trace_record_image_copy(command_wrapper, buffer, image, copy_info);
    PFN_vkCmdCopyBufferToImage2 copy =
        (PFN_vkCmdCopyBufferToImage2)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdCopyBufferToImage2");
    if (copy)
        copy(command_wrapper->real, &real_info);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdCopyImageToBuffer2(VkCommandBuffer command_buffer,
                        const VkCopyImageToBufferInfo2 *copy_info)
{
    if (!command_buffer || !copy_info ||
        !copy_info->srcImage || !copy_info->dstBuffer)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_image *image =
        (struct osito_image *)(uintptr_t)copy_info->srcImage;
    struct osito_buffer *buffer =
        (struct osito_buffer *)(uintptr_t)copy_info->dstBuffer;
    if (buffer->owner != device || image->owner != device)
        return;
    VkCopyImageToBufferInfo2 real_info = *copy_info;
    real_info.srcImage = image->real;
    real_info.dstBuffer = buffer->real;
    PFN_vkCmdCopyImageToBuffer2 copy =
        (PFN_vkCmdCopyImageToBuffer2)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdCopyImageToBuffer2");
    if (copy)
        copy(command_wrapper->real, &real_info);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdClearColorImage(VkCommandBuffer command_buffer, VkImage image,
                     VkImageLayout image_layout,
                     const VkClearColorValue *color,
                     uint32_t range_count,
                     const VkImageSubresourceRange *ranges)
{
    if (!command_buffer || !image || !color || !range_count || !ranges)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_image *image_wrapper =
        (struct osito_image *)(uintptr_t)image;
    if (image_wrapper->owner != device)
        return;
    PFN_vkCmdClearColorImage clear =
        (PFN_vkCmdClearColorImage)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdClearColorImage");
    if (clear)
        clear(command_wrapper->real, image_wrapper->real, image_layout,
              color, range_count, ranges);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdClearDepthStencilImage(
    VkCommandBuffer command_buffer, VkImage image,
    VkImageLayout image_layout,
    const VkClearDepthStencilValue *depth_stencil,
    uint32_t range_count, const VkImageSubresourceRange *ranges)
{
    if (!command_buffer || !image || !depth_stencil ||
        !range_count || !ranges)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_image *image_wrapper =
        (struct osito_image *)(uintptr_t)image;
    if (image_wrapper->owner != device)
        return;
    PFN_vkCmdClearDepthStencilImage clear =
        (PFN_vkCmdClearDepthStencilImage)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdClearDepthStencilImage");
    if (clear)
        clear(command_wrapper->real, image_wrapper->real, image_layout,
              depth_stencil, range_count, ranges);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdClearAttachments(VkCommandBuffer command_buffer,
                      uint32_t attachment_count,
                      const VkClearAttachment *attachments,
                      uint32_t rect_count,
                      const VkClearRect *rects)
{
    if (!command_buffer ||
        (attachment_count && !attachments) ||
        (rect_count && !rects))
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    PFN_vkCmdClearAttachments clear =
        (PFN_vkCmdClearAttachments)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdClearAttachments");
    if (clear)
        clear(command_wrapper->real, attachment_count, attachments,
              rect_count, rects);
}

static int unwrap_rendering_attachment(
    struct osito_device *device, VkRenderingAttachmentInfo *destination,
    const VkRenderingAttachmentInfo *source)
{
    *destination = *source;
    if (source->imageView) {
        struct osito_image_view *view =
            (struct osito_image_view *)(uintptr_t)source->imageView;
        if (view->owner != device)
            return -1;
        destination->imageView = view->real;
    }
    if (source->resolveImageView) {
        struct osito_image_view *view =
            (struct osito_image_view *)(uintptr_t)source->resolveImageView;
        if (view->owner != device)
            return -1;
        destination->resolveImageView = view->real;
    }
    return 0;
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRendering(VkCommandBuffer command_buffer,
                    const VkRenderingInfo *rendering_info)
{
    if (!command_buffer || !rendering_info ||
        (rendering_info->colorAttachmentCount &&
         !rendering_info->pColorAttachments))
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    VkRenderingAttachmentInfo *colors = 0;
    if (rendering_info->colorAttachmentCount) {
        colors = malloc(sizeof(*colors) *
                        rendering_info->colorAttachmentCount);
        if (!colors)
            return;
        for (uint32_t i = 0; i < rendering_info->colorAttachmentCount; i++) {
            if (unwrap_rendering_attachment(
                    device, &colors[i],
                    &rendering_info->pColorAttachments[i]) < 0) {
                free(colors);
                return;
            }
        }
    }
    VkRenderingAttachmentInfo depth;
    VkRenderingAttachmentInfo stencil;
    VkRenderingInfo real_info = *rendering_info;
    real_info.pColorAttachments = colors;
    if (rendering_info->pDepthAttachment) {
        if (unwrap_rendering_attachment(
                device, &depth, rendering_info->pDepthAttachment) < 0) {
            free(colors);
            return;
        }
        real_info.pDepthAttachment = &depth;
    }
    if (rendering_info->pStencilAttachment) {
        if (unwrap_rendering_attachment(
                device, &stencil, rendering_info->pStencilAttachment) < 0) {
            free(colors);
            return;
        }
        real_info.pStencilAttachment = &stencil;
    }
    PFN_vkCmdBeginRendering begin =
        (PFN_vkCmdBeginRendering)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdBeginRendering");
    if (begin)
        begin(command_wrapper->real, &real_info);
    free(colors);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdEndRendering(VkCommandBuffer command_buffer)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    PFN_vkCmdEndRendering end =
        (PFN_vkCmdEndRendering)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdEndRendering");
    if (end)
        end(command_wrapper->real);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindVertexBuffers(VkCommandBuffer command_buffer,
                       uint32_t first_binding, uint32_t binding_count,
                       const VkBuffer *buffers,
                       const VkDeviceSize *offsets)
{
    if (!command_buffer || !binding_count || !buffers || !offsets)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    VkBuffer *real_buffers = malloc(sizeof(*real_buffers) * binding_count);
    if (!real_buffers)
        return;
    for (uint32_t i = 0; i < binding_count; i++) {
        uint32_t binding_index = first_binding + i;
        if (!buffers[i]) {
            real_buffers[i] = VK_NULL_HANDLE;
            if (binding_index < 4) {
                command_wrapper->trace_vertex_buffers[binding_index] = 0;
                command_wrapper->trace_vertex_offsets[binding_index] = 0;
                command_wrapper->trace_vertex_strides[binding_index] = 0;
            }
            continue;
        }
        struct osito_buffer *buffer =
            (struct osito_buffer *)(uintptr_t)buffers[i];
        if (buffer->owner != device) {
            free(real_buffers);
            return;
        }
        real_buffers[i] = buffer->real;
        if (binding_index < 4) {
            command_wrapper->trace_vertex_buffers[binding_index] = buffer;
            command_wrapper->trace_vertex_offsets[binding_index] = offsets[i];
            command_wrapper->trace_vertex_strides[binding_index] = 0;
        }
    }
    PFN_vkCmdBindVertexBuffers bind =
        (PFN_vkCmdBindVertexBuffers)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdBindVertexBuffers");
    if (bind)
        bind(command_wrapper->real, first_binding, binding_count,
             real_buffers, offsets);
    free(real_buffers);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindVertexBuffers2(VkCommandBuffer command_buffer,
                        uint32_t first_binding, uint32_t binding_count,
                        const VkBuffer *buffers,
                        const VkDeviceSize *offsets,
                        const VkDeviceSize *sizes,
                        const VkDeviceSize *strides)
{
    if (!command_buffer || !binding_count || !buffers || !offsets)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    VkBuffer *real_buffers = malloc(sizeof(*real_buffers) * binding_count);
    if (!real_buffers)
        return;
    for (uint32_t i = 0; i < binding_count; i++) {
        uint32_t binding_index = first_binding + i;
        if (!buffers[i]) {
            real_buffers[i] = VK_NULL_HANDLE;
            if (binding_index < 4) {
                command_wrapper->trace_vertex_buffers[binding_index] = 0;
                command_wrapper->trace_vertex_offsets[binding_index] = 0;
                command_wrapper->trace_vertex_strides[binding_index] = 0;
            }
            continue;
        }
        struct osito_buffer *buffer =
            (struct osito_buffer *)(uintptr_t)buffers[i];
        if (buffer->owner != device) {
            free(real_buffers);
            return;
        }
        real_buffers[i] = buffer->real;
        if (binding_index < 4) {
            command_wrapper->trace_vertex_buffers[binding_index] = buffer;
            command_wrapper->trace_vertex_offsets[binding_index] = offsets[i];
            command_wrapper->trace_vertex_strides[binding_index] = strides
                ? strides[i] : 0;
        }
    }
    PFN_vkCmdBindVertexBuffers2 bind =
        (PFN_vkCmdBindVertexBuffers2)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdBindVertexBuffers2");
    if (bind)
        bind(command_wrapper->real, first_binding, binding_count,
             real_buffers, offsets, sizes, strides);
    free(real_buffers);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindIndexBuffer(VkCommandBuffer command_buffer, VkBuffer buffer,
                     VkDeviceSize offset, VkIndexType index_type)
{
    if (!command_buffer || !buffer)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_buffer *buffer_wrapper =
        (struct osito_buffer *)(uintptr_t)buffer;
    if (buffer_wrapper->owner != device)
        return;
    command_wrapper->trace_index_buffer = buffer_wrapper;
    command_wrapper->trace_index_offset = offset;
    command_wrapper->trace_index_type = index_type;
    PFN_vkCmdBindIndexBuffer bind =
        (PFN_vkCmdBindIndexBuffer)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdBindIndexBuffer");
    if (bind)
        bind(command_wrapper->real, buffer_wrapper->real, offset, index_type);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindIndexBuffer2KHR(VkCommandBuffer command_buffer, VkBuffer buffer,
                         VkDeviceSize offset, VkDeviceSize size,
                         VkIndexType index_type)
{
    if (!command_buffer || !buffer)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_buffer *buffer_wrapper =
        (struct osito_buffer *)(uintptr_t)buffer;
    if (buffer_wrapper->owner != device)
        return;
    command_wrapper->trace_index_buffer = buffer_wrapper;
    command_wrapper->trace_index_offset = offset;
    command_wrapper->trace_index_type = index_type;
    PFN_vkCmdBindIndexBuffer2KHR bind =
        (PFN_vkCmdBindIndexBuffer2KHR)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdBindIndexBuffer2KHR");
    if (!bind)
        bind = (PFN_vkCmdBindIndexBuffer2KHR)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdBindIndexBuffer2");
    if (bind)
        bind(command_wrapper->real, buffer_wrapper->real,
             offset, size, index_type);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindIndexBuffer2(VkCommandBuffer command_buffer, VkBuffer buffer,
                      VkDeviceSize offset, VkDeviceSize size,
                      VkIndexType index_type)
{
    vkCmdBindIndexBuffer2KHR(
        command_buffer, buffer, offset, size, index_type);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindPipeline(VkCommandBuffer command_buffer,
                  VkPipelineBindPoint bind_point, VkPipeline pipeline)
{
    if (!command_buffer || !pipeline)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_pipeline *pipeline_wrapper =
        (struct osito_pipeline *)(uintptr_t)pipeline;
    if (pipeline_wrapper->owner != device)
        return;
    PFN_vkCmdBindPipeline bind =
        (PFN_vkCmdBindPipeline)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdBindPipeline");
    if (bind)
        bind(command_wrapper->real, bind_point, pipeline_wrapper->real);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetAttachmentFeedbackLoopEnableEXT(VkCommandBuffer command_buffer,
                                        VkImageAspectFlags aspect_mask)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetAttachmentFeedbackLoopEnableEXT set =
        (PFN_vkCmdSetAttachmentFeedbackLoopEnableEXT)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle,
                "vkCmdSetAttachmentFeedbackLoopEnableEXT");
    if (set)
        set(wrapper->real, aspect_mask);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetCullMode(VkCommandBuffer command_buffer, VkCullModeFlags cull_mode)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetCullMode set =
        (PFN_vkCmdSetCullMode)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdSetCullMode");
    if (set)
        set(wrapper->real, cull_mode);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetFrontFace(VkCommandBuffer command_buffer, VkFrontFace front_face)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetFrontFace set =
        (PFN_vkCmdSetFrontFace)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdSetFrontFace");
    if (set)
        set(wrapper->real, front_face);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetPrimitiveTopology(VkCommandBuffer command_buffer,
                          VkPrimitiveTopology topology)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetPrimitiveTopology set =
        (PFN_vkCmdSetPrimitiveTopology)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle,
                "vkCmdSetPrimitiveTopology");
    if (set)
        set(wrapper->real, topology);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetViewport(VkCommandBuffer command_buffer, uint32_t first_viewport,
                 uint32_t viewport_count, const VkViewport *viewports)
{
    if (!command_buffer || !viewport_count || !viewports)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetViewport set =
        (PFN_vkCmdSetViewport)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdSetViewport");
    if (set)
        set(wrapper->real, first_viewport, viewport_count, viewports);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetScissor(VkCommandBuffer command_buffer, uint32_t first_scissor,
                uint32_t scissor_count, const VkRect2D *scissors)
{
    if (!command_buffer || !scissor_count || !scissors)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetScissor set =
        (PFN_vkCmdSetScissor)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdSetScissor");
    if (set)
        set(wrapper->real, first_scissor, scissor_count, scissors);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetLineWidth(VkCommandBuffer command_buffer, float line_width)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetLineWidth set =
        (PFN_vkCmdSetLineWidth)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdSetLineWidth");
    if (set)
        set(wrapper->real, line_width);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetDepthBias(VkCommandBuffer command_buffer,
                  float constant_factor, float clamp,
                  float slope_factor)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetDepthBias set =
        (PFN_vkCmdSetDepthBias)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdSetDepthBias");
    if (set)
        set(wrapper->real, constant_factor, clamp, slope_factor);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetBlendConstants(VkCommandBuffer command_buffer,
                       const float blend_constants[4])
{
    if (!command_buffer || !blend_constants)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetBlendConstants set =
        (PFN_vkCmdSetBlendConstants)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle, "vkCmdSetBlendConstants");
    if (set)
        set(wrapper->real, blend_constants);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetDepthBounds(VkCommandBuffer command_buffer,
                    float min_depth_bounds, float max_depth_bounds)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetDepthBounds set =
        (PFN_vkCmdSetDepthBounds)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdSetDepthBounds");
    if (set)
        set(wrapper->real, min_depth_bounds, max_depth_bounds);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetStencilCompareMask(VkCommandBuffer command_buffer,
                           VkStencilFaceFlags face_mask,
                           uint32_t compare_mask)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetStencilCompareMask set =
        (PFN_vkCmdSetStencilCompareMask)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle,
                "vkCmdSetStencilCompareMask");
    if (set)
        set(wrapper->real, face_mask, compare_mask);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetStencilWriteMask(VkCommandBuffer command_buffer,
                         VkStencilFaceFlags face_mask,
                         uint32_t write_mask)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetStencilWriteMask set =
        (PFN_vkCmdSetStencilWriteMask)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle,
                "vkCmdSetStencilWriteMask");
    if (set)
        set(wrapper->real, face_mask, write_mask);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetStencilReference(VkCommandBuffer command_buffer,
                         VkStencilFaceFlags face_mask,
                         uint32_t reference)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetStencilReference set =
        (PFN_vkCmdSetStencilReference)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle,
                "vkCmdSetStencilReference");
    if (set)
        set(wrapper->real, face_mask, reference);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetViewportWithCount(VkCommandBuffer command_buffer,
                          uint32_t viewport_count,
                          const VkViewport *viewports)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetViewportWithCount set =
        (PFN_vkCmdSetViewportWithCount)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle, "vkCmdSetViewportWithCount");
    if (set)
        set(wrapper->real, viewport_count, viewports);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetScissorWithCount(VkCommandBuffer command_buffer,
                         uint32_t scissor_count, const VkRect2D *scissors)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetScissorWithCount set =
        (PFN_vkCmdSetScissorWithCount)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle, "vkCmdSetScissorWithCount");
    if (set)
        set(wrapper->real, scissor_count, scissors);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdDraw(VkCommandBuffer command_buffer, uint32_t vertex_count,
          uint32_t instance_count, uint32_t first_vertex,
          uint32_t first_instance)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    loader_trace_descriptor_draw(wrapper, vertex_count, 0,
                                 first_vertex, 0);
    PFN_vkCmdDraw draw =
        (PFN_vkCmdDraw)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdDraw");
    if (draw)
        draw(wrapper->real, vertex_count, instance_count,
             first_vertex, first_instance);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndexed(VkCommandBuffer command_buffer,
                 uint32_t index_count, uint32_t instance_count,
                 uint32_t first_index, int32_t vertex_offset,
                 uint32_t first_instance)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    loader_trace_descriptor_draw(wrapper, index_count, 1,
                                 first_index, vertex_offset);
    PFN_vkCmdDrawIndexed draw =
        (PFN_vkCmdDrawIndexed)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdDrawIndexed");
    if (draw)
        draw(wrapper->real, index_count, instance_count,
             first_index, vertex_offset, first_instance);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindDescriptorSets(
    VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
    VkPipelineLayout layout, uint32_t first_set,
    uint32_t descriptor_set_count, const VkDescriptorSet *descriptor_sets,
    uint32_t dynamic_offset_count, const uint32_t *dynamic_offsets)
{
    if (!command_buffer || !layout ||
        (descriptor_set_count && !descriptor_sets))
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_pipeline_layout *layout_wrapper =
        (struct osito_pipeline_layout *)(uintptr_t)layout;
    if (layout_wrapper->owner != device)
        return;
    VkDescriptorSet *real_sets = descriptor_set_count
        ? malloc(sizeof(*real_sets) * descriptor_set_count) : 0;
    if (descriptor_set_count && !real_sets)
        return;
    for (uint32_t i = 0; i < descriptor_set_count; i++) {
        struct osito_descriptor_set *set =
            (struct osito_descriptor_set *)(uintptr_t)descriptor_sets[i];
        if (!set || set->owner != device) {
            free(real_sets);
            return;
        }
        real_sets[i] = set->real;
    }
    if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS && first_set <= 2 &&
        first_set + descriptor_set_count > 2) {
        command_wrapper->trace_graphics_set2 =
            (struct osito_descriptor_set *)(uintptr_t)
                descriptor_sets[2 - first_set];
    }
    static uint32_t sampler_bind_traces;
    if (OSITO_VK_RENDER_DIAGNOSTICS && first_set == 2 &&
        descriptor_set_count == 1) {
        struct osito_descriptor_set *set =
            (struct osito_descriptor_set *)(uintptr_t)descriptor_sets[0];
        if (set->image_generation != set->logged_image_generation &&
            sampler_bind_traces < 320) {
            sampler_bind_traces++;
            set->logged_image_generation = set->image_generation;
            okgl_diag3("[VK-DESC-IMG]", (uint64_t)set->real,
                       (uint64_t)set->image_view,
                       (uint64_t)set->image);
            okgl_diag3("[VK-DESC-DIM]", (uint64_t)set->real,
                       ((uint64_t)set->image_width << 32) |
                           set->image_height,
                       ((uint64_t)set->image_binding << 32) |
                           set->image_generation);
        }
    }
    PFN_vkCmdBindDescriptorSets bind =
        (PFN_vkCmdBindDescriptorSets)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdBindDescriptorSets");
    if (bind)
        bind(command_wrapper->real, bind_point, layout_wrapper->real,
             first_set, descriptor_set_count, real_sets,
             dynamic_offset_count, dynamic_offsets);
    free(real_sets);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdPushConstants(VkCommandBuffer command_buffer,
                   VkPipelineLayout layout,
                   VkShaderStageFlags stage_flags,
                   uint32_t offset, uint32_t size,
                   const void *values)
{
    if (!command_buffer || !layout || !size || !values)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_pipeline_layout *layout_wrapper =
        (struct osito_pipeline_layout *)(uintptr_t)layout;
    if (layout_wrapper->owner != device)
        return;
    PFN_vkCmdPushConstants push =
        (PFN_vkCmdPushConstants)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdPushConstants");
    if (push)
        push(command_wrapper->real, layout_wrapper->real,
             stage_flags, offset, size, values);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(VkDevice device, const VkImageCreateInfo *create_info,
              const VkAllocationCallbacks *allocator, VkImage *image)
{
    if (!device || !create_info || !image)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateImage create =
        (PFN_vkCreateImage)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateImage");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkImage real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_image *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyImage destroy =
            (PFN_vkDestroyImage)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyImage");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    wrapper->width = create_info->extent.width;
    wrapper->height = create_info->extent.height;
    wrapper->format = create_info->format;
    wrapper->usage = create_info->usage;
    wrapper->memory = 0;
    wrapper->memory_offset = 0;
    wrapper->memory_size = 0;
    wrapper->memory_alignment = 0;
    wrapper->trace_next = 0;
    wrapper->trace_staging_memory_type = ~0u;
    wrapper->trace_dashboard = 0;
    loader_trace_image_add(wrapper);
    *image = (VkImage)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyImage(VkDevice device, VkImage image,
               const VkAllocationCallbacks *allocator)
{
    if (!device || !image)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_image *wrapper =
        (struct osito_image *)(uintptr_t)image;
    PFN_vkDestroyImage destroy =
        (PFN_vkDestroyImage)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyImage");
    loader_trace_image_remove(wrapper);
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements(VkDevice device, VkImage image,
                             VkMemoryRequirements *requirements)
{
    if (!device || !image || !requirements)
        return;
    memset(requirements, 0, sizeof(*requirements));
    struct osito_device *self = osito_device_from(device);
    struct osito_image *wrapper =
        (struct osito_image *)(uintptr_t)image;
    PFN_vkGetImageMemoryRequirements get =
        (PFN_vkGetImageMemoryRequirements)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkGetImageMemoryRequirements");
    if (get) {
        get(self->real, wrapper->real, requirements);
        wrapper->memory_size = requirements->size;
        wrapper->memory_alignment = requirements->alignment;
    }
}

VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements2(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements)
{
    if (!device || !info || !requirements || !info->image)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_image *wrapper =
        (struct osito_image *)(uintptr_t)info->image;
    VkImageMemoryRequirementsInfo2 real_info = *info;
    real_info.image = wrapper->real;
    PFN_vkGetImageMemoryRequirements2 get =
        (PFN_vkGetImageMemoryRequirements2)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkGetImageMemoryRequirements2");
    if (get) {
        get(self->real, &real_info, requirements);
        wrapper->memory_size = requirements->memoryRequirements.size;
        wrapper->memory_alignment = requirements->memoryRequirements.alignment;
        return;
    }
    vkGetImageMemoryRequirements(device, info->image,
                                 &requirements->memoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                  VkDeviceSize memory_offset)
{
    if (!device || !image || !memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_image *image_wrapper =
        (struct osito_image *)(uintptr_t)image;
    struct osito_memory *memory_wrapper =
        (struct osito_memory *)(uintptr_t)memory;
    if (image_wrapper->owner != self || memory_wrapper->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkBindImageMemory bind =
        (PFN_vkBindImageMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkBindImageMemory");
    if (!bind)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult result = bind(self->real, image_wrapper->real,
                           memory_wrapper->real, memory_offset);
    if (result == VK_SUCCESS)
        loader_trace_image_bind(image_wrapper, memory_wrapper, memory_offset);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(VkDevice device,
                  const VkImageViewCreateInfo *create_info,
                  const VkAllocationCallbacks *allocator,
                  VkImageView *view)
{
    if (!device || !create_info || !create_info->image || !view)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_image *image =
        (struct osito_image *)(uintptr_t)create_info->image;
    if (image->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkImageViewCreateInfo real_info = *create_info;
    real_info.image = image->real;
    PFN_vkCreateImageView create =
        (PFN_vkCreateImageView)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateImageView");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkImageView real = VK_NULL_HANDLE;
    VkResult result = create(self->real, &real_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_image_view *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyImageView destroy =
            (PFN_vkDestroyImageView)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyImageView");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    wrapper->image = image->real;
    wrapper->image_wrapper = image;
    wrapper->width = image->width;
    wrapper->height = image->height;
    *view = (VkImageView)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyImageView(VkDevice device, VkImageView view,
                   const VkAllocationCallbacks *allocator)
{
    if (!device || !view)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_image_view *wrapper =
        (struct osito_image_view *)(uintptr_t)view;
    PFN_vkDestroyImageView destroy =
        (PFN_vkDestroyImageView)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyImageView");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *create_info,
                const VkAllocationCallbacks *allocator, VkSampler *sampler)
{
    if (!device || !create_info || !sampler)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateSampler create =
        (PFN_vkCreateSampler)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateSampler");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSampler real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_sampler *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroySampler destroy =
            (PFN_vkDestroySampler)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroySampler");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *sampler = (VkSampler)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySampler(VkDevice device, VkSampler sampler,
                 const VkAllocationCallbacks *allocator)
{
    if (!device || !sampler)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_sampler *wrapper =
        (struct osito_sampler *)(uintptr_t)sampler;
    PFN_vkDestroySampler destroy =
        (PFN_vkDestroySampler)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroySampler");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateShaderModule(VkDevice device,
                     const VkShaderModuleCreateInfo *create_info,
                     const VkAllocationCallbacks *allocator,
                     VkShaderModule *shader_module)
{
    if (!device || !create_info || !shader_module)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateShaderModule create =
        (PFN_vkCreateShaderModule)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateShaderModule");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkShaderModule real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_shader *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyShaderModule destroy =
            (PFN_vkDestroyShaderModule)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyShaderModule");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *shader_module = (VkShaderModule)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyShaderModule(VkDevice device, VkShaderModule shader_module,
                      const VkAllocationCallbacks *allocator)
{
    if (!device || !shader_module)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_shader *wrapper =
        (struct osito_shader *)(uintptr_t)shader_module;
    PFN_vkDestroyShaderModule destroy =
        (PFN_vkDestroyShaderModule)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyShaderModule");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipeline_cache,
    uint32_t create_info_count,
    const VkGraphicsPipelineCreateInfo *create_infos,
    const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
    okgl_trace("[OKGL-VKPIPE] loader-enter\n");
    if (!device || pipeline_cache || create_info_count != 1 ||
        !create_infos || !pipelines) {
        printf("[VK loader pipeline] invalid args cache=%llu count=%u infos=%u out=%u\n",
               (unsigned long long)pipeline_cache, create_info_count,
               !!create_infos, !!pipelines);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct osito_device *self = osito_device_from(device);
    const VkGraphicsPipelineCreateInfo *source = &create_infos[0];
    if (!source->layout || (source->stageCount && !source->pStages)) {
        printf("[VK loader pipeline] missing layout/stages layout=%llu count=%u stages=%u\n",
               (unsigned long long)source->layout, source->stageCount,
               !!source->pStages);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkPipelineShaderStageCreateInfo *stages = 0;
    if (source->stageCount) {
        stages = malloc(sizeof(*stages) * source->stageCount);
        if (!stages)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(stages, source->pStages,
               sizeof(*stages) * source->stageCount);
        for (uint32_t i = 0; i < source->stageCount; i++) {
            if (!stages[i].module)
                continue;
            struct osito_shader *shader =
                (struct osito_shader *)(uintptr_t)stages[i].module;
            if (shader->owner != self) {
                printf("[VK loader pipeline] shader owner mismatch stage=%u\n", i);
                free(stages);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            stages[i].module = shader->real;
        }
    }
    VkGraphicsPipelineCreateInfo real_info = *source;
    real_info.pStages = stages;
    struct osito_pipeline_layout *layout =
        (struct osito_pipeline_layout *)(uintptr_t)source->layout;
    if (layout->owner != self) {
        printf("[VK loader pipeline] layout owner mismatch\n");
        free(stages);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    real_info.layout = layout->real;
    if (source->renderPass) {
        struct osito_render_pass *render_pass =
            (struct osito_render_pass *)(uintptr_t)source->renderPass;
        if (render_pass->owner != self) {
            free(stages);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        real_info.renderPass = render_pass->real;
    }
    if (source->basePipelineHandle) {
        struct osito_pipeline *base =
            (struct osito_pipeline *)(uintptr_t)source->basePipelineHandle;
        if (base->owner != self) {
            free(stages);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        real_info.basePipelineHandle = base->real;
    }
    PFN_vkCreateGraphicsPipelines create =
        (PFN_vkCreateGraphicsPipelines)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateGraphicsPipelines");
    if (!create) {
        okgl_trace("[OKGL-VKPIPE] loader-icd-proc-missing\n");
        free(stages);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkPipeline real = VK_NULL_HANDLE;
    VkResult result = create(self->real, VK_NULL_HANDLE, 1,
                             &real_info, allocator, &real);
    okgl_trace(result == VK_SUCCESS
                   ? "[OKGL-VKPIPE] loader-icd-success\n"
                   : "[OKGL-VKPIPE] loader-icd-failed\n");
    free(stages);
    if (result != VK_SUCCESS)
        return result;
    struct osito_pipeline *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyPipeline destroy =
            (PFN_vkDestroyPipeline)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyPipeline");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    pipelines[0] = (VkPipeline)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateComputePipelines(
    VkDevice device, VkPipelineCache pipeline_cache,
    uint32_t create_info_count,
    const VkComputePipelineCreateInfo *create_infos,
    const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
    if (!device || pipeline_cache || create_info_count != 1 ||
        !create_infos || !pipelines)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    const VkComputePipelineCreateInfo *source = &create_infos[0];
    if (!source->stage.module || !source->layout)
        return VK_ERROR_INITIALIZATION_FAILED;

    struct osito_shader *shader =
        (struct osito_shader *)(uintptr_t)source->stage.module;
    struct osito_pipeline_layout *layout =
        (struct osito_pipeline_layout *)(uintptr_t)source->layout;
    if (shader->owner != self || layout->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkComputePipelineCreateInfo real_info = *source;
    real_info.stage.module = shader->real;
    real_info.layout = layout->real;
    if (source->basePipelineHandle) {
        struct osito_pipeline *base =
            (struct osito_pipeline *)(uintptr_t)source->basePipelineHandle;
        if (base->owner != self)
            return VK_ERROR_INITIALIZATION_FAILED;
        real_info.basePipelineHandle = base->real;
    }

    PFN_vkCreateComputePipelines create =
        (PFN_vkCreateComputePipelines)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateComputePipelines");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkPipeline real = VK_NULL_HANDLE;
    VkResult result = create(self->real, VK_NULL_HANDLE, 1,
                             &real_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;

    struct osito_pipeline *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyPipeline destroy =
            (PFN_vkDestroyPipeline)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyPipeline");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    pipelines[0] = (VkPipeline)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyPipeline(VkDevice device, VkPipeline pipeline,
                  const VkAllocationCallbacks *allocator)
{
    if (!device || !pipeline)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_pipeline *wrapper =
        (struct osito_pipeline *)(uintptr_t)pipeline;
    PFN_vkDestroyPipeline destroy =
        (PFN_vkDestroyPipeline)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyPipeline");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkDescriptorPool *pool)
{
    if (!device || !create_info || !pool)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateDescriptorPool create =
        (PFN_vkCreateDescriptorPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateDescriptorPool");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDescriptorPool real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_descriptor_pool *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyDescriptorPool destroy =
            (PFN_vkDestroyDescriptorPool)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyDescriptorPool");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *pool = (VkDescriptorPool)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool pool,
                        const VkAllocationCallbacks *allocator)
{
    if (!device || !pool)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_descriptor_pool *wrapper =
        (struct osito_descriptor_pool *)(uintptr_t)pool;
    PFN_vkDestroyDescriptorPool destroy =
        (PFN_vkDestroyDescriptorPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyDescriptorPool");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkResetDescriptorPool(VkDevice device, VkDescriptorPool pool,
                      VkDescriptorPoolResetFlags flags)
{
    if (!device || !pool)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_descriptor_pool *wrapper =
        (struct osito_descriptor_pool *)(uintptr_t)pool;
    if (wrapper->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkResetDescriptorPool reset =
        (PFN_vkResetDescriptorPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkResetDescriptorPool");
    return reset ? reset(self->real, wrapper->real, flags)
                 : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *allocate_info,
    VkDescriptorSet *sets)
{
    if (!device || !allocate_info || !allocate_info->descriptorPool ||
        !allocate_info->descriptorSetCount || !sets)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_descriptor_pool *pool =
        (struct osito_descriptor_pool *)(uintptr_t)
            allocate_info->descriptorPool;
    if (pool->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkDescriptorSetAllocateInfo real_info = *allocate_info;
    real_info.descriptorPool = pool->real;
    VkDescriptorSet *real_sets =
        malloc(sizeof(*real_sets) * allocate_info->descriptorSetCount);
    if (!real_sets)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    PFN_vkAllocateDescriptorSets allocate =
        (PFN_vkAllocateDescriptorSets)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkAllocateDescriptorSets");
    if (!allocate) {
        free(real_sets);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkResult result = allocate(self->real, &real_info, real_sets);
    if (result != VK_SUCCESS) {
        free(real_sets);
        return result;
    }
    for (uint32_t i = 0; i < allocate_info->descriptorSetCount; i++) {
        struct osito_descriptor_set *wrapper = malloc(sizeof(*wrapper));
        if (!wrapper) {
            free(real_sets);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        memset(wrapper, 0, sizeof(*wrapper));
        wrapper->owner = self;
        wrapper->real = real_sets[i];
        sets[i] = (VkDescriptorSet)(uintptr_t)wrapper;
    }
    free(real_sets);
    return VK_SUCCESS;
}

struct osito_descriptor_write_batch {
    uint32_t count;
    VkWriteDescriptorSet *writes;
    void **payloads;
};

static void
osito_descriptor_write_batch_finish(struct osito_descriptor_write_batch *batch)
{
    if (!batch)
        return;
    for (uint32_t i = 0; i < batch->count; i++)
        free(batch->payloads ? batch->payloads[i] : 0);
    free(batch->payloads);
    free(batch->writes);
    memset(batch, 0, sizeof(*batch));
}

static int
osito_descriptor_write_batch_init(
    struct osito_device *self, uint32_t write_count,
    const VkWriteDescriptorSet *writes, int allow_null_dst_set,
    struct osito_descriptor_write_batch *batch)
{
    memset(batch, 0, sizeof(*batch));
    batch->count = write_count;
    if (!write_count)
        return 1;
    batch->writes = malloc(sizeof(*batch->writes) * write_count);
    batch->payloads = malloc(sizeof(*batch->payloads) * write_count);
    if (!batch->writes || !batch->payloads)
        goto fail;
    memset(batch->payloads, 0, sizeof(*batch->payloads) * write_count);

    for (uint32_t i = 0; i < write_count; i++) {
        if (writes[i].sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET)
            goto fail;
        batch->writes[i] = writes[i];
        if (writes[i].dstSet) {
            struct osito_descriptor_set *set =
                (struct osito_descriptor_set *)(uintptr_t)writes[i].dstSet;
            if (set->owner != self)
                goto fail;
            batch->writes[i].dstSet = set->real;
        } else if (!allow_null_dst_set) {
            goto fail;
        }
        batch->writes[i].pImageInfo = 0;
        batch->writes[i].pBufferInfo = 0;
        batch->writes[i].pTexelBufferView = 0;

        uint32_t count = writes[i].descriptorCount;
        switch (writes[i].descriptorType) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
            if (count && !writes[i].pBufferInfo)
                goto fail;
            VkDescriptorBufferInfo *infos =
                count ? malloc(sizeof(*infos) * count) : 0;
            if (count && !infos)
                goto fail;
            batch->payloads[i] = infos;
            if (count)
                memcpy(infos, writes[i].pBufferInfo, sizeof(*infos) * count);
            for (uint32_t j = 0; j < count; j++) {
                if (!infos[j].buffer)
                    continue;
                struct osito_buffer *buffer =
                    (struct osito_buffer *)(uintptr_t)infos[j].buffer;
                if (buffer->owner != self)
                    goto fail;
                infos[j].buffer = buffer->real;
            }
            batch->writes[i].pBufferInfo = infos;
            break;
        }
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
            if (count && !writes[i].pImageInfo)
                goto fail;
            VkDescriptorImageInfo *infos =
                count ? malloc(sizeof(*infos) * count) : 0;
            if (count && !infos)
                goto fail;
            batch->payloads[i] = infos;
            if (count)
                memcpy(infos, writes[i].pImageInfo, sizeof(*infos) * count);
            for (uint32_t j = 0; j < count; j++) {
                if (infos[j].sampler) {
                    struct osito_sampler *sampler =
                        (struct osito_sampler *)(uintptr_t)infos[j].sampler;
                    if (sampler->owner != self)
                        goto fail;
                    infos[j].sampler = sampler->real;
                }
                if (infos[j].imageView) {
                    struct osito_image_view *view =
                        (struct osito_image_view *)(uintptr_t)
                            infos[j].imageView;
                    if (view->owner != self)
                        goto fail;
                    static uint32_t atlas_update_traces;
                    if (OSITO_VK_RENDER_DIAGNOSTICS &&
                        atlas_update_traces < 320 &&
                        ((view->width == 128 && view->height == 64) ||
                         (view->width >= 1024 && view->height <= 64))) {
                        atlas_update_traces++;
                        okgl_diag3("[VK-DESC-ATLAS]",
                                   (uint64_t)batch->writes[i].dstSet,
                                   (uint64_t)view->real,
                                   ((uint64_t)view->width << 32) |
                                       view->height);
                    }
                    infos[j].imageView = view->real;
                }
            }
            batch->writes[i].pImageInfo = infos;
            break;
        }
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
            if (count && !writes[i].pTexelBufferView)
                goto fail;
            VkBufferView *views = count ? malloc(sizeof(*views) * count) : 0;
            if (count && !views)
                goto fail;
            batch->payloads[i] = views;
            for (uint32_t j = 0; j < count; j++) {
                if (!writes[i].pTexelBufferView[j]) {
                    views[j] = VK_NULL_HANDLE;
                    continue;
                }
                struct osito_buffer_view *view =
                    (struct osito_buffer_view *)(uintptr_t)
                        writes[i].pTexelBufferView[j];
                if (view->owner != self)
                    goto fail;
                views[j] = view->real;
            }
            batch->writes[i].pTexelBufferView = views;
            break;
        }
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            if (!writes[i].pNext)
                goto fail;
            break;
        default:
            goto fail;
        }
    }
    return 1;

fail:
    osito_descriptor_write_batch_finish(batch);
    return 0;
}

static void
osito_descriptor_set_shadow_images(
    uint32_t write_count, const VkWriteDescriptorSet *writes)
{
    for (uint32_t i = 0; i < write_count; i++) {
        if (!writes[i].dstSet || !writes[i].pImageInfo)
            continue;
        switch (writes[i].descriptorType) {
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            break;
        default:
            continue;
        }

        struct osito_descriptor_set *set =
            (struct osito_descriptor_set *)(uintptr_t)writes[i].dstSet;
        for (uint32_t j = 0; j < writes[i].descriptorCount; j++) {
            const VkDescriptorImageInfo *info = &writes[i].pImageInfo[j];
            if (!info->imageView)
                continue;
            struct osito_image_view *view =
                (struct osito_image_view *)(uintptr_t)info->imageView;
            set->image_view = view->real;
            set->image = view->image;
            set->sampler = info->sampler
                ? ((struct osito_sampler *)(uintptr_t)info->sampler)->real
                : VK_NULL_HANDLE;
            set->image_layout = info->imageLayout;
            set->trace_image = view->image_wrapper;
            set->image_width = view->width;
            set->image_height = view->height;
            set->image_binding = writes[i].dstBinding;
            set->image_array_element = writes[i].dstArrayElement + j;
            uint32_t generation =
                loader_descriptor_generation_next(set);
            if (loader_trace_image_dimensions(view->width, view->height) &&
                loader_trace_take(&descriptor_update_trace_count, 480)) {
                okgl_diag3("[VK-DESC-UPD]", (uint64_t)set->real,
                           (uint64_t)view->real, (uint64_t)view->image);
                okgl_diag3("[VK-DESC-UGEN]",
                           ((uint64_t)view->width << 32) | view->height,
                           ((uint64_t)writes[i].dstBinding << 32) |
                               (writes[i].dstArrayElement + j),
                           generation);
            }
        }
    }
}

VKAPI_ATTR void VKAPI_CALL
vkUpdateDescriptorSets(VkDevice device, uint32_t write_count,
                       const VkWriteDescriptorSet *writes,
                       uint32_t copy_count,
                       const VkCopyDescriptorSet *copies)
{
    if (!device || (write_count && !writes) || copy_count || copies)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_descriptor_write_batch batch;
    if (!osito_descriptor_write_batch_init(
            self, write_count, writes, 0, &batch))
        return;
    PFN_vkUpdateDescriptorSets update =
        (PFN_vkUpdateDescriptorSets)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkUpdateDescriptorSets");
    if (update) {
        update(self->real, batch.count, batch.writes, 0, 0);
        osito_descriptor_set_shadow_images(write_count, writes);
    }
    osito_descriptor_write_batch_finish(&batch);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdPushDescriptorSetKHR(
    VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
    VkPipelineLayout layout, uint32_t set, uint32_t write_count,
    const VkWriteDescriptorSet *writes)
{
    if (!command_buffer || !layout || (write_count && !writes))
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_pipeline_layout *layout_wrapper =
        (struct osito_pipeline_layout *)(uintptr_t)layout;
    if (layout_wrapper->owner != device)
        return;

    struct osito_descriptor_write_batch batch;
    if (!osito_descriptor_write_batch_init(
            device, write_count, writes, 1, &batch))
        return;
    PFN_vkCmdPushDescriptorSetKHR push =
        (PFN_vkCmdPushDescriptorSetKHR)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdPushDescriptorSetKHR");
    if (!push) {
        push = (PFN_vkCmdPushDescriptorSetKHR)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdPushDescriptorSet");
    }
    if (push) {
        push(command_wrapper->real, bind_point, layout_wrapper->real, set,
             batch.count, batch.writes);
    }
    osito_descriptor_write_batch_finish(&batch);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdPushDescriptorSet(
    VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
    VkPipelineLayout layout, uint32_t set, uint32_t write_count,
    const VkWriteDescriptorSet *writes)
{
    vkCmdPushDescriptorSetKHR(command_buffer, bind_point, layout, set,
                              write_count, writes);
}

struct osito_template_write_batch {
    uint32_t count;
    VkWriteDescriptorSet *writes;
    void **payloads;
    void **chains;
};

static void
osito_template_write_batch_finish(struct osito_template_write_batch *batch)
{
    if (!batch)
        return;
    for (uint32_t i = 0; i < batch->count; i++) {
        free(batch->payloads ? batch->payloads[i] : 0);
        free(batch->chains ? batch->chains[i] : 0);
    }
    free(batch->chains);
    free(batch->payloads);
    free(batch->writes);
    memset(batch, 0, sizeof(*batch));
}

static int
osito_template_write_batch_init(
    const struct osito_descriptor_update_template *update_template,
    const void *data, struct osito_template_write_batch *batch)
{
    memset(batch, 0, sizeof(*batch));
    batch->count = update_template->entry_count;
    if (!batch->count)
        return 1;
    batch->writes = malloc(sizeof(*batch->writes) * batch->count);
    batch->payloads = malloc(sizeof(*batch->payloads) * batch->count);
    batch->chains = malloc(sizeof(*batch->chains) * batch->count);
    if (!batch->writes || !batch->payloads || !batch->chains)
        goto fail;
    memset(batch->payloads, 0, sizeof(*batch->payloads) * batch->count);
    memset(batch->chains, 0, sizeof(*batch->chains) * batch->count);

    for (uint32_t i = 0; i < batch->count; i++) {
        const VkDescriptorUpdateTemplateEntry *entry =
            &update_template->entries[i];
        const uint8_t *source = (const uint8_t *)data + entry->offset;
        VkWriteDescriptorSet *write = &batch->writes[i];
        memset(write, 0, sizeof(*write));
        write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write->dstBinding = entry->dstBinding;
        write->dstArrayElement = entry->dstArrayElement;
        write->descriptorCount = entry->descriptorCount;
        write->descriptorType = entry->descriptorType;

        switch (entry->descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
            VkDescriptorImageInfo *infos = entry->descriptorCount
                ? malloc(sizeof(*infos) * entry->descriptorCount) : 0;
            if (entry->descriptorCount && !infos)
                goto fail;
            batch->payloads[i] = infos;
            for (uint32_t j = 0; j < entry->descriptorCount; j++) {
                infos[j] = *(const VkDescriptorImageInfo *)source;
                source += entry->stride;
            }
            write->pImageInfo = infos;
            break;
        }
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
            VkDescriptorBufferInfo *infos = entry->descriptorCount
                ? malloc(sizeof(*infos) * entry->descriptorCount) : 0;
            if (entry->descriptorCount && !infos)
                goto fail;
            batch->payloads[i] = infos;
            for (uint32_t j = 0; j < entry->descriptorCount; j++) {
                infos[j] = *(const VkDescriptorBufferInfo *)source;
                source += entry->stride;
            }
            write->pBufferInfo = infos;
            break;
        }
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
            VkBufferView *views = entry->descriptorCount
                ? malloc(sizeof(*views) * entry->descriptorCount) : 0;
            if (entry->descriptorCount && !views)
                goto fail;
            batch->payloads[i] = views;
            for (uint32_t j = 0; j < entry->descriptorCount; j++) {
                views[j] = *(const VkBufferView *)source;
                source += entry->stride;
            }
            write->pTexelBufferView = views;
            break;
        }
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: {
            VkWriteDescriptorSetInlineUniformBlock *inline_block =
                malloc(sizeof(*inline_block));
            if (!inline_block)
                goto fail;
            *inline_block = (VkWriteDescriptorSetInlineUniformBlock) {
                .sType =
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,
                .dataSize = entry->descriptorCount,
                .pData = source,
            };
            batch->chains[i] = inline_block;
            write->pNext = inline_block;
            break;
        }
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
            VkWriteDescriptorSetAccelerationStructureKHR *accel =
                malloc(sizeof(*accel));
            if (!accel)
                goto fail;
            *accel = (VkWriteDescriptorSetAccelerationStructureKHR) {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                .accelerationStructureCount = entry->descriptorCount,
                .pAccelerationStructures =
                    (const VkAccelerationStructureKHR *)source,
            };
            batch->chains[i] = accel;
            write->pNext = accel;
            break;
        }
        default:
            goto fail;
        }
    }
    return 1;

fail:
    osito_template_write_batch_finish(batch);
    return 0;
}

VKAPI_ATTR void VKAPI_CALL
vkUpdateDescriptorSetWithTemplate(
    VkDevice device, VkDescriptorSet descriptor_set,
    VkDescriptorUpdateTemplate descriptor_update_template,
    const void *data)
{
    if (!device || !descriptor_set || !descriptor_update_template || !data)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_descriptor_update_template *update_template =
        (struct osito_descriptor_update_template *)(uintptr_t)
            descriptor_update_template;
    if (update_template->owner != self ||
        update_template->template_type !=
            VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET)
        return;

    struct osito_template_write_batch batch;
    if (!osito_template_write_batch_init(update_template, data, &batch))
        return;
    for (uint32_t i = 0; i < batch.count; i++)
        batch.writes[i].dstSet = descriptor_set;

    static int traced;
    if (!traced) {
        traced = 1;
        okgl_trace("[OKGL-DESC] expanded descriptor-set template\n");
    }
    vkUpdateDescriptorSets(device, batch.count, batch.writes, 0, NULL);
    osito_template_write_batch_finish(&batch);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdPushDescriptorSetWithTemplateKHR(
    VkCommandBuffer command_buffer,
    VkDescriptorUpdateTemplate descriptor_update_template,
    VkPipelineLayout layout, uint32_t set, const void *data)
{
    if (!command_buffer || !descriptor_update_template || !layout || !data)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_descriptor_update_template *update_template =
        (struct osito_descriptor_update_template *)(uintptr_t)
            descriptor_update_template;
    if (update_template->owner != command_wrapper->owner ||
        update_template->template_type !=
            VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR)
        return;

    struct osito_template_write_batch batch;
    if (!osito_template_write_batch_init(update_template, data, &batch))
        return;
    static int traced;
    if (!traced) {
        traced = 1;
        okgl_trace("[OKGL-DESC] expanded push descriptor template\n");
    }
    vkCmdPushDescriptorSetKHR(
        command_buffer, update_template->pipeline_bind_point, layout, set,
        batch.count, batch.writes);
    osito_template_write_batch_finish(&batch);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdPushDescriptorSetWithTemplate(
    VkCommandBuffer command_buffer,
    VkDescriptorUpdateTemplate descriptor_update_template,
    VkPipelineLayout layout, uint32_t set, const void *data)
{
    vkCmdPushDescriptorSetWithTemplateKHR(
        command_buffer, descriptor_update_template, layout, set, data);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *create_info,
               const VkAllocationCallbacks *allocator, VkBuffer *buffer)
{
    if (!device || !create_info || !buffer)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateBuffer create =
        (PFN_vkCreateBuffer)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateBuffer");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkBuffer real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_buffer *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyBuffer destroy =
            (PFN_vkDestroyBuffer)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyBuffer");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    wrapper->size = create_info->size;
    wrapper->memory = 0;
    wrapper->memory_offset = 0;
    wrapper->trace_shadow = 0;
    wrapper->trace_shadow_size = 0;
    *buffer = (VkBuffer)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyBuffer(VkDevice device, VkBuffer buffer,
                const VkAllocationCallbacks *allocator)
{
    if (!device || !buffer)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_buffer *wrapper =
        (struct osito_buffer *)(uintptr_t)buffer;
    PFN_vkDestroyBuffer destroy =
        (PFN_vkDestroyBuffer)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyBuffer");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper->trace_shadow);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBufferView(VkDevice device,
                   const VkBufferViewCreateInfo *create_info,
                   const VkAllocationCallbacks *allocator,
                   VkBufferView *view)
{
    if (!device || !create_info || !create_info->buffer || !view)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_buffer *buffer =
        (struct osito_buffer *)(uintptr_t)create_info->buffer;
    if (buffer->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkCreateBufferView create =
        (PFN_vkCreateBufferView)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateBufferView");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    VkBufferViewCreateInfo real_info = *create_info;
    real_info.buffer = buffer->real;
    VkBufferView real = VK_NULL_HANDLE;
    VkResult result = create(self->real, &real_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;

    struct osito_buffer_view *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyBufferView destroy =
            (PFN_vkDestroyBufferView)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyBufferView");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *view = (VkBufferView)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyBufferView(VkDevice device, VkBufferView view,
                    const VkAllocationCallbacks *allocator)
{
    if (!device || !view)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_buffer_view *wrapper =
        (struct osito_buffer_view *)(uintptr_t)view;
    if (wrapper->owner != self)
        return;
    PFN_vkDestroyBufferView destroy =
        (PFN_vkDestroyBufferView)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyBufferView");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                              VkMemoryRequirements *requirements)
{
    if (!device || !buffer || !requirements)
        return;
    memset(requirements, 0, sizeof(*requirements));
    struct osito_device *self = osito_device_from(device);
    struct osito_buffer *wrapper =
        (struct osito_buffer *)(uintptr_t)buffer;
    PFN_vkGetBufferMemoryRequirements get =
        (PFN_vkGetBufferMemoryRequirements)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkGetBufferMemoryRequirements");
    if (get)
        get(self->real, wrapper->real, requirements);
}

VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements)
{
    if (!device || !info || !requirements || !info->buffer)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_buffer *wrapper =
        (struct osito_buffer *)(uintptr_t)info->buffer;
    VkBufferMemoryRequirementsInfo2 real_info = *info;
    real_info.buffer = wrapper->real;
    PFN_vkGetBufferMemoryRequirements2 get =
        (PFN_vkGetBufferMemoryRequirements2)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkGetBufferMemoryRequirements2");
    if (get) {
        get(self->real, &real_info, requirements);
        return;
    }
    vkGetBufferMemoryRequirements(device, info->buffer,
                                  &requirements->memoryRequirements);
    for (VkBaseOutStructure *next = (VkBaseOutStructure *)requirements->pNext;
         next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            VkMemoryDedicatedRequirements *dedicated =
                (VkMemoryDedicatedRequirements *)next;
            dedicated->prefersDedicatedAllocation = VK_FALSE;
            dedicated->requiresDedicatedAllocation = VK_FALSE;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                   VkDeviceSize memory_offset)
{
    if (!device || !buffer || !memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_buffer *buffer_wrapper =
        (struct osito_buffer *)(uintptr_t)buffer;
    struct osito_memory *memory_wrapper =
        (struct osito_memory *)(uintptr_t)memory;
    if (buffer_wrapper->owner != self || memory_wrapper->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkBindBufferMemory bind =
        (PFN_vkBindBufferMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkBindBufferMemory");
    if (!bind)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult result = bind(self->real, buffer_wrapper->real,
                           memory_wrapper->real, memory_offset);
    if (result == VK_SUCCESS) {
        buffer_wrapper->memory = memory_wrapper;
        buffer_wrapper->memory_offset = memory_offset;
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateCommandPool(VkDevice device,
                    const VkCommandPoolCreateInfo *create_info,
                    const VkAllocationCallbacks *allocator,
                    VkCommandPool *command_pool)
{
    if (!device || !create_info || !command_pool)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateCommandPool create =
        (PFN_vkCreateCommandPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateCommandPool");
    return create ? create(self->real, create_info, allocator, command_pool)
                  : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyCommandPool(VkDevice device, VkCommandPool command_pool,
                     const VkAllocationCallbacks *allocator)
{
    if (!device || !command_pool)
        return;
    struct osito_device *self = osito_device_from(device);
    PFN_vkDestroyCommandPool destroy =
        (PFN_vkDestroyCommandPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyCommandPool");
    if (destroy)
        destroy(self->real, command_pool, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkResetCommandPool(VkDevice device, VkCommandPool command_pool,
                   VkCommandPoolResetFlags flags)
{
    if (!device || !command_pool)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkResetCommandPool reset =
        (PFN_vkResetCommandPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkResetCommandPool");
    return reset ? reset(self->real, command_pool, flags)
                 : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *allocate_info,
    VkCommandBuffer *command_buffers)
{
    if (!device || !allocate_info || !command_buffers ||
        !allocate_info->commandBufferCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkAllocateCommandBuffers allocate =
        (PFN_vkAllocateCommandBuffers)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkAllocateCommandBuffers");
    if (!allocate)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    uint32_t count = allocate_info->commandBufferCount;
    VkCommandBuffer *real = malloc((size_t)count * sizeof(*real));
    if (!real)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult result = allocate(self->real, allocate_info, real);
    if (result != VK_SUCCESS) {
        free(real);
        return result;
    }
    for (uint32_t i = 0; i < count; i++) {
        struct osito_cmd_buffer *wrapper = malloc(sizeof(*wrapper));
        if (!wrapper) {
            PFN_vkFreeCommandBuffers release =
                (PFN_vkFreeCommandBuffers)self->owner->icd->get_proc_addr(
                    self->owner->handle, "vkFreeCommandBuffers");
            if (release)
                release(self->real, allocate_info->commandPool, count, real);
            for (uint32_t j = 0; j < i; j++)
                free((struct osito_cmd_buffer *)command_buffers[j]);
            free(real);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        memset(wrapper, 0, sizeof(*wrapper));
        set_loader_magic_value(wrapper);
        wrapper->owner = self;
        wrapper->real = real[i];
        command_buffers[i] = (VkCommandBuffer)wrapper;
    }
    free(real);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkFreeCommandBuffers(VkDevice device, VkCommandPool command_pool,
                     uint32_t count,
                     const VkCommandBuffer *command_buffers)
{
    if (!device || !count || !command_buffers)
        return;
    struct osito_device *self = osito_device_from(device);
    VkCommandBuffer *real = malloc((size_t)count * sizeof(*real));
    if (!real)
        return;
    for (uint32_t i = 0; i < count; i++) {
        struct osito_cmd_buffer *wrapper =
            (struct osito_cmd_buffer *)command_buffers[i];
        real[i] = wrapper->real;
    }
    PFN_vkFreeCommandBuffers release =
        (PFN_vkFreeCommandBuffers)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkFreeCommandBuffers");
    if (release)
        release(self->real, command_pool, count, real);
    for (uint32_t i = 0; i < count; i++)
        free((struct osito_cmd_buffer *)command_buffers[i]);
    free(real);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkBeginCommandBuffer(VkCommandBuffer command_buffer,
                     const VkCommandBufferBeginInfo *begin_info)
{
    if (!command_buffer || !begin_info)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    wrapper->trace_graphics_set2 = 0;
    wrapper->trace_draw_sequence = 0;
    memset(wrapper->trace_vertex_buffers, 0,
           sizeof(wrapper->trace_vertex_buffers));
    memset(wrapper->trace_vertex_offsets, 0,
           sizeof(wrapper->trace_vertex_offsets));
    memset(wrapper->trace_vertex_strides, 0,
           sizeof(wrapper->trace_vertex_strides));
    wrapper->trace_index_buffer = 0;
    wrapper->trace_index_offset = 0;
    wrapper->trace_index_type = VK_INDEX_TYPE_UINT16;
    wrapper->trace_dashboard_draw_count = 0;
    memset(wrapper->trace_draw_samples, 0,
           sizeof(wrapper->trace_draw_samples));
    memset(wrapper->trace_image_copies, 0,
           sizeof(wrapper->trace_image_copies));
    PFN_vkBeginCommandBuffer begin =
        (PFN_vkBeginCommandBuffer)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle, "vkBeginCommandBuffer");
    return begin ? begin(wrapper->real, begin_info)
                 : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(VkCommandBuffer command_buffer)
{
    if (!command_buffer)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkEndCommandBuffer end =
        (PFN_vkEndCommandBuffer)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkEndCommandBuffer");
    return end ? end(wrapper->real) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkResetCommandBuffer(VkCommandBuffer command_buffer,
                     VkCommandBufferResetFlags flags)
{
    if (!command_buffer)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    wrapper->trace_graphics_set2 = 0;
    wrapper->trace_draw_sequence = 0;
    memset(wrapper->trace_vertex_buffers, 0,
           sizeof(wrapper->trace_vertex_buffers));
    memset(wrapper->trace_vertex_offsets, 0,
           sizeof(wrapper->trace_vertex_offsets));
    memset(wrapper->trace_vertex_strides, 0,
           sizeof(wrapper->trace_vertex_strides));
    wrapper->trace_index_buffer = 0;
    wrapper->trace_index_offset = 0;
    wrapper->trace_index_type = VK_INDEX_TYPE_UINT16;
    wrapper->trace_dashboard_draw_count = 0;
    memset(wrapper->trace_draw_samples, 0,
           sizeof(wrapper->trace_draw_samples));
    memset(wrapper->trace_image_copies, 0,
           sizeof(wrapper->trace_image_copies));
    PFN_vkResetCommandBuffer reset =
        (PFN_vkResetCommandBuffer)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle, "vkResetCommandBuffer");
    return reset ? reset(wrapper->real, flags)
                 : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(VkQueue queue, uint32_t submit_count,
              const VkSubmitInfo *submits, VkFence fence)
{
    if (!queue || (submit_count && !submits))
        return VK_ERROR_INITIALIZATION_FAILED;

    VkSubmitInfo2 *converted = 0;
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (submit_count) {
        converted = calloc(submit_count, sizeof(*converted));
        if (!converted)
            return result;
    }

    for (uint32_t i = 0; i < submit_count; i++) {
        const VkSubmitInfo *source = &submits[i];
        VkSubmitInfo2 *target = &converted[i];
        const VkTimelineSemaphoreSubmitInfo *timeline = 0;
        const VkDeviceGroupSubmitInfo *device_group = 0;
        const VkProtectedSubmitInfo *protected_submit = 0;

        if (source->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO ||
            (source->waitSemaphoreCount &&
             (!source->pWaitSemaphores || !source->pWaitDstStageMask)) ||
            (source->commandBufferCount && !source->pCommandBuffers) ||
            (source->signalSemaphoreCount && !source->pSignalSemaphores)) {
            result = VK_ERROR_INITIALIZATION_FAILED;
            goto cleanup;
        }

        for (const VkBaseInStructure *next =
                 (const VkBaseInStructure *)source->pNext;
             next; next = next->pNext) {
            switch (next->sType) {
            case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:
                timeline = (const VkTimelineSemaphoreSubmitInfo *)next;
                break;
            case VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO:
                device_group = (const VkDeviceGroupSubmitInfo *)next;
                break;
            case VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO:
                protected_submit = (const VkProtectedSubmitInfo *)next;
                break;
            default:
                result = VK_ERROR_EXTENSION_NOT_PRESENT;
                goto cleanup;
            }
        }

        if (timeline &&
            (timeline->waitSemaphoreValueCount !=
                 source->waitSemaphoreCount ||
             timeline->signalSemaphoreValueCount !=
                 source->signalSemaphoreCount ||
             (timeline->waitSemaphoreValueCount &&
              !timeline->pWaitSemaphoreValues) ||
             (timeline->signalSemaphoreValueCount &&
              !timeline->pSignalSemaphoreValues))) {
            result = VK_ERROR_INITIALIZATION_FAILED;
            goto cleanup;
        }
        if (device_group &&
            (device_group->waitSemaphoreCount !=
                 source->waitSemaphoreCount ||
             device_group->commandBufferCount !=
                 source->commandBufferCount ||
             device_group->signalSemaphoreCount !=
                 source->signalSemaphoreCount ||
             (device_group->waitSemaphoreCount &&
              !device_group->pWaitSemaphoreDeviceIndices) ||
             (device_group->commandBufferCount &&
              !device_group->pCommandBufferDeviceMasks) ||
             (device_group->signalSemaphoreCount &&
              !device_group->pSignalSemaphoreDeviceIndices))) {
            result = VK_ERROR_INITIALIZATION_FAILED;
            goto cleanup;
        }

        if (loader_trace_take(&queue_submit1_trace_count, 96u)) {
            uint64_t first_signal =
                timeline && timeline->signalSemaphoreValueCount
                ? timeline->pSignalSemaphoreValues[0] : 0;
            uint64_t last_signal =
                timeline && timeline->signalSemaphoreValueCount
                ? timeline->pSignalSemaphoreValues[
                    timeline->signalSemaphoreValueCount - 1] : 0;
            printf("[VK sync] submit1 item=%u cmds=%u waits=%u signals=%u "
                   "timeline=%p first=%llu last=%llu\n",
                   i, source->commandBufferCount,
                   source->waitSemaphoreCount, source->signalSemaphoreCount,
                   (void *)timeline,
                   (unsigned long long)first_signal,
                   (unsigned long long)last_signal);
        }

        target->sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        if (protected_submit && protected_submit->protectedSubmit)
            target->flags = VK_SUBMIT_PROTECTED_BIT;

        target->waitSemaphoreInfoCount = source->waitSemaphoreCount;
        if (source->waitSemaphoreCount) {
            VkSemaphoreSubmitInfo *waits = calloc(
                source->waitSemaphoreCount, sizeof(*waits));
            if (!waits)
                goto cleanup;
            target->pWaitSemaphoreInfos = waits;
            for (uint32_t j = 0; j < source->waitSemaphoreCount; j++) {
                waits[j].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                waits[j].semaphore = source->pWaitSemaphores[j];
                waits[j].value = timeline
                    ? timeline->pWaitSemaphoreValues[j] : 0;
                waits[j].stageMask = source->pWaitDstStageMask[j];
                waits[j].deviceIndex = device_group
                    ? device_group->pWaitSemaphoreDeviceIndices[j] : 0;
            }
        }

        target->commandBufferInfoCount = source->commandBufferCount;
        if (source->commandBufferCount) {
            VkCommandBufferSubmitInfo *commands = calloc(
                source->commandBufferCount, sizeof(*commands));
            if (!commands)
                goto cleanup;
            target->pCommandBufferInfos = commands;
            for (uint32_t j = 0; j < source->commandBufferCount; j++) {
                commands[j].sType =
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
                commands[j].commandBuffer = source->pCommandBuffers[j];
                commands[j].deviceMask = device_group
                    ? device_group->pCommandBufferDeviceMasks[j] : 0;
            }
        }

        target->signalSemaphoreInfoCount = source->signalSemaphoreCount;
        if (source->signalSemaphoreCount) {
            VkSemaphoreSubmitInfo *signals = calloc(
                source->signalSemaphoreCount, sizeof(*signals));
            if (!signals)
                goto cleanup;
            target->pSignalSemaphoreInfos = signals;
            for (uint32_t j = 0; j < source->signalSemaphoreCount; j++) {
                signals[j].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                signals[j].semaphore = source->pSignalSemaphores[j];
                signals[j].value = timeline
                    ? timeline->pSignalSemaphoreValues[j] : 0;
                signals[j].stageMask =
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                signals[j].deviceIndex = device_group
                    ? device_group->pSignalSemaphoreDeviceIndices[j] : 0;
            }
        }
    }

    result = vkQueueSubmit2(queue, submit_count, converted, fence);

cleanup:
    for (uint32_t i = 0; i < submit_count; i++) {
        free((void *)converted[i].pWaitSemaphoreInfos);
        free((void *)converted[i].pCommandBufferInfos);
        free((void *)converted[i].pSignalSemaphoreInfos);
    }
    free(converted);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2(VkQueue queue, uint32_t submit_count,
               const VkSubmitInfo2 *submits, VkFence fence)
{
    if (!queue || (submit_count && !submits))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_queue *queue_wrapper = (struct osito_queue *)queue;
    struct osito_device *device = queue_wrapper->owner;
    PFN_vkQueueSubmit2 submit =
        (PFN_vkQueueSubmit2)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkQueueSubmit2");
    if (!submit)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    if (loader_trace_take(&queue_submit2_trace_count, 96u)) {
        uint32_t signal_count = submit_count
            ? submits[submit_count - 1].signalSemaphoreInfoCount : 0;
        uint64_t first_signal = signal_count
            ? submits[submit_count - 1].pSignalSemaphoreInfos[0].value : 0;
        uint64_t last_signal = signal_count
            ? submits[submit_count - 1]
                .pSignalSemaphoreInfos[signal_count - 1].value : 0;
        printf("[VK sync] submit2 count=%u last_signals=%u "
               "first=%llu last=%llu fence=%p\n",
               submit_count, signal_count,
               (unsigned long long)first_signal,
               (unsigned long long)last_signal,
               (void *)(uintptr_t)fence);
        okgl_diag3("[VK-SUBMIT2]", submit_count, signal_count, last_signal);
    }

    VkSubmitInfo2 *real_submits = 0;
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    int trace_render_submit = 0;
    if (submit_count) {
        real_submits = malloc((size_t)submit_count * sizeof(*real_submits));
        if (!real_submits)
            return result;
        memset(real_submits, 0,
               (size_t)submit_count * sizeof(*real_submits));
    }

    for (uint32_t i = 0; i < submit_count; i++) {
        const VkSubmitInfo2 *source = &submits[i];
        VkSubmitInfo2 *target = &real_submits[i];
        if (source->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO_2 ||
            (source->waitSemaphoreInfoCount &&
             !source->pWaitSemaphoreInfos) ||
            (source->commandBufferInfoCount &&
             !source->pCommandBufferInfos) ||
            (source->signalSemaphoreInfoCount &&
             !source->pSignalSemaphoreInfos)) {
            result = VK_ERROR_INITIALIZATION_FAILED;
            goto cleanup;
        }
        *target = *source;
        target->pWaitSemaphoreInfos = 0;
        target->pCommandBufferInfos = 0;
        target->pSignalSemaphoreInfos = 0;

        if (source->waitSemaphoreInfoCount) {
            VkSemaphoreSubmitInfo *infos = malloc(
                (size_t)source->waitSemaphoreInfoCount * sizeof(*infos));
            if (!infos)
                goto cleanup;
            target->pWaitSemaphoreInfos = infos;
            for (uint32_t j = 0; j < source->waitSemaphoreInfoCount; j++) {
                infos[j] = source->pWaitSemaphoreInfos[j];
                struct osito_semaphore *wrapper =
                    (struct osito_semaphore *)(uintptr_t)infos[j].semaphore;
                if (!wrapper || wrapper->owner != device) {
                    result = VK_ERROR_INITIALIZATION_FAILED;
                    goto cleanup;
                }
                infos[j].semaphore = wrapper->real;
            }
        }

        if (source->commandBufferInfoCount) {
            VkCommandBufferSubmitInfo *infos = malloc(
                (size_t)source->commandBufferInfoCount * sizeof(*infos));
            if (!infos)
                goto cleanup;
            target->pCommandBufferInfos = infos;
            for (uint32_t j = 0; j < source->commandBufferInfoCount; j++) {
                infos[j] = source->pCommandBufferInfos[j];
                struct osito_cmd_buffer *wrapper =
                    (struct osito_cmd_buffer *)infos[j].commandBuffer;
                if (!wrapper || wrapper->owner != device) {
                    result = VK_ERROR_INITIALIZATION_FAILED;
                    goto cleanup;
                }
                if (OSITO_VK_RENDER_DIAGNOSTICS) {
                    loader_trace_submit_draws(wrapper);
                    trace_render_submit |=
                        loader_trace_submit_image_copies(wrapper);
                    for (uint32_t slot = 0; slot < 4; slot++)
                        trace_render_submit |=
                            wrapper->trace_draw_samples[slot].valid;
                }
                infos[j].commandBuffer = wrapper->real;
            }
        }

        if (source->signalSemaphoreInfoCount) {
            VkSemaphoreSubmitInfo *infos = malloc(
                (size_t)source->signalSemaphoreInfoCount * sizeof(*infos));
            if (!infos)
                goto cleanup;
            target->pSignalSemaphoreInfos = infos;
            for (uint32_t j = 0; j < source->signalSemaphoreInfoCount; j++) {
                infos[j] = source->pSignalSemaphoreInfos[j];
                struct osito_semaphore *wrapper =
                    (struct osito_semaphore *)(uintptr_t)infos[j].semaphore;
                if (!wrapper || wrapper->owner != device) {
                    result = VK_ERROR_INITIALIZATION_FAILED;
                    goto cleanup;
                }
                infos[j].semaphore = wrapper->real;
            }
        }
    }

    VkFence real_fence = VK_NULL_HANDLE;
    if (fence) {
        struct osito_fence *wrapper =
            (struct osito_fence *)(uintptr_t)fence;
        if (wrapper->owner != device) {
            result = VK_ERROR_INITIALIZATION_FAILED;
            goto cleanup;
        }
        real_fence = wrapper->real;
    }
    result = submit(queue_wrapper->real, submit_count, real_submits,
                    real_fence);
    if (OSITO_VK_RENDER_DIAGNOSTICS && result == VK_SUCCESS &&
        trace_render_submit) {
        PFN_vkQueueWaitIdle wait_idle =
            (PFN_vkQueueWaitIdle)device->owner->icd->get_proc_addr(
                device->owner->handle, "vkQueueWaitIdle");
        VkResult wait_result = wait_idle
            ? wait_idle(queue_wrapper->real)
            : VK_ERROR_EXTENSION_NOT_PRESENT;
        okgl_diag3("[VK-RENDER-IDLE]", submit_count,
                   (uint64_t)(uint32_t)result,
                   (uint64_t)(uint32_t)wait_result);
        if (wait_result == VK_SUCCESS) {
            uint32_t checked = 0;
            uint32_t mutations = 0;
            for (uint32_t i = 0; i < submit_count; i++) {
                const VkSubmitInfo2 *source = &submits[i];
                for (uint32_t j = 0;
                     j < source->commandBufferInfoCount; j++) {
                    struct osito_cmd_buffer *wrapper =
                        (struct osito_cmd_buffer *)
                            source->pCommandBufferInfos[j].commandBuffer;
                    if (!wrapper)
                        continue;
                    for (uint32_t slot = 0; slot < 4; slot++)
                        checked += wrapper->trace_draw_samples[slot].valid;
                    mutations +=
                        loader_trace_post_idle_descriptors(wrapper);
                    for (uint32_t slot = 0; slot < 4; slot++) {
                        struct osito_trace_draw *sample =
                            &wrapper->trace_draw_samples[slot];
                        struct osito_descriptor_set *set =
                            sample->descriptor_set;
                        if (sample->valid && set && set->trace_image &&
                            set->trace_image->trace_dashboard)
                            loader_trace_dashboard_readback(
                                queue_wrapper, set->trace_image,
                                set->image_layout);
                    }
                }
            }
            if (loader_trace_take(&descriptor_idle_check_trace_count, 128))
                okgl_diag3("[VK-DESC-IDLE-CHECK]", checked, mutations,
                           submit_count);
        }
        if (wait_result != VK_SUCCESS)
            result = wait_result;
    }

cleanup:
    for (uint32_t i = 0; i < submit_count; i++) {
        free((void *)real_submits[i].pWaitSemaphoreInfos);
        free((void *)real_submits[i].pCommandBufferInfos);
        free((void *)real_submits[i].pSignalSemaphoreInfos);
    }
    free(real_submits);
    return result;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!name)
        return 0;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)vkCreateInstance;
    if (strcmp(name, "vkEnumerateInstanceVersion") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceVersion;
    if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    if (strcmp(name, "vkEnumerateInstanceLayerProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties;
    if (!instance)
        return 0;
    if (strcmp(name, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)vkDestroyInstance;
    if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
        return (PFN_vkVoidFunction)vkEnumeratePhysicalDevices;
    if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties;
    if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties;
    if (strcmp(name, "vkGetPhysicalDeviceFeatures") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures;
    if (strcmp(name, "vkGetPhysicalDeviceFeatures2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures2;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties;
    if (strcmp(name, "vkGetPhysicalDeviceFormatProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties;
    if (strcmp(name, "vkGetPhysicalDeviceImageFormatProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties;
    if (strcmp(name,
               "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT") == 0)
        return (PFN_vkVoidFunction)
            vkGetPhysicalDeviceCalibrateableTimeDomainsEXT;
    if (strcmp(name, "vkEnumerateDeviceLayerProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateDeviceLayerProperties;
    if (strcmp(name, "vkGetPhysicalDeviceSparseImageFormatProperties") == 0)
        return (PFN_vkVoidFunction)
            vkGetPhysicalDeviceSparseImageFormatProperties;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2KHR") == 0)
        return (PFN_vkVoidFunction)
            vkGetPhysicalDeviceQueueFamilyProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceSparseImageFormatProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)
            vkGetPhysicalDeviceSparseImageFormatProperties2;
    if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties;
    if (strcmp(name, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)vkCreateDevice;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (strcmp(name, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)vkDestroyDevice;
    if (strcmp(name, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceQueue;
    if (strcmp(name, "vkDeviceWaitIdle") == 0)
        return (PFN_vkVoidFunction)vkDeviceWaitIdle;
    if (strcmp(name, "vkQueueWaitIdle") == 0)
        return (PFN_vkVoidFunction)vkQueueWaitIdle;
    if (strcmp(name, "vkGetCalibratedTimestampsEXT") == 0)
        return (PFN_vkVoidFunction)vkGetCalibratedTimestampsEXT;
    if (strcmp(name, "vkCreateDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorSetLayout;
    if (strcmp(name, "vkDestroyDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorSetLayout;
    if (strcmp(name, "vkCreateDescriptorUpdateTemplate") == 0 ||
        strcmp(name, "vkCreateDescriptorUpdateTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorUpdateTemplate;
    if (strcmp(name, "vkDestroyDescriptorUpdateTemplate") == 0 ||
        strcmp(name, "vkDestroyDescriptorUpdateTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorUpdateTemplate;
    if (strcmp(name, "vkUpdateDescriptorSetWithTemplate") == 0 ||
        strcmp(name, "vkUpdateDescriptorSetWithTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkUpdateDescriptorSetWithTemplate;
    if (strcmp(name, "vkCmdPushDescriptorSet") == 0 ||
        strcmp(name, "vkCmdPushDescriptorSetKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdPushDescriptorSetKHR;
    if (strcmp(name, "vkCmdPushDescriptorSetWithTemplate") == 0 ||
        strcmp(name, "vkCmdPushDescriptorSetWithTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdPushDescriptorSetWithTemplateKHR;
    if (strcmp(name, "vkCreatePipelineLayout") == 0)
        return (PFN_vkVoidFunction)vkCreatePipelineLayout;
    if (strcmp(name, "vkDestroyPipelineLayout") == 0)
        return (PFN_vkVoidFunction)vkDestroyPipelineLayout;
    if (strcmp(name, "vkCreateSemaphore") == 0)
        return (PFN_vkVoidFunction)vkCreateSemaphore;
    if (strcmp(name, "vkDestroySemaphore") == 0)
        return (PFN_vkVoidFunction)vkDestroySemaphore;
    if (strcmp(name, "vkGetSemaphoreCounterValue") == 0 ||
        strcmp(name, "vkGetSemaphoreCounterValueKHR") == 0)
        return (PFN_vkVoidFunction)vkGetSemaphoreCounterValue;
    if (strcmp(name, "vkWaitSemaphores") == 0 ||
        strcmp(name, "vkWaitSemaphoresKHR") == 0)
        return (PFN_vkVoidFunction)vkWaitSemaphores;
    if (strcmp(name, "vkSignalSemaphore") == 0 ||
        strcmp(name, "vkSignalSemaphoreKHR") == 0)
        return (PFN_vkVoidFunction)vkSignalSemaphore;
    if (strcmp(name, "vkCreateFence") == 0)
        return (PFN_vkVoidFunction)vkCreateFence;
    if (strcmp(name, "vkDestroyFence") == 0)
        return (PFN_vkVoidFunction)vkDestroyFence;
    if (strcmp(name, "vkResetFences") == 0)
        return (PFN_vkVoidFunction)vkResetFences;
    if (strcmp(name, "vkWaitForFences") == 0)
        return (PFN_vkVoidFunction)vkWaitForFences;
    if (strcmp(name, "vkCreateCommandPool") == 0)
        return (PFN_vkVoidFunction)vkCreateCommandPool;
    if (strcmp(name, "vkDestroyCommandPool") == 0)
        return (PFN_vkVoidFunction)vkDestroyCommandPool;
    if (strcmp(name, "vkResetCommandPool") == 0)
        return (PFN_vkVoidFunction)vkResetCommandPool;
    if (strcmp(name, "vkAllocateCommandBuffers") == 0)
        return (PFN_vkVoidFunction)vkAllocateCommandBuffers;
    if (strcmp(name, "vkFreeCommandBuffers") == 0)
        return (PFN_vkVoidFunction)vkFreeCommandBuffers;
    if (strcmp(name, "vkBeginCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkBeginCommandBuffer;
    if (strcmp(name, "vkEndCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkEndCommandBuffer;
    if (strcmp(name, "vkResetCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkResetCommandBuffer;
    if (strcmp(name, "vkQueueSubmit") == 0)
        return (PFN_vkVoidFunction)vkQueueSubmit;
    if (strcmp(name, "vkQueueSubmit2") == 0 ||
        strcmp(name, "vkQueueSubmit2KHR") == 0)
        return (PFN_vkVoidFunction)vkQueueSubmit2;
    if (strcmp(name, "vkCreateBuffer") == 0)
        return (PFN_vkVoidFunction)vkCreateBuffer;
    if (strcmp(name, "vkDestroyBuffer") == 0)
        return (PFN_vkVoidFunction)vkDestroyBuffer;
    if (strcmp(name, "vkCreateBufferView") == 0)
        return (PFN_vkVoidFunction)vkCreateBufferView;
    if (strcmp(name, "vkDestroyBufferView") == 0)
        return (PFN_vkVoidFunction)vkDestroyBufferView;
    if (strcmp(name, "vkGetBufferMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)vkGetBufferMemoryRequirements;
    if (strcmp(name, "vkGetBufferMemoryRequirements2") == 0 ||
        strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetBufferMemoryRequirements2;
    if (strcmp(name, "vkBindBufferMemory") == 0)
        return (PFN_vkVoidFunction)vkBindBufferMemory;
    if (strcmp(name, "vkAllocateMemory") == 0)
        return (PFN_vkVoidFunction)vkAllocateMemory;
    if (strcmp(name, "vkFreeMemory") == 0)
        return (PFN_vkVoidFunction)vkFreeMemory;
    if (strcmp(name, "vkMapMemory") == 0)
        return (PFN_vkVoidFunction)vkMapMemory;
    if (strcmp(name, "vkUnmapMemory") == 0)
        return (PFN_vkVoidFunction)vkUnmapMemory;
    if (strcmp(name, "vkCmdCopyBuffer") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBuffer;
    if (strcmp(name, "vkCmdPipelineBarrier") == 0)
        return (PFN_vkVoidFunction)vkCmdPipelineBarrier;
    if (strcmp(name, "vkCmdCopyBufferToImage") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBufferToImage;
    if (strcmp(name, "vkCmdCopyImageToBuffer") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyImageToBuffer;
    if (strcmp(name, "vkCmdCopyBuffer2") == 0 ||
        strcmp(name, "vkCmdCopyBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBuffer2;
    if (strcmp(name, "vkCmdPipelineBarrier2") == 0 ||
        strcmp(name, "vkCmdPipelineBarrier2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdPipelineBarrier2;
    if (strcmp(name, "vkCmdCopyBufferToImage2") == 0 ||
        strcmp(name, "vkCmdCopyBufferToImage2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBufferToImage2;
    if (strcmp(name, "vkCmdCopyImageToBuffer2") == 0 ||
        strcmp(name, "vkCmdCopyImageToBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyImageToBuffer2;
    if (strcmp(name, "vkCmdClearColorImage") == 0)
        return (PFN_vkVoidFunction)vkCmdClearColorImage;
    if (strcmp(name, "vkCmdClearDepthStencilImage") == 0)
        return (PFN_vkVoidFunction)vkCmdClearDepthStencilImage;
    if (strcmp(name, "vkCmdClearAttachments") == 0)
        return (PFN_vkVoidFunction)vkCmdClearAttachments;
    if (strcmp(name, "vkCmdBeginRendering") == 0 ||
        strcmp(name, "vkCmdBeginRenderingKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdBeginRendering;
    if (strcmp(name, "vkCmdEndRendering") == 0 ||
        strcmp(name, "vkCmdEndRenderingKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdEndRendering;
    if (strcmp(name, "vkCmdBindVertexBuffers") == 0)
        return (PFN_vkVoidFunction)vkCmdBindVertexBuffers;
    if (strcmp(name, "vkCmdBindVertexBuffers2") == 0 ||
        strcmp(name, "vkCmdBindVertexBuffers2EXT") == 0)
        return (PFN_vkVoidFunction)vkCmdBindVertexBuffers2;
    if (strcmp(name, "vkCmdBindIndexBuffer") == 0)
        return (PFN_vkVoidFunction)vkCmdBindIndexBuffer;
    if (strcmp(name, "vkCmdBindIndexBuffer2") == 0 ||
        strcmp(name, "vkCmdBindIndexBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdBindIndexBuffer2KHR;
    if (strcmp(name, "vkCmdBindPipeline") == 0)
        return (PFN_vkVoidFunction)vkCmdBindPipeline;
    if (strcmp(name, "vkCmdSetAttachmentFeedbackLoopEnableEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetAttachmentFeedbackLoopEnableEXT;
    if (strcmp(name, "vkCmdSetCullMode") == 0 ||
        strcmp(name, "vkCmdSetCullModeEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetCullMode;
    if (strcmp(name, "vkCmdSetFrontFace") == 0 ||
        strcmp(name, "vkCmdSetFrontFaceEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetFrontFace;
    if (strcmp(name, "vkCmdSetPrimitiveTopology") == 0 ||
        strcmp(name, "vkCmdSetPrimitiveTopologyEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetPrimitiveTopology;
    if (strcmp(name, "vkCmdSetViewport") == 0)
        return (PFN_vkVoidFunction)vkCmdSetViewport;
    if (strcmp(name, "vkCmdSetScissor") == 0)
        return (PFN_vkVoidFunction)vkCmdSetScissor;
    if (strcmp(name, "vkCmdSetLineWidth") == 0)
        return (PFN_vkVoidFunction)vkCmdSetLineWidth;
    if (strcmp(name, "vkCmdSetDepthBias") == 0)
        return (PFN_vkVoidFunction)vkCmdSetDepthBias;
    if (strcmp(name, "vkCmdSetBlendConstants") == 0)
        return (PFN_vkVoidFunction)vkCmdSetBlendConstants;
    if (strcmp(name, "vkCmdSetDepthBounds") == 0)
        return (PFN_vkVoidFunction)vkCmdSetDepthBounds;
    if (strcmp(name, "vkCmdSetStencilCompareMask") == 0)
        return (PFN_vkVoidFunction)vkCmdSetStencilCompareMask;
    if (strcmp(name, "vkCmdSetStencilWriteMask") == 0)
        return (PFN_vkVoidFunction)vkCmdSetStencilWriteMask;
    if (strcmp(name, "vkCmdSetStencilReference") == 0)
        return (PFN_vkVoidFunction)vkCmdSetStencilReference;
    if (strcmp(name, "vkCmdSetViewportWithCount") == 0 ||
        strcmp(name, "vkCmdSetViewportWithCountEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetViewportWithCount;
    if (strcmp(name, "vkCmdSetScissorWithCount") == 0 ||
        strcmp(name, "vkCmdSetScissorWithCountEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetScissorWithCount;
    if (strcmp(name, "vkCmdDraw") == 0)
        return (PFN_vkVoidFunction)vkCmdDraw;
    if (strcmp(name, "vkCmdDrawIndexed") == 0)
        return (PFN_vkVoidFunction)vkCmdDrawIndexed;
    if (strcmp(name, "vkCmdBindDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkCmdBindDescriptorSets;
    if (strcmp(name, "vkCmdPushConstants") == 0)
        return (PFN_vkVoidFunction)vkCmdPushConstants;
    if (strcmp(name, "vkCreateImage") == 0)
        return (PFN_vkVoidFunction)vkCreateImage;
    if (strcmp(name, "vkDestroyImage") == 0)
        return (PFN_vkVoidFunction)vkDestroyImage;
    if (strcmp(name, "vkGetImageMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)vkGetImageMemoryRequirements;
    if (strcmp(name, "vkGetImageMemoryRequirements2") == 0 ||
        strcmp(name, "vkGetImageMemoryRequirements2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetImageMemoryRequirements2;
    if (strcmp(name, "vkBindImageMemory") == 0)
        return (PFN_vkVoidFunction)vkBindImageMemory;
    if (strcmp(name, "vkCreateImageView") == 0)
        return (PFN_vkVoidFunction)vkCreateImageView;
    if (strcmp(name, "vkDestroyImageView") == 0)
        return (PFN_vkVoidFunction)vkDestroyImageView;
    if (strcmp(name, "vkCreateSampler") == 0)
        return (PFN_vkVoidFunction)vkCreateSampler;
    if (strcmp(name, "vkDestroySampler") == 0)
        return (PFN_vkVoidFunction)vkDestroySampler;
    if (strcmp(name, "vkCreateShaderModule") == 0)
        return (PFN_vkVoidFunction)vkCreateShaderModule;
    if (strcmp(name, "vkDestroyShaderModule") == 0)
        return (PFN_vkVoidFunction)vkDestroyShaderModule;
    if (strcmp(name, "vkCreateGraphicsPipelines") == 0)
        return (PFN_vkVoidFunction)vkCreateGraphicsPipelines;
    if (strcmp(name, "vkCreateComputePipelines") == 0)
        return (PFN_vkVoidFunction)vkCreateComputePipelines;
    if (strcmp(name, "vkDestroyPipeline") == 0)
        return (PFN_vkVoidFunction)vkDestroyPipeline;
    if (strcmp(name, "vkCreateDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorPool;
    if (strcmp(name, "vkDestroyDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorPool;
    if (strcmp(name, "vkResetDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkResetDescriptorPool;
    if (strcmp(name, "vkAllocateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkAllocateDescriptorSets;
    if (strcmp(name, "vkUpdateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkUpdateDescriptorSets;
    if (strcmp(name, "vkCreateOsitokCompositorSurfaceKHR") == 0)
        return (PFN_vkVoidFunction)vkCreateOsitokCompositorSurfaceKHR;
    if (strcmp(name, "vkDestroySurfaceKHR") == 0)
        return (PFN_vkVoidFunction)vkDestroySurfaceKHR;
    return 0;
}
