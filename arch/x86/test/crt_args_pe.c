/* Compare the shared bounded parser with the installed MSVCRT, or exercise
 * Osito's exported startup functions. No redistributable runtime is bundled. */
#include "../win32/crt_args.h"
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long,
                                           unsigned long *, void *);
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) const char *__stdcall GetCommandLineA(void);
__declspec(dllimport) const uint16_t *__stdcall GetCommandLineW(void);
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned);

static uint16_t strings[CRT_ARGS_MAX_UNITS];
static CRT_ARG_RECORD records[CRT_ARGS_MAX_UNITS];

void crt_args_test_entry(void)
{
    void *crt = LoadLibraryA("msvcrt.dll");
    typedef int (__cdecl *args_fn)(int *, void *, void *, int, void *);
    struct { int new_mode; } startup = {0};
    unsigned failures = 0;
    for (unsigned width = 1; width <= 2; ++width) {
        const void *cmd = width == 1 ? (const void *)GetCommandLineA()
                                    : (const void *)GetCommandLineW();
        args_fn get = (args_fn)GetProcAddress(crt, width == 1 ? "__getmainargs"
                                                             : "__wgetmainargs");
        CRT_ARG_COUNTS counts;
        if (crt_args_parse(cmd, CRT_ARGS_MAX_UNITS, width, strings,
                           CRT_ARGS_MAX_UNITS, records, CRT_ARGS_MAX_UNITS,
                           &counts)) { ++failures; continue; }
        uint64_t argv = 0xcccccccc00000000ULL, env = argv;
        int argc = -1;
        int status = get ? get(&argc, &argv, &env, 0, &startup) : -1;
        if (status || argc != (int)counts.argc || !((uintptr_t)argv) ||
            (sizeof(void *) == 4 &&
             (argv >> 32 != 0xcccccccc || env >> 32 != 0xcccccccc))) {
            ++failures; continue;
        }
        void **actual = (void **)(uintptr_t)argv;
        for (unsigned i = 0; i < counts.argc; ++i) {
            if (!actual[i]) { ++failures; continue; }
            for (unsigned j = 0; j <= counts.units; ++j) {
                unsigned want = width == 1 ?
                    ((uint8_t *)strings)[records[i].offset + j] :
                    strings[records[i].offset + j];
                unsigned got = width == 1 ? ((uint8_t *)actual[i])[j] :
                                             ((uint16_t *)actual[i])[j];
                if (got != want) { ++failures; break; }
                if (!want) break;
            }
        }
        failures += actual[counts.argc] != 0;
        failures += !(uintptr_t)env;
    }
    const char *message = failures ? "CRT ARGS FAIL\r\n" : "CRT ARGS PASS\r\n";
    unsigned long written;
    WriteFile(GetStdHandle((unsigned long)-11), message, 15, &written, 0);
    ExitProcess(failures ? 1 : 0);
}
