#ifndef OSITO_WIN32_WOW64_H
#define OSITO_WIN32_WOW64_H

#include "nttypes.h"
#include <stdbool.h>

/* Input/output: normalized, volume-relative backing path; apply only once. */
bool win32_redirect_path(char path[260]);
/* Resolve a Win32 file operation, preserving the CWD's physical directory. */
bool win32_resolve_file_path(PCSTR path, char out[260]);
BOOL WINAPI Wow64DisableWow64FsRedirection(PVOID *previous);
BOOL WINAPI Wow64RevertWow64FsRedirection(PVOID previous);
BYTE WINAPI Wow64EnableWow64FsRedirection(BYTE enable);

#endif
