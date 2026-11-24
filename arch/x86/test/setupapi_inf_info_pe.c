/* Native/guest oracle. Only this probe's INF files are written. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
typedef int(CALL *INFO)(const void *,DWORD,void *,DWORD,DWORD *);
typedef int(CALL *FILEINFO)(const void *,DWORD,void *,DWORD,DWORD *);
typedef int(CALL *VERSION)(const void *,DWORD,const void *,void *,DWORD,DWORD *);
static INFO info[2];
static FILEINFO fileinfo[2];
static VERSION version[2];
static void(CALL *seterror)(DWORD);
static DWORD(CALL *geterror)(void);
static unsigned failures;
static DWORD snapshot[8192],work[8192],needed;
static char path[260];
static WORD wpath[260];
static const char fixture[]=
    "[Version]\r\nSignature=\"$CHICAGO$\"\r\nClass=System\r\n"
    "Provider=%Maker%\r\nCatalogFile=probe.cat\r\n"
    "DriverVer=01/02/2020,1.2.3.4\r\nCustom=\"alpha, beta\"\r\n"
    "Dup=one\r\nDup=two\r\nEmpty=\r\n[Strings]\r\nMaker=\"Osito Fixture\"\r\n";
static void infi_text(const char *text)
{DWORD n=0,w;while(text[n])n++;WriteFile(GetStdHandle((DWORD)-11),text,n,&w,0);}
static void infi_hex(DWORD value)
{
    char text[10];text[0]=' ';
    for(unsigned i=0;i<8;i++)text[i+1]="0123456789abcdef"[(value>>(28-i*4))&15];
    text[9]=0;infi_text(text);
}
static void infi_check(int ok,const char *name)
{
    infi_text(ok?"INFI PASS ":"INFI FAIL ");infi_text(name);infi_text("\n");
    if(!ok)failures++;
}
static int infi_info(const char *label,unsigned wide,const void *spec,DWORD search,DWORD capacity,unsigned nulls)
{
    for(unsigned i=0;i<8192;i++)work[i]=0xcccccccc;
    DWORD required=0xdeadbeef;seterror(0x13579);
    int ok=info[wide](spec,search,(nulls&1)?0:work,capacity,(nulls&2)?0:&required);
    DWORD error=geterror();
    infi_text("INFI INFO ");infi_text(label);infi_hex(wide);infi_hex(search);infi_hex(nulls);
    infi_hex(ok);infi_hex(error);infi_hex(required);infi_hex(work[0]);infi_hex(work[1]);
    unsigned changed=0;for(unsigned i=0;i<8192;i++)if(work[i]!=0xcccccccc)changed++;
    infi_hex(changed);infi_text("\n");
    if(ok&&!(nulls&1)&&!(nulls&2)&&required<=sizeof(snapshot)){
        needed=required;for(unsigned i=0;i<(needed+3)/4;i++)snapshot[i]=work[i];
    }
    return ok;
}
static void infi_queries(const char *label)
{
    static const char *keys[]={"Signature","Provider","Class","CatalogFile","DriverVer","Custom","Dup","Empty","Missing",0};
    for(unsigned wide=0;wide<2;wide++){
        for(unsigned query=0;query<11;query++){
            const char *key=query?keys[query-1]:0;WORD wkey[260];unsigned j=0;
            if(key){while(key[j]){wkey[j]=(unsigned char)key[j];j++;}wkey[j]=0;}
            for(unsigned mode=0;mode<5;mode++){
                /* Native WOW64 SetupQueryInfFileInformationA corrupts its heap
                 * on index==InfCount (r1 evidence). Exercise that error on x64. */
                if(sizeof(UP)==4&&mode==4)continue;
                union{WORD w[1024];char a[2048];} buffer;
                for(j=0;j<sizeof(buffer);j++)buffer.a[j]=(char)0xcc;
                DWORD required=0xdeadbeef,capacity=mode==0?0:mode==1?1:1024;
                void *output=mode==0?0:&buffer;
                DWORD index=mode==4?1:0;seterror(0x13579);
                int ok=query?version[wide](snapshot,index,wide?(key?wkey:0):(const void *)key,output,capacity,mode==3?0:&required):
                    fileinfo[wide](snapshot,index,output,capacity,mode==3?0:&required);
                DWORD error=geterror();
                infi_text("INFI QUERY ");infi_text(label);infi_hex(wide);infi_hex(query);infi_hex(mode);
                infi_hex(ok);infi_hex(error);infi_hex(required);infi_hex((unsigned char)buffer.a[0]);
                if(ok&&output){
                    infi_text(" |");
                    for(j=0;j<1024;j++){
                        WORD c=wide?buffer.w[j]:(unsigned char)buffer.a[j];
                        if(!c){if(query==10&&j+1<1024&&(wide?buffer.w[j+1]:(unsigned char)buffer.a[j+1]))infi_text("~");else break;}
                        else{char text[2]={(char)c,0};infi_text(text);}
                    }
                }
                infi_text("\n");
            }
        }
    }
}
void infinfo_entry(void)
{
    void *s=LoadLibraryA("setupapi.dll"),*k=LoadLibraryA("kernel32.dll");
    void *(CALL *openinf)(const char *,const char *,DWORD,DWORD *);
    void(CALL *closeinf)(void *);
    void *(CALL *create)(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
    int(CALL *close)(void *);
    DWORD(CALL *fullpath)(const char *,DWORD,char *,char **);
#define LOAD(dst,module,name) dst=(__typeof__(dst))GetProcAddress(module,name)
    LOAD(openinf,s,"SetupOpenInfFileA");LOAD(closeinf,s,"SetupCloseInfFile");
    LOAD(info[0],s,"SetupGetInfInformationA");LOAD(info[1],s,"SetupGetInfInformationW");
    LOAD(fileinfo[0],s,"SetupQueryInfFileInformationA");LOAD(fileinfo[1],s,"SetupQueryInfFileInformationW");
    LOAD(version[0],s,"SetupQueryInfVersionInformationA");LOAD(version[1],s,"SetupQueryInfVersionInformationW");
    LOAD(seterror,k,"SetLastError");LOAD(geterror,k,"GetLastError");
    LOAD(create,k,"CreateFileA");LOAD(close,k,"CloseHandle");LOAD(fullpath,k,"GetFullPathNameA");
    infi_check(openinf&&closeinf&&info[0]&&info[1]&&fileinfo[0]&&fileinfo[1]&&version[0]&&version[1]&&seterror&&geterror&&create&&close&&fullpath,"exports");
    if(failures)ExitProcess(1);
    void *file=create("infinfo-probe.inf",0x40000000,7,0,2,0x80,0);DWORD written=0;
    infi_check(file!=(void *)(UP)-1&&WriteFile(file,fixture,sizeof(fixture)-1,&written,0)&&written==sizeof(fixture)-1,"fixture");
    if(file!=(void *)(UP)-1)close(file);
    DWORD count=fullpath(".\\infinfo-probe.inf",260,path,0);infi_check(count&&count<260,"fullpath");
    for(unsigned i=0;i<=count&&i<260;i++)wpath[i]=(unsigned char)path[i];
    DWORD line;void *inf=openinf(path,0,2,&line);infi_check(inf!=(void *)(UP)-1,"open");
    if(failures)ExitProcess(1);
    for(unsigned wide=0;wide<2;wide++){
        DWORD size=0;infi_check(info[wide](inf,1,0,0,&size)&&size>=8&&size<sizeof(snapshot),"size-query");
        infi_info("null",wide,inf,1,0,1);infi_info("null-large",wide,inf,1,1234,1);
        infi_info("null-both",wide,inf,1,0,3);
        infi_info("zero",wide,inf,1,0,0);infi_info("short",wide,inf,1,size-1,0);
        infi_info("exact",wide,inf,1,size,0);infi_info("no-size",wide,inf,1,size,2);
        infi_info("absolute",wide,wide?(const void *)wpath:path,2,sizeof(work),0);
        infi_queries(wide?"fromW":"fromA");
        for(unsigned search=0;search<=6;search++)if(search!=1&&search!=2)
            infi_info("search",wide,wide?(const void *)wpath:path,search,sizeof(work),0);
        infi_info("null-handle",wide,0,1,0,1);
        infi_info("bad-handle",wide,(void *)(UP)-1,1,0,1);
    }
    infi_info("retain",0,inf,1,sizeof(work),0);
    file=create("infinfo-snapshot.bin",0x40000000,7,0,2,0x80,0);
    if(file!=(void *)(UP)-1){WriteFile(file,snapshot,needed,&written,0);close(file);}
    const char *(CALL *commandline)(void);LOAD(commandline,k,"GetCommandLineA");
    unsigned guest=0;
    for(const char *p=commandline();*p;p++)
        if(p[0]==' '&&p[1]=='g'&&p[2]=='u'&&p[3]=='e'&&p[4]=='s'&&p[5]=='t')guest=1;
    if(guest){
        DWORD output[4],required;
        for(unsigned wide=0;wide<2;wide++){
            const unsigned fields[]={0,1,2,5,6,7};
            for(unsigned i=0;i<6;i++){
                DWORD saved=snapshot[fields[i]];snapshot[fields[i]]=0xffffffff;
                output[0]=0xcccccccc;required=0xdeadbeef;seterror(0);
                int ok=fileinfo[wide](snapshot,0,output,sizeof(output)/2,&required);
                infi_check(!ok&&geterror()==13&&output[0]==0xcccccccc&&required==0xdeadbeef,"reject-corrupt-snapshot");
                snapshot[fields[i]]=saved;
            }
            required=0xdeadbeef;output[0]=0xcccccccc;
            infi_check(!fileinfo[wide](snapshot,1,output,1,&required)&&geterror()==259&&required==0xdeadbeef&&output[0]==0xcccccccc,"invalid-index");
            infi_check(!fileinfo[wide](0,0,output,1,&required)&&geterror()==87,"null-snapshot");
            infi_check(!fileinfo[wide](snapshot,0,(void *)(UP)1,1024,&required)&&geterror()==87,"invalid-output");
            infi_check(!fileinfo[wide](snapshot,0,output,1,(void *)(UP)1)&&geterror()==87,"invalid-required");
        }
        int(CALL *mkdir)(const char *,void *);int(CALL *unlink)(const char *);
        int(CALL *setenv)(const char *,const char *);
        int(CALL *regcreate)(void *,const char *,DWORD,char *,DWORD,DWORD,void *,void **,DWORD *);
        int(CALL *regget)(void *,const char *,DWORD *,DWORD *,void *,DWORD *);
        int(CALL *regset)(void *,const char *,DWORD,DWORD,const void *,DWORD);
        int(CALL *regdelete)(void *,const char *);int(CALL *regclose)(void *);
        void *a=LoadLibraryA("advapi32.dll");
        LOAD(mkdir,k,"CreateDirectoryA");LOAD(unlink,k,"DeleteFileA");LOAD(setenv,k,"SetEnvironmentVariableA");
        LOAD(regcreate,a,"RegCreateKeyExA");LOAD(regget,a,"RegQueryValueExA");LOAD(regset,a,"RegSetValueExA");
        LOAD(regdelete,a,"RegDeleteValueA");LOAD(regclose,a,"RegCloseKey");
        infi_check(mkdir&&unlink&&setenv&&regcreate&&regget&&regset&&regdelete&&regclose,"guest-search-exports");
        if(mkdir&&unlink&&setenv&&regcreate&&regget&&regset&&regdelete&&regclose){
            const char *files[]={"C:\\Windows\\inf\\osito-infinfo-search.inf","C:\\Windows\\System32\\osito-infinfo-search.inf"};
            mkdir("C:\\Windows",0);mkdir("C:\\Windows\\inf",0);mkdir("C:\\Windows\\System32",0);
            for(unsigned i=0;i<2;i++){
                file=create(files[i],0x40000000,7,0,2,0x80,0);written=0;
                infi_check(file!=(void *)(UP)-1&&WriteFile(file,fixture,sizeof(fixture)-1,&written,0)&&written==sizeof(fixture)-1,"guest-search-fixture");
                if(file!=(void *)(UP)-1)close(file);
            }
            void *key=0;DWORD disposition=0,type=0,bytes=0;
            int status=regcreate((void *)(UP)-2147483646LL,"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",0,0,0,0x2001f,0,&key,&disposition);
            infi_check(status==0&&key,"guest-devicepath-key");
            int absent=key&&regget(key,"DevicePath",0,&type,0,&bytes)==2;
            infi_check(absent,"guest-devicepath-isolated");
            const char searchlist[]="C:\\absent;%OSITO_INF_INFO_DIR%";
            if(absent){
                infi_check(setenv("OSITO_INF_INFO_DIR","C:\\Windows\\System32"),"guest-search-environment");
                infi_check(regset(key,"DevicePath",0,2,searchlist,sizeof(searchlist))==0,"guest-devicepath-value");
            }
            for(unsigned search=3;search<=5;search++){
                if(search==5&&!absent)continue;
                int ok=info[0]("osito-infinfo-search.inf",search,work,sizeof(work),&required);
                char name[260];DWORD count=0;
                ok=ok&&fileinfo[0](work,0,name,sizeof(name),&count);
                const char *expected=files[search==3?0:1];unsigned i=0;
                if(ok){while(name[i]&&name[i]==expected[i])i++;ok=name[i]==0&&expected[i]==0;}
                infi_check(ok,"guest-search-location");
            }
            if(absent){regdelete(key,"DevicePath");setenv("OSITO_INF_INFO_DIR",0);}
            if(key)regclose(key);
            for(unsigned i=0;i<2;i++)unlink(files[i]);
        }
    }
    closeinf(inf);
    file=create("infinfo-probe.inf",0x40000000,7,0,2,0x80,0);if(file!=(void *)(UP)-1)close(file);
    infi_queries("closed");
    infi_info("stale-handle",0,inf,1,0,1);
    if(!failures)infi_text("INFI DONE\n");ExitProcess(failures?1:0);
}
