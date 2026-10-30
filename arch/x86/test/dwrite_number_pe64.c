/* PE64 COM ABI; this same binary also checks the NONE contract on Windows. */
typedef unsigned int UINT, DWORD;
typedef int BOOL, HRESULT;
typedef unsigned short WCHAR;
typedef void *HANDLE;
#define API __declspec(dllimport)
API HANDLE GetStdHandle(DWORD);
API BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE LoadLibraryA(const char *);
API void *GetProcAddress(HANDLE, const char *);
API void ExitProcess(UINT);
typedef struct { UINT a; unsigned short b, c; unsigned char d[8]; } GUID;
typedef HRESULT (*factory_fn)(UINT, const GUID *, void **);
typedef HRESULT (*create_fn)(void *, UINT, const WCHAR *, BOOL, void **);
typedef HRESULT (*query_fn)(void *, const GUID *, void **);
typedef UINT (*ref_fn)(void *);
static UINT failures;
static const GUID factory_iid = {
    0xB859EE5A, 0xD838, 0x4B5B, {0xA2,0xE8,0x1A,0xDC,0x7D,0x93,0xDB,0x48}
};
static const GUID number_iid = {
    0x14885CC9, 0xBAB0, 0x4F90, {0xB6,0xED,0x5C,0x36,0x6A,0x2C,0xD0,0x3D}
};
static const GUID unknown_iid = {0,0,0,{0xC0,0,0,0,0,0,0,0x46}};
static void report(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}
static void check(BOOL condition, const char *name)
{
    if (!condition) { failures++; report("FAIL "); report(name); report("\n"); }
}
void mainCRTStartup(void)
{
    HANDLE module = LoadLibraryA("dwrite.dll");
    factory_fn get_factory = (factory_fn)GetProcAddress(module, "DWriteCreateFactory");
    void *factory = 0;
    if (!get_factory || get_factory(0, &factory_iid, &factory) < 0 || !factory)
        ExitProcess(2);
    create_fn create = (create_fn)(*(void ***)factory)[22];
    static const WCHAR en[] = {'e','n','-','U','S',0}, empty[] = {0};
    static const WCHAR ar[] = {'a','r','-','S','A',0};
    static const WCHAR invalid[] = {'n','o','t','_','a','_','l','o','c','a','l','e',0};
    const WCHAR *locales[] = {en, empty, ar, invalid, 0};
    for (UINT round = 0; round < 32; round++) {
        for (UINT i = 0; i < 5; i++) {
            void *number = 0;
            HRESULT status = create(factory, 2, locales[i], round & 1, &number);
            if (status < 0 || !number) {
                report("DWRITE-NUMBER CREATE FAILED\n");
                ExitProcess(1);
            }
            query_fn query = (query_fn)(*(void ***)number)[0];
            ref_fn addref = (ref_fn)(*(void ***)number)[1];
            ref_fn release = (ref_fn)(*(void ***)number)[2];
            void *identity = 0, *unknown = 0, *missing = (void *)1;
            check(addref(number) == 2, "initial reference and AddRef");
            check(release(number) == 1, "balanced Release");
            check(query(number, &number_iid, &identity) == 0 && identity == number,
                  "number identity");
            check(query(number, &unknown_iid, &unknown) == 0 && unknown == number,
                  "IUnknown identity");
            check(query(number, &factory_iid, &missing) == (HRESULT)0x80004002 && !missing,
                  "unknown interface clears output");
            if (identity) check(release(identity) == 2, "QI owns number reference");
            if (unknown) check(release(unknown) == 1, "QI owns IUnknown reference");
            check(release(number) == 0, "final release");
        }
    }
    void *invalid_method = (void *)1;
    check(create(factory, 5, en, 1, &invalid_method) == (HRESULT)0x80070057 &&
          !invalid_method, "invalid method clears output");
    ((ref_fn)(*(void ***)factory)[2])(factory);
    report(failures ? "DWRITE-NUMBER FAIL\n" : "DWRITE-NUMBER PASS\n");
    ExitProcess(failures ? 1 : 0);
}
