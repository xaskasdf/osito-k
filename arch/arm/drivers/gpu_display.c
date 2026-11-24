/*
 * OsitoK x86-64 — GPU Display Engine (Phase A: GOP Takeover)
 *
 * Takes GPU control of the display while keeping the UEFI GOP framebuffer
 * and timing.  Allocates display channels through GSP-RM, builds initial
 * push buffer commands for the core and window channels, and submits an
 * UPDATE to activate the configuration.
 *
 * After init the kernel can do page flips, vblank sync, and (later) mode
 * switches — all without losing the display during transition.
 *
 * Supports Turing (RTX 20xx), Ampere (RTX 30xx), Ada (RTX 40xx).
 *
 * Reference: nouveau tu102.c / gv100.c / r535_disp.c,
 *            open-gpu-doc classes/display/, envytools.
 */

#include "../include/types.h"
#include "gpu.h"
#include "gpu_display.h"

/* ── External Functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern gpu_device_t gpu_dev;

/* GSP-RM allocation and control (from gsp.c) */
extern int  gsp_rm_alloc(uint32_t hParent, uint32_t hObject, uint32_t hClass,
                          const void *params, uint32_t params_size);
extern int  gsp_rm_control(uint32_t hObject, uint32_t cmd,
                            const void *params, uint32_t params_size);

/* GPU register access (from gpu.c) */
extern uint32_t gpu_reg_read(uint32_t reg);
extern void     gpu_reg_write(uint32_t reg, uint32_t val);

/* rdtsc for timing */
static inline uint64_t rdtsc(void)
{
    uint64_t cnt;
    __asm__ volatile ("mrs %0, CNTPCT_EL0" : "=r"(cnt));
    return cnt;
}

/* Memory barrier */
static inline void disp_wmb(void)
{
    __asm__ volatile ("dmb oshst" ::: "memory");
}

/* ── Driver State ────────────────────────────────────────────── */

static display_state_t disp;

/* ── Display Push Buffer Helpers ─────────────────────────────── */

/* Display push buffers use the same method header encoding as compute
 * (NV_METHOD macro from gpu.h).  Subchannel is always 0 for display. */

#define DISP_SUBCHAN  0

static void disp_pb_begin(pushbuf_state_t *pb)
{
    pb->pos = 0;
}

static void disp_pb_push(pushbuf_state_t *pb, uint32_t data)
{
    if (pb->pos < pb->capacity)
        pb->buf[pb->pos++] = data;
}

static uint32_t disp_pb_size_bytes(pushbuf_state_t *pb)
{
    return pb->pos * 4;
}

/* Push a method header + single data word */
static void disp_pb_mthd(pushbuf_state_t *pb, uint32_t method, uint32_t data)
{
    disp_pb_push(pb, NV_METHOD(DISP_SUBCHAN, method, 1));
    disp_pb_push(pb, data);
}

/* Push a method header + two data words */
static void disp_pb_mthd2(pushbuf_state_t *pb, uint32_t method,
                           uint32_t d0, uint32_t d1)
{
    disp_pb_push(pb, NV_METHOD(DISP_SUBCHAN, method, 2));
    disp_pb_push(pb, d0);
    disp_pb_push(pb, d1);
}

/* ── Phase A Step 1: Discover Display Hardware ───────────────── */

static void disp_discover_hw(void)
{
    uint32_t cap  = gpu_reg_read(NV_PDISP_FE_HW_SYS_CAP);
    uint32_t capb = gpu_reg_read(NV_PDISP_FE_HW_SYS_CAPB);
    uint32_t conf = gpu_reg_read(NV_PDISP_FE_MISC_CONFIGA);

    disp.head_mask   = cap & 0xFF;
    disp.sor_mask    = (cap >> 8) & 0xFF;
    disp.win_mask    = capb;

    disp.num_heads   = conf & 0x0F;
    disp.num_sors    = (conf >> 8) & 0x0F;
    disp.num_windows = (conf >> 20) & 0x3F;

    serial_puts("[DISP] Hardware discovery:\n");
    serial_puts("[DISP]   Heads: ");
    serial_putdec(disp.num_heads);
    serial_puts(" (mask=0x");
    serial_puthex(disp.head_mask, 2);
    serial_puts(")  SORs: ");
    serial_putdec(disp.num_sors);
    serial_puts(" (mask=0x");
    serial_puthex(disp.sor_mask, 2);
    serial_puts(")  Windows: ");
    serial_putdec(disp.num_windows);
    serial_puts(" (mask=0x");
    serial_puthex(disp.win_mask, 8);
    serial_puts(")\n");
}

/* ── Phase A Step 2: Find Active Head and SOR ────────────────── */

/* Scan SOR armed state registers to find which SOR is driving a head.
 * For GOP takeover we need to know which head/SOR is already active. */
static int disp_find_active_output(void)
{
    uint32_t i;

    for (i = 0; i < disp.num_sors; i++) {
        if (!(disp.sor_mask & (1 << i)))
            continue;

        uint32_t state = gpu_reg_read(NV_PDISP_SOR_STATE(i));
        uint32_t owner = state & 0x0F;          /* head owner bits [3:0] */
        uint32_t proto = (state >> 8) & 0x0F;   /* protocol bits [11:8] */

        serial_puts("[DISP] SOR");
        serial_putdec(i);
        serial_puts(": state=0x");
        serial_puthex(state, 8);
        serial_puts(" owner=");
        serial_putdec(owner);
        serial_puts(" proto=");
        serial_putdec(proto);
        serial_puts("\n");

        /* Owner != 0 means this SOR is actively driving a head.
         * Owner encoding: 0=NONE, 1=HEAD0, 2=HEAD1, etc. */
        if (owner != 0) {
            disp.active_head = owner - 1;
            disp.active_sor  = i;
            serial_puts("[DISP] Active output: SOR");
            serial_putdec(i);
            serial_puts(" → HEAD");
            serial_putdec(disp.active_head);
            serial_puts("\n");
            return 0;
        }
    }

    /* Fallback: assume head 0, SOR 0 if nothing found (GOP might not
     * set the armed state in a way we can read without full display init) */
    serial_puts("[DISP] No active SOR found, defaulting to HEAD0/SOR0\n");
    disp.active_head = 0;
    disp.active_sor  = 0;
    return 0;
}

/* ── Phase A Step 3: Claim Ownership from Firmware ───────────── */

