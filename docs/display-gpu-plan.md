# OsitoK Display Pipeline — GPU Acceleration & App Rendering

## Current State (Working)

### Software Pipeline (X-RETINA Phase 1)
```
App renders to SHM surface (RAM, ARGB8888)
  |
Compositor thread (60fps, QOS_INTERACTIVE)
  |-- Single fullscreen? -> direct scanout (skip compositing)
  |-- Multiple windows?  -> z-order sort, per-scanline blit
  |-- Cursor overlay (8x8 bitmap)
  |
display_flip()
  |-- VBlank wait (APIC timer simulation, 2 ticks @ 100Hz)
  |-- memcpy back_buffer -> GOP VRAM (REP MOVSQ)
```

### What Works
- Compositor: 32 windows, SHM surfaces, z-order, input ring buffer
- Display: double-buffered, gamma-correct text, dithering
- Win32 shims: DirectDraw (UT99 Flip/Blt), User32 (CreateWindow, message queue)
- Input: PS/2 keyboard (IRQ 1), xHCI USB keyboard, compositor key ring

### Bottleneck
`memcpy` back→front at 60fps for 800x600x4 = ~115 MB/s. Fine for software,
but wastes CPU cycles that could be spent on app rendering.

---

## Phase A: GPU Scanout (Zero-Copy Display)

**Goal**: Eliminate the memcpy by making the GPU display engine scan out
directly from the compositor's back buffer.

**Status**: `gpu_display.c` written (~1400 lines), register defs in `gpu_display.h`

### Architecture
```
App renders to SHM surface (RAM or VRAM)
  |
Compositor builds final frame in back_buffer
  |
gpu_display_flip(back_buffer_phys)
  |-- Write SET_OFFSET to window channel push buffer
  |-- GPU display engine scans out directly from RAM/VRAM
  |-- No memcpy needed
```

### Requirements
1. GSP firmware boot (Phases X20-X31 done, needs validation)
2. `gsp_rm_alloc()` for DISP ROOT + display channels
3. Push buffer construction (core + window channels)
4. SET_OFFSET register: `DISP_WIN_SET_OFFSET = phys_addr >> 8`
5. VRAM or system RAM address must be DMA-accessible

### Key Registers (NVC67E Window Channel)
```
DISP_WIN_SET_OFFSET(b)   0x0260 + b*4    scanout address >> 8
DISP_WIN_SET_SIZE         0x0224          active resolution
DISP_WIN_SET_STORAGE      0x0228          pitch + format
DISP_WIN_SET_PARAMS       0x022C          A8R8G8B8 = 0xCF
DISP_WIN_UPDATE           0x0200          apply changes
```

### Fallback
If GPU display init fails (no GSP, no NVIDIA GPU, QEMU):
keep current memcpy path. `display_flip()` checks `gpu_display_available`
flag and falls back automatically.

---

## Phase B: GPU-Accelerated Composition

**Goal**: Use GPU compute shaders to composite windows instead of CPU blitting.

### Architecture
```
Per-window SHM surfaces allocated in VRAM (via BAR1 + GMMU)
  |
compositor_render_gpu():
  |-- Upload dirty window surfaces to VRAM (CE DMA)
  |-- Launch SASS blit kernel per window (z-order, alpha blend)
  |-- Result in VRAM back buffer
  |-- SET_OFFSET -> GPU scans out directly
  |-- Zero CPU involvement in display path
```

### SASS Kernels Needed
1. `blit_opaque.sass` — memcpy per-scanline (window without transparency)
2. `blit_alpha.sass` — per-pixel A8R8G8B8 alpha blend
3. `fill_rect.sass` — desktop background fill
4. `cursor_overlay.sass` — 8x8 cursor with transparency

### Existing Infrastructure
- SASS kernel dispatch working (X37-X42)
- GMMU 5-level page tables working
- CE DMA (host↔VRAM) working
- QMD + GPFIFO dispatch working
- Compute bind + launch working

### Performance Target
At 800x600x32bpp:
- CPU path: ~2ms per frame (memcpy + blit)
- GPU path: <0.1ms per frame (DMA + compute dispatch)
- Frees CPU for app rendering + inference

