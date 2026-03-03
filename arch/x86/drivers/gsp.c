/*
 * OsitoK x86-64 — GSP Falcon Driver (Phase 4-5)
 *
 * Phase 4: Deep probe (HWCFG2, CPUCTL, mailboxes), firmware load + VRAM upload.
 * Phase 5: ELF64 parse, boot sequence (BOOTVEC, CPUCTL start), mailbox handshake.
 *
 * Reference: envytools (https://envytools.rtfd.io), nouveau driver.
 */

#include "../include/types.h"
#include "gpu.h"
#include "../../../include/common/ositofs2_format.h"

/* ── External Functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);
extern void fb_puthex(uint64_t val, int digits);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern osfs2_file_t *osfs2_find(const char *name);
extern int osfs2_read(osfs2_file_t *file, uint64_t offset, void *buf, uint64_t len);
extern bool osfs2_is_mounted(void);

/* rdtsc for timing */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ── Driver State ────────────────────────────────────────────── */

static gsp_state_t gsp;

/* ── GSP Falcon Probe ────────────────────────────────────────── */

int gsp_probe(void)
{
    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present || !p->gsp_present) {
        serial_puts("[GSP] GSP falcon not detected, skipping probe\n");
        return -1;
    }

    memset(&gsp, 0, sizeof(gsp));

    serial_puts("[GSP] Falcon probe:\n");

    /* HWCFG (already read in Phase 1, read again for our state) */
    gsp.hwcfg = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_HWCFG);

    /* HWCFG2 — IMEM/DMEM sizes */
    gsp.hwcfg2 = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_HWCFG2);

    if (gsp.hwcfg2 != NV_DEAD_REG && gsp.hwcfg2 != 0) {
        gsp.imem_size = (gsp.hwcfg2 & NV_FALCON_HWCFG2_IMEM_MASK) * 256;
        gsp.dmem_size = ((gsp.hwcfg2 & NV_FALCON_HWCFG2_DMEM_MASK)
                         >> NV_FALCON_HWCFG2_DMEM_SHIFT) * 256;
    }

    serial_puts("[GSP]   HWCFG=0x");
    serial_puthex(gsp.hwcfg, 8);
    serial_puts(" HWCFG2=0x");
    serial_puthex(gsp.hwcfg2, 8);
    serial_puts("\n");

    serial_puts("[GSP]   IMEM=");
    serial_putdec(gsp.imem_size / 1024);
    serial_puts("KB, DMEM=");
    serial_putdec(gsp.dmem_size / 1024);
    serial_puts("KB\n");

    /* CPUCTL — halted/stopped status */
    gsp.cpuctl = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_CPUCTL);
    gsp.halted  = (gsp.cpuctl & NV_FALCON_CPUCTL_HALTED)  ? true : false;
    gsp.stopped = (gsp.cpuctl & NV_FALCON_CPUCTL_STOPPED) ? true : false;

    serial_puts("[GSP]   CPUCTL=0x");
    serial_puthex(gsp.cpuctl, 8);
    serial_puts(" (");
    if (gsp.halted)  serial_puts("HALTED");
    if (gsp.halted && gsp.stopped) serial_puts("+");
    if (gsp.stopped) serial_puts("STOPPED");
    if (!gsp.halted && !gsp.stopped) serial_puts("RUNNING");
    serial_puts(")\n");

    /* Mailboxes */
    gsp.mailbox0 = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX0);
    gsp.mailbox1 = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX1);

    serial_puts("[GSP]   Mailbox0=0x");
    serial_puthex(gsp.mailbox0, 8);
    serial_puts(" Mailbox1=0x");
    serial_puthex(gsp.mailbox1, 8);
    serial_puts("\n");

    /* Framebuffer summary */
    fb_puts("\n GSP: IMEM=");
    fb_putdec(gsp.imem_size / 1024);
    fb_puts("KB DMEM=");
    fb_putdec(gsp.dmem_size / 1024);
    fb_puts("KB ");
    fb_puts(gsp.halted ? "HALTED" : "RUNNING");
    fb_puts("\n");

    return 0;
}

/* ── Load firmware from OsitoFS ──────────────────────────────── */

