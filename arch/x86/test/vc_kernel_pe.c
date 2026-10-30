typedef unsigned int UINT;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API void *CALL GetStdHandle(UINT);
API int CALL WriteFile(void *, const void *, UINT, UINT *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API UINT CALL GetLastError(void);
API void CALL SetLastError(UINT);
void *memset(void *p,int c,UPTR n){unsigned char *s=p;for(UPTR i=0;i<n;i++)s[i]=(unsigned char)c;return p;}
static unsigned failures;
#define SAY(s) do{UINT n;WriteFile(GetStdHandle((UINT)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("VCKERNEL PASS " name "\n");else{SAY("VCKERNEL FAIL " name "\n");failures++;}}while(0)
#define INVALID ((void *)(UPTR)-1)
#define FN(name,type) type name=(type)GetProcAddress(kernel,#name)
typedef void (CALL *init_fn)(void *);
typedef WORD (CALL *depth_fn)(void *);
typedef void *(CALL *push_fn)(void *,void *);
typedef void *(CALL *pop_fn)(void *);
typedef int (CALL *numa_fn)(UINT *);
typedef struct {UINT attr,times[6],high,low,reserved[2];char name[260],alternate[14];} FindA;
typedef struct {UINT attr,times[6],high,low,reserved[2];WORD name[260],alternate[14];} FindW;
typedef void *(CALL *find_fn)(const void *,UINT,void *,UINT,void *,UINT);
typedef int (CALL *next_fn)(void *,void *);
typedef int (CALL *close_fn)(void *);
typedef void *(CALL *create_fn)(const char *,UINT,UINT,void *,UINT,UINT,void *);
typedef int (CALL *delete_fn)(const char *);
typedef int (CALL *mkdir_fn)(const char *,void *);
typedef void *(CALL *event_fn)(void *,int,int,const char *);
typedef void *(CALL *open_fn)(UINT,int,const char *);
typedef void *(CALL *sem_fn)(void *,int,int,const char *);
typedef void *(CALL *mutex_fn)(void *,int,const char *);
typedef UINT (CALL *wait_fn)(void *,UINT);
typedef UINT (CALL *signal_fn)(void *,void *,UINT,int);
typedef UINT (CALL *threadproc_fn)(void *);
typedef void *(CALL *thread_fn)(void *,UPTR,threadproc_fn,void *,UINT,UINT *);
static wait_fn wait_one;
static close_fn set_event;
static void *gate,*finished;
static UINT CALL signal_worker(void *ignored)
{
    (void)ignored;
    if(wait_one(gate,5000)!=0)return 1;
    return set_event(finished)?0:2;
}
static int equals(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void wide(WORD *out,const char *in){while(*in)*out++=(unsigned char)*in++;*out=0;}

void CALL vc_kernel_entry(void)
{
    void *kernel=LoadLibraryA("kernel32.dll");
    FN(InitializeSListHead,init_fn);FN(QueryDepthSList,depth_fn);
    FN(InterlockedPushEntrySList,push_fn);FN(InterlockedPopEntrySList,pop_fn);
    FN(InterlockedFlushSList,pop_fn);FN(GetNumaHighestNodeNumber,numa_fn);
    FN(FindFirstFileExA,find_fn);FN(FindFirstFileExW,find_fn);
    FN(FindNextFileA,next_fn);FN(FindNextFileW,next_fn);FN(FindClose,close_fn);
    FN(CreateFileA,create_fn);FN(DeleteFileA,delete_fn);FN(CloseHandle,close_fn);
    FN(CreateDirectoryA,mkdir_fn);FN(RemoveDirectoryA,delete_fn);
    FN(CreateEventA,event_fn);FN(OpenEventA,open_fn);FN(SetEvent,close_fn);
    FN(CreateSemaphoreA,sem_fn);FN(CreateMutexA,mutex_fn);FN(ReleaseMutex,close_fn);
    FN(WaitForSingleObject,wait_fn);FN(SignalObjectAndWait,signal_fn);FN(CreateThread,thread_fn);
    CHECK(InitializeSListHead&&QueryDepthSList&&InterlockedPushEntrySList&&InterlockedPopEntrySList&&InterlockedFlushSList&&GetNumaHighestNodeNumber&&FindFirstFileExA&&FindFirstFileExW&&FindNextFileA&&FindNextFileW&&FindClose&&CreateFileA&&DeleteFileA&&CloseHandle&&CreateDirectoryA&&RemoveDirectoryA&&CreateEventA&&OpenEventA&&SetEvent&&CreateSemaphoreA&&CreateMutexA&&ReleaseMutex&&WaitForSingleObject&&SignalObjectAndWait&&CreateThread,"exports");
    if(failures)ExitProcess(1);
    struct __declspec(align(16)){unsigned char head[sizeof(void *)==8?16:8];volatile UINT guard;} list;
    struct __declspec(align(16)){void *next;UPTR padding;} entries[3];
    memset(&list,0xA5,sizeof(list));list.guard=0x13572468;
    InitializeSListHead(list.head);
    CHECK(QueryDepthSList(list.head)==0&&list.guard==0x13572468,"slist-initialize-layout");
    CHECK(InterlockedPushEntrySList(list.head,&entries[0])==0&&QueryDepthSList(list.head)==1,"slist-first-push");
    CHECK(InterlockedPushEntrySList(list.head,&entries[1])==&entries[0]&&QueryDepthSList(list.head)==2&&entries[1].next==&entries[0],"slist-second-push");
    CHECK(InterlockedPushEntrySList(list.head,&entries[2])==&entries[1]&&QueryDepthSList(list.head)==3,"slist-third-push");
    CHECK(InterlockedPopEntrySList(list.head)==&entries[2]&&QueryDepthSList(list.head)==2,"slist-pop-depth");
    CHECK(InterlockedFlushSList(list.head)==&entries[1]&&QueryDepthSList(list.head)==0&&list.guard==0x13572468,"slist-flush-depth");
    CHECK(!InterlockedPopEntrySList(list.head)&&QueryDepthSList(list.head)==0,"slist-empty-pop");
    struct{UINT node;volatile UINT guard;} numa={~0U,0x24681357};
    CHECK(GetNumaHighestNodeNumber(&numa.node)&&numa.node<65536&&numa.guard==0x24681357,"numa-output-width");

    const char *names[2]={"vck-first.dat","vck-second.dat"};
    int created=1;
    for(UINT i=0;i<2;i++){void *h=CreateFileA(names[i],0x40000000,7,0,2,0,0);created=created&&h!=INVALID;if(h!=INVALID){UINT n;created=created&&WriteFile(h,"abc",3,&n,0)&&n==3;CloseHandle(h);}}
    created=created&&(CreateDirectoryA("vck-directory",0)||GetLastError()==183);
    CHECK(created,"find-fixtures-created");
    struct{FindA data;volatile UINT guard;} fa;memset(&fa,0xA5,sizeof(fa));fa.guard=0xabcdef01;
    void *search=FindFirstFileExA("vck-*.dat",1,&fa.data,0,0,2);
    UINT count=0;int valid=search!=INVALID;
    if(valid){do{count++;valid=valid&&fa.data.low==3&&fa.data.high==0&&fa.data.alternate[0]==0&&(equals(fa.data.name,names[0])||equals(fa.data.name,names[1]));}while(count<8&&FindNextFileA(search,&fa.data));FindClose(search);}
    CHECK(valid&&count==2&&fa.guard==0xabcdef01,"find-basic-and-next-ansi");
    WORD pattern[40];wide(pattern,"vck-*.dat");
    struct{FindW data;volatile UINT guard;} fw;memset(&fw,0xA5,sizeof(fw));fw.guard=0x1234abcd;
    search=FindFirstFileExW(pattern,1,&fw.data,0,0,2);count=0;valid=search!=INVALID;
    if(valid){do{count++;valid=valid&&fw.data.low==3&&fw.data.high==0&&fw.data.name[0]=='v'&&fw.data.alternate[0]==0;}while(count<8&&FindNextFileW(search,&fw.data));FindClose(search);}
    CHECK(valid&&count==2&&fw.guard==0x1234abcd,"find-basic-and-next-wide");
    search=FindFirstFileExA("vck-directory",0,&fa.data,1,0,0);
    CHECK(search!=INVALID&&(fa.data.attr&0x10),"find-directory-search");if(search!=INVALID)FindClose(search);
    search=FindFirstFileExA("vck-first.dat",0,&fa.data,0,0,4);
    CHECK(search!=INVALID&&fa.data.low==3,"find-on-disk");if(search!=INVALID)FindClose(search);
    SetLastError(0);search=FindFirstFileExA("vck-*.dat",2,&fa.data,0,0,0);
    CHECK(search==INVALID&&GetLastError()==87,"find-reject-info-level");
    SetLastError(0);search=FindFirstFileExA("vck-*.dat",0,&fa.data,3,0,0);
    CHECK(search==INVALID&&GetLastError()==87,"find-reject-search-op");
    SetLastError(0);search=FindFirstFileExA("vck-*.dat",0,&fa.data,0,0,0x80000000);
    CHECK(search==INVALID&&GetLastError()==87,"find-reject-flags");
    SetLastError(0);search=FindFirstFileExA("vck-absent.dat",0,&fa.data,0,0,0);
    CHECK(search==INVALID&&GetLastError()==2,"find-missing-file");

    wait_one=WaitForSingleObject;set_event=SetEvent;
    gate=CreateEventA(0,0,0,"VCKernelSignalGate");finished=CreateEventA(0,1,0,0);
    void *worker=CreateThread(0,0,signal_worker,0,0,0);
    CHECK(gate&&finished&&worker,"signal-worker-created");
    CHECK(SignalObjectAndWait(gate,finished,5000,1)==0&&WaitForSingleObject(worker,1000)==0,"signal-event-cross-thread");
    CHECK(WaitForSingleObject(gate,0)==258,"signal-auto-reset-consumed");
    CHECK(SignalObjectAndWait(gate,gate,0,0)==0&&WaitForSingleObject(gate,0)==258,"signal-and-wait-same-event");
    void *semaphore=CreateSemaphoreA(0,0,1,0);
    CHECK(semaphore&&SignalObjectAndWait(semaphore,finished,0,0)==0&&WaitForSingleObject(semaphore,0)==0&&WaitForSingleObject(semaphore,0)==258,"signal-semaphore-increment");
    SetEvent(gate);CHECK(SignalObjectAndWait(semaphore,gate,0,0)==0,"signal-semaphore-fill");
    SetLastError(0);UINT result=SignalObjectAndWait(semaphore,finished,0,0);
    CHECK(result==~0U&&GetLastError()==298,"signal-semaphore-limit");
    void *mutex=CreateMutexA(0,1,0);
    CHECK(mutex&&SignalObjectAndWait(mutex,finished,0,0)==0,"signal-mutex-release");
    SetLastError(0);result=SignalObjectAndWait(mutex,finished,0,0);
    CHECK(result==~0U&&GetLastError()==288,"signal-mutex-requires-owner");
    CHECK(WaitForSingleObject(mutex,0)==0&&WaitForSingleObject(mutex,0)==0&&SignalObjectAndWait(mutex,finished,0,0)==0&&ReleaseMutex(mutex),"signal-mutex-recursion");
    void *read_only=OpenEventA(0x100000,0,"VCKernelSignalGate");
    SetLastError(0);result=SignalObjectAndWait(read_only,finished,0,0);
    CHECK(read_only&&result==~0U&&GetLastError()==5,"signal-access-check");
    SetLastError(0);result=SignalObjectAndWait((void *)(UPTR)0x7bad,finished,0,0);
    CHECK(result==~0U&&GetLastError()==6,"signal-invalid-handle");
    result=SignalObjectAndWait(finished,gate,20,0);
    CHECK(result==258&&WaitForSingleObject(finished,0)==0,"signal-timeout-keeps-signal");
    CloseHandle(read_only);CloseHandle(mutex);CloseHandle(semaphore);CloseHandle(worker);CloseHandle(gate);CloseHandle(finished);
    for(UINT i=0;i<2;i++)DeleteFileA(names[i]);RemoveDirectoryA("vck-directory");
    if(!failures)SAY("VCKERNEL DONE\n");
    ExitProcess(failures?1:0);
}
