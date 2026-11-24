/* Original UTF-16 command survives child setup and parameter refreshes. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API const WORD *CALL GetCommandLineW(void);
API DWORD CALL GetModuleFileNameW(void *,WORD *,DWORD);
API DWORD CALL GetCurrentDirectoryW(DWORD,WORD *);
API int CALL SetCurrentDirectoryW(const WORD *);
API int CALL SetEnvironmentVariableW(const WORD *,const WORD *);
API int CALL CreateProcessW(const WORD *,WORD *,void *,void *,int,DWORD,void *,const WORD *,void *,void *);
API DWORD CALL WaitForSingleObject(void *,DWORD);
API int CALL GetExitCodeProcess(void *,DWORD *);
API int CALL CloseHandle(void *);
API int CALL TerminateProcess(void *,DWORD);
typedef struct {DWORD cb;WORD *reserved,*desktop,*title;DWORD x,y,w,h,xc,yc,fill,flags;
    WORD show,bytes;BYTE *data;void *input,*output,*error;} STARTUP;
typedef struct {void *process,*thread;DWORD pid,tid;} PROCESS;
#define SAY(s) do{DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
static unsigned failures;
static WORD image[1024],command[2048],saved[2048],cwd[1024];
static const WORD tail[]={' ',':',':','c','h','i','l','d',' ',0x100,0x3a9,0x4e2d,0xf1,0xd83d,0xdc3b,0};
static void child_command_check(int ok)
{if(ok)SAY("CMDW PASS\n");else{failures++;SAY("CMDW FAIL\n");}}
void child_command_entry(void)
{
    const WORD *line=GetCommandLineW();unsigned length=0;
    while(line[length]&&length<2047){saved[length]=line[length];length++;}saved[length]=0;
    int child=0;
    for(unsigned i=0;i+8<length;i++)if(line[i]==' '&&line[i+1]==':'&&line[i+2]==':'){child=1;break;}
    if(child) {
        unsigned tail_length=sizeof(tail)/sizeof(tail[0])-1;
        for(unsigned stage=0;stage<3;stage++) {
            line=GetCommandLineW();unsigned j=0;
            while(line[j]&&line[j]==saved[j]&&j<2047)j++;
            child_command_check(!line[j]&&!saved[j]);
            int same=length>=tail_length;
            for(unsigned i=0;same&&i<tail_length;i++)same=line[length-tail_length+i]==tail[i];
            child_command_check(same);
            BYTE *peb;
#if __SIZEOF_POINTER__ == 4
            __asm__ volatile("movl %%fs:0x30,%0":"=r"(peb));
            BYTE *parameters=*(BYTE **)(peb+0x10);
            BYTE *descriptor=parameters+0x40;
            const WORD *buffer=*(const WORD **)(descriptor+4);
#else
            __asm__ volatile("movq %%gs:0x60,%0":"=r"(peb));
            BYTE *parameters=*(BYTE **)(peb+0x20);
            BYTE *descriptor=parameters+0x70;
            const WORD *buffer=*(const WORD **)(descriptor+8);
#endif
            same=*(WORD *)descriptor==length*2;
            for(unsigned i=0;same&&i<=length;i++)same=buffer[i]==saved[i];
            child_command_check(same);
            if(stage==0)child_command_check(SetEnvironmentVariableW((const WORD *)L"OSITO_CMDW",tail));
            if(stage==1){DWORD n=GetCurrentDirectoryW(1024,cwd);child_command_check(n&&n<1024&&SetCurrentDirectoryW(cwd));}
        }
        if(!failures)SAY("CMDW CHILD DONE\n");ExitProcess(failures?1:0);
    }
    DWORD n=GetModuleFileNameW(0,image,1024);child_command_check(n&&n<1024);
    if(!n||n>=1024)ExitProcess(1);
    unsigned offset=0;command[offset++]='"';
    for(unsigned i=0;i<n;i++)command[offset++]=image[i];command[offset++]='"';
    for(unsigned i=0;i<sizeof(tail)/sizeof(tail[0]);i++)command[offset++]=tail[i];
    STARTUP start;PROCESS process;
    for(unsigned i=0;i<sizeof(start);i++)((BYTE *)&start)[i]=0;
    for(unsigned i=0;i<sizeof(process);i++)((BYTE *)&process)[i]=0;
    start.cb=sizeof(start);start.flags=0x100;
    start.input=GetStdHandle((DWORD)-10);start.output=GetStdHandle((DWORD)-11);start.error=GetStdHandle((DWORD)-12);
    int created=CreateProcessW(image,command,0,0,1,0,0,0,&start,&process);child_command_check(created);
    if(created) {
        DWORD waited=WaitForSingleObject(process.process,30000);child_command_check(waited==0);
        if(waited!=0){TerminateProcess(process.process,99);WaitForSingleObject(process.process,5000);}
        DWORD status=~0U;child_command_check(GetExitCodeProcess(process.process,&status)&&status==0);
        child_command_check(CloseHandle(process.thread)&&CloseHandle(process.process));
    }
    if(!failures)SAY("CMDW DONE\n");ExitProcess(failures?1:0);
}
