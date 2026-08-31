/*
 * Public virtio-gpu 2D scanout API used by the x86 display subsystem.
 */
#ifndef OSITOK_DRIVERS_VIRTIO_GPU_H
#define OSITOK_DRIVERS_VIRTIO_GPU_H

#include "../types.h"

void      virtio_gpu_init(uint64_t ecam, uint8_t bus, uint8_t dev,
                          uint8_t func, uint32_t fb_width,
                          uint32_t fb_height);
bool      virtio_gpu_ready(void);
bool      virtio_gpu_is_vga_compatible(void);
uint32_t *virtio_gpu_get_fb(void);
uint32_t  virtio_gpu_get_width(void);
uint32_t  virtio_gpu_get_height(void);
bool      virtio_gpu_get_native_mode(uint32_t *width, uint32_t *height);
void      virtio_gpu_flush(void);
int       virtio_gpu_resize(uint32_t width, uint32_t height);

#endif
