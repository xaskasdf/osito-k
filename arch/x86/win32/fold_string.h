#ifndef OSITOK_FOLD_STRING_H
#define OSITOK_FOLD_STRING_H
#include "nttypes.h"
int WINAPI FoldStringW(DWORD flags, PCWSTR source, int length,
                       PWSTR destination, int capacity);
#endif
