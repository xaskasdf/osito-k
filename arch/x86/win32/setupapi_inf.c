/* Application INF file operations; runtime payloads come from external media. */
#include "setupapi_inf.h"
#include "kernel32_shim.h"
#include "advapi32_shim.h"
#include "ntdll_shim.h"
#include "version_shim.h"
#include "compat32.h"
#include "system_paths.h"
#include "../kernel/smp.h"

extern void *kmalloc(uint64_t bytes);
extern void kfree(void *memory);
#define INF_PATH 260
#define INF_TEXT 4096U
#define INF_QUERY_LIMIT (1024U * 1024U)
#define INF_HANDLES 128
#define INF_MAX_LINES 16384
#define INF_MAX_OPERATIONS 512
#define INF_WRONG_STYLE 0xe0000100U
#define INF_BAD_SECTION 0xe0000001U
#define INF_LINE_MISSING 0xe0000102U
#define INF_CLASS_MISMATCH 0xe0000201U
#define INF_ABORTED 995U
#define OPEN_EXISTING 3

typedef struct { char *section, *key, *value; } SETUP_LINE;
typedef struct setup_operation {
    struct setup_operation *next;
    DWORD type, flags;
    char source[INF_PATH], target[INF_PATH], directory[INF_PATH];
    char description[INF_PATH], tag[INF_PATH], leaf[INF_PATH];
} SETUP_OPERATION;
typedef struct {
    DWORD id, owner, kind, references;
    BOOL closed;
    char *text;
    SETUP_LINE *lines;
    unsigned count;
    char directory[INF_PATH], filename[INF_PATH];
    ULONGLONG modified;
    SETUP_OPERATION *operations;
} SETUP_OBJECT;
/* A pointer-free snapshot. VersionData uses UTF-16 for both A and W APIs.
 * The entry layout is measured with the native PE32/PE64 oracle. */
typedef struct {
    DWORD style, count, next, modified_low, modified_high;
    WORD keys, name_bytes;
    DWORD value_bytes, entry_bytes;
} SETUP_INF_INFO;
typedef struct {
    BYTE paths[32], media[40];
    char target[INF_PATH], source[INF_PATH], directory[INF_PATH];
    char description[INF_PATH], tag[INF_PATH], leaf[INF_PATH], newpath[INF_PATH];
} SETUP_NOTIFY;
static SETUP_OBJECT *setup_objects[INF_HANDLES];
static DWORD setup_next_id = 0x71000000;
static spinlock_t setup_object_lock = SPINLOCK_INIT;
#define SETUP_LOCK() uint64_t irq_flags; \
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(irq_flags) :: "memory"); \
    spin_lock(&setup_object_lock)
#define SETUP_UNLOCK() do { spin_unlock(&setup_object_lock); \
    if (irq_flags & 0x200) __asm__ volatile("sti" ::: "memory"); } while (0)

