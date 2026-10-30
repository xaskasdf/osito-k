#ifndef OSITO_WIN32_TIMER_QUEUE_H
#define OSITO_WIN32_TIMER_QUEUE_H

#include "nttypes.h"

typedef void (WINAPI *K32_TIMER_CALLBACK)(PVOID, BYTE);
typedef void (WINAPI *K32_POOL_TIMER_CALLBACK)(PVOID, PVOID, HANDLE);
HANDLE WINAPI k32_CreateThreadpoolTimer(K32_POOL_TIMER_CALLBACK, PVOID, PCVOID);
void WINAPI k32_SetThreadpoolTimer(HANDLE, PCVOID, DWORD, DWORD);
void WINAPI k32_WaitForThreadpoolTimerCallbacks(HANDLE, BOOL);
void WINAPI k32_CloseThreadpoolTimer(HANDLE);
BOOL WINAPI k32_IsThreadpoolTimerSet(HANDLE);
HANDLE WINAPI k32_CreateTimerQueue(void);
BOOL WINAPI k32_CreateTimerQueueTimer(HANDLE *, HANDLE, K32_TIMER_CALLBACK,
                                       PVOID, DWORD, DWORD, ULONG);
BOOL WINAPI k32_ChangeTimerQueueTimer(HANDLE, HANDLE, ULONG, ULONG);
BOOL WINAPI k32_DeleteTimerQueueTimer(HANDLE, HANDLE, HANDLE);
BOOL WINAPI k32_DeleteTimerQueueEx(HANDLE, HANDLE);
BOOL WINAPI k32_DeleteTimerQueue(HANDLE);
void k32_timer_queues_release_process(DWORD);

#endif
