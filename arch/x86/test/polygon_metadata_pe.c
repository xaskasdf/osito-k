#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "polygon_metadata_cases.h"
static unsigned checks,failures,current;
static unsigned char buffer[65536];
static char row[128];static unsigned used;
void *memset(void *p,int v,size_t n){unsigned char *d=p;while(n--)*d++=(unsigned char)v;return p;}
void *memcpy(void *p,const void *s,size_t n){unsigned char *d=p;const unsigned char *a=s;while(n--)*d++=*a++;return p;}
static void number(DWORD n){for(int i=28;i>=0;i-=4)row[used++]="0123456789abcdef"[(n>>i)&15];row[used++]=' ';}
static void emit(const char *s){DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&w,0);row[used-1]='\n';WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),row,used,&w,0);used=0;}
#define CHECK(x) do{checks++;if(!(x)){failures++;number(current);number(__LINE__);emit("POLYMETA FAIL ");}}while(0)
static void *resolve(HMODULE m,const char *s){void *p=GetProcAddress(m,s);if(!p)ExitProcess(2);return p;}
void polygon_metadata_entry(void)
{
    HMODULE gdi=LoadLibraryA("gdi32.dll");
    HRGN (WINAPI *polygon)(const POINT *,int,int)=(void *)resolve(gdi,"CreatePolygonRgn");
    HRGN (WINAPI *multi)(const POINT *,const int *,int,int)=(void *)resolve(gdi,"CreatePolyPolygonRgn");
    int (WINAPI *box_fn)(HRGN,RECT *)=(void *)resolve(gdi,"GetRgnBox");
    DWORD (WINAPI *data_fn)(HRGN,DWORD,RGNDATA *)=(void *)resolve(gdi,"GetRegionData");
    BOOL (WINAPI *delete_fn)(HGDIOBJ)=(void *)resolve(gdi,"DeleteObject");
    unsigned count=sizeof(metadata_cases)/sizeof(metadata_cases[0]);
    for(unsigned i=0;i<count;i++) {
        const POLYGON_METADATA_CASE *test=&metadata_cases[i];current=test->id;
        const POINT *points=metadata_points+test->point_offset;
        HRGN r=test->multi ? multi(points,metadata_counts+test->count_offset,test->polygons,test->mode) : polygon(points,test->vertices,test->mode);
        CHECK(r!=NULL);if(!r)continue;
        RECT box;CHECK(box_fn(r,&box)==(int)test->kind);
        CHECK(box.left==test->bounds.left && box.top==test->bounds.top && box.right==test->bounds.right && box.bottom==test->bounds.bottom);
        DWORD bytes=data_fn(r,0,NULL);CHECK(bytes==test->bytes && bytes<=sizeof(buffer)-16);
        if(bytes>sizeof(buffer)-16){delete_fn(r);continue;}
        memset(buffer,0xa5,sizeof(buffer));CHECK(data_fn(r,bytes,(RGNDATA *)buffer)==bytes);
        DWORD hash=2166136261u;for(unsigned b=0;b<bytes;b++)hash=(hash^buffer[b])*16777619u;
        CHECK(hash==test->hash);
        BOOL guard=TRUE;for(unsigned b=bytes;b<bytes+16;b++)if(buffer[b]!=0xa5)guard=FALSE;
        CHECK(guard);CHECK(delete_fn(r));
    }
    number(count);number(checks);number(failures);emit("POLYMETA RESULT ");ExitProcess(failures?1:0);
}
