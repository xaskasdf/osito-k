/* Same hidden, test-owned USER objects on Windows and OsitoK. Broadcast checks
 * require --scratch and must only run in the isolated OsitoK environment. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef void *HANDLE;
typedef __SIZE_TYPE__ SIZE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define SELF ((HANDLE)(__INTPTR_TYPE__)-1)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
void *memset(void *p,int c,SIZE n)
{ for(SIZE i=0;i<n;i++)((BYTE *)p)[i]=(BYTE)c;return p; }
void *memcpy(void *p,const void *s,SIZE n)
{ for(SIZE i=0;i<n;i++)((BYTE *)p)[i]=((const BYTE *)s)[i];return p; }
typedef struct {
    DWORD cb; char *reserved,*desktop,*title;
    DWORD x,y,w,h,xc,yc,fill,flags; WORD show,bytes; BYTE *data;
    HANDLE input,output,error;
} STARTUP;
typedef struct { HANDLE process,thread; DWORD pid,tid; } PROCESS;
typedef struct { HANDLE window; DWORD message; SIZE wp,lp; DWORD time; int point[2]; } MSG;
static HANDLE (CALL *create_job)(void *,const WORD *);
static BOOL (CALL *set_job)(HANDLE,DWORD,const void *,DWORD);
static BOOL (CALL *query_job)(HANDLE,DWORD,void *,DWORD,DWORD *);
static BOOL (CALL *assign_job)(HANDLE,HANDLE);
static BOOL (CALL *close_handle)(HANDLE);
static BOOL (CALL *handle_flags)(HANDLE,DWORD,DWORD);
static BOOL (CALL *dup_handle)(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
static BOOL (CALL *create_process)(const char *,char *,void *,void *,BOOL,DWORD,void *,const char *,void *,void *);
static DWORD (CALL *resume_thread)(HANDLE);
static DWORD (CALL *wait_object)(HANDLE,DWORD);
static BOOL (CALL *terminate_process)(HANDLE,DWORD);
static BOOL (CALL *exit_code)(HANDLE,DWORD *);
static DWORD (CALL *module_name)(HANDLE,char *,DWORD);
static char *(CALL *command_line)(void);
static HANDLE (CALL *create_window)(DWORD,const char *,const char *,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void *);
static BOOL (CALL *destroy_window)(HANDLE);
static BOOL (CALL *is_window)(HANDLE);
static BOOL (CALL *window_rect)(HANDLE,int *);
static DWORD (CALL *window_pid)(HANDLE,DWORD *);
static int (CALL *window_text)(HANDLE,char *,int);
static BOOL (CALL *post_message)(HANDLE,DWORD,SIZE,SIZE);
static SIZE (CALL *send_timeout)(HANDLE,DWORD,SIZE,SIZE,DWORD,DWORD,SIZE *);
static HANDLE (CALL *get_dc)(HANDLE);
static int (CALL *release_dc)(HANDLE,HANDLE);
static BOOL (CALL *enum_windows)(void *,SIZE);
static HANDLE (CALL *create_menu)(void);
static BOOL (CALL *destroy_menu)(HANDLE);
static BOOL (CALL *is_menu)(HANDLE);
static int (CALL *menu_count)(HANDLE);
static BOOL (CALL *grant_user)(HANDLE,HANDLE,BOOL);
static BOOL (CALL *peek_message)(MSG *,HANDLE,DWORD,DWORD,DWORD);
static HANDLE (CALL *set_hook)(int,void *,HANDLE,DWORD);
static BOOL (CALL *unhook)(HANDLE);
static BOOL (CALL *post_thread)(DWORD,DWORD,SIZE,SIZE);
static SIZE (CALL *window_long)(HANDLE,int);
static SIZE (CALL *call_proc)(SIZE,HANDLE,DWORD,SIZE,SIZE);
static BOOL (CALL *set_pos)(HANDLE,HANDLE,int,int,int,int,DWORD);
static HANDLE (CALL *get_item)(HANDLE,int);
static HANDLE (CALL *create_imc)(void);
static HANDLE (CALL *get_imc)(HANDLE);
static HANDLE (CALL *associate_imc)(HANDLE,HANDLE);
static BOOL (CALL *get_conversion)(HANDLE,DWORD *,DWORD *);
static BOOL (CALL *destroy_imc)(HANDLE);
static HANDLE (CALL *create_icon_indirect)(const void *);
static HANDLE (CALL *create_bitmap)(int,int,DWORD,DWORD,const void *);
static BOOL (CALL *delete_object)(HANDLE);
static HANDLE (CALL *copy_icon)(HANDLE);
static HANDLE (CALL *copy_image)(HANDLE,DWORD,int,int,DWORD);
static BOOL (CALL *destroy_icon)(HANDLE);
static HANDLE parent_imc,parent_icon;
static DWORD failures;
static BOOL scratch,seen;
static char path[320];
static void report(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void hex(DWORD n)
{ char s[9];for(DWORD i=0;i<8;i++)s[i]="0123456789abcdef"[(n>>(28-4*i))&15];s[8]=0;report(s); }
static void check(BOOL ok,const char *label)
{ if(!ok){DWORD e=GetLastError();failures++;report("JOB-UI-HANDLES FAIL: ");report(label);report(" error=");hex(e);report("\n");} }
static BOOL contains(const char *s,const char *n)
{ for(;*s;s++){DWORD i=0;while(n[i]&&s[i]==n[i])i++;if(!n[i])return 1;}return 0; }
static SIZE token(char key)
{
    const char *s=command_line();
    for(;*s;s++)if(s[0]==' '&&s[1]==key&&s[2]==':'){
        SIZE value=0;s+=3;
        while((*s>='0'&&*s<='9')||(*s>='a'&&*s<='f')){
            value=(value<<4)+(*s<='9'?*s-'0':*s-'a'+10);s++;
        }return value;
    }return 0;
}
static DWORD append(char *s,DWORD i,char key,SIZE value)
{
    s[i++]=' ';s[i++]=key;s[i++]=':';
    for(DWORD n=2*sizeof(SIZE);n;n--)s[i++]="0123456789abcdef"[(value>>((n-1)*4))&15];
    s[i]=0;return i;
}
static BOOL CALL enum_proc(HANDLE h,SIZE target)
{ if(h==(HANDLE)target)seen=1;return 1; }
static SIZE CALL hook_proc(int c,SIZE w,SIZE l)
{ (void)c;(void)w;(void)l;return 0; }
static void test_window(HANDLE h,BOOL allowed)
{
    int rect[4]={0};DWORD pid=0;char text[64]={0};
    SetLastError(1234);check(is_window(h)==allowed&&GetLastError()==1234,"window visibility and LastError");
    SetLastError(1234);BOOL ok=window_rect(h,rect);
    check(allowed?ok:!ok&&GetLastError()==1400,"window rectangle access");
    SetLastError(1234);DWORD tid=window_pid(h,&pid);
    check(allowed?tid&&pid:!tid&&GetLastError()==1400,"window thread/process access");
    SetLastError(1234);int length=window_text(h,text,sizeof(text));
    check(allowed?length>0:!length&&GetLastError()==1400,"window text access");
    SetLastError(1234);ok=post_message(h,0,0,0);
    check(allowed?ok:!ok&&GetLastError()==1400,"posted message access");
    SetLastError(1234);HANDLE dc=get_dc(h);
    check(allowed?dc!=0:!dc&&GetLastError()==1400,"window DC access");
    if(dc)release_dc(h,dc);
    seen=0;check(enum_windows(enum_proc,(SIZE)h),"enumerate windows");
    check(seen==allowed,"window enumeration respects grant");
    if(!allowed){
        SIZE result=1234;SetLastError(1234);
        check(!send_timeout(h,0,0,0,2,1,&result)&&GetLastError()==1400,"sent message denied before waiting");
    }
}
static void test_menu(HANDLE menu,BOOL allowed)
{
    SetLastError(1234);BOOL ok=is_menu(menu);
    check(allowed?ok:!ok&&GetLastError()==1401,"menu visibility");
    SetLastError(1234);int count=menu_count(menu);
    check(allowed?count==0:count==-1&&GetLastError()==1401,"menu access");
}
static HANDLE test_icon(void)
{
    BYTE mask[8]={0},pixels[16]={0};
    struct {BOOL icon;DWORD x,y;HANDLE mask,color;} info={1,0,0,0,0};
    info.mask=create_bitmap(2,2,1,1,mask);info.color=create_bitmap(2,2,1,32,pixels);
    HANDLE icon=info.mask&&info.color?create_icon_indirect(&info):0;
    if(info.mask)delete_object(info.mask);if(info.color)delete_object(info.color);
    return icon;
}
static void member(HANDLE job,HANDLE window,HANDLE menu,HANDLE hook,DWORD expected,BOOL assign,BOOL nested)
{
    char cmd[260]="job-ui-handles --child";DWORD i=0;while(cmd[i])i++;
    i=append(cmd,i,'j',(SIZE)job);i=append(cmd,i,'w',(SIZE)window);i=append(cmd,i,'m',(SIZE)menu);
    i=append(cmd,i,'h',(SIZE)hook);
    i=append(cmd,i,'t',window_pid(window,0));
    i=append(cmd,i,'c',(SIZE)parent_imc);i=append(cmd,i,'i',(SIZE)parent_icon);
    i=append(cmd,i,'e',expected);i=append(cmd,i,'n',nested);append(cmd,i,'s',scratch);
    STARTUP si={0};si.cb=sizeof(si);PROCESS pi={0};
    check(create_process(path,cmd,0,0,1,4,0,0,&si,&pi),"create suspended USER member");
    if(!pi.process)return;
    BOOL ready=!assign||assign_job(job,pi.process);check(ready,"assign USER member");
    if(ready){
        check(resume_thread(pi.thread)==1,"resume USER member");
        DWORD wait=wait_object(pi.process,30000);check(wait==0,"wait USER member");
        if(wait!=0)terminate_process(pi.process,99);
    }else terminate_process(pi.process,99);
    wait_object(pi.process,5000);DWORD code=99;
    check(exit_code(pi.process,&code)&&code==0,"USER member exits zero");
    close_handle(pi.thread);close_handle(pi.process);
}
static void child(void)
{
    HANDLE job=(HANDLE)token('j'),window=(HANDLE)token('w'),menu=(HANDLE)token('m');
    DWORD expected=(DWORD)token('e'),mask=0;
    check(query_job(0,4,&mask,4,0),"member job query");
    test_window(window,(expected&1)!=0);test_menu(menu,(expected&2)!=0);
    SetLastError(1234);BOOL removed=unhook((HANDLE)token('h'));
    check(expected&4 ? removed&&GetLastError()==1234 : !removed&&GetLastError()==1404,
          "hook access follows independent grant");
    SetLastError(1234);HANDLE remote_hook=set_hook(3,hook_proc,0,(DWORD)token('t'));
    check(!remote_hook&&GetLastError()==5,"foreign hook cannot use local callback");
    if(remote_hook)unhook(remote_hook);
    SetLastError(1234);check(post_thread((DWORD)token('t'),0,0,0),"thread messages do not use USER handle restriction");
    HANDLE own=create_window(0,"STATIC","Job handles member",0,0,0,32,32,0,0,0,0);
    HANDLE own_menu=create_menu();check(own&&own_menu,"create own USER objects");
    HANDLE own_child=create_window(0,"STATIC","member child",0x40000000,0,0,8,8,own,(HANDLE)42,0,0);
    check(own_child!=0,"create private child window");
    HANDLE own_hook=own?set_hook(3,hook_proc,0,window_pid(own,0)):0;
    check(own_hook!=0,"create thread hook");
    if(own)test_window(own,1);if(own_menu)test_menu(own_menu,1);
    SIZE proc=window_long(own,-4);char title[64]={0};check(proc!=0,"standard window procedure");
    SIZE length=call_proc(proc,own,0x0d,sizeof(title),(SIZE)title);
    check(length>0,"direct standard procedure still handles own window");
    /* Native control procedures have process-local private data. Exercise a
     * foreign HWND only in OsitoK, where the shim must reject it before use. */
    if(scratch&&!(expected&1)){
        SetLastError(1234);length=call_proc(proc,window,0x0d,sizeof(title),(SIZE)title);
        check(!length&&GetLastError()==1400,"direct standard procedure validates HWND");
    }
    SetLastError(1234);BOOL positioned=set_pos(own,window,0,0,0,0,0x413);
    check(expected&1 ? positioned : !positioned&&GetLastError()==1400,"z-order insert-after access");
    SetLastError(1234);HANDLE item=get_item(window,42);
    check(expected&1 ? item!=0 : !item&&GetLastError()==1400,"GetDlgItem validates parent access");
    if(item)check(is_window(item)==(!(mask&1)||(expected&32)!=0),"parent grant does not grant child");
    HANDLE context=(HANDLE)token('c'),icon=(HANDLE)token('i');
    SetLastError(1234);HANDLE associated=get_imc(window);
    check(!associated&&GetLastError()==(expected&1?1234:1400),"foreign IMM window access");
    DWORD conversion=0,sentence=0;
    SetLastError(1234);BOOL queried=get_conversion(context,&conversion,&sentence);
    check(!queried&&GetLastError()==(expected&16?1234:6),"foreign IMM context stays process-private");
    SetLastError(1234);associated=associate_imc(window,0);
    check(!associated&&GetLastError()==(expected&32?1234:expected&1?5:1400),"foreign IMM association access");
    SetLastError(1234);associated=associate_imc(own,context);
    check(!associated&&GetLastError()==(expected&16?1234:6),"foreign IMM context cannot be attached");
    SetLastError(1234);BOOL destroyed=destroy_imc(context);
    check(!destroyed&&GetLastError()==(expected&16?1234:6),"foreign IMM context cannot be destroyed");
    SetLastError(1234);HANDLE copied=copy_icon(icon);
    check(expected&8 ? copied!=0 : !copied&&GetLastError()==1402,"icon copy access");
    if(copied)destroy_icon(copied);
    SetLastError(1234);copied=copy_image(icon,1,2,2,4);
    check(expected&8 ? copied!=0 : !copied&&GetLastError()==1402,"copy-return-original validates icon");
    if(copied&&copied!=icon)destroy_icon(copied);
    SetLastError(1234);check(!destroy_icon(icon)&&GetLastError()==(expected&8?1435:1402),
                           "grant does not transfer icon destruction ownership");
    HANDLE own_imc=create_imc();check(own_imc!=0,"create own input context");
    if(own_imc){
        associate_imc(own,own_imc);
        check(get_imc(own)==own_imc,"own input context association");
        associate_imc(own,0);
        check(destroy_imc(own_imc),"destroy own input context");
    }
    if(token('n')&&own&&own_menu&&own_hook){
        parent_imc=create_imc();parent_icon=copy_icon(icon);
        /* The root icon is inaccessible in this phase. Create a private one. */
        if(!parent_icon)parent_icon=test_icon();
        check(parent_imc&&parent_icon,"create same-job contexts and icons");
        member(job,own,own_menu,own_hook,63,0,0);
        destroy_icon(parent_icon);destroy_imc(parent_imc);
    }
    if(scratch&&(mask&1)&&own){
        MSG message;
        check(post_message((HANDLE)0xffff,0x8511,0,0),"post job broadcast");
        check(peek_message(&message,own,0x8511,0x8511,1),"broadcast reaches own top-level window");
    }
    if(own_hook)unhook(own_hook);
    if(own)destroy_window(own);if(own_menu)destroy_menu(own_menu);
    /* Leave one context to process teardown, including a live job grant. */
    HANDLE exit_context=create_imc();check(exit_context!=0,"context for exit cleanup");
    if(exit_context&&(mask&1))check(grant_user(exit_context,job,1),"own input context grant");
}
static void root(void)
{
    HANDLE job=create_job(0,0);check(job!=0,"create USER job");if(!job)return;
    DWORD mask=1;
    if(!set_job(job,4,&mask,4)){check(0,"configure USER handle restriction");close_handle(job);return;}
    check(handle_flags(job,1,1),"inherit job handle");
    HANDLE window=create_window(0,"STATIC","Job handles root",0,0,0,32,32,0,0,0,0);
    HANDLE menu=create_menu();check(window&&menu,"create root USER objects");
    if(!window||!menu)goto done;
    HANDLE child_item=create_window(0,"STATIC","ungranted child",0x40000000,0,0,8,8,window,(HANDLE)42,0,0);
    check(child_item!=0,"create child with separate USER identity");
    parent_imc=create_imc();check(parent_imc!=0,"create root input context");
    associate_imc(window,parent_imc);
    parent_icon=test_icon();
    check(parent_icon!=0,"create root icon");
    SetLastError(1234);check(grant_user(window,job,0)&&GetLastError()==1234,"absent revoke preserves LastError");
    HANDLE query=0,set=0;
    check(dup_handle(SELF,job,SELF,&query,4,0,0),"duplicate query-only job");
    check(dup_handle(SELF,job,SELF,&set,2,0,0),"duplicate set-only job");
    check(!grant_user(window,query,1)&&GetLastError()==87,"grant requires SET_ATTRIBUTES");
    SetLastError(1234);check(grant_user(window,set,1)&&GetLastError()==1234,"set-only job grants USER access");
    check(grant_user(window,job,0),"remove initial grant");
    close_handle(query);close_handle(set);
    check(!grant_user(0,job,1)&&GetLastError()==87,"null USER object rejected");
    check(!grant_user(window,0,1)&&GetLastError()==87,"null job rejected");
    for(DWORD phase=0;phase<8;phase++){
        mask=(phase==0||phase==4)?0:1;
        check(set_job(job,4,&mask,4),"update handle restriction");
        DWORD expected=(mask==0)?31:0;
        HANDLE hook=set_hook(3,hook_proc,0,window_pid(window,0));
        check(hook!=0&&hook!=menu,"hook/menu have distinct USER identities");
        if(phase==2||phase>=6){
            check(grant_user(window,job,1)&&grant_user(window,job,1),"idempotent window grant");
            expected|=1;
        }
        if(phase==2||phase==7){check(grant_user(menu,job,1),"menu grant");expected|=2;}
        if(phase==2){check(grant_user(hook,job,1),"hook grant");expected|=4;}
        if(phase==2){
            check(grant_user(parent_icon,job,1),"icon grant");
            check(grant_user(parent_imc,job,1),"input context grant");expected|=24;
        }
        if(phase==3){
            check(grant_user(window,job,0)&&grant_user(window,job,0),"idempotent window revoke");
            check(grant_user(menu,job,0),"menu revoke");
            check(grant_user(parent_icon,job,0)&&grant_user(parent_imc,job,0),"icon/input context revoke");
        }
        member(job,window,menu,hook,expected,1,phase==1);
        if(hook)unhook(hook);
        test_window(window,1);test_menu(menu,1);
        if(scratch&&mask){MSG msg;check(!peek_message(&msg,window,0x8511,0x8511,1),
                                      "grant does not extend restricted broadcast domain");}
    }
    mask=0;check(set_job(job,4,&mask,4),"clear handles before destroy");
    check(!grant_user(window,job,1)&&GetLastError()==87,"grant requires restricted job");
    mask=1;set_job(job,4,&mask,4);
    destroy_window(window);destroy_menu(menu);
    check(!grant_user(window,job,1)&&GetLastError()==87,"destroyed window cannot be granted");
    check(!grant_user(menu,job,1)&&GetLastError()==87,"destroyed menu cannot be granted");
    window=menu=0;
done:
    if(window)destroy_window(window);if(menu)destroy_menu(menu);
    if(parent_icon)destroy_icon(parent_icon);if(parent_imc)destroy_imc(parent_imc);
    close_handle(job);
}
void mainCRTStartup(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),u=LoadLibraryA("user32.dll"),im=LoadLibraryA("imm32.dll");
    HANDLE g=LoadLibraryA("gdi32.dll");
