/*
 * virtio_modeset_abi_test.c -- compile-time ABI check for virtio display
 * resizing. This is a kernel-driver surface test: it verifies the public
 * prototype exists so display.c can call into the virtio backend.
 */

#include "../include/drivers/virtio_gpu.h"

typedef int (*virtio_resize_fn_t)(uint32_t width, uint32_t height);
typedef bool (*virtio_native_fn_t)(uint32_t *width, uint32_t *height);

void _start(void)
{
    volatile virtio_resize_fn_t resize = virtio_gpu_resize;
    volatile virtio_native_fn_t native = virtio_gpu_get_native_mode;
    (void)resize;
    (void)native;
    for (;;);
}
