/* One active kernel NIC (index 1) and the TCP loopback interface (index 2). */
#include "ipinterface.h"
#include "compat32.h"
#include "../kernel/nic.h"
#include "../kernel/crypto.h"

extern void net_get_mac(uint8_t mac[6]);

static DWORD interface_type(DWORD index)
{
    return index == 2 ? 24 : (nic_is_iwx() ? 71 : 6);
}

static ULONGLONG interface_luid(DWORD index)
{
    return (ULONGLONG)interface_type(index) << 48 | (ULONGLONG)index << 24;
}

static void interface_guid(DWORD index, GUID *guid)
{
    /* UUIDv5 under the DNS namespace. The active NIC's MAC and interface
     * kind make the single-NIC identity stable across boots/DHCP changes.
     * This is an interface ID, not a fabricated Windows network/profile ID. */
    static const BYTE dns_namespace[16] = {
        0x6b,0xa7,0xb8,0x10,0x9d,0xad,0x11,0xd1,
        0x80,0xb4,0x00,0xc0,0x4f,0xd4,0x30,0xc8
    };
    const char *kind = index == 2 ? "loopback/" :
                       nic_is_iwx() ? "wlan/" : "ethernet/";
    static const char prefix[] = "osito-k/interface/";
    BYTE mac[6], hash[20];
    char hex[12];
    /* The WLAN callback can return zeros while its probe lock is busy.
     * net.c retains the last accepted MAC across those transient snapshots. */
    net_get_mac(mac);
    for (unsigned i = 0; i < 6; i++) {
        hex[i * 2] = "0123456789abcdef"[mac[i] >> 4];
        hex[i * 2 + 1] = "0123456789abcdef"[mac[i] & 15];
    }
    sha1_ctx context;
    sha1_init(&context);
    sha1_update(&context, dns_namespace, sizeof(dns_namespace));
    sha1_update(&context, prefix, sizeof(prefix) - 1);
    sha1_update(&context, kind, (uint32_t)strlen(kind));
    sha1_update(&context, hex, sizeof(hex));
    sha1_final(&context, hash);
    hash[6] = (hash[6] & 15) | 0x50;
    hash[8] = (hash[8] & 63) | 0x80;
    guid->Data1 = (DWORD)hash[0] << 24 | (DWORD)hash[1] << 16 |
                  (DWORD)hash[2] << 8 | hash[3];
    guid->Data2 = (WORD)hash[4] << 8 | hash[5];
    guid->Data3 = (WORD)hash[6] << 8 | hash[7];
    memcpy(guid->Data4, hash + 8, 8);
}

void iphlp_interface_guid_name(const GUID *guid, char name[39])
{
    static const BYTE order[16] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    const BYTE *bytes = (const BYTE *)guid;
    unsigned at = 0;
    name[at++] = '{';
    for (unsigned i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) name[at++] = '-';
        BYTE value = bytes[order[i]];
        name[at++] = "0123456789ABCDEF"[value >> 4];
        name[at++] = "0123456789ABCDEF"[value & 15];
    }
    name[at++] = '}';
    name[at] = 0;
}

const char *iphlp_interface_description(DWORD index)
{
    return index == 2 ? "OsitoK Loopback" :
           nic_ops.name ? nic_ops.name : "OsitoK Network Interface";
}

static void interface_wide(WCHAR out[257], const char *text)
{
    unsigned i = 0;
    while (text[i] && i < 256) { out[i] = (BYTE)text[i]; i++; }
    out[i] = 0;
}

