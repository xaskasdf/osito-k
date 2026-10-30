/* Per-process CRT argv storage. Kernel-owned bookkeeping must not reside in
 * writable user arrays; all pointers exported to PE code use the caller ABI. */
#include "crt_startup.h"
#include <stdbool.h>
#include "crt_args.h"
#include "msvcrt_shim.h"
#include "kernel32_shim.h"
#include "compat32.h"

extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);
extern void sched_yield(void);
extern DWORD win32_current_process_id(void);
extern const char *win32_current_command_line(void);
extern const WCHAR *win32_current_command_line_w(void);

#define UNIT(p, i, w) ((w) == 1 ? ((const uint8_t *)(p))[i] : \
                                  ((const uint16_t *)(p))[i])
typedef struct CRT_STARTUP_CACHE {
    struct CRT_STARTUP_CACHE *next;
    DWORD pid;
    unsigned width, pointer_bytes;
    int wildcard, argc;
    void *argv;
} CRT_STARTUP_CACHE;
typedef struct CRT_STARTUP_ARG {
    struct CRT_STARTUP_ARG *next;
    uint32_t units;
    unsigned char text[];
} CRT_STARTUP_ARG;
typedef struct {
    CRT_STARTUP_ARG *head, *tail;
    uint32_t count, units;
} CRT_STARTUP_LIST;

static CRT_STARTUP_CACHE *crt_startup_cache;
static volatile uint32_t crt_startup_mutex;

static void crt_startup_lock(void)
{
    while (__sync_lock_test_and_set(&crt_startup_mutex, 1)) {
        for (unsigned i = 0; i < 100; ++i) __asm__ volatile("pause" ::: "memory");
        sched_yield();
    }
}

static CRT_STARTUP_CACHE *crt_startup_find(DWORD pid, unsigned width,
                                          unsigned pointer_bytes, int wildcard)
{
    for (CRT_STARTUP_CACHE *p = crt_startup_cache; p; p = p->next)
        if (p->pid == pid && p->width == width && p->pointer_bytes == pointer_bytes &&
            p->wildcard == wildcard) return p;
    return NULL;
}

static void crt_startup_pointer(void *cell, ULONG_PTR value, unsigned bytes)
{
    /* The address of the caller's pointer cell need not be naturally aligned. */
    for (unsigned i = 0; i < bytes; ++i)
        ((unsigned char *)cell)[i] = (unsigned char)(value >> (i * 8));
}

static void crt_startup_discard(CRT_STARTUP_LIST *list)
{
    while (list->head) {
        CRT_STARTUP_ARG *next = list->head->next;
        kfree(list->head);
        list->head = next;
    }
    list->tail = NULL;
    list->count = list->units = 0;
}

static int crt_startup_append(CRT_STARTUP_LIST *list, const void *prefix,
                              uint32_t prefix_units, const void *name,
                              unsigned width)
{
    uint32_t n = 0;
    while (n < CRT_ARGS_MAX_UNITS && UNIT(name, n, width)) ++n;
    if (n == CRT_ARGS_MAX_UNITS || prefix_units > UINT32_MAX - n - 1 ||
        list->units > UINT32_MAX - (prefix_units + n + 1) ||
        list->count == 0x7ffffffe) return -1;
    uint32_t units = prefix_units + n + 1;
    CRT_STARTUP_ARG *arg = kmalloc(sizeof(*arg) + (uint64_t)units * width);
    if (!arg) return -1;
    arg->next = NULL;
    arg->units = units;
    for (size_t i = 0; i < (size_t)prefix_units * width; ++i)
        arg->text[i] = ((const unsigned char *)prefix)[i];
    for (size_t i = 0; i < (size_t)(n + 1) * width; ++i)
        arg->text[(size_t)prefix_units * width + i] = ((const unsigned char *)name)[i];
    if (list->tail) list->tail->next = arg;
    else list->head = arg;
    list->tail = arg;
    ++list->count;
    list->units += units;
    return 0;
}

static int crt_startup_compare(const CRT_STARTUP_ARG *a,
                               const CRT_STARTUP_ARG *b, unsigned width)
{
    return width == 1 ? crt_stricmp((const char *)a->text, (const char *)b->text)
        : crt_wcsicmp((const WCHAR *)a->text, (const WCHAR *)b->text);
}

