/* OsitoK Win32 DirectSound playback shim. */

#ifndef DSOUND_SHIM_H
#define DSOUND_SHIM_H

#include "nttypes.h"

typedef LONG HRESULT;

HRESULT WINAPI DirectSoundCreate(LPCGUID device, PVOID output, PVOID outer);
HRESULT WINAPI DirectSoundEnumerateA(PVOID callback, PVOID context);
HRESULT WINAPI DirectSoundEnumerateW(PVOID callback, PVOID context);
HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID device, PVOID output,
                                        PVOID outer);
HRESULT WINAPI DirectSoundCaptureEnumerateA(PVOID callback, PVOID context);
HRESULT WINAPI DirectSoundCaptureEnumerateW(PVOID callback, PVOID context);

void dsound_release_process(DWORD process_id);
int dsound_selftest(void);

PVOID dsound_shim_init(void);
PVOID dsound_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif
