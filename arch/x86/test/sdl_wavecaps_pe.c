/* The two encodings must describe the same live wave device and honor cbwoc. */
typedef unsigned int UINT;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API void *CALL GetStdHandle(UINT);
API int CALL WriteFile(void *,const void *,UINT,UINT *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
void *memset(void *p,int c,UPTR n){unsigned char *s=p;for(UPTR i=0;i<n;i++)s[i]=(unsigned char)c;return p;}
void *memcpy(void *p,const void *q,UPTR n){unsigned char *d=p;const unsigned char *s=q;for(UPTR i=0;i<n;i++)d[i]=s[i];return p;}
static unsigned failures;
#define SAY(s) do{UINT n;WriteFile(GetStdHandle((UINT)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("WAVECAPS PASS " name "\n");else{SAY("WAVECAPS FAIL " name "\n");failures++;}}while(0)
typedef struct {WORD mid,pid;UINT version;char name[32];UINT formats;WORD channels,reserved;UINT support;} CapsA;
typedef struct {WORD mid,pid;UINT version;WORD name[32];UINT formats;WORD channels,reserved;UINT support;} CapsW;
typedef struct {WORD tag,channels;UINT rate,bytes;WORD align,bits,extra;} Format;
static int same(const unsigned char *a,const unsigned char *b,UINT n)
{for(UINT i=0;i<n;i++)if(a[i]!=b[i])return 0;return 1;}
void sdl_wavecaps_entry(void)
{
    void *winmm=LoadLibraryA("winmm.dll");
    typedef UINT (CALL *caps_fn)(UPTR,void *,UINT);
    typedef UINT (CALL *count_fn)(void);
    typedef UINT (CALL *open_fn)(void **,UINT,const Format *,UPTR,UPTR,UINT);
    typedef UINT (CALL *close_fn)(void *);
    caps_fn capsA=GetProcAddress(winmm,"waveOutGetDevCapsA"),capsW=GetProcAddress(winmm,"waveOutGetDevCapsW");
    count_fn count=GetProcAddress(winmm,"waveOutGetNumDevs");
    open_fn open=GetProcAddress(winmm,"waveOutOpen");close_fn close=GetProcAddress(winmm,"waveOutClose");
    CHECK(capsA&&capsW&&count&&open&&close,"exports");if(failures)ExitProcess(1);
    CHECK(sizeof(CapsA)==52&&sizeof(CapsW)==84,"caps-layouts");
    CHECK(count()>0,"live-output-device");if(failures)ExitProcess(1);
    struct{CapsA caps;UINT guard;}a;struct{CapsW caps;UINT guard;}w;
    memset(&a,0x5a,sizeof(a));memset(&w,0x5a,sizeof(w));
    CHECK(capsA(0,&a.caps,sizeof(a.caps))==0&&a.guard==0x5a5a5a5a,"ansi-full-guard");
    CHECK(capsW(0,&w.caps,sizeof(w.caps))==0&&w.guard==0x5a5a5a5a,"wide-full-guard");
    CHECK(a.caps.mid==w.caps.mid&&a.caps.pid==w.caps.pid&&a.caps.version==w.caps.version&&a.caps.formats==w.caps.formats&&a.caps.channels==w.caps.channels&&a.caps.support==w.caps.support,"same-device-capabilities");
    CHECK(a.caps.name[0]&&w.caps.name[0]&&a.caps.channels>0,"nonempty-description");
    unsigned char partial[100];memset(partial,0x77,sizeof(partial));
    UINT partial_status=capsW(0,partial,13);
    /* Some native drivers reject undersized CAPS; accepting it must honor
     * the byte count. Do not require their observed out-of-bounds writes. */
    CHECK(partial_status==11||(partial_status==0&&same(partial,(unsigned char*)&w.caps,13)&&partial[13]==0x77),"wide-short-byte-buffer");
    memset(partial,0x77,sizeof(partial));
    CHECK(capsW(0,partial,96)==0&&same(partial,(unsigned char*)&w.caps,sizeof(w.caps))&&partial[96]==0x77,"wide-large-buffer-boundary");
    CHECK(capsA(0,0,0)==0&&capsW(0,0,0)==0,"zero-size-no-output");
    CHECK(capsW(0,0,sizeof(CapsW))==11,"null-output-error");
    CHECK(capsW((UPTR)-2,&w.caps,sizeof(w.caps))==2,"invalid-device-id");
    CHECK(capsW(0xffffffffU,&w.caps,sizeof(w.caps))==0,"wave-mapper-id");
    Format format={1,2,44100,176400,4,16,0};void *device=0;
    UINT status=open(&device,0xffffffffU,&format,0,0,0);
    CHECK(status==0&&device,"open-device");
    if(device){
        CHECK(capsW((UPTR)device,&w.caps,sizeof(w.caps))==0,"wide-open-handle");
        CHECK(capsA((UPTR)device,&a.caps,sizeof(a.caps))==0,"ansi-open-handle");
        CHECK(close(device)==0,"close-device");
    }
    if(!failures)SAY("WAVECAPS DONE\n");
    ExitProcess(failures?1:0);
}
