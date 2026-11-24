#include "variant_host_types.h"
#include <pthread.h>
#include "../win32/ipinterface.h"
#include "../win32/compat32.h"
#include "../kernel/nic.h"
nic_ops_t nic_ops;
nic_traffic_t nic_traffic[2];
static int mode, deny_read, deny_write, wlan, carrier=1, busy, send_result, recv_result;
static unsigned checks, failures;
static BYTE mac[6]={2,3,4,5,6,7}, frame[64];
int *proc_win32_compat32_mode_slot(void){return &mode;}
bool nic_is_iwx(void){return wlan;}
int win32_user_range_readable(const void *p,SIZE_T n,BOOL compat)
{return !deny_read&&(uintptr_t)p>4096&&n&&compat==mode;}
int win32_user_range_writable(void *p,SIZE_T n,BOOL compat)
{return !deny_write&&(uintptr_t)p>4096&&n&&compat==mode;}
static int send_packet(const void *p,uint32_t n){(void)p;(void)n;return send_result;}
static int recv_packet(void *p,uint32_t *n){memcpy(p,frame,64);*n=64;return recv_result;}
void net_get_mac(uint8_t *out){memcpy(out,mac,6);}
static void get_mac(uint8_t *out){if(busy)memset(out,0,6);else memcpy(out,mac,6);}
static bool link_up(void){return carrier;}
static int read_link(nic_link_state_t *s){s->up=carrier;return busy?-1:0;}
static void check(int yes,const char *label)
{checks++;if(!yes){failures++;printf("IPINTERFACE FAIL %s\n",label);}}
static void failed_row(ULONGLONG luid,DWORD index,DWORD expected)
{
    iphlp_if_row2_t row,saved;memset(&row,0xa5,sizeof(row));row.luid=luid;row.index=index;saved=row;
    check(iphlp_GetIfEntry2(&row)==expected,"row status");
    check(!memcmp(&row,&saved,sizeof(row)),"failed row unchanged");
}
static void *traffic_worker(void *argument)
{
    unsigned kind=(unsigned)(uintptr_t)argument;
    for(unsigned i=0;i<20000;i++)nic_account_packet(&nic_traffic[0].tx,100,kind);
    return NULL;
}
int main(void)
{
    nic_ops=(nic_ops_t){.send=send_packet,.recv=recv_packet,.get_mac=get_mac,
        .link_up=link_up,.name="Test Ethernet"};
    for(mode=0;mode<=1;mode++){
        wlan=0;carrier=1;deny_read=deny_write=0;
        check(iphlp_GetIfEntry2(NULL)==87,"NULL row");
        check(iphlp_GetIfEntry2((void *)1)==998,"inaccessible row");
        failed_row(0,0,87);failed_row(0,3,2);failed_row(0,0xffffffff,2);failed_row(1,1,2);
        for(DWORD index=1;index<=2;index++){
            struct {ULONGLONG pre;iphlp_if_row2_t row;ULONGLONG post;} block;
            memset(&block,0xa5,sizeof(block));block.row.luid=0;block.row.index=index;
            check(iphlp_GetIfEntry2(&block.row)==0,"index lookup");
            check(block.pre==0xa5a5a5a5a5a5a5a5ULL&&block.post==block.pre,"row guards");
            iphlp_if_row2_t reference=block.row;
            check(reference.luid==((ULONGLONG)(index==1?6:24)<<48|((ULONGLONG)index<<24)),"LUID fields");
            char text[39];iphlp_interface_guid_name(&reference.guid,text);
            check(!strcmp(text,index==1?"{ADAA93C9-56AD-509B-89CF-DB21AE256192}":"{DE250F8C-DCF8-5C04-9A7E-55520D02A15E}"),"independent UUIDv5 oracle");
            check(reference.oper_status==1&&reference.admin_status==1&&reference.media_connect_state==1,"link up");
            check(reference.address_length==(index==1?6:0),"MAC length");
            for(unsigned trial=0;trial<32;trial++){
                memset(&block.row,0xa5,sizeof(block.row));block.row.luid=reference.luid;block.row.index=trial;
                check(iphlp_GetIfEntry2(&block.row)==0&&!memcmp(&block.row,&reference,sizeof(reference)),"LUID precedence");
                failed_row(reference.luid^(1ULL<<trial),index,2);
            }
            ULONGLONG luid=0;
            check(iphlp_ConvertInterfaceGuidToLuid(&reference.guid,&luid)==0&&luid==reference.luid,"GUID roundtrip");
            GUID overlapping=reference.guid;
            check(iphlp_ConvertInterfaceGuidToLuid(&overlapping,(ULONGLONG *)&overlapping)==0,"GUID/output alias");
            memcpy(&luid,&overlapping,8);check(luid==reference.luid,"aliased output value");
        }
        GUID unknown={0};ULONGLONG luid=0x123456789abcdef0ULL;
        check(iphlp_ConvertInterfaceGuidToLuid(NULL,&luid)==87&&luid==0x123456789abcdef0ULL,"NULL GUID preserves output");
        check(iphlp_ConvertInterfaceGuidToLuid(&unknown,NULL)==87,"NULL LUID");
        check(iphlp_ConvertInterfaceGuidToLuid(&unknown,&luid)==87&&!luid,"unknown GUID clears output");
        deny_read=1;failed_row(0,1,998);luid=9;
        check(iphlp_ConvertInterfaceGuidToLuid(&unknown,&luid)==998&&luid==9,"unreadable GUID");deny_read=0;
        deny_write=1;failed_row(0,1,998);
        check(iphlp_ConvertInterfaceGuidToLuid(&unknown,&luid)==998&&luid==9,"unwritable LUID");deny_write=0;
        iphlp_if_row2_t row;
        carrier=0;iphlp_interface_row(1,&row);
        check(row.oper_status==2&&row.media_connect_state==2&&(row.status_flags&16),"link down");
        GUID previous_guid=row.guid;
        nic_ops.read_link=read_link;busy=1;iphlp_interface_row(1,&row);
        check(row.oper_status==4&&!row.media_connect_state&&!(row.status_flags&16),"busy link is unknown");
        check(!memcmp(&previous_guid,&row.guid,sizeof(previous_guid))&&!memcmp(row.address,mac,6),"busy driver preserves identity");
        busy=0;carrier=1;wlan=1;iphlp_interface_row(1,&row);
        char text[39];iphlp_interface_guid_name(&row.guid,text);
        check(row.type==71&&row.media_type==16&&row.physical_medium_type==9,"WLAN metadata");
        check(!strcmp(text,"{A5C9B642-7B75-5E05-8FCE-EB885239B6BB}"),"WLAN identity");
        nic_ops.send=NULL;iphlp_interface_row(1,&row);
        check(row.oper_status==6&&row.admin_status==2&&!row.address_length,"absent NIC");
        nic_ops.send=send_packet;nic_ops.read_link=NULL;
    }
    mode=0;wlan=0;
    memset(nic_traffic,0,sizeof(nic_traffic));memset(frame,0,sizeof(frame));
    check(nic_send(frame,64)==0,"send return");check(nic_recv(frame,&(uint32_t){64})==0,"recv return");
    frame[0]=1;nic_send(frame,64);memset(frame,255,6);nic_send(frame,64);
    nic_send(frame,13);send_result=-1;nic_send(frame,64);recv_result=-1;nic_recv(frame,&(uint32_t){64});
    iphlp_if_row2_t row;iphlp_interface_row(1,&row);
    check(row.out_octets==192&&row.out_ucast_packets==1&&row.out_nucast_packets==2,"successful frames only");
    check(row.in_octets==64&&row.in_ucast_packets==1&&!row.in_errors,"empty RX not an error");
    check(row.out_ucast_octets==64&&row.out_multicast_octets==64&&row.out_broadcast_octets==64,"frame classes");
    memset(nic_traffic,0,sizeof(nic_traffic));
    pthread_t threads[4];for(unsigned i=0;i<4;i++)check(!pthread_create(&threads[i],NULL,traffic_worker,(void *)(uintptr_t)(i%3)),"thread create");
    for(unsigned i=0;i<4;i++)check(!pthread_join(threads[i],NULL),"thread join");
    iphlp_interface_row(1,&row);
    check(row.out_octets==8000000&&row.out_ucast_packets==40000&&row.out_nucast_packets==40000,"concurrent counters");
    nic_account_packet(&nic_traffic[0].rx,0xffffffffU,0);nic_account_packet(&nic_traffic[0].rx,2,0);
    iphlp_interface_row(1,&row);check(row.in_octets==0x100000001ULL,"64-bit byte count");
    nic_account_packet(&nic_traffic[1].rx,40,0);nic_account_packet(&nic_traffic[1].tx,40,0);
    iphlp_interface_row(2,&row);check(row.in_octets==40&&row.out_octets==40,"separate loopback counters");
    printf("IPINTERFACE HOST checks=%u failures=%u\n",checks,failures);return failures!=0;
}
