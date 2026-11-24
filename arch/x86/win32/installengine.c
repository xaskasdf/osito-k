/* Active Setup CIF objects. Payloads remain external, owned by the installer.
 * Interface layout: inseng.idl; parsing/return conventions checked on Windows
 * with test/installengine_pe.c. Component/group/mode views have no IUnknown:
 * their lifetime is the containing ICifFile's, unlike the enumerators. */
#include "installengine.h"
#include "ole32_shim.h"
#include "kernel32_shim.h"
#include "compat32.h"
#include "urlmon_shim.h"
#include "authenticode.h"

extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
extern void serial_puts(const char *text);
extern void serial_puthex(uint64_t value, int digits);

#define IE_FAIL ((HRESULT)0x80004005U)
#define IE_POINTER ((HRESULT)0x80004003U)
#define IE_INVALID ((HRESULT)0x80070057U)
#define IE_NOMEM ((HRESULT)0x8007000eU)
#define IE_NOTIMPL ((HRESULT)0x80004001U)
#define IE_NOINTERFACE ((HRESULT)0x80004002U)
#define IE_BUSY ((HRESULT)0x8000000aU)
#define IE_ABORT ((HRESULT)0x80004004U)
#define IE_OBJECTS 1024
#define IE_SECTIONS 256
#define IE_LINES 4096
#define IE_TEXT 1536
enum { IE_ENGINE=1, IE_FILE, IE_ENUM, IE_COMPONENT, IE_GROUP, IE_MODE };
typedef struct ie_object IE_OBJECT;
typedef struct ie_job IE_JOB;
typedef struct { char *name; unsigned kind; DWORD action, priority, downloaded; } IE_NODE;
typedef struct { unsigned section; char *key, *value; } IE_LINE;
typedef struct {
    char *text;
    unsigned sections, lines;
    IE_NODE node[IE_SECTIONS];
    IE_LINE line[IE_LINES];
} IE_DOCUMENT;
typedef struct {
    BYTE object[8 + 37 * 8];
    BYTE timing[8 + 5 * 8];
    char text[IE_TEXT];
} IE_GUEST;
struct ie_object {
    DWORD owner, refs;
    unsigned kind, slot, index, filter, cursor;
    BOOL compat;
    IE_GUEST *guest;
    IE_DOCUMENT *doc;
    IE_OBJECT *file;
    IE_JOB *job;
    PVOID callback;
    volatile DWORD state;
    volatile LONG cancelled;
    volatile DWORD download_remaining_kb;
    DWORD options;
    char base[IE_TEXT], directory[260], group[260];
};
typedef struct { PVOID proc; PCSTR name; BYTE argc; } IE_METHOD;
static IE_OBJECT *ie_objects[IE_OBJECTS];
static const IE_METHOD ie_engine_methods[27], ie_file_methods[11];
static const IE_METHOD ie_enum_methods[5], ie_comp_methods[37];
static const IE_METHOD ie_group_methods[5], ie_mode_methods[4];
static const IE_METHOD ie_timing_methods[5];
static DWORD WINAPI ie_addref(PVOID object);
static DWORD WINAPI ie_release(PVOID object);
static DWORD WINAPI ie_worker(PVOID object);
typedef struct {
    WCHAR url[IE_TEXT], temporary[260];
    char id[260], description[IE_TEXT], filename[260];
    DWORD problem_action;
} IE_TRANSFER_GUEST;
struct ie_job {
    IE_OBJECT *engine;
    IE_TRANSFER_GUEST *guest;
    PVOID callback;
    DWORD flags, phase;
    DWORD remaining_kb, component_kb;
    HRESULT bind_result;
    BOOL bind_stopped;
};

