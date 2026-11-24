/* Run only inside Osito-K after the original DirectX setup has installed its
 * libraries. No Microsoft payload is part of this fixture or its build. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __SIZE_TYPE__ SIZE_T;
typedef int HRESULT;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API DWORD CALL GetModuleFileNameA(void *, char *, DWORD);
API int CALL FreeLibrary(void *);

typedef struct Blob Blob;
typedef struct {
    HRESULT (CALL *query)(Blob *, const void *, void **);
    DWORD (CALL *addref)(Blob *);
    DWORD (CALL *release)(Blob *);
    void *(CALL *pointer)(Blob *);
    SIZE_T (CALL *size)(Blob *);
} BlobVtable;
struct Blob { BlobVtable *vtable; };
typedef struct D3DXBuffer D3DXBuffer;
typedef struct {
    HRESULT (CALL *query)(D3DXBuffer *, const void *, void **);
    DWORD (CALL *addref)(D3DXBuffer *);
    DWORD (CALL *release)(D3DXBuffer *);
    void *(CALL *pointer)(D3DXBuffer *);
    DWORD (CALL *size)(D3DXBuffer *);
} D3DXBufferVtable;
struct D3DXBuffer { D3DXBufferVtable *vtable; };

#define SAY(text) do { const char *s_ = (text); DWORD n_ = 0, w_; \
    while (s_[n_]) n_++; \
    WriteFile(GetStdHandle((DWORD)-11), s_, n_, &w_, 0); \
} while (0)
#define CHECK(ok, label) do { int pass_ = !!(ok); \
    SAY(pass_ ? "DXSYSTEM PASS " label "\n" : "DXSYSTEM FAIL " label "\n"); \
    if (!pass_) failures++; \
} while (0)

void directx_system_entry(void)
{
    DWORD failures = 0;
    const char *names[] = {"d3dx9_43.dll", "D3DCompiler_43.dll", "XINPUT1_3.dll"};
    void *modules[3] = {0, 0, 0};
    const char *directory = sizeof(void *) == 4
        ? "C:\\Windows\\SysWOW64\\" : "C:\\Windows\\System32\\";
    for (DWORD i = 0; i < 3; i++) {
        SAY("DXSYSTEM LIBRARY "); SAY(names[i]); SAY("\n");
        modules[i] = LoadLibraryA(names[i]);
        CHECK(modules[i] != 0, "load-by-name");
        if (!modules[i]) continue;
        char path[260];
        DWORD n = GetModuleFileNameA(modules[i], path, sizeof(path));
        int correct = n > 0 && n < sizeof(path);
        for (DWORD j = 0; correct && directory[j]; j++) {
            unsigned char a = (unsigned char)path[j];
            unsigned char b = (unsigned char)directory[j];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (j >= n || a != b) correct = 0;
        }
        CHECK(correct, "system-directory-for-caller");
        if (n && n < sizeof(path)) {
            SAY("DXSYSTEM PATH "); SAY(path); SAY("\n");
        }
        const unsigned char *image = modules[i];
        DWORD offset = *(const DWORD *)(image + 0x3c);
        CHECK(image[0] == 'M' && image[1] == 'Z' && offset < 4096 &&
            *(const DWORD *)(image + offset) == 0x4550 &&
            *(const WORD *)(image + offset + 4) ==
                (sizeof(void *) == 4 ? 0x14c : 0x8664), "original-pe-architecture");
    }
    if (failures) ExitProcess(1);

    typedef HRESULT (CALL *buffer_fn)(DWORD, D3DXBuffer **);
    buffer_fn create = GetProcAddress(modules[0], "D3DXCreateBuffer");
    CHECK(create != 0, "d3dx-buffer-export");
    D3DXBuffer *buffer = 0;
    HRESULT status = create ? create(257, &buffer) : -1;
    CHECK(status >= 0 && buffer && buffer->vtable &&
        buffer->vtable->size(buffer) == 257 && buffer->vtable->pointer(buffer),
        "d3dx-owned-buffer");
    if (buffer) buffer->vtable->release(buffer);

    typedef HRESULT (CALL *compile_fn)(const void *, SIZE_T, const char *,
        const void *, void *, const char *, const char *, DWORD, DWORD,
        Blob **, Blob **);
    compile_fn compile = GetProcAddress(modules[1], "D3DCompile");
    CHECK(compile != 0, "compiler-export");
    const char source[] =
        "float4 main(float2 uv:TEXCOORD0):COLOR0{return float4(uv,0.25,1);}";
    Blob *shader = 0, *errors = 0;
    status = compile ? compile(source, sizeof(source) - 1, "global.hlsl", 0,
        0, "main", "ps_2_0", 0, 0, &shader, &errors) : -1;
    CHECK(status >= 0 && shader && shader->vtable &&
        shader->vtable->size(shader) >= 8 && shader->vtable->pointer(shader) &&
        *(const DWORD *)shader->vtable->pointer(shader) == 0xffff0200,
        "installed-compiler-produces-ps20-bytecode");
    if (errors) errors->vtable->release(errors);
    if (shader) shader->vtable->release(shader);
    CHECK(GetProcAddress(modules[2], "XInputGetState") != 0,
        "installed-xinput-export");
    for (DWORD i = 0; i < 3; i++)
        CHECK(FreeLibrary(modules[i]), "release-library");
    if (!failures) SAY("DXSYSTEM DONE\n");
    ExitProcess(failures ? 1 : 0);
}
