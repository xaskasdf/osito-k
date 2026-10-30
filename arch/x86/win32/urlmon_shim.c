/* Synchronous URLDownloadToFile with a retained, guest-ABI IBinding object.
 * External installers own their payloads; no redistributables live here. */
#include "urlmon_shim.h"
#include "kernel32_shim.h"
#include "compat32.h"
#include "../kernel/http.h"

#define UM_ABORT ((LONG)0x80004004U)
#define UM_INVALID ((LONG)0x80070057U)
#define UM_NOTIMPL ((LONG)0x80004001U)
#define UM_DOWNLOAD ((LONG)0x800c0008U)
#define UM_RESOURCE ((LONG)0x800c0005U)
#define UM_OBJECT ((LONG)0x800c0006U)
#define UM_URL_MAX 1536
#define UM_PATH_MAX 512
#define UM_BINDINGS 16

typedef struct {
    BYTE object[80]; /* pointer + nine vtable entries, packed for each ABI */
    WCHAR text[UM_URL_MAX];
} URLMON_GUEST;

typedef struct {
    char url[UM_URL_MAX], host[256], authority[264], path[UM_URL_MAX];
    WCHAR destination[UM_PATH_MAX], temporary[UM_PATH_MAX];
    BYTE buffer[4096];
    http_response_t response;
    HANDLE output;
    DWORD transferred, expected;
    BOOL tls, save_failed, began, temporary_owned;
    uint16_t port;
} URLMON_TRANSFER;

typedef struct {
    volatile int used;
    DWORD owner, refs;
    BOOL compat, active;
    volatile LONG cancelled;
    LONG priority, result;
    PVOID callback;
    URLMON_INTERNAL_CALLBACK internal_callback;
    PVOID internal_context;
    URLMON_GUEST *guest;
    URLMON_TRANSFER *transfer;
    http_session_t session; /* survives forced guest heap teardown */
} URLMON_BINDING;

static URLMON_BINDING urlmon_bindings[UM_BINDINGS];

static BOOL urlmon_prefix(PCSTR text, PCSTR prefix)
{
    while (*prefix) {
        char c = *text++;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != *prefix++) return FALSE;
    }
    return TRUE;
}

static URLMON_BINDING *urlmon_binding_find(PVOID object)
{
    DWORD owner = GetCurrentProcessId();
    for (unsigned i = 0; i < UM_BINDINGS; i++)
        if (urlmon_bindings[i].used == 1 && urlmon_bindings[i].owner == owner &&
            urlmon_bindings[i].guest == object) return &urlmon_bindings[i];
    return NULL;
}

static DWORD WINAPI urlmon_binding_addref(PVOID object)
{
    URLMON_BINDING *b = urlmon_binding_find(object);
    return b ? __atomic_add_fetch(&b->refs, 1, __ATOMIC_SEQ_CST) : 0;
}

static DWORD WINAPI urlmon_binding_release(PVOID object)
{
    URLMON_BINDING *b = urlmon_binding_find(object);
    if (!b) return 0;
    DWORD refs = __atomic_sub_fetch(&b->refs, 1, __ATOMIC_SEQ_CST);
    if (!refs) {
        LocalFree(b->guest);
        b->guest = NULL;
        __atomic_store_n(&b->used, 0, __ATOMIC_RELEASE);
    }
    return refs;
}