#define LOAD(m,v,n) v=GetProcAddress(m,n);if(!v){report("JOB-UI-HANDLES MISSING " n "\n");ExitProcess(3);}
    LOAD(k,create_job,"CreateJobObjectW");LOAD(k,set_job,"SetInformationJobObject");
    LOAD(k,query_job,"QueryInformationJobObject");LOAD(k,assign_job,"AssignProcessToJobObject");
    LOAD(k,close_handle,"CloseHandle");LOAD(k,handle_flags,"SetHandleInformation");
    LOAD(k,dup_handle,"DuplicateHandle");LOAD(k,create_process,"CreateProcessA");
    LOAD(k,resume_thread,"ResumeThread");LOAD(k,wait_object,"WaitForSingleObject");
    LOAD(k,terminate_process,"TerminateProcess");LOAD(k,exit_code,"GetExitCodeProcess");
    LOAD(k,module_name,"GetModuleFileNameA");LOAD(k,command_line,"GetCommandLineA");
    LOAD(u,create_window,"CreateWindowExA");LOAD(u,destroy_window,"DestroyWindow");
    LOAD(u,is_window,"IsWindow");LOAD(u,window_rect,"GetWindowRect");
    LOAD(u,window_pid,"GetWindowThreadProcessId");LOAD(u,window_text,"GetWindowTextA");
    LOAD(u,post_message,"PostMessageA");LOAD(u,send_timeout,"SendMessageTimeoutW");
    LOAD(u,get_dc,"GetDC");LOAD(u,release_dc,"ReleaseDC");LOAD(u,enum_windows,"EnumWindows");
    LOAD(u,create_menu,"CreatePopupMenu");LOAD(u,destroy_menu,"DestroyMenu");
    LOAD(u,is_menu,"IsMenu");LOAD(u,menu_count,"GetMenuItemCount");
    LOAD(u,grant_user,"UserHandleGrantAccess");LOAD(u,peek_message,"PeekMessageA");
    LOAD(u,set_hook,"SetWindowsHookExA");LOAD(u,unhook,"UnhookWindowsHookEx");
    LOAD(u,post_thread,"PostThreadMessageA");
    LOAD(u,call_proc,"CallWindowProcA");
