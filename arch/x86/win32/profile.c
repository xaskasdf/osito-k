/* Win32 private-profile (INI) manager. */

#include "kernel32_shim.h"
#include "../kernel/smp.h"

extern void *kmalloc(uint64_t size);
extern void *krealloc(void *ptr, uint64_t size);
extern void kfree(void *ptr);
extern void *memmove(void *destination, const void *source, uint64_t size);

extern void *osfs2_find_exact_ci(const char *name);
extern int osfs2_read(void *file, uint64_t offset, void *buffer,
                      uint64_t length);
extern uint64_t osfs2_file_size(void *file);
extern uint64_t osfs2_file_revision(void *file);

#define PROFILE_ERROR_NOT_ENOUGH_MEMORY 8U
#define PROFILE_ERROR_WRITE_FAULT      29U
#define PROFILE_ERROR_READ_FAULT       30U
#define PROFILE_ERROR_INVALID_PARAMETER 87U
#define PROFILE_ERROR_FILENAME_EXCED_RANGE 206U
#define PROFILE_ERROR_FILE_TOO_LARGE   223U
#define PROFILE_MAX_FILE_SIZE          0x7FFFFFFFULL

typedef enum {
    INI_RECORD_RAW = 0,
    INI_RECORD_SECTION,
    INI_RECORD_KEY
} INI_RECORD_TYPE;

typedef struct {
    INI_RECORD_TYPE type;
    char *name;
    char *value;
} INI_RECORD;

typedef struct {
    char path[260];
    INI_RECORD *records;
    uint32_t count;
    uint32_t capacity;
    void *file_identity;
    uint64_t file_revision;
    uint64_t file_size;
    BOOL loaded;
} INI_DOCUMENT;

static INI_DOCUMENT *ini_documents;
static uint32_t ini_document_count;
static uint32_t ini_document_capacity;
static spinlock_t ini_lock = SPINLOCK_INIT;

