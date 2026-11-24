/* Probe an independently supplied original runtime. No runtime code is
 * embedded or linked, and no replacement factories are provided here. */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#define SAY(text) do { static const char message[]=text; DWORD written; \
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message,sizeof(message)-1,&written,NULL); } while(0)
#define HEX(label,value) do { DWORD v=(DWORD)(value),written; char hex[9]; SAY(label); \
    for (unsigned i=0;i<8;i++) hex[i]="0123456789abcdef"[(v>>(28-i*4))&15]; \
    hex[8]='\n'; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),hex,9,&written,NULL); } while(0)

void native_runtime_entry(void)
{
    SetErrorMode(0x8003);
    char path[512]; UINT n;
#ifdef RUNTIME_HOST
    n=GetSystemDirectoryA(path,sizeof(path));
    if (!n || n>sizeof(path)-12) ExitProcess(1);
    path[n++]='\\';
#else
    n=GetModuleFileNameA(NULL,path,sizeof(path));
    if (!n || n>sizeof(path)-12) ExitProcess(1);
    while (n && path[n-1]!='/' && path[n-1]!='\\') n--;
#endif
    const char name[]="d3d9.dll";
    for (unsigned i=0;i<sizeof(name);i++) path[n+i]=name[i];
    SAY("NATIVE-RUNTIME load-begin\n");
    HMODULE dll=LoadLibraryA(path);
    HEX("NATIVE-RUNTIME load-error=",dll ? 0 : GetLastError());
    if (!dll) { SAY("NATIVE-RUNTIME unavailable-load\n"); ExitProcess(2); }
    UINT length=GetModuleFileNameA(dll,path,sizeof(path));
    if (!length || length>=sizeof(path)) ExitProcess(1);
    DWORD written; SAY("NATIVE-RUNTIME owner=");
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),path,length,&written,NULL); SAY("\n");
    IDirect3D9 *(WINAPI *create)(UINT)=(void *)GetProcAddress(dll,"Direct3DCreate9");
    if (!create) { HEX("NATIVE-RUNTIME export-error=",GetLastError()); FreeLibrary(dll); ExitProcess(3); }
    HMODULE owner=NULL;
    BOOL found=GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (const char *)create,&owner);
    HEX("NATIVE-RUNTIME factory-owner-error=",found ? 0 : GetLastError());
    HEX("NATIVE-RUNTIME factory-address-low=",(UINT_PTR)create);
    HEX("NATIVE-RUNTIME module-address-low=",(UINT_PTR)dll);
    if (found) {
        length=GetModuleFileNameA(owner,path,sizeof(path));
        if (!length || length>=sizeof(path)) ExitProcess(1);
        SAY("NATIVE-RUNTIME factory-owner=");
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),path,length,&written,NULL); SAY("\n");
    } else { SAY("NATIVE-RUNTIME unknown-factory-owner\n"); ExitProcess(1); }
#ifndef RUNTIME_LOAD_ONLY
    SAY("NATIVE-RUNTIME factory-begin\n");
    IDirect3D9 *d3d=create(D3D_SDK_VERSION);
    if (!d3d) { SAY("NATIVE-RUNTIME unavailable-factory\n"); FreeLibrary(dll); ExitProcess(4); }
    UINT count=IDirect3D9_GetAdapterCount(d3d); HEX("NATIVE-RUNTIME adapters=",count);
    if (count>16) { SAY("NATIVE-RUNTIME invalid-adapter-count\n"); ExitProcess(1); }
    for (UINT i=0;i<count;i++) {
        D3DADAPTER_IDENTIFIER9 identity={0}; D3DDISPLAYMODE mode={0}; D3DCAPS9 caps={0};
        HRESULT result=IDirect3D9_GetAdapterIdentifier(d3d,i,0,&identity);
        HEX("NATIVE-RUNTIME identifier-status=",result);
        if (SUCCEEDED(result)) { HEX("NATIVE-RUNTIME vendor=",identity.VendorId); HEX("NATIVE-RUNTIME device=",identity.DeviceId); }
        result=IDirect3D9_GetAdapterDisplayMode(d3d,i,&mode); HEX("NATIVE-RUNTIME mode-status=",result);
        if (SUCCEEDED(result)) {HEX("NATIVE-RUNTIME width=",mode.Width);HEX("NATIVE-RUNTIME height=",mode.Height);}
        result=IDirect3D9_GetDeviceCaps(d3d,i,D3DDEVTYPE_HAL,&caps); HEX("NATIVE-RUNTIME caps-status=",result);
        if (SUCCEEDED(result)) {HEX("NATIVE-RUNTIME pixel-version=",caps.PixelShaderVersion);HEX("NATIVE-RUNTIME vertex-version=",caps.VertexShaderVersion);}
    }
    IDirect3D9_Release(d3d);
#endif
    FreeLibrary(dll); SAY("NATIVE-RUNTIME DONE\n"); ExitProcess(0);
}
