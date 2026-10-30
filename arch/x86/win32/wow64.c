#include "wow64.h"
#include "kernel32_shim.h"
#include "dllloader.h"
#include "ntprocess.h"
#include "system_paths.h"

extern int g_compat32_mode;

static bool wow64_is_32bit(void)
{
    int bits = dll_current_process_bitness();
    return bits ? bits == 32 : g_compat32_mode != 0;
}

static unsigned wow64_path_prefix(const char *path, const char *prefix)
{
    unsigned i = 0;
    while (prefix[i]) {
        char a = path[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
        i++;
    }
    return !path[i] || path[i] == '\\' ? i : 0;
}

static bool wow64_replace_prefix(char path[260], unsigned old_length,
                                  const char *replacement)
{
    char result[260];
    unsigned n = 0;
    while (*replacement && n < 259) result[n++] = *replacement++;
    if (*replacement) return false;
    const char *tail = path + old_length;
    while (*tail && n < 259) result[n++] = *tail++;
    if (*tail) return false;
    result[n] = 0;
    for (unsigned i = 0; i <= n; i++) path[i] = result[i];
    return true;
}

bool win32_redirect_path(char path[260])
{
    if (!path) return false;
    if (!wow64_is_32bit()) return true;
    unsigned length = wow64_path_prefix(path, WIN32_WINDOWS_RELATIVE "\\Sysnative");
    /* Sysnative is an explicit alias even while automatic redirection is off. */
    if (length)
        return wow64_replace_prefix(path, length, WIN32_SYSTEM32_RELATIVE);
    if (nt_thread_fs_redirection(-1)) return true;

    length = wow64_path_prefix(path, WIN32_SYSTEM32_RELATIVE);
    if (length) {
        static const char *const exclusions[] = {
            "catroot", "catroot2", "driverstore", "drivers\\etc", "logfiles", "spool"
        };
        if (path[length] == '\\') {
            for (unsigned i = 0; i < sizeof(exclusions) / sizeof(exclusions[0]); i++)
                if (wow64_path_prefix(path + length + 1, exclusions[i]))
                    return true;
        }
        return wow64_replace_prefix(path, length, WIN32_SYSWOW64_RELATIVE);
    }
    length = wow64_path_prefix(path, WIN32_WINDOWS_RELATIVE "\\lastgood\\System32");
    if (length)
        return wow64_replace_prefix(path, length,
                                    WIN32_WINDOWS_RELATIVE "\\lastgood\\SysWOW64");
    length = wow64_path_prefix(path, WIN32_WINDOWS_RELATIVE "\\regedit.exe");
    if (length && !path[length])
        return wow64_replace_prefix(path, length, WIN32_SYSWOW64_RELATIVE "\\regedit.exe");
    return true;
}

static bool wow64_path_is_relative(const char *path)
{
    if (!path || !*path || *path == '\\' || *path == '/') return false;
    return !(path[1] == ':' && (path[2] == '\\' || path[2] == '/'));
}

bool win32_resolve_file_path(PCSTR path, char out[260])
{
    if (!path || !out) return false;
    if (wow64_path_is_relative(path)) {
        /* Windows keeps an opened directory as well as its lexical CWD. A
         * later Disable call must not retarget relative I/O to another folder. */
        const char *cwd = kernel32_current_directory_backing();
        char absolute[524] = "C:\\";
        unsigned n = 3;
        for (unsigned i = 0; cwd[i] && n < sizeof(absolute) - 1; i++)
            absolute[n++] = cwd[i];
        if (n > 3) absolute[n++] = '\\';
        const char *tail = path[0] && path[1] == ':' ? path + 2 : path;
        while (*tail && n < sizeof(absolute) - 1) absolute[n++] = *tail++;
        if (*tail) return false;
        absolute[n] = 0;
        if (!win32_normalize_path(absolute, out)) return false;
        if (wow64_path_prefix(cwd, WIN32_SYSTEM32_RELATIVE) &&
            wow64_path_prefix(out, WIN32_SYSTEM32_RELATIVE)) return true;
        return win32_redirect_path(out);
    }
    return win32_normalize_path(path, out) && win32_redirect_path(out);
}

BOOL WINAPI Wow64DisableWow64FsRedirection(PVOID *previous)
{
    if (!wow64_is_32bit()) {
        SetLastError(1); /* ERROR_INVALID_FUNCTION on a native 64-bit caller. */
        return FALSE;
    }
    if (!previous) { SetLastError(87); return FALSE; }
    /* This API succeeds only in PE32; its output pointer is four bytes wide. */
    *(uint32_t *)previous = (uint32_t)nt_thread_fs_redirection(1);
    return TRUE;
}

BOOL WINAPI Wow64RevertWow64FsRedirection(PVOID previous)
{
    if (!wow64_is_32bit()) { SetLastError(1); return FALSE; }
    if ((ULONG_PTR)previous > 1) { SetLastError(87); return FALSE; }
    (void)nt_thread_fs_redirection(previous != NULL);
    return TRUE;
}

BYTE WINAPI Wow64EnableWow64FsRedirection(BYTE enable)
{
    if (!wow64_is_32bit()) { SetLastError(1); return FALSE; }
    (void)nt_thread_fs_redirection(!enable);
    return TRUE;
}
