#ifndef OSITOK_SHORT_PATH_H
#define OSITOK_SHORT_PATH_H
#include "nttypes.h"
DWORD WINAPI GetShortPathNameW(PCWSTR path, PWSTR output, DWORD capacity);
#endif
