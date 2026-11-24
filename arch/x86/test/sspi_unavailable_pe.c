/* Native/guest SSPI negative contracts. No tokens are sent to any server and
 * no default credentials are acquired. Known-package absence is guest-only. */
typedef unsigned char BYTE;
typedef unsigned int DWORD;
typedef int LONG;
typedef __UINTPTR_TYPE__ UP;
typedef unsigned long long U64;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void CALL SetLastError(DWORD);
API DWORD CALL GetLastError(void);
API char *CALL GetCommandLineA(void);
typedef struct {UP lower,upper;} Handle;
typedef struct {DWORD version,count;void *buffers;} BufferDesc;
static LONG (CALL *acquire)(const char *,const char *,DWORD,void *,void *,void *,void *,void *,void *);
static LONG (CALL *initialize)(void *,void *,const char *,DWORD,DWORD,DWORD,void *,DWORD,void *,void *,DWORD *,void *);
static LONG (CALL *complete)(void *,void *);
static LONG (CALL *delete_context)(void *);
static LONG (CALL *free_credential)(void *);
static DWORD checks,failures;
static void say(const char *s){DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(DWORD value){char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[value&15];value>>=4;}s[8]=0;say(s);}
static void check(int ok,const char *label){checks++;if(!ok){failures++;say("SSPI FAIL ");say(label);say("\n");}}
static int bytes_are(const void *data,BYTE expected,DWORD size)
{const BYTE *p=data;while(size--)if(*p++!=expected)return 0;return 1;}
static void fill(void *data,BYTE value,DWORD size){BYTE *p=data;while(size--)*p++=value;}
static int guest_mode(void)
{
    const char *text=GetCommandLineA(),*wanted="--osito-guest";
    for(DWORD i=0;text&&text[i];i++){DWORD j=0;while(wanted[j]&&text[i+j]==wanted[j])j++;if(!wanted[j])return 1;}
    return 0;
}
void sspi_entry(void)
{
    void *dll=LoadLibraryA("secur32.dll");
    acquire=GetProcAddress(dll,"AcquireCredentialsHandleA");
    initialize=GetProcAddress(dll,"InitializeSecurityContextA");
    complete=GetProcAddress(dll,"CompleteAuthToken");
    delete_context=GetProcAddress(dll,"DeleteSecurityContext");
    free_credential=GetProcAddress(dll,"FreeCredentialsHandle");
    if(!acquire||!initialize||!complete||!delete_context||!free_credential){say("SSPI MISSING\n");ExitProcess(2);}
    const char *packages[]={"Osito-SSPI-Not-Installed-20261007","",0};
    const DWORD uses[]={0,1,2,3,0xffffffffU};
    for(DWORD p=0;p<3;p++)for(DWORD u=0;u<5;u++)for(DWORD output=0;output<4;output++){
        struct {BYTE before[16];Handle handle;U64 expiry;BYTE after[16];} guarded;
        fill(&guarded,0xa5,sizeof(guarded));
        LONG status=acquire(0,packages[p],uses[u],0,0,0,0,
            output&1?&guarded.handle:0,output&2?&guarded.expiry:0);
        check((DWORD)status==0x80090305U,"missing package status");
        check(bytes_are(&guarded,0xa5,sizeof(guarded)),"missing package preserves outputs");
    }
    const UP values[]={0,1,(UP)0xffffffffU,~(UP)0};
    for(DWORD lo=0;lo<4;lo++)for(DWORD hi=0;hi<4;hi++)for(DWORD absent=0;absent<2;absent++){
        Handle handle={values[lo],values[hi]},original=handle;
        void *input=absent?0:&handle;
        SetLastError(0x1badb002);LONG status=free_credential(input);DWORD error=GetLastError();
        check((DWORD)status==0x80090301U,"free invalid credentials");check(error==0x1badb002,"free preserves last error");
        check(handle.lower==original.lower&&handle.upper==original.upper,"free preserves handle");
        SetLastError(0x1badb002);status=delete_context(input);error=GetLastError();
        check((DWORD)status==(!absent&&values[lo]<=1?0:0x80090301U),"delete empty or invalid context");
        check(error==0x1badb002,"delete preserves last error");
        check(handle.lower==original.lower&&handle.upper==original.upper,"delete preserves handle");
        SetLastError(0x1badb002);status=complete(input,0);error=GetLastError();
        check((DWORD)status==0x80090301U,"complete invalid context");check(error==0x1badb002,"complete preserves last error");
        check(handle.lower==original.lower&&handle.upper==original.upper,"complete preserves handle");
        for(DWORD continuation=0;continuation<2;continuation++){
            struct {BYTE before[16];Handle context;BufferDesc output;DWORD attributes;U64 expiry;BYTE after[16];} guarded;
            fill(&guarded,0xa5,sizeof(guarded));
            SetLastError(0x1badb002);
            status=initialize(continuation?0:input,continuation?input:0,0,0,0,0,0,0,
                &guarded.context,&guarded.output,&guarded.attributes,&guarded.expiry);
            error=GetLastError();
            check((DWORD)status==0x80090301U,"initialize invalid handle");
            check(error==0x1badb002,"initialize preserves last error");
            check(bytes_are(&guarded,0xa5,sizeof(guarded)),"initialize preserves outputs");
            check(handle.lower==original.lower&&handle.upper==original.upper,"initialize preserves input handle");
        }
    }
    if(guest_mode()){
        const char *unavailable[]={"NTLM","Negotiate","Kerberos","Microsoft Unified Security Protocol Provider"};
        for(DWORD i=0;i<4;i++){
            Handle credential={(UP)0xa5a5a5a5,(UP)0xa5a5a5a5};U64 expiry=0xa5a5a5a5a5a5a5a5ULL;
            check((DWORD)acquire(0,unavailable[i],2,0,0,0,0,&credential,&expiry)==0x80090305U,"guest package unavailable");
            check(credential.lower==(UP)0xa5a5a5a5&&credential.upper==(UP)0xa5a5a5a5&&expiry==0xa5a5a5a5a5a5a5a5ULL,"unavailable package preserves outputs");
        }
        check((DWORD)delete_context((void *)(UP)1)==0x80090301U,"guest inaccessible context rejected");
    }
    say("SSPI DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");
    ExitProcess(failures?1:0);
}
