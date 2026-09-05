/* Run the same LISTBOX contracts on native Windows and OsitoK. */
typedef unsigned int UINT, DWORD;
typedef int BOOL, LONG;
typedef unsigned short WCHAR;
typedef __UINTPTR_TYPE__ UPTR;
typedef __INTPTR_TYPE__ IPTR;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
typedef struct { LONG left,top,right,bottom; } RECT;
typedef IPTR (CALL *WNDPROC)(HANDLE,UINT,UPTR,IPTR);
typedef struct {
    UINT style; WNDPROC proc; int cls_extra,wnd_extra;
    HANDLE instance,icon,cursor,background; const char *menu,*name;
} WNDCLASS;
typedef struct {
    DWORD size; LONG width,height; unsigned short planes,bpp;
    DWORD compression,image_size; LONG xppm,yppm; DWORD used,important;
} BITMAPINFO;
typedef struct { UINT type,id,item,width,height; UPTR data; } MEASURE;
typedef struct { UINT type,id,item,action,state; HANDLE window,dc; RECT rect; UPTR data; } DRAW;
typedef struct { UINT type,id; HANDLE window; UINT first; UPTR first_data;
    UINT second; UPTR second_data; DWORD locale; } COMPARE;
typedef struct { UINT type,id,item; HANDLE window; UPTR data; } DELETED;
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL GetModuleHandleA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API const char *CALL GetCommandLineA(void);
API unsigned short CALL RegisterClassA(const WNDCLASS *);
API BOOL CALL UnregisterClassA(const char *,HANDLE);
API HANDLE CALL CreateWindowExA(DWORD,const char *,const char *,DWORD,
    int,int,int,int,HANDLE,HANDLE,HANDLE,void *);
API HANDLE CALL CreateWindowExW(DWORD,const WCHAR *,const WCHAR *,DWORD,
    int,int,int,int,HANDLE,HANDLE,HANDLE,void *);
API BOOL CALL DestroyWindow(HANDLE);
API BOOL CALL IsWindow(HANDLE);
API IPTR CALL SendMessageA(HANDLE,UINT,UPTR,IPTR);
API IPTR CALL SendMessageW(HANDLE,UINT,UPTR,IPTR);
API IPTR CALL DefWindowProcA(HANDLE,UINT,UPTR,IPTR);
API IPTR CALL CallWindowProcA(WNDPROC,HANDLE,UINT,UPTR,IPTR);
API LONG CALL SetWindowLongA(HANDLE,int,LONG);
API HANDLE CALL SetFocus(HANDLE);
API HANDLE CALL GetCapture(void);
API BOOL CALL GetClientRect(HANDLE,RECT *);
API DWORD CALL GetSysColor(int);
API int CALL FillRect(HANDLE,const RECT *,HANDLE);
API HANDLE CALL CreateCompatibleDC(HANDLE);
API HANDLE CALL CreateDIBSection(HANDLE,const BITMAPINFO *,UINT,void **,HANDLE,DWORD);
API HANDLE CALL CreateSolidBrush(DWORD);
API HANDLE CALL SelectObject(HANDLE,HANDLE);
API BOOL CALL DeleteDC(HANDLE);
API BOOL CALL DeleteObject(HANDLE);
API BOOL CALL GdiFlush(void);