static int gsp_load_file(void)
{
    if (!osfs2_is_mounted()) {
        serial_puts("[GSP] OsitoFS not mounted, cannot load firmware\n");
        return -1;
    }

    osfs2_file_t *file = osfs2_find("gsp.bin");
    if (!file) {
        serial_puts("[GSP] gsp.bin not found in OsitoFS\n");
        return -1;
    }

    /* Validate size: min 1MB, max 128MB */
    if (file->size < 1024 * 1024) {
        serial_puts("[GSP] gsp.bin too small (");
        serial_putdec(file->size);
        serial_puts(" bytes, min 1MB)\n");
        return -1;
    }
    if (file->size > 128ULL * 1024 * 1024) {
        serial_puts("[GSP] gsp.bin too large (");
        serial_putdec(file->size / (1024 * 1024));
        serial_puts(" MB, max 128MB)\n");
        return -1;
    }

    serial_puts("[GSP] Loading 'gsp.bin' (");
    serial_putdec(file->size / (1024 * 1024));
    serial_puts(" MB)...\n");

    fb_puts(" GSP FW: ");
    fb_putdec(file->size / (1024 * 1024));
    fb_puts(" MB...\n");

    /* Allocate contiguous RAM */
    gsp.fw_data = mem_alloc_aligned(file->size, 4096);
    if (!gsp.fw_data) {
        serial_puts("[GSP] Failed to allocate ");
        serial_putdec(file->size / (1024 * 1024));
        serial_puts(" MB\n");
        return -1;
    }

    gsp.fw_size = file->size;

    /* Read file data in 1MB chunks */
    uint64_t offset = 0;
    uint64_t remaining = file->size;

    while (remaining > 0) {
        uint64_t chunk = remaining < OSFS2_BLOCK_SIZE ? remaining : OSFS2_BLOCK_SIZE;
        if (osfs2_read(file, offset, (uint8_t *)gsp.fw_data + offset, chunk) < 0) {
            serial_puts("[GSP] Read failed at offset ");
            serial_puthex(offset, 16);
            serial_puts("\n");
            return -1;
        }
        offset += chunk;
        remaining -= chunk;

        /* Progress every 16MB */
        if ((offset % (16 * 1024 * 1024)) == 0 || remaining == 0) {
            serial_puts("[GSP] Reading... ");
            serial_putdec(offset / (1024 * 1024));
            serial_puts("/");
            serial_putdec(file->size / (1024 * 1024));
            serial_puts(" MB\n");
        }
    }

    /* Check ELF magic (informative, not fatal) */
    uint8_t *hdr = (uint8_t *)gsp.fw_data;
    if (hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
        serial_puts("[GSP] Firmware is ELF binary (expected)\n");
    } else {
        serial_puts("[GSP] Firmware header: ");
        serial_puthex(hdr[0], 2); serial_puts(" ");
        serial_puthex(hdr[1], 2); serial_puts(" ");
        serial_puthex(hdr[2], 2); serial_puts(" ");
        serial_puthex(hdr[3], 2);
        serial_puts(" (not ELF)\n");
    }

    serial_puts("[GSP] Firmware loaded to RAM at 0x");
    serial_puthex((uint64_t)gsp.fw_data, 16);
    serial_puts("\n");

    gsp.fw_loaded = true;
    return 0;
}

/* ── Upload firmware to VRAM via PRAMIN window slide ─────────── */

