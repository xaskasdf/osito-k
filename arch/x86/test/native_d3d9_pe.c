/* Loader boundary tests. The fixture DLLs implement no graphics operations.
 * DXTEST_RUNTIME inspects an independently supplied Microsoft D3D9 runtime.
 * DXTEST_ABSENT is only for a guest disk without installed runtime DLLs. */
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)

#if defined(DXTEST_DLL)
#ifdef DXTEST_MISSING_IMPORT
API DWORD CALL OsitoMissingDriverContract(void);
#endif
__declspec(dllexport) DWORD CALL dx_loader_identity(void)
{
#ifdef DXTEST_MISSING_IMPORT
    return OsitoMissingDriverContract();
#else
    return DXTEST_DLL;
#endif
}
#else
API HANDLE CALL LoadLibraryA(const char *);
API HANDLE CALL GetModuleHandleA(const char *);
API BOOL CALL GetModuleHandleExA(DWORD, const char *, HANDLE *);
API DWORD CALL GetModuleFileNameA(HANDLE, char *, DWORD);
API void *CALL GetProcAddress(HANDLE, const char *);
API BOOL CALL FreeLibrary(HANDLE);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API void CALL ExitProcess(DWORD);
API DWORD CALL SetErrorMode(DWORD);
#ifdef DXTEST_STATIC
API DWORD CALL dx_loader_identity(void);
#endif
#define REPORT(text) do { static const char msg[] = text; DWORD written; \
    WriteFile(GetStdHandle((DWORD)-11),msg,sizeof(msg)-1,&written,0); } while (0)
#define VALUE(label, value) do { DWORD number=(DWORD)(value),written; \
    char digits[9]; REPORT(label); \
    for(DWORD k=0;k<8;k++)digits[k]="0123456789abcdef"[(number>>((7-k)*4))&15]; \
    digits[8]='\n'; WriteFile(GetStdHandle((DWORD)-11),digits,9,&written,0); } while (0)
#define CHECK(test,label) do { if(!(test)){REPORT("DXLOADER FAIL " label "\n");failures++;} \
    else {REPORT("DXLOADER PASS " label "\n");} } while(0)

void native_d3d9_entry(void)
{
    DWORD failures=0;
    SetErrorMode(0x8001); /* Missing test imports must not open host dialogs. */
#ifdef DXTEST_STATIC
    CHECK(dx_loader_identity()==DXTEST_STATIC,"static-import-file-identity");
#endif
    static const char *names[]={"d3d9.dll","dxgi.dll"};
#ifdef DXTEST_RUNTIME
    const DWORD count=1;
#else
    const DWORD count=2;
#endif
    for(DWORD i=0;i<count;i++) {
        HANDLE module=LoadLibraryA(names[i]);
        DWORD error=GetLastError();
        VALUE("DXLOADER index=",i);
        VALUE("DXLOADER load-error=",module?0:error);
#if defined(DXTEST_ABSENT) || defined(DXTEST_INCOMPLETE)
#ifdef DXTEST_INCOMPLETE
        CHECK(!module && error==127,"missing-required-import");
#else
        CHECK(!module && error==126,"absent-runtime");
#endif
        CHECK(!GetModuleHandleA(names[i]),"no-synthetic-module");
#else
        CHECK(module!=0,"installed-runtime");
        if(module) {
            unsigned char *base=module;
            DWORD pe_offset=*(DWORD *)(base+0x3c);
            CHECK(base[0]=='M' && base[1]=='Z' && pe_offset<4096,"mapped-image");
            if(pe_offset<4096) {
                DWORD size=*(DWORD *)(base+pe_offset+24+56);
#ifdef DXTEST_RUNTIME
                void *entry=GetProcAddress(module,"Direct3DCreate9");
                VALUE("DXLOADER timestamp=",*(DWORD *)(base+pe_offset+8));
                VALUE("DXLOADER image-size=",size);
                CHECK(entry!=0,"runtime-factory-export");
                HANDLE owner=0;
                CHECK(entry && GetModuleHandleExA(6,(const char *)entry,&owner),
                      "runtime-factory-owner");
                if(owner) {
                    char path[260]; DWORD written;
                    DWORD n=GetModuleFileNameA(owner,path,sizeof(path));
                    CHECK(n>0 && n<sizeof(path),"runtime-factory-path");
                    if(n>0 && n<sizeof(path)) {
                        REPORT("DXLOADER factory-owner=");
                        WriteFile(GetStdHandle((DWORD)-11),path,n,&written,0);
                        REPORT("\n");
                    }
                }
#else
                void *entry=GetProcAddress(module,"dx_loader_identity");
                CHECK(entry && (unsigned char *)entry>=base &&
                      (unsigned char *)entry<base+size,"export-in-image");
                if(entry) {
                    DWORD (CALL *identity)(void)=entry;
                    CHECK(identity()==0x44580009U+i,"installed-file-identity");
                }
#endif
            }
        }
#endif
        if(module)CHECK(FreeLibrary(module),"release-runtime");
    }
    CHECK(!GetModuleHandleA("d3d9-provider.dll"),"no-renderer-substitute");
    CHECK(!GetModuleHandleA("wined3d.dll"),"no-wined3d");
    VALUE("DXLOADER DONE failures=",failures);
    ExitProcess(failures?1:0);
}
#endif
