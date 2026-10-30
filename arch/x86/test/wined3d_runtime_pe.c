/* Native Windows oracle for the legacy CRT/Win32 imports used by WineD3D. */
typedef unsigned int UINT, DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define CDECL __attribute__((cdecl))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
#define SAY(text) do {static const char msg[]=text;DWORD w; \
    WriteFile(GetStdHandle((DWORD)-11),msg,sizeof(msg)-1,&w,0);} while(0)
#define CHECK(test,label) do {checks++;if(!(test)){failures++;SAY("RUNTIME FAIL " label "\n");}} while(0)
#define LOAD(var,type,module,name) type var=(type)GetProcAddress(module,name); \
    if(!var){SAY("RUNTIME MISSING " name "\n");ExitProcess(2);}
typedef int *(CDECL *int_ptr_fn)(void);
typedef void *(CDECL *ptr_fn)(void);
typedef int (CDECL *int_fn)(void);
typedef int (CDECL *ctype_fn)(int,int);
typedef long long (CDECL *signed_fn)(const char *,char **,int);
typedef unsigned long long (CDECL *unsigned_fn)(const char *,char **,int);
typedef int (CDECL *vcount_fn)(const char *,void *);
typedef int (CDECL *stat_fn)(const char *,void *);
typedef BOOL (CALL *luid_fn)(unsigned long long *);
typedef BOOL (CALL *adjust_fn)(DWORD *,DWORD *,BOOL *);
typedef HANDLE (CALL *thread_fn)(void *,UPTR,DWORD(CALL *)(void *),void *,DWORD,DWORD *);
typedef DWORD (CALL *wait_fn)(HANDLE,DWORD);
typedef BOOL (CALL *close_fn)(HANDLE);
typedef void (CALL *cs_fn)(void *);
typedef BOOL (CALL *owned_fn)(void *);
static int_ptr_fn fpecode;
static ptr_fn exception_info;
static volatile DWORD thread_result;
static int *parent_fpecode;
static void *parent_exception;

static DWORD CALL wined3d_runtime_thread(void *unused)
{
    (void)unused;
    int *fp=fpecode();
    void *xp=exception_info();
    thread_result=fp && xp && fp!=parent_fpecode && xp!=parent_exception &&
                  *fp==0 && *(UPTR *)xp==0;
    if(fp)*fp=123;
    if(xp)*(UPTR *)xp=0x1234;
    return 0;
}

