typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define W(s) ((const WCHAR *)u##s)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API char *CALL GetCommandLineA(void);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API int CALL CloseHandle(HANDLE);
API HANDLE CALL CreateFileW(const WCHAR *,DWORD,DWORD,void *,DWORD,DWORD,HANDLE);
API DWORD CALL GetFileAttributesW(const WCHAR *);
API int CALL DeleteFileW(const WCHAR *);
API int CALL Wow64DisableWow64FsRedirection(void **);
API int CALL Wow64RevertWow64FsRedirection(void *);
API int CALL MsiGetComponentPathW(const WCHAR *,const WCHAR *,WCHAR *,DWORD *);
API int CALL MsiLocateComponentW(const WCHAR *,WCHAR *,DWORD *);
API DWORD CALL MsiEnumProductsW(DWORD,WCHAR *);
API DWORD CALL RegCreateKeyExW(HANDLE,const WCHAR *,DWORD,WCHAR *,DWORD,DWORD,void *,HANDLE *,DWORD *);
API DWORD CALL RegSetValueExW(HANDLE,const WCHAR *,DWORD,DWORD,const void *,DWORD);
API DWORD CALL RegDeleteValueW(HANDLE,const WCHAR *);
API DWORD CALL RegDeleteKeyExW(HANDLE,const WCHAR *,DWORD,DWORD);
API DWORD CALL RegCloseKey(HANDLE);
static unsigned checks,failures;
static const WCHAR *unknown=W("{B08BCF70-2BA1-4E13-B55B-20C7B79716F3}");
static const WCHAR *fixture_component=W("{05D04956-DA2E-437E-AE27-A78FB9AB2C09}");
static void component_say(const char *s)
{DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void component_hex(DWORD value)
{char s[9];for(unsigned i=0;i<8;i++)s[i]="0123456789abcdef"[(value>>((7-i)*4))&15];s[8]=0;component_say(s);}
static void component_check(int ok,unsigned line)
{checks++;if(!ok){failures++;component_say("MSICOMPONENT FAIL line=");component_hex(line);component_say("\n");}}
#define CHECK(c) component_check((c),__LINE__)
static DWORD component_length_test(const WCHAR *s)
{DWORD n=0;while(s[n])n++;return n;}
static int component_equal_test(const WCHAR *a,const WCHAR *b)
{while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void component_print_path(const WCHAR *path)
{for(unsigned i=0;path[i]&&i<512;i++){char text[2]={(char)(path[i]<128?path[i]:'?'),0};component_say(text);}component_say("\n");}
static int component_mode(const char *wanted)
{const char *cmd=GetCommandLineA();for(unsigned i=0;cmd[i];i++){unsigned j=0;while(wanted[j]&&cmd[i+j]==wanted[j])j++;if(!wanted[j])return 1;}return 0;}
#include "msi_components_cases.h"
static void component_negative(void)
{
    for(unsigned i=0;i<sizeof(component_negative_cases)/sizeof(*component_negative_cases);i++) {
        const COMPONENT_NEGATIVE *test=&component_negative_cases[i];WCHAR text[64];
        for(unsigned j=0;j<64;j++)text[j]='Z';DWORD size=64;
        int result=test->locate?MsiLocateComponentW(test->component,text,&size):MsiGetComponentPathW(test->product,test->component,text,&size);
        CHECK(result==test->result);CHECK(size==64);
        for(unsigned j=0;j<64;j++)CHECK(text[j]=='Z');
    }
}
static void component_buffer_checks(const WCHAR *product,const WCHAR *component,const WCHAR *expected,int state)
{
    DWORD length=component_length_test(expected),capacities[]={0,1,length,length+1,256};
    CHECK(length<256);
    for(unsigned locate=0;locate<2;locate++)for(unsigned j=0;j<5;j++)for(unsigned nulls=0;nulls<4;nulls++) {
        WCHAR text[256];for(unsigned k=0;k<256;k++)text[k]='Z';
        DWORD size=capacities[j];WCHAR *buffer=nulls&1?0:text;DWORD *count=nulls&2?0:&size;
        int result=locate?MsiLocateComponentW(component,buffer,count):MsiGetComponentPathW(product,component,buffer,count);
        int want=!count&&buffer?-2:buffer&&length&&capacities[j]<=length?-3:state;
        CHECK(result==want);CHECK(size==(count?length:capacities[j]));
        for(unsigned k=0;k<256;k++){
            WCHAR c=buffer&&count&&capacities[j]>length&&k<=length?expected[k]:'Z';
            CHECK(text[k]==c);
        }
    }
}
static void component_installed(void)
{
    for(unsigned i=0;i<sizeof(component_installed_cases)/sizeof(*component_installed_cases);i++) {
        const COMPONENT_INSTALLED *test=&component_installed_cases[i];WCHAR path[512];DWORD size=512;
        CHECK(MsiGetComponentPathW(test->product,test->component,path,&size)==3);
        component_say("MSICOMPONENT PATH ");component_print_path(path);
        CHECK(size==component_length_test(test->path)&&component_equal_test(path,test->path));
        size=512;CHECK(MsiLocateComponentW(test->component,path,&size)==3);
        CHECK(size==component_length_test(test->path)&&component_equal_test(path,test->path));
    }
    component_buffer_checks(component_installed_cases[0].product,component_installed_cases[0].component,component_installed_cases[0].path,3);
    WCHAR first[39],next[39];DWORD size=0;
    CHECK(MsiEnumProductsW(0,first)==0);
    CHECK(MsiLocateComponentW(component_installed_cases[0].component,0,&size)==3);
    CHECK(MsiEnumProductsW(1,next)==0&&!component_equal_test(first,next));
    CHECK(MsiEnumProductsW(2,next)==259);
}
static void component_native_buffers(void)
{
    /* Read an existing native installation; never install or mutate the host. */
    HANDLE msi=LoadLibraryA("msi.dll");
    DWORD (CALL *enumerate)(DWORD,WCHAR *)=GetProcAddress(msi,"MsiEnumComponentsW");
    DWORD (CALL *clients)(const WCHAR *,DWORD,WCHAR *)=GetProcAddress(msi,"MsiEnumClientsW");
    CHECK(enumerate&&clients);if(!enumerate||!clients)return;
    WCHAR component[39],product[39],path[256];int found=0;
    for(DWORD i=0;i<4000;i++) {
        if(enumerate(i,component))break;
        if(clients(component,0,product))continue;
        DWORD size=256;
        if(MsiGetComponentPathW(product,component,path,&size)!=3||size>=256)continue;
        /* A product-specific query and Locate can legitimately choose different
         * clients. Select a component whose complete native paths agree. */
        WCHAR located[256];DWORD capacity=256;
        if(MsiLocateComponentW(component,located,&capacity)!=3||!component_equal_test(path,located))continue;
        component_buffer_checks(product,component,path,3);found=1;break;
    }
    CHECK(found);
}
static void component_reg_number(HANDLE key,const WCHAR *name,DWORD value)
{CHECK(RegSetValueExW(key,name,0,4,&value,4)==0);}
static void component_reg_string(HANDLE key,const WCHAR *name,const WCHAR *value)
{CHECK(RegSetValueExW(key,name,0,1,value,(component_length_test(value)+1)*2)==0);}
static void component_metadata(void)
{
    HANDLE root=(HANDLE)(UPTR)0x80000002U,product=0,components=0,component=0,registry=0;
    const WCHAR *product_path=W("Software\\OsitoK\\Installer\\Products\\{B08BCF70-2BA1-4E13-B55B-20C7B79716F3}");
    const WCHAR *component_path=W("Components\\{05D04956-DA2E-437E-AE27-A78FB9AB2C09}");
    const WCHAR *file_path=W("C:\\Windows\\System32\\osito-msi-05D04956.tmp");
    const WCHAR *reg_path=W("Software\\OsitoK\\MsiComponent05D04956");
    CHECK(RegCreateKeyExW(root,product_path,0,0,0,0xF013F,0,&product,0)==0);
    component_reg_number(product,W("SchemaVersion"),1);component_reg_number(product,W("Context"),4);component_reg_number(product,W("State"),5);
    /* Make the fixture's parent explicit: the current flat registry does not
     * persist implicit parents once their last descendant is removed. */
    CHECK(RegCreateKeyExW(product,W("Components"),0,0,0,0xF013F,0,&components,0)==0);
    CHECK(RegCloseKey(components)==0);
    CHECK(RegCreateKeyExW(product,component_path,0,0,0,0xF013F,0,&component,0)==0);
    component_reg_number(component,W("State"),3);component_reg_number(component,W("Kind"),1);component_reg_string(component,W("Path"),file_path);
    void *previous=0;
    if(sizeof(void *)==4)CHECK(Wow64DisableWow64FsRedirection(&previous));
    HANDLE file=CreateFileW(file_path,0x40000000,0,0,1,0x80,0);
    CHECK(file!=(HANDLE)(UPTR)-1);if(file!=(HANDLE)(UPTR)-1)CHECK(CloseHandle(file));
    if(sizeof(void *)==4)CHECK(Wow64RevertWow64FsRedirection(previous));
    if(sizeof(void *)==4)CHECK(GetFileAttributesW(file_path)==0xffffffffU);
    component_buffer_checks(unknown,fixture_component,file_path,3);
    if(sizeof(void *)==4)CHECK(GetFileAttributesW(file_path)==0xffffffffU);
    if(sizeof(void *)==4)CHECK(Wow64DisableWow64FsRedirection(&previous));
    CHECK(DeleteFileW(file_path));
    if(sizeof(void *)==4)CHECK(Wow64RevertWow64FsRedirection(previous));
    component_buffer_checks(unknown,fixture_component,file_path,2);
    component_reg_number(component,W("Kind"),3);component_reg_string(component,W("Path"),W("C:\\Windows\\System32\\"));
    component_buffer_checks(unknown,fixture_component,W("C:\\Windows\\System32\\"),3);
    component_reg_number(component,W("Kind"),2);component_reg_number(component,W("Root"),2);component_reg_number(component,W("View"),0x200);component_reg_number(component,W("KeyOnly"),0);
    component_reg_string(component,W("Path"),reg_path);component_reg_string(component,W("Name"),W("Value\\with\\slashes"));
    CHECK(RegCreateKeyExW(root,reg_path,0,0,0,0xF023F,0,&registry,0)==0);
    component_reg_string(registry,W("Value\\with\\slashes"),W("present"));
    component_buffer_checks(unknown,fixture_component,W("02:\\Software\\OsitoK\\MsiComponent05D04956\\Value\\with\\slashes"),3);
    CHECK(RegDeleteValueW(registry,W("Value\\with\\slashes"))==0);
    component_buffer_checks(unknown,fixture_component,W("02:\\Software\\OsitoK\\MsiComponent05D04956\\Value\\with\\slashes"),2);
    component_reg_number(component,W("KeyOnly"),1);
    component_buffer_checks(unknown,fixture_component,W("02:\\Software\\OsitoK\\MsiComponent05D04956\\"),3);
    component_reg_number(component,W("View"),0x100);
    component_buffer_checks(unknown,fixture_component,W("22:\\Software\\OsitoK\\MsiComponent05D04956\\"),2);
    component_reg_number(component,W("State"),2);
    component_buffer_checks(unknown,fixture_component,W(""),-7);
    component_reg_number(component,W("State"),3);component_reg_number(component,W("Kind"),99);
    WCHAR text[8];text[0]='Z';DWORD size=8;
    CHECK(MsiGetComponentPathW(unknown,fixture_component,text,&size)==-6&&size==8&&text[0]=='Z');
    CHECK(RegCloseKey(registry)==0);CHECK(RegDeleteKeyExW(root,reg_path,0x200,0)==0);
    CHECK(RegCloseKey(component)==0);CHECK(RegDeleteKeyExW(product,component_path,0x100,0)==0);
    CHECK(RegDeleteKeyExW(product,W("Components"),0x100,0)==0);
    CHECK(RegCloseKey(product)==0);CHECK(RegDeleteKeyExW(root,product_path,0x100,0)==0);
    CHECK(MsiLocateComponentW(fixture_component,0,0)==-1);
}
void msi_components_entry(void)
{
    component_negative();int installed=component_mode("--installed"),absent=component_mode("--absent");
    if(installed)component_installed();
    if(component_mode("--metadata"))component_metadata();
    if(!installed&&!absent)component_native_buffers();
    if(installed||absent){
        CHECK(MsiGetComponentPathW((const WCHAR *)1,unknown,0,0)==-2);
        CHECK(MsiLocateComponentW((const WCHAR *)1,0,0)==-2);
        CHECK(MsiLocateComponentW(unknown,0,(DWORD *)1)==-2);
    }
    component_say("MSICOMPONENT DONE checks=");component_hex(checks);component_say(" failures=");component_hex(failures);component_say("\n");
    ExitProcess(failures?1:0);
}
