/* wsi_platform_ositok.cpp -- OsitoK W5.4 WSI bridge.
 *
 * Replaces the W5.2 placeholder. DXVK's src/wsi/ subsystem normally has
 * three backends (Win32, SDL2, GLFW); none apply to bare-metal OsitoK.
 * Here we drive the framebuffer directly through:
 *
 *   * `VK_OSITOK_compositor_surface` — the OsitoK-proprietary WSI ext
 *     defined in W3b.5 of the Vulkan ICD work. Provides
 *     `vkCreateOsitokCompositorSurfaceKHR(instance, info, alloc, &surf)`.
 *
 *   * `dxvk_compositor_create_window(width, height)` — our
 *     osito_compat helper which wraps SYS_SHM_MKSURFACE (506) +
 *     SYS_GUI_FLIP (507). Returns a uint32_t window_id used as the SHM
 *     handle for surface creation.
 *
 * The `HWND` opaque pointer is mapped to a window_id via a small
 * 1-1 table; D3D11/DXGI never inspects HWND values, only passes them
 * through, so any non-NULL pointer maps correctly.
 *
 * Build: included in libdxvk_core.a (W5.2 wired the source into
 * CORE_SRCS already, so we just edit the body).
 */

#include "wsi_platform.h"
#include "wsi_monitor.h"
#include "wsi_window.h"

#include "vulkan/vulkan_ositok.h"

#include <cstdint>
#include <cstring>
#include <mutex>

/* Forward declaration of our compositor bridge (T4). Implemented in
 * arch/x86/lib/dxvk/osito_compat/dxvk_compositor_bridge.cpp.            */
extern "C" {
  uint32_t dxvk_compositor_create_window(uint32_t width, uint32_t height);
  uint32_t dxvk_compositor_window_shm(uint32_t window_id);
  void     dxvk_compositor_destroy_window(uint32_t window_id);
}

namespace dxvk::wsi {

  /* HWND -> window_id table. DXGI/D3D11 invent HWNDs by casting them
   * out of CreateSwapChainForHwnd's caller (typically a Win32 HWND on
   * Windows). On OsitoK we treat any non-null HWND as a valid window
   * tag and intern it on first sight.
   *
   * 16 entries is plenty: D3D11 swap chains rarely exceed a handful per
   * process and we never enumerate from this table.                    */
  struct HwndEntry {
    HWND     hwnd;
    uint32_t window_id;
    uint32_t width;
    uint32_t height;
  };
  static constexpr size_t kMaxHwnds = 16;
  static HwndEntry s_hwnds[kMaxHwnds] = {};
  static std::mutex s_hwnds_mutex;

  /* Look up an HWND in the table; return -1 if not present. */
  static int findHwndEntry(HWND h) {
    for (size_t i = 0; i < kMaxHwnds; ++i)
      if (s_hwnds[i].hwnd == h) return (int)i;
    return -1;
  }

  /* Intern an HWND, allocating a compositor window on first sight.
   * Returns the entry index or -1 on failure.                           */
  static int internHwnd(HWND h, uint32_t width, uint32_t height) {
    if (!h) return -1;
    std::lock_guard<std::mutex> lock(s_hwnds_mutex);
    int existing = findHwndEntry(h);
    if (existing >= 0) return existing;

    for (size_t i = 0; i < kMaxHwnds; ++i) {
      if (s_hwnds[i].hwnd == nullptr) {
        uint32_t wid = dxvk_compositor_create_window(width, height);
        if (wid == 0) return -1;
        s_hwnds[i].hwnd      = h;
        s_hwnds[i].window_id = wid;
        s_hwnds[i].width     = width;
        s_hwnds[i].height    = height;
        return (int)i;
      }
    }
    return -1;
  }

  void init() {
    /* No platform driver to bring up; the compositor bridge is created
     * lazily on first surface creation.                                 */
  }

  void quit() {
    std::lock_guard<std::mutex> lock(s_hwnds_mutex);
    for (size_t i = 0; i < kMaxHwnds; ++i) {
      if (s_hwnds[i].hwnd) {
        dxvk_compositor_destroy_window(s_hwnds[i].window_id);
        s_hwnds[i] = {};
      }
    }
  }

  std::vector<const char *> getInstanceExtensions() {
    /* DXVK uses VK_KHR_surface unconditionally; we additionally request
     * our private OSITOK compositor surface ext so the ICD enables it. */
    return std::vector<const char *>{
      "VK_KHR_surface",
      VK_OSITOK_COMPOSITOR_SURFACE_EXTENSION_NAME,
    };
  }

  /* ------- Window queries -------- */
  /* W5.4-fix: hold s_hwnds_mutex across find+read/write. DXVK's CS thread
   * + present thread + game thread can all reach these paths in W5.5
   * (GTA V); without the lock there's a data race against internHwnd /
   * destroy_window writes. */
  void getWindowSize(HWND h, uint32_t *pWidth, uint32_t *pHeight) {
    std::lock_guard<std::mutex> lock(s_hwnds_mutex);
    int idx = findHwndEntry(h);
    if (idx >= 0) {
      if (pWidth)  *pWidth  = s_hwnds[idx].width;
      if (pHeight) *pHeight = s_hwnds[idx].height;
    } else {
      /* Sane default before the swapchain has been created. */
      if (pWidth)  *pWidth  = 1024;
      if (pHeight) *pHeight = 768;
    }
  }

