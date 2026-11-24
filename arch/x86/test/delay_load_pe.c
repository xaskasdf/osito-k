#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <delayloadhandler.h>
#include <stddef.h>
extern IMAGE_DOS_HEADER __ImageBase;
typedef PVOID (WINAPI *RESOLVE)(PVOID,PCIMAGE_DELAYLOAD_DESCRIPTOR,PDELAYLOAD_FAILURE_DLL_CALLBACK,
                               PVOID,PIMAGE_THUNK_DATA,ULONG);
static RESOLVE resolve;
static FARPROC (WINAPI *failure_hook)(LPCSTR,LPCSTR);
static IMAGE_DELAYLOAD_DESCRIPTOR descriptor;
static HMODULE cached;
static IMAGE_THUNK_DATA names[3],iat[3];
static struct { WORD hint; char name[64]; } imports[2];
static char dll_name[80];
static unsigned checks,failures,dll_calls,system_calls,dll_recover,system_recover;
static unsigned reason_seen,error_seen,size_seen,name_seen,module_seen,ordinal_seen;
static unsigned expect_default;
static char output[200]; static unsigned output_used;
static void number(unsigned value)
{for (int i=28;i>=0;i-=4) output[output_used++]="0123456789abcdef"[(value>>i)&15];output[output_used++]=' ';}
static void emit(const char *label)
{DWORD n=0,written;while(label[n])n++;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),label,n,&written,0);
 output[output_used-1]='\n';WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),output,output_used,&written,0);output_used=0;}
