#ifndef DINPUT8_SHIM_H
#define DINPUT8_SHIM_H

#include "nttypes.h"

typedef LONG HRESULT;

HRESULT WINAPI DirectInput8Create(HANDLE instance, DWORD version, REFIID iid,
                                  PVOID output, PVOID outer);

PVOID dinput8_shim_init(void);
PVOID dinput8_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
void dinput8_release_process(DWORD process_id);

#endif /* DINPUT8_SHIM_H */
