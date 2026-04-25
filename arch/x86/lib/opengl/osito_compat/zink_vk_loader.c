/* W4.4 — vk_dispatch_table bridge for Zink ↔ libvulkan loader.
 *
 * Zink's zink_create_screen() expects to discover Vulkan via dlopen()
 * + dlsym() of "libvulkan.so" — picking up vkGetInstanceProcAddr from
 * the host loader. On OsitoK we statically link our own libvulkan.a
 * (W2 loader + venus ICD), so dlopen is meaningless. We provide thin
 * overrides for util_dl_open() and util_dl_get_proc_address() that
 * resolve the two symbols zink actually queries:
 *
 *     "vkGetInstanceProcAddr"  -> &vkGetInstanceProcAddr   (loader_dispatch.c)
 *     "vkGetDeviceProcAddr"    -> &vkGetDeviceProcAddr     (loader_dispatch.c)
 *
 * Once zink has those two function pointers it walks Mesa's generated
 * vk_dispatch_table_load() helpers — no further bridge work needed.
 *
 * The util_dl_close() / util_dl_error() default-NULL implementations
 * from u_dl.c (under the freestanding #else branch) are still compiled
 * in libmesa_util.a; they're a no-op for our use case so we let them be.
 *
 * NOTE — symbol collision strategy:
 *   u_dl.c (static archive member of libmesa_util.a) defines the same
 *   util_dl_open/util_dl_get_proc_address with a NULL/NULL body. Our
 *   bridge object provides a stronger definition. With
 *   --allow-multiple-definition (already the link-time policy in every
 *   vulkan-tests/[name]/Makefile) the linker picks the first archive object
 *   it processes; placing libmesa_zink_loader.a (this object) BEFORE
 *   libmesa_util.a in the link line is what makes the override win.
 */

#define DUMMY_VK_LOADER_HANDLE ((struct util_dl_library *)(uintptr_t)0xCAFEBABEU)

#include <stdint.h>
#include <string.h>
#include "util/u_dl.h"

#include "vulkan/vulkan_core.h"

/* These come from arch/x86/lib/vulkan/loader/loader_dispatch.c (linked
 * in via libvulkan.a). They have C linkage and standard Vulkan calling
 * conventions. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance, const char *pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device, const char *pName);

struct util_dl_library *
util_dl_open(const char *filename)
{
    /* Zink only ever opens VK_LIBNAME ("libvulkan.so"/"vulkan-1.dll").
     * Other paths return NULL — preserves the upstream contract that
     * a NULL return means "library not found". */
    if (!filename)
        return NULL;

    /* Be permissive on the name; some versions of Zink also try
     * "libvulkan.so.1" depending on platform.  We accept anything that
     * starts with "libvulkan" or "vulkan-1". */
    if (strncmp(filename, "libvulkan", 9) == 0 ||
        strncmp(filename, "vulkan-1", 8)  == 0)
        return DUMMY_VK_LOADER_HANDLE;

    return NULL;
}

util_dl_proc
util_dl_get_proc_address(struct util_dl_library *library,
                         const char *procname)
{
    if (library != DUMMY_VK_LOADER_HANDLE || procname == NULL)
        return (util_dl_proc)0;

    /* Two entry points zink_screen.c queries directly. */
    if (strcmp(procname, "vkGetInstanceProcAddr") == 0)
        return (util_dl_proc)(uintptr_t)&vkGetInstanceProcAddr;
    if (strcmp(procname, "vkGetDeviceProcAddr") == 0)
        return (util_dl_proc)(uintptr_t)&vkGetDeviceProcAddr;

    /* Anything else: fall back to the instance-level loader (NULL
     * VkInstance is legal for global commands per the spec). This
     * lets zink resolve any extra symbols it might decide to look up
     * directly via util_dl_get_proc_address rather than the dispatch
     * table machinery. */
    return (util_dl_proc)(uintptr_t)
           vkGetInstanceProcAddr(VK_NULL_HANDLE, procname);
}
