/*
 * OsitoK x86-64 — Kernel Entry
 *
 * Called after ExitBootServices. We are now bare-metal.
 * Flow: memory init → PCI scan → NVMe init → mount OsitoFS → list files → halt
 */

#include "../include/types.h"
#include "../drivers/gpu.h"
#include "../fs/gguf.h"

/* ── External functions ──────────────────────────────────────── */

/* Serial */
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* Framebuffer */
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);
extern void fb_puthex(uint64_t val, int digits);

/* Memory */
extern void mem_init(void *mmap, uint64_t mmap_size, uint64_t desc_size);

/* PCI */
extern void pci_scan(void);
extern gpu_device_t *pci_get_gpu(void);
extern void *pci_get_nvme(void); /* Returns pci_dev_t* */
extern void *pci_get_nic(void);  /* Returns pci_dev_t* */

/* NVMe */
extern int nvme_init(uint64_t bar0_phys);
extern bool nvme_is_ready(void);

/* I211 Ethernet */
extern int  i211_init(uint64_t bar0_phys);
extern bool i211_link_up(void);

/* Network stack */
extern void net_init(const uint8_t ip[4]);
extern void net_poll(void);
extern void net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                         uint16_t src_port, const void *data, uint32_t len);
typedef void (*udp_handler_t)(const uint8_t *src_ip, uint16_t src_port,
                              const void *data, uint32_t len);
extern void net_udp_listen(uint16_t port, udp_handler_t handler);

/* OsitoFS */
extern int osfs2_mount(uint64_t part_offset);
extern void osfs2_list(void);
extern bool osfs2_is_mounted(void);

/* ── UDP Echo Handler ────────────────────────────────────────── */

static void echo_handler(const uint8_t *src_ip, uint16_t src_port,
                          const void *data, uint32_t len)
{
    serial_puts("[NET] UDP echo from ");
    serial_putdec(src_ip[0]); serial_puts(".");
    serial_putdec(src_ip[1]); serial_puts(".");
    serial_putdec(src_ip[2]); serial_puts(".");
    serial_putdec(src_ip[3]); serial_puts(":");
    serial_putdec(src_port);
    serial_puts(" (");
    serial_putdec(len);
    serial_puts(" bytes)\n");

    /* Echo back to sender */
    net_udp_send(src_ip, src_port, 7777, data, len);
}

/* ── Banner ──────────────────────────────────────────────────── */

static void print_banner(void)
{
    const char *banner =
        "\n"
        "  ____       _ _        _  __\n"
        " / __ \\  ___(_) |_ ___ | |/ /\n"
        "| |  | |/ __| | __/ _ \\| ' / \n"
        "| |__| |\\__ \\ | || (_) | . \\ \n"
        " \\____/ |___/_|\\__\\___/|_|\\_\\\n"
        "\n"
        " Bare-Metal AI OS — x86-64\n"
        " naranjositos.tech\n"
        "\n";

    serial_puts(banner);
    fb_puts_color(banner, 0x00FF8800); /* Orange */
}

/* ── Kernel Entry Point ──────────────────────────────────────── */

void kernel_entry(void *memory_map, uint64_t map_size,
                  uint64_t desc_size, uint64_t desc_version)
{
    (void)desc_version;

    print_banner();

    /* ── Step 1: Initialize memory manager ── */
    serial_puts("[KERN] Initializing memory manager...\n");
    fb_puts(" Initializing memory...\n");
    mem_init(memory_map, map_size, desc_size);

    /* ── Step 2: PCI enumeration ── */
    serial_puts("[KERN] Scanning PCIe bus...\n");
    fb_puts(" Scanning PCIe...\n");
    pci_scan();

    /* Report GPU */
    gpu_device_t *gpu = pci_get_gpu();
    if (gpu) {
        serial_puts("[KERN] GPU found: NVIDIA ");
        serial_puts(gpu_gen_name(gpu->generation));
        serial_puts(" [");
        serial_puthex(gpu->device_id, 4);
        serial_puts("] BAR0=");
        serial_puthex(gpu->bar0_base, 16);
        serial_puts("\n");

        fb_puts("\n GPU: NVIDIA ");
        fb_puts_color(gpu_gen_name(gpu->generation), 0x0000FF00);
        fb_puts(" [");
        fb_puthex(gpu->device_id, 4);
        fb_puts("]\n");
        fb_puts("  BAR0: ");
        fb_puthex(gpu->bar0_base, 16);
        fb_puts("\n");
        fb_puts("  Backend: GSP-shim (Phase 0 — detect only)\n");
    } else {
        serial_puts("[KERN] No NVIDIA GPU found\n");
        fb_puts("\n GPU: not detected\n");
    }

    /* ── Step 3: NVMe init ── */
    /* Get NVMe BAR0 from PCI scan */
    typedef struct {
        uint8_t  bus, dev, func;
        uint16_t vendor_id, device_id;
        uint8_t  class_code, subclass;
        uint64_t bar[6];
    } pci_dev_t;

    pci_dev_t *nvme_pci = (pci_dev_t *)pci_get_nvme();
    if (nvme_pci && nvme_pci->bar[0]) {
        serial_puts("[KERN] Initializing NVMe...\n");
        fb_puts("\n Initializing NVMe...\n");

        if (nvme_init(nvme_pci->bar[0]) == 0) {
            serial_puts("[KERN] NVMe ready\n");

            /* ── Step 4: Mount OsitoFS via GPT ── */
            extern int gpt_find_ositofs(uint64_t *part_offset, uint64_t *part_size);

            uint64_t part_offset, part_size;
            if (gpt_find_ositofs(&part_offset, &part_size) == 0) {
                if (osfs2_mount(part_offset) == 0) {
                    osfs2_list();

                    /* Load GGUF model (if present) */
                    static gguf_model_t gguf_model;
                    gguf_load(&gguf_model);
                }
            } else {
                serial_puts("[KERN] OsitoFS partition not found in GPT\n");
                fb_puts("\n OsitoFS: not found\n");
            }
        } else {
            serial_puts("[KERN] NVMe init failed\n");
            fb_puts(" NVMe: init failed\n");
        }
    } else {
        serial_puts("[KERN] No NVMe controller found\n");
        fb_puts("\n NVMe: not detected\n");
    }

    /* ── Step 4: Network (I211 Ethernet + UDP) ── */
    pci_dev_t *nic_pci = (pci_dev_t *)pci_get_nic();
    if (nic_pci && nic_pci->bar[0]) {
        serial_puts("[KERN] Initializing I211 NIC...\n");
        fb_puts("\n Initializing NIC...\n");

        if (i211_init(nic_pci->bar[0]) == 0) {
            uint8_t ip[] = {192, 168, 1, 100};
            net_init(ip);
            net_udp_listen(7777, echo_handler);
        } else {
            serial_puts("[KERN] I211 init failed\n");
            fb_puts(" NIC: init failed\n");
        }
    } else {
        serial_puts("[KERN] No Ethernet NIC found\n");
        fb_puts("\n NIC: not detected\n");
    }

    /* ── Done — main loop with net polling ── */
    serial_puts("\n[KERN] Boot complete. Entering main loop.\n");
    serial_puts("[KERN] Serial console ready (115200 8N1)\n");

    fb_puts("\n Boot complete. Network active.\n");

    for (;;) {
        net_poll();
        __asm__ volatile ("hlt");
    }
}