static char ini_fold(char value)
{
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static BOOL ini_equal_ci(const char *left, const char *right)
{
    if (!left || !right) return FALSE;
    while (*left && *right) {
        if (ini_fold(*left++) != ini_fold(*right++)) return FALSE;
    }
    return *left == *right;
}

static char *ini_duplicate_range(const char *source, uint64_t length)
{
    if (length == UINT64_MAX) return NULL;
    char *copy = (char *)kmalloc(length + 1);
    if (!copy) return NULL;
    if (length) memcpy(copy, source, length);
    copy[length] = 0;
    return copy;
}

static char *ini_duplicate(const char *source)
{
    return source ? ini_duplicate_range(source, strlen(source)) : NULL;
}

static void ini_record_release(INI_RECORD *record)
{
    if (!record) return;
    if (record->name) kfree(record->name);
    if (record->value) kfree(record->value);
    memset(record, 0, sizeof(*record));
}

static void ini_document_clear(INI_DOCUMENT *document)
{
    if (!document) return;
    for (uint32_t i = 0; i < document->count; i++)
        ini_record_release(&document->records[i]);
    if (document->records) kfree(document->records);
    document->records = NULL;
    document->count = 0;
    document->capacity = 0;
}

static void ini_document_invalidate(INI_DOCUMENT *document)
{
    if (!document) return;
    ini_document_clear(document);
    document->file_identity = NULL;
    document->file_revision = 0;
    document->file_size = 0;
    document->loaded = FALSE;
}

static BOOL ini_document_reserve(INI_DOCUMENT *document, uint32_t wanted)
{
    if (wanted <= document->capacity) return TRUE;
    uint32_t capacity = document->capacity ? document->capacity : 32;
    while (capacity < wanted) {
        if (capacity > UINT32_MAX / 2) {
            capacity = wanted;
            break;
        }
        capacity *= 2;
    }
    if ((uint64_t)capacity > UINT64_MAX / sizeof(INI_RECORD)) return FALSE;
    INI_RECORD *records = (INI_RECORD *)krealloc(
        document->records, (uint64_t)capacity * sizeof(INI_RECORD));
    if (!records) return FALSE;
    memset(records + document->capacity, 0,
           (uint64_t)(capacity - document->capacity) * sizeof(INI_RECORD));
    document->records = records;
    document->capacity = capacity;
    return TRUE;
}

static BOOL ini_record_make(INI_RECORD *record, INI_RECORD_TYPE type,
                            const char *name, uint64_t name_length,
                            const char *value, uint64_t value_length)
{
    memset(record, 0, sizeof(*record));
    record->type = type;
    record->name = ini_duplicate_range(name ? name : "", name_length);
    if (!record->name) return FALSE;
    if (type == INI_RECORD_KEY) {
        record->value = ini_duplicate_range(value ? value : "", value_length);
        if (!record->value) {
            ini_record_release(record);
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL ini_document_insert(INI_DOCUMENT *document, uint32_t index,
                                INI_RECORD *record)
{
    if (index > document->count ||
        !ini_document_reserve(document, document->count + 1))
        return FALSE;
    if (index < document->count) {
        memmove(&document->records[index + 1], &document->records[index],
                (uint64_t)(document->count - index) * sizeof(INI_RECORD));
    }
    document->records[index] = *record;
    memset(record, 0, sizeof(*record));
    document->count++;
    return TRUE;
}

static BOOL ini_document_append(INI_DOCUMENT *document, INI_RECORD_TYPE type,
                                const char *name, uint64_t name_length,
                                const char *value, uint64_t value_length)
{
    INI_RECORD record;
    if (!ini_record_make(&record, type, name, name_length,
                         value, value_length))
        return FALSE;
    if (!ini_document_insert(document, document->count, &record)) {
        ini_record_release(&record);
        return FALSE;
    }
    return TRUE;
}

static void ini_document_remove(INI_DOCUMENT *document, uint32_t index,
                                uint32_t count)
{
    if (!document || index >= document->count || !count) return;
    if (count > document->count - index) count = document->count - index;
    for (uint32_t i = 0; i < count; i++)
        ini_record_release(&document->records[index + i]);
    uint32_t tail = document->count - index - count;
    if (tail) {
        memmove(&document->records[index],
                &document->records[index + count],
                (uint64_t)tail * sizeof(INI_RECORD));
    }
    document->count -= count;
    memset(&document->records[document->count], 0,
           (uint64_t)count * sizeof(INI_RECORD));
}

static BOOL ini_documents_reserve(uint32_t wanted)
{
    if (wanted <= ini_document_capacity) return TRUE;
    uint32_t capacity = ini_document_capacity ? ini_document_capacity : 8;
    while (capacity < wanted) {
        if (capacity > UINT32_MAX / 2) {
            capacity = wanted;
            break;
        }
        capacity *= 2;
    }
    INI_DOCUMENT *documents = (INI_DOCUMENT *)krealloc(
        ini_documents, (uint64_t)capacity * sizeof(INI_DOCUMENT));
    if (!documents) return FALSE;
    memset(documents + ini_document_capacity, 0,
           (uint64_t)(capacity - ini_document_capacity) *
               sizeof(INI_DOCUMENT));
    ini_documents = documents;
    ini_document_capacity = capacity;
    return TRUE;
}

static INI_DOCUMENT *ini_document_lookup_locked(PCSTR file_name,
                                                 BOOL create)
{
    char path[260];
    if (!file_name || !*file_name ||
        !win32_normalize_path(file_name, path)) {
        SetLastError(file_name ? PROFILE_ERROR_FILENAME_EXCED_RANGE
                               : PROFILE_ERROR_INVALID_PARAMETER);
        return NULL;
    }

    for (uint32_t i = 0; i < ini_document_count; i++) {
        if (ini_equal_ci(ini_documents[i].path, path))
            return &ini_documents[i];
    }
    if (!create || !ini_documents_reserve(ini_document_count + 1)) {
        if (create) SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    INI_DOCUMENT *document = &ini_documents[ini_document_count++];
    memset(document, 0, sizeof(*document));
    memcpy(document->path, path, strlen(path) + 1);
    return document;
}

static void ini_trim_range(const char **start, const char **end)
{
    while (*start < *end && (**start == ' ' || **start == '\t')) (*start)++;
    while (*end > *start && ((*end)[-1] == ' ' || (*end)[-1] == '\t'))
        (*end)--;
}

static BOOL ini_parse(const char *buffer, uint64_t size,
                      INI_DOCUMENT *parsed)
{
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t line_start = offset;
        while (offset < size && buffer[offset] != '\r' &&
               buffer[offset] != '\n')
            offset++;
        uint64_t line_end = offset;
        if (offset < size && buffer[offset] == '\r') offset++;
        if (offset < size && buffer[offset] == '\n') offset++;

        const char *raw_start = buffer + line_start;
        const char *raw_end = buffer + line_end;
        const char *start = raw_start;
        const char *end = raw_end;
        if (line_start == 0 && end - start >= 3 &&
            (BYTE)start[0] == 0xEF && (BYTE)start[1] == 0xBB &&
            (BYTE)start[2] == 0xBF)
            start += 3;
        ini_trim_range(&start, &end);

        if (start < end && *start == '[' && end[-1] == ']') {
            const char *name_start = start + 1;
            const char *name_end = end - 1;
            ini_trim_range(&name_start, &name_end);
            if (!ini_document_append(parsed, INI_RECORD_SECTION,
                                     name_start,
                                     (uint64_t)(name_end - name_start),
                                     NULL, 0))
                return FALSE;
            continue;
        }

        const char *equals = NULL;
        if (start < end && *start != ';' && *start != '#') {
            for (const char *cursor = start; cursor < end; cursor++) {
                if (*cursor == '=') {
                    equals = cursor;
                    break;
                }
            }
        }
        if (equals) {
            const char *key_start = start;
            const char *key_end = equals;
            const char *value_start = equals + 1;
            const char *value_end = end;
            ini_trim_range(&key_start, &key_end);
            ini_trim_range(&value_start, &value_end);
            if (value_end - value_start >= 2 && *value_start == '"' &&
                value_end[-1] == '"') {
                value_start++;
                value_end--;
            }
            if (key_start < key_end &&
                !ini_document_append(parsed, INI_RECORD_KEY,
                                     key_start,
                                     (uint64_t)(key_end - key_start),
                                     value_start,
                                     (uint64_t)(value_end - value_start)))
                return FALSE;
            if (key_start < key_end) continue;
        }

        if (!ini_document_append(parsed, INI_RECORD_RAW, raw_start,
                                 (uint64_t)(raw_end - raw_start), NULL, 0))
            return FALSE;
    }
    return TRUE;
}

static BOOL ini_document_refresh_locked(INI_DOCUMENT *document)
{
    for (int attempt = 0; attempt < 32; attempt++) {
        void *file = osfs2_find_exact_ci(document->path);
        if (!file) {
            if (!document->loaded || document->file_identity) {
                ini_document_clear(document);
                document->file_identity = NULL;
                document->file_revision = 0;
                document->file_size = 0;
            }
            document->loaded = TRUE;
            return TRUE;
        }

        uint64_t revision_before = osfs2_file_revision(file);
        if (revision_before & 1U) {
            __asm__ volatile ("pause");
            continue;
        }
        uint64_t size = osfs2_file_size(file);
        if (document->loaded && document->file_identity == file &&
            document->file_revision == revision_before &&
            document->file_size == size)
            return TRUE;
        if (size > PROFILE_MAX_FILE_SIZE) {
            SetLastError(PROFILE_ERROR_FILE_TOO_LARGE);
            return FALSE;
        }

        char *buffer = (char *)kmalloc(size + 1);
        if (!buffer) {
            SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        int read_result = size ? osfs2_read(file, 0, buffer, size) : 0;
        uint64_t revision_after = osfs2_file_revision(file);
        if (read_result != (int)size) {
            kfree(buffer);
            SetLastError(PROFILE_ERROR_READ_FAULT);
            return FALSE;
        }
        if (revision_before != revision_after || (revision_after & 1U)) {
            kfree(buffer);
            continue;
        }
        buffer[size] = 0;

        INI_DOCUMENT parsed;
        memset(&parsed, 0, sizeof(parsed));
        BOOL parsed_ok = ini_parse(buffer, size, &parsed);
        kfree(buffer);
        if (!parsed_ok) {
            ini_document_clear(&parsed);
            SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }

        ini_document_clear(document);
        document->records = parsed.records;
        document->count = parsed.count;
        document->capacity = parsed.capacity;
        document->file_identity = file;
        document->file_revision = revision_after;
        document->file_size = size;
        document->loaded = TRUE;
        return TRUE;
    }

    SetLastError(PROFILE_ERROR_READ_FAULT);
    return FALSE;
}

static BOOL ini_record_is_in_section(const INI_RECORD *record,
                                     BOOL current_match)
{
    return record->type == INI_RECORD_KEY && current_match;
}

static const char *ini_find_value(const INI_DOCUMENT *document,
                                  const char *section, const char *key)
{
    BOOL in_section = FALSE;
    for (uint32_t i = 0; i < document->count; i++) {
        const INI_RECORD *record = &document->records[i];
        if (record->type == INI_RECORD_SECTION) {
            in_section = ini_equal_ci(record->name, section);
        } else if (ini_record_is_in_section(record, in_section) &&
                   ini_equal_ci(record->name, key)) {
            return record->value;
        }
    }
    return NULL;
}

typedef enum {
    INI_LIST_SECTIONS,
    INI_LIST_KEYS,
    INI_LIST_SECTION
} INI_LIST_MODE;

static BOOL ini_section_seen_before(const INI_DOCUMENT *document,
                                    uint32_t index)
{
    const char *section = document->records[index].name;
    for (uint32_t i = 0; i < index; i++) {
        if (document->records[i].type == INI_RECORD_SECTION &&
            ini_equal_ci(document->records[i].name, section))
            return TRUE;
    }
    return FALSE;
}

static void ini_list_append(char *destination, uint64_t copy_limit,
                            uint64_t *logical, const char *text)
{
    uint64_t length = strlen(text);
    for (uint64_t i = 0; i < length; i++) {
        if (destination && *logical < copy_limit)
            destination[*logical] = text[i];
        (*logical)++;
    }
}

static void ini_list_terminate_item(char *destination, uint64_t copy_limit,
                                    uint64_t *logical)
{
    if (destination && *logical < copy_limit)
        destination[*logical] = 0;
    (*logical)++;
}

static uint64_t ini_list_emit(const INI_DOCUMENT *document,
                              INI_LIST_MODE mode, const char *section,
                              char *destination, uint64_t copy_limit)
{
    uint64_t logical = 0;
    BOOL in_section = FALSE;
    for (uint32_t i = 0; i < document->count; i++) {
        const INI_RECORD *record = &document->records[i];
        if (record->type == INI_RECORD_SECTION) {
            in_section = section && ini_equal_ci(record->name, section);
            if (mode == INI_LIST_SECTIONS &&
                !ini_section_seen_before(document, i)) {
                ini_list_append(destination, copy_limit, &logical,
                                record->name);
                ini_list_terminate_item(destination, copy_limit, &logical);
            }
            continue;
        }
        if (!ini_record_is_in_section(record, in_section)) continue;
        if (mode == INI_LIST_KEYS) {
            ini_list_append(destination, copy_limit, &logical, record->name);
        } else if (mode == INI_LIST_SECTION) {
            ini_list_append(destination, copy_limit, &logical, record->name);
            ini_list_append(destination, copy_limit, &logical, "=");
            ini_list_append(destination, copy_limit, &logical, record->value);
        } else {
            continue;
        }
        ini_list_terminate_item(destination, copy_limit, &logical);
    }
    return logical;
}

static DWORD ini_write_multisz(const INI_DOCUMENT *document,
                               INI_LIST_MODE mode, const char *section,
                               PSTR buffer, DWORD size)
{
    if (!buffer || !size) return 0;
    uint64_t logical = ini_list_emit(document, mode, section, NULL, 0);
    if (!logical) {
        buffer[0] = 0;
        if (size > 1) buffer[1] = 0;
        return 0;
    }
    if (logical + 1 <= size) {
        (void)ini_list_emit(document, mode, section, buffer, logical);
        buffer[logical] = 0;
        return (DWORD)logical;
    }
    if (size == 1) {
        buffer[0] = 0;
        return 0;
    }
    uint64_t copied = size - 2;
    (void)ini_list_emit(document, mode, section, buffer, copied);
    buffer[size - 2] = 0;
    buffer[size - 1] = 0;
    return size - 2;
}

static BOOL ini_set_key(INI_DOCUMENT *document, const char *section,
                        const char *key, const char *value)
{
    BOOL in_section = FALSE;
    uint32_t insert_at = UINT32_MAX;
    for (uint32_t i = 0; i < document->count; i++) {
        INI_RECORD *record = &document->records[i];
        if (record->type == INI_RECORD_SECTION) {
            if (in_section && insert_at == UINT32_MAX) insert_at = i;
            in_section = ini_equal_ci(record->name, section);
            continue;
        }
        if (ini_record_is_in_section(record, in_section) &&
            ini_equal_ci(record->name, key)) {
            char *replacement = ini_duplicate(value);
            if (!replacement) return FALSE;
            kfree(record->value);
            record->value = replacement;
            return TRUE;
        }
    }
    if (in_section && insert_at == UINT32_MAX) insert_at = document->count;

    INI_RECORD key_record;
    if (!ini_record_make(&key_record, INI_RECORD_KEY, key, strlen(key),
                         value, strlen(value)))
        return FALSE;
    if (insert_at != UINT32_MAX) {
        if (!ini_document_insert(document, insert_at, &key_record)) {
            ini_record_release(&key_record);
            return FALSE;
        }
        return TRUE;
    }

    INI_RECORD section_record;
    if (!ini_record_make(&section_record, INI_RECORD_SECTION,
                         section, strlen(section), NULL, 0) ||
        !ini_document_reserve(document, document->count + 2)) {
        ini_record_release(&section_record);
        ini_record_release(&key_record);
        return FALSE;
    }
    document->records[document->count++] = section_record;
    document->records[document->count++] = key_record;
    return TRUE;
}

static BOOL ini_delete_key(INI_DOCUMENT *document, const char *section,
                           const char *key)
{
    BOOL in_section = FALSE;
    for (uint32_t i = 0; i < document->count; i++) {
        INI_RECORD *record = &document->records[i];
        if (record->type == INI_RECORD_SECTION) {
            in_section = ini_equal_ci(record->name, section);
        } else if (ini_record_is_in_section(record, in_section) &&
                   ini_equal_ci(record->name, key)) {
            ini_document_remove(document, i, 1);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL ini_delete_section(INI_DOCUMENT *document, const char *section)
{
    BOOL removed = FALSE;
    uint32_t index = 0;
    while (index < document->count) {
        INI_RECORD *record = &document->records[index];
        if (record->type != INI_RECORD_SECTION ||
            !ini_equal_ci(record->name, section)) {
            index++;
            continue;
        }
        uint32_t end = index + 1;
        while (end < document->count &&
               document->records[end].type != INI_RECORD_SECTION)
            end++;
        ini_document_remove(document, index, end - index);
        removed = TRUE;
    }
    return removed;
}

static BOOL ini_serialized_size(const INI_DOCUMENT *document,
                                uint64_t *size_out)
{
    uint64_t size = 0;
    for (uint32_t i = 0; i < document->count; i++) {
        const INI_RECORD *record = &document->records[i];
        uint64_t add = strlen(record->name) + 2;
        if (record->type == INI_RECORD_SECTION) add += 2;
        if (record->type == INI_RECORD_KEY)
            add += 1 + strlen(record->value);
        if (size > UINT64_MAX - add) return FALSE;
        size += add;
    }
    *size_out = size;
    return TRUE;
}

static void ini_serialize(const INI_DOCUMENT *document, char *buffer)
{
    uint64_t offset = 0;
    for (uint32_t i = 0; i < document->count; i++) {
        const INI_RECORD *record = &document->records[i];
        if (record->type == INI_RECORD_SECTION) buffer[offset++] = '[';
        uint64_t length = strlen(record->name);
        memcpy(buffer + offset, record->name, length);
        offset += length;
        if (record->type == INI_RECORD_SECTION) buffer[offset++] = ']';
        if (record->type == INI_RECORD_KEY) {
            buffer[offset++] = '=';
            length = strlen(record->value);
            memcpy(buffer + offset, record->value, length);
            offset += length;
        }
        buffer[offset++] = '\r';
        buffer[offset++] = '\n';
    }
}

static BOOL ini_document_persist_locked(INI_DOCUMENT *document)
{
    uint64_t size;
    if (!ini_serialized_size(document, &size) || size > UINT32_MAX) {
        SetLastError(PROFILE_ERROR_FILE_TOO_LARGE);
        return FALSE;
    }
    char *buffer = size ? (char *)kmalloc(size) : NULL;
    if (size && !buffer) {
        SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (size) ini_serialize(document, buffer);

    uint64_t path_length = strlen(document->path);
    if (path_length + 4 > 260) {
        if (buffer) kfree(buffer);
        SetLastError(PROFILE_ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    char absolute[260] = "C:\\";
    memcpy(absolute + 3, document->path, path_length + 1);
    HANDLE file = CreateFileA(absolute, GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, 2 /* CREATE_ALWAYS */,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (buffer) kfree(buffer);
        return FALSE;
    }
    DWORD written = 0;
    BOOL ok = !size || WriteFile(file, buffer, (DWORD)size, &written, NULL);
    DWORD error = ok && written == (DWORD)size ? 0 : GetLastError();
    if (ok && written != (DWORD)size) {
        ok = FALSE;
        error = PROFILE_ERROR_WRITE_FAULT;
    }
    CloseHandle(file);
    if (buffer) kfree(buffer);
    if (!ok) {
        SetLastError(error ? error : PROFILE_ERROR_WRITE_FAULT);
        return FALSE;
    }

    void *identity = osfs2_find_exact_ci(document->path);
    document->file_identity = identity;
    document->file_revision = identity ? osfs2_file_revision(identity) : 0;
    document->file_size = identity ? osfs2_file_size(identity) : 0;
    document->loaded = TRUE;
    return TRUE;
}

DWORD WINAPI GetPrivateProfileStringA(PCSTR app_name, PCSTR key_name,
                                       PCSTR default_value,
                                       PSTR returned_string, DWORD size,
                                       PCSTR file_name)
{
    if (!returned_string || !size) return 0;
    spin_lock(&ini_lock);
    INI_DOCUMENT *document = ini_document_lookup_locked(file_name, TRUE);
    if (!document || !ini_document_refresh_locked(document)) {
        returned_string[0] = 0;
        spin_unlock(&ini_lock);
        return 0;
    }

    if (!app_name) {
        DWORD result = ini_write_multisz(document, INI_LIST_SECTIONS, NULL,
                                         returned_string, size);
        spin_unlock(&ini_lock);
        return result;
    }
    if (!key_name) {
        DWORD result = ini_write_multisz(document, INI_LIST_KEYS, app_name,
                                         returned_string, size);
        spin_unlock(&ini_lock);
        return result;
    }

    const char *value = ini_find_value(document, app_name, key_name);
    BOOL found = value != NULL;
    if (!found) value = default_value ? default_value : "";
    uint64_t length = strlen(value);
    if (!found) {
        while (length && (value[length - 1] == ' ' ||
                          value[length - 1] == '\t'))
            length--;
    }
    if (length >= size) length = size - 1;
    if (length) memcpy(returned_string, value, length);
    returned_string[length] = 0;
    spin_unlock(&ini_lock);
    return (DWORD)length;
}

static char *ini_wide_duplicate(PCWSTR source)
{
    if (!source) return NULL;
    uint64_t length = 0;
    while (source[length]) length++;
    char *result = (char *)kmalloc(length + 1);
    if (!result) return NULL;
    for (uint64_t i = 0; i < length; i++)
        result[i] = source[i] <= 0xFF ? (char)source[i] : '?';
    result[length] = 0;
    return result;
}

static char *ini_wide_multisz_duplicate(PCWSTR source)
{
    if (!source) return NULL;
    uint64_t length = 0;
    do {
        while (source[length]) length++;
        length++;
    } while (source[length]);
    length++;
    char *result = (char *)kmalloc(length);
    if (!result) return NULL;
    for (uint64_t i = 0; i < length; i++)
        result[i] = source[i] <= 0xFF ? (char)source[i] : '?';
    return result;
}

DWORD WINAPI GetPrivateProfileStringW(PCWSTR app_name, PCWSTR key_name,
                                       PCWSTR default_value,
                                       PWSTR returned_string, DWORD size,
                                       PCWSTR file_name)
{
    if (!returned_string || !size) return 0;
    char *app = ini_wide_duplicate(app_name);
    char *key = ini_wide_duplicate(key_name);
    char *fallback = ini_wide_duplicate(default_value);
    char *file = ini_wide_duplicate(file_name);
    char *narrow = (char *)kmalloc(size);
    if ((app_name && !app) || (key_name && !key) ||
        (default_value && !fallback) || (file_name && !file) || !narrow) {
        if (app) kfree(app);
        if (key) kfree(key);
        if (fallback) kfree(fallback);
        if (file) kfree(file);
        if (narrow) kfree(narrow);
        returned_string[0] = 0;
        SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    memset(narrow, 0, size);
    DWORD result = GetPrivateProfileStringA(
        app_name ? app : NULL, key_name ? key : NULL,
        default_value ? fallback : NULL, narrow, size,
        file_name ? file : NULL);
    for (DWORD i = 0; i < size; i++)
        returned_string[i] = (BYTE)narrow[i];
    if (app) kfree(app);
    if (key) kfree(key);
    if (fallback) kfree(fallback);
    if (file) kfree(file);
    kfree(narrow);
    return result;
}

BOOL WINAPI WritePrivateProfileStringA(PCSTR app_name, PCSTR key_name,
                                        PCSTR value, PCSTR file_name)
{
    spin_lock(&ini_lock);
    INI_DOCUMENT *document = ini_document_lookup_locked(file_name, TRUE);
    if (!document) {
        spin_unlock(&ini_lock);
        return FALSE;
    }
    if (!app_name) {
        BOOL valid_flush = key_name == NULL && value == NULL;
        if (valid_flush) ini_document_invalidate(document);
        spin_unlock(&ini_lock);
        return valid_flush;
    }
    if (!ini_document_refresh_locked(document)) {
        spin_unlock(&ini_lock);
        return FALSE;
    }

    BOOL changed;
    if (!key_name) {
        if (value) {
            SetLastError(PROFILE_ERROR_INVALID_PARAMETER);
            spin_unlock(&ini_lock);
            return FALSE;
        }
        changed = ini_delete_section(document, app_name);
    } else if (!value) {
        changed = ini_delete_key(document, app_name, key_name);
    } else {
        changed = ini_set_key(document, app_name, key_name, value);
        if (!changed) SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
        if (!changed) {
            spin_unlock(&ini_lock);
            return FALSE;
        }
    }
    BOOL result = !changed || ini_document_persist_locked(document);
    if (!result) ini_document_invalidate(document);
    spin_unlock(&ini_lock);
    return result;
}

BOOL WINAPI WritePrivateProfileStringW(PCWSTR app_name, PCWSTR key_name,
                                        PCWSTR value, PCWSTR file_name)
{
    char *app = ini_wide_duplicate(app_name);
    char *key = ini_wide_duplicate(key_name);
    char *text = ini_wide_duplicate(value);
    char *file = ini_wide_duplicate(file_name);
    if ((app_name && !app) || (key_name && !key) || (value && !text) ||
        (file_name && !file)) {
        if (app) kfree(app);
        if (key) kfree(key);
        if (text) kfree(text);
        if (file) kfree(file);
        SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    BOOL result = WritePrivateProfileStringA(
        app_name ? app : NULL, key_name ? key : NULL,
        value ? text : NULL, file_name ? file : NULL);
    if (app) kfree(app);
    if (key) kfree(key);
    if (text) kfree(text);
    if (file) kfree(file);
    return result;
}

UINT WINAPI GetPrivateProfileIntA(PCSTR app_name, PCSTR key_name,
                                   int default_value, PCSTR file_name)
{
    char buffer[64];
    DWORD length = GetPrivateProfileStringA(app_name, key_name, NULL,
                                             buffer, sizeof(buffer),
                                             file_name);
    if (!length) return (UINT)default_value;
    int sign = 1;
    uint32_t index = 0;
    while (buffer[index] == ' ' || buffer[index] == '\t') index++;
    if (buffer[index] == '-') {
        sign = -1;
        index++;
    } else if (buffer[index] == '+') {
        index++;
    }
    int value = 0;
    BOOL digit = FALSE;
    while (buffer[index] >= '0' && buffer[index] <= '9') {
        digit = TRUE;
        value = value * 10 + (buffer[index++] - '0');
    }
    return digit ? (UINT)(value * sign) : (UINT)default_value;
}

UINT WINAPI GetPrivateProfileIntW(PCWSTR app_name, PCWSTR key_name,
                                   int default_value, PCWSTR file_name)
{
    char *app = ini_wide_duplicate(app_name);
    char *key = ini_wide_duplicate(key_name);
    char *file = ini_wide_duplicate(file_name);
    if ((app_name && !app) || (key_name && !key) || (file_name && !file)) {
        if (app) kfree(app);
        if (key) kfree(key);
        if (file) kfree(file);
        SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
        return (UINT)default_value;
    }
    UINT result = GetPrivateProfileIntA(app_name ? app : NULL,
                                        key_name ? key : NULL,
                                        default_value,
                                        file_name ? file : NULL);
    if (app) kfree(app);
    if (key) kfree(key);
    if (file) kfree(file);
    return result;
}

DWORD WINAPI GetPrivateProfileSectionNamesA(PSTR buffer, DWORD size,
                                             PCSTR file_name)
{
    return GetPrivateProfileStringA(NULL, NULL, NULL, buffer, size, file_name);
}

DWORD WINAPI GetPrivateProfileSectionNamesW(PWSTR buffer, DWORD size,
                                             PCWSTR file_name)
{
    return GetPrivateProfileStringW(NULL, NULL, NULL, buffer, size, file_name);
}

DWORD WINAPI GetPrivateProfileSectionA(PCSTR app_name, PSTR buffer,
                                        DWORD size, PCSTR file_name)
{
    if (!buffer || !size || !app_name) return 0;
    spin_lock(&ini_lock);
    INI_DOCUMENT *document = ini_document_lookup_locked(file_name, TRUE);
    DWORD result = 0;
    if (document && ini_document_refresh_locked(document))
        result = ini_write_multisz(document, INI_LIST_SECTION, app_name,
                                   buffer, size);
    else
        buffer[0] = 0;
    spin_unlock(&ini_lock);
    return result;
}

DWORD WINAPI GetPrivateProfileSectionW(PCWSTR app_name, PWSTR buffer,
                                        DWORD size, PCWSTR file_name)
{
    if (!buffer || !size || !app_name) return 0;
    char *app = ini_wide_duplicate(app_name);
    char *file = ini_wide_duplicate(file_name);
    char *narrow = (char *)kmalloc(size);
    if (!app || (file_name && !file) || !narrow) {
        if (app) kfree(app);
        if (file) kfree(file);
        if (narrow) kfree(narrow);
        buffer[0] = 0;
        SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    memset(narrow, 0, size);
    DWORD result = GetPrivateProfileSectionA(app, narrow, size,
                                              file_name ? file : NULL);
    for (DWORD i = 0; i < size; i++) buffer[i] = (BYTE)narrow[i];
    kfree(app);
    if (file) kfree(file);
    kfree(narrow);
    return result;
}

BOOL WINAPI WritePrivateProfileSectionA(PCSTR app_name, PCSTR strings,
                                         PCSTR file_name)
{
    if (!app_name) {
        SetLastError(PROFILE_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    spin_lock(&ini_lock);
    INI_DOCUMENT *document = ini_document_lookup_locked(file_name, TRUE);
    if (!document || !ini_document_refresh_locked(document)) {
        spin_unlock(&ini_lock);
        return FALSE;
    }
    (void)ini_delete_section(document, app_name);
    if (strings) {
        if (!ini_document_append(document, INI_RECORD_SECTION,
                                 app_name, strlen(app_name), NULL, 0)) {
            SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
            ini_document_invalidate(document);
            spin_unlock(&ini_lock);
            return FALSE;
        }
        for (const char *item = strings; *item; item += strlen(item) + 1) {
            const char *equals = item;
            while (*equals && *equals != '=') equals++;
            uint64_t key_length = (uint64_t)(equals - item);
            const char *value = *equals ? equals + 1 : equals;
            if (key_length &&
                !ini_document_append(document, INI_RECORD_KEY,
                                     item, key_length, value, strlen(value))) {
                SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
                ini_document_invalidate(document);
                spin_unlock(&ini_lock);
                return FALSE;
            }
        }
    }
    BOOL result = ini_document_persist_locked(document);
    if (!result) ini_document_invalidate(document);
    spin_unlock(&ini_lock);
    return result;
}

BOOL WINAPI WritePrivateProfileSectionW(PCWSTR app_name, PCWSTR strings,
                                         PCWSTR file_name)
{
    char *app = ini_wide_duplicate(app_name);
    char *items = ini_wide_multisz_duplicate(strings);
    char *file = ini_wide_duplicate(file_name);
    if ((app_name && !app) || (strings && !items) || (file_name && !file)) {
        if (app) kfree(app);
        if (items) kfree(items);
        if (file) kfree(file);
        SetLastError(PROFILE_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    BOOL result = WritePrivateProfileSectionA(
        app_name ? app : NULL, strings ? items : NULL,
        file_name ? file : NULL);
    if (app) kfree(app);
    if (items) kfree(items);
    if (file) kfree(file);
    return result;
}
