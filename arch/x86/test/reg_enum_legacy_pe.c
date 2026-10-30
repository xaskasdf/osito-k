/* Isolated HKCU legacy enumeration oracle; remove only a newly created key. */
typedef unsigned int DWORD;
typedef int LONG;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
typedef LONG (CALL *CREATE)(void *,const char *,DWORD,char *,DWORD,DWORD,void *,void **,DWORD *);
typedef LONG (CALL *ENUMERATE)(void *,DWORD,void *,DWORD);
typedef LONG (CALL *CLOSE)(void *);
typedef LONG (CALL *DELETE)(void *,const char *);
static unsigned failures;
#ifndef REG_ENUM_PROBE_NAME
#define REG_ENUM_PROBE_NAME "Alpha-LongName"
#endif
#define SAY(s) do {DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do {char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
static void reg_enum_check(int ok)
{if(ok)SAY("REGENUM PASS\n");else{failures++;SAY("REGENUM FAIL\n");}}
void reg_enum_entry(void)
{
    void *module=LoadLibraryA("advapi32.dll");
    CREATE create=GetProcAddress(module,"RegCreateKeyExA");
    CLOSE close=GetProcAddress(module,"RegCloseKey");DELETE erase=GetProcAddress(module,"RegDeleteKeyA");
    ENUMERATE enumerate[2]={GetProcAddress(module,"RegEnumKeyA"),GetProcAddress(module,"RegEnumKeyW")};
    reg_enum_check(create&&close&&erase&&enumerate[0]&&enumerate[1]);if(failures)ExitProcess(1);
    void *hkcu=(void *)(__INTPTR_TYPE__)(int)0x80000001U,*parent=0,*child=0;DWORD disposition=0;
    const char *path="Software\\OsitoKRegEnumLegacyProbe20261005";
    LONG status=create(hkcu,path,0,0,0,0xf003f,0,&parent,&disposition);
    reg_enum_check(status==0&&disposition==1);if(failures){if(parent)close(parent);ExitProcess(1);}
    status=create(parent,REG_ENUM_PROBE_NAME,0,0,0,0xf003f,0,&child,&disposition);
    reg_enum_check(status==0);if(child)close(child);
    static const DWORD sizes[]={0,1,13,14,15,16,40};
    for(unsigned wide=0;wide<2;wide++){
        for(unsigned i=0;i<10;i++){
            volatile unsigned char storage[88];for(unsigned n=0;n<88;n++)storage[n]=0x5a;
            DWORD capacity=i<7?sizes[i]:40,index=i==7?1:0;
            void *handle=i==8?0:(i==9?(void *)(UP)0x12345:parent);
            SetLastError(0x12345678);status=enumerate[wide](handle,index,(void *)(storage+4),capacity);
            DWORD error=GetLastError();unsigned hash=2166136261U;
            for(unsigned n=4;n<84;n++){hash^=storage[n];hash*=16777619U;}
            reg_enum_check(storage[0]==0x5a&&storage[1]==0x5a&&storage[2]==0x5a&&storage[3]==0x5a&&storage[84]==0x5a&&storage[85]==0x5a&&storage[86]==0x5a&&storage[87]==0x5a);
            SAY("REGENUM ROW ");HEX(wide);HEX(i);HEX(status);HEX(error);HEX(hash);SAY("\n");
        }
    }
    reg_enum_check(erase(parent,REG_ENUM_PROBE_NAME)==0);reg_enum_check(close(parent)==0);reg_enum_check(erase(hkcu,path)==0);
    if(!failures)SAY("REGENUM DONE\n");ExitProcess(failures?1:0);
}
