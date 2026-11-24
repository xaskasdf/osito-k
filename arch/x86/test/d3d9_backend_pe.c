/* Run on an Osito-K target without a D3D9 backend. */
typedef unsigned int UINT, DWORD;
typedef int BOOL, HRESULT;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API void CALL ExitProcess(UINT);
typedef void *(CALL *create_fn)(UINT);
typedef HRESULT (CALL *create_ex_fn)(UINT, void **);
static void report(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}
void mainCRTStartup(void)
{
    HANDLE module = LoadLibraryA("d3d9.dll");
    create_fn create = (create_fn)GetProcAddress(module, "Direct3DCreate9");
    create_ex_fn create_ex = (create_ex_fn)GetProcAddress(module, "Direct3DCreate9Ex");
    if (!create || !create_ex) {
        report("D3D9-BACKEND MISSING EXPORT\n");
        ExitProcess(2);
    }
    UINT failures = 0;
    const UINT versions[] = {32, 0, 0xFFFFFFFF};
    for (UINT round = 0; round < 100; round++) {
        for (UINT i = 0; i < 3; i++) {
            struct { void *object; DWORD guard; } result;
            result.object = (void *)1;
            result.guard = 0x13579BDF;
            if (create(versions[i]) != 0) failures++;
            if (create_ex(versions[i], &result.object) >= 0 || result.object ||
                result.guard != 0x13579BDF) failures++;
        }
    }
    if (create_ex(32, 0) >= 0) failures++;
    report(failures ? "D3D9-BACKEND FAIL\n" : "D3D9-BACKEND PASS\n");
    ExitProcess(failures ? 1 : 0);
}