static int gsp_upload_to_vram(void)
{
    gpu_probe_t *p = gpu_get_probe();

    /* Need PRAMIN R/W and enough VRAM */
    if (!p || !p->pramin_rw_ok) {
        serial_puts("[GSP] PRAMIN not writable, skipping VRAM upload\n");
        return -1;
    }

    uint64_t vram_bytes = (uint64_t)p->vram_size_mb * 1024 * 1024;
    uint64_t fw_vram_offset = (uint64_t)GSP_FW_VRAM_OFFSET_MB * 1024 * 1024;

    if (vram_bytes < fw_vram_offset + gsp.fw_size) {
        serial_puts("[GSP] Not enough VRAM (need ");
        serial_putdec((fw_vram_offset + gsp.fw_size) / (1024 * 1024));
        serial_puts(" MB, have ");
        serial_putdec(p->vram_size_mb);
        serial_puts(" MB)\n");
        return -1;
    }

    gsp.vram_offset = fw_vram_offset;

    serial_puts("[GSP] Uploading ");
    serial_putdec(gsp.fw_size / (1024 * 1024));
    serial_puts(" MB to VRAM+");
    serial_putdec(GSP_FW_VRAM_OFFSET_MB);
    serial_puts("MB via PRAMIN...\n");

    /* Save original window position */
    uint32_t orig_window = gpu_reg_read(NV_PBUS_BAR0_WINDOW);

    uint64_t t0 = rdtsc();

    /* Upload firmware in 1MB PRAMIN window chunks */
    uint64_t uploaded = 0;
    uint32_t *src = (uint32_t *)gsp.fw_data;

    while (uploaded < gsp.fw_size) {
        /* Slide window to current VRAM offset */
        uint64_t vram_addr = fw_vram_offset + uploaded;
        uint32_t window_val = (uint32_t)(vram_addr >> 16);
        gpu_reg_write(NV_PBUS_BAR0_WINDOW, window_val);
        wmb();

        /* Write up to 1MB through PRAMIN */
        uint64_t chunk = gsp.fw_size - uploaded;
        if (chunk > NV_PRAMIN_SIZE)
            chunk = NV_PRAMIN_SIZE;

        uint32_t dwords = (uint32_t)(chunk / 4);
        uint32_t pramin_off = 0;

        for (uint32_t i = 0; i < dwords; i++) {
            gpu_reg_write(NV_PRAMIN_BASE + pramin_off, src[uploaded / 4 + i]);
            pramin_off += 4;
        }
        wmb();

        uploaded += chunk;

        /* Progress every 4MB */
        if ((uploaded % (4 * 1024 * 1024)) == 0 || uploaded >= gsp.fw_size) {
            serial_puts("[GSP] Uploaded ");
            serial_putdec(uploaded / (1024 * 1024));
            serial_puts("/");
            serial_putdec(gsp.fw_size / (1024 * 1024));
            serial_puts(" MB\n");
        }
    }

    uint64_t t1 = rdtsc();
    uint64_t cycles = t1 - t0;

    /* Verify: read back first 4 dwords from VRAM */
    uint32_t verify_window = (uint32_t)(fw_vram_offset >> 16);
    gpu_reg_write(NV_PBUS_BAR0_WINDOW, verify_window);
    wmb();
    rmb();

    uint32_t *fw32 = (uint32_t *)gsp.fw_data;
    uint32_t v0 = gpu_reg_read(NV_PRAMIN_BASE + 0x00);
    uint32_t v1 = gpu_reg_read(NV_PRAMIN_BASE + 0x04);
    uint32_t v2 = gpu_reg_read(NV_PRAMIN_BASE + 0x08);
    uint32_t v3 = gpu_reg_read(NV_PRAMIN_BASE + 0x0C);

    serial_puts("[GSP] Verify VRAM[0..3]: ");
    serial_puthex(v0, 8); serial_puts(" ");
    serial_puthex(v1, 8); serial_puts(" ");
    serial_puthex(v2, 8); serial_puts(" ");
    serial_puthex(v3, 8);

    bool verify_ok = (v0 == fw32[0] && v1 == fw32[1] &&
                      v2 == fw32[2] && v3 == fw32[3]);

    if (verify_ok) {
        serial_puts(" OK\n");
    } else {
        serial_puts(" FAIL (expected ");
        serial_puthex(fw32[0], 8); serial_puts(" ");
        serial_puthex(fw32[1], 8); serial_puts(" ");
        serial_puthex(fw32[2], 8); serial_puts(" ");
        serial_puthex(fw32[3], 8);
        serial_puts(")\n");
    }

    /* Restore original window position */
    gpu_reg_write(NV_PBUS_BAR0_WINDOW, orig_window);
    wmb();

    /* Report timing */
    uint64_t ms_est = cycles / 3000000;  /* estimate @ 3GHz */
    serial_puts("[GSP] Upload complete: ~");
    serial_putdec(ms_est);
    serial_puts(" ms (");
    serial_putdec(cycles / 1000000);
    serial_puts("M cycles)\n");

    fb_puts(" GSP FW uploaded to VRAM");
    fb_puts(verify_ok ? " OK\n" : " VERIFY FAIL\n");

    gsp.fw_uploaded = verify_ok;
    return verify_ok ? 0 : -1;
}

