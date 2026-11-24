/*
 * OsitoK x86-64 — NIC dispatch (Intel I211 / Realtek RTL8111).
 *
 * Indirección minimal: el net.c llama nic_send/nic_recv/nic_get_mac
 * sin saber qué chip está debajo.  El boot (main.c) detecta el NIC
 * en pci_get_nic(), llama el _init del driver apropiado, y nic_bind_*
 * apunta los function-pointers a esa familia.
 */

#ifndef OSITOK_NIC_H
#define OSITOK_NIC_H

#include "../include/types.h"

typedef struct {
    uint64_t generation;
    uint8_t mac[6];
    bool up;
} nic_link_state_t;

typedef struct {
    int  (*send)   (const void *data, uint32_t len);
    int  (*send_sg)(const uint64_t frag_phys[], const uint32_t lens[], int n_frags);
    int  (*recv)   (void *buf, uint32_t *len);
    void (*get_mac)(uint8_t mac[6]);
    bool (*link_up)(void);
    /* Pointer al flag volátil de IRQ pending del driver activo.            */
    volatile bool *irq_pending;
    const char *name;
    /* Optional coherent snapshot. -1 means busy, not link down. */
    int (*read_link)(nic_link_state_t *state);
} nic_ops_t;

/* Inicializado por main.c según pci_get_nic().  net.c lo lee.              */
extern nic_ops_t nic_ops;

/* Successfully submitted/received frames, since boot. Bucket order is
 * unicast, multicast, broadcast. Driver errors/empty RX are not packets. */
typedef struct {
    uint64_t packets[3], bytes[3];
} nic_traffic_direction_t;
typedef struct {
    nic_traffic_direction_t rx, tx;
} nic_traffic_t;
extern nic_traffic_t nic_traffic[2]; /* active NIC, software loopback */

static inline void nic_account_packet(nic_traffic_direction_t *direction,
                                      uint32_t bytes, unsigned kind)
{
    __atomic_fetch_add(&direction->bytes[kind], bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&direction->packets[kind], 1, __ATOMIC_RELAXED);
}

static inline void nic_account_frame(nic_traffic_direction_t *direction,
                                     const void *frame, uint32_t bytes)
{
    if (!frame || bytes < 14) return;
    const uint8_t *address = frame;
    unsigned kind = 0;
    if (address[0] & 1) {
        kind = 1;
        if ((address[0] & address[1] & address[2] & address[3] &
             address[4] & address[5]) == 255)
            kind = 2;
    }
    nic_account_packet(direction, bytes, kind);
}

void nic_bind_i211(void);
void nic_bind_rtl8111(void);
void nic_bind_iwx(void);
bool nic_is_iwx(void);
/* Single-interface boot policy, before net_init or starting IRQ/RX services. */
bool nic_boot_prefer_wlan(bool wlan_present);

/* Conveniencias para no llenar net.c de "nic_ops.send(...)".               */
static inline int nic_send(const void *d, uint32_t l)
{
    int result = nic_ops.send ? nic_ops.send(d, l) : -1;
    if (result >= 0) nic_account_frame(&nic_traffic[0].tx, d, l);
    return result;
}
static inline int nic_recv(void *b, uint32_t *l)
{
    int result = nic_ops.recv ? nic_ops.recv(b, l) : -1;
    if (result == 0 && l) nic_account_frame(&nic_traffic[0].rx, b, *l);
    return result;
}
static inline void nic_get_mac(uint8_t m[6]) {
    if (nic_ops.get_mac) nic_ops.get_mac(m);
    else for (unsigned i = 0; i < 6; i++) m[i] = 0;
}
static inline bool nic_link_up(void)                          { return nic_ops.link_up && nic_ops.link_up(); }
static inline int  nic_send_sg(const uint64_t fp[], const uint32_t ln[], int n) {
    return nic_ops.send_sg ? nic_ops.send_sg(fp, ln, n) : -1;
}

#endif /* OSITOK_NIC_H */
