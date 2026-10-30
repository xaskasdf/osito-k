typedef unsigned int DWORD;
typedef int LONG;
typedef unsigned short WCHAR;
typedef void *HANDLE;
#define NULL ((void *)0)
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API DWORD CALL MsiCreateRecord(DWORD);
API DWORD CALL MsiCloseHandle(DWORD);
API DWORD CALL MsiRecordGetFieldCount(DWORD);
API LONG CALL MsiRecordGetInteger(DWORD,DWORD);
API DWORD CALL MsiRecordSetInteger(DWORD,DWORD,LONG);
API DWORD CALL MsiRecordSetStringW(DWORD,DWORD,const WCHAR *);
API DWORD CALL MsiRecordGetStringW(DWORD,DWORD,WCHAR *,DWORD *);
API DWORD CALL MsiRecordDataSize(DWORD,DWORD);
API int CALL MsiRecordIsNull(DWORD,DWORD);
API DWORD CALL MsiFormatRecordW(DWORD,DWORD,WCHAR *,DWORD *);
API HANDLE CALL CreateThread(void *,__SIZE_TYPE__,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
API DWORD CALL WaitForSingleObject(HANDLE,DWORD);
API int CALL CloseHandle(HANDLE);
API char *CALL GetCommandLineA(void);
API DWORD CALL GetModuleFileNameA(HANDLE,char *,DWORD);
typedef struct {
    DWORD cb; char *reserved,*desktop,*title;
    DWORD x,y,width,height,columns,rows,fill,flags;
    unsigned short show,reserved_bytes; unsigned char *reserved_data;
    HANDLE input,output,error;
} STARTUP;
typedef struct { HANDLE process,thread; DWORD pid,tid; } PROCESS;
API int CALL CreateProcessA(const char *,char *,void *,void *,int,DWORD,void *,const char *,STARTUP *,PROCESS *);
API int CALL GetExitCodeProcess(HANDLE,DWORD *);
API HANDLE CALL CreateEventA(void *,int,int,const char *);
API HANDLE CALL OpenEventA(DWORD,int,const char *);
API int CALL SetEvent(HANDLE);
API int CALL ResetEvent(HANDLE);
API int CALL TerminateProcess(HANDLE,DWORD);
API void CALL Sleep(DWORD);
#if __SIZEOF_POINTER__ == 4
static const char kill_ready_name[]="Local\\OsitoMsiRecordsKillReady32";
#else
static const char kill_ready_name[]="Local\\OsitoMsiRecordsKillReady64";
#endif
static unsigned checks,failures;
void *memcpy(void *out,const void *in,__SIZE_TYPE__ n)
{unsigned char *d=out;const unsigned char *s=in;for(__SIZE_TYPE__ i=0;i<n;i++)d[i]=s[i];return out;}
void *memset(void *out,int value,__SIZE_TYPE__ n)
{unsigned char *d=out;for(__SIZE_TYPE__ i=0;i<n;i++)d[i]=(unsigned char)value;return out;}
static void record_say(const char *text)
{DWORD n=0,wr;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&wr,NULL);}
static void record_hex(DWORD value)
{char text[9];for(unsigned i=0;i<8;i++)text[i]="0123456789abcdef"[(value>>((7-i)*4))&15];text[8]=0;record_say(text);}
static void record_check(int condition,unsigned line)
{
    __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED);
    if(!condition){__atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);record_say("MSIRECORD FAIL line=");record_hex(line);record_say("\n");}
}
#include "msi_records_checks.h"
static DWORD CALL record_thread(void *argument)
{
    DWORD record=MsiCreateRecord(2);RECORD_CHECK(record!=0);
    for(unsigned i=0;i<300;i++){
        if(argument)RECORD_CHECK(MsiRecordSetStringW(record,1,i&1?RW("Alpha"):RW("Beta"))==0);
        else{
            WCHAR text[16];DWORD size=16;
            RECORD_CHECK(MsiRecordGetStringW(record,1,text,&size)==0);
            RECORD_CHECK(record_equal(text,RW(""))||record_equal(text,RW("Alpha"))||record_equal(text,RW("Beta")));
        }
    }
    RECORD_CHECK(MsiCloseHandle(record)==0);
    return 0;
}
void msi_records_entry(void)
{
    const char *command=GetCommandLineA();int leak=0,guest=0,hold=0;
    for(unsigned i=0;command[i];i++)if(command[i]=='-'&&command[i+1]=='-'){
        if(command[i+2]=='l')leak=1;
        if(command[i+2]=='g')guest=1;
        if(command[i+2]=='k')hold=1;
    }
    if(leak||hold){
        for(unsigned i=0;i<600;i++){
            DWORD handle=MsiCreateRecord(2);
            if(!handle||MsiRecordSetStringW(handle,1,RW("owned by exiting child")))ExitProcess(3);
        }
        if(hold){
            HANDLE ready=OpenEventA(2,0,kill_ready_name);
            if(!ready||!SetEvent(ready))ExitProcess(4);
            CloseHandle(ready);
            record_say("MSIRECORD CHILD HOLDS 600\n");
            for(;;)Sleep(1000);
        }
        record_say("MSIRECORD CHILD LEAVES 600\n");ExitProcess(0);
    }
    record_contract_checks();
    record_say("MSIRECORD CONTRACT COMPLETE\n");
    HANDLE threads[3];
    for(unsigned i=0;i<3;i++){threads[i]=CreateThread(NULL,0,record_thread,(void *)(__UINTPTR_TYPE__)(i==0),0,NULL);RECORD_CHECK(threads[i]!=NULL);}
    for(unsigned i=0;i<3;i++)if(threads[i]){RECORD_CHECK(WaitForSingleObject(threads[i],30000)==0);RECORD_CHECK(CloseHandle(threads[i]));}
    char path[260],child_command[]="msirecord --leak";
    RECORD_CHECK(GetModuleFileNameA(NULL,path,sizeof(path))>0);
    DWORD parent=MsiCreateRecord(1);RECORD_CHECK(parent!=0);
    RECORD_CHECK(MsiRecordSetStringW(parent,1,RW("parent survives"))==0);
    for(unsigned i=0;i<3;i++){
        STARTUP startup={0};PROCESS process={0};startup.cb=sizeof(startup);
        int created=CreateProcessA(path,child_command,NULL,NULL,0,0,NULL,NULL,&startup,&process);
        RECORD_CHECK(created);
        if(created){
            RECORD_CHECK(WaitForSingleObject(process.process,30000)==0);
            DWORD status=99;RECORD_CHECK(GetExitCodeProcess(process.process,&status)&&status==0);
            RECORD_CHECK(CloseHandle(process.thread)&&CloseHandle(process.process));
        }
        WCHAR text[32];DWORD size=32;
        RECORD_CHECK(MsiRecordGetStringW(parent,1,text,&size)==0&&record_equal(text,RW("parent survives")));
    }
    HANDLE ready=CreateEventA(NULL,1,0,kill_ready_name);RECORD_CHECK(ready!=NULL);
    for(unsigned i=0;ready&&i<3;i++){
        RECORD_CHECK(ResetEvent(ready));
        char kill_command[]="msirecord --kill";
        STARTUP startup={0};PROCESS process={0};startup.cb=sizeof(startup);
        int created=CreateProcessA(path,kill_command,NULL,NULL,0,0,NULL,NULL,&startup,&process);
        RECORD_CHECK(created);
        if(created){
            RECORD_CHECK(WaitForSingleObject(ready,30000)==0);
            RECORD_CHECK(TerminateProcess(process.process,23));
            RECORD_CHECK(WaitForSingleObject(process.process,30000)==0);
            DWORD status=99;RECORD_CHECK(GetExitCodeProcess(process.process,&status)&&status==23);
            RECORD_CHECK(CloseHandle(process.thread)&&CloseHandle(process.process));
        }
        WCHAR text[32];DWORD size=32;
        RECORD_CHECK(MsiRecordGetStringW(parent,1,text,&size)==0&&record_equal(text,RW("parent survives")));
    }
    if(ready)RECORD_CHECK(CloseHandle(ready));
    if(guest){
        WCHAR text[32];DWORD size=32;
        RECORD_CHECK(MsiRecordSetStringW(parent,1,(void *)1)==87);
        RECORD_CHECK(MsiRecordGetStringW(parent,1,text,NULL)==87);
        RECORD_CHECK(MsiRecordGetStringW(parent,1,(void *)1,&size)==87&&size==32);
        RECORD_CHECK(MsiRecordSetStringW(parent,0,RW("{{[1]}}"))==0);
        text[0]='Z';RECORD_CHECK(MsiFormatRecordW(0,parent,text,&size)==120&&text[0]=='Z'&&size==32);
    }
    RECORD_CHECK(MsiCloseHandle(parent)==0);
    record_say("MSIRECORD DONE checks=");record_hex(checks);record_say(" failures=");record_hex(failures);record_say("\n");
    ExitProcess(failures?1:0);
}
