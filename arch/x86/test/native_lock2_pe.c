/* Portable ABI/error probe. The positive GPU mapping test is GPUCOPY_LOCK2
 * in native_gpu_copy_pe.c, using our installed native driver on the P1. */
#include "../win32/d3dkmt_abi.h"
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetStdHandle(OKMT_U32);
API int CALL WriteFile(void *,const void *,OKMT_U32,OKMT_U32 *,void *);
API void CALL ExitProcess(OKMT_U32);
API OKMT_U32 CALL SetErrorMode(OKMT_U32);
static unsigned checks,failures;
static void report(const char *label,unsigned value)
{
    char text[96];unsigned n=0;while(label[n]){text[n]=label[n];n++;}
    for(unsigned i=0;i<8;i++)text[n++]= "0123456789abcdef"[(value>>(28-i*4))&15];
    text[n++]='\n';OKMT_U32 written;WriteFile(GetStdHandle((OKMT_U32)-11),text,n,&written,0);
}
#define CHECK(x) do {checks++;if(!(x)){failures++;report("LOCK2 FAIL line=",__LINE__);}} while(0)
void native_lock2_entry(void)
{
    SetErrorMode(0x8003);
    void *gdi=LoadLibraryA("gdi32.dll");
    OKMT_S32 (CALL *lock)(OKMT_LOCK2 *)=GetProcAddress(gdi,"D3DKMTLock2");
    OKMT_S32 (CALL *unlock)(OKMT_UNLOCK2 *)=GetProcAddress(gdi,"D3DKMTUnlock2");
    CHECK(gdi && lock && unlock);if(failures)goto done;
    CHECK(lock(0)<0);CHECK(unlock(0)<0);
    for(unsigned i=0;i<35;i++) {
        struct { OKMT_U64 before;OKMT_LOCK2 args;OKMT_U64 after; } q;
        q.before=q.after=0x1234567887654321ULL;
        q.args=(OKMT_LOCK2){i==33 ? 0xbad00001 : 0,i==34 ? 0xbad00002 : 0,
            i<32 ? 1u<<i : 0,(void *)0xfeed};
        OKMT_S32 status=lock(&q.args);CHECK(status<0);
        CHECK(q.before==0x1234567887654321ULL && q.after==0x1234567887654321ULL);
        OKMT_UNLOCK2 release={q.args.device,q.args.allocation};CHECK(unlock(&release)<0);
        CHECK(release.device==q.args.device && release.allocation==q.args.allocation);
    }
done:
    report("LOCK2 checks=",checks);report("LOCK2 failures=",failures);ExitProcess(failures ? 1 : 0);
}