void iphlp_interface_row(DWORD index, iphlp_if_row2_t *row)
{
    memset(row, 0, sizeof(*row));
    row->index = index;
    row->type = interface_type(index);
    row->luid = interface_luid(index);
    interface_guid(index, &row->guid);
    interface_wide(row->alias, index == 2 ? "lo" : "osito0");
    interface_wide(row->description, iphlp_interface_description(index));
    row->connection_type = 1; /* NET_IF_CONNECTION_DEDICATED */
    row->admin_status = 1;
    if (index == 2) {
        row->mtu = 65536;
        row->media_type = 17; /* NdisMediumLoopback */
        row->access_type = 1; /* NET_IF_ACCESS_LOOPBACK */
        row->oper_status = 1;
        row->media_connect_state = 1;
    } else {
        row->mtu = 1500;
        row->media_type = row->type == 71 ? 16 : 0;
        row->physical_medium_type = row->type == 71 ? 9 : 14;
        row->access_type = 2; /* NET_IF_ACCESS_BROADCAST */
        row->status_flags = 5; /* HardwareInterface | ConnectorPresent */
        row->address_length = 6;
        net_get_mac(row->address);
        memcpy(row->permanent_address, row->address, 6);
        if (!nic_ops.send || !nic_ops.recv) {
            row->oper_status = 6; /* not present */
            row->admin_status = 2;
            row->status_flags = 0x10;
            row->address_length = 0;
        } else {
            nic_link_state_t state;
            int known = 1;
            bool up;
            if (nic_ops.read_link) {
                known = nic_ops.read_link(&state) == 0;
                up = known && state.up;
            } else {
                known = nic_ops.link_up != NULL;
                up = known && nic_link_up();
            }
            row->oper_status = !known ? 4 : up ? 1 : 2;
            row->media_connect_state = !known ? 0 : up ? 1 : 2;
            if (known && !up) row->status_flags |= 0x10;
        }
    }
    /* Drivers do not expose negotiated speed, hardware errors, queue depth
     * or a network-profile GUID yet. Leave these unavailable fields zero.
     * Successful frame counters include Ethernet headers; loopback counts IP.
     * Each counter is atomic; a snapshot can overlap ongoing traffic. */
    const nic_traffic_t *traffic = &nic_traffic[index == 2];
    ULONGLONG rx_packets[3], tx_packets[3], rx_bytes[3], tx_bytes[3];
    for (unsigned i = 0; i < 3; i++) {
        rx_packets[i] = __atomic_load_n(&traffic->rx.packets[i], __ATOMIC_RELAXED);
        tx_packets[i] = __atomic_load_n(&traffic->tx.packets[i], __ATOMIC_RELAXED);
        rx_bytes[i] = __atomic_load_n(&traffic->rx.bytes[i], __ATOMIC_RELAXED);
        tx_bytes[i] = __atomic_load_n(&traffic->tx.bytes[i], __ATOMIC_RELAXED);
    }
    row->in_ucast_packets = rx_packets[0];
    row->in_nucast_packets = rx_packets[1] + rx_packets[2];
    row->out_ucast_packets = tx_packets[0];
    row->out_nucast_packets = tx_packets[1] + tx_packets[2];
    row->in_ucast_octets = rx_bytes[0];
    row->in_multicast_octets = rx_bytes[1];
    row->in_broadcast_octets = rx_bytes[2];
    row->out_ucast_octets = tx_bytes[0];
    row->out_multicast_octets = tx_bytes[1];
    row->out_broadcast_octets = tx_bytes[2];
    row->in_octets = rx_bytes[0] + rx_bytes[1] + rx_bytes[2];
    row->out_octets = tx_bytes[0] + tx_bytes[1] + tx_bytes[2];
}

DWORD WINAPI iphlp_GetIfEntry2(PVOID buffer)
{
    if (!buffer) return 87;
    if (!win32_user_range_readable(buffer, sizeof(iphlp_if_row2_t), g_compat32_mode) ||
        !win32_user_range_writable(buffer, sizeof(iphlp_if_row2_t), g_compat32_mode))
        return 998;
    ULONGLONG luid;
    DWORD index;
    memcpy(&luid, buffer, sizeof(luid));
    memcpy(&index, (BYTE *)buffer + 8, sizeof(index));
    if (luid) {
        index = luid == interface_luid(1) ? 1 : luid == interface_luid(2) ? 2 : 0;
        if (!index) return 2; /* LUID takes precedence, including reserved bits. */
    } else if (!index) {
        return 87;
    } else if (index != 1 && index != 2) {
        return 2;
    }
    iphlp_if_row2_t row;
    iphlp_interface_row(index, &row);
    memcpy(buffer, &row, sizeof(row));
    return 0;
}

DWORD WINAPI iphlp_ConvertInterfaceGuidToLuid(const GUID *guid, ULONGLONG *luid)
{
    if (!guid || !luid) return 87;
    if (!win32_user_range_readable(guid, sizeof(*guid), g_compat32_mode) ||
        !win32_user_range_writable(luid, sizeof(*luid), g_compat32_mode))
        return 998;
    GUID input;
    memcpy(&input, guid, sizeof(input)); /* allow overlapping input/output */
    ULONGLONG result = 0;
    for (DWORD index = 1; index <= 2; index++) {
        GUID candidate;
        interface_guid(index, &candidate);
        if (!memcmp(&input, &candidate, sizeof(input))) {
            result = interface_luid(index);
            break;
        }
    }
    memcpy(luid, &result, sizeof(result));
    return result ? 0 : 87;
}