static int crt_startup_expand(CRT_STARTUP_LIST *list, const void *pattern,
                              unsigned width)
{
    uint32_t prefix = 0;
    for (uint32_t i = 0; UNIT(pattern, i, width); ++i)
        if (UNIT(pattern, i, width) == '\\' || UNIT(pattern, i, width) == '/' ||
            UNIT(pattern, i, width) == ':') prefix = i + 1;
    /* CRT globbing expands only the final path component. */
    for (uint32_t i = 0; i < prefix; ++i)
        if (UNIT(pattern, i, width) == '*' || UNIT(pattern, i, width) == '?')
            return crt_startup_append(list, NULL, 0, pattern, width);
    union { WIN32_FIND_DATAA a; WIN32_FIND_DATAW w; } found;
    HANDLE search = width == 1 ? FindFirstFileA(pattern, &found.a)
                              : FindFirstFileW(pattern, &found.w);
    if (search == INVALID_HANDLE_VALUE)
        return crt_startup_append(list, NULL, 0, pattern, width);
    CRT_STARTUP_LIST matches = {0};
    int error = 0;
    do {
        const void *name = width == 1 ? (const void *)found.a.cFileName
                                      : (const void *)found.w.cFileName;
        if (UNIT(name, 0, width) == '.' &&
            (!UNIT(name, 1, width) || (UNIT(name, 1, width) == '.' &&
                                      !UNIT(name, 2, width)))) continue;
        if (crt_startup_append(&matches, pattern, prefix, name, width)) {
            error = -1;
            break;
        }
    } while (width == 1 ? FindNextFileA(search, &found.a)
                       : FindNextFileW(search, &found.w));
    FindClose(search);
    if (error || list->count > 0x7ffffffe - matches.count ||
        list->units > UINT32_MAX - matches.units) {
        crt_startup_discard(&matches);
        return -1;
    }
    if (!matches.count) return crt_startup_append(list, NULL, 0, pattern, width);
    /* Bottom-up linked merge sort: bounded stack, O(n log n), preserving
     * enumeration order for names with equal case-insensitive comparisons. */
    for (uint32_t span = 1; span < matches.count; span *= 2) {
        CRT_STARTUP_ARG *at = matches.head, *head = NULL, **tail = &head;
        while (at) {
            CRT_STARTUP_ARG *left = at, *right = at;
            uint32_t nl = 0, nr = span;
            for (; nl < span && right; ++nl) right = right->next;
            while (nl || (nr && right)) {
                CRT_STARTUP_ARG *item;
                if (!nl || (nr && right && crt_startup_compare(left, right, width) > 0)) {
                    item = right; right = right->next; --nr;
                } else { item = left; left = left->next; --nl; }
                *tail = item;
                tail = &item->next;
            }
            at = right;
        }
        *tail = NULL;
        matches.head = head;
    }
    matches.tail = matches.head;
    while (matches.tail->next) matches.tail = matches.tail->next;
    if (list->tail) list->tail->next = matches.head;
    else list->head = matches.head;
    list->tail = matches.tail;
    list->count += matches.count;
    list->units += matches.units;
    return 0;
}

