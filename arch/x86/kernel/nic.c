/*
 * OsitoK x86-64 — NIC dispatch implementation.
 */

#include "nic.h"
#include "../drivers/i211.h"
#include "../drivers/rtl8111.h"

extern void serial_puts(const char *s);

extern volatile bool i211_irq_pending;
extern volatile bool rtl8111_irq_pending;

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
    serial_puts("[NIC] backend: Realtek RTL8111\n");
}
