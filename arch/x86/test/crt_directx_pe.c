/* Missing CRT/Kernel32 dependencies of the original June 2010 DirectX DLLs.
 * The same executable checks Windows and Osito-K, including PE32 FP ABI. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef unsigned long long U64;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
int _fltused;
#define SAY(s) do{DWORD n;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(t,s) do{if(t)SAY("DXCRT PASS " s "\n");else{SAY("DXCRT FAIL " s "\n");failures++;}}while(0)
static int equal(const char *a,const char *b)
{if(!a||!b)return 0;while(*a&&*a==*b){a++;b++;}return *a==*b;}
void *memset(void *destination,int value,UPTR size)
{unsigned char *p=destination;for(UPTR i=0;i<size;i++)p[i]=(unsigned char)value;return destination;}
void *memcpy(void *destination,const void *source,UPTR size)
{unsigned char *p=destination;const unsigned char *q=source;for(UPTR i=0;i<size;i++)p[i]=q[i];return destination;}
static void hex(DWORD value)
{
    char text[11]={'0','x'};DWORD written;
    for(unsigned i=0;i<8;i++)text[2+i]="0123456789abcdef"[(value>>(28-4*i))&15];
    text[10]='\n';WriteFile(GetStdHandle((DWORD)-11),text,11,&written,0);
}
void directx_crt_entry(void)
{
    DWORD failures=0;
    void *crt=LoadLibraryA("msvcrt.dll"),*kernel=LoadLibraryA("kernel32.dll");
    typedef char *(*itoa_fn)(int,char *,int);
    typedef char *(*ultoa_fn)(DWORD,char *,int);
    typedef UPTR (*length_fn)(const char *);
    typedef int (*wctomb_fn)(char *,WCHAR);
    typedef int (*class_fn)(int);
    typedef char *(*tempnam_fn)(const char *,const char *);
    typedef void (*free_fn)(void *);
    typedef int *(*errno_fn)(void);
    typedef int (*fpclass_fn)(double);
    typedef unsigned (*clear_fn)(void);
    itoa_fn itoa=(itoa_fn)GetProcAddress(crt,"_itoa");
    ultoa_fn ultoa=(ultoa_fn)GetProcAddress(crt,"_ultoa");
    length_fn mbstrlen=(length_fn)GetProcAddress(crt,"_mbstrlen");
    wctomb_fn wctomb=(wctomb_fn)GetProcAddress(crt,"wctomb");
    class_fn xdigit=(class_fn)GetProcAddress(crt,"isxdigit");
    class_fn punct=(class_fn)GetProcAddress(crt,"iswpunct");
    class_fn lead=(class_fn)GetProcAddress(crt,"isleadbyte");
    tempnam_fn tempnam=(tempnam_fn)GetProcAddress(crt,"_tempnam");
    free_fn release=(free_fn)GetProcAddress(crt,"free");
    errno_fn error=(errno_fn)GetProcAddress(crt,"_errno");
    fpclass_fn fpclass=(fpclass_fn)GetProcAddress(crt,"_fpclass");
    clear_fn clear=(clear_fn)GetProcAddress(crt,"_clearfp");
    CHECK(itoa&&ultoa&&mbstrlen&&wctomb&&xdigit&&punct&&lead&&
          tempnam&&release&&error&&fpclass&&clear,"exports");
    if(failures)ExitProcess(1);
    char buffer[80];
    CHECK(itoa(-2147483647-1,buffer,10)==buffer && equal(buffer,"-2147483648"),"itoa-int-min");
    CHECK(itoa(-1,buffer,16)==buffer && equal(buffer,"ffffffff"),"itoa-negative-hex32");
    CHECK(itoa(0,buffer,2)==buffer && equal(buffer,"0"),"itoa-zero");
    CHECK(ultoa(0xffffffffU,buffer,10)==buffer && equal(buffer,"4294967295"),"ultoa-32bit-width");
    CHECK(ultoa(0xffffffffU,buffer,36)==buffer && equal(buffer,"1z141z3"),"ultoa-radix36");
    CHECK(mbstrlen("abc\xff")==4 && mbstrlen("")==0,"single-byte-locale-length");
    buffer[0]='?';buffer[1]='!';
    CHECK(wctomb(buffer,'A')==1 && buffer[0]=='A' && buffer[1]=='!',"wctomb-ascii-width");
    CHECK(wctomb(buffer,0)==1 && !buffer[0],"wctomb-null-character");
    CHECK(wctomb(buffer,0xff)==1 && (unsigned char)buffer[0]==0xff,"wctomb-c-locale-byte");
    *error()=0;buffer[0]='!';
    int converted=wctomb(buffer,0x100);
    SAY("DXCRT INFO wctomb return/errno/byte\n");hex(converted);hex(*error());hex((unsigned char)buffer[0]);
    CHECK(converted==-1 && *error()==42 && !buffer[0],"wctomb-unrepresentable");
    CHECK(xdigit('a')&&xdigit('F')&&xdigit('9')&&!xdigit('g')&&!xdigit(-1),"hex-classification");
    CHECK(punct('!')&&punct('_')&&!punct('A')&&!punct(' '),"wide-punctuation");
    CHECK(!lead('A')&&!lead(0x81)&&!lead(0xff),"no-c-locale-lead-byte");
    const struct {U64 bits;int kind;} numbers[]={
        {0,0x40},{0x8000000000000000ULL,0x20},
        {1,0x80},{0x8000000000000001ULL,0x10},
        {0x3ff0000000000000ULL,0x100},{0xbff0000000000000ULL,8},
        {0x7ff0000000000000ULL,0x200},{0xfff0000000000000ULL,4},
        {0x7ff8000000000001ULL,2},{0x7ff0000000000001ULL,1}};
    for(DWORD i=0;i<sizeof(numbers)/sizeof(numbers[0]);i++){
        union{U64 bits;double value;}number={numbers[i].bits};
        int kind;
#if defined(__i386__)
        /* Pass the raw two stack words; loading a signaling NaN through x87
         * before calling the function would already quiet it. */
        typedef int (*raw_class_fn)(DWORD,DWORD);
        kind=((raw_class_fn)fpclass)((DWORD)number.bits,(DWORD)(number.bits>>32));
