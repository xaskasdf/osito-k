/* MSI record handles are DWORDs in both ABIs. All stored strings belong to
 * the kernel; no guest pointer survives an API call. Installation sessions
 * are deliberately not exported until their cancellation journal is owned.
 */
#include "msi_shim.h"
#include "msi_products.h"
#include "msi_discovery.h"
#include "msi_components.h"
#include "msi_engine.h"
#include "msi_install.h"
#include "compat32.h"
#include "../kernel/smp.h"

extern void *kmalloc(uint64_t bytes);
extern void kfree(void *pointer);
extern DWORD win32_current_process_id(void);

#define MSI_RECORD_HANDLES 1024U
#define MSI_RECORD_TEXT_MAX (1024U * 1024U)
#define MSI_NULL_INTEGER ((LONG)0x80000000U)
enum { MSI_FIELD_NULL, MSI_FIELD_INT, MSI_FIELD_STRING };
typedef struct { DWORD refs, length; WCHAR text[]; } MSI_TEXT;
typedef struct { DWORD kind; LONG integer; MSI_TEXT *string; } MSI_FIELD;
typedef struct { DWORD refs, count; MSI_FIELD fields[]; } MSI_RECORD;
typedef struct { DWORD id, owner; MSI_RECORD *record; } MSI_SLOT;
static MSI_SLOT msi_slots[MSI_RECORD_HANDLES];
static DWORD msi_next_handle = 1;
static spinlock_t msi_lock = SPINLOCK_INIT;

static uint64_t msi_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&msi_lock);
    return flags;
}
static void msi_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&msi_lock);
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}
static void msi_text_release(MSI_TEXT *text)
{
    if (text && __atomic_sub_fetch(&text->refs, 1, __ATOMIC_ACQ_REL) == 0)
        kfree(text);
}
static void msi_record_release(MSI_RECORD *record)
{
    if (!record || __atomic_sub_fetch(&record->refs, 1, __ATOMIC_ACQ_REL)) return;
    for (DWORD i = 0; i <= record->count; i++)
        msi_text_release(record->fields[i].string);
    kfree(record);
}
static MSI_RECORD *msi_record_acquire(DWORD id)
{
    DWORD owner = win32_current_process_id();
    MSI_RECORD *record = NULL;
    uint64_t flags = msi_lock_irqsave();
    for (unsigned i = 0; id && i < MSI_RECORD_HANDLES; i++) {
        if (msi_slots[i].id != id || msi_slots[i].owner != owner) continue;
        record = msi_slots[i].record;
        __atomic_add_fetch(&record->refs, 1, __ATOMIC_RELAXED);
        break;
    }
    msi_unlock_irqrestore(flags);
    return record;
}
static MSI_FIELD msi_field_acquire(MSI_RECORD *record, DWORD index)
{
    MSI_FIELD field = {0};
    uint64_t flags = msi_lock_irqsave();
    if (index <= record->count) {
        field = record->fields[index];
        if (field.string) __atomic_add_fetch(&field.string->refs, 1, __ATOMIC_RELAXED);
    }
    msi_unlock_irqrestore(flags);
    return field;
}
static void msi_field_replace(MSI_RECORD *record, DWORD index, MSI_FIELD value)
{
    uint64_t flags = msi_lock_irqsave();
    MSI_FIELD old = record->fields[index];
    record->fields[index] = value;
    msi_unlock_irqrestore(flags);
    msi_text_release(old.string);
}

