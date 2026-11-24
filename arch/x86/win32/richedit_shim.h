/*
 * OsitoK Windows Compatibility Layer - Rich Edit DLL classes
 */

#ifndef RICHEDIT_SHIM_H
#define RICHEDIT_SHIM_H

#include "dllloader.h"

PVOID richedit_resolve(const char *func_name, USHORT ordinal,
                       BOOL by_ordinal);
BOOL richedit_module_event(const char *dll_name, PVOID module,
                           DWORD reason, PVOID reserved);

#endif /* RICHEDIT_SHIM_H */
