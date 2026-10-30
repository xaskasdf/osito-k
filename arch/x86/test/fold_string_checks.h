#include "fold_string_cases.h"
static void fold_contract_checks(void)
{
    for (unsigned i=0;i<sizeof(fold_cases)/sizeof(*fold_cases);i++) {
        const FOLD_CASE *t=fold_cases+i; WCHAR input[64]={0},output[64];
        for (unsigned j=0;j<32;j++) input[j]=t->input[j];
        for (unsigned j=0;j<64;j++) output[j]=90;
        WCHAR *source=t->null_source?NULL:input;
        WCHAR *dest=t->kind==1?NULL:t->kind==2?source:output;
        SetLastError(0x1234);
        int result=FoldStringW(t->flags,source,t->length,dest,t->capacity);
        DWORD error=GetLastError();
        CHECK_CASE(result==t->result && error==t->error,i,1);
        /* A failed conversion has no specified output string. Compare every
         * successful byte and always check the guard beyond the capacity. */
        for (unsigned j=0;j<48;j++)
            if (t->result || t->kind || (int)j>=t->capacity)
                CHECK_CASE(output[j]==t->output[j],i,2+j);
        for (unsigned j=48;j<64;j++) CHECK_CASE(output[j]==90,i,2+j);
    }
    for (unsigned i=0;i<sizeof(short_cases)/sizeof(*short_cases);i++) {
        const SHORT_CASE *t=short_cases+i; WCHAR input[128]={0},output[128];
        for (unsigned j=0;j<64;j++) input[j]=t->input[j];
        for (unsigned j=0;j<128;j++) output[j]=90;
        WCHAR *source=t->null_source?NULL:input;
        WCHAR *dest=t->kind==1?NULL:t->kind==2?source:output;
        SetLastError(0x1234);
        DWORD result=GetShortPathNameW(source,dest,t->capacity),error=GetLastError();
        CHECK_CASE(result==t->result && error==t->error,10000+i,1);
        for (unsigned j=0;j<48;j++) CHECK_CASE(output[j]==t->output[j],10000+i,2+j);
        for (unsigned j=48;j<128;j++) CHECK_CASE(output[j]==90,10000+i,2+j);
        if (t->kind==2) for (unsigned j=0;j<64;j++) CHECK_CASE(input[j]==t->input[j],10000+i,200+j);
    }
}
