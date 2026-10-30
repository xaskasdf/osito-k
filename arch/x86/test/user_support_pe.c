/* USER32 launcher support. Native runs use a private desktop for windows and
 * read-only clipboard access; ownership mutations run only with --qemu in an
 * isolated VM. No test switches the visible desktop. */
typedef unsigned int DWORD, UINT;
typedef int BOOL;
typedef void *HANDLE;
typedef __INTPTR_TYPE__ LRESULT, LPARAM;
typedef __UINTPTR_TYPE__ WPARAM;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
void *memset(void *p, int value, __SIZE_TYPE__ count)
{ unsigned char *out=p; while(count--) *out++=(unsigned char)value; return p; }
void *memcpy(void *p, const void *s, __SIZE_TYPE__ count)
{ unsigned char *out=p; const unsigned char *in=s; while(count--) *out++=*in++; return p; }
typedef LRESULT (CALL *WNDPROC)(HANDLE,UINT,WPARAM,LPARAM);
typedef struct {
    UINT style; WNDPROC proc; int cls_extra,wnd_extra;
    HANDLE instance,icon,cursor,brush; const char *menu,*name;
} WNDCLASS;
typedef struct { int left,top,right,bottom; } RECT;
typedef HANDLE (CALL *window_fn)(DWORD,const char *,const char *,DWORD,
    int,int,int,int,HANDLE,HANDLE,HANDLE,void *);
typedef BOOL (CALL *handle_bool_fn)(HANDLE);
typedef BOOL (CALL *void_bool_fn)(void);
typedef HANDLE (CALL *void_handle_fn)(void);
typedef DWORD (CALL *id_fn)(void);
typedef const char *(CALL *command_fn)(void);
typedef BOOL (CALL *show_fn)(HANDLE,int);
typedef BOOL (CALL *rect_fn)(HANDLE,RECT *);
typedef unsigned short (CALL *register_fn)(const WNDCLASS *);
typedef HANDLE (CALL *thread_fn)(void *,__SIZE_TYPE__,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
typedef DWORD (CALL *wait_fn)(HANDLE,DWORD);
static window_fn create_window;
static handle_bool_fn destroy_window,open_icon,is_iconic,is_zoomed,is_visible;
static handle_bool_fn open_clip;
static void_bool_fn close_clip,empty_clip;
static void_handle_fn clip_owner,active_window;
static show_fn show_window;
static rect_fn get_rect;
static WNDPROC default_proc;
static thread_fn create_thread;
static wait_fn wait;
static handle_bool_fn close_handle;
static volatile unsigned query_count,clipboard_destroy_count;
static volatile BOOL allow_restore=1;
static volatile BOOL destroy_on_query;
static unsigned checks,failures;
static HANDLE module_user,module_kernel;

static void report(const char *text)
{
    DWORD length=0,written; while(text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11),text,length,&written,0);
}
static void hex(DWORD value)
{
    char text[9];
    for(int i=0;i<8;i++) text[i]="0123456789abcdef"[(value>>((7-i)*4))&15];
    text[8]=0;report(text);
}
static void check(BOOL condition,const char *name)
{
    checks++; report(condition?"U32SUP PASS ":"U32SUP FAIL ");report(name);report("\n");
    if(!condition) failures++;
}
static void observe(BOOL value,const char *name)
{
    DWORD error=GetLastError();report("U32SUP OBS ");report(name);report(" value=");hex(value);
    report(" error=");hex(error);report("\n");
}
static void *resolve(HANDLE module,const char *name)
{
    void *result=GetProcAddress(module,name);
    if(!result){report("U32SUP FAIL missing ");report(name);report("\n");ExitProcess(2);}
    return result;
}
static BOOL contains(const char *text,const char *word)
{
    for(;*text;text++) {const char *a=text,*b=word;while(*b&&*a==*b){a++;b++;}if(!*b)return 1;}
    return 0;
}

