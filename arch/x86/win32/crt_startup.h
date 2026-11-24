#ifndef OSITO_CRT_STARTUP_H
#define OSITO_CRT_STARTUP_H
#include "nttypes.h"
int crt_startup_args(int *argc, void *argv, void *env, int wildcard,
                     void *startinfo, unsigned width);
void crt_startup_release_process(DWORD process_id);
#endif
