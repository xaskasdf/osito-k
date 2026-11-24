/* Public certificate fixtures and native-policy oracle; no trust-store writes. */
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
typedef struct {DWORD error,info;} TRUST;
typedef struct {DWORD size;void *cert;TRUST trust;void *revocation,*issuance,*usage,*text;} ELEMENT;
typedef struct {DWORD size;TRUST trust;DWORD count;ELEMENT **elements;void *list;DWORD fresh,seconds;} SIMPLE;
typedef struct {DWORD size;TRUST trust;DWORD count;SIMPLE **chains;DWORD lower;void *lower_chains;DWORD fresh,seconds,flags;unsigned char id[16];} CHAIN;
typedef struct {DWORD size,flags;void *extra;} PARA;
typedef struct {DWORD size,error;int chain,element;void *extra;} STATUS;
_Static_assert(sizeof(CHAIN)==(sizeof(UP)==4?56:72),"chain ABI");
_Static_assert(sizeof(ELEMENT)==(sizeof(UP)==4?32:56),"element ABI");
#include "cert_policy_fixtures.h"
#include "cert_policy_expected.h"
#define SAY(s) do{DWORD w;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&w,0);}while(0)
#define HEX(v) do{char h[9];DWORD n=(DWORD)(v),w;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[n&15];n>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&w,0);}while(0)
void cert_policy_probe_entry(void)
{
    DWORD failures=0;
    void *module=LoadLibraryA("crypt32.dll");
    typedef void *(CALL *createfn)(DWORD,const void *,DWORD);
    typedef int (CALL *freefn)(void *);
    typedef int (CALL *policyfn)(const char *,const CHAIN *,const PARA *,STATUS *);
    createfn create=(createfn)GetProcAddress(module,"CertCreateCertificateContext");
    freefn release=(freefn)GetProcAddress(module,"CertFreeCertificateContext");
    policyfn policy=(policyfn)GetProcAddress(module,"CertVerifyCertificateChainPolicy");
    if(!create||!release||!policy){SAY("POLICY FAIL exports\n");ExitProcess(1);}
    DWORD flags[]={0,0x10000,0x20000,0x30000,0x40000};
    for(unsigned i=0;i<sizeof(fixtures)/sizeof(fixtures[0]);i++) {
        void *cert=create(1,fixtures[i].data,fixtures[i].length);
        if(!cert){SAY("POLICY FAIL certificate\n");ExitProcess(1);}
        ELEMENT element={sizeof(ELEMENT),cert,{0,0},0,0,0,0};ELEMENT *ep=&element;
        SIMPLE simple={sizeof(SIMPLE),{0,0},1,&ep,0,0,0};SIMPLE *sp=&simple;
        CHAIN chain={sizeof(CHAIN),{0,0},1,&sp,0,0,0,0,0,{0}};
        for(unsigned f=0;f<sizeof(flags)/sizeof(flags[0]);f++) {
            PARA para={sizeof(PARA),flags[f],0};STATUS status={sizeof(STATUS),0xcccccccc,0x11111111,0x22222222,0};
            int ok=policy((const char *)7,&chain,&para,&status);
            SAY("POLICY ROW ");HEX(i);HEX(flags[f]);HEX(ok);HEX(status.error);HEX(status.chain);HEX(status.element);SAY("\n");
            if(ok&&status.error==expected_errors[i][f]&&status.chain==0&&status.element==0)SAY("POLICY PASS native-root-policy\n");
            else{SAY("POLICY FAIL native-root-policy\n");failures++;}
        }
        chain.trust.error=simple.trust.error=element.trust.error=0xffffffffU;
        STATUS status={sizeof(STATUS),0xcccccccc,-1,-1,0};
        if(policy((const char *)7,&chain,0,&status)&&status.error==expected_errors[i][0])SAY("POLICY PASS root-policy-independent-of-chain-status\n");
        else{SAY("POLICY FAIL root-policy-independent-of-chain-status\n");failures++;}
        release(cert);
    }
    if(!failures)SAY("POLICY DONE\n");ExitProcess(failures?1:0);
}