#define CHECK(x) do {checks++;if(!(x)){failures++;number(__LINE__);emit("DELAY FAIL ");}}while(0)
static void text(char *to,const char *from) {while((*to++=*from++));}
static DWORD rva(const void *p) {return (DWORD)((ULONG_PTR)p-(ULONG_PTR)&__ImageBase);}
static unsigned WINAPI recovered(void) {return 0x98765432;}
static unsigned WINAPI initial_stub(void) {return 0x11001100;}
static PVOID WINAPI dll_hook(ULONG reason,PDELAYLOAD_INFO info)
{
    dll_calls++;reason_seen=reason;error_seen=info->LastError;size_seen=info->Size;
    name_seen=info->TargetApiDescriptor.ImportDescribedByName;
    module_seen=info->TargetModuleBase!=NULL;ordinal_seen=info->TargetApiDescriptor.Description.Ordinal;
    CHECK(info->DelayloadDescriptor==&descriptor && info->ThunkAddress==iat);
    CHECK(info->TargetDllName==dll_name && info->Unused==NULL);
    if (name_seen) CHECK(info->TargetApiDescriptor.Description.Name==imports[0].name);
    return dll_recover ? (PVOID)recovered : NULL;
}
static PVOID WINAPI system_hook(LPCSTR dll,LPCSTR name)
{
    system_calls++;CHECK(dll==dll_name);
    CHECK(name==(LPCSTR)imports[0].name || (ULONG_PTR)name==7 || (ULONG_PTR)name==60000);
    return system_recover ? (PVOID)recovered : NULL;
}
static void prepare(const char *dll,const char *name)
{
    cached=NULL;dll_calls=system_calls=reason_seen=error_seen=size_seen=name_seen=module_seen=ordinal_seen=0;
    dll_recover=system_recover=0;text(dll_name,dll);text(imports[0].name,name);imports[0].hint=0;
    names[0].u1.AddressOfData=rva(&imports[0]);names[1].u1.AddressOfData=names[2].u1.AddressOfData=0;
    iat[0].u1.Function=(ULONG_PTR)initial_stub;iat[1].u1.Function=0x22222222;iat[2].u1.Function=0;
    descriptor=(IMAGE_DELAYLOAD_DESCRIPTOR){0};descriptor.Attributes.AllAttributes=1;
    descriptor.DllNameRVA=rva(dll_name);descriptor.ModuleHandleRVA=rva(&cached);
    descriptor.ImportAddressTableRVA=rva(iat);descriptor.ImportNameTableRVA=rva(names);
}
static LONG WINAPI fault(PEXCEPTION_POINTERS e)
{
    if (expect_default) {
        CHECK(e->ExceptionRecord->ExceptionCode==0x505 && (e->ExceptionRecord->ExceptionFlags&1));
        CHECK(e->ExceptionRecord->NumberParameters==2);
        CHECK(e->ExceptionRecord->ExceptionInformation[0]==(ULONG_PTR)dll_name);
        CHECK(e->ExceptionRecord->ExceptionInformation[1]==(ULONG_PTR)imports[0].name);
        number(checks);number(failures);emit("DELAY RESULT ");ExitProcess(failures ? 1 : 0);
    }
    number(e->ExceptionRecord->ExceptionCode);number((unsigned)(ULONG_PTR)e->ExceptionRecord->ExceptionAddress);
    number(e->ExceptionRecord->ExceptionFlags);number(e->ExceptionRecord->NumberParameters);
    number((unsigned)e->ExceptionRecord->ExceptionInformation[0]);number((unsigned)e->ExceptionRecord->ExceptionInformation[1]);
    emit("DELAY FAULT ");ExitProcess(99);
}
void delay_load_entry(void)
{
    AddVectoredExceptionHandler(1,fault);
    HMODULE k=LoadLibraryA("kernel32.dll");CHECK(k);
    resolve=(RESOLVE)GetProcAddress(k,"ResolveDelayLoadedAPI");
    failure_hook=(void *)GetProcAddress(k,"DelayLoadFailureHook");CHECK(resolve && failure_hook);
    if (!resolve || !failure_hook) ExitProcess(1);
    for(unsigned mode=0;mode<7;mode++) {
        number(mode);emit("DELAY BEGIN ");
        prepare(mode==3 || mode==4 ? "osito-delay-missing-81283.dll" : "delay-target.dll",
                mode>=2 ? "MissingDelayValue" : "DelayValue");
        if(mode==1 || mode==6) names[0].u1.Ordinal=IMAGE_ORDINAL_FLAG|(mode==1 ? 7 : 60000);
        dll_recover=mode==2 || mode==3 || mode==6;system_recover=mode==4 || mode==5;
        PVOID result=resolve(&__ImageBase,&descriptor,dll_hook,(PVOID)system_hook,iat,0);
        number(mode);number(result==(PVOID)recovered);number(iat[0].u1.Function==(ULONG_PTR)recovered);
        number(iat[0].u1.Function==(ULONG_PTR)initial_stub);number(result==NULL);emit("DELAY PATCH ");
        CHECK(result && iat[0].u1.Function==(ULONG_PTR)(mode>=2 && mode<=5 ? (PVOID)initial_stub : result) &&
              iat[1].u1.Function==0x22222222);
        if(result) CHECK(((unsigned(WINAPI *)(void))result)()==(mode==0 ? 0x12345678u : mode==1 ? 0xabcdef12u : 0x98765432u));
        if(mode<2) CHECK(!dll_calls && !system_calls && cached);
        else {
            CHECK(dll_calls==1 && system_calls==(mode==4 || mode==5) && reason_seen==4);
            CHECK(error_seen==(mode==3 || mode==4 ? 126u : mode==6 ? 182u : 127u));
            CHECK(size_seen==sizeof(DELAYLOAD_INFO) && name_seen==(mode!=6) &&
                  module_seen==(mode!=3 && mode!=4) && (mode!=6 || ordinal_seen==60000));
        }
        number(mode);number(dll_calls);number(system_calls);number(reason_seen);number(error_seen);
        number(size_seen);number(name_seen);number(module_seen);number(cached!=NULL);emit("DELAY ROW ");
        if(cached) CHECK(FreeLibrary(cached));
    }
    /* The second entry must reuse the cached module without rewriting the
     * first IAT entry; subsequent calls return the already bound exports. */
    prepare("delay-target.dll","DelayValue");
    PVOID first=resolve(&__ImageBase,&descriptor,dll_hook,(PVOID)system_hook,iat,0);
    CHECK(first && ((unsigned(WINAPI *)(void))first)()==0x12345678u);
    HMODULE first_module=cached;
    names[1].u1.Ordinal=IMAGE_ORDINAL_FLAG|7;iat[1].u1.Function=(ULONG_PTR)initial_stub;
    PVOID second=resolve(&__ImageBase,&descriptor,dll_hook,(PVOID)system_hook,iat+1,0);
    CHECK(second && ((unsigned(WINAPI *)(void))second)()==0xabcdef12u);
    CHECK(cached && cached==first_module && iat[0].u1.Function==(ULONG_PTR)first && iat[1].u1.Function==(ULONG_PTR)second);
    CHECK(resolve(&__ImageBase,&descriptor,dll_hook,(PVOID)system_hook,iat,0)==first);
    CHECK(resolve(&__ImageBase,&descriptor,dll_hook,(PVOID)system_hook,iat+1,0)==second);
    CHECK(!dll_calls && !system_calls);
    if(cached) CHECK(FreeLibrary(cached));
    expect_default=1;failure_hook(dll_name,imports[0].name);
    CHECK(0);ExitProcess(1);
}
