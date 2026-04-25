/* windows.h -- minimal shim for OsitoK DXVK build.
 *
 * DXVK's com/com_include.h does `#include <windows.h>` even on Linux
 * builds (relying on Wine for the headers). We don't have Wine, so this
 * shim lives in com_stub/ and is found via -Idxvk/com_stub/.
 *
 * It re-exports IUnknown.h for the COM types and relies on the rest of
 * the Win32 ABI typedefs already provided by osito_compat/dxvk_compat.h
 * (force-included into every TU).
 */

#ifndef OSITOK_DXVK_WINDOWS_H
#define OSITOK_DXVK_WINDOWS_H

#include "IUnknown.h"

#endif /* OSITOK_DXVK_WINDOWS_H */
