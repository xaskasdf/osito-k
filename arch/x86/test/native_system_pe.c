/* Native/guest oracle for SYSTEM_INFO layout and WOW64 architecture. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
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
#define SAY(s) do { DWORD wr; WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0); } while(0)
#define HEX(v) do { char h[9]; DWORD x=(DWORD)(v),wr; for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0); } while(0)
typedef struct {
    WORD architecture, reserved;
    DWORD page_size;
    UPTR minimum, maximum, mask;
    DWORD count, processor_type, granularity;
    WORD level, revision;
} INFO;
static unsigned failures;
static void native_system_check(int ok)
{
    if(ok) SAY("NATIVESYSTEM PASS\n");
    else { failures++; SAY("NATIVESYSTEM FAIL\n"); }
}
void native_system_entry(void)
{
    void *module=LoadLibraryA("kernel32.dll");
    void (CALL *query[2])(void *)={GetProcAddress(module,"GetSystemInfo"),
                                 GetProcAddress(module,"GetNativeSystemInfo")};
    struct { UPTR before; INFO info; BYTE after[24]; } slots[2];
    native_system_check(query[0] && query[1]);
    if(!query[0] || !query[1]) ExitProcess(1);
    native_system_check(sizeof(INFO)==(sizeof(void *)==4?36:48));
    for(unsigned n=0;n<2;n++) {
        BYTE *raw=(BYTE *)&slots[n];
        for(unsigned i=0;i<sizeof(slots[n]);i++) raw[i]=0xa5;
        INFO *p=&slots[n].info;
        SetLastError(0x1234abcd); query[n](p); DWORD error=GetLastError();
        int intact=1;
        for(unsigned i=0;i<sizeof(UPTR);i++) if(raw[i]!=0xa5) intact=0;
        for(unsigned i=0;i<sizeof(slots[n].after);i++) if(slots[n].after[i]!=0xa5) intact=0;
        native_system_check(intact);
        native_system_check(error==0x1234abcd);
        native_system_check(p->architecture==(n || sizeof(void *)==8?9:0));
        native_system_check(p->reserved==0);
        native_system_check(p->page_size==4096);
        native_system_check(p->minimum==0x10000);
        native_system_check(p->maximum>=0x7ffeffff);
        native_system_check(p->count>0 && p->count<=sizeof(UPTR)*8);
        native_system_check(p->mask!=0);
        native_system_check(p->processor_type==(n || sizeof(void *)==8?8664:586));
        native_system_check(p->granularity==65536);
        SAY("NATIVESYSTEM ROW "); HEX(n); HEX(sizeof(INFO)); HEX(p->architecture);
        HEX(p->reserved); HEX(p->page_size); HEX(p->minimum); HEX(p->processor_type);
        HEX(p->granularity); HEX(error); HEX(intact); SAY("\n");
        SAY("NATIVESYSTEM HARDWARE "); HEX(n); HEX(p->maximum);
        HEX((unsigned long long)p->maximum>>32); HEX(p->mask);
        HEX((unsigned long long)p->mask>>32); HEX(p->count); HEX(p->level); HEX(p->revision); SAY("\n");
    }
    INFO *a=&slots[0].info, *b=&slots[1].info;
    native_system_check(a->count==b->count && a->mask==b->mask);
    native_system_check(a->minimum==b->minimum);
    native_system_check(b->maximum==(sizeof(void *)==4?(UPTR)0xfffeffff:a->maximum));
    native_system_check(a->level==b->level && a->revision==b->revision);
    if(sizeof(void *)==8) {
        int equal=1;
        for(unsigned i=0;i<sizeof(INFO);i++) if(((BYTE *)a)[i]!=((BYTE *)b)[i]) equal=0;
        native_system_check(equal);
    }
    if(!failures) SAY("NATIVESYSTEM DONE\n");
    ExitProcess(failures?1:0);
}
