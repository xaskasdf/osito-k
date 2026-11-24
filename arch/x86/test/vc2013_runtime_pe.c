typedef unsigned int UINT;
int _fltused;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API void *CALL GetStdHandle(UINT);
API int CALL WriteFile(void *,const void *,UINT,UINT *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
static unsigned failures;
#define SAY(s) do{UINT n;WriteFile(GetStdHandle((UINT)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("VCRUN PASS " name "\n");else{SAY("VCRUN FAIL " name "\n");failures++;}}while(0)
typedef void *(*malloc_fn)(UINT);
typedef void *(*calloc_fn)(UINT,UINT);
typedef void (*free_fn)(void *);
typedef UINT (*length_fn)(const char *);
typedef long (*parse_fn)(const char *,char **,int);
typedef double (*math_fn)(double);
typedef int *(*errno_fn)(void);
typedef unsigned (__attribute__((stdcall)) *thread_fn)(void *);
typedef __UINTPTR_TYPE__ (*begin_fn)(void *,UINT,thread_fn,void *,UINT,UINT *);
typedef UINT (CALL *wait_fn)(void *,UINT);
typedef int (CALL *close_fn)(void *);
typedef int (CALL *exitcode_fn)(void *,UINT *);
static errno_fn thread_error;
static malloc_fn thread_allocate;
static free_fn thread_release;
static volatile int *child_errno;
static volatile int child_memory_ok;

static unsigned CALL crt_thread(void *context)
{
    int *error=thread_error();child_errno=error;
    if(error)*error=99;
    unsigned char *memory=thread_allocate(37);
    if(memory){
        for(UINT i=0;i<37;i++)memory[i]=(unsigned char)(i+17);
        int valid=1;for(UINT i=0;i<37;i++)valid=valid&&memory[i]==(unsigned char)(i+17);
        child_memory_ok=valid&&context==(void *)(__UINTPTR_TYPE__)0x12345678;
        thread_release(memory);
    }
    return 176;
}

void CALL vc2013_runtime_entry(void)
{
    SAY("VCRUN START\n");
    void *crt=LoadLibraryA("msvcr120.dll");CHECK(crt!=0,"load-original-msvcr120");
    if(!crt)ExitProcess(1);
    malloc_fn allocate=(malloc_fn)GetProcAddress(crt,"malloc");
    calloc_fn zeros=(calloc_fn)GetProcAddress(crt,"calloc");
    free_fn release=(free_fn)GetProcAddress(crt,"free");
    length_fn length=(length_fn)GetProcAddress(crt,"strlen");
    parse_fn parse=(parse_fn)GetProcAddress(crt,"strtol");
    math_fn root=(math_fn)GetProcAddress(crt,"sqrt");
    math_fn acosine=(math_fn)GetProcAddress(crt,"acos");
    errno_fn error=(errno_fn)GetProcAddress(crt,"_errno");
    CHECK(allocate&&zeros&&release&&length&&parse&&root&&acosine&&error,"crt-exports");
    if(failures)ExitProcess(1);
    unsigned char *p=allocate(257);CHECK(p!=0,"heap-allocation");
    if(p){for(UINT i=0;i<257;i++)p[i]=(unsigned char)i;int same=1;for(UINT i=0;i<257;i++)same=same&&p[i]==(unsigned char)i;CHECK(same,"heap-write-read");release(p);}
    p=zeros(9,7);int zero=p!=0;if(p){for(UINT i=0;i<63;i++)zero=zero&&p[i]==0;release(p);}CHECK(zero,"zero-allocation");
    CHECK(length("Braid launcher")==14,"native-string-code");
    char *end=0;const char text[]=" -123end";long n=parse(text,&end,10);CHECK(n==-123&&end==text+5,"native-number-parse");
    volatile double a=81.0,b=0.0;double x=root(a),y=acosine(b);
    CHECK(x==9.0&&y>1.5707963267&&y<1.5707963269,"native-math-x87");
    int *err=error();CHECK(err!=0,"thread-errno");if(err)*err=71;
    CHECK(error()&&*error()==71,"errno-stability");
    void *kernel=LoadLibraryA("kernel32.dll");
    begin_fn begin=(begin_fn)GetProcAddress(crt,"_beginthreadex");
    wait_fn wait=(wait_fn)GetProcAddress(kernel,"WaitForSingleObject");
    close_fn close=(close_fn)GetProcAddress(kernel,"CloseHandle");
    exitcode_fn exitcode=(exitcode_fn)GetProcAddress(kernel,"GetExitCodeThread");
    CHECK(begin&&wait&&close&&exitcode,"crt-thread-exports");
    if(begin&&wait&&close&&exitcode){
        thread_error=error;thread_allocate=allocate;thread_release=release;
        UINT tid=0;
        void *thread=(void *)begin(0,0,crt_thread,(void *)(__UINTPTR_TYPE__)0x12345678,0,&tid);
        CHECK(thread&&tid,"crt-thread-created");
        if(thread){
            CHECK(wait(thread,5000)==0,"crt-thread-joined");
            CHECK(child_errno&&child_errno!=err&&child_memory_ok&&error()==err&&*err==71,"crt-thread-private-state");
            UINT code=0;CHECK(exitcode(thread,&code)&&code==176,"crt-thread-return-code");
            close(thread);
        }
    }
    void *cpp=LoadLibraryA("msvcp120.dll");CHECK(cpp!=0,"load-original-msvcp120");
    CHECK(cpp&&GetProcAddress(cpp,"?_Xbad_alloc@std@@YAXXZ")&&GetProcAddress(cpp,"?_Xlength_error@std@@YAXPBD@Z"),"cpp-exports");
    if(!failures)SAY("VCRUN DONE\n");
    ExitProcess(failures?1:0);
}