#else
        kind=fpclass(number.value);
#endif
        if(kind!=numbers[i].kind){SAY("DXCRT INFO fpclass index/actual\n");hex(i);hex(kind);}
        CHECK(kind==numbers[i].kind,"fpclass-ieee-bits");
    }
    clear();unsigned csr;
    __asm__ volatile("stmxcsr %0":"=m"(csr));
    unsigned raised=csr|1;
    __asm__ volatile("ldmxcsr %0"::"m"(raised):"memory");
    unsigned previous=clear(),after;
    __asm__ volatile("stmxcsr %0":"=m"(after));
    SAY("DXCRT INFO clearfp previous/csr/after\n");hex(previous);hex(csr);hex(after);
    CHECK((sizeof(void *)==4 ? previous==0 : (previous&0x10)!=0) &&
          !(after&0x3f) && (after&~0x3fU)==(csr&~0x3fU),"clearfp-sse-status-and-control");
    if(sizeof(void *)==4){
        __asm__ volatile("fldz; fldz; fdivp; fstp %%st(0)":::"st");
        previous=clear();unsigned short status;
        __asm__ volatile("fnstsw %0":"=m"(status));
        CHECK((previous&0x10) && !(status&0x3f),"clearfp-x87-status");
    }
    typedef DWORD (CALL *attributes_fn)(const char *);
    typedef int (CALL *remove_fn)(const char *);
    typedef DWORD (CALL *tempfile_fn)(const char *,const char *,DWORD,char *);
    typedef int (CALL *resource_fn)(void *);
    attributes_fn attributes=(attributes_fn)GetProcAddress(kernel,"GetFileAttributesA");
    remove_fn remove=(remove_fn)GetProcAddress(kernel,"DeleteFileA");
    tempfile_fn tempfile=(tempfile_fn)GetProcAddress(kernel,"GetTempFileNameA");
    resource_fn resource=(resource_fn)GetProcAddress(kernel,"FreeResource");
    CHECK(attributes&&remove&&tempfile&&resource,"kernel-dependency-exports");
    if(!attributes||!remove||!tempfile||!resource)ExitProcess(1);
    char *first=tempnam(".","bdx"),*second=tempnam(".","bdx");
    CHECK(first&&second&&!equal(first,second),"tempnam-distinct-owned-names");
    CHECK(first&&second&&attributes(first)==0xffffffffU&&attributes(second)==0xffffffffU,"tempnam-does-not-create");
    release(first);release(second);
    char filename[260];
    DWORD unique=tempfile(".","bdx",0,filename);
    CHECK(unique && attributes(filename)!=0xffffffffU,"tempfile-creates-new-file");
    if(unique)CHECK(remove(filename),"tempfile-cleanup");
    SetLastError(0x1234);
    CHECK(!resource((void *)1) && GetLastError()==0x1234,"freeresource-modern-no-op");
    if(!failures)SAY("DXCRT DONE\n");
    ExitProcess(failures ? 1 : 0);
}