/* ── Public API: Load firmware ───────────────────────────────── */

int gsp_load_firmware(void)
{
    if (gsp_load_file() < 0)
        return -1;

    /* Upload to VRAM if GPU is present with PRAMIN */
    gpu_probe_t *p = gpu_get_probe();
    if (p && p->present && p->pramin_rw_ok) {
        gsp_upload_to_vram();
    } else {
        serial_puts("[GSP] No GPU PRAMIN — firmware stays in RAM only\n");
    }

    return 0;
}

/* ── ELF64 Parser (firmware in RAM) ──────────────────────────── */

static int gsp_parse_elf(void)
{
    if (!gsp.fw_data || gsp.fw_size < sizeof(elf64_ehdr_t)) {
        serial_puts("[GSP] No firmware data for ELF parse\n");
        return -1;
    }

    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)gsp.fw_data;

    /* Validate ELF magic */
    uint32_t magic = *(uint32_t *)ehdr->e_ident;
    if (magic != ELF_MAGIC) {
        serial_puts("[GSP] Not an ELF binary (magic=0x");
        serial_puthex(magic, 8);
        serial_puts(")\n");
        return -1;
    }

    /* Validate ELF64 little-endian */
    if (ehdr->e_ident[4] != ELFCLASS64) {
        serial_puts("[GSP] Not ELF64 (class=");
        serial_putdec(ehdr->e_ident[4]);
        serial_puts(")\n");
        return -1;
    }
    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        serial_puts("[GSP] Not little-endian (data=");
        serial_putdec(ehdr->e_ident[5]);
        serial_puts(")\n");
        return -1;
    }

    /* Extract key fields */
    gsp.elf_entry   = ehdr->e_entry;
    gsp.elf_phnum   = ehdr->e_phnum;
    gsp.elf_machine = ehdr->e_machine;

    serial_puts("[GSP] ELF64: machine=0x");
    serial_puthex(ehdr->e_machine, 4);
    serial_puts(ehdr->e_machine == 0xF3 ? " (RISC-V)" : "");
    serial_puts(", entry=0x");
    serial_puthex(ehdr->e_entry, 16);
    serial_puts("\n");

    serial_puts("[GSP] ELF64: ");
    serial_putdec(ehdr->e_phnum);
    serial_puts(" program headers\n");

    /* Iterate program headers — informative only */
    if (ehdr->e_phoff && ehdr->e_phnum &&
        ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(elf64_phdr_t) <= gsp.fw_size) {

        elf64_phdr_t *phdr = (elf64_phdr_t *)((uint8_t *)gsp.fw_data + ehdr->e_phoff);

        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].p_type == PT_LOAD) {
                serial_puts("[GSP]   PT_LOAD: vaddr=0x");
                serial_puthex(phdr[i].p_vaddr, 16);
                serial_puts(" filesz=");
                serial_putdec(phdr[i].p_filesz);
                serial_puts(" memsz=");
                serial_putdec(phdr[i].p_memsz);
                serial_puts("\n");
            }
        }
    }

    return 0;
}

/* ── Phase 5: GSP Falcon Boot ────────────────────────────────── */

