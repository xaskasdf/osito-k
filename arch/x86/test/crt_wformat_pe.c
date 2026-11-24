/* Exercise the legacy wide va_list contract independently of an application. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef __SIZE_TYPE__ SIZE_T;
typedef int BOOL;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#define W(s) ((const WCHAR *)L##s)
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API const char *CALL GetCommandLineA(void);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API void CALL ExitProcess(DWORD);
int _fltused;

typedef __builtin_va_list VA_LIST;
typedef int (*VFORMAT)(WCHAR *, SIZE_T, const WCHAR *, VA_LIST);
typedef int (*C_FORMAT)(WCHAR *, SIZE_T, const WCHAR *, void *, VA_LIST);
typedef int (*FORMAT)(WCHAR *, SIZE_T, const WCHAR *, ...);
static VFORMAT legacy;
static C_FORMAT terminated;
static unsigned checks, failures;

static void report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}

static void number(unsigned value)
{
    char digits[12]; unsigned count = 0;
    do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
    while (count) { char one[2] = {digits[--count], 0}; report(one); }
}

static void check(BOOL ok, const char *name, unsigned index)
{
    checks++;
    if (ok) return;
    failures++;
    report("FAIL: "); report(name); report(" case "); number(index); report("\n");
}

static unsigned length(const WCHAR *text)
{
    unsigned n = 0;
    while (text[n]) n++;
    return n;
}

static void expect(const WCHAR *expected, const WCHAR *format, ...)
{
    WCHAR buffer[160];
    unsigned n = length(expected), index = checks;
    for (unsigned i = 0; i < 160; i++) buffer[i] = 0x5a5a;
    VA_LIST ap;
    __builtin_va_start(ap, format);
    int result = legacy(buffer, 159, format, ap);
    __builtin_va_end(ap);
    check(result == (int)n, "formatted length", index);
    BOOL equal = 1;
    for (unsigned i = 0; i <= n; i++)
        if (buffer[i] != expected[i]) equal = 0;
    check(equal, "formatted value", index);
    if (!equal) {
        report("  output: ");
        for (unsigned i = 0; i < 159 && buffer[i]; i++) {
            char one[2] = {buffer[i] < 128 ? (char)buffer[i] : '?', 0};
            report(one);
        }
        report("\n");
    }
    check(buffer[n + 1] == 0x5a5a && buffer[159] == 0x5a5a,
          "format preserves surrounding storage", index);
}

static int format_v(WCHAR *buffer, SIZE_T count, const WCHAR *format, ...)
{
    VA_LIST ap;
    __builtin_va_start(ap, format);
    int result = legacy(buffer, count, format, ap);
    __builtin_va_end(ap);
    return result;
}

static int format_c(WCHAR *buffer, SIZE_T count, const WCHAR *format, ...)
{
    VA_LIST ap;
    __builtin_va_start(ap, format);
    int result = terminated(buffer, count, format, 0, ap);
    __builtin_va_end(ap);
    return result;
}

void mainCRTStartup(void)
{
    const char *command = GetCommandLineA(); BOOL ucrt = 0, basic = 0;
    for (; command && *command; command++) {
        if (command[0] == 'u' && command[1] == 'c' &&
            command[2] == 'r' && command[3] == 't') ucrt = 1;
        if (command[0] == 'b' && command[1] == 'a' &&
            command[2] == 's' && command[3] == 'i' && command[4] == 'c') basic = 1;
    }
    HANDLE module = LoadLibraryA(ucrt ? "ucrtbase.dll" : "msvcrt.dll");
    legacy = (VFORMAT)GetProcAddress(module, "_vsnwprintf");
    terminated = (C_FORMAT)GetProcAddress(module, "_vswprintf_c_l");
    FORMAT direct = (FORMAT)GetProcAddress(module, "_snwprintf");
    check(module && legacy && terminated && direct, "wide formatter exports", 0);
    if (!legacy || !terminated || !direct) ExitProcess(1);

    expect(W("90.000000"), W("%f"), 90.0);
    expect(W("7|90.000000|-1.250000|11"), W("%d|%f|%f|%u"), 7, 90.0, -1.25, 11U);
    expect(W("+000012.50| 3.25"), W("%+010.2f|% .2f"), 12.5, 3.25);
    expect(W("1.25      |7"), W("%*.*f|%d"), -10, 2, 1.25, 7);
    expect(W("  90.000000"), W("%*.6f"), 11, 90.0);
    expect(W("90.000000|-2.250000"), W("%.*f|%f"), -1, 90.0, -2.25);
    expect(W("4294967298|-7|4294967295|1.500000"), W("%I64u|%I32d|%u|%f"),
           4294967298ULL, -7, 0xffffffffU, 1.5);
    expect(W("  -12|0x2a|0011|%"), W("%5d|%#x|%.4u|%%"), -12, 42U, 11U);
    if (!basic) {
        expect(W("wide|ansi|tail|3.500000|23"), W("%s|%S|%ls|%f|%d"),
               W("wide"), "ansi", W("tail"), 3.5, 23);
        expect(W("   ab|xy   |Z"), W("%5.2s|%-5.2hs|%c"), W("abcd"), "xyz", 'Z');
    }

    /* Legacy output may fill count completely, without appending a NUL. */
    const WCHAR *expected = W("90.000000");
    for (unsigned count = 0; count <= 12; count++) {
        WCHAR buffer[16];
        for (unsigned i = 0; i < 16; i++) buffer[i] = 0x5a5a;
        int result = format_v(buffer + 1, count, W("%f"), 90.0);
        check(result == (count < 9 ? -1 : 9), "legacy bounded return", count);
        BOOL equal = buffer[0] == 0x5a5a;
        for (unsigned i = 0; i < 15; i++) {
            WCHAR want = i < count && i < 9 ? expected[i] :
                         i == 9 && i < count ? 0 : 0x5a5a;
            if (buffer[i + 1] != want) equal = 0;
        }
        check(equal, "legacy bounded data and canaries", count);
    }
    check(format_v(0, 0, W("%d|%f"), 7, 90.0) == 11,
          "legacy measure without destination", 0);

    WCHAR buffer[32];
    for (unsigned i = 0; i < 32; i++) buffer[i] = 0x5a5a;
    check(format_c(buffer, 9, W("%f"), 90.0) == -1 && buffer[8] == 0 &&
          buffer[9] == 0x5a5a, "C formatter retains terminating truncation", 0);
    check(direct(buffer, 32, W("%d|%.2f|%u"), 7, 1.25, 11U) == 9 &&
          buffer[2] == '1' && buffer[6] == '|' && buffer[9] == 0,
          "direct variadic formatter regression", 0);

    report("CRT-WFORMAT "); number(checks); report(" checks, ");
    number(failures); report(" failures\n");
    ExitProcess(failures ? 1 : 0);
}
