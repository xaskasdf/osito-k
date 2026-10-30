/* The same off-screen bitmap probe runs on Windows and Osito-K. Compare its
 * POLYDRAW records byte for byte; do not use the implementation as an oracle. */
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
#define SAY(s) do { DWORD n_; WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&n_,0); } while(0)
#define HEX(v) do { DWORD v_=(DWORD)(v),n_; char s_[9]; for(int j_=0;j_<8;j_++) s_[j_]="0123456789ABCDEF"[(v_>>(28-4*j_))&15]; s_[8]=' '; WriteFile(GetStdHandle((DWORD)-11),s_,9,&n_,0); } while(0)
#define ROW(tag,values,count) do { char text_[192]; DWORD n_=0,w_; const char prefix_[]=tag; for(DWORD j_=0;j_<sizeof(prefix_)-1;j_++) text_[n_++]=prefix_[j_]; for(int i_=0;i_<(count);i_++){DWORD v_=(DWORD)(values)[i_];for(int j_=0;j_<8;j_++)text_[n_++]="0123456789ABCDEF"[(v_>>(28-4*j_))&15];text_[n_++]=' ';}text_[n_++]='\n';WriteFile(GetStdHandle((DWORD)-11),text_,n_,&w_,0); }while(0)
#define CHECK(c,s) do { if(c) SAY("POLYDRAW PASS " s "\n"); else { SAY("POLYDRAW FAIL " s "\n"); failures++; } } while(0)
#define IMPORT(ret,name,args) typedef ret (CALL *name##_fn) args; name##_fn name=(name##_fn)GetProcAddress(gdi,#name); if(!name) { SAY("POLYDRAW MISSING " #name "\n"); failures++; }

