/* Independent Windows SDK layouts; this same executable runs on Windows. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

void *memset(void *p,int c,size_t n){BYTE *b=p;while(n--)*b++=(BYTE)c;return p;}
void *memcpy(void *d,const void *s,size_t n){BYTE *a=d;const BYTE *b=s;while(n--)*a++=*b++;return d;}
int memcmp(const void *a,const void *b,size_t n){const BYTE *x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;x++;y++;}return 0;}
static DWORD checks,failures;
static DWORD (WINAPI *entry2)(MIB_IF_ROW2 *);
static DWORD (WINAPI *convert)(const GUID *,NET_LUID *);
static int guest;
static void say(const char *s){DWORD n=0,w;while(s[n])n++;WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);}
static void hex(DWORD v){char s[9];for(int i=7;i>=0;i--){s[i]="0123456789abcdef"[v&15];v>>=4;}s[8]=0;say(s);}
static void check(int ok,const char *label){checks++;if(!ok){failures++;say("IPINTERFACE FAIL ");say(label);say("\n");}}
static int same_guid(const GUID *a,const GUID *b){return !memcmp(a,b,sizeof(*a));}
static int terminated(const WCHAR *text){for(unsigned i=0;i<257;i++)if(!text[i])return 1;return 0;}
static void failure(ULONGLONG luid,DWORD index,DWORD expected)
{
    MIB_IF_ROW2 row,copy;memset(&row,0xa5,sizeof(row));row.InterfaceLuid.Value=luid;row.InterfaceIndex=index;copy=row;
    check(entry2(&row)==expected,"failed row status");check(!memcmp(&row,&copy,sizeof(row)),"failed row preserved");
}
static int hexval(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static int parse_guid(const char *s,GUID *g)
{
    static const BYTE order[]={3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    BYTE *bytes=(BYTE *)g;if(*s++!='{')return 0;
    for(unsigned i=0;i<16;i++){
        if((i==4||i==6||i==8||i==10)&&*s++!='-')return 0;
        int a=hexval(*s++),b=hexval(*s++);if(a<0||b<0)return 0;bytes[order[i]]=(BYTE)((a<<4)|b);
    }
    return *s++=='}'&&!*s;
}
static void identity(DWORD index,const MIB_IFROW *legacy)
{
    struct {ULONGLONG pre;MIB_IF_ROW2 row;ULONGLONG post;} block;
    memset(&block,0xa5,sizeof(block));block.row.InterfaceLuid.Value=0;block.row.InterfaceIndex=index;
    DWORD result=entry2(&block.row);check(result==0,"index query");if(result)return;
    check(block.pre==0xa5a5a5a5a5a5a5a5ULL&&block.post==block.pre,"row bounds");
    MIB_IF_ROW2 reference=block.row;
    check(reference.InterfaceIndex==index&&reference.InterfaceLuid.Value!=0,"index and LUID");
    check(reference.InterfaceLuid.Info.IfType==reference.Type&&!reference.InterfaceLuid.Info.Reserved,"LUID encoding");
    check(terminated(reference.Alias)&&terminated(reference.Description),"bounded names");
    check(reference.Type==legacy->dwType&&reference.Mtu==legacy->dwMtu,"legacy metadata");
    check(reference.PhysicalAddressLength==legacy->dwPhysAddrLen&&reference.PhysicalAddressLength<=8,"legacy MAC length");
    if(reference.PhysicalAddressLength<=8)check(!memcmp(reference.PhysicalAddress,legacy->bPhysAddr,reference.PhysicalAddressLength),"legacy MAC");
    DWORD invalid_indices[]={0,0xffffffff,index+1};
    for(unsigned i=0;i<3;i++){
        memset(&block.row,0xa5,sizeof(block.row));block.row.InterfaceLuid=reference.InterfaceLuid;block.row.InterfaceIndex=invalid_indices[i];
        result=entry2(&block.row);
        check(result==0&&block.row.InterfaceIndex==index&&same_guid(&block.row.InterfaceGuid,&reference.InterfaceGuid),"LUID overrides index");
    }
    for(unsigned bit=0;bit<24;bit++)failure(reference.InterfaceLuid.Value|(1ULL<<bit),index,2);
    NET_LUID luid;luid.Value=0;
    check(convert(&reference.InterfaceGuid,&luid)==0&&luid.Value==reference.InterfaceLuid.Value,"GUID to LUID");
    GUID overlap=reference.InterfaceGuid;
    check(convert(&overlap,(NET_LUID *)&overlap)==0,"overlapping conversion");
    memcpy(&luid,&overlap,sizeof(luid));check(luid.Value==reference.InterfaceLuid.Value,"overlapping LUID value");
    if(guest&&reference.Type!=24)check(reference.InOctets+reference.OutOctets>0,"physical traffic counted");
}
static union {ULONGLONG align;BYTE bytes[65536];} legacy_buffer,adapters_buffer;
void ipinterface_entry(void)
{
    const char *command=GetCommandLineA();
    for(unsigned i=0;command[i];i++){
        unsigned j=0;while(j<12&&command[i+j]&&command[i+j]=="--osito-guest"[j])j++;
        if(j==12){guest=1;break;}
    }
    HMODULE module=LoadLibraryA("iphlpapi.dll");
    entry2=(void *)GetProcAddress(module,"GetIfEntry2");convert=(void *)GetProcAddress(module,"ConvertInterfaceGuidToLuid");
    DWORD (WINAPI *table)(MIB_IFTABLE *,DWORD *,BOOL)=(void *)GetProcAddress(module,"GetIfTable");
    ULONG (WINAPI *adapters)(ULONG,ULONG,void *,IP_ADAPTER_ADDRESSES *,ULONG *)=(void *)GetProcAddress(module,"GetAdaptersAddresses");
    check(entry2&&convert&&table&&adapters,"exports");if(failures)ExitProcess(2);
    check(entry2(NULL)==87,"NULL row");failure(0,0,87);failure(0,0xffffffff,2);failure(1,1,2);failure(~0ULL,1,2);
    GUID unknown={0};NET_LUID luid;luid.Value=0x1122334455667788ULL;
    check(convert(NULL,&luid)==87&&luid.Value==0x1122334455667788ULL,"NULL GUID");
    check(convert(&unknown,NULL)==87,"NULL LUID");
    check(convert(&unknown,&luid)==87&&!luid.Value,"unknown GUID clears output");
    DWORD size=sizeof(legacy_buffer);MIB_IFTABLE *list=(void *)legacy_buffer.bytes;
    check(table(list,&size,FALSE)==0,"legacy table");if(failures)ExitProcess(2);
    check(list->dwNumEntries>0&&list->dwNumEntries<70,"interface count");if(failures)ExitProcess(2);
    for(DWORD i=0;i<list->dwNumEntries;i++)identity(list->table[i].dwIndex,&list->table[i]);
    DWORD flags[]={0,0x27,0x38};
    for(unsigned f=0;f<3;f++){
        size=0;check(adapters(2,flags[f],NULL,NULL,&size)==111&&size<=sizeof(adapters_buffer)-16,"adapter size");
        if(size>sizeof(adapters_buffer)-16)ExitProcess(2);
        memset(&adapters_buffer,0xa5,sizeof(adapters_buffer));size=sizeof(adapters_buffer)-16;
        check(adapters(2,flags[f],NULL,(void *)adapters_buffer.bytes,&size)==0,"adapter query");
        for(unsigned i=size;i<sizeof(adapters_buffer);i++)if(adapters_buffer.bytes[i]!=0xa5){check(0,"adapter guard");break;}
        unsigned count=0;
        for(IP_ADAPTER_ADDRESSES *p=(void *)adapters_buffer.bytes;p;p=p->Next){
            if(++count>70){check(0,"adapter list cycle");break;}
            MIB_IF_ROW2 row;memset(&row,0,sizeof(row));row.InterfaceIndex=p->IfIndex;
            DWORD result=entry2(&row);check(!result,"adapter row query");if(result)continue;
            check(p->Luid.Value==row.InterfaceLuid.Value,"adapter LUID");
            /* Native IP MTU can differ from link MTU (loopback/tunnel).
             * Osito currently has one MTU per supported interface. */
            check(p->IfType==row.Type&&p->Mtu&&(!guest||p->Mtu==row.Mtu),"adapter metadata");
            check(p->PhysicalAddressLength==row.PhysicalAddressLength&&!memcmp(p->PhysicalAddress,row.PhysicalAddress,row.PhysicalAddressLength),"adapter MAC");
            GUID guid;int parsed=p->AdapterName&&parse_guid(p->AdapterName,&guid);check(parsed,"adapter GUID string");
            if(parsed)check(same_guid(&guid,&row.InterfaceGuid),"adapter GUID identity");
            check(p->ConnectionType==row.ConnectionType,"adapter connection type");
            check(p->TransmitLinkSpeed==row.TransmitLinkSpeed&&p->ReceiveLinkSpeed==row.ReceiveLinkSpeed,"adapter speeds");
            check(p->Description&&!memcmp(p->Description,row.Description,2),"adapter description");
        }
    }
    if(guest){
        BYTE *pages=VirtualAlloc(NULL,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);check(pages!=NULL,"guard allocation");
        if(pages){
            DWORD previous;MIB_IF_ROW2 *row=(void *)(pages+4096-sizeof(*row)+8);memset(row,0xa5,sizeof(*row));row->InterfaceLuid.Value=0;row->InterfaceIndex=1;
            MIB_IF_ROW2 copy=*row;check(VirtualProtect(pages+4096,4096,PAGE_READONLY,&previous)!=0,"guard protection");
            check(entry2(row)==998&&!memcmp(row,&copy,sizeof(copy)),"cross-page output atomic failure");
            check(convert(&unknown,(void *)(pages+4092))==998,"cross-page LUID");
            check(VirtualFree(pages,0,MEM_RELEASE)!=0,"guard release");
        }
    }
    say("IPINTERFACE DONE checks=");hex(checks);say(" failures=");hex(failures);say("\n");ExitProcess(failures?1:0);
}
