/*
 * OsitoK x86-64 — Intel Gen9 display probe
 *
 * The qualified real-hardware target is the P1 UHD 630 (8086:3e9b).
 * Gen9 display programming is
 * tightly coupled to GGTT/stolen memory and power wells, so this driver
 * intentionally does not force a fresh modeset. It validates the
 * firmware/GOP-programmed pipe and primary plane and reports whether the
 * GOP scanout is safe to retain.
 */

#include "../include/types.h"
#include "../include/paging.h"
#include "../include/pci.h"
#include "../include/dma_diag.h"
#include "intel_gfx.h"

extern int  paging_map_mmio(uint64_t phys, uint64_t size);
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);
#define INTEL_GFX_MMIO_SIZE       INTEL_GGTT_BAR_BYTES

#define GEN9_PIPE_STRIDE          0x1000u
#define GEN9_TRANSCODER_EDP       3
#define GEN9_TRANS_OFFSET(t)      ((t)==GEN9_TRANSCODER_EDP ? 0xf000u : (t)*GEN9_PIPE_STRIDE)
#define GEN9_TRANS_CONF(t)        (0x70008u+GEN9_TRANS_OFFSET(t))
#define GEN9_TRANS_DDI(t)         (0x60400u+GEN9_TRANS_OFFSET(t))
#define GEN9_TRANS_HTOTAL(t)      (0x60000u+GEN9_TRANS_OFFSET(t))
#define GEN9_TRANS_VTOTAL(t)      (0x6000cu+GEN9_TRANS_OFFSET(t))
#define GEN9_TRANS_VBLANK(t)      (0x60010u+GEN9_TRANS_OFFSET(t))
#define GEN9_PIPE_SCANLINE(p)     (0x70000u+(p)*GEN9_PIPE_STRIDE)
#define GEN9_PIPE_FRMCNT(p)       (0x70040u+(p)*GEN9_PIPE_STRIDE)
#define GEN9_PIPE_FRMTMSTMP(p)    (0x70048u+(p)*GEN9_PIPE_STRIDE)
#define GEN9_PIPE_SRC(pipe)       (0x6001cu+(pipe)*GEN9_PIPE_STRIDE)
#define GEN9_POWER_WELL           0x45404u
#define GEN9_FUSE_STATUS          0x42000u
#define GEN9_PG1_STATE            (1u<<28)
#define GEN9_PG2_STATE            (1u<<30)
#define GEN9_PG1_DISTRIBUTED      (1u<<26)
#define GEN9_PG2_DISTRIBUTED      (1u<<25)
#define GEN9_DDI_BUF(port)        (0x64000u+(port)*0x100u)

#define GEN9_PLANE_CTL(pipe)      (0x70180 + (pipe) * GEN9_PIPE_STRIDE)
#define GEN9_PLANE_STRIDE(pipe)   (0x70188 + (pipe) * GEN9_PIPE_STRIDE)
#define GEN9_PLANE_SIZE(pipe)     (0x70190 + (pipe) * GEN9_PIPE_STRIDE)
#define GEN9_PLANE_SURF(pipe)     (0x7019C + (pipe) * GEN9_PIPE_STRIDE)
#define GEN9_PLANE_SURFLIVE(pipe) (0x701AC + (pipe) * GEN9_PIPE_STRIDE)
#define GEN9_PLANE_OFFSET(pipe)   (0x701A4 + (pipe) * GEN9_PIPE_STRIDE)
#define GEN9_PLANE_POS(pipe)      (0x7018C + (pipe) * GEN9_PIPE_STRIDE)

#define GEN9_PLANE_CTL_ENABLE     (1u << 31)
#define GEN9_PLANE_SURFACE_MASK   0xfffff000u

static intel_gfx_state_t igfx;
static volatile uint8_t *igfx_mmio;
static dma_diag_t igfx_dma;

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

