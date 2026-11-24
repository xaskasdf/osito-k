/* Identical dynamic-ABI fixture runs on native Windows and Osito-K. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
#ifdef _WIN64
typedef unsigned long long UINT_PTR;
#else
typedef unsigned long UINT_PTR;
#endif
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API void CALL ExitProcess(DWORD);
typedef WCHAR *(CALL *case_fn)(WCHAR *);
typedef DWORD (CALL *buffer_fn)(WCHAR *, DWORD);
static void report(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}
static const struct { WCHAR input, upper, lower; } cases[] = {
    {0, 0, 0}, {'a', 'A', 'a'}, {'Z', 'Z', 'z'}, {'i', 'I', 'i'},
    {0xE9, 0xC9, 0xE9}, {0xC9, 0xC9, 0xE9}, {0xFF, 0x178, 0xFF},
    {0x178, 0x178, 0xFF}, {0xDF, 0xDF, 0xDF}, {0x131, 0x131, 0x131},
    {0x130, 0x130, 0x130}, {0x3C3, 0x3A3, 0x3C3}, {0x3C2, 0x3C2, 0x3C2},
    {0x436, 0x416, 0x436}, {0x416, 0x416, 0x436},
    {0xD800, 0xD800, 0xD800}, {0xDFFF, 0xDFFF, 0xDFFF},
    {0xFFFF, 0xFFFF, 0xFFFF}, {0x4E00, 0x4E00, 0x4E00}
};
static WCHAR string[] = {'a', 0xE9, 0xD801, 0xDC28, 0, 'q'};
static const WCHAR string_upper[] = {'A', 0xC9, 0xD801, 0xDC00, 0, 'q'};
static const WCHAR string_lower[] = {'a', 0xE9, 0xD801, 0xDC28, 0, 'q'};
static WCHAR buffer[] = {'a', 0, 'b', 0xD801, 0xDC28, 'z', 0xD801, 'q', 0xDFFF};
static const WCHAR buffer_upper[] = {'A', 0, 'B', 0xD801, 0xDC00, 'Z', 0xD801, 'Q', 0xDFFF};
static const WCHAR buffer_lower[] = {'a', 0, 'b', 0xD801, 0xDC28, 'z', 0xD801, 'q', 0xDFFF};

void mainCRTStartup(void)
{
    void *module = LoadLibraryA("user32.dll");
    case_fn upper = GetProcAddress(module, "CharUpperW");
    case_fn lower = GetProcAddress(module, "CharLowerW");
    buffer_fn upper_buffer = GetProcAddress(module, "CharUpperBuffW");
    buffer_fn lower_buffer = GetProcAddress(module, "CharLowerBuffW");
    if (!upper || !lower || !upper_buffer || !lower_buffer) {
        report("USER32-CASE MISSING EXPORT\n");
        ExitProcess(2);
    }
    DWORD failures = 0;
    for (unsigned round = 0; round < 100; round++) {
        for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
            if ((UINT_PTR)upper((WCHAR *)(UINT_PTR)cases[i].input) != cases[i].upper) failures++;
            if ((UINT_PTR)lower((WCHAR *)(UINT_PTR)cases[i].input) != cases[i].lower) failures++;
        }
        if (upper(string) != string) failures++;
        for (unsigned i = 0; i < sizeof(string) / sizeof(*string); i++)
            if (string[i] != string_upper[i]) failures++;
        if (lower(string) != string) failures++;
        for (unsigned i = 0; i < sizeof(string) / sizeof(*string); i++)
            if (string[i] != string_lower[i]) failures++;
        if (upper_buffer(buffer, 0) != 0 || buffer[0] != 'a') failures++;
        if (upper_buffer(buffer, 9) != 9) failures++;
        for (unsigned i = 0; i < 9; i++)
            if (buffer[i] != buffer_upper[i]) failures++;
        if (lower_buffer(buffer, 9) != 9) failures++;
        for (unsigned i = 0; i < 9; i++)
            if (buffer[i] != buffer_lower[i]) failures++;
        /* A counted range ending on a high surrogate cannot touch its partner. */
        if (upper_buffer(buffer + 3, 1) != 1 || buffer[3] != 0xD801 ||
            buffer[4] != 0xDC28) failures++;
    }
    report(failures ? "USER32-CASE FAIL\n" : "USER32-CASE PASS\n");
    ExitProcess(failures ? 1 : 0);
}
