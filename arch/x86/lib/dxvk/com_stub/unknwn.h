/* unknwn.h -- minimal shim for OsitoK DXVK build.
 *
 * DXVK's com/com_include.h includes <unknwn.h> after <windows.h>. We
 * just route both back to IUnknown.h; the include guard prevents double
 * processing.
 */

#ifndef OSITOK_DXVK_UNKNWN_H
#define OSITOK_DXVK_UNKNWN_H

#include "IUnknown.h"

#endif /* OSITOK_DXVK_UNKNWN_H */
