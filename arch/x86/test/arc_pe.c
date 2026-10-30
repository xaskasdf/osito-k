/* The same off-screen bitmap probe runs on Windows and Osito-K. Compare its
 * ARCDRAW records byte for byte; do not use the implementation as an oracle. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef void *HANDLE;
#ifndef EXTPEN_HOST
typedef __SIZE_TYPE__ SIZE_T;
#endif
#ifdef EXTPEN_HOST
#define CALL WINAPI
#define API
#else
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#endif
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,HANDLE);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API const char *CALL GetCommandLineA(void);
typedef struct { int x,y; } Point;
typedef struct { DWORD size; int width,height; WORD planes,bpp; DWORD compression,bytes; int xppm,yppm; DWORD colors,important; } Info;
typedef struct { DWORD style; Point width; DWORD color; } LogPen;
typedef struct { float m11,m12,m21,m22,dx,dy; } Transform;
#define SAY(s) do { DWORD n_; WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&n_,0); } while(0)
#define HEX(v) do { DWORD v_=(DWORD)(v),n_; char s_[9]; for(int j_=0;j_<8;j_++) s_[j_]="0123456789ABCDEF"[(v_>>(28-4*j_))&15]; s_[8]=' '; WriteFile(GetStdHandle((DWORD)-11),s_,9,&n_,0); } while(0)
#define ROW(tag,values,count) do { char text_[192]; DWORD n_=0,w_; const char prefix_[]=tag; for(DWORD j_=0;j_<sizeof(prefix_)-1;j_++) text_[n_++]=prefix_[j_]; for(int i_=0;i_<(count);i_++){DWORD v_=(DWORD)(values)[i_];for(int j_=0;j_<8;j_++)text_[n_++]="0123456789ABCDEF"[(v_>>(28-4*j_))&15];text_[n_++]=' ';}text_[n_++]='\n';WriteFile(GetStdHandle((DWORD)-11),text_,n_,&w_,0); }while(0)
#define CHECK(c,s) do { if(c) SAY("ARCDRAW PASS " s "\n"); else { SAY("ARCDRAW FAIL " s "\n"); failures++; } } while(0)
#define IMPORT(ret,name,args) typedef ret (CALL *name##_fn) args; name##_fn name=(name##_fn)GetProcAddress(gdi,#name); if(!name) { SAY("ARCDRAW MISSING " #name "\n"); failures++; }



typedef struct { DWORD style,color; SIZE_T hatch; } Brush;
void arc_entry(void)
{
    DWORD failures=0;
    int dump=-1;const char *command=GetCommandLineA();
    for(int i=0;command[i];i++)if(command[i]=='d'&&command[i+1]=='u'&&command[i+2]=='m'&&command[i+3]=='p'&&command[i+4]=='='){
        dump=0;for(int j=i+5;command[j]>='0'&&command[j]<='9';j++)dump=dump*10+command[j]-'0';break;
    }
    HANDLE gdi=LoadLibraryA("gdi32.dll");
    IMPORT(HANDLE,CreateCompatibleDC,(HANDLE));
    IMPORT(int,DeleteDC,(HANDLE));
    IMPORT(HANDLE,CreateDIBSection,(HANDLE,const Info *,DWORD,void **,HANDLE,DWORD));
    IMPORT(HANDLE,CreatePen,(int,int,DWORD));
    IMPORT(HANDLE,ExtCreatePen,(DWORD,DWORD,const Brush *,DWORD,const DWORD *));
    IMPORT(HANDLE,CreateSolidBrush,(DWORD));
    IMPORT(HANDLE,GetStockObject,(int));
    IMPORT(HANDLE,SelectObject,(HANDLE,HANDLE));
    IMPORT(int,DeleteObject,(HANDLE));
    IMPORT(int,Arc,(HANDLE,int,int,int,int,int,int,int,int));
    IMPORT(int,Pie,(HANDLE,int,int,int,int,int,int,int,int));
    IMPORT(int,GetArcDirection,(HANDLE));
    IMPORT(int,SetArcDirection,(HANDLE,int));
    IMPORT(int,SaveDC,(HANDLE));
    IMPORT(int,RestoreDC,(HANDLE,int));
    IMPORT(int,SetROP2,(HANDLE,int));
    IMPORT(int,MoveToEx,(HANDLE,int,int,Point *));
    IMPORT(int,GetCurrentPositionEx,(HANDLE,Point *));
    IMPORT(int,GdiFlush,(void));
    IMPORT(int,SetMapMode,(HANDLE,int));
    IMPORT(int,SetWindowExtEx,(HANDLE,int,int,Point *));
    IMPORT(int,SetViewportExtEx,(HANDLE,int,int,Point *));
    IMPORT(int,SetViewportOrgEx,(HANDLE,int,int,Point *));
    IMPORT(int,SetGraphicsMode,(HANDLE,int));
    IMPORT(int,SetWorldTransform,(HANDLE,const Transform *));
    IMPORT(HANDLE,CreateRectRgn,(int,int,int,int));
    IMPORT(int,SelectClipRgn,(HANDLE,HANDLE));
    IMPORT(int,CombineRgn,(HANDLE,HANDLE,HANDLE,int));
#ifdef ARC_PATH_REFERENCE
    IMPORT(int,BeginPath,(HANDLE));
    IMPORT(int,EndPath,(HANDLE));
    IMPORT(int,FlattenPath,(HANDLE));
    IMPORT(int,GetPath,(HANDLE,Point *,unsigned char *,int));
#endif
    if(failures)ExitProcess(1);
    HANDLE dc=CreateCompatibleDC(0);DWORD *pixels=0;
    static const Info info={40,16,-16,1,32,0,0,0,0,0,0};
    HANDLE bitmap=CreateDIBSection(dc,&info,0,(void **)&pixels,0,0),old_bitmap=SelectObject(dc,bitmap);
    Brush brush={0,0x00332211,0};
    HANDLE pen=CreatePen(0,1,brush.color),geometric=ExtCreatePen(0x10200,0,&brush,0,0),fill=CreateSolidBrush(0x006699CC);
    CHECK(dc&&bitmap&&pixels&&pen&&geometric&&fill,"setup");
    if(!dc||!bitmap||!pixels||!pen||!geometric||!fill)ExitProcess(1);
    CHECK(GetArcDirection(dc)==1,"default-direction");
    CHECK(SetArcDirection(dc,2)==1,"set-direction");
    int saved=SaveDC(dc);CHECK(saved&&SetArcDirection(dc,1)==2&&RestoreDC(dc,saved)&&GetArcDirection(dc)==2,"saved-direction");
    DWORD row[18];
    static const DWORD dashes[]={3,2,1};
    HANDLE pens[]={pen,geometric,CreatePen(0,3,brush.color),
        ExtCreatePen(0x10000,3,&brush,0,0),ExtCreatePen(0x11200,3,&brush,0,0),
        ExtCreatePen(0x12100,3,&brush,0,0),CreatePen(1,1,brush.color),
        ExtCreatePen(0x11207,2,&brush,3,dashes),GetStockObject(8)};
    HANDLE clip=CreateRectRgn(3,3,12,12),hole=CreateRectRgn(6,6,10,10),
           ring=CreateRectRgn(1,1,15,15),empty=CreateRectRgn(0,0,0,0);
    for(int i=0;i<9;i++)CHECK(pens[i]!=0,"matrix-pen");
    CHECK(clip&&hole&&ring&&empty&&CombineRgn(ring,ring,hole,4),"matrix-regions");
    if(failures)ExitProcess(1);
    SetLastError(0xABCD);row[0]=GetArcDirection(0);row[1]=GetLastError();ROW("ARCDRAW INVALID-GET ",row,2);
    SetLastError(0xABCD);row[0]=SetArcDirection(0,1);row[1]=GetLastError();ROW("ARCDRAW INVALID-SET ",row,2);
    SetLastError(0xABCD);row[0]=Arc(0,2,2,14,14,20,8,8,20);row[1]=GetLastError();ROW("ARCDRAW INVALID-ARC ",row,2);
    SetLastError(0xABCD);row[0]=Pie(0,2,2,14,14,20,8,8,20);row[1]=GetLastError();ROW("ARCDRAW INVALID-PIE ",row,2);
    SetLastError(0xABCD);row[0]=SetArcDirection(dc,0);row[1]=GetLastError();row[2]=GetArcDirection(dc);ROW("ARCDRAW INVALID ",row,3);
    static const int rectangles[6][4]={{2,2,14,14},{2,4,15,11},{4,2,11,15},{13,12,2,3},{-4,-2,12,15},{7,2,8,14}};
    static const Point directions[8]={{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1},{0,1},{1,1}};
    /* 0..415: directions/bounds and ROP2; 416..703: 9 pens x 8
     * transforms/regions x Arc/Pie; 704..727: degenerate/extreme bounds. */
    for(int id=0;id<728;id++){
        int extended=id>=416&&id<704,edge=id>=704;
        int profile=extended?(id-416)/32:0,mode=extended?(id-416)/2%8:0;
        int shape=id<384?id%96:extended?1+(id-416)/16%2*7:0;
        int operation=id<384?(id/96)%2:id<416?(id-384)/16:(id&1);
        int bounds=shape/16,angle=shape%16;
        int start=angle<8?0:angle-8,end=angle<8?angle:(start+3)%8;
        static const int extreme[12][4]={{7,2,7,14},{2,7,14,7},{4,4,4,4},
            {14,14,2,2},{-1000000,-1000000,1000000,1000000},
            {0,-1000000,1000000,1000000},{-1000000,0,1000000,1000000},
            {-2147483647,-2147483647,2147483647,2147483647},
            {-2147483647,-2147483647,-2147483600,-2147483600},
            {2147483600,2147483600,2147483647,2147483647},
            {7,7,8,8},{2,7,14,8}};
        const int *rect=edge?extreme[(id-704)/2]:rectangles[bounds];
        int cx=(int)(((long long)rect[0]+rect[2])/2),cy=(int)(((long long)rect[1]+rect[3])/2);
        Point a={cx+20*directions[start].x,cy+20*directions[start].y};
        Point b={cx+20*directions[end].x,cy+20*directions[end].y};
        saved=SaveDC(dc);SelectObject(dc,extended?pens[profile]:id>=192&&id<384?geometric:pen);
        SelectObject(dc,extended&&mode==6?GetStockObject(5):fill);
        if(extended){
            if(mode==1)SelectClipRgn(dc,clip);
            if(mode==2)SelectClipRgn(dc,ring);
            if(mode==3){SetMapMode(dc,8);SetWindowExtEx(dc,2,2,0);SetViewportExtEx(dc,3,1,0);SetViewportOrgEx(dc,-3,4,0);}
            if(mode==4){SetMapMode(dc,8);SetViewportExtEx(dc,-1,1,0);SetViewportOrgEx(dc,16,0,0);}
            if(mode==5||mode==6)SetGraphicsMode(dc,2);
            if(mode==6){static const Transform matrix={1,0.2f,-0.25f,1,2,-2};SetWorldTransform(dc,&matrix);}
            if(mode==7)SelectClipRgn(dc,empty);
        }
        SetArcDirection(dc,angle>=8?2:1);SetROP2(dc,id>=384&&id<416?1+id%16:13);MoveToEx(dc,5,6,0);
        for(int i=0;i<256;i++)pixels[i]=0x00D1C2B3;
        SetLastError(0xABCD);
        int result=operation?Pie(dc,rect[0],rect[1],rect[2],rect[3],a.x,a.y,b.x,b.y):Arc(dc,rect[0],rect[1],rect[2],rect[3],a.x,a.y,b.x,b.y);
        DWORD error=GetLastError();Point position={-1,-1};int got=GetCurrentPositionEx(dc,&position);
        row[0]=id;row[1]=result;row[2]=error;row[3]=got;row[4]=position.x;row[5]=position.y;ROW("ARCDRAW DRAW ",row,6);
        CHECK((edge||result)&&got&&position.x==5&&position.y==6,"draw-position");GdiFlush();
        DWORD hashes[17];hashes[0]=id;
        for(int y=0;y<16;y++){
            DWORD hash=2166136261u;
            for(int x=0;x<16;x++){DWORD color=pixels[y*16+x]&0xFFFFFF;for(int b=0;b<3;b++)hash=(hash^((color>>(b*8))&255))*16777619u;}
            hashes[y+1]=hash;
            if(dump==id||dump==9999){row[0]=id;row[1]=y;for(int x=0;x<16;x++)row[x+2]=pixels[y*16+x]&0xFFFFFF;ROW("ARCDRAW PIX ",row,18);}
        }
        ROW("ARCDRAW IMAGE ",hashes,17);
#ifdef ARC_PATH_REFERENCE
        CHECK(BeginPath(dc),"path-begin");
        CHECK(operation?Pie(dc,rect[0],rect[1],rect[2],rect[3],a.x,a.y,b.x,b.y):Arc(dc,rect[0],rect[1],rect[2],rect[3],a.x,a.y,b.x,b.y),"path-arc");
        CHECK(EndPath(dc),"path-end");
        SetMapMode(dc,8);SetWindowExtEx(dc,16,16,0);SetViewportExtEx(dc,1,1,0);
        for(int flat=0;flat<2;flat++){
            if(flat)CHECK(FlattenPath(dc),"path-flatten");
            Point points[256];unsigned char types[256];int count=GetPath(dc,points,types,256);
            row[0]=id;row[1]=flat;row[2]=count;ROW("ARCPATH COUNT ",row,3);
            for(int i=0;i<count;i++){
                row[0]=id;row[1]=flat;row[2]=i;row[3]=points[i].x;row[4]=points[i].y;row[5]=types[i];ROW("ARCPATH POINT ",row,6);
            }
        }
#endif
        CHECK(RestoreDC(dc,saved),"restore");
    }
    for(int i=2;i<8;i++)CHECK(DeleteObject(pens[i]),"matrix-pen-cleanup");
    CHECK(DeleteObject(clip)&&DeleteObject(hole)&&DeleteObject(ring)&&DeleteObject(empty),"matrix-region-cleanup");
    SelectObject(dc,old_bitmap);CHECK(DeleteObject(pen)&&DeleteObject(geometric)&&DeleteObject(fill)&&DeleteObject(bitmap)&&DeleteDC(dc),"cleanup");
    if(!failures)SAY("ARCDRAW DONE\n");ExitProcess(failures?1:0);
}
