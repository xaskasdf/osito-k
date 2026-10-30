#include "process_shutdown_common.h"
typedef struct {void *process,*thread;DWORD pid,tid;} PROCESS_INFO;
API DWORD CALL GetModuleFileNameA(void *,char *,DWORD);
API int CALL CreateProcessA(const char *,char *,void *,void *,int,DWORD,void *,const char *,void *,PROCESS_INFO *);
API int CALL GetExitCodeProcess(void *,DWORD *);
static DWORD CALL parent_worker(void *argument){(void)argument;return 29;}
void shutdown_parent_entry(void)
{
    char path[512];DWORD length=GetModuleFileNameA(0,path,sizeof(path));
    if(!length||length>=sizeof(path))ExitProcess(111);
    while(length && path[length-1]!='/' && path[length-1]!='\\')length--;
#if __SIZEOF_POINTER__ == 4
    const char *names[]={"shutdown32-main.exe","shutdown32-worker.exe","shutdown32-forced.exe","shutdown32-busy-main.exe","shutdown32-busy-worker.exe"};
#else
    const char *names[]={"shutdown64-main.exe","shutdown64-worker.exe","shutdown64-forced.exe","shutdown64-busy-main.exe","shutdown64-busy-worker.exe"};
#endif
    for(unsigned round=0;round<2;round++)for(unsigned variant=0;variant<5;variant++) {
        unsigned i=0;while(names[variant][i]){path[length+i]=names[variant][i];i++;}path[length+i]=0;
        char command[516];command[0]='"';i=0;while(path[i]){command[i+1]=path[i];i++;}command[++i]='"';command[++i]=0;
        UP startup[__SIZEOF_POINTER__==4?17:13]={0};*(DWORD *)startup=sizeof(startup);
        PROCESS_INFO child={0};DWORD code=0;
        if(!CreateProcessA(path,command,0,0,0,0,0,0,startup,&child))ExitProcess(112);
        if(WaitForSingleObject(child.process,15000)!=0 || !GetExitCodeProcess(child.process,&code) || code!=37)ExitProcess(113);
        CloseHandle(child.thread);CloseHandle(child.process);
        if(RtlDllShutdownInProgress() || RtlIsThreadWithinLoaderCallout())ExitProcess(114);
        void *worker=CreateThread(0,0,parent_worker,0,0,0);
        if(!worker||WaitForSingleObject(worker,3000)!=0||!GetExitCodeThread(worker,&code)||code!=29)ExitProcess(115);
        CloseHandle(worker);shutdown_row("child-passed",round*5+variant);
    }
    shutdown_row("parent-done",1);ExitProcess(0);
}
