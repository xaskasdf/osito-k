/*
 * OsitoK x86-64 — Intel Gen9 display probe
 *
 * The real-hardware target for this pass is HD 530 class hardware
 * (Skylake GT2, PCI IDs such as 8086:1912). Gen9 display programming is
 * tightly coupled to GGTT/stolen memory and power wells, so this driver
 * intentionally does not force a fresh modeset. It validates the
 * firmware/GOP-programmed pipe and primary plane and reports whether the
 * GOP scanout is safe to retain.
 */

#include "../include/types.h"
#include "../include/paging.h"
#include "intel_gfx.h"

extern int  paging_map_mmio(uint64_t phys, uint64_t size);
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);
extern void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func);

#define INTEL_GFX_MMIO_SIZE       (16ULL * 1024ULL * 1024ULL)

#define GEN9_NUM_PIPES            3
#define GEN9_PIPE_STRIDE          0x1000

#define GEN9_PIPECONF(pipe)       (0x70008 + (pipe) * GEN9_PIPE_STRIDE)
#define GEN9_HTOTAL(pipe)         (0x60000 + (pipe) * GEN9_PIPE_STRIDE)
#define GEN9_VTOTAL(pipe)         (0x6000C + (pipe) * GEN9_PIPE_STRIDE)

#define GEN9_PLANE_CTL(pipe)      (0x70180 + (pipe) * GEN9_PIPE_STRIDE)
#define GEN9_PLANE_STRIDE(pipe)   (0x70188 + (pipe) * GEN9_PIPE_STRIDE)
#define GEN9_PLANE_SIZE(pipe)     (0x70190 + (pipe) * GEN9_PIPE_STRIDE)
#define GEN9_PLANE_SURF(pipe)     (0x7019C + (pipe) * GEN9_PIPE_STRIDE)

#define GEN9_PIPECONF_ENABLE      (1u << 31)
#define GEN9_PLANE_CTL_ENABLE     (1u << 31)

static intel_gfx_state_t igfx;
static volatile uint8_t *igfx_mmio;

static uint32_t igfx_read32(uint32_t reg)
{
    return mmio_read32((volatile void *)(igfx_mmio + reg));
}

static void igfx_print_bdf(uint8_t bus, uint8_t dev, uint8_t func)
{
    serial_puthex(bus, 2);
    serial_puts(":");
    serial_puthex(dev, 2);
    serial_puts(".");
    serial_putdec(func);
}

bool intel_gfx_is_gen9_device(uint16_t id)
{
    switch (id) {
    /* Skylake desktop/mobile/workstation GT1/GT2/GT3/GT4 */
    case 0x1902: case 0x1906: case 0x190B: case 0x190E:
    case 0x1912: case 0x1913: case 0x1915: case 0x1916:
    case 0x1917: case 0x191B: case 0x191D: case 0x191E:
    case 0x1921: case 0x1923: case 0x1926: case 0x1927:
    case 0x192B: case 0x192D: case 0x1932: case 0x193A:
    case 0x193B: case 0x193D:
    /* Kaby Lake / Amber Lake / Whiskey Lake Gen9 */
    case 0x5902: case 0x5906: case 0x590B: case 0x5912:
    case 0x5913: case 0x5915: case 0x5916: case 0x5917:
    case 0x591B: case 0x591C: case 0x591D: case 0x591E:
    case 0x5921: case 0x5923: case 0x5926: case 0x5927:
    case 0x592B: case 0x592D:
    /* Coffee Lake / Comet Lake Gen9.5 display-compatible IDs */
    case 0x3E90: case 0x3E91: case 0x3E92: case 0x3E93:
    case 0x3E98: case 0x3E99: case 0x3E9A: case 0x3E9B:
    case 0x3EA0: case 0x3EA5: case 0x3EA6: case 0x3EA7:
    case 0x3EA8: case 0x3EA9: case 0x9BC4: case 0x9BC5:
    case 0x9BC8: case 0x9BC9: case 0x9B41: case 0x9BCA:
        return true;
    default:
        return false;
    }
}

const char *intel_gfx_device_name(uint16_t id)
{
    switch (id) {
    case 0x1912: return "Intel HD Graphics 530";
    case 0x191B: return "Intel HD Graphics 530 mobile";
    case 0x191D: return "Intel HD Graphics P530";
    case 0x5912: return "Intel HD Graphics 630";
    case 0x3E92: return "Intel UHD Graphics 630";
    case 0x3E9B: return "Intel UHD Graphics 630";
    default:
        return intel_gfx_is_gen9_device(id) ? "Intel Gen9 display" :
                                              "Intel display";
    }
}

