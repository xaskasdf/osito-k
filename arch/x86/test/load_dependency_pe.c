/* A failing dependency must prevent the dependent DLL's attach callback. */
#include "load_dependency_common.h"
static DWORD checks,failures;
static void say(const char *s){DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(DWORD v){char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[v&15];v>>=4;}s[8]=0;say(s);}
static void check(int ok,const char *label){checks++;if(!ok){failures++;say("LOADDEP FAIL ");say(label);say("\n");}}
void load_dependency_entry(void)
{
    char name[80];dependency_name(name);
    void *event=CreateEventA(0,1,0,name);
    check(event!=0,"attach marker event");
    const struct {const char *name;DWORD error;} cases[]={
        {"dependency-fail.dll",1114},
#ifndef LOADDEP_NO_FORWARD
        {"dependency-forward.dll",1114},
#endif
#ifdef LOADDEP_OOM
        {"dependency-oom.dll",8},
#endif
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
        for(unsigned repeat=0;repeat<3;repeat++){
            check(ResetEvent(event),"reset attach marker");
            say("LOADDEP LOAD ");say(cases[i].name);say("\n");
            SetLastError(0x12345);
            void *module=LoadLibraryA(cases[i].name);DWORD error=GetLastError();
            check(module==0,"dependent DLL rejected");
            check(error==cases[i].error,"dependency error preserved");
            say("LOADDEP ERROR=");hex(error);say("\n");
            check(WaitForSingleObject(event,0)==258,"dependent attach not called");
            if(module)FreeLibrary(module);
            check(GetModuleHandleA(cases[i].name)==0,"failed dependent unpublished");
        }
    }
    ResetEvent(event);
    void *good=LoadLibraryA("dependency-good.dll");
    check(good!=0,"healthy dependency loads after failures");
    if(good){
        DWORD (*value)(void)=(void *)GetProcAddress(good,"dependency_value");
        check(value&&value()==0x12345678,"healthy dependency executes");
        check(WaitForSingleObject(event,0)==0,"healthy dependent attach called");
        check(FreeLibrary(good),"healthy dependent unloads");
    }
    void *ntdll=GetModuleHandleA("ntdll.dll");
    DWORD (CALL *dos_error)(DWORD)=(void *)GetProcAddress(ntdll,"RtlNtStatusToDosError");
    check(dos_error!=0,"status translator available");
    if(dos_error){
        const DWORD statuses[]={0xc0000017,0xc0000142,0xc0000135,0xc000007a,0xc0000001};
        const DWORD errors[]={8,1114,126,127,31};
        for(unsigned i=0;i<5;i++)check(dos_error(statuses[i])==errors[i],"status translation");
    }
    if(event)CloseHandle(event);
    say("LOADDEP DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");
    ExitProcess(failures?1:0);
}
