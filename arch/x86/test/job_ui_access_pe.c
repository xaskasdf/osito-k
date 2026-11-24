/* Native-safe default: missing-format reads, denied writes and CDS_TEST only.
 * --scratch mutates clipboard/display state; isolated OsitoK tests only. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef void *HANDLE;
typedef __SIZE_TYPE__ SIZE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
void *memset(void *out,int c,SIZE n)
{ for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=(BYTE)c;return out; }
void *memcpy(void *out,const void *in,SIZE n)
{ for(SIZE i=0;i<n;i++)((BYTE *)out)[i]=((const BYTE *)in)[i];return out; }
typedef struct {
    DWORD cb; char *reserved,*desktop,*title;
    DWORD x,y,w,h,xc,yc,fill,flags; WORD show,bytes; BYTE *data;
    HANDLE input,output,error;
} STARTUP;
typedef struct { HANDLE process,thread; DWORD pid,tid; } PROCESS;
static HANDLE (CALL *create_job)(void *,const WORD *);
static BOOL (CALL *set_job)(HANDLE,DWORD,const void *,DWORD);
static BOOL (CALL *query_job)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *assign_job)(HANDLE,HANDLE);
static BOOL (CALL *close_handle)(HANDLE);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static BOOL (CALL *terminate_process)(HANDLE,DWORD);
static DWORD (CALL *resume_thread)(HANDLE);
static DWORD (CALL *wait_object)(HANDLE,DWORD);
static BOOL (CALL *exit_code)(HANDLE,DWORD *);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static DWORD (CALL *get_pid)(void);
static char *(CALL *command_line)(void);
static void (CALL *sleep_ms)(DWORD);
static HANDLE (CALL *global_alloc)(DWORD,SIZE);
static HANDLE (CALL *global_free)(HANDLE);
static HANDLE (CALL *create_window)(DWORD,const char *,const char *,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void *);
static BOOL (CALL *destroy_window)(HANDLE);
static BOOL (CALL *open_clip)(HANDLE);
static BOOL (CALL *close_clip)(void);
static BOOL (CALL *empty_clip)(void);
static HANDLE (CALL *get_clip)(DWORD);
static HANDLE (CALL *set_clip)(DWORD,HANDLE);
static DWORD (CALL *reg_format)(const char *);
static int (CALL *format_name)(DWORD,char *,int);
static BOOL (CALL *available)(DWORD);
static DWORD (CALL *enum_clip)(DWORD);
static DWORD (CALL *sequence)(void);
static BOOL (CALL *enum_display_a)(const char *,DWORD,void *);
static BOOL (CALL *enum_display_w)(const WORD *,DWORD,void *);
static int (CALL *cds_a)(void *,DWORD);
static int (CALL *cds_w)(void *,DWORD);
static int (CALL *cds_ex_a)(const char *,void *,HANDLE,DWORD,void *);
static int (CALL *cds_ex_w)(const WORD *,void *,HANDLE,DWORD,void *);
static DWORD failures;
static BOOL scratch;
static char path[320],name[]="OsitoK.JobUI.Access.00000000";
static void report(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void hex(DWORD n)
{ char s[9];for(DWORD i=0;i<8;i++)s[i]="0123456789abcdef"[(n>>(28-4*i))&15];s[8]=0;report(s); }
static void check(BOOL ok,const char *label)
{
    if(!ok){DWORD error=GetLastError();failures++;report("JOB-UI-ACCESS FAIL: ");
        report(label);report(" error=");hex(error);report("\n");}
}
static BOOL contains(const char *text,const char *needle)
{ for(;*text;text++){DWORD i=0;while(needle[i]&&text[i]==needle[i])i++;if(!needle[i])return 1;}return 0; }
static void put_hex(char *out,DWORD value)
{ for(DWORD i=0;i<8;i++)out[i]="0123456789abcdef"[(value>>(28-4*i))&15]; }
static DWORD token(const char *cmd,char key)
{
    for(;*cmd;cmd++)if(cmd[0]==' '&&cmd[1]==key&&cmd[2]==':'){
        DWORD value=0;cmd+=3;
        for(DWORD i=0;i<8;i++){
            char c=cmd[i];DWORD digit=c>='0'&&c<='9'?(DWORD)(c-'0'):
                c>='a'&&c<='f'?(DWORD)(c-'a'+10):16;
            if(digit>15)return value;value=(value<<4)|digit;
        }
        return value;
    }
    return 0;
}
static BOOL clipboard_open(HANDLE window)
{
    for(DWORD i=0;i<50;i++){
        if(open_clip(window))return 1;
        sleep_ms(10);
    }
    check(0,"open clipboard");return 0;
}
static void display_result(int result,DWORD mask,const char *label)
{
    check(mask&0x10 ? result==-1&&GetLastError()==5 :
                     result==0&&GetLastError()==1234,label);
}
static void display(DWORD mask)
{
    BYTE a[156]={0},w[220]={0},after[156]={0};
    *(WORD *)(a+36)=156;*(WORD *)(w+68)=220;*(WORD *)(after+36)=156;
    check(enum_display_a(0,0xffffffff,a)&&enum_display_w(0,0xffffffff,w),
          "display queries remain available");
    check(*(DWORD *)(a+108)&&*(DWORD *)(a+112),"display query returns geometry");
    SetLastError(1234);display_result(cds_a(0,2),mask,"CDS A registry test restriction");
    SetLastError(1234);display_result(cds_w(0,2),mask,"CDS W registry test restriction");
    SetLastError(1234);display_result(cds_ex_a(0,0,0,2,0),mask,"CDS ExA restriction");
    SetLastError(1234);display_result(cds_ex_w(0,0,0,2,0),mask,"CDS ExW restriction");
    SetLastError(1234);display_result(cds_a(a,2),mask,"CDS A current-mode test restriction");
    SetLastError(1234);display_result(cds_w(w,2),mask,"CDS W current-mode test restriction");
    if(scratch&&(mask&0x10)){
        SetLastError(1234);display_result(cds_a(0,0),mask,"restricted actual display restore rejected");
        SetLastError(1234);display_result(cds_ex_w(0,w,0,0,0),mask,"restricted actual mode application rejected");
    }else if(scratch){
        BYTE candidate[156]={0};*(WORD *)(candidate+36)=156;
        check(enum_display_a(0,0,candidate),"enumerate scratch display mode");
        check(cds_a(candidate,4)==0,"unrestricted fullscreen mode application");
        check(enum_display_a(0,0xffffffff,after)&&
              *(DWORD *)(candidate+104)==*(DWORD *)(after+104)&&
              *(DWORD *)(candidate+108)==*(DWORD *)(after+108)&&
              *(DWORD *)(candidate+112)==*(DWORD *)(after+112),
              "unrestricted mode application updates geometry/depth");
        check(cds_a(a,0)==0,"restore original display mode");
    }
    check(enum_display_a(0,0xffffffff,after)&&
          *(DWORD *)(a+104)==*(DWORD *)(after+104)&&
          *(DWORD *)(a+108)==*(DWORD *)(after+108)&&
          *(DWORD *)(a+112)==*(DWORD *)(after+112),"display geometry/depth unchanged");
}
static void clipboard(DWORD mask,DWORD format)
{
    HANDLE window=0,memory=0;
    if(scratch){
        window=create_window(0,"STATIC","Job UI scratch",0,0,0,16,16,0,0,0,0);
        check(window!=0,"create scratch clipboard owner");
    }
    if(mask&2){SetLastError(1234);check(!get_clip(format)&&GetLastError()==5,
                                       "read restriction precedes clipboard open state");}
    if(mask&4){SetLastError(1234);check(!set_clip(format,0)&&GetLastError()==5,
                                       "write restriction precedes clipboard open state");}
    if(!clipboard_open(window))goto done;
    char buffer[64]={0};
    check(format_name(format,buffer,sizeof(buffer))>0&&contains(buffer,"OsitoK.JobUI.Access."),
          "registered format metadata stays visible");
    SetLastError(1234);enum_clip(0);
    check(GetLastError()!=5,"format enumeration is not denied by read restriction");
    check(!available(format),"test format starts without data");
    SetLastError(1234);
    check(!get_clip(format)&&GetLastError()==((mask&2)?5:1168),
          "data read has access or missing-format error");
    if(mask&4){
        DWORD before=sequence();
        SetLastError(1234);check(!set_clip(format,0)&&GetLastError()==5,
                               "delayed clipboard writes are denied");
        check(sequence()==before&&!available(format),"denied writes leave clipboard unchanged");
    }
    if(scratch){
        /* Native runs never call this branch: EmptyClipboard is allowed by
         * Windows even under WRITECLIPBOARD and would clear the user's data. */
        check(empty_clip(),"empty clipboard remains allowed under write restriction");
        memory=global_alloc(0x40,32);check(memory!=0,"allocate scratch data");
        if(memory){
            DWORD before=sequence();SetLastError(1234);
            HANDLE result=set_clip(format,memory);
            if(mask&4){
                check(!result&&GetLastError()==5,"non-null clipboard writes denied");
                check(sequence()==before&&!available(format),"denied write cannot replace data");
            }else{
                check(result==memory&&available(format),"write allowed independently of read restriction");
                SetLastError(1234);HANDLE read=get_clip(format);
                check(mask&2 ? !read&&GetLastError()==5 : read==memory,
                      "read restriction enforced with real data present");
                check(enum_clip(0)==format,"restricted reader can enumerate data formats");
            }
        }
        check(empty_clip(),"discard scratch data before releasing its storage");
    }
    close_clip();