typedef struct {
    int transcoder,pipe,port;
    uint32_t power,fuse,conf,ddi,buffer,htotal,vtotal,src;
    uint32_t ctl,stride,size,surface,live,offset,pos;
} IGFX_SCANOUT;

static bool igfx_read_scanout(int transcoder,IGFX_SCANOUT *s)
{
    memset(s,0,sizeof(*s)); s->transcoder=transcoder; s->pipe=s->port=-1;
    if (transcoder<0 || transcoder>GEN9_TRANSCODER_EDP) return false;
    s->power=igfx_read32(GEN9_POWER_WELL); s->fuse=igfx_read32(GEN9_FUSE_STATUS);
    if (s->power==UINT32_MAX || s->fuse==UINT32_MAX ||
        !(s->power&GEN9_PG1_STATE) || !(s->fuse&GEN9_PG1_DISTRIBUTED)) return false;
    bool pg2=(s->power&GEN9_PG2_STATE) && (s->fuse&GEN9_PG2_DISTRIBUTED);
    /* eDP lives in PG1 and selects a source pipe independently of A/B/C's
     * timing generators. Do not read disabled PG2 registers as if they were
     * evidence that the always-on eDP display is stopped. */
    if (transcoder!=GEN9_TRANSCODER_EDP && !pg2) return false;
    s->conf=igfx_read32(GEN9_TRANS_CONF(transcoder));
    if (s->conf==UINT32_MAX || (s->conf&0xc0000000u)!=0xc0000000u) return false;
    s->ddi=igfx_read32(GEN9_TRANS_DDI(transcoder));
    if (s->ddi==UINT32_MAX || !(s->ddi&(1u<<31))) return false;
    unsigned mode=(s->ddi>>24)&7;
    if (transcoder==GEN9_TRANSCODER_EDP) {
        unsigned input=(s->ddi>>12)&7;
        /* KBL/SKL PRM: 0=A always-on, 5=B, 6=C. The older generation's
         * encoding 4 for pipe A on/off is reserved on this target. */
        if (mode!=2 || (input!=0 && input!=5 && input!=6)) return false;
        s->pipe=input ? (int)input-4 : 0; s->port=0;
    } else {
        s->pipe=transcoder; s->port=(s->ddi>>28)&7;
        if (mode>3 || s->port<1 || s->port>4) return false;
    }
    if (s->pipe && !pg2) return false;
    unsigned io_state=1u<<(s->port==4 ? 2 : 2+2*s->port);
    if (!(s->power&io_state)) return false;
    s->buffer=igfx_read32(GEN9_DDI_BUF(s->port));
    if (s->buffer==UINT32_MAX || !(s->buffer&(1u<<31)) || (s->buffer&(1u<<7))) return false;
    s->ctl=igfx_read32(GEN9_PLANE_CTL(s->pipe));
    if (s->ctl==UINT32_MAX || !(s->ctl&GEN9_PLANE_CTL_ENABLE)) return false;
    s->htotal=igfx_read32(GEN9_TRANS_HTOTAL(transcoder));
    s->vtotal=igfx_read32(GEN9_TRANS_VTOTAL(transcoder));
    s->src=igfx_read32(GEN9_PIPE_SRC(s->pipe));
    s->stride=igfx_read32(GEN9_PLANE_STRIDE(s->pipe));
    s->size=igfx_read32(GEN9_PLANE_SIZE(s->pipe));
    s->surface=igfx_read32(GEN9_PLANE_SURF(s->pipe));
    s->live=igfx_read32(GEN9_PLANE_SURFLIVE(s->pipe));
    s->offset=igfx_read32(GEN9_PLANE_OFFSET(s->pipe));
    s->pos=igfx_read32(GEN9_PLANE_POS(s->pipe));
    return true;
}

