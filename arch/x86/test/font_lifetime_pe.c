/* Logical-font capacity and cleanup on process exit through public Win32 APIs. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API void CALL ExitProcess(DWORD);
typedef struct {
    int height,width,escape,orientation,weight;
    BYTE italic,underline,strike,charset,out_precision,clip_precision,quality,pitch;
    WORD face[32];
} FONT;
typedef struct {
    DWORD cb;char *reserved,*desktop,*title;
    DWORD x,y,width,height,columns,rows,fill,flags;
    WORD show,reserved_bytes;BYTE *reserved_data;
    HANDLE input,output,error;
} STARTUP;
typedef struct {HANDLE process,thread;DWORD pid,tid;} PROCESS;
_Static_assert(sizeof(FONT)==92,"LOGFONTW layout");
static HANDLE fonts[512];
static char executable[1024],command[2200];
static STARTUP startup;
#define SAY(text) WriteFile(output,text,sizeof(text)-1,&written,0)
#define HEX(value) do {DWORD n=(DWORD)(value);char text[8];for(int i=7;i>=0;i--){text[i]="0123456789abcdef"[n&15];n>>=4;}WriteFile(output,text,8,&written,0);} while(0)
#define CHECK(condition) do {checks++;if(!(condition)){failures++;SAY("FONTLIFE FAIL line=");HEX(__LINE__);SAY("\n");}} while(0)
void font_lifetime_entry(void)
{
    HANDLE output=GetStdHandle((DWORD)-11);DWORD written=0,checks=0,failures=0;
    HANDLE kernel=LoadLibraryA("kernel32.dll"),gdi=LoadLibraryA("gdi32.dll");
    CHECK(kernel && gdi);
    HANDLE (CALL *create_font)(const FONT *)=GetProcAddress(gdi,"CreateFontIndirectW");
    int (CALL *get_object)(HANDLE,int,void *)=GetProcAddress(gdi,"GetObjectW");
    int (CALL *delete_object)(HANDLE)=GetProcAddress(gdi,"DeleteObject");
    char *(CALL *command_line)(void)=GetProcAddress(kernel,"GetCommandLineA");
    DWORD (CALL *module_name)(HANDLE,char *,DWORD)=GetProcAddress(kernel,"GetModuleFileNameA");
    int (CALL *create_process)(const char *,char *,void *,void *,int,DWORD,void *,const char *,STARTUP *,PROCESS *)=GetProcAddress(kernel,"CreateProcessA");
    DWORD (CALL *wait_one)(HANDLE,DWORD)=GetProcAddress(kernel,"WaitForSingleObject");
    int (CALL *exit_code)(HANDLE,DWORD *)=GetProcAddress(kernel,"GetExitCodeProcess");
    int (CALL *close_handle)(HANDLE)=GetProcAddress(kernel,"CloseHandle");
    CHECK(create_font && get_object && delete_object && command_line && module_name && create_process && wait_one && exit_code && close_handle);
    if(failures)ExitProcess(2);
    int child=0,capacity=0,handles=0;DWORD foreign=0;
    for(const char *p=command_line();*p;p++)if(p[0]=='-' && p[1]=='-'){
        if(p[2]=='c' && p[3]=='h')child=1;
        if(p[2]=='c' && p[3]=='a')capacity=1;
        if(p[2]=='h' && p[3]=='a')handles=1;
        if(p[2]=='f' && p[3]=='o'){
            const char *digits=p+10;
            for(int i=0;i<8 && digits[i];i++){
                unsigned n=(unsigned)(unsigned char)digits[i];
                if(n>='0' && n<='9')n-='0';
                else if(n>='a' && n<='f')n=n-'a'+10;
                else break;
                foreign=(foreign<<4)|n;
            }
        }
    }
    FONT request={0},observed={0};
    request.height=-19;request.weight=400;
    request.face[0]='A';request.face[1]='r';request.face[2]='i';request.face[3]='a';request.face[4]='l';
    if(foreign){
        HANDLE borrowed=(HANDLE)(__UINTPTR_TYPE__)foreign;
        int queried=get_object(borrowed,sizeof(observed),&observed);
        int deleted=delete_object(borrowed);
        /* Native Windows allows this metadata query, but refuses destruction
         * by the other process. Selection is exercised separately in GDI. */
        CHECK(queried==sizeof(observed) && observed.height==-27);
        CHECK(!deleted);
    }else if(handles){
        HANDLE stale=create_font(&request);CHECK(stale!=0);
        CHECK(stale && delete_object(stale));
        request.height=-27;
        HANDLE sentinel=create_font(&request);CHECK(sentinel!=0);
        request.height=-44;
        CHECK(get_object(stale,sizeof(observed),&observed)==0);
        CHECK(!delete_object(stale));
        CHECK(get_object(sentinel,sizeof(observed),&observed)==sizeof(observed) && observed.height==-27);
        DWORD length=module_name(0,executable,sizeof(executable));CHECK(length>0 && length<sizeof(executable));
        if(sentinel && length>0 && length<sizeof(executable)){
            command[0]='"';for(DWORD i=0;i<length;i++)command[i+1]=executable[i];
            const char suffix[]="\" --foreign=";
            for(DWORD i=0;i<sizeof(suffix)-1;i++)command[length+1+i]=suffix[i];
            DWORD handle=(DWORD)(__UINTPTR_TYPE__)sentinel;
            for(int i=7;i>=0;i--){command[length+sizeof(suffix)+i]="0123456789abcdef"[handle&15];handle>>=4;}
            command[length+sizeof(suffix)+8]=0;
            startup.cb=sizeof(startup);
            PROCESS process={0};DWORD code=259;
            int started=create_process(executable,command,0,0,0,0x08000000,0,0,&startup,&process);CHECK(started);
            if(started){
                CHECK(wait_one(process.process,30000)==0);
                CHECK(exit_code(process.process,&code));
                SAY("FONTLIFE foreign-result=");HEX(code);SAY("\n");
                CHECK(exit_code(process.process,&code) && code==0);
                CHECK(close_handle(process.thread));CHECK(close_handle(process.process));
            }
        }
        CHECK(get_object(sentinel,sizeof(observed),&observed)==sizeof(observed) && observed.height==-27);
        CHECK(sentinel && delete_object(sentinel));
    }else if(child || capacity){
        DWORD wanted=child?128:512,allocated=0;
        for(DWORD i=0;i<wanted;i++){
            request.height=-12-(int)(i%25);
            fonts[i]=create_font(&request);CHECK(fonts[i]!=0);
            if(!fonts[i])break;
            allocated++;
            CHECK(get_object(fonts[i],sizeof(observed),&observed)==sizeof(observed) && observed.height==request.height && observed.weight==400);
        }
        SAY("FONTLIFE allocated=");HEX(allocated);SAY(" wanted=");HEX(wanted);SAY("\n");
        if(!child)for(DWORD i=0;i<allocated;i++)CHECK(delete_object(fonts[i]));
        /* Child exit deliberately leaves every font owned by the process. */
    }else{
        HANDLE sentinel=create_font(&request);CHECK(sentinel!=0);
        DWORD length=module_name(0,executable,sizeof(executable));CHECK(length>0 && length<sizeof(executable));
        if(length>0 && length<sizeof(executable)){
            command[0]='"';for(DWORD i=0;i<length;i++)command[i+1]=executable[i];
            const char suffix[]="\" --child";
            for(DWORD i=0;i<sizeof(suffix);i++)command[length+1+i]=suffix[i];
            startup.cb=sizeof(startup);
            for(DWORD round=0;round<4;round++){
                PROCESS process={0};DWORD code=259;
                int started=create_process(executable,command,0,0,0,0x08000000,0,0,&startup,&process);CHECK(started);
                if(started){
                    CHECK(wait_one(process.process,30000)==0);
                    CHECK(exit_code(process.process,&code) && code==0);
                    CHECK(close_handle(process.thread));CHECK(close_handle(process.process));
                }
                CHECK(get_object(sentinel,sizeof(observed),&observed)==sizeof(observed) && observed.height==-19 && observed.weight==400);
                SAY("FONTLIFE child-round=");HEX(round);SAY(" exit=");HEX(code);SAY("\n");
            }
        }
        CHECK(sentinel && delete_object(sentinel));
    }
    SAY("FONTLIFE DONE checks=");HEX(checks);SAY(" failures=");HEX(failures);SAY("\n");
    ExitProcess(failures?1:0);
}
