/*
 * OsitoK Windows Compatibility Layer — winmm.dll Shim
 * Multimedia timers and legacy PCM output. Core.dll imports this.
 */

#ifndef WINMM_SHIM_H
#define WINMM_SHIM_H

#include "nttypes.h"

/* Timer error codes */
#define TIMERR_NOERROR      0
#define TIMERR_NOCANDO      97
#define TIMERR_STRUCT       129

/* Joystick error */
#define JOYERR_PARMS        165

/* Multimedia timer */
DWORD WINAPI shim_timeGetTime(void);
UINT  WINAPI shim_timeBeginPeriod(UINT period);
UINT  WINAPI shim_timeEndPeriod(UINT period);
UINT  WINAPI shim_timeGetDevCaps(PVOID capabilities, UINT size);
UINT  WINAPI shim_timeSetEvent(UINT delay, UINT resolution, PVOID callback,
                               ULONG_PTR user, UINT flags);
UINT  WINAPI shim_timeKillEvent(UINT timerID);

/* Joystick */
UINT  WINAPI shim_joyGetNumDevs(void);
UINT  WINAPI shim_joyGetDevCapsA(UINT id, PVOID caps, UINT size);

/* Sound */
BOOL  WINAPI shim_PlaySoundA(const char *sound, PVOID hmod, DWORD flags);
BOOL  WINAPI shim_PlaySoundW(PCWSTR sound, PVOID hmod, DWORD flags);
UINT  WINAPI shim_waveOutGetNumDevs(void);

/* MCI */
DWORD WINAPI shim_mciSendCommandA(UINT device, UINT msg, ULONG_PTR flags,
                                   ULONG_PTR param);
DWORD WINAPI shim_mciSendStringA(const char *cmd, char *ret, UINT retLen,
                                  PVOID hwnd);
DWORD WINAPI shim_mciSendStringW(PCWSTR cmd, PWSTR ret, UINT retLen,
                                  PVOID hwnd);

void  winmm_shim_init(void);
void  winmm_release_process(DWORD process_id);
PVOID winmm_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* WINMM_SHIM_H */
