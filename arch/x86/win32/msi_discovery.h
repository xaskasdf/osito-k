#ifndef OSITOK_MSI_DISCOVERY_H
#define OSITOK_MSI_DISCOVERY_H
#include "nttypes.h"
DWORD WINAPI MsiEnumProductsW(DWORD index, PWSTR product);
DWORD WINAPI MsiEnumRelatedProductsW(PCWSTR upgrade, DWORD reserved, DWORD index, PWSTR product);
LONG WINAPI MsiQueryFeatureStateW(PCWSTR product, PCWSTR feature);
void msi_discovery_release_process(DWORD owner);
#endif
