/* Native/guest INF navigation oracle. Files stay in the probe's directory. */
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
typedef struct {void *inf,*current;DWORD section,line;} CONTEXT;
static void *(CALL *openinf)(const char *,const char *,DWORD,DWORD *);
static void(CALL *closeinf)(void *);
static int(CALL *finda)(void *,const char *,const char *,CONTEXT *);
static int(CALL *findw)(void *,const WORD *,const WORD *,CONTEXT *);
static int(CALL *nextline)(const CONTEXT *,CONTEXT *);
static int(CALL *texta)(const CONTEXT *,void *,const char *,const char *,char *,DWORD,DWORD *);
static int(CALL *textw)(const CONTEXT *,void *,const WORD *,const WORD *,WORD *,DWORD,DWORD *);
static void(CALL *seterror)(DWORD);
static DWORD(CALL *geterror)(void);
static unsigned failures,wide;
static void *inf;
static CONTEXT context;
static const char fixture[]=
    "[Version]\r\nSignature=\"$CHICAGO$\"\r\n"
    "[Strings]\r\nSub=\"value,with spaces\"\r\nNested=\"%Sub%\"\r\n"
    "[Main]\r\nKey = first, \"second, part\", %Sub%,, ; comment\r\n"
    "Dup=one\r\ndup=two\r\nbare, \"quoted, piece\", last\r\nEmpty=\r\n"
    "Joined=alpha, \\\r\n ; comment inside continuation\r\n beta, gamma\r\n"
    "Quote=\"say \"\"hi\"\"\", \" space \"\r\nPercent=%%,%Unknown%,%Nested%\r\n"
    "PlainJoined=alpha, \\\r\n beta, gamma\r\n"
    "Concat=ab\\\r\n cd\r\nSpaced=ab \\\r\n cd\r\n"
    "QuotedBack=\"a\\\"\r\n\"Quoted Key\"=\"has key\"\r\n"
    "[Other]\r\nSkip=there\r\n[MAIN]\r\nTail=end\r\n[EmptySection]\r\n"
    "[Directories]\r\nPaths=%0%,%10%,%11%,%12%,%17%,%18%,%20%,%30%,%16425%,%99999%\r\n";