static DWORD WINAPI MsiCreateRecord(DWORD count)
{
    DWORD owner = win32_current_process_id();
    if (count > 65535 || !owner) return 0;
    uint64_t bytes = sizeof(MSI_RECORD) + ((uint64_t)count + 1) * sizeof(MSI_FIELD);
    MSI_RECORD *record = kmalloc(bytes);
    if (!record) return 0;
    memset(record, 0, bytes);
    record->refs = 1; record->count = count;
    DWORD handle = 0;
    uint64_t flags = msi_lock_irqsave();
    for (unsigned i = 0; msi_next_handle && i < MSI_RECORD_HANDLES; i++) {
        if (msi_slots[i].id) continue;
        handle = msi_next_handle++;
        msi_slots[i] = (MSI_SLOT){handle, owner, record};
        break;
    }
    msi_unlock_irqrestore(flags);
    if (!handle) msi_record_release(record);
    return handle;
}
static DWORD WINAPI MsiCloseHandle(DWORD handle)
{
    DWORD owner = win32_current_process_id();
    MSI_RECORD *record = NULL;
    uint64_t flags = msi_lock_irqsave();
    for (unsigned i = 0; handle && i < MSI_RECORD_HANDLES; i++) {
        if (msi_slots[i].id != handle || msi_slots[i].owner != owner) continue;
        record = msi_slots[i].record; msi_slots[i] = (MSI_SLOT){0}; break;
    }
    msi_unlock_irqrestore(flags);
    msi_record_release(record);
    return record ? 0 : 6;
}
void msi_release_process(DWORD owner)
{
    msi_discovery_release_process(owner);
    /* The process teardown has already stopped its workers. A peer process
     * may still use MSI, so keep the table protected while detaching slots. */
    for (unsigned i = 0; i < MSI_RECORD_HANDLES; i++) {
        MSI_RECORD *record = NULL;
        uint64_t flags = msi_lock_irqsave();
        if (msi_slots[i].id && msi_slots[i].owner == owner) {
            record = msi_slots[i].record; msi_slots[i] = (MSI_SLOT){0};
        }
        msi_unlock_irqrestore(flags);
        msi_record_release(record);
    }
}
static DWORD WINAPI MsiRecordGetFieldCount(DWORD handle)
{
    MSI_RECORD *record = msi_record_acquire(handle);
    DWORD count = record ? record->count : 0xffffffffU;
    msi_record_release(record); return count;
}

