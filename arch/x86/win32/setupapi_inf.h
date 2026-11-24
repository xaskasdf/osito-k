#ifndef OSITO_SETUPAPI_INF_H
#define OSITO_SETUPAPI_INF_H
#include "nttypes.h"
HANDLE WINAPI shim_SetupOpenInfFileA(PCSTR path, PCSTR inf_class, DWORD style, UINT *line);
BOOL WINAPI shim_SetupGetInfInformationA(PCVOID spec, DWORD search, PVOID output, DWORD capacity, DWORD *required);
BOOL WINAPI shim_SetupGetInfInformationW(PCVOID spec, DWORD search, PVOID output, DWORD capacity, DWORD *required);
BOOL WINAPI shim_SetupQueryInfFileInformationA(PCVOID input, UINT index, PSTR output, DWORD capacity, DWORD *required);
BOOL WINAPI shim_SetupQueryInfFileInformationW(PCVOID input, UINT index, PWSTR output, DWORD capacity, DWORD *required);
BOOL WINAPI shim_SetupQueryInfVersionInformationA(PCVOID input, UINT index, PCSTR key, PSTR output, DWORD capacity, DWORD *required);
BOOL WINAPI shim_SetupQueryInfVersionInformationW(PCVOID input, UINT index, PCWSTR key, PWSTR output, DWORD capacity, DWORD *required);
void WINAPI shim_SetupCloseInfFile(HANDLE inf);
BOOL WINAPI shim_SetupFindFirstLineA(HANDLE inf, PCSTR section, PCSTR key, PVOID context);
BOOL WINAPI shim_SetupFindFirstLineW(HANDLE inf, PCWSTR section, PCWSTR key, PVOID context);
BOOL WINAPI shim_SetupFindNextLine(PCVOID input, PVOID output);
BOOL WINAPI shim_SetupGetLineTextA(PCVOID context, HANDLE inf, PCSTR section,
    PCSTR key, PSTR output, DWORD capacity, DWORD *required);
BOOL WINAPI shim_SetupGetLineTextW(PCVOID context, HANDLE inf, PCWSTR section,
    PCWSTR key, PWSTR output, DWORD capacity, DWORD *required);
HANDLE WINAPI shim_SetupOpenFileQueue(void);
BOOL WINAPI shim_SetupCloseFileQueue(HANDLE queue);
PVOID WINAPI shim_SetupInitDefaultQueueCallback(HANDLE owner);
void WINAPI shim_SetupTermDefaultQueueCallback(PVOID context);
UINT WINAPI shim_SetupDefaultQueueCallbackA(PVOID context, UINT notification,
                                           ULONG_PTR param1, ULONG_PTR param2);
BOOL WINAPI shim_SetupInstallFromInfSectionA(HANDLE owner, HANDLE inf, PCSTR section,
    UINT flags, HANDLE root, PCSTR source, UINT copy_flags, PVOID callback,
    PVOID context, HANDLE devices, PVOID device);
/* Called only after the process's workers have stopped. */
void setupapi_release_process(DWORD owner);
#endif
