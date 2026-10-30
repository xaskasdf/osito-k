/* Resolve installed component key paths without modifying installation state. */
#include "msi_components.h"
#include "msi_products.h"
#include "msi_registration.h"
#include "kernel32_shim.h"
#include "ntprocess.h"
#include "compat32.h"
extern void *kmalloc(uint64_t bytes);
extern void kfree(void *pointer);
#define CW(text) ((PCWSTR)u##text)

static DWORD component_length(PCWSTR text)
{ DWORD length = 0; while (text[length]) length++; return length; }
static int component_argument(PCWSTR input, WCHAR output[39])
{
    if (!input) return 0;
    for (unsigned i = 0; i < 39; i++) {
        if (!win32_user_range_readable(input+i, 2, g_compat32_mode != 0)) return 0;
        output[i] = input[i];
        if (!output[i]) return i == 38 && output[0] == '{' && output[37] == '}';
    }
    return 0;
}
static int component_buffer_valid(PWSTR buffer, DWORD *capacity)
{
    if (!capacity) return !buffer;
    return win32_user_range_readable(capacity, 4, g_compat32_mode != 0) &&
           win32_user_range_writable(capacity, 4, g_compat32_mode != 0);
}
static DWORD component_number(HKEY key, PCWSTR name, DWORD *value)
{
    DWORD type = 0, size = 4;
    DWORD error = RegQueryValueExW(key, name, NULL, &type, (BYTE *)value, &size);
    return error ? error : type != REG_DWORD || size != 4 ? 1610 : 0;
}
static LONG component_output(LONG state, PCWSTR path, PWSTR buffer, DWORD *capacity)
{
    if (state != 3 && state != 2 && state != -7) return state;
    if (!path) path = CW("");
    DWORD length = component_length(path), size = capacity ? *capacity : 0;
    if (buffer && size > length) {
        if (!win32_user_range_writable(buffer, ((uint64_t)length+1)*2, g_compat32_mode != 0)) return -2;
        memcpy(buffer, path, ((uint64_t)length+1)*2);
    }
    if (capacity) *capacity = length;
    return buffer && length && size <= length ? -3 : state;
}
static LONG component_registry_path(HKEY metadata, PCWSTR path, PWSTR *output)
{
    DWORD root = 0, view = 0, key_only = 0;
    DWORD error = component_number(metadata, CW("Root"), &root);
    if (!error) error = component_number(metadata, CW("View"), &view);
    if (!error) error = component_number(metadata, CW("KeyOnly"), &key_only);
    if (!error && (root > 3 || (view != 0x100 && view != 0x200) || key_only > 1)) error = 1610;
    PWSTR name = NULL;
    if (!error) error = msi_product_read_string(metadata, CW("Name"), &name);
    if (error) { kfree(name); return error == 8 ? -1 : -6; }
    DWORD path_length = component_length(path), name_length = key_only ? 0 : component_length(name);
    if (path_length + name_length > 32760) { kfree(name); return -6; }
    PWSTR result = kmalloc(((uint64_t)path_length+name_length+6)*2);
    if (!result) { kfree(name); return -1; }
    result[0] = view == 0x100 ? '2' : '0'; result[1] = (WCHAR)('0'+root);
    result[2] = ':'; result[3] = '\\';
    memcpy(result+4, path, path_length*2U); result[4+path_length] = '\\';
    if (name_length) memcpy(result+5+path_length, name, name_length*2U);
    result[5+path_length+name_length] = 0;
    HKEY key = NULL;
    error = RegOpenKeyExW((HKEY)(ULONG_PTR)(LONG)(0x80000000U+root), path, 0, KEY_READ | view, &key);
    if (!error && !key_only) {
        DWORD size = 0;
        /* Value names can themselves contain backslashes; keep the stored
         * name separate from the key instead of parsing our public string. */
        error = RegQueryValueExW(key, name, NULL, NULL, NULL, &size);
    }
    if (key) RegCloseKey(key);
    kfree(name); *output = result;
    return error ? 2 : 3;
}
static LONG component_lookup(PCWSTR product, PCWSTR component, PWSTR *output)
{
    *output = NULL;
    if (!msi_registration_guid(component)) return -1;
    HKEY installed = NULL, metadata = NULL;
    DWORD error = msi_product_open_registered(product, &installed);
    if (error) return error == 1610 ? -6 : -1;
    WCHAR relative[50];
    memcpy(relative, CW("Components\\"), 11U*2); memcpy(relative+11, component, 39U*2);
    error = RegOpenKeyExW(installed, relative, 0, KEY_READ | KEY_WOW64_64KEY, &metadata);
    RegCloseKey(installed);
    if (error) return -1;
    DWORD state = 0, kind = 0;
    error = component_number(metadata, CW("State"), &state);
    LONG result = -6;
    PWSTR path = NULL;
    if (!error && state == 2) result = -7; /* Authored, disabled component. */
    else if (!error && state == 3) {
        error = component_number(metadata, CW("Kind"), &kind);
        if (!error) error = msi_product_read_string(metadata, CW("Path"), &path);
        if (error == 8) result = -1;
        else if (!error && kind == 2) result = component_registry_path(metadata, path, output);
        else if (!error && (kind == 1 || kind == 3) && *path) {
            /* The cached path already names the installed physical directory.
             * A PE32 query of a 64-bit component must not redirect System32. */
            BOOL previous = nt_thread_fs_redirection(1);
            DWORD attributes = GetFileAttributesW(path);
            nt_thread_fs_redirection(previous);
            result = attributes == 0xffffffffU ? 2 : 3;
            *output = path; path = NULL;
        }
    }
    RegCloseKey(metadata); kfree(path); return result;
}
LONG WINAPI MsiGetComponentPathW(PCWSTR input_product, PCWSTR input_component, PWSTR buffer, DWORD *capacity)
{
    WCHAR product[39], component[39];
    if (!component_argument(input_product, product) || !component_argument(input_component, component) ||
        !component_buffer_valid(buffer, capacity)) return -2;
    PWSTR path = NULL;
    LONG state = component_lookup(product, component, &path);
    LONG result = component_output(state, path, buffer, capacity);
    kfree(path); return result;
}
LONG WINAPI MsiLocateComponentW(PCWSTR input_component, PWSTR buffer, DWORD *capacity)
{
    WCHAR component[39];
    if (!component_argument(input_component, component) || !component_buffer_valid(buffer, capacity)) return -2;
    if (!msi_registration_guid(component)) return -1;
    HKEY store = NULL;
    DWORD error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, MSI_PRODUCT_STORE, 0, KEY_READ | KEY_WOW64_64KEY, &store);
    if (error) return -1;
    LONG best = -1; PWSTR best_path = NULL;
    /* Walk independently of the caller's MsiEnumProducts cursor. Shared
     * components can have disabled clients as well as an installed client. */
    for (DWORD index = 0;; index++) {
        WCHAR product[39]; DWORD length = 39;
        error = RegEnumKeyExW(store, index, product, &length, NULL, NULL, NULL, NULL);
        if (error) { if (error != 259 && best == -1) best = -6; break; }
        if (!msi_registration_guid(product)) continue;
        PWSTR path = NULL;
        LONG state = component_lookup(product, component, &path);
        if (state == 3 || ((state == 2 || state == -7) &&
            (best == -1 || best == -6 || (best == -7 && state == 2)))) {
            kfree(best_path); best_path = path; path = NULL; best = state;
        } else if (state == -6 && best == -1) best = -6;
        kfree(path);
        if (best == 3) break;
    }
    RegCloseKey(store);
    LONG result = component_output(best, best_path, buffer, capacity);
    kfree(best_path); return result;
}
