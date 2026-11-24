/*
 * OsitoK Vulkan loader — internal shared types.
 *
 * The loader keeps a static list of registered ICDs (icd_table.c).
 * Each VkInstance created by the loader is backed by a struct
 * containing a dispatch table + one VkInstance per ICD that
 * successfully created its own instance.
 *
 * Wave 2 deviation from plan: we call OsitoK libc (malloc/free/memset/
 * memcpy/strcmp) directly rather than through osito_* aliases — those
 * symbols already exist in tcclib.c / malloc.c.
 */
#ifndef OSITOK_VK_LOADER_H
#define OSITOK_VK_LOADER_H

#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

/* Freestanding-friendly libc shim — brings in size_t/NULL and the function
 * declarations we need without dragging in a hosted stdlib. */
#include <stddef.h>

#define OSITOK_VK_LOADER_MAX_ICDS 4

/* Signature of an ICD's "get entry point" function — the ONE symbol the
 * loader requires each ICD to export. Already typedef'd by vk_icd.h as
 * PFN_vk_icdGetInstanceProcAddr. */

/* Per-ICD table entry. Compile-time registered. */
struct osito_icd_entry {
    const char                      *name;            /* e.g. "venus", "nvk-stub" */
    PFN_vk_icdGetInstanceProcAddr    get_proc_addr;
};

extern const struct osito_icd_entry osito_icd_table[];
extern const unsigned                osito_icd_count;

/* Per-ICD instance state inside an Instance handle. */
struct osito_icd_inst {
    const struct osito_icd_entry *icd;
    VkInstance                    handle;     /* instance handle owned by the ICD */
};

/* The loader's opaque VkInstance target. App sees a VkInstance, we see this. */
struct osito_instance {
    /* Vulkan magic loader value — first field MUST be this sentinel
     * (matches the Khronos LoaderMagic pattern in vk_icd.h). */
    VK_LOADER_DATA            loader_data;

    unsigned                  icd_instance_count;
    struct osito_icd_inst     icd_instances[OSITOK_VK_LOADER_MAX_ICDS];
};

/* Handle helpers — cast between VkInstance and struct osito_instance*. */
static inline struct osito_instance *osito_instance_from(VkInstance h) {
    return (struct osito_instance *)h;
}
static inline VkInstance osito_instance_to(struct osito_instance *s) {
    return (VkInstance)s;
}

/* W3b.2 — wrappers for VkPhysicalDevice / VkDevice so the loader can
 * dispatch to the correct owning ICD (fixes the "first ICD wins"
 * gotcha from W3a). Each wrapper stores the ICD-returned handle plus
 * a pointer back to the ICD's state inside the owning instance.
 *
 * Handle lifetime: wrappers are leaked if the app destroys the
 * owning VkInstance without destroying its devices / enumerated phys
 * devices first. W3b.3 will track them on the instance; for W3b.2
 * we document that restriction. */
struct osito_phys_device {
    VK_LOADER_DATA             loader_data;
    struct osito_icd_inst     *owner;
    VkPhysicalDevice           real;
};

struct osito_queue;

struct osito_device {
    VK_LOADER_DATA             loader_data;
    struct osito_icd_inst     *owner;
    VkDevice                   real;
    struct osito_queue        *queues;
};

/* W3b.3 — non-dispatchable wrappers. These don't carry VK_LOADER_DATA
 * because non-dispatchable handles are plain u64 as far as Vulkan is
 * concerned. We still allocate a heap struct so the handle encodes a
 * pointer the loader can follow, and we rely on the owning device to
 * dispatch to the correct ICD. Same wrapper-leak caveat as W3b.2 —
 * cleaned up in W3b.4. */
struct osito_memory {
    struct osito_device *owner;
    VkDeviceMemory        real;
    VkDeviceSize          allocation_size;
    uint32_t              memory_type_index;
    void                 *mapped_data;
    VkDeviceSize          mapped_offset;
    VkDeviceSize          mapped_size;
};

struct osito_buffer {
    struct osito_device *owner;
    VkBuffer              real;
    VkDeviceSize          size;
    struct osito_memory  *memory;
    VkDeviceSize          memory_offset;
    uint8_t              *trace_shadow;
    VkDeviceSize          trace_shadow_size;
};

/* W3b.4 — remaining non-dispatchable wrappers.
 *
 * Same shape as W3b.3: {owner, real}. Same leak caveat (app must destroy
 * before DestroyDevice). */
