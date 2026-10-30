/* Native/Osito contract: ShellExecuteEx launches an executable and returns
 * a waitable process handle only when SEE_MASK_NOCLOSEPROCESS is requested. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
#define SAY(text) do { DWORD n; WriteFile(GetStdHandle((DWORD)-11),text,sizeof(text)-1,&n,0); } while (0)
#define CHECK(test,label) do { if (test) SAY("SHELLEXEC PASS " label "\n"); \
    else { SAY("SHELLEXEC FAIL " label "\n"); failures++; } } while (0)
typedef struct {
    DWORD size, mask;
    void *window;
    const void *verb, *file, *parameters, *directory;
    int show;
    void *instance, *id_list;
    const void *class_name;
    void *class_key;
    DWORD hotkey;
    void *icon, *process;
} EXEC_INFO;
_Static_assert(sizeof(EXEC_INFO)==(sizeof(void *)==4 ? 60 : 112),"SHELLEXECUTEINFO layout");
void *memcpy(void *destination, const void *source, UPTR size)
{
    unsigned char *out=destination;
    const unsigned char *in=source;
    for (UPTR i=0;i<size;i++) out[i]=in[i];
    return destination;
}
void *memset(void *destination, int value, UPTR size)
{
    unsigned char *out=destination;
    for (UPTR i=0;i<size;i++) out[i]=(unsigned char)value;
    return destination;
}
void shellexec_probe_entry(void)
{
    DWORD failures=0;
    void *kernel=LoadLibraryA("kernel32.dll");
    typedef const char *(CALL *command_fn)(void);
    command_fn command=(command_fn)GetProcAddress(kernel,"GetCommandLineA");
    const char *cmd=command();
    for (DWORD i=0;cmd[i];i++) {
        if (cmd[i]=='@' && cmd[i+1]=='c' && cmd[i+2]=='h') ExitProcess(37);
    }
    void *shell=LoadLibraryA("shell32.dll");
    typedef int (CALL *execute_fn)(EXEC_INFO *);
    execute_fn execute_a=(execute_fn)GetProcAddress(shell,"ShellExecuteExA");
    execute_fn execute_w=(execute_fn)GetProcAddress(shell,"ShellExecuteExW");
    CHECK(execute_a && execute_w,"exports");
    if (failures) ExitProcess(1);
    typedef DWORD (CALL *filename_fn)(void *,char *,DWORD);
    typedef DWORD (CALL *wait_fn)(void *,DWORD);
    typedef int (CALL *exit_fn)(void *,DWORD *);
    typedef int (CALL *close_fn)(void *);
    typedef void (CALL *sleep_fn)(DWORD);
    filename_fn filename=(filename_fn)GetProcAddress(kernel,"GetModuleFileNameA");
    wait_fn wait=(wait_fn)GetProcAddress(kernel,"WaitForSingleObject");
    exit_fn exit_code=(exit_fn)GetProcAddress(kernel,"GetExitCodeProcess");
    close_fn close=(close_fn)GetProcAddress(kernel,"CloseHandle");
    sleep_fn sleep=(sleep_fn)GetProcAddress(kernel,"Sleep");
    CHECK(filename && wait && exit_code && close && sleep,"support-exports");
    if (failures) ExitProcess(1);
    char path[1024]; WCHAR wide[1024];
    DWORD length=filename(0,path,sizeof(path));
    CHECK(length && length<1024,"self-path");
    if (failures) ExitProcess(1);
    for (DWORD i=0;i<=length;i++) wide[i]=(unsigned char)path[i];
    const WCHAR child_w[]={'@','c','h',0};
    const WCHAR missing_w[]={'s','h','e','l','l','e','x','e','c','-','a','b','s','e','n','t','.','e','x','e',0};
    const WCHAR verb_w[]={'n','o','s','u','c','h','v','e','r','b',0};
    struct { UPTR before; EXEC_INFO info; UPTR after; } guarded;
    guarded.before=0x13579bdf;guarded.after=0x2468ace0;
    EXEC_INFO *info=&guarded.info;
    for (DWORD w=0;w<2;w++) {
        execute_fn execute=w ? execute_w : execute_a;
        memset(info,0,sizeof(*info));info->size=sizeof(*info)-1;info->mask=0x8500;
        CHECK(!execute(info),"invalid-size-rejected");
        memset(info,0,sizeof(*info));info->size=sizeof(*info);info->mask=0x8540;
        info->file=w ? (const void *)missing_w : "shellexec-absent.exe";
        SetLastError(0);
        CHECK(!execute(info) && GetLastError()!=0 && !info->process &&
              (UPTR)info->instance<=32,"missing-file-rejected");
        info->file=w ? (const void *)wide : (const void *)path;
        info->verb=w ? (const void *)verb_w : "nosuchverb";
        CHECK(!execute(info) && !info->process,"unsupported-verb-rejected");
        info->verb=0;info->parameters=w ? (const void *)child_w : "@ch";
        CHECK(execute(info) && (UPTR)info->instance>32 && info->process,"launch-handle");
        if (info->process) {
            DWORD code=0;
            CHECK(wait(info->process,10000)==0,"child-signaled");
            CHECK(exit_code(info->process,&code) && code==37,"child-exit-code");
            CHECK(close(info->process),"handle-close");
        }
        info->mask=0x8500;info->process=(void *)(UPTR)0x1234;
        CHECK(execute(info) && (UPTR)info->instance>32 && !info->process,"launch-without-retained-handle");
        sleep(100);
        CHECK(guarded.before==0x13579bdf && guarded.after==0x2468ace0,"structure-canaries");
    }
    SAY("SHELLEXEC DONE\n");
    ExitProcess(failures ? 1 : 0);
}
