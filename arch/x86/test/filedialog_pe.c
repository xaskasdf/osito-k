/* Native and guest modal file/folder dialog contract. Only fixture-owned files
 * are used; hooks choose or cancel each dialog without manual interaction. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned short WCHAR;
typedef __SIZE_TYPE__ SIZE_T;
typedef __UINTPTR_TYPE__ UINT_PTR;
typedef __INTPTR_TYPE__ LONG_PTR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,HANDLE);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
API void CALL SetLastError(DWORD);
API DWORD CALL GetLastError(void);
API const char *CALL GetCommandLineA(void);
#define SAY(s) do { DWORD written_;WriteFile(GetStdHandle((DWORD)-11),s,sizeof(s)-1,&written_,0); } while(0)
#define CHECK(c,s) do { if(c)SAY("FILEDLG PASS " s "\n");else{SAY("FILEDLG FAIL " s "\n");failures++;} }while(0)
#define IMPORT(module,ret,name,args) typedef ret(CALL *name##_fn) args;name##_fn name=(name##_fn)GetProcAddress(module,#name);CHECK(name!=0,"import-" #name)
typedef UINT_PTR(CALL *Hook)(HANDLE,DWORD,UINT_PTR,LONG_PTR);
typedef struct {
    DWORD size;HANDLE owner,instance;const void *filter;void *custom;DWORD max_custom,index;
    void *file;DWORD max_file;void *file_title;DWORD max_title;const void *initial,*title;
    DWORD flags;WORD offset,extension;const void *defext;LONG_PTR data;Hook hook;
    const void *template_name;void *reserved;DWORD reserved_flags,flags_ex;
} OpenFile;
typedef struct {HANDLE window;UINT_PTR id;DWORD code;} Header;
typedef struct {HANDLE owner;const void *root;void *display;const void *title;DWORD flags;Hook callback;LONG_PTR data;int image;} Browse;
static HANDLE (CALL *parent_window)(HANDLE);
static int (CALL *post_message)(HANDLE,DWORD,UINT_PTR,LONG_PTR);
static LONG_PTR (CALL *send_message)(HANDLE,DWORD,UINT_PTR,LONG_PTR);
static LONG_PTR (CALL *set_window_long)(HANDLE,int,LONG_PTR);
static UINT_PTR(CALL *start_timer)(HANDLE,UINT_PTR,DWORD,void *);
static int(CALL *stop_timer)(HANDLE,UINT_PTR);
static OpenFile *active;
static int action,hook_initialized,hook_file_ok,hook_cancelled,wide_call;
static DWORD failures;
static WCHAR current_directory[260];
void *memset(void *data,int byte,SIZE_T count)
{
    volatile unsigned char *p=data;for(SIZE_T i=0;i<count;i++)p[i]=(unsigned char)byte;
    return data;
}
static void write_record(const char *tag,const DWORD *values,int count)
{
    char text[240];DWORD size=0,written;
    while(*tag)text[size++]=*tag++;
    for(int i=0;i<count;i++){for(int j=0;j<8;j++)text[size++]="0123456789ABCDEF"[(values[i]>>(28-4*j))&15];text[size++]=' ';}
    text[size++]='\n';WriteFile(GetStdHandle((DWORD)-11),text,size,&written,0);
}
static UINT_PTR CALL file_hook(HANDLE window,DWORD message,UINT_PTR wp,LONG_PTR lp)
{
    (void)wp;
    HANDLE dialog=(active->flags&0x80000)?parent_window(window):window;
    if(message==0x110){
        hook_initialized++;
        CHECK((void *)lp==active,"hook-descriptor");
    }
    if((message==0x110&&!(active->flags&0x80000))||
        (message==0x4e&&((Header *)lp)->code==(DWORD)-601)){
        if(action)start_timer(window,0x321,350,0);
        else {post_message(dialog,0x111,2,0);hook_cancelled++;}
    }
    if(message==0x113&&wp==0x321){stop_timer(window,0x321);post_message(dialog,0x111,1,0);}
    if(message==0x4e){DWORD values[2]={((Header *)lp)->code,(DWORD)action};write_record("FILEDLG NOTIFY ",values,2);}
    if(message==0x4e&&((Header *)lp)->code==(DWORD)-606){
        hook_file_ok++;
        if(action==2){set_window_long(window,0,1);post_message(dialog,0x111,2,0);return 1;}
    }
    return 0;
}
static UINT_PTR CALL browse_hook(HANDLE window,DWORD message,UINT_PTR wp,LONG_PTR lp)
{
    (void)wp;(void)lp;
    if(message==1){
        hook_initialized++;
        if(action)send_message(window,0x467,1,(LONG_PTR)current_directory);
        post_message(window,0x111,action?1:2,0);
    }
    return 0;
}
void filedialog_entry(void)
{
    HANDLE kernel=LoadLibraryA("kernel32.dll"),user=LoadLibraryA("user32.dll"),common=LoadLibraryA("comdlg32.dll"),shell=LoadLibraryA("shell32.dll"),ole=LoadLibraryA("ole32.dll");
    IMPORT(common,int,GetOpenFileNameA,(OpenFile *));IMPORT(common,int,GetOpenFileNameW,(OpenFile *));
    IMPORT(common,int,GetSaveFileNameA,(OpenFile *));IMPORT(common,int,GetSaveFileNameW,(OpenFile *));
    IMPORT(common,DWORD,CommDlgExtendedError,(void));
    IMPORT(kernel,HANDLE,CreateFileA,(const char *,DWORD,DWORD,void *,DWORD,DWORD,HANDLE));
    IMPORT(kernel,int,CloseHandle,(HANDLE));IMPORT(kernel,int,DeleteFileA,(const char *));
    IMPORT(kernel,DWORD,GetCurrentDirectoryW,(DWORD,WCHAR *));
    IMPORT(shell,HANDLE,SHBrowseForFolderA,(Browse *));IMPORT(shell,HANDLE,SHBrowseForFolderW,(Browse *));
    IMPORT(shell,int,SHGetPathFromIDListA,(const void *,char *));IMPORT(shell,int,SHGetPathFromIDListW,(const void *,WCHAR *));
    IMPORT(shell,LONG_PTR,SHGetMalloc,(void *));
    IMPORT(ole,LONG_PTR,CoInitializeEx,(void *,DWORD));IMPORT(ole,void,CoUninitialize,(void));
    IMPORT(ole,void,CoTaskMemFree,(void *));
    IMPORT(ole,DWORD,CoGetMalloc,(DWORD,void *));
    parent_window=(void *)GetProcAddress(user,"GetParent");post_message=(void *)GetProcAddress(user,"PostMessageA");
    send_message=(void *)GetProcAddress(user,"SendMessageW");set_window_long=(void *)GetProcAddress(user,sizeof(void *)==4?"SetWindowLongA":"SetWindowLongPtrA");
    start_timer=(void *)GetProcAddress(user,"SetTimer");stop_timer=(void *)GetProcAddress(user,"KillTimer");
    CHECK(parent_window&&post_message&&send_message&&set_window_long&&start_timer&&stop_timer,"hook-support");
    int only=-1;const char *command=GetCommandLineA();
    for(int i=0;command[i];i++)if(command[i]=='c'&&command[i+1]=='a'&&command[i+2]=='s'&&command[i+3]=='e'&&command[i+4]=='='){
        only=0;for(int j=i+5;command[j]>='0'&&command[j]<='9';j++)only=only*10+command[j]-'0';break;
    }
    if(failures)ExitProcess(1);
    CHECK(CommDlgExtendedError()==0,"initial-error");
    OpenFile bad={0};DWORD row[8];
    for(int i=0;i<4;i++){
        bad.size=i==0?0:i==1?1:i==2?sizeof(bad)-1:sizeof(bad)+4;
        SetLastError(0xABCD);row[0]=i;row[1]=GetOpenFileNameW(&bad);row[2]=CommDlgExtendedError();row[3]=GetLastError();
        write_record("FILEDLG BADSIZE ",row,4);CHECK(!row[1]&&row[2]==1,"bad-size");
    }
    HANDLE created=CreateFileA("osito-ofn-fixture.txt",0x40000000,3,0,2,0x80,0);
    CHECK(created!=(HANDLE)(UINT_PTR)-1&&created&&CloseHandle(created),"fixture-file");
    CHECK(GetCurrentDirectoryW(260,current_directory)>0,"current-directory");
    static const WCHAR title_w[]={'O','s','i','t','o',' ','f','i','l','e',' ','d','i','a','l','o','g',' ','p','r','o','b','e',0};
    static const WCHAR extension_w[]={'t','x','t',0};
    static const WCHAR filter_w[]={'T','e','x','t',0,'*','.','t','x','t',0,'A','l','l',0,'*','.','*',0,0};
    static const char filter_a[]="Text\0*.txt\0All\0*.*\0";
    for(int id=0;id<14;id++){
        if(only>=0&&only!=id)continue;
        char file_a[1024]={0},title_a[64]={0};WCHAR file_w[1024]={0},file_title_w[64]={0};
        wide_call=id&1;int save=id>=6,old_style=id<2;
        action=id<4?0:id==10||id==11?2:1;
        const char *initial=save?"osito-ofn-new":"osito-ofn-fixture.txt";
        int prefix=0;
        if(id!=8&&id!=9){
            while(current_directory[prefix]){file_a[prefix]=(char)current_directory[prefix];file_w[prefix]=current_directory[prefix];prefix++;}
            file_a[prefix]='\\';file_w[prefix++]='\\';
        }
        for(int i=0;initial[i];i++){file_a[prefix+i]=initial[i];file_w[prefix+i]=(WCHAR)initial[i];}
        char directory_a[260];int directory_length=0;
        while(current_directory[directory_length]){directory_a[directory_length]=(char)current_directory[directory_length];directory_length++;}
        directory_a[directory_length]=0;
        OpenFile ofn={0};ofn.size=sizeof(ofn);ofn.file=wide_call?(void *)file_w:(void *)file_a;
        ofn.max_file=id==8||id==9?16:1024;ofn.file_title=wide_call?(void *)file_title_w:(void *)title_a;ofn.max_title=id>=12?8:64;
        ofn.initial=wide_call?(const void *)current_directory:(const void *)directory_a;
        ofn.title=wide_call?(const void *)title_w:"Osito file dialog probe";
        ofn.filter=wide_call?(const void *)filter_w:(const void *)filter_a;ofn.index=1;
        ofn.defext=wide_call?(const void *)extension_w:"txt";
        ofn.flags=0x20|8|4|(old_style?0:0x80000)|(save?0:0x1000);ofn.hook=file_hook;
        active=&ofn;hook_initialized=hook_file_ok=hook_cancelled=0;
        SAY("FILEDLG BEGIN\n");row[0]=id;
        row[1]=save?(wide_call?GetSaveFileNameW(&ofn):GetSaveFileNameA(&ofn)):(wide_call?GetOpenFileNameW(&ofn):GetOpenFileNameA(&ofn));
        row[2]=CommDlgExtendedError();row[3]=hook_initialized;row[4]=hook_file_ok;row[5]=ofn.index;
        row[6]=ofn.offset;row[7]=ofn.extension;
        write_record("FILEDLG DIALOG ",row,8);
        char visible[200];DWORD visible_length=0,written;
        while(visible_length<199&&(wide_call?file_w[visible_length]:file_a[visible_length])){
            visible[visible_length]=wide_call?(char)file_w[visible_length]:file_a[visible_length];visible_length++;
        }
        SAY("FILEDLG PATH ");WriteFile(GetStdHandle((DWORD)-11),visible,visible_length,&written,0);SAY("\n");
        CHECK(hook_initialized==1,"hook-initialized-once");
        /* Current Windows may return TRUE with an installed hook even when
         * CommDlgExtendedError reports FNERR_BUFFERTOOSMALL. Preserve the
         * raw BOOL above; check the buffer/error contract independently. */
        if(id==8||id==9)CHECK(row[2]==0x3003&&*(WORD *)ofn.file>ofn.max_file,"buffer-too-small");
        else if(action!=1)CHECK(!row[1]&&!row[2],"cancel-or-veto");
        else{
            CHECK(row[1]&&!row[2],"file-accepted");
            const char *expected=save?"osito-ofn-new.txt":"osito-ofn-fixture.txt";int equal=1,i;
            for(i=0;expected[i];i++)if((wide_call?file_w[ofn.offset+i]:(unsigned char)file_a[ofn.offset+i])!=(unsigned char)expected[i])equal=0;
            equal=equal&&!(wide_call?file_w[ofn.offset+i]:file_a[ofn.offset+i]);
            CHECK(equal,"file-name");CHECK(ofn.extension==ofn.offset+(save?14:18),"extension-offset");
            equal=1;for(i=0;expected[i]&&i<(id>=12?7:63);i++)if((wide_call?file_title_w[i]:(unsigned char)title_a[i])!=(unsigned char)expected[i])equal=0;
            CHECK(equal&&!(wide_call?file_title_w[i]:title_a[i]),"file-title");
        }
    }
    if(only>=0){CHECK(DeleteFileA("osito-ofn-fixture.txt"),"fixture-cleanup");ExitProcess(failures?1:0);}
    CHECK((DWORD)CoInitializeEx(0,2)<=1,"shell-apartment");
    for(int id=0;id<4;id++){
        char display_a[260]={0},path_a[260]={0};WCHAR display_w[260]={0},path_w[260]={0};
        action=id>=2;wide_call=id&1;hook_initialized=0;
        Browse info={0};info.display=wide_call?(void *)display_w:(void *)display_a;info.flags=1;info.callback=browse_hook;
        info.title=wide_call?(const void *)title_w:"Osito folder dialog probe";
        HANDLE pidl=wide_call?SHBrowseForFolderW(&info):SHBrowseForFolderA(&info);
        row[0]=id;row[1]=pidl!=0;row[2]=hook_initialized;write_record("FILEDLG BROWSE ",row,3);
        CHECK(hook_initialized==1,"browse-initialized-once");CHECK((pidl!=0)==action,"browse-result");
        if(pidl){
            CHECK(SHGetPathFromIDListW(pidl,path_w)&&SHGetPathFromIDListA(pidl,path_a),"browse-pidl-path");
            int equal=1,i;for(i=0;current_directory[i];i++)if(current_directory[i]!=path_w[i])equal=0;
            CHECK(equal&&!path_w[i],"browse-selected-path");CoTaskMemFree(pidl);
        }
    }
    struct {void *value;DWORD guard;} allocator={0,0xA1B2C3D4};
    CHECK((DWORD)SHGetMalloc(&allocator.value)==0&&allocator.value&&allocator.guard==0xA1B2C3D4,"shell-allocator-output");
    if(allocator.value){
        void **vt=*(void ***)allocator.value;
        DWORD(CALL *query)(void *,const void *,void *)=(void *)vt[0];
        void *(CALL *allocate)(void *,SIZE_T)=(void *)vt[3];
        void *(CALL *reallocate)(void *,void *,SIZE_T)=(void *)vt[4];
        int(CALL *did_allocate)(void *,void *)=(void *)vt[7];
        void (CALL *release_memory)(void *,void *)=(void *)vt[5];
        SIZE_T(CALL *get_size)(void *,void *)=(void *)vt[6];
        DWORD(CALL *release)(void *)=(void *)vt[2];
        unsigned char *block=allocate(allocator.value,73);CHECK(block&&get_size(allocator.value,block)>=73,"shell-allocator-block");
        CHECK(did_allocate(allocator.value,0)==-1,"allocator-null-origin");
        if(block){
            int ownership=did_allocate(allocator.value,block);
            CHECK(ownership==1||ownership==-1,"allocator-block-origin");
            for(int i=0;i<73;i++)block[i]=(unsigned char)i;
            unsigned char *grown=reallocate(allocator.value,block,151);
            CHECK(grown&&get_size(allocator.value,grown)>=151,"allocator-reallocate");
            if(grown){int equal=1;for(int i=0;i<73;i++)if(grown[i]!=(unsigned char)i)equal=0;
                CHECK(equal,"allocator-preserve-data");block=grown;}
            release_memory(allocator.value,block);
        }
        struct {DWORD data1;WORD data2,data3;unsigned char data4[8];} iid={0,0,0,{0xC0,0,0,0,0,0,0,0x46}};
        struct {void *value;DWORD guard;} queried={0,0xC1D2E3F4};
        CHECK(query(allocator.value,&iid,&queried.value)==0&&queried.value==allocator.value&&queried.guard==0xC1D2E3F4,"allocator-iunknown");
        if(queried.value)release(queried.value);
        iid.data1=2;queried.value=0;
        CHECK(query(allocator.value,&iid,&queried.value)==0&&queried.value==allocator.value&&queried.guard==0xC1D2E3F4,"allocator-imalloc");
        if(queried.value)release(queried.value);
        iid.data1=0x12345678;queried.value=(void *)(UINT_PTR)123;
        CHECK(query(allocator.value,&iid,&queried.value)==0x80004002&&!queried.value&&queried.guard==0xC1D2E3F4,"allocator-reject-iid");
        queried.value=0;
        CHECK(CoGetMalloc(1,&queried.value)==0&&queried.value==allocator.value&&queried.guard==0xC1D2E3F4,"allocator-task-identity");
        if(queried.value)release(queried.value);
        release(allocator.value);
    }
    CoUninitialize();CHECK(DeleteFileA("osito-ofn-fixture.txt"),"fixture-cleanup");
    if(!failures)SAY("FILEDLG DONE\n");ExitProcess(failures?1:0);
}
