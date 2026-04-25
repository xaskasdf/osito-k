/* dxvk_device.h -- OsitoK W5.1 minimal stub.
 *
 * The real DXVK dxvk_device.h is a 1000+ line header pulling in the
 * entire DXVK runtime (memory allocator, descriptor pool, queue
 * families, pipeline cache, bindings, etc.). It arrives in W5.2.
 *
 * DXBC only references Rc<DxvkDevice> via the dxbc_options.h ctor
 * signature; a forward declaration is sufficient for every TU we ship
 * in W5.1 (dxbc_options.cpp itself is deferred until the D3D11 frontend
 * is vendored).
 *
 * This stub intentionally lives under dxvk/src/dxvk/ (upstream path) so
 * the usual `#include "../dxvk/dxvk_device.h"` pattern keeps working.
 */
#pragma once

#include "dxvk_include.h"
#include "../util/rc/util_rc_ptr.h"

namespace dxvk {

  class DxvkDevice;

}
