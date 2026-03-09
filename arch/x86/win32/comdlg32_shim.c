/*
 * OsitoK Windows Compatibility Layer — comdlg32.dll Shim
 * Common Dialogs — all dialogs return FALSE (user cancelled).
 */

#include "comdlg32_shim.h"

extern void serial_puts(const char *s);

/* ── Common Dialogs ────────────────────────────────────────── */

BOOL WINAPI shim_GetOpenFileNameA(PVOID ofn)
{
    (void)ofn;
    serial_puts("[COMDLG32] GetOpenFileNameA (stub — cancelled)\n");
    return FALSE;
}

BOOL WINAPI shim_GetSaveFileNameA(PVOID ofn)
{
    (void)ofn;
    serial_puts("[COMDLG32] GetSaveFileNameA (stub — cancelled)\n");
    return FALSE;
}

BOOL WINAPI shim_ChooseColorA(PVOID cc)
{
    (void)cc;
    serial_puts("[COMDLG32] ChooseColorA (stub — cancelled)\n");
    return FALSE;
}

BOOL WINAPI shim_ChooseFontA(PVOID cf)
{
    (void)cf;
    serial_puts("[COMDLG32] ChooseFontA (stub — cancelled)\n");
    return FALSE;
}

DWORD WINAPI shim_CommDlgExtendedError(void)
{
    return 0; /* no error */
}

BOOL WINAPI shim_PrintDlgA(PVOID pd)
{
    (void)pd;
    serial_puts("[COMDLG32] PrintDlgA (stub — cancelled)\n");
    return FALSE;
}

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; } SHIM_EXPORT;

static const SHIM_EXPORT comdlg32_exports[] = {
    { "GetOpenFileNameA",    (PVOID)shim_GetOpenFileNameA },
    { "GetSaveFileNameA",    (PVOID)shim_GetSaveFileNameA },
    { "ChooseColorA",        (PVOID)shim_ChooseColorA },
    { "ChooseFontA",         (PVOID)shim_ChooseFontA },
    { "CommDlgExtendedError",(PVOID)shim_CommDlgExtendedError },
    { "PrintDlgA",           (PVOID)shim_PrintDlgA },
    { NULL, NULL }
};

static int cd_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID comdlg32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal) return NULL;
    for (int i = 0; comdlg32_exports[i].name; i++) {
        if (cd_strcmp(func_name, comdlg32_exports[i].name) == 0)
            return comdlg32_exports[i].func;
    }
    return NULL;
}

PVOID comdlg32_shim_init(void)
{
    serial_puts("[COMDLG32] comdlg32.dll shim initialized\n");
    return (PVOID)comdlg32_exports;
}
