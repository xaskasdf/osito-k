/* Native/guest RPC string ownership and guest-pointer-width oracle. */
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
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
typedef struct {DWORD a;WORD b,c;unsigned char d[8];} UUID;
typedef DWORD (CALL *CONVERT)(const UUID *,void *);
typedef DWORD (CALL *RELEASE)(void *);
static unsigned failures;
#define SAY(s) do {DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do {char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
static void rpc_check(int ok)
{if(ok)SAY("RPCUUID PASS\n");else{failures++;SAY("RPCUUID FAIL\n");}}
static int rpc_equal_a(const unsigned char *s,const char *expected)
{if(!s)return 0;for(unsigned i=0;i<37;i++)if(s[i]!=(unsigned char)expected[i])return 0;return 1;}
static int rpc_equal_w(const WORD *s,const char *expected)
{if(!s)return 0;for(unsigned i=0;i<37;i++)if(s[i]!=(unsigned char)expected[i])return 0;return 1;}
void rpc_uuid_entry(void)
{
    void *module=LoadLibraryA("rpcrt4.dll");
    CONVERT convert[2]={GetProcAddress(module,"UuidToStringA"),GetProcAddress(module,"UuidToStringW")};
    RELEASE release[2]={GetProcAddress(module,"RpcStringFreeA"),GetProcAddress(module,"RpcStringFreeW")};
    rpc_check(module!=0);for(unsigned i=0;i<2;i++){rpc_check(convert[i]!=0);rpc_check(release[i]!=0);}
    if(failures)ExitProcess(1);
    static const UUID values[]={ {0,0,0,{0}}, {0x01234567,0x89ab,0xcdef,{1,0x23,0x45,0x67,0x89,0xab,0xcd,0xef}},
        {0xffffffff,0xffff,0xffff,{255,255,255,255,255,255,255,255}} };
    static const char *const expected[]={"00000000-0000-0000-0000-000000000000","01234567-89ab-cdef-0123-456789abcdef","ffffffff-ffff-ffff-ffff-ffffffffffff","00000000-0000-0000-0000-000000000000"};
    void *warmup=0;rpc_check(convert[0](&values[0],&warmup)==0);rpc_check(release[0](&warmup)==0);
    for(unsigned wide=0;wide<2;wide++){
        for(unsigned i=0;i<3;i++){
            struct {UP before;void *value;UP after;} output={0x12345678,(void *)(UP)0x1bad,0x87654321};
            SetLastError(0x12345678);DWORD status=convert[wide](&values[i],&output.value),error=GetLastError();
            rpc_check(status==0);rpc_check(output.before==0x12345678&&output.after==0x87654321);
            int equal=status==0&&(wide?rpc_equal_w(output.value,expected[i]):rpc_equal_a(output.value,expected[i]));rpc_check(equal);
            SAY("RPCUUID ROW ");HEX(wide);HEX(i);HEX(status);HEX(error);HEX(equal);
            DWORD freed=release[wide](&output.value),free_error=GetLastError();
            HEX(freed);HEX(free_error);HEX(output.value==0);SAY("\n");
            rpc_check(freed==0&&output.value==0);rpc_check(output.before==0x12345678&&output.after==0x87654321);
            rpc_check(release[wide](&output.value)==0&&output.value==0);
        }
        void *first=0,*second=0;
        rpc_check(convert[wide](&values[1],&first)==0&&convert[wide](&values[2],&second)==0&&first!=second);
        rpc_check(release[wide](&first)==0&&first==0);
        rpc_check(wide?rpc_equal_w(second,expected[2]):rpc_equal_a(second,expected[2]));
        rpc_check(release[wide](&second)==0&&second==0);
        int repeated=1;
        for(unsigned i=0;i<128;i++){
            void *p=0;DWORD status=convert[wide](&values[1],&p);
            repeated &= status==0&&(wide?rpc_equal_w(p,expected[1]):rpc_equal_a(p,expected[1]));
            repeated &= release[wide](&p)==0&&p==0;
        }
        rpc_check(repeated);
    }
    if(!failures)SAY("RPCUUID DONE\n");ExitProcess(failures?1:0);
}