static bool igfx_scanout_matches_gop(const IGFX_SCANOUT *s)
{
    /* Identify the firmware's linear XRGB plane; do not infer ownership,
     * modeset support or permission to replace its GGTT backing. */
    if (!igfx.memory_valid || !igfx.gop_in_aperture || !igfx.gop_sample_present ||
        !igfx.gop_width || !igfx.gop_height || !igfx.gop_pitch_bytes ||
        (s->conf&(3u<<21)) || ((s->ctl>>24)&15)!=4 ||
        (s->ctl&((7u<<10)|(1u<<15)|(1u<<20)|(1u<<8)|3u)) ||
        s->offset || s->pos || (s->stride&~0x3ffu) ||
        (s->size&0xf000e000u) || (s->src&0xe000f000u) ||
        (s->htotal&0xe000e000u) || (s->vtotal&0xe000e000u) ||
        (s->surface&GEN9_PLANE_SURFACE_MASK)!=(s->live&GEN9_PLANE_SURFACE_MASK)) return false;
    /* PRM PLANE_SURF/SURFLIVE: only bits 31:12 are the GGTT address.
     * SURF also has ring-flip metadata and reserved readback bits are not
     * address/alignment errors (the P1's SURFLIVE reads 0x20 at base 0). */
    uint32_t width=(s->size&0x1fffu)+1,height=((s->size>>16)&0xfffu)+1;
    return width==igfx.gop_width && height==igfx.gop_height &&
        (s->src>>16)+1==width && (s->src&0xfffu)+1==height &&
        (s->htotal&0x1fffu)+1==width && (s->vtotal&0x1fffu)+1==height &&
        (s->htotal>>16)+1>width && (s->vtotal>>16)+1>height &&
        s->stride*64u==igfx.gop_pitch_bytes && igfx.gop_pitch_bytes>=width*4u &&
        igfx.gop_ggtt_offset==(s->live&GEN9_PLANE_SURFACE_MASK);
}

int intel_gfx_read_timing(intel_gfx_timing_t *output)
{
    if (!output || !igfx.ready || !igfx.gen9 || !igfx.gop_scanout_retained ||
        !igfx_mmio || igfx.active_pipe<0 || igfx.active_pipe>2 ||
        igfx.active_transcoder<0 || igfx.active_transcoder>3) return 1;
    IGFX_SCANOUT before,after;
    int transcoder=igfx.active_transcoder;
    if (!igfx_read_scanout(transcoder,&before) || !igfx_scanout_matches_gop(&before) ||
        before.pipe!=igfx.active_pipe || before.port!=igfx.active_port) return 1;
    uint32_t blank=igfx_read32(GEN9_TRANS_VBLANK(transcoder));
    /* KBL PRM requires blank boundaries equal active/total boundaries.
     * Reject another timing layout, interlace or a changed retained mode. */
    if (blank!=before.vtotal || before.vtotal!=igfx.trans_vtotal ||
        before.htotal!=igfx.trans_htotal || before.conf!=igfx.pipeconf ||
        before.ddi!=igfx.trans_ddi) return 1;
    uint32_t active=(before.vtotal&0x1fffu)+1,total=(before.vtotal>>16)+1;
    for (unsigned attempt=0;attempt<4;attempt++) {
        uint32_t frame=igfx_read32(GEN9_PIPE_FRMCNT(before.pipe));
        uint32_t timestamp=igfx_read32(GEN9_PIPE_FRMTMSTMP(before.pipe));
        uint32_t raw=igfx_read32(GEN9_PIPE_SCANLINE(before.pipe));
        uint32_t frame_after=igfx_read32(GEN9_PIPE_FRMCNT(before.pipe));
        if (frame!=frame_after) continue;
        /* Only 12:0 is a progressive KBL scan line. All ones is a failed
         * read, whereas frame/timestamp UINT32_MAX is valid at rollover. */
        if ((raw&~0x1fffu) || raw>=total) return 1;
        if (!igfx_read_scanout(transcoder,&after) ||
            memcmp(&before,&after,sizeof(before)) ||
            blank!=igfx_read32(GEN9_TRANS_VBLANK(transcoder))) return 2;
        /* Intel's display implementation documents one line of offset on
         * DP/eDP and two on HSW+ HDMI. Normalize to active line zero. */
        unsigned mode=(before.ddi>>24)&7;
        uint32_t line=(raw+(mode==0 ? 2u : 1u))%total;
        *output=(intel_gfx_timing_t){frame,timestamp,raw,line,active,total,line>=active};
        return 0;
    }
    return 2;
}