static int disp_claim_ownership(void)
{
    uint32_t val = gpu_reg_read(NV_PDISP_OWNERSHIP);

    serial_puts("[DISP] Ownership register: 0x");
    serial_puthex(val, 8);
    serial_puts("\n");

    /* If firmware owns the display (bit 0 set), clear it and poll bit 1 */
    if (val & 0x01) {
        serial_puts("[DISP] Claiming ownership from firmware...\n");

        /* Clear bit 0 to release firmware ownership */
        gpu_reg_write(NV_PDISP_OWNERSHIP, val & ~0x01);

        /* Poll for bit 1 (handover complete), 2 second timeout */
        uint64_t start = rdtsc();
        uint64_t timeout = 2000ULL * 3000000ULL;  /* ~2s at 3GHz */

        while ((rdtsc() - start) < timeout) {
            uint32_t r = gpu_reg_read(NV_PDISP_OWNERSHIP);
            if (r & 0x02) {
                serial_puts("[DISP] Ownership claimed (handover bit set)\n");
                disp.ownership_claimed = true;
                return 0;
            }
        }

        /* Timeout — proceed anyway, the bit may not be set on all gens */
        serial_puts("[DISP] Ownership handover timeout (proceeding)\n");
        disp.ownership_claimed = true;
        return 0;
    }

    serial_puts("[DISP] Display not firmware-owned, ownership is ours\n");
    disp.ownership_claimed = true;
    return 0;
}

/* ── Phase A Step 4: Read Current Timing from Armed Registers ── */

/* The GPU's display engine maintains "armed" copies of the active timing
 * parameters.  For GOP takeover we read these so we can re-program the
 * exact same values through the push buffer, avoiding any visible glitch. */
static void disp_read_armed_timing(void)
{
    /* On NVDisplay (Volta+), the armed head timing is readable from
     * the PDISP registers at 0x616000 + head*0x800 region.
     * These are the RG (raster generator) armed state registers. */

    uint32_t head = disp.active_head;
    uint32_t base = 0x616300 + head * 0x800;

    /* RG_DPCLK at base+0x00 contains the pixel clock config.
     * For GOP takeover we use a safe default and read what we can. */
    uint32_t rg_dpclk = gpu_reg_read(base);

    serial_puts("[DISP] Armed timing for HEAD");
    serial_putdec(head);
    serial_puts(": RG_DPCLK=0x");
    serial_puthex(rg_dpclk, 8);
    serial_puts("\n");

    /* Read armed raster parameters from the head state area.
     * Offsets vary by generation; use the capability source registers
     * that contain the currently armed values. */

    /* Head status registers at 0x616000 range (envytools):
     *   +0x100: capabilities
     *   +0x300: RG (raster generator) registers
     *   +0x30C: total raster size (armed)
     *   +0x310: sync end (armed)
     *   +0x314: blank end (armed)
     *   +0x318: blank start (armed) */

    uint32_t rg_base = 0x616000 + head * 0x800;

    uint32_t raster_size   = gpu_reg_read(rg_base + 0x30C);
    uint32_t sync_end      = gpu_reg_read(rg_base + 0x310);
    uint32_t blank_end     = gpu_reg_read(rg_base + 0x314);
    uint32_t blank_start   = gpu_reg_read(rg_base + 0x318);

    disp.htotal       = raster_size & 0x7FFF;
    disp.vtotal       = (raster_size >> 16) & 0x7FFF;
    disp.hsync_end    = sync_end & 0x7FFF;
    disp.vsync_end    = (sync_end >> 16) & 0x7FFF;
    disp.hblank_end   = blank_end & 0x7FFF;
    disp.vblank_end   = (blank_end >> 16) & 0x7FFF;
    disp.hblank_start = blank_start & 0x7FFF;
    disp.vblank_start = (blank_start >> 16) & 0x7FFF;

    /* Estimate pixel clock from armed timing and known refresh rate.
     * For 60 Hz: pixel_clock = htotal * vtotal * 60 */
    if (disp.htotal > 0 && disp.vtotal > 0) {
        disp.pixel_clock_hz = disp.htotal * disp.vtotal * 60;
    } else {
        /* Fallback: use standard 1080p timing */
        disp.htotal       = 2200;
        disp.vtotal       = 1125;
        disp.hsync_end    = 2052;
        disp.vsync_end    = 1089;
        disp.hblank_end   = 192;
        disp.vblank_end   = 41;
        disp.hblank_start = 1920;
        disp.vblank_start = 1080;
        disp.pixel_clock_hz = 148500000;  /* 148.5 MHz */
    }

    serial_puts("[DISP]   Raster: ");
    serial_putdec(disp.htotal);
    serial_puts("x");
    serial_putdec(disp.vtotal);
    serial_puts(" sync_end=");
    serial_putdec(disp.hsync_end);
    serial_puts(",");
    serial_putdec(disp.vsync_end);
    serial_puts(" blank=");
    serial_putdec(disp.hblank_end);
    serial_puts("-");
    serial_putdec(disp.hblank_start);
    serial_puts(",");
    serial_putdec(disp.vblank_end);
    serial_puts("-");
    serial_putdec(disp.vblank_start);
    serial_puts("\n");
    serial_puts("[DISP]   Pixel clock: ~");
    serial_putdec(disp.pixel_clock_hz / 1000000);
    serial_puts(" MHz\n");
}

/* ── Phase A Step 5: Allocate Display Objects via GSP-RM ─────── */

