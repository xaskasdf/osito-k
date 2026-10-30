/* OsitoFS has no DOS aliases: existing names retain their original spelling. */
#include "short_path.h"
#include "kernel32_shim.h"
#include "compat32.h"
extern void *memmove(void *destination, const void *source, uint64_t bytes);

DWORD WINAPI GetShortPathNameW(PCWSTR path, PWSTR output, DWORD capacity)
{
    if (!path) { SetLastError(87); return 0; }
    BOOL compat = g_compat32_mode != 0;
    DWORD length = 0, saved = GetLastError();
    for (;;) {
        if (!win32_user_range_readable(path+length, 2, compat)) { SetLastError(87); return 0; }
        if (!path[length]) break;
        if (++length == 32767) { SetLastError(206); return 0; }
    }
    if (!length) { SetLastError(3); return 0; }
    if (GetFileAttributesW(path) == 0xffffffffU) return 0;
    SetLastError(saved);
    if (!output || capacity <= length) return length+1;
    if (!win32_user_range_writable(output, ((uint64_t)length+1)*2, compat)) {
        SetLastError(87); return 0;
    }
    memmove(output, path, ((uint64_t)length+1)*2);
    return length;
}
