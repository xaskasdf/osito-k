/* GetDCEx contract and pixel tests. Native windows live on a private desktop;
 * the fixture never switches the user's visible desktop or draws to it. */
typedef unsigned int DWORD,UINT;
typedef int BOOL;
typedef void *HANDLE;
typedef __INTPTR_TYPE__ LRESULT,LPARAM;
typedef __UINTPTR_TYPE__ WPARAM;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
void *memset(void *p,int v,__SIZE_TYPE__ n){unsigned char *d=p;while(n--)*d++=(unsigned char)v;return p;}
void *memcpy(void *p,const void *s,__SIZE_TYPE__ n){unsigned char *d=p;const unsigned char *a=s;while(n--)*d++=*a++;return p;}
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {int x,y;} POINT;
typedef LRESULT (CALL *WNDPROC)(HANDLE,UINT,WPARAM,LPARAM);
typedef struct {UINT style;WNDPROC proc;int cls,wnd;HANDLE instance,icon,cursor,brush;const char *menu,*name;} WCLASS;
typedef struct {DWORD size;int width,height;unsigned short planes,bpp;DWORD compression,bytes;int xppm,yppm;DWORD used,important;} BMI;
typedef HANDLE (CALL *dcex_fn)(HANDLE,HANDLE,DWORD);
typedef HANDLE (CALL *handle_fn)(HANDLE);
typedef BOOL (CALL *bool_fn)(HANDLE);
typedef int (CALL *release_fn)(HANDLE,HANDLE);
typedef HANDLE (CALL *region_fn)(int,int,int,int);
typedef int (CALL *combine_fn)(HANDLE,HANDLE,HANDLE,int);
typedef int (CALL *box_fn)(HANDLE,RECT *);
typedef DWORD (CALL *pixel_fn)(HANDLE,int,int,DWORD);
typedef DWORD (CALL *read_fn)(HANDLE,int,int);
typedef DWORD (CALL *color_fn)(HANDLE,DWORD);
typedef DWORD (CALL *get_color_fn)(HANDLE);
typedef BOOL (CALL *pat_fn)(HANDLE,int,int,int,int,DWORD);
typedef BOOL (CALL *blt_fn)(HANDLE,int,int,int,int,HANDLE,int,int,DWORD);
typedef int (CALL *dib_fn)(HANDLE,int,int,int,int,int,int,int,int,const void *,const BMI *,UINT,DWORD);
typedef HANDLE (CALL *bitmap_fn)(HANDLE,int,int);
typedef int (CALL *save_fn)(HANDLE);
typedef BOOL (CALL *restore_fn)(HANDLE,int);
typedef HANDLE (CALL *create_fn)(DWORD,const char *,const char *,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void *);
static HANDLE user,kernel,gdi,window,desktop,old_desktop;
static dcex_fn get_dcex;
static release_fn release,select_clip;
static handle_fn window_from_dc,compatible_dc;
static bool_fn delete_object,delete_dc,destroy_window,set_desktop,close_desktop;
static region_fn rect_region;
static combine_fn combine;
static box_fn region_box;
static pixel_fn set_pixel;
static read_fn get_pixel;
static color_fn set_color;
static get_color_fn get_color;
static pat_fn pat;
static blt_fn blt;
static dib_fn dib;
static bitmap_fn bitmap;
static HANDLE (CALL *select_object)(HANDLE,HANDLE);
static save_fn save_dc;
static restore_fn restore_dc;
static create_fn create_window;
static unsigned checks,failures;
static BOOL native;
static void dcx_report(const char *text){DWORD n=0,w;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&w,0);}
static void dcx_check(BOOL ok,const char *text){checks++;if(!ok)failures++;dcx_report(ok?"DCEX PASS ":"DCEX FAIL ");dcx_report(text);dcx_report("\n");}
static void *dcx_resolve(HANDLE module,const char *name){void *p=GetProcAddress(module,name);if(!p){dcx_report("DCEX FAIL missing ");dcx_report(name);dcx_report("\n");ExitProcess(2);}return p;}
static BOOL dcx_contains(const char *text,const char *word){for(;*text;text++){const char *a=text,*b=word;while(*b&&*a==*b){a++;b++;}if(!*b)return 1;}return 0;}
static void dcx_private_setup(void)
{
    HANDLE (CALL *create)(const char *,const char *,void *,DWORD,DWORD,void *)=dcx_resolve(user,"CreateDesktopA");
    HANDLE (CALL *get)(DWORD)=dcx_resolve(user,"GetThreadDesktop");
    DWORD (CALL *tid)(void)=dcx_resolve(kernel,"GetCurrentThreadId");
    DWORD (CALL *pid)(void)=dcx_resolve(kernel,"GetCurrentProcessId");
    set_desktop=dcx_resolve(user,"SetThreadDesktop");close_desktop=dcx_resolve(user,"CloseDesktop");
    old_desktop=get(tid());char name[]="OsitoDCEx-00000000";DWORD id=pid();
    for(int i=0;i<8;i++)name[10+i]="0123456789abcdef"[(id>>((7-i)*4))&15];
    desktop=create(name,0,0,0,0xF01FF,0);
    if(!desktop||!set_desktop(desktop)){dcx_report("DCEX FAIL private desktop\n");ExitProcess(2);}
    dcx_report("DCEX PRIVATE DESKTOP READY\n");
}
static void dcx_paint_checks(int mode)
{
    HANDLE base=get_dcex(window,0,2),mem=compatible_dc(base),bmp=bitmap(base,32,24);
    HANDLE prev=select_object(mem,bmp);
    dcx_check(base&&mem&&bmp&&prev,"pixel fixture");
    pat(mem,0,0,32,24,0x00FF0062); /* WHITENESS */
    DWORD pixels[32*24];for(unsigned i=0;i<32*24;i++)pixels[i]=0x00FFFFFF;
    BMI info={40,32,-24,1,32,0,32*24*4,0,0,0,0};
    for(int operation=0;operation<3;operation++) {
        pat(base,0,0,32,24,0x42); /* BLACKNESS */
        HANDLE region=rect_region(84,105,98,118);
        HANDLE dc=get_dcex(window,region,2|(mode?0x40:0x80));
        BOOL painted=operation==0?pat(dc,0,0,32,24,0x00FF0062):operation==1?
            blt(dc,0,0,32,24,mem,0,0,0x00CC0020):dib(dc,0,0,32,24,0,0,32,24,pixels,&info,0,0x00CC0020)>0;
        BOOL match=painted;
        for(int y=0;y<24;y++)for(int x=0;x<32;x++) {
            BOOL inside=x>=4&&x<18&&y>=5&&y<18;
            DWORD expected=(mode?!inside:inside)?0xFFFFFF:0;
            if(get_pixel(base,x,y)!=expected)match=0;
        }
        dcx_check(match,operation==0?"PatBlt honors system region":operation==1?
            "BitBlt honors system region":"DIB honors system region");
        dcx_check(release(window,dc)==1,"release owns region");
    }
    select_object(mem,prev);dcx_check(delete_object(bmp)&&delete_dc(mem)&&release(window,base),"pixel fixture cleanup");
}
static HANDLE abandoned_dc,abandoned_region;
static DWORD CALL dcx_thread(void *arg)
{
    (void)arg;
    if(native&&!set_desktop(desktop))return 1;
    abandoned_region=rect_region(84,105,98,118);
    abandoned_dc=get_dcex(window,abandoned_region,2|0x80);
    return abandoned_dc?0:1;
}
static void dcx_probe(void)
{
    HANDLE dc=get_dcex(window,0,2),other=get_dcex(window,0,2);
    dcx_check(dc&&other&&dc!=other&&window_from_dc(dc)==window,"independent simultaneous cached DCs");
    dcx_check(set_color(dc,0x123456)==0&&get_color(other)==0,"cached attributes isolated");
    dcx_check(set_pixel(dc,6,7,0x123456)==0x123456,"cached DC draws to client");
    dcx_check(release(window,dc)==1&&release(window,dc)==0,"cached release and duplicate release");
    dc=get_dcex(window,0,2|4);dcx_check(dc&&get_color(dc)==0,"NORESETATTRS ignored");release(window,dc);release(window,other);
    BOOL (CALL *move)(HANDLE,int,int,int,int,BOOL)=dcx_resolve(user,"MoveWindow");
    BOOL (CALL *show)(HANDLE,int)=dcx_resolve(user,"ShowWindow");
    dc=get_dcex(window,0,2);move(window,80,100,64,48,0);
    dcx_check(set_pixel(dc,56,44,0x123456)==0x123456,"retained DC follows larger surface");
    show(window,0);dcx_check(set_pixel(dc,6,7,0x123456)==0xFFFFFFFF,"retained DC follows hidden window");
    show(window,5);dcx_check(set_pixel(dc,56,44,0x123456)==0x123456,"retained DC follows visible window");
    move(window,80,100,48,40,0);dcx_check(set_pixel(dc,56,44,0x123456)==0xFFFFFFFF,"retained DC follows smaller surface");release(window,dc);
    HANDLE mem=compatible_dc(0);dcx_check(release(0,mem)==0&&delete_dc(mem),"ReleaseDC rejects memory DC");
    dcx_check(get_dcex((HANDLE)0x12345,0,2)==0,"invalid window rejected");
    HANDLE region=rect_region(84,105,98,118);RECT box;
    dc=get_dcex(window,region,2);release(window,dc);
    dcx_check(region_box(region,&box)==2,"unused region remains caller owned");delete_object(region);
    region=rect_region(84,105,98,118);dc=get_dcex(window,region,2|0x80);
    dcx_check(dc&&set_pixel(dc,6,7,0xABCDEF)==0xABCDEF&&set_pixel(dc,2,2,0xABCDEF)==0xFFFFFFFF,"screen-space intersection");
    select_clip(dc,0);dcx_check(set_pixel(dc,2,2,0xABCDEF)==0xFFFFFFFF,"clearing app clip preserves system clip");
    int saved=save_dc(dc);HANDLE app=rect_region(8,8,12,12);select_clip(dc,app);delete_object(app);
    dcx_check(set_pixel(dc,6,7,0xABCDEF)==0xFFFFFFFF&&set_pixel(dc,9,9,0xABCDEF)==0xABCDEF,"app and system regions intersect");
    dcx_check(restore_dc(dc,saved)&&set_pixel(dc,6,7,0xABCDEF)==0xABCDEF&&set_pixel(dc,2,2,0xABCDEF)==0xFFFFFFFF,"RestoreDC preserves system clip");
    move(window,180,200,48,40,0);
    dcx_check(set_pixel(dc,6,7,0xABCDEF)==0xABCDEF&&set_pixel(dc,2,2,0xABCDEF)==0xFFFFFFFF,"extra region stays relative to moved window");
    move(window,80,100,48,40,0);
    release(window,dc);
    dc=get_dcex(window,0,2|0x80);dcx_check(dc&&set_pixel(dc,6,7,0x123456)==0xFFFFFFFF,"NULL intersection is empty");release(window,dc);
    dc=get_dcex(window,0,2|0x40);dcx_check(dc&&set_pixel(dc,6,7,0x123456)==0x123456,"NULL exclusion leaves client");release(window,dc);
    region=rect_region(81,101,85,105);HANDLE part=rect_region(90,110,95,115);combine(region,region,part,2);delete_object(part);
    dc=get_dcex(window,region,2|0x80);
    dcx_check(dc&&set_pixel(dc,2,2,0x123456)==0x123456&&set_pixel(dc,12,12,0x123456)==0x123456&&set_pixel(dc,7,7,0x123456)==0xFFFFFFFF,"complex intersection retains hole");release(window,dc);
    HANDLE child=create_window(0,"OsitoDCEx","child",0x50000000,4,5,14,13,window,0,0,0);
    dc=get_dcex(window,0,2|8);dcx_check(child&&dc&&set_pixel(dc,6,7,0x123456)==0xFFFFFFFF&&set_pixel(dc,2,2,0x123456)==0x123456,"CLIPCHILDREN excludes child");release(window,dc);
    int (CALL *set_style)(HANDLE,int,int)=dcx_resolve(user,"SetWindowLongA");
    dc=get_dcex(window,0,0x10002);
    dcx_check(dc&&window_from_dc(dc)==window&&set_pixel(dc,6,7,0x123456)==0x123456,"USESTYLE CACHE targets client without style clipping");release(window,dc);
    dc=get_dcex(window,0,0x10002|8);
    dcx_check(dc&&set_pixel(dc,6,7,0x123456)==0x123456,"USESTYLE replaces explicit child clipping");release(window,dc);
    DWORD old_style=set_style(window,-16,0x92000000);
    dc=get_dcex(window,0,2);
    dcx_check(dc&&set_pixel(dc,6,7,0x123456)==0x123456,"CACHE alone does not inherit child clipping");release(window,dc);
    dc=get_dcex(window,0,0x10002);
    dcx_check(dc&&set_pixel(dc,6,7,0x123456)==0xFFFFFFFF&&set_pixel(dc,2,2,0x123456)==0x123456,"USESTYLE inherits child clipping");release(window,dc);
    dc=get_dcex(window,0,0x10002|1);
    dcx_check(dc&&set_pixel(dc,6,7,0x123456)==0x123456,"USESTYLE WINDOW ignores child clipping");release(window,dc);
    set_style(window,-16,old_style);
    HANDLE sibling=create_window(0,"OsitoDCEx","sibling",0x50000000,8,9,10,10,window,0,0,0);
    bool_fn raise=dcx_resolve(user,"BringWindowToTop");raise(sibling);
    dc=get_dcex(child,0,0x10002|16);
    dcx_check(dc&&set_pixel(dc,5,5,0x123456)==0x123456,"USESTYLE replaces explicit sibling clipping");release(child,dc);
    old_style=set_style(child,-16,0x54000000);
    dc=get_dcex(child,0,0x10002);
    dcx_check(dc&&set_pixel(dc,5,5,0x123456)==0xFFFFFFFF&&set_pixel(dc,1,1,0x123456)==0x123456,"USESTYLE inherits sibling clipping");release(child,dc);
    set_style(child,-16,old_style);
    dc=get_dcex(child,0,2|16);dcx_check(sibling&&dc&&set_pixel(dc,5,5,0x123456)==0xFFFFFFFF&&set_pixel(dc,1,1,0x123456)==0x123456,"CLIPSIBLINGS excludes higher sibling");
    destroy_window(sibling);dcx_check(set_pixel(dc,5,5,0x123456)==0x123456,"retained DC follows sibling destruction");release(child,dc);
    destroy_window(child);
    HANDLE frame=create_window(0,"OsitoDCEx","frame",0x10CF0000,200,200,180,120,0,0,0,0);
    BOOL (CALL *client_screen)(HANDLE,POINT *)=dcx_resolve(user,"ClientToScreen");
    box_fn window_rect=dcx_resolve(user,"GetWindowRect");
    POINT origin={0,0};RECT outer;client_screen(frame,&origin);window_rect(frame,&outer);
    HANDLE whole=get_dcex(frame,0,2|1),client=get_dcex(frame,0,2);
    dcx_check(frame&&whole&&client&&set_pixel(whole,0,0,0x123456)==0x123456&&
        set_pixel(client,2,2,0xABCDEF)==0xABCDEF&&
        get_pixel(whole,origin.x-outer.left+2,origin.y-outer.top+2)==0xABCDEF,
        "WINDOW DC includes nonclient and shares client pixels");
    release(frame,whole);release(frame,client);destroy_window(frame);
    dcx_paint_checks(0);dcx_paint_checks(1);
    show(window,0);
    dc=get_dcex(window,0,2);dcx_check(dc&&set_pixel(dc,6,7,0x123456)==0xFFFFFFFF,"hidden window has empty visible region");release(window,dc);show(window,5);
    HANDLE (CALL *thread)(void *,__SIZE_TYPE__,DWORD(CALL *)(void *),void *,DWORD,DWORD *)=dcx_resolve(kernel,"CreateThread");
    DWORD (CALL *wait)(HANDLE,DWORD)=dcx_resolve(kernel,"WaitForSingleObject");
    bool_fn close=dcx_resolve(kernel,"CloseHandle");
    HANDLE worker=thread(0,0,dcx_thread,0,0,0);
    BOOL joined=worker&&wait(worker,5000)==0;
    dcx_check(joined&&abandoned_dc&&region_box(abandoned_region,&box)==0,"thread exit releases cached DC and owned region");
    if(worker)close(worker);
    region=rect_region(84,105,98,118);dc=get_dcex(window,region,2|0x80);
    dcx_check(dc&&destroy_window(window)&&region_box(region,&box)==0,"window destruction releases cached DC and region");
    window=0;
}
void mainCRTStartup(void)
{
    kernel=LoadLibraryA("kernel32.dll");user=LoadLibraryA("user32.dll");gdi=LoadLibraryA("gdi32.dll");
    const char *(CALL *command)(void)=dcx_resolve(kernel,"GetCommandLineA");
    native=dcx_contains(command(),"--private");
    if(!native&&!dcx_contains(command(),"--qemu")){dcx_report("DCEX FAIL explicit isolated mode required\n");ExitProcess(2);}
    if(native)dcx_private_setup();
#define U(field,name) field=dcx_resolve(user,name)
#define G(field,name) field=dcx_resolve(gdi,name)
    U(get_dcex,"GetDCEx");U(release,"ReleaseDC");U(window_from_dc,"WindowFromDC");U(create_window,"CreateWindowExA");U(destroy_window,"DestroyWindow");
    G(rect_region,"CreateRectRgn");G(combine,"CombineRgn");G(region_box,"GetRgnBox");G(select_clip,"SelectClipRgn");G(delete_object,"DeleteObject");G(delete_dc,"DeleteDC");
    G(set_pixel,"SetPixel");G(get_pixel,"GetPixel");G(set_color,"SetTextColor");G(get_color,"GetTextColor");G(pat,"PatBlt");G(blt,"BitBlt");G(dib,"StretchDIBits");
    G(compatible_dc,"CreateCompatibleDC");G(bitmap,"CreateCompatibleBitmap");G(select_object,"SelectObject");G(save_dc,"SaveDC");G(restore_dc,"RestoreDC");
    WCLASS cls={0};cls.proc=dcx_resolve(user,"DefWindowProcA");cls.name="OsitoDCEx";
    unsigned short (CALL *reg)(const WCLASS *)=dcx_resolve(user,"RegisterClassA");
    BOOL (CALL *unreg)(const char *,HANDLE)=dcx_resolve(user,"UnregisterClassA");
    dcx_check(reg(&cls)!=0,"register class");
    window=create_window(0,cls.name,"DCEx contract",0x90000000,80,100,48,40,0,0,0,0);
    dcx_check(window!=0,"create window");if(window)dcx_probe();
    dcx_check(unreg(cls.name,0),"unregister class");
    if(native)dcx_check(set_desktop(old_desktop)&&close_desktop(desktop),"private desktop cleanup");
    dcx_report("DCEX DONE\n");ExitProcess(failures?1:0);
}
