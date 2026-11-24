/* Public ABI oracle: runs unchanged on Windows and Osito-K. */
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef struct { int x, y; } POINT;
typedef struct { int cx, cy; } SIZE;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { unsigned char op, flags, alpha, format; } BLEND;
typedef struct {
    DWORD size; int width, height; unsigned short planes, bits;
    DWORD compression, image_size; int xppm, yppm; DWORD colors, important;
} BITMAPINFO;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API void CALL ExitProcess(DWORD);

static HANDLE (CALL *create)(DWORD,const char*,const char*,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void*);
static BOOL (CALL *destroy)(HANDLE);
static BOOL (CALL *show)(HANDLE,int);
static BOOL (CALL *rect)(HANDLE,RECT*);
static int (CALL *style)(HANDLE,int,int);
static BOOL (CALL *update)(HANDLE,HANDLE,const POINT*,const SIZE*,HANDLE,const POINT*,DWORD,const BLEND*,DWORD);
static BOOL (CALL *set_alpha)(HANDLE,DWORD,unsigned char,DWORD);
static BOOL (CALL *get_alpha)(HANDLE,DWORD*,unsigned char*,DWORD*);
static HANDLE (CALL *create_dc)(HANDLE);
static BOOL (CALL *delete_dc)(HANDLE);
static HANDLE (CALL *dib)(HANDLE,const BITMAPINFO*,DWORD,void**,HANDLE,DWORD);
static HANDLE (CALL *select_object)(HANDLE,HANDLE);
static BOOL (CALL *delete_object)(HANDLE);
static void (CALL *sleep_ms)(DWORD);
static char *(CALL *command_line)(void);
static BOOL (CALL *position_window)(HANDLE,HANDLE,int,int,int,int,DWORD);
static DWORD failures;
static void report(const char *text)
{
    DWORD bytes=0,written;
    while (text[bytes]) bytes++;
    WriteFile(GetStdHandle((DWORD)-11),text,bytes,&written,0);
}
static void check(BOOL condition,const char *message)
{
    if (!condition) { report("LAYERED FAIL: "); report(message); report("\n"); failures++; }
}
#define LOAD(module,var,name) do { *(void**)&var=GetProcAddress(module,name); if (!var) {report("LAYERED MISSING " name "\n"); ExitProcess(2);} } while(0)
static BOOL same_rect(HANDLE window,int x,int y,int width,int height)
{
    RECT r;
    return rect(window,&r) && r.left==x && r.top==y && r.right==x+width && r.bottom==y+height;
}
void mainCRTStartup(void)
{
    HANDLE user=LoadLibraryA("user32.dll"),gdi=LoadLibraryA("gdi32.dll"),kernel=LoadLibraryA("kernel32.dll");
    LOAD(user,create,"CreateWindowExA"); LOAD(user,destroy,"DestroyWindow");
    LOAD(user,show,"ShowWindow"); LOAD(user,rect,"GetWindowRect");
    LOAD(user,style,"SetWindowLongA"); LOAD(user,update,"UpdateLayeredWindow");
    LOAD(user,set_alpha,"SetLayeredWindowAttributes"); LOAD(user,get_alpha,"GetLayeredWindowAttributes");
    LOAD(gdi,create_dc,"CreateCompatibleDC"); LOAD(gdi,delete_dc,"DeleteDC");
    LOAD(gdi,dib,"CreateDIBSection"); LOAD(gdi,select_object,"SelectObject");
    LOAD(gdi,delete_object,"DeleteObject"); LOAD(kernel,sleep_ms,"Sleep");
    LOAD(kernel,command_line,"GetCommandLineA"); LOAD(user,position_window,"SetWindowPos");
    HANDLE w=create(0x80000,"STATIC","Layered ABI",0x80000000u,40,60,64,48,0,0,0,0);
    HANDLE plain=create(0,"STATIC","Opaque ABI",0x80000000u,32,52,80,64,0,0,0,0);
    check(w && plain,"create windows");
    HANDLE dc=create_dc(0);
    BITMAPINFO info={40,64,-48,1,32,0,0,0,0,0,0};
    DWORD *pixels=0;
    HANDLE bmp=dib(dc,&info,0,(void**)&pixels,0,0),old=select_object(dc,bmp);
    check(dc && bmp && pixels,"create source bitmap");
    if (!dc || !bmp || !pixels || !w || !plain) ExitProcess(1);
    for (int y=0;y<48;y++) for (int x=0;x<64;x++)
        pixels[y*64+x]=y<24 ? (x<32 ? 0xffff0000u : 0x80008000u)
                                  : (x<32 ? 0 : 0xff0000ffu);
    POINT pos={40,60},src={0,0}; SIZE size={64,48}; BLEND blend={0,0,255,1};
    check(!update(0,0,&pos,&size,dc,&src,0,&blend,2) && GetLastError()==1400,"invalid window");
    check(!update(plain,0,&pos,&size,dc,&src,0,&blend,2),"requires layered style");
    check(!update(w,0,&pos,&size,dc,&src,0,0,2),"alpha requires blend");
    check(!update(w,0,&pos,&size,dc,&src,0,&blend,8),"NORESIZE is indirect only");
    check(same_rect(w,40,60,64,48),"failed updates preserve geometry");
    check(update(w,0,&pos,&size,dc,&src,0,&blend,2),"premultiplied source");
    DWORD key=0,flags=0; unsigned char alpha=0;
    check(!get_alpha(w,&key,&alpha,&flags),"ULW is not SetLayeredWindowAttributes");
    pos=(POINT){44,66};
    check(update(w,0,&pos,0,0,0,0,0,0) && same_rect(w,44,66,64,48),"position only retains size");
    check(set_alpha(w,0,128,2),"set constant alpha");
    check(get_alpha(w,&key,&alpha,&flags) && alpha==128 && flags==2,"get attributes");
    check(!update(w,0,&pos,&size,dc,&src,0,&blend,2),"attribute mode blocks ULW");
    style(w,-20,0); style(w,-20,0x80000);
    check(update(w,0,&pos,&size,dc,&src,0,&blend,2),"style reset re-enables ULW");
    check(update(w,0,0,0,dc,&src,0,0,4),"opaque without blend");
    check(update(w,0,0,0,dc,&src,0x000000ff,0,1),"color key without blend");
    check(!set_alpha(plain,0,255,2),"attributes require layered style");
    check(update(w,0,&pos,&size,dc,&src,0,&blend,2),"restore source");
    const char *command=command_line();
    BOOL visual=0;
    for (int i=0;command[i];i++)
        if (command[i]=='-' && command[i+1]=='-' && command[i+2]=='v') visual=1;
    if (visual && !failures) {
        HANDLE bgdc=create_dc(0); DWORD *bgpixels=0;
        BITMAPINFO bginfo={40,400,100,1,32,0,0,0,0,0,0};
        HANDLE bgbmp=dib(bgdc,&bginfo,0,(void**)&bgpixels,0,0);
        HANDLE bgold=select_object(bgdc,bgbmp);
        if (!bgbmp || !bgpixels) ExitProcess(1);
        for (int i=0;i<400*100;i++) bgpixels[i]=0xff204060u;
        style(plain,-20,0x80000);
        POINT bgpos={32,52}; SIZE bgsize={400,100};
        check(update(plain,0,&bgpos,&bgsize,bgdc,&src,0,0,4),"background presentation");
        show(plain,5);
        HANDLE constant=create(0x80000,"STATIC","Constant alpha",0x80000000u,140,60,64,48,0,0,0,0);
        HANDLE keyed=create(0x80000,"STATIC","Color key",0x80000000u,240,60,64,48,0,0,0,0);
        pos=(POINT){40,60};
        check(update(w,0,&pos,&size,dc,&src,0,&blend,2),"visual per-pixel");
        blend.alpha=128;
        check(update(constant,0,0,&size,dc,&src,0,&blend,2),"visual combined alpha");
        check(update(keyed,0,0,&size,dc,&src,0x000000ff,0,1),"visual color key");
        show(w,5); show(constant,5); show(keyed,5);
        position_window(w,0,0,0,0,0,0x13);
        select_object(bgdc,bgold); delete_object(bgbmp); delete_dc(bgdc);
        select_object(dc,old); delete_object(bmp); delete_dc(dc);
        report(failures ? "LAYERED VISUAL FAIL\n" : "LAYERED VISUAL READY\n");
        for (;;) sleep_ms(60000);
    }
    select_object(dc,old); delete_object(bmp); delete_dc(dc);
    destroy(plain); destroy(w);
    report(failures ? "LAYERED CONTRACT FAIL\n" : "LAYERED CONTRACT PASS\n");
    ExitProcess(failures ? 1 : 0);
}
