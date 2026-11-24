typedef unsigned int UINT;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UPTR;
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
#define CHECK(ok,name) do{if(ok)SAY("RAWINPUT PASS " name "\n");else{SAY("RAWINPUT FAIL " name "\n");failures++;}}while(0)
typedef struct {void *device;UINT type;} DeviceList;
typedef struct {WORD page,usage;UINT flags;void *target;} Registration;
typedef struct {UINT type,size;void *device;UPTR code;} Header;
typedef UINT (CALL *list_fn)(DeviceList *,UINT *,UINT);
typedef UINT (CALL *info_fn)(void *,UINT,void *,UINT *);
typedef int (CALL *register_fn)(const Registration *,UINT,UINT);
typedef UINT (CALL *registered_fn)(Registration *,UINT *,UINT);
typedef UINT (CALL *data_fn)(void *,UINT,void *,UINT *,UINT);
typedef void *(CALL *window_fn)(UINT,const char *,const char *,UINT,int,int,int,int,void *,void *,void *,void *);
typedef int (CALL *destroy_fn)(void *);

void CALL raw_input_entry(void)
{
    void *user=LoadLibraryA("user32.dll");
    list_fn list=(list_fn)GetProcAddress(user,"GetRawInputDeviceList");
    info_fn info=(info_fn)GetProcAddress(user,"GetRawInputDeviceInfoA");
    info_fn infow=(info_fn)GetProcAddress(user,"GetRawInputDeviceInfoW");
    register_fn reg=(register_fn)GetProcAddress(user,"RegisterRawInputDevices");
    registered_fn registered=(registered_fn)GetProcAddress(user,"GetRegisteredRawInputDevices");
    data_fn data=(data_fn)GetProcAddress(user,"GetRawInputData");
    window_fn window=(window_fn)GetProcAddress(user,"CreateWindowExA");
    destroy_fn destroy=(destroy_fn)GetProcAddress(user,"DestroyWindow");
    CHECK(list&&info&&infow&&reg&&registered&&data&&window&&destroy,"exports");
    if(failures)ExitProcess(1);
    CHECK(sizeof(DeviceList)==(sizeof(void *)==8?16:8)&&sizeof(Registration)==(sizeof(void *)==8?16:12)&&sizeof(Header)==(sizeof(void *)==8?24:16),"guest-layouts");
    UINT count=0;UINT r=list(0,&count,sizeof(DeviceList));
    CHECK(r==0&&count>0&&count<128,"enumerate-count");
    struct{DeviceList items[128];volatile UINT guard;}devices;memset(&devices,0,sizeof(devices));devices.guard=0x12345678;
    UINT capacity=0;SetLastError(0);r=list(devices.items,&capacity,sizeof(DeviceList));
    CHECK(r==(UINT)-1&&GetLastError()==122&&capacity==count&&devices.guard==0x12345678,"short-list");
    capacity=128;r=list(devices.items,&capacity,sizeof(DeviceList));
    CHECK(r==count&&capacity==128&&devices.guard==0x12345678,"enumerate-guard");
    void *mouse=0,*keyboard=0;
    for(UINT i=0;i<r&&i<128;i++){if(devices.items[i].type==0)mouse=devices.items[i].device;if(devices.items[i].type==1)keyboard=devices.items[i].device;}
    CHECK(mouse&&keyboard&&mouse!=keyboard,"system-mouse-keyboard");
    UINT bytes=0;r=info(keyboard,0x2000000b,0,&bytes);
    CHECK(r==0&&bytes==32,"device-info-size");
    struct{UINT value[8];volatile UINT guard;}details;memset(&details,0,sizeof(details));details.value[0]=32;details.guard=0xabcdef01;
    bytes=32;r=info(keyboard,0x2000000b,details.value,&bytes);
    CHECK(r==32&&bytes==32&&details.value[0]==32&&details.value[1]==1&&details.value[5]>0&&details.value[7]>0&&details.guard==0xabcdef01,"keyboard-info-guard");
    details.value[0]=32;bytes=32;r=info(mouse,0x2000000b,details.value,&bytes);
    CHECK(r==32&&details.value[1]==0&&details.value[3]>0&&details.guard==0xabcdef01,"mouse-info-guard");
    bytes=4;SetLastError(0);r=info(mouse,0x2000000b,details.value,&bytes);
    CHECK(r==(UINT)-1&&GetLastError()==122&&bytes==32&&details.guard==0xabcdef01,"short-device-info");
    UINT chars=0;r=info(keyboard,0x20000007,0,&chars);
    CHECK(r==0&&chars>1&&chars<1024,"name-size");
    struct{char text[1024];volatile UINT guard;}name;memset(&name,0,sizeof(name));name.guard=0x87654321;
    chars=1024;r=info(keyboard,0x20000007,name.text,&chars);
    CHECK(r>1&&r<1024&&name.text[r-1]==0&&chars==1024&&name.guard==0x87654321,"name-ansi-guard");
    struct{WORD text[1024];volatile UINT guard;}wide;memset(&wide,0,sizeof(wide));wide.guard=0x11223344;
    chars=1024;UINT wr=infow(keyboard,0x20000007,wide.text,&chars);
    int same=wr==r;for(UINT i=0;i<r&&i<1024;i++)same=same&&wide.text[i]==(unsigned char)name.text[i];
    CHECK(same&&chars==1024&&wide.guard==0x11223344,"name-unicode");
    bytes=123;r=info(mouse,0x20000005,0,&bytes);
    CHECK(r==0&&bytes==0,"non-hid-preparsed-empty");
    capacity=0;SetLastError(0);r=list(0,&capacity,sizeof(DeviceList)+1);
    CHECK(r==(UINT)-1&&GetLastError()==87,"reject-list-layout");
    bytes=sizeof(Header);SetLastError(0);r=data((void *)(UPTR)0x12345678,0x10000003,0,&bytes,sizeof(Header));
    CHECK(r==(UINT)-1&&GetLastError()==6,"reject-packet-handle");
    void *hwnd=window(0,"STATIC","Raw input test",0,0,0,1,1,(void *)(UPTR)-3,0,0,0);
    CHECK(hwnd!=0,"message-only-window");
    Registration requests[3]={{1,2,0,0},{1,6,0x130,hwnd},{1,5,0,hwnd}};
    CHECK(reg(requests,3,sizeof(Registration)),"register-input-and-hid-usage");
    capacity=0;r=registered(0,&capacity,sizeof(Registration));
    CHECK(r==0&&capacity==3,"registered-count");
    struct{Registration items[4];volatile UINT guard;}regs;memset(&regs,0,sizeof(regs));regs.guard=0x24681357;
    capacity=1;SetLastError(0);r=registered(regs.items,&capacity,sizeof(Registration));
    CHECK(r==(UINT)-1&&GetLastError()==122&&capacity==3&&regs.guard==0x24681357,"registered-short");
    capacity=4;r=registered(regs.items,&capacity,sizeof(Registration));
    CHECK(r==3&&capacity==4&&regs.items[0].usage==2&&regs.items[1].usage==5&&regs.items[2].usage==6&&regs.items[2].flags==0x130&&regs.items[2].target==hwnd&&regs.guard==0x24681357,"registered-order-guard");
    Registration bad={1,2,1,hwnd};SetLastError(0);
    CHECK(!reg(&bad,1,sizeof(bad))&&GetLastError()==87,"reject-remove-window");
    bad.flags=0x100;bad.target=0;SetLastError(0);
    CHECK(!reg(&bad,1,sizeof(bad))&&GetLastError()==87,"reject-sink-without-window");
    Registration replace={1,6,0,0};CHECK(reg(&replace,1,sizeof(replace)),"replace-class-registration");
    capacity=4;r=registered(regs.items,&capacity,sizeof(Registration));
    CHECK(r==3&&regs.items[2].usage==6&&regs.items[2].flags==0&&!regs.items[2].target,"replacement-no-duplicate");
    CHECK(destroy(hwnd),"destroy-window");
    for(UINT i=0;i<3;i++){requests[i].flags=1;requests[i].target=0;}
    CHECK(reg(requests,3,sizeof(Registration)),"remove-registrations");
    capacity=0;r=registered(0,&capacity,sizeof(Registration));
    CHECK(r==0&&capacity==0,"registrations-empty");
    if(!failures)SAY("RAWINPUT DONE\n");
    ExitProcess(failures?1:0);
}
