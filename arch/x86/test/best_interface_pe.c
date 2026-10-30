typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL VirtualAlloc(void *,UP,DWORD,DWORD);
API int CALL VirtualProtect(void *,UP,DWORD,DWORD *);
API int CALL VirtualFree(void *,UP,DWORD);
static DWORD checks,failures;
static DWORD (CALL *best)(DWORD,DWORD *);
static DWORD (CALL *extended)(const void *,DWORD *);
static void say(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void hex(DWORD v)
{ char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[v&15];v>>=4;}s[8]=0;say(s); }
static void check(int yes,const char *label,DWORD got,DWORD want)
{
    checks++;
    if(!yes){failures++;say("BESTIF FAIL ");say(label);say(" got=");hex(got);say(" want=");hex(want);say("\n");}
}
static void query(DWORD address,DWORD status,DWORD index)
{
    if(extended){
        struct {WORD family,port;DWORD address;DWORD pad[2];} socket_address={2,0,address,{0,0}};
        index=0x55667788;status=extended(&socket_address,&index);
    }
    DWORD guarded[3]={0x11223344,0x55667788,0x99aabbcc};
    DWORD result=best(address,&guarded[1]);
    check(result==status,"status",result,status);
    check(guarded[1]==(status?0x55667788:index),"index",guarded[1],status?0x55667788:index);
    check(guarded[0]==0x11223344&&guarded[2]==0x99aabbcc,"guard",guarded[2],0x99aabbcc);
}
void best_interface_entry(void)
{
    void *module=LoadLibraryA("iphlpapi.dll");best=GetProcAddress(module,"GetBestInterface");
    extended=GetProcAddress(module,"GetBestInterfaceEx");
    DWORD (CALL *table)(void *,DWORD *,int)=GetProcAddress(module,"GetIpAddrTable");
    check(best&&table,"exports",0,0);if(failures)ExitProcess(2);
    DWORD hr=best(0x0100007f,0);check(hr==87,"NULL output",hr,87);
    hr=best(0x0100007f,(DWORD *)(UP)1);check(hr==998,"invalid output",hr,998);
    static DWORD buffer[2048];DWORD bytes=sizeof(buffer);
    hr=table(buffer,&bytes,0);check(hr==0,"address table",hr,0);if(hr)ExitProcess(2);
    DWORD loopback=0;
    for(DWORD i=0;i<buffer[0];i++){
        DWORD *row=buffer+1+i*6;
        if((row[0]&255)==127)loopback=row[1];
        if(row[0])query(row[0],0,row[1]);
    }
    check(loopback!=0,"loopback exists",loopback,2);
    for(DWORD i=0;i<256;i++)query(0x0100007f|(i<<8),0,loopback);
    for(DWORD i=224;i<=239;i++)query(0x03020100|i,1232,0);
    query(0x01010101,0,1);query(0xffffffff,0,1);query(0,0,1);
    query(0x010000f0,1231,0);query(0x010000ff,1231,0);query(0x03020100,1231,0);
    unsigned char *pages=VirtualAlloc(0,8192,0x3000,4);check(pages!=0,"page allocation",0,0);
    if(pages){
        DWORD *cross=(DWORD *)(pages+4094),old=0;*cross=0x12345678;
        check(VirtualProtect(pages+4096,4096,2,&old)!=0,"read-only protection",0,0);
        hr=best(0x0100007f,cross);check(hr==998,"cross-page output",hr,998);
        check(*cross==0x12345678,"failed output unchanged",*cross,0x12345678);
        check(VirtualFree(pages,0,0x8000)!=0,"page release",0,0);
    }
    say("BESTIF DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");
    ExitProcess(failures?1:0);
}
