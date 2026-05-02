/*
 * OsitoK x86-64 — Kernel Entry
 *
 * Called after ExitBootServices. We are now bare-metal.
 * Flow: memory init → PCI scan → NVMe init → mount OsitoFS → list files → halt
 */

#include "../include/types.h"
#include "../include/boot_info.h"
#include "../include/paging.h"
#include "../include/tensor_arena.h"
#include "../drivers/gpu.h"
#include "../drivers/gpu_inference.h"
#include "../fs/gguf.h"
#include "../fs/vfs.h"
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

/* Memory reclaim */
extern void reclaim_init_memory(void);

/* VDSO */
extern void vdso_init(void);
extern void vdso_thunks_init(void);

/* SMP */
extern void smp_init(void);

/* Dynamic linker */
extern void dl_init(void);

/* Win32 compatibility layer */
extern void win32_init(void);

/* xHCI USB */
extern int  xhci_init(uint64_t bar0_phys, uint8_t bus, uint8_t dev, uint8_t func);
extern void xhci_poll(void);

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
extern void *mem_alloc_pages(uint64_t count);

/* ACPI RSDP — set from boot_info, read by smp.c */
uint64_t kernel_acpi_rsdp;

/* Saved boot_info for kexec — preserved across kernel lifetime */
boot_info_t saved_boot_info;
static uint8_t saved_mmap[8192] __attribute__((aligned(16)));  /* copy of UEFI mmap */

/* PCI */
extern void pci_scan(void);
extern gpu_device_t *pci_get_gpu(void);
extern void *pci_get_nvme(void); /* Returns pci_dev_t* */
extern int   pci_get_nvme_count(void);
extern void *pci_get_nvme_idx(int idx); /* Returns pci_dev_t* */
extern void *pci_get_nic(void);  /* Returns pci_dev_t* */
extern void *pci_get_xhci(void); /* Returns pci_dev_t* */

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

extern int osfs3_mount(uint64_t part_offset);
extern uint32_t osfs3_resolve_path(const char *path);
extern bool osfs3_is_mounted(void);

static bool osfs_find(const char *n, vfs_node_t *node) {
    return vfs_find(n, VFS_MODE_POSIX, node);
}

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

