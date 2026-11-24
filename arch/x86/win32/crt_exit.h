#ifndef OSITO_CRT_EXIT_H
#define OSITO_CRT_EXIT_H
#include "nttypes.h"
int crt_exit_callbacks_register(PVOID function);
void crt_exit_callbacks_run(void);
void crt_exit_callbacks_release(DWORD pid);
#endif
