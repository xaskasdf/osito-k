#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static unsigned checks,failures;
static DWORD (WINAPI *data_fn)(HRGN,DWORD,RGNDATA *);
static int (WINAPI *random_fn)(HDC,HRGN,int);
static HRGN (WINAPI *rect_fn)(int,int,int,int);
static HRGN (WINAPI *polygon_fn)(const POINT *,int,int);
static int (WINAPI *combine_fn)(HRGN,HRGN,HRGN,int);
static int (WINAPI *box_fn)(HRGN,RECT *);
static BOOL (WINAPI *point_fn)(HRGN,int,int);
static BOOL (WINAPI *delete_fn)(HGDIOBJ);
static HDC (WINAPI *memory_fn)(HDC);
static HBITMAP (WINAPI *bitmap_fn)(HDC,int,int);
static HGDIOBJ (WINAPI *select_fn)(HDC,HGDIOBJ);
static BOOL (WINAPI *delete_dc)(HDC);
static int (WINAPI *clip_fn)(HDC,HRGN);
static BOOL (WINAPI *viewport_fn)(HDC,int,int,POINT *);
static HDC (WINAPI *get_dc)(HWND,HRGN,DWORD);
static int (WINAPI *release_dc)(HWND,HDC);
static HWND (WINAPI *create_window)(DWORD,LPCSTR,LPCSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID);
static BOOL (WINAPI *destroy_window)(HWND);
static BOOL (WINAPI *move_window)(HWND,int,int,int,int,BOOL);
static BOOL (WINAPI *show_window)(HWND,int);
static BOOL (WINAPI *invalidate)(HWND,const RECT *,BOOL);
static BOOL (WINAPI *validate)(HWND,const RECT *);
static HDC (WINAPI *begin_paint)(HWND,PAINTSTRUCT *);
static BOOL (WINAPI *end_paint)(HWND,const PAINTSTRUCT *);
static unsigned char buffer[65536+32];
static char row[300];static unsigned used;
void *memset(void *p,int v,size_t n){unsigned char *d=p;while(n--)*d++=(unsigned char)v;return p;}
void *memcpy(void *p,const void *s,size_t n){unsigned char *d=p;const unsigned char *a=s;while(n--)*d++=*a++;return p;}
static void number(DWORD n){for(int i=28;i>=0;i-=4)row[used++]="0123456789abcdef"[(n>>i)&15];row[used++]=' ';}
static void emit(const char *s){DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&w,0);row[used-1]='\n';WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),row,used,&w,0);used=0;}
#define CHECK(x) do{checks++;if(!(x)){failures++;number(__LINE__);emit("REGION FAIL ");}}while(0)
static void *resolve(HMODULE m,const char *s){void *p=GetProcAddress(m,s);if(!p){number(0);emit(s);ExitProcess(2);}return p;}
static void inspect(HRGN r,unsigned tag,BOOL dense)
{
    DWORD bytes=data_fn(r,0,NULL);CHECK(bytes>=32 && bytes<=65536);
    if(bytes<32 || bytes>65536)return;
    CHECK(data_fn(r,123,NULL)==bytes);
    memset(buffer,0xa5,sizeof(buffer));SetLastError(0x1234);
    CHECK(data_fn(r,bytes-1,(RGNDATA *)buffer)==0);
    number(tag);number(GetLastError());emit("REGION ERROR ");
    BOOL unchanged=TRUE;for(unsigned i=0;i<sizeof(buffer);i++)if(buffer[i]!=0xa5)unchanged=FALSE;CHECK(unchanged);
    CHECK(data_fn(r,bytes+16,(RGNDATA *)buffer)==bytes);
    RGNDATA *d=(void *)buffer;RECT bound;box_fn(r,&bound);
    CHECK(d->rdh.dwSize==32 && d->rdh.iType==RDH_RECTANGLES && bytes==32+d->rdh.nCount*16);
    CHECK(d->rdh.nRgnSize==d->rdh.nCount*16);
    CHECK(d->rdh.rcBound.left==bound.left && d->rdh.rcBound.top==bound.top &&
          d->rdh.rcBound.right==bound.right && d->rdh.rcBound.bottom==bound.bottom);
    unchanged=TRUE;for(unsigned i=bytes;i<sizeof(buffer);i++)if(buffer[i]!=0xa5)unchanged=FALSE;CHECK(unchanged);
    RECT *rects=(void *)d->Buffer;BOOL valid=TRUE;
    for(unsigned i=0;i<d->rdh.nCount;i++) {
        RECT a=rects[i];if(a.left>=a.right || a.top>=a.bottom)valid=FALSE;
        if(i && (a.top<rects[i-1].top || (a.top==rects[i-1].top && a.left<rects[i-1].left)))valid=FALSE;
        for(unsigned j=0;j<i;j++){RECT b=rects[j];if(a.left<b.right&&b.left<a.right&&a.top<b.bottom&&b.top<a.bottom)valid=FALSE;}
    }
    CHECK(valid);
    if(dense)for(int y=-4;y<25;y++)for(int x=-4;x<35;x++) {
        BOOL in=FALSE;for(unsigned i=0;i<d->rdh.nCount;i++){RECT a=rects[i];if(x>=a.left&&x<a.right&&y>=a.top&&y<a.bottom)in=TRUE;}
        CHECK(!!point_fn(r,x,y)==in);
    }
    DWORD hash=2166136261u;for(unsigned i=0;i<bytes;i++)hash=(hash^buffer[i])*16777619u;
    number(tag);number(bytes);number(d->rdh.nCount);number(hash);emit("REGION DATA ");
}
static void region_cases(void)
{
    HRGN r=rect_fn(0,0,0,0),part=rect_fn(2,3,9,11);CHECK(r&&part);
    inspect(r,0,TRUE);combine_fn(r,part,0,RGN_COPY);inspect(r,1,TRUE);
    HRGN cut=rect_fn(6,7,14,18);combine_fn(r,r,cut,RGN_OR);inspect(r,2,TRUE);
    combine_fn(r,r,part,RGN_DIFF);inspect(r,3,TRUE);delete_fn(r);
    r=rect_fn(-2,-3,20,19);combine_fn(r,r,cut,RGN_XOR);inspect(r,4,TRUE);delete_fn(r);
    POINT triangle[3]={{0,0},{19,3},{4,19}};r=polygon_fn(triangle,3,ALTERNATE);inspect(r,5,TRUE);delete_fn(r);
    r=rect_fn(0,0,0,0);
    for(int i=159;i>=0;i--) {HRGN p=rect_fn((i%20)*3,(i/20)*3,(i%20)*3+1,(i/20)*3+1);CHECK(p && combine_fn(r,r,p,RGN_OR)!=ERROR);delete_fn(p);}
    inspect(r,6,FALSE);delete_fn(r);delete_fn(cut);delete_fn(part);
    memset(buffer,0xa5,sizeof(buffer));SetLastError(0x1234);CHECK(!data_fn(NULL,sizeof(buffer),(RGNDATA *)buffer));
    number(7);number(GetLastError());emit("REGION ERROR ");
    CHECK(buffer[0]==0xa5 && buffer[65535]==0xa5);
}
static void random_row(HDC dc,HRGN r,unsigned tag)
{
    SetLastError(0x1234);int result=random_fn(dc,r,SYSRGN);DWORD error=GetLastError();RECT b;int kind=box_fn(r,&b);
    number(tag);number(result);number(error);number(kind);number(b.left);number(b.top);number(b.right);number(b.bottom);emit("REGION SYS ");
}
static void memory_cases(void)
{
    HDC dc=memory_fn(NULL);HRGN r=rect_fn(100,200,300,400),clip=rect_fn(2,2,4,4);
    CHECK(dc&&r&&clip);random_row(dc,r,0);
    HBITMAP bitmap=bitmap_fn(dc,24,16);HGDIOBJ old=select_fn(dc,bitmap);CHECK(bitmap&&old);
    random_row(dc,r,1);clip_fn(dc,clip);viewport_fn(dc,7,9,NULL);random_row(dc,r,2);
    random_row(NULL,r,3);select_fn(dc,old);delete_fn(bitmap);delete_fn(r);delete_fn(clip);CHECK(delete_dc(dc));
}
void region_query_entry(void)
{
    HMODULE gdi=LoadLibraryA("gdi32.dll"),user=LoadLibraryA("user32.dll");
#define G(field,name) field=(void *)resolve(gdi,name)
#define U(field,name) field=(void *)resolve(user,name)
    G(data_fn,"GetRegionData");G(random_fn,"GetRandomRgn");G(rect_fn,"CreateRectRgn");G(polygon_fn,"CreatePolygonRgn");
    G(combine_fn,"CombineRgn");G(box_fn,"GetRgnBox");G(point_fn,"PtInRegion");G(delete_fn,"DeleteObject");
    G(memory_fn,"CreateCompatibleDC");G(bitmap_fn,"CreateCompatibleBitmap");G(select_fn,"SelectObject");G(delete_dc,"DeleteDC");
    G(clip_fn,"SelectClipRgn");G(viewport_fn,"SetViewportOrgEx");
    region_cases();memory_cases();
    U(get_dc,"GetDCEx");U(release_dc,"ReleaseDC");U(create_window,"CreateWindowExA");U(destroy_window,"DestroyWindow");
    U(move_window,"MoveWindow");U(show_window,"ShowWindow");U(invalidate,"InvalidateRect");U(validate,"ValidateRect");U(begin_paint,"BeginPaint");U(end_paint,"EndPaint");
    /* Isolate fixtures on Windows. Osito-K currently has a single desktop;
     * its short-lived probe window is closed before returning to the shell. */
    HDESK(WINAPI *create_desktop)(LPCSTR,LPCSTR,DEVMODEA *,DWORD,ACCESS_MASK,LPSECURITY_ATTRIBUTES)=(void *)GetProcAddress(user,"CreateDesktopA");
    BOOL(WINAPI *set_desktop)(HDESK)=NULL;
    BOOL(WINAPI *close_desktop)(HDESK)=NULL;
    HDESK old=NULL,desk=NULL;
    if(create_desktop) {
        HDESK(WINAPI *get_desktop)(DWORD)=(void *)resolve(user,"GetThreadDesktop");
        set_desktop=(void *)resolve(user,"SetThreadDesktop");close_desktop=(void *)resolve(user,"CloseDesktop");
        HMODULE k=LoadLibraryA("kernel32.dll");DWORD(WINAPI *thread_id)(void)=(void *)resolve(k,"GetCurrentThreadId");
        old=get_desktop(thread_id());desk=create_desktop("OsitoRegionQuery",NULL,NULL,0,0xF01FF,NULL);
        CHECK(desk && set_desktop(desk));
    }
    ATOM(WINAPI *register_class)(const WNDCLASSA *)=(void *)resolve(user,"RegisterClassA");
    BOOL(WINAPI *unregister_class)(LPCSTR,HINSTANCE)=(void *)resolve(user,"UnregisterClassA");
    WNDCLASSA cls={0};cls.lpfnWndProc=(void *)resolve(user,"DefWindowProcA");cls.lpszClassName="OsitoRegionQuery";CHECK(register_class(&cls));
    HWND w=create_window(0,cls.lpszClassName,"Region query",WS_POPUP|WS_VISIBLE,80,100,48,40,0,0,0,0);CHECK(w);
    HRGN r=rect_fn(900,900,901,901);HDC dc=get_dc(w,NULL,DCX_CACHE);CHECK(r&&dc);random_row(dc,r,10);
    HRGN clip=rect_fn(8,9,12,15);clip_fn(dc,clip);delete_fn(clip);viewport_fn(dc,1000,2000,NULL);random_row(dc,r,11);
    move_window(w,180,200,60,44,FALSE);random_row(dc,r,12);
    show_window(w,SW_HIDE);random_row(dc,r,13);show_window(w,SW_SHOW);release_dc(w,dc);
    validate(w,NULL);RECT update={3,4,23,19};invalidate(w,&update,FALSE);PAINTSTRUCT ps;HDC paint=begin_paint(w,&ps);CHECK(paint);random_row(paint,r,14);CHECK(end_paint(w,&ps));
    clip=rect_fn(181,201,185,206);dc=get_dc(w,clip,DCX_CACHE|DCX_INTERSECTRGN);CHECK(dc);random_row(dc,r,15);release_dc(w,dc);
    CHECK(delete_fn(r)&&destroy_window(w)&&unregister_class(cls.lpszClassName,0));
    if(desk)CHECK(set_desktop(old)&&close_desktop(desk));
    number(checks);number(failures);emit("REGION RESULT ");ExitProcess(failures?1:0);
}
