/* Read-only native/guest CAB trust oracle. Never loads CAB payloads. */
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
typedef struct {DWORD size;const WORD *path;void *file;const GUID *subject;} FILEINFO;
typedef struct {DWORD size;void *policy,*sip;DWORD ui,revoke,choice;FILEINFO *file;DWORD action;void *state;const WORD *url;DWORD flags,context;void *settings;} DATA;
typedef struct {DWORD lo,hi;} FILETIME;
typedef struct {DWORD size;FILETIME time;DWORD count;void *certificates;DWORD type;void *info;DWORD error,counters;void *counter_chain,*chain;} SIGNER;
#define SAY(s) do{DWORD written;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&written,0);}while(0)
#define HEX(v) do{char hex[9];DWORD value=(DWORD)(v),written;for(int j=7;j>=0;j--){hex[j]="0123456789abcdef"[value&15];value>>=4;}hex[8]=' ';WriteFile(GetStdHandle((DWORD)-11),hex,9,&written,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("CABTRUST PASS " name "\n");else{SAY("CABTRUST FAIL " name "\n");failures++;}}while(0)
void cabtrust_probe_entry(void)
{
    DWORD failures=0;
    void *w=LoadLibraryA("wintrust.dll");
    typedef int (CALL *VERIFY)(void *,const GUID *,DATA *);
    typedef void *(CALL *PROVIDER)(void *);
    typedef SIGNER *(CALL *GETSIGNER)(void *,DWORD,int,DWORD);
    VERIFY verify=GetProcAddress(w,"WinVerifyTrustEx");
    PROVIDER provider=GetProcAddress(w,"WTHelperProvDataFromStateData");
    GETSIGNER signer=GetProcAddress(w,"WTHelperGetProvSignerFromChain");
    CHECK(verify&&provider&&signer,"exports");if(failures)ExitProcess(1);
    GUID action={0xaac56b,0xcd44,0x11d0,{0x8c,0xc2,0,0xc0,0x4f,0xc2,0x95,0xee}};
    const WORD *files[]={
        (const WORD *)L"cab-good.cab",(const WORD *)L"cab-body.cab",
        (const WORD *)L"cab-badsig.cab",(const WORD *)L"cab-badtime.cab",
        (const WORD *)L"cab-badchain.cab",(const WORD *)L"cab-offset.cab",
        (const WORD *)L"cab-length.cab",(const WORD *)L"cab-unsigned.cab",
        (const WORD *)L"cab-reserved1.cab",(const WORD *)L"cab-reserved2.cab",
        (const WORD *)L"cab-index.cab",(const WORD *)L"cab-reserved.cab",
        (const WORD *)L"cab-tail.cab",(const WORD *)L"cab-absent.cab",
        (const WORD *)L"cab-reserved-end.cab",(const WORD *)L"cab-imprint.cab",
        (const WORD *)L"cab-enhanced.cab",(const WORD *)L"cab-timecontent.cab",
        (const WORD *)L"cab-truncated.cab",(const WORD *)L"cab-index-high.cab"
    };
    for(unsigned mode=0;mode<2;mode++)for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);i++) {
        FILEINFO file={sizeof(FILEINFO),files[i],0,0};
        struct {UP before;DATA value;UP after;} guard={0x12345678,{sizeof(DATA),0,0,2,0,1,&file,1,0,0,mode?0x1580:0x1000,0,0},0x87654321};
        DATA *data=&guard.value;
        DWORD rc=(DWORD)verify((void *)(UP)-1,&action,data);
        SAY("CABTRUST RESULT ");HEX(mode);HEX(i);HEX(rc);SAY("\n");
        CHECK(guard.before==0x12345678&&guard.after==0x87654321,"data-canaries");
        if(i==0||i==8||i==11||i==14)CHECK(rc==0,"original-or-excluded-reserve-trusted");
        else CHECK(rc!=0,"modified-or-unsigned-rejected");
        if(i==0&&rc==0) {
            void *p=provider(data->state);
            SIGNER *s=p?signer(p,0,0,0):0;
            CHECK(s&&s->size==sizeof(SIGNER)&&s->count>=2&&s->chain&&!s->error,"signer-chain");
            CHECK(s&&s->time.hi&&s->counters==1&&signer(p,0,1,0),"timestamp-chain");
        }
        data->action=2;
        CHECK(verify((void *)(UP)-1,&action,data)==0&&!data->state,"close-state");
    }
    if(!failures)SAY("CABTRUST DONE\n");
    ExitProcess(failures?1:0);
}
