/* Only the small Osito-built fixture DLLs are used by the native oracle. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL LoadLibraryW(const WCHAR *);
API void *CALL LoadLibraryExA(const char *, void *, DWORD);
API void *CALL LoadLibraryExW(const WCHAR *, void *, DWORD);
API void *CALL GetProcAddress(void *, const char *);
API int CALL FreeLibrary(void *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
#define SAY(text) do { const char *s_ = (text); DWORD n_ = 0, w_; \
    while (s_[n_]) n_++; \
    WriteFile(GetStdHandle((DWORD)-11), s_, n_, &w_, 0); \
} while (0)
#define CHECK(ok, label) do { int pass_ = !!(ok); \
    SAY(pass_ ? "MODNAME PASS " label "\n" : "MODNAME FAIL " label "\n"); \
    if (!pass_) failures++; \
} while (0)
void module_name_entry(void)
{
    DWORD failures = 0;
    const char *names[] = {"xinput-probe.dll", "XINPUT1_3.dll",
        "dinput-probe.dll", "hidapi.dll", "Xinput-osito-missing.dll",
        "Dinput-osito-missing.dll"};
    for (DWORD i = 0; i < 6; i++) {
        WCHAR wide[64];
        DWORD j = 0;
        do { wide[j] = (unsigned char)names[i][j]; } while (names[i][j++]);
        SAY("MODNAME LIBRARY "); SAY(names[i]); SAY("\n");
        for (DWORD mode = 0; mode < 4; mode++) {
            SetLastError(0x13579);
            void *module = mode == 0 ? LoadLibraryA(names[i]) :
                mode == 1 ? LoadLibraryW(wide) :
                mode == 2 ? LoadLibraryExA(names[i], 0, 0) :
                            LoadLibraryExW(wide, 0, 0);
            DWORD error = GetLastError();
            if (i >= 4) {
                CHECK(!module && error == 126, "missing-file-reports-module-not-found");
                if (module) FreeLibrary(module);
                continue;
            }
            CHECK(module != 0, "present-file-is-loadable");
            typedef DWORD (*value_fn)(void);
            value_fn value = module ? GetProcAddress(module, "system_path_probe_value") : 0;
            CHECK(value && value() == sizeof(void *) * 8, "real-fixture-export");
            CHECK(module && FreeLibrary(module), "release-present-file");
        }
    }
    if (!failures) SAY("MODNAME DONE\n");
    ExitProcess(failures ? 1 : 0);
}
