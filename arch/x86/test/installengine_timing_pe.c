/* Query-only native oracle: no download or installation methods are called. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UP;
typedef int HR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
typedef struct {DWORD a;WORD b,c;unsigned char d[8];} GUID;
static const GUID clsid={0x6e449686,0xc509,0x11cf,{0xaa,0xfa,0,0xaa,0,0xb6,1,0x5c}};
static const GUID engine2={0x6e449689,0xc509,0x11cf,{0xaa,0xfa,0,0xaa,0,0xb6,1,0x5c}};
static const GUID timing={0x6e449687,0xc509,0x11cf,{0xaa,0xfa,0,0xaa,0,0xb6,1,0x5c}};
static const GUID unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static unsigned failures;
typedef HR (CALL *QUERY)(void *,const GUID *,void **);
typedef DWORD (CALL *REF)(void *);
static void timing_text(const char *text)
{DWORD n=0,w;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&w,0);}
static void timing_hex(DWORD value)
{
    char text[10];text[0]=' ';
    for(unsigned i=0;i<8;i++)text[i+1]="0123456789abcdef"[(value>>(28-i*4))&15];
    text[9]=0;timing_text(text);
}
static void timing_check(int ok,const char *name)
{
    timing_text(ok?"TIMING PASS ":"TIMING FAIL ");timing_text(name);timing_text("\n");
    if(!ok)failures++;
}
static void timing_snapshot(void *object)
{
    void **v=*(void ***)object;
    typedef HR (CALL *RATES)(void *,DWORD *,DWORD *);
    typedef HR (CALL *PROGRESS)(void *,DWORD *);
    DWORD a=0xdeadbeef,b=0xabcdef12;
    HR hr=((RATES)v[3])(object,&a,&b);
    timing_text("TIMING RATES");timing_hex((DWORD)hr);timing_hex(a);timing_hex(b);timing_text("\n");
    const DWORD sizes[]={0,4,16,20,24,0xffffffff};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++){
        DWORD data[8];for(unsigned j=0;j<8;j++)data[j]=0x12345678;
        data[0]=sizes[i];hr=((PROGRESS)v[4])(object,data);
        timing_text("TIMING PROGRESS");timing_hex((DWORD)hr);
        for(unsigned j=0;j<8;j++)timing_hex(data[j]);timing_text("\n");
    }
}
void timing_entry(void)
{
    void *ole=LoadLibraryA("ole32.dll");
    typedef HR (CALL *INIT)(void *,DWORD);
    typedef void (CALL *UNINIT)(void);
    typedef HR (CALL *CREATE)(const GUID *,void *,DWORD,const GUID *,void **);
    INIT init=GetProcAddress(ole,"CoInitializeEx");
    UNINIT uninit=GetProcAddress(ole,"CoUninitialize");
    CREATE create=GetProcAddress(ole,"CoCreateInstance");
    if(!init||!uninit||!create)ExitProcess(2);
    timing_check(init(0,2)>=0,"initialize");
    void *engine=0,*view=0;
    HR hr=create(&clsid,0,1,&engine2,&engine);
    timing_check(hr==0&&engine,"engine");if(!engine)ExitProcess(3);
    void **ev=*(void ***)engine;
    hr=((QUERY)ev[0])(engine,&timing,&view);
    timing_check(hr==0&&view,"timing-interface");
    if(view){
        void **tv=*(void ***)view;void *identity=0,*back=0,*repeat=0;
        timing_check(((QUERY)tv[0])(view,&unknown,&identity)==0&&identity==engine,"canonical-unknown");
        timing_check(((QUERY)tv[0])(view,&engine2,&back)==0&&back==engine,"timing-to-engine");
        timing_check(((QUERY)tv[0])(view,&timing,&repeat)==0&&repeat==view,"stable-timing-view");
        if(identity)((REF)(*(void ***)identity)[2])(identity);
        if(back)((REF)(*(void ***)back)[2])(back);
        if(repeat)((REF)(*(void ***)repeat)[2])(repeat);
        timing_snapshot(view);
        timing_check(((REF)ev[2])(engine)==1,"timing-retains-engine");engine=0;
        timing_snapshot(view);
        timing_check(((REF)tv[2])(view)==0,"last-reference");
    }
    if(engine)((REF)ev[2])(engine);
    uninit();if(!failures)timing_text("TIMING DONE\n");ExitProcess(failures?1:0);
}
