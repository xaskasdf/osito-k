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

struct osito_device {
    VK_LOADER_DATA             loader_data;
    struct osito_icd_inst     *owner;
    VkDevice                   real;
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
};

struct osito_buffer {
    struct osito_device *owner;
    VkBuffer              real;
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
extern void  free(void *p);
extern void *memset(void *p, int c, size_t n);
extern void *memcpy(void *d, const void *s, size_t n);
extern int   strcmp(const char *a, const char *b);

#endif
