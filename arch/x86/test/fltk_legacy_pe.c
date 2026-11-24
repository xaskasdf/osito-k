typedef unsigned int UINT;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UPTR;
typedef __INTPTR_TYPE__ IPTR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(UINT);
API int CALL WriteFile(HANDLE,const void *,UINT,UINT *,HANDLE);
API HANDLE CALL LoadLibraryA(const char *);
API HANDLE CALL GetProcAddress(HANDLE,const char *);
static UINT failures,callbacks,callback_ok;
static IPTR cookie;
#define SAY(s) do {UINT n;WriteFile(GetStdHandle((UINT)-11),s,sizeof(s)-1,&n,0);} while(0)
#define CHECK(ok,name) do {if(ok)SAY("FLTKLEG PASS " name "\n");else{SAY("FLTKLEG FAIL " name "\n");failures++;}} while(0)
void *memset(void *p,int c,UPTR n) {unsigned char *b=p;while(n--)*b++=(unsigned char)c;return p;}
void *memcpy(void *p,const void *q,UPTR n) {unsigned char *b=p;const unsigned char *a=q;while(n--)*b++=*a++;return p;}
typedef struct {int height,width,escape,orientation,weight;unsigned char flags[8];WORD face[32];} Font;
static int CALL font_callback(const Font *font,const int *metrics,UINT type,IPTR data)
{
    callbacks++;
    callback_ok=font&&metrics&&font->face[0]&&font->height&&metrics[0]>0&&type&&data==cookie;
    return 0;
}
#define IMPORT(dll,ret,name,args) typedef ret (CALL *name##_fn) args;name##_fn name=(name##_fn)GetProcAddress(dll,#name);if(!name){SAY("FLTKLEG MISSING " #name "\n");failures++;}
void fltk_legacy_entry(void)
{
    HANDLE kernel=LoadLibraryA("kernel32.dll"),gdi=LoadLibraryA("gdi32.dll"),advapi=LoadLibraryA("advapi32.dll"),user=LoadLibraryA("user32.dll"),common=LoadLibraryA("comctl32.dll");
    IMPORT(kernel,int,CompareFileTime,(const void *,const void *));
    IMPORT(kernel,UINT,GetCurrentProcessId,(void));
    IMPORT(gdi,HANDLE,CreateCompatibleDC,(HANDLE));
    IMPORT(gdi,int,DeleteDC,(HANDLE));
    IMPORT(gdi,int,EnumFontFamiliesW,(HANDLE,const WORD *,HANDLE,IPTR));
    IMPORT(gdi,int,StartDocA,(HANDLE,const void *));
    IMPORT(gdi,int,StartPage,(HANDLE));
    IMPORT(gdi,int,EndPage,(HANDLE));
    IMPORT(gdi,int,EndDoc,(HANDLE));
    IMPORT(gdi,int,GetDCOrgEx,(HANDLE,void *));
    IMPORT(advapi,int,RegCreateKeyExA,(HANDLE,const char *,UINT,char *,UINT,UINT,void *,HANDLE *,UINT *));
    IMPORT(advapi,int,RegOpenKeyW,(HANDLE,const WORD *,HANDLE *));
    IMPORT(advapi,int,RegSetValueExA,(HANDLE,const char *,UINT,UINT,const void *,UINT));
    IMPORT(advapi,int,RegQueryValueExA,(HANDLE,const char *,UINT *,UINT *,void *,UINT *));
    IMPORT(advapi,int,RegCloseKey,(HANDLE));
    IMPORT(advapi,int,RegDeleteKeyA,(HANDLE,const char *));
    IMPORT(common,int,_TrackMouseEvent,(void *));
    IMPORT(user,HANDLE,CreateWindowExA,(UINT,const char *,const char *,UINT,int,int,int,int,HANDLE,HANDLE,HANDLE,void *));
    IMPORT(user,int,DestroyWindow,(HANDLE));
    IMPORT(user,HANDLE,GetDC,(HANDLE));
    IMPORT(user,HANDLE,GetWindowDC,(HANDLE));
    IMPORT(user,int,ReleaseDC,(HANDLE,HANDLE));
    IMPORT(user,int,ClientToScreen,(HANDLE,void *));
    IMPORT(user,int,GetWindowRect,(HANDLE,void *));
    CHECK(!failures,"exports");
    if(failures)ExitProcess(1);
    UINT a[2]={0,0},b[2]={0,0};
    CHECK(CompareFileTime(a,b)==0,"filetime-equality");
    a[0]=0xFFFFFFFF;b[1]=1;
    CHECK(CompareFileTime(a,b)==-1&&CompareFileTime(b,a)==1,"filetime-carry-between-words");
    a[1]=0xFFFFFFFF;b[0]=0xFFFFFFFF;b[1]=0x7FFFFFFF;
    CHECK(CompareFileTime(a,b)==1&&CompareFileTime(b,a)==-1,"filetime-unsigned-high-bit");
    unsigned char unaligned[17]={0};unaligned[1]=1;unaligned[9]=2;
    CHECK(CompareFileTime(unaligned+1,unaligned+9)==-1,"filetime-unaligned-input");
    HANDLE dc=CreateCompatibleDC(0);cookie=(IPTR)(UPTR)0x1122334455667788ULL;
    int result=EnumFontFamiliesW(dc,0,(HANDLE)font_callback,cookie);
    CHECK(dc&&result==0&&callbacks==1&&callback_ok,"font-callback-width-and-stop");
    struct {int size;const char *name,*output,*type;UINT flags;} doc={sizeof(doc),"Osito display DC contract",0,0,0};
    CHECK(StartDocA(dc,&doc)==-1&&StartDocA(0,&doc)==-1,"display-dc-rejects-print-job");
    CHECK(StartPage(dc)==-1&&EndPage(dc)==-1&&EndDoc(dc)==-1,"display-dc-rejects-print-pages");
    CHECK(StartPage(0)==-1&&EndPage(0)==-1&&EndDoc(0)==-1,"invalid-print-dc");
    CHECK(DeleteDC(dc),"memory-dc-release");
    /* A per-process temporary key is deleted after readback; no existing
     * application setting is modified by the native reference run. */
    char path[80]="Software\\OsitoFltkProbe-";UINT at=24,pid=GetCurrentProcessId();
    while(at&&path[at-1]==0)at--;
    for(int shift=28;shift>=0;shift-=4)path[at++]="0123456789abcdef"[(pid>>shift)&15];path[at]=0;
    WORD wide[80]={0};for(UINT k=0;k<at;k++)wide[k]=(WORD)(unsigned char)path[k];
    HANDLE root=(HANDLE)(IPTR)(int)0x80000001U,key=0;UINT disposition=0,value=0x1234CAFE;
    result=RegCreateKeyExA(root,path,0,0,0,0xF003F,0,&key,&disposition);
    CHECK(result==0&&key&&RegSetValueExA(key,"marker",0,4,&value,4)==0,"registry-fixture");
    struct {UPTR before;HANDLE key;UPTR after;} opened={0x11223344,0,0x55667788};
    CHECK(RegOpenKeyW(root,wide,&opened.key)==0&&opened.key&&opened.before==0x11223344&&opened.after==0x55667788,"legacy-wide-registry-handle-width");
    UINT read=0,bytes=4,type=0;
    CHECK(RegQueryValueExA(opened.key,"marker",0,&type,&read,&bytes)==0&&read==value&&bytes==4&&type==4,"legacy-registry-real-readback");
    RegCloseKey(opened.key);RegCloseKey(key);
    CHECK(RegDeleteKeyA(root,path)==0&&RegOpenKeyW(root,wide,&opened.key)==2,"legacy-registry-missing-and-cleanup");
    HANDLE window=CreateWindowExA(0,"STATIC","Osito tracking probe",0x80000000U,31,47,64,48,0,0,0,0);
    CHECK(window!=0,"tracking-window");
    struct Track {UINT size,flags;HANDLE window;UINT time;};
    struct {UPTR before;struct Track event;UPTR after;} track={0x12345678,{sizeof(struct Track),0x80000002U,window,0},0x87654321};
    CHECK(_TrackMouseEvent(&track.event)&&track.before==0x12345678&&track.after==0x87654321,"common-controls-cancel-tracking");
    track.event.flags=0x40000000;
    CHECK(_TrackMouseEvent(&track.event)&&track.event.flags==0&&track.event.window==0&&track.event.time==0&&track.before==0x12345678&&track.after==0x87654321,"common-controls-query-width");
    track.event.flags=0x40000000;track.event.window=0;track.event.time=123;
    CHECK(_TrackMouseEvent(&track.event)&&!track.event.flags&&!track.event.window&&!track.event.time,"common-controls-query-without-window");
    track.event.size=0;
    CHECK(!_TrackMouseEvent(&track.event),"common-controls-invalid-size");
    HANDLE clientdc=GetDC(window),windowdc=GetWindowDC(window);
    int client[2]={0,0},origin[2]={0,0},rect[4]={0,0,0,0};
    CHECK(clientdc&&ClientToScreen(window,client)&&GetDCOrgEx(clientdc,origin)&&origin[0]==client[0]&&origin[1]==client[1],"client-dc-screen-origin");
    CHECK(windowdc&&GetWindowRect(window,rect)&&GetDCOrgEx(windowdc,origin)&&origin[0]==rect[0]&&origin[1]==rect[1],"whole-window-dc-screen-origin");
    ReleaseDC(window,clientdc);ReleaseDC(window,windowdc);
    CHECK(DestroyWindow(window),"window-cleanup");
    if(!failures)SAY("FLTKLEG DONE\n");
    ExitProcess(failures?1:0);
}
