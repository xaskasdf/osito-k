#ifndef OSITOK_WIN32_IPROUTE_H
#define OSITOK_WIN32_IPROUTE_H
#include "nttypes.h"
DWORD WINAPI iphlp_GetBestInterface(DWORD destination, DWORD *index);
#endif