void wined3d_runtime_entry(void)
{
    UINT checks=0,failures=0;
    HANDLE crt=LoadLibraryA("msvcrt.dll"),k32=LoadLibraryA("kernel32.dll");
    HANDLE adv=LoadLibraryA("advapi32.dll"),nt=LoadLibraryA("ntdll.dll");
    LOAD(codepage,int_fn,crt,"___lc_codepage_func");
    LOAD(mb,int_fn,crt,"___mb_cur_max_func");
    LOAD(ctype,ctype_fn,crt,"_isctype");
    LOAD(si,signed_fn,crt,"_strtoi64");
    LOAD(ui,unsigned_fn,crt,"_strtoui64");
    LOAD(error,int_ptr_fn,crt,"_errno");
    LOAD(iob,ptr_fn,crt,"__iob_func");
    ptr_fn piob=sizeof(UPTR)==4?(ptr_fn)GetProcAddress(crt,"__p__iob"):iob;
    LOAD(vc,vcount_fn,crt,"_vscprintf");
    LOAD(stat,stat_fn,crt,"_stati64");
    LOAD(luid,luid_fn,adv,"AllocateLocallyUniqueId");
    LOAD(adjust,adjust_fn,k32,"GetSystemTimeAdjustment");
    LOAD(thread,thread_fn,k32,"CreateThread");
    LOAD(wait,wait_fn,k32,"WaitForSingleObject");
    LOAD(close,close_fn,k32,"CloseHandle");
    LOAD(init_cs,cs_fn,k32,"InitializeCriticalSection");
    LOAD(enter_cs,cs_fn,k32,"EnterCriticalSection");
    LOAD(leave_cs,cs_fn,k32,"LeaveCriticalSection");
    LOAD(delete_cs,cs_fn,k32,"DeleteCriticalSection");
    LOAD(owned,owned_fn,nt,"RtlIsCriticalSectionLockedByThread");
    fpecode=(int_ptr_fn)GetProcAddress(crt,"__fpecode");
    exception_info=(ptr_fn)GetProcAddress(crt,"__pxcptinfoptrs");
    if(!fpecode || !exception_info)ExitProcess(2);
    CHECK(codepage()==0 && mb()==1,"C locale accessors");
    CHECK(iob()!=0 && piob && piob()==iob(),"iob alias");
    for(int c=-1;c<256;c++){
        int expected=0;
        if(c>=0 && c<256){
            if(c>='A' && c<='Z')expected|=1;
            if(c>='a' && c<='z')expected|=2;
            if(c>='0' && c<='9')expected|=4;
            if(c==32 || (c>=9 && c<=13))expected|=8;
            if(c<32 || c==127)expected|=32;
            if(c==32)expected|=64;
            if((c>='0' && c<='9') || (c>='A' && c<='F') || (c>='a' && c<='f'))expected|=128;
            if(c>32 && c<127 && !(expected&7))expected|=16;
        }
        CHECK(ctype(c,0xffff)==expected,"C ctype flags");
    }
    static const struct {const char *s; int base; long long value; UINT consumed; int overflow;} cases[]={
        {"0",0,0,1,0},{"  -0x10x",0,-16,7,0},{"z!",36,35,1,0},
        {"0x",0,0,0,0},{"09",0,0,1,0},{" +!",10,0,0,0},
        {"9223372036854775807",10,0x7fffffffffffffffLL,19,0},
        {"-9223372036854775808",10,(-0x7fffffffffffffffLL-1),20,0},
        {"9223372036854775808!",10,0x7fffffffffffffffLL,19,1},
        {"-9223372036854775809!",10,(-0x7fffffffffffffffLL-1),20,1}
    };
    for(UINT i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
        struct {char *end; UPTR guard;} end={0,(UPTR)0x6e7f8a9b};
        *error()=0;
        CHECK(si(cases[i].s,&end.end,cases[i].base)==cases[i].value,"signed 64-bit value");
        CHECK(end.end==cases[i].s+cases[i].consumed && end.guard==(UPTR)0x6e7f8a9b,"end pointer ABI");
        CHECK(*error()==(cases[i].overflow?34:0),"signed overflow errno");
    }
    char *end=0;
    CHECK(ui("18446744073709551615",&end,10)==~0ULL && *end==0,"unsigned max");
    *error()=0;
    CHECK(ui("18446744073709551616!",&end,10)==~0ULL && *end=='!' && *error()==34,"unsigned overflow");
    CHECK(ui("-1",0,10)==~0ULL,"unsigned negative");
    UPTR args[3]={(UPTR)"abc",0,0};
    if(sizeof(UPTR)==4){args[1]=0x89abcdefU;args[2]=0x1234567U;}
    else args[1]=0x123456789abcdefULL;
    CHECK(vc("%s:%I64x",args)==19,"va_list count");
    unsigned long long previous=0;
    for(UINT i=0;i<1024;i++){
        unsigned long long value=0;
        CHECK(luid(&value) && value && value!=previous,"unique LUID");previous=value;
    }
    DWORD adjustment=0,increment=0;BOOL disabled=0;
    CHECK(adjust(&adjustment,&increment,&disabled) && increment>0,"time adjustment");
    unsigned char data[72];
    for(UINT i=0;i<sizeof(data);i++)data[i]=0xa5;
    CHECK(stat(sizeof(UPTR)==4?"runtime32.exe":"runtime64.exe",data)==0,"legacy stat");
    UINT stat_size=sizeof(UPTR)==4?48:56;
    CHECK(*(unsigned long long *)(void *)(data+24)>0,"legacy stat size");
    for(UINT i=stat_size;i<sizeof(data);i++)CHECK(data[i]==0xa5,"stat output bounds");
    UPTR cs[8]={0};
    init_cs(cs);CHECK(!owned(cs),"unowned critical section");
    enter_cs(cs);enter_cs(cs);CHECK(owned(cs),"recursive critical section");
    leave_cs(cs);CHECK(owned(cs),"recursive section remains owned");
    leave_cs(cs);CHECK(!owned(cs),"released critical section");delete_cs(cs);
    parent_fpecode=fpecode();parent_exception=exception_info();
    CHECK(parent_fpecode && parent_exception,"thread storage");
    if(parent_fpecode && parent_exception){
        *parent_fpecode=37;*(UPTR *)parent_exception=0x5678;
        HANDLE worker=thread(0,0,wined3d_runtime_thread,0,0,0);
        CHECK(worker && wait(worker,10000)==0,"thread completed");
        CHECK(thread_result && *parent_fpecode==37 && *(UPTR *)parent_exception==0x5678,"thread isolation");
        if(worker)close(worker);
        *parent_fpecode=0;*(UPTR *)parent_exception=0;
    }
    char numbers[18];DWORD written;
    for(UINT i=0;i<8;i++)numbers[i]="0123456789abcdef"[(checks>>((7-i)*4))&15];
    numbers[8]=' ';for(UINT i=0;i<8;i++)numbers[9+i]="0123456789abcdef"[(failures>>((7-i)*4))&15];numbers[17]='\n';
    SAY("RUNTIME DONE checks/failures=");WriteFile(GetStdHandle((DWORD)-11),numbers,18,&written,0);
    ExitProcess(failures?1:0);
}
