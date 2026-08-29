/*
 * OsitoK Windows Compatibility Layer — Co-located ABI descriptor registry +
 * MSVC name demangler. See win32_abi.h for the rationale (NT .thk model).
 */
#include "win32_abi.h"

extern void serial_puts(const char *s);

/* ── Registry: dll_name → co-located export table ───────────────── */

#define WIN32_ABI_MAX_DLLS 64
#define WIN32_ABI_MAX_COMPAT32_BRIDGES 32

static struct {
    const char         *dll;
    const WIN32_EXPORT *table;
    int                 count;
} g_abi[WIN32_ABI_MAX_DLLS];
static int g_abi_count;

void win32_abi_reset(void)
{
    for (int i = 0; i < WIN32_ABI_MAX_DLLS; i++) {
        g_abi[i].dll = NULL;
        g_abi[i].table = NULL;
        g_abi[i].count = 0;
    }
    g_abi_count = 0;
}

/* Case-insensitive ASCII compare, NUL-terminated. */
static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

void win32_abi_register(const char *dll_name, const WIN32_EXPORT *table, int count)
{
    if (!dll_name || !table) return;
    for (int i = 0; i < g_abi_count; i++) {
        if (ci_eq(g_abi[i].dll, dll_name)) {
            g_abi[i].table = table;
            g_abi[i].count = count;
            return;
        }
    }
    if (g_abi_count >= WIN32_ABI_MAX_DLLS) return;
    g_abi[g_abi_count].dll   = dll_name;
    g_abi[g_abi_count].table = table;
    g_abi[g_abi_count].count = count;
    g_abi_count++;
}

static const WIN32_EXPORT *find_in_table(const WIN32_EXPORT *t, int n,
                                         const char *name)
{
    for (int i = 0; i < n; i++)
        if (t[i].name && ci_eq(t[i].name, name))
            return &t[i];
    return NULL;
}

static const WIN32_EXPORT *find_target_in_table(const WIN32_EXPORT *t, int n,
                                                const void *target)
{
    for (int i = 0; i < n; i++)
        if (t[i].name && t[i].func == target)
            return &t[i];
    return NULL;
}

static uint8_t export_callconv(const WIN32_EXPORT *e)
{
    return (uint8_t)(e->cc & WIN32_EXPORT_ABI_MASK);
}

void win32_abi_register_compat32_bridge(const void *native_target,
                                        const void *compat32_target)
{
    if (!native_target || !compat32_target) return;

    for (int i = 0; i < g_compat32_bridge_count; i++) {
        if (g_compat32_bridges[i].native_target == native_target) {
            g_compat32_bridges[i].compat32_target = compat32_target;
            return;
        }
    }
    if (g_compat32_bridge_count >= WIN32_ABI_MAX_COMPAT32_BRIDGES) return;

    g_compat32_bridges[g_compat32_bridge_count].native_target = native_target;
    g_compat32_bridges[g_compat32_bridge_count].compat32_target = compat32_target;
    g_compat32_bridge_count++;
}

const void *win32_abi_compat32_bridge(const void *native_target)
{
    for (int i = 0; i < g_compat32_bridge_count; i++)
        if (g_compat32_bridges[i].native_target == native_target)
            return g_compat32_bridges[i].compat32_target;
    return NULL;
}

/* ── MSVC name demangler (argc + calling convention only) ───────── */

/* Calling-convention code → CC_*. Pairs are near/far; we treat both. */
static int cc_from_code(char c, uint8_t *cc)
{
    switch (c) {
        case 'A': case 'B': *cc = CC_CDECL;    return 1;
        case 'C': case 'D': *cc = CC_STDCALL;  return 1; /* __pascal: callee-clean */
        case 'E': case 'F': *cc = CC_THISCALL; return 1;
        case 'G': case 'H': *cc = CC_STDCALL;  return 1;
        case 'I': case 'J': *cc = CC_FASTCALL; return 1;
        default: return 0;
    }
}

static int bounded_cstr_len(const char *s, int limit)
{
    if (!s) return -1;
    for (int i = 0; i < limit; i++) {
        if (s[i] == 0) return i;
    }
    return -1;
}

/* Advance *pp past a `<name>@@`-terminated qualified name. 1 ok / 0 malformed. */
static int skip_qual_name(const char **pp, const char *end)
{
    const char *q = *pp;
    while (q < end) {
        if (q + 1 >= end) return 0;
        if (q[0] == '?' && q[1] == '$') return 0;   /* template — bail */
        if (q[0] == '@' && q[1] == '@') { *pp = q + 2; return 1; }
        q++;
    }
    return 0;
}