static CRT_STARTUP_CACHE *crt_startup_build(DWORD pid, unsigned width,
                                           unsigned pointer_bytes, int wildcard)
{
    void *snapshot = kmalloc(CRT_ARGS_MAX_UNITS * width);
    void *strings = NULL;
    CRT_ARG_RECORD *records = NULL;
    CRT_STARTUP_CACHE *cache = NULL;
    CRT_STARTUP_LIST list = {0};
    if (!snapshot) return NULL;
    const WCHAR *wide = width == 2 ? win32_current_command_line_w() : NULL;
    const char *narrow = win32_current_command_line();
    uint32_t n;
    for (n = 0; n < CRT_ARGS_MAX_UNITS; ++n) {
        uint16_t c = wide ? wide[n] : (narrow ? (uint8_t)narrow[n] : 0);
        if (width == 1) ((uint8_t *)snapshot)[n] = (uint8_t)c;
        else ((uint16_t *)snapshot)[n] = c;
        if (!c) break;
    }
    if (n == CRT_ARGS_MAX_UNITS) goto done;
    if (!n) {
        void *path = (unsigned char *)snapshot + width;
        n = width == 1 ? GetModuleFileNameA(NULL, path, CRT_ARGS_MAX_UNITS - 3)
                       : GetModuleFileNameW(NULL, path, CRT_ARGS_MAX_UNITS - 3);
        if (!n || n >= CRT_ARGS_MAX_UNITS - 3) goto done;
        if (width == 1) {
            ((uint8_t *)snapshot)[0] = ((uint8_t *)snapshot)[n + 1] = '"';
            ((uint8_t *)snapshot)[n + 2] = 0;
        } else {
            ((uint16_t *)snapshot)[0] = ((uint16_t *)snapshot)[n + 1] = '"';
            ((uint16_t *)snapshot)[n + 2] = 0;
        }
        n += 2;
    }
    CRT_ARG_COUNTS counts;
    if (crt_args_parse(snapshot, n + 1, width, NULL, 0, NULL, 0, &counts)) goto done;
    strings = kmalloc((uint64_t)counts.units * width);
    records = kmalloc((uint64_t)counts.argc * sizeof(*records));
    if (!strings || !records ||
        crt_args_parse(snapshot, n + 1, width, strings, counts.units,
                        records, counts.argc, &counts)) goto done;
    for (uint32_t i = 0; i < counts.argc; ++i) {
        const void *text = (const unsigned char *)strings + records[i].offset * width;
        if (wildcard && records[i].expand) {
            if (crt_startup_expand(&list, text, width)) goto done;
        } else if (crt_startup_append(&list, NULL, 0, text, width)) goto done;
    }
    cache = kmalloc(sizeof(*cache));
    if (!cache) goto done;
    SIZE_T vector_bytes = ((SIZE_T)list.count + 1) * pointer_bytes;
    SIZE_T bytes = vector_bytes + (SIZE_T)list.units * width;
    void *user = crt_malloc(bytes);
    if (!user || (pointer_bytes == 4 &&
        ((ULONG_PTR)user > UINT32_MAX || bytes - 1 > UINT32_MAX - (ULONG_PTR)user))) {
        if (user) crt_free(user);
        kfree(cache); cache = NULL;
        goto done;
    }
    unsigned char *dst = (unsigned char *)user + vector_bytes;
    uint32_t i = 0;
    for (CRT_STARTUP_ARG *p = list.head; p; p = p->next, ++i) {
        crt_startup_pointer((unsigned char *)user + (SIZE_T)i * pointer_bytes,
                             (ULONG_PTR)dst, pointer_bytes);
        for (SIZE_T j = 0; j < (SIZE_T)p->units * width; ++j) *dst++ = p->text[j];
    }
    crt_startup_pointer((unsigned char *)user + (SIZE_T)i * pointer_bytes, 0, pointer_bytes);
    *cache = (CRT_STARTUP_CACHE){NULL, pid, width, pointer_bytes, wildcard,
                                 (int)list.count, user};
done:
    crt_startup_discard(&list);
    if (records) kfree(records);
    if (strings) kfree(strings);
    kfree(snapshot);
    return cache;
}

int crt_startup_args(int *argc, void *argv, void *env, int wildcard,
                     void *startinfo, unsigned width)
{
    if (!argc || !argv || !env || (width != 1 && width != 2)) {
        *crt_errno() = 22;
        return -1;
    }
    DWORD pid = win32_current_process_id();
    if (!pid) pid = 1;
    unsigned pointer_bytes = g_compat32_mode ? 4 : 8;
    wildcard = !!wildcard;
    void *env_cell = width == 1 ? (void *)crt_p_environ() : (void *)crt_p_wenviron();
    if (!env_cell) return -1;
    crt_startup_lock();
    CRT_STARTUP_CACHE *cache = crt_startup_find(pid, width, pointer_bytes, wildcard);
    __sync_lock_release(&crt_startup_mutex);
    if (!cache) {
        CRT_STARTUP_CACHE *candidate = crt_startup_build(pid, width, pointer_bytes, wildcard);
        if (!candidate) { *crt_errno() = 12; return -1; }
        crt_startup_lock();
        cache = crt_startup_find(pid, width, pointer_bytes, wildcard);
        if (!cache) {
            candidate->next = crt_startup_cache;
            crt_startup_cache = candidate;
            cache = candidate;
            candidate = NULL;
        }
        __sync_lock_release(&crt_startup_mutex);
        if (candidate) { crt_free(candidate->argv); kfree(candidate); }
    }
    if (startinfo) _set_new_mode(*(const int *)startinfo);
    crt_startup_pointer(argv, (ULONG_PTR)cache->argv, pointer_bytes);
    for (unsigned i = 0; i < pointer_bytes; ++i)
        ((unsigned char *)env)[i] = ((const unsigned char *)env_cell)[i];
    *argc = cache->argc;
    return 0;
}

void crt_startup_release_process(DWORD pid)
{
    CRT_STARTUP_CACHE *released = NULL;
    crt_startup_lock();
    CRT_STARTUP_CACHE **link = &crt_startup_cache;
    while (*link) {
        CRT_STARTUP_CACHE *cache = *link;
        if (cache->pid == pid) {
            *link = cache->next;
            cache->next = released;
            released = cache;
        } else link = &cache->next;
    }
    __sync_lock_release(&crt_startup_mutex);
    while (released) {
        CRT_STARTUP_CACHE *next = released->next;
        /* User buffers are reclaimed with this process's VM, just like the
         * environment cache. The caller may already be in another CR3. */
        kfree(released);
        released = next;
    }
}
