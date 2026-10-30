/* Differential oracle for the bounded memory/string CRT contract. */
typedef unsigned int DWORD;
typedef __SIZE_TYPE__ SZ;
typedef __UINTPTR_TYPE__ UP;
typedef unsigned short WCHAR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API int CALL FreeLibrary(void *);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API void CALL ExitProcess(DWORD);
static unsigned invalid_calls,invalid_args,failures,checks;
static int invalid_errno;
static int *(*error_slot)(void);
#define SAY(s) do { DWORD wr;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&wr,0); } while(0)
#define HEX(value) do { DWORD v=(DWORD)(value),wr;char h[9];for(unsigned j=0;j<8;j++)h[j]="0123456789abcdef"[(v>>(28-j*4))&15];h[8]=' ';WriteFile(GetStdHandle((DWORD)-11),h,9,&wr,0); } while(0)
#define CHECK(value) do { checks++;if(!(value)){failures++;SAY("SECURE FAIL\n");HEX(__LINE__);SAY("\n");} } while(0)
static void secure_invalid(const WCHAR *expression,const WCHAR *function,const WCHAR *file,unsigned line,UP reserved)
{
    invalid_calls++;
    invalid_args |= expression!=0 || function!=0 || file!=0 || line!=0 || reserved!=0;
    invalid_errno=*error_slot();
}
static void secure_row(unsigned trial,int result,int error,DWORD last,const unsigned char *buffer)
{
    volatile DWORD values[23];
    values[0]=trial;values[1]=(DWORD)result;values[2]=(DWORD)error;values[3]=last;
    values[4]=invalid_calls;values[5]=(DWORD)invalid_errno;values[6]=invalid_args;
    for(unsigned i=0;i<64;i+=4) {
        values[7+i/4]=buffer[i]|((DWORD)buffer[i+1]<<8)|((DWORD)buffer[i+2]<<16)|((DWORD)buffer[i+3]<<24);
    }
    char row[218];const char prefix[]="SECURE ROW ";unsigned position=11;
    for(unsigned i=0;i<11;i++)((volatile char *)row)[i]=prefix[i];
    for(unsigned i=0;i<23;i++) {
        for(unsigned j=0;j<8;j++)row[position++]="0123456789abcdef"[(values[i]>>(28-j*4))&15];
        row[position++]=' ';
    }
    row[position-1]='\n';DWORD written;
    WriteFile(GetStdHandle((DWORD)-11),row,position,&written,0);
}
void secure_entry(void)
{
#ifdef CRT_USE_UCRT
    void *module=LoadLibraryA("ucrtbase.dll");
#else
    void *module=LoadLibraryA("msvcrt.dll");
#endif
    int (*copy)(void *,SZ,const void *,SZ)=module?GetProcAddress(module,"memcpy_s"):0;
    int (*move)(void *,SZ,const void *,SZ)=module?GetProcAddress(module,"memmove_s"):0;
    int (*strcopy)(char *,SZ,const char *)=module?GetProcAddress(module,"strcpy_s"):0;
    int (*append)(char *,SZ,const char *)=module?GetProcAddress(module,"strcat_s"):0;
    void *(*set_handler)(void *)=module?GetProcAddress(module,"_set_invalid_parameter_handler"):0;
    error_slot=module?GetProcAddress(module,"_errno"):0;
    CHECK(module&&copy&&move&&strcopy&&append&&error_slot);
    if(failures)ExitProcess(2);
    void *previous=set_handler?set_handler((void *)secure_invalid):0;
    const unsigned good=(1u<<0)|(1u<<1)|(1u<<2)|(1u<<8)|(1u<<9)|(1u<<10)|
        (1u<<14)|(1u<<15)|(1u<<16)|(1u<<20)|(1u<<24)|(1u<<26)|(1u<<27)|(1u<<28);
    for(unsigned trial=0;trial<29;trial++) {
        if(!set_handler && !(good&(1u<<trial)))continue;
        unsigned char buffer[64];
        for(unsigned i=0;i<sizeof(buffer);i++)buffer[i]=(unsigned char)(0x40+i);
        char *dest=(char *)buffer+8;
        if(trial>=14){dest[0]='x';dest[1]='y';dest[2]=0;}
        if(trial==22)for(unsigned i=0;i<24;i++)dest[i]='x';
        if(trial==24)dest[0]=0;
        invalid_calls=invalid_args=0;invalid_errno=-1;*error_slot()=123;SetLastError(0x12345678);
        int result=0;
        switch(trial) {
        case 0:result=copy(dest,24,"abcdefghijklmnop",16);break;
        case 1:result=copy(0,0,0,0);break;
        case 2:result=copy(0,99,0,0);break;
        case 3:result=copy(0,24,"abc",1);break;
        case 4:result=copy(dest,24,0,1);break;
        case 5:result=copy(dest,4,"abcdefgh",5);break;
        case 6:result=copy(dest,0,"a",1);break;
        case 7:result=copy(dest,0,0,1);break;
        case 8:result=move(dest+4,20,dest,12);break;
        case 9:result=move(dest,24,dest+4,12);break;
        case 10:result=move(0,0,0,0);break;
        case 11:result=move(dest,24,0,1);break;
        case 12:result=move(dest,4,"abcdefgh",5);break;
        case 13:result=move(0,24,"a",1);break;
        case 14:result=strcopy(dest,24,"abc");break;
        case 15:result=strcopy(dest,24,"");break;
        case 16:result=strcopy(dest,4,"abc");break;
        case 17:result=strcopy(dest,3,"abc");break;
        case 18:result=strcopy(dest,0,"abc");break;
        case 19:result=strcopy(dest,24,0);break;
        case 20:result=append(dest,5,"pq");break;
        case 21:result=append(dest,4,"pq");break;
        case 22:result=append(dest,24,"a");break;
        case 23:result=append(dest,24,0);break;
        case 24:result=append(dest,1,"");break;
        case 25:result=append(dest,0,"a");break;
        case 26:result=move(dest,24,dest,24);break;
        case 27:result=move(0,99,0,0);break;
        case 28:result=copy(dest,24,0,0);break;
        }
        int error=*error_slot();DWORD last=GetLastError();
        secure_row(trial,result,error,last,buffer);
        for(unsigned i=0;i<8;i++)CHECK(buffer[i]==0x40+i);
        for(unsigned i=40;i<64;i++)CHECK(buffer[i]==0x40+i);
        CHECK(!invalid_args);
        if(good&(1u<<trial))CHECK(result==0 && error==123 && invalid_calls==0);
    }
    if(set_handler)set_handler(previous);
    FreeLibrary(module);SAY("SECURE checks=");HEX(checks);SAY("failures=");HEX(failures);SAY("\n");
    ExitProcess(failures?1:0);
}
