/* wsi_edid_ositok.cpp -- OsitoK W5.2 EDID parser stub.
 *
 * The upstream wsi_edid.cpp pulls in libdisplay-info to extract HDR
 * colorimetry from a monitor's EDID blob. We don't have libdisplay-info
 * (no Mesa, no display-info bindings), and on a bare-metal QEMU GPU the
 * EDID is fixed and uninteresting. Stub returns nullopt so DXVK's
 * NormalizeDisplayMetadata caller falls back to its sensible defaults
 * (sRGB primaries, no HDR static metadata).
 */

#include "wsi_edid.h"

namespace dxvk::wsi {

  std::optional<WsiDisplayMetadata> parseColorimetryInfo(
      const WsiEdidData&) {
    return std::nullopt;
  }

}
