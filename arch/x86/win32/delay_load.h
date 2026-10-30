#ifndef OSITO_DELAY_LOAD_H
#define OSITO_DELAY_LOAD_H
#include "nttypes.h"
PVOID WINAPI ResolveDelayLoadedAPI(PVOID parent,PCVOID descriptor,PVOID dll_hook,
                                   PVOID system_hook,PVOID thunk,ULONG flags);
PVOID WINAPI DelayLoadFailureHook(PCSTR dll,PCSTR procedure);
#endif
