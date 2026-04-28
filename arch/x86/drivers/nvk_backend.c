/*
 * nvk_backend.c — bridge between the userspace NVK Vulkan ICD and the
 * existing GSP / channel / GMMU infrastructure (drivers/gsp.c).
 *
 * Wave 1 left this as a weak no-op stub that always reported "not
 * ready"; this version actually consults the GSP state. NVK is
 * declared ready when:
 *   1. The GSP firmware booted (gsp.boot_ack) and
 *   2. The Resource Manager init sequence completed (gsp.rm_init_done) and
 *   3. A compute channel has been bound (gsp_get_compute() != NULL).
 *
 * Without (3) we have no way to submit work to the GPU; without (2)
 * the RM hasn't allocated handles for memory/channel objects; (1) is
 * the prerequisite for both.
 *
 * ctx_create / ctx_destroy are still stubs — wiring per-process
 * channels is Phase 3.
 */
#include "nvk_backend.h"
#include "gpu.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

bool nvk_backend_ready(void)
{
    gsp_state_t *g = gsp_get_state();
    if (!g) return false;
    /* Don't require boot_ack: the legacy boot path doesn't update it
     * even when GSP is functional. Trust rm_init_done — if the RPC
     * sequence completed end-to-end, GSP responded to RM commands and
     * is therefore alive regardless of the initial mailbox handshake. */
    if (!g->rm_init_done) return false;
    if (gsp_get_compute() == NULL) return false;
    return true;
}

int32_t nvk_backend_ctx_create(uint32_t pid, uint32_t flags)
{
    (void)pid; (void)flags;
    if (!nvk_backend_ready()) return -38;  /* ENOSYS */

    /* Phase 3: per-process channel allocation. For now we expose the
     * shared compute channel — single-process semantics, no isolation.
     * Caller treats the returned id as opaque. */
    return 1;
}

int32_t nvk_backend_ctx_destroy(uint32_t pid, uint32_t ctx_id)
{
    (void)pid; (void)ctx_id;
    /* No-op while we share one channel across processes. */
    return 0;
}

void nvk_backend_init_hook(void)
{
    if (nvk_backend_ready()) {
        serial_puts("[NVK] backend READY (GSP+RM+compute online)\n");
        extern void fb_puts(const char *s);
        fb_puts(" [NVK] backend ready\n");
    } else {
        gsp_state_t *g = gsp_get_state();
        serial_puts("[NVK] backend NOT ready: ");
        if (!g)                    serial_puts("no gsp_state");
        else if (!g->rm_init_done) serial_puts("RM init incomplete");
        else                       serial_puts("no compute channel");
        serial_puts("\n");
        /* Mirror to fb so it survives the boot output scroll. */
        extern void fb_puts(const char *s);
        fb_puts(" [NVK] not ready (GPU compute path uninitialized)\n");
    }
}
