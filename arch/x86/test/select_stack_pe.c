/* Exercise a public GDI call near the bottom of a real reserved thread stack. */
typedef unsigned int DWORD;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
typedef struct {
    void *base, *allocation;
    DWORD allocation_protection;
    __SIZE_TYPE__ size;
    DWORD state, protection, type;
} MEMORY_INFORMATION;
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API HANDLE CALL CreateThread(void *,__SIZE_TYPE__,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
API __SIZE_TYPE__ CALL VirtualQuery(const void *,MEMORY_INFORMATION *,__SIZE_TYPE__);
API void *CALL VirtualAlloc(void *,__SIZE_TYPE__,DWORD,DWORD);
API int CALL VirtualProtect(void *,__SIZE_TYPE__,DWORD,DWORD *);
API DWORD CALL WaitForSingleObject(HANDLE,DWORD);
API int CALL GetExitCodeThread(HANDLE,DWORD *);
API int CALL CloseHandle(HANDLE);
API DWORD CALL SetErrorMode(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API void CALL ExitProcess(DWORD);
static HANDLE dc;
static HANDLE (CALL *select_object)(HANDLE,HANDLE);
static volatile DWORD leaf_gap, reached;

__declspec(noinline) static DWORD select_stack_touch(UPTR allocation,DWORD depth)
{
    volatile unsigned char padding[384];
    padding[0]=(unsigned char)depth;
    padding[383]=(unsigned char)(depth^0x5a);
    UPTR sp;
#if __SIZEOF_POINTER__ == 8
    __asm__ volatile ("movq %%rsp,%0":"=r"(sp));
#else
    __asm__ volatile ("movl %%esp,%0":"=r"(sp));
#endif
    DWORD result;
    if(sp-allocation>0x900) {
        result=select_stack_touch(allocation,depth+1);
    } else {
        leaf_gap=(DWORD)(sp-allocation);
        result=select_object(dc,0)!=0;
        reached=1;
    }
    return result+(padding[0]!=(unsigned char)depth || padding[383]!=(unsigned char)(depth^0x5a));
}

static DWORD CALL select_stack_worker(void *unused)
{
    (void)unused;
    MEMORY_INFORMATION info={0};
    if(VirtualQuery(&info,&info,sizeof(info))!=sizeof(info) || !info.allocation)return 10;
    /* Put an explicit inaccessible page halfway through the reservation so
     * the same remaining-stack budget applies on Windows and Osito. Windows
     * reserves additional room at the true stack bottom for overflow handling.
     * Commit the upper half before replacing that page's protection. */
    UPTR boundary=(UPTR)info.allocation+0x8000;
    DWORD old=0;
    if(!VirtualAlloc((void *)boundary,0x8000,0x1000,4))return 11;
    if(!VirtualProtect((void *)boundary,0x1000,1,&old))return 12;
    DWORD result=select_stack_touch(boundary+0x1000,0);
    if(!VirtualProtect((void *)boundary,0x1000,old,&old))return 13;
    return result;
}

#define SAY(text) WriteFile(output,text,sizeof(text)-1,&written,0)
#define HEX(value) do {DWORD n=(DWORD)(value);char text[8];for(int i=7;i>=0;i--){text[i]="0123456789abcdef"[n&15];n>>=4;}WriteFile(output,text,8,&written,0);} while(0)
#define CHECK(condition) do {checks++;if(!(condition)){failures++;SAY("SELECTSTACK FAIL line=");HEX(__LINE__);SAY("\n");}} while(0)
void select_stack_entry(void)
{
    HANDLE output=GetStdHandle((DWORD)-11);DWORD written=0,checks=0,failures=0;
    SetErrorMode(0x8003);
    HANDLE gdi=LoadLibraryA("gdi32.dll");CHECK(gdi!=0);
    HANDLE (CALL *create_dc)(HANDLE)=gdi?GetProcAddress(gdi,"CreateCompatibleDC"):0;
    int (CALL *delete_dc)(HANDLE)=gdi?GetProcAddress(gdi,"DeleteDC"):0;
    select_object=gdi?GetProcAddress(gdi,"SelectObject"):0;
    CHECK(create_dc && delete_dc && select_object);
    if(!failures){
        dc=create_dc(0);CHECK(dc!=0);
        if(dc){
            HANDLE thread=CreateThread(0,65536,select_stack_worker,0,0x10000,0);CHECK(thread!=0);
            if(thread){
                DWORD code=259;
                CHECK(WaitForSingleObject(thread,20000)==0);
                CHECK(GetExitCodeThread(thread,&code) && code==0);
                CHECK(reached==1 && leaf_gap>0x600 && leaf_gap<=0x900);
                CHECK(CloseHandle(thread));
            }
            CHECK(delete_dc(dc));
        }
    }
    SAY("SELECTSTACK gap=");HEX(leaf_gap);SAY(" reached=");HEX(reached);SAY("\n");
    SAY("SELECTSTACK DONE checks=");HEX(checks);SAY(" failures=");HEX(failures);SAY("\n");
    ExitProcess(failures?1:0);
}