static LONG WINAPI urlmon_binding_query(PVOID object, PCVOID iid, PVOID out)
{
    static const BYTE unknown[16] = {0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    static const BYTE binding[16] = {0xc0,0xc9,0xea,0x79,0xf9,0xba,0xce,0x11,
                                     0x8c,0x82,0,0xaa,0,0x4b,0xa9,0x0b};
    URLMON_BINDING *b = urlmon_binding_find(object);
    if (!b || !win32_user_range_writable(out, b->compat ? 4 : 8, b->compat) ||
        !win32_user_range_readable(iid, 16, b->compat)) return (LONG)0x80004003U;
    ULONG_PTR pointer = 0;
    BOOL match = !memcmp(iid, unknown, 16) || !memcmp(iid, binding, 16);
    if (match) { pointer = (ULONG_PTR)object; urlmon_binding_addref(object); }
    memcpy(out, &pointer, b->compat ? 4 : 8);
    return match ? 0 : (LONG)0x80004002U;
}

static LONG WINAPI urlmon_binding_abort(PVOID object)
{
    URLMON_BINDING *b = urlmon_binding_find(object);
    if (!b) return UM_INVALID;
    if (b->active) __atomic_store_n(&b->cancelled, UM_ABORT, __ATOMIC_RELEASE);
    return 0;
}
static LONG WINAPI urlmon_binding_suspend(PVOID object)
{ (void)object; return UM_NOTIMPL; }
static LONG WINAPI urlmon_binding_resume(PVOID object)
{ (void)object; return UM_NOTIMPL; }
static LONG WINAPI urlmon_binding_set_priority(PVOID object, LONG priority)
{
    URLMON_BINDING *b = urlmon_binding_find(object);
    if (!b) return UM_INVALID;
    b->priority = priority; return 0;
}
static LONG WINAPI urlmon_binding_get_priority(PVOID object, LONG *priority)
{
    URLMON_BINDING *b = urlmon_binding_find(object);
    if (!b || !win32_user_range_writable(priority, 4, b->compat)) return UM_INVALID;
    *priority = b->priority; return 0;
}
static LONG WINAPI urlmon_binding_result(PVOID object, PVOID protocol,
                                         DWORD *result, PVOID text, DWORD *reserved)
{
    (void)object; (void)protocol; (void)result; (void)text; (void)reserved;
    return UM_NOTIMPL;
}

static LONG urlmon_callback(URLMON_BINDING *b, unsigned index, unsigned count,
                             ULONG_PTR a, ULONG_PTR c, ULONG_PTR d, ULONG_PTR e)
{
    if (b->internal_callback)
        return b->internal_callback(b->internal_context,index,a,c,d,e);
    if (!b->callback) return 0;
    ULONG_PTR table = 0, proc = 0;
    unsigned step = b->compat ? 4 : 8;
    if (!win32_user_range_readable(b->callback, step, b->compat)) return UM_DOWNLOAD;
    memcpy(&table, b->callback, step);
    PCVOID entry = (PCVOID)(table + index * step);
    if (!win32_user_range_readable(entry, step, b->compat)) return UM_DOWNLOAD;
    memcpy(&proc, entry, step);
    if (!win32_user_range_executable((PCVOID)proc, 1, b->compat)) return UM_DOWNLOAD;
    if (b->compat) {
        uint32_t args[5] = {(uint32_t)(ULONG_PTR)b->callback, (uint32_t)a,
                            (uint32_t)c, (uint32_t)d, (uint32_t)e};
        uint32_t stack = compat32_current_user_stack_top();
        return (LONG)(stack ? compat32_callback_args_on_stack((uint32_t)proc,
            (int)count, args, stack) : compat32_callback_args((uint32_t)proc,
            (int)count, args));
    }
    if (count == 1) {
        typedef LONG (WINAPI *FN)(PVOID);
        return ((FN)proc)(b->callback);
    }
    if (count == 3) {
        typedef LONG (WINAPI *FN)(PVOID, ULONG_PTR, ULONG_PTR);
        return ((FN)proc)(b->callback, a, c);
    }
    typedef LONG (WINAPI *FN)(PVOID, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);
    return ((FN)proc)(b->callback, a, c, d, e);
}

static URLMON_BINDING *urlmon_binding_create(PVOID callback)
{
    static const struct {PVOID proc; const char *name; BYTE argc;} methods[] = {
        {(PVOID)urlmon_binding_query,"IBinding::QueryInterface",3},
        {(PVOID)urlmon_binding_addref,"IBinding::AddRef",1},
        {(PVOID)urlmon_binding_release,"IBinding::Release",1},
        {(PVOID)urlmon_binding_abort,"IBinding::Abort",1},
        {(PVOID)urlmon_binding_suspend,"IBinding::Suspend",1},
        {(PVOID)urlmon_binding_resume,"IBinding::Resume",1},
        {(PVOID)urlmon_binding_set_priority,"IBinding::SetPriority",2},
        {(PVOID)urlmon_binding_get_priority,"IBinding::GetPriority",2},
        {(PVOID)urlmon_binding_result,"IBinding::GetBindResult",5}
    };
    for (unsigned i = 0; i < UM_BINDINGS; i++) {
        URLMON_BINDING *b = &urlmon_bindings[i];
        if (!__sync_bool_compare_and_swap(&b->used, 0, -1)) continue;
        b->owner = GetCurrentProcessId(); b->compat = g_compat32_mode != 0;
        b->refs = 1; b->active = TRUE; b->cancelled = 0; b->priority = 0;
        b->result = (LONG)0x8000000aU; b->callback = callback;
        b->internal_callback = NULL; b->internal_context = NULL;
        b->guest = LocalAlloc(0x40, sizeof(*b->guest)); b->transfer = NULL;
        memset(&b->session, 0, sizeof(b->session)); b->session.tcp_conn = -1;
        if (!b->guest) { b->used = 0; return NULL; }
        ULONG_PTR table = (ULONG_PTR)b->guest->object + 8;
        unsigned step = b->compat ? 4 : 8;
        memcpy(b->guest->object, &table, step);
        for (unsigned n = 0; n < 9; n++) {
            ULONG_PTR proc = (ULONG_PTR)methods[n].proc;
            if (b->compat) proc = compat32_make_thunk_ex(proc, methods[n].name,
                                                          methods[n].argc, CC_STDCALL);
            if (!proc) { LocalFree(b->guest); b->used = 0; return NULL; }
            memcpy(b->guest->object + 8 + n * step, &proc, step);
        }
        __atomic_store_n(&b->used, 1, __ATOMIC_RELEASE);
        return b;
    }
    return NULL;
}

static LONG urlmon_progress(URLMON_BINDING *b, DWORD status, PCSTR text)
{
    if (b->cancelled) return b->cancelled;
    unsigned i = 0;
    if (text) for (; i < UM_URL_MAX - 1 && text[i]; i++) b->guest->text[i] = (BYTE)text[i];
    b->guest->text[i] = 0;
    LONG rc = urlmon_callback(b, 6, 5, b->transfer->transferred,
        b->transfer->expected, status, text ? (ULONG_PTR)b->guest->text : 0);
    if (rc < 0) b->cancelled = rc;
    return b->cancelled;
}

static BOOL urlmon_wide_ascii(PCWSTR wide, char *ascii, unsigned capacity, BOOL compat)
{
    if (!wide) return FALSE;
    for (unsigned i = 0; i < capacity; i++) {
        if (!win32_user_range_readable(wide + i, 2, compat)) return FALSE;
        WCHAR c = wide[i];
        if (!c) { ascii[i] = 0; return TRUE; }
        if (c < 0x20 || c >= 0x7f) return FALSE;
        ascii[i] = (char)c;
    }
    return FALSE;
}

static BOOL urlmon_parse_url(URLMON_TRANSFER *t)
{
    unsigned start;
    if (urlmon_prefix(t->url, "http://")) { start = 7; t->tls = FALSE; t->port = 80; }
    else if (urlmon_prefix(t->url, "https://")) { start = 8; t->tls = TRUE; t->port = 443; }
    else return FALSE;
    unsigned end = start;
    while (t->url[end] && t->url[end] != '/' && t->url[end] != '?' && t->url[end] != '#') end++;
    if (end == start || end - start >= sizeof(t->authority)) return FALSE;
    memcpy(t->authority, t->url + start, end - start); t->authority[end - start] = 0;
    unsigned n = 0;
    while (start < end && t->url[start] != ':') {
        char c = t->url[start++];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-') || n >= sizeof(t->host)-1)
            return FALSE;
        t->host[n++] = c;
    }
    if (!n) return FALSE;
    t->host[n] = 0;
    if (start < end) {
        unsigned port = 0; start++;
        if (start == end) return FALSE;
        while (start < end) {
            char c = t->url[start++];
            if (c < '0' || c > '9') return FALSE;
            port = port * 10 + (unsigned)(c - '0');
            if (port > 65535) return FALSE;
        }
        if (!port) return FALSE;
        t->port = (uint16_t)port;
    }
    n = 0;
    if (t->url[end] != '/') t->path[n++] = '/';
    while (t->url[end] && t->url[end] != '#') {
        char c = t->url[end++];
        if (c <= ' ' || n >= sizeof(t->path)-1) return FALSE;
        t->path[n++] = c;
    }
    t->path[n] = 0;
    /* http_request has a 2048-byte request buffer. */
    return n + strlen(t->authority) + 160 < 2048;
}

static BOOL urlmon_redirect(URLMON_TRANSFER *t, PCSTR location)
{
    if (!location || !*location) return FALSE;
    char next[UM_URL_MAX];
    unsigned n = 0;
    if (!urlmon_prefix(location, "http://") && !urlmon_prefix(location, "https://")) {
        PCSTR scheme = t->tls ? "https:" : "http:";
        while (*scheme) next[n++] = *scheme++;
        if (location[0] != '/' || location[1] != '/') {
            next[n++] = '/'; next[n++] = '/';
            for (unsigned i = 0; t->authority[i]; i++) next[n++] = t->authority[i];
            if (location[0] != '/') {
                unsigned end = 0;
                for (unsigned i = 0; t->path[i] && t->path[i] != '?'; i++)
                    if (t->path[i] == '/') end = i + 1;
                if (n + end >= sizeof(next)) return FALSE;
                memcpy(next + n, t->path, end); n += end;
            }
        }
    }
    while (*location) {
        if (n >= sizeof(next)-1 || (BYTE)*location <= ' ' || (BYTE)*location >= 0x7f) return FALSE;
        next[n++] = *location++;
    }
    next[n] = 0; memcpy(t->url, next, n + 1);
    return urlmon_parse_url(t);
}

static int urlmon_body(PCVOID data, uint32_t size, PVOID context)
{
    URLMON_BINDING *b = context;
    URLMON_TRANSFER *t = b->transfer;
    if (b->cancelled) return -1;
    DWORD written = 0;
    if (!WriteFile(t->output, data, size, &written, NULL) || written != size) {
        t->save_failed = TRUE; return -1;
    }
    t->transferred += size;
    DWORD status = t->began ? 5 : 4; /* BEGIN/DOWNLOADINGDATA */
    t->began = TRUE;
    return urlmon_progress(b, status, t->url) < 0 ? -1 : 0;
}

static LONG urlmon_download_http(URLMON_BINDING *b)
{
    URLMON_TRANSFER *t = b->transfer;
    static const char *const headers[] = {"User-Agent: OsitoK URLMON/1.0",
        "Accept: */*", "Accept-Encoding: identity", "Connection: close", NULL};
    for (unsigned redirects = 0; redirects < 10; redirects++) {
        if (!urlmon_parse_url(t)) return UM_DOWNLOAD;
        if (urlmon_progress(b, 1, t->host) < 0) return b->cancelled;
        if (http_open_port(&b->session, t->host, t->port, t->tls) < 0) return UM_RESOURCE;
        LONG rc = urlmon_progress(b, 2, t->host);
        if (rc >= 0) rc = urlmon_progress(b, 11, NULL);
        if (rc < 0) { http_close(&b->session); return rc; }
        if (http_request(&b->session, "GET", t->path, t->authority, headers,
                         NULL, 0, &t->response) < 0) {
            http_close(&b->session); return UM_DOWNLOAD;
        }
        int status = t->response.status_code;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            PCSTR location = http_get_header(&t->response, "Location");
            BOOL valid = urlmon_redirect(t, location);
            http_close(&b->session);
            if (!valid) return UM_DOWNLOAD;
            if (urlmon_progress(b, 3, t->url) < 0) return b->cancelled;
            continue;
        }
        if (status < 200 || status >= 300 || status == 206) {
            http_close(&b->session); return status == 404 ? UM_OBJECT : UM_DOWNLOAD;
        }
        PCSTR encoding = http_get_header(&t->response, "Content-Encoding");
        if (encoding && strcmp(encoding, "identity")) {
            http_close(&b->session); return UM_DOWNLOAD;
        }
        t->expected = t->response.chunked ? 0 : t->response.content_length;
        if (urlmon_progress(b, 13, NULL) < 0) { http_close(&b->session); return b->cancelled; }
        int count = http_read_body(&b->session, &t->response, urlmon_body, b);
        http_close(&b->session);
        if (b->cancelled) return b->cancelled;
        return count < 0 ? UM_DOWNLOAD : 0;
    }
    return UM_DOWNLOAD;
}

