#include "variant_host_types.h"
#include "../win32/iproute.h"
#include "../win32/compat32.h"
static uint8_t ip[4], mask[4], gateway[4];
static int mode, deny;
static DWORD *allowed;
static unsigned checks,failures;
int *proc_win32_compat32_mode_slot(void) { return &mode; }
uint8_t *net_get_ip_ptr(void) { return ip; }
uint8_t *net_get_netmask_ptr(void) { return mask; }
uint8_t *net_get_gateway_ptr(void) { return gateway; }
int win32_user_range_writable(void *p,SIZE_T bytes,BOOL compat32)
{ return !deny&&p==allowed&&bytes==4&&compat32==mode; }
static void expect(int yes)
{ checks++;if(!yes)failures++; }
static void query(DWORD address,DWORD status,DWORD index)
{
    DWORD guarded[3]={0x11223344,0x55667788,0x99aabbcc};allowed=&guarded[1];
    DWORD result=iphlp_GetBestInterface(address,&guarded[1]);
    expect(result==status);expect(guarded[1]==(status?0x55667788:index));
    expect(guarded[0]==0x11223344&&guarded[2]==0x99aabbcc);
}
int main(void)
{
    for(mode=0;mode<=1;mode++){
        memset(ip,0,4);memset(gateway,0,4);memset(mask,0,4);
        expect(iphlp_GetBestInterface(0x0100007f,0)==87);
        query(0x0100007f,0,2);query(0xffffffff,1232,0);query(0x01010101,1232,0);
        for(unsigned i=0;i<256;i++)query(0x0100007f|(i<<8),0,2);
        ip[0]=10;ip[1]=20;ip[2]=30;ip[3]=40;
        mask[0]=255;mask[1]=255;mask[2]=255;
        query(0x281e140a,0,1);query(0x291e140a,0,1);
        query(0x281f140a,1232,0);query(0x01010101,1232,0);query(0xffffffff,0,1);
        mask[2]=0;query(0x281f140a,0,1);query(0x281e150a,1232,0);
        gateway[0]=10;gateway[1]=20;gateway[2]=30;gateway[3]=1;
        query(0x01010101,0,1);query(0,0,1);query(0x7f000001,0,1);
        for(unsigned i=224;i<=239;i++)query(0x03020100|i,1232,0);
        query(0x010000f0,1231,0);query(0x010000ff,1231,0);query(0x03020100,1231,0);
        deny=1;query(0x0100007f,998,0);query(0x01010101,998,0);deny=0;
    }
    printf("IPROUTE HOST checks=%u failures=%u\n",checks,failures);
    return failures!=0;
}
