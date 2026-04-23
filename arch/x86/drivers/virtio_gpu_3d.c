/*
 * virtio-gpu 3D driver -- Wave 1 skeleton.
 * Functions grow task-by-task. Until implemented, each returns -ENOSYS
 * and the corresponding selftest marker prints FAIL.
 */
#include "virtio_gpu_3d.h"
#include "virtio_gpu_internal.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

#define ENOSYS 38

static bool g_3d_ready = false;

void virtio_gpu_3d_init(void) {
    if (!vgpu_is_initialized()) {
        serial_puts("[VG3D] skipped (2D driver not initialized)\n");
        g_3d_ready = false;
        return;
    }
    uint64_t feat = vgpu_device_features();
    if (!(feat & (1ull << VIRTIO_GPU_F_VIRGL))) {
        serial_puts("[VG3D] skipped (VIRGL feature not offered by host)\n");
        g_3d_ready = false;
        return;
    }
    serial_puts("[VG3D] ready -- VIRGL negotiated\n");
    g_3d_ready = true;
}

uint32_t vg3d_caps(void) {
    return g_3d_ready ? GPU_CAP_VENUS_READY : 0u;
}

int32_t vg3d_ctx_create(uint32_t pid, uint32_t flags) { (void)pid; (void)flags; return -ENOSYS; }
int32_t vg3d_ctx_destroy(uint32_t pid, uint32_t ctx_id) { (void)pid; (void)ctx_id; return -ENOSYS; }
int32_t vg3d_res_create(uint32_t pid, uint32_t ctx_id,
                        const struct gpu_res_create_args *args) {
    (void)pid; (void)ctx_id; (void)args; return -ENOSYS;
}
uint64_t vg3d_res_map(uint32_t pid, uint32_t res_id) { (void)pid; (void)res_id; return 0; }
int32_t vg3d_submit(uint32_t pid, uint32_t ctx_id,
                    const void *cmd_bytes, uint64_t cmd_len,
                    uint64_t *out_fence) {
    (void)pid; (void)ctx_id; (void)cmd_bytes; (void)cmd_len; (void)out_fence;
    return -ENOSYS;
}
int32_t vg3d_fence_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return -ENOSYS; }
int32_t vg3d_present(uint32_t pid, uint32_t ctx_id, uint32_t res_id, uint32_t shm_handle) {
    (void)pid; (void)ctx_id; (void)res_id; (void)shm_handle; return -ENOSYS;
}
void vg3d_cleanup_process(uint32_t pid) { (void)pid; }

/* -- Self-test harness -------------------------------------- */

static void vg3d_t2_skeleton(void) {
    /* Task 2 passes if the skeleton links and this symbol is callable. */
    serial_puts("[VG3D-T2] skeleton OK\n");
}

static void vg3d_t3_virgl(void) {
    if (g_3d_ready) {
        serial_puts("[VG3D-T3] virgl-negotiated OK\n");
    } else {
        serial_puts("[VG3D-T3] virgl-negotiated FAIL (feature bit 0 not set)\n");
    }
}

void virtio_gpu_3d_selftest(void) {
    serial_puts("[VG3D] selftest begin\n");
    vg3d_t2_skeleton();
    vg3d_t3_virgl();
    /* Later tasks append more markers here. */
    serial_puts("[VG3D] selftest end\n");
}
