/*
 * OsitoK — Display modes syscall ABI.
 *
 * This is the small KMS-like surface used by user programs and shell tools to
 * inspect the active scanout mode. Runtime modeset is intentionally conservative:
 * backends may reject mode changes they cannot perform safely.
 */
#ifndef OSITOK_DISPLAY_SYSCALLS_H
#define OSITOK_DISPLAY_SYSCALLS_H

#include "../types.h"

/* Private display syscall numbers. Keep clear of GPU 600..607. */
#define SYS_DISPLAY_GET_MODE_COUNT    620
#define SYS_DISPLAY_GET_MODE          621
#define SYS_DISPLAY_GET_CURRENT_MODE  622
#define SYS_DISPLAY_SET_MODE          623

/* display_mode_info_t.backend */
#define DISPLAY_BACKEND_NONE     0u
#define DISPLAY_BACKEND_GOP      1u
#define DISPLAY_BACKEND_VIRTIO   2u
#define DISPLAY_BACKEND_NVIDIA   3u

/* display_mode_info_t.flags */
#define DISPLAY_MODE_CURRENT     (1u << 0)
#define DISPLAY_MODE_BOOT        (1u << 1)
#define DISPLAY_MODE_NATIVE      (1u << 2)
#define DISPLAY_MODE_HARDWARE    (1u << 3)

/* SYS_DISPLAY_SET_MODE flags */
#define DISPLAY_SET_NATIVE       (1u << 0)  /* use detected preferred EDID mode */
#define DISPLAY_SET_REFRESH_ONLY (1u << 1)  /* only update display pacing Hz */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;          /* pixels per scanline */
    uint32_t pixel_format;   /* 0 = RGBX, 1 = BGRX, backend-defined otherwise */
    uint32_t refresh_hz;     /* 0 when unknown */
    uint32_t flags;          /* DISPLAY_MODE_* */
    uint32_t backend;        /* DISPLAY_BACKEND_* */
    uint32_t reserved;
} display_mode_info_t;

#endif /* OSITOK_DISPLAY_SYSCALLS_H */