#if __SIZEOF_POINTER__ == 4
    LOAD(u,window_long,"GetWindowLongA");
#else
    LOAD(u,window_long,"GetWindowLongPtrA");
#endif
    LOAD(u,set_pos,"SetWindowPos");LOAD(u,get_item,"GetDlgItem");
    LOAD(im,create_imc,"ImmCreateContext");LOAD(im,get_imc,"ImmGetContext");
    LOAD(im,associate_imc,"ImmAssociateContext");LOAD(im,get_conversion,"ImmGetConversionStatus");
    LOAD(im,destroy_imc,"ImmDestroyContext");LOAD(u,create_icon_indirect,"CreateIconIndirect");
    LOAD(g,create_bitmap,"CreateBitmap");LOAD(g,delete_object,"DeleteObject");
    LOAD(u,copy_icon,"CopyIcon");LOAD(u,copy_image,"CopyImage");LOAD(u,destroy_icon,"DestroyIcon");
#undef LOAD
    module_name(0,path,sizeof(path));scratch=contains(command_line(),"--scratch")||token('s');
    if(contains(command_line(),"--child"))child();
    else{root();report(failures?"JOB-UI-HANDLES FAILED\n":"JOB-UI-HANDLES PASS\n");}
    ExitProcess(failures?1:0);
}
