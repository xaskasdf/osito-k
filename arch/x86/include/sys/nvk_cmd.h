/*
 * NVK command-buffer byte-stream opcodes (shared kernel ↔ userland).
 *
 * The userland Vulkan ICD (lib/vulkan/icd/nvk_stub) records each Cmd*
 * call as a packed struct in a per-cmdbuf byte buffer. SYS_GPU_SUBMIT
 * hands that buffer to the kernel, which walks the opcodes and emits
 * the corresponding GPU work via the existing gsp_compute_dispatch /
 * gsp_ce_copy primitives.
 *
 * Wire format: each entry begins with `uint32_t op` (NVK_CMD_*) and is
 * followed by op-specific payload. Payloads are fixed-size structs (no
 * variable-length data) so the kernel can advance by sizeof(struct).
 *
 * Both sides include this header and use the same struct definitions —
 * keeps the protocol from drifting.
 */
#ifndef OSITOK_NVK_CMD_H
#define OSITOK_NVK_CMD_H

#include "../types.h"

enum nvk_cmd_op {
    NVK_CMD_CLEAR_COLOR_IMAGE    = 1,
    NVK_CMD_COPY_IMAGE_TO_BUFFER = 2,
    NVK_CMD_COPY_BUFFER          = 3,
    NVK_CMD_FILL_BUFFER          = 4,
};

struct nvk_cmd_clear_color {
    uint32_t op;            /* NVK_CMD_CLEAR_COLOR_IMAGE */
    uint32_t res_id;        /* image resource handle */
    uint32_t width;
    uint32_t height;
    uint32_t color_rgba;    /* packed BGRA8888 */
    uint32_t reserved;
};

struct nvk_cmd_copy_i2b {
    uint32_t op;            /* NVK_CMD_COPY_IMAGE_TO_BUFFER */
    uint32_t src_res_id;
    uint32_t dst_res_id;
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
};

struct nvk_cmd_copy_buffer {
    uint32_t op;            /* NVK_CMD_COPY_BUFFER */
    uint32_t src_res_id;
    uint32_t dst_res_id;
    uint32_t src_offset;
    uint32_t dst_offset;
    uint32_t size;
};

struct nvk_cmd_fill_buffer {
    uint32_t op;            /* NVK_CMD_FILL_BUFFER */
    uint32_t res_id;
    uint32_t offset;
    uint32_t size;          /* bytes (multiple of 4) */
    uint32_t value;         /* uint32 fill pattern */
    uint32_t reserved;
};

#endif /* OSITOK_NVK_CMD_H */
