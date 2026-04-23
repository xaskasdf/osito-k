/*
 * nvk_backend.c -- Wave 1 stub backend.
 *
 * All entry points are weak no-ops returning "not ready" / -ENOSYS. The
 * real NVK/GSP driver will override these in a later wave.
 */
#include "nvk_backend.h"

extern void serial_puts(const char *s);

__attribute__((weak)) bool nvk_backend_ready(void) {
    return false;
}

__attribute__((weak)) int32_t nvk_backend_ctx_create(uint32_t pid, uint32_t flags) {
    (void)pid; (void)flags;
    return -38;  /* ENOSYS */
}

__attribute__((weak)) int32_t nvk_backend_ctx_destroy(uint32_t pid, uint32_t ctx_id) {
    (void)pid; (void)ctx_id;
    return -38;
}

void nvk_backend_init_hook(void) {
    serial_puts("[NVK] stub backend (0 devices)\n");
}
