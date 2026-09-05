/* Probe printf destinations, va_list ABIs, and bounded-output policies. */
typedef unsigned char BYTE;
typedef unsigned short WCHAR;
typedef unsigned int DWORD;
typedef unsigned long long U64;
typedef __SIZE_TYPE__ SIZE_T;
typedef __INTPTR_TYPE__ INTPTR;
typedef int BOOL;
typedef void *HANDLE;
typedef __builtin_va_list VA_LIST;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API const char *CALL GetCommandLineA(void);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
API BOOL CALL CloseHandle(HANDLE);
API BOOL CALL DeleteFileA(const char *);
API BOOL CALL DuplicateHandle(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD);
API void CALL ExitProcess(DWORD);
int _fltused;

typedef int (*PRINT)(const char *, ...);
typedef int (*VPRINT)(const char *, VA_LIST);
typedef int (*FPRINT)(void *, const char *, ...);
typedef int (*VFPRINT)(void *, const char *, VA_LIST);
typedef int (*VSPRINT)(char *, const char *, VA_LIST);
typedef int (*SNPRINT)(char *, SIZE_T, const char *, ...);
typedef int (*VSNPRINT)(char *, SIZE_T, const char *, VA_LIST);
typedef int (*COMMON)(U64, void *, SIZE_T, const void *, void *, VA_LIST);
typedef int (*OPEN_HANDLE)(INTPTR, int);
typedef void *(*FDOPEN)(int, const char *);
typedef void *(*FOPEN)(const char *, const char *);
typedef int (*FILE_OP)(void *);
typedef int (*FSEEK)(void *, long, int);
typedef SIZE_T (*FREAD)(void *, SIZE_T, SIZE_T, void *);
typedef int (*FD_OP)(int);
typedef int (*DUP2)(int, int);
typedef int (*SETVBUF)(void *, char *, int, SIZE_T);

static PRINT output;
static VPRINT voutput;
static FPRINT foutput;
static VFPRINT vfoutput;
static VSPRINT vsoutput;
static SNPRINT snoutput;
static VSNPRINT vsnoutput;
static COMMON common_a, common_w;
static unsigned checks, failures;
static HANDLE report_handle;

static void report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(report_handle, text, length, &written, 0);
}

static void number(unsigned value)
{
    char digits[12]; unsigned count = 0;
    do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
    while (count) { char one[2] = {digits[--count], 0}; report(one); }
}

static void check(BOOL ok, const char *name)
{
    checks++;
    if (ok) return;
    failures++;
    report("FAIL: "); report(name); report("\n");
}

static unsigned length(const char *text)
{
    unsigned n = 0;
    while (text[n]) n++;
    return n;
}

