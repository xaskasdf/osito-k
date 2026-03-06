/*
 * OsitoK x86-64 — Kernel Entry
 *
 * Called after ExitBootServices. We are now bare-metal.
 * Flow: memory init → PCI scan → NVMe init → mount OsitoFS → list files → halt
 */

#include "../include/types.h"
#include "../drivers/gpu.h"
#include "../drivers/gpu_inference.h"
#include "../fs/gguf.h"
#include "tensor.h"
#include "inference.h"

/* ── External functions ──────────────────────────────────────── */

/* IDT + Interrupts */
extern void idt_init(void);
extern uint64_t idt_get_ticks(void);

/* Paging */
extern void paging_init(void);
extern uint64_t paging_get_kernel_cr3(void);
extern int paging_map_mmio(uint64_t phys, uint64_t size);

/* Heap */
extern void heap_init(void);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern void *kcalloc(uint64_t count, uint64_t size);
extern void *krealloc(void *ptr, uint64_t new_size);

/* Syscall */
extern void syscall_init(void);

/* ELF loader */
extern int elf_exec(const char *filename, int argc, const char **argv);

/* Process */
extern void proc_init(void);
extern int  proc_exec(const char *filename, int argc, const char **argv);
extern void proc_list(void);

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

/* GSP Falcon */
extern int  gsp_probe(void);
extern int  gsp_load_firmware(void);
extern int  gsp_queue_init(void);
extern int  gsp_boot(void);

/* ── X-CPU3: UDP Prompt Server ────────────────────────────────── */

static llama_state_t *prompt_llama;  /* Set after model init */

