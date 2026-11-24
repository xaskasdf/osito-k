/* dxvk_compositor_bridge.cpp -- DXVK W5.4 T4
 *
 * Wraps OsitoK compositor syscalls so DXVK's swap-chain code can request
 * a back-buffer SHM surface and present it without going through the
 * Win32 / X11 / Wayland scaffolding the upstream code expects.
 *
 *   * SYS_SHM_MKSURFACE (506) — create a BGRA surface of (w,h)
 *   * SYS_SHM_MAP       (501) — map the surface into the caller VA
 *   * SYS_GUI_FLIP      (507) — flip the SHM region to the framebuffer
 *   * SYS_SHM_DESTROY   (503) — release the SHM region
 *
 * The bridge owns up to 16 concurrent windows; each "window" is just a
 * SHM handle + cached dimensions. Window IDs are 1-indexed slot numbers
 * (0 is reserved as "invalid").
 *
 * Used from:
 *   * dxvk/src/wsi/wsi_platform_ositok.cpp -- HWND -> window_id intern
 *   * test/runtime D3D11 swap-chain code   -- direct flip if VK absent
 */

#include <cstdint>
#include <cstring>
#include <atomic>

extern "C" long syscall(long number, ...);

#define SYS_SHM_MAP              501L
#define SYS_SHM_DESTROY          503L
#define SYS_SHM_MKSURFACE        506L
#define SYS_GUI_FLIP             507L
#define SHM_FLAG_CPU_WRITE       (1L << 0)
#define SHM_FLAG_CPU_READ        (1L << 1)
#define SHM_FLAG_GPU_SCANOUT     (1L << 2)
#define SHM_SURFACE_FLAGS        (SHM_FLAG_CPU_WRITE | SHM_FLAG_CPU_READ | SHM_FLAG_GPU_SCANOUT)

namespace {
  struct CompositorWindow {
    std::atomic<uint32_t> in_use { 0 };
    uint32_t shm_handle { 0 };
    uint32_t width      { 0 };
    uint32_t height     { 0 };
    void *   mapped     { nullptr };
  };

  static constexpr size_t kMaxWindows = 16;
  static CompositorWindow s_windows[kMaxWindows];
}

extern "C" {

/* Return a 1-indexed window_id, 0 on failure. */
uint32_t dxvk_compositor_create_window(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) return 0;
  if (width  > 4096) width  = 4096;
  if (height > 4096) height = 4096;

  /* Find a free slot. */
  uint32_t slot = (uint32_t)kMaxWindows;
  for (uint32_t i = 0; i < kMaxWindows; ++i) {
    uint32_t expected = 0;
    if (s_windows[i].in_use.compare_exchange_strong(expected, 1)) {
      slot = i;
      break;
    }
  }
  if (slot == kMaxWindows) return 0;

  long shm = syscall(SYS_SHM_MKSURFACE, (long)width, (long)height,
                     SHM_SURFACE_FLAGS);
  if (shm <= 0) {
    s_windows[slot].in_use.store(0);
    return 0;
  }

  long mapped = syscall(SYS_SHM_MAP, shm);
  if (mapped == 0) {
    (void)syscall(SYS_SHM_DESTROY, shm);
    s_windows[slot].in_use.store(0);
    return 0;
  }

  s_windows[slot].shm_handle = (uint32_t)shm;
  s_windows[slot].width      = width;
  s_windows[slot].height     = height;
  s_windows[slot].mapped     = (void *)(uintptr_t)mapped;
  return slot + 1;   /* 1-indexed: 0 reserved for invalid */
}

/* Returns the SHM handle for a window or 0 if invalid. */
uint32_t dxvk_compositor_window_shm(uint32_t window_id) {
  if (window_id == 0 || window_id > kMaxWindows) return 0;
  uint32_t slot = window_id - 1;
  if (s_windows[slot].in_use.load() == 0) return 0;
  return s_windows[slot].shm_handle;
}

/* Returns the mapped CPU pointer for a window or nullptr if invalid. */
void *dxvk_compositor_window_mapped(uint32_t window_id) {
  if (window_id == 0 || window_id > kMaxWindows) return nullptr;
  uint32_t slot = window_id - 1;
  if (s_windows[slot].in_use.load() == 0) return nullptr;
  return s_windows[slot].mapped;
}

/* Return cached dimensions; 0,0 if invalid. */
void dxvk_compositor_window_size(uint32_t window_id,
                                 uint32_t *out_w, uint32_t *out_h) {
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (window_id == 0 || window_id > kMaxWindows) return;
  uint32_t slot = window_id - 1;
  if (s_windows[slot].in_use.load() == 0) return;
  if (out_w) *out_w = s_windows[slot].width;
  if (out_h) *out_h = s_windows[slot].height;
}

/* Trigger a compositor flip for the given window's SHM surface. */
int dxvk_compositor_flip(uint32_t window_id) {
  if (window_id == 0 || window_id > kMaxWindows) return -1;
  uint32_t slot = window_id - 1;
  if (s_windows[slot].in_use.load() == 0) return -1;
  return (int)syscall(SYS_GUI_FLIP, (long)s_windows[slot].shm_handle);
}

void dxvk_compositor_destroy_window(uint32_t window_id) {
  if (window_id == 0 || window_id > kMaxWindows) return;
  uint32_t slot = window_id - 1;
  if (s_windows[slot].in_use.load() == 0) return;

  if (s_windows[slot].shm_handle)
    (void)syscall(SYS_SHM_DESTROY, (long)s_windows[slot].shm_handle);
  s_windows[slot].shm_handle = 0;
  s_windows[slot].width      = 0;
  s_windows[slot].height     = 0;
  s_windows[slot].mapped     = nullptr;
  s_windows[slot].in_use.store(0);
}

} /* extern "C" */