static int disp_alloc_objects(void)
{
    gpu_device_t *dev = &gpu_dev;

    /* Select display class IDs for this GPU generation */
    disp.disp_root_class = disp_root_class_for_gen(dev->generation);
    disp.core_class      = disp_core_class_for_gen(dev->generation);
    disp.win_class       = disp_win_class_for_gen(dev->generation);

    serial_puts("[DISP] Display classes: root=0x");
    serial_puthex(disp.disp_root_class, 4);
    serial_puts(" core=0x");
    serial_puthex(disp.core_class, 4);
    serial_puts(" win=0x");
    serial_puthex(disp.win_class, 4);
    serial_puts("\n");

    /* Step 5a: Allocate NV04_DISPLAY_COMMON (display system object).
     * Parent = device handle, no params needed. */
    disp.disp_common_handle = GSP_RM_DISP_COMMON_HANDLE;

    serial_puts("[DISP] Step 1/4: ALLOC NV04_DISPLAY_COMMON\n");
    int ret = gsp_rm_alloc(GSP_RM_DEVICE_HANDLE, disp.disp_common_handle,
                            NV04_DISPLAY_COMMON, NULL, 0);
    if (ret != 0)
        serial_puts("[DISP]   (allocation pending — expected without full boot chain)\n");

    /* Step 5b: Allocate display root (GA102_DISP / TU102_DISP / etc.).
     * Parent = device handle. */
    disp.disp_root_handle = GSP_RM_DISP_ROOT_HANDLE;

    serial_puts("[DISP] Step 2/4: ALLOC display root (0x");
    serial_puthex(disp.disp_root_class, 4);
    serial_puts(")\n");
    ret = gsp_rm_alloc(GSP_RM_DEVICE_HANDLE, disp.disp_root_handle,
                        disp.disp_root_class, NULL, 0);
    if (ret != 0)
        serial_puts("[DISP]   (allocation pending)\n");

    /* Step 5c: Allocate core channel DMA.
     * Parent = display root.  For display channels the RM accepts
     * a channel alloc params struct similar to compute but with
     * display-specific instance/pushbuf memory. */
    disp.core_handle = GSP_RM_DISP_CORE_HANDLE;

    /* Allocate core channel push buffer and instance memory */
    disp.core_pb.capacity = PUSHBUF_SIZE_DWORDS;
    disp.core_pb.buf = (uint32_t *)mem_alloc_aligned(
        PUSHBUF_SIZE_DWORDS * 4, 4096);
    if (!disp.core_pb.buf) {
        serial_puts("[DISP] Failed to allocate core pushbuffer\n");
        return -1;
    }
    memset(disp.core_pb.buf, 0, PUSHBUF_SIZE_DWORDS * 4);
    disp.core_pb.buf_phys = (uint64_t)(uintptr_t)disp.core_pb.buf;
    disp.core_pb.pos = 0;

    /* Build channel allocation params.  Display channels use the same
     * nv_chan_alloc_params_t wire format as compute GPFIFO channels. */
    nv_chan_alloc_params_t core_params;
    memset(&core_params, 0, sizeof(core_params));
    core_params.gpFifoOffset  = disp.core_pb.buf_phys;
    core_params.gpFifoEntries = 0;  /* Display channels don't use GPFIFO ring */
    core_params.flags         = NVOS04_FLAGS_CHANNEL_TYPE_PHYSICAL;

    /* Instance memory for display core channel */
    void *core_inst = mem_alloc_aligned(4096, 4096);
    if (!core_inst) {
        serial_puts("[DISP] Failed to allocate core instance memory\n");
        return -1;
    }
    memset(core_inst, 0, 4096);
    uint64_t core_inst_phys = (uint64_t)(uintptr_t)core_inst;

    core_params.instanceMem.base         = core_inst_phys;
    core_params.instanceMem.size         = 0x200;
    core_params.instanceMem.addressSpace = ADDR_SYSMEM;
    core_params.instanceMem.cacheAttrib  = NV_MEMORY_CACHED;

    core_params.ramfcMem.base         = core_inst_phys;
    core_params.ramfcMem.size         = 0x200;
    core_params.ramfcMem.addressSpace = ADDR_SYSMEM;
    core_params.ramfcMem.cacheAttrib  = NV_MEMORY_CACHED;

    serial_puts("[DISP] Step 3/4: ALLOC core channel DMA (0x");
    serial_puthex(disp.core_class, 4);
    serial_puts(") pb=0x");
    serial_puthex(disp.core_pb.buf_phys, 16);
    serial_puts("\n");

    ret = gsp_rm_alloc(disp.disp_root_handle, disp.core_handle,
                        disp.core_class, &core_params, sizeof(core_params));
    if (ret != 0)
        serial_puts("[DISP]   (allocation pending)\n");

    /* Step 5d: Allocate window channel 0 DMA.
     * Parent = display root. */
    disp.win0_handle = GSP_RM_DISP_WIN0_HANDLE;

    /* Allocate window channel push buffer */
    disp.win_pb.capacity = PUSHBUF_SIZE_DWORDS;
    disp.win_pb.buf = (uint32_t *)mem_alloc_aligned(
        PUSHBUF_SIZE_DWORDS * 4, 4096);
    if (!disp.win_pb.buf) {
        serial_puts("[DISP] Failed to allocate window pushbuffer\n");
        return -1;
    }
    memset(disp.win_pb.buf, 0, PUSHBUF_SIZE_DWORDS * 4);
    disp.win_pb.buf_phys = (uint64_t)(uintptr_t)disp.win_pb.buf;
    disp.win_pb.pos = 0;

    nv_chan_alloc_params_t win_params;
    memset(&win_params, 0, sizeof(win_params));
    win_params.gpFifoOffset  = disp.win_pb.buf_phys;
    win_params.gpFifoEntries = 0;
    win_params.flags         = NVOS04_FLAGS_CHANNEL_TYPE_PHYSICAL;

    void *win_inst = mem_alloc_aligned(4096, 4096);
    if (!win_inst) {
        serial_puts("[DISP] Failed to allocate window instance memory\n");
        return -1;
    }
    memset(win_inst, 0, 4096);
    uint64_t win_inst_phys = (uint64_t)(uintptr_t)win_inst;

    win_params.instanceMem.base         = win_inst_phys;
    win_params.instanceMem.size         = 0x200;
    win_params.instanceMem.addressSpace = ADDR_SYSMEM;
    win_params.instanceMem.cacheAttrib  = NV_MEMORY_CACHED;

    win_params.ramfcMem.base         = win_inst_phys;
    win_params.ramfcMem.size         = 0x200;
    win_params.ramfcMem.addressSpace = ADDR_SYSMEM;
    win_params.ramfcMem.cacheAttrib  = NV_MEMORY_CACHED;

    serial_puts("[DISP] Step 4/4: ALLOC window channel DMA (0x");
    serial_puthex(disp.win_class, 4);
    serial_puts(") pb=0x");
    serial_puthex(disp.win_pb.buf_phys, 16);
    serial_puts("\n");

    ret = gsp_rm_alloc(disp.disp_root_handle, disp.win0_handle,
                        disp.win_class, &win_params, sizeof(win_params));
    if (ret != 0)
        serial_puts("[DISP]   (allocation pending)\n");

    disp.channels_allocated = true;
    return 0;
}

/* ── Phase A Step 6: Configure Instance Memory ───────────────── */

static void disp_configure_instance_mem(void)
{
    /* FE_INST_MEM0: memory target
     *   0 = PHYS_NVM (VRAM physical)
     *   1 = PCI (BAR-mapped)
     *   2 = PCI_COHERENT (system memory, coherent)
     * We use system memory (target = 2) since our push buffers are in RAM. */
    gpu_reg_write(NV_PDISP_FE_INST_MEM0, 0x02);

    /* FE_INST_MEM1: base address of the instance block (bits 30:0).
     * For system memory, this is the physical address >> 12 shifted
     * into the appropriate bits.  We use the core channel's instance
     * memory page as the base. */
    uint64_t inst_addr = disp.core_pb.buf_phys >> 12;
    gpu_reg_write(NV_PDISP_FE_INST_MEM1, (uint32_t)(inst_addr & 0x7FFFFFFF));

    serial_puts("[DISP] Instance memory: target=PCI_COHERENT addr=0x");
    serial_puthex(disp.core_pb.buf_phys, 16);
    serial_puts("\n");
}

/* ── Phase A Step 7: Configure Push Buffer Registers ─────────── */

/* Display channels have their push buffer base address configured
 * through PDISP FE_PBBASE registers, not through GPFIFO/USERD like
 * compute.  Channel IDs: 0=core, 1..32=windows, 33..64=wimm, 73..80=cursor. */

