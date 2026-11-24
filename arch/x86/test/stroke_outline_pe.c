/* The same off-screen bitmap probe runs on Windows and Osito-K. Compare its
 * EXTPEN records byte for byte; do not use the implementation as an oracle. */
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
#define CHECK(c,s) do { if(c) SAY("EXTPEN PASS " s "\n"); else { SAY("EXTPEN FAIL " s "\n"); failures++; } } while(0)
#define IMPORT(ret,name,args) typedef ret (CALL *name##_fn) args; name##_fn name=(name##_fn)GetProcAddress(gdi,#name); if(!name) { SAY("EXTPEN MISSING " #name "\n"); failures++; }


typedef struct { DWORD style,color; SIZE_T hatch; } Brush;
typedef struct { DWORD style,width,brush_style,color; SIZE_T hatch; DWORD count,entries[16]; } ExtPen;

void stroke_outline_entry(void)
{
    DWORD failures=0;
    HANDLE gdi=LoadLibraryA("gdi32.dll");
    IMPORT(HANDLE,CreateCompatibleDC,(HANDLE));
    IMPORT(HANDLE,ExtCreatePen,(DWORD,DWORD,const Brush *,DWORD,const DWORD *));
    IMPORT(HANDLE,SelectObject,(HANDLE,HANDLE));
    IMPORT(int,DeleteObject,(HANDLE));
    IMPORT(int,DeleteDC,(HANDLE));
    IMPORT(int,BeginPath,(HANDLE));
    IMPORT(int,EndPath,(HANDLE));
    IMPORT(int,WidenPath,(HANDLE));
    IMPORT(int,GetPath,(HANDLE,Point *,unsigned char *,int));
    IMPORT(int,Polyline,(HANDLE,const Point *,int));
    IMPORT(int,Polygon,(HANDLE,const Point *,int));
    IMPORT(int,SetMapMode,(HANDLE,int));
    IMPORT(int,SetWindowExtEx,(HANDLE,int,int,Point *));
    IMPORT(int,SetViewportExtEx,(HANDLE,int,int,Point *));
    if(failures)ExitProcess(1);
    static const Point shapes[8][4]={{{2,4},{13,4}},{{4,2},{4,13}},{{2,2},{13,9}},{{2,13},{9,2}},{{2,3},{12,3},{12,12}},{{2,2},{12,11},{2,11}},{{-4,4},{8,8},{20,4}},{{2,2},{13,2},{13,13},{2,13}}};
    for(int id=0;id<96;id++) {
        HANDLE dc=CreateCompatibleDC(0);Brush brush={0,0x112233,0};
        DWORD width=id/24,style=0x10000|((id/8)%3)<<8;
        int shape=id%8;if(shape>=4)style|=((id/24)%3)<<12;
        HANDLE pen=ExtCreatePen(style,width,&brush,0,0),old=SelectObject(dc,pen);
        CHECK(BeginPath(dc),"begin-path");
        CHECK(shape==7?Polygon(dc,shapes[shape],4):Polyline(dc,shapes[shape],shape>=4?3:2),"path-line");
        CHECK(EndPath(dc)&&WidenPath(dc),"widen-path");
        SetMapMode(dc,8);SetWindowExtEx(dc,16,16,0);SetViewportExtEx(dc,1,1,0);
        Point points[128];unsigned char types[128];int count=GetPath(dc,points,types,128);
        DWORD row[6]={id,width,style,(DWORD)count};ROW("OUTLINE CASE ",row,4);
        for(int i=0;i<count;i++){row[0]=id;row[1]=i;row[2]=points[i].x;row[3]=points[i].y;row[4]=types[i];ROW("OUTLINE POINT ",row,5);}
        SelectObject(dc,old);DeleteObject(pen);DeleteDC(dc);
    }
    ExitProcess(failures?1:0);
}
