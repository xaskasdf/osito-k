/* Real PE32/PE64 input fixture: custom frame with two transparent child levels. */
typedef unsigned int UINT, DWORD;
typedef int BOOL, LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef __INTPTR_TYPE__ IPTR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
typedef struct { LONG x, y; } POINT;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { HANDLE window; UINT message; UPTR wp; IPTR lp; DWORD time; POINT point; DWORD private_data; } MSG;
typedef IPTR (CALL *WNDPROC)(HANDLE, UINT, UPTR, IPTR);
typedef struct { UINT style; WNDPROC proc; int cls_extra, wnd_extra; HANDLE instance, icon, cursor, background; const char *menu, *name; } WNDCLASS;
typedef struct { HANDLE dc; BOOL erase; RECT rect; BOOL restore, update; unsigned char reserved[32]; } PAINT;
typedef struct { POINT reserved, max_size, max_position, min_track, max_track; } MINMAX;
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API HANDLE CALL GetModuleHandleA(const char *);
API void CALL ExitProcess(DWORD);

static unsigned short (CALL *register_class)(const WNDCLASS *);
static HANDLE (CALL *create_window)(DWORD,const char*,const char*,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void*);
static BOOL (CALL *destroy_window)(HANDLE);
static IPTR (CALL *default_proc)(HANDLE,UINT,UPTR,IPTR);
static IPTR (CALL *send_message)(HANDLE,UINT,UPTR,IPTR);
static BOOL (CALL *window_rect)(HANDLE,RECT *);
static BOOL (CALL *client_rect)(HANDLE,RECT *);
static BOOL (CALL *position)(HANDLE,HANDLE,int,int,int,int,UINT);
static BOOL (CALL *get_message)(MSG *,HANDLE,UINT,UINT);
static BOOL (CALL *translate)(const MSG *);
static IPTR (CALL *dispatch)(const MSG *);
static void (CALL *quit)(int);
static HANDLE (CALL *begin_paint)(HANDLE,PAINT *);
static BOOL (CALL *end_paint)(HANDLE,const PAINT *);
static int (CALL *fill_rect)(HANDLE,const RECT *,HANDLE);
static HANDLE (CALL *brush)(DWORD);
static BOOL (CALL *delete_object)(HANDLE);
static BOOL (CALL *text_out)(HANDLE,int,int,const char *,int);
static DWORD (CALL *text_color)(HANDLE,DWORD);
static int (CALL *background_mode)(HANDLE,int);
static UPTR (CALL *set_timer)(HANDLE,UPTR,UINT,void *);
static HANDLE root, middle, leaf, fill;
static UINT enters, exits, sizing, moving, clicks, last_edge, failures;
static BOOL snap;
static const char class_name[]="OsitoCustomDesktopFixture";

