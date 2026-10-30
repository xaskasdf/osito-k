/* Queries use the same per-machine, versioned metadata the install journal
 * publishes. No product is inferred from DLL presence or authored properties. */
#include "msi_products.h"
#include "msi_registration.h"
#include "advapi32_shim.h"
#include "compat32.h"
extern void *kmalloc(uint64_t bytes);
extern void kfree(void *pointer);
#define PW(text) ((PCWSTR)u##text)

static DWORD product_argument(PCWSTR input, PWSTR output, DWORD capacity)
{
    if (!input) return 87;
    for (DWORD i = 0; i < capacity; i++) {
        if (!win32_user_range_readable(input + i, 2, g_compat32_mode != 0)) return 87;
        output[i] = input[i]; if (!output[i]) return 0;
    }
    return 87;
}
static int product_equal(PCWSTR a, PCWSTR b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static DWORD product_length(PCWSTR text)
{ DWORD n = 0; while (text[n]) n++; return n; }
static DWORD product_number(HKEY key, PCWSTR name, DWORD *value)
{
    DWORD type = 0, size = 4;
    DWORD error = RegQueryValueExW(key, name, NULL, &type, (BYTE *)value, &size);
    if (error == 2 || error == 234 || (!error && (type != REG_DWORD || size != 4))) return 1610;
    return error;
}
static DWORD product_open(PCWSTR product, HKEY *key)
{
    *key = NULL;
    if (!msi_registration_guid(product)) return 1605;
    WCHAR path[128]; DWORD n = product_length(MSI_PRODUCT_STORE);
    memcpy(path, MSI_PRODUCT_STORE, n * 2U); memcpy(path + n, product, 39U * 2);
    DWORD error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, key);
    if (error) return error == 2 || error == 3 ? 1605 : error;
    DWORD state = 0, schema = 0, context = 0, type = 0, size = 4;
    error = RegQueryValueExW(*key, PW("State"), NULL, &type, (BYTE *)&state, &size);
    if (error == 2) error = 1605; /* Registration has not committed its marker. */
    else if (error == 234 || (!error && (type != REG_DWORD || size != 4))) error = 1610;
    else if (!error && state != 5) error = 1605;
    if (!error) error = product_number(*key, PW("SchemaVersion"), &schema);
    if (!error) error = product_number(*key, PW("Context"), &context);
    if (!error && (schema != 1 || context != 4)) error = 1610;
    if (error) { RegCloseKey(*key); *key = NULL; }
    return error;
}
static DWORD product_string(HKEY key, PCWSTR name, PWSTR *out)
{
    *out = NULL;
    DWORD type = 0, size = 0;
    DWORD error = RegQueryValueExW(key, name, NULL, &type, NULL, &size);
    if (error == 2) return 1610;
    if (error) return error;
    if (type != REG_SZ || size < 2 || (size & 1) || size > 2U * 1024U * 1024U) return 1610;
    PWSTR text = kmalloc(size);
    if (!text) return 8;
    DWORD capacity = size;
    error = RegQueryValueExW(key, name, NULL, &type, (BYTE *)text, &capacity);
    if (error == 2 || error == 234) error = 1610;
    if (!error && (type != REG_SZ || capacity != size || text[size / 2 - 1])) error = 1610;
    for (DWORD i = 0; !error && i + 1 < size / 2; i++) if (!text[i]) error = 1610;
    if (error) kfree(text); else *out = text;
    return error;
}
static DWORD product_output(PCWSTR text, PWSTR buffer, DWORD *capacity)
{
    BOOL compat = g_compat32_mode != 0;
    if (!buffer && !capacity) return 0;
    if (!win32_user_range_readable(capacity, 4, compat) ||
        !win32_user_range_writable(capacity, 4, compat)) return 87;
    DWORD size = *capacity, length = product_length(text);
    /* Product queries leave a short buffer untouched. Windows also writes a
     * terminator for an empty value even when the supplied capacity is zero. */
    if (buffer && (!length || size > length)) {
        if (!win32_user_range_writable(buffer, ((uint64_t)length + 1) * 2, compat)) return 87;
        memcpy(buffer, text, ((uint64_t)length + 1) * 2);
    }
    *capacity = length;
    return buffer && length && size <= length ? 234 : 0;
}
DWORD msi_product_open_registered(PCWSTR product, HKEY *key)
{ return product_open(product, key); }
DWORD msi_product_read_string(HKEY key, PCWSTR name, PWSTR *text)
{ return product_string(key, name, text); }
static void product_decimal(DWORD value, WCHAR text[11])
{
    WCHAR reverse[10]; DWORD n = 0, out = 0;
    do { reverse[n++] = (WCHAR)('0' + value % 10); value /= 10; } while (value);
    while (n) text[out++] = reverse[--n];
    text[out] = 0;
}
static DWORD product_version(PCWSTR text, DWORD numbers[3])
{
    for (unsigned i = 0; i < 3; i++) {
        DWORD value = 0, limit = i == 2 ? 65535 : 255;
        if (*text < '0' || *text > '9') return 1610;
        do {
            value = value * 10 + (DWORD)(*text++ - '0');
            if (value > limit) return 1610;
        } while (*text >= '0' && *text <= '9');
        numbers[i] = value;
        if (i < 2) { if (*text++ != '.') return 1610; }
    }
    /* Windows' packed version ignores a fourth dotted component. */
    if (*text && *text != '.') return 1610;
    return 0;
}
DWORD WINAPI MsiGetProductInfoW(PCWSTR input_product, PCWSTR input_attribute, PWSTR buffer, DWORD *capacity)
{
    WCHAR product[39], attribute[80];
    DWORD error = product_argument(input_product, product, 39);
    if (!error && (product_length(product) != 38 || product[0] != '{' || product[37] != '}')) error = 87;
    if (!error) error = product_argument(input_attribute, attribute, 80);
    if (error) return error;
    static const struct { PCWSTR public_name, stored_name; } names[] = {
        {PW("ProductName"), PW("ProductName")}, {PW("InstalledProductName"), PW("ProductName")},
        {PW("Publisher"), PW("Manufacturer")}, {PW("Language"), PW("ProductLanguage")},
        {PW("InstalledLanguage"), PW("ProductLanguage")}, {PW("VersionString"), PW("ProductVersion")},
        {PW("PackageCode"), PW("PackageCode")}, {PW("LocalPackage"), PW("LocalPackage")},
        {PW("InstallSource"), PW("InstallSource")}
    };
    PCWSTR stored = NULL, constant = NULL;
    for (unsigned i = 0; i < sizeof(names)/sizeof(*names); i++)
        if (product_equal(attribute, names[i].public_name)) stored = names[i].stored_name;
    int version = product_equal(attribute, PW("Version")) ? 1 :
        product_equal(attribute, PW("VersionMajor")) ? 2 : product_equal(attribute, PW("VersionMinor")) ? 3 : 0;
    if (version) stored = PW("ProductVersion");
    if (product_equal(attribute, PW("AssignmentType"))) constant = PW("1");
    if (product_equal(attribute, PW("Transforms"))) constant = PW("");
    if (product_equal(attribute, PW("InstanceType"))) constant = PW("0");
    /* Metadata for these recognized properties is not captured yet. Do not
     * fabricate an empty value for authored information we have discarded. */
    static const PCWSTR unsupported[] = {PW("HelpLink"), PW("HelpTelephone"), PW("InstallDate"),
        PW("InstallLocation"), PW("URLInfoAbout"), PW("URLUpdateInfo"), PW("ProductID"),
        PW("RegCompany"), PW("RegOwner"), PW("ProductIcon"), PW("PackageName"), PW("AuthorizedLUAApp")};
    BOOL recognized = stored || constant;
    for (unsigned i = 0; i < sizeof(unsupported)/sizeof(*unsupported); i++)
        if (product_equal(attribute, unsupported[i])) recognized = TRUE;
    if (!recognized) return 1608;
    HKEY key;
    error = product_open(product, &key);
    if (error) return error;
    PWSTR text = NULL;
    if (stored) error = product_string(key, stored, &text);
    else if (!constant) error = 120;
    RegCloseKey(key);
    WCHAR number[11]; DWORD parts[3];
    if (!error && version) {
        error = product_version(text, parts);
        if (!error) {
            DWORD value = version == 1 ? (parts[0] << 24) | (parts[1] << 16) | parts[2] : parts[version - 2];
            product_decimal(value, number); constant = number;
        }
    }
    if (!error) error = product_output(constant ? constant : text, buffer, capacity);
    kfree(text); return error;
}