static void prompt_handler(const uint8_t *src_ip, uint16_t src_port,
                           const void *data, uint32_t len)
{
    serial_puts("[PROMPT] UDP from ");
    serial_putdec(src_ip[0]); serial_puts(".");
    serial_putdec(src_ip[1]); serial_puts(".");
    serial_putdec(src_ip[2]); serial_puts(".");
    serial_putdec(src_ip[3]); serial_puts(":");
    serial_putdec(src_port);
    serial_puts(" (");
    serial_putdec(len);
    serial_puts(" bytes)\n");

    /* If no model loaded, echo back */
    if (!prompt_llama) {
        const char *msg = "[OsitoK] No model loaded — echo: ";
        net_udp_send(src_ip, src_port, 7777, msg, strlen(msg));
        net_udp_send(src_ip, src_port, 7777, data, len);
        return;
    }

    /* Reset model state for fresh generation */
    prompt_llama->pos = 0;

    /* Use BOS token as prompt — received text is logged but not tokenized
     * (tokenizer not implemented yet; token IDs would come from client) */
    uint32_t prompt_tokens[] = { 128000 };  /* BOS */
    uint32_t prompt_len_tok = 1;

    /* Check if input looks like raw token IDs (starts with '#') */
    const uint8_t *input = (const uint8_t *)data;
    if (len >= 2 && input[0] == '#') {
        /* Parse space-separated token IDs: "#128000 1234 5678" */
        uint32_t tokens[64];
        uint32_t ntok = 0;
        uint32_t val = 0;
        bool in_num = false;

        for (uint32_t i = 1; i < len && ntok < 64; i++) {
            if (input[i] >= '0' && input[i] <= '9') {
                val = val * 10 + (input[i] - '0');
                in_num = true;
            } else {
                if (in_num) { tokens[ntok++] = val; val = 0; in_num = false; }
            }
        }
        if (in_num && ntok < 64) tokens[ntok++] = val;

        if (ntok > 0) {
            serial_puts("[PROMPT] Token IDs: ");
            serial_putdec(ntok);
            serial_puts(" tokens\n");

            /* Prefill */
            for (uint32_t i = 0; i < ntok; i++)
                llama_forward(prompt_llama, tokens[i]);
        }
        prompt_len_tok = 0;  /* Already prefilled */
    }

    /* Prefill BOS if not already done */
    if (prompt_len_tok > 0) {
        for (uint32_t i = 0; i < prompt_len_tok; i++)
            llama_forward(prompt_llama, prompt_tokens[i]);
    }

    /* Generate up to 32 tokens, collect IDs */
    uint32_t gen_tokens[32];
    uint32_t gen_count = 0;

    /* Get vocab size from state */
    uint32_t vocab = prompt_llama->vocab_size;

    /* Simple argmax */
    extern uint32_t argmax(const float *v, uint32_t n);

    uint32_t next = argmax(prompt_llama->logits, vocab);

    for (uint32_t step = 0; step < 32; step++) {
        if (next == 128001 || next == 128009) break;  /* EOS */
        gen_tokens[gen_count++] = next;
        llama_forward(prompt_llama, next);
        next = argmax(prompt_llama->logits, vocab);
    }

    /* Send token IDs back as text: "128000 1234 5678\n" */
    char resp[512];
    uint32_t pos = 0;
    for (uint32_t i = 0; i < gen_count && pos < 480; i++) {
        uint32_t tok = gen_tokens[i];
        /* Convert to decimal */
        char num[12];
        int nlen = 0;
        if (tok == 0) { num[nlen++] = '0'; }
        else {
            uint32_t t = tok;
            while (t > 0) { num[nlen++] = '0' + (t % 10); t /= 10; }
        }
        /* Reverse */
        for (int j = nlen - 1; j >= 0; j--)
            resp[pos++] = num[j];
        resp[pos++] = ' ';
    }
    if (pos > 0) resp[pos - 1] = '\n';  /* Replace trailing space */

    net_udp_send(src_ip, src_port, 7777, resp, pos);

    serial_puts("[PROMPT] Generated ");
    serial_putdec(gen_count);
    serial_puts(" tokens, sent response\n");
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

    /* ── Step 1.5: IDT + Exceptions + APIC timer ── */
    idt_init();

    /* ── Step 1.6: Kernel page tables ── */
    paging_init();

    /* ── Step 1.7: Kernel heap ── */
    heap_init();

    /* ── Step 1.8: Syscall interface ── */
    syscall_init();

    /* ── Step 1.9: Process subsystem ── */
    proc_init();

    /* ── Tensor compute self-test ── */
    tensor_benchmark();

    /* ── Step 2: PCI enumeration ── */
    serial_puts("[KERN] Scanning PCIe bus...\n");
    fb_puts(" Scanning PCIe...\n");
    pci_scan();

    /* GPU MMIO probe (Phase 1) */
    gpu_device_t *gpu = pci_get_gpu();
    if (gpu && gpu->bar0_base) {
        gpu_init(gpu->bar0_base);
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
                    static llama_state_t llama;
                    bool model_ready = false;

                    if (gguf_load(&gguf_model) == 0 && gguf_model.num_tensors > 0) {
                        if (llama_init(&llama, &gguf_model, 256) == 0) {
                            /* CPU inference (baseline) */
                            uint32_t prompt[] = { 128000 };  /* BOS */
                            llama_generate(&llama, prompt, 1, 32);
                            model_ready = true;
                        }
                    }

                    /* GSP firmware loading + boot */
                    gpu_probe_t *gp = gpu_get_probe();
                    if (gp && gp->gsp_present)
                        gsp_probe();
                    gsp_load_firmware();
                    if (gp && gp->gsp_present)
                        gsp_queue_init();
                    if (gp && gp->gsp_present)
                        gsp_boot();

                    /* X40: GPU-accelerated inference (after GPU init) */
                    if (model_ready) {
                        static gpu_llama_state_t gpu_llama;
                        if (gpu_llama_init(&gpu_llama, &llama) == 0) {
                            gpu_llama_benchmark(&gpu_llama);

                            /* GPU-accelerated inference run */
                            llama.pos = 0;  /* Reset position for fresh run */
                            uint32_t prompt2[] = { 128000 };
                            gpu_llama_generate(&gpu_llama, prompt2, 1, 32);

                            gpu_llama_free(&gpu_llama);
                        }
                        /* Keep llama state alive for UDP prompt server */
                        prompt_llama = &llama;
                    } else {
                        /* No model — run standalone GPU benchmark */
                        gpu_llama_benchmark_standalone();
                    }
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
            net_udp_listen(7777, prompt_handler);
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