static int igfx_find_active_scanout(void)
{
    igfx.active_pipe=igfx.active_plane=igfx.active_transcoder=igfx.active_port=-1;
    unsigned matches=0; IGFX_SCANOUT selected={0};
    for (int t=0;t<=GEN9_TRANSCODER_EDP;t++) {
        IGFX_SCANOUT a,b;
        bool active=igfx_read_scanout(t,&a);
        bool stable=active && igfx_read_scanout(t,&b) && !memcmp(&a,&b,sizeof(a));
        bool match=stable && igfx_scanout_matches_gop(&a);
        serial_puts("[IGFX] scanout trans="); serial_putdec(t);
        serial_puts(" power="); serial_puthex(a.power,8);
        serial_puts(" fuse="); serial_puthex(a.fuse,8);
        serial_puts(" conf="); serial_puthex(a.conf,8);
        serial_puts(" ddi="); serial_puthex(a.ddi,8);
        serial_puts(" buffer="); serial_puthex(a.buffer,8);
        serial_puts(" plane="); serial_puthex(a.ctl,8);
        serial_puts(" active="); serial_putdec(active);
        serial_puts(" stable="); serial_putdec(stable);
        serial_puts(" gop_match="); serial_putdec(match); serial_puts("\n");
        if (!active) continue;
        serial_puts("[IGFX] scanout pipe="); serial_putdec(a.pipe);
        serial_puts(" port="); serial_putdec(a.port);
        serial_puts(" htotal="); serial_puthex(a.htotal,8);
        serial_puts(" vtotal="); serial_puthex(a.vtotal,8);
        serial_puts(" src="); serial_puthex(a.src,8);
        serial_puts(" stride="); serial_puthex(a.stride,8);
        serial_puts(" size="); serial_puthex(a.size,8);
        serial_puts(" surface="); serial_puthex(a.surface,8);
        serial_puts(" live="); serial_puthex(a.live,8);
        serial_puts(" offset="); serial_puthex(a.offset,8);
        serial_puts(" pos="); serial_puthex(a.pos,8); serial_puts("\n");
        if (match) { selected=a; matches++; }
    }
    if (matches!=1) return -1;
    igfx.active_pipe=selected.pipe; igfx.active_plane=0;
    igfx.active_transcoder=selected.transcoder; igfx.active_port=selected.port;
    igfx.power_well=selected.power; igfx.fuse_status=selected.fuse;
    igfx.pipeconf=selected.conf; igfx.trans_ddi=selected.ddi; igfx.ddi_buf=selected.buffer;
    igfx.trans_htotal=selected.htotal; igfx.trans_vtotal=selected.vtotal; igfx.pipe_src=selected.src;
    igfx.plane_ctl=selected.ctl; igfx.plane_stride=selected.stride; igfx.plane_size=selected.size;
    igfx.plane_surf=selected.surface; igfx.plane_surf_live=selected.live;
    igfx.plane_offset=selected.offset; igfx.plane_pos=selected.pos;
    return 0;
}

static void igfx_log_range(const char *name, uint64_t base, uint64_t bytes)
{
    serial_puts("[IGFX] ");
    serial_puts(name);
    serial_puts(" base=0x");
    serial_puthex(base, 16);
    serial_puts(" bytes=0x");
    serial_puthex(bytes, 16);
    serial_puts("\n");
}

