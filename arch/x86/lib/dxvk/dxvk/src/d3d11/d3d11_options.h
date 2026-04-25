#pragma once

/* d3d11_options.h -- OsitoK W5.2 stub.
 *
 * The upstream `dxbc/dxbc_options.cpp` references a handful of fields
 * from `dxvk::D3D11Options` (the runtime tuning knobs the D3D11 frontend
 * passes to the DXBC compiler at shader translation time). The full
 * struct lives in src/d3d11/ which is W5.3 territory, so this is a
 * minimal stub exposing exactly the fields read by dxbc_options.cpp.
 *
 * When the real D3D11 frontend lands in W5.3 this file goes away — the
 * vendored upstream src/d3d11/d3d11_options.h overwrites it via the
 * include search-path order.
 *
 * Fields cross-checked against dxbc_options.cpp lines 35-44:
 *   invariantPosition, zeroInitWorkgroupMemory, forceVolatileTgsmAccess,
 *   disableMsaa, forceSampleRateShading, longMad, floatControls.
 *
 * All defaulted to false/0 so dxbc translation runs the conservative
 * D3D11 codepath (no quirks enabled).
 */

#include <cstdint>

namespace dxvk {

  struct D3D11Options {
    /* Fields read by dxbc/dxbc_options.cpp. Default-initialise to the
     * upstream "no quirks" state. */
    bool   invariantPosition       = false;
    bool   zeroInitWorkgroupMemory = false;
    bool   forceVolatileTgsmAccess = false;
    bool   disableMsaa             = false;
    bool   forceSampleRateShading  = false;
    bool   longMad                 = false;
    bool   floatControls           = false;
  };

}
