/*
 * OsitoK Windows Compatibility Layer — comdlg32.dll Shim
 * Common Dialogs — all dialogs return FALSE (user cancelled).
 */

#include "comdlg32_shim.h"
#include "win32_abi.h"

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

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT comdlg32_exports[] = {
    { "GetOpenFileNameA",    (PVOID)shim_GetOpenFileNameA,     1, CC_STDCALL },
    { "GetSaveFileNameA",    (PVOID)shim_GetSaveFileNameA,     1, CC_STDCALL },
    { "ChooseColorA",        (PVOID)shim_ChooseColorA,         1, CC_STDCALL },
    { "ChooseFontA",         (PVOID)shim_ChooseFontA,          1, CC_STDCALL },
    { "CommDlgExtendedError",(PVOID)shim_CommDlgExtendedError, 0, CC_STDCALL },
    { "PrintDlgA",           (PVOID)shim_PrintDlgA,            1, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *comdlg32_abi_table(int *count) {
    *count = (int)(sizeof(comdlg32_exports)/sizeof(comdlg32_exports[0]));
    return (const WIN32_EXPORT *)comdlg32_exports;
}

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
