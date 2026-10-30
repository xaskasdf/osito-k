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
void extpen_entry(void)
{
    DWORD failures=0;
    int dump=-1;const char *command=GetCommandLineA();
    for(int i=0;command[i];i++) if(command[i]=='d'&&command[i+1]=='u'&&command[i+2]=='m'&&command[i+3]=='p'&&command[i+4]=='=') {
        dump=0;for(int j=i+5;command[j]>='0'&&command[j]<='9';j++)dump=dump*10+command[j]-'0';break;
    }
    HANDLE gdi=LoadLibraryA("gdi32.dll");
    IMPORT(HANDLE,ExtCreatePen,(DWORD,DWORD,const Brush *,DWORD,const DWORD *));
    IMPORT(HANDLE,CreatePen,(int,int,DWORD));
    IMPORT(int,GetObjectA,(HANDLE,int,void *));
    IMPORT(int,GetObjectW,(HANDLE,int,void *));
    IMPORT(DWORD,GetObjectType,(HANDLE));
    IMPORT(HANDLE,GetCurrentObject,(HANDLE,DWORD));
    IMPORT(HANDLE,GetStockObject,(int));
    IMPORT(int,DeleteObject,(HANDLE));
    IMPORT(HANDLE,CreateCompatibleDC,(HANDLE));
    IMPORT(int,DeleteDC,(HANDLE));
    IMPORT(HANDLE,CreateDIBSection,(HANDLE,const Info *,DWORD,void **,HANDLE,DWORD));
    IMPORT(HANDLE,CreateBitmap,(int,int,DWORD,DWORD,const void *));
    IMPORT(HANDLE,SelectObject,(HANDLE,HANDLE));
    IMPORT(int,MoveToEx,(HANDLE,int,int,Point *));
    IMPORT(int,LineTo,(HANDLE,int,int));
    IMPORT(int,Polyline,(HANDLE,const Point *,int));
    IMPORT(int,Polygon,(HANDLE,const Point *,int));
    IMPORT(int,SetBkMode,(HANDLE,int));
    IMPORT(DWORD,SetBkColor,(HANDLE,DWORD));
    IMPORT(DWORD,SetTextColor,(HANDLE,DWORD));
    IMPORT(int,SetBrushOrgEx,(HANDLE,int,int,Point *));
    IMPORT(int,SetROP2,(HANDLE,int));
    IMPORT(int,SaveDC,(HANDLE));
    IMPORT(int,RestoreDC,(HANDLE,int));
    IMPORT(int,GdiFlush,(void));
    if(failures) ExitProcess(1);
    HANDLE dc=CreateCompatibleDC(0);DWORD *pixels=0;
    static const Info info={40,16,-16,1,32,0,0,0,0,0,0};
    HANDLE bitmap=CreateDIBSection(dc,&info,0,(void **)&pixels,0,0),old_bitmap=SelectObject(dc,bitmap);
    CHECK(dc&&bitmap&&pixels,"bitmap");if(!dc||!bitmap||!pixels)ExitProcess(1);
    HANDLE black=GetStockObject(7);SelectObject(dc,GetStockObject(5));
    Brush brush={0,0x00332211,0x12345678};
    DWORD styles[17]={3,2,1,0,4,3,2,1,3,2,1,4,3,2,1,4,1};
    for(int id=0;id<79;id++) {
        DWORD style=0,width=1,count=0;const DWORD *array=0;const Brush *input=&brush;
        brush.style=0;brush.color=0x00332211;brush.hatch=0x12345678;
        if(id<9)style=id;
        else if(id<36){style=0x10200+(id-9)%9;width=(id-9)/9==0?0:(id-9)/9==1?1:3;}
        else style=0x10200;
        if((style&15)==7){count=3;array=styles;}
        if(id==36)style=0x20000;
        if(id==37)style=0x10300;
        if(id==38)style=0x13000;
        if(id==39)style=0x10009;
        if(id==40){style=0;width=0;}
        if(id==41){style=0;width=2;}
        if(id==42)width=0xFFFFFFFF;
        if(id==43)width=0x80000000;
        if(id==44)style=0x20200;
        if(id==45)style=0x10205;
        if(id==46)brush.style=1;
        if(id>=47&&id<=52){brush.style=2;brush.hatch=id-47;}
        if(id==53){brush.style=2;brush.hatch=6;}
        if(id==54){brush.style=0xFFFF;}
        if(id==55){style=7;count=0;array=0;}
        if(id==56){style=7;count=17;array=styles;}
        if(id==57){style=7;count=4;array=styles;}
        if(id==58){style=7;count=16;array=styles;}
        if(id==59){style=0;count=3;array=styles;}
        if(id==60){style=0;count=0;array=styles;}
        if(id==61){style=0;brush.style=1;}
        if(id==62){style=0;brush.style=2;brush.hatch=0;}
        if(id==63){style=0x10207;count=4;array=styles;}
        if(id>=64){style=(id-64)%6;width=2+(id-64)/6;count=0;array=0;}
        if(id==78){style=0;width=1;}
        SetLastError(0xABCD);
        HANDLE pen=id==76?GetStockObject(7):id==77?GetStockObject(6):id>=64?CreatePen((int)style,(int)width,brush.color):ExtCreatePen(style,width,input,count,array);
        DWORD row[18]={id,pen!=0,GetLastError()};ROW("EXTPEN CREATE ",row,3);
        if(!pen)continue;
        DWORD type=GetObjectType(pen);int size=GetObjectA(pen,0,0);
        ExtPen result;unsigned char *bytes=(unsigned char *)&result;
        for(SIZE_T i=0;i<sizeof(result);i++)bytes[i]=0xA5;
        SetLastError(0xABCD);int returned=GetObjectA(pen,sizeof(result),&result);DWORD err=GetLastError();
        row[0]=id;row[1]=type;row[2]=size;row[3]=returned;row[4]=err;
        if(type==11){row[5]=result.style;row[6]=result.width;row[7]=result.brush_style;row[8]=result.color;row[9]=(DWORD)result.hatch;row[10]=result.count;}
        else {LogPen *lp=(LogPen *)&result;row[5]=lp->style;row[6]=lp->width.x;row[7]=lp->width.y;row[8]=lp->color;row[9]=row[10]=0;}
        ROW("EXTPEN OBJECT ",row,11);
        if(type==11&&result.count<=16){row[0]=id;for(DWORD i=0;i<result.count;i++)row[i+1]=result.entries[i];ROW("EXTPEN STYLES ",row,(int)result.count+1);}
        int guard=1;for(SIZE_T i=(SIZE_T)returned;i<sizeof(result);i++)if(bytes[i]!=0xA5)guard=0;
        CHECK(returned>0&&guard,"object-boundary");
        static const int requests[]={0,4,15,16,20,23,24,27,28,31,32,40};
        for(int i=0;i<12;i++) {
            for(SIZE_T j=0;j<sizeof(result);j++)bytes[j]=0xA5;
            SetLastError(0xABCD);int got=GetObjectW(pen,requests[i],&result);
            DWORD state[6]={id,(DWORD)requests[i],(DWORD)got,GetLastError(),0,0};
            for(SIZE_T j=0;j<sizeof(result);j++){if(bytes[j]!=0xA5)state[4]++;if(j>=(SIZE_T)(got>0?got:0)&&bytes[j]!=0xA5)state[5]++;}
            ROW("EXTPEN QUERY ",state,6);
        }
        SelectObject(dc,pen);row[0]=id;row[1]=GetCurrentObject(dc,1)==pen;row[2]=GetCurrentObject(dc,11)==pen;
        ROW("EXTPEN CURRENT ",row,3);SelectObject(dc,black);
        CHECK(DeleteObject(pen),"delete-pen");
    }
    brush.style=0;brush.color=0x00332211;brush.hatch=0;
    for(int id=0;id<336;id++) {
        DWORD width=id<96?(DWORD)(id/24):id<144?1:3;
        DWORD cap=id<96?(DWORD)((id/8)%3):2;
        DWORD style=0x10000|(cap<<8);
        int shape=id%8;
        if(id>=96){style=(id<144?0:0x10200)|(DWORD)(1+(id/8)%6);}
        if(id>=184&&id<192)style=0x10207;
        HANDLE pattern=0;
        brush.style=0;brush.color=0x00332211;brush.hatch=0;
        if(id>=192&&id<224){
            style=0x10200;width=20;brush.style=2;brush.hatch=(id-192)%6;
            if(id>=216){
                static const DWORD color_bits[16]={0x224466,0x113355,0x557799,0x7799BB,0x224466,0x113355,0x557799,0x7799BB,0xAACCEE,0x335577,0x113355,0x557799,0x224466,0x113355,0x557799,0x7799BB};
                static const WORD mono_bits[8]={0xA5,0x5A,0x0F,0xF0,0xAA,0x55,0xCC,0x33};
                pattern=id<220?CreateBitmap(4,4,1,32,color_bits):CreateBitmap(8,8,1,1,mono_bits);
                CHECK(pattern,"pattern-bitmap");brush.style=3;brush.hatch=(SIZE_T)pattern;
            }
        }
        if(id>=224&&id<288){style=(id<256?0:0x10200)|3;width=1;}
        if(id>=288){style=((id-288)/4)%6;width=2+(id-288)/24;shape=id%4;}
        DWORD count=(style&15)==7?3:0;
        if(shape>=4&&id<96)style|=((id/24)%3)<<12;
        HANDLE pen=(id>=224&&id<240)||id>=288?CreatePen((int)style,(int)width,brush.color):ExtCreatePen(style,width,&brush,count,count?styles:0);
        DWORD row[18]={id,style,width,pen!=0};ROW("EXTPEN SETUP ",row,4);
        if(!pen)continue;
        int saved=SaveDC(dc);SelectObject(dc,pen);SetROP2(dc,id>=224&&id<288?1+id%16:13);
        SetBkMode(dc,(id&4)?2:1);SetBkColor(dc,0x00AA6644);
        SetTextColor(dc,0x00223399);
        if(id>=192&&id<224){SetBkMode(dc,(id/6)&1?2:1);if(id>=204)SetBrushOrgEx(dc,3,-2,0);}
        static const Point shapes[8][4]={{{2,4},{13,4}},{{4,2},{4,13}},{{2,2},{13,9}},{{2,13},{9,2}},{{2,3},{12,3},{12,12}},{{2,2},{12,11},{2,11}},{{-4,4},{8,8},{20,4}},{{2,2},{13,2},{13,13},{2,13}}};
        for(int i=0;i<256;i++)pixels[i]=0x00D1C2B3;
        static const Point stripe[2]={{0,8},{16,8}};
        static const Point crossed[5]={{1,2},{14,12},{1,12},{14,2},{1,2}};
        SetLastError(0xABCD);int result=id>=224&&id<288?Polyline(dc,crossed,5):id>=192&&id<224?Polyline(dc,stripe,2):shape==7?Polygon(dc,shapes[shape],4):Polyline(dc,shapes[shape],shape>=4?3:2);
        row[0]=id;row[1]=result;row[2]=GetLastError();ROW("EXTPEN DRAW ",row,3);GdiFlush();
        DWORD hashes[17];hashes[0]=id;
        for(int y=0;y<16;y++){
            DWORD hash=2166136261u;for(int x=0;x<16;x++){DWORD color=pixels[y*16+x]&0xFFFFFF;for(int b=0;b<3;b++)hash=(hash^((color>>(b*8))&255))*16777619u;}
            hashes[y+1]=hash;
            if(dump==id||dump==9999){row[0]=id;row[1]=y;for(int x=0;x<16;x++)row[x+2]=pixels[y*16+x]&0xFFFFFF;ROW("EXTPEN PIX ",row,18);}
        }
        ROW("EXTPEN IMAGE ",hashes,17);CHECK(result,"draw");RestoreDC(dc,saved);CHECK(DeleteObject(pen),"draw-cleanup");
        if(pattern)CHECK(DeleteObject(pattern),"pattern-cleanup");
    }
    SelectObject(dc,old_bitmap);CHECK(DeleteObject(bitmap)&&DeleteDC(dc),"cleanup");
    if(!failures)SAY("EXTPEN DONE\n");ExitProcess(failures?1:0);
}
