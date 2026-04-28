/*
 * nvk_backend.h — public API for the kernel-side NVK Vulkan backend.
 *
 * Called from kernel/syscall.c when SYS_GPU_* receives a request from a
 * userland process and nvk_backend_ready() is true (== bare-metal NVIDIA
 * GSP path is online).
 */
#ifndef OSITOK_NVK_BACKEND_H
#define OSITOK_NVK_BACKEND_H

#include "../include/types.h"

struct gpu_res_create_args;

/* Returns true once GSP firmware is booted, RM init RPC sequence has
 * completed, and a compute channel is bound. Until then SYS_GPU_*
 * dispatch falls back to vg3d_* (virtio-gpu, only useful in QEMU). */
bool nvk_backend_ready(void);

/* Per-process context lifecycle. ctx_create returns an opaque ctx_id
 * (>= 1) or -errno. ctx_destroy frees the per-pid resource table and
 * any leftover VRAM allocations. */
int32_t nvk_backend_ctx_create(uint32_t pid, uint32_t flags);
int32_t nvk_backend_ctx_destroy(uint32_t pid, uint32_t ctx_id);

/* Resource lifecycle. res_create allocates VRAM via gmmu_alloc_vram
 * and returns a per-pid res_id. res_map returns the kernel virtual
 * address (which is also valid in the userspace process because the
 * upper-half PML4[256] is shared across all PML4s). */
int32_t nvk_backend_res_create(uint32_t pid, uint32_t ctx_id,
                                const struct gpu_res_create_args *a);
int64_t nvk_backend_res_map(uint32_t pid, uint32_t res_id);

/* Submit: walk the userland-recorded command-buffer byte stream
 * (NVK_CMD_* opcodes from include/sys/nvk_cmd.h) and emit the work.
 * Returns 0 on success and writes the issued fence to *out_fence. */
int32_t nvk_backend_submit(uint32_t pid, uint32_t ctx_id,
                            const uint8_t *cmd_bytes, uint32_t cmd_len,
                            uint64_t *out_fence);

int32_t nvk_backend_fence_wait(uint64_t fence, uint64_t timeout_ns);

/* Present: copy a VRAM image to a compositor SHM surface and trigger
 * the compositor's flip. shm_handle comes from SYS_SHM_MKSURFACE. */
int32_t nvk_backend_present(uint32_t pid, uint32_t ctx_id,
                             uint32_t res_id, uint32_t shm_handle);

/* Boot hook: called once after gsp_boot completes; logs the NVK
 * readiness state to serial + framebuffer. */
void nvk_backend_init_hook(void);

#endif
