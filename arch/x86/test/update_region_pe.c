/* Exact USER32 update regions, paint pixels, and reentrant erasure.
 * Run native windows only on an isolated desktop (--private), never switch
 * the user's desktop. --qemu is for a disposable guest. */
typedef unsigned int UINT,DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef __INTPTR_TYPE__ LP;
typedef __UINTPTR_TYPE__ UP;
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
typedef struct {HANDLE dc;BOOL erase;RECT rect;BOOL restore,inc;unsigned char reserved[32];} PAINT;
typedef LP(CALL *PROC)(HANDLE,UINT,UP,LP);
typedef struct {UINT style;PROC proc;int cls,wnd;HANDLE instance,icon,cursor,brush;const char *menu,*name;} WCLASS;
typedef struct {DWORD size;int width,height;unsigned short planes,bpp;DWORD compression,bytes;int xp,yp;DWORD used,important;} BMI;
static HANDLE user,gdi,kernel,window,desktop,old_desktop,callback_region;
static unsigned checks,failures,erasures;
static int native,erase_mode,inside_kind;
static RECT a={2,3,20,17},b={30,5,46,22},hole={9,8,14,14};
static BOOL(CALL *invalidate_rect)(HANDLE,const RECT *,BOOL);
static BOOL(CALL *validate_rect)(HANDLE,const RECT *);
static BOOL(CALL *invalidate_region)(HANDLE,HANDLE,BOOL);
static BOOL(CALL *validate_region)(HANDLE,HANDLE);
static int(CALL *update_region)(HANDLE,HANDLE,BOOL);
static BOOL(CALL *update_rect)(HANDLE,RECT *,BOOL);
static HANDLE(CALL *begin)(HANDLE,PAINT *);
static BOOL(CALL *end)(HANDLE,PAINT *);
static HANDLE(CALL *create)(DWORD,const char *,const char *,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void *);
static BOOL(CALL *destroy)(HANDLE);
static BOOL(CALL *is_window)(HANDLE);
static PROC default_proc;
static HANDLE(CALL *region_rect)(int,int,int,int);
static int(CALL *combine)(HANDLE,HANDLE,HANDLE,int);
static int(CALL *region_box)(HANDLE,RECT *);
static BOOL(CALL *point)(HANDLE,int,int);
static BOOL(CALL *delete_object)(HANDLE);
static HANDLE(CALL *dcex)(HANDLE,HANDLE,DWORD);
static int(CALL *release)(HANDLE,HANDLE);
static BOOL(CALL *pat)(HANDLE,int,int,int,int,DWORD);
static DWORD(CALL *pixel)(HANDLE,int,int);
static HANDLE(CALL *memory_dc)(HANDLE);
static HANDLE(CALL *bitmap)(HANDLE,int,int);
static HANDLE(CALL *select_object)(HANDLE,HANDLE);
static BOOL(CALL *delete_dc)(HANDLE);
static int(CALL *select_clip)(HANDLE,HANDLE);
static BOOL(CALL *blt)(HANDLE,int,int,int,int,HANDLE,int,int,DWORD);
static int(CALL *dib)(HANDLE,int,int,int,int,int,int,int,int,const void *,const BMI *,UINT,DWORD);
static BOOL(CALL *set_desktop)(HANDLE);
static BOOL(CALL *close_desktop)(HANDLE);

