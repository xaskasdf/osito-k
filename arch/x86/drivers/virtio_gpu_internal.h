/*
 * virtio_gpu_internal.h -- private handles exposed from virtio_gpu.c
 * to virtio_gpu_3d.c. NOT part of the public API.
 */
#ifndef OSITOK_VIRTIO_GPU_INTERNAL_H
#define OSITOK_VIRTIO_GPU_INTERNAL_H

#include "../include/types.h"

/* Accessor functions, not direct state exposure -- lets virtio_gpu.c
 * keep its `static struct gpu` and still collaborate with the 3D side. */
volatile uint8_t *vgpu_common_cfg(void);
volatile uint8_t *vgpu_notify_base(void);
uint32_t          vgpu_notify_off_mult(void);
bool              vgpu_is_initialized(void);

/* Negotiated device features from common_cfg[0x08..0x0C]. Populated by
 * virtio_gpu_init before status DRIVER_OK is set. */
uint64_t          vgpu_device_features(void);

/* Read a u32 from the GPU's PCI config via ECAM. */
uint32_t          vgpu_ecam_read32(uint16_t offset);
uint64_t          vgpu_hostmem_base(void);
uint64_t          vgpu_hostmem_size(void);

/* Submit a command buffer on the control virtqueue and wait for the
 * response synchronously. Returns 0 on success, <0 on error. */
int vgpu_controlq_submit(const void *cmd, uint32_t cmd_len,
                         void *resp, uint32_t resp_len);

#endif
