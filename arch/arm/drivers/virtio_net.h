/*
 * virtio_net.h -- VirtIO network device driver (MMIO transport)
 */

#ifndef OSITO_VIRTIO_NET_H
#define OSITO_VIRTIO_NET_H

#include <stdint.h>

int  virtio_net_init(void);
void virtio_net_get_mac(uint8_t mac[6]);
int  virtio_net_send(const void *data, uint32_t len);
int  virtio_net_recv(void *buf, uint32_t *len);

#endif /* OSITO_VIRTIO_NET_H */
