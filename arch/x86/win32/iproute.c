/* IP Helper routing for the kernel's single IPv4 link and local loopback. */
#include "iproute.h"
#include "compat32.h"

extern uint8_t *net_get_ip_ptr(void);
extern uint8_t *net_get_netmask_ptr(void);
extern uint8_t *net_get_gateway_ptr(void);

static DWORD iproute_address(const uint8_t *bytes)
{
    return (DWORD)bytes[0] | (DWORD)bytes[1] << 8 |
           (DWORD)bytes[2] << 16 | (DWORD)bytes[3] << 24;
}

DWORD WINAPI iphlp_GetBestInterface(DWORD destination, DWORD *index)
{
    if (!index)
        return 87; /* ERROR_INVALID_PARAMETER */
    if (!win32_user_range_writable(index, sizeof(*index), g_compat32_mode))
        return 998; /* ERROR_NOACCESS on native x86/x64 for a non-NULL buffer. */
    unsigned first = destination & 255;
    if ((!first && destination) || (first >= 240 && destination != 0xffffffffU))
        return 1231; /* ERROR_NETWORK_UNREACHABLE: reserved destination. */
    if (first >= 224 && first <= 239)
        return 1232; /* ERROR_HOST_UNREACHABLE: no unicast route */
    if (first == 127) {
        *index = 2; /* Same loopback index as GetIfTable/GetIpAddrTable. */
        return 0;
    }
    DWORD local = iproute_address(net_get_ip_ptr());
    DWORD mask = iproute_address(net_get_netmask_ptr());
    DWORD gateway = iproute_address(net_get_gateway_ptr());
    if (!local || ((destination & mask) != (local & mask) &&
                   !gateway && destination != 0xffffffffU))
        return 1232;
    /* net.c uses ARP directly on the subnet, the gateway off-subnet. The
     * configured link is interface 1 in the existing IP Helper tables. */
    *index = 1;
    return 0;
}
