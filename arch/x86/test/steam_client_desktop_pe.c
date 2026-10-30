/* Native reference for COM/path/escape/key lookup; input injection is VM-only. */
typedef unsigned int DWORD,UINT;
typedef unsigned short WORD,WCHAR;
typedef unsigned char BYTE;
typedef int HRESULT,BOOL;
typedef __UINTPTR_TYPE__ UP;
typedef __INTPTR_TYPE__ SP;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define W(s) ((const WCHAR *)u##s)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API const char *CALL GetCommandLineA(void);
static unsigned checks,failures;
static void say(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(DWORD v)
{char b[9];for(unsigned i=0;i<8;i++)b[i]="0123456789abcdef"[(v>>((7-i)*4))&15];b[8]=0;say(b);}
static void check(int ok,unsigned test,unsigned item)
{checks++;if(!ok){failures++;say("CLIENTDESKTOP FAIL case=");hex(test);say(" item=");hex(item);say(" error=");hex(GetLastError());say("\n");}}
static void *proc(HANDLE dll,const char *name)
{void *p=GetProcAddress(dll,name);if(!p){say("CLIENTDESKTOP MISSING ");say(name);say("\n");ExitProcess(2);}return p;}
#include "steam_client_keyscan_cases.h"
static void keyscan_checks(HANDLE user,int guest)
{
    short (CALL *scan)(char,HANDLE)=proc(user,"VkKeyScanExA");
    HANDLE (CALL *current)(DWORD)=proc(user,"GetKeyboardLayout");
    HANDLE before=current(0),layout=before;
    if(!guest) {
        HANDLE (CALL *load)(const WCHAR *,UINT)=proc(user,"LoadKeyboardLayoutW");
        layout=load(W("00000409"),0x80);check(layout!=0,100,0);
        check(current(0)==before,100,1);
    }
    for(unsigned i=0;i<256;i++) {
        SetLastError(0x1234);short v=scan((char)i,layout);DWORD error=GetLastError();
        check(v==keyscan_us[i],101+i,0);check(error==0x1234,101+i,1);
        SetLastError(0x1234);v=scan((char)i,(HANDLE)(UP)0x12345678);error=GetLastError();
        check(v==-1&&error==87,101+i,2);
        if(guest)check(scan((char)i,0)==keyscan_us[i],101+i,3);
    }
    if(!guest&&layout&&layout!=before) {
        BOOL (CALL *unload)(HANDLE)=proc(user,"UnloadKeyboardLayout");unload(layout);
    }
    check(current(0)==before,400,0);
}
static void path_checks(void)
{
    BOOL (CALL *exists)(const WCHAR *)=proc(LoadLibraryA("shlwapi.dll"),"PathFileExistsW");
    static const struct {const WCHAR *path;BOOL exists;DWORD error;} cases[]={
        {0,0,0x1234},{W(""),0,3},
        {W("file-unicode"),1,0x1234},{W("file-unicode\\ascii.exe"),1,0x1234},
        {W("file-unicode\\caf\u00e9.exe"),1,0x1234},
        {W("file-unicode\\face-\U0001f642.exe"),1,0x1234},
        {W("file-unicode\\dir-\u65e5-\U0001f642"),1,0x1234},
        {W("file-unicode\\absent.exe"),0,2},
        {W("file-unicode\\absent-dir\\file.exe"),0,3},
        {W("file-unicode\\ascii.exe\\file.exe"),0,3}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++) {
        SetLastError(0x1234);BOOL result=exists(cases[i].path);DWORD error=GetLastError();
        check(result==cases[i].exists,500+i,0);check(error==cases[i].error,500+i,1);
    }
    HANDLE kernel=LoadLibraryA("kernel32.dll");
    DWORD (CALL *attributes_a)(const char *)=proc(kernel,"GetFileAttributesA");
    DWORD (CALL *attributes_w)(const WCHAR *)=proc(kernel,"GetFileAttributesW");
    const char *absent[]={"file-unicode\\absent.exe","file-unicode\\absent-dir\\file.exe","file-unicode\\ascii.exe\\file.exe"};
    for(unsigned i=0;i<3;i++) {
        check(attributes_a(absent[i])==0xffffffffU&&GetLastError()==(i?3U:2U),520+i,0);
        check(attributes_w(cases[7+i].path)==0xffffffffU&&GetLastError()==(i?3U:2U),520+i,1);
    }
}
static void escape_checks(void)
{
    HANDLE gdi=LoadLibraryA("gdi32.dll");
    HANDLE (CALL *create)(HANDLE)=proc(gdi,"CreateCompatibleDC");
    BOOL (CALL *destroy)(HANDLE)=proc(gdi,"DeleteDC");
    int (CALL *escape)(HANDLE,int,int,const void *,int,void *)=proc(gdi,"ExtEscape");
    HANDLE dc=create(0);check(dc!=0,600,0);
    const int input_sizes[]={-1,0,1,3,4,5,8};const int output_sizes[]={-1,0,1,8};
    for(unsigned h=0;h<3;h++)for(unsigned e=0;e<2;e++)
    for(unsigned a=0;a<7;a++)for(unsigned b=0;b<2;b++)for(unsigned c=0;c<4;c++) {
        unsigned id=601+((((h*2+e)*7+a)*2+b)*4+c);
        int input[2]={8,0};BYTE output[10];for(unsigned i=0;i<10;i++)output[i]=0xa5;
        int early=e==0&&(input_sizes[a]<4||!b);
        int bad=!early&&h==0&&((b&&input_sizes[a]<0)||output_sizes[c]<0);
        int expected=bad?-1:0;DWORD expected_error=(!early&&h)?6:0x1234;
        SetLastError(0x1234);
        int result=escape(h==0?dc:h==1?0:(HANDLE)(UP)0x123456,e==0?8:0xdead,
                          input_sizes[a],b?input:0,output_sizes[c],output+1);
        DWORD error=GetLastError();check(result==expected,id,0);check(error==expected_error,id,1);
        int same=1;
        for(unsigned i=0;i<10;i++) {
            int cleared=!early&&!bad&&!h&&output_sizes[c]>0&&i>=1&&i<1+(unsigned)output_sizes[c];
            if(output[i]!=(cleared?0:0xa5))same=0;
        }
        check(same,id,2);
    }
    check(destroy(dc),1000,0);
}
typedef struct {DWORD a;WORD b,c;BYTE d[8];} GUID;
typedef struct {void **vtable;DWORD refs;} OBJECT;
static OBJECT proxy,security;
static DWORD queried,blankets,released;
static HRESULT query_result,blanket_result;
static const WCHAR principal[]={0x65e5,0x00e1,0};
static DWORD identity[3]={0x12345678,0x87654321,0};
static HRESULT CALL query(OBJECT *self,const GUID *iid,OBJECT **result)
{
    queried++;check(self==&proxy,1100,0);
    static const BYTE tail[8]={0xc0,0,0,0,0,0,0,0x46};int correct=iid&&iid->a==0x13d&&!iid->b&&!iid->c;
    for(unsigned i=0;correct&&i<8;i++)if(iid->d[i]!=tail[i])correct=0;
    check(correct,1100,1);*result=0;
    if(query_result>=0){security.refs++;*result=&security;}
    return query_result;
}
static DWORD CALL addref(OBJECT *self){return ++self->refs;}
static DWORD CALL release(OBJECT *self)
{check(self==&security,1101,0);released++;return --self->refs;}
static HRESULT CALL blanket(OBJECT *self,OBJECT *original,DWORD authn,DWORD authz,
    const WCHAR *name,DWORD level,DWORD impersonation,void *auth,DWORD caps)
{
    blankets++;check(self==&security&&original==&proxy,1102,0);
    check(authn==10&&authz==0&&name==principal&&level==6&&impersonation==3&&auth==identity&&caps==0x40,1102,1);
    return blanket_result;
}
static void com_checks(void)
{
    HANDLE ole=LoadLibraryA("ole32.dll");
    HRESULT (CALL *set)(void *,DWORD,DWORD,const WCHAR *,DWORD,DWORD,void *,DWORD)=proc(ole,"CoSetProxyBlanket");
    HRESULT (CALL *initialize)(void *,DWORD)=proc(ole,"CoInitializeEx");
    void (CALL *uninitialize)(void)=proc(ole,"CoUninitialize");
    check(initialize(0,0)>=0,1103,0);
    check((DWORD)set(0,10,0,principal,6,3,identity,0x40)==0x80070057U,1103,1);
    void *table[6]={(void *)query,(void *)addref,(void *)release,0,(void *)blanket,0};
    proxy.vtable=security.vtable=table;proxy.refs=security.refs=1;
    const DWORD queries[]={0,0x80004002,0x80070005};
    const DWORD results[]={0,1,0x80070057,0x80004005};
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<4;j++) {
        queried=blankets=released=0;query_result=(HRESULT)queries[i];blanket_result=(HRESULT)results[j];
        DWORD result=(DWORD)set(&proxy,10,0,principal,6,3,identity,0x40);
        check(result==(i?queries[i]:results[j]),1110+i*4+j,0);
        check(queried==1&&blankets==(i?0:1)&&released==(i?0:1),1110+i*4+j,1);
        check(proxy.refs==1&&security.refs==1,1110+i*4+j,2);
    }
    uninitialize();
}
typedef SP (CALL *WNDPROC)(HANDLE,UINT,UP,SP);
typedef struct {UINT style;WNDPROC proc;int cbclass,cbwindow;HANDLE instance,icon,cursor,brush;const char *menu,*name;} WNDCLASS;
typedef struct {HANDLE window;UINT message;UP wparam;SP lparam;DWORD time;int x,y;DWORD private_data;} MSG;
static void input_checks(HANDLE user)
{
    WORD (CALL *register_class)(const WNDCLASS *)=proc(user,"RegisterClassA");
    HANDLE (CALL *create)(DWORD,const char *,const char *,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void *)=proc(user,"CreateWindowExA");
    BOOL (CALL *destroy)(HANDLE)=proc(user,"DestroyWindow");
    HANDLE (CALL *focus)(HANDLE)=proc(user,"SetFocus");
    BOOL (CALL *peek)(MSG *,HANDLE,UINT,UINT,UINT)=proc(user,"PeekMessageA");
    void (CALL *key)(BYTE,BYTE,DWORD,UP)=proc(user,"keybd_event");
    short (CALL *state)(int)=proc(user,"GetAsyncKeyState");
    SP (CALL *extra)(void)=proc(user,"GetMessageExtraInfo");
    WNDCLASS wc={0};wc.proc=proc(user,"DefWindowProcA");wc.name="OsitoKeyEventProbe";
    check(register_class(&wc)!=0,1200,0);
    HANDLE window=create(0,wc.name,"probe",0,0,0,64,64,0,0,0,0);check(window!=0,1200,1);if(!window)return;
    focus(window);MSG message;
    for(unsigned i=0;i<64&&peek(&message,0,0,0,1);i++){}
    UP info=sizeof(UP)==8?(UP)0x1122334455667788ULL:(UP)0x55667788;
    key('A',0,0,info);check((state('A')&0x8000)!=0,1201,0);
    key('A',0,2,info);check((state('A')&0x8000)==0,1201,1);
    unsigned seen=0;
    for(unsigned i=0;i<64&&peek(&message,0,0,0,1);i++) {
        if(message.message!=0x100&&message.message!=0x101)continue;
        check(message.window==window&&message.wparam=='A',1202,0);
        check((DWORD)message.lparam==(seen?0xc01e0001U:0x001e0001U),1202,1);
        check((UP)extra()==info,1202,2);seen++;
    }
    check(seen==2,1202,3);
    key(0x11,0x1d,1,info);check((state(0xa3)&0x8000)!=0,1203,0);
    key(0x11,0x1d,3,info);check((state(0xa3)&0x8000)==0,1203,1);
    while(peek(&message,0,0,0,1)){}
    check(destroy(window),1204,0);
}
void client_desktop_entry(void)
{
    int guest=0;const char *s=GetCommandLineA();
    for(unsigned i=0;s&&s[i];i++)if(s[i]=='-'&&s[i+1]=='-'&&s[i+2]=='g')guest=1;
    HANDLE user=LoadLibraryA("user32.dll");keyscan_checks(user,guest);path_checks();escape_checks();com_checks();
    if(guest)input_checks(user);
    say("CLIENTDESKTOP DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");
    ExitProcess(failures?1:0);
}