  void resizeWindow(HWND h, DxvkWindowState *, uint32_t w, uint32_t hgt) {
    std::lock_guard<std::mutex> lock(s_hwnds_mutex);
    int idx = findHwndEntry(h);
    if (idx >= 0) {
      s_hwnds[idx].width  = w;
      s_hwnds[idx].height = hgt;
    }
  }

  bool setWindowMode(HMONITOR, HWND, const WsiMode &) {
    return true;
  }

  bool enterFullscreenMode(HMONITOR, HWND, DxvkWindowState *, bool) {
    return true;
  }

  bool leaveFullscreenMode(HWND, DxvkWindowState *, bool) {
    return true;
  }

  bool restoreDisplayMode() {
    return true;
  }

  HMONITOR getWindowMonitor(HWND) {
    return reinterpret_cast<HMONITOR>(intptr_t(1));
  }

  bool isWindow(HWND hWindow) {
    return hWindow != nullptr;
  }

  void updateFullscreenWindow(HMONITOR, HWND, bool) {
  }

  /* The real meat: route through VK_OSITOK_compositor_surface. */
  VkResult createSurface(HWND hWindow, PFN_vkGetInstanceProcAddr pfnVkGipa,
                         VkInstance instance, VkSurfaceKHR *pSurface) {
    if (!pSurface) return VK_ERROR_INITIALIZATION_FAILED;
    *pSurface = VK_NULL_HANDLE;

    /* Intern HWND. If the caller hasn't sized its window yet, fall back
     * to the OsitoK compositor default 1024x768.                       */
    int idx = internHwnd(hWindow, 1024, 768);
    if (idx < 0) return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;

    /* Look up the OSITOK extension entry. We prefer instance dispatch
     * (vkGetInstanceProcAddr) so layered loaders can interpose.        */
    PFN_vkVoidFunction pfn = nullptr;
    if (pfnVkGipa)
      pfn = pfnVkGipa(instance, "vkCreateOsitokCompositorSurfaceKHR");

    if (!pfn) {
      /* Fall back to the directly-linked symbol from our libvulkan.    */
      *pSurface = VK_NULL_HANDLE;
      VkOsitoCompositorSurfaceCreateInfoOSITOK info = {};
      info.sType     = VK_STRUCTURE_TYPE_OSITOK_COMPOSITOR_SURFACE_CREATE_INFO;
      info.shmHandle = dxvk_compositor_window_shm(s_hwnds[idx].window_id);
      return ::vkCreateOsitokCompositorSurfaceKHR(instance, &info,
                                                  nullptr, pSurface);
    }

    using PFN_T = VkResult (VKAPI_PTR *)(VkInstance,
        const VkOsitoCompositorSurfaceCreateInfoOSITOK *,
        const VkAllocationCallbacks *, VkSurfaceKHR *);
    auto fp = reinterpret_cast<PFN_T>(pfn);

    VkOsitoCompositorSurfaceCreateInfoOSITOK info = {};
    info.sType     = VK_STRUCTURE_TYPE_OSITOK_COMPOSITOR_SURFACE_CREATE_INFO;
    info.pNext     = nullptr;
    info.flags     = 0;
    info.shmHandle = dxvk_compositor_window_shm(s_hwnds[idx].window_id);

    return fp(instance, &info, nullptr, pSurface);
  }

  /* ------- Monitor queries -------- */
  HMONITOR getDefaultMonitor() {
    return reinterpret_cast<HMONITOR>(intptr_t(1));
  }

  HMONITOR enumMonitors(uint32_t index) {
    return index == 0 ? reinterpret_cast<HMONITOR>(intptr_t(1)) : nullptr;
  }

  HMONITOR enumMonitors(const LUID *[], uint32_t, uint32_t index) {
    return index == 0 ? reinterpret_cast<HMONITOR>(intptr_t(1)) : nullptr;
  }

  bool getDisplayName(HMONITOR, WCHAR (&Name)[32]) {
    static const char kName[] = "OsitoK";
    for (uint32_t i = 0; i < 32; ++i)
      Name[i] = (i < sizeof(kName)) ? WCHAR(kName[i]) : WCHAR(0);
    return true;
  }

  bool getDesktopCoordinates(HMONITOR, RECT *pRect) {
    if (pRect) { pRect->left = 0; pRect->top = 0; pRect->right = 1024; pRect->bottom = 768; }
    return true;
  }

  static void fillMode(WsiMode *pMode) {
    if (!pMode) return;
    pMode->width        = 1024;
    pMode->height       = 768;
    pMode->refreshRate  = WsiRational{ 60, 1 };
    pMode->bitsPerPixel = 32;
    pMode->interlaced   = false;
  }

  bool getDisplayMode(HMONITOR, uint32_t modeNumber, WsiMode *pMode) {
    if (modeNumber > 0) return false;
    fillMode(pMode);
    return true;
  }

  bool getCurrentDisplayMode(HMONITOR, WsiMode *pMode) {
    fillMode(pMode);
    return true;
  }

  bool getDesktopDisplayMode(HMONITOR, WsiMode *pMode) {
    fillMode(pMode);
    return true;
  }

  WsiEdidData getMonitorEdid(HMONITOR) {
    return WsiEdidData{};
  }

}
