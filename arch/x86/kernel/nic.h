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
    int  (*send)   (const void *data, uint32_t len);
    int  (*send_sg)(const uint64_t frag_phys[], const uint32_t lens[], int n_frags);
    int  (*recv)   (void *buf, uint32_t *len);
    void (*get_mac)(uint8_t mac[6]);
    bool (*link_up)(void);
    /* Pointer al flag volátil de IRQ pending del driver activo.            */
    volatile bool *irq_pending;
    const char *name;
} nic_ops_t;

/* Inicializado por main.c según pci_get_nic().  net.c lo lee.              */
extern nic_ops_t nic_ops;

void nic_bind_i211(void);
void nic_bind_rtl8111(void);

/* Conveniencias para no llenar net.c de "nic_ops.send(...)".               */
static inline int  nic_send   (const void *d, uint32_t l)    { return nic_ops.send(d, l); }
static inline int  nic_recv   (void *b, uint32_t *l)         { return nic_ops.recv(b, l); }
static inline void nic_get_mac(uint8_t m[6])                  { nic_ops.get_mac(m); }
static inline bool nic_link_up(void)                          { return nic_ops.link_up(); }
static inline int  nic_send_sg(const uint64_t fp[], const uint32_t ln[], int n) {
    return nic_ops.send_sg(fp, ln, n);
}

#endif /* OSITOK_NIC_H */