/* Skip a full type token (size irrelevant — used for pointer/ref pointees).
 * Returns 1 ok / 0 if unparseable. */
static int skip_type(const char **pp, const char *end)
{
    const char *p = *pp;
    int depth = 0;

again:
    if (depth++ > 32) return 0;
    if (p >= end) return 0;
    char c = *p;
    if (!c) return 0;
    switch (c) {
        /* primitives (single char) */
        case 'X': case 'D': case 'C': case 'E': case 'F': case 'G':
        case 'H': case 'I': case 'J': case 'K': case 'M': case 'N': case 'O':
            *pp = p + 1; return 1;
        case '_':                                /* extended primitive */
            if (p + 1 >= end || !p[1]) return 0;
            *pp = p + 2; return 1;
        case 'P': case 'Q': case 'R': case 'S':  /* pointer */
        case 'A': case 'B': {                    /* reference */
            p++;
            if (p >= end) return 0;
            if (*p == '6' || *p == '7') return 0;/* ptr-to-function — bail */
            if (*p >= 'A' && *p <= 'Z') p++;     /* cv qualifier */
            else if (*p == '_') return 0;
            goto again;                          /* skip pointee */
        }
        case 'V': case 'U': case 'T':            /* class/struct/union */
            p++; *pp = p;
            return skip_qual_name(pp, end);
        case 'W':                                /* enum: W<digit><name>@@ */
            p++;
            if (p >= end) return 0;
            if (*p) p++;                          /* underlying-type digit */
            *pp = p;
            return skip_qual_name(pp, end);
        default:
            return 0;
    }
}

/* Size (in 32-bit DWORDs) of a top-level argument type; advances *pp.
 * Returns 1 ok / 0 if ambiguous (by-value user type) or unparseable. */
static int arg_size(const char **pp, const char *end, int *dw)
{
    const char *p = *pp;
    if (p >= end) return 0;
    switch (*p) {
        case 'N':                                /* double */
            *dw = 2; *pp = p + 1; return 1;
        case 'O':                                /* long double (x86 80-bit) */
            *dw = 3; *pp = p + 1; return 1;
        case 'D': case 'C': case 'E': case 'F': case 'G':
        case 'H': case 'I': case 'J': case 'K': case 'M':
            *dw = 1; *pp = p + 1; return 1;
        case '_':                                /* bool/wchar=1, int64/uint64=2 */
            if (p + 1 >= end) return 0;
            if (p[1] == 'J' || p[1] == 'K') { *dw = 2; *pp = p + 2; return 1; }
            if (p[1] == 'N' || p[1] == 'W') { *dw = 1; *pp = p + 2; return 1; }
            return 0;
        case 'P': case 'Q': case 'R': case 'S':  /* pointer = 1 DWORD */
        case 'A': case 'B':                      /* reference = 1 DWORD */
            *dw = 1;
            return skip_type(pp, end);
        case 'W':                                /* enum = int */
            *dw = 1;
            return skip_type(pp, end);
        case 'V': case 'U': case 'T':            /* by-value struct — unknown */
            return 0;
        default:
            return 0;
    }
}