done:
    if(memory)global_free(memory);
    if(window)destroy_window(window);
}
static void run_child(HANDLE job,DWORD expected)
{
    STARTUP si={0};si.cb=sizeof(si);PROCESS pi={0};
    char cmd[112]="job-ui-access --child m:00000000";
    put_hex(cmd+sizeof("job-ui-access --child m:")-1,expected);
    if(scratch){DWORD i=0;while(cmd[i])i++;const char *s=" --scratch";while(*s)cmd[i++]=*s++;cmd[i]=0;}
    check(create_process(path,cmd,0,0,0,4,0,0,&si,&pi),"create UI child suspended");
    if(!pi.process)return;
    BOOL assigned=assign_job(job,pi.process);check(assigned,"assign UI child before updating limits");
    BOOL set=assigned&&set_job(job,4,&expected,4);check(set,"configure clipboard/display UI limits");
    if(set){
        check(resume_thread(pi.thread)==1,"resume UI child");
        DWORD wait=wait_object(pi.process,30000);check(wait==0,"UI child completes");
        if(wait!=0)terminate_process(pi.process,99);
    }else terminate_process(pi.process,99);
    wait_object(pi.process,5000);DWORD code=0xffffffff;
    check(exit_code(pi.process,&code)&&code==0,"UI child exit status");
    close_handle(pi.thread);close_handle(pi.process);
}
static void contract(void)
{
    HANDLE job=create_job(0,0);check(job!=0,"create UI job");if(!job)return;
    DWORD first=0x16;
    BOOL supported=set_job(job,4,&first,4);
    check(supported,"configure clipboard/display UI limits");
    if(!supported){close_handle(job);return;}
    const DWORD masks[]={0,2,4,6,0x10,0x12,0x14,0x16,0x36,0};
    for(DWORD i=0;i<sizeof(masks)/sizeof(masks[0]);i++){
        run_child(job,masks[i]);
        DWORD readback=0xffffffff;
        check(query_job(job,4,&readback,4,0)&&readback==masks[i],"UI restriction readback");
        /* The controlling process is not in the child's job. */
        display(0);
        DWORD format=reg_format(name);
        if(clipboard_open(0)){
            SetLastError(1234);check(!get_clip(format)&&GetLastError()==1168,
                                   "child restrictions do not affect parent data access");
            close_clip();
        }
    }
    DWORD mask=0x36,readback=0;
    check(set_job(job,4,&mask,4),"restore combined UI limits");
    mask=0x80000036;
    check(!set_job(job,4,&mask,4)&&GetLastError()==87,"invalid mask rejected");
    check(query_job(job,4,&readback,4,0)&&readback==0x36,"invalid mask leaves restrictions intact");
    mask=0xff;
    check(set_job(job,4,&mask,4),"full basic UI mask accepted");
    check(query_job(job,4,&readback,4,0)&&readback==0xff,"full basic UI mask readback");
    close_handle(job);
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),u=LoadLibraryA("user32.dll");
#define LOAD(module,variable,symbol) variable=GetProcAddress(module,symbol);if(!variable){report("JOB-UI-ACCESS MISSING " symbol "\n");ExitProcess(3);}
    LOAD(k,create_job,"CreateJobObjectW");LOAD(k,set_job,"SetInformationJobObject");
    LOAD(k,query_job,"QueryInformationJobObject");LOAD(k,assign_job,"AssignProcessToJobObject");
    LOAD(k,close_handle,"CloseHandle");LOAD(k,create_process,"CreateProcessA");
    LOAD(k,terminate_process,"TerminateProcess");LOAD(k,resume_thread,"ResumeThread");
    LOAD(k,wait_object,"WaitForSingleObject");LOAD(k,exit_code,"GetExitCodeProcess");
    LOAD(k,module_name,"GetModuleFileNameA");LOAD(k,get_pid,"GetCurrentProcessId");
    LOAD(k,command_line,"GetCommandLineA");LOAD(k,sleep_ms,"Sleep");
    LOAD(k,global_alloc,"GlobalAlloc");LOAD(k,global_free,"GlobalFree");
    LOAD(u,create_window,"CreateWindowExA");LOAD(u,destroy_window,"DestroyWindow");
    LOAD(u,open_clip,"OpenClipboard");LOAD(u,close_clip,"CloseClipboard");
    LOAD(u,empty_clip,"EmptyClipboard");LOAD(u,get_clip,"GetClipboardData");
    LOAD(u,set_clip,"SetClipboardData");LOAD(u,reg_format,"RegisterClipboardFormatA");
    LOAD(u,format_name,"GetClipboardFormatNameA");LOAD(u,available,"IsClipboardFormatAvailable");
    LOAD(u,enum_clip,"EnumClipboardFormats");LOAD(u,sequence,"GetClipboardSequenceNumber");
    LOAD(u,enum_display_a,"EnumDisplaySettingsA");LOAD(u,enum_display_w,"EnumDisplaySettingsW");
    LOAD(u,cds_a,"ChangeDisplaySettingsA");LOAD(u,cds_w,"ChangeDisplaySettingsW");
    LOAD(u,cds_ex_a,"ChangeDisplaySettingsExA");LOAD(u,cds_ex_w,"ChangeDisplaySettingsExW");
#undef LOAD
    module_name(0,path,sizeof(path));put_hex(name+sizeof(name)-9,get_pid());
    scratch=contains(command_line(),"--scratch");
    if(contains(command_line(),"--child")){
        DWORD expected=token(command_line(),'m'),mask=0xffffffff;
        check(query_job(0,4,&mask,4,0)&&mask==expected,"existing member observes job restriction update");
        if(!failures){display(mask);clipboard(mask,reg_format(name));}
    }else{
        contract();report(failures?"JOB-UI-ACCESS FAILED\n":"JOB-UI-ACCESS PASS\n");
    }
    ExitProcess(failures?1:0);
}
