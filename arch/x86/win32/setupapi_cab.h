#ifndef OSITO_SETUPAPI_CAB_H
#define OSITO_SETUPAPI_CAB_H
#include "nttypes.h"
BOOL WINAPI shim_SetupIterateCabinetA(PCSTR path, DWORD reserved,
                                      PVOID callback, PVOID context);
BOOL WINAPI shim_SetupIterateCabinetW(PCWSTR path, DWORD reserved,
                                      PVOID callback, PVOID context);
#endif
