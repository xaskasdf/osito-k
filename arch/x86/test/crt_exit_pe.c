__declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long, unsigned long *, void *);
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned);
typedef int (__cdecl *register_fn)(void (__cdecl *)(void));
static register_fn register_exit;
static unsigned count, failures, final_exit;
#define CHECK(x) do { if (!(x)) ++failures; } while (0)
static void __cdecl callback_first(void)
{
    CHECK(count == (final_exit ? 83u : 82u));
    if (final_exit) {
        const char *text = failures ? "CRT EXIT FAIL\r\n" : "CRT EXIT PASS\r\n";
        unsigned long written;
        WriteFile(GetStdHandle((unsigned long)-11), text, 15, &written, 0);
    } else ++count;
}
static void __cdecl callback_many(void) { ++count; }
static void __cdecl callback_added(void) { CHECK(count == 1); ++count; }
static void __cdecl callback_last(void)
{
    CHECK(!count); ++count;
    CHECK(!register_exit(callback_added));
}
void crt_exit_test_entry(void)
{
    void *crt = LoadLibraryA("msvcrt.dll");
    register_exit = (register_fn)GetProcAddress(crt, "atexit");
    void (__cdecl *cleanup)(void) = (void *)GetProcAddress(crt, "_cexit");
    void (__cdecl *quick)(void) = (void *)GetProcAddress(crt, "_c_exit");
    void (__cdecl *terminate)(int) = (void *)GetProcAddress(crt, "exit");
    if (!register_exit || !cleanup || !quick || !terminate) ExitProcess(2);
    CHECK(!register_exit(callback_first));
    for (unsigned i = 0; i < 80; ++i) CHECK(!register_exit(callback_many));
    CHECK(!register_exit(callback_last));
    quick(); CHECK(!count);
    cleanup(); CHECK(count == 83);
    cleanup(); CHECK(count == 83);
    final_exit = 1;
    CHECK(!register_exit(callback_first));
    terminate(failures ? 1 : 0);
    ExitProcess(3);
}
