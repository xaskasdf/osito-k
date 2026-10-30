/* Diagnose compiler startup prerequisites without relying on CRT startup. */
#include <stdint.h>
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long,
                                           unsigned long *, void *);
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned);

static void log_value(const char *label, unsigned value)
{
    char line[100];
    unsigned length = 0;
    while (label[length] && length < 80) {
        line[length] = label[length];
        ++length;
    }
    for (int shift = 28; shift >= 0; shift -= 4)
        line[length++] = "0123456789abcdef"[(value >> shift) & 15];
    line[length++] = '\r'; line[length++] = '\n';
    unsigned long written;
    WriteFile(GetStdHandle((unsigned long)-11), line, length, &written, 0);
}

void compiler_crt_entry(void)
{
    void *crt = LoadLibraryA("msvcrt.dll");
    void *synch = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");
    typedef int (__cdecl *args_fn)(int *, void *, void *, int, void *);
    args_fn narrow = (args_fn)GetProcAddress(crt, "__getmainargs");
    args_fn wide = (args_fn)GetProcAddress(crt, "__wgetmainargs");
    struct { int new_mode; } startup = {0};
    /* Oversized cells detect a PE32 shim writing kernel-width pointers without
     * corrupting this probe's neighboring stack variables. */
    uint64_t argv = 0xcccccccc00000000ULL, env = argv;
    int argc = -1;
    unsigned failures = 0;
    int status = narrow ? narrow(&argc, &argv, &env, 0, &startup) : -1;
    log_value("COMPILER CRT narrow status=", (unsigned)status);
    log_value("COMPILER CRT narrow argc=", (unsigned)argc);
    failures += status != 0 || argc != 3;
    unsigned narrow_width = sizeof(void *) == 8 ||
        (argv >> 32 == 0xcccccccc && env >> 32 == 0xcccccccc);
    log_value("COMPILER CRT narrow width=", narrow_width);
    failures += !narrow_width;
    argc = -1; argv = env = 0xcccccccc00000000ULL;
    status = wide ? wide(&argc, &argv, &env, 0, &startup) : -1;
    log_value("COMPILER CRT wide status=", (unsigned)status);
    log_value("COMPILER CRT wide argc=", (unsigned)argc);
    failures += status != 0 || argc != 3;
    unsigned wide_width = sizeof(void *) == 8 ||
        (argv >> 32 == 0xcccccccc && env >> 32 == 0xcccccccc);
    log_value("COMPILER CRT wide width=", wide_width);
    failures += !wide_width;
    const char *names[] = {"putc", "WaitOnAddress", "WakeByAddressSingle"};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned present = GetProcAddress(i ? synch : crt, names[i]) != 0;
        log_value(names[i], present);
        failures += !present;
    }
    log_value("COMPILER CRT failures=", failures);
    ExitProcess(failures ? 1 : 0);
}