static BOOL setup_inf_eq(const char *a, const char *b)
{
    if (!a || !b) return FALSE;
    while (*a && *b) {
        unsigned x = (BYTE)*a++, y = (BYTE)*b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return FALSE;
    }
    return !*a && !*b;
}
static char *setup_inf_trim(char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r') text++;
    unsigned n = strlen(text);
    while (n && (text[n-1] == ' ' || text[n-1] == '\t' || text[n-1] == '\r')) text[--n] = 0;
    return text;
}
static BOOL setup_inf_next_field(const char **cursor, char *out, unsigned capacity)
{
    const char *text = *cursor;
    unsigned length = 0, kept = 0;
    BOOL quoted = FALSE;
    if (!text) { out[0] = 0; return TRUE; }
    while (*text) {
        char c = *text++;
        if (c == '"') {
            if (quoted && *text == '"') text++;
            else { quoted = !quoted; continue; }
        } else if (c == ',' && !quoted) {
            *cursor = text;
            out[kept] = 0;
            return TRUE;
        }
        if (!length && !quoted && (c == ' ' || c == '\t')) continue;
        if (length + 1 >= capacity) { SetLastError(206); return FALSE; }
        out[length++] = c;
        if (quoted || (c != ' ' && c != '\t')) kept = length;
    }
    out[kept] = 0;
    *cursor = NULL;
    return TRUE;
}
static BOOL setup_inf_field(const char *text, unsigned field, char *out)
{
    for (unsigned i = 0; i <= field; i++) {
        if (!setup_inf_next_field(&text, out, INF_PATH)) return FALSE;
        if (!text && i < field) { out[0] = 0; break; }
    }
    return TRUE;
}
static SETUP_LINE *setup_inf_find(SETUP_OBJECT *inf, const char *section, const char *key)
{
    for (unsigned i = 0; i < inf->count; i++)
        if (setup_inf_eq(inf->lines[i].section, section) && setup_inf_eq(inf->lines[i].key, key))
            return &inf->lines[i];
    return NULL;
}
static const char *setup_inf_directory(SETUP_OBJECT *inf, DWORD id)
{
    switch (id) {
    case 0: case 0xffff: case 0xffffffffU: return "";
    case 1: return inf->directory;
    case 10: return WIN32_WINDOWS_DIRECTORY;
    case 11: return WIN32_SYSTEM32_DIRECTORY;
    case 12: return WIN32_SYSTEM32_DIRECTORY "\\drivers";
    case 17: return WIN32_WINDOWS_DIRECTORY "\\inf";
    case 18: return WIN32_WINDOWS_DIRECTORY "\\Help";
    case 20: return WIN32_WINDOWS_DIRECTORY "\\Fonts";
    case 30: return "C:\\";
    case 16425: return WIN32_SYSWOW64_DIRECTORY;
    default: return NULL;
    }
}
static BOOL setup_inf_expand_text(SETUP_OBJECT *inf, const char *input,
                                  char *out, unsigned capacity)
{
    char *work = NULL;
    BOOL ok = FALSE;
    unsigned n = 0;
    while (*input) {
        if (n + 1 >= capacity) { SetLastError(206); goto done; }
        if (*input != '%') { out[n++] = *input++; continue; }
        if (input[1] == '%') { out[n++] = '%'; input += 2; continue; }
        const char *end = input + 1;
        while (*end && *end != '%') end++;
        if (!*end || end - input >= INF_TEXT) { out[n++] = *input++; continue; }
        if (!work) work = kmalloc(INF_TEXT * 2U);
        if (!work) { SetLastError(8); goto done; }
        memcpy(work, input + 1, end - input - 1); work[end - input - 1] = 0;
        SETUP_LINE *line = setup_inf_find(inf, "Strings", work);
        const char *replacement = NULL;
        if (line) {
            const char *value = line->value;
            if (!setup_inf_next_field(&value, work + INF_TEXT, INF_TEXT)) goto done;
            replacement = work + INF_TEXT;
        } else {
            DWORD id = 0; BOOL numeric = work[0] != 0;
            for (const char *p = work; *p && numeric; p++) {
                unsigned digit = (BYTE)*p - '0';
                if (digit > 9 || id > (0xffffffffU - digit) / 10U) numeric = FALSE;
                else id = id * 10U + digit;
            }
            if (numeric) replacement = setup_inf_directory(inf, id);
        }
        if (!replacement) { out[n++] = *input++; continue; }
        unsigned size = strlen(replacement);
        if (size >= capacity - n) { SetLastError(206); goto done; }
        /* INF string substitutions are one pass, including nested %tokens%. */
        memcpy(out + n, replacement, size); n += size; input = end + 1;
    }
    out[n] = 0;
    ok = TRUE;
done:
    if (work) kfree(work);
    return ok;
}
static BOOL setup_inf_expand(SETUP_OBJECT *inf, const char *input, char *out, unsigned depth)
{
    (void)depth;
    return setup_inf_expand_text(inf, input, out, INF_PATH);
}
static BOOL setup_inf_number(const char *text, DWORD *value)
{
    unsigned base = 10; DWORD n = 0;
    BOOL negative = *text == '-';
    if (negative) text++;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; text += 2; }
    if (!*text) { SetLastError(13); return FALSE; }
    while (*text) {
        unsigned d = (BYTE)*text++;
        if (d >= '0' && d <= '9') d -= '0';
        else if (base == 16 && d >= 'a' && d <= 'f') d -= 'a' - 10;
        else if (base == 16 && d >= 'A' && d <= 'F') d -= 'A' - 10;
        else { SetLastError(13); return FALSE; }
        if (d >= base || n > (0xffffffffU - d) / base) { SetLastError(13); return FALSE; }
        n = n * base + d;
    }
    *value = negative ? 0U - n : n;
    return TRUE;
}
static BOOL setup_inf_join(char *out, const char *base, const char *leaf)
{
    unsigned n = strlen(base), m = strlen(leaf);
    BOOL slash = n && m && base[n-1] != '\\' && base[n-1] != '/';
    if (n + m + slash >= INF_PATH) { SetLastError(206); return FALSE; }
    if (out != base) memcpy(out, base, n);
    if (slash) out[n++] = '\\';
    memcpy(out + n, leaf, m + 1);
    return TRUE;
}
static BOOL setup_inf_dir(SETUP_OBJECT *inf, const char *section, char *out)
{
    SETUP_LINE *line = section ? setup_inf_find(inf, "DestinationDirs", section) : NULL;
    if (!line) line = setup_inf_find(inf, "DestinationDirs", "DefaultDestDir");
    DWORD id = 11; char field[INF_PATH], subdir[INF_PATH] = "";
    if (line) {
        if (!setup_inf_field(line->value, 0, field) || !setup_inf_number(field, &id) ||
            !setup_inf_field(line->value, 1, field) || !setup_inf_expand(inf, field, subdir, 0)) return FALSE;
    }
    const char *base = setup_inf_directory(inf, id);
    if (!base) { SetLastError(120); return FALSE; }
    return setup_inf_join(out, base, subdir);
}
static BOOL setup_inf_parse(SETUP_OBJECT *inf, UINT *error_line)
{
    unsigned lines = 1;
    for (char *p = inf->text; *p; p++) if (*p == '\n') lines++;
    if (lines > INF_MAX_LINES) { SetLastError(8); return FALSE; }
    inf->lines = kmalloc((uint64_t)lines * sizeof(SETUP_LINE));
    if (!inf->lines) { SetLastError(8); return FALSE; }
    char *p = inf->text, *section = NULL;
    unsigned physical_line = 0;
    while (*p) {
        unsigned line = physical_line + 1;
        char *start = p, *write = start;
        BOOL continued, quoted;
        do {
            char *part = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            physical_line++;
            quoted = FALSE;
            for (char *c = part; *c; c++) {
                if (*c == '"') quoted = !quoted;
                if (!quoted && *c == ';') { *c = 0; break; }
            }
            part = setup_inf_trim(part);
            unsigned length = strlen(part);
            continued = !quoted && length && part[length-1] == '\\';
            if (continued) { part[--length] = 0; length = strlen(setup_inf_trim(part)); }
            /* Removing the backslash joins tokens without an added space.
             * A following comment-only physical line ends the continuation. */
            for (unsigned i = 0; i < length; i++) write[i] = part[i];
            write += length; *write = 0;
        } while (continued && *p);
        if (!*start) continue;
        if (*start == '[') {
            char *end = start + 1; while (*end && *end != ']') end++;
            if (!*end || *setup_inf_trim(end + 1)) {
                if (error_line) *error_line = line;
                SetLastError(INF_BAD_SECTION); return FALSE;
            }
            *end = 0; section = setup_inf_trim(start + 1);
            if (!*section || strlen(section) >= INF_PATH) {
                if (error_line) *error_line = line;
                SetLastError(INF_BAD_SECTION); return FALSE;
            }
            continue;
        }
        if (!section) {
            if (error_line) *error_line = line;
            SetLastError(0xe0000000U); return FALSE;
        }
        SETUP_LINE *entry = &inf->lines[inf->count++];
        entry->section = section; entry->key = NULL; entry->value = start;
        quoted = FALSE;
        for (char *c = start; *c; c++) {
            if (*c == '"') quoted = !quoted;
            if (!quoted && *c == '=') {
                *c = 0; entry->key = setup_inf_trim(start); entry->value = setup_inf_trim(c + 1);
                const char *key = entry->key;
                if (!setup_inf_next_field(&key, entry->key, strlen(entry->key) + 1U)) return FALSE;
                break;
            }
        }
    }
    return TRUE;
}
static void setup_inf_destroy(SETUP_OBJECT *object)
{
    SETUP_OPERATION *op = object->operations;
    while (op) { SETUP_OPERATION *next = op->next; kfree(op); op = next; }
    if (object->lines) kfree(object->lines);
    if (object->text) kfree(object->text);
    kfree(object);
}
static HANDLE setup_handle_add(SETUP_OBJECT *object, DWORD kind)
{
    object->owner = GetCurrentProcessId(); object->kind = kind; object->references = 1;
    HANDLE result = INVALID_HANDLE_VALUE;
    SETUP_LOCK();
    for (unsigned i = 0; i < INF_HANDLES; i++) if (!setup_objects[i]) {
        if (++setup_next_id >= 0x7fffffff) setup_next_id = 0x71000001;
        object->id = setup_next_id; setup_objects[i] = object;
        result = (HANDLE)(ULONG_PTR)object->id; break;
    }
    SETUP_UNLOCK();
    if (result == INVALID_HANDLE_VALUE) SetLastError(8);
    return result;
}
static SETUP_OBJECT *setup_handle_ref(HANDLE handle, DWORD kind)
{
    SETUP_OBJECT *result = NULL; DWORD owner = GetCurrentProcessId();
    SETUP_LOCK();
    for (unsigned i = 0; i < INF_HANDLES; i++) {
        SETUP_OBJECT *p = setup_objects[i];
        if (p && !p->closed && (ULONG_PTR)handle == p->id && p->kind == kind && p->owner == owner) {
            p->references++; result = p; break;
        }
    }
    SETUP_UNLOCK();
    if (!result) SetLastError(6);
    return result;
}
static void setup_handle_unref(SETUP_OBJECT *object)
{
    BOOL destroy = FALSE;
    SETUP_LOCK();
    if (!--object->references) {
        for (unsigned i = 0; i < INF_HANDLES; i++) if (setup_objects[i] == object) setup_objects[i] = NULL;
        destroy = TRUE;
    }
    SETUP_UNLOCK();
    if (destroy) setup_inf_destroy(object);
}
static BOOL setup_handle_drop(HANDLE handle, DWORD kind)
{
    SETUP_OBJECT *object = setup_handle_ref(handle, kind);
    if (!object) return FALSE;
    BOOL drop = FALSE;
    SETUP_LOCK();
    if (!object->closed) { object->closed = TRUE; drop = TRUE; }
    SETUP_UNLOCK();
    if (drop) setup_handle_unref(object);
    setup_handle_unref(object);
    SetLastError(drop ? 0 : 6);
    return drop;
}

