#include <stdio.h>
#include <string.h>
#include "../win32/msvcrt_shim.h"
static unsigned checks,failures,reported;
static int error_value,observed_error;
int *WINAPI crt_errno(void) { return &error_value; }
void crt_report_invalid_parameter(void) { reported++; observed_error=error_value; }
PVOID WINAPI crt_memcpy(PVOID d,PCVOID s,SIZE_T n) { return memcpy(d,s,n); }
PVOID WINAPI crt_memmove(PVOID d,PCVOID s,SIZE_T n) { return memmove(d,s,n); }
PVOID WINAPI crt_memset(PVOID d,int c,SIZE_T n) { return memset(d,c,n); }
#define CHECK(x) do { checks++;if(!(x)){failures++;printf("FAIL line %d\n",__LINE__);} } while(0)
int main(void)
{
    unsigned char actual[128],expected[128],source[80];
    for(unsigned i=0;i<sizeof(source);i++)source[i]=(unsigned char)(i+1);
    for(unsigned size=0;size<=64;size++)for(unsigned count=0;count<=64;count++) {
        memset(actual,0x5a,sizeof(actual));memcpy(expected,actual,sizeof(actual));
        error_value=123;reported=0;
        int result=crt_memcpy_s(actual+8,size,source,count);
        if(count<=size)memcpy(expected+8,source,count);else memset(expected+8,0,size);
        CHECK(!memcmp(actual,expected,sizeof(actual)));
        CHECK(result==(count<=size?0:34));
        CHECK(error_value==(count<=size?123:34));CHECK(reported==(count>size));
        if(reported)CHECK(observed_error==34);
        for(unsigned direction=0;direction<2;direction++) {
            for(unsigned i=0;i<sizeof(actual);i++)actual[i]=(unsigned char)i;
            memcpy(expected,actual,sizeof(actual));error_value=123;reported=0;
            unsigned dst=direction?12:8,src=direction?8:12;
            result=crt_memmove_s(actual+dst,size,actual+src,count);
            if(count<=size)memmove(expected+dst,expected+src,count);
            CHECK(!memcmp(actual,expected,sizeof(actual)));
            CHECK(result==(count<=size?0:34));CHECK(error_value==(count<=size?123:34));
            CHECK(reported==(count>size));
        }
    }
    for(unsigned length=0;length<=32;length++)for(unsigned prefix=0;prefix<=32;prefix++)for(unsigned size=0;size<=65;size++) {
        memset(source,'s',length);source[length]=0;
        memset(actual,0x5a,sizeof(actual));memset(actual+8,'d',prefix);actual[8+prefix]=0;
        memcpy(expected,actual,sizeof(actual));error_value=123;reported=0;
        int result=crt_strcat_s((char *)actual+8,size,(char *)source);
        int success=size>length+prefix;
        int error=(!size || size<=prefix)?22:34;
        CHECK(result==(success?0:error));CHECK(error_value==(success?123:error));
        CHECK(reported==!success);if(reported)CHECK(observed_error==error);
        CHECK(!memcmp(actual,expected,8));
        CHECK(!memcmp(actual+8+size,expected+8+size,sizeof(actual)-8-size));
        if(success) {
            CHECK(!memcmp(actual+8,expected+8,prefix));
            CHECK(!memcmp(actual+8+prefix,source,length+1));
        } else if(size)CHECK(!actual[8]);
    }
    CHECK(crt_memcpy_s(NULL,0,NULL,0)==0);
    CHECK(crt_memmove_s(NULL,0,NULL,0)==0);
    printf("CRT secure: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
