/*
 * usb.h -- SM8350 USB: CDC-ACM serial gadget on usb_1 (the side USB-C port)
 */
#ifndef SM8350_USB_H
#define SM8350_USB_H

#include "../../include/types.h"

/* usb.c: clocks, SMMU check, gadget start (called from platform_probe_devices) */
void usb_probe(void);

/* dwc3_acm.c. `dma` is 16 KB of zeroed memory whose physical address is also
 * the device address the controller uses (checked against the SMMU first). */
int  acm_start(uint64_t dwc3_base, uint64_t dma);
void acm_poll(void);        /* process controller events, push pending TX */
int  acm_getc(void);        /* next received byte, or -1 */
void acm_putc(char c);      /* queue a byte for the host (dropped when full) */
int  acm_tx_idle(void);     /* everything queued has reached the host */
void acm_stop(void);        /* disconnect from the host and halt the controller */

#endif
