#ifndef OSITOK_MSI_COMPONENTS_H
#define OSITOK_MSI_COMPONENTS_H
#include "nttypes.h"
LONG WINAPI MsiGetComponentPathW(PCWSTR product, PCWSTR component, PWSTR path, DWORD *capacity);
LONG WINAPI MsiLocateComponentW(PCWSTR component, PWSTR path, DWORD *capacity);
#endif