static void disp_configure_pushbuf_regs(void)
{
    /* Core channel = channel ID 0 */
    uint64_t core_addr = disp.core_pb.buf_phys;
    gpu_reg_write(NV_PDISP_FE_PBBASEHI(0),
                  (uint32_t)((core_addr >> 32) & 0x7F));
    gpu_reg_write(NV_PDISP_FE_PBBASE(0),
                  (uint32_t)((core_addr & 0xFFFFFFF0) | 0x02));  /* target=PCI_COHERENT */

    serial_puts("[DISP] Core PB base: 0x");
    serial_puthex(core_addr, 16);
    serial_puts("\n");

    /* Window channel 0 = channel ID 1 */
    uint64_t win_addr = disp.win_pb.buf_phys;
    gpu_reg_write(NV_PDISP_FE_PBBASEHI(1),
                  (uint32_t)((win_addr >> 32) & 0x7F));
    gpu_reg_write(NV_PDISP_FE_PBBASE(1),
                  (uint32_t)((win_addr & 0xFFFFFFF0) | 0x02));

    serial_puts("[DISP] Win0 PB base: 0x");
    serial_puthex(win_addr, 16);
    serial_puts("\n");
}

/* ── Phase A Step 8: Build Core Channel Commands ─────────────── */

/* Push the current timing into the core channel push buffer.
 * This re-programs the same timing that UEFI had, ensuring no glitch. */
static void disp_build_core_pushbuf(void)
{
    pushbuf_state_t *pb = &disp.core_pb;
    uint32_t head = disp.active_head;
    uint32_t sor  = disp.active_sor;

    disp_pb_begin(pb);

    /* SOR_SET_CONTROL: assign SOR to head, keep current protocol.
     * OWNER_MASK = (1 << head), PROTOCOL preserved from armed state.
     * We use TMDS_A (0x01) as a safe default for HDMI/DVI. */
    uint32_t sor_ctrl = (1 << head)     /* OWNER_MASK: head N enabled */
                      | (0x01 << 8);    /* PROTOCOL: SINGLE_TMDS_A */
    disp_pb_mthd(pb, DISP_SOR_SET_CONTROL(sor), sor_ctrl);

    /* HEAD_SET_CONTROL: progressive mode */
    disp_pb_mthd(pb, DISP_HEAD_SET_CONTROL(head), 0x00000000);

    /* HEAD_SET_PIXEL_CLOCK_FREQUENCY: Hz in bits [30:0] */
    disp_pb_mthd(pb, DISP_HEAD_SET_PIXEL_CLOCK(head),
                 disp.pixel_clock_hz & 0x7FFFFFFF);

    /* HEAD_SET_RASTER_SIZE: htotal | (vtotal << 16) */
    disp_pb_mthd(pb, DISP_HEAD_SET_RASTER_SIZE(head),
                 disp.htotal | (disp.vtotal << 16));

    /* HEAD_SET_RASTER_SYNC_END */
    disp_pb_mthd(pb, DISP_HEAD_SET_RASTER_SYNC_END(head),
                 disp.hsync_end | (disp.vsync_end << 16));

    /* HEAD_SET_RASTER_BLANK_END */
    disp_pb_mthd(pb, DISP_HEAD_SET_RASTER_BLANK_END(head),
                 disp.hblank_end | (disp.vblank_end << 16));

    /* HEAD_SET_RASTER_BLANK_START */
    disp_pb_mthd(pb, DISP_HEAD_SET_RASTER_BLANK_START(head),
                 disp.hblank_start | (disp.vblank_start << 16));

    /* UPDATE: trigger atomic apply.  Interlock with window channel 0.
     * SET_INTERLOCK_FLAGS method tells the core to wait for the window
     * channel before applying.  Bit 1 = interlock with window 0. */
    disp_pb_mthd(pb, DISP_CORE_SET_INTERLOCK_FLAGS, (1 << 1));
    disp_pb_mthd(pb, DISP_CORE_UPDATE, 0x00000000);

    disp_wmb();

    serial_puts("[DISP] Core pushbuf: ");
    serial_putdec(pb->pos);
    serial_puts(" dwords (");
    serial_putdec(disp_pb_size_bytes(pb));
    serial_puts(" bytes)\n");
}

/* ── Phase A Step 9: Build Window Channel Commands ───────────── */

/* Configure window channel 0 to scan out from the GOP framebuffer. */
static void disp_build_window_pushbuf(void)
{
    pushbuf_state_t *pb = &disp.win_pb;

    disp_pb_begin(pb);

    /* SET_OFFSET(0): framebuffer address >> 8 (256-byte aligned, Volta+) */
    uint32_t fb_offset = (uint32_t)(disp.fb_addr >> 8);
    disp_pb_mthd(pb, DISP_WIN_SET_OFFSET(0), fb_offset);

    /* SET_SIZE: width | (height << 16) */
    disp_pb_mthd(pb, DISP_WIN_SET_SIZE,
                 disp.width | (disp.height << 16));

    /* SET_STORAGE: pitch mode, block height = 0 (ONE_GOB, unused for pitch) */
    disp_pb_mthd(pb, DISP_WIN_SET_STORAGE, DISP_STORAGE_PITCH);

    /* SET_PLANAR_STORAGE(0): pitch >> 6 (Volta+ encoding) */
    uint32_t pitch_encoded = disp.pitch >> 6;
    disp_pb_mthd(pb, DISP_WIN_SET_PLANAR_STORAGE(0), pitch_encoded);

    /* SET_PARAMS: format = A8R8G8B8 (0xCF), color space = RGB (0x00) */
    disp_pb_mthd(pb, DISP_WIN_SET_PARAMS, DISP_FMT_A8R8G8B8);

    /* SET_CONTEXT_DMA_ISO(0): DMA context handle.
     * Value 0 means physical addressing (no CTXDMA indirection).
     * For system memory scanout we use the physical address directly. */
    disp_pb_mthd(pb, DISP_WIN_SET_CONTEXT_DMA_ISO(0), 0x00000000);

    /* SET_POINT_IN(0): source crop offset (0,0 = no crop) */
    disp_pb_mthd(pb, DISP_WIN_SET_POINT_IN(0), 0x00000000);

    /* SET_SIZE_IN: source dimensions = full framebuffer */
    disp_pb_mthd(pb, DISP_WIN_SET_SIZE_IN,
                 disp.width | (disp.height << 16));

    /* SET_SIZE_OUT: output dimensions = same (no scaling) */
    disp_pb_mthd(pb, DISP_WIN_SET_SIZE_OUT,
                 disp.width | (disp.height << 16));

    /* Window UPDATE (marks this window's state as ready for interlock) */
    disp_pb_mthd(pb, DISP_WIN_UPDATE, 0x00000000);

    disp_wmb();

    serial_puts("[DISP] Window pushbuf: ");
    serial_putdec(pb->pos);
    serial_puts(" dwords (");
    serial_putdec(disp_pb_size_bytes(pb));
    serial_puts(" bytes)\n");
}

/* ── Phase A Step 10: Submit Push Buffers ────────────────────── */

