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
bool              vgpu_has_feature(uint32_t feature);

/* Query a negotiated renderer capset. Returns its byte count or a negative
 * error. The caller supplies the destination and its capacity. */
int               vgpu_get_capset(uint32_t capset_id, void *data,
                                   uint32_t capacity);

/* Read a u32 from the GPU's PCI config via ECAM. */
uint32_t          vgpu_ecam_read32(uint16_t offset);
uint64_t          vgpu_hostmem_base(void);
uint64_t          vgpu_hostmem_size(void);

/* Submit a command buffer on the control virtqueue and wait for the
 * response synchronously. Returns 0 on success, <0 on error. */
int vgpu_controlq_submit(const void *cmd, uint32_t cmd_len,
                         void *resp, uint32_t resp_len);

/* Allocate a host-owned, CPU-mappable blob in the virtio-gpu host-visible
 * aperture. The resource belongs to ctx_id and must be released with
 * vgpu_blob_destroy(). */
int vgpu_blob_create_map(uint32_t ctx_id, uint64_t size, uint64_t blob_id,
                          uint32_t *resource_id, void **mapping);
int vgpu_blob_destroy(uint32_t resource_id);

#endif