static void igfx_probe_memory(void)
{
    /* First bring-up target: P1 Coffee Lake-H UHD 630. Other display IDs
     * retain GOP without a verified scanout until their memory layout is
     * qualified. Host-bridge registers below are specific to this topology. */
    if (igfx.device_id != 0x3e9b || igfx.bus != 0 || igfx.dev != 2 ||
        igfx.func != 0 || pci_diag_segment(0) != 0)
        return;

    intel_ggtt_config_t *c = &igfx.memory_config;
    uint32_t msac;
    if (!pci_diag_read32(0, 0, 0, 0x00, &c->host_id) ||
        !pci_diag_read32(0, 0, 0, 0x50, &c->ggc) ||
        !pci_diag_read32(0, 0, 0, 0xb0, &c->bdsm) ||
        !pci_diag_read32(0, 0, 0, 0xb4, &c->bgsm) ||
        !pci_diag_read32(0, 0, 0, 0xbc, &c->tolud) ||
        !pci_diag_read32(0, 2, 0, 0x00, &c->igd_id) ||
        !pci_diag_read32(0, 2, 0, 0x04, &c->command) ||
        !pci_diag_read32(0, 2, 0, 0x10, &c->bar0_lo) ||
        !pci_diag_read32(0, 2, 0, 0x14, &c->bar0_hi) ||
        !pci_diag_read32(0, 2, 0, 0x18, &c->bar2_lo) ||
        !pci_diag_read32(0, 2, 0, 0x1c, &c->bar2_hi) ||
        !pci_diag_read32(0, 2, 0, 0x60, &msac)) {
        serial_puts("[IGFX] memory config unavailable\n");
        return;
    }
    c->msac = (uint8_t)(msac >> 16);
    serial_puts("[IGFX] memory GGC/BDSM/BGSM/TOLUD/MSAC=0x");
    serial_puthex(c->ggc & 0xffff, 4);
    serial_puts("/"); serial_puthex(c->bdsm, 8);
    serial_puts("/"); serial_puthex(c->bgsm, 8);
    serial_puts("/"); serial_puthex(c->tolud, 8);
    serial_puts("/"); serial_puthex(c->msac, 2);
    serial_puts("\n");
    enum intel_ggtt_status status = intel_ggtt_decode(c, &igfx.memory);
    if (status != INTEL_GGTT_OK || c->igd_id != 0x3e9b8086u ||
        igfx.memory.mmio_base != igfx.bar0_phys ||
        igfx.memory.aperture_base != igfx.bar2_phys) {
        serial_puts("[IGFX] memory layout rejected, status=");
        serial_putdec(status);
        serial_puts(" (also requires matching PCI snapshot/BARs)\n");
        return;
    }
    igfx.memory_valid = true;
    igfx_log_range("GGTT aperture", igfx.memory.ggtt_mmio_base,
                   igfx.memory.ggtt_bytes);
    igfx_log_range("CPU aperture", igfx.memory.aperture_base,
                   igfx.memory.aperture_bytes);
    igfx_log_range("GPU virtual space", 0, igfx.memory.gpu_va_bytes);
    igfx_log_range("firmware data stolen", igfx.memory.stolen_base,
                   igfx.memory.stolen_bytes);
    igfx_log_range("firmware table stolen", igfx.memory.table_stolen_base,
                   igfx.memory.ggtt_bytes);

    /* Gen9 FENCE_0..31 describe CPU-aperture tiling, not completion fences.
     * Even an otherwise unused GPU VA may belong to one of these regions. */
    igfx.fence_snapshot_valid = true;
    for (unsigned i = 0; i < 32; i++) {
        volatile void *reg = (volatile void *)(igfx_mmio + 0x100000 + i * 8);
        uint64_t raw = mmio_read64(reg);
        igfx.fence[i] = raw;
        if (raw == UINT64_MAX || mmio_read64(reg) != raw) {
            igfx.fence_snapshot_valid = false;
            continue;
        }
        if (!(raw & 1)) continue;
        igfx.fence_valid_mask |= 1u << i;
        uint64_t first = raw & 0xfffff000ULL;
        uint64_t end = ((raw >> 32) & 0xfffff000ULL) + 4096;
        if (end <= first || end > igfx.memory.gpu_va_bytes ||
            (raw & ((1ULL << 43) | 0xffc)))
            igfx.fence_snapshot_valid = false;
        serial_puts("[IGFX] firmware tiled fence "); serial_putdec(i);
        serial_puts(" start="); serial_puthex(first, 8);
        serial_puts(" end="); serial_puthex(end, 16);
        serial_puts(" raw="); serial_puthex(raw, 16);
        serial_puts("\n");
    }
    serial_puts("[IGFX] tiled fence snapshot valid=");
    serial_putdec(igfx.fence_snapshot_valid);
    serial_puts(" mask="); serial_puthex(igfx.fence_valid_mask, 8);
    serial_puts("\n");

    uint64_t bytes = (uint64_t)igfx.gop_pitch_bytes * igfx.gop_height;
    if (!igfx.gop_width || igfx.gop_width > igfx.gop_pitch_bytes / 4 ||
        !intel_ggtt_aperture_offset(&igfx.memory, igfx.gop_fb_phys, bytes,
                                    &igfx.gop_ggtt_offset)) {
        serial_puts("[IGFX] GOP range is outside validated aperture/GGTT\n");
        return;
    }
    igfx.gop_in_aperture = true;
    uint64_t last = igfx.gop_ggtt_offset + bytes - 1;
    uint32_t first_reg, last_reg;
    if (!intel_ggtt_pte_offset(&igfx.memory, igfx.gop_ggtt_offset, &first_reg) ||
        !intel_ggtt_pte_offset(&igfx.memory, last, &last_reg))
        return;
    /* Two read-only samples, not validation of all framebuffer pages. Do not
     * dereference the resulting backing addresses or infer free GGTT slots. */
    igfx.gop_first_pte = mmio_read64((volatile void *)(igfx_mmio + first_reg));
    igfx.gop_last_pte = mmio_read64((volatile void *)(igfx_mmio + last_reg));
    igfx.gop_sample_present =
        igfx.gop_first_pte != UINT64_MAX && igfx.gop_last_pte != UINT64_MAX &&
        intel_ggtt_decode_pte(igfx.gop_first_pte, igfx.gop_ggtt_offset,
                              &igfx.gop_first_phys) &&
        intel_ggtt_decode_pte(igfx.gop_last_pte, last, &igfx.gop_last_phys);
    serial_puts("[IGFX] GOP GGTT samples PTE=0x");
    serial_puthex(igfx.gop_first_pte, 16);
    serial_puts("/0x"); serial_puthex(igfx.gop_last_pte, 16);
    serial_puts(" present="); serial_putdec(igfx.gop_sample_present);
    serial_puts(" backing=0x"); serial_puthex(igfx.gop_first_phys, 16);
    serial_puts("/0x"); serial_puthex(igfx.gop_last_phys, 16);
    serial_puts("\n");
}

