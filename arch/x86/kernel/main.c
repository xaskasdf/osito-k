/*
 * OsitoK x86-64 — Kernel Entry
 *
 * Called after ExitBootServices. We are now bare-metal.
 * Flow: memory init → PCI scan → NVMe init → mount OsitoFS → list files → halt
 */

#include "../include/types.h"
#include "../include/boot_info.h"
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

/* Keyboard + Terminal + Shell */
extern void kb_init(void);
extern void term_init(void);
extern void shell_run(void);

/* SMP */
extern void smp_init(void);

/* Dynamic linker */
extern void dl_init(void);

/* Win32 compatibility layer */
extern void win32_init(void);

/* Serial */
extern void serial_init(void);
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* Framebuffer */
extern void fb_init(uint32_t *base, uint32_t w, uint32_t h, uint32_t pitch);
extern void fb_clear(void);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);
extern void fb_puthex(uint64_t val, int digits);

/* Memory */
extern void mem_init(void *mmap, uint64_t mmap_size, uint64_t desc_size);
extern void mem_reserve_kernel(uint64_t phys_base, uint64_t size);

/* ACPI RSDP — set from boot_info, read by smp.c */
uint64_t kernel_acpi_rsdp;

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
extern void *osfs2_find(const char *name);

/* GSP Falcon */
extern int  gsp_probe(void);
extern int  gsp_load_firmware(void);
extern int  gsp_queue_init(void);
extern int  gsp_boot(void);

/* ── X-CPU3: UDP Prompt Server ────────────────────────────────── */

llama_state_t *prompt_llama;  /* Set after model init, used by shell chat cmd */

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

    const uint8_t *input = (const uint8_t *)data;

    /* Check if input looks like raw token IDs (starts with '#') */
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

            for (uint32_t i = 0; i < ntok; i++)
                llama_forward(prompt_llama, tokens[i]);
        }
    } else {
        /* Text input — tokenize with BPE and use Llama 3 chat template */
        extern int tok_encode(const void *, const char *, uint32_t,
                              uint32_t *, uint32_t);
        extern bool tok_is_ready(const void *);
        extern char g_tokenizer[];

        uint32_t tokens[256];
        uint32_t n = 0;

        if (tok_is_ready(g_tokenizer)) {
            /* Llama 3 chat template */
            tokens[n++] = 128000;  /* <|begin_of_text|> */
            tokens[n++] = 128006;  /* <|start_header_id|> */
            int r = tok_encode(g_tokenizer, "user", 4, tokens + n, 256 - n);
            if (r > 0) n += (uint32_t)r;
            tokens[n++] = 128007;  /* <|end_header_id|> */
            r = tok_encode(g_tokenizer, "\n\n", 2, tokens + n, 256 - n);
            if (r > 0) n += (uint32_t)r;
            r = tok_encode(g_tokenizer, (const char *)input, len, tokens + n, 256 - n);
            if (r > 0) n += (uint32_t)r;
            tokens[n++] = 128009;  /* <|eot_id|> */
            tokens[n++] = 128006;  /* <|start_header_id|> */
            r = tok_encode(g_tokenizer, "assistant", 9, tokens + n, 256 - n);
            if (r > 0) n += (uint32_t)r;
            tokens[n++] = 128007;  /* <|end_header_id|> */
            r = tok_encode(g_tokenizer, "\n\n", 2, tokens + n, 256 - n);
            if (r > 0) n += (uint32_t)r;

            serial_puts("[PROMPT] Tokenized text: ");
            serial_putdec(n);
            serial_puts(" tokens\n");
        } else {
            /* Fallback: BOS only */
            tokens[n++] = 128000;
        }

        for (uint32_t i = 0; i < n; i++)
            llama_forward(prompt_llama, tokens[i]);
    }

    /* Generate up to 32 tokens, collect IDs */
    uint32_t gen_tokens[32];
    uint32_t gen_count = 0;

    /* Get vocab size from state */
    uint32_t vocab = prompt_llama->vocab_size;

    /* Sample with temperature + top-p */
    extern uint32_t sample_topp(float *, uint32_t, float, float);

    uint32_t next = sample_topp(prompt_llama->logits, vocab, 0.6f, 0.9f);

    for (uint32_t step = 0; step < 32; step++) {
        if (next == 128001 || next == 128009) break;  /* EOS */
        gen_tokens[gen_count++] = next;
        llama_forward(prompt_llama, next);
        next = sample_topp(prompt_llama->logits, vocab, 0.6f, 0.9f);
    }

    /* Send decoded text (or token IDs if no tokenizer) */
    extern const char *tok_global_decode(uint32_t id);
    char resp[512];
    uint32_t pos = 0;

    for (uint32_t i = 0; i < gen_count && pos < 480; i++) {
        const char *text = tok_global_decode(gen_tokens[i]);
        if (text) {
            while (*text && pos < 480) resp[pos++] = *text++;
        } else {
            /* Fallback: decimal token ID */
            uint32_t tok = gen_tokens[i];
            char num[12];
            int nlen = 0;
            if (tok == 0) { num[nlen++] = '0'; }
            else {
                uint32_t t = tok;
                while (t > 0) { num[nlen++] = '0' + (t % 10); t /= 10; }
            }
            for (int j = nlen - 1; j >= 0; j--)
                resp[pos++] = num[j];
            resp[pos++] = ' ';
        }
    }
    if (pos > 0 && resp[pos - 1] == ' ') resp[pos - 1] = '\n';

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

