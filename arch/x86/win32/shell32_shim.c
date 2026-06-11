/*
 * OsitoK Windows Compatibility Layer — shell32.dll Shim Implementation
 *
 * Stub implementation — ShellExecute logs the request and returns
 * success (handle > 32) without launching any process.
 */

#include "shell32_shim.h"
#include "win32_abi.h"

extern void serial_puts(const char *s);

/* ── API Implementations ───────────────────────────────────── */

/* True if path ends in ".exe" (case-insensitive) — UT99 relaunches itself this
 * way to apply a video/color-depth change. */
static int ends_in_exe(PCSTR p)
{
    if (!p) return 0;
    int n = 0; while (p[n]) n++;
    if (n < 4) return 0;
    const char *e = p + n - 4;
    return (e[0] == '.' &&
            (e[1] == 'e' || e[1] == 'E') &&
            (e[2] == 'x' || e[2] == 'X') &&
            (e[3] == 'e' || e[3] == 'E'));
}

HINSTANCE WINAPI ShellExecuteA(HWND hwnd, PCSTR lpOperation, PCSTR lpFile,
                                PCSTR lpParameters, PCSTR lpDirectory, int nShowCmd)
{
    (void)hwnd; (void)lpParameters; (void)lpDirectory; (void)nShowCmd;
    serial_puts("[SHELL32] ShellExecuteA: ");
    if (lpOperation) { serial_puts(lpOperation); serial_puts(" "); }
    if (lpFile) serial_puts(lpFile);
    serial_puts("\n");
    /* Launching an .exe = the game relaunching itself → request a re-exec so the
     * current process's ExitProcess restarts the EXE instead of exiting dead. */
    if (ends_in_exe(lpFile)) {
        extern int g_win32_relaunch;
        g_win32_relaunch = 1;
        serial_puts("[SHELL32] .exe launch → RE-EXEC requested\n");
    }
    return (HINSTANCE)(ULONG_PTR)32; /* >32 = success */
}

HINSTANCE WINAPI ShellExecuteW(HWND hwnd, PCWSTR lpOperation, PCWSTR lpFile,
                                PCWSTR lpParameters, PCWSTR lpDirectory, int nShowCmd)
{
    (void)hwnd; (void)lpOperation; (void)lpFile;
    (void)lpParameters; (void)lpDirectory; (void)nShowCmd;
    serial_puts("[SHELL32] ShellExecuteW: stub\n");
    return (HINSTANCE)(ULONG_PTR)32; /* >32 = success */
}

BOOL WINAPI Shell_NotifyIconA(DWORD dwMessage, PVOID lpData)
{
    (void)dwMessage; (void)lpData;
    return TRUE;
}

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT shell32_exports[] = {
    { "ShellExecuteA",      (PVOID)ShellExecuteA,     6, CC_STDCALL },
    { "ShellExecuteW",      (PVOID)ShellExecuteW,     6, CC_STDCALL },
    { "Shell_NotifyIconA",  (PVOID)Shell_NotifyIconA, 2, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *shell32_abi_table(int *count) {
    *count = (int)(sizeof(shell32_exports)/sizeof(shell32_exports[0]));
    return (const WIN32_EXPORT *)shell32_exports;
}

static int shell_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID shell32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;
    for (int i = 0; shell32_exports[i].name; i++) {
        if (shell_strcmp(func_name, shell32_exports[i].name) == 0)
            return shell32_exports[i].func;
    }
    return NULL;
}

PVOID shell32_shim_init(void)
{
    return (PVOID)shell32_exports;
}