static void igfx_probe_dma(void)
{
    if (!igfx.memory_valid) return;
    pci_dev_t device = {.bus = igfx.bus, .dev = igfx.dev, .func = igfx.func,
        .vendor_id = igfx.vendor_id, .device_id = igfx.device_id,
        .class_code = 3, .bar = {igfx.bar0_phys, 0, igfx.bar2_phys}};
    dma_diag_prepare(&device, &igfx_dma);
    const dma_iommu_sample_t *m = &igfx_dma.sample[0].iommu;
    igfx.dma_state_valid = igfx_dma.prepared && igfx_dma.iommu_ready &&
        !igfx_dma.path_note && igfx_dma.sample[0].captured && m->valid && !m->changed;
    igfx.dma_gsts = m->gsts;
    igfx.dma_pmen = m->pmen;
    igfx.dma_target_faults = m->target;
    /* VT-d GSTS.TES is bit 31; PMEN.EPM/PRS are bits 31/0. Unknown, enabled
     * or transitioning remapping must not be treated as identity DMA. */
    igfx.dma_identity_confirmed = igfx.dma_state_valid && !(m->gsts & (1u << 31)) &&
        !(m->pmen & 0x80000001u);
    serial_puts("[IGFX] DMA state valid="); serial_putdec(igfx.dma_state_valid);
    serial_puts(" identity="); serial_putdec(igfx.dma_identity_confirmed);
    serial_puts(" unit="); serial_puthex(igfx_dma.base, 16);
    serial_puts(" GSTS="); serial_puthex(m->gsts, 8);
    serial_puts(" PMEN="); serial_puthex(m->pmen, 8);
    serial_puts(" target_faults="); serial_putdec(m->target);
    serial_puts("\n");
    const char *note = igfx_dma.path_note ? igfx_dma.path_note :
                       igfx_dma.dmar_note ? igfx_dma.dmar_note : m->note;
    if (note) { serial_puts("[IGFX] DMA note: "); serial_puts(note); serial_puts("\n"); }
}

