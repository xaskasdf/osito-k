/* Discovery of products installed by the MSI engine. Enumeration state belongs
 * to the calling process/thread; no caller buffer is kept between calls. */
#include "msi_discovery.h"
#include "msi_products.h"
#include "msi_registration.h"
#include "advapi32_shim.h"
#include "kernel32_shim.h"
#include "compat32.h"
#include "../kernel/smp.h"

extern void *kmalloc(uint64_t bytes);
extern void kfree(void *pointer);
extern DWORD win32_current_process_id(void);
#define DW(text) ((PCWSTR)u##text)

typedef struct MSI_ENUM_CURSOR {
    struct MSI_ENUM_CURSOR *next;
    DWORD owner, thread, related, index, registry_index;
    WCHAR upgrade[39];
} MSI_ENUM_CURSOR;
static MSI_ENUM_CURSOR *discovery_cursors;
static spinlock_t discovery_lock = SPINLOCK_INIT;

static uint64_t discovery_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&discovery_lock); return flags;
}
static void discovery_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&discovery_lock);
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}
static int discovery_argument(PCWSTR input, WCHAR output[39])
{
    if (!input) return 0;
    for (unsigned i = 0; i < 39; i++) {
        if (!win32_user_range_readable(input+i, 2, g_compat32_mode != 0)) return 0;
        output[i] = input[i]; if (!output[i]) return 1;
    }
    return 0;
}
static int discovery_guid_shape(PCWSTR text)
{
    unsigned length = 0; while (text[length]) length++;
    /* Like the native MSI query APIs, validate framing here. Malformed hex
     * inside a framed identifier is simply not a registered product. */
    return length == 38 && text[0] == '{' && text[37] == '}';
}
static int discovery_guid_equal(PCWSTR a, PCWSTR b)
{
    for (;;) {
        WCHAR x = *a++, y = *b++;
        if (x >= 'a' && x <= 'f') x -= 'a'-'A';
        if (y >= 'a' && y <= 'f') y -= 'a'-'A';
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static MSI_ENUM_CURSOR *discovery_detach(DWORD owner, DWORD thread, DWORD related)
{
    uint64_t flags = discovery_lock_irqsave();
    MSI_ENUM_CURSOR **link = &discovery_cursors, *cursor = NULL;
    while (*link) {
        if ((*link)->owner == owner && (*link)->thread == thread && (*link)->related == related) {
            cursor = *link; *link = cursor->next; cursor->next = NULL; break;
        }
        link = &(*link)->next;
    }
    discovery_unlock_irqrestore(flags); return cursor;
}
static void discovery_attach(MSI_ENUM_CURSOR *cursor)
{
    uint64_t flags = discovery_lock_irqsave();
    cursor->next = discovery_cursors; discovery_cursors = cursor;
    discovery_unlock_irqrestore(flags);
}
void msi_discovery_release_process(DWORD owner)
{
    /* The process teardown has stopped its workers before calling us. */
    MSI_ENUM_CURSOR *retired = NULL;
    uint64_t flags = discovery_lock_irqsave();
    MSI_ENUM_CURSOR **link = &discovery_cursors;
    while (*link) {
        MSI_ENUM_CURSOR *cursor = *link;
        if (cursor->owner != owner) { link = &cursor->next; continue; }
        *link = cursor->next; cursor->next = retired; retired = cursor;
    }
    discovery_unlock_irqrestore(flags);
    while (retired) { MSI_ENUM_CURSOR *next = retired->next; kfree(retired); retired = next; }
}
static DWORD discovery_next(MSI_ENUM_CURSOR *cursor, WCHAR product[39])
{
    HKEY store = NULL;
    DWORD error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, MSI_PRODUCT_STORE, 0,
                                KEY_READ | KEY_WOW64_64KEY, &store);
    if (error) return error == 2 || error == 3 ? 259 : error;
    for (;;) {
        DWORD length = 39;
        error = RegEnumKeyExW(store, cursor->registry_index++, product, &length,
                              NULL, NULL, NULL, NULL);
        if (error) { if (error == 234) error = 1610; break; }
        if (!msi_registration_guid(product)) { error = 1610; break; }
        /* The registry normalizes key paths; MSI returns canonical GUIDs. */
        for (unsigned i = 0; i < 38; i++)
            if (product[i] >= 'a' && product[i] <= 'f') product[i] -= 'a'-'A';
        HKEY key = NULL;
        error = msi_product_open_registered(product, &key);
        if (error == 1605) continue; /* Installation has not committed State. */
        if (error) break;
        PWSTR upgrade = NULL;
        if (cursor->related) error = msi_product_read_string(key, DW("UpgradeCode"), &upgrade);
        RegCloseKey(key);
        int match = !error && (!cursor->related || discovery_guid_equal(upgrade, cursor->upgrade));
        kfree(upgrade);
        if (error || match) break;
    }
    RegCloseKey(store); return error;
}
static DWORD discovery_enumerate(DWORD index, PWSTR product, PCWSTR upgrade)
{
    if (!win32_user_range_writable(product, 39U*2, g_compat32_mode != 0)) return 87;
    DWORD owner = win32_current_process_id(), thread = GetCurrentThreadId();
    DWORD related = upgrade != NULL;
    MSI_ENUM_CURSOR *cursor = discovery_detach(owner, thread, related);
    if (!index) {
        if (!cursor) cursor = kmalloc(sizeof(*cursor));
        if (!cursor) return 8;
        memset(cursor, 0, sizeof(*cursor));
        cursor->owner = owner; cursor->thread = thread; cursor->related = related;
        if (upgrade) memcpy(cursor->upgrade, upgrade, 39U*2);
    } else if (!cursor || cursor->index != index ||
               (related && !discovery_guid_equal(upgrade, cursor->upgrade))) {
        if (cursor) discovery_attach(cursor);
        return 87;
    }
    WCHAR result[39]; DWORD error = discovery_next(cursor, result);
    if (!error) {
        cursor->index = index + 1;
        discovery_attach(cursor);
        memcpy(product, result, 39U*2);
    } else kfree(cursor);
    return error;
}
DWORD WINAPI MsiEnumProductsW(DWORD index, PWSTR product)
{ return discovery_enumerate(index, product, NULL); }
DWORD WINAPI MsiEnumRelatedProductsW(PCWSTR input_upgrade, DWORD reserved, DWORD index, PWSTR product)
{
    WCHAR upgrade[39];
    if (reserved || !discovery_argument(input_upgrade, upgrade) || !discovery_guid_shape(upgrade)) return 87;
    return discovery_enumerate(index, product, upgrade);
}
LONG WINAPI MsiQueryFeatureStateW(PCWSTR input_product, PCWSTR input_feature)
{
    WCHAR product[39], feature[39];
    if (!discovery_argument(input_product, product) || !discovery_guid_shape(product) ||
        !discovery_argument(input_feature, feature)) return -2;
    HKEY key = NULL, features = NULL;
    DWORD error = msi_product_open_registered(product, &key);
    if (error) return error == 1610 ? -6 : -1;
    error = RegOpenKeyExW(key, DW("Features"), 0, KEY_READ | KEY_WOW64_64KEY, &features);
    RegCloseKey(key);
    if (error) return -1;
    DWORD state = 0, type = 0, bytes = 4;
    error = RegQueryValueExW(features, feature, NULL, &type, (BYTE *)&state, &bytes);
    RegCloseKey(features);
    if (error == 2) return -1;
    if (error || type != REG_DWORD || bytes != 4 || state < 1 || state > 4) return -6;
    /* This query reports registration state, not physical file accessibility. */
    return (LONG)state;
}
