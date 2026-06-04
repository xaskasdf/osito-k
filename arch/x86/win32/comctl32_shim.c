/*
 * OsitoK Windows Compatibility Layer — comctl32.dll Shim
 * Common Controls stubs — all creation calls return NULL.
 */

#include "comctl32_shim.h"
#include "win32_abi.h"

extern void serial_puts(const char *s);

/* ── Common Controls ───────────────────────────────────────── */

void WINAPI shim_InitCommonControls(void)
{
    serial_puts("[COMCTL32] InitCommonControls (stub)\n");
    /* no-op */
}

BOOL WINAPI shim_InitCommonControlsEx(PVOID icc)
{
    (void)icc;
    serial_puts("[COMCTL32] InitCommonControlsEx (stub)\n");
    return TRUE;
}

PVOID WINAPI shim_CreateStatusWindowA(LONG style, const char *text,
                                       PVOID hwnd, UINT id)
{
    (void)style; (void)text; (void)hwnd; (void)id;
    serial_puts("[COMCTL32] CreateStatusWindowA (stub — NULL)\n");
    return NULL;
}

PVOID WINAPI shim_ImageList_Create(int cx, int cy, UINT flags,
                                    int initial, int grow)
{
    (void)cx; (void)cy; (void)flags; (void)initial; (void)grow;
    serial_puts("[COMCTL32] ImageList_Create (stub — NULL)\n");
    return NULL;
}

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT comctl32_exports[] = {
    { "InitCommonControls",   (PVOID)shim_InitCommonControls,   0, CC_STDCALL },
    { "InitCommonControlsEx", (PVOID)shim_InitCommonControlsEx, 1, CC_STDCALL },
    { "CreateStatusWindowA",  (PVOID)shim_CreateStatusWindowA,  4, CC_STDCALL },
    { "ImageList_Create",     (PVOID)shim_ImageList_Create,     5, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *comctl32_abi_table(int *count) {
    *count = (int)(sizeof(comctl32_exports)/sizeof(comctl32_exports[0]));
    return (const WIN32_EXPORT *)comctl32_exports;
}

static int cc_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID comctl32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) {
        /* Well-known COMCTL32 ordinals */
        switch (ordinal) {
            case 17: return (PVOID)shim_InitCommonControlsEx;
            case 345: return (PVOID)shim_InitCommonControls;
            default: return NULL;
        }
    }
    for (int i = 0; comctl32_exports[i].name; i++) {
        if (cc_strcmp(func_name, comctl32_exports[i].name) == 0)
            return comctl32_exports[i].func;
    }
    return NULL;
}

PVOID comctl32_shim_init(void)
{
    serial_puts("[COMCTL32] comctl32.dll shim initialized\n");
    return (PVOID)comctl32_exports;
}