static BOOL equal(const void *left, const void *right, SIZE_T count)
{
    const BYTE *a = left, *b = right;
    for (SIZE_T i = 0; i < count; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int call_vs(char *buffer, const char *format, ...)
{
    VA_LIST ap;
    __builtin_va_start(ap, format);
    int result = vsoutput(buffer, format, ap);
    __builtin_va_end(ap);
    return result;
}

static int call_vsn(char *buffer, SIZE_T size, const char *format, ...)
{
    VA_LIST ap;
    __builtin_va_start(ap, format);
    int result = vsnoutput(buffer, size, format, ap);
    __builtin_va_end(ap);
    return result;
}

static int call_vf(void *stream, const char *format, ...)
{
    VA_LIST ap;
    __builtin_va_start(ap, format);
    int result = vfoutput(stream, format, ap);
    __builtin_va_end(ap);
    return result;
}

static int call_vp(const char *format, ...)
{
    VA_LIST ap;
    __builtin_va_start(ap, format);
    int result = voutput(format, ap);
    __builtin_va_end(ap);
    return result;
}

static int call_common(COMMON fn, U64 options, void *buffer, SIZE_T size,
                       const void *format, ...)
{
    VA_LIST ap;
    __builtin_va_start(ap, format);
    int result = fn(options, buffer, size, format, 0, ap);
    __builtin_va_end(ap);
    return result;
}

static void expect_format(const char *expected, const char *format, ...)
{
    char buffer[160];
    for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0x5a;
    VA_LIST ap;
    __builtin_va_start(ap, format);
    int result = vsoutput ? vsoutput(buffer, format, ap) :
                 common_a(2, buffer, sizeof(buffer) - 1, format, 0, ap);
    __builtin_va_end(ap);
    unsigned n = length(expected);
    check(result == (int)n && equal(buffer, expected, n + 1), format);
    check(buffer[n + 1] == 0x5a && buffer[159] == 0x5a, "integer-format storage bounds");
}

static void test_buffers(BOOL basic)
{
    char buffer[160];
    if (vsoutput) {
        int n = call_vs(buffer, "%d|%.2f|%I64u|%u", 7, 1.25, 4294967298ULL, 23U);
        const char expected[] = "7|1.25|4294967298|23";
        check(n == sizeof(expected) - 1 && equal(buffer, expected, sizeof(expected)),
              "vsprintf mixed argument widths");
        if (!basic) {
            n = call_vs(buffer, "%s|%d|%s", "first", 17, "last");
            check(n == 13 && equal(buffer, "first|17|last", 14), "vsprintf pointer arguments");
        }
    }
    const char expected[] = "123/45/6";
    for (unsigned count = 0; count <= 11; count++) {
        for (unsigned api = 0; api < 2; api++) {
            if (api ? !vsnoutput : !snoutput) continue;
            for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0x5a;
            int n = api ? call_vsn(buffer + 1, count, "%d/%d/%d", 123, 45, 6) :
                          snoutput(buffer + 1, count, "%d/%d/%d", 123, 45, 6);
            check(n == (count < 8 ? -1 : 8), api ? "_vsnprintf return" : "_snprintf return");
            BOOL correct = buffer[0] == 0x5a;
            for (unsigned i = 0; i < sizeof(buffer) - 1; i++) {
                char want = i < count && i < 8 ? expected[i] :
                            i == 8 && i < count ? 0 : 0x5a;
                if (buffer[i + 1] != want) correct = 0;
            }
            check(correct, api ? "_vsnprintf bounds and terminator" : "_snprintf bounds and terminator");
        }
    }
    if (vsnoutput)
        check(call_vsn(0, 0, "%d/%d/%d", 123, 45, 6) == 8, "_vsnprintf measure");
    if (vsoutput || common_a) {
        expect_format("-7|4294967295|17|9", "%ld|%lu|%lo|%u", -7L, 0xffffffffUL, 15UL, 9U);
        expect_format("-1|1|23", "%hd|%hu|%u", 65535, 65537, 23U);
        if (common_a)
            expect_format("-1|1|23", "%hhd|%hhu|%u", 255, 257, 23U);
        expect_format("-17|17|11|9", "%Id|%Iu|%Ix|%u", (INTPTR)-17, (SIZE_T)17, (SIZE_T)17, 9U);
        expect_format("12   |0003  ", "%*d|%*.*u", -5, 12, -6, 4, 3U);
        expect_format("0x2a|0X2A|011|0", "%#x|%#X|%#.3o|%#.0o", 42U, 42U, 9U, 0U);
        expect_format("000011|0x00002a", "%#06o|%#08x", 9U, 42U);
        expect_format(sizeof(void *) == 8 ? "000000000000123A" : "0000123A",
                      "%p", (void *)(INTPTR)0x123a);
    }
}

static void test_common(COMMON fn, BOOL wide)
{
    const WCHAR format_w[] = {'%','d','|','%','.','2','f','|','%','d',0};
    const void *format = wide ? (const void *)format_w : "%d|%.2f|%d";
    const char expected[] = "7|1.25|9";
    for (unsigned policy = 1; policy <= 2; policy++) {
        for (unsigned count = 0; count <= 11; count++) {
            WCHAR buffer[20];
            BYTE *raw = (BYTE *)buffer;
            for (unsigned i = 0; i < sizeof(buffer); i++) raw[i] = 0x5a;
            void *out = wide ? (void *)(buffer + 1) : (void *)(raw + 1);
            int n = call_common(fn, policy, out, count, format, 7, 1.25, 9);
            check(n == (policy == 2 || count >= 8 ? 8 : -1),
                  wide ? "common wide return" : "common narrow return");
            unsigned unit = wide ? 2 : 1;
            BOOL correct = raw[0] == 0x5a && (!wide || raw[1] == 0x5a);
            for (unsigned i = 0; i < sizeof(buffer) / unit - 1; i++) {
                unsigned limit = policy == 1 ? count : count ? count - 1 : 0;
                unsigned end = 8 < limit ? 8 : limit;
                WCHAR want = i < limit && i < 8 ? (BYTE)expected[i] :
                    i == end && (policy == 2 ? count != 0 : 8 < count) ? 0 :
                    wide ? 0x5a5a : 0x5a;
                WCHAR value = wide ? buffer[i + 1] : raw[i + 1];
                if (value != want) correct = 0;
            }
            check(correct, wide ? "common wide bounded output" : "common narrow bounded output");
        }
    }
    check(call_common(fn, 2, 0, 0, format, 7, 1.25, 9) == 8,
          wide ? "common wide measurement" : "common narrow measurement");
}

static void test_streams(HANDLE module, BOOL basic)
{
    OPEN_HANDLE open_handle = (OPEN_HANDLE)GetProcAddress(module, "_open_osfhandle");
    FDOPEN fdopen = (FDOPEN)GetProcAddress(module, "_fdopen");
    FOPEN fopen = (FOPEN)GetProcAddress(module, "fopen");
    FILE_OP fclose = (FILE_OP)GetProcAddress(module, "fclose");
    FILE_OP fflush = (FILE_OP)GetProcAddress(module, "fflush");
    FILE_OP ferror = (FILE_OP)GetProcAddress(module, "ferror");
    FSEEK fseek = (FSEEK)GetProcAddress(module, "fseek");
    FREAD fread = (FREAD)GetProcAddress(module, "fread");
    FD_OP dup = (FD_OP)GetProcAddress(module, "_dup");
    FD_OP close = (FD_OP)GetProcAddress(module, "_close");
    DUP2 dup2 = (DUP2)GetProcAddress(module, "_dup2");
    SETVBUF setvbuf = (SETVBUF)GetProcAddress(module, "setvbuf");
    BOOL exports = open_handle && fdopen && fopen && fclose && fflush && ferror &&
                   fseek && fread && dup && close && dup2 && setvbuf;
    check(exports, "stream contract exports");
    if (!exports) return;

    const char name[] = "crt-format-contract.tmp";
    HANDLE handle = CreateFileA(name, 0xc0000000U, 3, 0, 1 /* CREATE_NEW */, 0x80, 0);
    check(handle != (HANDLE)(INTPTR)-1, "create owned stream fixture");
    if (handle == (HANDLE)(INTPTR)-1) return;
    int fd = open_handle((INTPTR)handle, 2 | 0x8000);
    check(fd >= 0, "adopt fixture handle");
    if (fd < 0) { CloseHandle(handle); DeleteFileA(name); return; }
    void *stream = fdopen(fd, "w+b");
    check(stream != 0, "create binary update stream");
    if (!stream) { close(fd); DeleteFileA(name); return; }
    check(setvbuf(stream, 0, 4 /* _IONBF */, 0) == 0, "unbuffered stream");

    char expected[1024]; unsigned used = 0;
    if (foutput) {
        check(foutput(stream, "first:%d", 17) == 8, "fprintf return");
        const char text[] = "first:17";
        for (unsigned i = 0; i < sizeof(text) - 1; i++) expected[used++] = text[i];
    }
    if (vfoutput) {
        check(call_vf(stream, ":%.2f:%u", 1.25, 23U) == 8, "vfprintf return");
        const char text[] = ":1.25:23";
        for (unsigned i = 0; i < sizeof(text) - 1; i++) expected[used++] = text[i];
    }
    if (foutput && !basic) {
        char text[641];
        for (unsigned i = 0; i < sizeof(text) - 1; i++) text[i] = 'a' + i % 26;
        text[640] = 0;
        check(foutput(stream, "%s", text) == 640, "fprintf crosses output chunks");
        for (unsigned i = 0; i < 640; i++) expected[used++] = text[i];
        check(foutput(stream, "%c", 0) == 1, "fprintf embedded NUL");
        expected[used++] = 0;
    }
    check(fflush(stream) == 0, "flush destination stream");

    int saved_stdout = dup(1);
    check(saved_stdout >= 0, "preserve stdout descriptor");
    if (saved_stdout >= 0) {
        BOOL redirected = dup2(fd, 1) == 0;
        check(redirected, "redirect stdout descriptor");
        if (redirected) {
            if (output) {
                check(output("stdout:%d", 8) == 8, "printf return");
                const char text[] = "stdout:8";
                for (unsigned i = 0; i < sizeof(text) - 1; i++) expected[used++] = text[i];
            }
            if (voutput) {
                check(call_vp("vstdout:%.2f", 1.25) == 12, "vprintf return");
                const char text[] = "vstdout:1.25";
                for (unsigned i = 0; i < sizeof(text) - 1; i++) expected[used++] = text[i];
            }
            if (vsnoutput)
                check(call_vsn(0, 0, "%d/%d", 1, 2) == 3, "measurement while stdout redirected");
            check(fflush(0) == 0, "flush before restoring stdout");
            check(dup2(saved_stdout, 1) == 0, "restore stdout descriptor");
        }
        check(close(saved_stdout) == 0, "release preserved stdout");
    }
    char actual[1024];
    check(fseek(stream, 0, 0) == 0, "rewind output stream");
    SIZE_T bytes = fread(actual, 1, sizeof(actual), stream);
    check(bytes == used && equal(actual, expected, used), "file receives exact output without measurement text");
    check(fclose(stream) == 0, "close output stream");

    stream = fopen(name, "rb");
    check(stream != 0, "open read-only stream");
    if (stream) {
        check(setvbuf(stream, 0, 4, 0) == 0, "unbuffered read-only stream");
        if (foutput) check(foutput(stream, "denied") < 0, "fprintf propagates output error");
        if (vfoutput) check(call_vf(stream, "%d", 3) < 0, "vfprintf propagates output error");
        if (foutput || vfoutput) check(ferror(stream) != 0, "stream error flag");
        check(fclose(stream) == 0, "close read-only stream");
    }
    check(DeleteFileA(name), "remove owned stream fixture");
}

void mainCRTStartup(void)
{
    report_handle = GetStdHandle((DWORD)-11);
    HANDLE duplicate = 0;
    BOOL owned_report = DuplicateHandle((HANDLE)(INTPTR)-1, report_handle,
        (HANDLE)(INTPTR)-1, &duplicate, 0, 0, 2);
    if (owned_report) report_handle = duplicate;
    BOOL ucrt = 0, basic = 0;
    const char *command = GetCommandLineA();
    for (; command && *command; command++) {
        if (command[0] == 'u' && command[1] == 'c' && command[2] == 'r' && command[3] == 't') ucrt = 1;
        if (command[0] == 'b' && command[1] == 'a' && command[2] == 's' && command[3] == 'i' && command[4] == 'c') basic = 1;
    }
    HANDLE module = LoadLibraryA(ucrt ? "ucrtbase.dll" : "msvcrt.dll");
    output = (PRINT)GetProcAddress(module, "printf");
    voutput = (VPRINT)GetProcAddress(module, "vprintf");
    foutput = (FPRINT)GetProcAddress(module, "fprintf");
    vfoutput = (VFPRINT)GetProcAddress(module, "vfprintf");
    vsoutput = (VSPRINT)GetProcAddress(module, "vsprintf");
    snoutput = (SNPRINT)GetProcAddress(module, "_snprintf");
    vsnoutput = (VSNPRINT)GetProcAddress(module, "_vsnprintf");
    common_a = (COMMON)GetProcAddress(module, "__stdio_common_vsprintf");
    common_w = (COMMON)GetProcAddress(module, "__stdio_common_vswprintf");
    check(module && (ucrt ? common_a && common_w :
          output && voutput && foutput && vfoutput && vsoutput && snoutput && vsnoutput),
          "provider formatting exports");
    report("CRT-FORMAT buffers\n");
    test_buffers(basic);
    /* The old PE32 common thunk loses a DWORD; run it only after correction. */
    if (!basic || sizeof(void *) == 8) {
        if (common_a) { report("CRT-FORMAT common narrow\n"); test_common(common_a, 0); }
        if (common_w) { report("CRT-FORMAT common wide\n"); test_common(common_w, 1); }
    }
    report("CRT-FORMAT streams\n");
    if (owned_report) test_streams(module, basic);
    else check(0, "independent diagnostic output handle");
    report("CRT-FORMAT "); number(checks); report(" checks, ");
    number(failures); report(" failures\n");
    if (owned_report) CloseHandle(report_handle);
    ExitProcess(failures ? 1 : 0);
}
