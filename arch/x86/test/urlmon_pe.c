/* URLMON callback oracle. Fixture paths stay in the selected test directory. */
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
typedef struct {DWORD a;WORD b,c;unsigned char d[8];} GUID;
typedef struct {void **vtable;DWORD refs;void *binding;} CALLBACK;
static DWORD mode,started,stopped,progressed,data_calls,stop_result,abort_result;
static DWORD failures;
static char path[1024];
static WORD url[1100];
#define SAY(s) do{DWORD w;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("URLMON PASS " name "\n");else{SAY("URLMON FAIL " name "\n");failures++;}}while(0)
#define HEX(v) do{char h[9];DWORD n=(DWORD)(v),w;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[n&15];n>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&w,0);}while(0)
static int CALL urlmon_probe_query(CALLBACK *self,const GUID *iid,void **out)
{
    static const GUID unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
    static const GUID status={0x79eac9c1,0xbaf9,0x11ce,{0x8c,0x82,0,0xaa,0,0x4b,0xa9,0xb}};
    if(!out)return (int)0x80004003U;
    *out=0;int match1=1,match2=1;
    for(unsigned i=0;i<16;i++){
        if(((const unsigned char *)iid)[i]!=((const unsigned char *)&unknown)[i])match1=0;
        if(((const unsigned char *)iid)[i]!=((const unsigned char *)&status)[i])match2=0;
    }
    if(!match1&&!match2)return (int)0x80004002U;
    self->refs++;*out=self;return 0;
}
static DWORD CALL urlmon_probe_addref(CALLBACK *self) {return ++self->refs;}
static DWORD CALL urlmon_probe_release(CALLBACK *self) {return --self->refs;}
static int CALL urlmon_probe_start(CALLBACK *self,DWORD reserved,void *binding)
{
    (void)reserved;started++;self->binding=binding;
    if(binding){typedef DWORD (CALL *fn)(void *);((fn)(*(void ***)binding)[1])(binding);}
    SAY("URLMON START ");HEX(binding!=0);SAY("\n");
    return 0;
}
static int CALL urlmon_probe_priority(CALLBACK *self,int *priority)
{(void)self;(void)priority;return (int)0x80004001U;}
static int CALL urlmon_probe_low(CALLBACK *self,DWORD reserved)
{(void)self;(void)reserved;return (int)0x80004001U;}
static int CALL urlmon_probe_progress(CALLBACK *self,DWORD current,DWORD maximum,DWORD status,const WORD *text)
{
    (void)self;(void)text;progressed++;
    SAY("URLMON PROGRESS ");HEX(status);HEX(current);HEX(maximum);SAY("\n");
    int data_status=status==4||status==5;
    if((mode==3||(mode==6&&data_status))&&self->binding&&abort_result==0xccccccccU){typedef int (CALL *fn)(void *);abort_result=(DWORD)((fn)(*(void ***)self->binding)[3])(self->binding);}
    return mode==2||((mode==5||mode==7)&&data_status)?(int)0x80004004U:0;
}
static int CALL urlmon_probe_stop(CALLBACK *self,DWORD result,const WORD *error)
{
    (void)error;stopped++;stop_result=result;
    if(self->binding){typedef DWORD (CALL *fn)(void *);((fn)(*(void ***)self->binding)[2])(self->binding);self->binding=0;}
    return 0;
}
static int CALL urlmon_probe_bindinfo(CALLBACK *self,DWORD *flags,void *info)
{(void)self;(void)flags;(void)info;return (int)0x80004001U;}
static int CALL urlmon_probe_data(CALLBACK *self,DWORD flags,DWORD count,void *format,void *medium)
{(void)self;(void)flags;(void)count;(void)format;(void)medium;data_calls++;return (int)0x80004001U;}
static int CALL urlmon_probe_object(CALLBACK *self,const GUID *iid,void *object)
{(void)self;(void)iid;(void)object;return (int)0x80004001U;}
static void *callback_vtable[]={urlmon_probe_query,urlmon_probe_addref,urlmon_probe_release,
    urlmon_probe_start,urlmon_probe_priority,urlmon_probe_low,urlmon_probe_progress,
    urlmon_probe_stop,urlmon_probe_bindinfo,urlmon_probe_data,urlmon_probe_object};
