/* Native/guest DLL reload, reference and failed-attach cleanup regression. */
#include "dll_lifecycle_common.h"
static unsigned failures;
#define SAY(s) do{DWORD written;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&written,0);}while(0)
static void lifecycle_check(int ok,const char *name)
{
    if(ok)SAY("LIFECYCLE PASS ");else{SAY("LIFECYCLE FAIL ");failures++;}
    DWORD n=0,written;while(name[n])n++;WriteFile(GetStdHandle((DWORD)-11),name,n,&written,0);SAY("\n");
}
static DWORD CALL lifecycle_worker(void *argument)
{
    (void)argument;return 77;
}
void lifecycle_entry(void)
{
    const char *libraries[]={"lifecycle-good.dll","lifecycle-fail.dll",
        "lifecycle-static-good.dll","lifecycle-static-fail.dll"};
    for(unsigned variant=0;variant<4;variant++){
        unsigned kind=variant&1;
        const char *library=libraries[variant];
        char name[64];lifecycle_name(name,kind);
        for(unsigned repeat=0;repeat<4;repeat++){
            SetLastError(0x12345);void *module=LoadLibraryA(library);DWORD error=GetLastError();
            if(kind){
                lifecycle_check(!module,"failed-attach-returns-null");
                lifecycle_check(error==1114,"failed-attach-error-1114");
                if(module)FreeLibrary(module);
            }else{
                lifecycle_check(module!=0,"load-or-reload");
                if(module){
                    DWORD (*value)(void)=(void *)GetProcAddress(module,"lifecycle_value");
                    lifecycle_check(value&&value()==0x12345678,"initialized-dynamic-tls");
                    void *again=LoadLibraryA(library);
                    lifecycle_check(again==module,"second-reference");
                    lifecycle_check(again&&FreeLibrary(again),"release-one-reference");
                    lifecycle_check(GetModuleHandleA(library)==module,"module-retained");
                    void *mutex=CreateMutexA(0,0,name);
                    lifecycle_check(mutex&&GetLastError()==183,"singleton-retained");if(mutex)CloseHandle(mutex);
                    lifecycle_check(FreeLibrary(module),"final-release");
                }
            }
            lifecycle_check(GetModuleHandleA(library)==0,"module-removed");
            void *mutex=CreateMutexA(0,0,name);
            lifecycle_check(mutex&&GetLastError()!=183,"process-detach-released-singleton");if(mutex)CloseHandle(mutex);
            /* A stale TLS descriptor would read the unmapped DLL's template
             * when initializing this new thread, even without callbacks. */
            void *thread=CreateThread(0,0,lifecycle_worker,0,0,0);DWORD code=0;
            lifecycle_check(thread!=0,"thread-after-unload-created");
            lifecycle_check(thread&&WaitForSingleObject(thread,5000)==0&&
                GetExitCodeThread(thread,&code)&&code==77,"thread-after-unload-returned");
            if(thread)CloseHandle(thread);
        }
    }
    if(!failures)SAY("LIFECYCLE DONE\n");ExitProcess(failures?1:0);
}
