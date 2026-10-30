/* Default runs only read host settings and create/close private desktops.
 * --scratch includes actual SPI writes and ExitWindowsEx: OsitoK only. */
#define mainCRTStartup unusedJobEntry
#include "job_object_pe.c"
#undef mainCRTStartup
static BOOL (CALL *spi_a)(DWORD,DWORD,void *,DWORD);
static BOOL (CALL *spi_w)(DWORD,DWORD,void *,DWORD);
static HANDLE (CALL *create_desktop)(const WORD *,const WORD *,void *,DWORD,DWORD,void *);
static BOOL (CALL *close_desktop)(HANDLE);
static BOOL (CALL *switch_desktop)(HANDLE);
static BOOL (CALL *exit_windows)(DWORD,DWORD);
static HANDLE (CALL *thread_desktop)(DWORD);
static DWORD (CALL *thread_id)(void);
static HANDLE (CALL *monitor_point)(U64,DWORD);
static BOOL (CALL *monitor_info)(HANDLE,void *);
static HANDLE (CALL *create_window)(DWORD,const char *,const char *,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void *);
static BOOL (CALL *destroy_window)(HANDLE);
static SIZE (CALL *send_message)(HANDLE,DWORD,SIZE,SIZE);
static BOOL scratch;

static void system_child(void)
{
    DWORD mask=0;
    check(query_job(0,4,&mask,4,0),"query current UI policy");
    if(failures)return;
    report("JOB-UI-SYSTEM mask=");hex(mask);report("\n");
    DWORD lines=0xcccccccc;
    SetLastError(1234);
    check(spi_a(0x68,0,&lines,0)&&GetLastError()==0&&lines!=0xcccccccc,
          "scroll-lines query writes data under every mask");
    int area[4]={0};SetLastError(1234);
    check(spi_w(0x30,0,area,0)&&GetLastError()==1234&&area[2]>area[0]&&area[3]>area[1],
          "work-area query under every mask");
    if(mask&8){
        /* Verify deny bit first; these no-change calls cannot alter host settings. */
        SetLastError(1234);
        check(!spi_a(0x69,lines,0,0)&&GetLastError()==5,"restricted SPI A write denied");
        SetLastError(1234);
        check(!spi_w(0x69,lines,0,0)&&GetLastError()==5,"restricted SPI W write denied");
        SetLastError(1234);
        check(!spi_w(0x2f,0,0,0)&&GetLastError()==5,"restriction precedes setter pointer validation");
    }
    SetLastError(1234);
    check(!spi_a(0xffffffff,0,0,0)&&GetLastError()==
          (sizeof(void *)==4?120:(mask&8?5:87)),"unknown SPI action ABI/error ordering");
    SetLastError(1234);
    HANDLE d=create_desktop(L"OsitoK.JobUI.System.Private",0,0,0,0xf01ff,0);
    if(mask&0x40)check(!d&&GetLastError()==5,"desktop creation denied independently");
    else check(d!=0,"private desktop creation permitted");
    if(d)check(close_desktop(d),"close private desktop");
    SetLastError(1234);
    check(!switch_desktop(0)&&GetLastError()==(mask&0x40?5:6),"switch policy precedes handle lookup");
    if(!scratch)return;

    /* Never run this section on the development host. */
    SetLastError(1234);
    check(!exit_windows(0,0)&&GetLastError()==(mask&0x80?5:50),
          "exit restriction or explicit missing session backend");
    HANDLE active=thread_desktop(thread_id());SetLastError(1234);
    BOOL switched=switch_desktop(active);
    check(mask&0x40 ? !switched&&GetLastError()==5 : switched&&GetLastError()==1234,
          "active desktop no-op honors restriction");
    if(!(mask&0x40)){
        d=create_desktop(L"OsitoK.JobUI.System.Inactive",0,0,0,0xf01ff,0);
        check(d!=0,"create inactive desktop");
        if(d){
            check(!switch_desktop(d)&&GetLastError()==50,"no fake private desktop switch");
            check(close_desktop(d),"close inactive desktop");
        }
    }
    DWORD sentinel=0xcccccccc;
    check(!spi_a(0x1f,0,&sentinel,0)&&GetLastError()==50&&sentinel==0xcccccccc,
          "unimplemented query fails without touching output");
    check(!spi_w(0x0d,0,&sentinel,0)&&GetLastError()==50,
          "bidirectional icon-spacing query is not denied");
    if(mask&8){
        check(!spi_w(0x0d,1,0,0)&&GetLastError()==5,"bidirectional icon-spacing setter denied");
        check(!spi_a(0x2f,0,area,0)&&GetLastError()==5,"work-area mutation denied");
        DWORD before=lines;
        check(!spi_w(0x69,9,0,0)&&GetLastError()==5,"different scroll value denied");
        check(spi_a(0x68,0,&lines,0)&&lines==before,"denied write preserves settings");
    }else{
        DWORD before=lines;
        check(spi_a(0x69,9,0,0)&&spi_w(0x68,0,&lines,0)&&lines==9,"shared A/W scroll state");
        HANDLE list=create_window(0,"LISTBOX","SPI wheel test",0x00200000,0,0,160,100,0,0,0,0);
        check(list!=0,"create scratch listbox");
        if(list){
            for(DWORD i=0;i<50;i++)send_message(list,0x180,0,(SIZE)L"row");
            send_message(list,0x197,20,0);
            send_message(list,0x20a,(SIZE)120<<16,0);
            check(send_message(list,0x18e,0,0)==11,"listbox consumes updated SPI wheel setting");
            check(destroy_window(list),"destroy scratch listbox");
        }
        check(spi_w(0x69,before,0,0),"restore scroll state");
        check(!spi_a(0x69,13,0,1)&&GetLastError()==50,"unsupported persistence fails");
        check(spi_w(0x68,0,&lines,0)&&lines==before,"failed persistence leaves state intact");
        int changed[4]={area[0]+1,area[1]+1,area[2]-1,area[3]-1},after[4]={0};
        check(spi_w(0x2f,0,changed,0)&&spi_a(0x30,0,after,0),"set and query work area");
        for(DWORD i=0;i<4;i++)check(changed[i]==after[i],"work area roundtrip");
        struct {DWORD size;int monitor[4],work[4];DWORD flags;} mi={0};mi.size=sizeof(mi);
        check(monitor_info(monitor_point(0,1),&mi),"monitor query");
        for(DWORD i=0;i<4;i++)check(changed[i]==mi.work[i],"monitor shares work area");
        check(spi_a(0x2f,0,area,0),"restore work area");
        check(!spi_w(0x30,0,0,0)&&GetLastError()==998,"null output rejected");
        check(!spi_a(0x2f,0,(void *)1,0)&&GetLastError()==998,"unmapped setter input rejected");
    }
}