/* BSS symbols: provided by kernel.ld when linking with ld, or by these
 * weak fallbacks when linking with TCC's linker (BSS already zeroed by
 * UEFI AllocatePages, so the zeroing loop becomes a no-op). */
extern char __bss_start[] __attribute__((weak));
extern char __bss_end[]   __attribute__((weak));

void kernel_entry(boot_info_t *info)
{
    /* ── Step -1: Zero BSS (UEFI AllocatePages returns zeroed memory, but
     * the kernel's BSS extends beyond the file-backed data segment) ── */
    {
        uint64_t *p = (uint64_t *)__bss_start;
        uint64_t *end = (uint64_t *)((uintptr_t)__bss_end & ~7ULL);
        while (p < end)
            *p++ = 0;
    }

    /* ── Step 0: Initialize serial + framebuffer (moved from boot) ── */
    serial_init();
    serial_puts("\r\n[OsitoK] Serial initialized (COM1 115200)\r\n");

    fb_init((uint32_t *)(uintptr_t)info->fb_base,
            info->fb_width, info->fb_height, info->fb_pitch);
    fb_clear();

    /* Store ACPI RSDP for smp.c */
    kernel_acpi_rsdp = info->acpi_rsdp;

    print_banner();

    /* ── Step 1: Initialize memory manager ── */
    serial_puts("[KERN] Initializing memory manager...\n");
    fb_puts(" Initializing memory...\n");
    mem_init((void *)(uintptr_t)info->mmap_addr, info->mmap_size, info->mmap_desc_size);

    /* Reserve kernel pages so allocator doesn't hand them out */
    if (info->kernel_phys_base && info->kernel_size)
        mem_reserve_kernel(info->kernel_phys_base, info->kernel_size);

    /* ── Step 1.5: IDT + Exceptions + APIC timer ── */
    idt_init();

    /* ── Step 1.6: Kernel page tables ── */
    paging_init();

    /* ── Step 1.7: Kernel heap ── */
    heap_init();

    /* ── Step 1.75: SMP — wake AP cores ── */
    smp_init();

    /* ── Step 1.8: Syscall interface ── */
    syscall_init();

    /* ── Step 1.9: Process subsystem ── */
    proc_init();

    /* ── Step 1.10: Dynamic linker ── */
    dl_init();

    /* ── Step 1.11: Win32 compatibility layer ── */
    win32_init();

    /* ── Crypto self-test ── */
    {
        extern int crypto_selftest(void);
        crypto_selftest();
    }

    /* ── Tensor compute self-test ── */
    tensor_benchmark();

    /* ── Step 2: PCI enumeration ── */
    serial_puts("[KERN] Scanning PCIe bus...\n");
    fb_puts(" Scanning PCIe...\n");
    pci_scan();

    /* ── Map PCI device BARs into page tables ── */
    typedef struct {
        uint8_t  bus, dev, func;
        uint16_t vendor_id, device_id;
        uint8_t  class_code, subclass;
        uint64_t bar[6];
    } pci_dev_t;

    /* Map GPU BAR0 if present */
    gpu_device_t *gpu = pci_get_gpu();
    if (gpu && gpu->bar0_base) {
        paging_map_mmio(gpu->bar0_base, 32ULL * 1024 * 1024);  /* 32MB */
    }

    /* Map NVMe BAR0 */
    pci_dev_t *nvme_pci = (pci_dev_t *)pci_get_nvme();
    if (nvme_pci && nvme_pci->bar[0]) {
        paging_map_mmio(nvme_pci->bar[0], 16 * 1024);  /* 16KB NVMe regs */
        serial_puts("[KERN] Mapped NVMe BAR0 0x");
        serial_puthex(nvme_pci->bar[0], 16);
        serial_puts("\n");
    }

    /* Map NIC BAR0 */
    pci_dev_t *nic_pci_early = (pci_dev_t *)pci_get_nic();
    if (nic_pci_early && nic_pci_early->bar[0]) {
        paging_map_mmio(nic_pci_early->bar[0], 128 * 1024);  /* 128KB igb */
    }

    /* Flush TLB after all MMIO mappings */
    __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    /* GPU MMIO probe (Phase 1) */
    if (gpu && gpu->bar0_base) {
        gpu_init(gpu->bar0_base);
    } else {
        serial_puts("[KERN] No NVIDIA GPU found\n");
        fb_puts("\n GPU: not detected\n");
    }

    /* ── Step 3: NVMe init ── */
    if (nvme_pci && nvme_pci->bar[0]) {
        serial_puts("[KERN] Initializing NVMe...\n");
        fb_puts("\n Initializing NVMe...\n");

        if (nvme_init(nvme_pci->bar[0]) == 0) {
            serial_puts("[KERN] NVMe ready\n");

            /* ── Step 4: Mount OsitoFS via GPT ── */
            extern int gpt_find_ositofs(uint64_t *part_offset, uint64_t *part_size);

            uint64_t part_offset, part_size;
            bool fs_mounted = false;

            if (gpt_find_ositofs(&part_offset, &part_size) == 0) {
                fs_mounted = (osfs2_mount(part_offset) == 0);
            }

            /* Fallback: try raw OsitoFS at offset 0 (no GPT) */
            if (!fs_mounted) {
                serial_puts("[KERN] GPT not found, trying raw OsitoFS at offset 0...\n");
                fs_mounted = (osfs2_mount(0) == 0);
            }

            if (fs_mounted) {
                osfs2_list();

                /* Load GGUF model (if present) */
                static gguf_model_t gguf_model;
                static llama_state_t llama;
                bool model_ready = false;

                if (gguf_load(&gguf_model) == 0 && gguf_model.num_tensors > 0) {
                    /* Load tokenizer from GGUF metadata */
                    {
                        static gguf_tokenizer_t gtok;
                        if (gguf_load_tokenizer(&gguf_model, &gtok) == 0) {
                            extern int tok_init(void *, const char **,
                                const uint32_t *, uint32_t, const char **,
                                const uint32_t *, uint32_t, uint32_t, uint32_t);
                            extern char g_tokenizer[];
                            tok_init(g_tokenizer,
                                     gtok.tokens, gtok.token_lens, gtok.n_tokens,
                                     gtok.merges, gtok.merge_lens, gtok.n_merges,
                                     gtok.bos_id, gtok.eos_id);
                        }
                    }

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

                /* ── Try executing ELF programs if present ── */
                if (osfs2_find("hello.elf")) {
                    serial_puts("[KERN] Found hello.elf — executing...\n");
                    proc_exec("hello.elf", 0, NULL);
                }

                if (osfs2_find("fileio.elf")) {
                    serial_puts("[KERN] Found fileio.elf — executing...\n");
                    int ret = proc_exec("fileio.elf", 0, NULL);
                    serial_puts("[KERN] fileio.elf exited with code ");
                    serial_putdec(ret < 0 ? (uint64_t)(-(int64_t)ret) : (uint64_t)ret);
                    serial_puts("\n");
                }

                if (osfs2_find("hello_c.elf")) {
                    serial_puts("[KERN] Found hello_c.elf (TCC) — executing...\n");
                    int ret = proc_exec("hello_c.elf", 0, NULL);
                    serial_puts("[KERN] hello_c.elf exited with code ");
                    serial_putdec(ret < 0 ? (uint64_t)(-(int64_t)ret) : (uint64_t)ret);
                    serial_puts("\n");
                }

                if (osfs2_find("tcc.elf") && osfs2_find("selftest.c")) {
                    /* Step 1: TCC compiles+links selftest.c → selftest.elf */
                    serial_puts("[KERN] === Self-hosting test ===\n");
                    serial_puts("[KERN] Step 1: TCC compiling selftest.c...\n");
                    const char *tcc_argv[] = {
                        "tcc", "-nostdlib", "-nostdinc", "-static",
                        "selftest.c", "-o", "selftest.elf"
                    };
                    int ret = proc_exec("tcc.elf", 7, tcc_argv);
                    serial_puts("[KERN] TCC exited with code ");
                    serial_putdec(ret < 0 ? (uint64_t)(-(int64_t)ret) : (uint64_t)ret);
                    serial_puts("\n");

                    /* Step 2: Execute the freshly compiled binary */
                    if (ret == 0 && osfs2_find("selftest.elf")) {
                        serial_puts("[KERN] Step 2: Executing selftest.elf...\n");
                        ret = proc_exec("selftest.elf", 0, NULL);
                        serial_puts("[KERN] selftest.elf exited with code ");
                        serial_putdec(ret < 0 ? (uint64_t)(-(int64_t)ret) : (uint64_t)ret);
                        serial_puts("\n");
                    } else if (ret == 0) {
                        serial_puts("[KERN] selftest.elf not found on disk after compile\n");
                    }
                }
            } else {
                serial_puts("[KERN] OsitoFS not found (no GPT, no raw)\n");
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
            /* 10.0.2.15 = QEMU SLIRP default DHCP.
             * Change to 192.168.1.100 for real hardware. */
            uint8_t ip[] = {10, 0, 2, 15};
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

    /* HTTP + Claude API available via shell commands (curl, apikey, ask) */

    /* ── Step 5: Keyboard + Terminal + Shell ── */
    kb_init();
    term_init();

    serial_puts("\n[KERN] Boot complete.\n");
    fb_puts("\n Boot complete.\n");

    /* Run interactive shell (never returns) */
    shell_run();
}
