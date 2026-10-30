/* Legacy MSVCRT aligned allocation: native/guest contract and ownership. */
typedef unsigned int DWORD;
typedef unsigned short WCHAR;
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
static unsigned failures,invalid_calls,invalid_args;
static int invalid_errno;
static int *(*error_slot)(void);
#define SAY(s) do {DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0);}while(0)
#define HEX(v) do {char h[9];DWORD x=(DWORD)(v),wr;for(int j=7;j>=0;j--){h[j]="0123456789abcdef"[x&15];x>>=4;}h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0);}while(0)
static void aligned_check(int ok)
{if(ok)SAY("ALIGNED PASS\n");else{failures++;SAY("ALIGNED FAIL\n");}}
static void aligned_invalid(const WCHAR *expression,const WCHAR *function,
                            const WCHAR *file,unsigned line,UP reserved)
{
    invalid_calls++;
    invalid_args |= expression!=0 || function!=0 || file!=0 || line!=0 || reserved!=0;
    invalid_errno=*error_slot();
}
void crt_aligned_entry(void)
{
    void *module=LoadLibraryA("msvcrt.dll");
    void *(*allocate)(UP,UP)=GetProcAddress(module,"_aligned_malloc");
    void (*release)(void *)=GetProcAddress(module,"_aligned_free");
    void *(*set_handler)(void *)=GetProcAddress(module,"_set_invalid_parameter_handler");
    error_slot=GetProcAddress(module,"_errno");
    aligned_check(module&&allocate&&release&&error_slot);
    if(failures)ExitProcess(1);
    /* Windows' legacy msvcrt has no public handler setter. */
    void *previous=set_handler?set_handler((void *)aligned_invalid):0;
    const UP alignments[]={1,2,4,8,16,32,64,4096,65536};
    const UP sizes[]={1,7,64,4097};
    for(unsigned a=0;a<sizeof(alignments)/sizeof(alignments[0]);a++){
        for(unsigned s=0;s<sizeof(sizes)/sizeof(sizes[0]);s++){
            *error_slot()=123;SetLastError(0x12345678);invalid_calls=invalid_args=0;
            unsigned char *p=allocate(sizes[s],alignments[a]);
            int error=*error_slot();DWORD last=GetLastError();
            int aligned=p&&(((UP)p&(alignments[a]-1))==0);
            aligned_check(aligned);aligned_check(error==123&&invalid_calls==0);
            int intact=p!=0;
            if(p){
                for(UP i=0;i<sizes[s];i++)p[i]=(unsigned char)(i*13+a+s);
                for(UP i=0;i<sizes[s];i++)intact &= p[i]==(unsigned char)(i*13+a+s);
            }
            aligned_check(intact);
            SAY("ALIGNED ROW ");HEX(a);HEX(s);HEX(p!=0);HEX(aligned);HEX(error);HEX(last);HEX(invalid_calls);
            release(p);int freed_error=*error_slot();DWORD freed_last=GetLastError();
            HEX(freed_error);HEX(freed_last);SAY("\n");aligned_check(freed_error==123);
        }
    }
    /* Record legacy edge behavior instead of assuming the newer UCRT contract. */
    const UP edge[][2]={{0,16},{8,0},{8,3},{8,6},{8,12},{0,0},
        {(UP)-1,16},{(UP)-16,16},{(UP)-32,64}};
    for(unsigned i=0;i<sizeof(edge)/sizeof(edge[0]);i++){
        /* Legacy MSVCRT terminates on non-power-of-two alignment, with no
         * public returning handler. Such cases need a separate process. */
        if(i>=2&&i<=4)continue;
        *error_slot()=123;SetLastError(0x12345678);invalid_calls=invalid_args=0;invalid_errno=-1;
        void *p=allocate(edge[i][0],edge[i][1]);
        int error=*error_slot();DWORD last=GetLastError();
        SAY("ALIGNED EDGE ");HEX(i);HEX(p!=0);HEX(error);HEX(last);HEX(invalid_calls);HEX(invalid_args);HEX(invalid_errno);SAY("\n");
        aligned_check(invalid_args==0);release(p);
    }
    *error_slot()=123;SetLastError(0x12345678);invalid_calls=0;release(0);
    aligned_check(*error_slot()==123&&invalid_calls==0);
    SAY("ALIGNED NULLFREE ");HEX(*error_slot());HEX(GetLastError());HEX(invalid_calls);SAY("\n");
    unsigned char *live[48];int intact=1;
    for(unsigned i=0;i<48;i++){
        live[i]=allocate(257+(UP)i,1U<<(i%13));
        intact &= live[i]!=0;
        if(live[i])for(unsigned j=0;j<257+i;j++)live[i][j]=(unsigned char)(i+j);
    }
    for(unsigned i=0;i<48;i++){
        if(live[i])for(unsigned j=0;j<257+i;j++)intact &= live[i][j]==(unsigned char)(i+j);
        release(live[i]);
    }
    aligned_check(intact);
    if(set_handler)set_handler(previous);
    if(!failures)SAY("ALIGNED DONE\n");ExitProcess(failures?1:0);
}
