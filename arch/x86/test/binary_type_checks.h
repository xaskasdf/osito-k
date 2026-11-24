#include "binary_type_cases.h"
static void binary_contract_checks(void)
{
    for(unsigned i=0;i<sizeof(binary_cases)/sizeof(*binary_cases);i++) {
        const BINARY_CASE *test=binary_cases+i;
        DWORD guard[3]={0x11223344,0xabababab,0x55667788};
        SetLastError(0x1234);
        int result=GetBinaryTypeW(test->name,guard+1);DWORD error=GetLastError();
        CHECK_CASE(result==test->result,i,0);
        CHECK_CASE(error==test->error,i,1);
        CHECK_CASE(guard[1]==(BINARY_CALLER_32 ? test->type32 : test->type),i,2);
        CHECK_CASE(guard[0]==0x11223344&&guard[2]==0x55667788,i,3);
    }
    static const struct {const WCHAR *name;DWORD error;} invalid[]={
        {NULL,123},{W(""),123},{W("binary-fixtures"),5},
        {W("binary-fixtures\\does-not-exist.exe"),2}
    };
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
        DWORD type=0xabababab;SetLastError(0x1234);
        CHECK_CASE(!GetBinaryTypeW(invalid[i].name,&type),1000+i,0);
        CHECK_CASE(GetLastError()==invalid[i].error&&type==0xabababab,1000+i,1);
    }
}
