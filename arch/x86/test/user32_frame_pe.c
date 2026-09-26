/* Hidden-window geometry and nonclient contracts, identical on Windows/Osito. */
typedef unsigned int UINT, DWORD;
typedef int BOOL, LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef __INTPTR_TYPE__ IPTR;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct {
    HANDLE window; UINT message; UPTR wp; IPTR lp;
    DWORD time; POINT point; DWORD private_data;
} MSG;
typedef IPTR (CALL *WNDPROC)(HANDLE, UINT, UPTR, IPTR);
typedef struct {
    UINT style; WNDPROC proc; int cls_extra, wnd_extra;
    HANDLE instance, icon, cursor, background;
    const char *menu, *name;
} WNDCLASS;
typedef struct {
    DWORD size; RECT window, client; DWORD style, ex_style, status;
    UINT border_x, border_y; unsigned short atom, version;
} WINDOWINFO;
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL GetModuleHandleA(const char *);
API unsigned short CALL RegisterClassA(const WNDCLASS *);
API BOOL CALL UnregisterClassA(const char *, HANDLE);
API HANDLE CALL CreateWindowExA(DWORD, const char *, const char *, DWORD,
    int, int, int, int, HANDLE, HANDLE, HANDLE, void *);
API BOOL CALL DestroyWindow(HANDLE);
API BOOL CALL IsWindow(HANDLE);
API IPTR CALL DefWindowProcA(HANDLE, UINT, UPTR, IPTR);
API IPTR CALL SendMessageA(HANDLE, UINT, UPTR, IPTR);
API BOOL CALL GetClientRect(HANDLE, RECT *);
API BOOL CALL GetWindowRect(HANDLE, RECT *);
API BOOL CALL GetWindowInfo(HANDLE, WINDOWINFO *);
API BOOL CALL ClientToScreen(HANDLE, POINT *);
API BOOL CALL ScreenToClient(HANDLE, POINT *);
API int CALL MapWindowPoints(HANDLE, HANDLE, POINT *, UINT);
API BOOL CALL AdjustWindowRectEx(RECT *, DWORD, BOOL, DWORD);
API BOOL CALL SetWindowPos(HANDLE, HANDLE, int, int, int, int, UINT);
API BOOL CALL ShowWindow(HANDLE, int);
API BOOL CALL UpdateWindow(HANDLE);
API BOOL CALL GetMessageA(MSG *, HANDLE, UINT, UINT);
API BOOL CALL TranslateMessage(const MSG *);
API IPTR CALL DispatchMessageA(const MSG *);
API void CALL PostQuitMessage(int);
API HANDLE CALL GetFocus(void);
API BOOL CALL IsIconic(HANDLE);
API BOOL CALL IsZoomed(HANDLE);
API UPTR CALL SetTimer(HANDLE, UPTR, UINT, void *);
API BOOL CALL KillTimer(HANDLE, UPTR);
static UINT checks, failures, nc_false, nc_true;
static HANDLE custom;
static const char name[] = "OsitoNonclientContracts";

