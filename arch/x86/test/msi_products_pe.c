typedef unsigned int DWORD;
typedef unsigned short WCHAR;
#define NULL ((void *)0)
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API char *CALL GetCommandLineA(void);
API DWORD CALL MsiGetProductInfoW(const WCHAR *,const WCHAR *,WCHAR *,DWORD *);
static unsigned checks,failures;
static void product_say(const char *text)
{DWORD n=0,wr;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&wr,NULL);}
static void product_hex(DWORD value)
{char text[9];for(unsigned i=0;i<8;i++)text[i]="0123456789abcdef"[(value>>((7-i)*4))&15];text[8]=0;product_say(text);}
static void product_check(int condition,unsigned line)
{checks++;if(!condition){failures++;product_say("MSIPRODUCT FAIL line=");product_hex(line);product_say("\n");}}
#include "msi_products_checks.h"
void msi_products_entry(void)
{
    const char *command=GetCommandLineA();int installed=0,absent=0;
    for(unsigned i=0;command[i];i++)if(command[i]=='-'&&command[i+1]=='-'){
        if(command[i+2]=='i')installed=1;if(command[i+2]=='a')absent=1;
    }
    product_negative_checks();
    if(installed){
        product_installed_checks();
        WCHAR text[32];DWORD size=32;
        PRODUCT_CHECK(MsiGetProductInfoW((void *)1,MW("VersionString"),text,&size)==87);
        PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,(void *)1,text,&size)==87);
        PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,MW("VersionString"),(void *)1,&size)==87);
        PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,MW("VersionString"),text,(void *)1)==87);
    }
    if(absent)PRODUCT_CHECK(MsiGetProductInfoW(product_minimum,MW("VersionString"),NULL,NULL)==1605);
    product_say("MSIPRODUCT DONE checks=");product_hex(checks);product_say(" failures=");product_hex(failures);product_say("\n");
    ExitProcess(failures?1:0);
}