int intel_gfx_init(uint16_t device_id, uint8_t bus, uint8_t dev, uint8_t func,
                   uint64_t bar0_phys, uint64_t bar2_phys,
                   uint64_t gop_fb_phys, uint32_t width, uint32_t height,
                   uint32_t pitch_bytes)
{
    memset(&igfx, 0, sizeof(igfx));
    igfx_mmio = NULL;
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

    uint32_t id, command, bar_lo, bar_hi;
    if (!pci_diag_read32(bus, dev, func, 0, &id) ||
        id != (((uint32_t)device_id << 16) | 0x8086u) ||
        !pci_diag_read32(bus, dev, func, 4, &command) || !(command & 2) ||
        !pci_diag_read32(bus, dev, func, 0x10, &bar_lo) ||
        !pci_diag_read32(bus, dev, func, 0x14, &bar_hi) ||
        (bar_lo & 15) != 4 ||
        (((uint64_t)bar_hi << 32) | (bar_lo & ~15u)) != bar0_phys) {
        serial_puts("[IGFX] PCI memory decoding unavailable, GOP fallback only\n");
        return -1;
    }
    if ((bar0_phys & (INTEL_GFX_MMIO_SIZE - 1)) ||
        bar0_phys >= INTEL_GGTT_PHYS_LIMIT ||
        paging_map_mmio(bar0_phys, INTEL_GFX_MMIO_SIZE) != 0) {
        serial_puts("[IGFX] invalid or unmapped BAR0, GOP fallback only\n");
        return -1;
    }
    __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    igfx_mmio = (volatile uint8_t *)PHYS_TO_VIRT(bar0_phys);

    /* Discovery is read-only: leave firmware's DMA enable state alone. */

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

    igfx_probe_memory();
    igfx_probe_dma();
#if INTEL_GGTT_DIAGNOSTIC
    intel_gfx_memory_test(&igfx);
#endif

    if (igfx_find_active_scanout() != 0) {
        serial_puts("[IGFX] no unique verified Gen9 GOP scanout, retaining firmware mapping\n");
        fb_puts(" Intel GFX: fallback GOP\n");
        return -1;
    }

    /*
     * On Gen9, PLANE_SURF is a GGTT/stolen-memory offset, not a system
     * physical address. Discovery does not acquire display ownership or
     * allocate a new plane surface. Retaining the
     * firmware GOP scanout is safe: the kernel keeps drawing through the
     * GOP framebuffer mapping and the display engine keeps using the mode
     * the firmware already validated.
     */
    igfx.gop_scanout_retained = true;
    igfx.ready = true;

    serial_puts("[IGFX] Gen9 GOP scanout identified; no modeset or display ownership change\n");
    fb_puts(" Intel GFX: Gen9 GOP scanout identified\n");
#if INTEL_SCANOUT_DIAGNOSTIC
    intel_gfx_timing_test();
#endif
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
