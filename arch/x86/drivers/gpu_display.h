/*
 * OsitoK x86-64 — GPU Display Engine (Phase A: GOP Takeover)
 *
 * Takes ownership of the NVIDIA display engine from UEFI GOP firmware,
 * allocates display channels via GSP-RM, and configures scanout to point
 * at the existing GOP framebuffer.  Provides page-flip and vblank support.
 *
 * Supports Turing (RTX 20xx), Ampere (RTX 30xx), and Ada (RTX 40xx).
 * Uses NVDisplay (Volta+) register layout and method classes.
 *
 * Reference: nouveau drivers/gpu/drm/nouveau/nvkm/engine/disp/,
 *            open-gpu-doc classes/display/, envytools.
 */

#ifndef OSITOK_GPU_DISPLAY_H
#define OSITOK_GPU_DISPLAY_H

#include "../include/types.h"
#include "gpu.h"

/* ── PDISP Frontend Registers (Volta+, BAR0 offsets) ───────── */

#define NV_PDISP_FE_HW_SYS_CAP       0x00610060  /* heads[7:0], SORs[15:8] */
#define NV_PDISP_FE_HW_SYS_CAPB      0x00610064  /* windows[31:0] */
#define NV_PDISP_FE_MISC_CONFIGA      0x00610074  /* heads(3:0), SORs(11:8), wins(25:20) */
#define NV_PDISP_FE_INST_MEM0         0x00610010  /* instance memory target */
#define NV_PDISP_FE_INST_MEM1         0x00610014  /* instance memory address */
#define NV_PDISP_FE_CHNCTL_CORE       0x006104E0  /* core channel control */
#define NV_PDISP_FE_CHNCTL_WIN(i)     (0x006104E4 + (i)*4)
#define NV_PDISP_FE_CHNCTL_CURS(i)    (0x00610604 + (i)*4)
#define NV_PDISP_OWNERSHIP            0x006254E8  /* firmware ownership */

/* Push buffer configuration (per channel, 73 slots) */
#define NV_PDISP_FE_PBBASEHI(i)       (0x00610B20 + (i)*16)  /* addr[38:32] */
#define NV_PDISP_FE_PBBASE(i)         (0x00610B24 + (i)*16)  /* target + addr[31:4] */
#define NV_PDISP_FE_PBSUBDEV(i)       (0x00610B28 + (i)*16)
#define NV_PDISP_FE_PBCLIENT(i)       (0x00610B2C + (i)*16)

/* Channel status */
#define NV_PDISP_FE_CHNSTATUS_CORE    0x00610630
#define NV_PDISP_FE_CHNSTATUS_WIN(i)  (0x00610664 + (i)*4)

/* SOR armed state (protocol/owner readback) */
#define NV_PDISP_SOR_STATE(i)         (0x00610B70 + (i)*8)

/* Head vblank counter (Volta+) */
#define NV_PDISP_RG_DPCLK(head)       (0x00616300 + (head)*0x800)

/* ── Display Root Class IDs ────────────────────────────────── */

#define TU102_DISP                     0xC570
#define GA102_DISP                     0xC670
#define AD102_DISP                     0xC770

/* Core Channel DMA Class IDs */
#define TU102_DISP_CORE_CHANNEL_DMA    0xC57D
#define GA102_DISP_CORE_CHANNEL_DMA    0xC67D
#define AD102_DISP_CORE_CHANNEL_DMA    0xC77D

/* Window Channel DMA Class IDs */
#define TU102_DISP_WINDOW_CHANNEL_DMA  0xC57E
#define GA102_DISP_WINDOW_CHANNEL_DMA  0xC67E

/* Cursor Class IDs */
#define TU102_DISP_CURSOR              0xC57A
#define GA102_DISP_CURSOR              0xC67A

/* NV04_DISPLAY_COMMON (display system object) */
#define NV04_DISPLAY_COMMON            0x00730073