static HANDLE old_desktop,new_desktop;
static handle_bool_fn set_desktop,close_desktop;
static BOOL native_readonly;
static void private_setup(void)
{
    typedef HANDLE (CALL *desktop_fn)(const char *,const char *,void *,DWORD,DWORD,void *);
    typedef HANDLE (CALL *thread_desktop_fn)(DWORD);
    desktop_fn create_desktop=(desktop_fn)resolve(module_user,"CreateDesktopA");
    thread_desktop_fn get_desktop=(thread_desktop_fn)resolve(module_user,"GetThreadDesktop");
    id_fn tid=(id_fn)resolve(module_kernel,"GetCurrentThreadId");
    id_fn pid=(id_fn)resolve(module_kernel,"GetCurrentProcessId");
    set_desktop=(handle_bool_fn)resolve(module_user,"SetThreadDesktop");
    close_desktop=(handle_bool_fn)resolve(module_user,"CloseDesktop");
    old_desktop=get_desktop(tid());
    char name[]="OsitoUserSupport-00000000";DWORD identity=pid();
    for(int i=0;i<8;i++)name[17+i]="0123456789abcdef"[(identity>>((7-i)*4))&15];
    new_desktop=create_desktop(name,0,0,0,0x000F01FF,0);
    if(!new_desktop||!set_desktop(new_desktop)||get_desktop(tid())!=new_desktop){
        report("U32SUP FAIL private desktop setup error=");hex(GetLastError());report("\n");ExitProcess(2);
    }
    native_readonly=1;
    report("U32SUP PRIVATE DESKTOP READY; CLIPBOARD READ-ONLY\n");
}
static void private_finish(void)
{
    if(!new_desktop)return;
    check(set_desktop(old_desktop),"restore original desktop");
    check(close_desktop(new_desktop),"release private desktop");
}
static LRESULT CALL callback(HANDLE hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    if(message==0x13){query_count++;if(destroy_on_query)destroy_window(hwnd);return allow_restore;}
    if(message==0x307){clipboard_destroy_count++;return 0;}
    return default_proc(hwnd,message,wp,lp);
}
static BOOL same_rect(const RECT *a,const RECT *b)
{ return a->left==b->left&&a->top==b->top&&a->right==b->right&&a->bottom==b->bottom; }
static void icon_probe(HANDLE window)
{
    RECT normal,restored;
    SetLastError(0);check(!open_icon(0)&&GetLastError()==1400,"OpenIcon rejects invalid window");
    SetLastError(0);observe(open_icon(window),"OpenIcon on hidden noniconic window");
    observe(is_visible(window),"visibility after noniconic OpenIcon");
    check(open_icon(window)&&!is_visible(window),"noniconic hidden window remains hidden");
    show_window(window,1);get_rect(window,&normal);
    show_window(window,6);check(is_iconic(window),"minimized fixture");
    query_count=0;allow_restore=0;
    SetLastError(0);BOOL veto=open_icon(window);observe(veto,"OpenIcon veto return");
    check(veto&&query_count==1&&is_iconic(window),"WM_QUERYOPEN veto preserves minimized state");
    allow_restore=1;
    check(open_icon(window)&&query_count==2&&!is_iconic(window),"OpenIcon restores after consent");
    check(get_rect(window,&restored)&&same_rect(&normal,&restored),"restore normal rectangle");
    check(active_window()==window,"restored window is activated");
    show_window(window,3);show_window(window,6);
    check(open_icon(window)&&!is_zoomed(window)&&!is_iconic(window),"OpenIcon restores normal state after maximized minimize");
    check(get_rect(window,&restored)&&same_rect(&normal,&restored),"OpenIcon uses normal placement rectangle");
    show_window(window,1);
    check(default_proc(window,0x13,0,0)==1,"default window procedure allows query open");
}
static volatile BOOL thread_open,thread_close;
static volatile DWORD thread_open_error,thread_close_error;
static DWORD CALL competing_clipboard(void *arg)
{
    (void)arg;SetLastError(0);thread_open=open_clip(0);thread_open_error=GetLastError();
    SetLastError(0);thread_close=close_clip();thread_close_error=GetLastError();return 0;
}
static DWORD CALL abandon_clipboard(void *arg)
{
    (void)arg;thread_open=open_clip(0);return 0;
}
static void abandoned_clipboard_probe(HANDLE window)
{
    HANDLE owner=clip_owner();
    HANDLE thread=create_thread(0,0,abandon_clipboard,0,0,0);
    check(thread&&wait(thread,5000)==0&&thread_open,"thread exits with clipboard open");
    if(thread)close_handle(thread);
    check(open_clip(window),"thread exit releases clipboard lock");
    check(close_clip()&&clip_owner()==owner,"abandoned opener does not change content owner");
}
static void clipboard_probe(HANDLE first,HANDLE second)
{
    if(native_readonly){
        HANDLE owner=clip_owner();
        check(open_clip(first)&&clip_owner()==owner,"open preserves user's clipboard owner");
        SetLastError(0);observe(open_clip(first),"repeat OpenClipboard same window and thread");
        SetLastError(0);observe(open_clip(second),"repeat OpenClipboard other window same thread");
        HANDLE thread=create_thread(0,0,competing_clipboard,0,0,0);
        check(thread&&wait(thread,5000)==0,"clipboard contender joined");
        if(thread)close_handle(thread);
        check(!thread_open&&thread_open_error==5,"other thread cannot acquire open clipboard");
        check(!thread_close&&thread_close_error==1418,"other thread cannot close clipboard");
        check(close_clip()&&clip_owner()==owner,"close preserves user's clipboard owner");
        SetLastError(0);observe(close_clip(),"second close after repeated open");
        check(destroy_window(second),"destroy unused clipboard fixture");
        return;
    }
    SetLastError(0);check(!empty_clip()&&GetLastError()==1418,"empty requires open clipboard");
    SetLastError(0);check(!close_clip()&&GetLastError()==1418,"close requires open clipboard");
    check(!clip_owner(),"fresh private clipboard has no owner");
    check(open_clip(first)&&!clip_owner(),"open does not assign ownership");
    check(empty_clip()&&clip_owner()==first,"empty assigns opening window as owner");
    SetLastError(0);observe(open_clip(first),"repeat OpenClipboard same window and thread");
    SetLastError(0);observe(open_clip(second),"repeat OpenClipboard other window same thread");
    HANDLE thread=create_thread(0,0,competing_clipboard,0,0,0);
    check(thread&&wait(thread,5000)==0,"clipboard contender joined");
    if(thread)close_handle(thread);
    check(!thread_open&&thread_open_error==5,"other thread cannot acquire open clipboard");
    check(!thread_close&&thread_close_error==1418,"other thread cannot close clipboard");
    check(close_clip()&&clip_owner()==first,"close retains ownership");
    SetLastError(0);observe(close_clip(),"second close after repeated open");
    check(open_clip(second)&&clip_owner()==first,"new opener preserves owner until empty");
    unsigned old=clipboard_destroy_count;
    check(empty_clip()&&clip_owner()==second,"ownership transfer");
    check(clipboard_destroy_count==old+1,"old owner receives WM_DESTROYCLIPBOARD");
    check(close_clip(),"close new owner");
    check(open_clip(0)&&empty_clip()&&!clip_owner()&&close_clip(),"null opener clears ownership");
    check(open_clip(second)&&empty_clip()&&close_clip(),"assign owner before destruction");
    check(destroy_window(second)&&!clip_owner(),"destroyed window cannot remain clipboard owner");
    SetLastError(0);check(!open_clip(second)&&GetLastError()==1400,"reject destroyed clipboard window");
}
void mainCRTStartup(void)
{
    module_kernel=LoadLibraryA("kernel32.dll");module_user=LoadLibraryA("user32.dll");
    command_fn command=(command_fn)resolve(module_kernel,"GetCommandLineA");
    if(contains(command(),"--private"))private_setup();
    else if(!contains(command(),"--qemu")){report("U32SUP REFUSED: pass --private on Windows or --qemu in an isolated VM\n");ExitProcess(2);}
    create_window=(window_fn)resolve(module_user,"CreateWindowExA");
    destroy_window=(handle_bool_fn)resolve(module_user,"DestroyWindow");
    open_icon=(handle_bool_fn)resolve(module_user,"OpenIcon");
    is_iconic=(handle_bool_fn)resolve(module_user,"IsIconic");
    is_zoomed=(handle_bool_fn)resolve(module_user,"IsZoomed");
    is_visible=(handle_bool_fn)resolve(module_user,"IsWindowVisible");
    show_window=(show_fn)resolve(module_user,"ShowWindow");
    get_rect=(rect_fn)resolve(module_user,"GetWindowRect");
    active_window=(void_handle_fn)resolve(module_user,"GetActiveWindow");
    default_proc=(WNDPROC)resolve(module_user,"DefWindowProcA");
    open_clip=(handle_bool_fn)resolve(module_user,"OpenClipboard");
    close_clip=(void_bool_fn)resolve(module_user,"CloseClipboard");
    empty_clip=(void_bool_fn)resolve(module_user,"EmptyClipboard");
    clip_owner=(void_handle_fn)resolve(module_user,"GetClipboardOwner");
    create_thread=(thread_fn)resolve(module_kernel,"CreateThread");
    wait=(wait_fn)resolve(module_kernel,"WaitForSingleObject");
    close_handle=(handle_bool_fn)resolve(module_kernel,"CloseHandle");
    register_fn register_class=(register_fn)resolve(module_user,"RegisterClassA");
    WNDCLASS cls={0};cls.proc=callback;cls.name="OsitoUserSupport";
    check(register_class(&cls)!=0,"register fixture class");
    HANDLE first=create_window(0,cls.name,"First",0x00CF0000,30,40,180,140,0,0,0,0);
    HANDLE second=create_window(0,cls.name,"Second",0x00CF0000,240,40,180,140,0,0,0,0);
    if(!first||!second){report("U32SUP FAIL fixture windows\n");ExitProcess(2);}
    icon_probe(first);abandoned_clipboard_probe(first);clipboard_probe(first,second);
    HANDLE reentrant=create_window(0,cls.name,"Destroy in QueryOpen",0x00CF0000,30,40,180,140,0,0,0,0);
    check(reentrant!=0,"create reentrant restore fixture");
    show_window(reentrant,6);destroy_on_query=1;
    BOOL restored=open_icon(reentrant);destroy_on_query=0;
    handle_bool_fn is_window=(handle_bool_fn)resolve(module_user,"IsWindow");
    check(restored&&!is_window(reentrant),"query callback may destroy its window");
    check(destroy_window(first),"destroy remaining fixture");
    typedef BOOL(CALL *unregister_fn)(const char *,HANDLE);
    unregister_fn unregister_class=(unregister_fn)resolve(module_user,"UnregisterClassA");
    check(unregister_class(cls.name,0),"unregister fixture class");
    private_finish();
    report("U32SUP DONE checks=");hex(checks);report(" failures=");hex(failures);report("\n");
    ExitProcess(failures?1:0);
}
