/* Native/guest process architecture query, including explicit remote handles. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define SELF ((HANDLE)(UPTR)-1)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
#define SAY(s) do {DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do {char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
typedef struct {DWORD cb;WORD *reserved,*desktop,*title;DWORD x,y,w,h,xc,yc,fill,flags;
    WORD show,bytes;BYTE *data;HANDLE input,output,error;} STARTUP;
typedef struct {HANDLE process,thread;DWORD pid,tid;} PROCESS;
static unsigned failures;
static void wow64_info_check(int ok)
{if(ok)SAY("WOWINFO PASS\n");else{failures++;SAY("WOWINFO FAIL\n");}}
void wow64_info_entry(void)
{
    HANDLE k=LoadLibraryA("kernel32.dll"),n=LoadLibraryA("ntdll.dll");
    int (CALL *query)(HANDLE,DWORD,void *,DWORD,DWORD *)=GetProcAddress(n,"NtQueryInformationProcess");
    HANDLE (CALL *open)(DWORD,int,DWORD)=GetProcAddress(k,"OpenProcess");
    DWORD (CALL *pid)(void)=GetProcAddress(k,"GetCurrentProcessId");
    int (CALL *close)(HANDLE)=GetProcAddress(k,"CloseHandle");
    DWORD (CALL *module_name)(HANDLE,char *,DWORD)=GetProcAddress(k,"GetModuleFileNameA");
    int (CALL *create)(const char *,char *,void *,void *,int,DWORD,void *,const char *,void *,void *)=GetProcAddress(k,"CreateProcessA");
    int (CALL *terminate)(HANDLE,DWORD)=GetProcAddress(k,"TerminateProcess");
    DWORD (CALL *wait)(HANDLE,DWORD)=GetProcAddress(k,"WaitForSingleObject");
    wow64_info_check(query&&open&&pid&&close&&module_name&&create&&terminate&&wait);
    if(!query||!open||!pid||!close||!module_name||!create||!terminate||!wait)ExitProcess(1);
    union {UPTR align;BYTE b[40];} storage;
    struct {DWORD value,guard;} returned;
    DWORD width=sizeof(UPTR),sizes[]={0,1,width-1,width,width+1,width*2};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
        for(unsigned j=0;j<sizeof(storage);j++)storage.b[j]=0xa5;
        returned.value=0x11223344;returned.guard=0x55667788;
        SetLastError(0x1234abcd);
        int status=query(SELF,26,storage.b+8,sizes[i],&returned.value);
        DWORD error=GetLastError();int outside=1,unchanged=1;
        for(unsigned j=0;j<sizeof(storage);j++) {
            if(storage.b[j]!=0xa5)unchanged=0;
            if((j<8||j>=8+width)&&storage.b[j]!=0xa5)outside=0;
        }
        UPTR value=*(UPTR *)(storage.b+8);
        wow64_info_check(outside&&returned.guard==0x55667788&&error==0x1234abcd);
        if(sizes[i]==width)wow64_info_check(status==0&&!!value==(width==4)&&returned.value==width);
        SAY("WOWINFO SIZE ");HEX(sizes[i]);HEX(status);HEX(returned.value);
        HEX(status==0?!!value:unchanged);HEX(outside);HEX(error);SAY("\n");
        if(sizes[i]==width) { SAY("WOWINFO VALUE ");HEX(value);HEX((unsigned long long)value>>32);SAY("\n"); }
    }
    DWORD access[]={0x400,0x1000,0x100000};
    for(unsigned i=0;i<3;i++) {
        HANDLE h=open(access[i],0,pid());wow64_info_check(h!=0);
        UPTR value=(UPTR)0x11223344;DWORD length=0x55667788;
        int status=query(h,26,&value,width,&length);
        if(i<2)wow64_info_check(status==0&&!!value==(width==4)&&length==width);
        SAY("WOWINFO ACCESS ");HEX(access[i]);HEX(status);HEX(length);HEX(status==0?!!value:value==0x11223344);SAY("\n");
        if(h)wow64_info_check(close(h));
    }
    for(unsigned i=0;i<6;i++) {
        for(unsigned j=0;j<sizeof(storage);j++)storage.b[j]=0xa5;
        returned.value=0x11223344;returned.guard=0x55667788;
        HANDLE h=i==0?0:i==1?(HANDLE)(UPTR)0x12345678:SELF;
        void *buffer=i==2?0:storage.b+8+(i==3?1:i==4?4:0);
        int status=query(h,26,buffer,width,i==5?0:&returned.value);
        SAY("WOWINFO EDGE ");HEX(i);HEX(status);HEX(returned.value);HEX(returned.guard);SAY("\n");
        wow64_info_check(returned.guard==0x55667788);
    }
    char path[512];DWORD length=module_name(0,path,sizeof(path));
    wow64_info_check(length>0&&length<sizeof(path)-20);
    DWORD leaf=length;while(leaf&&path[leaf-1]!='\\'&&path[leaf-1]!='/')leaf--;
    for(unsigned bits=32;bits<=64;bits+=32) {
        const char *name=bits==32?"wowinfo32.exe":"wowinfo64.exe";
        unsigned j=0;do{path[leaf+j]=name[j];}while(name[j++]);
        STARTUP startup;PROCESS child;
        for(unsigned j=0;j<sizeof(startup);j++)((BYTE *)&startup)[j]=0;
        for(unsigned j=0;j<sizeof(child);j++)((BYTE *)&child)[j]=0;
        startup.cb=sizeof(startup);
        int ok=create(path,0,0,0,0,4,0,0,&startup,&child);
        wow64_info_check(ok);if(!ok)continue;
        UPTR value=0;DWORD received=0;
        int status=query(child.process,26,&value,width,&received);
        wow64_info_check(status==0&&!!value==(bits==32)&&received==width);
        SAY("WOWINFO CHILD ");HEX(bits);HEX(status);HEX(!!value);HEX(received);SAY("\n");
        wow64_info_check(terminate(child.process,77));
        wow64_info_check(wait(child.process,5000)==0);
        wow64_info_check(close(child.thread));wow64_info_check(close(child.process));
    }
    if(!failures)SAY("WOWINFO DONE\n");ExitProcess(failures?1:0);
}
