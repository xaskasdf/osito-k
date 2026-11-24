#ifndef OSITOK_WIN32_IPINTERFACE_H
#define OSITOK_WIN32_IPINTERFACE_H
#include "nttypes.h"

/* MIB_IF_ROW2 has no pointers: the layout is identical for PE32 and PE64. */
typedef struct {
    ULONGLONG luid;
    DWORD index;
    GUID guid;
    WCHAR alias[257], description[257];
    DWORD address_length;
    BYTE address[32], permanent_address[32];
    DWORD mtu, type, tunnel_type, media_type, physical_medium_type;
    DWORD access_type, direction_type;
    BYTE status_flags;
    DWORD oper_status, admin_status, media_connect_state;
    GUID network_guid;
    DWORD connection_type;
    ULONGLONG tx_speed, rx_speed;
    ULONGLONG in_octets, in_ucast_packets, in_nucast_packets;
    ULONGLONG in_discards, in_errors, in_unknown_protocols;
    ULONGLONG in_ucast_octets, in_multicast_octets, in_broadcast_octets;
    ULONGLONG out_octets, out_ucast_packets, out_nucast_packets;
    ULONGLONG out_discards, out_errors;
    ULONGLONG out_ucast_octets, out_multicast_octets, out_broadcast_octets;
    ULONGLONG out_queue_length;
} iphlp_if_row2_t;

_Static_assert(sizeof(iphlp_if_row2_t) == 1352, "MIB_IF_ROW2 size");
_Static_assert(__builtin_offsetof(iphlp_if_row2_t, guid) == 12, "InterfaceGuid");
_Static_assert(__builtin_offsetof(iphlp_if_row2_t, address_length) == 1056, "PhysicalAddressLength");
_Static_assert(__builtin_offsetof(iphlp_if_row2_t, status_flags) == 1152, "InterfaceAndOperStatusFlags");
_Static_assert(__builtin_offsetof(iphlp_if_row2_t, tx_speed) == 1192, "TransmitLinkSpeed");
_Static_assert(__builtin_offsetof(iphlp_if_row2_t, out_queue_length) == 1344, "OutQLen");

/* Kernel-owned buffers; used by the other IP Helper views of these links. */
void iphlp_interface_row(DWORD index, iphlp_if_row2_t *row);
void iphlp_interface_guid_name(const GUID *guid, char name[39]);
const char *iphlp_interface_description(DWORD index);
DWORD WINAPI iphlp_GetIfEntry2(PVOID row);
DWORD WINAPI iphlp_ConvertInterfaceGuidToLuid(const GUID *guid, ULONGLONG *luid);
#endif