---

## Phase C: Modesetting (Native Resolution)

**Goal**: Control display resolution independently of UEFI GOP.

### Requirements
1. DDC/I2C bit-bang for EDID read (monitor capabilities)
2. EDID parser (resolution, refresh rate, pixel clock)
3. HEAD raster timing configuration
4. SOR protocol (TMDS for HDMI, DP for DisplayPort)
5. Pixel clock PLL programming

### Registers (per HEAD, offset +0x400)
```
DISP_HEAD_SET_CONTROL        enable/disable
DISP_HEAD_SET_PIXEL_CLOCK    pixel clock freq
DISP_HEAD_SET_RASTER_SIZE    H/V active pixels
DISP_HEAD_SET_RASTER_SYNC    sync pulse endpoints
```

### Hardware Boot Target (H7)
RTX 3090 (GA102) with native modesetting at monitor's preferred resolution.

---

## Chromium Ozone Backend (Documented for Future)

### Status
14 files exist at `ui/ozone/platform/ositok/`:
- `ozone_platform_ositok.cc` — Platform registration (76 lines)
- `ositok_surface_factory.cc` — SHM surface creation via SYS_SHM_MKSURFACE (89 lines)
- `ositok_window.cc` — Window management stubs (107 lines)
- `ositok_screen.cc` — Display info (800x600) (44 lines)
- `ositok_window_manager.cc` — Window tracking (21 lines)
- Build system registered in `ozone.gni` + `BUILD.gn`

### What's Needed
1. **Kernel SHM syscalls**: SYS_SHM_MKSURFACE (506), SYS_SHM_MAP (501) —
   allocate compositor-visible surface, return pixel pointer
2. **Surface→Compositor bridge**: PresentCanvas() must notify compositor
   via compositor_signal_dirty()
3. **Input loop**: Read compositor keyboard ring → inject into Ozone event queue
4. **Rebuild Chrome**: After kernel SHM syscalls work, rebuild with
   `--ozone-platform=ositok`

### Rendering Flow
```
Chrome render process
  |-- Skia rasterizes to SkSurface (software)
  |-- SkSurface wraps SHM pixel buffer
  |
PresentCanvas(damage_rect)
  |-- compositor_signal_dirty(window_id)
  |
Compositor picks up window surface, composites, flips
```

---

## UT99 Rendering (Already Integrated)

### Flow
```
UT99 game loop (winexec)
  |-- IDirectDrawSurface7::Lock(back_buffer)
  |-- SoftDrv.dll renders to RAM (RGB565)
  |-- IDirectDrawSurface7::Unlock()
  |-- IDirectDrawSurface7::Flip()
       |-- RGB565 -> XRGB8888 conversion
       |-- Copy to compositor SHM surface
       |-- ddraw_compositor_notify() -> compositor_signal_dirty()
  |
Compositor displays window
```

### Status
- DirectDraw vtable: Lock/Unlock/Flip/Blt implemented
- Color conversion: RGB565 → XRGB8888 per-scanline
- Compositor integration: Window registered, dirty signaling works
- Game loop: 202K+ timeGetTime calls confirmed running

---

## Remote Display (VNC)

QEMU built-in VNC for viewing OsitoK's framebuffer from macOS/remote:
```bash
# qemu-test.sh now includes -vnc :0,password=on
# Password set via QEMU monitor: "change vnc password osito"
# Connect from Mac: open vnc://<host-ip>:5900
# UFW rule: sudo ufw allow 5900/tcp
```

---

## Implementation Priority

| Priority | Task | Effort | Impact |
|----------|------|--------|--------|
| 1 | Verify UT99 renders to screen (debug compositor thread) | Low | High |
| 2 | Phase A: GPU scanout (validate GSP-RM display channel) | Medium | High |
| 3 | Kernel SHM syscalls for Ozone | Low | Medium |
| 4 | Phase B: GPU composition kernels | Medium | Medium |
| 5 | Chromium Ozone surface→compositor bridge | Medium | High |
| 6 | Phase C: Native modesetting | High | Medium |
