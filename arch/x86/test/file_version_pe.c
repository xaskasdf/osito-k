#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static unsigned checks,failures,current,used;
static char row[128],path[600];static WCHAR wide[600];
static unsigned char buffer[1200],legacy_buffer[1200];
static DWORD (WINAPI *size_a)(DWORD,LPCSTR,DWORD *);
static DWORD (WINAPI *size_w)(DWORD,LPCWSTR,DWORD *);
static BOOL (WINAPI *load_a)(DWORD,LPCSTR,DWORD,DWORD,LPVOID);
static BOOL (WINAPI *load_w)(DWORD,LPCWSTR,DWORD,DWORD,LPVOID);
static BOOL (WINAPI *query_a)(LPCVOID,LPCSTR,LPVOID *,PUINT);
static BOOL (WINAPI *query_w)(LPCVOID,LPCWSTR,LPVOID *,PUINT);
void *memset(void *p,int v,size_t n){unsigned char *d=p;while(n--)*d++=(unsigned char)v;return p;}
void *memcpy(void *p,const void *s,size_t n){unsigned char *d=p;const unsigned char *a=s;while(n--)*d++=*a++;return p;}
static void number(DWORD n){for(int i=28;i>=0;i-=4)row[used++]="0123456789abcdef"[(n>>i)&15];row[used++]=' ';}
static void emit(const char *s){DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&w,0);row[used-1]='\n';WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),row,used,&w,0);used=0;}
#define CHECK(x) do{checks++;if(!(x)){failures++;number(current);number(__LINE__);emit("FILEVER FAIL ");}}while(0)
static void *resolve(HMODULE m,const char *s){void *p=GetProcAddress(m,s);if(!p)ExitProcess(2);return p;}
static DWORD hash_bytes(DWORD hash,const void *p,unsigned n){const BYTE *b=p;while(n--)hash=(hash^*b++)*16777619u;return hash;}
void file_version_entry(void)
{
    HMODULE k=LoadLibraryA("kernel32.dll"),v=LoadLibraryA("version.dll");
    DWORD (WINAPI *module_name)(HMODULE,LPSTR,DWORD)=(void *)resolve(k,"GetModuleFileNameA");
    size_a=(void *)resolve(v,"GetFileVersionInfoSizeExA");size_w=(void *)resolve(v,"GetFileVersionInfoSizeExW");
    load_a=(void *)resolve(v,"GetFileVersionInfoExA");load_w=(void *)resolve(v,"GetFileVersionInfoExW");
    query_a=(void *)resolve(v,"VerQueryValueA");query_w=(void *)resolve(v,"VerQueryValueW");
    DWORD (WINAPI *old_size_a)(LPCSTR,DWORD *)=(void *)resolve(v,"GetFileVersionInfoSizeA");
    DWORD (WINAPI *old_size_w)(LPCWSTR,DWORD *)=(void *)resolve(v,"GetFileVersionInfoSizeW");
    BOOL (WINAPI *old_load_a)(LPCSTR,DWORD,DWORD,LPVOID)=(void *)resolve(v,"GetFileVersionInfoA");
    BOOL (WINAPI *old_load_w)(LPCWSTR,DWORD,DWORD,LPVOID)=(void *)resolve(v,"GetFileVersionInfoW");
    unsigned n=module_name(NULL,path,sizeof(path)),prefix=0;CHECK(n>0 && n<500);
    for(unsigned i=0;i<n;i++)if(path[i]=='/' || path[i]=='\\')prefix=i+1;
    const char *files[]={"fixture32.dll","fixture64.dll","localized64.dll","bad64.dll","fallback64.dll","padded64.dll","missing.dll"};
    DWORD lengths[]={0,1,52,91,92,95,96,127,184,188,500,1003,1004,1100};
    for(unsigned file=0;file<sizeof(files)/sizeof(files[0]);file++) {
        n=prefix;for(unsigned i=0;files[file][i];i++)path[n++]=files[file][i];path[n]=0;
        for(unsigned i=0;i<=n;i++)wide[i]=(BYTE)path[i];
        for(unsigned api=0;api<2;api++) {
            DWORD hash=2166136261u;
            for(unsigned flags=0;flags<12;flags++) {
                current=(file<<16)|(api<<8)|flags;
                DWORD handle=0xa5;SetLastError(0x1234);
                DWORD size=api?size_w(flags,wide,&handle):size_a(flags,path,&handle),error=GetLastError();
                hash=hash_bytes(hash,&size,4);hash=hash_bytes(hash,&error,4);hash=hash_bytes(hash,&handle,4);
                if(file==6){number(api);number(flags);number(error);emit("FILEVER MISSING SIZE ");}
                CHECK(size==(file==6?0:1004) && handle==0);
                for(unsigned j=0;j<sizeof(lengths)/sizeof(lengths[0]);j++) {
                    memset(buffer,0xa5,sizeof(buffer));SetLastError(0x1234);
                    BOOL ok=api?load_w(flags,wide,99,lengths[j],buffer):load_a(flags,path,99,lengths[j],buffer);error=GetLastError();
                    if(file==6){number(api);number(flags);number(lengths[j]);number(ok);number(error);emit("FILEVER MISSING LOAD ");}
                    hash=hash_bytes(hash,&ok,4);hash=hash_bytes(hash,&error,4);hash=hash_bytes(hash,buffer,sizeof(buffer));
                    BOOL guard=TRUE;for(unsigned i=lengths[j];i<sizeof(buffer);i++)if(buffer[i]!=0xa5)guard=FALSE;CHECK(guard);
                    if(flags==3 && lengths[j]==1004) {
                        CHECK((api?old_size_w(wide,NULL):old_size_a(path,NULL))==size);
                        memset(legacy_buffer,0xa5,sizeof(legacy_buffer));
                        CHECK((api?old_load_w(wide,0,1004,legacy_buffer):old_load_a(path,0,1004,legacy_buffer))==ok);
                        BOOL same=TRUE;for(unsigned i=0;i<sizeof(buffer);i++)if(buffer[i]!=legacy_buffer[i])same=FALSE;CHECK(same);
                    }
                    if(ok && lengths[j]>=1004) {
                        void *p=NULL;UINT len=0;CHECK(query_w(buffer,L"\\",&p,&len) && len==52);
                        DWORD expected=0x0007000b;
                        if((file==2 || file==4) && (flags&3)==1)expected=0x00250029;
                        CHECK(((VS_FIXEDFILEINFO *)p)->dwFileVersionMS==expected);
                        p=NULL;CHECK(query_a(buffer,"\\StringFileInfo\\040904b0\\FileVersion",&p,&len) && len==11);
                        CHECK(((char *)p)[0]=='7' && ((char *)p)[1]=='.' && ((char *)p)[10]==0);
                        p=NULL;CHECK(query_w(buffer,L"\\StringFileInfo\\040904b0\\CompanyName",&p,&len) && len==21);
                        hash=hash_bytes(hash,p,len*2);
                    }
                }
            }
            number(file);number(api);number(hash);emit("FILEVER MATRIX ");
        }
    }
    number(checks);number(failures);emit("FILEVER RESULT ");ExitProcess(failures?1:0);
}