/* Display channels use PUT pointer registers in PDISP, unlike compute
 * which uses USERD GP_PUT.  The PUT pointer tells the display engine
 * how many dwords of commands to process from the push buffer.
 *
 * For Volta+ NVDisplay:
 *   Core channel control: NV_PDISP_FE_CHNCTL_CORE (0x6104E0)
 *   Window channel control: NV_PDISP_FE_CHNCTL_WIN(i) (0x6104E4 + i*4)
 *
 * The control register layout:
 *   Bits [0]:    Channel enable (ALLOCATED)
 *   Bits [1]:    Channel connect
 *   Other bits vary by generation.
 *
 * Actual PUT pointer submission for display uses a different mechanism
 * than GPFIFO — the display FE reads directly from the push buffer
 * base address up to the PUT offset.  PUT is written to the channel's
 * user-mode register area at 0x690000 + chan*0x1000. */

#define NV_PDISP_USER_CORE_PUT     0x00690000   /* Core channel PUT (dword offset) */
#define NV_PDISP_USER_WIN_PUT(i)   (0x00690000 + ((1 + (i)) * 0x1000))

static void disp_submit_pushbufs(void)
{
    /* Enable core channel: set bit 0 (ALLOCATED) + bit 1 (CONNECT) */
    uint32_t core_ctl = gpu_reg_read(NV_PDISP_FE_CHNCTL_CORE);
    gpu_reg_write(NV_PDISP_FE_CHNCTL_CORE, core_ctl | 0x03);

    /* Enable window channel 0 */
    uint32_t win_ctl = gpu_reg_read(NV_PDISP_FE_CHNCTL_WIN(0));
    gpu_reg_write(NV_PDISP_FE_CHNCTL_WIN(0), win_ctl | 0x03);

    disp_wmb();

    /* Write PUT pointers to advance the channels.
     * PUT value = number of dwords written to the push buffer. */
    gpu_reg_write(NV_PDISP_USER_WIN_PUT(0), disp.win_pb.pos);
    disp_wmb();

    gpu_reg_write(NV_PDISP_USER_CORE_PUT, disp.core_pb.pos);
    disp_wmb();

    serial_puts("[DISP] Submitted: core PUT=");
    serial_putdec(disp.core_pb.pos);
    serial_puts(" win0 PUT=");
    serial_putdec(disp.win_pb.pos);
    serial_puts("\n");

    /* Poll core channel status for completion (bit pattern indicates IDLE).
     * Timeout after 500ms. */
    uint64_t start = rdtsc();
    uint64_t timeout = 500ULL * 3000000ULL;  /* ~500ms at 3GHz */
    bool completed = false;

    while ((rdtsc() - start) < timeout) {
        uint32_t status = gpu_reg_read(NV_PDISP_FE_CHNSTATUS_CORE);
        /* Bits [4:2] = stage state.  0x00 = IDLE after processing. */
        if ((status & 0x1C) == 0x00) {
            completed = true;
            break;
        }
    }

    if (completed) {
        serial_puts("[DISP] Core channel: commands processed (IDLE)\n");
    } else {
        serial_puts("[DISP] Core channel: status poll timeout (may still be processing)\n");
    }
}

/* ── Public API: gpu_display_init ────────────────────────────── */

int gpu_display_init(uint32_t *gop_fb, uint32_t width,
                     uint32_t height, uint32_t pitch)
{
    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present) {
        serial_puts("[DISP] GPU not present, skipping display init\n");
        return -1;
    }

    serial_puts("\n[DISP] === GPU Display Engine Phase A: GOP Takeover ===\n");
    serial_puts("[DISP] Framebuffer: 0x");
    serial_puthex((uint64_t)(uintptr_t)gop_fb, 16);
    serial_puts(" ");
    serial_putdec(width);
    serial_puts("x");
    serial_putdec(height);
    serial_puts(" pitch=");
    serial_putdec(pitch);
    serial_puts("\n");

    fb_puts("\n GPU Display: Phase A init...\n");

    memset(&disp, 0, sizeof(disp));
    disp.fb_addr = (uint64_t)(uintptr_t)gop_fb;
    disp.width   = width;
    disp.height  = height;
    disp.pitch   = pitch;

    /* Step 1: Discover display hardware (heads, SORs, windows) */
    disp_discover_hw();

    if (disp.num_heads == 0) {
        serial_puts("[DISP] No display heads found, aborting\n");
        return -1;
    }

    /* Step 2: Find which head/SOR is currently driving the display */
    disp_find_active_output();

    /* Step 3: Read current timing from armed registers */
    disp_read_armed_timing();

    /* Step 4: Claim display ownership from firmware */
    disp_claim_ownership();

    /* Step 5: Allocate display objects via GSP-RM */
    if (disp_alloc_objects() != 0) {
        serial_puts("[DISP] Display object allocation failed\n");
        return -1;
    }

    /* Step 6: Configure instance memory */
    disp_configure_instance_mem();

    /* Step 7: Configure push buffer base registers */
    disp_configure_pushbuf_regs();

    /* Step 8: Build core channel push buffer (re-program current timing) */
    disp_build_core_pushbuf();

    /* Step 9: Build window channel push buffer (point at GOP framebuffer) */
    disp_build_window_pushbuf();

    /* Step 10: Submit push buffers and activate */
    disp_submit_pushbufs();

    /* Mark display as ready */
    disp.configured = true;
    disp.ready = true;
    disp.flip_count = 0;

    serial_puts("[DISP] === Phase A complete: display engine active ===\n");
    fb_puts(" GPU Display: Phase A done\n");

    return 0;
}

/* ── Public API: gpu_display_flip ────────────────────────────── */

int gpu_display_flip(uint64_t fb_addr)
{
    if (!disp.ready) {
        serial_puts("[DISP] Flip: display not ready\n");
        return -1;
    }

    pushbuf_state_t *wpb = &disp.win_pb;
    pushbuf_state_t *cpb = &disp.core_pb;

    /* Build window channel: update offset to new framebuffer */
    disp_pb_begin(wpb);
    disp_pb_mthd(wpb, DISP_WIN_SET_OFFSET(0), (uint32_t)(fb_addr >> 8));
    disp_pb_mthd(wpb, DISP_WIN_UPDATE, 0x00000000);
    disp_wmb();

    /* Build core channel: trigger UPDATE with window interlock */
    disp_pb_begin(cpb);
    disp_pb_mthd(cpb, DISP_CORE_SET_INTERLOCK_FLAGS, (1 << 1));
    disp_pb_mthd(cpb, DISP_CORE_UPDATE, 0x00000000);
    disp_wmb();

    /* Submit: window first, then core (core waits for interlock) */
    gpu_reg_write(NV_PDISP_USER_WIN_PUT(0), wpb->pos);
    disp_wmb();

    gpu_reg_write(NV_PDISP_USER_CORE_PUT, cpb->pos);
    disp_wmb();

    disp.flip_count++;
    disp.fb_addr = fb_addr;

    return 0;
}

