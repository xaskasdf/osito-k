/* Local-file transfer oracle. Never invokes InstallComponents or setup code. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UPTR;
typedef int HR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
typedef struct {DWORD a;WORD b,c;unsigned char d[8];} GUID;
static const GUID clsid={0x6e449686,0xc509,0x11cf,{0xaa,0xfa,0,0xaa,0,0xb6,1,0x5c}};
static const GUID engine_iid={0x6e449689,0xc509,0x11cf,{0xaa,0xfa,0,0xaa,0,0xb6,1,0x5c}};
static unsigned failures;
static volatile DWORD done, callback_refs=1, progress_calls;
static volatile HR completion;
static void *engine,*timing_view;
static const GUID timing_iid={0x6e449687,0xc509,0x11cf,{0xaa,0xfa,0,0xaa,0,0xb6,1,0x5c}};
static void (CALL *itd_sleep)(DWORD);
static DWORD (CALL *itd_ticks)(void);
static DWORD cancel_transfer;
static void itd_print(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void itd_hex(DWORD n)
{ char b[9];unsigned i=8;b[8]=0;do{unsigned d=n&15;b[--i]=(char)(d<10?'0'+d:'a'+d-10);n>>=4;}while(n);itd_print("0x");itd_print(b+i); }
static void itd_log(const char *label,DWORD a,DWORD b)
{ itd_print("IED EVENT ");itd_print(label);itd_print(" ");itd_hex(a);itd_print(" ");itd_hex(b);itd_print("\n"); }
static void itd_check(const char *label,int ok)
{ if(!ok)failures++;itd_print(ok?"IED PASS ":"IED FAIL ");itd_print(label);itd_print("\n"); }
static DWORD CALL itd_addref(void *o)
{ (void)o;return ++callback_refs; }
static DWORD CALL itd_release(void *o)
{ (void)o;return --callback_refs; }
static HR CALL itd_query(void *o,const GUID *id,void **out)
{
    if(!out)return (HR)0x80004003;
    *out=0;
    if(id->a!=0&&id->a!=0x6e449685)return (HR)0x80004002;
    *out=o;itd_addref(o);return 0;
}
static void itd_snapshot(const char *label)
{
    if(!timing_view)return;
    typedef HR (CALL *ratesfn)(void *,DWORD *,DWORD *);
    typedef HR (CALL *progressfn)(void *,DWORD *);
    void **v=*(void ***)timing_view;DWORD down=0xdead,install=0xbeef,progress[5]={20,0xdead,0xdead,0xdead,0xdead};
    HR hr=((ratesfn)v[3])(timing_view,&down,&install);
    itd_print("ITD SAMPLE ");itd_print(label);itd_print(" ticks=");itd_hex(itd_ticks());
    itd_print(" rates=");itd_hex((DWORD)hr);itd_print(" ");itd_hex(down);itd_print(" ");itd_hex(install);
    hr=((progressfn)v[4])(timing_view,progress);itd_print(" progress=");itd_hex((DWORD)hr);
    for(unsigned i=0;i<5;i++){itd_print(" ");itd_hex(progress[i]);}itd_print("\n");
}
static HR CALL itd_status(void *o,DWORD state,DWORD sub)
{ (void)o;itd_log("state",state,sub);return 0; }
static HR CALL itd_start(void *o,DWORD download,DWORD install)
{ (void)o;itd_log("start",download,install);itd_snapshot("start");return 0; }
static HR CALL itd_component(void *o,const char *id,DWORD download,DWORD install,const char *text)
{ (void)o;(void)id;(void)text;itd_log("component",download,install);return 0; }
static HR CALL itd_progress(void *o,const char *id,DWORD phase,const char *text,const char *message,DWORD current,DWORD maximum)
{
    (void)o;(void)id;(void)text;(void)message;progress_calls++;
    itd_log("progress",phase,current);itd_log("maximum",maximum,0);itd_snapshot("progress");if(phase==2)itd_sleep(300);
    if(cancel_transfer){
        typedef HR (CALL *abortfn)(void *,DWORD);
        ((abortfn)(*(void ***)engine)[22])(engine,0);
        return (HR)0x80004004;
    }
    return 0;
}
static HR CALL itd_stop_component(void *o,const char *id,HR status,DWORD phase,const char *text,DWORD flags)
{ (void)o;(void)id;(void)text;(void)flags;itd_log("stop-component",(DWORD)status,phase);return 0; }
static HR CALL itd_stop(void *o,HR status,const char *text,DWORD flags)
{ (void)o;(void)text;itd_log("stop",(DWORD)status,flags);completion=status;itd_snapshot("stop");done=1;return 0; }
static HR CALL itd_problem(void *o,DWORD problem,DWORD *action)
{ (void)o;itd_log("problem",problem,0);if(action)*action=0;return (HR)0x80004004; }
static void *callback_table[]={itd_query,itd_addref,itd_release,itd_status,itd_start,
    itd_component,itd_progress,itd_stop_component,itd_stop,itd_problem};
static void **callback=callback_table;
void installengine_timing_download_entry(void)
{
    typedef HR (CALL *initfn)(void *,DWORD);
    typedef void (CALL *uninitfn)(void);
    typedef HR (CALL *createfn)(const GUID *,void *,DWORD,const GUID *,void **);
    typedef DWORD (CALL *modulefn)(void *,char *,DWORD);
    typedef void *(CALL *filefn)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
    typedef int (CALL *readfn)(void *,void *,DWORD,DWORD *,void *);
    typedef int (CALL *closefn)(void *);
    typedef void (CALL *sleepfn)(DWORD);
    typedef HR (CALL *stringfn)(void *,const char *);
    typedef HR (CALL *wordfn)(void *,DWORD);
    typedef HR (CALL *callbackfn)(void *,void *);
    typedef HR (CALL *actionfn)(void *,const char *,DWORD,DWORD);
    typedef DWORD (CALL *reffn)(void *);
    void *k=LoadLibraryA("kernel32.dll"),*ole=LoadLibraryA("ole32.dll");
    initfn init=GetProcAddress(ole,"CoInitializeEx");
    uninitfn uninit=GetProcAddress(ole,"CoUninitialize");
    createfn create=GetProcAddress(ole,"CoCreateInstance");
    modulefn module=GetProcAddress(k,"GetModuleFileNameA");
    filefn open=GetProcAddress(k,"CreateFileA");
    readfn read=GetProcAddress(k,"ReadFile");
    closefn close=GetProcAddress(k,"CloseHandle");
    sleepfn sleep=GetProcAddress(k,"Sleep");itd_sleep=sleep;itd_ticks=GetProcAddress(k,"GetTickCount");
    typedef char *(CALL *commandfn)(void);
    commandfn command=GetProcAddress(k,"GetCommandLineA");
    const char *line=command();DWORD allow=0,extract=0;
    for(unsigned i=0;line[i];i++)if(line[i]==' '){if(line[i+1]=='a')allow=1;if(line[i+1]=='e'){allow=1;extract=1;}if(line[i+1]=='c'){allow=1;cancel_transfer=1;}}
    if(!init||!uninit||!create||!module||!open||!read||!close||!sleep)ExitProcess(2);
    init(0,2);
    HR hr=create(&clsid,0,1,&engine_iid,&engine);
    itd_check("activation",hr==0&&engine);if(hr<0||!engine)ExitProcess(2);
    void **v=*(void ***)engine;
    typedef HR (CALL *queryfn)(void *,const GUID *,void **);
    hr=((queryfn)v[0])(engine,&timing_iid,&timing_view);itd_check("timing-interface",hr==0&&timing_view);itd_snapshot("initial");
    char base[260],path[260],url[300];DWORD length=module(0,base,260),slash=0;
    for(DWORD i=0;i<length;i++)if(base[i]=='\\'||base[i]=='/')slash=i+1;
    base[slash]=0;
    const char *names[]={"timing-transfer-source.bin","timing-transfer-probe.cif","engine-timing-download-probe"};
    static char payload[32769];for(unsigned i=0;i<sizeof(payload);i++)payload[i]=(char)(i*17);
    const char catalog[]="[Version]\nSignature=$Chicago$\n[Transfer]\nSectionType=Component\nSize=32,0\nURL1=\"timing-transfer-source.bin\",2\n";
    const char catalog_extract[]="[Version]\nSignature=$Chicago$\n[Transfer]\nSectionType=Component\nSize=32,0\nURL1=\"timing-transfer-source.bin\",3\n";
    for(unsigned file=0;file<3;file++){
        for(unsigned i=0;i<slash;i++)path[i]=base[i];
        unsigned n=0;while(names[file][n]){path[slash+n]=names[file][n];n++;}path[slash+n]=0;
        if(file<2){
            void *handle=open(path,0x40000000,0,0,2,0x80,0);
            DWORD written=0,size=file?sizeof(catalog)-1:sizeof(payload)-1;
            int ok=handle!=(void *)(UPTR)-1&&WriteFile(handle,file?(extract?catalog_extract:catalog):payload,size,&written,0)&&written==size;
            itd_check("write-local-fixture",ok);if(handle!=(void *)(UPTR)-1)close(handle);
            if(file==1){hr=((stringfn)v[25])(engine,path);itd_check("local-cif",hr==0);}
        }else{hr=((stringfn)v[17])(engine,path);itd_check("download-directory",hr==0);}
    }
    const char *scheme="file:///";for(unsigned i=0;i<8;i++)url[i]=scheme[i];
    for(unsigned i=0;i<slash;i++)url[8+i]=base[i]=='\\'?'/':base[i];url[8+slash]=0;
    for(unsigned i=0;line[i];i++)if(line[i]==' '&&line[i+1]=='h'&&line[i+2]=='t'){
        unsigned n=0;while(line[i+1+n]&&line[i+1+n]!=' '&&n<sizeof(url)-1){url[n]=line[i+1+n];n++;}
        url[n]=0;break;
    }
    hr=((stringfn)v[16])(engine,url);itd_check("file-base-url",hr==0);
    hr=((actionfn)v[12])(engine,"Transfer",1,0);itd_check("select-transfer",hr==0);
    hr=((wordfn)v[19])(engine,2);itd_check("download-options",hr==0);
    hr=((callbackfn)v[10])(engine,&callback);itd_check("register-callback",hr==0);
    const char *leaf="engine-timing-download-probe\\timing-transfer-source.bin";
    for(unsigned i=0;i<slash;i++)path[i]=base[i];
    unsigned n=0;while(leaf[n]){path[slash+n]=leaf[n];n++;}path[slash+n]=0;
    typedef int (CALL *deletefn)(const char *);
    deletefn remove=GetProcAddress(k,"DeleteFileA");
    remove(path); /* Only the named output of this fixture, to avoid cache hits. */
    itd_snapshot("selected");hr=((wordfn)v[5])(engine,allow?17:1);itd_log("download-return",(DWORD)hr,done);
    for(unsigned i=0;i<1500&&!done&&hr>=0;i++)sleep(10);
    sleep(100);
    itd_check("completion-callback",done!=0);
    itd_check("download-result",done&&(cancel_transfer?(DWORD)completion==0x80004004:allow?completion==0:(DWORD)completion==0x800b0003));
    void *handle=open(path,0x80000000,1,0,3,0x80,0);
    static char content[sizeof(payload)];DWORD got=0;
    int correct=handle!=(void *)(UPTR)-1&&read(handle,content,sizeof(content),&got,0)&&got==sizeof(payload)-1;
    for(unsigned i=0;correct&&i<got;i++)if(content[i]!=payload[i])correct=0;
    if(allow&&!cancel_transfer)itd_check("downloaded-bytes",correct);
    if(handle!=(void *)(UPTR)-1)close(handle);
    itd_check("progress-callback",progress_calls>0);
    ((reffn)v[11])(engine);itd_log("callback-refs",callback_refs,0);
    itd_check("callback-references-balanced",callback_refs==1);
    itd_snapshot("final");if(timing_view)((reffn)(*(void ***)timing_view)[2])(timing_view);((reffn)v[2])(engine);uninit();itd_print("IED DONE\n");ExitProcess(failures?1:0);
}