void *memset(void *p, int c, __SIZE_TYPE__ n)
{
    unsigned char *b=p; while(n--) *b++=(unsigned char)c; return p;
}
static void report(const char *text)
{
    DWORD count=0, written; while(text[count]) count++;
    WriteFile(GetStdHandle((DWORD)-11),text,count,&written,0);
}
static void number(UINT value)
{
    char text[12]; UINT n=0;
    do {text[n++]='0'+value%10; value/=10;} while(value);
    while(n) {char s[2]={text[--n],0}; report(s);}
}
static void check(BOOL ok, const char *text)
{
    checks++; if(!ok) {failures++; report("FAIL "); report(text); report("\n");}
}
static IPTR CALL proc(HANDLE window, UINT message, UPTR wp, IPTR lp)
{
    if(message==0x83) {
        if(wp) nc_true++; else nc_false++;
        if(window==custom && wp) return 0;
    }
    return DefWindowProcA(window,message,wp,lp);
}
static IPTR point_lparam(int x, int y)
{
    return (IPTR)(unsigned short)x|((IPTR)(unsigned short)y<<16);
}
static void geometry(DWORD style, DWORD ex)
{
    RECT adjusted={0,0,320,180};
    check(AdjustWindowRectEx(&adjusted,style,0,ex),"adjust outer rectangle");
    BOOL frame=(style&0x00C00000)==0x00C00000;
    if(frame) check(adjusted.left<0 && adjusted.top<adjusted.left,"caption contributes to size");
    UINT before=nc_false;
    HANDLE window=CreateWindowExA(ex,name,"Geometry",style,100,100,
        adjusted.right-adjusted.left,adjusted.bottom-adjusted.top,0,0,GetModuleHandleA(0),0);
    check(window!=0,"create window");
    if(!window) return;
    check(nc_false>before,"creation dispatches NCCALCSIZE");
    RECT client, outer;
    check(GetClientRect(window,&client) && client.right==320 && client.bottom==180,
          "adjust/create client size roundtrip");
    check(GetWindowRect(window,&outer) && outer.left==100 && outer.top==100,
          "outer origin is not client origin");
    POINT point={0,0};
    check(ClientToScreen(window,&point) && point.x==100-adjusted.left && point.y==100-adjusted.top,
          "client origin includes frame");
    POINT origin=point;
    check(ScreenToClient(window,&point) && !point.x && !point.y,"screen/client inverse");
    WINDOWINFO wi={0}; wi.size=sizeof(wi);
    check(GetWindowInfo(window,&wi) && wi.client.left==origin.x && wi.client.top==origin.y &&
          wi.client.right-wi.client.left==320 && wi.client.bottom-wi.client.top==180,
          "WINDOWINFO separates client and outer rectangles");
    check(SendMessageA(window,0x84,0,point_lparam(origin.x+10,origin.y+10))==1,
          "client hit test");
    if(frame) check(SendMessageA(window,0x84,0,point_lparam(outer.left+100,origin.y-8))==2,
          "caption hit test");
    HANDLE child=CreateWindowExA(0,name,"child",0x40800000,11,13,80,60,
        window,0,GetModuleHandleA(0),0);
    check(child!=0,"child created");
    RECT cr;
    check(GetWindowRect(child,&cr) && cr.left==origin.x+11 && cr.top==origin.y+13,
          "child position is parent client relative");
    point=(POINT){0,0};
    MapWindowPoints(child,window,&point,1);
    check(point.x==12 && point.y==14,"nested client offsets include child border");
    before=nc_true;
    check(SetWindowPos(window,0,130,120,400,250,0x14),"resize outer rectangle");
    check(nc_true>before,"resize dispatches NCCALCSIZE_PARAMS");
    check(GetClientRect(window,&client) && client.right==400+adjusted.left-(adjusted.right-320) &&
          client.bottom==250+adjusted.top-(adjusted.bottom-180),"resize maintains client insets");
    DestroyWindow(child);
    SendMessageA(window,0x112,0xF060,0);
    check(!IsWindow(window),"system close destroys default window");
}
static void custom_frame(void)
{
    custom=CreateWindowExA(0,name,"Custom",0x00CF0000,100,100,400,250,0,0,GetModuleHandleA(0),0);
    check(custom!=0,"custom frame window");
    if(!custom) return;
    check(SetWindowPos(custom,0,0,0,0,0,0x37),"frame changed without size change");
    RECT r; POINT p={0,0};
    check(GetClientRect(custom,&r) && r.right==400 && r.bottom==250,
          "NCCALCSIZE override removes default frame");
    check(ClientToScreen(custom,&p) && p.x==100 && p.y==100,"custom frame client origin");
    check(SendMessageA(custom,0x84,0,point_lparam(110,110))==1,"no caption hit in custom client");
    DestroyWindow(custom); custom=0;
}
#ifdef FRAME_INTERACTIVE
static UINT clicked, typed, moved, maximized, restored, minimized, closed;
static RECT initial_outer;

static void interactive_points(HANDLE window, const char *stage)
{
    WINDOWINFO wi={0}; wi.size=sizeof(wi);
    GetWindowInfo(window,&wi);
    POINT points[5]={{wi.client.left+30,wi.client.top+30},
        {wi.window.left+100,wi.client.top-10},{0,0},{0,0},{0,0}};
    const UINT hits[]={8,9,20};
    int y=wi.client.top-10;
    for(UINT i=0;i<3;i++) {
        int first=-1,last=-1;
        for(int x=wi.window.left;x<wi.window.right;x++) {
            if(SendMessageA(window,0x84,0,point_lparam(x,y))==(IPTR)hits[i]) {
                if(first<0) first=x;
                last=x;
            }
        }
        check(first>=0,"caption button has a hit region");
        points[i+2]=(POINT){(first+last)/2,y};
    }
    report("[FRAME-UI] "); report(stage);
    for(UINT i=0;i<5;i++) {
        report(" "); number((UINT)points[i].x);
        report(" "); number((UINT)points[i].y);
    }
    report("\n");
}

