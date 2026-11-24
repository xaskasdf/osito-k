/* A real two-page wizard: ABI, notification vetoes, ownership and modal loop. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UP;
typedef __INTPTR_TYPE__ IP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
static DWORD failures,refs,adds,creates,releases,init_count,active_count,next_count,back_count,finish_count,cancel_count,reset_count;
static int veto_next,veto_cancel,veto_kill,veto_finish,modal;
static void *page_windows[2];
#define SAY(s) do{DWORD w;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("PROPSHEET PASS " name "\n");else{SAY("PROPSHEET FAIL " name "\n");failures++;}}while(0)
typedef struct {
    DWORD size,flags;void *instance;const void *resource;void *icon;const char *title;
    void *proc;IP param;void *callback;DWORD *ref_parent;
} PAGE;
typedef struct {
    DWORD size,flags;void *parent,*instance,*icon;const char *caption;
    DWORD count;UP start;const void *pages;void *callback;
} HEADER;
typedef struct {void *from;UP id;DWORD code;IP param;} NOTIFY;
typedef IP (CALL *sendfn)(void *,DWORD,UP,IP);
typedef int (CALL *postfn)(void *,DWORD,UP,IP);
typedef IP (CALL *setfn)(void *,int,IP);
typedef IP (CALL *getfn)(void *,int);
static sendfn send;
static postfn post;
static setfn set;
static getfn get;
static IP CALL propsheet_page_proc(void *window,DWORD message,UP wparam,IP lparam)
{
    (void)wparam;
    if(message==0x110){PAGE *page=(PAGE *)lparam;UP index=page->param;
        if(index<2){page_windows[index]=window;set(window,2*sizeof(UP),(IP)index);init_count++;}
        return 1;
    }
    if(message==0x8001){set(window,0,(IP)(sizeof(UP)==8?0x1234567887654321ULL:0x76543210));return 1;}
    if(message==0x8002)return 1;
    if(message!=0x4E)return 0;
    NOTIFY *notice=(NOTIFY *)lparam;DWORD code=notice->code;
    UP index=(UP)get(window,2*sizeof(UP));
    IP result=0;
    if(code==(DWORD)-200){active_count++;send(notice->from,0x470,0,index?5:2);}
    if(code==(DWORD)-201)result=veto_kill;
    if(code==(DWORD)-207){next_count++;result=veto_next?-1:0;}
    if(code==(DWORD)-206)back_count++;
    if(code==(DWORD)-208){finish_count++;result=veto_finish;}
    if(code==(DWORD)-209){cancel_count++;result=veto_cancel;}
    if(code==(DWORD)-203)reset_count++;
    set(window,0,result);
    return 1;
}
static DWORD CALL propsheet_page_callback(void *window,DWORD message,PAGE *page)
{
    if(window||!page||page->size!=sizeof(PAGE)||page->param<0||page->param>1)failures++;
    if(message==0){adds++;SAY("PROPSHEET CALLBACK ADDREF\n");}
    if(message==1)releases++;
    if(message==2)creates++;
    return 1;
}
static int CALL propsheet_header_callback(void *window,DWORD message,IP param)
{
    (void)param;
    if(message==1&&modal)post(window,0x471,2,0);
    return 0;
}
static int propsheet_child_entry(void)
{
    void *kernel=LoadLibraryA("kernel32.dll");
    typedef const char *(CALL *CMD)(void);
    CMD command=(CMD)GetProcAddress(kernel,"GetCommandLineA");
    const char *args=command();int parent=0;
    for(unsigned i=0;args[i];i++)if(args[i]==' '&&args[i+1]=='p'&&args[i+2]=='a'&&args[i+3]=='r'&&args[i+4]=='e'&&args[i+5]=='n'&&args[i+6]=='t'&&!args[i+7])parent=1;
    if(!parent)return 0;
    typedef struct {DWORD cb;void *reserved,*desktop,*title;DWORD x,y,w,h,xc,yc,fill,flags;
        WORD show,bytes;void *data,*input,*output,*error;} STARTUP;
    typedef struct {void *process,*thread;DWORD pid,tid;} PROCESS;
    typedef DWORD(CALL *MODULE)(void *,char *,DWORD);
    typedef int(CALL *CREATE)(const char *,char *,void *,void *,int,DWORD,void *,const char *,STARTUP *,PROCESS *);
    typedef DWORD(CALL *WAIT)(void *,DWORD);
    typedef int(CALL *EXITCODE)(void *,DWORD *);
    typedef int(CALL *CLOSE)(void *);
    MODULE module=(MODULE)GetProcAddress(kernel,"GetModuleFileNameA");
    CREATE create=(CREATE)GetProcAddress(kernel,"CreateProcessA");
    WAIT wait=(WAIT)GetProcAddress(kernel,"WaitForSingleObject");
    EXITCODE exitcode=(EXITCODE)GetProcAddress(kernel,"GetExitCodeProcess");
    CLOSE close=(CLOSE)GetProcAddress(kernel,"CloseHandle");
    static char path[1024],child_command[1050];
    DWORD n=module(0,path,sizeof(path));CHECK(n&&n<sizeof(path),"child-executable-path");
    if(!n||n>=sizeof(path))ExitProcess(1);
    unsigned length=0;child_command[length++]='"';
    for(unsigned i=0;i<n;i++)child_command[length++]=path[i];
    child_command[length++]='"';child_command[length]=0;
    STARTUP startup={0};PROCESS process={0};startup.cb=sizeof(startup);
    int made=create(path,child_command,0,0,0,0,0,0,&startup,&process);
    CHECK(made,"create-wizard-child");
    /* Diagnostic guests synchronously log each window; allow the child to
     * finish under UART overhead without tearing down its GUI mid-test. */
    if(made){DWORD code=99;CHECK(wait(process.process,120000)==0&&exitcode(process.process,&code)&&!code,"child-wizard-completes");close(process.thread);close(process.process);}
    if(!failures)SAY("PROPSHEET CHILD DONE\n");ExitProcess(failures?1:0);return 1;
}
void propsheet_test_entry(void)
{
    if(propsheet_child_entry())return;
    void *u=LoadLibraryA("user32.dll"),*c=LoadLibraryA("comctl32.dll");
    typedef int(CALL *KEYBOARD)(int);
    typedef void(CALL *SETERROR)(DWORD);
    typedef DWORD(CALL *ERROR)(void);
    void *kernel=LoadLibraryA("kernel32.dll");
    KEYBOARD keyboard=(KEYBOARD)GetProcAddress(u,"GetKeyboardType");
    SETERROR seterror=(SETERROR)GetProcAddress(kernel,"SetLastError");
    ERROR error=(ERROR)GetProcAddress(kernel,"GetLastError");
    CHECK(keyboard&&seterror&&error,"keyboard-query-exports");
    if(keyboard&&seterror&&error){
        seterror(0x13579);
        CHECK(keyboard(0)==4&&keyboard(1)==0&&keyboard(2)==12,"enhanced-pc-keyboard");
        CHECK(keyboard(-1)==0&&keyboard(3)==0&&error()==0x13579,"keyboard-query-last-error");
    }
    typedef void *(CALL *desktopfn)(const WORD *,const WORD *,void *,DWORD,DWORD,void *);
    typedef int (CALL *setdesktopfn)(void *);
    desktopfn desktop=GetProcAddress(u,"CreateDesktopW");
    setdesktopfn setdesktop=GetProcAddress(u,"SetThreadDesktop");
    if(desktop&&setdesktop){void *d=desktop((const WORD *)L"OsitoPropSheetTest",0,0,0,0xF01FF,0);CHECK(d&&setdesktop(d),"private-desktop");}
    typedef void *(CALL *pagefn)(const PAGE *);
    typedef int (CALL *destroypagefn)(void *);
    typedef IP (CALL *sheetfn)(const HEADER *);
    typedef int (CALL *destroyfn)(void *);
    typedef int (CALL *windowfn)(void *);
    typedef void *(CALL *itemfn)(void *,int);
    typedef struct {int left,top,right,bottom;} RECT;
    typedef int(CALL *rectfn)(void *,RECT *);
    typedef int(CALL *textfn)(void *,char *,int);
    typedef void (CALL *initfn)(void);
    pagefn create=GetProcAddress(c,"CreatePropertySheetPageA");
    destroypagefn release=GetProcAddress(c,"DestroyPropertySheetPage");
    sheetfn sheet=GetProcAddress(c,"PropertySheetA");
    initfn init=GetProcAddress(c,"InitCommonControls");
    destroyfn destroy=GetProcAddress(u,"DestroyWindow");
    windowfn iswindow=GetProcAddress(u,"IsWindow");
    windowfn enabled=GetProcAddress(u,"IsWindowEnabled");
    itemfn item=GetProcAddress(u,"GetDlgItem");
    rectfn clientrect=GetProcAddress(u,"GetClientRect");
    rectfn windowrect=GetProcAddress(u,"GetWindowRect");
    textfn windowtext=GetProcAddress(u,"GetWindowTextA");
    send=GetProcAddress(u,"SendMessageA");post=GetProcAddress(u,"PostMessageA");
    set=GetProcAddress(u,sizeof(UP)==4?"SetWindowLongA":"SetWindowLongPtrA");
    get=GetProcAddress(u,sizeof(UP)==4?"GetWindowLongA":"GetWindowLongPtrA");
    CHECK(create&&release&&sheet&&init&&send&&set&&get,"exports");
    if(failures)ExitProcess(1);
    init();
    /* comctl32 v5 temporarily patches indirect dialog styles in place. */
    static WORD dialog[]={0x0400,0x4000,0,0,0,0,0,180,80,0,0,0};
    static WORD large_dialog[]={0x0440,0x40c8,0,0,1,0,0,420,270,0,0,0,
        8,'M','S',' ','S','h','e','l','l',' ','D','l','g',0,
        0,0x5000,0,0,400,250,20,12,777,0xffff,0x82,'X',0,0};
    PAGE p={sizeof(PAGE),0xC9,0,dialog,0,"DirectX setup",propsheet_page_proc,0,propsheet_page_callback,&refs};
    void *standalone=create(&p);
    CHECK(standalone&&refs==1&&creates==0,"page-is-lazy-and-referenced");
    CHECK(release(standalone)&&refs==0&&releases==1,"release-unshown-page");
    void *pages[2];pages[0]=create(&p);p.param=1;p.resource=large_dialog;pages[1]=create(&p);
    CHECK(pages[0]&&pages[1]&&refs==2,"create-two-page-handles");
    HEADER h={sizeof(HEADER),0x20|0x400|0x100,0,0,0,"Installer test",2,0,pages,propsheet_header_callback};
    void *window=(void *)sheet(&h);
    CHECK(window&&(IP)window!=-1&&iswindow(window),"create-modeless-wizard");
    if(!window||(IP)window==-1)ExitProcess(1);
    CHECK(init_count==1&&creates==1&&page_windows[0]&&send(window,0x476,0,0)==(IP)page_windows[0],"first-page-lazy-activation");
    RECT initial_sheet={0};CHECK(windowrect&&clientrect&&windowrect(window,&initial_sheet),"initial-sheet-geometry");
    CHECK(send(page_windows[0],0x8001,0,0)==(IP)(sizeof(UP)==8?0x1234567887654321ULL:0x76543210),"dialog-message-result-width");
    CHECK(send(page_windows[0],0x8002,0,0)==0,"dialog-message-result-reset");
    CHECK(item(window,0x3024)&&item(window,0x3023)&&enabled(item(window,0x3024))&&!enabled(item(window,0x3023)),"wizard-button-state");
    char before_close[64]={0},after_close[64]={0};
    int old_length=windowtext(item(window,2),before_close,sizeof(before_close));
    send(window,0x46B,0,0);
    int new_length=windowtext(item(window,2),after_close,sizeof(after_close));
    SAY("PROPSHEET CLOSE BEFORE ");{DWORD w;WriteFile(GetStdHandle((DWORD)-11),before_close,old_length,&w,0);}SAY(" AFTER ");{DWORD w;WriteFile(GetStdHandle((DWORD)-11),after_close,new_length,&w,0);}SAY("\n");
    int changed=old_length!=new_length;
    for(int i=0;i<old_length&&i<new_length;i++)if(before_close[i]!=after_close[i])changed=1;
    CHECK(old_length&&new_length&&!changed&&!enabled(item(window,2)),"cancel-to-close-disables-wizard-cancel");
    veto_next=1;send(window,0x471,1,0);
    CHECK(next_count==1&&init_count==1&&send(window,0x476,0,0)==(IP)page_windows[0],"next-veto");
    veto_next=0;veto_kill=1;send(window,0x471,1,0);
    CHECK(send(window,0x476,0,0)==(IP)page_windows[0],"kill-active-veto");
    veto_kill=0;send(window,0x471,1,0);
    CHECK(init_count==2&&creates==2&&page_windows[1]&&send(window,0x476,0,0)==(IP)page_windows[1],"next-opens-second-page");
    RECT current_sheet={0},page_rect={0},edge_rect={0},sheet_client={0};
    void *edge=item(page_windows[1],777);
    CHECK(edge&&windowrect(page_windows[1],&page_rect)&&windowrect(edge,&edge_rect)&&
        edge_rect.left>=page_rect.left&&edge_rect.top>=page_rect.top&&
        edge_rect.right<=page_rect.right&&edge_rect.bottom<=page_rect.bottom,"largest-page-controls-fit");
    CHECK(windowrect(window,&current_sheet)&&current_sheet.right-current_sheet.left==initial_sheet.right-initial_sheet.left&&
        current_sheet.bottom-current_sheet.top==initial_sheet.bottom-initial_sheet.top&&
        clientrect(window,&sheet_client)&&page_rect.right-page_rect.left<=sheet_client.right&&
        page_rect.bottom-page_rect.top<sheet_client.bottom,"sheet-sized-before-lazy-page-creation");
    CHECK(!(get(page_windows[1],-16)&0x00c00000),"page-caption-is-removed");
    send(window,0x471,0,0);
    CHECK(back_count==1&&send(window,0x476,0,0)==(IP)page_windows[0],"back-reuses-first-page");
    veto_cancel=1;send(window,0x471,5,0);
    CHECK(cancel_count==1&&iswindow(window)&&send(window,0x476,0,0)==(IP)page_windows[0],"cancel-veto");
    veto_cancel=0;send(window,0x471,5,0);
    CHECK(cancel_count==2&&reset_count==2&&send(window,0x476,0,0)==0,"cancel-notifies-pages");
    CHECK(destroy(window)&&refs==0&&releases==3,"modeless-owner-releases-pages");
    init_count=0;p.param=0;pages[0]=create(&p);p.param=1;pages[1]=create(&p);
    h.count=2;h.start=1;window=(void *)sheet(&h);
    CHECK(window&&(IP)window!=-1&&init_count==1&&send(window,0x476,0,0)==(IP)page_windows[1],"explicit-start-page");
    if(!window||(IP)window==-1)ExitProcess(1);
    veto_finish=1;send(window,0x471,2,0);
    CHECK(finish_count==1&&send(window,0x476,0,0)==(IP)page_windows[1],"finish-veto");
    veto_finish=0;send(window,0x471,2,0);
    CHECK(finish_count==2&&send(window,0x476,0,0)==0&&send(window,0x487,0,0)==1,"finish-result");
    CHECK(destroy(window)&&refs==0&&releases==5,"finish-releases-pages");
    modal=1;p.param=0;pages[0]=create(&p);h.flags=0x20|0x100;h.count=1;h.start=0;
    CHECK(sheet(&h)==1&&refs==0&&releases==6,"modal-loop-and-release");
    modal=0;
    if(!failures)SAY("PROPSHEET DONE\n");
    ExitProcess(failures?1:0);
}
