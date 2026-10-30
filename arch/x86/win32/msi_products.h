#ifndef OSITOK_MSI_PRODUCTS_H
#define OSITOK_MSI_PRODUCTS_H
#include "nttypes.h"
#include "advapi32_shim.h"
DWORD WINAPI MsiGetProductInfoW(PCWSTR product, PCWSTR attribute, PWSTR buffer, DWORD *capacity);
/* Internal readers of the completed, versioned per-machine registration. */
DWORD msi_product_open_registered(PCWSTR product, HKEY *key);
DWORD msi_product_read_string(HKEY key, PCWSTR name, PWSTR *text);
#endif
