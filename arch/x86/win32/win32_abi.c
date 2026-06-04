/*
 * OsitoK Windows Compatibility Layer — Co-located ABI descriptor registry +
 * MSVC name demangler. See win32_abi.h for the rationale (NT .thk model).
 */
#include "win32_abi.h"

extern void serial_puts(const char *s);

/* ── Registry: dll_name → co-located export table ───────────────── */

#define WIN32_ABI_MAX_DLLS 24

static struct {
    const char         *dll;
    const WIN32_EXPORT *table;
    int                 count;
} g_abi[WIN32_ABI_MAX_DLLS];
static int g_abi_count;

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
    if (g_abi_count >= WIN32_ABI_MAX_DLLS || !dll_name || !table) return;
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

/* Advance *pp past a `<name>@@`-terminated qualified name. 1 ok / 0 malformed. */
static int skip_qual_name(const char **pp)
{
    const char *q = *pp;
    while (q[0]) {
        if (q[0] == '?' && q[1] == '$') return 0;   /* template — bail */
        if (q[0] == '@' && q[1] == '@') { *pp = q + 2; return 1; }
        q++;
    }
    return 0;
}

/* Skip a full type token (size irrelevant — used for pointer/ref pointees).
 * Returns 1 ok / 0 if unparseable. */
static int skip_type(const char **pp)
{
    const char *p = *pp;
    char c = *p;
    if (!c) return 0;
    switch (c) {
        /* primitives (single char) */
        case 'X': case 'D': case 'C': case 'E': case 'F': case 'G':
        case 'H': case 'I': case 'J': case 'K': case 'M': case 'N': case 'O':
            *pp = p + 1; return 1;
        case '_':                                /* extended primitive */
            if (!p[1]) return 0;
            *pp = p + 2; return 1;
        case 'P': case 'Q': case 'R': case 'S':  /* pointer */
        case 'A': case 'B': {                    /* reference */
            p++;
            if (*p == '6' || *p == '7') return 0;/* ptr-to-function — bail */
            if (*p >= 'A' && *p <= 'Z') p++;     /* cv qualifier */
            else if (*p == '_') return 0;
            *pp = p;
            return skip_type(pp);                /* skip pointee */
        }
        case 'V': case 'U': case 'T':            /* class/struct/union */
            p++; *pp = p;
            return skip_qual_name(pp);
        case 'W':                                /* enum: W<digit><name>@@ */
            p++;
            if (*p) p++;                          /* underlying-type digit */
            *pp = p;
            return skip_qual_name(pp);
        default:
            return 0;
    }
}

/* Size (in 32-bit DWORDs) of a top-level argument type; advances *pp.
 * Returns 1 ok / 0 if ambiguous (by-value user type) or unparseable. */
static int arg_size(const char **pp, int *dw)
{
    const char *p = *pp;
    switch (*p) {
        case 'N':                                /* double */
            *dw = 2; *pp = p + 1; return 1;
        case 'O':                                /* long double (x86 80-bit) */
            *dw = 3; *pp = p + 1; return 1;
        case 'D': case 'C': case 'E': case 'F': case 'G':
        case 'H': case 'I': case 'J': case 'K': case 'M':
            *dw = 1; *pp = p + 1; return 1;
        case '_':                                /* bool/wchar=1, int64/uint64=2 */
            if (p[1] == 'J' || p[1] == 'K') { *dw = 2; *pp = p + 2; return 1; }
            if (p[1] == 'N' || p[1] == 'W') { *dw = 1; *pp = p + 2; return 1; }
            return 0;
        case 'P': case 'Q': case 'R': case 'S':  /* pointer = 1 DWORD */
        case 'A': case 'B':                      /* reference = 1 DWORD */
            *dw = 1;
            return skip_type(pp);
        case 'W':                                /* enum = int */
            *dw = 1;
            return skip_type(pp);
        case 'V': case 'U': case 'T':            /* by-value struct — unknown */
            return 0;
        default:
            return 0;
    }
}

int msvc_demangle_abi(const char *s, uint8_t *out_argc, uint8_t *out_cc)
{
    if (!s || s[0] != '?') return 0;
    const char *p = s + 1;

    if (*p == '?') {                              /* ??x operator/special */
        p++;
        if (*p) p++;                              /* op code char */
    }
    if (!skip_qual_name(&p)) return 0;            /* skip up to `@@` */

    char kind = *p++;
    if (kind != 'Y') return 0;                    /* only free functions; members → miss */

    uint8_t cc;
    if (!cc_from_code(*p, &cc)) return 0;
    p++;

    if (!skip_type(&p)) return 0;                 /* skip return type */

    int argc = 0;
    if (*p == 'X') {                              /* (void) */
        p++;
    } else {
        while (*p && *p != 'Z') {                 /* 'Z' alone = ellipsis/end */
            if (*p == '@') { p++; break; }        /* end of arg list */
            int dw;
            if (!arg_size(&p, &dw)) return 0;     /* ambiguous → bail */
            argc += dw;
            if (argc > 64) return 0;              /* sanity */
        }
    }

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
                if (e) { *out_argc = e->argc; *out_cc = e->cc; return 1; }
                break;
            }
        }
    }

    /* 2. any registered table (api-ms-win-crt-* redirections, fallback resolve) */
    for (int i = 0; i < g_abi_count; i++) {
        const WIN32_EXPORT *e =
            find_in_table(g_abi[i].table, g_abi[i].count, func_name);
        if (e) { *out_argc = e->argc; *out_cc = e->cc; return 1; }
    }

    /* 3. MSVC C++ mangled name → exact argc + cc */
    if (func_name[0] == '?')
        return msvc_demangle_abi(func_name, out_argc, out_cc);

    return 0;
}
