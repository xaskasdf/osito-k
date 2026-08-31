/*
 * OsitoK Windows Compatibility Layer — dsound.dll Shim
 * Stub — no-op buffers, silent audio. UT99 initializes DirectSound
 * but can run without it (silent mode).
 */

#ifndef DSOUND_SHIM_H
#define DSOUND_SHIM_H

#include "nttypes.h"

typedef LONG HRESULT;

/* GUID, LPGUID, LPCGUID defined in nttypes.h */

HRESULT WINAPI DirectSoundCreate(LPCGUID lpcGuidDevice, PVOID *ppDS, PVOID pUnkOuter);
HRESULT WINAPI DirectSoundEnumerateA(PVOID lpDSEnumCallback, PVOID lpContext);
HRESULT WINAPI DirectSoundEnumerateW(PVOID lpDSEnumCallback, PVOID lpContext);
HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID lpcGuidDevice, PVOID *ppDSC,
                                        PVOID pUnkOuter);
HRESULT WINAPI DirectSoundCaptureEnumerateA(PVOID lpDSEnumCallback,
                                             PVOID lpContext);
HRESULT WINAPI DirectSoundCaptureEnumerateW(PVOID lpDSEnumCallback,
                                             PVOID lpContext);

PVOID dsound_shim_init(void);
PVOID dsound_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* DSOUND_SHIM_H */
