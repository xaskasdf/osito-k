/* Portable public ABI rejection probe; positive Windows oracle is separate. */
#include "../win32/d3dkmt_abi.h"
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetStdHandle(OKMT_U32);
API int CALL WriteFile(void *,const void *,OKMT_U32,OKMT_U32 *,void *);
API void CALL ExitProcess(OKMT_U32);
API OKMT_U32 CALL SetErrorMode(OKMT_U32);
static unsigned checks,failures;
static void number(const char *label,OKMT_U32 value)
{
 char text[96];unsigned n=0;while(label[n]){text[n]=label[n];n++;}
 for(unsigned i=0;i<8;i++)text[n++]="0123456789abcdef"[(value>>(28-i*4))&15];
 text[n++]='\n';OKMT_U32 written;WriteFile(GetStdHandle((OKMT_U32)-11),text,n,&written,0);
}
static void fill(void *data,unsigned bytes){unsigned char *p=data;while(bytes--)*p++=0xa5;}
static int same(const void *a,const void *b,unsigned bytes){const unsigned char *x=a,*y=b;while(bytes--)if(*x++!=*y++)return 0;return 1;}
static void copy(void *a,const void *b,unsigned bytes){unsigned char *x=a;const unsigned char *y=b;while(bytes--)*x++=*y++;}
#define CHECK(x) do{checks++;if(!(x)){failures++;number("RESIDENCY FAIL line=",__LINE__);}}while(0)
void native_residency_entry(void)
{
 SetErrorMode(0x8003);void *gdi=LoadLibraryA("gdi32.dll");
 OKMT_S32 (CALL *make)(OKMT_MAKE_RESIDENT *)=GetProcAddress(gdi,"D3DKMTMakeResident");
 OKMT_S32 (CALL *evict)(OKMT_EVICT *)=GetProcAddress(gdi,"D3DKMTEvict");
 CHECK(gdi && make && evict);if(!gdi || !make || !evict)goto done;
 CHECK(make(0)<0);CHECK(evict(0)<0);
 for(unsigned variant=0;variant<8;variant++) {
  OKMT_HANDLE list[2]={0xdead001,0xdead002};
  OKMT_MAKE_RESIDENT input,old;OKMT_EVICT drop,before;fill(&input,sizeof(input));fill(&drop,sizeof(drop));
  input.queue=variant&1?0xdead003:0;input.count=variant==2?0:variant==3?1025:2;
  input.allocations=variant==4?0:list;input.priorities=(void *)1;input.flags=variant==5?4:variant==6?0xffffffff:0;
  drop.device=input.queue;drop.count=input.count;drop.allocations=input.allocations;drop.flags=input.flags;
  copy(&old,&input,sizeof(old));copy(&before,&drop,sizeof(before));
  CHECK(make(&input)<0);CHECK(same(&input,&old,sizeof(old)));
  CHECK(evict(&drop)<0);CHECK(same(&drop,&before,sizeof(before)));
 }
done:number("RESIDENCY checks=",checks);number("RESIDENCY failures=",failures);ExitProcess(failures?1:0);
}
