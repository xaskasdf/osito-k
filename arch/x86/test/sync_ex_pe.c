/* Compare extended sync creation and rights with the installed Windows API. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef int BOOL;
typedef int LONG;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
typedef struct { DWORD length; void *descriptor; BOOL inherit; } ATTRIBUTES;
static unsigned checks,failures;
static DWORD (CALL *error)(void), (CALL *wait_one)(HANDLE,DWORD);
static DWORD (CALL *wait_many)(DWORD,const HANDLE *,BOOL,DWORD);
static void (CALL *set_error)(DWORD);
static BOOL (CALL *close_handle)(HANDLE), (CALL *release_mutex)(HANDLE);
static BOOL (CALL *release_semaphore)(HANDLE,LONG,LONG *), (CALL *information)(HANDLE,DWORD *);
static HANDLE (CALL *mutex_a)(void *,const char *,DWORD,DWORD);
static HANDLE (CALL *mutex_w)(void *,const WORD *,DWORD,DWORD);
static HANDLE (CALL *semaphore_a)(void *,LONG,LONG,const char *,DWORD,DWORD);
static HANDLE (CALL *semaphore_w)(void *,LONG,LONG,const WORD *,DWORD,DWORD);
static HANDLE (CALL *open_mutex)(DWORD,BOOL,const char *), (CALL *open_semaphore)(DWORD,BOOL,const char *);
static HANDLE (CALL *legacy_mutex)(void *,BOOL,const char *);
static HANDLE (CALL *legacy_semaphore)(void *,LONG,LONG,const char *);
static LONG (CALL *nt_release_semaphore)(HANDLE,LONG,LONG *);
static void say(const char *s)
{
    DWORD bytes=0,written;
    while (s[bytes]) bytes++;
    WriteFile(GetStdHandle((DWORD)-11),s,bytes,&written,0);
}
static void hex(DWORD value)
{
    char bytes[10];DWORD written;
    for (unsigned i=0;i<8;i++) bytes[i]="0123456789abcdef"[(value>>(28-i*4))&15];
    bytes[8]='\n';bytes[9]=0;
    WriteFile(GetStdHandle((DWORD)-11),bytes,9,&written,0);
}
static void check(BOOL ok,const char *label)
{
    checks++;if (!ok) failures++;
    say(ok?"SYNCEX PASS ":"SYNCEX FAIL ");say(label);say("\n");
}
static void require(BOOL ok,const char *label)
{
    check(ok,label);if (!ok) ExitProcess(1);
}
static void deny_wait(HANDLE handle)
{
    set_error(0xfeed);
    DWORD result=wait_one(handle,0),last=error();
    check(result==0xffffffffu && last==5,"single wait requires SYNCHRONIZE");
    set_error(0xfeed);result=wait_many(1,&handle,0,0);last=error();
    check(result==0xffffffffu && last==5,"multiple wait requires SYNCHRONIZE");
}
void sync_ex_entry(void)
{
    HANDLE kernel=LoadLibraryA("kernel32.dll"),ntdll=LoadLibraryA("ntdll.dll");
#define LOAD(variable,name) do { variable=GetProcAddress(kernel,name); require(variable!=0,"export " name); } while(0)
    LOAD(error,"GetLastError");LOAD(set_error,"SetLastError");LOAD(wait_one,"WaitForSingleObject");
    LOAD(wait_many,"WaitForMultipleObjects");LOAD(close_handle,"CloseHandle");
    LOAD(release_mutex,"ReleaseMutex");LOAD(release_semaphore,"ReleaseSemaphore");
    LOAD(information,"GetHandleInformation");LOAD(mutex_a,"CreateMutexExA");LOAD(mutex_w,"CreateMutexExW");
    LOAD(semaphore_a,"CreateSemaphoreExA");LOAD(semaphore_w,"CreateSemaphoreExW");
    LOAD(open_mutex,"OpenMutexA");LOAD(open_semaphore,"OpenSemaphoreA");
    LOAD(legacy_mutex,"CreateMutexA");LOAD(legacy_semaphore,"CreateSemaphoreA");
#undef LOAD
    nt_release_semaphore=GetProcAddress(ntdll,"NtReleaseSemaphore");require(nt_release_semaphore!=0,"NT release export");
    const char name[]="Local\\OsitoRuntimeSyncEx";
    WORD wide[sizeof(name)];for (unsigned i=0;i<sizeof(name);i++) wide[i]=(unsigned char)name[i];
    ATTRIBUTES attributes={sizeof(attributes),0,1};
    for (unsigned wide_api=0;wide_api<2;wide_api++) {
        HANDLE mutex=wide_api?mutex_w(&attributes,wide,1,0x100000):mutex_a(&attributes,name,1,0x100000);
        require(mutex!=0,"mutex created with initial ownership");
        DWORD flags=0;
        check(information(mutex,&flags) && (flags&1),"creation inherits handle flag");
        check(wait_one(mutex,0)==0 && wait_one(mutex,0)==0,"mutex recursion");
        check(release_mutex(mutex) && release_mutex(mutex) && release_mutex(mutex),"mutex releases without modify-state bit");
        set_error(0xfeed);
        check(!release_mutex(mutex) && error()==288,"non-owner mutex error");
        HANDLE again=mutex_a(0,name,0,0x100000);DWORD last=error();
        require(again!=0 && again!=mutex && last==183,"named mutex returns existing independent handle");
        flags=1;check(information(again,&flags) && !(flags&1),"reopening controls inheritance independently");
        require(close_handle(mutex),"close original mutex");
        check(wait_one(again,0)==0 && release_mutex(again),"remaining named mutex is live");
        require(close_handle(again),"close last mutex");
        check(!open_mutex(0x100000,0,name),"last close removes mutex name");
        HANDLE semaphore=wide_api?semaphore_w(&attributes,1,2,wide,0,2):semaphore_a(&attributes,1,2,name,0,2);
        require(semaphore!=0,"semaphore created with modify-only access");
        flags=0;check(information(semaphore,&flags) && (flags&1),"semaphore inherits handle flag");
        deny_wait(semaphore);
        HANDLE waiter=semaphore_a(0,0,17,name,0,0x100000);last=error();
        require(waiter!=0 && last==183,"semaphore reopen keeps original count and maximum");
        check(wait_one(waiter,0)==0 && wait_one(waiter,0)==258,"denied wait did not consume semaphore");
        LONG previous=0x12345678;
        set_error(0xfeed);BOOL ok=release_semaphore(waiter,1,&previous);last=error();
        check(!ok && last==5 && previous==0x12345678,"wait-only semaphore cannot modify count or output");
        check(release_semaphore(semaphore,2,&previous) && previous==0,"modify-only handle signals counted semaphore");
        previous=0x12345678;set_error(0xfeed);ok=release_semaphore(semaphore,1,&previous);last=error();
        check(!ok && last==298 && previous==0x12345678,"overflow leaves semaphore and output unchanged");
        set_error(0xfeed);ok=release_semaphore(semaphore,0x7fffffff,&previous);last=error();
        check(!ok && last==298 && previous==0x12345678,"large release cannot overflow signed count");
        check(wait_one(waiter,0)==0 && wait_one(waiter,0)==0 && wait_one(waiter,0)==258,"semaphore has exact count");
        previous=0x12345678;
        check(nt_release_semaphore(waiter,1,&previous)==(LONG)0xc0000022u && previous==0x12345678,"native NT release enforces access");
        check(nt_release_semaphore(semaphore,3,&previous)==(LONG)0xc0000047u && previous==0x12345678,"native NT maximum-count status");
        check(nt_release_semaphore(semaphore,0,&previous)==(LONG)0xc000000du && previous==0x12345678,"native NT invalid release count");
        require(close_handle(semaphore) && close_handle(waiter),"close semaphore handles");
        check(!open_semaphore(2,0,name),"last close removes semaphore name");
    }
    HANDLE none=mutex_a(0,0,1,0);require(none!=0,"zero-access owned mutex");
    deny_wait(none);check(release_mutex(none),"owned mutex release does not require access mask");close_handle(none);
    none=semaphore_a(0,0,1,0,0,0);require(none!=0,"zero-access semaphore");
    deny_wait(none);set_error(0xfeed);check(!release_semaphore(none,1,0) && error()==5,"zero-access semaphore cannot signal");close_handle(none);
    const DWORD masks[]={0x80000000u,0x40000000u,0x20000000u,0x10000000u,4u,0x02000000u};
    for (unsigned i=0;i<sizeof(masks)/sizeof(*masks);i++) {
        HANDLE sem=semaphore_a(0,1,2,0,0,masks[i]);DWORD last=error();
        say("SYNCEX OBS mask=");hex(masks[i]);say("SYNCEX OBS create=");hex(sem?0:last);
        if (sem) {
            say("SYNCEX OBS wait=");hex(wait_one(sem,0));
            BOOL ok=release_semaphore(sem,1,0);last=error();say("SYNCEX OBS release=");hex(ok?0:last);close_handle(sem);
        }
    }
    for (unsigned flag=1;flag<=2;flag++) {
        HANDLE h=semaphore_a(0,0,1,0,flag,0x1f0003);DWORD last=error();
        say("SYNCEX OBS semaphore-flags=");hex(flag);say("SYNCEX OBS result=");hex(h?0:last);if(h)close_handle(h);
    }
    HANDLE bad=mutex_a(0,0,2,0x1f0001);DWORD last=error();
    say("SYNCEX OBS mutex-flags2=");hex(bad?0:last);if(bad)close_handle(bad);
    /* Legacy creators and Ex/open paths share the same actual object. */
    HANDLE original=legacy_semaphore(0,1,1,name),extended=semaphore_w(0,0,2,wide,0,0x100000);
    require(original && extended,"legacy and extended semaphore interoperate");
    check(wait_one(extended,0)==0 && wait_one(original,0)==258,"legacy and extended semaphore share state");
    close_handle(original);close_handle(extended);
    original=legacy_mutex(0,0,name);extended=mutex_w(0,wide,0,0x100000);
    require(original && extended,"legacy and extended mutex interoperate");
    check(wait_one(extended,0)==0 && release_mutex(original),"mutex ownership is per thread");
    close_handle(original);close_handle(extended);
    say("SYNCEX checks=");hex(checks);say("SYNCEX failures=");hex(failures);ExitProcess(failures?1:0);
}
