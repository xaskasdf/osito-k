typedef unsigned int UINT;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UPTR;
typedef __INTPTR_TYPE__ IPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API void *CALL GetStdHandle(UINT);
API int CALL WriteFile(void *,const void *,UINT,UINT *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API UINT CALL GetLastError(void);
API void CALL SetLastError(UINT);
void *memset(void *p,int c,UPTR n){unsigned char *s=p;for(UPTR i=0;i<n;i++)s[i]=(unsigned char)c;return p;}
static unsigned failures;
#define SAY(s) do{UINT n;WriteFile(GetStdHandle((UINT)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("RAWLIVE PASS " name "\n");else{SAY("RAWLIVE FAIL " name "\n");failures++;}}while(0)
typedef struct{void *hwnd;UINT message;UPTR wp;IPTR lp;UINT time;int x,y;}Message;
typedef struct{WORD page,usage;UINT flags;void *target;}Registration;
typedef struct{UINT type,size;void *device;UPTR code;}Header;
typedef struct{WORD flags,pad,button_flags,button_data;UINT buttons;int x,y;UINT extra;}Mouse;
typedef struct{WORD make,flags,reserved,vk;UINT message,extra;}Keyboard;
typedef void *(CALL *window_fn)(UINT,const char *,const char *,UINT,int,int,int,int,void *,void *,void *,void *);
typedef int (CALL *boolwindow_fn)(void *);
typedef void *(CALL *focus_fn)(void *);
typedef int (CALL *register_fn)(const Registration *,UINT,UINT);
typedef UINT (CALL *registered_fn)(Registration *,UINT *,UINT);
typedef int (CALL *peek_fn)(Message *,void *,UINT,UINT,UINT);
typedef IPTR (CALL *dispatch_fn)(const Message *);
typedef IPTR (CALL *default_fn)(void *,UINT,UPTR,IPTR);
typedef UINT (CALL *data_fn)(void *,UINT,void *,UINT *,UINT);
typedef UINT (CALL *tick_fn)(void);
typedef void (CALL *sleep_fn)(UINT);
typedef int (CALL *metric_fn)(int);
static peek_fn peek;static dispatch_fn dispatch;static default_fn def;static data_fn data;
static tick_fn tick;static sleep_fn pause_ms;
static unsigned raw_keys,raw_mouse,key_edges,button_edges,moved,legacy_keys,legacy_mouse;
static int valid;static void *old_packet;

static void pump(void *expected,UINT vk,WORD scan,int dx,int dy,WORD buttons)
{
    Message msg;
    while(peek(&msg,0,0,0,0)) {
        if(msg.message==0xff) {
            void *packet=(void *)(UPTR)msg.lp;
            UINT needed=0;valid=valid&&data(packet,0x10000003,0,&needed,sizeof(Header))==0;
            union{UPTR align;unsigned char bytes[64];}buffer;memset(&buffer,0xa5,sizeof(buffer));
            UINT capacity=sizeof(buffer);UINT got=data(packet,0x10000003,buffer.bytes,&capacity,sizeof(Header));
            valid=valid&&got==needed&&got>=sizeof(Header)&&got<=sizeof(Header)+24&&capacity==sizeof(buffer);
            Header *header=(Header *)buffer.bytes;
            valid=valid&&header->size==got&&header->device&&header->code==0&&msg.wp==0&&msg.hwnd==expected;
            for(UINT i=got;i<sizeof(buffer);i++)valid=valid&&buffer.bytes[i]==0xa5;
            if(old_packet&&old_packet!=packet){UINT n=0;SetLastError(0);valid=valid&&data(old_packet,0x10000003,0,&n,sizeof(Header))==(UINT)-1&&GetLastError()==6;}
            struct{Header header;volatile UINT guard;}head;memset(&head,0,sizeof(head));head.guard=0x12345678;
            UINT n=sizeof(Header);valid=valid&&data(packet,0x10000005,&head.header,&n,sizeof(Header))==sizeof(Header)&&head.header.size==got&&head.guard==0x12345678;
            if(header->type==1) {
                Keyboard *key=(Keyboard *)(buffer.bytes+sizeof(Header));
                raw_keys++;valid=valid&&got==sizeof(Header)+16&&key->vk==vk&&key->make==scan&&!key->reserved;
                key_edges|=(key->flags&1)?2:1;
                valid=valid&&key->message==((key->flags&1)?0x101u:0x100u);
            } else if(header->type==0) {
                Mouse *mouse=(Mouse *)(buffer.bytes+sizeof(Header));
                raw_mouse++;valid=valid&&got==sizeof(Header)+24&&mouse->flags==0;
                button_edges|=mouse->button_flags&buttons;
                if(mouse->x==dx&&mouse->y==dy)moved=1;
            } else valid=0;
            Message removed;int consumed=peek(&removed,0,0xff,0xff,1);
            valid=valid&&consumed&&removed.lp==msg.lp;
            if(consumed)def(removed.hwnd,removed.message,removed.wp,removed.lp);
            else return;
            old_packet=packet;
        } else {
            if(!peek(&msg,0,0,0,1))continue;
            if(msg.message==0x100||msg.message==0x101)legacy_keys++;
            if(msg.message>=0x200&&msg.message<=0x20e)legacy_mouse++;
            dispatch(&msg);
        }
    }
}

static void reset_counts(void)
{
    raw_keys=raw_mouse=key_edges=button_edges=moved=legacy_keys=legacy_mouse=0;
    valid=1;old_packet=0;
}

void CALL raw_input_live_entry(void)
{
    void *user=LoadLibraryA("user32.dll"),*kernel=LoadLibraryA("kernel32.dll");
    window_fn window=(window_fn)GetProcAddress(user,"CreateWindowExA");
    boolwindow_fn destroy=(boolwindow_fn)GetProcAddress(user,"DestroyWindow");
    boolwindow_fn foreground=(boolwindow_fn)GetProcAddress(user,"SetForegroundWindow");
    focus_fn focus=(focus_fn)GetProcAddress(user,"SetFocus");
    register_fn reg=(register_fn)GetProcAddress(user,"RegisterRawInputDevices");
    registered_fn registered=(registered_fn)GetProcAddress(user,"GetRegisteredRawInputDevices");
    metric_fn metric=(metric_fn)GetProcAddress(user,"GetSystemMetrics");
    peek=(peek_fn)GetProcAddress(user,"PeekMessageA");dispatch=(dispatch_fn)GetProcAddress(user,"DispatchMessageA");
    def=(default_fn)GetProcAddress(user,"DefWindowProcA");data=(data_fn)GetProcAddress(user,"GetRawInputData");
    tick=(tick_fn)GetProcAddress(kernel,"GetTickCount");pause_ms=(sleep_fn)GetProcAddress(kernel,"Sleep");
    CHECK(window&&destroy&&foreground&&focus&&reg&&registered&&metric&&peek&&dispatch&&def&&data&&tick&&pause_ms,"exports");
    if(failures)ExitProcess(1);
    void *hwnd=window(0,"STATIC","Raw input hardware test",0x90000000u,0,0,metric(0),metric(1),0,0,0,0);
    CHECK(hwnd!=0&&foreground(hwnd),"foreground-window");focus(hwnd);
    Message msg;while(peek(&msg,0,0,0,1))dispatch(&msg);
    Registration requests[2]={{1,2,0x30,hwnd},{1,6,0x30,hwnd}};
    CHECK(reg(requests,2,sizeof(Registration)),"register-no-legacy");
    reset_counts();SAY("RAWLIVE READY1\n");
    UINT start=tick();
    while(tick()-start<15000&&(key_edges!=3||button_edges!=3||!moved)) {pump(hwnd,0x41,0x1e,9,-4,3);pause_ms(2);}
    pause_ms(50);pump(hwnd,0x41,0x1e,9,-4,3);
    CHECK(valid,"packet-layout-and-lifetime");
    CHECK(raw_keys>=2&&key_edges==3,"physical-key-make-break");
    CHECK(raw_mouse>=2&&button_edges==3&&moved,"physical-mouse-unscaled");
    CHECK(!legacy_keys&&!legacy_mouse,"no-legacy-delivery");
    requests[0].flags=requests[1].flags=0;
    CHECK(reg(requests,2,sizeof(Registration)),"enable-legacy-alongside-raw");
    reset_counts();SAY("RAWLIVE READY2\n");start=tick();
    while(tick()-start<15000&&(key_edges!=3||button_edges!=12||!moved||legacy_keys<2||legacy_mouse<2)) {pump(hwnd,0x42,0x30,-5,6,12);pause_ms(2);}
    pause_ms(50);pump(hwnd,0x42,0x30,-5,6,12);
    CHECK(valid&&raw_keys>=2&&raw_mouse>=2&&key_edges==3&&button_edges==12&&moved,"raw-after-registration-change");
    CHECK(legacy_keys>=2&&legacy_mouse>=2,"legacy-restored");
    CHECK(destroy(hwnd),"destroy-registered-window");
    Registration stored[2];UINT count=2;
    CHECK(registered(stored,&count,sizeof(Registration))==2&&stored[0].target==hwnd&&stored[1].target==hwnd,"process-registration-persists");
    requests[0].flags=requests[1].flags=1;requests[0].target=requests[1].target=0;
    CHECK(reg(requests,2,sizeof(Registration)),"unregister-devices");
    hwnd=window(0,"STATIC","Raw input cleanup test",0x90000000u,0,0,metric(0),metric(1),0,0,0,0);
    foreground(hwnd);focus(hwnd);while(peek(&msg,0,0,0,1))dispatch(&msg);
    reset_counts();SAY("RAWLIVE READY3\n");start=tick();
    while(tick()-start<15000&&legacy_keys<2){pump(hwnd,0x43,0x2e,0,0,0);pause_ms(2);}
    CHECK(legacy_keys>=2&&!raw_keys&&!raw_mouse,"no-raw-after-unregister");
    CHECK(destroy(hwnd),"cleanup");
    if(!failures)SAY("RAWLIVE DONE\n");
    ExitProcess(failures?1:0);
}
