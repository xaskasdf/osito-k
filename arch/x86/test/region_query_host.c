#define main screen_contract_main
#include "gdi_screen_host.c"
#undef main
extern void *__real_malloc(size_t);
extern void __real_free(void *);
static long allocations,fail_after=-1;
void *__wrap_malloc(size_t n)
{if(fail_after==0)return NULL;if(fail_after>0)fail_after--;void *p=__real_malloc(n);if(p)allocations++;return p;}
void __wrap_free(void *p){if(p)allocations--;__real_free(p);}
BOOL WINAPI ClientToScreen(HANDLE window,PVOID p)
{CHECK(window==WINDOW);GDI_POINT *point=p;point->x+=80;point->y+=100;return TRUE;}
typedef struct {DWORD size,type,count,bytes;GDI_RECT bounds;GDI_RECT rects[2048];} REGION_BUFFER;
static REGION_BUFFER buffer;
int win32_user_range_writable(void *p,SIZE_T n,BOOL mode)
{(void)mode;return p==&buffer && n<=sizeof(buffer);}
static unsigned char oracle[32][32];
static void verify(HGDIOBJ handle)
{
    long baseline=allocations;
    DWORD required=GetRegionData(handle,0,NULL);CHECK(required>=32 && required<=sizeof(buffer));
    gdi_memset(&buffer,0xa5,sizeof(buffer));CHECK(GetRegionData(handle,sizeof(buffer),&buffer)==required);
    CHECK(buffer.size==32 && buffer.type==1 && required==32+buffer.bytes && buffer.bytes==16*buffer.count);
    BOOL ordered=TRUE;
    for(UINT i=0;i<buffer.count;i++) {
        GDI_RECT a=buffer.rects[i];CHECK(a.left<a.right && a.top<a.bottom);
        if(i && (a.top<buffer.rects[i-1].top || (a.top==buffer.rects[i-1].top && a.left<=buffer.rects[i-1].left))) ordered=FALSE;
        for(UINT j=0;j<i;j++){GDI_RECT b=buffer.rects[j];CHECK(!(a.left<b.right&&b.left<a.right&&a.top<b.bottom&&b.top<a.bottom));}
    }
    CHECK(ordered);
    for(int y=0;y<32;y++)for(int x=0;x<32;x++) {
        BOOL inside=FALSE;
        for(UINT i=0;i<buffer.count;i++){GDI_RECT r=buffer.rects[i];if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom)inside=TRUE;}
        CHECK(inside==!!oracle[y][x]);
    }
    /* Independent cell-grid canonicalization: adjacent equal scanlines form
     * one band. Each row's maximal occupied runs form that band's rectangles. */
    UINT index=0;
    for(int top=0;top<32;) {
        int bottom=top+1;
        while(bottom<32) {
            BOOL same=TRUE;for(int x=0;x<32;x++)if(oracle[bottom][x]!=oracle[top][x])same=FALSE;
            if(!same)break;
            bottom++;
        }
        for(int x=0;x<32;) {
            if(!oracle[top][x]){x++;continue;}
            int left=x;while(x<32&&oracle[top][x])x++;
            CHECK(index<buffer.count);
            if(index<buffer.count){GDI_RECT r=buffer.rects[index];CHECK(r.left==left&&r.top==top&&r.right==x&&r.bottom==bottom);}
            index++;
        }
        top=bottom;
    }
    CHECK(index==buffer.count);
    for(UINT i=required;i<sizeof(buffer);i++)CHECK(((BYTE *)&buffer)[i]==0xa5);
    CHECK(allocations==baseline);
}
int main(void)
{
    uint32_t seed=0x881221;
    for(UINT iteration=0;iteration<200;iteration++) {
        GDI_REGION_SCOPE(source);gdi_memset(oracle,0,sizeof(oracle));
        for(UINT j=0;j<20;j++) {
            LONG p[4];for(UINT k=0;k<4;k++){seed=seed*1664525+1013904223;p[k]=(seed>>16)%33;}
            GDI_RECT r={p[0],p[1],p[2],p[3]};region_normalize_rect(&r);
            CHECK(region_append_rect(&source,r));
            for(int y=r.top;y<r.bottom;y++)for(int x=r.left;x<r.right;x++)oracle[y][x]=1;
        }
        HGDIOBJ r=CreateRectRgn(0,0,0,0);CHECK(gdi32_region_data_write(r,&source)!=0);verify(r);CHECK(DeleteObject(r));
    }
    GDI_REGION_SCOPE(source);
    for(int i=159;i>=0;i--)CHECK(region_append_rect(&source,(GDI_RECT){i*3,(i%9)*3,i*3+1,(i%9)*3+1}));
    HGDIOBJ r=CreateRectRgn(0,0,0,0);CHECK(gdi32_region_data_write(r,&source)==3);
    long baseline=allocations;BOOL success=FALSE;
    for(long point=0;point<16;point++) {
        gdi_memset(&buffer,0xa5,sizeof(buffer));fail_after=point;
        DWORD bytes=GetRegionData(r,sizeof(buffer),&buffer);fail_after=-1;
        CHECK(allocations==baseline);
        if(bytes){CHECK(bytes==32+160*16);success=TRUE;break;}
        for(UINT i=0;i<sizeof(buffer);i++)CHECK(((BYTE *)&buffer)[i]==0xa5);
    }
    CHECK(success);
    for(compat32=0;compat32<=1;compat32++) {
        SetLastError(0x1234);CHECK(!GetRegionData(NULL,0,NULL));CHECK(GetLastError()==(compat32?0x1234:6));
        SetLastError(0x1234);CHECK(!GetRegionData(r,31,&buffer));CHECK(GetLastError()==(compat32?0x1234:87));
        SetLastError(0x1234);CHECK(GetRandomRgn(NULL,r,4)==-1);CHECK(GetLastError()==(compat32?0x1234:6));
    }
    compat32=0;CHECK(DeleteObject(r));gdi32_region_data_clear(&source);CHECK(!allocations);
    HDC dc=CreateCompatibleDC(NULL);r=CreateRectRgn(2,2,3,3);GDI_RECT bounds;
    CHECK(GetRandomRgn(dc,r,4)==1 && GetRgnBox(r,&bounds)==2 && bounds.right==1 && bounds.bottom==1);
    CHECK(DeleteDC(dc));
    window_ready=1;dc=gdi32_alloc_window_dc(WINDOW);GDI_DC *native=dc_from_handle(dc);
    native->visible_region=kmalloc(sizeof(*native->visible_region));gdi_memset(native->visible_region,0,sizeof(*native->visible_region));
    CHECK(region_append_rect(native->visible_region,(GDI_RECT){0,0,5,4}));
    CHECK(GetRandomRgn(dc,r,4)==1 && GetRgnBox(r,&bounds)==2 && bounds.left==80&&bounds.top==100&&bounds.right==85&&bounds.bottom==104);
    HGDIOBJ clip=CreateRectRgn(90,90,95,95);CHECK(SelectClipRgn(dc,clip)==2);CHECK(DeleteObject(clip));
    GDI_RECT paint={1,1,4,3};uint64_t token;CHECK(gdi32_push_paint_clip(dc,&paint,&token));
    CHECK(GetRandomRgn(dc,r,4)==1 && GetRgnBox(r,&bounds)==2 && bounds.left==81&&bounds.top==101&&bounds.right==84&&bounds.bottom==103);
    gdi32_pop_paint_clip(dc,token,TRUE);CHECK(DeleteObject(r));gdi32_release_window_dc(WINDOW);
    CHECK(!allocations);
    printf("GDI region query: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