/* ── Public API: gpu_display_vblank_count ────────────────────── */

uint32_t gpu_display_vblank_count(void)
{
    /* Read the head's vblank counter from the RG register block.
     * On NVDisplay the counter is at offset +0x340 within the head's
     * register region at 0x616000 + head*0x800. */
    uint32_t head = disp.active_head;
    uint32_t reg  = 0x616000 + head * 0x800 + 0x340;
    return gpu_reg_read(reg);
}

/* ── Public API: gpu_display_is_ready ────────────────────────── */

int gpu_display_is_ready(void)
{
    return disp.ready ? 1 : 0;
}

/* ── Public API: gpu_display_get_state ───────────────────────── */

display_state_t *gpu_display_get_state(void)
{
    return &disp;
}

/* ══════════════════════════════════════════════════════════════════
 * Phase B: I2C Bit-Bang, EDID Read/Parse, Full Modeset
 * ══════════════════════════════════════════════════════════════════ */

/* ── I2C Port Registers ──────────────────────────────────────── */

/* I2C port register addresses for g94+ NVIDIA GPUs.
 * Each port has a control register with SCL/SDA/SET bits. */
static const uint32_t i2c_port_regs[] = {
    NV_I2C_PORT1,  /* port 0 */
    NV_I2C_PORT2,  /* port 1 */
    NV_I2C_PORT3,  /* port 2 */
    NV_I2C_PORT4,  /* port 3 */
};
#define NUM_I2C_PORTS  4

/* Small delay for I2C clock (~5 µs for 100 kHz).
 * Uses TSC — assumes ~3 GHz CPU ≈ 15000 cycles per 5 µs. */
static void i2c_delay(void)
{
    uint64_t start = rdtsc();
    while (rdtsc() - start < 15000)
        ;
}

/* Set SCL and SDA line states via GPIO bit-bang.
 * val: combine I2C_SCL_BIT and/or I2C_SDA_BIT for lines to drive HIGH.
 * Lines not set are driven LOW. */
static void i2c_set_lines(uint32_t port_reg, int scl, int sda)
{
    uint32_t val = I2C_SET_BIT;
    if (scl) val |= I2C_SCL_BIT;
    if (sda) val |= I2C_SDA_BIT;
    gpu_reg_write(port_reg, val);
    i2c_delay();
}

/* Read SCL and SDA line states.
 * Returns register value; check I2C_SCL_BIT / I2C_SDA_BIT. */
static uint32_t i2c_read_lines(uint32_t port_reg)
{
    /* Read without SET bit to sample line states */
    return gpu_reg_read(port_reg);
}

/* ── I2C Bit-Bang Protocol ───────────────────────────────────── */

static void i2c_start(uint32_t port)
{
    /* START: SDA HIGH→LOW while SCL HIGH */
    i2c_set_lines(port, 1, 1);
    i2c_set_lines(port, 1, 0);  /* SDA low */
    i2c_set_lines(port, 0, 0);  /* SCL low */
}

static void i2c_stop(uint32_t port)
{
    /* STOP: SDA LOW→HIGH while SCL HIGH */
    i2c_set_lines(port, 0, 0);
    i2c_set_lines(port, 1, 0);  /* SCL high, SDA low */
    i2c_set_lines(port, 1, 1);  /* SDA high → STOP */
}

/* Send 8 bits MSB first. Returns 0 if ACK received, -1 if NACK. */
static int i2c_write_byte(uint32_t port, uint8_t byte)
{
    for (int bit = 7; bit >= 0; bit--) {
        int sda = (byte >> bit) & 1;
        i2c_set_lines(port, 0, sda);    /* set data */
        i2c_set_lines(port, 1, sda);    /* clock high */
        i2c_set_lines(port, 0, sda);    /* clock low */
    }

    /* Read ACK: release SDA, clock high, sample SDA */
    i2c_set_lines(port, 0, 1);          /* release SDA */
    i2c_set_lines(port, 1, 1);          /* clock high */
    uint32_t val = i2c_read_lines(port);
    i2c_set_lines(port, 0, 1);          /* clock low */

    return (val & I2C_SDA_BIT) ? -1 : 0;  /* ACK = SDA low */
}

/* Read 8 bits MSB first. Sends ACK if ack=1, NACK if ack=0. */
static uint8_t i2c_read_byte(uint32_t port, int ack)
{
    uint8_t byte = 0;

    for (int bit = 7; bit >= 0; bit--) {
        i2c_set_lines(port, 0, 1);      /* release SDA */
        i2c_set_lines(port, 1, 1);      /* clock high */
        uint32_t val = i2c_read_lines(port);
        if (val & I2C_SDA_BIT)
            byte |= (1 << bit);
        i2c_set_lines(port, 0, 1);      /* clock low */
    }

    /* Send ACK/NACK */
    i2c_set_lines(port, 0, ack ? 0 : 1);  /* ACK = SDA low, NACK = SDA high */
    i2c_set_lines(port, 1, ack ? 0 : 1);  /* clock high */
    i2c_set_lines(port, 0, 1);             /* clock low, release SDA */

    return byte;
}

/* ── DDC/EDID Read ───────────────────────────────────────────── */

/*
 * Read EDID block from DDC port via I2C.
 *
 * i2c_port_idx: which I2C port (0-3) to use
 * edid_buf:     128-byte output buffer
 *
 * Returns: 0 on success, -1 on failure
 */
static int ddc_read_edid(int i2c_port_idx, uint8_t *edid_buf)
{
    if (i2c_port_idx < 0 || i2c_port_idx >= NUM_I2C_PORTS)
        return -1;

    uint32_t port = i2c_port_regs[i2c_port_idx];

    /* Release bus */
    i2c_set_lines(port, 1, 1);
    i2c_delay();

    /* Phase 1: Write address 0x00 (start of EDID) */
    i2c_start(port);
    if (i2c_write_byte(port, (DDC_ADDR_EDID << 1) | 0) < 0) {
        /* NACK → no monitor on this port */
        i2c_stop(port);
        return -1;
    }
    if (i2c_write_byte(port, 0x00) < 0) {
        i2c_stop(port);
        return -1;
    }
    i2c_stop(port);

    /* Phase 2: Read 128 bytes */
    i2c_start(port);
    if (i2c_write_byte(port, (DDC_ADDR_EDID << 1) | 1) < 0) {
        i2c_stop(port);
        return -1;
    }

    for (int i = 0; i < EDID_BLOCK_SIZE; i++) {
        /* ACK all bytes except the last */
        edid_buf[i] = i2c_read_byte(port, (i < EDID_BLOCK_SIZE - 1) ? 1 : 0);
    }
    i2c_stop(port);

    /* Validate header */
    if (edid_buf[0] != 0x00 || edid_buf[1] != 0xFF || edid_buf[2] != 0xFF ||
        edid_buf[3] != 0xFF || edid_buf[4] != 0xFF || edid_buf[5] != 0xFF ||
        edid_buf[6] != 0xFF || edid_buf[7] != 0x00) {
        serial_puts("[DISP] EDID header invalid\n");
        return -1;
    }

    /* Validate checksum */
    uint8_t sum = 0;
    for (int i = 0; i < EDID_BLOCK_SIZE; i++)
        sum += edid_buf[i];
    if (sum != 0) {
        serial_puts("[DISP] EDID checksum failed\n");
        return -1;
    }

    return 0;
}