enum {
    ADD=0x180, INSERT, DELETE, RANGE_EX, RESET, SETSEL, SETCURSEL,
    GETSEL, GETCURSEL, GETTEXT, GETTEXTLEN, GETCOUNT, SELECTSTRING,
    DIR, GETTOP, FIND, GETSELCOUNT, GETSELITEMS, SETTABS, GETEXTENT,
    SETEXTENT, SETCOLUMN, ADDFILE, SETTOP, GETRECT, GETDATA, SETDATA,
    RANGE, SETANCHOR, GETANCHOR, SETCARET, GETCARET, SETHEIGHT,
    GETHEIGHT, FINDEXACT, SETLOCALE, GETLOCALE, SETCOUNT, RESERVE
};
static unsigned checks,failures,selections,measures,draws,deletes,compares;
static UINT last_id;
static HANDLE last_window,parent,instance,dc,dib,ink;
static DWORD *pixels;
static BOOL destroy_draw,destroy_color,ansi_payload,reentrant;
static WNDPROC previous_proc;
static IPTR (CALL *set_long_ptr)(HANDLE,int,IPTR);
static const char class_name[] = "OsitoListboxContracts";
void *memset(void *out,int value,__SIZE_TYPE__ size)
{
    unsigned char *p = out;
    for (__SIZE_TYPE__ i=0;i<size;i++) p[i]=(unsigned char)value;
    return out;
}
static void report(const char *text)
{
    DWORD size=0,written;
    while (text[size]) size++;
    WriteFile(GetStdHandle((DWORD)-11),text,size,&written,0);
}
static void number(unsigned value)
{
    char digits[12]; unsigned n=0;
    do { digits[n++]='0'+value%10; value/=10; } while (value);
    while (n) { char text[2]={digits[--n],0}; report(text); }
}
static void check(BOOL condition,const char *name)
{
    checks++;
    if (!condition) { failures++; report("FAIL: "); report(name); report("\n"); }
}
static IPTR CALL owner(HANDLE window,UINT message,UPTR wp,IPTR lp)
{
    if (message==0x111 && lp && ((wp>>16)&65535)==1) {
        selections++; last_id=(UINT)wp&65535; last_window=(HANDLE)lp;
        return 0;
    }
    if (message==0x2C) {
        MEASURE *item=(MEASURE *)lp;
        measures++;
        check(item && item->type==2 && item->id==wp,"MEASUREITEM ABI and control ID");
        item->height=item->id==302 ? 18+(item->item%3)*4 : 20;
        return 1;
    }
    if (message==0x2B) {
        DRAW *item=(DRAW *)lp;
        draws++;
        check(item && item->type==2 && item->id==wp && item->window && item->dc,
            "DRAWITEM ABI and handles");
        if (item->item!=(UINT)-1) {
            check((UPTR)SendMessageA(item->window,GETDATA,item->item,0)==item->data,
                "owner drawing receives item data at native pointer width");
            RECT rect=item->rect; rect.right+=20; rect.bottom+=20;
            FillRect(item->dc,&rect,ink);
        }
        if (destroy_draw) { destroy_draw=0; DestroyWindow(item->window); }
        return 1;
    }
    if (message==0x2D) {
        DELETED *item=(DELETED *)lp;
        deletes++;
        check(item && item->type==2 && item->id==wp && item->window,
            "DELETEITEM ABI and handles");
        return 1;
    }
    if (message==0x39) {
        COMPARE *item=(COMPARE *)lp;
        compares++;
        check(item && item->type==2 && item->id==wp && item->window,
            "COMPAREITEM ABI and handles");
        return item->first_data<item->second_data ? -1 : item->first_data>item->second_data;
    }
    if (message==0x134 && destroy_color) {
        destroy_color=0; DestroyWindow((HANDLE)lp);
        return (IPTR)ink;
    }
    return DefWindowProcA(window,message,wp,lp);
}
static IPTR CALL subclass(HANDLE window,UINT message,UPTR wp,IPTR lp)
{
    if (message==ADD) ansi_payload=lp && ((const char *)lp)[0]=='A' &&
        (unsigned char)((const char *)lp)[1]==0xE9;
    return CallWindowProcA(previous_proc,window,message,wp,lp);
}
static HANDLE list(UINT style,UINT id)
{
    HANDLE result=CreateWindowExA(0,"LISTBOX","",0x50000100u|style,
        5,5,180,80,parent,(HANDLE)(UPTR)id,instance,0);
    check(result!=0,"create listbox");
    return result;
}
static void print(HANDLE window)
{
    GdiFlush();
    for (int i=0;i<240*120;i++) pixels[i]=0x112233;
    SendMessageA(window,0x318,(UPTR)dc,4); GdiFlush();
}
static DWORD rgb(DWORD color)
{
    return ((color&255)<<16)|(color&0xFF00)|((color>>16)&255);
}
static void storage(void)
{
    HANDLE window=list(1,101);
    check(SendMessageA(window,GETCOUNT,0,0)==0 && SendMessageA(window,GETCURSEL,0,0)==-1,
        "new list is empty and unselected");
    check(SendMessageA(window,GETTEXTLEN,0,0)==-1 && SendMessageA(window,DELETE,0,0)==-1,
        "invalid item operations return LB_ERR");
    check(SendMessageA(window,ADD,0,(IPTR)"first")==0 &&
        SendMessageA(window,ADD,0,(IPTR)"second")==1,"append returns actual index");
    UPTR data=sizeof(void *)==8 ? (UPTR)0x12345678ABCDEF00ULL : (UPTR)0xABCDEF00u;
    SendMessageA(window,SETDATA,1,(IPTR)data);
    unsigned before=selections;
    check(SendMessageA(window,SETCURSEL,1,0)==1,"programmatic selection returns index");
    check(SendMessageA(window,INSERT,0,(IPTR)"new")==0 &&
        SendMessageA(window,GETCURSEL,0,0)==2 &&
        (UPTR)SendMessageA(window,GETDATA,2,0)==data,"insertion preserves selected item and pointer data");
    check(SendMessageA(window,DELETE,0,0)==2 && SendMessageA(window,GETCURSEL,0,0)==1,
        "deletion shifts selected item index");
    char text[1024]; WCHAR wide[1024];
    check(SendMessageW(window,GETTEXT,1,(IPTR)wide)==6 && wide[0]=='s' && !wide[6],
        "Unicode retrieval from ANSI listbox");
    check(selections==before,"programmatic selection does not send LBN_SELCHANGE");
    check(SendMessageA(window,SETCURSEL,(UPTR)-1,0)==-1 &&
        SendMessageA(window,GETCURSEL,0,0)==-1,"clear selection returns LB_ERR by contract");
    SendMessageA(window,RESET,0,0);
    check(SendMessageA(window,GETCOUNT,0,0)==0,"reset removes contents");
    if (sizeof(void *)==4) previous_proc=(WNDPROC)(UPTR)(UINT)SetWindowLongA(window,-4,(LONG)(UPTR)subclass);
    else previous_proc=(WNDPROC)set_long_ptr(window,-4,(IPTR)subclass);
    const WCHAR acp[]={'A',0xE9,0}; ansi_payload=0;
    SendMessageW(window,ADD,0,(IPTR)acp);
    check(ansi_payload && SendMessageW(window,GETTEXT,0,(IPTR)wide)==2 && wide[1]==0xE9,
        "list message thunk converts text for ANSI subclass and getter");
    if (sizeof(void *)==4) SetWindowLongA(window,-4,(LONG)(UPTR)previous_proc);
    else set_long_ptr(window,-4,(IPTR)previous_proc);
    SendMessageA(window,RESET,0,0); SendMessageA(window,0xB,0,0);
    check(SendMessageA(window,RESERVE,1024,4096)>=1024,"reserve storage for large list");
    BOOL grew=1;
    for (int i=0;i<1024;i++) if (SendMessageA(window,ADD,0,(IPTR)"item")!=i) grew=0;
    check(grew && SendMessageA(window,GETCOUNT,0,0)==1024,"dynamic list grows beyond 256 entries");
    for (int i=0;i<900;i++) text[i]='a'+i%26; text[900]=0;
    check(SendMessageA(window,ADD,0,(IPTR)text)==1024 &&
        SendMessageW(window,GETTEXTLEN,1024,0)==900 &&
        SendMessageW(window,GETTEXT,1024,(IPTR)wide)==900 && wide[899]==text[899],
        "item text is not truncated by a title buffer");
    SendMessageA(window,0xB,1,0);
    SendMessageA(window,SETCURSEL,1000,0);
    RECT rect;
    SendMessageA(window,GETRECT,1000,(IPTR)&rect);
    check(SendMessageA(window,GETTOP,0,0)>0 && rect.top>=0 && rect.bottom<=80,
        "selection scrolls into view");
    SendMessageA(window,SETTOP,0,0);
    SendMessageA(window,GETRECT,0,(IPTR)&rect);
    check(rect.top==0 && rect.bottom==SendMessageA(window,GETHEIGHT,0,0),"item rectangle uses row height");
    check((SendMessageA(window,0x1A9,0,2|(2<<16))&0xFFFF)==0,"hit test identifies visible row");
    SendMessageA(window,SETHEIGHT,0,20);
    check(SendMessageA(window,GETHEIGHT,0,0)==20 && SendMessageA(window,SETHEIGHT,0,256)==-1,
        "item height range and retrieval");
    DestroyWindow(window);
    static const WCHAR cls[]={'L','I','S','T','B','O','X',0};
    const WCHAR unicode[]={'A',0x03A9,0x20AC,0};
    window=CreateWindowExW(0,cls,0,0x50000100u,5,5,180,80,parent,(HANDLE)102,instance,0);
    check(window && SendMessageW(window,ADD,0,(IPTR)unicode)==0 &&
        SendMessageW(window,GETTEXT,0,(IPTR)wide)==3 && wide[1]==0x03A9 && wide[2]==0x20AC,
        "Unicode list preserves non-ACP text");
    DestroyWindow(window);
}
static void sorting_and_input(void)
{
    HANDLE window=list(3,201);
    SendMessageA(window,ADD,0,(IPTR)"beta");
    check(SendMessageA(window,ADD,0,(IPTR)"Alpha")==0 &&
        SendMessageA(window,ADD,0,(IPTR)"gamma")==2,"LBS_SORT orders inserted strings");
    check(SendMessageA(window,FIND,(UPTR)-1,(IPTR)"AL")==0 &&
        SendMessageA(window,FINDEXACT,(UPTR)-1,(IPTR)"BETA")==1 &&
        SendMessageA(window,FINDEXACT,(UPTR)-1,(IPTR)"bet")==-1,"case-insensitive prefix and exact search");
    check(SendMessageA(window,INSERT,1,(IPTR)"zeta")==1,"explicit insertion ignores LBS_SORT");
    char text[32];
    check(SendMessageA(window,GETTEXT,1,(IPTR)text)==4 && text[0]=='z',"inserted item stays at requested index");
    SendMessageA(window,RESET,0,0);
    SendMessageA(window,ADD,0,(IPTR)"Alpha"); SendMessageA(window,ADD,0,(IPTR)"Beta");
    SendMessageA(window,ADD,0,(IPTR)"Gamma");
    SetFocus(window);
    SendMessageA(window,SETCURSEL,0,0);
    unsigned before=selections;
    int height=(int)SendMessageA(window,GETHEIGHT,0,0);
    IPTR point=6|((height+3)<<16);
    SendMessageA(window,0x201,1,point); SendMessageA(window,0x202,0,point);
    check(SendMessageA(window,GETCURSEL,0,0)==1 && selections==before+1 &&
        last_id==201 && last_window==window && GetCapture()!=window,
        "mouse selects row and reports LBN_SELCHANGE with capture released");
    SendMessageA(window,0x100,0x28,0);
    check(SendMessageA(window,GETCURSEL,0,0)==2 && selections==before+2,
        "Down key selects next row and notifies");
    SendMessageA(window,0x102,'a',0);
    check(SendMessageA(window,GETCURSEL,0,0)==0,"type search selects matching item");
    check((SendMessageA(window,0x87,0,0)&0x81)==0x81,"list requests arrows and characters from dialog");
    print(window);
    check((pixels[5*240+100]&0xFFFFFF)==rgb(GetSysColor(13)),"selected row is painted with highlight color");
    check((pixels[100*240]&0xFFFFFF)==0x112233,"printing is bounded to list client");
    DestroyWindow(window);
}
static void multiple(void)
{
    HANDLE window=list(0x800,251);
    for (int i=0;i<8;i++) SendMessageA(window,ADD,0,(IPTR)"entry");
    check(SendMessageA(window,SETCURSEL,2,0)==-1,"single-selection message rejected on multi list");
    SendMessageA(window,SETSEL,1,1); SendMessageA(window,SETSEL,1,4);
    int items[4]={-1,-1,-1,-1};
    check(SendMessageA(window,GETSELCOUNT,0,0)==2 &&
        SendMessageA(window,GETSELITEMS,4,(IPTR)items)==2 && items[0]==1 && items[1]==4,
        "multiple selection count and index array");
    SendMessageA(window,SETANCHOR,1,0); SendMessageA(window,SETCARET,4,0);
    check(SendMessageA(window,GETANCHOR,0,0)==1 && SendMessageA(window,GETCARET,0,0)==4,
        "anchor and caret are independent of selection");
    SendMessageA(window,SETSEL,0,-1); SendMessageA(window,RANGE_EX,2,5);
    check(SendMessageA(window,GETSELCOUNT,0,0)==4,"inclusive extended selection range");
    SendMessageA(window,RANGE_EX,5,3);
    check(SendMessageA(window,GETSELCOUNT,0,0)==1 && SendMessageA(window,GETSEL,2,0)==1,
        "reversed extended range deselects");
    SendMessageA(window,SETSEL,1,-1);
    check(SendMessageA(window,GETSELCOUNT,0,0)==8,"select all without fixed bitmask size");
    DestroyWindow(window);
}
static void owner_drawing(void)
{
    unsigned before=measures;
    HANDLE window=list(0x12,301);
    check(measures==before+1 && SendMessageA(window,GETHEIGHT,0,0)==20,"fixed owner-draw measures once at creation");
    check(SendMessageA(window,ADD,0,30)==0 && SendMessageA(window,ADD,0,10)==0 &&
        SendMessageA(window,ADD,0,20)==1 && compares>0,"owner-sorted opaque items use comparison callback");
    UPTR value=0;
    check(SendMessageA(window,GETTEXT,1,(IPTR)&value)==sizeof(UPTR) && value==20,
        "owner list without strings returns pointer-sized item data");
    print(window);
    check(draws>0 && (pixels[3*240+3]&0xFFFFFF)==0x55AA33 &&
        (pixels[85*240+3]&0xFFFFFF)==0x112233,"owner draw clips to the control client");
    before=deletes;
    SendMessageA(window,DELETE,1,0); SendMessageA(window,RESET,0,0);
    check(deletes==before+3,"delete and reset notify once per owner-drawn item");
    DestroyWindow(window);
    window=list(0x20,302); before=measures;
    SendMessageA(window,ADD,0,40); SendMessageA(window,ADD,0,50);
    check(measures==before+2 && SendMessageA(window,GETHEIGHT,0,0)==18 &&
        SendMessageA(window,GETHEIGHT,1,0)==22,"variable owner-draw measures each item");
    RECT rect; SendMessageA(window,GETRECT,1,(IPTR)&rect);
    check(rect.top==18 && rect.bottom==40,"variable-height item geometry");
    if (reentrant) {
        destroy_draw=1; print(window);
        check(!IsWindow(window),"destruction inside draw callback does not retain list state");
        window=list(0,303); SendMessageA(window,ADD,0,(IPTR)"text");
        destroy_color=1; print(window);
        check(!IsWindow(window),"destruction inside control-color callback does not retain list state");
    } else DestroyWindow(window);
}
void mainCRTStartup(void)
{
    instance=GetModuleHandleA(0);
    const char *command=GetCommandLineA(), option[]=" --reentrant";
    for (const char *p=command; p && *p; p++) {
        unsigned i=0;
        while (option[i] && p[i]==option[i]) i++;
        if (!option[i] && (!p[i] || p[i]==' ')) reentrant=1;
    }
    set_long_ptr=GetProcAddress(GetModuleHandleA("user32.dll"),"SetWindowLongPtrA");
    WNDCLASS cls={0,owner,0,0,instance,0,0,(HANDLE)16,0,class_name};
    if (!RegisterClassA(&cls) || (sizeof(void *)==8 && !set_long_ptr)) ExitProcess(2);
    parent=CreateWindowExA(0,class_name,"Listbox contracts",0x90000000u,
        40,40,260,160,0,0,instance,0);
    dc=CreateCompatibleDC(0);
    BITMAPINFO info={40,240,-120,1,32,0,0,0,0,0,0};
    dib=CreateDIBSection(dc,&info,0,(void **)&pixels,0,0);
    if (!parent || !dc || !dib) ExitProcess(2);
    HANDLE old=SelectObject(dc,dib);
    ink=CreateSolidBrush(0x0033AA55);
    storage(); sorting_and_input(); multiple(); owner_drawing();
    DestroyWindow(parent); UnregisterClassA(class_name,instance);
    SelectObject(dc,old); DeleteObject(dib); DeleteDC(dc); DeleteObject(ink);
    report(reentrant ? "[USER32-LISTBOX-REENTRANT] checks=" : "[USER32-LISTBOX] checks="); number(checks);
    report(" failures="); number(failures); report(failures ? " FAIL\n" : " PASS\n");
    ExitProcess(failures ? 3 : 0);
}
