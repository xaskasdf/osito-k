/* Open independently imported UTF-8 names through both Win32 W entrypoints. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define W(s) ((const WCHAR *)u##s)
#define BAD ((HANDLE)(long long)-1)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API int CALL ReadFile(HANDLE,void *,DWORD,DWORD *,void *);
API HANDLE CALL CreateFileW(const WCHAR *,DWORD,DWORD,void *,DWORD,DWORD,HANDLE);
API int CALL CloseHandle(HANDLE);
API DWORD CALL GetFileAttributesW(const WCHAR *);
API DWORD CALL GetShortPathNameW(const WCHAR *,WCHAR *,DWORD);
API int CALL GetBinaryTypeW(const WCHAR *,DWORD *);
API DWORD CALL GetLastError(void);
API const char *CALL GetCommandLineA(void);
static unsigned checks,failures;
static void say(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(DWORD v)
{char b[9];for(unsigned i=0;i<8;i++)b[i]="0123456789abcdef"[(v>>((7-i)*4))&15];b[8]=0;say(b);}
static void check(int ok,unsigned test,unsigned item)
{checks++;if(!ok){failures++;say("FILEUNICODE FAIL case=");hex(test);say(" item=");hex(item);say(" error=");hex(GetLastError());say("\n");}}
static void read_marker(const WCHAR *path,unsigned marker,unsigned test)
{
    HANDLE file=CreateFileW(path,0x80000000U,7,0,3,0,0);
    check(file!=BAD,test,1);
    if(file==BAD)return;
    unsigned char data[4]={0};DWORD got=0;
    check(ReadFile(file,data,sizeof(data),&got,0)&&got==sizeof(data)&&
          data[0]=='M'&&data[1]=='Z'&&data[2]==marker&&data[3]==0xa5,test,2);
    check(CloseHandle(file),test,3);
}
void file_unicode_entry(void)
{
    static const WCHAR *paths[]={
        W("file-unicode\\ascii.exe"),
        W("file-unicode\\caf\u00e9.exe"),
        W("file-unicode\\\u65e5\u672c.exe"),
        W("file-unicode\\face-\U0001f642.exe"),
        W("file-unicode\\ascii.exe\u0100.exe"),
        W("file-unicode\\e\u0301.exe"),
        W("file-unicode\\dir-\u65e5-\U0001f642\\child.exe")
    };
    for(unsigned i=0;i<sizeof(paths)/sizeof(*paths);i++) {
        DWORD attributes=GetFileAttributesW(paths[i]);
        check(attributes!=0xffffffffU&&!(attributes&16),i,0);
        read_marker(paths[i],i,i);
        DWORD type=0xffffffffU;
        check(GetBinaryTypeW(paths[i],&type)&&type==1,i,4);
        WCHAR short_path[260];
        DWORD length=GetShortPathNameW(paths[i],short_path,260);
        check(length>0&&length<260,i,5);
        if(length>0&&length<260)read_marker(short_path,i,100+i);
    }
    DWORD attributes=GetFileAttributesW(W("file-unicode\\dir-\u65e5-\U0001f642"));
    check(attributes!=0xffffffffU&&(attributes&16),200,0);
    static const WCHAR *created=W("file-unicode\\dir-\u65e5-\U0001f642\\new-\u0100-\U0001f680.exe");
    HANDLE file=CreateFileW(created,0x40000000U,7,0,2,0,0);
    check(file!=BAD,201,0);
    if(file!=BAD) {
        const unsigned char data[4]={'M','Z',77,0xa5};DWORD put=0;
        check(WriteFile(file,data,4,&put,0)&&put==4,201,1);
        check(CloseHandle(file),201,2);
        check(GetFileAttributesW(created)!=0xffffffffU,201,3);
        read_marker(created,77,201);
    }
    const WCHAR *missing=W("file-unicode\\missing-\u0100-\U0001f642.exe");
    check(GetFileAttributesW(missing)==0xffffffffU&&GetLastError()==2,202,0);
    check(CreateFileW(missing,0x80000000U,7,0,3,0,0)==BAD&&GetLastError()==2,202,1);
    const WCHAR *empty[]={0,W("")};
    for(unsigned i=0;i<2;i++) {
        check(GetFileAttributesW(empty[i])==0xffffffffU&&GetLastError()==3,208+i,0);
        check(CreateFileW(empty[i],0x80000000U,7,0,3,0,0)==BAD&&GetLastError()==3,208+i,1);
    }
    /* OsitoFS currently limits the encoded path to 259 bytes. Check that
     * overflow and malformed UTF-16 fail explicitly instead of aliasing. */
    int guest=0;const char *command=GetCommandLineA();
    for(unsigned i=0;command&&command[i];i++)
        if(command[i]=='-'&&command[i+1]=='-'&&command[i+2]=='g')guest=1;
    if(guest) {
        WCHAR long_name[261];for(unsigned i=0;i<260;i++)long_name[i]='a';long_name[260]=0;
        check(GetFileAttributesW(long_name)==0xffffffffU&&GetLastError()==206,203,0);
        check(CreateFileW(long_name,0x80000000U,7,0,3,0,0)==BAD&&GetLastError()==206,203,1);
        for(unsigned i=0;i<90;i++)long_name[i]=0x65e5;long_name[90]=0;
        check(GetFileAttributesW(long_name)==0xffffffffU&&GetLastError()==206,204,0);
        check(CreateFileW(long_name,0x80000000U,7,0,3,0,0)==BAD&&GetLastError()==206,204,1);
        const WCHAR invalid[][3]={{0xd800,0,0},{0xdc00,0,0},{0xd800,'x',0}};
        for(unsigned i=0;i<3;i++) {
            check(GetFileAttributesW(invalid[i])==0xffffffffU&&GetLastError()==1113,205+i,0);
            check(CreateFileW(invalid[i],0x80000000U,7,0,3,0,0)==BAD&&GetLastError()==1113,205+i,1);
        }
    }
    say("FILEUNICODE DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");
    ExitProcess(failures?1:0);
}
