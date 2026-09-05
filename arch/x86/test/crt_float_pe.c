/* Native-reference differential probe for binary64 CRT formatting. */
typedef unsigned char BYTE;
typedef unsigned short WCHAR;
typedef unsigned int DWORD;
typedef unsigned long long U64;
typedef __SIZE_TYPE__ SIZE_T;
typedef __INTPTR_TYPE__ INTPTR;
typedef __builtin_va_list VA_LIST;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API const char *CALL GetCommandLineA(void);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API int CALL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
API HANDLE CALL CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
API int CALL CloseHandle(HANDLE);
API void CALL ExitProcess(DWORD);
int _fltused;

#ifdef TEST_LOCAL_FLOAT
#include "../win32/crt_float.h"
void *memset(void *buffer, int c, SIZE_T size)
{
    BYTE *p = buffer;
    while (size--) *p++ = (BYTE)c;
    return buffer;
}
#endif

typedef int (*FORMAT)(void *, SIZE_T, const void *, VA_LIST);
typedef int (*COMMON)(U64, void *, SIZE_T, const void *, void *, VA_LIST);
typedef unsigned (*CONTROL)(unsigned, unsigned);
static FORMAT legacy_a, legacy_w;
static COMMON common_a, common_w;
static CONTROL control;
static HANDLE reference;
static unsigned checks, failures, cases;
static unsigned report_limit = 20;
static int recording, ucrt, basic, local, file_failed;

static void report(const char *s)
{
    unsigned n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}

static void number(unsigned value)
{
    char digits[12]; unsigned count = 0;
    do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
    while (count) { char one[2] = {digits[--count], 0}; report(one); }
}

static void check(int ok, const char *name)
{
    checks++;
    if (ok) return;
    failures++;
    if (failures <= report_limit) { report("FAIL: "); report(name); report(" case="); number(cases); report("\n"); }
}

static int has(const char *s, const char *word)
{
    for (; s && *s; s++) {
        unsigned n = 0;
        while (word[n] && word[n] == s[n]) n++;
        if (!word[n]) return 1;
    }
    return 0;
}