static LONG urlmon_download_file(URLMON_BINDING *b)
{
    URLMON_TRANSFER *t = b->transfer;
    if (!urlmon_prefix(t->url, "file:///")) return UM_DOWNLOAD;
    char source[UM_PATH_MAX]; unsigned n = 0;
    for (unsigned i = 8; t->url[i] && t->url[i] != '#'; i++) {
        unsigned c = (BYTE)t->url[i];
        if (c == '%') {
            c = 0;
            for (unsigned j = 0; j < 2; j++) {
                char digit = t->url[++i];
                unsigned value;
                if (digit >= '0' && digit <= '9') value = digit - '0';
                else if (digit >= 'A' && digit <= 'F') value = digit - 'A' + 10;
                else if (digit >= 'a' && digit <= 'f') value = digit - 'a' + 10;
                else return UM_DOWNLOAD;
                c = c * 16 + value;
            }
        }
        if (!c || n >= sizeof(source)-1) return UM_DOWNLOAD;
        source[n++] = (char)c;
    }
    source[n] = 0;
    HANDLE input = CreateFileA(source, GENERIC_READ, 7, NULL, 3, 0x80, NULL);
    if (input == INVALID_HANDLE_VALUE) return UM_RESOURCE;
    DWORD high = 0;
    t->expected = GetFileSize(input, &high);
    LONG rc = high || t->expected > 0x7fffffffU ? UM_DOWNLOAD : urlmon_progress(b, 13, NULL);
    while (rc >= 0) {
        DWORD count = 0;
        if (!ReadFile(input, t->buffer, sizeof(t->buffer), &count, NULL)) { rc = UM_DOWNLOAD; break; }
        if (!count) break;
        if (urlmon_body(t->buffer, count, b) < 0) { rc = b->cancelled ? b->cancelled : UM_DOWNLOAD; break; }
    }
    CloseHandle(input);
    if (rc >= 0 && t->transferred != t->expected) rc = UM_DOWNLOAD;
    return rc;
}

