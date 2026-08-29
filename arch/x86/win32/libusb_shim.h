/* Minimal libusb-1.0.dll surface for SDL HIDAPI. */

#ifndef LIBUSB_SHIM_H
#define LIBUSB_SHIM_H

#include "nttypes.h"
#include "win32_abi.h"

void libusb_shim_init(void);
PVOID libusb_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
const WIN32_EXPORT *libusb_abi_table(int *count);

#endif /* LIBUSB_SHIM_H */