/* ── RM Handle Constants for Display ──────────────────────── */

#define GSP_RM_DISP_COMMON_HANDLE      0x00730000
#define GSP_RM_DISP_ROOT_HANDLE        0xD15B0000
#define GSP_RM_DISP_CORE_HANDLE        0xF1F00000  /* chan ID 0 */
#define GSP_RM_DISP_WIN0_HANDLE        0xF1F00001  /* chan ID 1 */

/* NV2080 engine type for display */
#define NV2080_ENGINE_TYPE_DISP        0x13

/* ── Core Channel Methods (NVC37D/NVC67D — Volta+) ────────── */

#define DISP_CORE_UPDATE                          0x0200
#define DISP_CORE_SET_CONTEXT_DMA_NOTIFIER        0x0208
#define DISP_CORE_SET_INTERLOCK_FLAGS             0x0214

/* Head timing methods (core channel, offset per head = 0x400) */
#define DISP_HEAD_SET_CONTROL(a)          (0x2008 + (a)*0x400)
#define DISP_HEAD_SET_PIXEL_CLOCK(a)      (0x200C + (a)*0x400)
#define DISP_HEAD_SET_RASTER_SIZE(a)      (0x2064 + (a)*0x400)
#define DISP_HEAD_SET_RASTER_SYNC_END(a)  (0x2068 + (a)*0x400)
#define DISP_HEAD_SET_RASTER_BLANK_END(a) (0x206C + (a)*0x400)
#define DISP_HEAD_SET_RASTER_BLANK_START(a) (0x2070 + (a)*0x400)

/* Head output config (core channel) */
#define DISP_HEAD_SET_CONTROL_OUTPUT_RESOURCE(a) (0x2060 + (a)*0x400)
#define DISP_HEAD_SET_DITHER_CONTROL(a)  (0x2018 + (a)*0x400)
#define DISP_HEAD_SET_PROCAMP(a)         (0x2000 + (a)*0x400)

/* SOR methods (core channel, offset per SOR = 0x20) */
#define DISP_SOR_SET_CONTROL(a)           (0x0300 + (a)*0x20)

/* ── Window Channel Methods (NVC37E/NVC67E — Volta+) ──────── */

#define DISP_WIN_SET_SIZE                  0x0224
#define DISP_WIN_SET_STORAGE               0x0228
#define DISP_WIN_SET_PARAMS                0x022C
#define DISP_WIN_SET_PLANAR_STORAGE(b)    (0x0230 + (b)*4)
#define DISP_WIN_SET_CONTEXT_DMA_ISO(b)   (0x0240 + (b)*4)
#define DISP_WIN_SET_OFFSET(b)            (0x0260 + (b)*4)
#define DISP_WIN_SET_POINT_IN(b)          (0x0290 + (b)*4)
#define DISP_WIN_SET_SIZE_IN               0x0298
#define DISP_WIN_SET_SIZE_OUT              0x02A4
#define DISP_WIN_UPDATE                    0x0200

/* ── Pixel Format Values ──────────────────────────────────── */

#define DISP_FMT_A8R8G8B8                  0xCF
#define DISP_FMT_A8B8G8R8                  0xD5
#define DISP_FMT_R5G6B5                    0xE8

/* ── Storage Layout ───────────────────────────────────────── */

#define DISP_STORAGE_PITCH                 (1 << 4)  /* MEMORY_LAYOUT = PITCH */

/* ── Display State ────────────────────────────────────────── */

