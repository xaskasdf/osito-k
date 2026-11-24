/* Runs against native Windows or Osito-K using external, read-only PE fixtures. */
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
typedef struct {DWORD error,info;} TRUST;
typedef struct {DWORD size;TRUST trust;DWORD count;void *chains;DWORD lower;void *lower_chains;DWORD fresh,seconds,flags;unsigned char id[16];} CHAIN;
typedef struct {DWORD size,flags;void *extra;} PARA;
typedef struct {DWORD size,error;int chain,element;void *extra;} STATUS;
_Static_assert(sizeof(SIGNER)==(sizeof(UP)==4?44:64),"signer ABI");
#define SAY(s) do{DWORD w;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("WINTRUST PASS " name "\n");else{SAY("WINTRUST FAIL " name "\n");failures++;}}while(0)
#define HEX(v) do{char h[9];DWORD n=(DWORD)(v),w;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[n&15];n>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&w,0);}while(0)
void wintrust_probe_entry(void)
{
    DWORD failures=0;
    void *w=LoadLibraryA("wintrust.dll"),*c=LoadLibraryA("crypt32.dll");
    typedef int (CALL *verifyfn)(void *,const GUID *,DATA *);
    typedef void *(CALL *providerfn)(void *);
    typedef SIGNER *(CALL *signerfn)(void *,DWORD,int,DWORD);
    typedef int (CALL *policyfn)(const char *,const CHAIN *,const PARA *,STATUS *);
    verifyfn verify=(verifyfn)GetProcAddress(w,"WinVerifyTrustEx");
    providerfn provider=(providerfn)GetProcAddress(w,"WTHelperProvDataFromStateData");
    signerfn signer=(signerfn)GetProcAddress(w,"WTHelperGetProvSignerFromChain");
    policyfn policy=(policyfn)GetProcAddress(c,"CertVerifyCertificateChainPolicy");
    CHECK(verify&&provider&&signer&&policy,"exports");if(failures)ExitProcess(1);
    GUID action={0xaac56b,0xcd44,0x11d0,{0x8c,0xc2,0,0xc0,0x4f,0xc2,0x95,0xee}};
    const WORD *files[]={(const WORD *)L"dsetup.dll",(const WORD *)L"dsetup-bad.dll",(const WORD *)L"unsigned.exe",(const WORD *)L"absent-auth-file.exe",(const WORD *)L"dsetup-badsig.dll",(const WORD *)L"dsetup-badchain.dll"};
    typedef const char *(CALL *commandfn)(void);
    commandfn command=(commandfn)GetProcAddress(LoadLibraryA("kernel32.dll"),"GetCommandLineA");
    const char *cmd=command?command():"";int guest=0,leak=0;
    for(unsigned i=0;cmd[i];i++) {
        if(cmd[i]==' '&&cmd[i+1]=='g'&&cmd[i+2]=='u'&&cmd[i+3]=='e'&&cmd[i+4]=='s'&&cmd[i+5]=='t'&&!cmd[i+6])guest=1;
        if(cmd[i]==' '&&cmd[i+1]=='l'&&cmd[i+2]=='e'&&cmd[i+3]=='a'&&cmd[i+4]=='k'&&!cmd[i+5])leak=1;
    }
    if(leak) {
        FILEINFO file={sizeof(FILEINFO),files[2],0,0};
        for(unsigned i=0;i<64;i++) {
            DATA data={sizeof(DATA),0,0,2,0,1,&file,1,0,0,0x1000,0,0};
            if((DWORD)verify((void *)(UP)-1,&action,&data)!=0x800b0100U||!data.state){SAY("WINTRUST FAIL process-state-lifetime\n");ExitProcess(1);}
        }
        SAY("WINTRUST PASS process-state-lifetime\nWINTRUST DONE\n");ExitProcess(0);
    }
    for(unsigned i=0;i<6;i++) {
        FILEINFO file={sizeof(FILEINFO),files[i],0,0};
        struct {UP before;DATA value;UP after;} guard={0x11223344,{sizeof(DATA),0,0,2,0,1,&file,1,0,0,0x1000,0,0},0x55667788};
        DATA *data=&guard.value;
        DWORD rc=(DWORD)verify((void *)(UP)-1,&action,data);
        SAY("WINTRUST RESULT ");HEX(i);HEX(rc);SAY("\n");
        CHECK(i==0?rc==0:i==1?rc==0x80096010U:i==2?rc==0x800b0100U:i==3?rc==0x80092003U:rc!=0,"verification-result");
        CHECK(guard.before==0x11223344&&guard.after==0x55667788,"trust-data-canaries");
        if(i<3)CHECK(data->state!=0,"retained-state");
        if(i==0&&rc==0) {
            void *p=provider(data->state);
            CHECK(p&&*(DWORD *)p>=(sizeof(UP)==4?128U:224U),"provider-layout");
            SIGNER *s=p?signer(p,0,0,0):0;
            CHECK(s&&s->size==sizeof(SIGNER)&&s->count>=2&&s->chain&&s->certificates,"signer-chain-layout");
            if(s&&s->chain) {
                CHAIN *chain=s->chain;
                CHECK(chain->size==sizeof(CHAIN)&&chain->count>=1&&!chain->trust.error,"verified-chain");
                PARA para={sizeof(PARA),0,0};STATUS status={sizeof(STATUS),0xcccccccc,-1,-1,0};
                CHECK(policy((const char *)7,chain,&para,&status)&&!status.error,"microsoft-product-root");
                para.flags=0x20000;
                CHECK(policy((const char *)7,chain,&para,&status)&&status.error==0x800b0109U,"different-root-policy-rejected");
                CHECK(s->time.hi&&s->counters==1&&signer(p,0,1,0)!=0,"verified-timestamp-chain");
                CHECK(!signer(p,1,0,0)&&!signer(p,0,1,1),"signer-index-bounds");
            }
        }
        data->action=2;CHECK(verify((void *)(UP)-1,&action,data)==0&&!data->state,"close-clears-state");
        CHECK(verify((void *)(UP)-1,&action,data)==0,"close-empty-state");
    }
    FILEINFO file={sizeof(FILEINFO),files[0],0,0};
    DATA directx={sizeof(DATA),0,0,2,0,1,&file,1,0,0,0x1580,0,0};
    DWORD rc=(DWORD)verify((void *)(UP)-1,&action,&directx);
    SAY("WINTRUST DIRECTX_FLAGS ");HEX(rc);SAY("\n");
    CHECK(!rc,"directx-verification-policy");
    if(directx.state) {
        void *p=provider(directx.state);SIGNER *s=p?signer(p,0,0,0):0;
        if(p){SAY("WINTRUST POLICY_SETTINGS ");HEX(*(DWORD *)((unsigned char *)p+(sizeof(UP)==4?32:56)));SAY("\n");}
        if(s&&s->chain){SAY("WINTRUST DIRECTX_CHAIN ");HEX(((CHAIN *)s->chain)->trust.error);HEX(s->error);SAY("\n");}
    }
    directx.action=2;CHECK(verify((void *)(UP)-1,&action,&directx)==0&&!directx.state,"close-directx-policy-state");
    /* Registry-policy mutation is explicitly restricted to the guest run. */
    if(guest) {
        void *a=LoadLibraryA("advapi32.dll"),*key=0;
        typedef int (CALL *createfn)(void *,const char *,DWORD,char *,DWORD,DWORD,void *,void **,DWORD *);
        typedef int (CALL *queryfn)(void *,const char *,DWORD *,DWORD *,void *,DWORD *);
        typedef int (CALL *setfn)(void *,const char *,DWORD,DWORD,const void *,DWORD);
        typedef int (CALL *deletefn)(void *,const char *);
        typedef int (CALL *closefn)(void *);
        createfn create=(createfn)GetProcAddress(a,"RegCreateKeyExA");
        queryfn query=(queryfn)GetProcAddress(a,"RegQueryValueExA");
        setfn set=(setfn)GetProcAddress(a,"RegSetValueExA");
        deletefn remove=(deletefn)GetProcAddress(a,"RegDeleteValueA");
        closefn close=(closefn)GetProcAddress(a,"RegCloseKey");
        DWORD disposition=0,old=0,type=0,size=sizeof(old),strict=0;
        int ready=create&&query&&set&&remove&&close&&create((void *)(UP)(int)0x80000001U,
            "Software\\Microsoft\\Windows\\CurrentVersion\\WinTrust\\Trust Providers\\Software Publishing",0,0,0,3,0,&key,&disposition)==0;
        CHECK(ready,"guest-policy-key");
        if(ready) {
            int present=query(key,"State",0,&type,&old,&size)==0;
            CHECK(set(key,"State",0,4,&strict,sizeof(strict))==0,"guest-strict-policy");
            directx.action=1;directx.state=0;
            CHECK(verify((void *)(UP)-1,&action,&directx)!=0,"unavailable-revocation-rejected-under-strict-policy");
            directx.action=2;verify((void *)(UP)-1,&action,&directx);
            int restored=present?set(key,"State",0,type,&old,size)==0:remove(key,"State")==0;
            CHECK(restored,"guest-policy-restored");close(key);
        }
    }
    for(unsigned i=0;i<70;i++) {
        file.path=files[2];DATA data={sizeof(DATA),0,0,2,0,1,&file,1,0,0,0x1000,0,0};
        int good=(DWORD)verify((void *)(UP)-1,&action,&data)==0x800b0100U&&data.state;
        data.action=2;good=good&&verify((void *)(UP)-1,&action,&data)==0&&!data.state;
        if(!good){CHECK(0,"repeated-state-lifetime");break;}
        if(i==69)CHECK(1,"repeated-state-lifetime");
    }
    if(!failures)SAY("WINTRUST DONE\n");ExitProcess(failures?1:0);
}