int msvc_demangle_abi(const char *s, uint8_t *out_argc, uint8_t *out_cc)
{
    if (!s || s[0] != '?') return 0;
    int slen = bounded_cstr_len(s, 256);
    if (slen < 0) return 0;
    const char *end = s + slen;
    const char *p = s + 1;

    if (p >= end) return 0;
    if (*p == '?') {                              /* ??x operator/special */
        p++;
        if (p >= end) return 0;
        p++;                                      /* op code char */
    }
    if (!skip_qual_name(&p, end)) return 0;       /* skip up to `@@` */

    if (p >= end) return 0;
    char kind = *p++;
    int code = kind - 'A';
    if (code < 0 || code > 25) return 0;

    /* MSVC function-type code (A-Z → 0-25):
     *   bit 0   (0x01): near(0) / far(1)
     *   bits 1-2(0x06): member(0) / static(2) / virtual(4)
     *   bits 3-4(0x18): private(0) / protected(8) / public(16)
     *   code < 24:     member-function; code >= 24: free function
     * Ref: NT5 undname.cxx getTypeEncoding + undname.hxx TE_ constants. */
    int is_member = (code < 24);                   /* 0-23 = member, 24-25 = free */
    int is_static = is_member && ((code & 0x06) == 2);

    uint8_t cc;
    if (is_member && !is_static) {
        /* Non-static member: skip `thistype` prefix (A/B reference + CV
         * letter, e.g. `AE` in `?Tick@UObject@@UA E X X Z`), then read
         * the calling-convention character.  If the CC char is not a valid
         * code (constructor/destructor `@`), default to thiscall. */
        if (p >= end) return 0;
        if (*p == 'A' || *p == 'B') {
            p++;                                   /* skip reference marker */
            if (p >= end) return 0;
            if (*p >= 'A' && *p <= 'Z') p++;       /* skip CV qualifier */
        }
        if (p >= end) return 0;
        if (!cc_from_code(*p, &cc)) {
            cc = CC_THISCALL;                      /* ctor/dtor @ → fallback */
            if (*p != '@') return 0;               /* truly unparseable */
        }
        p++;
    } else {
        /* Free function or static member: read CC from next char */
        if (p >= end) return 0;
        if (!cc_from_code(*p, &cc)) return 0;
        p++;
    }

    /* Skip return type (constructors/destructors use `@` as empty return) */
    if (p >= end) return 0;
    if (*p == '@') {
        p++;
    } else {
        if (!skip_type(&p, end)) return 0;
    }

    int argc = 0;
    if (p >= end) return 0;
    if (*p == 'X') {                              /* (void) */
        p++;
    } else {
        while (p < end && *p && *p != 'Z') {      /* 'Z' alone = ellipsis/end */
            if (*p == '@') { p++; break; }        /* end of arg list */
            int dw;
            if (!arg_size(&p, end, &dw)) return 0;/* ambiguous → bail */
            argc += dw;
            if (argc > 64) return 0;              /* sanity */
        }
    }

    /* Non-static member functions: `this` pointer counts as 1 DWORD arg */
    if (is_member && !is_static) argc += 1;

    *out_cc   = cc;
    *out_argc = (uint8_t)argc;
    return 1;
}

/* ── Public lookup ──────────────────────────────────────────────── */

int win32_abi_lookup(const char *dll_name, const char *func_name,
                     uint8_t *out_argc, uint8_t *out_cc)
{
    if (!func_name) return 0;

    /* 1. exact DLL's co-located table */
    if (dll_name) {
        for (int i = 0; i < g_abi_count; i++) {
            if (ci_eq(g_abi[i].dll, dll_name)) {
                const WIN32_EXPORT *e =
                    find_in_table(g_abi[i].table, g_abi[i].count, func_name);
                if (e) {
                    *out_argc = e->argc;
                    *out_cc = export_callconv(e);
                    return 1;
                }
                break;
            }
        }
    }

    /* 2. any registered table (api-ms-win-crt-* redirections, fallback resolve) */
    for (int i = 0; i < g_abi_count; i++) {
        const WIN32_EXPORT *e =
            find_in_table(g_abi[i].table, g_abi[i].count, func_name);
        if (e) {
            *out_argc = e->argc;
            *out_cc = export_callconv(e);
            return 1;
        }
    }

    /* 3. MSVC C++ mangled name → exact argc + cc */
    if (func_name[0] == '?')
        return msvc_demangle_abi(func_name, out_argc, out_cc);

    return 0;
}

int win32_abi_lookup_target(const char *dll_name, const void *target,
                            const char **out_name, uint8_t *out_argc,
                            uint8_t *out_cc)
{
    if (!target) return 0;

    if (dll_name) {
        for (int i = 0; i < g_abi_count; i++) {
            if (ci_eq(g_abi[i].dll, dll_name)) {
                const WIN32_EXPORT *e = find_target_in_table(
                    g_abi[i].table, g_abi[i].count, target);
                if (e) {
                    *out_name = e->name;
                    *out_argc = e->argc;
                    *out_cc = export_callconv(e);
                    return 1;
                }
                break;
            }
        }
    }

    for (int i = 0; i < g_abi_count; i++) {
        const WIN32_EXPORT *e = find_target_in_table(
            g_abi[i].table, g_abi[i].count, target);
        if (e) {
            *out_name = e->name;
            *out_argc = e->argc;
            *out_cc = export_callconv(e);
            return 1;
        }
    }
    return 0;
}


int win32_abi_target_is_data(const char *dll_name, const void *target)
{
    if (!target) return 0;

    if (dll_name) {
        for (int i = 0; i < g_abi_count; i++) {
            if (ci_eq(g_abi[i].dll, dll_name)) {
                const WIN32_EXPORT *e = find_target_in_table(
                    g_abi[i].table, g_abi[i].count, target);
                if (e) return (e->cc & WIN32_EXPORT_DATA_FLAG) != 0;
                break;
            }
        }
    }

    for (int i = 0; i < g_abi_count; i++) {
        const WIN32_EXPORT *e = find_target_in_table(
            g_abi[i].table, g_abi[i].count, target);
        if (e) return (e->cc & WIN32_EXPORT_DATA_FLAG) != 0;
    }
    return 0;
}