static DWORD msi_guest_string(PCWSTR input, MSI_TEXT **out)
{
    *out = NULL;
    if (!input) return 0;
    BOOL compat = g_compat32_mode != 0;
    DWORD length = 0;
    while (length <= MSI_RECORD_TEXT_MAX) {
        /* Validate one page at a time, including an unaligned WCHAR crossing
         * its end. Never scan or copy beyond the checked guest range. */
        uint64_t address = (uint64_t)(ULONG_PTR)input + (uint64_t)length * 2;
        if (address < (uint64_t)(ULONG_PTR)input) return 87;
        DWORD span = (4096U - (DWORD)(address & 4095U)) / 2;
        if (!span) span = 1;
        if (span > MSI_RECORD_TEXT_MAX + 1 - length)
            span = MSI_RECORD_TEXT_MAX + 1 - length;
        if (!win32_user_range_readable((void *)(ULONG_PTR)address, span * 2U, compat)) return 87;
        for (DWORD j = 0; j < span; j++, length++) if (!input[length]) goto found;
    }
    return 8;
found:
    if (!length) return 0;
    if (!win32_user_range_readable(input, ((uint64_t)length + 1) * 2, compat)) return 87;
    MSI_TEXT *text = kmalloc(sizeof(*text) + ((uint64_t)length + 1) * 2);
    if (!text) return 8;
    text->refs = 1; text->length = length;
    memcpy(text->text, input, (uint64_t)length * 2);
    text->text[length] = 0;
    for (DWORD i = 0; i < length; i++) if (!text->text[i]) { text->length = i; break; }
    if (!text->length) { kfree(text); return 0; }
    *out = text; return 0;
}
static DWORD WINAPI MsiRecordSetStringW(DWORD handle, DWORD index, PCWSTR value)
{
    MSI_RECORD *record = msi_record_acquire(handle);
    if (!record) return 6;
    DWORD error = 87;
    if (index <= record->count) {
        MSI_TEXT *text;
        error = msi_guest_string(value, &text);
        if (!error) msi_field_replace(record, index,
            (MSI_FIELD){.kind = text ? MSI_FIELD_STRING : MSI_FIELD_NULL, .string = text});
    }
    msi_record_release(record); return error;
}
static DWORD WINAPI MsiRecordSetInteger(DWORD handle, DWORD index, LONG number)
{
    MSI_RECORD *record = msi_record_acquire(handle);
    if (!record) return 6;
    DWORD error = index > record->count ? 87 : 0;
    if (!error) msi_field_replace(record, index, (MSI_FIELD){
        .kind = number == MSI_NULL_INTEGER ? MSI_FIELD_NULL : MSI_FIELD_INT, .integer = number});
    msi_record_release(record); return error;
}
static LONG WINAPI MsiRecordGetInteger(DWORD handle, DWORD index)
{
    MSI_RECORD *record = msi_record_acquire(handle);
    if (!record) return MSI_NULL_INTEGER;
    MSI_FIELD field = msi_field_acquire(record, index);
    LONG number = MSI_NULL_INTEGER;
    if (field.kind == MSI_FIELD_INT) number = field.integer;
    if (field.string) {
        DWORD i = 0, value = 0; BOOL negative = field.string->text[0] == '-';
        if (negative) i++;
        BOOL valid = i < field.string->length;
        for (; i < field.string->length; i++) {
            WCHAR c = field.string->text[i];
            if (c < '0' || c > '9') { valid = FALSE; break; }
            value = value * 10U + (DWORD)(c - '0');
        }
        if (valid) number = (LONG)(negative ? 0U - value : value);
    }
    msi_text_release(field.string); msi_record_release(record); return number;
}
static DWORD msi_integer_text(LONG number, WCHAR buffer[12])
{
    DWORD value = number < 0 ? 0U - (DWORD)number : (DWORD)number, count = 0, out = 0;
    WCHAR reverse[10];
    do { reverse[count++] = (WCHAR)('0' + value % 10); value /= 10; } while (value);
    if (number < 0) buffer[out++] = '-';
    while (count) buffer[out++] = reverse[--count];
    buffer[out] = 0; return out;
}
static PCWSTR msi_field_text(const MSI_FIELD *field, WCHAR number[12], DWORD *length)
{
    if (field->string) { *length = field->string->length; return field->string->text; }
    *length = field->kind == MSI_FIELD_INT ? msi_integer_text(field->integer, number) : 0;
    return number;
}
static DWORD msi_output(PCWSTR text, DWORD length, PWSTR buffer, DWORD *capacity)
{
    BOOL compat = g_compat32_mode != 0;
    if (!win32_user_range_readable(capacity, sizeof(*capacity), compat) ||
        !win32_user_range_writable(capacity, sizeof(*capacity), compat)) return 87;
    DWORD size = *capacity;
    if (buffer && size) {
        DWORD copied = length < size ? length : size - 1;
        if (!win32_user_range_writable(buffer, ((uint64_t)copied + 1) * 2, compat)) return 87;
        if (copied) memcpy(buffer, text, (uint64_t)copied * 2);
        buffer[copied] = 0;
    }
    *capacity = length;
    return buffer && size <= length ? 234 : 0;
}
static DWORD WINAPI MsiRecordGetStringW(DWORD handle, DWORD index, PWSTR out, DWORD *capacity)
{
    MSI_RECORD *record = msi_record_acquire(handle);
    if (!record) return 6;
    MSI_FIELD field = msi_field_acquire(record, index);
    WCHAR number[12]; DWORD length;
    PCWSTR text = msi_field_text(&field, number, &length);
    DWORD error = msi_output(text, length, out, capacity);
    msi_text_release(field.string); msi_record_release(record); return error;
}
static BOOL WINAPI MsiRecordIsNull(DWORD handle, DWORD index)
{
    MSI_RECORD *record = msi_record_acquire(handle);
    if (!record) return FALSE;
    MSI_FIELD field = msi_field_acquire(record, index);
    BOOL null = field.kind == MSI_FIELD_NULL;
    msi_text_release(field.string); msi_record_release(record); return null;
}
static DWORD WINAPI MsiRecordDataSize(DWORD handle, DWORD index)
{
    MSI_RECORD *record = msi_record_acquire(handle);
    if (!record) return 0;
    MSI_FIELD field = msi_field_acquire(record, index);
    DWORD size = field.string ? field.string->length : field.kind == MSI_FIELD_INT ? 4 : 0;
    msi_text_release(field.string); msi_record_release(record); return size;
}

/* Formatting takes an immutable snapshot, then releases the table lock before
 * any parsing/allocation/output. Numeric replacements are opaque data: a field
 * containing "[2]" is not recursively interpreted as another substitution. */