static void upd_report(const char *s){DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void upd_value(const char *s,DWORD v){char text[11]="0x00000000";for(UINT i=0;i<8;i++)text[2+i]="0123456789abcdef"[(v>>((7-i)*4))&15];upd_report(s);upd_report(text);upd_report("\n");}
static void upd_check(BOOL ok,const char *s){checks++;if(!ok)failures++;upd_report(ok?"UPDRGN PASS ":"UPDRGN FAIL ");upd_report(s);upd_report("\n");}
static void *upd_resolve(HANDLE m,const char *name){void *p=GetProcAddress(m,name);if(!p){upd_report("UPDRGN FAIL export ");upd_report(name);upd_report("\n");ExitProcess(2);}return p;}
static BOOL upd_contains(const char *s,const char *word){for(;*s;s++){const char *a=s,*b=word;while(*b&&*a==*b){a++;b++;}if(!*b)return 1;}return 0;}
static BOOL upd_same(RECT x,RECT y){return x.left==y.left&&x.top==y.top&&x.right==y.right&&x.bottom==y.bottom;}
static HANDLE upd_region(RECT r){return region_rect(r.left,r.top,r.right,r.bottom);}
static BOOL upd_in(RECT r,int x,int y){return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}
static HANDLE upd_shape(BOOL cut)
{
    HANDLE r=upd_region(a),other=upd_region(b);combine(r,r,other,2);delete_object(other);
    if(cut){other=upd_region(hole);combine(r,r,other,4);delete_object(other);}return r;
}
static BOOL upd_shape_matches(HANDLE r,BOOL cut)
{
    for(int y=0;y<48;y++)for(int x=0;x<64;x++)
        if(!!point(r,x,y)!=((upd_in(a,x,y)||upd_in(b,x,y))&&(!cut||!upd_in(hole,x,y))))return 0;
    return 1;
}
static LP CALL upd_proc(HANDLE h,UINT msg,UP wp,LP lp)
{
    if(msg==0x14) {
        erasures++;inside_kind=update_region(h,callback_region,0);
        if(erase_mode==1){validate_rect(h,0);invalidate_rect(h,&b,0);}
        if(erase_mode==2)destroy(h);
        if(erase_mode==3)return 0;
        if(erase_mode==4){select_clip((HANDLE)wp,0);pat((HANDLE)wp,0,0,64,48,0x00FF0062);}
        return 1;
    }
    return default_proc(h,msg,wp,lp);
}
static void upd_private(void)
{
    HANDLE(CALL *make)(const char *,const char *,void *,DWORD,DWORD,void *)=upd_resolve(user,"CreateDesktopA");
    HANDLE(CALL *get)(DWORD)=upd_resolve(user,"GetThreadDesktop");
    DWORD(CALL *tid)(void)=upd_resolve(kernel,"GetCurrentThreadId");
    DWORD(CALL *pid)(void)=upd_resolve(kernel,"GetCurrentProcessId");
    set_desktop=upd_resolve(user,"SetThreadDesktop");close_desktop=upd_resolve(user,"CloseDesktop");
    old_desktop=get(tid());char name[]="OsitoUpdate-00000000";DWORD id=pid();
    for(int i=0;i<8;i++)name[12+i]="0123456789abcdef"[(id>>((7-i)*4))&15];
    desktop=make(name,0,0,0,0xf01ff,0);
    if(!desktop||!set_desktop(desktop)){upd_report("UPDRGN FAIL private desktop\n");ExitProcess(2);}
    upd_report("UPDRGN PRIVATE DESKTOP READY\n");
}
static void upd_regions(void)
{
    HANDLE out=region_rect(0,0,0,0),shape=upd_shape(0);RECT bounds;
    validate_rect(window,0);
    upd_check(update_region(window,out,0)==1&&!update_rect(window,&bounds,0)&&upd_same(bounds,(RECT){0,0,0,0}),"empty update region");
    upd_check(invalidate_region(window,shape,0)&&update_region(window,out,0)==3&&upd_shape_matches(out,0),"disjoint invalidation is exact");
    upd_check(delete_object(shape)&&update_region(window,out,0)==3&&upd_shape_matches(out,0),"input region is copied");
    upd_check(validate_rect(window,&hole)&&update_region(window,out,0)==3&&upd_shape_matches(out,1),"partial rectangle validation preserves hole");
    shape=upd_region(a);
    upd_check(validate_region(window,shape)&&update_region(window,out,0)==2&&region_box(out,&bounds)==2&&upd_same(bounds,b),"region validation removes only its area");delete_object(shape);
    RECT strip={30,5,46,10};validate_rect(window,&strip);update_region(window,out,0);region_box(out,&bounds);
    upd_check(upd_same(bounds,(RECT){30,10,46,22}),"partial edge validation changes bounds");
    upd_check(validate_region(window,0)&&update_region(window,out,0)==1,"NULL region validates all");
    RECT reversed={20,17,2,3};invalidate_rect(window,&reversed,0);update_rect(window,&bounds,0);
    upd_check(upd_same(bounds,a),"reversed invalidation rectangle normalizes");
    validate_rect(window,0);RECT large={-10,-20,200,300};invalidate_rect(window,&large,0);update_rect(window,&bounds,0);
    upd_check(upd_same(bounds,(RECT){0,0,64,48}),"invalidation clips to client bounds");
    validate_rect(window,0);shape=region_rect(0,0,0,0);erasures=0;
    upd_check(invalidate_region(window,shape,1)&&update_region(window,out,1)==1&&erasures==0,"empty invalidation does not request erase");delete_object(shape);
    invalidate_rect(window,&a,0);SetLastError(0x4567);
    upd_check(validate_region(window,(HANDLE)(UP)0xdeadbeef)&&GetLastError()==6&&update_region(window,out,0)==2,"invalid region validation retains update");
    SetLastError(0x4567);upd_check(update_region(window,0,0)==0&&GetLastError()==6,"NULL update destination fails");
    SetLastError(0x4567);int bad=update_region(window,(HANDLE)(UP)0xdeadbeef,0);DWORD error=GetLastError();upd_value("UPDRGN OBS invalid destination result ",bad);upd_value("UPDRGN OBS invalid destination error ",error);upd_check(bad==1&&error==(sizeof(void *)==4?0x4567:6),"invalid non-NULL destination matches Windows ABI");
    SetLastError(0x4567);upd_check(!validate_region((HANDLE)(UP)0xdeadbeef,out)&&GetLastError()==1400,"invalid validation window fails");
    SetLastError(0x4567);upd_check(update_region((HANDLE)(UP)0xdeadbeef,out,0)==0&&GetLastError()==1400,"invalid query window fails");
    validate_rect(window,0);delete_object(out);
}
static void upd_paint_pixels(void)
{
    HANDLE dc=dcex(window,0,2),memory=memory_dc(dc),image=bitmap(dc,64,48),old=select_object(memory,image);
    pat(memory,0,0,64,48,0x00FF0062);static DWORD pixels[64*48];for(UINT i=0;i<64*48;i++)pixels[i]=0xffffff;
    BMI info={40,64,-48,1,32,0,sizeof(pixels),0,0,0,0};HANDLE out=region_rect(0,0,0,0);
    for(int operation=0;operation<3;operation++) {
        pat(dc,0,0,64,48,0x42);validate_rect(window,0);HANDLE shape=upd_shape(1);
        invalidate_region(window,shape,0);delete_object(shape);PAINT ps;HANDLE paint=begin(window,&ps);
        upd_check(paint&&upd_same(ps.rect,(RECT){2,3,46,22})&&update_region(window,out,0)==1,"BeginPaint consumes complex update");
        select_clip(paint,0);
        BOOL painted=operation==0?pat(paint,0,0,64,48,0x00FF0062):operation==1?
            blt(paint,0,0,64,48,memory,0,0,0x00CC0020):dib(paint,0,0,64,48,0,0,64,48,pixels,&info,0,0x00CC0020)>0;
        end(window,&ps);BOOL match=painted;
        for(int y=0;y<48;y++)for(int x=0;x<64;x++) {
            DWORD expected=((upd_in(a,x,y)||upd_in(b,x,y))&&!upd_in(hole,x,y))?0xffffff:0;
            if(pixel(dc,x,y)!=expected)match=0;
        }
        upd_check(match,operation==0?"PatBlt preserves update holes":operation==1?"BitBlt preserves update holes":"DIB preserves update holes");
    }
    pat(dc,0,0,64,48,0x42);HANDLE shape=upd_shape(1);invalidate_region(window,shape,1);delete_object(shape);
    erase_mode=4;erasures=0;upd_check(update_region(window,out,1)==3&&erasures==1,"query erases without validating");erase_mode=0;
    BOOL match=1;for(int y=0;y<48;y++)for(int x=0;x<64;x++) {
        DWORD expected=((upd_in(a,x,y)||upd_in(b,x,y))&&!upd_in(hole,x,y))?0xffffff:0;
        if(pixel(dc,x,y)!=expected)match=0;
    }
    upd_check(match,"WM_ERASEBKGND respects update holes");
    validate_rect(window,0);select_object(memory,old);
    upd_check(delete_object(out)&&delete_object(image)&&delete_dc(memory)&&release(window,dc),"paint resources released");
}
static void upd_reentrant(void)
{
    HANDLE out=region_rect(0,0,0,0);RECT bounds;PAINT ps;
    for(int operation=0;operation<3;operation++) {
        validate_rect(window,0);erase_mode=1;erasures=0;invalidate_rect(window,&a,1);
        BOOL ok;
        if(operation==0){ok=update_region(window,out,1)==2;region_box(out,&bounds);ok=ok&&upd_same(bounds,b);}
        else if(operation==1)ok=update_rect(window,&bounds,1)&&upd_same(bounds,b);
        else {ok=begin(window,&ps)!=0&&upd_same(ps.rect,a);end(window,&ps);}
        ok=ok&&erasures==1&&inside_kind==(operation==2?1:2)&&update_region(window,out,0)==2;
        region_box(out,&bounds);ok=ok&&upd_same(bounds,b);
        upd_check(ok,operation==0?"GetUpdateRgn observes erase callback changes":operation==1?"GetUpdateRect observes erase callback changes":"BeginPaint preserves callback reinvalidation");erase_mode=0;
    }
    validate_rect(window,0);erase_mode=3;erasures=0;invalidate_rect(window,&a,1);
    update_region(window,out,1);update_region(window,out,1);begin(window,&ps);
    upd_value("UPDRGN OBS unhandled erase calls ",erasures);upd_value("UPDRGN OBS unhandled erase flag ",ps.erase);
    upd_check(erasures==1&&ps.erase&&update_region(window,out,0)==1,"unhandled erase reports obligation without repeating callback");end(window,&ps);
    erasures=0;invalidate_rect(window,&a,1);begin(window,&ps);
    upd_check(erasures==1&&ps.erase&&update_region(window,out,0)==1,"BeginPaint reports directly unhandled erase");end(window,&ps);
    erasures=0;invalidate_rect(window,&a,1);update_region(window,out,1);invalidate_rect(window,&b,1);update_region(window,out,1);
    upd_check(erasures==2,"new invalidation renews erase request");validate_rect(window,0);erase_mode=0;
    for(int operation=0;operation<3;operation++) {
        HANDLE victim=create(0,"OsitoUpdate","erase-destroy",0x90000000,200,200,64,48,0,0,0,0);
        validate_rect(victim,0);invalidate_rect(victim,&a,1);erase_mode=2;erasures=0;
        BOOL ok;
        if(operation==0)ok=update_region(victim,out,1)==1;
        else if(operation==1)ok=!update_rect(victim,&bounds,1)&&upd_same(bounds,(RECT){0,0,0,0});
        else {ok=begin(victim,&ps)!=0&&upd_same(ps.rect,a);end(victim,&ps);}
        upd_check(ok&&!is_window(victim)&&erasures==1,"erase callback can destroy window");erase_mode=0;
    }
    delete_object(out);
}
static DWORD CALL upd_worker(void *argument)
{
    if(native&&!set_desktop(desktop))return 1;
    const RECT *r=argument;for(UINT i=0;i<250;i++)if(!invalidate_rect(window,r,0))return 2;return 0;
}
static void upd_concurrent(void)
{
    HANDLE(CALL *thread)(void *,__SIZE_TYPE__,DWORD(CALL *)(void *),void *,DWORD,DWORD *)=upd_resolve(kernel,"CreateThread");
    DWORD(CALL *wait)(HANDLE,DWORD)=upd_resolve(kernel,"WaitForSingleObject");
    BOOL(CALL *close)(HANDLE)=upd_resolve(kernel,"CloseHandle");
    BOOL(CALL *exit_code)(HANDLE,DWORD *)=upd_resolve(kernel,"GetExitCodeThread");
    validate_rect(window,0);HANDLE t1=thread(0,0,upd_worker,&a,0,0),t2=thread(0,0,upd_worker,&b,0,0);
    DWORD e1=9,e2=9;BOOL ok=t1&&t2&&wait(t1,10000)==0&&wait(t2,10000)==0&&exit_code(t1,&e1)&&exit_code(t2,&e2)&&!e1&&!e2;
    HANDLE out=region_rect(0,0,0,0);upd_check(ok&&update_region(window,out,0)==3&&upd_shape_matches(out,0),"concurrent invalidations preserve both areas");
    close(t1);close(t2);delete_object(out);validate_rect(window,0);
}
void mainCRTStartup(void)
{
    kernel=LoadLibraryA("kernel32.dll");user=LoadLibraryA("user32.dll");gdi=LoadLibraryA("gdi32.dll");
    const char *(CALL *command)(void)=upd_resolve(kernel,"GetCommandLineA");native=upd_contains(command(),"--private");
    if(!native&&!upd_contains(command(),"--qemu")){upd_report("UPDRGN FAIL isolated mode required\n");ExitProcess(2);}
    if(native)upd_private();
#define U(f,n) f=upd_resolve(user,n)
#define G(f,n) f=upd_resolve(gdi,n)
    U(invalidate_rect,"InvalidateRect");U(invalidate_region,"InvalidateRgn");U(validate_rect,"ValidateRect");U(validate_region,"ValidateRgn");
    U(update_region,"GetUpdateRgn");U(update_rect,"GetUpdateRect");U(begin,"BeginPaint");U(end,"EndPaint");U(create,"CreateWindowExA");U(destroy,"DestroyWindow");U(is_window,"IsWindow");
    U(default_proc,"DefWindowProcA");U(dcex,"GetDCEx");U(release,"ReleaseDC");
    G(region_rect,"CreateRectRgn");G(combine,"CombineRgn");G(region_box,"GetRgnBox");G(point,"PtInRegion");G(delete_object,"DeleteObject");
    G(pat,"PatBlt");G(pixel,"GetPixel");G(memory_dc,"CreateCompatibleDC");G(bitmap,"CreateCompatibleBitmap");G(select_object,"SelectObject");G(delete_dc,"DeleteDC");G(select_clip,"SelectClipRgn");G(blt,"BitBlt");G(dib,"StretchDIBits");
    callback_region=region_rect(0,0,0,0);WCLASS cls={0};cls.proc=upd_proc;cls.name="OsitoUpdate";
    unsigned short(CALL *reg)(const WCLASS *)=upd_resolve(user,"RegisterClassA");BOOL(CALL *unreg)(const char *,HANDLE)=upd_resolve(user,"UnregisterClassA");
    upd_check(reg(&cls)!=0,"register class");window=create(0,cls.name,"update region",0x90000000,80,100,64,48,0,0,0,0);
    upd_check(window!=0,"create window");if(window){upd_regions();upd_paint_pixels();upd_reentrant();upd_concurrent();}
    upd_check(destroy(window)&&delete_object(callback_region)&&unreg(cls.name,0),"window resources released");
    if(native)upd_check(set_desktop(old_desktop)&&close_desktop(desktop),"private desktop cleanup");
    upd_report("UPDRGN DONE\n");ExitProcess(failures?1:0);
}