static LONG urlmon_download_to_file(PCWSTR url, PCWSTR destination, PVOID callback,
    URLMON_INTERNAL_CALLBACK internal_callback, PVOID internal_context)
{
    if (!url || !destination) return UM_INVALID;
    URLMON_BINDING *b = urlmon_binding_create(callback);
    if (!b) return (LONG)0x8007000eU;
    b->internal_callback = internal_callback; b->internal_context = internal_context;
    URLMON_TRANSFER *t = LocalAlloc(0x40, sizeof(*t));
    b->transfer = t;
    LONG rc = UM_DOWNLOAD;
    BOOL retained = FALSE, started = FALSE;
    if (!t) { rc = (LONG)0x8007000eU; goto done; }
    t->output = INVALID_HANDLE_VALUE;
    if (!urlmon_wide_ascii(url, t->url, sizeof(t->url), b->compat)) goto done;
    unsigned n = 0;
    for (; n < UM_PATH_MAX - 24; n++) {
        if (!win32_user_range_readable(destination + n, 2, b->compat)) goto done;
        t->temporary[n] = t->destination[n] = destination[n];
        if (!destination[n]) break;
    }
    if (!n || n == UM_PATH_MAX - 24) goto done;
    const char *suffix = ".urlmon-";
    while (*suffix) t->temporary[n++] = (BYTE)*suffix++;
    DWORD unique = b->owner * UM_BINDINGS + (DWORD)(b - urlmon_bindings);
    for (int i = 7; i >= 0; i--) t->temporary[n++] = "0123456789abcdef"[(unique >> (i * 4)) & 15];
    t->temporary[n] = 0;
    if (callback) {
        rc = urlmon_callback(b, 1, 1, 0, 0, 0, 0);
        if (rc < 0) goto done;
        retained = TRUE;
    }
    started = TRUE;
    rc = urlmon_callback(b, 3, 3, 0, (ULONG_PTR)b->guest, 0, 0);
    if (rc < 0) { b->cancelled = rc; goto done; }
    /* Cancellation before the first byte leaves an existing destination intact. */
    if (urlmon_progress(b, 32, NULL) < 0) { rc = b->cancelled; goto done; }
    t->output = CreateFileW(t->temporary, GENERIC_WRITE, 0, NULL, 1, 0x80, NULL);
    if (t->output == INVALID_HANDLE_VALUE) { t->save_failed = TRUE; rc = UM_DOWNLOAD; goto done; }
    t->temporary_owned = TRUE;
    rc = urlmon_prefix(t->url, "file:") ? urlmon_download_file(b) : urlmon_download_http(b);
    CloseHandle(t->output); t->output = INVALID_HANDLE_VALUE;
    if (rc >= 0) {
        if (!MoveFileExW_k32(t->temporary, t->destination, 1)) {
            t->save_failed = TRUE; rc = UM_DOWNLOAD;
        } else {
            t->temporary[0] = 0;
            rc = urlmon_progress(b, 6, t->url);
        }
    }
done:
    if (t) {
        if (t->output != INVALID_HANDLE_VALUE) CloseHandle(t->output);
        if (t->temporary_owned && t->temporary[0]) DeleteFileW(t->temporary);
    }
    b->active = FALSE;
    b->result = rc < 0 ? rc : 1; /* native URLDownloadToFile wrapper uses S_FALSE */
    if (started) urlmon_callback(b, 7, 3, (DWORD)b->result, 0, 0, 0);
    if (retained) urlmon_callback(b, 2, 1, 0, 0, 0, 0);
    /* S_OK describes a started bind; OnStopBinding carries cancel/save failure. */
    if (started && (b->cancelled || (t && t->save_failed))) rc = 0;
    if (t) LocalFree(t);
    b->transfer = NULL; b->callback = NULL;
    b->internal_callback = NULL; b->internal_context = NULL;
    urlmon_binding_release(b->guest);
    return rc;
}
static LONG WINAPI shim_URLDownloadToFileW(PVOID caller, PCWSTR url,
                                            PCWSTR destination, DWORD reserved,
                                            PVOID callback)
{
    if (caller) return UM_NOTIMPL; /* ActiveX container/IBindHost not implemented. */
    if (reserved) return UM_INVALID;
    return urlmon_download_to_file(url,destination,callback,NULL,NULL);
}
LONG urlmon_download_to_file_internal(PCWSTR url, PCWSTR destination,
    URLMON_INTERNAL_CALLBACK callback, PVOID context)
{
    return urlmon_download_to_file(url,destination,NULL,callback,context);
}

void urlmon_release_process(DWORD process_id)
{
    for (unsigned i = 0; i < UM_BINDINGS; i++) {
        URLMON_BINDING *b = &urlmon_bindings[i];
        if (b->used != 1 || b->owner != process_id) continue;
        http_abort(&b->session);
        /* Workers are stopped. Guest allocations/handles belong to the process
         * reaper; never call callbacks or free through a foreign process heap. */
        b->guest = NULL; b->transfer = NULL; b->callback = NULL;
        __atomic_store_n(&b->used, 0, __ATOMIC_RELEASE);
    }
}

static const WIN32_EXPORT urlmon_exports[] = {
    WX_STD("URLDownloadToFileW", shim_URLDownloadToFileW, 5),
    {NULL, NULL, 0, CC_STDCALL}
};
const WIN32_EXPORT *urlmon_abi_table(int *count)
{
    *count = sizeof(urlmon_exports) / sizeof(urlmon_exports[0]);
    return urlmon_exports;
}
PVOID urlmon_resolve(const char *name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (!by_ordinal && name)
        for (unsigned i = 0; urlmon_exports[i].name; i++)
            if (!strcmp(name, urlmon_exports[i].name)) return urlmon_exports[i].func;
    return NULL;
}
