/*
 * OsitoK — GPU 3D syscall ABI (Vulkan Phase 1)
 *
 * This header is shared between the kernel (drivers/virtio_gpu_3d.c,
 * kernel/syscall.c) and future libvulkan.a (arch/x86/lib/vulkan/).
 *
 * All syscalls return a non-negative value on success, or -errno on failure
 * (kernel convention; SYSCALL/SYSRET returns value in RAX).
 */
#ifndef OSITOK_GPU_SYSCALLS_H
#define OSITOK_GPU_SYSCALLS_H

#include "../types.h"

/* -- Syscall numbers (600..608) ------------------------------ */
#define SYS_GPU_CAPS         600
#define SYS_GPU_CTX_CREATE   601
#define SYS_GPU_CTX_DESTROY  602
#define SYS_GPU_RES_CREATE   603
#define SYS_GPU_RES_MAP      604
#define SYS_GPU_SUBMIT       605
#define SYS_GPU_FENCE_WAIT   606
#define SYS_GPU_PRESENT      607
#define SYS_GPU_RES_DESTROY  608

/* -- SYS_GPU_CAPS: bitfield of available backends ------------ */
#define GPU_CAP_VENUS_READY  (1u << 0)  /* virtio-gpu with VIRGL feature */
#define GPU_CAP_NVK_READY    (1u << 1)  /* native NVIDIA GSP backend */

/* -- SYS_GPU_CTX_CREATE flags -------------------------------- */
#define GPU_CTX_VENUS        0x00u      /* venus protocol context */
#define GPU_CTX_NVK          0x01u      /* NVK native backend */

/* -- SYS_GPU_RES_CREATE args --------------------------------- */
#define GPU_RES_KIND_BUFFER  0u
#define GPU_RES_KIND_IMAGE2D 1u

#define GPU_RES_FLAG_HOST_COHERENT (1u << 0)  /* map writable, no flush */
#define GPU_RES_FLAG_TRANSFER_SRC  (1u << 1)
#define GPU_RES_FLAG_TRANSFER_DST  (1u << 2)

struct gpu_res_create_args {
    uint32_t kind;       /* GPU_RES_KIND_* */
    uint32_t flags;      /* GPU_RES_FLAG_* OR-mask */
    uint32_t format;     /* venus format code, opaque to kernel */
    uint32_t width;      /* IMAGE2D only; 0 for BUFFER */
    uint32_t height;     /* IMAGE2D only; 0 for BUFFER */
    uint32_t pitch;      /* IMAGE2D only; 0 = tightly packed */
    uint64_t size;       /* BUFFER only; 0 for IMAGE2D */
    uint64_t blob_id;    /* Venus object ID, or 0 for generic blobs */
};

/* -- SYS_GPU_SUBMIT: single entrypoint for venus command streams -- */
struct gpu_submit_args {
    uint32_t   ctx_id;
    uint32_t   reserved;
    const void *cmd_bytes;   /* encoded venus ring data */
    uint64_t   cmd_len;      /* bytes */
    uint64_t  *out_fence;    /* written by kernel; 0 on error */
};

/* -- SYS_GPU_PRESENT: bridge to compositor SHM surface ------- */
struct gpu_present_args {
    uint32_t ctx_id;
    uint32_t res_id;
    uint32_t shm_handle;     /* from SYS_SHM_MKSURFACE (506) */
    uint32_t reserved;
};

#endif /* OSITOK_GPU_SYSCALLS_H */
