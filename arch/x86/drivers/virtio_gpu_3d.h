/*
 * OsitoK -- virtio-gpu 3D extension (Vulkan Phase 1 / Wave 1)
 *
 * Builds on drivers/virtio_gpu.c. Adds VIRGL feature negotiation,
 * per-process GPU context tracking, resource table, and venus wire
 * command submission.
 *
 * Callers come from kernel/syscall.c (sys_gpu_* cases 600-607).
 */
#ifndef OSITOK_VIRTIO_GPU_3D_H
#define OSITOK_VIRTIO_GPU_3D_H

#include "../include/types.h"
#include "../include/sys/gpu_syscalls.h"

/* -- virtio-gpu 3D feature bit (spec, section 5.7.3) --------- */
#define VIRTIO_GPU_F_VIRGL         0
#define VIRTIO_GPU_F_EDID          1
#define VIRTIO_GPU_F_RESOURCE_UUID 2
#define VIRTIO_GPU_F_RESOURCE_BLOB 3
#define VIRTIO_GPU_F_CONTEXT_INIT  4

/* -- 3D command types (virtio-gpu-3d section) --------------- */
#define VIRTIO_GPU_CMD_CTX_CREATE            0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY           0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE   0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE   0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D    0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D   0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D             0x0207

/* -- Public API (called from kernel/syscall.c + main.c) ---- */

/* Called once after virtio_gpu_init(); probes VIRGL feature.
 * Emits `[VG3D] ready` if negotiated, or `[VG3D] skipped` otherwise. */
void virtio_gpu_3d_init(void);

/* Returns bitfield of GPU_CAP_* constants. */
uint32_t vg3d_caps(void);

/* Context table. Returns ctx_id >= 1 or -errno. */
int32_t vg3d_ctx_create(uint32_t pid, uint32_t flags);
int32_t vg3d_ctx_destroy(uint32_t pid, uint32_t ctx_id);

/* Resource create. Returns res_id >= 1 or -errno. */
int32_t vg3d_res_create(uint32_t pid, uint32_t ctx_id,
                        const struct gpu_res_create_args *args);

/* Map resource backing memory into caller's VA space.
 * Returns virtual address or 0 on error. */
uint64_t vg3d_res_map(uint32_t pid, uint32_t res_id);

/* Submit venus command bytes. Writes fence_id to *out_fence on success. */
int32_t vg3d_submit(uint32_t pid, uint32_t ctx_id,
                    const void *cmd_bytes, uint64_t cmd_len,
                    uint64_t *out_fence);

/* Wait on fence with timeout in nanoseconds (0 = poll). Returns 0 on
 * signaled, -ETIMEDOUT on timeout, -EINVAL on bad id. */
int32_t vg3d_fence_wait(uint64_t fence, uint64_t timeout_ns);

/* Bridge to compositor: push a resource's memory into the SHM surface
 * backing `shm_handle` and signal it dirty. */
int32_t vg3d_present(uint32_t pid, uint32_t ctx_id,
                     uint32_t res_id, uint32_t shm_handle);

/* Process teardown hook -- invoked from process_free(). */
void vg3d_cleanup_process(uint32_t pid);

/* Self-test: called from main.c after init. Emits named [VG3D-T*] markers. */
void virtio_gpu_3d_selftest(void);

#endif /* OSITOK_VIRTIO_GPU_3D_H */
