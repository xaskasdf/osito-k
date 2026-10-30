/* Query-only native InstallEngine2 oracle. No Download/Install/extra command. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UPTR;
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
static unsigned failures,checks;
static int original_catalog;
static void ie_report(const char *s)
{ DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0); }
static void ie_number(UPTR n)
{ char b[2*sizeof(UPTR)+1];unsigned i=sizeof(b)-1;b[i]=0;do{unsigned d=(unsigned)(n&15);b[--i]=(char)(d<10?'0'+d:'a'+d-10);n>>=4;}while(n);ie_report("0x");ie_report(b+i); }
static void ie_result(const char *name,HR status)
{ ie_report("INSENG ");ie_report(name);ie_report(" hr=");ie_number((DWORD)status);ie_report("\n"); }
static int ie_equal(const char *a,const char *b)
{ while(*a&&*a==*b){a++;b++;}return *a==*b; }
static void ie_check(const char *name,int passed)
{ checks++;if(!passed)failures++;ie_report(passed?"INSENG PASS ":"INSENG FAIL ");ie_report(name);ie_report("\n"); }
static void ie_text(void *object,unsigned index,const char *label)
{
    char text[260];for(unsigned i=0;i<260;i++)text[i]='#';text[259]=0;
    typedef HR (CALL *fn)(void *,char *,DWORD);
    HR status=((fn)(*(void ***)object)[index])(object,text,260);
    ie_result(label,status);if(status>=0){ie_report("INSENG TEXT ");ie_report(label);ie_report("=");ie_report(text);ie_report("\n");}
    const char *expected=0;
    if(ie_equal(label,"file-description"))expected=original_catalog?"%SetupTitle%":"Neutral title";
    if(ie_equal(label,"id"))expected=original_catalog?"DXUpdate":"ComponentA";
    if(ie_equal(label,"guid"))expected=original_catalog?"{44BBA855-CC51-11CF-AAFA-00AA00B6015C}":"{12345678-1234-5678-9ABC-001122334455}";
    if(ie_equal(label,"description"))expected=original_catalog?"%DirectX_Setup%":"Neutral component";
    if(ie_equal(label,"details"))expected=original_catalog?"%DirectX_Desc%":"Component details";
    if(ie_equal(label,"locale"))expected=original_catalog?"en":"EN";
    if(ie_equal(label,"group"))expected=original_catalog?"DirectX":"GroupA";
    if(expected)ie_check(label,status==0&&ie_equal(text,expected));
    if(ie_equal(label,"detection-dlls")||ie_equal(label,"uninstall-key")||ie_equal(label,"success-key"))ie_check(label,(DWORD)status==0x80004005);
}
void installengine_entry(void)
{
    void *ole=LoadLibraryA("ole32.dll"),*k=LoadLibraryA("kernel32.dll");
    typedef char *(CALL *commandfn)(void);
    commandfn command=GetProcAddress(k,"GetCommandLineA");
    const char *line=command();int original=0;
    for(unsigned i=0;line[i];i++)if(line[i]==' '&&line[i+1]=='o')original=1;
    original_catalog=original;
    const char *component_id=original?"DXUpdate":"ComponentA";
    typedef HR (CALL *initfn)(void *,DWORD);
    typedef void (CALL *uninitfn)(void);
    typedef HR (CALL *createfn)(const GUID *,void *,DWORD,const GUID *,void **);
    typedef HR (CALL *statusfn)(void *,DWORD *);
    typedef HR (CALL *stringfn)(void *,const char *);
    typedef HR (CALL *ciffn)(void *,void **);
    typedef DWORD (CALL *reffn)(void *);
    initfn init=GetProcAddress(ole,"CoInitializeEx");
    uninitfn uninit=GetProcAddress(ole,"CoUninitialize");
    createfn create=GetProcAddress(ole,"CoCreateInstance");
    if(!init||!uninit||!create){ie_report("INSENG MISSING COM\n");ExitProcess(1);}
    HR hr=init(0,2);ie_result("initialize",hr);
    void *engine=0;hr=create(&clsid,0,1,&engine2,&engine);ie_result("activate",hr);
    ie_check("activation",hr==0&&engine!=0);
    if(hr<0||!engine){uninit();ExitProcess(2);}
    void **v=*(void ***)engine;DWORD state=0xdead;
    hr=((statusfn)v[3])(engine,&state);ie_result("initial-status",hr);ie_number(state);ie_report("\n");
    ie_check("initial-state",hr==0&&state==0);
    void *cif=0;hr=((ciffn)v[26])(engine,&cif);ie_result("initial-cif",hr);
    ie_check("initial-cif-object",hr==0&&cif!=0);
    if(cif)((reffn)(*(void ***)cif)[2])(cif);
    hr=((stringfn)v[25])(engine,"C:\\missing-installengine-file.cif");ie_result("missing-local-cif",hr);
    ie_check("missing-cif-fails",(DWORD)hr==0x80004005);
    typedef DWORD (CALL *modulefn)(void *,char *,DWORD);
    modulefn module=GetProcAddress(k,"GetModuleFileNameA");
    char path[260];DWORD length=module(0,path,260),slash=0;
    for(DWORD i=0;i<length;i++)if(path[i]=='\\'||path[i]=='/')slash=i+1;
    const char *name=original?"dxwsetup.cif":"engine-probe.cif";unsigned i=0;while(name[i]){path[slash+i]=name[i];i++;}path[slash+i]=0;
    hr=((stringfn)v[25])(engine,path);ie_result("set-local-cif",hr);
    ie_check("load-catalog",hr==0);
    hr=((statusfn)v[3])(engine,&state);ie_result("loaded-status",hr);ie_number(state);ie_report("\n");
    ie_check("ready-state",hr==0&&state==3);
    cif=0;hr=((ciffn)v[26])(engine,&cif);ie_result("loaded-cif",hr);
    ie_check("loaded-cif-object",hr==0&&cif!=0);
    if(cif){
        void **cv=*(void ***)cif;
        ie_text(cif,9,"file-description");ie_text(cif,10,"detection-dlls");
        typedef HR (CALL *findfn)(void *,const char *,void **);
        void *component=0;hr=((findfn)cv[4])(cif,component_id,&component);ie_result("find-component",hr);
        ie_check("find-component",hr==0&&component!=0);
        if(component){
            void **c=*(void ***)component;
            ie_text(component,0,"id");ie_text(component,1,"guid");ie_text(component,2,"description");
            ie_text(component,3,"details");ie_text(component,9,"locale");ie_text(component,10,"uninstall-key");
            ie_text(component,14,"success-key");ie_text(component,23,"group");
            typedef HR (CALL *textfn)(void *,char *,DWORD);
            for(DWORD size=0;size<5;size++){
                char small[8];for(unsigned n=0;n<8;n++)small[n]='#';
                hr=((textfn)c[0])(component,small,size);ie_result("small-id",hr);
                int correct=hr==0;
                for(unsigned n=0;n<8;n++){char want=n>=size?'#':n==size-1?0:component_id[n];if(small[n]!=want)correct=0;}
                ie_check("truncated-id-and-guard",correct);
                ie_number(size);ie_report(" ");for(unsigned n=0;n<8;n++){ie_number((unsigned char)small[n]);ie_report(" ");}ie_report("\n");
            }
            typedef HR (CALL *urlfn)(void *,unsigned,char *,DWORD,DWORD *);
            for(unsigned index=0;index<4;index++){
                char url[260];url[0]=0;DWORD flags=0xdead;hr=((urlfn)c[4])(component,index,url,260,&flags);
                ie_result("url",hr);ie_report("INSENG URL index=");ie_number(index);ie_report(" flags=");ie_number(flags);ie_report(" ");ie_report(url);ie_report("\n");
                if(index==0)ie_check("first-url",hr==0&&flags==3&&ie_equal(url,original?"dxupdate.cab":"alpha.cab"));
                else if(!original&&index==1)ie_check("second-url",hr==0&&flags==0&&ie_equal(url,"http://example.invalid/beta.cab"));
                else ie_check("missing-url-preserves-flags",(DWORD)hr==0x80004005&&flags==0xdead);
            }
            typedef HR (CALL *pairfn)(void *,DWORD *,DWORD *);
            DWORD first=0xdead,second=0xbeef;
            hr=((pairfn)c[8])(component,&first,&second);ie_result("version",hr);ie_number(first);ie_report(" ");ie_number(second);ie_report("\n");
            ie_check("packed-version",hr==0&&first==(original?0x9001dU:0x10002U)&&second==(original?0x07b60000U:0x30004U));
            hr=((pairfn)c[11])(component,&first,&second);ie_result("installed-size",hr);ie_number(first);ie_report(" ");ie_number(second);ie_report("\n");
            ie_check("estimated-installed-size",hr==0&&first==0&&second==(original?162U:46U));
            const unsigned slots[]={12,13,16,17,18,19,21,24,29,30,32,34,35};
            const DWORD fixture_values[]={23,17,1,0,0,7,10,0,0,1,0,23,0x01300007};
            const DWORD original_values[]={81,0,1,1,0,0,74,0,0,1,0,81,0x06400000};
            for(unsigned j=0;j<sizeof(slots)/sizeof(slots[0]);j++){
                DWORD value=((reffn)c[slots[j]])(component);
                ie_check("component-metadata",value==(original?original_values[j]:fixture_values[j]));
                ie_report("INSENG COMPONENT slot=");ie_number(slots[j]);ie_report(" value=");ie_number(value);ie_report("\n");
            }
        }
        typedef HR (CALL *enumfn)(void *,void **,DWORD,void *);
        void *enumerator=0;hr=((enumfn)cv[3])(cif,&enumerator,0,0);ie_result("enumerate-components",hr);
        if(enumerator){
            void **ev=*(void ***)enumerator;
            for(unsigned n=0;n<5;n++){
                component=0;hr=((ciffn)ev[3])(enumerator,&component);ie_result("next-component",hr);
                if(!component)break;ie_text(component,0,"enumerated-id");
            }
            ((reffn)ev[2])(enumerator);
        }
        ((reffn)cv[2])(cif);
    }
    typedef HR (CALL *actionfn)(void *,const char *,DWORD,DWORD);
    typedef HR (CALL *enumidfn)(void *,unsigned,char **);
    hr=((actionfn)v[12])(engine,"Missing",1,100);ie_result("missing-action",hr);
    ie_check("missing-action",(DWORD)hr==0x80070057);
    hr=((actionfn)v[12])(engine,component_id,1,77);ie_result("select-component",hr);
    ie_check("select-component",hr==0);
    typedef HR (CALL *sizesfn)(void *,const char *,void *);
    DWORD sizes[11];for(unsigned n=0;n<11;n++)sizes[n]=0;sizes[0]=sizeof(sizes);
    hr=((sizesfn)v[13])(engine,component_id,sizes);ie_result("sizes",hr);
    int sizes_ok=hr==0&&sizes[0]==32&&sizes[1]==(original?162U:46U)&&sizes[2]==0&&sizes[3]==(original?81U:23U);
    for(unsigned n=4;n<11;n++)if(sizes[n])sizes_ok=0;
    ie_check("sizes-layout",sizes_ok);
    for(unsigned n=0;n<11;n++){ie_number(sizes[n]);ie_report(" ");}ie_report("\n");
    for(unsigned slot=7;slot<=8;slot++)for(unsigned n=0;n<3;n++){
        char *id=0;hr=((enumidfn)v[slot])(engine,n,&id);ie_result(slot==7?"install-id":"download-id",hr);
        if(id){ie_report("INSENG SELECTED ");ie_report(id);ie_report("\n");}
        ie_check("selected-id-enumeration",n==0?hr==0&&id&&ie_equal(id,component_id):(DWORD)hr==0x80004005);
    }
    hr=((stringfn)v[16])(engine,"https://example.invalid/unused/");ie_result("set-base-url",hr);
    ie_check("base-url",hr==0);
    name="engine-download-probe";i=0;
    while(name[i]){path[slash+i]=name[i];i++;}path[slash+i]=0;
    hr=((stringfn)v[17])(engine,path);ie_result("set-download-dir",hr);
    ie_check("download-directory",hr==0);
    const DWORD actions[]={0,1,2,3,4,0x100,0xffffffffU};
    for(unsigned n=0;n<sizeof(actions)/sizeof(actions[0]);n++){
        hr=((actionfn)v[12])(engine,component_id,actions[n],77);
        ie_result("action-policy",hr);ie_number(actions[n]);ie_report("\n");
        ie_check("action-validation",actions[n]<=1?hr==0:(DWORD)hr==0x80070057);
    }
    DWORD refs=((reffn)v[2])(engine);ie_report("INSENG RELEASE ");ie_number(refs);ie_report("\n");
    ie_check("final-release",refs==0);
    uninit();ie_report("INSENG DONE checks=");ie_number(checks);ie_report(" failures=");ie_number(failures);ie_report("\n");ExitProcess(failures?1:0);
}
