/* Runs custom DLLs only. The case table records installed Windows regsvr32. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API DWORD CALL GetModuleFileNameA(void *,char *,DWORD);
API DWORD CALL GetSystemDirectoryW(WORD *,DWORD);
API int CALL SetCurrentDirectoryA(const char *);
API int CALL SetEnvironmentVariableA(const char *,const char *);
API int CALL DeleteFileA(const char *);
API int CALL CreateProcessW(const WORD *,WORD *,void *,void *,int,DWORD,void *,const WORD *,void *,void *);
API DWORD CALL WaitForSingleObject(void *,DWORD);
API int CALL GetExitCodeProcess(void *,DWORD *);
API int CALL CloseHandle(void *);
API void *CALL CreateFileA(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
API int CALL ReadFile(void *,void *,DWORD,DWORD *,void *);
API int CALL TerminateProcess(void *,DWORD);
#define SAY(s) do {DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do {char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
typedef struct {DWORD cb;WORD *reserved,*desktop,*title;DWORD x,y,w,h,xc,yc,fill,flags;
    WORD show,bytes;BYTE *data;void *input,*output,*error;} STARTUP;
typedef struct {void *process,*thread;DWORD pid,tid;} PROCESS;
struct regsvr_case {const WORD *arguments;const char *fail,*false_result;DWORD status;const char *events;};
#include "regsvr_cases.h"
static char directory[1024],events[1200];
static WORD application[512],command[2048];
static unsigned failures;
static void regsvr_test_check(int ok)
{if(ok)SAY("REGSVR PASS\n");else{failures++;SAY("REGSVR FAIL\n");}}

void regsvr_test_entry(void)
{
    DWORD length=GetModuleFileNameA(0,directory,sizeof(directory));
    regsvr_test_check(length>0&&length<sizeof(directory));
    if(!length||length>=sizeof(directory))ExitProcess(1);
    while(length&&directory[length-1]!='\\'&&directory[length-1]!='/')length--;
    directory[length]=0;
    regsvr_test_check(SetCurrentDirectoryA(directory));
    length=GetSystemDirectoryW(application,480);
    regsvr_test_check(length>0&&length<480);
    if(!length||length>=480)ExitProcess(1);
    const char *suffix="\\regsvr32.exe";while(*suffix)application[length++]=(WORD)*suffix++;
    application[length]=0;
    for(unsigned i=0;i<sizeof(regsvr_cases)/sizeof(regsvr_cases[0]);i++) {
        const struct regsvr_case *test=&regsvr_cases[i];
        DeleteFileA("regsvr-events.log");
        regsvr_test_check(SetEnvironmentVariableA("OSITO_REGSVR_FAIL",test->fail)&&
                          SetEnvironmentVariableA("OSITO_REGSVR_S_FALSE",test->false_result));
        unsigned n=0;command[n++]='"';
        for(unsigned j=0;application[j];j++)command[n++]=application[j];
        command[n++]='"';command[n++]=' ';
        for(unsigned j=0;test->arguments[j];j++)command[n++]=test->arguments[j];
        command[n]=0;
        STARTUP start;PROCESS child;
        for(unsigned j=0;j<sizeof(start);j++)((BYTE *)&start)[j]=0;
        for(unsigned j=0;j<sizeof(child);j++)((BYTE *)&child)[j]=0;
        start.cb=sizeof(start);
        int created=CreateProcessW(application,command,0,0,0,0,0,0,&start,&child);
        regsvr_test_check(created);if(!created)break;
        DWORD waited=WaitForSingleObject(child.process,30000);
        regsvr_test_check(waited==0);
        if(waited!=0){TerminateProcess(child.process,99);WaitForSingleObject(child.process,5000);}
        DWORD status=0x12345678;
        regsvr_test_check(GetExitCodeProcess(child.process,&status)&&status==test->status);
        regsvr_test_check(CloseHandle(child.thread)&&CloseHandle(child.process));
        DWORD bytes=0;events[0]=0;
        void *file=CreateFileA("regsvr-events.log",0x80000000,3,0,3,0x80,0);
        if(file!=(void *)(UPTR)-1){ReadFile(file,events,sizeof(events)-1,&bytes,0);CloseHandle(file);events[bytes]=0;}
        unsigned j=0;while(events[j]&&events[j]==test->events[j])j++;
        regsvr_test_check(events[j]==test->events[j]);
        SAY("REGSVR ROW ");HEX(i);HEX(status);
        DWORD wr;WriteFile(GetStdHandle((DWORD)-11),events,bytes,&wr,0);SAY("\n");
        if(waited!=0)break;
    }
    if(!failures)SAY("REGSVR DONE\n");
    ExitProcess(failures?1:0);
}
