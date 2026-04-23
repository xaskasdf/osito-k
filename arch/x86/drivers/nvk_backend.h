/*
 * nvk_backend.h -- placeholder for native GSP/SASS Vulkan ICD.
 *
 * In Wave 1 all functions are weak no-ops that return "not ready".
 * Wave 2 / Phase 2 will replace with real GSP dispatch.
 */
#ifndef OSITOK_NVK_BACKEND_H
#define OSITOK_NVK_BACKEND_H

#include "../include/types.h"

bool nvk_backend_ready(void);
int32_t nvk_backend_ctx_create(uint32_t pid, uint32_t flags);
int32_t nvk_backend_ctx_destroy(uint32_t pid, uint32_t ctx_id);

/* Boot hook: called once after virtio_gpu_3d_init. */
void nvk_backend_init_hook(void);

#endif