typedef struct { WCHAR *text; DWORD length, capacity, error; } MSI_BUFFER;
static void msi_append(MSI_BUFFER *out, PCWSTR text, DWORD length)
{
    if (out->error || !length) return;
    if (length > MSI_RECORD_TEXT_MAX - out->length) { out->error = 8; return; }
    DWORD need = out->length + length;
    if (need > out->capacity) {
        DWORD capacity = out->capacity ? out->capacity * 2 : 64;
        if (capacity < need) capacity = need;
        if (capacity > MSI_RECORD_TEXT_MAX) capacity = MSI_RECORD_TEXT_MAX;
        WCHAR *next = kmalloc(((uint64_t)capacity + 1) * 2);
        if (!next) { out->error = 8; return; }
        if (out->length) memcpy(next, out->text, (uint64_t)out->length * 2);
        kfree(out->text); out->text = next; out->capacity = capacity;
    }
    memcpy(out->text + out->length, text, (uint64_t)length * 2);
    out->length += length; out->text[out->length] = 0;
}
static void msi_append_field(MSI_BUFFER *out, const MSI_FIELD *field)
{
    WCHAR number[12]; DWORD length;
    PCWSTR text = msi_field_text(field, number, &length);
    msi_append(out, text, length);
}
enum { MSI_FORMAT_NUMERIC = 1, MSI_FORMAT_MISSING = 2, MSI_FORMAT_LITERAL = 4 };
static DWORD msi_record_format_range(const MSI_RECORD *record, PCWSTR text, DWORD length,
                                      unsigned depth, MSI_BUFFER *out)
{
    DWORD properties = 0;
    if (depth > 32) { out->error = 120; return 0; }
    for (DWORD i = 0; i < length && !out->error;) {
        if (text[i] == '[') {
            DWORD end = i + 1, nesting = 1;
            for (; end < length; end++) {
                if (text[end] == '[') nesting++;
                if (text[end] == ']' && !--nesting) break;
            }
            if (end == length) { msi_append(out, text + i, length - i); break; }
            MSI_BUFFER inner = {0};
            (void)msi_record_format_range(record, text + i + 1, end - i - 1, depth + 1, &inner);
            if (inner.error) out->error = inner.error;
            DWORD index = 0; BOOL numeric = inner.length != 0;
            for (DWORD j = 0; j < inner.length; j++) {
                if (inner.text[j] < '0' || inner.text[j] > '9') { numeric = FALSE; break; }
                index = index * 10U + inner.text[j] - '0';
            }
            if (numeric && index) {
                properties |= MSI_FORMAT_NUMERIC;
                if (index > record->count || record->fields[index].kind == MSI_FIELD_NULL)
                    properties |= MSI_FORMAT_MISSING;
                else msi_append_field(out, &record->fields[index]);
            } else {
                properties |= MSI_FORMAT_LITERAL;
                msi_append(out, text + i, 1); msi_append(out, inner.text, inner.length);
                msi_append(out, text + end, 1);
            }
            kfree(inner.text); i = end + 1;
        } else if (text[i] == '{') {
            DWORD end = i + 1;
            for (; end < length && text[end] != '}'; end++) {
                /* Nested braces have Windows-specific malformed-format rules.
                 * Reject this unsupported grammar without returning wrong text. */
                if (text[end] == '{') { out->error = 120; return properties; }
            }
            if (end == length) { msi_append(out, text + i, 1); i++; continue; }
            MSI_BUFFER inner = {0};
            DWORD flags = msi_record_format_range(record, text + i + 1, end - i - 1, depth + 1, &inner);
            if (inner.error) out->error = inner.error;
            if ((flags & MSI_FORMAT_LITERAL) || !(flags & MSI_FORMAT_NUMERIC)) {
                msi_append(out, text + i, 1); msi_append(out, inner.text, inner.length);
                msi_append(out, text + end, 1);
            } else if (!(flags & MSI_FORMAT_MISSING)) msi_append(out, inner.text, inner.length);
            properties |= flags; kfree(inner.text); i = end + 1;
        } else { msi_append(out, text + i, 1); i++; }
    }
    return properties;
}
static DWORD WINAPI MsiFormatRecordW(DWORD install, DWORD handle, PWSTR out, DWORD *capacity)
{
    if (install) return 6; /* No session handles have been published yet. */
    MSI_RECORD *record = msi_record_acquire(handle);
    if (!record) return 6;
    uint64_t bytes = sizeof(*record) + ((uint64_t)record->count + 1) * sizeof(MSI_FIELD);
    MSI_RECORD *snapshot = kmalloc(bytes);
    if (!snapshot) { msi_record_release(record); return 8; }
    snapshot->refs = 1; snapshot->count = record->count;
    uint64_t flags = msi_lock_irqsave();
    for (DWORD i = 0; i <= record->count; i++) {
        snapshot->fields[i] = record->fields[i];
        if (snapshot->fields[i].string)
            __atomic_add_fetch(&snapshot->fields[i].string->refs, 1, __ATOMIC_RELAXED);
    }
    msi_unlock_irqrestore(flags); msi_record_release(record);
    MSI_BUFFER result = {0};
    if (snapshot->fields[0].kind == MSI_FIELD_NULL) {
        for (DWORD i = 1; i <= snapshot->count && !result.error; i++) {
            WCHAR number[12]; DWORD length = msi_integer_text((LONG)i, number);
            msi_append(&result, number, length); msi_append(&result, (PCWSTR)u": ", 2);
            msi_append_field(&result, &snapshot->fields[i]); msi_append(&result, (PCWSTR)u" ", 1);
        }
    } else {
        WCHAR number[12]; DWORD length;
        PCWSTR text = msi_field_text(&snapshot->fields[0], number, &length);
        msi_record_format_range(snapshot, text, length, 0, &result);
    }
    DWORD error = result.error ? result.error : msi_output(result.text, result.length, out, capacity);
    kfree(result.text); msi_record_release(snapshot); return error;
}

