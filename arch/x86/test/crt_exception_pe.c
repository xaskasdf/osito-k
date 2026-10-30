/* Calls the installed legacy CRT through its public decorated entry points.
 * The same PE runs on Windows and Osito; no Microsoft code is embedded. */
typedef unsigned int UINT;
typedef __SIZE_TYPE__ SIZE_T;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#define THIS __attribute__((thiscall))
API void CALL ExitProcess(UINT);
API void *CALL GetStdHandle(UINT);
API int CALL WriteFile(void *,const void *,UINT,UINT *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
typedef struct {void **vt;char *message;int owned;} EXCEPTION;
typedef EXCEPTION *(THIS *CTOR)(EXCEPTION *);
typedef EXCEPTION *(THIS *MESSAGE)(EXCEPTION *,const char **);
typedef EXCEPTION *(THIS *BORROW)(EXCEPTION *,const char **,int);
typedef EXCEPTION *(THIS *COPY)(EXCEPTION *,const EXCEPTION *);
typedef void (THIS *DTOR)(EXCEPTION *);
typedef const char *(THIS *WHAT)(const EXCEPTION *);
typedef void *(THIS *DELETE)(EXCEPTION *,UINT);
static UINT checks,failures;
#define SAY(s) do {UINT n;WriteFile(GetStdHandle((UINT)-11),s,sizeof(s)-1,&n,0);}while(0)
#define HEX(v) do {UINT x=(UINT)(v),n;char h[9];for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((UINT)-11),h,9,&n,0);}while(0)
static int equal(const char *a,const char *b)
{if(!a||!b)return a==b;while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void check(int ok,UINT line)
{checks++;if(!ok){failures++;SAY("CRTEX FAIL ");HEX(line);SAY("\n");}}
#define CHECK(x) check(!!(x),__LINE__)
static void row(UINT kind,const EXCEPTION *obj,const char *original,WHAT what)
{
    SAY("CRTEX ROW ");HEX(kind);HEX(obj->owned);HEX(obj->message==0);
    HEX(obj->message==original);HEX(what(obj)==obj->message);
    const char *s=what(obj);UINT n=0;while(s&&s[n]&&n<200)n++;
    UINT written;WriteFile(GetStdHandle((UINT)-11),s,n,&written,0);SAY("\n");
    CHECK(obj->vt&&obj->vt[0]&&obj->vt[1]);
    CHECK(equal(((WHAT)obj->vt[1])(obj),what(obj)));
}
void crt_exception_entry(void)
{
    const char *n32[]={"??0exception@@QAE@XZ","??0exception@@QAE@ABQBD@Z",
      "??0exception@@QAE@ABQBDH@Z","??0exception@@QAE@ABV0@@Z",
      "??1exception@@UAE@XZ","?what@exception@@UBEPBDXZ"};
    const char *n64[]={"??0exception@@QEAA@XZ","??0exception@@QEAA@AEBQEBD@Z",
      "??0exception@@QEAA@AEBQEBDH@Z","??0exception@@QEAA@AEBV0@@Z",
      "??1exception@@UEAA@XZ","?what@exception@@UEBAPEBDXZ"};
    const char **names=sizeof(void*)==4?n32:n64;
    void *crt=LoadLibraryA("msvcrt.dll"),*fn[6];CHECK(crt!=0);
    for(UINT i=0;i<6;i++){fn[i]=GetProcAddress(crt,names[i]);CHECK(fn[i]!=0);}
    if(failures)ExitProcess(1);
    CTOR ctor=fn[0];MESSAGE message=fn[1];BORROW borrow=fn[2];
    COPY copy=fn[3];DTOR dtor=fn[4];WHAT what=fn[5];
    void *(*alloc)(SIZE_T)=GetProcAddress(crt,"malloc");
    void (*release)(void*)=GetProcAddress(crt,"free");
    int *(*error)(void)=GetProcAddress(crt,"_errno");
    CHECK(alloc&&release&&error);if(failures)ExitProcess(1);
    EXCEPTION a,b;const char *null=0;char text[]="native exception message";const char *s=text;
    *error()=123;CHECK(ctor(&a)==&a);row(0,&a,0,what);CHECK(*error()==123);
    /* Inspect the public MSVC object metadata without calling RTTI helpers. */
    UINT *locator=a.vt[-1];CHECK(locator!=0);
    if(locator){
        SAY("CRTEX RTTI ");for(UINT i=0;i<3;i++)HEX(locator[i]);
        SIZE_T base=sizeof(void*)==4?0:(SIZE_T)locator-locator[5];
        char *type=(char*)(base+locator[3]);UINT *hierarchy=(UINT*)(base+locator[4]);
        for(UINT i=0;i<3;i++)HEX(hierarchy[i]);
        UINT *bases=(UINT*)(base+hierarchy[3]);UINT *desc=(UINT*)(base+bases[0]);
        for(UINT i=1;i<6;i++)HEX(desc[i]);
        const char *name=type+2*sizeof(void*);UINT n=0,w;while(name[n]&&n<80)n++;
        WriteFile(GetStdHandle((UINT)-11),name,n,&w,0);SAY("\n");
    }
    CHECK(copy(&b,&a)==&b);row(1,&b,0,what);dtor(&b);dtor(&a);
    CHECK(message(&a,&null)==&a);row(2,&a,0,what);dtor(&a);
    CHECK(message(&a,&s)==&a);row(3,&a,s,what);
    CHECK(a.owned==1&&a.message!=s&&equal(what(&a),s));
    CHECK(copy(&b,&a)==&b);row(4,&b,a.message,what);
    CHECK(b.message!=a.message&&equal(what(&a),what(&b)));
    a.message[0]='N';CHECK(b.message[0]=='n');dtor(&a);
    CHECK(equal(what(&b),s));dtor(&b);
    const int flags[]={0,1,-1,42};
    for(UINT i=0;i<4;i++){
        CHECK(borrow(&a,&s,flags[i])==&a);row(5+i*2,&a,s,what);
        CHECK(a.owned==0&&a.message==s);
        CHECK(copy(&b,&a)==&b);row(6+i*2,&b,a.message,what);
        CHECK(b.message==s&&b.owned==0);text[0]='N';CHECK(what(&b)[0]=='N');text[0]='n';
        dtor(&b);dtor(&a);CHECK(equal(s,"native exception message"));
    }
    CHECK(borrow(&a,&null,7)==&a);row(13,&a,0,what);dtor(&a);
    const char *empty="";CHECK(message(&a,&empty)==&a);row(14,&a,empty,what);
    CHECK(a.owned==1&&a.message!=empty&&equal(what(&a),empty));
    DELETE del=(DELETE)a.vt[0];CHECK(del(&a,0)==&a);
    /* Virtual scalar and array deleting destructors own the object storage. */
    EXCEPTION *heap=alloc(sizeof(*heap));CHECK(heap!=0);
    if(heap){message(heap,&s);CHECK(((DELETE)heap->vt[0])(heap,1)==heap);}
    SIZE_T *array=alloc(sizeof(SIZE_T)+3*sizeof(EXCEPTION));CHECK(array!=0);
    if(array){*array=3;EXCEPTION *items=(EXCEPTION*)(array+1);
        for(UINT i=0;i<3;i++)message(items+i,&s);
        CHECK(((DELETE)items->vt[0])(items,3)==array);}
    /* Repeat construction/destruction to expose ownership and stack errors. */
    for(UINT i=0;i<128;i++){message(&a,&s);copy(&b,&a);CHECK(equal(what(&b),s));dtor(&a);dtor(&b);}
    SAY("CRTEX checks=");HEX(checks);SAY("failures=");HEX(failures);SAY("\n");
    ExitProcess(failures?1:0);
}
