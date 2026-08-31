/* W4.4 — okGLZinkCreateScreen: OsitoK-specific zink screen entry.
 *
 * Wraps zink_create_screen() with init order tailored to OsitoK:
 *   1. Provide a minimal pipe_screen_config (NULL DRI options).
 *   2. Pass NULL for sw_winsys — we rely on Zink's internal "swrast"
 *      fallback path when winsys==NULL; eventually this will plug into
 *      our compositor surface (VK_OSITOK_compositor_surface) but the
 *      W4.4 task is just to verify the screen object materialises.
 *   3. Forward to zink_create_screen.
 *
 * The (instance, physical_device) parameters are RESERVED for the
 * future code path that pre-creates the Vulkan instance/device on
 * the application's behalf and passes them into zink. Today
 * zink_create_screen owns its own Vulkan stack and opens it via the
 * util_dl_* bridge in zink_vk_loader.c. We accept the parameters so
 * the public ABI doesn't churn when we wire that path through.
 *
 * Returns the pipe_screen* on success, NULL on failure.
 */

#include <stdint.h>
#include <stddef.h>

/* p_screen.h pulls pipe_screen + pipe_screen_config */
#include "pipe/p_screen.h"

/* zink_public.h declares zink_create_screen */
#include "zink_public.h"

#include "vulkan/vulkan_core.h"

struct pipe_screen *
okGLZinkCreateScreen(VkInstance instance, VkPhysicalDevice phys)
{
    /* Minimal config — no DRI options means Zink falls back to defaults
     * for everything driconf-controlled. */
    struct pipe_screen_config config = {
        .driver_name_is_inferred = false,
        .options                 = NULL,
        .options_info            = NULL,
    };

    /* Suppress unused warnings until the precreate path lands. */
    (void)instance;
    (void)phys;

    /* Venus can expose its virtual renderer as a CPU Vulkan device even
     * when the host transport is accelerated. The OsitoK entry accepts
     * that device explicitly without changing Zink's standard policy. */
    return zink_ositok_create_screen(&config);
}
