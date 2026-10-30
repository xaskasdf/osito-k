#define main screen_contract_main
#include "gdi_screen_host.c"
#undef main
extern int scanf(const char *,...);
static void *output_buffer;
static SIZE_T output_bytes;
int win32_user_range_writable(PVOID p,SIZE_T n,BOOL mode)
{(void)mode;return p==output_buffer && n<=output_bytes;}
int main(void)
{
    unsigned id,mode,multi,polygons;
    while(scanf("%u%u%u%u",&id,&mode,&multi,&polygons)==4) {
        if(polygons<1 || polygons>8)return 2;
        int counts[8];GDI_POINT points[1024];unsigned vertices=0;
        for(unsigned i=0;i<polygons;i++) {
            if(scanf("%d",counts+i)!=1 || counts[i]<2 || counts[i]>128 || vertices+(unsigned)counts[i]>1024)return 2;
            vertices+=(unsigned)counts[i];
        }
        for(unsigned i=0;i<vertices;i++)if(scanf("%d%d",&points[i].x,&points[i].y)!=2)return 2;
        HGDIOBJ region=multi ? CreatePolyPolygonRgn(points,counts,polygons,mode) : CreatePolygonRgn(points,vertices,mode);
        if(!region)return 3;
        GDI_RECT box;int kind=GetRgnBox(region,&box);
        output_bytes=GetRegionData(region,0,NULL);output_buffer=malloc(output_bytes);
        if(!output_buffer || GetRegionData(region,output_bytes,output_buffer)!=output_bytes)return 4;
        uint32_t hash=2166136261u;for(SIZE_T i=0;i<output_bytes;i++)hash=(hash^((BYTE *)output_buffer)[i])*16777619u;
        printf("POLYMETA %u %d %d %d %d %d %llu %08x\n",id,kind,box.left,box.top,box.right,box.bottom,(unsigned long long)output_bytes,hash);
        free(output_buffer);output_buffer=NULL;CHECK(DeleteObject(region));
    }
    return failures ? 1 : 0;
}
