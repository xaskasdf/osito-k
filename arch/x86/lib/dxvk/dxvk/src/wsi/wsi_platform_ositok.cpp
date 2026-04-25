/* wsi_platform_ositok.cpp -- OsitoK W5.2 minimal WSI implementation.
 *
 * The DXVK src/wsi/ subsystem normally has three backends: Win32, SDL2,
 * GLFW. None of them apply to bare-metal OsitoK; we drive the framebuffer
 * directly via OsitoK syscalls. So we provide a one-file stub that:
 *
 *   * `init()`       — no-op; returns a dummy s_driver-equivalent
 *   * `quit()`       — no-op
 *   * `getInstanceExtensions()` — returns just VK_KHR_surface (host
 *                                  side requires no platform extensions)
 *   * Window/monitor queries     — return safe defaults (1920x1080, 60Hz)
 *   * createSurface              — returns VK_ERROR_FEATURE_NOT_PRESENT
 *
 * DXVK itself can run "headless" through the swapchain blitter as long
 * as the device is up; this stub allows DxvkInstance to instantiate
 * without forcing the full WSI plumbing. The real OsitoK presenter
 * (W5.4 / W5.5) will route VkSurface to the OsitoK compositor SHM.
 */

#include "wsi_platform.h"
#include "wsi_monitor.h"
#include "wsi_window.h"

namespace dxvk::wsi {

  void init() {
    /* no-op: no platform driver to bring up */
  }

  void quit() {
    /* no-op */
  }

  std::vector<const char *> getInstanceExtensions() {
    /* Just VK_KHR_surface; the ICD enables this unconditionally. */
    return std::vector<const char *>{ "VK_KHR_surface" };
  }

  /* ------- Window queries -------- */
  void getWindowSize(HWND, uint32_t *pWidth, uint32_t *pHeight) {
    if (pWidth)  *pWidth  = 1920;
    if (pHeight) *pHeight = 1080;
  }

  void resizeWindow(HWND, DxvkWindowState *, uint32_t, uint32_t) {
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

  VkResult createSurface(HWND, PFN_vkGetInstanceProcAddr, VkInstance, VkSurfaceKHR *pSurface) {
    if (pSurface) *pSurface = VK_NULL_HANDLE;
    return VK_ERROR_FEATURE_NOT_PRESENT;
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
    /* "OsitoK\0" in UTF-16; pad rest with zeros. */
    static const char kName[] = "OsitoK";
    for (uint32_t i = 0; i < 32; ++i)
      Name[i] = (i < sizeof(kName)) ? WCHAR(kName[i]) : WCHAR(0);
    return true;
  }

  bool getDesktopCoordinates(HMONITOR, RECT *pRect) {
    if (pRect) { pRect->left = 0; pRect->top = 0; pRect->right = 1920; pRect->bottom = 1080; }
    return true;
  }

  static void fillMode(WsiMode *pMode) {
    if (!pMode) return;
    pMode->width        = 1920;
    pMode->height       = 1080;
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