void polygon_draw_entry(void)
{
    DWORD failures=0;
    int dump=-1;
    const char *command=GetCommandLineA();
    for(int i=0;command[i];i++) {
        if(command[i]=='d'&&command[i+1]=='u'&&command[i+2]=='m'&&command[i+3]=='p'&&command[i+4]=='=') {
            dump=0;for(int j=i+5;command[j]>='0'&&command[j]<='9';j++)dump=dump*10+command[j]-'0';
            break;
        }
    }
    HANDLE gdi=LoadLibraryA("gdi32.dll");
    IMPORT(HANDLE,CreateCompatibleDC,(HANDLE));
    IMPORT(int,DeleteDC,(HANDLE));
    IMPORT(HANDLE,CreateDIBSection,(HANDLE,const Info *,DWORD,void **,HANDLE,DWORD));
    IMPORT(HANDLE,SelectObject,(HANDLE,HANDLE));
    IMPORT(HANDLE,GetStockObject,(int));
    IMPORT(HANDLE,GetCurrentObject,(HANDLE,DWORD));
    IMPORT(DWORD,GetObjectType,(HANDLE));
    IMPORT(int,GetObjectA,(HANDLE,int,void *));
    IMPORT(int,DeleteObject,(HANDLE));
    IMPORT(HANDLE,CreatePen,(int,int,DWORD));
    IMPORT(HANDLE,CreateSolidBrush,(DWORD));
    IMPORT(int,Polygon,(HANDLE,const Point *,int));
    IMPORT(int,PolyPolygon,(HANDLE,const Point *,const int *,int));
    IMPORT(int,Polyline,(HANDLE,const Point *,int));
    IMPORT(int,MoveToEx,(HANDLE,int,int,Point *));
    IMPORT(int,LineTo,(HANDLE,int,int));
    IMPORT(int,GetCurrentPositionEx,(HANDLE,Point *));
    IMPORT(int,GetPolyFillMode,(HANDLE));
    IMPORT(int,SetPolyFillMode,(HANDLE,int));
    IMPORT(int,GetROP2,(HANDLE));
    IMPORT(int,SetROP2,(HANDLE,int));
    IMPORT(DWORD,SetDCPenColor,(HANDLE,DWORD));
    IMPORT(DWORD,GetDCPenColor,(HANDLE));
    IMPORT(int,SaveDC,(HANDLE));
    IMPORT(int,RestoreDC,(HANDLE,int));
    IMPORT(int,SetViewportOrgEx,(HANDLE,int,int,Point *));
    IMPORT(int,SetWindowOrgEx,(HANDLE,int,int,Point *));
    IMPORT(int,SetMapMode,(HANDLE,int));
    IMPORT(int,SetWindowExtEx,(HANDLE,int,int,Point *));
    IMPORT(int,SetViewportExtEx,(HANDLE,int,int,Point *));
    IMPORT(int,LPtoDP,(HANDLE,Point *,int));
    IMPORT(int,DPtoLP,(HANDLE,Point *,int));
    IMPORT(HANDLE,CreateRectRgn,(int,int,int,int));
    IMPORT(int,SelectClipRgn,(HANDLE,HANDLE));
    IMPORT(int,GdiFlush,(void));
    if(failures) ExitProcess(1);
    HANDLE dc=CreateCompatibleDC(0);
    static const Info info={40,16,-16,1,32,0,0,0,0,0,0};
    DWORD *pixels=0;
    HANDLE bitmap=CreateDIBSection(dc,&info,0,(void **)&pixels,0,0);
    HANDLE old_bitmap=SelectObject(dc,bitmap);
    CHECK(dc&&bitmap&&pixels,"bitmap");
    if(!dc||!bitmap||!pixels) ExitProcess(1);
    HANDLE black=GetStockObject(7),nullpen=GetStockObject(8),dcpen=GetStockObject(19);
    CHECK(GetCurrentObject(dc,1)==black&&GetPolyFillMode(dc)==1&&GetROP2(dc)==13&&GetDCPenColor(dc)==0,"default-state");
    int round_state=SaveDC(dc);
    SetMapMode(dc,8);SetWindowExtEx(dc,2,2,0);SetViewportExtEx(dc,3,3,0);
    Point fractions[7];DWORD round_row[14];
    for(int i=0;i<7;i++) fractions[i]=(Point){i-3,3-i};
    CHECK(LPtoDP(dc,fractions,7),"fractional-transform");
    for(int i=0;i<7;i++){round_row[2*i]=fractions[i].x;round_row[2*i+1]=fractions[i].y;}
    ROW("POLYDRAW ROUND ",round_row,14);
    CHECK(DPtoLP(dc,fractions,7),"fractional-inverse");
    for(int i=0;i<7;i++){round_row[2*i]=fractions[i].x;round_row[2*i+1]=fractions[i].y;}
    ROW("POLYDRAW INVERSE ",round_row,14);RestoreDC(dc,round_state);
    HANDLE pen=CreatePen(0,0,0x00332211),brush=CreateSolidBrush(0x0066AA44);
    LogPen lp;
    CHECK(pen&&GetObjectType(pen)==1&&GetObjectA(pen,sizeof(lp),&lp)==16&&lp.style==0&&lp.width.x==0&&lp.width.y==0&&lp.color==0x00332211,"logical-pen");
    CHECK(SelectObject(dc,pen)==black&&GetCurrentObject(dc,1)==pen,"select-pen");
    HANDLE old_brush=SelectObject(dc,brush);
    CHECK(SetPolyFillMode(dc,2)==1&&SetROP2(dc,7)==13&&SetDCPenColor(dc,0x00224488)==0,"set-state");
    int saved=SaveDC(dc);
    SetPolyFillMode(dc,1);SetROP2(dc,13);SetDCPenColor(dc,0);SelectObject(dc,nullpen);
    CHECK(saved&&RestoreDC(dc,-1)&&GetPolyFillMode(dc)==2&&GetROP2(dc)==7&&GetDCPenColor(dc)==0x00224488&&GetCurrentObject(dc,1)==pen,"restore-state");
    SAY("POLYDRAW STATE ");HEX(SetPolyFillMode(dc,3));HEX(GetPolyFillMode(dc));HEX(SetROP2(dc,0));HEX(GetROP2(dc));SAY("\n");
    SetPolyFillMode(dc,2);SetROP2(dc,7);
    static const Point shapes[][8]={
        {{2,2},{12,2},{12,12},{2,12}},
        {{2,2},{12,5},{5,13}},
        {{2,2},{13,13},{2,13},{13,2}},
        {{2,2},{12,2},{12,6},{6,6},{6,12},{2,12}},
        {{1,1},{14,1},{14,14},{1,14},{4,4},{11,4},{11,11},{4,11}},
        {{1,1},{14,1},{14,14},{1,14},{4,4},{4,11},{11,11},{11,4}},
        {{1,1},{9,1},{9,9},{1,9},{5,5},{14,5},{14,14},{5,14}},
        {{2,2},{12,7}},
        {{2,2},{7,7},{12,12}},
        {{2,2},{12,2},{12,12},{2,12},{2,2}},
        {{-4,-3},{21,1},{9,23}},
        {{3,3},{3,3},{12,3},{3,12}},
        {{2,2},{12,7},{2,2},{12,7}},
        {{12,2},{2,7}},
        {{2,12},{7,2}},
        {{12,12},{7,2}}
    };
    static const int sizes[]={4,3,4,6,8,8,8,2,3,5,3,4,4,2,2,2};
    DWORD seed=0x917BCC21;
    for(int id=0;id<144;id++) {
        Point points[12];int counts[2]={0,0};int polygons=1;
        int shape=id%16,mode=(id/16)%2+1,pen_mode=id/32;
        int count=sizes[shape];
        for(int i=0;i<count;i++) points[i]=shapes[shape][i];
        if(shape>=4&&shape<=6) { polygons=2;counts[0]=4;counts[1]=4; }
        else counts[0]=count;
        int state=SaveDC(dc);
        SetROP2(dc,id>=96&&id<112?id-95:13);
        SetPolyFillMode(dc,mode);
        SelectObject(dc,pen_mode==0?nullpen:pen_mode==1?pen:dcpen);
        if(id>=64&&id<96) {
            HANDLE clip=CreateRectRgn(3,4,12,11);SelectClipRgn(dc,clip);DeleteObject(clip);
            SetWindowOrgEx(dc,-1,1,0);SetViewportOrgEx(dc,2,1,0);
            if(id>=80) {SetMapMode(dc,8);SetWindowExtEx(dc,2,2,0);SetViewportExtEx(dc,3,-2,0);SetViewportOrgEx(dc,0,14,0);}
        }
        if(id>=96&&id<112) {
            count=8;polygons=2;counts[0]=4;counts[1]=4;
            for(int i=0;i<8;i++) {seed=seed*1664525+1013904223;points[i].x=(int)((seed>>20)%22)-3;seed=seed*1664525+1013904223;points[i].y=(int)((seed>>20)%22)-3;}
        }
        if(id==90) {
            Point converted[3];for(int i=0;i<3;i++)converted[i]=points[i];LPtoDP(dc,converted,3);
            DWORD coords[6];for(int i=0;i<3;i++){coords[i*2]=converted[i].x;coords[i*2+1]=converted[i].y;}
            ROW("POLYDRAW MAPPED ",coords,6);
        }
        if(id>=112) {
            static const Point directions[]={{10,3},{-10,3},{3,10},{3,-10},{10,-3},{-10,-3},{-3,10},{-3,-10}};
            Point delta=directions[id%8];points[0]=(Point){8,8};points[1]=(Point){8+delta.x,8+delta.y};
            if(id&8) {Point swap=points[0];points[0]=points[1];points[1]=swap;}
            count=2;polygons=1;counts[0]=2;
        }
        for(int i=0;i<256;i++) pixels[i]=0x00D1C2B3;
        MoveToEx(dc,9,-7,0);
        SetLastError(0xABCD);
        int result;
        if(id>=128) {MoveToEx(dc,points[0].x,points[0].y,0);result=LineTo(dc,points[1].x,points[1].y);}
        else if(id>=112) result=Polyline(dc,points,count);
        else result=polygons==1?Polygon(dc,points,count):PolyPolygon(dc,points,counts,polygons);
        DWORD error=GetLastError();Point position;
        GetCurrentPositionEx(dc,&position);GdiFlush();
        DWORD row[18];row[0]=id;row[1]=result;row[2]=error;row[3]=position.x;row[4]=position.y;
        ROW("POLYDRAW CASE ",row,5);
        DWORD hashes[17];hashes[0]=id;
        for(int y=0;y<16;y++) {
            DWORD hash=2166136261u;
            for(int x=0;x<16;x++) { DWORD color=pixels[y*16+x]&0xFFFFFF;for(int b=0;b<3;b++)hash=(hash^((color>>(b*8))&255))*16777619u; }
            hashes[y+1]=hash;
            if(dump==id) { row[0]=id;row[1]=y;for(int x=0;x<16;x++)row[x+2]=pixels[y*16+x]&0xFFFFFF;ROW("POLYDRAW PIX ",row,18); }
        }
        ROW("POLYDRAW IMAGE ",hashes,17);
        CHECK(result&&position.x==(id>=128?points[1].x:9)&&position.y==(id>=128?points[1].y:-7),"draw-position");
        RestoreDC(dc,state);
    }
    static const Point line[]={{2,3},{13,8},{4,13}};
    int counts[2]={3,1};
    for(int test=0;test<9;test++) {
        SetLastError(0xABCD);
        int value=0;
        if(test==0)value=Polygon(0,line,3);
        if(test==1)value=Polygon(dc,0,3);
        if(test==2)value=Polygon(dc,line,0);
        if(test==3)value=Polygon(dc,line,1);
        if(test==4)value=Polygon(dc,line,-1);
        if(test==5)value=PolyPolygon(dc,line,counts,0);
        if(test==6)value=PolyPolygon(dc,line,counts,-1);
        if(test==7)value=PolyPolygon(dc,line,0,1);
        if(test==8)value=PolyPolygon(dc,line,counts,2);
        DWORD error=GetLastError();SAY("POLYDRAW INVALID ");HEX(test);HEX(value);HEX(error);SAY("\n");
    }
    SetROP2(dc,13);SetPolyFillMode(dc,1);SelectObject(dc,pen);
    CHECK(MoveToEx(dc,2,3,0)&&LineTo(dc,13,8)&&GetCurrentPositionEx(dc,(Point *)&lp)&&lp.style==13&&lp.width.x==8,"line-updates-position");
    CHECK(Polyline(dc,line,3)&&GetCurrentPositionEx(dc,(Point *)&lp)&&lp.style==13&&lp.width.x==8,"polyline-preserves-position");
    HANDLE lifetime=CreatePen(0,1,0x00553311),second=CreateCompatibleDC(0);
    SelectObject(dc,lifetime);SelectObject(second,lifetime);
    int lifetime_state=SaveDC(dc);
    CHECK(lifetime&&second&&lifetime_state&&GetObjectA(lifetime,sizeof(lp),&lp)==16,"shared-pen");
    SelectObject(dc,black);SelectObject(second,black);
    CHECK(RestoreDC(dc,lifetime_state)&&GetCurrentObject(dc,1)==lifetime&&GetObjectA(lifetime,sizeof(lp),&lp)==16,"saved-pen-reference");
    SelectObject(dc,black);
    CHECK(DeleteObject(lifetime)&&DeleteDC(second),"shared-pen-cleanup");
    SelectObject(dc,black);SelectObject(dc,old_brush);SelectObject(dc,old_bitmap);
    CHECK(DeleteObject(pen)&&DeleteObject(brush)&&DeleteObject(bitmap)&&DeleteDC(dc),"cleanup");
    if(!failures) SAY("POLYDRAW DONE\n");
    ExitProcess(failures?1:0);
}
