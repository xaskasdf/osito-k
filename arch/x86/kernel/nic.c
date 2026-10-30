/*
 * OsitoK x86-64 — NIC dispatch implementation.
 */

#include "nic.h"
#include "../drivers/i211.h"
#include "../drivers/rtl8111.h"
#include "../drivers/iwx.h"

extern void serial_puts(const char *s);

extern volatile bool i211_irq_pending;
extern volatile bool rtl8111_irq_pending;

extern int  virtio_net_send(const void *data, uint32_t len);
extern int  virtio_net_recv(void *buf, uint32_t *len_out);
extern void virtio_net_get_mac(uint8_t mac_out[6]);
extern bool virtio_net_is_ready(void);

nic_ops_t nic_ops;

void nic_bind_i211(void)
{
    nic_ops.send        = i211_send;
    nic_ops.send_sg     = i211_send_sg;
    nic_ops.recv        = i211_recv;
    nic_ops.get_mac     = i211_get_mac;
    nic_ops.link_up     = i211_link_up;
    nic_ops.irq_pending = &i211_irq_pending;
    nic_ops.name        = "Intel I211";
    nic_ops.read_link   = NULL;
    serial_puts("[NIC] backend: Intel I211\n");
}

void nic_bind_rtl8111(void)
{
    nic_ops.send        = rtl8111_send;
    nic_ops.send_sg     = rtl8111_send_sg;
    nic_ops.recv        = rtl8111_recv;
    nic_ops.get_mac     = rtl8111_get_mac;
    nic_ops.link_up     = rtl8111_link_up;
    nic_ops.irq_pending = &rtl8111_irq_pending;
    nic_ops.name        = "Realtek RTL8111";
    nic_ops.read_link   = NULL;
    serial_puts("[NIC] backend: Realtek RTL8111\n");
}

/* virtio_net_recv returns 1=packet/0=empty/-1=err; net.c expects
 * 0=packet/-1=empty-or-err. Adapter normalizes the convention. */
static int vnet_recv_adapter(void *buf, uint32_t *len)
{
    int r = virtio_net_recv(buf, len);
    return (r == 1) ? 0 : -1;
}

static bool vnet_link_up(void) { return virtio_net_is_ready(); }

void nic_bind_virtio_net(void)
{
    nic_ops.send        = virtio_net_send;
    nic_ops.send_sg     = NULL;
    nic_ops.recv        = vnet_recv_adapter;
    nic_ops.get_mac     = virtio_net_get_mac;
    nic_ops.link_up     = vnet_link_up;
    nic_ops.irq_pending = NULL;          /* polled, no IRQ */
    nic_ops.name        = "virtio-net";
    nic_ops.read_link   = NULL;
    serial_puts("[NIC] backend: virtio-net\n");
}

void nic_bind_iwx(void)
{
    nic_ops = (nic_ops_t){.send = iwx_send, .recv = iwx_recv,
        .get_mac = iwx_get_mac, .link_up = iwx_link_up,
        .read_link = iwx_read_link, .name = "Intel AX200"};
    serial_puts("[NIC] backend: Intel AX200 (link down until connected)\n");
}

bool nic_is_iwx(void)
{
    return nic_ops.read_link == iwx_read_link;
}

bool nic_boot_prefer_wlan(bool wlan_present)
{
    return wlan_present && !nic_link_up();
}
