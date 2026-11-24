/*
 * OsitoK x86-64 — PCI Hotplug Detection
 *
 * Periodic rescan of PCI bus for new/removed devices.
 * Notifies drivers when devices appear or disappear.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);

/* PCI config access */
extern uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg)
    __attribute__((weak));

#define PCI_MAX_TRACKED 32

typedef struct {
    bool     present;
    uint8_t  bus, dev, func;
    uint16_t vendor, device;
    uint8_t  class_code, subclass;
} pci_tracked_t;

static pci_tracked_t tracked[PCI_MAX_TRACKED];
static int tracked_count;
static bool hotplug_initialized;

/* Callback for device events */
typedef void (*pci_hotplug_cb)(uint8_t bus, uint8_t dev, uint8_t func,
                               uint16_t vendor, uint16_t device, bool added);
static pci_hotplug_cb hotplug_callback;

void pci_hotplug_init(pci_hotplug_cb cb)
{
    hotplug_callback = cb;
    hotplug_initialized = true;
    serial_puts("[PCI-HP] Hotplug detection initialized\n");
}

/* Scan PCI bus and detect changes */
void pci_hotplug_rescan(void)
{
    if (!hotplug_initialized || !pci_read_config) return;

    /* Quick scan bus 0 (simplified — full scan would do all buses) */
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t reg0 = pci_read_config(0, dev, 0, 0);
        uint16_t vendor = reg0 & 0xFFFF;
        uint16_t device_id = (reg0 >> 16) & 0xFFFF;

        if (vendor == 0xFFFF) {
            /* No device — check if was previously tracked */
            for (int i = 0; i < tracked_count; i++) {
                if (tracked[i].present && tracked[i].bus == 0 &&
                    tracked[i].dev == dev && tracked[i].func == 0) {
                    tracked[i].present = false;
                    serial_puts("[PCI-HP] Removed: ");
                    serial_puthex(tracked[i].vendor, 4);
                    serial_puts(":");
                    serial_puthex(tracked[i].device, 4);
                    serial_puts("\n");
                    if (hotplug_callback)
                        hotplug_callback(0, dev, 0, tracked[i].vendor,
                                        tracked[i].device, false);
                }
            }
            continue;
        }

        /* Device present — check if new */
        bool found = false;
        for (int i = 0; i < tracked_count; i++) {
            if (tracked[i].present && tracked[i].bus == 0 &&
                tracked[i].dev == dev) {
                found = true;
                break;
            }
        }

        if (!found && tracked_count < PCI_MAX_TRACKED) {
            tracked[tracked_count].present = true;
            tracked[tracked_count].bus = 0;
            tracked[tracked_count].dev = dev;
            tracked[tracked_count].func = 0;
            tracked[tracked_count].vendor = vendor;
            tracked[tracked_count].device = device_id;
            tracked_count++;
            serial_puts("[PCI-HP] New device: ");
            serial_puthex(vendor, 4);
            serial_puts(":");
            serial_puthex(device_id, 4);
            serial_puts("\n");
            if (hotplug_callback)
                hotplug_callback(0, dev, 0, vendor, device_id, true);
        }
    }
}