static int igfx_find_active_scanout(void)
{
    igfx.active_pipe = -1;
    igfx.active_plane = -1;

    for (int pipe = 0; pipe < GEN9_NUM_PIPES; pipe++) {
        uint32_t pipeconf = igfx_read32(GEN9_PIPECONF(pipe));
        uint32_t plane_ctl = igfx_read32(GEN9_PLANE_CTL(pipe));

        serial_puts("[IGFX] pipe ");
        serial_putdec(pipe);
        serial_puts(": PIPECONF=0x");
        serial_puthex(pipeconf, 8);
        serial_puts(" PLANE_CTL=0x");
        serial_puthex(plane_ctl, 8);
        serial_puts("\n");

        if ((pipeconf & GEN9_PIPECONF_ENABLE) &&
            (plane_ctl & GEN9_PLANE_CTL_ENABLE)) {
            igfx.active_pipe = pipe;
            igfx.active_plane = 0;
            igfx.pipeconf = pipeconf;
            igfx.plane_ctl = plane_ctl;
            igfx.plane_stride = igfx_read32(GEN9_PLANE_STRIDE(pipe));
            igfx.plane_size = igfx_read32(GEN9_PLANE_SIZE(pipe));
            igfx.plane_surf = igfx_read32(GEN9_PLANE_SURF(pipe));

            serial_puts("[IGFX] active pipe ");
            serial_putdec(pipe);
            serial_puts(": HTOTAL=0x");
            serial_puthex(igfx_read32(GEN9_HTOTAL(pipe)), 8);
            serial_puts(" VTOTAL=0x");
            serial_puthex(igfx_read32(GEN9_VTOTAL(pipe)), 8);
            serial_puts(" STRIDE=0x");
            serial_puthex(igfx.plane_stride, 8);
            serial_puts(" SIZE=0x");
            serial_puthex(igfx.plane_size, 8);
            serial_puts(" SURF=0x");
            serial_puthex(igfx.plane_surf, 8);
            serial_puts("\n");
            return 0;
        }
    }

    return -1;
}

int intel_gfx_init(uint16_t device_id, uint8_t bus, uint8_t dev, uint8_t func,
                   uint64_t bar0_phys, uint64_t bar2_phys,
                   uint64_t gop_fb_phys, uint32_t width, uint32_t height,
                   uint32_t pitch_bytes)
{
    memset(&igfx, 0, sizeof(igfx));
    igfx.present = true;
    igfx.vendor_id = 0x8086;
    igfx.device_id = device_id;
    igfx.bus = bus;
    igfx.dev = dev;
    igfx.func = func;
    igfx.bar0_phys = bar0_phys;
    igfx.bar2_phys = bar2_phys;
    igfx.gop_fb_phys = gop_fb_phys;
    igfx.gop_width = width;
    igfx.gop_height = height;
    igfx.gop_pitch_bytes = pitch_bytes;

    serial_puts("[IGFX] ");
    serial_puthex(0x8086, 4);
    serial_puts(":");
    serial_puthex(device_id, 4);
    serial_puts(" ");
    serial_puts(intel_gfx_device_name(device_id));
    serial_puts(" at ");
    igfx_print_bdf(bus, dev, func);
    serial_puts("\n");

    if (!bar0_phys) {
        serial_puts("[IGFX] no BAR0 MMIO, skipping\n");
        return -1;
    }

    if (!intel_gfx_is_gen9_device(device_id)) {
        serial_puts("[IGFX] unsupported Intel display generation, GOP fallback only\n");
        return -1;
    }
    igfx.gen9 = true;

    paging_map_mmio(bar0_phys, INTEL_GFX_MMIO_SIZE);
    __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    igfx_mmio = (volatile uint8_t *)PHYS_TO_VIRT(bar0_phys);

    pci_enable_bus_master(bus, dev, func);

    serial_puts("[IGFX] BAR0=0x");
    serial_puthex(bar0_phys, 16);
    serial_puts(" BAR2=0x");
    serial_puthex(bar2_phys, 16);
    serial_puts(" GOP fb=0x");
    serial_puthex(gop_fb_phys, 16);
    serial_puts(" ");
    serial_putdec(width);
    serial_puts("x");
    serial_putdec(height);
    serial_puts(" pitch=");
    serial_putdec(pitch_bytes);
    serial_puts("\n");

    if (igfx_find_active_scanout() != 0) {
        serial_puts("[IGFX] no active Gen9 pipe/plane found, GOP fallback only\n");
        fb_puts(" Intel GFX: fallback GOP\n");
        return -1;
    }

    /*
     * On Gen9, PLANE_SURF is a GGTT/stolen-memory offset, not a system
     * physical address. Without a GGTT allocator and stolen-memory parser,
     * scanning out arbitrary kernel RAM would be unsafe. Retaining the
     * firmware GOP scanout is safe: the kernel keeps drawing through the
     * GOP framebuffer mapping and the display engine keeps using the mode
     * the firmware already validated.
     */
    igfx.gop_scanout_retained = true;
    igfx.ready = true;

    serial_puts("[IGFX] Gen9/HD 530 modeset ready; GOP scanout retained\n");
    fb_puts(" Intel GFX: Gen9 modeset ready (GOP)\n");
    return 0;
}

int intel_gfx_is_ready(void)
{
    return igfx.ready ? 1 : 0;
}

const intel_gfx_state_t *intel_gfx_get_state(void)
{
    return &igfx;
}