static BOOL ie_caseeq(PCSTR a, PCSTR b)
{
    if (!a || !b) return FALSE;
    while (*a && *b) {
        BYTE x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return FALSE;
    }
    return !*a && !*b;
}
static char *ie_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    unsigned n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
    return s;
}
static BOOL ie_input(PCSTR source, char *out, unsigned capacity, BOOL compat)
{
    if (!source) return FALSE;
    for (unsigned i = 0; i < capacity; i++) {
        if (!win32_user_range_readable(source + i, 1, compat)) return FALSE;
        out[i] = source[i];
        if (!out[i]) return TRUE;
    }
    return FALSE;
}
/* One CSV field. A quoted version "1,2,3,4" is unquoted separately below. */
static BOOL ie_field(PCSTR source, unsigned field, char *out, unsigned capacity)
{
    unsigned index = 0, n = 0;
    BOOL quote = FALSE;
    if (!source || !capacity) return FALSE;
    while (*source) {
        char c = *source++;
        if (c == '"') {
            if (quote && *source == '"') source++;
            else { quote = !quote; continue; }
        } else if (c == ',' && !quote) {
            if (index == field) break;
            index++; continue;
        }
        if (index != field) continue;
        if (!n && !quote && (c == ' ' || c == '\t')) continue;
        if (n + 1 == capacity) return FALSE;
        out[n++] = c;
    }
    while (n && (out[n-1] == ' ' || out[n-1] == '\t')) n--;
    out[n] = 0;
    return index == field && !quote;
}
static DWORD ie_uint(PCSTR text)
{
    DWORD n = 0;
    if (text) while (*text >= '0' && *text <= '9') n = n * 10 + (BYTE)*text++ - '0';
    return n;
}
static BOOL ie_guid_equal(PCVOID pointer, DWORD first)
{
    static const BYTE tail[12] = {9,0xc5,0xcf,0x11,0xaa,0xfa,0,0xaa,0,0xb6,1,0x5c};
    DWORD value;
    memcpy(&value, pointer, 4);
    return value == first && !memcmp((const BYTE *)pointer + 4, tail, 12);
}
static IE_OBJECT *ie_find(PVOID guest)
{
    DWORD owner = GetCurrentProcessId();
    for (unsigned i = 0; i < IE_OBJECTS; i++) {
        IE_OBJECT *o = __atomic_load_n(&ie_objects[i], __ATOMIC_ACQUIRE);
        if (o && o->owner == owner &&
            ((PVOID)o->guest == guest ||
             (o->kind == IE_ENGINE && (PVOID)o->guest->timing == guest)))
            return o;
    }
    return NULL;
}
static BOOL ie_writeptr(PVOID out, PVOID value, BOOL compat)
{
    unsigned size = compat ? 4 : 8;
    if (!win32_user_range_writable(out, size, compat)) return FALSE;
    ULONG_PTR pointer = (ULONG_PTR)value;
    memcpy(out, &pointer, size);
    return TRUE;
}
static BOOL ie_write32(DWORD *out, DWORD value, BOOL compat)
{
    if (!win32_user_range_writable(out, 4, compat)) return FALSE;
    *out = value;
    return TRUE;
}
static HRESULT ie_text_out(PCSTR source, char *out, DWORD capacity, BOOL compat)
{
    if (!source) return IE_FAIL;
    if (!capacity) return S_OK;
    unsigned n = strlen(source);
    if (n >= capacity) n = capacity - 1;
    if (!win32_user_range_writable(out, n + 1, compat)) return IE_POINTER;
    memcpy(out, source, n); out[n] = 0;
    return S_OK;
}
static IE_OBJECT *ie_new(unsigned kind)
{
    IE_OBJECT *o = kmalloc(sizeof(*o));
    if (!o) return NULL;
    memset(o, 0, sizeof(*o));
    o->owner = GetCurrentProcessId(); o->compat = g_compat32_mode != 0;
    o->kind = kind; o->refs = 1;
    o->guest = LocalAlloc(0x40, sizeof(*o->guest));
    if (!o->guest) { kfree(o); return NULL; }
    const IE_METHOD *methods = kind == IE_ENGINE ? ie_engine_methods :
        kind == IE_FILE ? ie_file_methods : kind == IE_ENUM ? ie_enum_methods :
        kind == IE_GROUP ? ie_group_methods : kind == IE_MODE ? ie_mode_methods : ie_comp_methods;
    unsigned count = kind == IE_ENGINE ? 27 : kind == IE_FILE ? 11 :
        kind == IE_ENUM || kind == IE_GROUP ? 5 : kind == IE_MODE ? 4 : 37;
    unsigned step = o->compat ? 4 : 8;
    ULONG_PTR table = (ULONG_PTR)o->guest->object + 8;
    memcpy(o->guest->object, &table, step);
    for (unsigned i = 0; i < count; i++) {
        ULONG_PTR proc = (ULONG_PTR)methods[i].proc;
        if (o->compat) proc = compat32_make_thunk_ex(proc, methods[i].name, methods[i].argc, CC_STDCALL);
        if (!proc) { LocalFree(o->guest); kfree(o); return NULL; }
        memcpy(o->guest->object + 8 + i * step, &proc, step);
    }
    if (kind == IE_ENGINE) {
        table = (ULONG_PTR)o->guest->timing + 8;
        memcpy(o->guest->timing, &table, step);
        for (unsigned i = 0; i < 5; i++) {
            const IE_METHOD *method = &ie_timing_methods[i];
            ULONG_PTR proc = (ULONG_PTR)method->proc;
            if (o->compat) proc = compat32_make_thunk_ex(
                proc, method->name, method->argc, CC_STDCALL);
            if (!proc) { LocalFree(o->guest); kfree(o); return NULL; }
            memcpy(o->guest->timing + 8 + i * step, &proc, step);
        }
    }
    for (unsigned i = 0; i < IE_OBJECTS; i++) {
        o->slot = i;
        if (__sync_bool_compare_and_swap(&ie_objects[i], NULL, o)) return o;
    }
    LocalFree(o->guest); kfree(o); return NULL;
}
static HRESULT ie_call(IE_OBJECT *o, PVOID callback, unsigned slot, unsigned count,
                       ULONG_PTR a, ULONG_PTR b, ULONG_PTR c, ULONG_PTR d,
                       ULONG_PTR e, ULONG_PTR f)
{
    if (!callback) return S_OK;
    unsigned step = o->compat ? 4 : 8;
    ULONG_PTR table = 0, proc = 0;
    if (!win32_user_range_readable(callback, step, o->compat)) return IE_POINTER;
    memcpy(&table, callback, step);
    PCVOID entry = (PCVOID)(table + slot * step);
    if (!win32_user_range_readable(entry, step, o->compat)) return IE_POINTER;
    memcpy(&proc, entry, step);
    if (!win32_user_range_executable((PCVOID)proc, 1, o->compat)) return IE_POINTER;
    if (o->compat) {
        uint32_t args[7] = {(uint32_t)(ULONG_PTR)callback, a, b, c, d, e, f};
        uint32_t stack = compat32_current_user_stack_top();
        return stack ? compat32_callback_args_on_stack((uint32_t)proc, count, args, stack) :
            compat32_callback_args((uint32_t)proc, count, args);
    }
    typedef HRESULT (WINAPI *FN)(PVOID,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR);
    return ((FN)proc)(callback,a,b,c,d,e,f);
}
static void ie_destroy(IE_OBJECT *o)
{
    __atomic_store_n(&ie_objects[o->slot], NULL, __ATOMIC_RELEASE);
    if (o->kind == IE_FILE) {
        for (unsigned i = 0; i < IE_OBJECTS; i++) {
            IE_OBJECT *child = ie_objects[i];
            if (child && child->file == o && child->kind >= IE_COMPONENT) ie_destroy(child);
        }
        if (o->doc) { kfree(o->doc->text); kfree(o->doc); }
    }
    if (o->kind == IE_ENGINE) ie_call(o, o->callback, 2, 1, 0,0,0,0,0,0);
    if ((o->kind == IE_ENGINE || o->kind == IE_ENUM) && o->file) ie_release(o->file->guest);
    LocalFree(o->guest); kfree(o);
}
static DWORD WINAPI ie_addref(PVOID object)
{
    IE_OBJECT *o = ie_find(object);
    return o ? __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST) : 0;
}
static DWORD WINAPI ie_release(PVOID object)
{
    IE_OBJECT *o = ie_find(object);
    if (!o) return 0;
    DWORD refs = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
    if (!refs) ie_destroy(o);
    return refs;
}
static HRESULT WINAPI ie_query(PVOID object, PCVOID iid, PVOID out)
{
    static const BYTE unknown[16] = {0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    IE_OBJECT *o = ie_find(object);
    if (!o || !ie_writeptr(out, NULL, o->compat) ||
        !win32_user_range_readable(iid,16,o->compat)) return IE_POINTER;
    BOOL timing = o->kind == IE_ENGINE && ie_guid_equal(iid,0x6e449687);
    BOOL match = timing || !memcmp(iid,unknown,16) ||
        (o->kind == IE_ENGINE && (ie_guid_equal(iid,0x6e449684) || ie_guid_equal(iid,0x6e449689))) ||
        (o->kind == IE_FILE && ie_guid_equal(iid,0x6e449688));
    if (!match) return IE_NOINTERFACE;
    /* The two engine interfaces share lifetime and canonical IUnknown.
     * GetRates occupies slot 3 only on the separate timing view. */
    ie_addref(object);
    ie_writeptr(out,timing ? (PVOID)o->guest->timing : (PVOID)o->guest,o->compat);
    return S_OK;
}
static HRESULT WINAPI ie_timing_rates(PVOID object, DWORD *download,
                                      DWORD *install)
{
    IE_OBJECT *o = ie_find(object);
    if (!o || o->kind != IE_ENGINE ||
        !win32_user_range_writable(download,4,o->compat) ||
        !win32_user_range_writable(install,4,o->compat)) return IE_POINTER;
    /* Native measurements return no download-rate estimate for file/HTTP
     * transfers and retain the default installation estimate of 60000. */
    *download = 0;
    *install = 60000;
    return S_OK;
}
static HRESULT WINAPI ie_timing_progress(PVOID object, DWORD *progress)
{
    IE_OBJECT *o = ie_find(object);
    if (!o || o->kind != IE_ENGINE ||
        !win32_user_range_writable(progress,5*sizeof(DWORD),o->compat))
        return IE_POINTER;
    /* Windows preserves cbSize, including nonstandard values. No install
     * job is running here; unknown download duration is UINT32_MAX. */
    progress[1] = __atomic_load_n(&o->download_remaining_kb,__ATOMIC_ACQUIRE);
    progress[2] = 0;
    progress[3] = 0xffffffffU;
    progress[4] = 0;
    return S_OK;
}
static PCSTR ie_raw(IE_DOCUMENT *doc, unsigned section, PCSTR key)
{
    for (unsigned i = 0; i < doc->lines; i++)
        if (doc->line[i].section == section && ie_caseeq(doc->line[i].key,key)) return doc->line[i].value;
    return NULL;
}
static int ie_node_find(IE_DOCUMENT *doc, PCSTR id, unsigned kind)
{
    for (unsigned i = 0; i < doc->sections; i++)
        if ((!kind || doc->node[i].kind == kind) && ie_caseeq(doc->node[i].name,id)) return i;
    return -1;
}
static BOOL ie_expand(IE_DOCUMENT *doc, PCSTR source, char *out, unsigned capacity, unsigned depth)
{
    if (!source || depth > 8) return FALSE;
    int strings = ie_node_find(doc,"Strings",0);
    unsigned n = 0;
    while (*source) {
        if (*source == '%' && strings >= 0) {
            PCSTR end = source + 1;
            while (*end && *end != '%') end++;
            unsigned len = end - source - 1;
            if (*end && len < 256) {
                char key[256], value[IE_TEXT], expanded[IE_TEXT];
                memcpy(key,source+1,len); key[len] = 0;
                PCSTR raw = ie_raw(doc,strings,key);
                if (raw) {
                    if (!ie_field(raw,0,value,sizeof(value)) ||
                        !ie_expand(doc,value,expanded,sizeof(expanded),depth+1)) return FALSE;
                    unsigned count = strlen(expanded);
                    if (n + count >= capacity) return FALSE;
                    memcpy(out+n,expanded,count); n += count; source = end+1; continue;
                }
            }
        }
        if (n+1 == capacity) return FALSE;
        out[n++] = *source++;
    }
    out[n] = 0; return TRUE;
}
static BOOL ie_property(IE_OBJECT *o, PCSTR key, unsigned field, char *out, unsigned capacity)
{
    if (!o || !o->doc || o->index >= o->doc->sections) return FALSE;
    char value[IE_TEXT];
    return ie_field(ie_raw(o->doc,o->index,key),field,value,sizeof(value)) &&
        ie_expand(o->doc,value,out,capacity,0);
}
static DWORD ie_node_uint(IE_OBJECT *o, PCSTR key, unsigned field, DWORD fallback)
{
    char value[IE_TEXT];
    return ie_property(o,key,field,value,sizeof(value)) ? ie_uint(value) : fallback;
}
static IE_OBJECT *ie_load_document(PCSTR path)
{
    HANDLE file = CreateFileA(path,GENERIC_READ,7,NULL,3,0x80,NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    DWORD high = 0, size = GetFileSize(file,&high), got = 0;
    IE_OBJECT *o = NULL;
    IE_DOCUMENT *doc = NULL;
    if (high || !size || size > 1024*1024) goto done;
    doc = kmalloc(sizeof(*doc));
    if (!doc) goto done;
    memset(doc,0,sizeof(*doc)); doc->text = kmalloc((uint64_t)size+1);
    if (!doc->text || !ReadFile(file,doc->text,size,&got,NULL) || got != size) goto done;
    doc->text[size] = 0;
    /* Reject embedded NULs instead of silently parsing a truncated catalog. */
    for (DWORD i = 0; i < size; i++) if (!doc->text[i]) goto done;
    char *line = doc->text;
    if (size >= 3 && (BYTE)line[0] == 0xef && (BYTE)line[1] == 0xbb && (BYTE)line[2] == 0xbf) line += 3;
    int current = -1;
    while (*line) {
        char *next = line;
        while (*next && *next != '\n') next++;
        if (*next) *next++ = 0;
        char *s = ie_trim(line);
        if (*s == '[') {
            char *end = s+1;
            while (*end && *end != ']') end++;
            if (*end != ']') goto done;
            *end = 0; s = ie_trim(s+1);
            if (!*s || strlen(s) >= 260) goto done;
            current = ie_node_find(doc,s,0);
            if (current < 0) {
                if (doc->sections == IE_SECTIONS) goto done;
                current = doc->sections++; doc->node[current].name = s;
            }
        } else if (*s && *s != ';' && *s != '#' && current >= 0) {
            char *eq = s;
            while (*eq && *eq != '=') eq++;
            if (*eq == '=') {
                if (doc->lines == IE_LINES) goto done;
                *eq++ = 0;
                BOOL quoted = FALSE;
                for (char *p = eq; *p; p++) {
                    if (*p == '"') quoted = !quoted;
                    else if (*p == ';' && !quoted) { *p = 0; break; }
                }
                doc->line[doc->lines++] = (IE_LINE){current,ie_trim(s),ie_trim(eq)};
            }
        }
        line = next;
    }
    if (!doc->sections) goto done;
    for (unsigned i = 0; i < doc->sections; i++) {
        PCSTR type = ie_raw(doc,i,"SectionType"); char field[80];
        if (ie_caseeq(doc->node[i].name,"Version") || ie_caseeq(doc->node[i].name,"Strings")) continue;
        doc->node[i].kind = IE_COMPONENT;
        if (type && ie_field(type,0,field,sizeof(field))) {
            if (ie_caseeq(field,"Group")) doc->node[i].kind = IE_GROUP;
            else if (ie_caseeq(field,"Mode")) doc->node[i].kind = IE_MODE;
        }
    }
    o = ie_new(IE_FILE);
    if (o) { o->doc = doc; doc = NULL; }
done:
    CloseHandle(file);
    if (doc) { if (doc->text) kfree(doc->text); kfree(doc); }
    return o;
}
static IE_OBJECT *ie_node_object(IE_OBJECT *file, unsigned index)
{
    for (unsigned i = 0; i < IE_OBJECTS; i++) {
        IE_OBJECT *o = ie_objects[i];
        if (o && o->file == file && o->kind >= IE_COMPONENT && o->index == index) return o;
    }
    IE_OBJECT *o = ie_new(file->doc->node[index].kind);
    if (o) { o->file = file; o->doc = file->doc; o->index = index; }
    return o;
}
static HRESULT ie_file_enum(PVOID object, PVOID out, unsigned kind, PCSTR group)
{
    IE_OBJECT *file = ie_find(object);
    if (!file || !ie_writeptr(out,NULL,file->compat)) return IE_POINTER;
    IE_OBJECT *en = ie_new(IE_ENUM);
    if (!en) return IE_NOMEM;
    en->file = file; en->doc = file->doc; en->filter = kind;
    if (group) memcpy(en->group,group,strlen(group)+1);
    ie_addref(object); ie_writeptr(out,en->guest,file->compat); return S_OK;
}
static HRESULT WINAPI ie_file_enum_components(PVOID o, PVOID out, DWORD filter, PVOID pv)
{ (void)filter; (void)pv; return ie_file_enum(o,out,IE_COMPONENT,NULL); }
static HRESULT WINAPI ie_file_enum_groups(PVOID o, PVOID out, DWORD filter, PVOID pv)
{ (void)filter; (void)pv; return ie_file_enum(o,out,IE_GROUP,NULL); }
static HRESULT WINAPI ie_file_enum_modes(PVOID o, PVOID out, DWORD filter, PVOID pv)
{ (void)filter; (void)pv; return ie_file_enum(o,out,IE_MODE,NULL); }
static HRESULT ie_file_find(PVOID object, PCSTR id, PVOID out, unsigned kind)
{
    IE_OBJECT *o = ie_find(object); char key[260];
    if (!o || !ie_writeptr(out,NULL,o->compat)) return IE_POINTER;
    if (!ie_input(id,key,sizeof(key),o->compat)) return IE_INVALID;
    int index = ie_node_find(o->doc,key,kind);
    if (index < 0) return IE_FAIL;
    IE_OBJECT *node = ie_node_object(o,index);
    if (!node) return IE_NOMEM;
    ie_writeptr(out,node->guest,o->compat); return S_OK;
}
static HRESULT WINAPI ie_file_find_component(PVOID o, PCSTR id, PVOID out)
{ return ie_file_find(o,id,out,IE_COMPONENT); }
static HRESULT WINAPI ie_file_find_group(PVOID o, PCSTR id, PVOID out)
{ return ie_file_find(o,id,out,IE_GROUP); }
static HRESULT WINAPI ie_file_find_mode(PVOID o, PCSTR id, PVOID out)
{ return ie_file_find(o,id,out,IE_MODE); }
static HRESULT WINAPI ie_file_description(PVOID object, char *out, DWORD size)
{
    IE_OBJECT *o = ie_find(object); char text[IE_TEXT], value[IE_TEXT];
    if (!o) return IE_INVALID;
    int section = ie_node_find(o->doc,"Version",0);
    PCSTR raw = section < 0 ? NULL : ie_raw(o->doc,section,"DisplayName");
    if (!raw) return ie_text_out("Active Setup Installation",out,size,o->compat);
    if (!ie_field(raw,0,value,sizeof(value)) || !ie_expand(o->doc,value,text,sizeof(text),0)) return IE_FAIL;
    return ie_text_out(text,out,size,o->compat);
}
static HRESULT WINAPI ie_file_dlls(PVOID object, char *out, DWORD size)
{ (void)object; (void)out; (void)size; return IE_FAIL; }
static HRESULT WINAPI ie_enum_next(PVOID object, PVOID out)
{
    IE_OBJECT *o = ie_find(object);
    if (!o || !ie_writeptr(out,NULL,o->compat)) return IE_POINTER;
    while (o->cursor < o->doc->sections) {
        unsigned index = o->cursor++;
        if (o->doc->node[index].kind != o->filter) continue;
        if (o->group[0]) {
            char group[260];
            if (!ie_field(ie_raw(o->doc,index,"Group"),0,group,sizeof(group)) || !ie_caseeq(group,o->group)) continue;
        }
        IE_OBJECT *node = ie_node_object(o->file,index);
        if (!node) return IE_NOMEM;
        ie_writeptr(out,node->guest,o->compat); return S_OK;
    }
    return IE_FAIL;
}
static HRESULT WINAPI ie_enum_reset(PVOID object)
{ IE_OBJECT *o = ie_find(object); if (!o) return IE_INVALID; o->cursor = 0; return S_OK; }
static HRESULT WINAPI ie_comp_id(PVOID object, char *out, DWORD size)
{
    IE_OBJECT *o = ie_find(object);
    return o ? ie_text_out(o->doc->node[o->index].name,out,size,o->compat) : IE_INVALID;
}
#define IE_STRING(fn,key,default_value) \
static HRESULT WINAPI fn(PVOID object, char *out, DWORD size) \
{ IE_OBJECT *o = ie_find(object); char value[IE_TEXT]; \
  if (!o) return IE_INVALID; \
  return ie_text_out(ie_property(o,key,0,value,sizeof(value)) ? value : default_value,out,size,o->compat); }
IE_STRING(ie_comp_guid,"GUID","")
IE_STRING(ie_comp_description,"DisplayName","")
IE_STRING(ie_comp_details,"Details","")
IE_STRING(ie_comp_locale,"Locale","en")
IE_STRING(ie_comp_uninstall,"UninstallKey",NULL)
IE_STRING(ie_comp_success,"SuccessKey",NULL)
IE_STRING(ie_comp_group,"Group","")
IE_STRING(ie_comp_patch,"PatchID","")
static HRESULT WINAPI ie_comp_url(PVOID object, UINT index, char *out, DWORD size, DWORD *flags)
{
    IE_OBJECT *o = ie_find(object); char key[16] = "URL", digits[10], value[IE_TEXT];
    if (!o || index == 0xffffffffU) return IE_INVALID;
    unsigned n = 0, number = index+1;
    do { digits[n++] = '0' + number % 10; number /= 10; } while (number);
    for (unsigned i = 0; i < n; i++) key[3+i] = digits[n-i-1]; key[3+n] = 0;
    if (!ie_property(o,key,0,value,sizeof(value))) return IE_FAIL;
    if (flags && !ie_write32(flags,ie_node_uint(o,key,1,0),o->compat)) return IE_POINTER;
    return ie_text_out(value,out,size,o->compat);
}
static HRESULT WINAPI ie_comp_extract_list(PVOID o, UINT i, char *s, DWORD n)
{ (void)o;(void)i;(void)s;(void)n;return IE_NOTIMPL; }
static HRESULT WINAPI ie_comp_url_range(PVOID o, UINT i, DWORD *a, DWORD *b)
{ (void)o;(void)i;(void)a;(void)b;return IE_NOTIMPL; }
static HRESULT WINAPI ie_comp_command(PVOID o, UINT i, char *s, DWORD n, char *t, DWORD m, DWORD *type)
{ (void)o;(void)i;(void)s;(void)n;(void)t;(void)m;(void)type;return IE_NOTIMPL; }
static void ie_version(IE_OBJECT *o, DWORD *version, DWORD *build)
{
    PCSTR raw = ie_raw(o->doc,o->index,"Version");
    char value[IE_TEXT], part[32]; DWORD v[4] = {0,0,0,0};
    if (raw) {
        if (*raw == '"') { if (!ie_field(raw,0,value,sizeof(value))) return; raw = value; }
        for (unsigned i = 0; i < 4; i++) if (ie_field(raw,i,part,sizeof(part))) v[i] = ie_uint(part) & 0xffff;
    }
    *version = (v[0] << 16) | v[1]; *build = (v[2] << 16) | v[3];
}
static HRESULT WINAPI ie_comp_version(PVOID object, DWORD *version, DWORD *build)
{
    IE_OBJECT *o = ie_find(object); DWORD a = 0, b = 0;
    if (!o) return IE_INVALID;
    ie_version(o,&a,&b);
    return ie_write32(version,a,o->compat) && ie_write32(build,b,o->compat) ? S_OK : IE_POINTER;
}
static HRESULT WINAPI ie_comp_sizes(PVOID object, DWORD *win, DWORD *app)
{
    IE_OBJECT *o = ie_find(object);
    if (!o) return IE_INVALID;
    return ie_write32(win,0,o->compat) && ie_write32(app,ie_node_uint(o,"Size",0,0)*2,o->compat) ? S_OK : IE_POINTER;
}
#define IE_NUMBER(fn,key,field,fallback) \
static DWORD WINAPI fn(PVOID object) \
{ IE_OBJECT *o = ie_find(object); return o ? ie_node_uint(o,key,field,fallback) : fallback; }
IE_NUMBER(ie_comp_download_size,"Size",0,0)
IE_NUMBER(ie_comp_extract_size,"Size",1,0)
IE_NUMBER(ie_comp_priority,"Priority",0,0)
IE_NUMBER(ie_comp_actual,"Size",0,0)
#define IE_BOOLEAN(fn,key,fallback) \
static HRESULT WINAPI fn(PVOID object) \
{ IE_OBJECT *o = ie_find(object); return o ? (ie_node_uint(o,key,0,fallback) ? S_OK : 1) : IE_INVALID; }
IE_BOOLEAN(ie_comp_active,"ActiveSetupAware",0)
IE_BOOLEAN(ie_comp_reboot,"Reboot",0)
IE_BOOLEAN(ie_comp_admin,"AdminCheck",0)
IE_BOOLEAN(ie_comp_visible,"UIVisible",1)
static DWORD WINAPI ie_comp_platform(PVOID object)
{
    static const PCSTR names[] = {"Win95","Win98","NT4","NT5","NT4Alpha","NT5Alpha","Millen"};
    IE_OBJECT *o = ie_find(object); char value[80]; DWORD flags = 0;
    if (!o) return 0;
    for (unsigned i = 0; i < 32 && ie_property(o,"Platform",i,value,sizeof(value)); i++)
        for (unsigned n = 0; n < 7; n++) if (ie_caseeq(value,names[n])) flags |= 1U << n;
    return flags;
}
static HRESULT WINAPI ie_comp_dependency(PVOID o, UINT i, char *s, DWORD n, char *t, DWORD *v, DWORD *b)
{ (void)o;(void)i;(void)s;(void)n;(void)t;(void)v;(void)b;return IE_NOTIMPL; }
static HRESULT WINAPI ie_comp_mode(PVOID o, UINT i, char *s, DWORD n)
{ (void)o;(void)i;(void)s;(void)n;return IE_NOTIMPL; }
static HRESULT WINAPI ie_comp_detect(PVOID o, char *s, DWORD n, char *t, DWORD m)
{ (void)o;(void)s;(void)n;(void)t;(void)m;return IE_NOTIMPL; }
static HRESULT WINAPI ie_comp_progress(PVOID o, char *s, DWORD n, char *t, DWORD m)
{ (void)o;(void)s;(void)n;(void)t;(void)m;return IE_NOTIMPL; }
static HRESULT WINAPI ie_comp_treat(PVOID o, UINT i, char *s, DWORD n)
{ (void)o;(void)i;(void)s;(void)n;return IE_NOTIMPL; }
static HRESULT WINAPI ie_comp_custom(PVOID object, PCSTR key, char *out, DWORD size)
{
    IE_OBJECT *o = ie_find(object); char name[260], value[IE_TEXT];
    if (!o || !ie_input(key,name,sizeof(name),o->compat)) return IE_INVALID;
    return ie_property(o,name,0,value,sizeof(value)) ? ie_text_out(value,out,size,o->compat) : IE_FAIL;
}
static DWORD WINAPI ie_comp_installed(PVOID object)
{ (void)object; return 0; }
static HRESULT WINAPI ie_comp_downloaded(PVOID object)
{ IE_OBJECT *o = ie_find(object); return o && o->doc->node[o->index].downloaded ? S_OK : 1; }
static DWORD WINAPI ie_comp_version_installed(PVOID o, DWORD v, DWORD b, DWORD *rv, DWORD *rb)
{ (void)o;(void)v;(void)b;(void)rv;(void)rb;return 0; }
static DWORD WINAPI ie_comp_queue(PVOID object)
{ IE_OBJECT *o = ie_find(object); return o ? o->doc->node[o->index].action : 0; }
static HRESULT WINAPI ie_comp_set_queue(PVOID object, DWORD state)
{ IE_OBJECT *o = ie_find(object); if (!o) return IE_INVALID; o->doc->node[o->index].action = state; return S_OK; }
static DWORD WINAPI ie_comp_current_priority(PVOID object)
{
    IE_OBJECT *o = ie_find(object); char group[260];
    if (!o) return 0;
    IE_NODE *node = &o->doc->node[o->index];
    if (node->priority) return node->priority;
    DWORD priority = ie_node_uint(o,"Priority",0,0);
    if (ie_property(o,"Group",0,group,sizeof(group))) {
        int index = ie_node_find(o->doc,group,IE_GROUP);
        if (index >= 0) priority |= ie_uint(ie_raw(o->doc,index,"Priority")) << 20;
    }
    return priority;
}
static HRESULT WINAPI ie_comp_set_priority(PVOID object, DWORD priority)
{ IE_OBJECT *o = ie_find(object); if (!o) return IE_INVALID; o->doc->node[o->index].priority = priority; return S_OK; }
static HRESULT WINAPI ie_group_enum(PVOID object, PVOID out, DWORD filter, PVOID pv)
{
    (void)filter; (void)pv; IE_OBJECT *o = ie_find(object);
    if (!o) return IE_INVALID;
    if (o->kind == IE_MODE) return IE_NOTIMPL;
    return ie_file_enum(o->file->guest,out,IE_COMPONENT,o->doc->node[o->index].name);
}
static void ie_trace(PCSTR method, PCSTR text, DWORD value)
{
    serial_puts("[INSENG] "); serial_puts(method);
    if (text) { serial_puts(" "); serial_puts(text); }
    serial_puts(" value="); serial_puthex(value,8); serial_puts("\n");
}
static HRESULT WINAPI ie_engine_status(PVOID object, DWORD *status)
{ IE_OBJECT *o = ie_find(object); return o && ie_write32(status,o->state,o->compat) ? S_OK : IE_POINTER; }
static HRESULT WINAPI ie_engine_local(PVOID object, PCSTR path)
{
    IE_OBJECT *o = ie_find(object); char name[260];
    if (!o || !ie_input(path,name,sizeof(name),o->compat)) return IE_INVALID;
    if (o->state == 2) return IE_BUSY;
    IE_OBJECT *file = ie_load_document(name);
    ie_trace("SetLocalCif",name,file ? S_OK : IE_FAIL);
    if (!file) return IE_FAIL;
    IE_OBJECT *old = o->file; o->file = file; o->state = 3;
    if (old) ie_release(old->guest);
    return S_OK;
}
static HRESULT WINAPI ie_engine_set_cif(PVOID object, PCSTR cab, PCSTR cif)
{ (void)object;(void)cab;(void)cif;ie_trace("SetCifFile",NULL,IE_NOTIMPL);return IE_NOTIMPL; }
static HRESULT WINAPI ie_engine_download(PVOID object, DWORD flags)
{
    IE_OBJECT *o = ie_find(object);
    if (!o || !o->file || !o->directory[0]) return IE_INVALID;
    ie_trace("DownloadComponents",NULL,flags);
    if (!__sync_bool_compare_and_swap(&o->state,3,2)) return IE_BUSY;
    __atomic_store_n(&o->cancelled,0,__ATOMIC_RELEASE);
    IE_JOB *job = kmalloc(sizeof(*job));
    if (!job) { o->state = 3; return IE_NOMEM; }
    memset(job,0,sizeof(*job)); job->engine = o; job->flags = flags;
    job->guest = LocalAlloc(0x40,sizeof(*job->guest));
    if (!job->guest) {
        kfree(job); o->state = 3; return IE_NOMEM;
    }
    /* Apartment-owned configuration is snapshotted before starting the worker.
     * Its callback reference remains valid if the application unregisters. */
    job->callback = o->callback;
    ie_call(o,job->callback,1,1,0,0,0,0,0,0);
    ie_addref(object); o->job = job;
    LPTHREAD_START_ROUTINE worker = ie_worker;
    if (o->compat) worker = (LPTHREAD_START_ROUTINE)(ULONG_PTR)
        compat32_make_thunk_ex((ULONG_PTR)ie_worker,"InstallEngine::worker",1,CC_STDCALL);
    ie_call(o,job->callback,3,3,2,0,0,0,0,0);
    HANDLE thread = worker ? CreateThread(NULL,0,worker,object,0,NULL) : NULL;
    if (!thread) {
        o->job = NULL; o->state = 3;
        ie_call(o,job->callback,3,3,3,0,0,0,0,0);
        ie_call(o,job->callback,2,1,0,0,0,0,0,0);
        LocalFree(job->guest); kfree(job);
        ie_release(object); return IE_NOMEM;
    }
    CloseHandle(thread); return S_OK;
}
static HRESULT WINAPI ie_engine_install(PVOID object, DWORD flags)
{ (void)object;ie_trace("InstallComponents",NULL,flags);return IE_NOTIMPL; }
static HRESULT WINAPI ie_engine_enum_install(PVOID object, UINT index, PVOID out)
{
    IE_OBJECT *o = ie_find(object);
    if (!o || !ie_writeptr(out,NULL,o->compat)) return IE_POINTER;
    if (!o->file) return IE_FAIL;
    for (unsigned i = 0; i < o->file->doc->sections; i++) {
        IE_NODE *node = &o->file->doc->node[i];
        if (node->kind != IE_COMPONENT || !node->action) continue;
        if (index--) continue;
        IE_OBJECT *view = ie_node_object(o->file,i);
        if (!view) return IE_NOMEM;
        memcpy(view->guest->text,node->name,strlen(node->name)+1);
        ie_writeptr(out,view->guest->text,o->compat); return S_OK;
    }
    return IE_FAIL;
}
static HRESULT WINAPI ie_engine_enum_download(PVOID object, UINT index, PVOID out)
{ return ie_engine_enum_install(object,index,out); }
static HRESULT WINAPI ie_engine_is_installed(PVOID object, PCSTR id, DWORD *status)
{
    IE_OBJECT *o = ie_find(object); char key[260];
    if (!o || !ie_input(id,key,sizeof(key),o->compat)) return IE_INVALID;
    if (!o->file || ie_node_find(o->file->doc,key,IE_COMPONENT) < 0) return IE_INVALID;
    return ie_write32(status,0,o->compat) ? S_OK : IE_POINTER;
}
static HRESULT WINAPI ie_engine_register(PVOID object, PVOID callback)
{
    IE_OBJECT *o = ie_find(object);
    if (!o || !callback) return IE_INVALID;
    HRESULT result = ie_call(o,callback,1,1,0,0,0,0,0,0);
    if (result < 0) return result;
    PVOID old = o->callback; o->callback = callback;
    ie_call(o,old,2,1,0,0,0,0,0,0); return S_OK;
}
static HRESULT WINAPI ie_engine_unregister(PVOID object)
{
    IE_OBJECT *o = ie_find(object); if (!o) return IE_INVALID;
    PVOID old = o->callback; o->callback = NULL;
    ie_call(o,old,2,1,0,0,0,0,0,0); return S_OK;
}
static HRESULT WINAPI ie_engine_action(PVOID object, PCSTR id, DWORD action, DWORD priority)
{
    IE_OBJECT *o = ie_find(object); char key[260];
    if (!o || !ie_input(id,key,sizeof(key),o->compat) || !o->file) return IE_INVALID;
    ie_trace("SetAction request",key,action);
    if (o->state == 2) return IE_BUSY;
    int index = ie_node_find(o->file->doc,key,IE_COMPONENT);
    if (index < 0 || action > 1) return IE_INVALID;
    o->file->doc->node[index].action = action;
    o->file->doc->node[index].priority = priority;
    return S_OK;
}
static HRESULT WINAPI ie_engine_sizes(PVOID object, PCSTR id, DWORD *sizes)
{
    IE_OBJECT *o = ie_find(object); char key[260];
    if (!o || !o->file || !ie_input(id,key,sizeof(key),o->compat)) return IE_INVALID;
    int index = ie_node_find(o->file->doc,key,IE_COMPONENT);
    if (index < 0) return IE_INVALID;
    if (!win32_user_range_writable(sizes,32,o->compat)) return IE_POINTER;
    DWORD download = ie_uint(ie_raw(o->file->doc,index,"Size"));
    DWORD result[8] = {32,download*2,0,download,0,0,0,0};
    memcpy(sizes,result,sizeof(result)); return S_OK;
}
static HRESULT WINAPI ie_engine_launch(PVOID o, PCSTR inf, PCSTR section)
{ (void)o;(void)inf;(void)section;return IE_NOTIMPL; }
static HRESULT WINAPI ie_engine_display(PVOID o, PCSTR id, PCSTR name)
{ (void)o;(void)id;(void)name;return IE_NOTIMPL; }
static HRESULT WINAPI ie_engine_base(PVOID object, PCSTR base)
{
    IE_OBJECT *o = ie_find(object); char value[IE_TEXT];
    if (!o || !ie_input(base,value,sizeof(value),o->compat)) return IE_INVALID;
    if (o->state == 2) return IE_BUSY;
    memcpy(o->base,value,strlen(value)+1); ie_trace("SetBaseUrl",value,0); return S_OK;
}
static HRESULT WINAPI ie_engine_directory(PVOID object, PCSTR path)
{
    IE_OBJECT *o = ie_find(object); char value[260];
    if (!o || !ie_input(path,value,sizeof(value),o->compat)) return IE_INVALID;
    if (o->state == 2) return IE_BUSY;
    DWORD attributes = GetFileAttributesA(value);
    if (attributes == 0xffffffffU) {
        if (!CreateDirectoryA(value,NULL)) return IE_FAIL;
    } else if (!(attributes & 0x10)) return IE_FAIL;
    memcpy(o->directory,value,strlen(value)+1); ie_trace("SetDownloadDir",value,0); return S_OK;
}
static HRESULT WINAPI ie_engine_drive(PVOID o, BYTE drive)
{ (void)o;(void)drive;return IE_NOTIMPL; }
static HRESULT WINAPI ie_engine_options(PVOID object, DWORD options)
{ IE_OBJECT *o = ie_find(object); if (!o) return IE_INVALID; o->options = options; return S_OK; }
static HRESULT WINAPI ie_engine_hwnd(PVOID object, HANDLE hwnd)
{ (void)object;(void)hwnd;return S_OK; }
static HRESULT WINAPI ie_engine_stream(PVOID object, PVOID stream)
{ (void)object;(void)stream;return IE_NOTIMPL; }
static HRESULT WINAPI ie_engine_abort(PVOID object, DWORD flags)
{
    (void)flags; IE_OBJECT *o = ie_find(object);
    if (!o) return IE_INVALID;
    if (o->state == 2) __atomic_store_n(&o->cancelled,IE_ABORT,__ATOMIC_RELEASE);
    return S_OK;
}
static HRESULT WINAPI ie_engine_suspend(PVOID object)
{ (void)object;return IE_NOTIMPL; }
static HRESULT WINAPI ie_engine_resume(PVOID object)
{ (void)object;return IE_NOTIMPL; }
static HRESULT WINAPI ie_engine_cif(PVOID object, PVOID out)
{
    IE_OBJECT *o = ie_find(object);
    if (!o || !ie_writeptr(out,o->file ? o->file->guest : NULL,o->compat)) return IE_POINTER;
    if (o->file) ie_addref(o->file->guest);
    return S_OK;
}
static HRESULT ie_notify(IE_JOB *job, DWORD phase, DWORD current, DWORD maximum)
{
    if (job->engine->cancelled) return job->engine->cancelled;
    job->phase = phase;
    if (phase == 2) {
        /* URLMON reports bytes; InstallEngine callbacks report whole KiB. */
        current /= 1024;
        maximum /= 1024;
        DWORD credited = current < job->component_kb ? current : job->component_kb;
        DWORD remaining = job->remaining_kb > credited
            ? job->remaining_kb - credited : 0;
        __atomic_store_n(&job->engine->download_remaining_kb,remaining,
                         __ATOMIC_RELEASE);
    }
    HRESULT rc = ie_call(job->engine,job->callback,6,7,
        (ULONG_PTR)job->guest->id,phase,(ULONG_PTR)job->guest->description,
        (ULONG_PTR)job->guest->filename,current,maximum);
    if (rc < 0) __atomic_store_n(&job->engine->cancelled,rc,__ATOMIC_RELEASE);
    return job->engine->cancelled;
}
static LONG ie_urlmon_notify(PVOID context, unsigned slot, ULONG_PTR a,
                             ULONG_PTR b, ULONG_PTR c, ULONG_PTR d)
{
    (void)c; (void)d; IE_JOB *job = context;
    if (slot == 3) return job->engine->cancelled;
    if (slot == 6) return ie_notify(job,2,(DWORD)a,(DWORD)b);
    if (slot == 7) { job->bind_result = (HRESULT)a; job->bind_stopped = TRUE; }
    return S_OK;
}
static BOOL ie_join(char *out, unsigned capacity, PCSTR directory, PCSTR leaf, char separator)
{
    unsigned n = strlen(directory), m = strlen(leaf);
    BOOL append = n && directory[n-1] != '/' && directory[n-1] != '\\';
    if (n+m+append >= capacity) return FALSE;
    memcpy(out,directory,n);
    if (append) out[n++] = separator;
    memcpy(out+n,leaf,m+1); return TRUE;
}
static HRESULT ie_download_one(IE_JOB *job, IE_OBJECT *component, unsigned url_index)
{
    IE_OBJECT *o = job->engine;
    char url[IE_TEXT], relative[IE_TEXT], key[16] = "URL", digits[10];
    unsigned count = 0, number = url_index+1;
    do { digits[count++] = '0' + number%10; number /= 10; } while (number);
    for (unsigned i = 0; i < count; i++) key[3+i] = digits[count-i-1]; key[3+count] = 0;
    if (!ie_property(component,key,0,relative,sizeof(relative))) return IE_FAIL;
    DWORD flags = ie_node_uint(component,key,1,0);
    if (flags & 2) {
        if (!o->base[0] || !ie_join(url,sizeof(url),o->base,relative,'/')) return IE_INVALID;
    } else memcpy(url,relative,strlen(relative)+1);
    PCSTR leaf = url;
    for (PCSTR p = url; *p && *p != '?' && *p != '#'; p++) if (*p == '/' || *p == '\\') leaf = p+1;
    unsigned length = 0;
    while (leaf[length] && leaf[length] != '?' && leaf[length] != '#') {
        BYTE c = leaf[length];
        if (length == sizeof(job->guest->filename)-1 || c < 0x20 || c == ':' || c == '\\') return IE_INVALID;
        job->guest->filename[length++] = c;
    }
    job->guest->filename[length] = 0;
    if (!length || ie_caseeq(job->guest->filename,".") || ie_caseeq(job->guest->filename,"..")) return IE_INVALID;
    char final[260], temporary[260];
    if (!ie_join(final,sizeof(final),o->directory,job->guest->filename,'\\')) return IE_INVALID;
    /* Reserve a unique destination before URLMON's atomic write. Trust failure
     * or cancellation cannot overwrite a previously installed/cached file. */
    static DWORD temporary_id;
    HANDLE reserved = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 64; attempt++) {
        DWORD id = __atomic_add_fetch(&temporary_id,1,__ATOMIC_SEQ_CST);
        char name[32] = ".osito-inseng-00000000.tmp";
        for (unsigned i = 0; i < 8; i++) name[13+i] = "0123456789abcdef"[(id >> ((7-i)*4)) & 15];
        if (!ie_join(temporary,sizeof(temporary),o->directory,name,'\\')) return IE_INVALID;
        reserved = CreateFileA(temporary,GENERIC_WRITE,0,NULL,1,0x80,NULL);
        if (reserved != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != 80 && GetLastError() != 183) return IE_FAIL;
    }
    if (reserved == INVALID_HANDLE_VALUE) return IE_FAIL;
    CloseHandle(reserved);
    for (unsigned i = 0; i <= strlen(url); i++) job->guest->url[i] = (BYTE)url[i];
    for (unsigned i = 0; i <= strlen(temporary); i++) job->guest->temporary[i] = (BYTE)temporary[i];
    job->bind_stopped = FALSE; job->bind_result = IE_FAIL;
    HRESULT rc = ie_notify(job,2,0,0);
    if (rc >= 0) rc = urlmon_download_to_file_internal(job->guest->url,
        job->guest->temporary,ie_urlmon_notify,job);
    if (job->bind_stopped && job->bind_result < 0) rc = job->bind_result;
    if (o->cancelled) rc = o->cancelled;
    if (rc >= 0) rc = ie_notify(job,5,0,0);
    if (rc >= 0 && !(job->flags & 16)) {
        HANDLE file = CreateFileW(job->guest->temporary,GENERIC_READ,7,NULL,3,0x80,NULL);
        BYTE magic[2]; DWORD got = 0;
        authenticode_result_t trust = AUTHENTICODE_VERIFY_IO_ERROR;
        if (file != INVALID_HANDLE_VALUE) {
            if (ReadFile(file,magic,2,&got,NULL))
                trust = got == 2 && magic[0] == 'M' && magic[1] == 'Z' ?
                    authenticode_verify_file(job->guest->temporary,NULL,0,0) : AUTHENTICODE_VERIFY_UNSUPPORTED;
            CloseHandle(file);
        }
        rc = trust == AUTHENTICODE_VERIFY_OK ? S_OK : trust == AUTHENTICODE_VERIFY_BAD_DIGEST ?
            (HRESULT)0x80096010U : trust == AUTHENTICODE_VERIFY_IO_ERROR ?
            (HRESULT)0x80092003U : (HRESULT)0x800b0003U;
    }
    if (o->cancelled) rc = o->cancelled;
    if (rc >= 0 && !MoveFileExA_k32(temporary,final,1)) rc = IE_FAIL;
    if (rc < 0) DeleteFileA(temporary);
    ie_trace("DownloadResult",url,rc);
    return rc < 0 ? rc : S_OK;
}
static DWORD WINAPI ie_worker(PVOID object)
{
    IE_OBJECT *o = ie_find(object);
    if (!o || !o->job) return 1;
    IE_JOB *job = o->job;
    DWORD total = 0; HRESULT result = S_OK;
    IE_DOCUMENT *doc = o->file->doc;
    for (unsigned i = 0; i < doc->sections; i++)
        if (doc->node[i].kind == IE_COMPONENT && doc->node[i].action)
            total += ie_uint(ie_raw(doc,i,"Size"));
    job->remaining_kb = total;
    __atomic_store_n(&o->download_remaining_kb,total,__ATOMIC_RELEASE);
    ie_call(o,job->callback,4,3,total,0,0,0,0,0);
    for (unsigned i = 0; i < doc->sections && result >= 0; i++) {
        IE_NODE *node = &doc->node[i];
        if (node->kind != IE_COMPONENT || !node->action) continue;
        IE_OBJECT *component = ie_node_object(o->file,i);
        if (!component) { result = IE_NOMEM; break; }
        memcpy(job->guest->id,node->name,strlen(node->name)+1);
        job->guest->description[0] = 0; job->guest->filename[0] = 0;
        ie_property(component,"DisplayName",0,job->guest->description,sizeof(job->guest->description));
        DWORD size = ie_node_uint(component,"Size",0,0);
        job->component_kb = size;
        ie_call(o,job->callback,5,5,(ULONG_PTR)job->guest->id,size,0,(ULONG_PTR)job->guest->description,0,0);
        result = ie_notify(job,0,0,0);
        if (result >= 0) {
            /* DownloadComponents fetches the cabinet even for URLF_EXTRACT;
             * extraction belongs to InstallComponents/the caller. */
            result = ie_download_one(job,component,0);
        }
        if (result >= 0) {
            node->downloaded = 1; job->phase = 9;
            job->remaining_kb = job->remaining_kb > size
                ? job->remaining_kb - size : 0;
            __atomic_store_n(&o->download_remaining_kb,job->remaining_kb,
                             __ATOMIC_RELEASE);
        }
        else if (result != IE_ABORT) {
            job->guest->problem_action = 0;
            ie_call(o,job->callback,9,3,1,(ULONG_PTR)&job->guest->problem_action,0,0,0,0);
        }
        ie_call(o,job->callback,7,6,(ULONG_PTR)job->guest->id,(DWORD)result,
            job->phase,(ULONG_PTR)job->guest->description,0,0);
    }
    if (o->cancelled) result = o->cancelled;
    ie_call(o,job->callback,8,4,(DWORD)result,0,0,0,0,0);
    o->job = NULL; __atomic_store_n(&o->state,3,__ATOMIC_RELEASE);
    ie_call(o,job->callback,3,3,3,0,0,0,0,0);
    ie_call(o,job->callback,2,1,0,0,0,0,0,0);
    LocalFree(job->guest); kfree(job);
    ie_release(object); return 0;
}
#define M(fn,argc) {(PVOID)fn,#fn,argc}
static const IE_METHOD ie_timing_methods[5] = {
    M(ie_query,3),M(ie_addref,1),M(ie_release,1),
    M(ie_timing_rates,3),M(ie_timing_progress,2)
};
static const IE_METHOD ie_engine_methods[27] = {
    M(ie_query,3),M(ie_addref,1),M(ie_release,1),M(ie_engine_status,2),M(ie_engine_set_cif,3),
    M(ie_engine_download,2),M(ie_engine_install,2),M(ie_engine_enum_install,3),M(ie_engine_enum_download,3),
    M(ie_engine_is_installed,3),M(ie_engine_register,2),M(ie_engine_unregister,1),M(ie_engine_action,4),
    M(ie_engine_sizes,3),M(ie_engine_launch,3),M(ie_engine_display,3),M(ie_engine_base,2),M(ie_engine_directory,2),
    M(ie_engine_drive,2),M(ie_engine_options,2),M(ie_engine_hwnd,2),M(ie_engine_stream,2),M(ie_engine_abort,2),
    M(ie_engine_suspend,1),M(ie_engine_resume,1),M(ie_engine_local,2),M(ie_engine_cif,2)
};
static const IE_METHOD ie_file_methods[11] = {
    M(ie_query,3),M(ie_addref,1),M(ie_release,1),M(ie_file_enum_components,4),M(ie_file_find_component,3),
    M(ie_file_enum_groups,4),M(ie_file_find_group,3),M(ie_file_enum_modes,4),M(ie_file_find_mode,3),
    M(ie_file_description,3),M(ie_file_dlls,3)
};
static const IE_METHOD ie_enum_methods[5] = {M(ie_query,3),M(ie_addref,1),M(ie_release,1),M(ie_enum_next,2),M(ie_enum_reset,1)};
static const IE_METHOD ie_comp_methods[37] = {
    M(ie_comp_id,3),M(ie_comp_guid,3),M(ie_comp_description,3),M(ie_comp_details,3),M(ie_comp_url,5),
    M(ie_comp_extract_list,4),M(ie_comp_url_range,4),M(ie_comp_command,7),M(ie_comp_version,3),M(ie_comp_locale,3),
    M(ie_comp_uninstall,3),M(ie_comp_sizes,3),M(ie_comp_download_size,1),M(ie_comp_extract_size,1),M(ie_comp_success,3),
    M(ie_comp_progress,5),M(ie_comp_active,1),M(ie_comp_reboot,1),M(ie_comp_admin,1),M(ie_comp_priority,1),
    M(ie_comp_dependency,7),M(ie_comp_platform,1),M(ie_comp_mode,4),M(ie_comp_group,3),M(ie_comp_visible,1),
    M(ie_comp_patch,3),M(ie_comp_detect,5),M(ie_comp_treat,4),M(ie_comp_custom,4),M(ie_comp_installed,1),
    M(ie_comp_downloaded,1),M(ie_comp_version_installed,5),M(ie_comp_queue,1),M(ie_comp_set_queue,2),
    M(ie_comp_actual,1),M(ie_comp_current_priority,1),M(ie_comp_set_priority,2)
};
static const IE_METHOD ie_group_methods[5] = {
    M(ie_comp_id,3),M(ie_comp_description,3),M(ie_comp_priority,1),M(ie_group_enum,4),M(ie_comp_current_priority,1)
};
static const IE_METHOD ie_mode_methods[4] = {
    M(ie_comp_id,3),M(ie_comp_description,3),M(ie_comp_details,3),M(ie_group_enum,4)
};
static HRESULT ie_activate(LPCGUID iid, PVOID outer, PVOID out)
{
    BOOL compat = g_compat32_mode != 0;
    if (!ie_writeptr(out,NULL,compat)) return IE_POINTER;
    if (outer) return (HRESULT)0x80040110U;
    IE_OBJECT *o = ie_new(IE_ENGINE);
    if (!o) return IE_NOMEM;
    o->file = ie_new(IE_FILE);
    if (o->file) {
        o->file->doc = kmalloc(sizeof(*o->file->doc));
        if (o->file->doc) memset(o->file->doc,0,sizeof(*o->file->doc));
    }
    if (!o->file || !o->file->doc) { ie_release(o->guest); return IE_NOMEM; }
    HRESULT result = ie_query(o->guest,iid,out);
    ie_release(o->guest); return result;
}
void installengine_init(void)
{
    static const GUID clsid = {0x6e449686,0xc509,0x11cf,{0xaa,0xfa,0,0xaa,0,0xb6,1,0x5c}};
    ole32_register_class(&clsid,ie_activate);
}
void installengine_release_process(DWORD owner)
{
    /* All threads stopped. Guest memory belongs to the owner heap reaper;
     * never invoke a foreign callback or LocalFree through another process. */
    for (unsigned i = 0; i < IE_OBJECTS; i++) {
        IE_OBJECT *o = ie_objects[i];
        if (!o || o->owner != owner) continue;
        __atomic_store_n(&ie_objects[i],NULL,__ATOMIC_RELEASE);
        if (o->kind == IE_ENGINE && o->job) kfree(o->job);
        if (o->kind == IE_FILE && o->doc) { kfree(o->doc->text); kfree(o->doc); }
        kfree(o);
    }
}
