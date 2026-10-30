/* Custom profile oracle. The optional original INF is read as data only. */
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
typedef DWORD(CALL *SECTION)(const void *,void *,DWORD,const void *);
typedef DWORD(CALL *STRING)(const void *,const void *,const void *,void *,DWORD,const void *);
static SECTION sections[2];
static STRING strings[2];
static unsigned failures;
static char path[260];static WORD wpath[260];
static union {char a[32768];WORD w[16384];} buffer;
static void bare_text(const char *text)
{DWORD n=0,w;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&w,0);}
static void bare_hex(DWORD value)
{
    char text[10];text[0]=' ';
    for(unsigned i=0;i<8;i++)text[i+1]="0123456789abcdef"[(value>>(28-i*4))&15];
    text[9]=0;bare_text(text);
}
static void bare_check(int ok,const char *name)
{
    bare_text(ok?"BARE PASS ":"BARE FAIL ");bare_text(name);bare_text("\n");if(!ok)failures++;
}
static void bare_query(const char *label,const char *section)
{
    WORD ws[128],fallback[]={'d','e','f','a','u','l','t',0},bare[]={'B','a','r','e',0},key[]={'K','e','y',0};
    unsigned j=0;while(section[j]){ws[j]=(unsigned char)section[j];j++;}ws[j]=0;
    for(unsigned wide=0;wide<2;wide++)for(unsigned mode=0;mode<4;mode++){
        const void *name=wide?(const void *)ws:section,*file=wide?(const void *)wpath:path;
        DWORD needed=mode==0?sections[wide](name,&buffer,16384,file):strings[wide](name,mode==1?0:wide?(mode==2?bare:key):(const void *)(mode==2?"Bare":"Key"),wide?(const void *)fallback:"default",&buffer,16384,file);
        const DWORD capacities[]={0,1,2,5,16,needed,needed+1,needed+2,16384};
        for(unsigned c=0;c<sizeof(capacities)/sizeof(capacities[0]);c++){
        DWORD capacity=capacities[c];for(j=0;j<sizeof(buffer);j++)buffer.a[j]=(char)0xcc;
        DWORD result=mode==0?sections[wide](name,&buffer,capacity,file):strings[wide](name,mode==1?0:wide?(mode==2?bare:key):(const void *)(mode==2?"Bare":"Key"),wide?(const void *)fallback:"default",&buffer,capacity,file);
        DWORD span=result+1;
        if(result&&mode<=1&&capacity>=2&&needed+1>=capacity)span=capacity;
        if(span>capacity)span=capacity;DWORD hash=2166136261U,zeros=0;
        for(j=0;j<span;j++){WORD ch=wide?buffer.w[j]:(unsigned char)buffer.a[j];hash=(hash^ch)*16777619U;if(!ch)zeros++;}
        unsigned guard=1;for(j=capacity*(wide?2:1);j<sizeof(buffer)&&j<capacity*(wide?2:1)+16;j++)if((unsigned char)buffer.a[j]!=0xcc)guard=0;
        bare_text("BARE ROW ");bare_text(label);bare_hex(wide);bare_hex(mode);bare_hex(capacity);bare_hex(result);bare_hex(hash);bare_hex(zeros);bare_hex(guard);bare_text(" |");
        for(j=0;j<span&&j<100;j++){WORD ch=wide?buffer.w[j]:(unsigned char)buffer.a[j];char s[2]={ch==0?'~':ch>=32&&ch<127?(char)ch:'?',0};bare_text(s);}
        bare_text("\n");
        }
    }
}
void profile_bare_entry(void)
{
    void *k=LoadLibraryA("kernel32.dll");
#define LOAD(dst,name) dst=(__typeof__(dst))GetProcAddress(k,name)
    void *(CALL *create)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
    int(CALL *close)(void *);DWORD(CALL *fullpath)(const char *,DWORD,char *,char **);
    int(CALL *writekey)(const char *,const char *,const char *,const char *);
    int(CALL *writesection)(const char *,const char *,const char *);
    const char *(CALL *cmdline)(void);
    LOAD(sections[0],"GetPrivateProfileSectionA");LOAD(sections[1],"GetPrivateProfileSectionW");
    LOAD(strings[0],"GetPrivateProfileStringA");LOAD(strings[1],"GetPrivateProfileStringW");
    LOAD(create,"CreateFileA");LOAD(close,"CloseHandle");LOAD(fullpath,"GetFullPathNameA");
    LOAD(writekey,"WritePrivateProfileStringA");LOAD(writesection,"WritePrivateProfileSectionA");LOAD(cmdline,"GetCommandLineA");
    bare_check(sections[0]&&sections[1]&&strings[0]&&strings[1]&&create&&close&&fullpath&&writekey&&writesection&&cmdline,"exports");
    if(failures)ExitProcess(1);
    unsigned original=0;for(const char *p=cmdline();*p;p++)if(p[0]==' '&&p[1]=='o'&&p[2]=='r'&&p[3]=='i'&&p[4]=='g')original=1;
    DWORD length=fullpath(original?".\\dxupdate.inf":".\\profile-bare.ini",260,path,0);
    bare_check(length&&length<260,"path");if(failures)ExitProcess(1);
    for(unsigned i=0;i<=length;i++)wpath[i]=(unsigned char)path[i];
    if(original)bare_query("original","DirectX_Update_PlugIn");
    else{
        static const char fixture[]="[Main]\r\nfirst.cab,    1\r\n  two.cab,  9  \r\nBare\r\nKey=value\r\nEmpty=\r\n[Other]\r\nelsewhere\r\n;comment\r\n#hash\r\n";
        void *file=create(path,0x40000000,7,0,2,0x80,0);DWORD written=0;
        bare_check(file!=(void *)(UP)-1&&WriteFile(file,fixture,sizeof(fixture)-1,&written,0)&&written==sizeof(fixture)-1,"fixture");if(file!=(void *)(UP)-1)close(file);
        bare_query("initial","Main");bare_query("hash","Other");
        bare_check(writekey("Main","Bare","changed",path),"set-bare");bare_query("changed","Main");
        bare_check(writekey("Main","first.cab,    1",0,path),"delete-bare");bare_query("deleted","Main");
        bare_check(writesection("Main","bare-again\0Bare\0Key=other\0last, 1\0",path),"write-section");bare_query("replaced","Main");
    }
    if(!failures)bare_text("BARE DONE\n");ExitProcess(failures?1:0);
}
