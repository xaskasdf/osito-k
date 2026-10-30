typedef unsigned int UINT;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API void *CALL GetStdHandle(UINT);
API int CALL WriteFile(void *,const void *,UINT,UINT *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
void *memset(void *p,int c,UPTR n){unsigned char *s=p;for(UPTR i=0;i<n;i++)s[i]=(unsigned char)c;return p;}
void *memcpy(void *p,const void *q,UPTR n){unsigned char *d=p;const unsigned char *s=q;for(UPTR i=0;i<n;i++)d[i]=s[i];return p;}
static unsigned failures;
#define SAY(s) do{UINT n;WriteFile(GetStdHandle((UINT)-11),s,sizeof(s)-1,&n,0);}while(0)
#define CHECK(ok,name) do{if(ok)SAY("ICONRES PASS " name "\n");else{SAY("ICONRES FAIL " name "\n");failures++;}}while(0)
typedef struct{UINT size;int width,height;WORD planes,bpp;UINT compression,image;int xppm,yppm;UINT used,important;} Header;
typedef struct{int icon;UINT x,y;void *mask,*color;} IconInfo;
typedef struct{int type,width,height,pitch;WORD planes,bpp;void *bits;} Bitmap;
typedef void *(CALL *resource_fn)(const void *,UINT,int,UINT);
typedef void *(CALL *resource_ex_fn)(const void *,UINT,int,UINT,int,int,UINT);
typedef int (CALL *getinfo_fn)(void *,IconInfo *);
typedef int (CALL *object_fn)(void *,int,void *);
typedef int (CALL *delete_fn)(void *);
typedef void *(CALL *dc_fn)(void *);
typedef void *(CALL *dib_fn)(void *,const Header *,UINT,void **,void *,UINT);
typedef void *(CALL *select_fn)(void *,void *);
typedef int (CALL *draw_fn)(void *,int,int,void *,int,int,UINT,void *,UINT);
typedef int (CALL *metric_fn)(int);
static getinfo_fn getinfo;static object_fn getobject;static delete_fn remove_bitmap;
static int dimensions(void *icon,int width,int height,int is_icon,UINT x,UINT y)
{
    struct{IconInfo info;volatile UINT guard;}result;memset(&result,0,sizeof(result));result.guard=0x13579bdf;
    Bitmap bitmap={0};
    if(!getinfo(icon,&result.info))return 0;
    int okay=result.guard==0x13579bdf&&result.info.icon==is_icon&&result.info.x==x&&result.info.y==y;
    void *image=result.info.color?result.info.color:result.info.mask;
    okay=okay&&getobject(image,sizeof(bitmap),&bitmap)==sizeof(bitmap)&&bitmap.width==width&&bitmap.height==(result.info.color?height:height*2);
    if(result.info.mask)remove_bitmap(result.info.mask);
    if(result.info.color)remove_bitmap(result.info.color);
    return okay;
}
void icon_resource_entry(void)
{
    void *user=LoadLibraryA("user32.dll"),*gdi=LoadLibraryA("gdi32.dll");
    resource_fn create=GetProcAddress(user,"CreateIconFromResource");
    resource_ex_fn create_ex=GetProcAddress(user,"CreateIconFromResourceEx");
    getinfo=GetProcAddress(user,"GetIconInfo");getobject=GetProcAddress(gdi,"GetObjectA");
    delete_fn destroy=GetProcAddress(user,"DestroyIcon");remove_bitmap=GetProcAddress(gdi,"DeleteObject");
    dc_fn make_dc=GetProcAddress(gdi,"CreateCompatibleDC");delete_fn delete_dc=GetProcAddress(gdi,"DeleteDC");
    dib_fn make_dib=GetProcAddress(gdi,"CreateDIBSection");select_fn select=GetProcAddress(gdi,"SelectObject");
    draw_fn draw=GetProcAddress(user,"DrawIconEx");metric_fn metric=GetProcAddress(user,"GetSystemMetrics");
    CHECK(create&&create_ex&&getinfo&&getobject&&destroy&&remove_bitmap&&make_dc&&delete_dc&&make_dib&&select&&draw&&metric,"exports");
    if(failures)ExitProcess(1);
    struct{Header header;UINT pixels[4];UINT mask[2];}resource={
        {40,2,4,1,32,0,24,0,0,0,0},
        {0xff0000ffU,0xffffffffU,0xffff0000U,0xff00ff00U},{0,0}};
    void *icon=create_ex(&resource,sizeof(resource),1,0x30000,0,0,0);
    CHECK(icon&&dimensions(icon,2,2,1,1,1),"original-size-icon");
    void *dc=make_dc(0);Header output={40,2,-2,1,32,0,16,0,0,0,0};UINT *pixels=0;
    void *bitmap=make_dib(dc,&output,0,(void**)&pixels,0,0);void *previous=select(dc,bitmap);
    CHECK(dc&&bitmap&&pixels,"drawing-surface");if(!pixels)ExitProcess(1);
    memset(pixels,0,16);
    CHECK(draw(dc,0,0,icon,2,2,0,0,3)&&(pixels[0]&0xffffff)==0xff0000&&(pixels[1]&0xffffff)==0x00ff00&&(pixels[2]&0xffffff)==0x0000ff&&(pixels[3]&0xffffff)==0xffffff,"decoded-bitmap-orientation");
    CHECK(destroy(icon),"destroy-original-icon");
    icon=create(&resource,sizeof(resource),1,0x30000);
    int width=metric(11),height=metric(12);
    CHECK(icon&&dimensions(icon,width,height,1,(UINT)width/2,(UINT)height/2),"wrapper-default-size");
    CHECK(destroy(icon),"destroy-wrapper-icon");
    icon=create_ex(&resource,sizeof(resource),1,0x30000,4,6,0);
    CHECK(icon&&dimensions(icon,4,6,1,2,3),"explicit-resize");
    CHECK(destroy(icon),"destroy-resized-icon");
    struct{WORD x,y;unsigned char resource[64];}cursor={1,0,{0}};
    memcpy(cursor.resource,&resource,sizeof(resource));
    icon=create_ex(&cursor,sizeof(cursor),0,0x30000,2,2,0);
    CHECK(icon&&dimensions(icon,2,2,0,1,0),"cursor-hotspot-prefix");
    CHECK(destroy(icon),"destroy-cursor");
    struct{Header header;UINT palette[2];UINT xor_rows[2],and_rows[2];}mono={
        {40,2,4,1,1,0,16,0,0,2,0},{0,0xffffff},{0x40,0x40},{0xc0,0}};
    icon=create_ex(&mono,sizeof(mono),1,0x30000,2,2,0);
    CHECK(icon&&dimensions(icon,2,2,1,1,1),"monochrome-mask-pair");
    for(UINT i=0;i<4;i++)pixels[i]=0x123456;
    CHECK(draw(dc,0,0,icon,2,2,0,0,3)&&(pixels[0]&0xffffff)==0&&(pixels[1]&0xffffff)==0xffffff&&(pixels[2]&0xffffff)==0x123456&&(pixels[3]&0xffffff)==0xedcba9,"monochrome-and-xor");
    CHECK(destroy(icon),"destroy-monochrome");
    CHECK(!create_ex(&resource,sizeof(resource),1,0x10000,2,2,0),"invalid-resource-version");
    CHECK(resource.header.height==4&&resource.pixels[0]==0xff0000ffU,"source-not-modified");
    select(dc,previous);CHECK(remove_bitmap(bitmap)&&delete_dc(dc),"release-drawing-surface");
    if(!failures)SAY("ICONRES DONE\n");
    ExitProcess(failures?1:0);
}
