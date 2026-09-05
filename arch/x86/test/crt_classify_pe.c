/* Call CRT classifiers through the PE32 and PE64 floating-point ABIs. */
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

typedef int (*CLASSIFY)(double);
typedef union { double value; unsigned long long bits; } FP_BITS;
static unsigned checks, failures;

static void report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}

static void number(unsigned value)
{
    char digits[12];
    unsigned count = 0;
    do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
    while (count) {
        char one[2] = {digits[--count], 0};
        report(one);
    }
}

static void check(BOOL ok, const char *name, unsigned index)
{
    checks++;
    if (ok) return;
    failures++;
    report("FAIL: "); report(name); report(" case "); number(index); report("\n");
}

static int classify(CLASSIFY function, unsigned long long bits,
                    unsigned long long xmm_bits, unsigned index)
{
#ifdef _WIN64
    FP_BITS input = {.bits = bits};
    (void)xmm_bits;
    (void)index;
    return function(input.value);
#else
    FP_BITS poison = {.bits = xmm_bits};
    const double sentinel = 123.25;
    double remaining;
    int result;
    /* A PE32 double is on the integer stack. XMM0 is unrelated caller state. */
    __asm__ volatile (
        "fldl %[sentinel]; movsd %[poison], %%xmm0; "
        "push %[high]; push %[low]; call *%[function]; add $8, %%esp; "
        "fstpl %[remaining]"
        : "=a"(result), [remaining] "=m"(remaining)
        : [function] "r"(function), [low] "r"((DWORD)bits),
          [high] "r"((DWORD)(bits >> 32)), [poison] "m"(poison.value),
          [sentinel] "m"(sentinel)
        : "ecx", "edx", "st", "st(1)", "st(2)", "st(3)",
          "st(4)", "st(5)", "st(6)", "st(7)", "xmm0", "xmm1",
          "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "cc", "memory");
    check(remaining == sentinel, "classifier preserves live x87 value", index);
    return result;
#endif
}

void mainCRTStartup(void)
{
    const char *command = GetCommandLineA();
    BOOL ucrt = 0;
    for (; command && *command; command++) {
        if (command[0] == 'u' && command[1] == 'c' &&
            command[2] == 'r' && command[3] == 't') {
            ucrt = 1;
            break;
        }
    }
    HANDLE module = LoadLibraryA(ucrt ? "ucrtbase.dll" : "msvcrt.dll");
    CLASSIFY isnan = (CLASSIFY)GetProcAddress(module, "_isnan");
    CLASSIFY finite = (CLASSIFY)GetProcAddress(module, "_finite");
    check(module && isnan && finite, "classifier exports", 0);
    if (!isnan || !finite) ExitProcess(1);

    static const struct { unsigned long long bits; BOOL nan, finite; } cases[] = {
        {0x0000000000000000ULL, 0, 1}, {0x8000000000000000ULL, 0, 1},
        {0x0000000000000001ULL, 0, 1}, {0x8000000000000001ULL, 0, 1},
        {0x000FFFFFFFFFFFFFULL, 0, 1}, {0x800FFFFFFFFFFFFFULL, 0, 1},
        {0x0010000000000000ULL, 0, 1}, {0x8010000000000000ULL, 0, 1},
        {0x3FF0000000000000ULL, 0, 1}, {0xBFF0000000000000ULL, 0, 1},
        {0x7FEFFFFFFFFFFFFFULL, 0, 1}, {0xFFEFFFFFFFFFFFFFULL, 0, 1},
        {0x7FF0000000000000ULL, 0, 0}, {0xFFF0000000000000ULL, 0, 0},
        {0x7FF8000000000000ULL, 1, 0}, {0xFFF8000000000000ULL, 1, 0},
        {0x7FF0000000000001ULL, 1, 0}, {0xFFF0000000000001ULL, 1, 0},
        {0x7FF0000100000000ULL, 1, 0}, {0xFFF0000100000000ULL, 1, 0},
        {0x7FFFFFFFFFFFFFFFULL, 1, 0}, {0xFFFFFFFFFFFFFFFFULL, 1, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        check((classify(isnan, cases[i].bits, 0, i) != 0) == cases[i].nan,
              "isnan with unrelated zero XMM0", i);
        check((classify(isnan, cases[i].bits, 0x7FF8000000000000ULL, i) != 0)
                  == cases[i].nan,
              "isnan with unrelated NaN XMM0", i);
        check((classify(finite, cases[i].bits, 0x7FF8000000000000ULL, i) != 0)
                  == cases[i].finite,
              "finite regression", i);
    }
    report("CRT-CLASSIFY "); number(checks); report(" checks, ");
    number(failures); report(" failures\n");
    ExitProcess(failures ? 1 : 0);
}