struct osito_shader {
    struct osito_device *owner;
    VkShaderModule        real;
};
struct osito_render_pass {
    struct osito_device *owner;
    VkRenderPass          real;
};
struct osito_image {
    struct osito_device *owner;
    VkImage               real;
    uint32_t              width;
    uint32_t              height;
    VkFormat              format;
    VkImageUsageFlags     usage;
    struct osito_memory  *memory;
    VkDeviceSize          memory_offset;
    VkDeviceSize          memory_size;
    VkDeviceSize          memory_alignment;
    struct osito_image   *trace_next;
    uint32_t              trace_staging_memory_type;
    uint8_t               trace_dashboard;
};
struct osito_image_view {
    struct osito_device *owner;
    VkImageView           real;
    VkImage               image;
    struct osito_image   *image_wrapper;
    uint32_t              width;
    uint32_t              height;
};
struct osito_buffer_view {
    struct osito_device *owner;
    VkBufferView          real;
};
struct osito_framebuffer {
    struct osito_device *owner;
    VkFramebuffer         real;
};
struct osito_pipeline_layout {
    struct osito_device *owner;
    VkPipelineLayout      real;
};
struct osito_pipeline {
    struct osito_device *owner;
    VkPipeline            real;
};
struct osito_cmd_pool {
    struct osito_device *owner;
    VkCommandPool         real;
};
struct osito_descriptor_set;
struct osito_trace_draw {
    struct osito_buffer  *vertex_buffers[2];
    VkDeviceSize          vertex_offsets[2];
    VkDeviceSize          vertex_strides[2];
    struct osito_buffer  *index_buffer;
    struct osito_descriptor_set *descriptor_set;
    VkDeviceSize          index_offset;
    VkIndexType           index_type;
    VkImage               image;
    VkImage               submit_image;
    uint64_t              record_hash;
    uint32_t              sequence;
    uint32_t              element_count;
    uint32_t              first_element;
    uint32_t              descriptor_generation;
    uint32_t              submit_descriptor_generation;
    int32_t               vertex_offset;
    uint8_t               indexed;
    uint8_t               valid;
};
struct osito_trace_image_copy {
    struct osito_buffer  *buffer;
    struct osito_image   *image;
    VkDeviceSize          buffer_offset;
    uint32_t              row_length;
    uint64_t              record_hashes[4];
    uint8_t               valid;
};
/* Command buffer is a dispatchable handle — VK_LOADER_DATA first. */
struct osito_cmd_buffer {
    VK_LOADER_DATA        loader_data;
    struct osito_device  *owner;
    VkCommandBuffer       real;
    struct osito_descriptor_set *trace_graphics_set2;
    uint32_t               trace_draw_sequence;
    struct osito_buffer   *trace_vertex_buffers[4];
    VkDeviceSize           trace_vertex_offsets[4];
    VkDeviceSize           trace_vertex_strides[4];
    struct osito_buffer   *trace_index_buffer;
    VkDeviceSize           trace_index_offset;
    VkIndexType            trace_index_type;
    uint32_t               trace_dashboard_draw_count;
    struct osito_trace_draw trace_draw_samples[4];
    struct osito_trace_image_copy trace_image_copies[2];
};

/* W3b.5 — WSI + sync + queue wrappers. Surface is instance-scoped;
 * queue is dispatchable; fence/sema/swapchain are non-dispatchable. */
struct osito_surface {
    struct osito_instance *owner_inst;
    struct osito_icd_inst *owner_icd;
    VkSurfaceKHR           real;
};
/* VkQueue is dispatchable — carries VK_LOADER_DATA. */
struct osito_queue {
    VK_LOADER_DATA        loader_data;
    struct osito_device  *owner;
    VkQueue               real;
    uint32_t              family_index;
    struct osito_queue   *next;
};
struct osito_fence {
    struct osito_device  *owner;
    VkFence               real;
};
struct osito_semaphore {
    struct osito_device  *owner;
    VkSemaphore           real;
};
struct osito_swapchain {
    struct osito_device  *owner;
    VkSwapchainKHR        real;
    uint32_t              width;
    uint32_t              height;
};

/* Wave 3 (W4.10) — wrappers for descriptor / sampler / event /
 * pipeline-cache / query-pool / compute-pipeline objects. All non-
 * dispatchable, all share the {owner, real} shape. Used by the new
 * loader trampolines in loader_dispatch.c. */
struct osito_descriptor_set_layout {
    struct osito_device  *owner;
    VkDescriptorSetLayout real;
};
struct osito_descriptor_pool {
    struct osito_device  *owner;
    VkDescriptorPool      real;
};
struct osito_descriptor_set {
    struct osito_device  *owner;
    VkDescriptorSet       real;
    VkImageView           image_view;
    VkImage               image;
    VkSampler             sampler;
    VkImageLayout         image_layout;
    struct osito_image   *trace_image;
    uint32_t              image_width;
    uint32_t              image_height;
    uint32_t              image_binding;
    uint32_t              image_array_element;
    uint32_t              image_generation;
    uint32_t              logged_image_generation;
};
struct osito_descriptor_update_template {
    struct osito_device        *owner;
    VkDescriptorUpdateTemplate  real;
    VkDescriptorUpdateTemplateType template_type;
    VkPipelineBindPoint         pipeline_bind_point;
    uint32_t                    entry_count;
    VkDescriptorUpdateTemplateEntry entries[];
};
struct osito_event {
    struct osito_device  *owner;
    VkEvent               real;
};
struct osito_pipeline_cache {
    struct osito_device  *owner;
    VkPipelineCache       real;
};
struct osito_query_pool {
    struct osito_device  *owner;
    VkQueryPool           real;
};
struct osito_sampler {
    struct osito_device  *owner;
    VkSampler             real;
};

static inline struct osito_phys_device *osito_phys_from(VkPhysicalDevice h) {
    return (struct osito_phys_device *)h;
}
static inline VkPhysicalDevice osito_phys_to(struct osito_phys_device *s) {
    return (VkPhysicalDevice)s;
}
static inline struct osito_device *osito_device_from(VkDevice h) {
    return (struct osito_device *)h;
}
static inline VkDevice osito_device_to(struct osito_device *s) {
    return (VkDevice)s;
}

/* From loader_dispatch.c. */
PFN_vkVoidFunction osito_loader_get_instance_proc_addr(VkInstance, const char *);

/* Minimal libc bridges — real symbols live in OsitoK libc (tcclib.c + malloc.c). */
extern void *malloc(size_t n);
extern void *calloc(size_t n, size_t size);
extern void  free(void *p);
extern void *memset(void *p, int c, size_t n);
extern void *memcpy(void *d, const void *s, size_t n);
extern int   strcmp(const char *a, const char *b);

#endif