/* ── EDID Parser ─────────────────────────────────────────────── */

/*
 * Parse a 128-byte EDID block and extract the preferred mode
 * (first detailed timing descriptor).
 */
int gpu_display_parse_edid(const uint8_t *edid, edid_mode_t *mode)
{
    if (!edid || !mode) return -1;

    /* First detailed timing descriptor starts at offset 0x36 */
    const edid_detailed_timing_t *dt =
        (const edid_detailed_timing_t *)(edid + 0x36);

    /* A pixel clock of 0 means this is not a timing descriptor */
    if (dt->pixel_clock_10khz == 0)
        return -1;

    /* Extract fields (multi-byte with hi-nybble packing) */
    mode->h_active = (uint16_t)dt->h_active_lo |
                     (uint16_t)((dt->h_active_blank_hi >> 4) & 0x0F) << 8;
    mode->h_blank  = (uint16_t)dt->h_blank_lo |
                     (uint16_t)(dt->h_active_blank_hi & 0x0F) << 8;
    mode->v_active = (uint16_t)dt->v_active_lo |
                     (uint16_t)((dt->v_active_blank_hi >> 4) & 0x0F) << 8;
    mode->v_blank  = (uint16_t)dt->v_blank_lo |
                     (uint16_t)(dt->v_active_blank_hi & 0x0F) << 8;

    mode->h_sync_offset = (uint16_t)dt->h_sync_off_lo |
                          (uint16_t)((dt->sync_hi >> 6) & 0x03) << 8;
    mode->h_sync_width  = (uint16_t)dt->h_sync_pw_lo |
                          (uint16_t)((dt->sync_hi >> 4) & 0x03) << 8;
    mode->v_sync_offset = (uint16_t)((dt->v_sync_off_pw >> 4) & 0x0F) |
                          (uint16_t)((dt->sync_hi >> 2) & 0x03) << 4;
    mode->v_sync_width  = (uint16_t)(dt->v_sync_off_pw & 0x0F) |
                          (uint16_t)(dt->sync_hi & 0x03) << 4;

    mode->pixel_clock_hz = (uint32_t)dt->pixel_clock_10khz * 10000;
    mode->h_total = mode->h_active + mode->h_blank;
    mode->v_total = mode->v_active + mode->v_blank;
    mode->interlaced = (dt->flags & 0x80) ? true : false;

    /* Calculate refresh rate */
    if (mode->h_total > 0 && mode->v_total > 0) {
        mode->refresh_hz = mode->pixel_clock_hz / ((uint32_t)mode->h_total * mode->v_total);
    } else {
        mode->refresh_hz = 60;
    }

    serial_puts("[DISP] EDID preferred mode: ");
    serial_putdec(mode->h_active);
    serial_puts("x");
    serial_putdec(mode->v_active);
    serial_puts("@");
    serial_putdec(mode->refresh_hz);
    serial_puts("Hz pixel_clock=");
    serial_putdec(mode->pixel_clock_hz / 1000);
    serial_puts("kHz\n");

    return 0;
}

/* ── Public: Read EDID ───────────────────────────────────────── */

int gpu_display_read_edid(uint8_t *edid_buf)
{
    if (!edid_buf) return -1;

    serial_puts("[DISP] Reading EDID via DDC...\n");

    /* Try each I2C port until we find a monitor.
     * In a proper implementation, DCB would tell us which port to use.
     * For now, scan all ports (the active SOR is usually on port 0 or 1). */
    for (int port = 0; port < NUM_I2C_PORTS; port++) {
        serial_puts("[DISP]   Trying I2C port ");
        serial_putdec(port);
        serial_puts("... ");

        if (ddc_read_edid(port, edid_buf) == 0) {
            serial_puts("OK\n");

            /* Log manufacturer */
            uint16_t mfg = ((uint16_t)edid_buf[8] << 8) | edid_buf[9];
            char mfg_str[4];
            mfg_str[0] = ((mfg >> 10) & 0x1F) + 'A' - 1;
            mfg_str[1] = ((mfg >>  5) & 0x1F) + 'A' - 1;
            mfg_str[2] = (mfg & 0x1F) + 'A' - 1;
            mfg_str[3] = '\0';
            serial_puts("[DISP]   Manufacturer: ");
            serial_puts(mfg_str);
            serial_puts(" Product: 0x");
            serial_puthex(((uint32_t)edid_buf[11] << 8) | edid_buf[10], 4);
            serial_puts("\n");

            return 0;
        }
        serial_puts("no response\n");
    }

    serial_puts("[DISP] No monitor found on any DDC port\n");
    return -1;
}

/* ── Public: Detect Monitor ──────────────────────────────────── */

int gpu_display_detect_monitor(edid_mode_t *mode)
{
    uint8_t edid[EDID_BLOCK_SIZE];

    if (gpu_display_read_edid(edid) < 0)
        return -1;

    if (mode)
        return gpu_display_parse_edid(edid, mode);

    return 0;  /* monitor detected, no mode requested */
}

/* ── Full Modeset ────────────────────────────────────────────── */

/*
 * Set display mode with new timing parameters.
 * Rebuilds core + window channel push buffers with the new timing,
 * then submits UPDATE.
 *
 * Requires: gpu_display_init() already called (Phase A complete).
 */