void urlmon_probe_entry(void)
{
    void *k=LoadLibraryA("kernel32.dll"),*u=LoadLibraryA("urlmon.dll");
    typedef int (CALL *downloadfn)(void *,const WORD *,const WORD *,DWORD,void *);
    typedef DWORD (CALL *fullfn)(const char *,DWORD,char *,char **);
    typedef void *(CALL *createfn)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
    typedef int (CALL *closefn)(void *);
    typedef int (CALL *readfn)(void *,void *,DWORD,DWORD *,void *);
    typedef int (CALL *deletefn)(const char *);
    downloadfn download=(downloadfn)GetProcAddress(u,"URLDownloadToFileW");
    fullfn full=(fullfn)GetProcAddress(k,"GetFullPathNameA");
    createfn create=(createfn)GetProcAddress(k,"CreateFileA");
    closefn close=(closefn)GetProcAddress(k,"CloseHandle");
    readfn read=(readfn)GetProcAddress(k,"ReadFile");
    deletefn remove=(deletefn)GetProcAddress(k,"DeleteFileA");
    typedef const char *(CALL *cmdfn)(void);
    cmdfn command=(cmdfn)GetProcAddress(k,"GetCommandLineA");
    int file_mode=0;
    if(command){const char *s=command();while(*s){if(s[0]==' '&&s[1]=='f'&&s[2]=='i'&&s[3]=='l'&&s[4]=='e'){file_mode=1;break;}s++;}}
    CHECK(download&&full&&create&&close&&read&&remove,"exports");if(failures)ExitProcess(1);
    void *file=create("urlmon-source.txt",0x40000000,3,0,2,0x80,0);DWORD written=0;
    CHECK(file!=(void *)(UP)-1&&WriteFile(file,"DirectX download probe",22,&written,0)&&written==22,"create-source");
    if(file!=(void *)(UP)-1)close(file);if(failures)ExitProcess(1);
    DWORD length=full("urlmon-source.txt",1024,path,0);
    CHECK(length&&length<1024,"source-path");if(failures)ExitProcess(1);
    const char *prefix="file:///";unsigned at=0;
    while(prefix[at]){url[at]=(WORD)prefix[at];at++;}
    for(unsigned i=0;i<length;i++)url[at++]=path[i]=='\\'?'/':(unsigned char)path[i];
    url[at]=0;
    /* Optional HTTP URL supplied by the host/QEMU test runner. */
    file=create("urlmon-url.txt",0x80000000,3,0,3,0x80,0);
    if(file!=(void *)(UP)-1&&!file_mode) {
        DWORD count=0;if(read(file,path,1023,&count,0)&&count){
            while(count&&(path[count-1]=='\r'||path[count-1]=='\n'))count--;
            for(unsigned i=0;i<count;i++)url[i]=(unsigned char)path[i];url[count]=0;
        }
    }
    if(file!=(void *)(UP)-1)close(file);
    unsigned base_length=0;while(url[base_length])base_length++;
    for(mode=0;mode<8;mode++) {
        url[base_length]=0;
        CALLBACK callback={callback_vtable,1,0};
        started=stopped=progressed=data_calls=0;stop_result=abort_result=0xcccccccc;
        remove("urlmon-download.out");
        if(mode==7){file=create("urlmon-download.out",0x40000000,3,0,2,0x80,0);written=0;
            CHECK(file!=(void *)(UP)-1&&WriteFile(file,"keep",4,&written,0)&&written==4,"prepare-existing-destination");
            if(file!=(void *)(UP)-1)close(file);}
        if(mode==4){unsigned i=0;while(url[i])i++;const char *suffix=".absent";while(*suffix)url[i++]=(WORD)*suffix++;url[i]=0;}
        DWORD result=(DWORD)download(0,url,(const WORD *)L"urlmon-download.out",0,mode==1?0:&callback);
        file=create("urlmon-download.out",0x80000000,3,0,3,0x80,0);
        char payload[32]={0};DWORD size=0;int exists=file!=(void *)(UP)-1;
        if(exists){read(file,payload,sizeof(payload),&size,0);close(file);}
        SAY("URLMON RESULT ");HEX(mode);HEX(result);HEX(started);HEX(stopped);HEX(progressed);HEX(data_calls);HEX(stop_result);HEX(callback.refs);HEX(exists);HEX(size);HEX(abort_result);SAY("\n");
        if(mode<2)CHECK(result==0&&size==22&&payload[0]=='D'&&payload[21]=='e',"downloaded-payload");
        if(mode==0)CHECK(started==1&&stopped==1&&progressed&&stop_result==1&&data_calls==0,"completed-callbacks");
        if(mode==1)CHECK(!started&&!stopped&&!progressed,"optional-callback");
        if(mode==4)CHECK((int)result<0&&stop_result==result&&started==1&&stopped==1&&!exists,"missing-resource");
        if(mode==2||mode==3||mode>=5){
            CHECK(result==0&&started==1&&stopped==1&&stop_result==0x80004004U,"cancellation-callback");
            if(mode==7)CHECK(exists&&size==4&&payload[0]=='k'&&payload[3]=='p',"cancel-preserves-existing-file");
            else CHECK(!exists,"cancel-no-destination");
            if(mode==3||mode==6)CHECK(abort_result==0,"binding-abort");
        }
        CHECK(callback.refs==1&&!callback.binding,"balanced-callback-lifetime");
        if(callback.binding){typedef DWORD (CALL *fn)(void *);((fn)(*(void ***)callback.binding)[2])(callback.binding);}
    }
    if(!failures)SAY("URLMON DONE\n");ExitProcess(failures?1:0);
}
