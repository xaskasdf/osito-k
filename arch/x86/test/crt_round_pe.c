/* Validate rounding values, control state and the PE32 ST(0) return ABI. */
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API const char *CALL GetCommandLineA(void);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API void CALL ExitProcess(DWORD);
int _fltused;
typedef union { double value; unsigned long long bits; } FP_BITS;
typedef double (*ROUND)(double);
typedef double (*PARSE)(const char *, char **);
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
static BOOL same(FP_BITS actual, FP_BITS expected)
{
    if ((expected.bits & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL)
        return (actual.bits & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL;
    return actual.bits == expected.bits;
}
static FP_BITS round_call(ROUND function, FP_BITS input, unsigned short control,
                         unsigned index)
{
    FP_BITS result;
    unsigned short after;
    /* Reset between cases so a missing PE32 result cannot spoil later checks. */
    __asm__ volatile ("fninit; fldcw %0" : : "m"(control) : "memory");
#ifdef _WIN64
    result.value = function(input.value);
#else
    const double sentinel = 123.25;
    double remaining;
    __asm__ volatile (
        "fldl %[sentinel]; push %[high]; push %[low]; "
        "call *%[function]; add $8, %%esp; fstpl %[result]; fstpl %[remaining]"
        : [result] "=m"(result.value), [remaining] "=m"(remaining)
        : [function] "r"(function), [low] "r"((DWORD)input.bits),
          [high] "r"((DWORD)(input.bits >> 32)), [sentinel] "m"(sentinel)
        : "eax", "ecx", "edx", "st", "st(1)", "st(2)", "st(3)",
          "st(4)", "st(5)", "st(6)", "st(7)", "xmm0", "xmm1",
          "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "cc", "memory");
    check(remaining == sentinel, "rounding preserves live x87 value", index);
#endif
    __asm__ volatile ("fnstcw %0" : "=m"(after));
    check(after == control, "rounding preserves x87 control", index);
    return result;
}
void mainCRTStartup(void)
{
    const char *command = GetCommandLineA(); BOOL ucrt = 0;
    for (; command && *command; command++) {
        if (command[0] == 'u' && command[1] == 'c' &&
            command[2] == 'r' && command[3] == 't') { ucrt = 1; break; }
    }
    HANDLE module = LoadLibraryA(ucrt ? "ucrtbase.dll" : "msvcrt.dll");
    ROUND up = (ROUND)GetProcAddress(module, "ceil");
    ROUND down = (ROUND)GetProcAddress(module, "floor");
    PARSE parse = (PARSE)GetProcAddress(module, "strtod");
    check(module && up && down && parse, "floating-point exports", 0);
    if (!up || !down || !parse) ExitProcess(1);
    static const struct { FP_BITS input, up, down; } cases[] = {
        {{.value = 0.0}, {.value = 0.0}, {.value = 0.0}},
        {{.value = -0.0}, {.value = -0.0}, {.value = -0.0}},
        {{.value = 1.25}, {.value = 2.0}, {.value = 1.0}},
        {{.value = -1.25}, {.value = -1.0}, {.value = -2.0}},
        {{.value = 0.25}, {.value = 1.0}, {.value = 0.0}},
        {{.value = -0.25}, {.value = -0.0}, {.value = -1.0}},
        {{.bits = 1}, {.value = 1.0}, {.value = 0.0}},
        {{.bits = 0x8000000000000001ULL}, {.value = -0.0}, {.value = -1.0}},
        {{.value = 0x1.fffffffffffffp51}, {.value = 0x1p52}, {.value = 0x1.ffffffffffffep51}},
        {{.value = -0x1.fffffffffffffp51}, {.value = -0x1.ffffffffffffep51}, {.value = -0x1p52}},
        {{.value = 0x1p64}, {.value = 0x1p64}, {.value = 0x1p64}},
        {{.value = -0x1p64}, {.value = -0x1p64}, {.value = -0x1p64}},
        {{.bits = 0x7FEFFFFFFFFFFFFFULL}, {.bits = 0x7FEFFFFFFFFFFFFFULL}, {.bits = 0x7FEFFFFFFFFFFFFFULL}},
        {{.bits = 0xFFEFFFFFFFFFFFFFULL}, {.bits = 0xFFEFFFFFFFFFFFFFULL}, {.bits = 0xFFEFFFFFFFFFFFFFULL}},
        {{.bits = 0x7FF0000000000000ULL}, {.bits = 0x7FF0000000000000ULL}, {.bits = 0x7FF0000000000000ULL}},
        {{.bits = 0xFFF0000000000000ULL}, {.bits = 0xFFF0000000000000ULL}, {.bits = 0xFFF0000000000000ULL}},
        {{.bits = 0x7FF8000000000001ULL}, {.bits = 0x7FF8000000000001ULL}, {.bits = 0x7FF8000000000001ULL}},
        {{.bits = 0xFFF0000000000001ULL}, {.bits = 0xFFF0000000000001ULL}, {.bits = 0xFFF0000000000001ULL}},
    };
    unsigned short original;
    __asm__ volatile ("fnstcw %0" : "=m"(original));
    for (unsigned pc = 0; pc < 3; pc++) {
        for (unsigned rc = 0; rc < 4; rc++) {
            unsigned short control = 0x7F | ((pc ? pc + 1 : 0) << 8) | (rc << 10);
            for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
                check(same(round_call(up, cases[i].input, control, i), cases[i].up), "ceil value", i);
                check(same(round_call(down, cases[i].input, control, i), cases[i].down), "floor value", i);
            }
        }
    }
    __asm__ volatile ("fninit; fldcw %0" : : "m"(original) : "memory");
    static const struct { const char *text; FP_BITS value; unsigned end; } parses[] = {
        {" -1.25e3!", {.value = -1250.0}, 8},
        {"1e+", {.value = 1.0}, 1},
        {"foo", {.value = 0.0}, 0},
        {"-0.0!", {.value = -0.0}, 4},
    };
    for (unsigned i = 0; i < sizeof(parses) / sizeof(parses[0]); i++) {
        char *end = 0;
        FP_BITS result = {.value = parse(parses[i].text, &end)};
        check(same(result, parses[i].value), "strtod return regression", i);
        check(end == parses[i].text + parses[i].end, "strtod end pointer", i);
    }
    report("CRT-ROUND "); number(checks); report(" checks, ");
    number(failures); report(" failures\n");
    ExitProcess(failures ? 1 : 0);
}