static BOOL setup_inf_query_string(PCVOID input, BOOL wide, char *out, unsigned capacity)
{
    if (!input) { SetLastError(87); return FALSE; }
    unsigned step = wide ? 2U : 1U, length = 0;
    for (; length < capacity; length++) {
        const BYTE *p = (const BYTE *)input + length * step;
        if (!win32_user_range_readable(p, step, g_compat32_mode != 0)) {
            SetLastError(87); return FALSE;
        }
        if (wide ? !*(const WCHAR *)p : !*p) break;
    }
    if (length == capacity) { SetLastError(206); return FALSE; }
    if (wide) return WideCharToMultiByte(0, 0, input, length + 1, out, capacity, NULL, NULL) != 0;
    memcpy(out, input, length + 1);
    return TRUE;
}
static SETUP_OBJECT *setup_inf_lookup(HANDLE handle, PCVOID section, PCVOID key,
                                      BOOL wide, unsigned *index)
{
    char section_name[INF_PATH];
    if (!setup_inf_query_string(section, wide, section_name, sizeof(section_name))) return NULL;
    char *key_name = key ? kmalloc(INF_TEXT) : NULL;
    if (key && !key_name) { SetLastError(8); return NULL; }
    if (key && !setup_inf_query_string(key, wide, key_name, INF_TEXT)) {
        kfree(key_name); return NULL;
    }
    SETUP_OBJECT *inf = setup_handle_ref(handle, 1);
    if (inf) {
        unsigned i;
        for (i = 0; i < inf->count; i++)
            if (setup_inf_eq(inf->lines[i].section, section_name) &&
                (!key || setup_inf_eq(inf->lines[i].key, key_name))) break;
        if (i == inf->count) {
            setup_handle_unref(inf); inf = NULL; SetLastError(INF_LINE_MISSING);
        } else *index = i;
    }
    if (key_name) kfree(key_name);
    return inf;
}
static BOOL setup_inf_context_store(SETUP_OBJECT *inf, unsigned index, PVOID output)
{
    unsigned word = g_compat32_mode ? 4U : 8U;
    if (!win32_user_range_writable(output, word * 2U + 8U, g_compat32_mode != 0)) {
        SetLastError(87); return FALSE;
    }
    BYTE context[24] = {0};
    uint64_t handle = inf->id;
    DWORD section = 0, line = index;
    while (section < inf->count && !setup_inf_eq(inf->lines[section].section, inf->lines[index].section)) section++;
    /* Guest contexts carry process-owned integer handles, never kernel pointers. */
    memcpy(context, &handle, word); memcpy(context + word, &handle, word);
    memcpy(context + word * 2U, &section, 4); memcpy(context + word * 2U + 4U, &line, 4);
    memcpy(output, context, word * 2U + 8U);
    return TRUE;
}
static SETUP_OBJECT *setup_inf_context_ref(PCVOID input, unsigned *index)
{
    unsigned word = g_compat32_mode ? 4U : 8U;
    if (!win32_user_range_readable(input, word * 2U + 8U, g_compat32_mode != 0)) {
        SetLastError(87); return NULL;
    }
    uint64_t handle = 0, current = 0;
    DWORD section, line;
    memcpy(&handle, input, word); memcpy(&current, (const BYTE *)input + word, word);
    memcpy(&section, (const BYTE *)input + word * 2U, 4);
    memcpy(&line, (const BYTE *)input + word * 2U + 4U, 4);
    SETUP_OBJECT *inf = setup_handle_ref((HANDLE)(ULONG_PTR)handle, 1);
    if (!inf) { SetLastError(87); return NULL; }
    if (current != handle || section >= inf->count || line >= inf->count ||
        !setup_inf_eq(inf->lines[section].section, inf->lines[line].section)) {
        setup_handle_unref(inf); SetLastError(87); return NULL;
    }
    *index = line;
    return inf;
}
static BOOL setup_inf_find_first(HANDLE handle, PCVOID section, PCVOID key, PVOID context, BOOL wide)
{
    if (!win32_user_range_writable(context, g_compat32_mode ? 16U : 24U, g_compat32_mode != 0)) {
        SetLastError(87); return FALSE;
    }
    unsigned index;
    SETUP_OBJECT *inf = setup_inf_lookup(handle, section, key, wide, &index);
    if (!inf) return FALSE;
    BOOL ok = setup_inf_context_store(inf, index, context);
    setup_handle_unref(inf);
    if (ok) SetLastError(0);
    return ok;
}
BOOL WINAPI shim_SetupFindFirstLineA(HANDLE inf, PCSTR section, PCSTR key, PVOID context)
{
    return setup_inf_find_first(inf, section, key, context, FALSE);
}
BOOL WINAPI shim_SetupFindFirstLineW(HANDLE inf, PCWSTR section, PCWSTR key, PVOID context)
{
    return setup_inf_find_first(inf, section, key, context, TRUE);
}
BOOL WINAPI shim_SetupFindNextLine(PCVOID input, PVOID output)
{
    if (!win32_user_range_writable(output, g_compat32_mode ? 16U : 24U, g_compat32_mode != 0)) {
        SetLastError(87); return FALSE;
    }
    unsigned index;
    SETUP_OBJECT *inf = setup_inf_context_ref(input, &index);
    if (!inf) return FALSE;
    unsigned next = index + 1;
    while (next < inf->count && !setup_inf_eq(inf->lines[next].section, inf->lines[index].section)) next++;
    BOOL ok = FALSE;
    if (next == inf->count) SetLastError(INF_LINE_MISSING);
    else ok = setup_inf_context_store(inf, next, output);
    setup_handle_unref(inf);
    if (ok) SetLastError(0);
    return ok;
}
static char *setup_inf_compact(SETUP_OBJECT *inf, unsigned index, unsigned *length)
{
    char *work = kmalloc(INF_TEXT * 2U), *output = NULL;
    if (!work) { SetLastError(8); return NULL; }
    for (unsigned pass = 0; pass < 2; pass++) {
        const char *cursor = inf->lines[index].value;
        unsigned size = 0, fields = 0;
        while (cursor) {
            if (!setup_inf_next_field(&cursor, work, INF_TEXT) ||
                !setup_inf_expand_text(inf, work, work + INF_TEXT, INF_TEXT)) goto fail;
            unsigned field_size = strlen(work + INF_TEXT), comma = fields++ != 0;
            if (field_size + comma > INF_QUERY_LIMIT - size) { SetLastError(8); goto fail; }
            if (output) {
                if (comma) output[size] = ',';
                memcpy(output + size + comma, work + INF_TEXT, field_size);
            }
            size += field_size + comma;
        }
        if (!pass) {
            output = kmalloc((uint64_t)size + 1);
            if (!output) { SetLastError(8); goto fail; }
        } else { output[size] = 0; *length = size + 1; }
    }
    kfree(work);
    return output;
fail:
    if (output) kfree(output);
    kfree(work);
    return NULL;
}
static BOOL setup_inf_line_text(PCVOID context, HANDLE handle, PCVOID section,
                                 PCVOID key, PVOID output, DWORD capacity, DWORD *required, BOOL wide)
{
    if ((!context && (!section || !key)) ||
        (required && !win32_user_range_writable(required, 4, g_compat32_mode != 0))) {
        SetLastError(87); return FALSE;
    }
    unsigned index, length;
    SETUP_OBJECT *inf = context ? setup_inf_context_ref(context, &index) :
                                 setup_inf_lookup(handle, section, key, wide, &index);
    if (!inf) return FALSE;
    char *text = setup_inf_compact(inf, index, &length);
    setup_handle_unref(inf);
    if (!text) return FALSE;
    DWORD error = 0, needed = length;
    if (wide) {
        needed = MultiByteToWideChar(0, 0, text, length, NULL, 0);
        if (!needed) { error = GetLastError(); goto done; }
    }
    if (required) *required = needed;
    if (output) {
        if (capacity < needed) { error = 122; goto done; }
        if (!win32_user_range_writable(output, (SIZE_T)needed * (wide ? 2U : 1U), g_compat32_mode != 0)) {
            error = 87; goto done;
        }
        if (wide) {
            if (!MultiByteToWideChar(0, 0, text, length, output, needed)) error = GetLastError();
        } else memcpy(output, text, length);
    }
done:
    kfree(text);
    SetLastError(error);
    return error == 0;
}
BOOL WINAPI shim_SetupGetLineTextA(PCVOID context, HANDLE inf, PCSTR section,
                                  PCSTR key, PSTR output, DWORD capacity, DWORD *required)
{
    return setup_inf_line_text(context, inf, section, key, output, capacity, required, FALSE);
}
BOOL WINAPI shim_SetupGetLineTextW(PCVOID context, HANDLE inf, PCWSTR section,
                                  PCWSTR key, PWSTR output, DWORD capacity, DWORD *required)
{
    return setup_inf_line_text(context, inf, section, key, output, capacity, required, TRUE);
}
void setupapi_release_process(DWORD owner)
{
    /* All owner threads must already be stopped, including queued callbacks. */
    for (unsigned i = 0; i < INF_HANDLES; i++) {
        SETUP_OBJECT *object = NULL;
        SETUP_LOCK();
        if (setup_objects[i] && setup_objects[i]->owner == owner) {
            object = setup_objects[i]; setup_objects[i] = NULL;
        }
        SETUP_UNLOCK();
        if (object) setup_inf_destroy(object);
    }
}

