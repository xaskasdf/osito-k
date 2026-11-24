typedef unsigned int DWORD;
typedef unsigned short WCHAR;
typedef __UINTPTR_TYPE__ SIZE;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define W(s) ((const WCHAR *)u##s)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API char *CALL GetCommandLineA(void);
API HANDLE CALL CreateThread(void *,SIZE,DWORD (CALL *)(void *),void *,DWORD,DWORD *);
API DWORD CALL WaitForSingleObject(HANDLE,DWORD);
API int CALL CloseHandle(HANDLE);
API DWORD CALL MsiEnumProductsW(DWORD,WCHAR *);
API DWORD CALL MsiEnumRelatedProductsW(const WCHAR *,DWORD,DWORD,WCHAR *);
API int CALL MsiQueryFeatureStateW(const WCHAR *,const WCHAR *);
API DWORD CALL RegCreateKeyExW(HANDLE,const WCHAR *,DWORD,WCHAR *,DWORD,DWORD,void *,HANDLE *,DWORD *);
API DWORD CALL RegSetValueExW(HANDLE,const WCHAR *,DWORD,DWORD,const void *,DWORD);
API DWORD CALL RegDeleteKeyW(HANDLE,const WCHAR *);
API DWORD CALL RegDeleteKeyExW(HANDLE,const WCHAR *,DWORD,DWORD);
API DWORD CALL RegCloseKey(HANDLE);
static unsigned checks,failures;
static const WCHAR *minimum=W("{13A4EE12-23EA-3371-91EE-EFB36DDFFF3E}");
static const WCHAR *additional=W("{F8CFEB22-A2E7-3971-9EDA-4B11EDEFC185}");
static const WCHAR *upgrade_minimum=W("{B59F5BF1-67C8-3802-8E59-2CE551A39FC5}");
static const WCHAR *upgrade_additional=W("{936E696B-0C8D-3A48-98DF-344FEA4E1139}");
static const WCHAR *unknown=W("{B08BCF70-2BA1-4E13-B55B-20C7B79716F3}");
static void discovery_say(const char *text)
{DWORD length=0,written;while(text[length])length++;WriteFile(GetStdHandle((DWORD)-11),text,length,&written,0);}
static void discovery_hex(DWORD value)
{char text[9];for(unsigned i=0;i<8;i++)text[i]="0123456789abcdef"[(value>>((7-i)*4))&15];text[8]=0;discovery_say(text);}
static void discovery_check(int ok,unsigned line)
{checks++;if(!ok){failures++;discovery_say("MSIDISCOVERY FAIL line=");discovery_hex(line);discovery_say("\n");}}
#define CHECK(c) discovery_check((c),__LINE__)
static int discovery_equal(const WCHAR *a,const WCHAR *b)
{while(*a&&*a==*b){a++;b++;}return *a==*b;}
static int discovery_mode(const char *wanted)
{
    const char *cmd=GetCommandLineA();
    for(unsigned i=0;cmd[i];i++){unsigned j=0;while(wanted[j]&&cmd[i+j]==wanted[j])j++;if(!wanted[j])return 1;}
    return 0;
}
#include "msi_discovery_cases.h"
static void discovery_negative(void)
{
    for(unsigned i=0;i<sizeof(discovery_feature_cases)/sizeof(*discovery_feature_cases);i++) {
        const FEATURE_CASE *test=&discovery_feature_cases[i];
        CHECK(MsiQueryFeatureStateW(test->product,test->feature)==test->result);
    }
    for(unsigned i=0;i<sizeof(discovery_related_cases)/sizeof(*discovery_related_cases);i++) {
        const RELATED_CASE *test=&discovery_related_cases[i];
        WCHAR buffer[40];for(unsigned j=0;j<40;j++)buffer[j]='Z';
        CHECK(MsiEnumRelatedProductsW(test->upgrade,test->reserved,0,buffer)==test->result);
        for(unsigned j=0;j<40;j++)CHECK(buffer[j]=='Z');
    }
    CHECK(MsiEnumProductsW(0,0)==87);
    CHECK(MsiEnumRelatedProductsW(unknown,0,0,0)==87);
}
static DWORD CALL discovery_worker(void *parameter)
{
    DWORD expected=(DWORD)(SIZE)parameter;WCHAR text[40];
    /* Main's active cursor must not make this thread's index 1 valid. */
    CHECK(MsiEnumProductsW(1,text)==87);
    CHECK(MsiEnumProductsW(0,text)==expected);
    /* Deliberately abandon a cursor; process teardown must reclaim it. */
    return 0;
}
static void discovery_products(int expected_count)
{
    WCHAR first[40],buffer[40];DWORD result=MsiEnumProductsW(0,first),count=0;
    CHECK(result==0||result==259);
    if(result==0) {
        CHECK(first[0]=='{'&&first[37]=='}'&&first[38]==0);
        CHECK(MsiEnumProductsW(2,buffer)==87);
    }
    HANDLE worker=CreateThread(0,0,discovery_worker,(void *)(SIZE)result,0,0);
    CHECK(worker!=0);
    if(worker){CHECK(WaitForSingleObject(worker,10000)==0);CHECK(CloseHandle(worker));}
    if(result==0) {
        count=1;
        for(;count<2048;count++){
            for(unsigned j=0;j<40;j++)buffer[j]='Z';
            result=MsiEnumProductsW(count,buffer);
            if(result)break;
            CHECK(buffer[0]=='{'&&buffer[37]=='}'&&buffer[38]==0&&buffer[39]=='Z');
        }
        CHECK(result==259);
        for(unsigned j=0;j<40;j++)CHECK(buffer[j]=='Z');
        CHECK(MsiEnumProductsW(count,buffer)==87);
        CHECK(MsiEnumProductsW(0,buffer)==0&&discovery_equal(first,buffer));
        CHECK(MsiEnumProductsW(0,buffer)==0&&discovery_equal(first,buffer));
    }
    if(expected_count>=0)CHECK(count==(DWORD)expected_count);
}
static void discovery_installed(void)
{
    WCHAR text[40],lower[39];
    CHECK(MsiEnumRelatedProductsW(upgrade_minimum,0,0,text)==0&&discovery_equal(text,minimum));
    CHECK(MsiEnumRelatedProductsW(upgrade_minimum,0,1,text)==259);
    for(unsigned i=0;i<39;i++){WCHAR c=upgrade_additional[i];lower[i]=(c>='A'&&c<='F')?c+32:c;}
    CHECK(MsiEnumRelatedProductsW(lower,0,0,text)==0&&discovery_equal(text,additional));
    CHECK(MsiEnumRelatedProductsW(lower,0,1,text)==259);
    CHECK(MsiQueryFeatureStateW(minimum,W("VC_Runtime_Minimum"))==3);
    CHECK(MsiQueryFeatureStateW(minimum,W("vc_runtime_minimum"))==3);
    CHECK(MsiQueryFeatureStateW(minimum,W("Provider"))==3);
    CHECK(MsiQueryFeatureStateW(minimum,W("Servicing_Key"))==3);
    CHECK(MsiQueryFeatureStateW(additional,W("VC_Runtime_Additional"))==3);
    CHECK(MsiQueryFeatureStateW(additional,W("Provider"))==3);
    CHECK(MsiQueryFeatureStateW(additional,W("Servicing_Key"))==3);
    CHECK(MsiQueryFeatureStateW(minimum,W("not_a_feature"))==-1);
}
static void discovery_metadata(void)
{
    /* Disposable QEMU only: our own uncommitted/corrupt metadata, never a host
     * installation and never changes to either original runtime product. */
    HANDLE root=(HANDLE)(SIZE)0x80000002U,key=0,features=0;
    const WCHAR *path=W("Software\\OsitoK\\Installer\\Products\\{B08BCF70-2BA1-4E13-B55B-20C7B79716F3}");
    DWORD one=1,context=4,state=5,feature=2;
    CHECK(RegCreateKeyExW(root,path,0,0,0,0xF013F,0,&key,0)==0);
    CHECK(RegSetValueExW(key,W("SchemaVersion"),0,4,&one,4)==0);
    CHECK(RegSetValueExW(key,W("Context"),0,4,&context,4)==0);
    CHECK(RegSetValueExW(key,W("UpgradeCode"),0,1,unknown,78)==0);
    CHECK(RegCreateKeyExW(key,W("Features"),0,0,0,0xF013F,0,&features,0)==0);
    CHECK(RegSetValueExW(features,W("Optional"),0,4,&feature,4)==0);
    WCHAR text[40];
    CHECK(MsiEnumRelatedProductsW(unknown,0,0,text)==259);
    CHECK(MsiQueryFeatureStateW(unknown,W("Optional"))==-1);
    CHECK(RegSetValueExW(key,W("State"),0,4,&state,4)==0);
    CHECK(MsiEnumRelatedProductsW(unknown,0,0,text)==0&&discovery_equal(text,unknown));
    CHECK(MsiQueryFeatureStateW(unknown,W("Optional"))==2);
    CHECK(RegSetValueExW(features,W("Optional"),0,3,&feature,4)==0);
    CHECK(MsiQueryFeatureStateW(unknown,W("Optional"))==-6);
    one=2;CHECK(RegSetValueExW(key,W("SchemaVersion"),0,4,&one,4)==0);
    for(unsigned j=0;j<40;j++)text[j]='Z';
    CHECK(MsiEnumRelatedProductsW(unknown,0,0,text)==1610);
    for(unsigned j=0;j<40;j++)CHECK(text[j]=='Z');
    CHECK(MsiQueryFeatureStateW(unknown,W("Optional"))==-6);
    CHECK(RegCloseKey(features)==0);CHECK(RegDeleteKeyW(key,W("Features"))==0);
    CHECK(RegCloseKey(key)==0);CHECK(RegDeleteKeyExW(root,path,0x100,0)==0);
    CHECK(MsiEnumRelatedProductsW(unknown,0,0,text)==259);
}
void msi_discovery_entry(void)
{
    discovery_negative();
    int installed=discovery_mode("--installed"),absent=discovery_mode("--absent");
    discovery_products(installed?2:absent?0:-1);
    if(installed)discovery_installed();
    if(discovery_mode("--metadata"))discovery_metadata();
    if(installed||absent) {
        CHECK(MsiEnumProductsW(0,(WCHAR *)1)==87);
        CHECK(MsiEnumRelatedProductsW((const WCHAR *)1,0,0,0)==87);
        CHECK(MsiQueryFeatureStateW((const WCHAR *)1,W("x"))==-2);
        CHECK(MsiQueryFeatureStateW(minimum,(const WCHAR *)1)==-2);
    }
    discovery_say("MSIDISCOVERY DONE checks=");discovery_hex(checks);
    discovery_say(" failures=");discovery_hex(failures);discovery_say("\n");
    ExitProcess(failures?1:0);
}
