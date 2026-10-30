/* Native/guest registry string ABI probe. Only private HKCU keys are written. */
typedef unsigned int DWORD;
typedef int LONG;
typedef __WCHAR_TYPE__ WCHAR;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void CALL ExitProcess(DWORD);
API DWORD CALL GetCurrentProcessId(void);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void CALL SetLastError(DWORD);
API DWORD CALL GetLastError(void);
API LONG CALL RegCreateKeyExA(void *,const char *,DWORD,char *,DWORD,DWORD,void *,void **,DWORD *);
API LONG CALL RegSetValueExA(void *,const char *,DWORD,DWORD,const void *,DWORD);
API LONG CALL RegSetValueExW(void *,const WCHAR *,DWORD,DWORD,const void *,DWORD);
API LONG CALL RegQueryValueExA(void *,const char *,DWORD *,DWORD *,void *,DWORD *);
API LONG CALL RegQueryValueExW(void *,const WCHAR *,DWORD *,DWORD *,void *,DWORD *);
API LONG CALL RegEnumValueA(void *,DWORD,char *,DWORD *,DWORD *,DWORD *,void *,DWORD *);
API LONG CALL RegEnumValueW(void *,DWORD,WCHAR *,DWORD *,DWORD *,DWORD *,void *,DWORD *);
API LONG CALL RegQueryInfoKeyA(void *,char *,DWORD *,DWORD *,DWORD *,DWORD *,DWORD *,DWORD *,DWORD *,DWORD *,DWORD *,void *);
API LONG CALL RegQueryInfoKeyW(void *,WCHAR *,DWORD *,DWORD *,DWORD *,DWORD *,DWORD *,DWORD *,DWORD *,DWORD *,DWORD *,void *);
API LONG CALL RegDeleteTreeA(void *,const char *);
API LONG CALL RegDeleteKeyA(void *,const char *);
API LONG CALL RegCloseKey(void *);
#define HKCU ((void *)(__INTPTR_TYPE__)(int)0x80000001U)
static unsigned failures;
static void registry_strings_write(const char *text)
{
    DWORD size=0,written;
    while(text[size]) size++;
    WriteFile(GetStdHandle((DWORD)-11),text,size,&written,0);
}
static void registry_strings_check(int ok,const char *label)
{
    if(!ok) failures++;
    registry_strings_write(ok?"REGSTRING PASS ":"REGSTRING FAIL ");
    registry_strings_write(label);registry_strings_write("\n");
}
void registry_strings_entry(void)
{
    static const char hex[]="0123456789abcdef";
    static char path[]="Software\\OsitoRegistryStrings-00000000";
    DWORD pid=GetCurrentProcessId();
    for(unsigned i=0;i<8;i++)path[sizeof(path)-2-i]=hex[(pid>>(4*i))&15];
    void *key=0;
    LONG status=RegCreateKeyExA(HKCU,path,0,0,0,0xF003F,0,&key,0);
    registry_strings_check(!status&&key,"create private key");
    if(status)ExitProcess(1);
    static const char text[]="4.09.00.0904";
    static const WCHAR wide[]=L"4.09.00.0904";
    static const char expand[]="%TEMP%\\library";
    static const WCHAR expandw[]=L"%TEMP%\\library";
    static const char multi[]="alpha\0beta\0";
    static const WCHAR multiw[]=L"alpha\0beta\0";
    static const char latin[]={ 'c','a','f',(char)0xE9,0 };
    static const WCHAR latinw[]={ 'c','a','f',0xE9,0 };
    static const unsigned char binary[]={0x41,0,0xFE,0xFF,0,0x80,7};
    static const struct sample {DWORD type;const void *ansi,*wide;DWORD asize,wsize;} cases[]={
        {1,text,wide,sizeof(text),sizeof(wide)},
        {2,expand,expandw,sizeof(expand),sizeof(expandw)},
        {7,multi,multiw,sizeof(multi),sizeof(multiw)},
        {1,latin,latinw,sizeof(latin),sizeof(latinw)},
        {3,binary,binary,sizeof(binary),sizeof(binary)},
        {1,text,wide,0,0}
    };
    DWORD max_written=0;
    for(unsigned c=0;c<sizeof(cases)/sizeof(cases[0]);c++){
        const struct sample *sample=&cases[c];
        if(sample->wsize>max_written)max_written=sample->wsize;
        for(unsigned writer=0;writer<2;writer++){
            status=writer?RegSetValueExW(key,L"Value",0,sample->type,sample->wide,sample->wsize):
                          RegSetValueExA(key,"Value",0,sample->type,sample->ansi,sample->asize);
            registry_strings_check(!status,"write value");
            for(unsigned reader=0;reader<2;reader++){
                DWORD size=0,type=0;
                const unsigned char *expected=reader?sample->wide:sample->ansi;
                DWORD length=reader?sample->wsize:sample->asize;
                SetLastError(0x196713);
                status=reader?RegQueryValueExW(key,L"Value",0,&type,0,&size):RegQueryValueExA(key,"Value",0,&type,0,&size);
                DWORD last=GetLastError();
                registry_strings_check(!status&&type==sample->type&&size==length,"query byte size");
                registry_strings_check(last==0x196713,"query preserves LastError");
                struct{unsigned char data[128];DWORD guard;}result={{0},0x13579BDF};
                if(length){
                    size=length-1;
                    status=reader?RegQueryValueExW(key,L"Value",0,&type,result.data,&size):RegQueryValueExA(key,"Value",0,&type,result.data,&size);
                    registry_strings_check(status==234&&size==length&&result.guard==0x13579BDF,"short byte buffer");
                }
                size=sizeof(result.data);
                status=reader?RegQueryValueExW(key,L"Value",0,&type,result.data,&size):RegQueryValueExA(key,"Value",0,&type,result.data,&size);
                int same=1;
                for(DWORD j=0;j<length;j++)if(result.data[j]!=expected[j])same=0;
                registry_strings_check(!status&&type==sample->type&&size==length&&same&&result.guard==0x13579BDF,"query converted value");
                DWORD name_size=32;
                union{char ansi[32];WCHAR wide[32];}name;
                size=sizeof(result.data);
                status=reader?RegEnumValueW(key,0,name.wide,&name_size,0,&type,result.data,&size):RegEnumValueA(key,0,name.ansi,&name_size,0,&type,result.data,&size);
                same=1;
                for(DWORD j=0;j<length;j++)if(result.data[j]!=expected[j])same=0;
                registry_strings_check(!status&&name_size==5&&type==sample->type&&size==length&&same,"enumeration encoding");
                DWORD count=0,max_size=0;
                status=reader?RegQueryInfoKeyW(key,0,0,0,0,0,0,&count,0,&max_size,0,0):RegQueryInfoKeyA(key,0,0,0,0,0,0,&count,0,&max_size,0,0);
                /* Native Windows retains a high-water size after overwrite. */
                registry_strings_check(!status&&count==1&&max_size>=sample->wsize&&max_size<=max_written,"key size bound covers Unicode data");
            }
        }
    }
    /* UTF16-only data remains exact even when no ANSI representation exists. */
    static const WCHAR unicode[]={0x4E2D,0xD83D,0xDE03,0};
    status=RegSetValueExW(key,L"Value",0,1,unicode,sizeof(unicode));
    registry_strings_check(!status,"write non-ANSI UTF16");
    WCHAR returned[8]={0};DWORD size=sizeof(returned),type=0;
    status=RegQueryValueExW(key,L"Value",0,&type,returned,&size);
    int same=1;for(unsigned i=0;i<4;i++)if(returned[i]!=unicode[i])same=0;
    registry_strings_check(!status&&type==1&&size==sizeof(unicode)&&same,"preserve Unicode and surrogate pair");
    registry_strings_check(!RegDeleteTreeA(key,0),"remove private values");
    registry_strings_check(!RegCloseKey(key),"close private key");
    registry_strings_check(!RegDeleteKeyA(HKCU,path),"remove private key");
    registry_strings_write("REGSTRING DONE\n");
    ExitProcess(failures?1:0);
}