void *memset(void *ptr,int value,__SIZE_TYPE__ bytes)
{ unsigned char *p=ptr; while(bytes--) *p++=(unsigned char)value; return ptr; }
void *memcpy(void *destination,const void *source,__SIZE_TYPE__ bytes)
{ unsigned char *d=destination; const unsigned char *s=source; while(bytes--) *d++=*s++; return destination; }
static void report(const char *text)
{ DWORD n=0,written; while(text[n]) n++; WriteFile(GetStdHandle((DWORD)-11),text,n,&written,0); }
static void number(int value)
{
    char text[12]; unsigned n=0,v;
    if(value<0) {report("-"); v=0u-(unsigned)value;} else v=(unsigned)value;
    do {text[n++]=(char)('0'+v%10);v/=10;} while(v);
    while(n) {char s[2]={text[--n],0};report(s);}
}
static void status(const char *stage)
{
    RECT r={0}; window_rect(root,&r);
    int values[]={r.left,r.top,r.right-r.left,r.bottom-r.top,
                  (int)enters,(int)exits,(int)sizing,(int)moving,(int)clicks,(int)last_edge,snap};
    report("[GUI-WM] "); report(stage);
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++) {report(" ");number(values[i]);}
    report("\n");
}
static IPTR frame_hit(POINT point)
{
    RECT r; if(!root || !window_rect(root,&r)) return 1;
    int x=point.x-r.left,y=point.y-r.top,w=r.right-r.left,h=r.bottom-r.top;
    BOOL left=x<6,right=x>=w-6,top=y<6,bottom=y>=h-6;
    if(top) return left?13:right?14:12;
    if(bottom) return left?16:right?17:15;
    if(left) return 10;
    if(right) return 11;
    return y<34?2:1;
}
static IPTR CALL fixture_proc(HANDLE window,UINT message,UPTR wp,IPTR lp)
{
    if(message==0x83) return 0; /* custom frame owns the entire client rectangle */
    if(message==0x84) {
        IPTR hit=frame_hit((POINT){(short)lp,(short)(lp>>16)});
        return window!=root && hit>1 ? -1 : hit;
    }
    if(message==0x24 && lp) {
        MINMAX *m=(MINMAX *)(UPTR)lp;
        m->min_track=(POINT){320,200}; m->max_track=(POINT){760,540};
        return 0;
    }
    if(message==0x231) enters++;
    if(message==0x232) {exits++; status("changed");}
    if(message==0x214 && lp) {
        RECT *r=(RECT *)(UPTR)lp; sizing++; last_edge=(UINT)wp;
        if(wp<1 || wp>8) failures++;
        if(snap) {r->right=r->left+(r->right-r->left)/16*16; r->bottom=r->top+(r->bottom-r->top)/8*8;}
        return 1;
    }
    if(message==0x216) {moving++; return 1;}
    if(message==0x201) clicks++;
    if(message==5 && window==root) {
        RECT r; client_rect(window,&r);
        if(middle) position(middle,0,0,0,r.right,r.bottom,0x14);
        if(leaf) position(leaf,0,0,0,r.right,r.bottom,0x14);
    }
    if(message==0x0F) {
        PAINT paint={0}; HANDLE dc=begin_paint(window,&paint); RECT r;
        if(dc && client_rect(window,&r)) {
            fill_rect(dc,&r,fill);
            text_color(dc,0x00EDE5DE); background_mode(dc,1);
            const char title[]="Custom frame / nested renderer";
            const char body[]="Drag the title or resize any edge. R resets; Q finishes.";
            text_out(dc,18,12,title,sizeof(title)-1);
            text_out(dc,18,70,body,sizeof(body)-1);
        }
        end_paint(window,&paint); return 0;
    }
    if(message==0x100) {
        if(window!=root) return send_message(root,message,wp,lp);
        if(wp=='R') {position(root,0,240,200,520,330,0x14);status("ready");return 0;}
        if(wp=='K') {status("state");return 0;}
        if(wp=='S') {snap=!snap;status("state");return 0;}
        if(wp=='Q') {
            if(enters!=exits || exits<13 || !sizing || !moving || !clicks) failures++;
            status("final"); report(failures ? "GUI-WM FAIL\n" : "GUI-WM PASS\n");
            destroy_window(root); return 0;
        }
    }
    if(message==0x113) {report("GUI-WM TIMEOUT\n");ExitProcess(2);}
    if(message==2 && window==root) quit(failures?1:0);
    return default_proc(window,message,wp,lp);
}
#define LOAD(module,var,name) do {*(void**)&var=GetProcAddress(module,name); \
    if(!var) {report("GUI-WM missing " name "\n");ExitProcess(2);}} while(0)
void mainCRTStartup(void)
{
    HANDLE user=LoadLibraryA("user32.dll"),gdi=LoadLibraryA("gdi32.dll");
    LOAD(user,register_class,"RegisterClassA"); LOAD(user,create_window,"CreateWindowExA");
    LOAD(user,destroy_window,"DestroyWindow"); LOAD(user,default_proc,"DefWindowProcA");
    LOAD(user,send_message,"SendMessageA"); LOAD(user,window_rect,"GetWindowRect");
    LOAD(user,client_rect,"GetClientRect"); LOAD(user,position,"SetWindowPos");
    LOAD(user,get_message,"GetMessageA"); LOAD(user,translate,"TranslateMessage");
    LOAD(user,dispatch,"DispatchMessageA"); LOAD(user,quit,"PostQuitMessage");
    LOAD(user,begin_paint,"BeginPaint"); LOAD(user,end_paint,"EndPaint");
    LOAD(user,fill_rect,"FillRect"); LOAD(user,set_timer,"SetTimer");
    LOAD(gdi,brush,"CreateSolidBrush"); LOAD(gdi,delete_object,"DeleteObject");
    LOAD(gdi,text_out,"TextOutA"); LOAD(gdi,text_color,"SetTextColor");
    LOAD(gdi,background_mode,"SetBkMode");
    fill=brush(0x00543424);
    WNDCLASS cls={0};cls.proc=fixture_proc;cls.instance=GetModuleHandleA(0);cls.name=class_name;cls.background=fill;
    if(!register_class(&cls)) {report("GUI-WM class failed\n");ExitProcess(2);}
    root=create_window(0,class_name,"Custom frame test",0x90040000,240,200,520,330,0,0,cls.instance,0);
    middle=create_window(0,class_name,"middle",0x50000000,0,0,520,330,root,0,cls.instance,0);
    leaf=create_window(0,class_name,"renderer",0x50000000,0,0,520,330,middle,0,cls.instance,0);
    if(!root || !middle || !leaf) {report("GUI-WM window failed\n");ExitProcess(2);}
    set_timer(root,1,180000,0);
    status("ready");
    MSG message;
    while(get_message(&message,0,0,0)>0) {translate(&message);dispatch(&message);}
    delete_object(fill); ExitProcess(failures?1:0);
}