int gsp_boot(void)
{
    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present || !p->gsp_present) {
        return -1;   /* No GPU/GSP — silent */
    }

    /* Parse ELF from firmware in RAM */
    if (gsp_parse_elf() < 0) {
        serial_puts("[GSP] ELF parse failed, skipping boot\n");
        return -1;
    }

    /* Pre-conditions */
    if (!gsp.fw_uploaded) {
        serial_puts("[GSP] Firmware not in VRAM, skipping boot\n");
        return -1;
    }
    if (gsp.elf_entry == 0) {
        serial_puts("[GSP] ELF entry point is 0, skipping boot\n");
        return -1;
    }

    /* ── Step 1: Halt Falcon ── */
    serial_puts("[GSP] Boot: halting Falcon...\n");
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_HALTED);
    wmb();

    /* ── Step 2: Clear mailboxes ── */
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_MAILBOX0, 0);
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_MAILBOX1, 0);
    wmb();

    /* ── Step 3: Set DMATRFBASE (firmware location in VRAM, >> 8) ── */
    uint32_t dma_base = (uint32_t)(gsp.vram_offset >> 8);
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_DMATRFBASE, dma_base);
    wmb();

    /* ── Step 4: Set BOOTVEC (entry point >> 8) ── */
    uint32_t bootvec = (uint32_t)(gsp.elf_entry >> 8);
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_BOOTVEC, bootvec);
    wmb();

    serial_puts("[GSP] Boot: DMATRFBASE=0x");
    serial_puthex(dma_base, 8);
    serial_puts(" BOOTVEC=0x");
    serial_puthex(bootvec, 8);
    serial_puts("\n");

    /* ── Step 5: Start CPU ── */
    serial_puts("[GSP] Boot: starting CPU...\n");
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_STARTCPU);
    wmb();
    gsp.booted = true;

    /* ── Step 6: Poll mailbox (timeout ~1 second @ 3GHz) ── */
    serial_puts("[GSP] Boot: polling mailbox (timeout 1s)...\n");

    uint64_t t0 = rdtsc();
    uint64_t timeout_cycles = 3000000000ULL;  /* ~1s @ 3GHz */
    uint32_t mbox0 = 0;

    while (1) {
        rmb();
        mbox0 = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX0);
        if (mbox0 != 0)
            break;

        uint64_t elapsed = rdtsc() - t0;
        if (elapsed >= timeout_cycles)
            break;
    }

    uint64_t t1 = rdtsc();
    uint64_t elapsed_cycles = t1 - t0;
    uint64_t elapsed_ms = elapsed_cycles / 3000000;

    gsp.boot_status = mbox0;
    gsp.boot_ack = (mbox0 != 0);

    if (gsp.boot_ack) {
        /* Success — GSP responded */
        serial_puts("[GSP] Boot: mailbox0=0x");
        serial_puthex(mbox0, 8);
        serial_puts(" after ~");
        serial_putdec(elapsed_ms);
        serial_puts(" ms (");
        serial_putdec(elapsed_cycles / 1000000);
        serial_puts("M cycles)\n");

        uint32_t cpuctl = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_CPUCTL);
        serial_puts("[GSP] Boot: GSP alive! CPUCTL=0x");
        serial_puthex(cpuctl, 8);
        serial_puts("\n");

        fb_puts(" GSP: boot OK, mailbox=0x");
        fb_puthex(mbox0, 8);
        fb_puts("\n");
    } else {
        /* Timeout — dump diagnostics */
        serial_puts("[GSP] Boot: TIMEOUT after ");
        serial_putdec(elapsed_ms);
        serial_puts(" ms\n");

        uint32_t cpuctl = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_CPUCTL);
        uint32_t mbox1  = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX1);

        serial_puts("[GSP] Boot: CPUCTL=0x");
        serial_puthex(cpuctl, 8);
        serial_puts(" (");
        if (cpuctl & NV_FALCON_CPUCTL_HALTED)  serial_puts("HALTED");
        if ((cpuctl & NV_FALCON_CPUCTL_HALTED) &&
            (cpuctl & NV_FALCON_CPUCTL_STOPPED)) serial_puts("+");
        if (cpuctl & NV_FALCON_CPUCTL_STOPPED) serial_puts("STOPPED");
        if (!(cpuctl & (NV_FALCON_CPUCTL_HALTED | NV_FALCON_CPUCTL_STOPPED)))
            serial_puts("RUNNING");
        serial_puts(") Mailbox0=0x");
        serial_puthex(mbox0, 8);
        serial_puts(" Mailbox1=0x");
        serial_puthex(mbox1, 8);
        serial_puts("\n");

        serial_puts("[GSP] Boot: GSP did not respond — firmware may require additional init\n");

        fb_puts(" GSP: boot timeout\n");
    }

    return gsp.boot_ack ? 0 : -1;
}

gsp_state_t *gsp_get_state(void)
{
    return &gsp;
}