#define SAY(s) do{DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do{char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
static void infquery_report(const char *text)
{ DWORD length=0,written;while(text[length])length++;WriteFile(GetStdHandle((DWORD)-11),text,length,&written,0); }
static void infquery_check(int ok,const char *name)
{
    if(ok)SAY("INFQUERY PASS ");else{SAY("INFQUERY FAIL ");failures++;}
    infquery_report(name);SAY("\n");
}
static int infquery_find(const char *section,const char *key,unsigned mode)
{
    WORD ws[260],wk[260];unsigned i=0;
    if(section){while(section[i]){ws[i]=(unsigned char)section[i];i++;}ws[i]=0;}
    if(key){i=0;while(key[i]){wk[i]=(unsigned char)key[i];i++;}wk[i]=0;}
    unsigned char *bytes=(void *)&context;for(i=0;i<sizeof(context);i++)bytes[i]=0xcc;
    CONTEXT before=context;
    seterror(0x13579);
    int rc=wide?findw(mode==1?(void *)(UP)-1:inf,section?ws:0,key?wk:0,mode==2?0:&context):
                finda(mode==1?(void *)(UP)-1:inf,section,key,mode==2?0:&context);
    DWORD last=geterror();int same=1;for(i=0;i<sizeof(context);i++)if(bytes[i]!=((unsigned char *)&before)[i])same=0;
    SAY("INFQUERY FIND ");HEX(wide);HEX(mode);HEX(rc);HEX(last);HEX(same);
    infquery_report(section?section:"<null>");SAY("|");infquery_report(key?key:"<null>");SAY("\n");
    return rc;
}
static void infquery_text(const CONTEXT *ctx,const char *section,const char *key,unsigned capacity,unsigned nulls)
{
    WORD ws[260],wk[260];unsigned i=0;
    if(section){while(section[i]){ws[i]=(unsigned char)section[i];i++;}ws[i]=0;}
    if(key){i=0;while(key[i]){wk[i]=(unsigned char)key[i];i++;}wk[i]=0;}
    union {WORD w[512];char a[1024];} buf;for(i=0;i<sizeof(buf);i++)buf.a[i]=(char)0xcc;
    DWORD required=0xcccccccc;seterror(0x13579);
    int rc=wide?textw(ctx,inf,section?ws:0,key?wk:0,(nulls&1)?0:buf.w,capacity,(nulls&2)?0:&required):
                texta(ctx,inf,section,key,(nulls&1)?0:buf.a,capacity,(nulls&2)?0:&required);
    DWORD last=geterror();
    SAY("INFQUERY TEXT ");HEX(wide);HEX(capacity);HEX(nulls);HEX(rc);HEX(last);HEX(required);
    HEX((unsigned char)buf.a[0]);
    if(rc&&!(nulls&1)&&capacity){
        for(i=0;i<512;i++){
            WORD ch=wide?buf.w[i]:(unsigned char)buf.a[i];if(!ch)break;
            char c=(char)ch;DWORD wr;WriteFile(GetStdHandle((DWORD)-11),&c,1,&wr,0);
        }
    }
    SAY("\n");
}
void infquery_entry(void)
{
    void *s=LoadLibraryA("setupapi.dll"),*k=LoadLibraryA("kernel32.dll");
#define LOAD(dst,module,name) dst=(__typeof__(dst))GetProcAddress(module,name)
    LOAD(openinf,s,"SetupOpenInfFileA");LOAD(closeinf,s,"SetupCloseInfFile");
    LOAD(finda,s,"SetupFindFirstLineA");LOAD(findw,s,"SetupFindFirstLineW");
    LOAD(nextline,s,"SetupFindNextLine");LOAD(texta,s,"SetupGetLineTextA");LOAD(textw,s,"SetupGetLineTextW");
    LOAD(seterror,k,"SetLastError");LOAD(geterror,k,"GetLastError");
    void *(CALL *create)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
    int(CALL *close)(void *);LOAD(create,k,"CreateFileA");LOAD(close,k,"CloseHandle");
    infquery_check(openinf&&closeinf&&finda&&findw&&nextline&&texta&&textw&&seterror&&geterror&&create&&close,"exports");
    if(failures)ExitProcess(1);
    void *file=create("infquery.inf",0x40000000,7,0,2,0x80,0);DWORD written=0;
    infquery_check(file!=(void *)(UP)-1&&WriteFile(file,fixture,sizeof(fixture)-1,&written,0)&&written==sizeof(fixture)-1,"fixture");
    if(file!=(void *)(UP)-1)close(file);
    DWORD line;inf=openinf(".\\infquery.inf",0,2,&line);
    infquery_check(inf!=(void *)(UP)-1,"open");if(failures)ExitProcess(1);
    for(wide=0;wide<2;wide++){
        if(infquery_find("main",0,0))for(unsigned n=0;n<30;n++){
            infquery_text(&context,0,0,512,0);
            CONTEXT before=context;seterror(0x13579);int rc=nextline(&context,&context);DWORD last=geterror();
            int same=1;for(unsigned i=0;i<sizeof(context);i++)if(((unsigned char *)&context)[i]!=((unsigned char *)&before)[i])same=0;
            SAY("INFQUERY NEXT ");HEX(wide);HEX(n);HEX(rc);HEX(last);HEX(same);SAY("\n");if(!rc)break;
        }
        infquery_find("main","DUP",0);infquery_text(&context,0,0,512,0);
        infquery_find("main","Quoted Key",0);infquery_text(&context,0,0,512,0);
        if(infquery_find("Main","Key",0))for(unsigned n=0;n<4;n++){
            const unsigned capacities[]={0,1,44,512};
            for(unsigned flags=0;flags<4;flags++)infquery_text(&context,"ignored","ignored",capacities[n],flags);
        }
        infquery_text(0,"main","Tail",512,0);
        infquery_text(0,"Directories","Paths",512,0);
        infquery_text(0,"main",0,512,0);
        infquery_text(0,"absent","key",512,0);
        infquery_text(0,"EmptySection",0,512,0);
        infquery_find("main","Absent",0);infquery_find("Absent",0,0);infquery_find("EmptySection",0,0);
        infquery_find("main","Dup",1);infquery_find("main","Dup",2);infquery_find(0,0,0);
    }
    for(wide=0;wide<2;wide++){
        CONTEXT invalid={0};infquery_text(&invalid,0,0,512,0);
        seterror(0x13579);int rc=nextline(&invalid,&invalid);DWORD last=geterror();
        SAY("INFQUERY INVALID NEXT ");HEX(wide);HEX(rc);HEX(last);SAY("\n");
    }
    wide=0;infquery_find("Main","Key",0);closeinf(inf);
    for(wide=0;wide<2;wide++)infquery_text(&context,0,0,512,0);
    if(!failures)SAY("INFQUERY DONE\n");ExitProcess(failures?1:0);
}
