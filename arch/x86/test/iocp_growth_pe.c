/* Concurrent public-API port growth and reuse, shared by Windows and OsitoK. */
typedef unsigned int DWORD;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API HANDLE CALL CreateIoCompletionPort(HANDLE,HANDLE,UPTR,DWORD);
API int CALL PostQueuedCompletionStatus(HANDLE,DWORD,UPTR,void *);
API int CALL GetQueuedCompletionStatus(HANDLE,DWORD *,UPTR *,void **,DWORD);
API HANDLE CALL CreateEventA(void *,int,int,const char *);
API int CALL SetEvent(HANDLE);
API int CALL ResetEvent(HANDLE);
API HANDLE CALL CreateThread(void *,__SIZE_TYPE__,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
API DWORD CALL WaitForSingleObject(HANDLE,DWORD);
API int CALL GetExitCodeThread(HANDLE,DWORD *);
API DWORD CALL GetTickCount(void);
API void CALL Sleep(DWORD);
API int CALL CloseHandle(HANDLE);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API void CALL ExitProcess(DWORD);
static HANDLE release_workers;
static volatile DWORD ready;

static DWORD CALL iocp_growth_worker(void *argument)
{
    UPTR worker=(UPTR)argument;
    HANDLE ports[32]={0};DWORD failures=0;
    for(unsigned i=0;i<32;i++){
        ports[i]=CreateIoCompletionPort((HANDLE)(UPTR)-1,0,0,1);
        if(!ports[i]){failures++;continue;}
        if(!PostQueuedCompletionStatus(ports[i],i+1,worker+1,(void *)(UPTR)(0x12340000+i*16)))failures++;
    }
    __atomic_add_fetch(&ready,1,__ATOMIC_RELEASE);
    if(WaitForSingleObject(release_workers,15000)!=0)failures++;
    for(unsigned i=0;i<32;i++)if(ports[i]){
        DWORD bytes=0;UPTR key=0;void *overlapped=0;
        if(!GetQueuedCompletionStatus(ports[i],&bytes,&key,&overlapped,1000) ||
           bytes!=i+1 || key!=worker+1 || overlapped!=(void *)(UPTR)(0x12340000+i*16))failures++;
        if(!CloseHandle(ports[i]))failures++;
    }
    return failures;
}

#define SAY(text) WriteFile(output,text,sizeof(text)-1,&written,0)
#define HEX(value) do {DWORD n=(DWORD)(value);char text[8];for(int j=7;j>=0;j--){text[j]="0123456789abcdef"[n&15];n>>=4;}WriteFile(output,text,8,&written,0);} while(0)
#define CHECK(condition) do {checks++;if(!(condition)){failures++;SAY("IOCPGROW FAIL line=");HEX(__LINE__);SAY("\n");}} while(0)
void iocp_growth_entry(void)
{
    HANDLE output=GetStdHandle((DWORD)-11);DWORD written=0,checks=0,failures=0;
    release_workers=CreateEventA(0,1,0,0);CHECK(release_workers!=0);
    for(unsigned round=0;round<4 && release_workers;round++){
        CHECK(ResetEvent(release_workers));
        __atomic_store_n(&ready,0,__ATOMIC_RELEASE);
        HANDLE threads[4]={0};DWORD started=0;
        for(unsigned i=0;i<4;i++){
            threads[i]=CreateThread(0,0,iocp_growth_worker,(void *)(UPTR)i,0,0);
            CHECK(threads[i]!=0);if(threads[i])started++;
        }
        DWORD begin=GetTickCount();
        while(__atomic_load_n(&ready,__ATOMIC_ACQUIRE)<started && GetTickCount()-begin<10000)Sleep(1);
        CHECK(__atomic_load_n(&ready,__ATOMIC_ACQUIRE)==started);
        CHECK(SetEvent(release_workers));
        for(unsigned i=0;i<4;i++)if(threads[i]){
            DWORD code=259;
            CHECK(WaitForSingleObject(threads[i],20000)==0);
            CHECK(GetExitCodeThread(threads[i],&code)&&code==0);
            CHECK(CloseHandle(threads[i]));
        }
        SAY("IOCPGROW ROUND=");HEX(round+1);SAY("\n");
    }
    if(release_workers)CHECK(CloseHandle(release_workers));
    SAY("IOCPGROW DONE checks=");HEX(checks);SAY(" failures=");HEX(failures);SAY("\n");
    ExitProcess(failures?1:0);
}