HANDLE WINAPI shim_SetupOpenInfFileA(PCSTR path, PCSTR inf_class, DWORD style, UINT *line)
{
    if (line) *line = 0;
    if (!path) { SetLastError(87); return INVALID_HANDLE_VALUE; }
    if (!(style & 2)) { SetLastError(INF_WRONG_STYLE); return INVALID_HANDLE_VALUE; }
    char full[INF_PATH]; BOOL separator = FALSE;
    for (const char *p = path; *p; p++) if (*p == '\\' || *p == '/') separator = TRUE;
    if (separator) {
        DWORD size = GetFullPathNameA(path, sizeof(full), full, NULL);
        if (!size || size >= sizeof(full)) { SetLastError(206); return INVALID_HANDLE_VALUE; }
    } else {
        if (!setup_inf_join(full, WIN32_WINDOWS_DIRECTORY "\\inf", path)) return INVALID_HANDLE_VALUE;
        if (GetFileAttributesA(full) == 0xffffffffU && !setup_inf_join(full, WIN32_SYSTEM32_DIRECTORY, path)) return INVALID_HANDLE_VALUE;
    }
    HANDLE file = CreateFileA(full, GENERIC_READ, 7, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return file;
    DWORD high = 0, size = GetFileSize(file, &high), got = 0;
    if (high || size > 1024 * 1024) { CloseHandle(file); SetLastError(8); return INVALID_HANDLE_VALUE; }
    SETUP_OBJECT *inf = kmalloc(sizeof(*inf));
    if (!inf) { CloseHandle(file); SetLastError(8); return INVALID_HANDLE_VALUE; }
    memset(inf, 0, sizeof(*inf)); inf->text = kmalloc((uint64_t)size + 1);
    if (!inf->text) { CloseHandle(file); setup_inf_destroy(inf); SetLastError(8); return INVALID_HANDLE_VALUE; }
    BOOL ok = ReadFile(file, inf->text, size, &got, NULL) && got == size;
    DWORD last = GetLastError();
    IO_STATUS_BLOCK iosb; FILE_BASIC_INFORMATION basic;
    if (NT_SUCCESS(NtQueryInformationFile(file, &iosb, &basic, sizeof(basic), FileBasicInformation)))
        memcpy(&inf->modified, &basic.LastWriteTime, sizeof(inf->modified));
    CloseHandle(file);
    if (!ok) { setup_inf_destroy(inf); SetLastError(last ? last : 30); return INVALID_HANDLE_VALUE; }
    inf->text[size] = 0;
    if (size >= 2 && (BYTE)inf->text[0] == 0xff && (BYTE)inf->text[1] == 0xfe) {
        unsigned n = 0;
        for (unsigned i = 2; i + 1 < size; i += 2) {
            if (inf->text[i+1] || (BYTE)inf->text[i] >= 128) { setup_inf_destroy(inf); SetLastError(1113); return INVALID_HANDLE_VALUE; }
            inf->text[n++] = inf->text[i];
        }
        inf->text[n] = 0;
    }
    memcpy(inf->filename, full, strlen(full) + 1);
    memcpy(inf->directory, full, strlen(full) + 1);
    unsigned end = strlen(inf->directory);
    while (end && inf->directory[end-1] != '\\' && inf->directory[end-1] != '/') end--;
    if (end > 3) end--;
    inf->directory[end] = 0;
    if (!setup_inf_parse(inf, line)) { last = GetLastError(); setup_inf_destroy(inf); SetLastError(last); return INVALID_HANDLE_VALUE; }
    SETUP_LINE *signature = setup_inf_find(inf, "Version", "Signature");
    char value[INF_PATH];
    if (!signature || !setup_inf_field(signature->value, 0, value) ||
        (!setup_inf_eq(value, "$CHICAGO$") && !setup_inf_eq(value, "$Windows NT$"))) {
        setup_inf_destroy(inf); SetLastError(INF_WRONG_STYLE); return INVALID_HANDLE_VALUE;
    }
    if (inf_class) {
        SETUP_LINE *cls = setup_inf_find(inf, "Version", "Class");
        if (!cls || !setup_inf_field(cls->value, 0, value) || !setup_inf_eq(value, inf_class)) {
            setup_inf_destroy(inf); SetLastError(INF_CLASS_MISMATCH); return INVALID_HANDLE_VALUE;
        }
    }
    HANDLE result = setup_handle_add(inf, 1);
    if (result == INVALID_HANDLE_VALUE) setup_inf_destroy(inf); else SetLastError(0);
    return result;
}
static HANDLE setup_inf_info_open(PCVOID spec, DWORD search, BOOL wide)
{
    char name[INF_PATH], full[INF_PATH];
    if (!setup_inf_query_string(spec, wide, name, sizeof(name))) return INVALID_HANDLE_VALUE;
    if (search == 2) {
        DWORD size = GetFullPathNameA(name, sizeof(full), full, NULL);
        if (!size || size >= sizeof(full)) { SetLastError(206); return INVALID_HANDLE_VALUE; }
        return shim_SetupOpenInfFileA(full, NULL, 2, NULL);
    }
    for (const char *p = name; *p; p++) if (*p == ':' || *p == '/' || *p == '\\') {
        SetLastError(123); return INVALID_HANDLE_VALUE;
    }
    if (search == 3 || search == 4) {
        const char *dirs[2] = { WIN32_WINDOWS_DIRECTORY "\\inf", WIN32_SYSTEM32_DIRECTORY };
        for (unsigned i = 0; i < 2; i++) {
            if (!setup_inf_join(full, dirs[search == 4 ? 1U - i : i], name)) return INVALID_HANDLE_VALUE;
            HANDLE result = shim_SetupOpenInfFileA(full, NULL, 2, NULL);
            if (result != INVALID_HANDLE_VALUE) return result;
            DWORD error = GetLastError();
            if (error != 2 && error != 3) return INVALID_HANDLE_VALUE;
        }
        SetLastError(2); return INVALID_HANDLE_VALUE;
    }
    HKEY key = NULL; DWORD size = 0, type = 0;
    LONG status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion", 0, KEY_QUERY_VALUE, &key);
    if (status) { SetLastError(status); return INVALID_HANDLE_VALUE; }
    status = RegQueryValueExA(key, "DevicePath", NULL, &type, NULL, &size);
    if (status || !size || size > INF_QUERY_LIMIT || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        RegCloseKey(key); SetLastError(status ? (DWORD)status : 13); return INVALID_HANDLE_VALUE;
    }
    char *paths = kmalloc((uint64_t)size + 1);
    if (!paths) { RegCloseKey(key); SetLastError(8); return INVALID_HANDLE_VALUE; }
    status = RegQueryValueExA(key, "DevicePath", NULL, &type, (BYTE *)paths, &size);
    RegCloseKey(key);
    HANDLE result = INVALID_HANDLE_VALUE;
    if (status || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        SetLastError(status ? (DWORD)status : 13); goto done;
    }
    paths[size] = 0;
    for (char *cursor = paths; *cursor;) {
        char *end = cursor; while (*end && *end != ';') end++;
        BOOL more = *end != 0; *end = 0;
        unsigned n = 0;
        for (char *p = cursor; *p;) {
            char variable[INF_PATH], value[INF_PATH];
            const char *text = p; unsigned bytes = 1;
            if (*p == '%') {
                char *finish = p + 1; while (*finish && *finish != '%') finish++;
                if (*finish && finish - p - 1 < INF_PATH) {
                    unsigned count = finish - p - 1;
                    memcpy(variable, p + 1, count); variable[count] = 0;
                    DWORD got = GetEnvironmentVariableA(variable, value, sizeof(value));
                    if (got >= sizeof(value)) { SetLastError(206); goto done; }
                    if (got) { text = value; bytes = got; p = finish + 1; }
                    else p++;
                } else p++;
            } else p++;
            if (bytes >= sizeof(full) - n) { SetLastError(206); goto done; }
            memcpy(full + n, text, bytes); n += bytes;
        }
        full[n] = 0;
        if (n) {
            if (!setup_inf_join(full, full, name)) goto done;
            result = shim_SetupOpenInfFileA(full, NULL, 2, NULL);
            if (result != INVALID_HANDLE_VALUE) goto done;
            DWORD error = GetLastError();
            if (error != 2 && error != 3) goto done;
        }
        cursor = more ? end + 1 : end;
    }
    SetLastError(2);
done:
    kfree(paths);
    return result;
}
static SETUP_INF_INFO *setup_inf_info_pack(SETUP_OBJECT *inf)
{
    char *work = kmalloc(INF_TEXT * 2U);
    SETUP_INF_INFO *info = NULL;
    if (!work) { SetLastError(8); return NULL; }
    DWORD name_chars = MultiByteToWideChar(0, 0, inf->filename, -1, NULL, 0);
    if (!name_chars || name_chars > 0x7fff) goto fail;
    for (unsigned pass = 0; pass < 2; pass++) {
        DWORD bytes = 0, keys = 0;
        for (unsigned i = 0; i < inf->count; i++) {
            SETUP_LINE *line = &inf->lines[i];
            if (!setup_inf_eq(line->section, "Version") || !*line->key) continue;
            const char *cursor = line->value;
            if (!setup_inf_next_field(&cursor, work, INF_TEXT) ||
                !setup_inf_expand_text(inf, work, work + INF_TEXT, INF_TEXT)) goto fail;
            const char *parts[2] = { line->key, work + INF_TEXT };
            for (unsigned part = 0; part < 2; part++) {
                DWORD chars = MultiByteToWideChar(0, 0, parts[part], -1, NULL, 0);
                if (!chars) goto fail;
                if (chars > (INF_QUERY_LIMIT - sizeof(*info) - name_chars * 2U - bytes) / 2U) {
                    SetLastError(8); goto fail;
                }
                if (info && !MultiByteToWideChar(0, 0, parts[part], -1,
                    (WCHAR *)((BYTE *)(info + 1) + name_chars * 2U + bytes), chars)) goto fail;
                bytes += chars * 2U;
            }
            keys++;
        }
        if (!pass) {
            info = kmalloc(sizeof(*info) + name_chars * 2U + bytes);
            if (!info) { SetLastError(8); goto fail; }
            memset(info, 0, sizeof(*info)); info->style = 2; info->count = 1;
            info->modified_low = (DWORD)inf->modified; info->modified_high = inf->modified >> 32;
            info->keys = keys; info->name_bytes = name_chars * 2U; info->value_bytes = bytes;
            info->entry_bytes = sizeof(*info) - 8U + info->name_bytes + bytes;
            if (!MultiByteToWideChar(0, 0, inf->filename, -1, (WCHAR *)(info + 1), name_chars)) goto fail;
        }
    }
    kfree(work); return info;
fail:
    if (info) kfree(info);
    kfree(work); return NULL;
}
static BOOL setup_inf_information(PCVOID spec, DWORD search, PVOID output,
                                    DWORD capacity, DWORD *required, BOOL wide)
{
    DWORD saved = GetLastError();
    if (search < 1 || search > 5 ||
        (required && !win32_user_range_writable(required, 4, g_compat32_mode != 0))) {
        SetLastError(87); return FALSE;
    }
    HANDLE handle = (HANDLE)spec;
    if (search != 1) {
        handle = setup_inf_info_open(spec, search, wide);
        if (handle == INVALID_HANDLE_VALUE) return FALSE;
    }
    SETUP_OBJECT *inf = setup_handle_ref(handle, 1);
    SETUP_INF_INFO *info = inf ? setup_inf_info_pack(inf) : NULL;
    DWORD error = GetLastError();
    if (inf) setup_handle_unref(inf);
    if (search != 1) setup_handle_drop(handle, 1);
    if (!info) { SetLastError(error); return FALSE; }
    DWORD bytes = info->entry_bytes + 8U;
    if (required) *required = bytes;
    error = 0;
    if (!output) { if (capacity) error = 87; }
    else if (capacity < bytes) error = 122;
    else if (!win32_user_range_writable(output, bytes, g_compat32_mode != 0)) error = 87;
    else memcpy(output, info, bytes);
    kfree(info);
    SetLastError(error ? error : (search == 1 || wide ? saved : 0));
    return error == 0;
}
BOOL WINAPI shim_SetupGetInfInformationA(PCVOID spec, DWORD search, PVOID output, DWORD capacity, DWORD *required)
{ return setup_inf_information(spec, search, output, capacity, required, FALSE); }
BOOL WINAPI shim_SetupGetInfInformationW(PCVOID spec, DWORD search, PVOID output, DWORD capacity, DWORD *required)
{ return setup_inf_information(spec, search, output, capacity, required, TRUE); }
static SETUP_INF_INFO *setup_inf_info_validate(PCVOID input)
{
    SETUP_INF_INFO head;
    if (!win32_user_range_readable(input, sizeof(head), g_compat32_mode != 0)) {
        SetLastError(87); return NULL;
    }
    memcpy(&head, input, sizeof(head));
    if (head.style != 2 || head.count != 1 || head.next || head.name_bytes < 2 ||
        (head.name_bytes & 1) || (head.value_bytes & 1) ||
        head.entry_bytes > INF_QUERY_LIMIT - 8U || head.entry_bytes < sizeof(head) - 8U + head.name_bytes ||
        head.value_bytes != head.entry_bytes - (sizeof(head) - 8U) - head.name_bytes ||
        !win32_user_range_readable(input, head.entry_bytes + 8U, g_compat32_mode != 0)) {
        SetLastError(13); return NULL;
    }
    SETUP_INF_INFO *copy = kmalloc(head.entry_bytes + 8U);
    if (!copy) { SetLastError(8); return NULL; }
    memcpy(copy, input, head.entry_bytes + 8U);
    if (memcmp(copy, &head, sizeof(head))) goto invalid;
    const WCHAR *name = (const WCHAR *)(copy + 1);
    if (name[head.name_bytes / 2U - 1]) goto invalid;
    const WCHAR *p = name + head.name_bytes / 2U, *end = p + head.value_bytes / 2U;
    for (unsigned i = 0; i < (unsigned)head.keys * 2U; i++) {
        while (p < end && *p) p++;
        if (p == end) goto invalid;
        p++;
    }
    if (p == end) return copy;
invalid:
    kfree(copy); SetLastError(13); return NULL;
}
static BOOL setup_inf_info_query(PCVOID input, UINT index, PCVOID key, BOOL version,
                                 PVOID output, DWORD capacity, DWORD *required, BOOL wide)
{
    if (required && !win32_user_range_writable(required, 4, g_compat32_mode != 0)) {
        SetLastError(87); return FALSE;
    }
    SETUP_INF_INFO *info = setup_inf_info_validate(input);
    if (!info) return FALSE;
    DWORD error = 0; char *key_text = NULL; WCHAR *list = NULL;
    if (index) { error = 259; goto done; }
    const WCHAR *value = (const WCHAR *)(info + 1);
    DWORD chars = info->name_bytes / 2U;
    if (version) {
        value += chars; chars = info->value_bytes / 2U;
        if (key) {
            key_text = kmalloc(INF_TEXT * 2U);
            if (!key_text) { error = 8; goto done; }
            if (!setup_inf_query_string(key, wide, key_text, INF_TEXT)) { error = GetLastError(); goto done; }
            BOOL found = FALSE;
            for (unsigned i = 0; i < info->keys; i++) {
                const WCHAR *name = value; while (*value) value++;
                DWORD name_chars = value++ - name;
                const WCHAR *text = value; while (*value) value++;
                DWORD text_chars = value++ - text + 1;
                if (!WideCharToMultiByte(0, 0, name, name_chars + 1, key_text + INF_TEXT, INF_TEXT, NULL, NULL)) {
                    error = GetLastError(); goto done;
                }
                if (setup_inf_eq(key_text, key_text + INF_TEXT)) {
                    value = text; chars = text_chars; found = TRUE; break;
                }
            }
            if (!found) { error = 13; goto done; }
        } else {
            list = kmalloc(((uint64_t)chars + 1U) * 2U);
            if (!list) { error = 8; goto done; }
            memcpy(list, value, chars * 2U); list[chars++] = 0; value = list;
        }
    }
    DWORD needed = wide ? chars : (DWORD)WideCharToMultiByte(0, 0, value, chars, NULL, 0, NULL, NULL);
    if (!needed) { error = GetLastError(); goto done; }
    if (required) *required = needed;
    if (output) {
        if (capacity < needed) { error = 122; goto done; }
        if (!win32_user_range_writable(output, (SIZE_T)needed * (wide ? 2U : 1U), g_compat32_mode != 0)) {
            error = 87; goto done;
        }
        if (wide) memcpy(output, value, chars * 2U);
        else if (!WideCharToMultiByte(0, 0, value, chars, output, needed, NULL, NULL)) error = GetLastError();
    }
done:
    if (key_text) kfree(key_text);
    if (list) kfree(list);
    kfree(info); SetLastError(error); return error == 0;
}
BOOL WINAPI shim_SetupQueryInfFileInformationA(PCVOID input, UINT index, PSTR output, DWORD capacity, DWORD *required)
{ return setup_inf_info_query(input, index, NULL, FALSE, output, capacity, required, FALSE); }
BOOL WINAPI shim_SetupQueryInfFileInformationW(PCVOID input, UINT index, PWSTR output, DWORD capacity, DWORD *required)
{ return setup_inf_info_query(input, index, NULL, FALSE, output, capacity, required, TRUE); }
BOOL WINAPI shim_SetupQueryInfVersionInformationA(PCVOID input, UINT index, PCSTR key, PSTR output, DWORD capacity, DWORD *required)
{ return setup_inf_info_query(input, index, key, TRUE, output, capacity, required, FALSE); }
BOOL WINAPI shim_SetupQueryInfVersionInformationW(PCVOID input, UINT index, PCWSTR key, PWSTR output, DWORD capacity, DWORD *required)
{ return setup_inf_info_query(input, index, key, TRUE, output, capacity, required, TRUE); }
void WINAPI shim_SetupCloseInfFile(HANDLE inf) { setup_handle_drop(inf, 1); }
HANDLE WINAPI shim_SetupOpenFileQueue(void)
{
    SETUP_OBJECT *queue = kmalloc(sizeof(*queue));
    if (!queue) { SetLastError(8); return INVALID_HANDLE_VALUE; }
    memset(queue, 0, sizeof(*queue));
    HANDLE handle = setup_handle_add(queue, 2);
    if (handle == INVALID_HANDLE_VALUE) setup_inf_destroy(queue); else SetLastError(0);
    return handle;
}
BOOL WINAPI shim_SetupCloseFileQueue(HANDLE queue) { return setup_handle_drop(queue, 2); }
PVOID WINAPI shim_SetupInitDefaultQueueCallback(HANDLE owner)
{
    (void)owner;
    HANDLE queue = shim_SetupOpenFileQueue();
    return queue == INVALID_HANDLE_VALUE ? NULL : queue;
}
void WINAPI shim_SetupTermDefaultQueueCallback(PVOID context) { setup_handle_drop(context, 2); }
UINT WINAPI shim_SetupDefaultQueueCallbackA(PVOID context, UINT notification, ULONG_PTR p1, ULONG_PTR p2)
{
    (void)context; (void)p1; (void)p2;
    switch (notification) {
    case 1: case 3: case 5: case 8: case 11: case 14: return 1;
    /* No fabricated recovery on a missing source, failed copy or conflict.
     * Interactive media/error dialogs are not implemented yet. */
    default: return 0;
    }
}
static BOOL setup_inf_file_version(const char *path, uint64_t *version)
{
    DWORD size = shim_GetFileVersionInfoSizeA(path, NULL);
    if (!size || size > 1024 * 1024) return FALSE;
    void *buffer = LocalAlloc(0, size);
    if (!buffer) return FALSE;
    void *fixed = NULL; UINT length = 0;
    BOOL ok = shim_GetFileVersionInfoA(path, 0, size, buffer) &&
              shim_VerQueryValueA(buffer, "\\", &fixed, &length) && length >= 16;
    if (ok) {
        DWORD *fields = fixed;
        ok = fields[0] == 0xfeef04bdU;
        if (ok) *version = ((uint64_t)fields[2] << 32) | fields[3];
    }
    LocalFree(buffer); return ok;
}
static BOOL setup_inf_make_dirs(const char *file)
{
    char path[INF_PATH]; memcpy(path, file, strlen(file) + 1);
    for (unsigned i = 3; path[i]; i++) if (path[i] == '\\' || path[i] == '/') {
        char saved = path[i]; path[i] = 0;
        DWORD attr = GetFileAttributesA(path);
        if (attr == 0xffffffffU) {
            if (!CreateDirectoryA(path, NULL)) return FALSE;
        } else if (!(attr & 0x10)) { SetLastError(3); return FALSE; }
        path[i] = saved;
    }
    return TRUE;
}
static UINT setup_inf_callback(PVOID callback, PVOID context, UINT notification, ULONG_PTR p1, ULONG_PTR p2)
{
    if (!callback) return shim_SetupDefaultQueueCallbackA(context, notification, p1, p2);
    if (!win32_user_range_executable(callback, 1, g_compat32_mode != 0)) { SetLastError(87); return 0; }
    if (g_compat32_mode) {
        uint32_t args[4] = {(uint32_t)(ULONG_PTR)context, notification, (uint32_t)p1, (uint32_t)p2};
        uint32_t stack = compat32_current_user_stack_top();
        return stack ? compat32_callback_args_on_stack((uint32_t)(ULONG_PTR)callback, 4, args, stack) :
                       compat32_callback_args((uint32_t)(ULONG_PTR)callback, 4, args);
    }
    typedef UINT (WINAPI *FN)(PVOID, UINT, ULONG_PTR, ULONG_PTR);
    return ((FN)callback)(context, notification, p1, p2);
}
static void setup_inf_file_paths(SETUP_NOTIFY *n, SETUP_OPERATION *op, DWORD error)
{
    unsigned step = g_compat32_mode ? 4 : 8;
    ULONG_PTR target = (ULONG_PTR)n->target, source = op->type == 2 ? 0 : (ULONG_PTR)n->source;
    memcpy(n->target, op->target, strlen(op->target) + 1);
    memcpy(n->source, op->source, strlen(op->source) + 1);
    memcpy(n->paths, &target, step); memcpy(n->paths + step, &source, step);
    memcpy(n->paths + step * 2, &error, 4); memcpy(n->paths + step * 2 + 4, &op->flags, 4);
}
static UINT setup_inf_media(SETUP_NOTIFY *n, SETUP_OPERATION *op, PVOID callback, PVOID context)
{
    unsigned step = g_compat32_mode ? 4 : 8;
    memcpy(n->directory, op->directory, strlen(op->directory) + 1);
    memcpy(n->description, op->description, strlen(op->description) + 1);
    memcpy(n->tag, op->tag, strlen(op->tag) + 1);
    memcpy(n->leaf, op->leaf, strlen(op->leaf) + 1); n->newpath[0] = 0;
    ULONG_PTR pointers[4] = {op->tag[0] ? (ULONG_PTR)n->tag : 0, (ULONG_PTR)n->description,
                            (ULONG_PTR)n->directory, (ULONG_PTR)n->leaf};
    for (unsigned i = 0; i < 4; i++) memcpy(n->media + i * step, &pointers[i], step);
    memcpy(n->media + 4 * step, &op->flags, 4);
    UINT action = setup_inf_callback(callback, context, 14, (ULONG_PTR)n->media, (ULONG_PTR)n->newpath);
    if (action == 4) {
        n->newpath[INF_PATH-1] = 0;
        if (!setup_inf_join(op->source, n->newpath, op->leaf)) return 0;
        return 1;
    }
    return action;
}
static DWORD setup_inf_operation(SETUP_OPERATION *op, SETUP_NOTIFY *n, PVOID callback, PVOID context)
{
    UINT start = op->type == 2 ? 5 : op->type == 1 ? 8 : 11;
    if (!op->type) {
        UINT media = setup_inf_media(n, op, callback, context);
        if (!media) return INF_ABORTED;
        if (media == 2) return 0;
    }
    setup_inf_file_paths(n, op, 0);
    UINT action = setup_inf_callback(callback, context, start, (ULONG_PTR)n->paths, op->type);
    DWORD failure = action == 0 ? INF_ABORTED : 0;
    if (action != 2 && action != 0) for (unsigned attempt = 0; attempt < 8; attempt++) {
        BOOL ok;
        if (op->type == 2) ok = DeleteFileA(op->target);
        else if (op->type == 1) ok = MoveFileA(op->source, op->target);
        else {
            BOOL skip = FALSE;
            DWORD attributes = GetFileAttributesA(op->target);
            if ((op->flags & 2) && attributes == 0xffffffffU) skip = TRUE;
            if (attributes != 0xffffffffU) {
                if (op->flags & 0x1000) skip = TRUE;
                if ((op->flags & 8) && !setup_inf_callback(callback, context, 0x20000, (ULONG_PTR)n->paths, 0)) skip = TRUE;
                if (op->flags & (4 | 0x2000 | 0x10000)) {
                    uint64_t target_version = 0, source_version = 0;
                    BOOL tv = setup_inf_file_version(op->target, &target_version);
                    BOOL sv = setup_inf_file_version(op->source, &source_version);
                    if (tv && (!sv || target_version > source_version ||
                        ((op->flags & 0x10000) && target_version == source_version))) {
                        if (op->flags & (0x2000 | 0x10000)) skip = TRUE;
                        else if (!setup_inf_callback(callback, context, 0x40000, (ULONG_PTR)n->paths, 0)) skip = TRUE;
                    }
                }
            }
            ok = skip || (setup_inf_make_dirs(op->target) && CopyFileA(op->source, op->target, FALSE));
            if (ok && !skip && (op->flags & 1)) ok = DeleteFileA(op->source);
        }
        if (ok) { failure = 0; break; }
        failure = GetLastError(); setup_inf_file_paths(n, op, failure);
        n->newpath[0] = 0;
        action = setup_inf_callback(callback, context, start + 2, (ULONG_PTR)n->paths, (ULONG_PTR)n->newpath);
        if (action == 2) { failure = 0; break; }
        if (action == 4 && !op->type) {
            n->newpath[INF_PATH-1] = 0;
            if (!setup_inf_join(op->source, n->newpath, op->leaf)) { failure = GetLastError(); break; }
        } else if (action != 1) { failure = INF_ABORTED; break; }
    }
    setup_inf_file_paths(n, op, failure);
    setup_inf_callback(callback, context, start + 1, (ULONG_PTR)n->paths, op->type);
    return failure;
}
static BOOL setup_inf_collect(SETUP_OBJECT *inf, SETUP_OBJECT *queue, const char *section,
                              const char *source, DWORD flags, DWORD copy_flags)
{
    SETUP_OPERATION **tail = &queue->operations;
    unsigned operations = 0;
    const struct {const char *name; DWORD flag;} unsupported[] = {
        {"Include",0x3ff},{"Needs",0x3ff},{"AddReg",4},{"DelReg",4},{"BitReg",32},
        {"UpdateInis",2},{"UpdateIniFields",2},{"Ini2Reg",8},{"RegisterDlls",64},
        {"UnregisterDlls",128},{"ProfileItems",256},{"CopyINF",512},{"LogConfig",1}
    };
    for (unsigned i = 0; i < sizeof(unsupported)/sizeof(unsupported[0]); i++)
        if ((flags & unsupported[i].flag) && setup_inf_find(inf, section, unsupported[i].name)) {
            SetLastError(120); return FALSE;
        }
    if (!(flags & 16)) return TRUE;
    for (unsigned i = 0; i < inf->count; i++) {
        SETUP_LINE *directive = &inf->lines[i];
        if (!setup_inf_eq(directive->section, section) || !directive->key) continue;
        DWORD type;
        if (setup_inf_eq(directive->key, "CopyFiles")) type = 0;
        else if (setup_inf_eq(directive->key, "RenFiles")) type = 1;
        else if (setup_inf_eq(directive->key, "DelFiles")) type = 2;
        else continue;
        for (unsigned field_index = 0; field_index < INF_MAX_OPERATIONS; field_index++) {
            char field[INF_PATH], list[INF_PATH], target_directory[INF_PATH];
            if (!setup_inf_field(directive->value, field_index, field) || !setup_inf_expand(inf, field, list, 0)) return FALSE;
            if (!list[0]) break;
            BOOL single = list[0] == '@';
            if (!setup_inf_dir(inf, single ? NULL : list, target_directory)) return FALSE;
            BOOL found = FALSE;
            for (unsigned j = 0; j < (single ? 1 : inf->count); j++) {
                SETUP_LINE *row = &inf->lines[j];
                if (!single && !setup_inf_eq(row->section, list)) continue;
                found = TRUE;
                if (++operations > INF_MAX_OPERATIONS) { SetLastError(8); return FALSE; }
                SETUP_OPERATION *op = kmalloc(sizeof(*op));
                if (!op) { SetLastError(8); return FALSE; }
                memset(op, 0, sizeof(*op)); *tail = op; tail = &op->next;
                op->type = type; op->flags = type == 0 ? copy_flags : 0;
                const char *values = single ? list + 1 : row->value;
                char target[INF_PATH], input[INF_PATH]; DWORD file_flags = 0;
                if (!setup_inf_field(values, 0, field) || !setup_inf_expand(inf, field, target, 0) || !target[0] ||
                    !setup_inf_join(op->target, target_directory, target)) return FALSE;
                if (type == 2) continue;
                if (!setup_inf_field(values, 1, field) || !setup_inf_expand(inf, field, input, 0)) return FALSE;
                if (!input[0]) memcpy(input, target, strlen(target) + 1);
                if (type == 1) {
                    if (!setup_inf_join(op->source, target_directory, input)) return FALSE;
                    continue;
                }
                if (!setup_inf_field(values, 3, field) || (field[0] && !setup_inf_number(field, &file_flags))) return FALSE;
                if (file_flags & ~(1U|2U|4U|16U|32U|64U|0x400U|0x800U|0x2000U)) { SetLastError(120); return FALSE; }
                if (file_flags & 1) op->flags |= 0x4000;
                if (file_flags & 2) op->flags |= 0x400;
                if (file_flags & 4) op->flags &= ~(4U|0x2000U|0x10000U);
                if (file_flags & 16) op->flags |= 0x1000;
                if (file_flags & 32) op->flags |= 0x2000;
                if (file_flags & 64) op->flags |= 0x10000;
                if (file_flags & 0x400) op->flags |= 2;
                if (file_flags & 0x800) op->flags |= 0x10;
                SETUP_LINE *disk_file = setup_inf_find(inf, "SourceDisksFiles", input);
                if (!disk_file) { SetLastError(INF_LINE_MISSING); return FALSE; }
                if (!setup_inf_field(disk_file->value, 0, field)) return FALSE;
                SETUP_LINE *disk = setup_inf_find(inf, "SourceDisksNames", field);
                if (!disk) { SetLastError(INF_LINE_MISSING); return FALSE; }
                if (!setup_inf_field(disk->value, 0, field) || !setup_inf_expand(inf, field, op->description, 0) ||
                    !setup_inf_field(disk->value, 1, field) || !setup_inf_expand(inf, field, op->tag, 0)) return FALSE;
                char sub[INF_PATH];
                if (!setup_inf_field(disk->value, 3, field) || !setup_inf_expand(inf, field, sub, 0) ||
                    !setup_inf_join(op->directory, source ? source : inf->directory, sub) ||
                    !setup_inf_field(disk_file->value, 1, field) || !setup_inf_expand(inf, field, sub, 0) ||
                    !setup_inf_join(op->directory, op->directory, sub) ||
                    !setup_inf_join(op->source, op->directory, input)) return FALSE;
                memcpy(op->leaf, input, strlen(input) + 1);
            }
            if (!found) { SetLastError(0xe0000101U); return FALSE; }
        }
    }
    return TRUE;
}
static BOOL setup_inf_execute(SETUP_OBJECT *queue, PVOID callback, PVOID context)
{
    if (!queue->operations) { SetLastError(0); return TRUE; }
    SETUP_NOTIFY *notify = LocalAlloc(0x40, sizeof(*notify));
    if (!notify) { SetLastError(8); return FALSE; }
    DWORD failure = setup_inf_callback(callback, context, 1, 0, 0) ? 0 : INF_ABORTED;
    const DWORD order[3] = {2, 1, 0};
    for (unsigned i = 0; !failure && i < 3; i++) {
        DWORD type = order[i], count = 0;
        for (SETUP_OPERATION *p = queue->operations; p; p = p->next) if (p->type == type) count++;
        if (!count) continue;
        if (!setup_inf_callback(callback, context, 3, type, count)) { failure = INF_ABORTED; break; }
        for (SETUP_OPERATION *p = queue->operations; p; p = p->next) if (p->type == type) {
            failure = setup_inf_operation(p, notify, callback, context);
            if (failure) break;
        }
        if (!failure) setup_inf_callback(callback, context, 4, type, 0);
    }
    setup_inf_callback(callback, context, 2, failure == 0, 0);
    LocalFree(notify); SetLastError(failure); return failure == 0;
}
BOOL WINAPI shim_SetupInstallFromInfSectionA(HANDLE owner, HANDLE handle, PCSTR section,
    UINT flags, HANDLE root, PCSTR source, UINT copy_flags, PVOID callback,
    PVOID context, HANDLE devices, PVOID device)
{
    (void)owner; (void)root;
    if (!section || devices || device) { SetLastError(87); return FALSE; }
    SETUP_OBJECT *inf = setup_handle_ref(handle, 1);
    if (!inf) return FALSE;
    HANDLE queue_handle = shim_SetupOpenFileQueue();
    if (queue_handle == INVALID_HANDLE_VALUE) { setup_handle_unref(inf); return FALSE; }
    SETUP_OBJECT *queue = setup_handle_ref(queue_handle, 2);
    BOOL ok = queue && setup_inf_collect(inf, queue, section, source, flags, copy_flags) &&
              setup_inf_execute(queue, callback, context);
    DWORD last = GetLastError();
    if (queue) setup_handle_unref(queue);
    shim_SetupCloseFileQueue(queue_handle); setup_handle_unref(inf);
    SetLastError(last); return ok;
}