static int equal(const void *a, const void *b, SIZE_T n)
{
    const BYTE *x = a, *y = b;
    for (SIZE_T i = 0; i < n; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static int transfer(void *buffer, unsigned bytes)
{
    unsigned done = 0;
    int ok = recording ? WriteFile(reference, buffer, bytes, &done, 0) :
                         ReadFile(reference, buffer, bytes, &done, 0);
    if (!ok || done != bytes) {
        if (!file_failed) check(0, "reference I/O");
        file_failed = 1;
        return 0;
    }
    return 1;
}

static int format(int wide, U64 options, void *buffer, SIZE_T count,
                   const void *spec, ...)
{
    VA_LIST args;
    __builtin_va_start(args, spec);
    int n = ucrt ? (wide ? common_w : common_a)(options | 2, buffer, count, spec, 0, args) :
                   (wide ? legacy_w : legacy_a)(buffer, count, spec, args);
    __builtin_va_end(args);
    return n;
}

#ifdef TEST_LOCAL_FLOAT
typedef struct { char data[4096]; unsigned used; } LOCAL_BUFFER;
static int local_write(void *opaque, const char *data, SIZE_T n)
{
    LOCAL_BUFFER *out = opaque;
    if (n > sizeof(out->data) - out->used - 1) return 0;
    for (SIZE_T i = 0; i < n; i++) out->data[out->used++] = data[i];
    out->data[out->used] = 0;
    return 1;
}
static int local_repeat(void *opaque, char c, int n)
{
    while (n-- > 0) if (!local_write(opaque, &c, 1)) return 0;
    return 1;
}
#endif

static void run_case(U64 bits, char type, int width, int precision,
                     unsigned flags, U64 options, unsigned rc)
{
    if (file_failed) return;
    unsigned exponent = (unsigned)(bits >> 52) & 0x7ff;
    if (basic && (precision > 32 || exponent > 1050 || exponent < 1000)) return;
    cases++;
    char spec[24]; WCHAR wide_spec[24]; unsigned pos = 0;
    spec[pos++] = '%';
    if (flags & 1) spec[pos++] = '-';
    if (flags & 2) spec[pos++] = '0';
    if (flags & 4) spec[pos++] = '+';
    if (flags & 8) spec[pos++] = ' ';
    if (flags & 16) spec[pos++] = '#';
    spec[pos++] = '*'; spec[pos++] = '.'; spec[pos++] = '*';
    spec[pos++] = type; spec[pos] = 0;
    for (unsigned i = 0; i <= pos; i++) wide_spec[i] = spec[i];
    union { U64 bits; double value; } value = {bits};
    unsigned saved = control(rc << 8, 0x300);
    (void)saved;
    for (unsigned wide = 0; wide < 2; wide++) {
        static WCHAR buffer[4098]; BYTE *raw = (BYTE *)buffer;
        static char ascii[4096], expected[4096];
        for (unsigned i = 0; i < sizeof(buffer); i++) raw[i] = 0x5a;
        void *destination = wide ? (void *)(buffer + 1) : (void *)(raw + 1);
        int n = format(wide, options, destination, 4096,
                       wide ? (const void *)wide_spec : (const void *)spec,
                       width, precision, value.value);
        check(n >= 0 && n < 4096, "conversion result");
        if (n < 0 || n >= 4096) return;
        int bounds = raw[0] == 0x5a && (!wide || raw[1] == 0x5a);
        for (unsigned i = 0; i <= (unsigned)n; i++) {
            WCHAR c = wide ? buffer[i + 1] : raw[i + 1];
            if (c > 127) bounds = 0;
            ascii[i] = (char)c;
        }
        for (unsigned i = (n + 2) * (wide ? 2 : 1); i < sizeof(buffer); i++)
            if (raw[i] != 0x5a) bounds = 0;
        check(bounds && ascii[n] == 0, "storage bounds and terminator");
        if (local) {
#ifdef TEST_LOCAL_FLOAT
            static LOCAL_BUFFER out;
            out.used = 0; out.data[0] = 0;
            CRT_FLOAT_SINK sink = {&out, local_write, local_repeat, 0};
            int actual = crt_float_format(&sink, bits, width, precision, flags,
                type, ucrt ? options : CRT_FLOAT_LEGACY, rc);
            int match = actual == n && out.used == (unsigned)n && equal(out.data, ascii, n);
            check(match, spec);
            if (!match && failures <= report_limit) {
                report("want="); report(ascii); report("\ngot="); report(out.data); report("\n");
            }
#endif
        } else {
            unsigned stored = n;
            if (!transfer(&stored, sizeof(stored))) return;
            if (stored >= sizeof(expected)) { check(0, "reference length"); file_failed = 1; return; }
            if (recording) {
                if (!transfer(ascii, stored)) return;
            } else {
                if (!transfer(expected, stored)) return;
                int match = stored == (unsigned)n && equal(expected, ascii, stored);
                check(match, spec);
                if (!match && failures <= report_limit) {
                    expected[stored] = 0;
                    report("want="); report(expected); report("\ngot="); report(ascii); report("\n");
                }
            }
        }
        check((control(0, 0) & 0x300) == rc << 8, "rounding control preserved");
    }
}

void mainCRTStartup(void)
{
    const char *command = GetCommandLineA();
    ucrt = has(command, "ucrt"); recording = has(command, "record");
    basic = has(command, "basic"); local = has(command, "local");
    if (has(command, "verbose")) report_limit = 200;
    HANDLE module = LoadLibraryA(ucrt ? "ucrtbase.dll" : "msvcrt.dll");
    legacy_a = (FORMAT)GetProcAddress(module, "_vsnprintf");
    legacy_w = (FORMAT)GetProcAddress(module, "_vsnwprintf");
    common_a = (COMMON)GetProcAddress(module, "__stdio_common_vsprintf");
    common_w = (COMMON)GetProcAddress(module, "__stdio_common_vswprintf");
    control = (CONTROL)GetProcAddress(module, "_controlfp");
    if (!control || (ucrt ? !common_a || !common_w : !legacy_a || !legacy_w)) {
        report("CRT-FLOAT missing exports\n"); ExitProcess(2);
    }
#ifndef TEST_LOCAL_FLOAT
    if (local) { report("CRT-FLOAT local converter not linked\n"); ExitProcess(2); }
#endif
    if (!local) {
        char name[] = "crt-float-ms64.ref";
        if (ucrt) { name[10] = 'u'; name[11] = 'c'; }
        if (sizeof(void *) == 4) { name[12] = '3'; name[13] = '2'; }
        reference = CreateFileA(name, recording ? 0x40000000U : 0x80000000U,
                                1, 0, recording ? 1 : 3, 0x80, 0);
        if (reference == (HANDLE)(INTPTR)-1) { report("CRT-FLOAT cannot open reference\n"); ExitProcess(2); }
        unsigned header[] = {0x464c5431, sizeof(void *), (unsigned)ucrt, (unsigned)basic};
        unsigned readback[4];
        for (unsigned i = 0; i < 4; i++) readback[i] = header[i];
        if (!transfer(readback, sizeof(readback)) || !equal(header, readback, sizeof(header))) {
            report("CRT-FLOAT incompatible reference\n"); CloseHandle(reference); ExitProcess(2);
        }
    }
    unsigned initial_control = control(0, 0);
    const U64 patterns[] = {
        0, 0x8000000000000000ULL, 0x4004000000000000ULL, 0xc004000000000000ULL,
        0x400c000000000000ULL, 0x3ff4000000000000ULL, 0x3ff6000000000000ULL,
        0x3fffe00000000000ULL, 0x3f1a36e2eb1c432dULL, 0x4415af1d78b58c40ULL,
        1, 0x0010000000000000ULL, 0x7fefffffffffffffULL,
        0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000001ULL,
        0x7ff0000000000001ULL, 0xfff8000000000000ULL
    };
    const char types[] = "fFeEgGaA";
    const int precisions[] = {-1, 0, 1, 2, 6, 17};
    const unsigned flag_sets[] = {0, 6, 17, 16, 8};
    for (unsigned profile = 0; profile < (ucrt ? 8U : 1U); profile++) {
        U64 options = profile << 3;
        for (unsigned v = 0; v < sizeof(patterns)/sizeof(*patterns); v++)
            for (unsigned t = 0; t < 8; t++)
                for (unsigned p = 0; p < sizeof(precisions)/sizeof(*precisions); p++)
                    run_case(patterns[v], types[t], (p & 1) ? 22 : 0, precisions[p],
                             flag_sets[(v + p + t) % 5], options, (v + p) & 3);
        for (unsigned p = 48; p <= 1200; p = p == 48 ? 80 : p == 80 ? 128 : p == 128 ? 1200 : 1201)
            for (unsigned t = 0; t < 8; t++)
                for (unsigned v = 0; v < 3; v++)
                    run_case(patterns[v == 0 ? 5 : v == 1 ? 10 : 12], types[t], 0, p, 16, options, 0);
        for (unsigned v = 13; v < 18; v++)
            for (unsigned rc = 0; rc < 4; rc++) {
                run_case(patterns[v], 'f', 22, 4, 0, options, rc);
                run_case(patterns[v], 'g', 22, 17, 0, options, rc);
            }
        unsigned seed = 0x734192;
        for (unsigned i = 0; i < 128; i++) {
            seed = seed * 1664525U + 1013904223U; U64 bits = (U64)seed << 32;
            seed = seed * 1664525U + 1013904223U; bits |= seed;
            run_case(bits, types[i % 8], 18, i % 24, flag_sets[i % 5], options, i % 4);
        }
    }
    control(initial_control, 0x300);
    if (!local) {
        if (!recording && !file_failed) {
            BYTE extra; unsigned bytes;
            check(ReadFile(reference, &extra, 1, &bytes, 0) && bytes == 0, "reference consumed exactly");
        }
        check(CloseHandle(reference), "close reference");
    }
    report("CRT-FLOAT "); number(cases); report(" cases, "); number(checks);
    report(" checks, "); number(failures); report(" failures\n");
    ExitProcess(failures ? 1 : 0);
}
