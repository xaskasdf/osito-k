/* Native/guest comparison for the CryptoAPI hashes used by bootstrappers. */
typedef unsigned int DWORD;
typedef unsigned char BYTE;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
static int (CALL *acquire)(UP *,const char *,const char *,DWORD,DWORD);
static int (CALL *release)(UP,DWORD);
static int (CALL *create)(UP,DWORD,UP,DWORD,UP *);
static int (CALL *update)(UP,const BYTE *,DWORD,DWORD);
static int (CALL *query)(UP,DWORD,BYTE *,DWORD *,DWORD);
static int (CALL *destroy)(UP);
static char *(CALL *command_line)(void);
static DWORD (CALL *module_path)(void *,char *,DWORD);
typedef struct { void *process,*thread; DWORD pid,tid; } PROCESS;
static int (CALL *create_process)(const char *,char *,void *,void *,int,DWORD,void *,const char *,void *,PROCESS *);
static DWORD (CALL *wait_for)(void *,DWORD);
static int (CALL *exit_code)(void *,DWORD *);
static int (CALL *close_handle)(void *);
static DWORD failures,checks,algorithm;
static void say(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void hex(DWORD value)
{
    char out[9];for(unsigned i=0;i<8;i++)out[i]="0123456789abcdef"[(value>>(28-4*i))&15];
    out[8]=0;say(out);
}
static void check(int good,const char *label)
{ checks++;if(!good)failures++;say(good?"HASH PASS ":"HASH FAIL ");say(label);say("\n"); }
static void row(const char *label,int result,DWORD size,DWORD value)
{
    DWORD error=GetLastError();
    say("HASH ROW ");say(label);say(" alg=");hex(algorithm);say(" result=");hex(result);
    say(" error=");hex(error);say(" size=");hex(size);say(" value=");hex(value);say("\n");
}
static void get_value(UP hash,const char *label)
{
    BYTE digest[64];for(unsigned i=0;i<64;i++)digest[i]=0xaa;
    DWORD size=sizeof(digest);SetLastError(0x1234);
    int result=query(hash,2,digest,&size,0);row(label,result,size,0);
    if(result && size<=sizeof(digest)){
        say("HASH DIGEST ");say(label);say(" alg=");hex(algorithm);say(" value=");
        for(unsigned i=0;i<size;i++){
            char s[3]={"0123456789abcdef"[digest[i]>>4],"0123456789abcdef"[digest[i]&15],0};say(s);
        }
        say("\n");
    }
}
static UP make_hash(UP provider)
{ UP h=0;check(create(provider,algorithm,0,0,&h) && h,"create hash");return h; }
static void algorithm_cases(UP provider,DWORD id)
{
    algorithm=id;UP hash=make_hash(provider);DWORD size,value;
    size=sizeof(value);value=0xcccccccc;SetLastError(0x1234);
    int result=query(hash,1,(BYTE *)&value,&size,0);row("algorithm",result,size,value);
    size=sizeof(value);value=0xcccccccc;SetLastError(0x1234);
    result=query(hash,4,(BYTE *)&value,&size,0);row("digest-size",result,size,value);
    check(update(hash,0,0,0),"empty update");
    check(update(hash,(const BYTE *)"a",1,0) && update(hash,(const BYTE *)"bc",2,0),"streamed abc");
    get_value(hash,"abc");get_value(hash,"repeat");
    SetLastError(0x1234);result=update(hash,(const BYTE *)"d",1,0);row("after-finish",result,0,0);
    SetLastError(0x1234);result=update(hash,0,0,0);row("empty-after-finish",result,0,0);
    check(destroy(hash),"destroy completed hash");

    hash=make_hash(provider);size=0;SetLastError(0x1234);
    result=query(hash,2,0,&size,0);row("value-size-query",result,size,0);
    SetLastError(0x1234);result=update(hash,(const BYTE *)"abc",3,0);row("after-size-query",result,0,0);
    get_value(hash,"after-size");check(destroy(hash),"destroy queried hash");

    hash=make_hash(provider);check(update(hash,(const BYTE *)"a",1,0),"short query prefix");
    BYTE short_buffer[4]={0xcc,0xcc,0xcc,0xcc};size=1;SetLastError(0x1234);
    result=query(hash,2,short_buffer,&size,0);row("short-buffer",result,size,*(DWORD *)short_buffer);
    SetLastError(0x1234);result=update(hash,(const BYTE *)"bc",2,0);row("after-short",result,0,0);
    get_value(hash,"after-short");check(destroy(hash),"destroy short-buffer hash");

    hash=make_hash(provider);get_value(hash,"empty");check(destroy(hash),"destroy empty hash");
    hash=make_hash(provider);
    BYTE block[257];for(unsigned i=0;i<sizeof(block);i++)block[i]=(BYTE)i;
    for(unsigned i=0;i<1021;i++)if(!update(hash,block,sizeof(block),0)){check(0,"long stream update");break;}
    get_value(hash,"long-stream");check(destroy(hash),"destroy long-stream hash");
}
void installer_hash_entry(void)
{
    void *a=LoadLibraryA("advapi32.dll"),*k=LoadLibraryA("kernel32.dll");
#define LOAD(dst,mod,name) dst=(__typeof__(dst))GetProcAddress(mod,name)
    LOAD(acquire,a,"CryptAcquireContextA");LOAD(release,a,"CryptReleaseContext");
    LOAD(create,a,"CryptCreateHash");LOAD(update,a,"CryptHashData");
    LOAD(query,a,"CryptGetHashParam");LOAD(destroy,a,"CryptDestroyHash");
    LOAD(command_line,k,"GetCommandLineA");LOAD(module_path,k,"GetModuleFileNameA");
    LOAD(create_process,k,"CreateProcessA");LOAD(wait_for,k,"WaitForSingleObject");
    LOAD(exit_code,k,"GetExitCodeProcess");LOAD(close_handle,k,"CloseHandle");
    check(acquire&&release&&create&&update&&query&&destroy&&command_line&&module_path&&create_process&&wait_for&&exit_code&&close_handle,"exports");
    if(failures)ExitProcess(1);
    UP provider=0;check(acquire(&provider,0,0,24,0xf0000000U),"acquire RSA AES provider");
    if(failures)ExitProcess(1);
    for(const char *p=command_line();*p;p++)if(p[0]==' '&&p[1]=='l'&&p[2]=='e'&&p[3]=='a'&&p[4]=='k'){
        algorithm=0x800c;
        for(unsigned i=0;i<96;i++){UP h=make_hash(provider);check(update(h,(const BYTE *)"child",5,0),"child hash state");}
        say("HASH CHILD DONE\n");ExitProcess(failures?1:0);
    }
    const DWORD algorithms[]={0x8003,0x8004,0x800c,0x800d,0x800e};
    for(unsigned i=0;i<5;i++)algorithm_cases(provider,algorithms[i]);
    algorithm=0x8004;UP hash=make_hash(provider);DWORD size=4,value=0xcccccccc;
    SetLastError(0x1234);int result=query(hash,0x1234,(BYTE *)&value,&size,0);row("bad-parameter",result,size,value);
    size=4;SetLastError(0x1234);result=query(hash,1,(BYTE *)&value,&size,1);row("query-flags",result,size,value);
    SetLastError(0x1234);result=update(hash,(const BYTE *)"abc",3,0x100);row("update-bad-flags",result,0,0);
    SetLastError(0x1234);result=update(hash,(const BYTE *)"abc",3,1);row("update-userdata",result,0,0);
    get_value(hash,"flag-result");check(destroy(hash),"destroy flags hash");
    for(unsigned flags=1;flags<=2;flags++){
        hash=(UP)0xcccccccc;SetLastError(0x1234);result=create(provider,algorithm,0,flags,&hash);
        row(flags==1?"create-secret":"create-bad-flags",result,0,hash==(UP)0xcccccccc);
        if(result)destroy(hash);
    }
    hash=(UP)0xcccccccc;SetLastError(0x1234);result=create(provider,0x12345678,0,0,&hash);row("bad-algorithm",result,0,hash==(UP)0xcccccccc);
    if(result)destroy(hash);
    hash=(UP)0xcccccccc;SetLastError(0x1234);result=create(0,algorithm,0,0,&hash);row("null-provider",result,0,hash==(UP)0xcccccccc);
    SetLastError(0x1234);result=destroy(0);row("null-destroy",result,0,0);
    SetLastError(0x1234);result=update(0,(const BYTE *)"a",1,0);row("null-update",result,0,0);
    size=4;value=0xcccccccc;SetLastError(0x1234);result=query(0,1,(BYTE *)&value,&size,0);row("null-query",result,size,value);
    UP rsa=0;check(acquire(&rsa,0,0,1,0xf0000000U),"acquire RSA full provider");
    for(unsigned i=0;i<5;i++){
        algorithm=algorithms[i];hash=(UP)0xcccccccc;SetLastError(0x1234);
        result=create(rsa,algorithm,0,0,&hash);row("RSA-full-algorithm",result,0,hash==(UP)0xcccccccc);
        if(result)check(destroy(hash),"destroy RSA full hash");
    }
    check(release(rsa,0),"release RSA full provider");
    algorithm=0x800c;hash=make_hash(provider);check(update(hash,(const BYTE *)"abc",3,0),"parent prefix");
    char path[512];check(module_path(0,path,sizeof(path))!=0,"child image path");
    for(unsigned i=0;i<4;i++){
        char args[540];unsigned p=0,n=0;args[p++]='"';
        while(path[n]&&p<520)args[p++]=path[n++];args[p++]='"';
        const char *suffix=" leak";while(*suffix)args[p++]=*suffix++;args[p]=0;
        BYTE startup[sizeof(UP)==8?104:68]={0};*(DWORD *)startup=sizeof(startup);
        PROCESS child={0};DWORD code=1;
        int made=create_process(path,args,0,0,0,0,0,0,startup,&child);check(made,"create hash owner child");
        if(!made)continue;
        check(wait_for(child.process,30000)==0&&exit_code(child.process,&code)&&code==0,"child hash cleanup");
        check(close_handle(child.thread)&&close_handle(child.process),"close child handles");
    }
    get_value(hash,"parent-survives");check(destroy(hash),"destroy parent hash");
    for(unsigned i=0;i<600;i++){hash=make_hash(provider);check(destroy(hash),"reuse hash slot");}
    check(release(provider,0),"release provider");
    say("HASH DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");
    ExitProcess(failures?1:0);
}
