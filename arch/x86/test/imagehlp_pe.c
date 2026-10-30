/* Public PE32/PE64 ImageHlp fixture. Generates disposable image files rather
 * than changing an installed application or a signed system binary. */
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef int BOOL;
typedef __UINTPTR_TYPE__ UP;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *,const void *,DWORD,DWORD *,void *);
API int CALL ReadFile(void *,void *,DWORD,DWORD *,void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL CreateFileA(const char *,DWORD,DWORD,void *,DWORD,DWORD,void *);
API int CALL CloseHandle(void *);
API int CALL DeleteFileA(const char *);
API DWORD CALL GetFileSize(void *,DWORD *);
API DWORD CALL SetFilePointer(void *,int,int *,DWORD);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
static BOOL (CALL *add)(void *,void *,DWORD *);
static BOOL (CALL *enumerate)(void *,WORD,DWORD *,DWORD *,DWORD);
static BOOL (CALL *remove_cert)(void *,DWORD);
static BYTE original[2048],readback[4096],certificates[3][64];
static DWORD lengths[3]={13,24,9},checks,failures,original_size,security,checksum;
static const char *name="osito-imagehlp-fixture.bin";
static void say(const char *s){DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(DWORD value){char text[9];for(int i=7;i>=0;i--){text[i]="0123456789abcdef"[value&15];value>>=4;}text[8]=0;say(text);}
static void check(int ok,const char *label){checks++;if(!ok){failures++;say("IMAGEHLP FAIL ");say(label);say("\n");}}
static void fill(BYTE *p,BYTE value,DWORD n){while(n--)*p++=value;}
static int equal(const BYTE *a,const BYTE *b,DWORD n){while(n--)if(*a++!=*b++)return 0;return 1;}
static DWORD u32(const BYTE *p){return (DWORD)p[0]|(DWORD)p[1]<<8|(DWORD)p[2]<<16|(DWORD)p[3]<<24;}
static void put32(BYTE *p,DWORD value){for(unsigned i=0;i<4;i++)p[i]=(BYTE)(value>>(i*8));}
static DWORD aligned(DWORD value){return (value+7)&~7U;}
static DWORD pointer(void *file){return SetFilePointer(file,0,0,1);}
static void fixture(unsigned bits,unsigned overlay)
{
    fill(original,0,sizeof(original));original[0]='M';original[1]='Z';put32(original+60,128);
    put32(original+128,0x4550);original[132]=bits==32?0x4c:0x64;original[133]=bits==32?1:0x86;
    original[134]=1;original[148]=bits==32?224:240;original[150]=2;
    DWORD opt=152;original[opt]=bits==32?0x0b:0x0b;original[opt+1]=bits==32?1:2;
    original[opt+2]=14; /* Match a current compiler's PE linker version. */
    put32(original+opt+16,4096);put32(original+opt+32,4096);put32(original+opt+36,512);
    put32(original+opt+56,8192);put32(original+opt+60,512);original[opt+68]=3;
    put32(original+opt+(bits==32?92:108),16);checksum=opt+64;security=opt+(bits==32?128:144);
    DWORD section=opt+(bits==32?224:240);original[section]='.';original[section+1]='t';
    put32(original+section+8,512);put32(original+section+12,4096);put32(original+section+16,512);put32(original+section+20,512);
    put32(original+section+36,0x60000020);for(DWORD i=512;i<1024;i++)original[i]=(BYTE)(i*17+3);
    original_size=1024+overlay;
    for(DWORD i=1024;i<original_size;i++)original[i]=(BYTE)(i+71);
    for(unsigned j=0;j<3;j++){
        fill(certificates[j],0,64);put32(certificates[j],lengths[j]);certificates[j][5]=2;certificates[j][6]=j==1?1:2;
        for(DWORD i=8;i<lengths[j];i++)certificates[j][i]=(BYTE)(i+j);
    }
}
static void inspect(void *file,const unsigned *list,unsigned count,int checksum_expected)
{
    DWORD before=pointer(file),size=GetFileSize(file,0),n=0;
    check(size<=sizeof(readback),"file size bounded");if(size>sizeof(readback))return;
    check(SetFilePointer(file,0,0,0)==0,"seek for verification");
    check(ReadFile(file,readback,size,&n,0)&&n==size,"read file bytes");
    check(SetFilePointer(file,(int)before,0,0)==before,"restore verification cursor");
    for(DWORD i=0;i<original_size;i++){
        if((i>=checksum&&i<checksum+4)||(i>=security&&i<security+8))continue;
        if(readback[i]!=original[i]){check(0,"non-certificate bytes preserved");break;}
    }
    DWORD certificate_size=0;for(unsigned i=0;i<count;i++)certificate_size+=aligned(lengths[list[i]]);
    check(size==aligned(original_size)+certificate_size,"exact file length");
    check(u32(readback+security)==(count?aligned(original_size):0),"certificate offset");
    check(u32(readback+security+4)==certificate_size,"certificate table size");
    DWORD at=aligned(original_size);
    for(unsigned i=0;i<count;i++){
        DWORD length=lengths[list[i]];
        check(equal(readback+at,certificates[list[i]],length),"opaque certificate bytes");
        for(DWORD j=length;j<aligned(length);j++)check(readback[at+j]==0,"record alignment padding");
        at+=aligned(length);
    }
    if(checksum_expected){
        DWORD sum=0;
        for(DWORD i=0;i<size;i+=2){
            DWORD word=readback[i]|(i+1<size?(DWORD)readback[i+1]<<8:0);
            if(i>=checksum&&i<checksum+4)word=0;
            sum+=word;sum=(sum&65535)+(sum>>16);
        }
        sum=(sum&65535)+(sum>>16);check(u32(readback+checksum)==sum+size,"PE checksum");
    }
}
static void enumeration(void *file,const unsigned *list,unsigned count)
{
    const WORD filters[]={255,0,1,2,65535};
    for(unsigned f=0;f<5;f++)for(unsigned capacity=0;capacity<=4;capacity++){
        DWORD before=pointer(file),got_count=0x12345678,indices[6];for(unsigned i=0;i<6;i++)indices[i]=0xa5a5a5a5;
        SetLastError(0x12345678);BOOL result=enumerate(file,filters[f],&got_count,indices,capacity);DWORD error=GetLastError();
        check(result==(f!=4)&&error==(f==4?87:0),"enumeration status");
        check(pointer(file)==before,"enumeration preserves cursor");
        DWORD matches=0;
        if(f!=4)for(unsigned i=0;i<count;i++)if(filters[f]==255||filters[f]==certificates[list[i]][6]){
            if(matches<capacity)check(indices[matches]==i,"filtered ordinal");matches++;
        }
        check(got_count==(f==4?0x12345678:matches),"enumeration count");
        for(DWORD i=(matches<capacity?matches:capacity);i<6;i++)check(indices[i]==0xa5a5a5a5,"indices bounds");
    }
    DWORD got_count=0;check(enumerate(file,255,&got_count,0,0)&&got_count==count,"optional indices");
    SetLastError(0x12345678);check(!enumerate(file,255,0,0,0)&&GetLastError()==87,"NULL count");
}
void imagehlp_entry(void)
{
    void *module=LoadLibraryA("imagehlp.dll");add=GetProcAddress(module,"ImageAddCertificate");
    enumerate=GetProcAddress(module,"ImageEnumerateCertificates");remove_cert=GetProcAddress(module,"ImageRemoveCertificate");
    check(add&&enumerate&&remove_cert,"exports");if(failures)ExitProcess(2);
    for(unsigned bits=32;bits<=64;bits+=32)for(unsigned overlay=0;overlay<=19;overlay+=19){
        fixture(bits,overlay);void *file=CreateFileA(name,0xc0000000,3,0,1,0x80,0);
        check(file!=(void *)(UP)-1,"create owned fixture");if(file==(void *)(UP)-1)ExitProcess(2);
        DWORD written=0;check(WriteFile(file,original,original_size,&written,0)&&written==original_size,"initial file");
        check(SetFilePointer(file,17,0,0)==17,"initial cursor");
        unsigned list[3]={0,1,2};enumeration(file,list,0);
        DWORD index=0x12345678;check(!add(file,0,&index)&&GetLastError()==87&&index==0xffffffff,"NULL certificate");
        check(!add(file,certificates[0],0)&&GetLastError()==87,"NULL index");
        for(unsigned i=0;i<3;i++){
            DWORD guard[3]={0xaabbccdd,0x12345678,0x99887766};SetLastError(0x12345678);
            BOOL ok=add(file,certificates[i],&guard[1]);DWORD error=GetLastError();
            check(ok&&error==0&&guard[1]==i,"append certificate");check(guard[0]==0xaabbccdd&&guard[2]==0x99887766,"index bounds");
            check(pointer(file)==GetFileSize(file,0),"append leaves EOF cursor");inspect(file,list,i+1,1);enumeration(file,list,i+1);
        }
        DWORD before=pointer(file);check(!remove_cert(file,99)&&GetLastError()==87,"invalid removal");check(pointer(file)==before,"failed removal cursor");
        check(remove_cert(file,1)&&GetLastError()==0,"remove middle");list[1]=2;inspect(file,list,2,1);enumeration(file,list,2);
        check(remove_cert(file,0)&&GetLastError()==0,"remove first");list[0]=2;inspect(file,list,1,1);
        check(remove_cert(file,0)&&GetLastError()==0,"remove last");inspect(file,list,0,1);enumeration(file,list,0);
        check(!remove_cert(file,0)&&GetLastError()==87,"empty removal");check(CloseHandle(file),"close fixture");
        file=CreateFileA(name,0x80000000,3,0,3,0x80,0);check(file!=(void *)(UP)-1,"read-only reopen");
        index=0x12345678;check(!add(file,certificates[0],&index)&&GetLastError()==87&&index==0x12345678,"read-only append");
        check(!remove_cert(file,0)&&GetLastError()==87,"read-only removal");enumeration(file,list,0);
        check(CloseHandle(file),"read-only close");check(DeleteFileA(name),"remove owned fixture");
    }
    say("IMAGEHLP DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");ExitProcess(failures?1:0);
}