static DWORD WINAPI MsiInstallProductW(PCWSTR path, PCWSTR properties)
{
    /* Protect copied arguments as well as the engine's transaction journal. */
    if (!msi_install_scope_enter()) return 1618;
    MSI_TEXT *file = NULL, *arguments = NULL;
    DWORD error = msi_guest_string(path, &file);
    if (!error && !file) error = 87;
    if (!error) error = msi_guest_string(properties, &arguments);
    if (!error) error = msi_engine_install_controlled(file->text,
        arguments ? arguments->text : NULL, msi_install_cancelled, NULL);
    msi_text_release(arguments); msi_text_release(file);
    msi_install_scope_leave();
    extern void win32_main_termination_checkpoint(void);
    win32_main_termination_checkpoint();
    return error;
}

static const WIN32_EXPORT msi_exports[] = {
    WX_STD("MsiGetComponentPathW", MsiGetComponentPathW, 4),
    WX_STD("MsiLocateComponentW", MsiLocateComponentW, 3),
    WX_STD("MsiEnumProductsW", MsiEnumProductsW, 2),
    WX_STD("MsiEnumRelatedProductsW", MsiEnumRelatedProductsW, 4),
    WX_STD("MsiQueryFeatureStateW", MsiQueryFeatureStateW, 2),
    WX_STD("MsiInstallProductW", MsiInstallProductW, 2),
    WX_STD("MsiGetProductInfoW", MsiGetProductInfoW, 4),
    WX_STD("MsiCloseHandle", MsiCloseHandle, 1),
    WX_STD("MsiCreateRecord", MsiCreateRecord, 1),
    WX_STD("MsiRecordGetFieldCount", MsiRecordGetFieldCount, 1),
    WX_STD("MsiRecordGetInteger", MsiRecordGetInteger, 2),
    WX_STD("MsiRecordGetStringW", MsiRecordGetStringW, 4),
    WX_STD("MsiRecordSetStringW", MsiRecordSetStringW, 3),
    WX_STD("MsiRecordSetInteger", MsiRecordSetInteger, 3),
    WX_STD("MsiRecordIsNull", MsiRecordIsNull, 2),
    WX_STD("MsiRecordDataSize", MsiRecordDataSize, 2),
    WX_STD("MsiFormatRecordW", MsiFormatRecordW, 4),
    {NULL, NULL, 0, CC_STDCALL}
};
const WIN32_EXPORT *msi_abi_table(int *count)
{
    *count = sizeof(msi_exports) / sizeof(msi_exports[0]); return msi_exports;
}
PVOID msi_resolve(const char *name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) {
        switch (ordinal) {
        case 8: return (PVOID)MsiCloseHandle;
        case 17: return (PVOID)MsiCreateRecord;
        case 45: return (PVOID)MsiEnumProductsW;
        case 70: return (PVOID)MsiGetProductInfoW;
        case 88: return (PVOID)MsiInstallProductW;
        case 90: return (PVOID)MsiLocateComponentW;
        case 111: return (PVOID)MsiQueryFeatureStateW;
        case 115: return (PVOID)MsiRecordGetFieldCount;
        case 116: return (PVOID)MsiRecordGetInteger;
        case 118: return (PVOID)MsiRecordGetStringW;
        case 125: return (PVOID)MsiRecordSetStringW;
        case 171: return (PVOID)MsiFormatRecordW;
        case 173: return (PVOID)MsiGetComponentPathW;
        case 205: return (PVOID)MsiEnumRelatedProductsW;
        default: return NULL;
        }
    }
    if (name) for (unsigned i = 0; msi_exports[i].name; i++)
        if (!strcmp(name, msi_exports[i].name)) return msi_exports[i].func;
    return NULL;
}
