/* Native/Osito PE32 and PE32+ low-I/O data-export and file-state contract. */
typedef unsigned int DWORD;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define CRT __attribute__((cdecl))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API extern void *__pioinfo[64];
API extern unsigned char __badioinfo[];
#define SAY(s) do {DWORD n;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(t,s) do {if(t)SAY("LOWIO PASS " s "\n");else{SAY("LOWIO FAIL " s "\n");failures++;}}while(0)
static unsigned char *entry(int fd)
{
    return (unsigned char *)__pioinfo[(unsigned)fd>>5]+
        ((unsigned)fd&31)*(sizeof(void *)==4 ? 36 : 56);
}
void *memcpy(void *destination,const void *source,UPTR size)
{unsigned char *p=destination;const unsigned char *q=source;for(UPTR i=0;i<size;i++)p[i]=q[i];return destination;}
void *memset(void *destination,int value,UPTR size)
{unsigned char *p=destination;for(UPTR i=0;i<size;i++)p[i]=(unsigned char)value;return destination;}
void lowio_probe_entry(void)
{
    DWORD failures=0;
    void *crt=LoadLibraryA("msvcrt.dll");
    CHECK(GetProcAddress(crt,"__pioinfo")==__pioinfo,"table-is-data");
    CHECK(GetProcAddress(crt,"__badioinfo")==__badioinfo,"bad-descriptor-is-data");
    CHECK(__pioinfo[0]!=0,"first-block");
    CHECK(*(UPTR *)(void *)__badioinfo==~(UPTR)0 &&
          __badioinfo[sizeof(void *)]==0x80,"bad-descriptor-layout");
    typedef int (CRT *open_fn)(const char *,int,int);
    typedef int (CRT *close_fn)(int);
    typedef int (CRT *setmode_fn)(int,int);
    typedef UPTR (CRT *handle_fn)(int);
    typedef int (CRT *write_fn)(int,const void *,unsigned);
    open_fn open=(open_fn)GetProcAddress(crt,"_open");
    close_fn close=(close_fn)GetProcAddress(crt,"_close");
    setmode_fn setmode=(setmode_fn)GetProcAddress(crt,"_setmode");
    handle_fn gethandle=(handle_fn)GetProcAddress(crt,"_get_osfhandle");
    write_fn write=(write_fn)GetProcAddress(crt,"_write");
    CHECK(open && close && setmode && gethandle && write,"file-exports");
    if(failures)ExitProcess(1);
    CHECK(*(UPTR *)(void *)entry(1)==gethandle(1) &&
          (entry(1)[sizeof(void *)]&1),"stdout-layout");
    int fd=open("lowio-probe.tmp",0x0100|0x0200|0x4000|2,0600);
    CHECK(fd>=3 && fd<32,"open-text-file");
    if(fd<0 || fd>=32)ExitProcess(1);
    CHECK(*(UPTR *)(void *)entry(fd)==gethandle(fd) &&
          (entry(fd)[sizeof(void *)]&0x81)==0x81,"open-handle-and-flags");
    CHECK(write(fd,"abc",3)==3,"write-file");
    CHECK(setmode(fd,0x8000)==0x4000 &&
          !(entry(fd)[sizeof(void *)]&0x80),"text-to-binary-sync");
    CHECK(setmode(fd,0x4000)==0x8000 &&
          (entry(fd)[sizeof(void *)]&0x80),"binary-to-text-sync");
    CHECK(close(fd)==0 && !(entry(fd)[sizeof(void *)]&1) &&
          *(UPTR *)(void *)entry(fd)==~(UPTR)0,"closed-handle-sync");
    fd=open("lowio-probe.tmp",0x4000|0x0008|1,0600);
    CHECK(fd>=3 && fd<32,"open-append-file");
    if(fd>=3 && fd<32) {
        CHECK((entry(fd)[sizeof(void *)]&0xA1)==0xA1,"d3dx-append-bit");
        CHECK(write(fd,"d",1)==1 && close(fd)==0,"append-and-close");
    }
    void *kernel=LoadLibraryA("kernel32.dll");
    typedef int (CALL *delete_fn)(const char *);
    delete_fn remove=(delete_fn)GetProcAddress(kernel,"DeleteFileA");
    CHECK(remove && remove("lowio-probe.tmp"),"remove-test-file");
    if(!failures)SAY("LOWIO DONE\n");
    ExitProcess(failures ? 1 : 0);
}