static void enable_sse(void) { uint64_t cr0, cr4; __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0)); cr0 &= ~(1ULL << 2); cr0 |= (1ULL << 1); __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0)); __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4)); cr4 |= (1ULL << 9); cr4 |= (1ULL << 10); __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4)); }
void __initk kernel_entry(boot_info_t *info)
{
    /* ── Step -1: Zero BSS (UEFI AllocatePages returns zeroed memory, but
     * the kernel's BSS extends beyond the file-backed data segment) ── */
    {
        uint64_t *p = (uint64_t *)__bss_start;
        uint64_t *end = (uint64_t *)((uintptr_t)__bss_end & ~7ULL);
        while (p < end)
            *p++ = 0;
    }

    /* ── Save boot_info + UEFI mmap for kexec ── */
    saved_boot_info = *info;
    {
        uint64_t mmap_copy_sz = info->mmap_size;
        if (mmap_copy_sz > sizeof(saved_mmap))
            mmap_copy_sz = sizeof(saved_mmap);
        uint8_t *src = (uint8_t *)(uintptr_t)info->mmap_addr;
        for (uint64_t i = 0; i < mmap_copy_sz; i++)
            saved_mmap[i] = src[i];
        saved_boot_info.mmap_addr = (uint64_t)(uintptr_t)saved_mmap;
        saved_boot_info.mmap_size = mmap_copy_sz;
    }

    /* ── Step 0: Initialize serial + framebuffer (moved from boot) ── */
    serial_init();
    enable_sse();
    serial_puts("\r\n[OsitoK] Serial initialized (COM1 115200)\r\n");

    /* Map VRAM through the upper-half mirror so the fb driver can
     * write pixels from any process's CR3 (PML4[256] is shared, the
     * lower-half identity map is per-process). */
    fb_init((uint32_t *)PHYS_TO_VIRT(info->fb_base),
            info->fb_width, info->fb_height, info->fb_pitch);
    fb_clear();

    /* Store ACPI RSDP for smp.c */
    kernel_acpi_rsdp = info->acpi_rsdp;

    print_banner();

    /* ── Step 0.5: CPU feature detection (must run before any AVX/PMU use) ── */
    {
        extern void cpu_features_detect(void);
        extern void cpu_features_dump(void);
        extern void perf_init(void);
        extern void dispatch_init(void);
        cpu_features_detect();
        cpu_features_dump();
        perf_init();
        dispatch_init();
    }

    /* ── Step 1: Initialize memory manager ── */
    serial_puts("[KERN] Initializing memory manager...\n");
    fb_puts(" Initializing memory...\n");
    mem_init((void *)(uintptr_t)info->mmap_addr, info->mmap_size, info->mmap_desc_size);

    /* Reserve kernel pages so allocator doesn't hand them out */
    if (info->kernel_phys_base && info->kernel_size)
        mem_reserve_kernel(info->kernel_phys_base, info->kernel_size);

    /* Compute system capabilities from actual hardware */
    {
        extern void sys_caps_init(void);
        sys_caps_init();
    }

    /* ── Step 1.5: IDT + Exceptions + APIC timer ── */
    idt_init();

    /* ── Step 1.6: Kernel page tables ── */
    paging_init();

    /* ── Step 1.6b: PAT WC + shadow framebuffer ── */
    {
        extern void paging_setup_pat(void);
        extern int  paging_map_wc(uint64_t phys, uint64_t size);
        extern void fb_enable_shadow(void *buf);

        /* Program PAT entry 1 = WC for fast framebuffer writes */
        paging_setup_pat();

        /* Map framebuffer VRAM as Write-Combining */
        uint64_t fb_phys = info->fb_base;
        uint64_t fb_size = (uint64_t)info->fb_height * info->fb_pitch * 4;
        fb_size = (fb_size + 0x1FFFFF) & ~0x1FFFFFULL; /* Round up to 2MB */
        paging_map_wc(fb_phys, fb_size);
        __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

        /* Allocate shadow buffer in RAM for fast drawing. CPU-only
         * buffer: accessed via the upper-half mirror so we stay aligned
         * with heap.c and other migrated subsystems. */
        uint64_t shadow_pages = (fb_size + 4095) / 4096;
        void *shadow_phys = mem_alloc_pages(shadow_pages);
        if (shadow_phys) {
            fb_enable_shadow(PHYS_TO_VIRT(shadow_phys));
            serial_puts("[FB] Shadow framebuffer enabled (");
            serial_putdec(fb_size / 1024);
            serial_puts(" KB)\n");
        }
    }

    /* ── Step 1.7: Kernel heap ── */
    heap_init();

    /* ── Step 1.72: klog ring buffer (so dmesg works after boot) ──
     * Must come right after heap_init since klog allocates via kmalloc.
     * Anything serial_puts'd before this point is lost — we mostly care
     * about GPU/NVMe/USB diagnostics which happen later. */
    {
        extern void klog_init(void);
        klog_init();
    }

    /* ── Step 1.74: Per-CPU FPU state (must be before SMP) ── */
    {
        extern void fpu_percpu_init(void);
        fpu_percpu_init();
    }

    /* ── Step 1.75: SMP — wake AP cores ── */
    smp_init();

    /* ── Step 1.76: SMP work distribution — activate AP workers ── */
    {
        extern void smp_work_init(void);
        smp_work_init();
    }

    /* ── Step 1.8: Syscall interface ── */
    syscall_init();

    /* ── Step 1.8b: VDSO shared data page + code thunks ── */
    vdso_init();
    vdso_thunks_init();

    /* ── Step 1.9: Process subsystem ── */
    proc_init();

    /* ── Step 1.10: Dynamic linker ── */
    dl_init();

    /* ── Step 1.11: Win32 compatibility layer ── */
    win32_init();

    /* ── Crypto self-test (on AP) + PCI scan (on BSP) in parallel ── */
    {
        extern int crypto_selftest(void);
        extern int smp_submit_any(void (*)(void*, void*), void*, void*);
        extern void smp_wait(int);
        extern void boot_crypto_worker(void *, void *);
        int crypto_ap = smp_submit_any(boot_crypto_worker, NULL, NULL);

        /* Step 2: PCI enumeration (BSP, while crypto runs on AP) */
        serial_puts("[KERN] Scanning PCIe bus...\n");
        fb_puts(" Scanning PCIe...\n");
        pci_scan();

        if (crypto_ap >= 0) smp_wait(crypto_ap);
        else crypto_selftest();  /* fallback: no AP available */
    }

    /* ── Tensor benchmark — bare-metal friendly: skip AP dispatch.
     * On QEMU the AP runs the bench in parallel with BSP MMIO mapping;
     * on real hardware the AP can hang on framebuffer access before
     * the BSP's TLB flush propagates. Force BSP-only path; benchmark
     * runs serially after MMIO mapping completes. */
    int tensor_ap = -1;

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

    /* Map NVMe BAR0 (all controllers) */
    int nvme_count = pci_get_nvme_count();
    for (int i = 0; i < nvme_count; i++) {
        pci_dev_t *nd = (pci_dev_t *)pci_get_nvme_idx(i);
        if (nd && nd->bar[0]) {
            paging_map_mmio(nd->bar[0], 16 * 1024);
            serial_puts("[KERN] Mapped NVMe[");
            serial_putdec(i);
            serial_puts("] BAR0 0x");
            serial_puthex(nd->bar[0], 16);
            serial_puts("\n");
        }
    }
    pci_dev_t *nvme_pci = (pci_dev_t *)pci_get_nvme(); /* first, for compat */

    /* Map NIC BAR0 */
    pci_dev_t *nic_pci_early = (pci_dev_t *)pci_get_nic();
    if (nic_pci_early && nic_pci_early->bar[0]) {
        paging_map_mmio(nic_pci_early->bar[0], 128 * 1024);  /* 128KB igb */
    }

    /* Map xHCI BAR0 */
    pci_dev_t *xhci_pci = (pci_dev_t *)pci_get_xhci();
    if (xhci_pci && xhci_pci->bar[0]) {
        paging_map_mmio(xhci_pci->bar[0], 64 * 1024);  /* 64KB xHCI regs */
        serial_puts("[KERN] Mapped xHCI BAR0 0x");
        serial_puthex(xhci_pci->bar[0], 16);
        serial_puts("\n");
    }

    /* Map HDA BAR0 */
    extern pci_dev_t *pci_get_hda(void);
    pci_dev_t *hda_pci = (pci_dev_t *)pci_get_hda();
    if (hda_pci && hda_pci->bar[0]) {
        paging_map_mmio(hda_pci->bar[0], 32 * 1024);  /* 32KB HDA regs */
    }

    /* Flush TLB after all MMIO mappings */
    __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    /* Wait for tensor benchmark (was running on AP during BAR mapping) */
    {
        extern void smp_wait(int);
        if (tensor_ap >= 0) smp_wait(tensor_ap);
        /* TODO(bare-metal): tensor_benchmark hangs on real hw at the
         *   first fb_puts inside the self-test. AP-dispatch already
         *   disabled. BSP-direct also hangs (verified bare-metal).
         *   Suspect: framebuffer access pattern races with paging_map_mmio
         *   (called just above for GPU/NVMe/NIC/xHCI/HDA BARs) — the
         *   TLB-flush at line 536 may not reach the framebuffer mapping,
         *   or the fb_addr captured at boot is stale after MMIO mapping
         *   shifts the page tables. To debug: add serial_puts before
         *   each fb_puts in tensor_benchmark to find which one hangs.
         *   For now, skip entirely so we get to the shell. */
        /* else tensor_benchmark(); */
        else serial_puts("[TENSOR] benchmark skipped (bare-metal workaround)\n");
    }

    /* GPU MMIO probe (Phase 1) */
    if (gpu && gpu->bar0_base) {
        gpu_init(gpu->bar0_base);
    } else {
        serial_puts("[KERN] No NVIDIA GPU found\n");
    }

    /* Virtio GPU probe */
    {
        extern pci_dev_t *pci_get_virtio_gpu(void);
        extern uint64_t   pci_get_ecam_base(void);
        extern void virtio_gpu_init(uint64_t ecam, uint8_t bus, uint8_t dev, uint8_t func,
                                    uint32_t fb_w, uint32_t fb_h);
        pci_dev_t *vgpu = pci_get_virtio_gpu();
        if (vgpu) {
            virtio_gpu_init(pci_get_ecam_base(), vgpu->bus, vgpu->dev, vgpu->func,
                           info->fb_width, info->fb_height);

            /* Vulkan Phase 1 -- Wave 1: 3D extension + selftest */
            extern void virtio_gpu_3d_init(void);
            extern void virtio_gpu_3d_selftest(void);
            virtio_gpu_3d_init();
            virtio_gpu_3d_selftest();
        } else {
            serial_puts("[KERN] No virtio-GPU found\n");
        }
    }

    /* (NVK backend status is checked in Step 4.9 after GSP/RM boot,
     * since on bare-metal GSP only comes online after the FS is mounted
     * and the firmware blob has been loaded.) */

    /* ── Step 3: bring up every NVMe controller (no FS scan yet) ──
     *
     * Each successful nvme_init registers the controller as a blkdev
     * (drivers/nvme.c → blkdev_register). The OsitoFS scan happens
     * AFTER xHCI init (Step 4.7) so USB devices are also in the pool. */
    serial_puts("[KERN] Initializing NVMe (");
    serial_putdec(nvme_count);
    serial_puts(" controller(s))...\n");
    fb_puts("\n Initializing NVMe...\n");

    for (int i = 0; i < nvme_count; i++) {
        pci_dev_t *nd = (pci_dev_t *)pci_get_nvme_idx(i);
        if (!nd || !nd->bar[0]) continue;
        serial_puts("[KERN] NVMe[");
        serial_putdec(i);
        serial_puts("] init...\n");
        if (nvme_init(nd->bar[0]) != 0) {
            serial_puts("[KERN] NVMe[");
            serial_putdec(i);
            serial_puts("] init failed\n");
        }
    }

    /* ── Step 4: Network (I211 Ethernet + UDP) ── */
    pci_dev_t *nic_pci = (pci_dev_t *)pci_get_nic();
    if (nic_pci && nic_pci->bar[0]) {
        serial_puts("[KERN] Initializing I211 NIC...\n");
        fb_puts("\n Initializing NIC...\n");

        /* Detectar familia del NIC y elegir driver.                       */
        extern int  rtl8111_init(uint64_t bar0_phys, uint64_t bar2_phys);
        extern void nic_bind_i211(void);
        extern void nic_bind_rtl8111(void);
        extern void rtl8111_enable_interrupts(uint8_t b, uint8_t d, uint8_t f);

        int nic_ok = -1;
        if (nic_pci->vendor_id == 0x10EC &&
            (nic_pci->device_id == 0x8168 || nic_pci->device_id == 0x8136 ||
             nic_pci->device_id == 0x8161)) {
            /* Realtek RTL8111/8168/8169 family.  BAR0 = PIO, BAR2 = MMIO. */
            uint64_t bar2 = nic_pci->bar[2];
            if (bar2) {
                serial_puts("[KERN] Initializing Realtek RTL8111 NIC...\n");
                fb_puts("\n Initializing NIC (RTL8111)...\n");
                nic_ok = rtl8111_init(nic_pci->bar[0], bar2);
                if (nic_ok == 0) nic_bind_rtl8111();
            }
        } else if (i211_init(nic_pci->bar[0]) == 0) {
            nic_bind_i211();
            nic_ok = 0;
        }

        if (nic_ok == 0) {
            extern void net_set_ip(const uint8_t ip[4]);
            extern void net_set_gateway(const uint8_t gw[4]);
            extern void net_dns_set_server(const uint8_t ip[4]);
            extern int  dhcp_discover(void);

            /* Arrancar net con IP placeholder 0.0.0.0 — necesario para
             * que ARP table / TX path estén inicializados antes del
             * primer broadcast.  net_set_ip() abajo escribe la IP real
             * (DHCP o estática) sin re-armar el resto.                  */
            uint8_t zero_ip[4] = {0, 0, 0, 0};
            net_init(zero_ip);

            /* Habilitar IRQ del NIC primero — DHCP necesita poll-with-yield
             * para no bloquear todo el boot mientras espera OFFER/ACK.
             * Cada driver expone su _enable_interrupts; dispatch manual
             * basado en el chip detectado.                                  */
            extern void i211_enable_interrupts(uint8_t b, uint8_t d, uint8_t f);
            if (nic_pci->vendor_id == 0x10EC) {
                rtl8111_enable_interrupts(nic_pci->bus, nic_pci->dev, nic_pci->func);
            } else {
                i211_enable_interrupts(nic_pci->bus, nic_pci->dev, nic_pci->func);
            }

            /* Intentar DHCP primero; si falla (red sin servidor o link
             * down), caer a configuración estática según el tipo de NIC. */
            int dhcp_ok = dhcp_discover();
            if (dhcp_ok != 0) {
                serial_puts("[KERN] DHCP failed, using static fallback\n");
                fb_puts(" Net: static fallback\n");
                if (nic_pci->device_id == 0x1539) {
                    /* Real hardware (I211 8086:1539) — LAN típica */
                    uint8_t ip[] = {192, 168, 0, 50};
                    net_set_ip(ip);
                    uint8_t gw[] = {192, 168, 0, 1};
                    net_set_gateway(gw);
                    uint8_t dns[] = {8, 8, 8, 8};
                    net_dns_set_server(dns);
                } else {
                    /* QEMU SLIRP user-mode networking — gateway 10.0.2.2 */
                    uint8_t ip[] = {10, 0, 2, 15};
                    net_set_ip(ip);
                    /* gateway/DNS defaults in net.c match QEMU SLIRP */
                }
            }

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

    /* ── Step 4.5: xHCI USB init ── */
    if (xhci_pci && xhci_pci->bar[0]) {
        xhci_init(xhci_pci->bar[0], xhci_pci->bus, xhci_pci->dev, xhci_pci->func);
    }

    /* ── Step 4.6: HDA audio init ── */
    if (hda_pci && hda_pci->bar[0]) {
        extern int hda_init(uint64_t, uint8_t, uint8_t, uint8_t);
        hda_init(hda_pci->bar[0], hda_pci->bus, hda_pci->dev, hda_pci->func);
    }

    /* ── Step 4.7: OsitoFS scan across every blkdev (NVMe + USB-MSC) ──
     *
     * Walk every registered blkdev, set it active, try GPT->OsitoFS,
     * fall back to raw mount at offset 0. First hit wins. */
    bool fs_mounted = false;
    {
        extern int  blkdev_count(void);
        extern void disk_set_active(int dev_idx);
        extern int  gpt_find_ositofs(uint64_t *part_offset, uint64_t *part_size);
        extern const char *blkdev_name(int idx);

        int n_bd = blkdev_count();
        serial_puts("[KERN] OsitoFS scan across ");
        serial_putdec((uint64_t)n_bd);
        serial_puts(" block device(s)...\n");
        fb_puts("\n Scanning for OsitoFS...\n");

        for (int i = 0; i < n_bd && !fs_mounted; i++) {
            disk_set_active(i);
            uint64_t part_off = 0, part_size = 0;
            (void)part_size;

            /* (1) Try GPT — if the disk has a GPT header at LBA 1 we
             * can find OsitoFS by partition name or magic probe. */
            if (gpt_find_ositofs(&part_off, &part_size) == 0) {
                if (osfs3_mount(part_off) == 0) fs_mounted = true;
                else fs_mounted = (osfs2_mount(part_off) == 0);
            }

            /* (2) Raw OsitoFS at offset 0. */
            if (!fs_mounted) {
                if (osfs3_mount(0) == 0) fs_mounted = true;
                else fs_mounted = (osfs2_mount(0) == 0);
            }

            /* (3) Magic scan at MB-aligned offsets — handles macOS-flashed
             * USB sticks where deploy-usb.sh skipped sgdisk. We probe
             * positions where deploy-usb.sh might have placed the data
             * partition (and a few extras for older layouts). */
            if (!fs_mounted) {
                extern int disk_read_bytes(uint64_t, void *, uint64_t);
                static const uint64_t probe_mb[] = {
                    1,    /* +1 MB  (boot.efi sometimes lands here) */
                    65,   /* +65 MB (current deploy-usb.sh data start) */
                    128,  /* +128 MB */
                    256,  /* +256 MB */
                    512,  /* +512 MB */
                };
                for (unsigned k = 0;
                     k < sizeof(probe_mb)/sizeof(probe_mb[0]) && !fs_mounted;
                     k++) {
                    uint64_t off = probe_mb[k] * 1024ULL * 1024ULL;
                    uint8_t  probe[4];
                    if (disk_read_bytes(off, probe, 4) < 0) continue;
                    uint32_t magic = (uint32_t)probe[0] |
                                     ((uint32_t)probe[1] << 8) |
                                     ((uint32_t)probe[2] << 16) |
                                     ((uint32_t)probe[3] << 24);
                    if (magic != 0x4F534632 /* "OSF2" */) continue;

                    serial_puts("[KERN] OSFS magic at +");
                    serial_putdec(probe_mb[k]);
                    serial_puts(" MB on ");
                    serial_puts(blkdev_name(i));
                    serial_puts("\n");
                    if (osfs3_mount(off) == 0) fs_mounted = true;
                    else fs_mounted = (osfs2_mount(off) == 0);
                }
            }

            if (fs_mounted) {
                serial_puts("[KERN] OsitoFS mounted from ");
                serial_puts(blkdev_name(i));
                serial_puts("\n");
                fb_puts(" OsitoFS: mounted from ");
                fb_puts(blkdev_name(i));
                fb_puts("\n");
            }
        }
        if (!fs_mounted) {
            serial_puts("[KERN] OsitoFS not found on any block device\n");
            fb_puts(" OsitoFS: not found\n");
        }
    }

    /* ── Step 4.8: post-mount initialization (GGUF, GSP, auto-launch) ── */
    static llama_state_t llama;
    bool model_ready = false;
    if (fs_mounted) {
        osfs2_list();

        static gguf_model_t gguf_model;
        if (gguf_load(&gguf_model) == 0 && gguf_model.num_tensors > 0) {
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
            if (!g_tensor_arena.virt_base)
                tensor_arena_init(&g_tensor_arena, 512);
            if (llama_init(&llama, &gguf_model, 256) == 0) {
                model_ready = true;
                if (g_tensor_arena.virt_base)
                    tensor_arena_stats(&g_tensor_arena, "post-llama_init");
            }
        }

        gpu_probe_t *gp = gpu_get_probe();
        if (gp && gp->gsp_present) gsp_probe();
        gsp_load_firmware();
        if (gp && gp->gsp_present) gsp_queue_init();
        if (gp && gp->gsp_present) gsp_boot();

        /* Replace boot-time VRAM probe (PRI-locked, returned 97 MB on
         * Ampere) with the real value from RM. This is what the SASS
         * uploader and any DMA-buffer allocator should see. */
        if (gp && gp->gsp_present) {
            extern uint32_t gsp_query_vram_mb(void);
            uint32_t real_mb = gsp_query_vram_mb();
            if (real_mb > 0) {
                gp->vram_size_mb = real_mb;
                fb_puts(" GPU: VRAM=");
                fb_putdec(real_mb);
                fb_puts(" MB (RM)\n");
            }
        }

        if (model_ready) prompt_llama = &llama;
    }

    /* ── Step 4.9: NVK backend status (after GSP boot chain) ──
     * On bare-metal Ampere, GSP only comes online after Step 4.8 has
     * loaded gsp.bin from OsitoFS, run the RM init RPCs, and bound a
     * compute channel. Now that all that's done we can ask the NVK
     * backend whether it's actually ready to talk to userspace. */
    {
        extern void nvk_backend_init_hook(void);
        nvk_backend_init_hook();
    }

    /* ── Step 5: Keyboard + Terminal + Shell ── */
    kb_init();
    term_init();

    /* ── Diagnostic summary (visible just before shell) ── */
    {
        extern uint64_t pci_get_ecam_base(void);
        extern uint8_t  pci_get_ecam_end_bus(void);
        extern int      pci_get_device_count(void);

        fb_puts("\n --- HW Summary ---\n");
        uint64_t ecam = pci_get_ecam_base();
        fb_puts(" PCI: ");
        if (ecam) {
            fb_puts("ECAM ");
            fb_puthex(ecam, 8);
            fb_puts(" (bus 0-");
            fb_putdec(pci_get_ecam_end_bus());
            fb_puts(")");
        } else {
            fb_puts("legacy I/O (no ECAM)");
        }
        fb_puts(", ");
        fb_putdec(pci_get_device_count());
        fb_puts(" devs\n");

        fb_puts(" GPU: ");
        fb_puts(gpu ? "yes" : "no");
        fb_puts("  NVMe: ");
        fb_puts(nvme_pci ? "yes" : "no");
        fb_puts("  NIC: ");
        fb_puts(nic_pci ? "yes" : "no");
        fb_puts("  xHCI: ");
        fb_puts(xhci_pci ? "yes" : "no");
        fb_puts("\n");

        fb_puts(" RSDP: ");
        fb_puthex(info->acpi_rsdp, 16);
        fb_puts("\n");
    }

    serial_puts("\n[KERN] Boot complete.\n");
    fb_puts("\n Boot complete.\n");

    /* Reclaim init-only code pages */
    reclaim_init_memory();

    /* Run interactive shell (never returns) */
    shell_run();
}