static IPTR CALL interactive_proc(HANDLE window, UINT message, UPTR wp, IPTR lp)
{
    if(message==0x202) {
        /* Absolute HID positions may round down one pixel when quantized. */
        check((short)lp>=29 && (short)lp<=30 && (short)(lp>>16)>=29 && (short)(lp>>16)<=30,
              "physical click uses client coordinates");
        check(GetFocus()==window,"physical click gives keyboard focus");
        clicked++;
        report("[FRAME-UI] clicked\n");
    } else if(message==0x100 && wp=='A') {
        typed++;
        report("[FRAME-UI] typed\n");
    } else if(message==0x232) {
        RECT rect; GetWindowRect(window,&rect);
        check(rect.left==initial_outer.left+50 && rect.top==initial_outer.top+30,
              "caption drag moves outer origin");
        check(rect.right-rect.left==initial_outer.right-initial_outer.left &&
              rect.bottom-rect.top==initial_outer.bottom-initial_outer.top,
              "caption drag preserves size");
        moved++;
        interactive_points(window,"moved");
    } else if(message==0x112) {
        IPTR result=DefWindowProcA(window,message,wp,lp);
        if((wp&0xFFF0)==0xF030) {
            check(IsZoomed(window),"caption maximize enters maximized state");
            maximized++;
            interactive_points(window,"maximized");
        } else if((wp&0xFFF0)==0xF120) {
            check(!IsZoomed(window) && !IsIconic(window),"caption restore returns to normal");
            restored++;
            interactive_points(window,"restored");
        } else if((wp&0xFFF0)==0xF020) {
            check(IsIconic(window),"caption minimize enters iconic state");
            minimized++;
            SetTimer(window,1,600,0);
            report("[FRAME-UI] minimized\n");
        }
        return result;
    } else if(message==0x113 && wp==1) {
        KillTimer(window,1);
        ShowWindow(window,9);
        interactive_points(window,"unminimized");
        return 0;
    } else if(message==0x10) {
        closed++;
    } else if(message==2) {
        PostQuitMessage(0);
    }
    return DefWindowProcA(window,message,wp,lp);
}

static void interactive(void)
{
    WNDCLASS wc={0}; wc.proc=interactive_proc; wc.instance=GetModuleHandleA(0);
    wc.name="OsitoNonclientInput"; wc.background=(HANDLE)6;
    check(RegisterClassA(&wc)!=0,"interactive class");
    RECT r={0,0,320,180};
    AdjustWindowRectEx(&r,0x00CF0000,0,0);
    HANDLE window=CreateWindowExA(0,wc.name,"Window input test",0x00CF0000,
        100,100,r.right-r.left,r.bottom-r.top,0,0,wc.instance,0);
    check(window!=0,"interactive window");
    if(!window) return;
    ShowWindow(window,5);
    UpdateWindow(window);
    GetWindowRect(window,&initial_outer);
    interactive_points(window,"ready");
    MSG message;
    while(GetMessageA(&message,0,0,0)>0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    check(clicked==1 && typed==1,"physical client mouse and keyboard events");
    check(moved==1,"one completed caption drag");
    check(maximized==1 && restored==1 && minimized==1,"caption state transitions");
    check(closed==1 && !IsWindow(window),"caption close completes destruction");
    check(UnregisterClassA(wc.name,wc.instance),"interactive class released");
}
#endif
void mainCRTStartup(void)
{
#ifdef FRAME_INTERACTIVE
    interactive();
#else
    WNDCLASS wc={0}; wc.proc=proc; wc.instance=GetModuleHandleA(0); wc.name=name;
    check(RegisterClassA(&wc)!=0,"class registered");
    geometry(0x00CF0000,0);
    geometry(0x80C80000,0);
    geometry(0x80000000,0);
    geometry(0x80C80000,0x80);
    geometry(0x00CF0000,0x200);
    custom_frame();
    check(UnregisterClassA(name,wc.instance),"class released");
#endif
    report("[USER32-FRAME] checks="); number(checks); report(" failures="); number(failures);
    report(failures ? " FAIL\n" : " PASS\n");
    ExitProcess(failures?3:0);
}
