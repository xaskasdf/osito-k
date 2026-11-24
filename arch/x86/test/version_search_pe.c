#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static DWORD (WINAPI *size_fn)(DWORD,LPCSTR,DWORD *);
static BOOL (WINAPI *load_fn)(DWORD,LPCSTR,DWORD,DWORD,LPVOID);
static BOOL (WINAPI *query_fn)(LPCVOID,LPCSTR,LPVOID *,PUINT);
static char line[160],base[600],path[700];static unsigned used,checks,failures;
static unsigned char buffer[1600];
static DWORD resource_flags=3;
void *memset(void *p,int v,size_t n){unsigned char *d=p;while(n--)*d++=(unsigned char)v;return p;}
void *memcpy(void *p,const void *s,size_t n){unsigned char *d=p;const unsigned char *a=s;while(n--)*d++=*a++;return p;}
static void number(DWORD n){for(int i=28;i>=0;i-=4)line[used++]="0123456789abcdef"[(n>>i)&15];line[used++]=' ';}
static void emit(const char *s){DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&w,0);line[used-1]='\n';WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,used,&w,0);used=0;}
#define CHECK(x) do{checks++;if(!(x)){failures++;number(__LINE__);emit("VSEARCH FAIL ");}}while(0)
static void *resolve(HMODULE m,const char *s){void *p=GetProcAddress(m,s);if(!p)ExitProcess(2);return p;}
static char *join(const char *suffix){unsigned n=0;while(base[n]){path[n]=base[n];n++;}while(*suffix)path[n++]=*suffix++;path[n]=0;return path;}
static void observe(unsigned tag,const char *name)
{
    DWORD h=0x9999;SetLastError(0x1234);DWORD n=size_fn(resource_flags,name,&h),error=GetLastError();
    DWORD value=0;BOOL ok=FALSE;CHECK(h==0);
    if(n && n<=sizeof(buffer)) {
        ok=load_fn(resource_flags,name,0,n,buffer);CHECK(ok);
        if(ok){void *p=NULL;UINT len=0;CHECK(query_fn(buffer,"\\",&p,&len)&&len==52);if(p)value=((VS_FIXEDFILEINFO *)p)->dwFileVersionMS;}
    }
    number(tag);number(n);number(error);number(value);emit("VSEARCH ROW ");
}
void version_search_entry(void)
{
    HMODULE k=LoadLibraryA("kernel32.dll"),v=LoadLibraryA("version.dll");
    DWORD (WINAPI *module_name)(HMODULE,LPSTR,DWORD)=(void *)resolve(k,"GetModuleFileNameA");
    BOOL (WINAPI *set_current)(LPCSTR)=(void *)resolve(k,"SetCurrentDirectoryA");
    BOOL (WINAPI *set_directory)(LPCSTR)=(void *)resolve(k,"SetDllDirectoryA");
    BOOL (WINAPI *set_environment)(LPCSTR,LPCSTR)=(void *)resolve(k,"SetEnvironmentVariableA");
    DLL_DIRECTORY_COOKIE (WINAPI *add_directory)(LPCWSTR)=(void *)resolve(k,"AddDllDirectory");
    BOOL (WINAPI *remove_directory)(DLL_DIRECTORY_COOKIE)=(void *)resolve(k,"RemoveDllDirectory");
    BOOL (WINAPI *defaults)(DWORD)=(void *)resolve(k,"SetDefaultDllDirectories");
    HMODULE (WINAPI *module_handle)(LPCSTR)=(void *)resolve(k,"GetModuleHandleA");
    BOOL (WINAPI *free_module)(HMODULE)=(void *)resolve(k,"FreeLibrary");
    size_fn=(void *)resolve(v,"GetFileVersionInfoSizeExA");load_fn=(void *)resolve(v,"GetFileVersionInfoExA");query_fn=(void *)resolve(v,"VerQueryValueA");
    unsigned n=module_name(NULL,base,sizeof(base)),sep=0;CHECK(n&&n<sizeof(base));
    for(unsigned i=0;i<n;i++)if(base[i]=='/'||base[i]=='\\')sep=i+1;base[sep]=0;
    CHECK(set_current(join("cwd")));CHECK(set_environment("PATH",join("path")));
    CHECK(!module_handle("choice.dll"));
    observe(0,"choice.dll");observe(1,"current.dll");observe(2,"user.dll");observe(3,"onpath.dll");
    observe(4,"noext");observe(5,"plain");observe(6,"choice");observe(7,"choice.dll.");
    resource_flags=1;observe(8,"localized.dll");observe(9,"onmui.dll");resource_flags=3;
    CHECK(!module_handle("choice.dll"));
    CHECK(set_directory(join("user")));
    observe(10,"choice.dll");observe(11,"user.dll");observe(12,"current.dll");observe(13,"onpath.dll");
    CHECK(set_directory(""));observe(20,"current.dll");observe(21,"onpath.dll");
    CHECK(set_directory(NULL));observe(22,"current.dll");
    HMODULE loaded=LoadLibraryA(join("user\\choice.dll"));CHECK(loaded!=NULL);
    observe(30,"choice.dll");observe(31,join("choice.dll"));CHECK(free_module(loaded));
    observe(32,join("plain"));observe(33,join("plain."));observe(34,join("noext"));
    const char *names[]={"noext","onpath.dll","plain"};
    for(unsigned i=0;i<3;i++){
        loaded=LoadLibraryA(names[i]);CHECK((loaded!=NULL)==(i!=2));
        if(loaded){char full[700];CHECK(module_name(loaded,full,sizeof(full))!=0);observe(50+i,full);CHECK(free_module(loaded));}
    }
    loaded=LoadLibraryA(join("plain"));number(53);number(loaded!=NULL);emit("VSEARCH LOAD ");if(loaded)CHECK(free_module(loaded));
    char quoted[704];join("path");unsigned q=0;quoted[q++]='"';for(unsigned i=0;path[i];i++)quoted[q++]=path[i];quoted[q++]='"';quoted[q]=0;
    CHECK(set_environment("PATH",quoted));observe(54,"onpath.dll");
    CHECK(set_environment("PATH",";..\\absent;;..\\path;"));observe(55,"onpath.dll");
    CHECK(set_environment("PATH",join("path")));
    WCHAR user[700];join("user");for(unsigned i=0;i<=600;i++){user[i]=(BYTE)path[i];if(!path[i])break;}
    DLL_DIRECTORY_COOKIE cookie=add_directory(user);CHECK(cookie!=NULL);
    observe(40,"user.dll");CHECK(defaults(0x1400));
    observe(41,"user.dll");observe(42,"choice.dll");observe(43,"current.dll");observe(44,"onpath.dll");
    CHECK(remove_directory(cookie));observe(45,"user.dll");
    number(checks);number(failures);emit("VSEARCH RESULT ");ExitProcess(failures?1:0);
}
