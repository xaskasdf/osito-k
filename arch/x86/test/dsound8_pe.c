/* PE32/PE32+ regression for DSOUND ordinal 11, versioned COM interfaces,
 * and real PCM buffer operations. Also runs against Windows dsound.dll. */
typedef unsigned int DWORD;
typedef int HRESULT;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API HRESULT CALL DirectSoundCreate8(const void *, void **, void *);
#define SAY(s) do { DWORD n; WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&n,0); } while (0)
#define CHECK(t,s) do { if(t) SAY("DSOUND8 PASS " s "\n"); \
    else { SAY("DSOUND8 FAIL " s "\n"); failures++; } } while (0)
#define HR(v) ((HRESULT)(v))
#define INVALID HR(0x80070057)
#define NOINTERFACE HR(0x80004002)
#define NODRIVER HR(0x88780078)
typedef struct { DWORD a; WORD b,c; unsigned char d[8]; } GUID;
static const GUID ds1_iid={0x279afa83,0x4981,0x11ce,{0xa5,0x21,0,0x20,0xaf,0xb,0xe5,0x60}};
static const GUID ds8_iid={0xc50a7e93,0xf395,0x4834,{0x9e,0xf6,0x7f,0xa9,0x9d,0xe5,9,0x66}};
static const GUID buffer8_iid={0x6825a449,0x7524,0x4d82,{0x92,0xf,0x50,0xe3,0x6a,0xb3,0xab,0x1e}};
static const GUID ds8_class={0x3901cc3f,0x84b5,0x4fa4,{0xba,0x35,0xaa,0x81,0x72,0xb8,0xa0,0x9b}};
static const GUID unknown_guid={0x12345678,0x1234,0x1234,{1,2,3,4,5,6,7,8}};
typedef struct __attribute__((packed)) {
    WORD tag,channels; DWORD rate,byte_rate; WORD align,bits,extra;
} WAVE;
typedef struct {
    DWORD size,flags,bytes,reserved; WAVE *wave; GUID algorithm;
} DESC;
typedef HRESULT (CALL *qi_fn)(void *,const GUID *,void **);
typedef DWORD (CALL *ref_fn)(void *);
typedef HRESULT (CALL *create_fn)(const GUID *,void **,void *);
typedef HRESULT (CALL *buffer_fn)(void *,const DESC *,void **,void *);
typedef HRESULT (CALL *cert_fn)(void *,DWORD *);
typedef HRESULT (CALL *lock_fn)(void *,DWORD,DWORD,void **,DWORD *,void **,DWORD *,DWORD);
typedef HRESULT (CALL *unlock_fn)(void *,void *,DWORD,void *,DWORD);
typedef HRESULT (CALL *play_fn)(void *,DWORD,DWORD,DWORD);
typedef HRESULT (CALL *stop_fn)(void *);
typedef HRESULT (CALL *dup_fn)(void *,void *,void **);
typedef HRESULT (CALL *init_fn)(void *,const GUID *);
typedef HRESULT (CALL *fx_fn)(void *,DWORD,void *,DWORD *);
typedef HRESULT (CALL *acquire_fn)(void *,DWORD,DWORD,DWORD *);
typedef HRESULT (CALL *path_fn)(void *,const GUID *,DWORD,const GUID *,void **);
#define METHOD(o,n,t) ((t)(*(void ***)(o))[n])
static HRESULT query(void *object,const GUID *iid,void **output)
{ return METHOD(object,0,qi_fn)(object,iid,output); }
static void release(void *object)
{ if(object) METHOD(object,2,ref_fn)(object); }
static void report(DWORD value)
{
    char text[11]={'0','x',0,0,0,0,0,0,0,0,'\n'};
    const char *digits="0123456789abcdef";
    for(DWORD i=0;i<8;i++)text[2+i]=digits[(value>>(28-4*i))&15];
    DWORD n; WriteFile(GetStdHandle((DWORD)-11),text,sizeof(text),&n,0);
}
void *memset(void *destination,int value,UPTR size)
{ unsigned char *p=destination; for(UPTR i=0;i<size;i++)p[i]=(unsigned char)value; return destination; }
void *memcpy(void *destination,const void *source,UPTR size)
{ unsigned char *p=destination; const unsigned char *q=source; for(UPTR i=0;i<size;i++)p[i]=q[i]; return destination; }
void dsound8_probe_entry(void)
{
    DWORD failures=0;
    void *module=LoadLibraryA("dsound.dll");
    create_fn old=(create_fn)GetProcAddress(module,"DirectSoundCreate");
    create_fn named=(create_fn)GetProcAddress(module,"DirectSoundCreate8");
    create_fn ordinal=(create_fn)GetProcAddress(module,(const char *)(UPTR)11);
    CHECK(module && old && named && ordinal,"exports");
    if(failures) ExitProcess(1);
    CHECK(named==ordinal,"name-ordinal-identity");
    CHECK(DirectSoundCreate8(0,0,0)==INVALID,"null-output");
    void *object=(void *)1;
    HRESULT result=ordinal(0,&object,(void *)1);
    SAY("DSOUND8 aggregation hr/output\n"); report(result); report((DWORD)(UPTR)object);
    CHECK(result==HR(0x80040110) || result==INVALID,"no-aggregation");
    object=(void *)1;
    result=named(&unknown_guid,&object,0);
    SAY("DSOUND8 unknown-device hr/output\n"); report(result); report((DWORD)(UPTR)object);
    CHECK(result==NODRIVER,"unknown-device");
    void *kernel=LoadLibraryA("kernel32.dll");
    CHECK(GetProcAddress(kernel,"FatalAppExitA") && GetProcAddress(kernel,"FatalAppExitW"),"fatal-exit-exports");
    void *ole=LoadLibraryA("ole32.dll");
    typedef HRESULT (CALL *coinit_fn)(void *,DWORD);
    typedef HRESULT (CALL *cocreate_fn)(const GUID *,void *,DWORD,const GUID *,void **);
    typedef void (CALL *couninit_fn)(void);
    coinit_fn coinit=(coinit_fn)GetProcAddress(ole,"CoInitializeEx");
    cocreate_fn cocreate=(cocreate_fn)GetProcAddress(ole,"CoCreateInstance");
    couninit_fn couninit=(couninit_fn)GetProcAddress(ole,"CoUninitialize");
    CHECK(coinit && cocreate && couninit && coinit(0,0)>=0,"com-initialize");
    if(failures) ExitProcess(1);
    object=0;
    result=cocreate(&ds8_class,0,1,&ds8_iid,&object);
    CHECK(result==0 && object,"com-create8");
    if(object) {
        result=METHOD(object,10,init_fn)(object,0);
        CHECK(result==0 || result==NODRIVER,"com-initialize-device");
        if(result==0) CHECK(METHOD(object,10,init_fn)(object,0)==HR(0x88780082),"already-initialized");
        release(object);
    }
    object=0;
    result=DirectSoundCreate8(0,&object,0);
    if(result==NODRIVER && !object) {
        SAY("DSOUND8 NO-AUDIO\n"); couninit(); ExitProcess(failures ? 1 : 0);
    }
    CHECK(result==0 && object,"create8-ordinal-iat");
    if(!object) ExitProcess(1);
    void *queried=0;
    CHECK(query(object,&ds8_iid,&queried)==0 && queried,"query-device8"); release(queried);
    queried=0;
    CHECK(query(object,&ds1_iid,&queried)==0 && queried,"query-base-device"); release(queried);
    DWORD certified=0xdeadbeef;
    result=METHOD(object,11,cert_fn)(object,&certified);
    CHECK((result==0 && certified<=1) || result==HR(0x80004001),"certification-slot");
    CHECK(METHOD(object,11,cert_fn)(object,0)==INVALID,"certification-null");
    void *user=LoadLibraryA("user32.dll");
    typedef void *(CALL *desktop_fn)(void);
    typedef HRESULT (CALL *coop_fn)(void *,void *,DWORD);
    desktop_fn desktop=(desktop_fn)GetProcAddress(user,"GetDesktopWindow");
    CHECK(desktop && METHOD(object,6,coop_fn)(object,desktop(),1)==0,"cooperative-level");
    DESC description; memset(&description,0,sizeof(description));
    description.size=sizeof(description); description.flags=1;
    void *primary=0;
    result=METHOD(object,3,buffer_fn)(object,&description,&primary,0);
    CHECK(result==0 && primary,"primary-buffer");
    if(primary) {
        queried=(void *)1;
        CHECK(query(primary,&buffer8_iid,&queried)==NOINTERFACE && !queried,"primary-not-buffer8");
        release(primary);
    }
    WAVE wave={1,1,22050,44100,2,16,0};
    description.flags=0x88; description.bytes=4096; description.wave=&wave;
    void *buffer=0;
    result=METHOD(object,3,buffer_fn)(object,&description,&buffer,0);
    CHECK(result==0 && buffer,"secondary-pcm");
    if(buffer) {
        queried=0;
        CHECK(query(buffer,&buffer8_iid,&queried)==0 && queried,"query-buffer8");
        if(queried) {
            CHECK(METHOD(queried,21,fx_fn)(queried,0,(void *)1,0)==INVALID,"setfx-slot-invalid");
            result=METHOD(queried,22,acquire_fn)(queried,0,0,0);
            SAY("DSOUND8 acquire hr\n"); report(result);
            CHECK(result==HR(0x88780032),"acquire-without-deferred-resources");
            void *effect=(void *)1;
            result=METHOD(queried,23,path_fn)(queried,&unknown_guid,0,&unknown_guid,&effect);
            SAY("DSOUND8 effect hr/output\n"); report(result); report((DWORD)(UPTR)effect);
            CHECK(result<0,"no-fabricated-effect");
            release(queried);
        }
        void *part1=0,*part2=0; DWORD bytes1=0,bytes2=0;
        result=METHOD(buffer,11,lock_fn)(buffer,0,0,&part1,&bytes1,&part2,&bytes2,2);
        CHECK(result==0 && part1 && bytes1+bytes2==4096,"lock-pcm-memory");
        if(result==0) {
            memset(part1,0,bytes1); if(part2)memset(part2,0,bytes2);
            CHECK(METHOD(buffer,19,unlock_fn)(buffer,part1,bytes1,part2,bytes2)==0,"unlock-pcm-memory");
        }
        CHECK(METHOD(buffer,12,play_fn)(buffer,0,0,1)==0,"play-pcm");
        CHECK(METHOD(buffer,18,stop_fn)(buffer)==0,"stop-pcm");
        void *copy=0;
        CHECK(METHOD(object,5,dup_fn)(object,buffer,&copy)==0 && copy,"duplicate-buffer");
        if(copy) {
            queried=0;
            CHECK(query(copy,&buffer8_iid,&queried)==0 && queried,"duplicate-retains-buffer8");
            release(queried); release(copy);
        }
        release(buffer);
    }
    description.flags=0x18; wave.channels=2; wave.align=4; wave.byte_rate=88200;
    buffer=(void *)1;
    result=METHOD(object,3,buffer_fn)(object,&description,&buffer,0);
    SAY("DSOUND8 stereo-3d hr/output\n"); report(result); report((DWORD)(UPTR)buffer);
    CHECK(result==INVALID,"stereo-3d-rejected");
    release(object);
    object=0;
    CHECK(old(0,&object,0)==0 && object,"legacy-create");
    if(object) {
        queried=(void *)1;
        CHECK(query(object,&ds8_iid,&queried)==NOINTERFACE && !queried,"legacy-not-device8");
        release(object);
    }
    couninit();
    if(!failures) SAY("DSOUND8 DONE\n");
    ExitProcess(failures ? 1 : 0);
}
