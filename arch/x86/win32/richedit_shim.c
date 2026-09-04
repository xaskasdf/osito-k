/*
 * OsitoK Windows Compatibility Layer - Rich Edit DLL classes
 *
 * Windows registers Rich Edit window classes when the corresponding DLL is
 * loaded. Keep that ownership boundary so GetClassInfo and FreeLibrary see
 * the same class lifetime as native Win32.
 */

#include "richedit_shim.h"
#include "compat32.h"
#include "user32_shim.h"

extern void serial_puts(const char *s);

#define RICHEDIT_CLASS_STYLE 0x00004088U

static int rich_stricmp(const char *a, const char *b)
{
    if (!a || !b)
        return a ? 1 : (b ? -1 : 0);
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static LRESULT WINAPI rich_edit_wndproc(HWND window, DWORD message,
                                        WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

static BOOL register_classes(const char *const *classes, SIZE_T count)
{
    int cb_wnd_extra = (g_compat32_mode ||
                        dll_current_process_bitness() == 32) ? 4 : 8;
    SIZE_T registered = 0;
    for (; registered < count; registered++) {
        if (!user32_register_library_class(
                classes[registered], RICHEDIT_CLASS_STYLE, 0, cb_wnd_extra,
                (HBRUSH)(ULONG_PTR)6, rich_edit_wndproc)) {
            while (registered > 0) {
                registered--;
                user32_unregister_library_class(classes[registered],
                                                rich_edit_wndproc);
            }
            return FALSE;
        }
    }
    return TRUE;
}

static void unregister_classes(const char *const *classes, SIZE_T count)
{
    for (SIZE_T i = 0; i < count; i++)
        user32_unregister_library_class(classes[i], rich_edit_wndproc);
}

BOOL richedit_module_event(const char *dll_name, PVOID module,
                           DWORD reason, PVOID reserved)
{
    static const char *const riched32_classes[] = { "RICHEDIT" };
    static const char *const riched20_classes[] = {
        "RichEdit20A", "RichEdit20W"
    };
    static const char *const msftedit_classes[] = { "RICHEDIT50W" };
    const char *const *classes = NULL;
    SIZE_T count = 0;
    (void)module;
    (void)reserved;

    if (rich_stricmp(dll_name, "riched32.dll") == 0) {
        classes = riched32_classes;
        count = sizeof(riched32_classes) / sizeof(riched32_classes[0]);
    } else if (rich_stricmp(dll_name, "riched20.dll") == 0) {
        classes = riched20_classes;
        count = sizeof(riched20_classes) / sizeof(riched20_classes[0]);
    } else if (rich_stricmp(dll_name, "msftedit.dll") == 0) {
        classes = msftedit_classes;
        count = sizeof(msftedit_classes) / sizeof(msftedit_classes[0]);
    } else {
        return FALSE;
    }

    if (reason == DLL_PROCESS_ATTACH) {
        serial_puts("[RICHEDIT] attach ");
        serial_puts(dll_name);
        serial_puts("\n");
        return register_classes(classes, count);
    }
    if (reason == DLL_PROCESS_DETACH) {
        unregister_classes(classes, count);
        return TRUE;
    }
    return TRUE;
}

PVOID richedit_resolve(const char *func_name, USHORT ordinal,
                       BOOL by_ordinal)
{
    (void)func_name;
    (void)ordinal;
    (void)by_ordinal;
    return NULL;
}