typedef struct {
    /* Hardware capabilities */
    uint32_t  num_heads;
    uint32_t  num_sors;
    uint32_t  num_windows;
    uint32_t  head_mask;         /* bitmask of available heads */
    uint32_t  sor_mask;          /* bitmask of available SORs */
    uint32_t  win_mask;          /* bitmask of available windows */

    /* Active head/SOR for our display */
    uint32_t  active_head;       /* head index driving the display */
    uint32_t  active_sor;        /* SOR index driving the output */

    /* Display class IDs (selected per GPU generation) */
    uint32_t  disp_root_class;
    uint32_t  core_class;
    uint32_t  win_class;

    /* Push buffers (display channels use PDISP, not GPFIFO) */
    pushbuf_state_t core_pb;     /* core channel push buffer */
    pushbuf_state_t win_pb;      /* window channel push buffer */

    /* RM handles */
    uint32_t  disp_common_handle;
    uint32_t  disp_root_handle;
    uint32_t  core_handle;
    uint32_t  win0_handle;

    /* Framebuffer info (from GOP) */
    uint64_t  fb_addr;           /* physical address of scanout */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;             /* bytes per scanline */

    /* Current timing (read from armed registers) */
    uint32_t  pixel_clock_hz;
    uint32_t  htotal, vtotal;
    uint32_t  hsync_end, vsync_end;
    uint32_t  hblank_end, vblank_end;
    uint32_t  hblank_start, vblank_start;

    /* Runtime counters */
    uint32_t  flip_count;
    uint32_t  vblank_base;

    /* State flags */
    bool      ownership_claimed;
    bool      channels_allocated;
    bool      configured;
    bool      ready;
} display_state_t;

/* ── Display Class Selection ──────────────────────────────── */

/* Select display root class ID for the current GPU generation */
static inline uint32_t disp_root_class_for_gen(gpu_gen_t gen)
{
    switch (gen) {
    case GPU_GEN_TURING:       return TU102_DISP;
    case GPU_GEN_AMPERE:       return GA102_DISP;
    case GPU_GEN_ADA_LOVELACE: return AD102_DISP;
    default:                   return GA102_DISP;
    }
}

/* Select core channel DMA class ID */
static inline uint32_t disp_core_class_for_gen(gpu_gen_t gen)
{
    switch (gen) {
    case GPU_GEN_TURING:       return TU102_DISP_CORE_CHANNEL_DMA;
    case GPU_GEN_AMPERE:       return GA102_DISP_CORE_CHANNEL_DMA;
    case GPU_GEN_ADA_LOVELACE: return AD102_DISP_CORE_CHANNEL_DMA;
    default:                   return GA102_DISP_CORE_CHANNEL_DMA;
    }
}

/* Select window channel DMA class ID */
static inline uint32_t disp_win_class_for_gen(gpu_gen_t gen)
{
    switch (gen) {
    case GPU_GEN_TURING:       return TU102_DISP_WINDOW_CHANNEL_DMA;
    case GPU_GEN_AMPERE:       return GA102_DISP_WINDOW_CHANNEL_DMA;
    case GPU_GEN_ADA_LOVELACE: return GA102_DISP_WINDOW_CHANNEL_DMA;
    default:                   return GA102_DISP_WINDOW_CHANNEL_DMA;
    }
}

/* ── Public API ───────────────────────────────────────────── */

/* Initialize display engine: claim ownership, allocate channels,
 * configure scanout to point at the existing GOP framebuffer.
 * gop_fb:  physical address of the GOP framebuffer.
 * width:   horizontal resolution in pixels.
 * height:  vertical resolution in pixels.
 * pitch:   bytes per scanline (width * bpp / 8, may be padded). */
int gpu_display_init(uint32_t *gop_fb, uint32_t width,
                     uint32_t height, uint32_t pitch);

/* Flip display to a new framebuffer address.
 * fb_addr: physical address of the new scanout surface. */
int gpu_display_flip(uint64_t fb_addr);

/* Return total vblank count for the active head. */
uint32_t gpu_display_vblank_count(void);

/* Return 1 if the display engine is fully initialized. */
int gpu_display_is_ready(void);

/* Get display state (for diagnostics). */
display_state_t *gpu_display_get_state(void);

#endif /* OSITOK_GPU_DISPLAY_H */
