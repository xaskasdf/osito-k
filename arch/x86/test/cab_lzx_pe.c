/* SetupAPI A/W data-only extraction probe; file hashes are compared to native
 * Windows. Every extracted file is read and removed, never executed. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
#if __SIZEOF_POINTER__ == 4
#pragma pack(push,1)
#endif
typedef struct { const void *name; DWORD size,error; WORD date,time,attributes; char target[260]; } INFOA;
typedef struct { const void *name; DWORD size,error; WORD date,time,attributes; WORD target[260]; } INFOW;
typedef struct { const void *target,*source; DWORD error,flags; } PATHS;
#if __SIZEOF_POINTER__ == 4
#pragma pack(pop)
#endif
static int (CALL *iterateA)(const char *,DWORD,void *,void *);
static int (CALL *iterateW)(const WORD *,DWORD,void *,void *);
static DWORD (CALL *error)(void);
static char *(CALL *commandline)(void);
static int (CALL *deletefile)(const char *), (CALL *closefile)(void *);
static void *(CALL *create)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
static int (CALL *readfile)(void *,void *,DWORD,DWORD *,void *);
static unsigned failures,wide,files,extracted;
static DWORD expected_size;
static const char output_name[]="lzx-probe-output.bin";
#define SAY(s) do { DWORD wr; WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0); } while(0)
#define HEX(v) do { char h[9]; DWORD x=(DWORD)(v),wr; for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;} h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0); } while(0)
static void lzx_probe_check(int ok)
{
    if(ok)SAY("LZX PASS\n");else {failures++;SAY("LZX FAIL\n");}
}
static DWORD CALL lzx_probe_callback(void *context,DWORD notice,UP param1,UP param2)
{
    (void)param2;
    lzx_probe_check(context==(void *)(UP)0x1a2b3c);
    if(notice==16)return 0;
    if(notice==17){
        INFOA *a=(void *)param1;INFOW *w=(void *)param1;files++;expected_size=a->size;
        for(unsigned i=0;i<sizeof(output_name);i++){
            if(wide)w->target[i]=(unsigned char)output_name[i];else a->target[i]=output_name[i];
        }
        return 1;
    }
    if(notice==19){
        PATHS *p=(void *)param1;extracted++;lzx_probe_check(p->error==0);
        DWORD size=0,hash=2166136261U;
        if(!p->error){
            void *f=create(output_name,0x80000000U,7,0,3,0x80,0);
            lzx_probe_check(f!=(void *)(UP)-1);
            if(f!=(void *)(UP)-1){
                static unsigned char data[65536];DWORD got;int rc;
                while((rc=readfile(f,data,sizeof(data),&got,0))&&got){
                    size+=got;for(DWORD i=0;i<got;i++)hash=(hash^data[i])*16777619U;
                }
                lzx_probe_check(rc!=0);closefile(f);
            }
            lzx_probe_check(size==expected_size);lzx_probe_check(deletefile(output_name));
        }
        SAY("LZX ROW ");HEX(wide);HEX(extracted);HEX(size);HEX(hash);HEX(p->error);SAY("\n");
        return p->error;
    }
    lzx_probe_check(0);return 87;
}
void lzx_probe_entry(void)
{
    void *s=LoadLibraryA("setupapi.dll"),*k=LoadLibraryA("kernel32.dll");
#define LOAD(dst,mod,name) dst=(__typeof__(dst))GetProcAddress(mod,name)
    LOAD(iterateA,s,"SetupIterateCabinetA");LOAD(iterateW,s,"SetupIterateCabinetW");
    LOAD(error,k,"GetLastError");LOAD(commandline,k,"GetCommandLineA");
    LOAD(deletefile,k,"DeleteFileA");LOAD(create,k,"CreateFileA");
    LOAD(closefile,k,"CloseHandle");LOAD(readfile,k,"ReadFile");
    lzx_probe_check(iterateA&&iterateW&&error&&commandline&&deletefile&&create&&closefile&&readfile);
    if(failures)ExitProcess(1);
    char *p=commandline();
    if(*p=='"'){p++;while(*p&&*p!='"')p++;if(*p)p++;}else while(*p&&*p!=' ')p++;
    while(*p==' ')p++;
    lzx_probe_check(*p!=0);if(failures)ExitProcess(1);
    char name[260];WORD namew[260];unsigned n=0;
    while(p[n]&&p[n]!=' '&&n<259){name[n]=p[n];namew[n]=(unsigned char)p[n];n++;}
    name[n]=0;namew[n]=0;
    for(wide=0;wide<2;wide++){
        files=extracted=0;deletefile(output_name);
        int rc=wide?iterateW(namew,0,(void *)lzx_probe_callback,(void *)(UP)0x1a2b3c):
                    iterateA(name,0,(void *)lzx_probe_callback,(void *)(UP)0x1a2b3c);
        DWORD last=error();lzx_probe_check(rc&&last==0&&files&&files==extracted);
        SAY("LZX RESULT ");HEX(wide);HEX(rc);HEX(last);HEX(files);HEX(extracted);SAY("\n");
    }
    if(!failures)SAY("LZX DONE\n");ExitProcess(failures?1:0);
}