void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),u=LoadLibraryA("user32.dll");
#define LOAD(m,v,n) v=GetProcAddress(m,n);if(!v){report("JOB-UI-SYSTEM MISSING " n "\n");ExitProcess(3);}
    LOAD(k,create_job,"CreateJobObjectW");LOAD(k,set_job,"SetInformationJobObject");
    LOAD(k,query_job,"QueryInformationJobObject");LOAD(k,assign_job,"AssignProcessToJobObject");
    LOAD(k,close_handle,"CloseHandle");LOAD(k,wait_object,"WaitForSingleObject");
    LOAD(k,exit_code,"GetExitCodeProcess");LOAD(k,create_process,"CreateProcessA");
    LOAD(k,resume_thread,"ResumeThread");LOAD(k,module_name,"GetModuleFileNameA");
    LOAD(k,command_line,"GetCommandLineA");LOAD(k,terminate_process,"TerminateProcess");
    LOAD(k,handle_flags,"SetHandleInformation");LOAD(k,thread_id,"GetCurrentThreadId");
    LOAD(u,spi_a,"SystemParametersInfoA");LOAD(u,spi_w,"SystemParametersInfoW");
    LOAD(u,create_desktop,"CreateDesktopW");LOAD(u,close_desktop,"CloseDesktop");
    LOAD(u,switch_desktop,"SwitchDesktop");LOAD(u,thread_desktop,"GetThreadDesktop");
    LOAD(u,exit_windows,"ExitWindowsEx");LOAD(u,monitor_point,"MonitorFromPoint");
    LOAD(u,monitor_info,"GetMonitorInfoW");
    LOAD(u,create_window,"CreateWindowExA");LOAD(u,destroy_window,"DestroyWindow");
    LOAD(u,send_message,"SendMessageW");
#undef LOAD
    module_name(0,path,sizeof(path));scratch=contains(command_line(),"--scratch");
    if(contains(command_line(),"--system-child"))system_child();
    else{
        expected_job=create_job(0,0);
        check(expected_job&&handle_flags(expected_job,1,1),"create inheritable system UI job");
        if(expected_job){
            DWORD masks[]={0,8,0x40,0x80,0xc8,0xff,0};
            for(DWORD i=0;i<sizeof(masks)/sizeof(masks[0]);i++){
                PROCESS p={0};check(spawn(scratch?"--system-child --scratch":"--system-child",4,&p),"create suspended system UI child");
                if(!p.process)continue;
                BOOL assigned=assign_job(expected_job,p.process);
                check(assigned,"assign before updating system UI policy");
                SetLastError(1234);
                BOOL set=assigned&&set_job(expected_job,4,&masks[i],4);
                check(set&&GetLastError()==1234,"configure system UI restrictions");
                if(!set)terminate_process(p.process,99);
                check(finish(&p,set)==0,"system UI child exit");
                DWORD readback=0xffffffff;
                check(query_job(expected_job,4,&readback,4,0)&&readback==masks[i],"system UI readback");
            }
            DWORD invalid=0x80000000,readback=0xffffffff;
            check(!set_job(expected_job,4,&invalid,4)&&GetLastError()==87,"invalid UI bit rejected");
            check(query_job(expected_job,4,&readback,4,0)&&readback==0,"failed update is atomic");
            close_handle(expected_job);
        }
        report(failures?"JOB-UI-SYSTEM FAILED\n":"JOB-UI-SYSTEM PASS\n");
    }
    ExitProcess(failures?1:0);
}