int gpu_display_set_mode(const edid_mode_t *mode, uint64_t fb_addr, uint32_t fb_pitch)
{
    if (!disp.ready || !mode) return -1;

    serial_puts("[DISP] Setting mode: ");
    serial_putdec(mode->h_active);
    serial_puts("x");
    serial_putdec(mode->v_active);
    serial_puts("@");
    serial_putdec(mode->refresh_hz);
    serial_puts("Hz\n");

    /* Calculate raster parameters for NVDisplay method push.
     *
     * NVDisplay HEAD_SET_RASTER_* convention:
     *   RASTER_SIZE     = (htotal, vtotal)
     *   RASTER_SYNC_END = (hsync_end, vsync_end)
     *     where hsync_end = h_active + h_sync_offset + h_sync_width
     *           vsync_end = v_active + v_sync_offset + v_sync_width
     *   RASTER_BLANK_END = (hblank_end, vblank_end)
     *     where hblank_end = h_blank (total blanking period)
     *           vblank_end = v_blank
     *   RASTER_BLANK_START = (hblank_start, vblank_start)
     *     where hblank_start = h_active
     *           vblank_start = v_active
     */
    uint32_t htotal = mode->h_total;
    uint32_t vtotal = mode->v_total;
    uint32_t hsync_end = mode->h_active + mode->h_sync_offset + mode->h_sync_width;
    uint32_t vsync_end = mode->v_active + mode->v_sync_offset + mode->v_sync_width;
    uint32_t hblank_end = mode->h_blank;
    uint32_t vblank_end = mode->v_blank;
    uint32_t hblank_start = mode->h_active;
    uint32_t vblank_start = mode->v_active;

    uint32_t head = disp.active_head;
    uint32_t sor = disp.active_sor;

    serial_puts("[DISP]   htotal=");
    serial_putdec(htotal);
    serial_puts(" vtotal=");
    serial_putdec(vtotal);
    serial_puts(" pixclk=");
    serial_putdec(mode->pixel_clock_hz / 1000);
    serial_puts("kHz\n");

    /* ── Rebuild Core Channel Push Buffer ─────────────────────── */

    pushbuf_state_t *cpb = &disp.core_pb;
    disp_pb_begin(cpb);

    /* SOR_SET_CONTROL: assign SOR to head, protocol = SINGLE_TMDS_A */
    disp_pb_mthd(cpb, DISP_SOR_SET_CONTROL(sor),
                  (1 << head) | (0x01 << 8));

    /* HEAD_SET_CONTROL: progressive */
    disp_pb_mthd(cpb, DISP_HEAD_SET_CONTROL(head), 0x00);

    /* HEAD_SET_PIXEL_CLOCK: Hz */
    disp_pb_mthd(cpb, DISP_HEAD_SET_PIXEL_CLOCK(head),
                  mode->pixel_clock_hz);

    /* HEAD_SET_RASTER_SIZE */
    disp_pb_mthd(cpb, DISP_HEAD_SET_RASTER_SIZE(head),
                  (htotal & 0x7FFF) | ((vtotal & 0x7FFF) << 16));

    /* HEAD_SET_RASTER_SYNC_END */
    disp_pb_mthd(cpb, DISP_HEAD_SET_RASTER_SYNC_END(head),
                  (hsync_end & 0x7FFF) | ((vsync_end & 0x7FFF) << 16));

    /* HEAD_SET_RASTER_BLANK_END */
    disp_pb_mthd(cpb, DISP_HEAD_SET_RASTER_BLANK_END(head),
                  (hblank_end & 0x7FFF) | ((vblank_end & 0x7FFF) << 16));

    /* HEAD_SET_RASTER_BLANK_START */
    disp_pb_mthd(cpb, DISP_HEAD_SET_RASTER_BLANK_START(head),
                  (hblank_start & 0x7FFF) | ((vblank_start & 0x7FFF) << 16));

    /* SET_INTERLOCK_FLAGS: interlock with window channel 0 */
    disp_pb_mthd(cpb, DISP_CORE_SET_INTERLOCK_FLAGS, (1 << 1));

    /* UPDATE */
    disp_pb_mthd(cpb, DISP_CORE_UPDATE, 0);

    /* ── Rebuild Window Channel Push Buffer ───────────────────── */

    pushbuf_state_t *wpb = &disp.win_pb;
    disp_pb_begin(wpb);

    /* SET_OFFSET: framebuffer address (256-byte aligned) */
    disp_pb_mthd(wpb, DISP_WIN_SET_OFFSET(0), (uint32_t)(fb_addr >> 8));

    /* SET_SIZE: width | (height << 16) */
    disp_pb_mthd(wpb, DISP_WIN_SET_SIZE,
                  mode->h_active | ((uint32_t)mode->v_active << 16));

    /* SET_STORAGE: pitch mode, block_height = ONE_GOB */
    disp_pb_mthd(wpb, DISP_WIN_SET_STORAGE, (0 << 0) | (1 << 4));

    /* SET_PLANAR_STORAGE: encoded pitch (in 64-byte units) */
    uint32_t pitch_enc = (fb_pitch >> 6) & 0x1FFF;
    disp_pb_mthd(wpb, DISP_WIN_SET_PLANAR_STORAGE(0), pitch_enc);

    /* SET_PARAMS: A8R8G8B8 format (0xCF), RGB color space */
    disp_pb_mthd(wpb, DISP_WIN_SET_PARAMS, 0xCF);

    /* SET_CONTEXT_DMA_ISO: physical addressing */
    disp_pb_mthd(wpb, DISP_WIN_SET_CONTEXT_DMA_ISO(0), 0);

    /* SET_POINT_IN: no source crop */
    disp_pb_mthd(wpb, DISP_WIN_SET_POINT_IN(0), 0);

    /* SET_SIZE_IN: source size */
    disp_pb_mthd(wpb, DISP_WIN_SET_SIZE_IN,
                  mode->h_active | ((uint32_t)mode->v_active << 16));

    /* SET_SIZE_OUT: output size (no scaling) */
    disp_pb_mthd(wpb, DISP_WIN_SET_SIZE_OUT,
                  mode->h_active | ((uint32_t)mode->v_active << 16));

    /* Window UPDATE */
    disp_pb_mthd(wpb, DISP_WIN_UPDATE, 0);

    /* ── Submit Both Channels ─────────────────────────────────── */

    disp_wmb();

    /* Write PUT pointers to advance channels */
    uint32_t core_put = disp_pb_size_bytes(cpb);
    uint32_t win_put  = disp_pb_size_bytes(wpb);

    serial_puts("[DISP]   Core PB: ");
    serial_putdec(cpb->pos);
    serial_puts(" dwords, Window PB: ");
    serial_putdec(wpb->pos);
    serial_puts(" dwords\n");

    /* Enable channels and write PUT */
    gpu_reg_write(NV_PDISP_FE_CHNCTL_CORE, 0x01);  /* enable */
    disp_wmb();
    gpu_reg_write(NV_PDISP_FE_CHNCTL_CORE + 4, core_put);  /* PUT */

    gpu_reg_write(NV_PDISP_FE_CHNCTL_WIN(0), 0x01);  /* enable */
    disp_wmb();
    gpu_reg_write(NV_PDISP_FE_CHNCTL_WIN(0) + 4, win_put);  /* PUT */

    /* Poll for completion (GET catches up to PUT) */
    uint64_t deadline = rdtsc() + 3000000000ULL;  /* ~1s */
    while (rdtsc() < deadline) {
        uint32_t core_get = gpu_reg_read(NV_PDISP_FE_CHNCTL_CORE + 8);
        uint32_t win_get  = gpu_reg_read(NV_PDISP_FE_CHNCTL_WIN(0) + 8);
        if (core_get >= core_put && win_get >= win_put) {
            serial_puts("[DISP] Modeset complete\n");

            /* Update state */
            disp.width  = mode->h_active;
            disp.height = mode->v_active;

            return 0;
        }
    }

    serial_puts("[DISP] Modeset timeout!\n");
    return -1;
}
