/*
 * OsitoK x86-64 — GSP Falcon Driver (Phase 4-7)
 *
 * Phase 4: Deep probe (HWCFG2, CPUCTL, mailboxes), firmware load + VRAM upload.
 * Phase 5: ELF64 parse, boot sequence (BOOTVEC, CPUCTL start), mailbox handshake.
 * Phase 6: Shared memory message queues (host↔GSP bidirectional).
 * Phase 7: RPC protocol (function IDs, poll with timeout, init sequence).
 * Phase 8: RM init commands (SET_SYSTEM_INFO, ALLOC_ROOT, etc.).
 *
 * Reference: envytools (https://envytools.rtfd.io), nouveau driver,
 *            NVIDIA open-gpu-kernel-modules (rpc_global_enums.h).
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
    uint64_t cnt;
    __asm__ volatile ("mrs %0, CNTPCT_EL0" : "=r"(cnt));
    return cnt;
}

/* ── Driver State ────────────────────────────────────────────── */

static gsp_state_t gsp;

/* X29 state (forward declarations — defined fully in X29 section) */
static radix3_state_t radix3;
static rm_riscv_ucode_desc_t bl_desc;
static uint8_t *bl_data;
static uint32_t bl_size;

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

/* ── Phase 6: Message Queues ─────────────────────────────────── */

int gsp_queue_init(void)
{
    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present || !p->gsp_present) {
        return -1;
    }

    /* Allocate 513KB shared memory, page-aligned */
    gsp.shm_base = mem_alloc_aligned(GSP_SHM_TOTAL_SIZE, GSP_PAGE_SIZE);
    if (!gsp.shm_base) {
        serial_puts("[GSP] Failed to allocate shared memory (513KB)\n");
        return -1;
    }

    /* Identity-mapped physical address */
    gsp.shm_phys = (uint64_t)gsp.shm_base;

    /* Zero entire region */
    memset(gsp.shm_base, 0, GSP_SHM_TOTAL_SIZE);

    uint8_t *base = (uint8_t *)gsp.shm_base;

    /* ── CPU queue TX header (host→GSP) at offset 0x1000 ── */
    gsp_msgq_tx_hdr_t *cpu_tx = (gsp_msgq_tx_hdr_t *)(base + GSP_SHM_CPUQ_HDR_OFF);
    cpu_tx->version  = 1;
    cpu_tx->size     = GSP_MSGQ_NUM_PAGES * GSP_PAGE_SIZE;
    cpu_tx->msgSize  = GSP_PAGE_SIZE;
    cpu_tx->msgCount = GSP_MSGQ_NUM_PAGES;
    cpu_tx->writePtr = 0;
    cpu_tx->flags    = 0;
    cpu_tx->rxHdrOff = sizeof(gsp_msgq_tx_hdr_t);   /* RX header follows TX */
    cpu_tx->entryOff = GSP_SHM_CPUQ_DATA_OFF - GSP_SHM_CPUQ_HDR_OFF;

    /* CPU queue RX header (GSP's read pointer for this queue) */
    gsp_msgq_rx_hdr_t *cpu_rx = (gsp_msgq_rx_hdr_t *)(base + GSP_SHM_CPUQ_HDR_OFF
                                                        + sizeof(gsp_msgq_tx_hdr_t));
    cpu_rx->readPtr = 0;

    /* ── GSP queue TX header (GSP→host) at offset 0x41000 ── */
    gsp_msgq_tx_hdr_t *gsp_tx = (gsp_msgq_tx_hdr_t *)(base + GSP_SHM_GSPQ_HDR_OFF);
    gsp_tx->version  = 1;
    gsp_tx->size     = GSP_MSGQ_NUM_PAGES * GSP_PAGE_SIZE;
    gsp_tx->msgSize  = GSP_PAGE_SIZE;
    gsp_tx->msgCount = GSP_MSGQ_NUM_PAGES;
    gsp_tx->writePtr = 0;
    gsp_tx->flags    = 0;
    gsp_tx->rxHdrOff = sizeof(gsp_msgq_tx_hdr_t);
    gsp_tx->entryOff = GSP_SHM_GSPQ_DATA_OFF - GSP_SHM_GSPQ_HDR_OFF;

    /* GSP queue RX header (our read pointer) */
    gsp_msgq_rx_hdr_t *gsp_rx = (gsp_msgq_rx_hdr_t *)(base + GSP_SHM_GSPQ_HDR_OFF
                                                        + sizeof(gsp_msgq_tx_hdr_t));
    gsp_rx->readPtr = 0;

    gsp.cmd_seq = 0;
    gsp.rpc_seq = 0;
    gsp.queues_ready = true;

    serial_puts("[GSP] Message queues: 513KB at 0x");
    serial_puthex(gsp.shm_phys, 16);
    serial_puts(" (cpuq@+0x1000, gspq@+0x41000)\n");

    fb_puts(" GSP queues: 513KB OK\n");

    return 0;
}

/* Write queue init args to VRAM via PRAMIN so GSP finds them at boot */
static int gsp_queue_write_args(void)
{
    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->pramin_rw_ok) {
        serial_puts("[GSP] PRAMIN not writable, cannot write queue args\n");
        return -1;
    }

    gsp_msgq_init_args_t args;
    memset(&args, 0, sizeof(args));
    args.sharedMemPhysAddr   = gsp.shm_phys;
    args.pageTableEntryCount = 512;    /* PTEs in first page */
    args.cmdQueueOffset      = GSP_SHM_CPUQ_HDR_OFF;
    args.statQueueOffset     = GSP_SHM_GSPQ_HDR_OFF;

    /* Write to VRAM+126MB via PRAMIN window */
    uint64_t vram_off = (uint64_t)GSP_QUEUE_ARGS_VRAM_MB * 1024 * 1024;
    uint32_t orig_window = gpu_reg_read(NV_PBUS_BAR0_WINDOW);

    uint32_t window_val = (uint32_t)(vram_off >> 16);
    gpu_reg_write(NV_PBUS_BAR0_WINDOW, window_val);
    wmb();

    /* Write args structure as dwords */
    uint32_t *src = (uint32_t *)&args;
    uint32_t dwords = sizeof(args) / 4;
    for (uint32_t i = 0; i < dwords; i++) {
        gpu_reg_write(NV_PRAMIN_BASE + (i * 4), src[i]);
    }
    wmb();

    /* Restore window */
    gpu_reg_write(NV_PBUS_BAR0_WINDOW, orig_window);
    wmb();

    serial_puts("[GSP] Queue init args written to VRAM+");
    serial_putdec(GSP_QUEUE_ARGS_VRAM_MB);
    serial_puts("MB\n");

    return 0;
}

static uint32_t gsp_xor_checksum(const uint32_t *data, uint32_t dwords)
{
    uint32_t xor = 0;
    for (uint32_t i = 0; i < dwords; i++)
        xor ^= data[i];
    return xor;
}

int gsp_queue_send(uint32_t function, const void *payload, uint32_t len)
{
    if (!gsp.queues_ready) {
        serial_puts("[GSP] TX: queues not ready\n");
        return -1;
    }

    uint8_t *base = (uint8_t *)gsp.shm_base;

    /* Read current write pointer and consumer's read pointer */
    gsp_msgq_tx_hdr_t *tx_hdr = (gsp_msgq_tx_hdr_t *)(base + GSP_SHM_CPUQ_HDR_OFF);
    gsp_msgq_rx_hdr_t *rx_hdr = (gsp_msgq_rx_hdr_t *)(base + GSP_SHM_CPUQ_HDR_OFF
                                                        + sizeof(gsp_msgq_tx_hdr_t));

    uint32_t wp = tx_hdr->writePtr;
    uint32_t rp = rx_hdr->readPtr;

    /* Check if queue is full */
    uint32_t next_wp = (wp + 1) % GSP_MSGQ_NUM_PAGES;
    if (next_wp == rp) {
        serial_puts("[GSP] TX: queue full\n");
        return -1;
    }

    /* Payload must fit in one page minus headers */
    uint32_t max_payload = GSP_PAGE_SIZE - sizeof(gsp_msg_elem_hdr_t)
                           - sizeof(gsp_rpc_hdr_t);
    if (len > max_payload) {
        serial_puts("[GSP] TX: payload too large (");
        serial_putdec(len);
        serial_puts(" > ");
        serial_putdec(max_payload);
        serial_puts(")\n");
        return -1;
    }

    /* Build message in queue entry */
    uint8_t *entry = base + GSP_SHM_CPUQ_DATA_OFF + (wp * GSP_PAGE_SIZE);
    memset(entry, 0, GSP_PAGE_SIZE);

    /* Element header */
    gsp_msg_elem_hdr_t *elem = (gsp_msg_elem_hdr_t *)entry;
    elem->seqNum    = gsp.cmd_seq++;
    elem->elemCount = 1;

    /* RPC header */
    gsp_rpc_hdr_t *rpc = (gsp_rpc_hdr_t *)(entry + sizeof(gsp_msg_elem_hdr_t));
    rpc->header_version = GSP_MSG_HDR_VERSION;
    rpc->signature      = GSP_MSG_SIGNATURE;
    rpc->length         = sizeof(gsp_rpc_hdr_t) + len;
    rpc->function       = function;
    rpc->rpc_result     = 0;
    rpc->sequence       = gsp.rpc_seq++;

    /* Copy payload */
    if (payload && len > 0) {
        uint8_t *dst = entry + sizeof(gsp_msg_elem_hdr_t) + sizeof(gsp_rpc_hdr_t);
        const uint8_t *src = (const uint8_t *)payload;
        for (uint32_t i = 0; i < len; i++)
            dst[i] = src[i];
    }

    /* XOR checksum over entire page (checksum field set so total XOR = 0) */
    elem->checkSum = 0;
    uint32_t xor = gsp_xor_checksum((uint32_t *)entry,
                                     GSP_PAGE_SIZE / sizeof(uint32_t));
    elem->checkSum = xor;  /* Now total XOR of page = 0 */

    /* Advance write pointer */
    wmb();
    tx_hdr->writePtr = next_wp;
    wmb();

    /* Ring doorbell */
    gpu_reg_write(NV_PGSP_QUEUE_HEAD, 0);

    serial_puts("[GSP] TX: func=0x");
    serial_puthex(function, 8);
    serial_puts(" seq=");
    serial_putdec(rpc->sequence);
    serial_puts(" len=");
    serial_putdec(len);
    serial_puts("\n");

    return 0;
}

int gsp_queue_recv(void *buf, uint32_t buf_size,
                   uint32_t *function, uint32_t *rpc_result)
{
    if (!gsp.queues_ready) {
        return -1;
    }

    uint8_t *base = (uint8_t *)gsp.shm_base;

    /* Read GSP's write pointer and our read pointer */
    gsp_msgq_tx_hdr_t *tx_hdr = (gsp_msgq_tx_hdr_t *)(base + GSP_SHM_GSPQ_HDR_OFF);
    gsp_msgq_rx_hdr_t *rx_hdr = (gsp_msgq_rx_hdr_t *)(base + GSP_SHM_GSPQ_HDR_OFF
                                                        + sizeof(gsp_msgq_tx_hdr_t));

    rmb();
    uint32_t wp = tx_hdr->writePtr;
    uint32_t rp = rx_hdr->readPtr;

    /* Empty? */
    if (wp == rp) {
        return -1;
    }

    rmb();

    /* Read entry */
    uint8_t *entry = base + GSP_SHM_GSPQ_DATA_OFF + (rp * GSP_PAGE_SIZE);

    /* Validate checksum */
    uint32_t xor = gsp_xor_checksum((uint32_t *)entry,
                                     GSP_PAGE_SIZE / sizeof(uint32_t));
    if (xor != 0) {
        serial_puts("[GSP] RX: checksum mismatch (xor=0x");
        serial_puthex(xor, 8);
        serial_puts(")\n");
    }

    /* Parse headers */
    gsp_msg_elem_hdr_t *elem = (gsp_msg_elem_hdr_t *)entry;
    gsp_rpc_hdr_t *rpc = (gsp_rpc_hdr_t *)(entry + sizeof(gsp_msg_elem_hdr_t));

    if (function)
        *function = rpc->function;
    if (rpc_result)
        *rpc_result = rpc->rpc_result;

    /* Copy payload to caller buffer */
    uint32_t payload_off = sizeof(gsp_msg_elem_hdr_t) + sizeof(gsp_rpc_hdr_t);
    uint32_t payload_len = 0;
    if (rpc->length > sizeof(gsp_rpc_hdr_t))
        payload_len = rpc->length - sizeof(gsp_rpc_hdr_t);
    if (payload_len > buf_size)
        payload_len = buf_size;

    if (buf && payload_len > 0) {
        uint8_t *src = entry + payload_off;
        uint8_t *dst = (uint8_t *)buf;
        for (uint32_t i = 0; i < payload_len; i++)
            dst[i] = src[i];
    }

    /* Advance read pointer */
    rx_hdr->readPtr = (rp + 1) % GSP_MSGQ_NUM_PAGES;

    serial_puts("[GSP] RX: func=0x");
    serial_puthex(rpc->function, 8);
    serial_puts(" result=0x");
    serial_puthex(rpc->rpc_result, 8);
    serial_puts(" seq=");
    serial_putdec(rpc->sequence);
    serial_puts("\n");

    (void)elem;
    return 0;
}

/* ── Phase 7: RPC Protocol ───────────────────────────────────── */

int gsp_rpc_poll(uint32_t *function, uint32_t *result,
                 void *buf, uint32_t buf_size, uint32_t timeout_ms)
{
    uint64_t t0 = rdtsc();
    uint64_t timeout_cycles = (uint64_t)timeout_ms * 3000000ULL;

    while (1) {
        uint32_t func, res;
        if (gsp_queue_recv(buf, buf_size, &func, &res) == 0) {
            if (function) *function = func;
            if (result)   *result   = res;
            return 0;
        }

        uint64_t elapsed = rdtsc() - t0;
        if (elapsed >= timeout_cycles)
            return -1;  /* Timeout */
    }
}

int gsp_rpc_init(void)
{
    if (!gsp.queues_ready) return -1;

    serial_puts("[GSP] RPC: waiting for INIT_DONE...\n");

    /* ── Step 1: Poll for GSP_INIT_DONE event ── */
    uint32_t func, result;
    if (gsp_rpc_poll(&func, &result, NULL, 0, 2000) == 0) {
        serial_puts("[GSP] RPC: received func=0x");
        serial_puthex(func, 8);
        if (func == GSP_EVENT_GSP_INIT_DONE) {
            serial_puts(" (INIT_DONE)\n");
            gsp.rpc_ready = true;
        } else {
            serial_puts(" (unexpected, expected INIT_DONE)\n");
        }
    } else {
        serial_puts("[GSP] RPC: INIT_DONE timeout (expected without full boot chain)\n");
        return -1;
    }

    /* ── Step 2: Send GET_GSP_STATIC_INFO ── */
    serial_puts("[GSP] RPC: sending GET_GSP_STATIC_INFO...\n");
    gsp_queue_send(GSP_RPC_GET_GSP_STATIC_INFO, NULL, 0);

    /* ── Step 3: Poll for response ── */
    uint8_t info_buf[256];
    if (gsp_rpc_poll(&func, &result, info_buf, sizeof(info_buf), 2000) == 0) {
        serial_puts("[GSP] RPC: response func=0x");
        serial_puthex(func, 8);
        serial_puts(" result=0x");
        serial_puthex(result, 8);
        serial_puts("\n");

        if (func == GSP_RPC_GET_GSP_STATIC_INFO &&
            result == GSP_RPC_RESULT_OK) {
            /* Parse GPU name from first 40 bytes of payload */
            gsp_static_info_t *info = (gsp_static_info_t *)info_buf;
            info->gpu_name[39] = '\0';
            for (int i = 0; i < 40; i++)
                gsp.gpu_name[i] = info->gpu_name[i];

            serial_puts("[GSP] GPU: ");
            serial_puts(gsp.gpu_name);
            serial_puts("\n");

            fb_puts(" GSP GPU: ");
            fb_puts(gsp.gpu_name);
            fb_puts("\n");
        }
    } else {
        serial_puts("[GSP] RPC: GET_GSP_STATIC_INFO timeout\n");
    }

    return gsp.rpc_ready ? 0 : -1;
}

/* ── Phase 8: GSP-RM Init Commands ───────────────────────────── */

extern gpu_device_t gpu_dev;  /* from pci.c */

static uint64_t pci_encode_bdf(uint8_t bus, uint8_t dev, uint8_t func)
{
    return ((uint64_t)bus << 8) | ((uint64_t)dev << 3) | func;
}

static void str_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Step 1: SET_SYSTEM_INFO (func 72) — fire-and-forget */
static void gsp_rm_send_system_info(void)
{
    gsp_system_info_t info;
    memset(&info, 0, sizeof(info));

    info.gpuPhysAddr           = gpu_dev.bar0_base;
    info.gpuPhysFbAddr         = gpu_dev.bar1_base;
    info.nvDomainBusDeviceFunc = pci_encode_bdf(gpu_dev.pci_bus,
                                                gpu_dev.pci_dev,
                                                gpu_dev.pci_func);
    info.maxUserVa             = (1ULL << 47) - 4096;
    info.pciConfigMirrorBase   = 0x088000;
    info.pciConfigMirrorSize   = 0x001000;
    info.PCIDeviceID           = ((uint32_t)gpu_dev.device_id << 16) |
                                 gpu_dev.vendor_id;

    serial_puts("[GSP-RM] SET_SYSTEM_INFO: BAR0=0x");
    serial_puthex(info.gpuPhysAddr, 16);
    serial_puts(" BAR1=0x");
    serial_puthex(info.gpuPhysFbAddr, 16);
    serial_puts(" BDF=0x");
    serial_puthex(info.nvDomainBusDeviceFunc, 4);
    serial_puts(" PCI=0x");
    serial_puthex(info.PCIDeviceID, 8);
    serial_puts("\n");

    gsp_queue_send(GSP_RPC_GSP_SET_SYSTEM_INFO, &info, sizeof(info));
}

/* Step 2: SET_REGISTRY (func 73) — fire-and-forget */
static void gsp_rm_send_registry(void)
{
    gsp_registry_table_t reg;
    memset(&reg, 0, sizeof(reg));

    reg.numEntries = 2;

    str_copy(reg.entries[0].name, "RMSecBusResetEnable", 64);
    reg.entries[0].type  = 1;  /* DWORD */
    reg.entries[0].len   = 4;
    reg.entries[0].value = 1;

    str_copy(reg.entries[1].name, "RMForcePcieConfigSave", 64);
    reg.entries[1].type  = 1;
    reg.entries[1].len   = 4;
    reg.entries[1].value = 1;

    gsp_queue_send(GSP_RPC_SET_REGISTRY, &reg, sizeof(reg));
}

/* Step 3: ALLOC_ROOT (func 2) — poll response */
static void gsp_rm_alloc_root(void)
{
    gsp_alloc_root_t alloc;
    memset(&alloc, 0, sizeof(alloc));

    alloc.hClient = GSP_RM_CLIENT_HANDLE;
    alloc.hClass  = 0x0000;  /* NV01_ROOT */

    gsp_queue_send(GSP_RPC_ALLOC_ROOT, &alloc, sizeof(alloc));

    uint32_t func, result;
    if (gsp_rpc_poll(&func, &result, NULL, 0, 2000) == 0) {
        serial_puts("[GSP-RM] ALLOC_ROOT response: func=0x");
        serial_puthex(func, 8);
        serial_puts(" result=0x");
        serial_puthex(result, 8);
        serial_puts("\n");
    } else {
        serial_puts("[GSP-RM] ALLOC_ROOT timeout (expected without full boot chain)\n");
    }
}

/* Step 4: ALLOC_DEVICE (func 3) — poll response */
static void gsp_rm_alloc_device(void)
{
    gsp_alloc_device_t alloc;
    memset(&alloc, 0, sizeof(alloc));

    alloc.hClient        = GSP_RM_CLIENT_HANDLE;
    alloc.hDevice        = GSP_RM_DEVICE_HANDLE;
    alloc.hClass         = 0x0080;  /* NV01_DEVICE */
    alloc.deviceInstance = 0;

    gsp_queue_send(GSP_RPC_ALLOC_DEVICE, &alloc, sizeof(alloc));

    uint32_t func, result;
    if (gsp_rpc_poll(&func, &result, NULL, 0, 2000) == 0) {
        serial_puts("[GSP-RM] ALLOC_DEVICE response: func=0x");
        serial_puthex(func, 8);
        serial_puts(" result=0x");
        serial_puthex(result, 8);
        serial_puts("\n");
    } else {
        serial_puts("[GSP-RM] ALLOC_DEVICE timeout (expected without full boot chain)\n");
    }
}

/* Step 5: INIT_POST_OBJGPU (func 74) — no payload, poll response */
static void gsp_rm_init_post_objgpu(void)
{
    gsp_queue_send(GSP_RPC_GSP_INIT_POST_OBJGPU, NULL, 0);

    uint32_t func, result;
    if (gsp_rpc_poll(&func, &result, NULL, 0, 2000) == 0) {
        serial_puts("[GSP-RM] INIT_POST_OBJGPU response: func=0x");
        serial_puthex(func, 8);
        serial_puts(" result=0x");
        serial_puthex(result, 8);
        serial_puts("\n");
    } else {
        serial_puts("[GSP-RM] INIT_POST_OBJGPU timeout (expected without full boot chain)\n");
    }
}

/* ── X32: Generic RM Alloc + Control ─────────────────────────── */

/* Generic RM_ALLOC (func 103). Builds rpc_rm_alloc_hdr_t + inline params.
 * Returns 0 on success response, -1 on timeout/error. */
int gsp_rm_alloc(uint32_t hParent, uint32_t hObject, uint32_t hClass,
                 const void *params, uint32_t params_size)
{
    /* Build message: 32-byte header + params_size inline */
    uint8_t buf[512];  /* Max: 32 + params (largest is ~48B VASPACE) */
    uint32_t total = sizeof(rpc_rm_alloc_hdr_t) + params_size;
    if (total > sizeof(buf)) return -1;

    memset(buf, 0, total);
    rpc_rm_alloc_hdr_t *hdr = (rpc_rm_alloc_hdr_t *)buf;
    hdr->hClient   = GSP_RM_CLIENT_HANDLE;
    hdr->hParent   = hParent;
    hdr->hObject   = hObject;
    hdr->hClass    = hClass;
    hdr->status    = 0;
    hdr->paramsSize = params_size;
    hdr->flags     = 0;

    if (params && params_size > 0)
        memcpy(buf + sizeof(rpc_rm_alloc_hdr_t), params, params_size);

    serial_puts("[GSP-RM] RM_ALLOC: class=0x");
    serial_puthex(hClass, 4);
    serial_puts(" parent=0x");
    serial_puthex(hParent, 8);
    serial_puts(" obj=0x");
    serial_puthex(hObject, 8);
    serial_puts(" params=");
    serial_putdec(params_size);
    serial_puts("B\n");

    gsp_queue_send(GSP_RPC_GSP_RM_ALLOC, buf, total);

    uint32_t func, result;
    if (gsp_rpc_poll(&func, &result, NULL, 0, 2000) == 0) {
        serial_puts("[GSP-RM] RM_ALLOC response: func=0x");
        serial_puthex(func, 8);
        serial_puts(" result=0x");
        serial_puthex(result, 8);
        serial_puts("\n");
        return (result == GSP_RPC_RESULT_OK) ? 0 : -1;
    }

    serial_puts("[GSP-RM] RM_ALLOC timeout (expected without full boot chain)\n");
    return -1;
}

/* Generic RM_CONTROL (func 76). Builds rpc_rm_ctrl_hdr_t + inline params.
 * Returns 0 on success response, -1 on timeout/error. */
int gsp_rm_control(uint32_t hObject, uint32_t cmd,
                   const void *params, uint32_t params_size)
{
    uint8_t buf[512];
    uint32_t total = sizeof(rpc_rm_ctrl_hdr_t) + params_size;
    if (total > sizeof(buf)) return -1;

    memset(buf, 0, total);
    rpc_rm_ctrl_hdr_t *hdr = (rpc_rm_ctrl_hdr_t *)buf;
    hdr->hClient   = GSP_RM_CLIENT_HANDLE;
    hdr->hObject   = hObject;
    hdr->cmd       = cmd;
    hdr->status    = 0;
    hdr->paramsSize = params_size;
    hdr->flags     = 0;

    if (params && params_size > 0)
        memcpy(buf + sizeof(rpc_rm_ctrl_hdr_t), params, params_size);

    serial_puts("[GSP-RM] RM_CONTROL: obj=0x");
    serial_puthex(hObject, 8);
    serial_puts(" cmd=0x");
    serial_puthex(cmd, 8);
    serial_puts(" params=");
    serial_putdec(params_size);
    serial_puts("B\n");

    gsp_queue_send(GSP_RPC_GSP_RM_CONTROL, buf, total);

    uint32_t func, result;
    if (gsp_rpc_poll(&func, &result, NULL, 0, 2000) == 0) {
        serial_puts("[GSP-RM] RM_CONTROL response: func=0x");
        serial_puthex(func, 8);
        serial_puts(" result=0x");
        serial_puthex(result, 8);
        serial_puts("\n");
        return (result == GSP_RPC_RESULT_OK) ? 0 : -1;
    }

    serial_puts("[GSP-RM] RM_CONTROL timeout (expected without full boot chain)\n");
    return -1;
}

/* Step 6: ALLOC_SUBDEVICE (class 0x2080) via generic RM_ALLOC */
static void gsp_rm_alloc_subdevice(void)
{
    nv2080_alloc_params_t params;
    memset(&params, 0, sizeof(params));
    params.subDeviceId = 0;

    gsp_rm_alloc(GSP_RM_DEVICE_HANDLE, GSP_RM_SUBDEVICE_HANDLE,
                 NV20_SUBDEVICE_0, &params, sizeof(params));
}

/* Step 7: ALLOC_VASPACE (class 0x90F1) via generic RM_ALLOC */
static void gsp_rm_alloc_vaspace(void)
{
    nv_vaspace_alloc_params_t params;
    memset(&params, 0, sizeof(params));
    params.index = NV_VASPACE_ALLOCATION_INDEX_GPU_NEW;
    params.flags = NV_VASPACE_ALLOCATION_FLAGS_IS_EXTERNALLY_OWNED;

    gsp_rm_alloc(GSP_RM_DEVICE_HANDLE, GSP_RM_VASPACE_HANDLE,
                 FERMI_VASPACE_A, &params, sizeof(params));
}

int gsp_rm_init(void)
{
    if (!gsp.queues_ready) {
        serial_puts("[GSP-RM] Queues not ready, skipping RM init\n");
        return -1;
    }

    serial_puts("[GSP-RM] === GSP-RM Init Sequence ===\n");

    serial_puts("[GSP-RM] Step 1/7: SET_SYSTEM_INFO\n");
    gsp_rm_send_system_info();

    serial_puts("[GSP-RM] Step 2/7: SET_REGISTRY\n");
    gsp_rm_send_registry();

    serial_puts("[GSP-RM] Step 3/7: ALLOC_ROOT\n");
    gsp_rm_alloc_root();

    serial_puts("[GSP-RM] Step 4/7: ALLOC_DEVICE\n");
    gsp_rm_alloc_device();

    serial_puts("[GSP-RM] Step 5/7: INIT_POST_OBJGPU\n");
    gsp_rm_init_post_objgpu();

    serial_puts("[GSP-RM] Step 6/7: ALLOC_SUBDEVICE\n");
    gsp_rm_alloc_subdevice();

    serial_puts("[GSP-RM] Step 7/7: ALLOC_VASPACE\n");
    gsp_rm_alloc_vaspace();

    gsp.rm_init_done = true;

    serial_puts("[GSP-RM] === Init sequence complete (7 steps) ===\n");

    fb_puts(" GSP-RM: init sequence done\n");

    return 0;
}

/* ══════════════════════════════════════════════════════════
 *  X33: Channel + GPFIFO
 *
 *  Allocate a GPU compute channel via RM:
 *  1. Allocate host memory (GPFIFO ring, instance mem, USERD)
 *  2. Allocate TSG (Thread Scheduling Group) via RM_ALLOC
 *  3. Allocate GPFIFO channel inside TSG via RM_ALLOC
 *
 *  The channel is the fundamental unit for pushing GPU commands.
 *  GPFIFO is a ring buffer of 8-byte entries, each pointing to a
 *  pushbuffer segment containing GPU method calls.
 *
 *  Reference: nouveau r535_chan_new, open-gpu-kernel-modules alloc_channel.h
 * ══════════════════════════════════════════════════════════ */

static channel_state_t channel;

channel_state_t *gsp_get_channel(void)
{
    return &channel;
}

/* Select channel GPFIFO class based on GPU generation.
 * Ada Lovelace reuses Ampere's class (confirmed by nouveau). */
static uint32_t channel_class_for_gen(gpu_gen_t gen)
{
    switch (gen) {
    case GPU_GEN_TURING:       return TURING_CHANNEL_GPFIFO_A;
    case GPU_GEN_AMPERE:       return AMPERE_CHANNEL_GPFIFO_A;
    case GPU_GEN_ADA_LOVELACE: return AMPERE_CHANNEL_GPFIFO_A;
    default:                   return AMPERE_CHANNEL_GPFIFO_A;
    }
}

int gsp_channel_init(void)
{
    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present) return -1;
    if (!gsp.rm_init_done) {
        serial_puts("[CHAN] RM init not done, skipping channel setup\n");
        return -1;
    }

    serial_puts("[CHAN] === Channel + GPFIFO Init ===\n");

    memset(&channel, 0, sizeof(channel));

    /* ── Step 1: Allocate host-side memory ── */

    /* GPFIFO ring: 512 entries × 8B = 4KB, page-aligned */
    channel.gpfifo_entries = GPFIFO_ENTRY_COUNT;
    channel.gpfifo = (gpfifo_entry_t *)mem_alloc_aligned(
        channel.gpfifo_entries * sizeof(gpfifo_entry_t), 4096);
    if (!channel.gpfifo) {
        serial_puts("[CHAN] Failed to allocate GPFIFO ring\n");
        return -1;
    }
    memset(channel.gpfifo, 0, channel.gpfifo_entries * sizeof(gpfifo_entry_t));
    channel.gpfifo_phys = (uint64_t)(uintptr_t)channel.gpfifo;
    channel.gp_put = 0;

    /* Instance memory (RAMFC): 4KB page */
    channel.inst_mem = mem_alloc_aligned(4096, 4096);
    if (!channel.inst_mem) {
        serial_puts("[CHAN] Failed to allocate instance memory\n");
        return -1;
    }
    memset(channel.inst_mem, 0, 4096);
    channel.inst_phys = (uint64_t)(uintptr_t)channel.inst_mem;

    /* USERD (user submit data): 4KB page */
    channel.userd_mem = mem_alloc_aligned(4096, 4096);
    if (!channel.userd_mem) {
        serial_puts("[CHAN] Failed to allocate USERD memory\n");
        return -1;
    }
    memset(channel.userd_mem, 0, 4096);
    channel.userd_phys = (uint64_t)(uintptr_t)channel.userd_mem;

    serial_puts("[CHAN] GPFIFO=0x");
    serial_puthex(channel.gpfifo_phys, 16);
    serial_puts(" inst=0x");
    serial_puthex(channel.inst_phys, 16);
    serial_puts(" userd=0x");
    serial_puthex(channel.userd_phys, 16);
    serial_puts("\n");

    /* ── Step 2: Allocate TSG (channel group) ── */

    channel.tsg_handle = GSP_RM_TSG_HANDLE;

    nv_tsg_alloc_params_t tsg_params;
    memset(&tsg_params, 0, sizeof(tsg_params));
    tsg_params.hVASpace    = GSP_RM_VASPACE_HANDLE;
    tsg_params.engineType  = NV2080_ENGINE_TYPE_GR0;

    serial_puts("[CHAN] Step 1/2: ALLOC_TSG (0xA06C)\n");
    gsp_rm_alloc(GSP_RM_DEVICE_HANDLE, channel.tsg_handle,
                 KEPLER_CHANNEL_GROUP_A, &tsg_params, sizeof(tsg_params));

    /* ── Step 3: Allocate channel GPFIFO ── */

    gpu_device_t *dev = &gpu_dev;
    channel.chan_class = channel_class_for_gen(dev->generation);
    channel.chan_handle = GSP_RM_CHAN_HANDLE;

    nv_chan_alloc_params_t chan_params;
    memset(&chan_params, 0, sizeof(chan_params));

    chan_params.gpFifoOffset  = channel.gpfifo_phys;
    chan_params.gpFifoEntries = channel.gpfifo_entries;
    chan_params.flags         = NVOS04_FLAGS_CHANNEL_TYPE_PHYSICAL
                              | NVOS04_FLAGS_PRIVILEGED_CHANNEL;
    chan_params.hVASpace      = GSP_RM_VASPACE_HANDLE;
    chan_params.engineType    = NV2080_ENGINE_TYPE_GR0;

    /* Instance memory — system RAM, cached (matches nouveau r535) */
    chan_params.instanceMem.base         = channel.inst_phys;
    chan_params.instanceMem.size         = 0x200;
    chan_params.instanceMem.addressSpace = ADDR_SYSMEM;
    chan_params.instanceMem.cacheAttrib  = NV_MEMORY_CACHED;

    /* USERD — system RAM, cached */
    chan_params.userdMem.base         = channel.userd_phys;
    chan_params.userdMem.size         = 4096;
    chan_params.userdMem.addressSpace = ADDR_SYSMEM;
    chan_params.userdMem.cacheAttrib  = NV_MEMORY_CACHED;

    /* RAMFC — same as instance memory */
    chan_params.ramfcMem.base         = channel.inst_phys;
    chan_params.ramfcMem.size         = 0x200;
    chan_params.ramfcMem.addressSpace = ADDR_SYSMEM;
    chan_params.ramfcMem.cacheAttrib  = NV_MEMORY_CACHED;

    serial_puts("[CHAN] Step 2/2: ALLOC_CHANNEL (class 0x");
    serial_puthex(channel.chan_class, 4);
    serial_puts(")\n");

    int ret = gsp_rm_alloc(channel.tsg_handle, channel.chan_handle,
                           channel.chan_class, &chan_params, sizeof(chan_params));

    channel.allocated = (ret == 0);

    serial_puts("[CHAN] Channel ");
    serial_puts(channel.allocated ? "allocated" : "allocation pending (expected without full boot)");
    serial_puts("\n");

    serial_puts("[CHAN] === Channel init complete ===\n");
    fb_puts(" Channel+GPFIFO: init done\n");

    return 0;
}

/* ══════════════════════════════════════════════════════════
 *  X34: Compute Class Bind + Kernel Dispatch
 *
 *  Binds compute class to GPFIFO channel (subchannel 1),
 *  activates channel via NVA06F_CTRL_BIND + GPFIFO_SCHEDULE,
 *  pushes SET_OBJECT + INVALIDATE_SHADER_CACHES + WAIT_FOR_IDLE
 *  as initial compute barrier / smoke test.
 *
 *  Reference: NVIDIA open-gpu-doc dev_ram.ref.txt (pushbuffer format),
 *             clc5c0.h/clc6c0.h/clc9c0.h (compute methods),
 *             ctrla06f (channel control commands).
 * ══════════════════════════════════════════════════════════ */

static compute_state_t compute;

compute_state_t *gsp_get_compute(void)
{
    return &compute;
}

/* Select compute class based on GPU generation */
static uint32_t compute_class_for_gen(gpu_gen_t gen)
{
    switch (gen) {
    case GPU_GEN_TURING:       return TURING_COMPUTE_A;
    case GPU_GEN_AMPERE:       return AMPERE_COMPUTE_A;
    case GPU_GEN_ADA_LOVELACE: return ADA_COMPUTE_A;
    default:                   return AMPERE_COMPUTE_A;
    }
}

/* ── Pushbuffer helpers ── */

static void pb_begin(pushbuf_state_t *pb)
{
    pb->pos = 0;
}

static void pb_push(pushbuf_state_t *pb, uint32_t data)
{
    if (pb->pos < pb->capacity)
        pb->buf[pb->pos++] = data;
}

static uint32_t pb_size_bytes(pushbuf_state_t *pb)
{
    return pb->pos * 4;
}

/* Submit pushbuffer via GPFIFO ring + USERD GP_PUT doorbell */
static int pb_submit(pushbuf_state_t *pb)
{
    if (pb->pos == 0) return 0;

    uint32_t gp_idx = channel.gp_put % channel.gpfifo_entries;

    /* Build GPFIFO entry pointing to pushbuffer data */
    gpfifo_make_entry(&channel.gpfifo[gp_idx], pb->buf_phys, pb_size_bytes(pb));
    wmb();

    /* Advance GP_PUT */
    channel.gp_put++;

    /* Write GP_PUT to USERD doorbell */
    volatile uint32_t *userd = (volatile uint32_t *)channel.userd_mem;
    userd[USERD_GP_PUT / 4] = channel.gp_put;
    wmb();

    serial_puts("[COMPUTE] Submitted ");
    serial_putdec(pb->pos);
    serial_puts(" dwords via GPFIFO[");
    serial_putdec(gp_idx);
    serial_puts("], gp_put=");
    serial_putdec(channel.gp_put);
    serial_puts("\n");

    return 0;
}

/* ── Channel activation via RM control commands ── */

static int compute_bind_channel(void)
{
    serial_puts("[COMPUTE] Step 1/4: CTRL_BIND (engine=GR0)\n");

    nva06f_ctrl_bind_params_t bind_params;
    memset(&bind_params, 0, sizeof(bind_params));
    bind_params.engineType = NV2080_ENGINE_TYPE_GR0;

    int ret = gsp_rm_control(GSP_RM_CHAN_HANDLE, NVA06F_CTRL_CMD_BIND,
                             &bind_params, sizeof(bind_params));

    compute.channel_bound = (ret == 0);
    serial_puts("[COMPUTE] Bind: ");
    serial_puts(compute.channel_bound ? "sent" : "failed");
    serial_puts("\n");

    return ret;
}

static int compute_schedule_channel(void)
{
    serial_puts("[COMPUTE] Step 2/4: CTRL_GPFIFO_SCHEDULE (enable)\n");

    nva06f_ctrl_gpfifo_schedule_params_t sched_params;
    memset(&sched_params, 0, sizeof(sched_params));
    sched_params.bEnable     = 1;
    sched_params.bSkipSubmit = 0;

    int ret = gsp_rm_control(GSP_RM_CHAN_HANDLE, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE,
                             &sched_params, sizeof(sched_params));

    compute.channel_scheduled = (ret == 0);
    serial_puts("[COMPUTE] Schedule: ");
    serial_puts(compute.channel_scheduled ? "sent" : "failed");
    serial_puts("\n");

    return ret;
}

/* ── Push compute class binding + initial barrier ── */

static int compute_push_set_object(void)
{
    serial_puts("[COMPUTE] Step 3/4: SET_OBJECT (class 0x");
    serial_puthex(compute.compute_class, 4);
    serial_puts(") on subchannel ");
    serial_putdec(SUBCHANNEL_COMPUTE);
    serial_puts("\n");

    pushbuf_state_t *pb = &compute.pb;
    pb_begin(pb);

    /* SET_OBJECT: bind compute class to subchannel 1 */
    pb_push(pb, NV_METHOD(SUBCHANNEL_COMPUTE, NVC5C0_SET_OBJECT, 1));
    pb_push(pb, compute.compute_class);

    /* INVALIDATE_SHADER_CACHES: flush I$/D$/const caches */
    pb_push(pb, NV_METHOD(SUBCHANNEL_COMPUTE, NVC5C0_INVALIDATE_SHADER_CACHES, 1));
    pb_push(pb, INVALIDATE_SHADER_CACHES_INSTRUCTION
              | INVALIDATE_SHADER_CACHES_DATA
              | INVALIDATE_SHADER_CACHES_CONSTANT
              | INVALIDATE_SHADER_CACHES_FLUSH_DATA);

    /* SET_SHADER_SHARED_MEMORY_WINDOW: 0xFE000000 (standard) */
    pb_push(pb, NV_METHOD(SUBCHANNEL_COMPUTE, NVC5C0_SET_SHADER_SHARED_MEMORY_WINDOW_A, 2));
    pb_push(pb, 0x00000000);  /* upper bits */
    pb_push(pb, 0xFE000000);  /* lower bits */

    /* SET_SHADER_LOCAL_MEMORY_WINDOW: 0xFF000000 (standard) */
    pb_push(pb, NV_METHOD(SUBCHANNEL_COMPUTE, NVC5C0_SET_SHADER_LOCAL_MEMORY_WINDOW, 1));
    pb_push(pb, 0xFF000000);

    /* WAIT_FOR_IDLE: barrier */
    pb_push(pb, NV_METHOD(SUBCHANNEL_COMPUTE, NVC5C0_WAIT_FOR_IDLE, 1));
    pb_push(pb, 0x00000000);

    int ret = pb_submit(pb);
    compute.class_bound = (ret == 0);

    return ret;
}

/* ── Semaphore fence (GPU→CPU completion signal) ── */

static int compute_push_semaphore_fence(void)
{
    if (!compute.semaphore) return -1;

    serial_puts("[COMPUTE] Step 4/4: Semaphore fence at 0x");
    serial_puthex(compute.sem_phys, 16);
    serial_puts("\n");

    /* Reset semaphore to 0 */
    compute.semaphore[0] = 0;
    wmb();

    pushbuf_state_t *pb = &compute.pb;
    pb_begin(pb);

    /* SEMAPHORE RELEASE on compute subchannel */
    pb_push(pb, NV_METHOD(SUBCHANNEL_COMPUTE, NVA06F_SEMAPHOREA, 4));
    pb_push(pb, (uint32_t)(compute.sem_phys >> 32));  /* SEMAPHOREA: addr upper */
    pb_push(pb, (uint32_t)(compute.sem_phys & 0xFFFFFFFF));  /* SEMAPHOREB: addr lower */
    pb_push(pb, 0x00000001);  /* SEMAPHOREC: payload value (1 = done) */
    pb_push(pb, NVA06F_SEMAPHORED_OPERATION_RELEASE
              | NVA06F_SEMAPHORED_RELEASE_SIZE_4BYTE);  /* SEMAPHORED */

    return pb_submit(pb);
}

/* ── Poll semaphore for GPU completion ── */

static int compute_poll_semaphore(uint32_t expected, uint32_t timeout_ms)
{
    uint64_t timeout_cycles = (uint64_t)timeout_ms * 3000000ULL;  /* ~3GHz estimate */
    uint64_t t0 = rdtsc();

    while (1) {
        rmb();
        uint32_t val = compute.semaphore[0];
        if (val == expected) {
            serial_puts("[COMPUTE] Semaphore = ");
            serial_putdec(val);
            serial_puts(" (OK)\n");
            return 0;
        }

        uint64_t elapsed = rdtsc() - t0;
        if (elapsed > timeout_cycles) {
            serial_puts("[COMPUTE] Semaphore timeout: expected ");
            serial_putdec(expected);
            serial_puts(", got ");
            serial_putdec(val);
            serial_puts("\n");
            return -1;
        }
    }
}

/* ── Public API: gsp_compute_init ── */

int gsp_compute_init(void)
{
    if (!channel.allocated) {
        serial_puts("[COMPUTE] Channel not allocated, skipping compute init\n");
        return -1;
    }

    serial_puts("[COMPUTE] === Compute Class Init ===\n");

    memset(&compute, 0, sizeof(compute));

    /* Determine compute class for this GPU */
    gpu_device_t *dev = &gpu_dev;
    compute.compute_class = compute_class_for_gen(dev->generation);

    serial_puts("[COMPUTE] GPU gen=");
    serial_puts(gpu_gen_name(dev->generation));
    serial_puts(", compute class=0x");
    serial_puthex(compute.compute_class, 4);
    serial_puts("\n");

    /* Allocate pushbuffer (4KB, page-aligned) */
    compute.pb.capacity = PUSHBUF_SIZE_DWORDS;
    compute.pb.buf = (uint32_t *)mem_alloc_aligned(PUSHBUF_SIZE_DWORDS * 4, 4096);
    if (!compute.pb.buf) {
        serial_puts("[COMPUTE] Failed to allocate pushbuffer\n");
        return -1;
    }
    memset(compute.pb.buf, 0, PUSHBUF_SIZE_DWORDS * 4);
    compute.pb.buf_phys = (uint64_t)(uintptr_t)compute.pb.buf;
    compute.pb.pos = 0;

    serial_puts("[COMPUTE] Pushbuffer at 0x");
    serial_puthex(compute.pb.buf_phys, 16);
    serial_puts(" (");
    serial_putdec(PUSHBUF_SIZE_DWORDS * 4);
    serial_puts(" bytes)\n");

    /* Allocate semaphore memory (4KB page, only first dword used) */
    compute.semaphore = (uint32_t *)mem_alloc_aligned(4096, 4096);
    if (!compute.semaphore) {
        serial_puts("[COMPUTE] Failed to allocate semaphore memory\n");
        return -1;
    }
    memset(compute.semaphore, 0, 4096);
    compute.sem_phys = (uint64_t)(uintptr_t)compute.semaphore;

    serial_puts("[COMPUTE] Semaphore at 0x");
    serial_puthex(compute.sem_phys, 16);
    serial_puts("\n");

    /* Step 1: Bind channel to GR0 engine */
    compute_bind_channel();

    /* Step 2: Schedule channel on runlist */
    compute_schedule_channel();

    /* Step 3: Push SET_OBJECT + cache invalidate + barrier */
    compute_push_set_object();

    /* Step 4: Push semaphore fence */
    compute_push_semaphore_fence();

    /* Poll for semaphore (2s timeout) — will timeout without full GSP boot chain */
    int sem_ret = compute_poll_semaphore(1, 2000);
    if (sem_ret == 0) {
        compute.ready = true;
        serial_puts("[COMPUTE] Compute engine READY\n");
        fb_puts_color(" Compute: READY\n", 0x0000FF00);
    } else {
        serial_puts("[COMPUTE] Semaphore timeout (expected without full boot chain)\n");
        fb_puts(" Compute: init sent (pending boot chain)\n");
    }

    serial_puts("[COMPUTE] === Compute init complete ===\n");

    return 0;
}

/* ── Public API: gsp_compute_barrier ── */

int gsp_compute_barrier(void)
{
    if (!compute.class_bound) return -1;

    pushbuf_state_t *pb = &compute.pb;
    pb_begin(pb);

    /* WAIT_FOR_IDLE on compute subchannel */
    pb_push(pb, NV_METHOD(SUBCHANNEL_COMPUTE, NVC5C0_WAIT_FOR_IDLE, 1));
    pb_push(pb, 0x00000000);

    /* Semaphore release for CPU synchronization */
    uint32_t fence_val = compute.semaphore[0] + 1;

    pb_push(pb, NV_METHOD(SUBCHANNEL_COMPUTE, NVA06F_SEMAPHOREA, 4));
    pb_push(pb, (uint32_t)(compute.sem_phys >> 32));
    pb_push(pb, (uint32_t)(compute.sem_phys & 0xFFFFFFFF));
    pb_push(pb, fence_val);
    pb_push(pb, NVA06F_SEMAPHORED_OPERATION_RELEASE
              | NVA06F_SEMAPHORED_RELEASE_SIZE_4BYTE);

    pb_submit(pb);

    return compute_poll_semaphore(fence_val, 2000);
}

/* ══════════════════════════════════════════════════════════
 *  X35: Copy Engine DMA
 *
 *  Host-to-device and device-to-host DMA transfers via the
 *  Copy Engine (CE). Uses physical addressing for both system
 *  RAM and VRAM. Shares the same GPFIFO channel as compute
 *  (subchannel 4 = CE, subchannel 1 = compute).
 *
 *  Reference: NVIDIA open-gpu-doc clc5b5.h/clc6b5.h/clc7b5.h,
 *             Nouveau nvc0 CE usage patterns.
 * ══════════════════════════════════════════════════════════ */

static ce_state_t ce;

ce_state_t *gsp_get_ce(void)
{
    return &ce;
}

/* Select CE class based on GPU generation */
static uint32_t ce_class_for_gen(gpu_gen_t gen)
{
    switch (gen) {
    case GPU_GEN_TURING:       return TURING_DMA_COPY_A;
    case GPU_GEN_AMPERE:       return AMPERE_DMA_COPY_A;
    case GPU_GEN_ADA_LOVELACE: return AMPERE_DMA_COPY_B;
    default:                   return AMPERE_DMA_COPY_A;
    }
}

/* Poll CE semaphore for completion */
static int ce_poll_semaphore(uint32_t expected, uint32_t timeout_ms)
{
    uint64_t timeout_cycles = (uint64_t)timeout_ms * 3000000ULL;
    uint64_t t0 = rdtsc();

    while (1) {
        rmb();
        uint32_t val = ce.semaphore[0];
        if (val == expected) {
            serial_puts("[CE] Semaphore = ");
            serial_putdec(val);
            serial_puts(" (OK)\n");
            return 0;
        }

        uint64_t elapsed = rdtsc() - t0;
        if (elapsed > timeout_cycles) {
            serial_puts("[CE] Semaphore timeout: expected ");
            serial_putdec(expected);
            serial_puts(", got ");
            serial_putdec(val);
            serial_puts("\n");
            return -1;
        }
    }
}

/* Submit CE pushbuffer via shared GPFIFO channel */
static int ce_pb_submit(pushbuf_state_t *pb)
{
    if (pb->pos == 0) return 0;

    uint32_t gp_idx = channel.gp_put % channel.gpfifo_entries;

    gpfifo_make_entry(&channel.gpfifo[gp_idx], pb->buf_phys, pb_size_bytes(pb));
    wmb();

    channel.gp_put++;

    volatile uint32_t *userd = (volatile uint32_t *)channel.userd_mem;
    userd[USERD_GP_PUT / 4] = channel.gp_put;
    wmb();

    serial_puts("[CE] Submitted ");
    serial_putdec(pb->pos);
    serial_puts(" dwords via GPFIFO[");
    serial_putdec(gp_idx);
    serial_puts("]\n");

    return 0;
}

/* ── Internal: push a CE DMA copy ── */

static int ce_push_copy(uint64_t src_phys, uint64_t dst_phys,
                        uint32_t size, uint32_t src_target, uint32_t dst_target)
{
    if (!ce.class_bound) return -1;
    if (size == 0) return 0;

    /* Max single transfer: 16MB (CE LINE_LENGTH_IN is 32 bits, but
     * practical limit due to pushbuffer space). Chunk larger copies. */
    #define CE_MAX_COPY_SIZE  (16 * 1024 * 1024)

    uint64_t remaining = size;
    uint64_t src = src_phys;
    uint64_t dst = dst_phys;

    while (remaining > 0) {
        uint32_t chunk = remaining > CE_MAX_COPY_SIZE ?
                         CE_MAX_COPY_SIZE : (uint32_t)remaining;

        pushbuf_state_t *pb = &ce.pb;
        pb_begin(pb);

        /* Set physical memory targets */
        pb_push(pb, NV_METHOD(SUBCHANNEL_CE, CE_SET_SRC_PHYS_MODE, 2));
        pb_push(pb, src_target);
        pb_push(pb, dst_target);

        /* Set source address */
        pb_push(pb, NV_METHOD(SUBCHANNEL_CE, CE_OFFSET_IN_UPPER, 4));
        pb_push(pb, (uint32_t)(src >> 32));
        pb_push(pb, (uint32_t)(src & 0xFFFFFFFF));
        /* Set destination address */
        pb_push(pb, (uint32_t)(dst >> 32));
        pb_push(pb, (uint32_t)(dst & 0xFFFFFFFF));

        /* Set transfer size: linear 1D copy */
        pb_push(pb, NV_METHOD(SUBCHANNEL_CE, CE_LINE_LENGTH_IN, 2));
        pb_push(pb, chunk);
        pb_push(pb, 1);  /* LINE_COUNT = 1 (linear) */

        /* Semaphore: release on last chunk only */
        if (remaining <= CE_MAX_COPY_SIZE) {
            ce.fence_seq++;
            pb_push(pb, NV_METHOD(SUBCHANNEL_CE, CE_SET_SEMAPHORE_A, 3));
            pb_push(pb, (uint32_t)(ce.sem_phys >> 32) & 0x1FFFF);
            pb_push(pb, (uint32_t)(ce.sem_phys & 0xFFFFFFFF));
            pb_push(pb, ce.fence_seq);
        }

        /* LAUNCH_DMA */
        uint32_t launch = CE_LAUNCH_DMA_TRANSFER_NON_PIPELINED
                        | CE_LAUNCH_DMA_SRC_PITCH
                        | CE_LAUNCH_DMA_DST_PITCH
                        | CE_LAUNCH_DMA_SRC_PHYSICAL
                        | CE_LAUNCH_DMA_DST_PHYSICAL;

        /* Add semaphore release on last chunk */
        if (remaining <= CE_MAX_COPY_SIZE)
            launch |= CE_LAUNCH_DMA_SEM_RELEASE_1WORD;

        pb_push(pb, NV_METHOD(SUBCHANNEL_CE, CE_LAUNCH_DMA, 1));
        pb_push(pb, launch);

        ce_pb_submit(pb);

        src += chunk;
        dst += chunk;
        remaining -= chunk;
    }

    return 0;
}

/* ── Public API: gsp_ce_copy_h2d — Host to Device (sysmem → VRAM) ── */

int gsp_ce_copy_h2d(uint64_t src_phys, uint64_t dst_vram, uint32_t size)
{
    serial_puts("[CE] H2D copy: src=0x");
    serial_puthex(src_phys, 16);
    serial_puts(" dst=VRAM+0x");
    serial_puthex(dst_vram, 16);
    serial_puts(" size=");
    serial_putdec(size);
    serial_puts("\n");

    int ret = ce_push_copy(src_phys, dst_vram, size,
                           CE_PHYS_TARGET_COHERENT_SYSMEM,
                           CE_PHYS_TARGET_LOCAL_FB);
    if (ret < 0) return ret;

    /* Wait for completion */
    return ce_poll_semaphore(ce.fence_seq, 5000);
}

/* ── Public API: gsp_ce_copy_d2h — Device to Host (VRAM → sysmem) ── */

int gsp_ce_copy_d2h(uint64_t src_vram, uint64_t dst_phys, uint32_t size)
{
    serial_puts("[CE] D2H copy: src=VRAM+0x");
    serial_puthex(src_vram, 16);
    serial_puts(" dst=0x");
    serial_puthex(dst_phys, 16);
    serial_puts(" size=");
    serial_putdec(size);
    serial_puts("\n");

    int ret = ce_push_copy(src_vram, dst_phys, size,
                           CE_PHYS_TARGET_LOCAL_FB,
                           CE_PHYS_TARGET_COHERENT_SYSMEM);
    if (ret < 0) return ret;

    /* Wait for completion */
    return ce_poll_semaphore(ce.fence_seq, 5000);
}

/* ── Public API: gsp_ce_init ── */

int gsp_ce_init(void)
{
    if (!channel.allocated) {
        serial_puts("[CE] Channel not allocated, skipping CE init\n");
        return -1;
    }

    serial_puts("[CE] === Copy Engine Init ===\n");

    memset(&ce, 0, sizeof(ce));

    /* Determine CE class for this GPU */
    gpu_device_t *dev = &gpu_dev;
    ce.ce_class = ce_class_for_gen(dev->generation);

    serial_puts("[CE] GPU gen=");
    serial_puts(gpu_gen_name(dev->generation));
    serial_puts(", CE class=0x");
    serial_puthex(ce.ce_class, 4);
    serial_puts("\n");

    /* Allocate CE pushbuffer (4KB, page-aligned) */
    ce.pb.capacity = PUSHBUF_SIZE_DWORDS;
    ce.pb.buf = (uint32_t *)mem_alloc_aligned(PUSHBUF_SIZE_DWORDS * 4, 4096);
    if (!ce.pb.buf) {
        serial_puts("[CE] Failed to allocate pushbuffer\n");
        return -1;
    }
    memset(ce.pb.buf, 0, PUSHBUF_SIZE_DWORDS * 4);
    ce.pb.buf_phys = (uint64_t)(uintptr_t)ce.pb.buf;
    ce.pb.pos = 0;

    /* Allocate CE semaphore (4KB page) */
    ce.semaphore = (uint32_t *)mem_alloc_aligned(4096, 4096);
    if (!ce.semaphore) {
        serial_puts("[CE] Failed to allocate semaphore\n");
        return -1;
    }
    memset(ce.semaphore, 0, 4096);
    ce.sem_phys = (uint64_t)(uintptr_t)ce.semaphore;
    ce.fence_seq = 0;

    serial_puts("[CE] Pushbuf=0x");
    serial_puthex(ce.pb.buf_phys, 16);
    serial_puts(" Sem=0x");
    serial_puthex(ce.sem_phys, 16);
    serial_puts("\n");

    /* Bind CE class to subchannel 4 via SET_OBJECT */
    serial_puts("[CE] SET_OBJECT (class 0x");
    serial_puthex(ce.ce_class, 4);
    serial_puts(") on subchannel ");
    serial_putdec(SUBCHANNEL_CE);
    serial_puts("\n");

    pushbuf_state_t *pb = &ce.pb;
    pb_begin(pb);

    pb_push(pb, NV_METHOD(SUBCHANNEL_CE, NVA06F_SET_OBJECT, 1));
    pb_push(pb, ce.ce_class);

    /* NOP padding */
    pb_push(pb, NV_NOP);
    pb_push(pb, NV_NOP);

    ce_pb_submit(pb);

    ce.class_bound = true;

    /* Test: semaphore-only fence (no DMA copy, just verify CE responds) */
    ce.fence_seq = 1;
    ce.semaphore[0] = 0;
    wmb();

    pb_begin(pb);

    pb_push(pb, NV_METHOD(SUBCHANNEL_CE, CE_SET_SEMAPHORE_A, 3));
    pb_push(pb, (uint32_t)(ce.sem_phys >> 32) & 0x1FFFF);
    pb_push(pb, (uint32_t)(ce.sem_phys & 0xFFFFFFFF));
    pb_push(pb, 1);  /* payload = 1 */

    /* Launch with semaphore release only, no actual data transfer */
    pb_push(pb, NV_METHOD(SUBCHANNEL_CE, CE_LAUNCH_DMA, 1));
    pb_push(pb, CE_LAUNCH_DMA_TRANSFER_NONE
              | CE_LAUNCH_DMA_SEM_RELEASE_1WORD);

    ce_pb_submit(pb);

    int ret = ce_poll_semaphore(1, 2000);
    if (ret == 0) {
        ce.ready = true;
        serial_puts("[CE] Copy Engine READY\n");
        fb_puts_color(" CE DMA: READY\n", 0x0000FF00);
    } else {
        serial_puts("[CE] Semaphore timeout (expected without full boot chain)\n");
        fb_puts(" CE DMA: init sent (pending boot chain)\n");
    }

    serial_puts("[CE] === CE init complete ===\n");

    return 0;
}

/* ══════════════════════════════════════════════════════════
 *  X36: Kernel Completion + Semaphore Sync
 *
 *  Builds QMD (Queue Meta Data, 256 bytes), dispatches via
 *  SEND_PCAS_A + SEND_SIGNALING_PCAS_B, waits for QMD
 *  RELEASE0 semaphore, reads results via CE DMA.
 *
 *  QMD version: V02_03 (Turing/Ampere compatible).
 *  Reference: NVIDIA open-gpu-doc clc5c0qmd.h, Mesa NAK qmd.rs.
 * ══════════════════════════════════════════════════════════ */

/* ── QMD builder (QMDV02_03, 256 bytes = 64 dwords) ── */

static void qmd_build(uint32_t *qmd, const compute_dispatch_t *desc)
{
    memset(qmd, 0, QMD_SIZE_BYTES);

    /* DW4: SM_GLOBAL_CACHING_ENABLE + SEMAPHORE_RELEASE_ENABLE0 */
    qmd[QMD_DW4] = (1 << 6);   /* SM_GLOBAL_CACHING_ENABLE */
    if (desc->sem_addr)
        qmd[QMD_DW4] |= (1 << 10); /* SEMAPHORE_RELEASE_ENABLE0 */

    /* DW5: Invalidate all caches */
    qmd[QMD_DW5] = (1 << 26)   /* INVALIDATE_TEXTURE_HEADER_CACHE */
                  | (1 << 27)   /* INVALIDATE_TEXTURE_SAMPLER_CACHE */
                  | (1 << 28)   /* INVALIDATE_TEXTURE_DATA_CACHE */
                  | (1 << 29)   /* INVALIDATE_SHADER_DATA_CACHE */
                  | (1 << 30)   /* INVALIDATE_INSTRUCTION_CACHE */
                  | (1 << 31);  /* INVALIDATE_SHADER_CONSTANT_CACHE */

    /* DW11: RELEASE_MEMBAR_TYPE=FE_SYSMEMBAR, CWD_MEMBAR=L1_SYSMEMBAR,
     *       API_VISIBLE_CALL_LIMIT=NO_CHECK */
    qmd[QMD_DW11] = (1 << 14)   /* RELEASE_MEMBAR_TYPE = FE_SYSMEMBAR */
                   | (1 << 16)   /* CWD_MEMBAR_TYPE = L1_SYSMEMBAR */
                   | (1 << 26);  /* API_VISIBLE_CALL_LIMIT = NO_CHECK */

    /* DW12-14: Grid dimensions (CTA raster) */
    qmd[QMD_DW12] = desc->grid_x;
    qmd[QMD_DW13] = desc->grid_y & 0xFFFF;
    qmd[QMD_DW14] = desc->grid_z & 0xFFFF;

    /* DW17: Shared memory size (aligned to 0x100) */
    qmd[QMD_DW17] = (desc->shared_mem_size + 0xFF) & ~0xFF;

    /* DW18: QMD version + CTA_THREAD_DIMENSION0 */
    qmd[QMD_DW18] = (QMD_VERSION_V02_03)          /* bits 3:0 */
                   | (QMD_MAJOR_VERSION_V02 << 4)  /* bits 7:4 */
                   | (desc->block_x << 16);         /* bits 31:16 */

    /* DW19: CTA_THREAD_DIMENSION1 + CTA_THREAD_DIMENSION2 */
    qmd[QMD_DW19] = (desc->block_y & 0xFFFF)
                   | ((desc->block_z & 0xFFFF) << 16);

    /* DW20: CONSTANT_BUFFER_VALID (bit 0 = CB0) + REGISTER_COUNT_V (bits 16:8) */
    qmd[QMD_DW20] = (desc->register_count & 0xFF) << 8;
    if (desc->cbuf_size > 0)
        qmd[QMD_DW20] |= (1 << 0);  /* CONSTANT_BUFFER_VALID(0) */

    /* DW23-25: RELEASE0 semaphore (if enabled) */
    if (desc->sem_addr) {
        qmd[QMD_DW23] = (uint32_t)(desc->sem_addr & 0xFFFFFFFF);
        qmd[QMD_DW24] = (uint32_t)((desc->sem_addr >> 32) & 0xFF);
        /* RELEASE0_STRUCTURE_SIZE = ONE_WORD (bit 31 = 1) */
        qmd[QMD_DW24] |= (1 << 31);
        qmd[QMD_DW25] = desc->sem_payload;
    }

    /* DW29: SHADER_LOCAL_MEMORY_LOW_SIZE + BARRIER_COUNT */
    qmd[QMD_DW29] = (desc->barrier_count & 0x1F) << 27;

    /* DW32-33: CONSTANT_BUFFER(0) address + size (if enabled) */
    if (desc->cbuf_size > 0) {
        qmd[QMD_DW32] = (uint32_t)(desc->cbuf_addr & 0xFFFFFFFF);
        /* DW33: ADDR_UPPER(16:0) | SIZE_SHIFTED4(31:17) */
        uint32_t size_shifted = (desc->cbuf_size >> 4) & 0x7FFF;
        qmd[QMD_DW33] = ((uint32_t)(desc->cbuf_addr >> 32) & 0x1FFFF)
                       | (size_shifted << 17);
    }

    /* DW48-49: PROGRAM_ADDRESS */
    qmd[QMD_DW48] = (uint32_t)(desc->program_addr & 0xFFFFFFFF);
    qmd[QMD_DW49] = (uint32_t)((desc->program_addr >> 32) & 0x1FFFF);
}

/* ── Dispatch: push SEND_PCAS + signal ── */

int gsp_compute_dispatch(const compute_dispatch_t *desc)
{
    if (!compute.class_bound) {
        serial_puts("[DISPATCH] Compute class not bound\n");
        return -1;
    }
    if (!desc || !desc->program_addr) {
        serial_puts("[DISPATCH] Invalid dispatch descriptor\n");
        return -1;
    }

    serial_puts("[DISPATCH] Grid=");
    serial_putdec(desc->grid_x); serial_puts("x");
    serial_putdec(desc->grid_y); serial_puts("x");
    serial_putdec(desc->grid_z);
    serial_puts(" Block=");
    serial_putdec(desc->block_x); serial_puts("x");
    serial_putdec(desc->block_y); serial_puts("x");
    serial_putdec(desc->block_z);
    serial_puts(" Regs=");
    serial_putdec(desc->register_count);
    serial_puts("\n");

    /* Allocate QMD (256-byte aligned) */
    uint32_t *qmd = (uint32_t *)mem_alloc_aligned(QMD_SIZE_BYTES, QMD_ALIGNMENT);
    if (!qmd) {
        serial_puts("[DISPATCH] Failed to allocate QMD\n");
        return -1;
    }

    /* Build QMD */
    qmd_build(qmd, desc);
    wmb();

    uint64_t qmd_phys = (uint64_t)(uintptr_t)qmd;

    serial_puts("[DISPATCH] QMD at 0x");
    serial_puthex(qmd_phys, 16);
    serial_puts(" prog=0x");
    serial_puthex(desc->program_addr, 16);
    serial_puts("\n");

    /* Push dispatch commands */
    pushbuf_state_t *pb = &compute.pb;
    pb_begin(pb);

    /* SEND_PCAS_A: QMD address >> 8 */
    pb_push(pb, NV_METHOD(SUBCHANNEL_COMPUTE, NVC5C0_SEND_PCAS_A, 1));
    pb_push(pb, (uint32_t)(qmd_phys >> 8));

    /* Determine dispatch signal method based on GPU generation */
    gpu_device_t *dev = &gpu_dev;
    if (dev->generation >= GPU_GEN_AMPERE) {
        /* Ampere+: SEND_SIGNALING_PCAS2_B */
        pb_push(pb, NV_METHOD_IMMD(SUBCHANNEL_COMPUTE,
                    NVC6C0_SEND_SIGNALING_PCAS2_B,
                    PCAS2_ACTION_INVALIDATE_COPY_SCHEDULE));
    } else {
        /* Turing: SEND_SIGNALING_PCAS_B */
        pb_push(pb, NV_METHOD_IMMD(SUBCHANNEL_COMPUTE,
                    NVC5C0_SEND_SIGNALING_PCAS_B,
                    SIGNALING_PCAS_B_INVALIDATE | SIGNALING_PCAS_B_SCHEDULE));
    }

    pb_submit(pb);

    serial_puts("[DISPATCH] Kernel launched\n");

    return 0;
}

/* ── Wait for kernel completion via semaphore ── */

int gsp_compute_wait(uint64_t sem_addr, uint32_t expected, uint32_t timeout_ms)
{
    volatile uint32_t *sem = (volatile uint32_t *)(uintptr_t)sem_addr;
    uint64_t timeout_cycles = (uint64_t)timeout_ms * 3000000ULL;
    uint64_t t0 = rdtsc();

    serial_puts("[DISPATCH] Waiting for semaphore at 0x");
    serial_puthex(sem_addr, 16);
    serial_puts(" expected=");
    serial_putdec(expected);
    serial_puts("...\n");

    while (1) {
        rmb();
        uint32_t val = *sem;
        if (val == expected) {
            uint64_t elapsed_cycles = rdtsc() - t0;
            uint32_t elapsed_us = (uint32_t)(elapsed_cycles / 3000);
            serial_puts("[DISPATCH] Kernel complete (");
            serial_putdec(elapsed_us);
            serial_puts(" us)\n");
            return 0;
        }

        uint64_t elapsed = rdtsc() - t0;
        if (elapsed > timeout_cycles) {
            serial_puts("[DISPATCH] Timeout waiting for kernel (");
            serial_putdec(timeout_ms);
            serial_puts(" ms), sem=");
            serial_putdec(val);
            serial_puts("\n");
            return -1;
        }
    }
}

/* ── Read results back from GPU memory via CE DMA ── */

int gsp_compute_read_results(uint64_t src_vram, void *dst, uint32_t size)
{
    if (!dst || size == 0) return -1;

    serial_puts("[DISPATCH] Reading ");
    serial_putdec(size);
    serial_puts(" bytes from VRAM+0x");
    serial_puthex(src_vram, 16);
    serial_puts("\n");

    /* Use CE D2H copy (X35) */
    uint64_t dst_phys = (uint64_t)(uintptr_t)dst;
    return gsp_ce_copy_d2h(src_vram, dst_phys, size);
}

/* ── Phase 10: FWSEC-FRTS Execution + WPR2 ───────────────────── */

/*
 * FWSEC-FRTS: Load FWSEC from VBIOS into GSP Falcon, execute FRTS command
 * to create WPR2 region in VRAM. This must happen before GSP firmware boot.
 *
 * Sequence (Ampere/Ada — direct GSP Falcon):
 *  1. Halt GSP Falcon
 *  2. Upload FWSEC image to VRAM via PRAMIN
 *  3. Set DMATRFBASE to FWSEC location in VRAM
 *  4. Set BOOTVEC to FWSEC entry point
 *  5. Set MAILBOX0 = FRTS command, MAILBOX1 = 0
 *  6. Start Falcon
 *  7. Poll mailbox for completion
 *  8. Check for WPR2 metadata in VRAM
 */

#define FWSEC_VRAM_OFFSET_MB  192   /* Place FWSEC at VRAM+192MB (away from gsp.bin at +128MB) */

static int gsp_fwsec_frts_legacy(void)
{
    gpu_probe_t *p = gpu_get_probe();
    fwsec_state_t *fw = gpu_get_fwsec();

    if (!p || !p->present || !p->gsp_present) {
        serial_puts("[FWSEC] No GPU/GSP detected\n");
        return -1;
    }

    if (!fw || !fw->found || !fw->data || fw->size == 0) {
        serial_puts("[FWSEC] No FWSEC image available (VBIOS parse may have failed)\n");
        return -1;
    }

    if (!p->pramin_rw_ok) {
        serial_puts("[FWSEC] PRAMIN not writable, cannot upload FWSEC\n");
        return -1;
    }

    uint64_t vram_bytes = (uint64_t)p->vram_size_mb * 1024 * 1024;
    uint64_t fwsec_vram_off = (uint64_t)FWSEC_VRAM_OFFSET_MB * 1024 * 1024;

    if (vram_bytes < fwsec_vram_off + fw->size) {
        serial_puts("[FWSEC] Not enough VRAM for FWSEC placement\n");
        return -1;
    }

    serial_puts("[FWSEC] === FWSEC-FRTS Execution ===\n");
    serial_puts("[FWSEC] Image: ");
    serial_putdec(fw->size);
    serial_puts(" bytes, target=0x");
    serial_puthex(fw->target_id, 2);
    serial_puts("\n");

    /* ── Step 1: Halt Falcon ── */
    serial_puts("[FWSEC] Step 1: Halting GSP Falcon...\n");
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_HALTED);
    wmb();

    /* Verify halted */
    uint32_t cpuctl = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_CPUCTL);
    if (!(cpuctl & NV_FALCON_CPUCTL_HALTED)) {
        serial_puts("[FWSEC] WARNING: Falcon did not halt (CPUCTL=0x");
        serial_puthex(cpuctl, 8);
        serial_puts(")\n");
    }

    /* ── Step 2: Upload FWSEC to VRAM via PRAMIN ── */
    serial_puts("[FWSEC] Step 2: Uploading FWSEC to VRAM+");
    serial_putdec(FWSEC_VRAM_OFFSET_MB);
    serial_puts("MB...\n");

    uint32_t orig_window = gpu_reg_read(NV_PBUS_BAR0_WINDOW);

    uint64_t uploaded = 0;
    uint32_t *src32 = (uint32_t *)fw->data;

    while (uploaded < fw->size) {
        uint64_t vram_addr = fwsec_vram_off + uploaded;
        uint32_t window_val = (uint32_t)(vram_addr >> 16);
        gpu_reg_write(NV_PBUS_BAR0_WINDOW, window_val);
        wmb();

        uint64_t chunk = fw->size - uploaded;
        if (chunk > NV_PRAMIN_SIZE)
            chunk = NV_PRAMIN_SIZE;

        uint32_t pramin_start = (uint32_t)(vram_addr & (NV_PRAMIN_SIZE - 1));
        uint32_t available = NV_PRAMIN_SIZE - pramin_start;
        if (chunk > available)
            chunk = available;

        uint32_t dwords = (uint32_t)((chunk + 3) / 4);
        for (uint32_t i = 0; i < dwords; i++)
            gpu_reg_write(NV_PRAMIN_BASE + pramin_start + (i * 4),
                          src32[(uploaded / 4) + i]);
        wmb();

        uploaded += chunk;
    }

    serial_puts("[FWSEC] Uploaded ");
    serial_putdec(uploaded);
    serial_puts(" bytes\n");

    /* Verify first 4 dwords */
    gpu_reg_write(NV_PBUS_BAR0_WINDOW, (uint32_t)(fwsec_vram_off >> 16));
    wmb();
    rmb();

    uint32_t v0 = gpu_reg_read(NV_PRAMIN_BASE);
    uint32_t v1 = gpu_reg_read(NV_PRAMIN_BASE + 4);
    bool verify_ok = (v0 == src32[0] && v1 == src32[1]);

    serial_puts("[FWSEC] Verify: ");
    serial_puthex(v0, 8);
    serial_puts(" ");
    serial_puthex(v1, 8);
    serial_puts(verify_ok ? " OK\n" : " MISMATCH\n");

    /* ── Step 3: Set DMATRFBASE ── */
    uint32_t dma_base = (uint32_t)(fwsec_vram_off >> 8);
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_DMATRFBASE, dma_base);
    wmb();

    /* ── Step 4: Set BOOTVEC ── */
    /* FWSEC entry point is typically at offset 0 */
    uint32_t bootvec = 0;
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_BOOTVEC, bootvec);
    wmb();

    /* ── Step 5: Set mailboxes with FRTS command ── */
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_MAILBOX0, FWSEC_FRTS_CMD);
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_MAILBOX1, 0);
    wmb();

    serial_puts("[FWSEC] Step 3-5: DMATRFBASE=0x");
    serial_puthex(dma_base, 8);
    serial_puts(" BOOTVEC=0x");
    serial_puthex(bootvec, 8);
    serial_puts(" MAILBOX0=0x");
    serial_puthex(FWSEC_FRTS_CMD, 2);
    serial_puts("\n");

    /* ── Step 6: Start Falcon ── */
    serial_puts("[FWSEC] Step 6: Starting Falcon (FWSEC-FRTS)...\n");
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_STARTCPU);
    wmb();

    /* ── Step 7: Poll mailbox for completion ── */
    serial_puts("[FWSEC] Step 7: Polling mailbox (timeout 2s)...\n");

    uint64_t t0 = rdtsc();
    uint64_t timeout_cycles = 6000000000ULL;  /* ~2s @ 3GHz */
    uint32_t mbox0;
    bool responded = false;

    while (1) {
        rmb();
        mbox0 = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX0);

        /* FWSEC clears or changes mailbox when done */
        if (mbox0 != FWSEC_FRTS_CMD) {
            responded = true;
            break;
        }

        uint64_t elapsed = rdtsc() - t0;
        if (elapsed >= timeout_cycles)
            break;
    }

    uint64_t t1 = rdtsc();
    uint64_t elapsed_ms = (t1 - t0) / 3000000;

    cpuctl = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_CPUCTL);
    uint32_t mbox1 = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX1);

    if (responded) {
        serial_puts("[FWSEC] FRTS completed in ~");
        serial_putdec(elapsed_ms);
        serial_puts(" ms, MAILBOX0=0x");
        serial_puthex(mbox0, 8);
        serial_puts(" MAILBOX1=0x");
        serial_puthex(mbox1, 8);
        serial_puts("\n");
    } else {
        serial_puts("[FWSEC] FRTS TIMEOUT after ");
        serial_putdec(elapsed_ms);
        serial_puts(" ms\n");
        serial_puts("[FWSEC] CPUCTL=0x");
        serial_puthex(cpuctl, 8);
        serial_puts(" MAILBOX0=0x");
        serial_puthex(mbox0, 8);
        serial_puts(" MAILBOX1=0x");
        serial_puthex(mbox1, 8);
        serial_puts("\n");

        if (cpuctl & NV_FALCON_CPUCTL_HALTED)
            serial_puts("[FWSEC] Falcon HALTED — FWSEC may require SEC2 bootstrap\n");
    }

    /* ── Step 8: Check for WPR2 metadata ── */
    serial_puts("[FWSEC] Step 8: Checking for WPR2 metadata...\n");

    /* WPR2 metadata is typically at the end of VRAM minus a page */
    /* Try several known locations: end-4KB, end-1MB, specific offsets */
    uint64_t wpr2_check_offsets[] = {
        vram_bytes - 4096,              /* End of VRAM - 4KB */
        vram_bytes - (1024 * 1024),     /* End of VRAM - 1MB */
        vram_bytes - (2 * 1024 * 1024), /* End of VRAM - 2MB */
    };

    bool wpr2_found = false;
    for (int i = 0; i < 3 && !wpr2_found; i++) {
        uint64_t check_off = wpr2_check_offsets[i];
        if (check_off >= vram_bytes)
            continue;

        gpu_reg_write(NV_PBUS_BAR0_WINDOW, (uint32_t)(check_off >> 16));
        wmb();
        rmb();

        uint32_t pramin_off = (uint32_t)(check_off & (NV_PRAMIN_SIZE - 1));
        uint32_t magic = gpu_reg_read(NV_PRAMIN_BASE + pramin_off);

        if (magic == WPR2_MAGIC) {
            serial_puts("[FWSEC] WPR2 metadata found at VRAM+0x");
            serial_puthex(check_off, 16);
            serial_puts("!\n");

            /* Read WPR meta fields for diagnostics */
            uint32_t rev = gpu_reg_read(NV_PRAMIN_BASE + pramin_off + 4);
            serial_puts("[FWSEC] WPR2 revision: ");
            serial_putdec(rev);
            serial_puts("\n");

            wpr2_found = true;
        }
    }

    if (!wpr2_found)
        serial_puts("[FWSEC] WPR2 metadata not found (expected without full secure boot chain)\n");

    /* Restore PRAMIN window */
    gpu_reg_write(NV_PBUS_BAR0_WINDOW, orig_window);
    wmb();

    /* Halt Falcon — ready for GSP firmware re-boot */
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_HALTED);
    wmb();

    serial_puts("[FWSEC] === FWSEC-FRTS ");
    serial_puts(responded ? "completed" : "timed out");
    serial_puts(wpr2_found ? " — WPR2 active" : " — no WPR2");
    serial_puts(" ===\n");

    fb_puts(" FWSEC: ");
    fb_puts(responded ? "FRTS done" : "FRTS timeout");
    fb_puts(wpr2_found ? ", WPR2 OK" : ", no WPR2");
    fb_puts("\n");

    return responded ? 0 : -1;
}

/* ── X28: GBL-based FWSEC-FRTS Execution ─────────────────────── */
/*
 * Correct Turing+ boot sequence using Generic Bootloader (GBL):
 *  1. Parse FWSEC internal header → extract GBL code offsets
 *  2. Select target Falcon (SEC2 for Turing, GSP for Ampere+)
 *  3. Allocate FWSEC code+data in system RAM (identity-mapped → DMA-accessible)
 *  4. falcon_reset() target Falcon
 *  5. Program FBIF TRANSCFG for system memory DMA access
 *  6. PIO-load GBL microcode to Falcon IMEM
 *  7. Build BootloaderDmemDescV2 (physical addrs of FWSEC code/data in RAM)
 *  8. PIO-load descriptor to Falcon DMEM
 *  9. Boot Falcon — GBL DMA-loads FWSEC, executes FRTS
 * 10. Poll mailbox, check WPR2
 *
 * Reference: nouveau, nova-core PATCH v10 (GBL + BootloaderDmemDescV2)
 */

static int gsp_fwsec_frts_v2(void)
{
    gpu_probe_t *p = gpu_get_probe();
    fwsec_state_t *fw = gpu_get_fwsec();

    if (!p || !p->present) {
        serial_puts("[FWSEC] v2: No GPU detected\n");
        return -1;
    }
    if (!fw || !fw->found || !fw->data || fw->size == 0) {
        serial_puts("[FWSEC] v2: No FWSEC image available\n");
        return -1;
    }

    /* ── Step 1: Parse FWSEC internal header ── */
    if (fw->size < sizeof(falcon_fw_hdr_t)) {
        serial_puts("[FWSEC] v2: FWSEC too small for firmware header\n");
        return -1;
    }

    falcon_fw_hdr_t *hdr = (falcon_fw_hdr_t *)fw->data;

    serial_puts("[FWSEC] v2: === GBL-based FWSEC-FRTS ===\n");
    serial_puts("[FWSEC] v2: FW header: bl_code_off=0x");
    serial_puthex(hdr->bl_code_offset, 8);
    serial_puts(" bl_code_sz=");
    serial_putdec(hdr->bl_code_size);
    serial_puts(" bl_data_off=0x");
    serial_puthex(hdr->bl_data_offset, 8);
    serial_puts(" bl_data_sz=");
    serial_putdec(hdr->bl_data_size);
    serial_puts("\n");

    serial_puts("[FWSEC] v2: os_code_off=0x");
    serial_puthex(hdr->os_code_offset, 8);
    serial_puts(" os_code_sz=");
    serial_putdec(hdr->os_code_size);
    serial_puts(" os_data_off=0x");
    serial_puthex(hdr->os_data_offset, 8);
    serial_puts(" os_data_sz=");
    serial_putdec(hdr->os_data_size);
    serial_puts("\n");

    /* Determine GBL code source: prefer BIT-extracted, fallback to header */
    gbl_state_t *gbl_st = gpu_get_gbl();
    uint8_t *bl_code;
    uint32_t bl_code_size;

    if (gbl_st && gbl_st->found && gbl_st->code_size > 0) {
        bl_code = gbl_st->code;
        bl_code_size = gbl_st->code_size;
        serial_puts("[FWSEC] v2: Using BIT-extracted GBL (");
        serial_putdec(bl_code_size);
        serial_puts(" bytes)\n");
    } else if (hdr->bl_code_size > 0 &&
               hdr->bl_code_offset + hdr->bl_code_size <= fw->size) {
        bl_code = fw->data + hdr->bl_code_offset;
        bl_code_size = hdr->bl_code_size;
        serial_puts("[FWSEC] v2: Using header-embedded GBL (");
        serial_putdec(bl_code_size);
        serial_puts(" bytes)\n");
    } else {
        serial_puts("[FWSEC] v2: No GBL code found (bl_code_size=");
        serial_putdec(hdr->bl_code_size);
        serial_puts(", bl_code_offset=0x");
        serial_puthex(hdr->bl_code_offset, 8);
        serial_puts(")\n");
        return -1;
    }

    /* Validate OS code/data sections */
    uint32_t code_off  = hdr->os_code_offset;
    uint32_t code_size = hdr->os_code_size;
    uint32_t data_off  = hdr->os_data_offset;
    uint32_t data_size = hdr->os_data_size;

    if (code_size == 0 || code_off + code_size > fw->size) {
        serial_puts("[FWSEC] v2: Invalid OS code section\n");
        return -1;
    }
    if (data_size > 0 && data_off + data_size > fw->size) {
        serial_puts("[FWSEC] v2: Invalid OS data section\n");
        return -1;
    }

    /* ── Step 2: Select target Falcon ── */
    extern gpu_device_t gpu_dev;
    gpu_gen_t gen = gpu_dev.generation;
    uint32_t falcon_base;

    if (fw->target_id == FALCON_TARGET_SEC2 || gen == GPU_GEN_TURING) {
        falcon_base = NV_PSEC_BASE;
        serial_puts("[FWSEC] v2: Target = SEC2 Falcon (");
        serial_puts(gpu_gen_name(gen));
        serial_puts(")\n");
        if (!p->sec2_present) {
            serial_puts("[FWSEC] v2: SEC2 Falcon not present!\n");
            return -1;
        }
    } else {
        falcon_base = NV_PGSP_BASE;
        serial_puts("[FWSEC] v2: Target = GSP Falcon (");
        serial_puts(gpu_gen_name(gen));
        serial_puts(")\n");
        if (!p->gsp_present) {
            serial_puts("[FWSEC] v2: GSP Falcon not present!\n");
            return -1;
        }
    }

    /* ── Step 3: Allocate FWSEC code+data in system RAM ── */
    uint32_t total_fw_size = code_size + data_size;
    uint8_t *fw_buf = (uint8_t *)mem_alloc_aligned(total_fw_size, 256);
    if (!fw_buf) {
        serial_puts("[FWSEC] v2: Failed to allocate ");
        serial_putdec(total_fw_size);
        serial_puts(" bytes for FWSEC\n");
        return -1;
    }

    /* Copy code section */
    memcpy(fw_buf, fw->data + code_off, code_size);
    /* Copy data section (contiguous after code) */
    if (data_size > 0)
        memcpy(fw_buf + code_size, fw->data + data_off, data_size);

    uint64_t code_phys = (uint64_t)(uintptr_t)fw_buf;
    uint64_t data_phys = (uint64_t)(uintptr_t)(fw_buf + code_size);

    serial_puts("[FWSEC] v2: FWSEC in RAM at 0x");
    serial_puthex(code_phys, 16);
    serial_puts(" (code=");
    serial_putdec(code_size);
    serial_puts(" + data=");
    serial_putdec(data_size);
    serial_puts(")\n");

    /* ── Step 4: Reset target Falcon ── */
    serial_puts("[FWSEC] v2: Resetting Falcon...\n");
    if (falcon_reset(falcon_base) < 0) {
        serial_puts("[FWSEC] v2: Falcon reset failed\n");
        return -1;
    }

    /* ── Step 5: Program FBIF TRANSCFG ── */
    gpu_reg_write(falcon_base + NV_PFALCON_FBIF_TRANSCFG,
                  FBIF_TRANSCFG_TARGET_COHERENT_SYSMEM);
    wmb();
    serial_puts("[FWSEC] v2: FBIF TRANSCFG = 0x02 (coherent sysmem)\n");

    /* ── Step 6: PIO-load GBL to IMEM ── */
    serial_puts("[FWSEC] v2: PIO loading GBL to IMEM (");
    serial_putdec(bl_code_size);
    serial_puts(" bytes)...\n");

    falcon_pio_load_imem(falcon_base, 0, (const uint32_t *)bl_code, bl_code_size);

    /* Verify first 2 dwords */
    uint32_t imem_verify[2] = {0};
    falcon_pio_read_imem(falcon_base, 0, imem_verify, 8);
    uint32_t *bl_src = (uint32_t *)bl_code;
    if (imem_verify[0] == bl_src[0] && imem_verify[1] == bl_src[1]) {
        serial_puts("[FWSEC] v2: IMEM verify OK (");
        serial_puthex(imem_verify[0], 8);
        serial_puts(" ");
        serial_puthex(imem_verify[1], 8);
        serial_puts(")\n");
    } else {
        serial_puts("[FWSEC] v2: IMEM verify MISMATCH (expected ");
        serial_puthex(bl_src[0], 8);
        serial_puts("/");
        serial_puthex(bl_src[1], 8);
        serial_puts(", got ");
        serial_puthex(imem_verify[0], 8);
        serial_puts("/");
        serial_puthex(imem_verify[1], 8);
        serial_puts(")\n");
        return -1;
    }

    /* ── Step 7: Build BootloaderDmemDescV2 ── */
    bl_dmem_desc_v2_t desc;
    memset(&desc, 0, sizeof(desc));

    desc.signature       = 0x42444456;   /* "VDBD" LE */
    desc.ctx_dma         = 0;            /* Bare-metal: no DMA context */
    desc.code_dma_base   = (uint32_t)(code_phys & 0xFFFFFFFF);
    desc.code_dma_base1  = (uint32_t)(code_phys >> 32);
    desc.non_sec_code_off  = 0;
    desc.non_sec_code_size = code_size;
    desc.sec_code_off    = 0;
    desc.sec_code_size   = code_size;
    desc.code_entry_point = 0;
    desc.data_dma_base   = (uint32_t)(data_phys & 0xFFFFFFFF);
    desc.data_dma_base1  = (uint32_t)(data_phys >> 32);
    desc.data_size       = data_size;
    desc.argc            = 1;
    desc.argv            = FWSEC_FRTS_CMD;

    serial_puts("[FWSEC] v2: DMEM desc: code=0x");
    serial_puthex(code_phys, 16);
    serial_puts(" data=0x");
    serial_puthex(data_phys, 16);
    serial_puts(" argv=0x");
    serial_puthex(FWSEC_FRTS_CMD, 2);
    serial_puts("\n");

    /* ── Step 8: PIO-load descriptor to DMEM ── */
    serial_puts("[FWSEC] v2: PIO loading DMEM descriptor (");
    serial_putdec(sizeof(desc));
    serial_puts(" bytes)...\n");

    falcon_pio_load_dmem(falcon_base, 0, (const uint32_t *)&desc, sizeof(desc));

    /* ── Step 9: Boot Falcon ── */
    gpu_reg_write(falcon_base + NV_FALCON_MAILBOX0, 0);
    wmb();

    serial_puts("[FWSEC] v2: Booting Falcon (GBL → FWSEC → FRTS)...\n");
    falcon_boot(falcon_base, 0);

    /* ── Step 10: Poll mailbox (2s timeout) ── */
    uint64_t t0 = rdtsc();
    uint64_t timeout_cycles = 6000000000ULL;  /* ~2s @ 3GHz */
    uint32_t mbox0;
    bool responded = false;

    /* Wait for mailbox to become non-zero (GBL/FWSEC sets it on completion)
     * or for Falcon to halt (indicates completion or error) */
    while (1) {
        rmb();
        mbox0 = gpu_reg_read(falcon_base + NV_FALCON_MAILBOX0);

        if (mbox0 != 0) {
            responded = true;
            break;
        }

        /* Also check if Falcon halted (FWSEC done or error) */
        uint32_t cpuctl = gpu_reg_read(falcon_base + NV_FALCON_CPUCTL);
        if (cpuctl & NV_FALCON_CPUCTL_HALTED) {
            responded = true;
            break;
        }

        uint64_t elapsed = rdtsc() - t0;
        if (elapsed >= timeout_cycles)
            break;
    }

    uint64_t t1 = rdtsc();
    uint64_t elapsed_ms = (t1 - t0) / 3000000;

    uint32_t final_cpuctl = gpu_reg_read(falcon_base + NV_FALCON_CPUCTL);
    uint32_t mbox1 = gpu_reg_read(falcon_base + NV_FALCON_MAILBOX1);

    serial_puts("[FWSEC] v2: ");
    serial_puts(responded ? "Responded" : "TIMEOUT");
    serial_puts(" after ~");
    serial_putdec(elapsed_ms);
    serial_puts(" ms\n");
    serial_puts("[FWSEC] v2: CPUCTL=0x");
    serial_puthex(final_cpuctl, 8);
    serial_puts(" MAILBOX0=0x");
    serial_puthex(mbox0, 8);
    serial_puts(" MAILBOX1=0x");
    serial_puthex(mbox1, 8);
    serial_puts("\n");

    /* mailbox0 == 0 after execution typically indicates success */
    bool exec_ok = responded && (mbox0 == 0);
    if (exec_ok) {
        serial_puts("[FWSEC] v2: FWSEC execution appears successful (mailbox0=0)\n");
    } else if (responded) {
        serial_puts("[FWSEC] v2: FWSEC returned non-zero mailbox (error or status)\n");
    }

    /* ── Step 11: Check for WPR2 metadata ── */
    serial_puts("[FWSEC] v2: Checking WPR2...\n");
    uint64_t vram_bytes = (uint64_t)p->vram_size_mb * 1024 * 1024;
    uint32_t orig_window = gpu_reg_read(NV_PBUS_BAR0_WINDOW);

    uint64_t wpr2_check_offsets[] = {
        vram_bytes - 4096,
        vram_bytes - (1024 * 1024),
        vram_bytes - (2 * 1024 * 1024),
    };

    bool wpr2_found = false;
    for (int i = 0; i < 3 && !wpr2_found; i++) {
        uint64_t check_off = wpr2_check_offsets[i];
        if (check_off >= vram_bytes)
            continue;

        gpu_reg_write(NV_PBUS_BAR0_WINDOW, (uint32_t)(check_off >> 16));
        wmb();
        rmb();

        uint32_t pramin_off = (uint32_t)(check_off & (NV_PRAMIN_SIZE - 1));
        uint32_t magic = gpu_reg_read(NV_PRAMIN_BASE + pramin_off);

        if (magic == WPR2_MAGIC) {
            serial_puts("[FWSEC] v2: WPR2 found at VRAM+0x");
            serial_puthex(check_off, 16);
            serial_puts("!\n");

            uint32_t rev = gpu_reg_read(NV_PRAMIN_BASE + pramin_off + 4);
            serial_puts("[FWSEC] v2: WPR2 revision: ");
            serial_putdec(rev);
            serial_puts("\n");

            wpr2_found = true;
        }
    }

    if (!wpr2_found)
        serial_puts("[FWSEC] v2: WPR2 not found (may require full SEC2 bootstrap)\n");

    /* ── Step 12: Restore and halt ── */
    gpu_reg_write(NV_PBUS_BAR0_WINDOW, orig_window);
    wmb();

    gpu_reg_write(falcon_base + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_HALTED);
    wmb();

    serial_puts("[FWSEC] v2: === GBL FWSEC-FRTS ");
    serial_puts(responded ? "completed" : "timed out");
    serial_puts(wpr2_found ? " — WPR2 active" : " — no WPR2");
    serial_puts(" ===\n");

    fb_puts(" FWSEC-v2: ");
    fb_puts(responded ? "GBL done" : "GBL timeout");
    fb_puts(wpr2_found ? ", WPR2 OK" : ", no WPR2");
    fb_puts("\n");

    return responded ? 0 : -1;
}

/* ── Public FWSEC-FRTS: try GBL v2 first, fallback to legacy ── */

int gsp_fwsec_frts(void)
{
    serial_puts("[FWSEC] Attempting GBL-based execution (v2)...\n");
    int ret = gsp_fwsec_frts_v2();

    if (ret < 0) {
        serial_puts("[FWSEC] v2 failed, falling back to legacy...\n");
        ret = gsp_fwsec_frts_legacy();
    }

    return ret;
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

/* ── X30: Two-stage boot via bootloader + radix3 ── */

static int gsp_boot_v2(void)
{
    gpu_probe_t *p = gpu_get_probe();

    if (!bl_data || bl_size == 0) {
        serial_puts("[GSP] Boot v2: no bootloader extracted\n");
        return -1;
    }
    if (!radix3.built) {
        serial_puts("[GSP] Boot v2: radix3 not built\n");
        return -1;
    }

    serial_puts("[GSP] Boot v2: === Two-stage bootloader boot ===\n");

    /* ── Step 1: Reset GSP Falcon ── */
    serial_puts("[GSP] Boot v2: resetting GSP Falcon...\n");
    if (falcon_reset(NV_PGSP_BASE) < 0) {
        serial_puts("[GSP] Boot v2: reset failed\n");
        return -1;
    }

    /* ── Step 2: DMA-load bootloader to GSP IMEM ── */
    uint64_t bl_phys = (uint64_t)(uintptr_t)bl_data;

    serial_puts("[GSP] Boot v2: DMA loading bootloader (");
    serial_putdec(bl_size);
    serial_puts(" bytes) to GSP IMEM...\n");

    if (falcon_dma_load(NV_PGSP_BASE, bl_phys, 0, bl_size, true) < 0) {
        serial_puts("[GSP] Boot v2: DMA load to IMEM failed\n");
        return -1;
    }

    /* ── Step 3: Load bootloader data to DMEM (if present) ── */
    if (bl_desc.bootloader_param_size > 0 &&
        bl_desc.bootloader_param_offset + bl_desc.bootloader_param_size <= gsp.fw_size) {
        uint8_t *param_data = (uint8_t *)gsp.fw_data + bl_desc.bootloader_param_offset;
        uint64_t param_phys = (uint64_t)(uintptr_t)param_data;

        serial_puts("[GSP] Boot v2: DMA loading BL params (");
        serial_putdec(bl_desc.bootloader_param_size);
        serial_puts(" bytes) to DMEM...\n");

        if (falcon_dma_load(NV_PGSP_BASE, param_phys, 0,
                            bl_desc.bootloader_param_size, false) < 0) {
            serial_puts("[GSP] Boot v2: DMA load to DMEM failed\n");
            return -1;
        }
    }

    /* ── Step 4: Write queue init args to VRAM ── */
    if (gsp.queues_ready)
        gsp_queue_write_args();

    /* ── Step 5: Set BOOTVEC to bootloader entry (0) ── */
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_BOOTVEC, 0);
    wmb();

    /* ── Step 6: Set MAILBOX0/1 to WPR metadata address in VRAM ── */
    uint64_t vram_bytes = (uint64_t)p->vram_size_mb * 1024 * 1024;
    uint64_t wpr_meta_addr = vram_bytes - WPR_META_VRAM_OFFSET_FROM_END;

    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_MAILBOX0,
                  (uint32_t)(wpr_meta_addr & 0xFFFFFFFF));
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_MAILBOX1,
                  (uint32_t)(wpr_meta_addr >> 32));
    wmb();

    serial_puts("[GSP] Boot v2: BOOTVEC=0, MAILBOX0/1=0x");
    serial_puthex(wpr_meta_addr, 16);
    serial_puts(" (WPR meta)\n");

    /* ── Step 7: Start CPU ── */
    serial_puts("[GSP] Boot v2: starting GSP...\n");
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_STARTCPU);
    wmb();
    gsp.booted = true;

    /* ── Step 8: Poll mailbox (2s timeout) ── */
    uint64_t t0 = rdtsc();
    uint64_t timeout_cycles = 6000000000ULL;  /* ~2s @ 3GHz */
    uint32_t mbox0_initial = (uint32_t)(wpr_meta_addr & 0xFFFFFFFF);
    uint32_t mbox0 = mbox0_initial;
    bool responded = false;

    while (1) {
        rmb();
        mbox0 = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX0);
        if (mbox0 != mbox0_initial) {
            responded = true;
            break;
        }

        /* Also check if Falcon halted (bootloader done or error) */
        uint32_t cpuctl = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_CPUCTL);
        if ((cpuctl & NV_FALCON_CPUCTL_HALTED) && mbox0 != mbox0_initial) {
            responded = true;
            break;
        }

        if (rdtsc() - t0 >= timeout_cycles)
            break;
    }

    uint64_t elapsed_ms = (rdtsc() - t0) / 3000000;
    uint32_t final_cpuctl = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_CPUCTL);
    uint32_t mbox1 = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX1);

    serial_puts("[GSP] Boot v2: ");
    serial_puts(responded ? "responded" : "TIMEOUT");
    serial_puts(" after ~");
    serial_putdec(elapsed_ms);
    serial_puts(" ms\n");
    serial_puts("[GSP] Boot v2: CPUCTL=0x");
    serial_puthex(final_cpuctl, 8);
    serial_puts(" MBOX0=0x");
    serial_puthex(mbox0, 8);
    serial_puts(" MBOX1=0x");
    serial_puthex(mbox1, 8);
    serial_puts("\n");

    gsp.boot_status = mbox0;
    gsp.boot_ack = responded;

    if (responded) {
        serial_puts("[GSP] Boot v2: GSP bootloader responded!\n");
        fb_puts(" GSP boot v2: OK mbox=0x");
        fb_puthex(mbox0, 8);
        fb_puts("\n");
    } else {
        serial_puts("[GSP] Boot v2: timeout — may need SEC2 booter chain\n");
        fb_puts(" GSP boot v2: timeout\n");
    }

    serial_puts("[GSP] Boot v2: === end ===\n");
    return responded ? 0 : -1;
}

/* ── Legacy direct boot (X20 original) ── */

static int gsp_boot_legacy(void)
{
    /* Pre-conditions */
    if (!gsp.fw_uploaded) {
        serial_puts("[GSP] Legacy boot: firmware not in VRAM\n");
        return -1;
    }
    if (gsp.elf_entry == 0) {
        serial_puts("[GSP] Legacy boot: ELF entry point is 0\n");
        return -1;
    }

    serial_puts("[GSP] Legacy boot: direct DMATRFBASE+BOOTVEC...\n");

    /* ── Step 1: Halt Falcon ── */
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_HALTED);
    wmb();

    /* ── Step 2: Write queue init args to VRAM ── */
    if (gsp.queues_ready)
        gsp_queue_write_args();

    /* ── Step 3: Set mailboxes to shared memory address ── */
    if (gsp.queues_ready) {
        gpu_reg_write(NV_PGSP_BASE + NV_FALCON_MAILBOX0,
                      (uint32_t)(gsp.shm_phys & 0xFFFFFFFF));
        gpu_reg_write(NV_PGSP_BASE + NV_FALCON_MAILBOX1,
                      (uint32_t)(gsp.shm_phys >> 32));
    } else {
        gpu_reg_write(NV_PGSP_BASE + NV_FALCON_MAILBOX0, 0);
        gpu_reg_write(NV_PGSP_BASE + NV_FALCON_MAILBOX1, 0);
    }
    wmb();

    /* ── Step 4: Set DMATRFBASE ── */
    uint32_t dma_base = (uint32_t)(gsp.vram_offset >> 8);
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_DMATRFBASE, dma_base);
    wmb();

    /* ── Step 5: Set BOOTVEC ── */
    uint32_t bootvec = (uint32_t)(gsp.elf_entry >> 8);
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_BOOTVEC, bootvec);
    wmb();

    serial_puts("[GSP] Legacy: DMATRFBASE=0x");
    serial_puthex(dma_base, 8);
    serial_puts(" BOOTVEC=0x");
    serial_puthex(bootvec, 8);
    serial_puts("\n");

    /* ── Step 6: Start CPU ── */
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_STARTCPU);
    wmb();
    gsp.booted = true;

    /* ── Step 7: Poll mailbox (timeout ~1 second @ 3GHz) ── */
    serial_puts("[GSP] Legacy: polling mailbox (timeout 1s)...\n");

    uint32_t mbox0_initial = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX0);
    uint64_t t0 = rdtsc();
    uint64_t timeout_cycles = 3000000000ULL;  /* ~1s @ 3GHz */
    uint32_t mbox0 = mbox0_initial;

    while (1) {
        rmb();
        mbox0 = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX0);
        if (mbox0 != mbox0_initial)
            break;
        if (rdtsc() - t0 >= timeout_cycles)
            break;
    }

    uint64_t elapsed_ms = (rdtsc() - t0) / 3000000;
    gsp.boot_status = mbox0;
    gsp.boot_ack = (mbox0 != mbox0_initial);

    serial_puts("[GSP] Legacy: ");
    serial_puts(gsp.boot_ack ? "responded" : "TIMEOUT");
    serial_puts(" after ~");
    serial_putdec(elapsed_ms);
    serial_puts(" ms, MBOX0=0x");
    serial_puthex(mbox0, 8);
    serial_puts("\n");

    if (gsp.boot_ack) {
        fb_puts(" GSP legacy: OK\n");
    } else {
        uint32_t cpuctl = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_CPUCTL);
        serial_puts("[GSP] Legacy: CPUCTL=0x");
        serial_puthex(cpuctl, 8);
        serial_puts("\n");
        fb_puts(" GSP legacy: timeout\n");
    }

    return gsp.boot_ack ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════
 *  X31: SEC2 Booter Load
 *
 *  The SEC2 falcon runs a "booter_load" firmware that:
 *  1. Reads GspFwWprMeta from address in MAILBOX0/1
 *  2. Walks radix3 page tables to find firmware pages
 *  3. DMA-copies firmware into WPR2 protected VRAM region
 *  4. Validates cryptographic signatures
 *  5. Sets up GSP boot environment
 *
 *  The booter is a separate firmware blob ("booter.bin") from
 *  the NVIDIA firmware package, loaded from OsitoFS.
 *
 *  Reference: nouveau tu102_gsp_booter_load(), nova-core run_booter()
 * ══════════════════════════════════════════════════════════ */

static int gsp_load_booter(void)
{
    if (!osfs2_is_mounted()) {
        serial_puts("[SEC2] OsitoFS not mounted, cannot load booter\n");
        return -1;
    }

    osfs2_file_t *file = osfs2_find("booter.bin");
    if (!file) {
        serial_puts("[SEC2] booter.bin not found in OsitoFS (optional)\n");
        return -1;
    }

    /* Validate size: min 4KB, max 4MB */
    if (file->size < 4096) {
        serial_puts("[SEC2] booter.bin too small (");
        serial_putdec(file->size);
        serial_puts(" bytes)\n");
        return -1;
    }
    if (file->size > 4ULL * 1024 * 1024) {
        serial_puts("[SEC2] booter.bin too large (");
        serial_putdec(file->size / 1024);
        serial_puts(" KB, max 4MB)\n");
        return -1;
    }

    serial_puts("[SEC2] Loading booter.bin (");
    serial_putdec(file->size);
    serial_puts(" bytes)...\n");

    gsp.booter_data = mem_alloc_aligned(file->size, 4096);
    if (!gsp.booter_data) {
        serial_puts("[SEC2] Failed to allocate booter buffer\n");
        return -1;
    }

    gsp.booter_size = file->size;

    /* Read in chunks */
    uint64_t offset = 0;
    uint64_t remaining = file->size;

    while (remaining > 0) {
        uint64_t chunk = remaining < OSFS2_BLOCK_SIZE ? remaining : OSFS2_BLOCK_SIZE;
        if (osfs2_read(file, offset, (uint8_t *)gsp.booter_data + offset, chunk) < 0) {
            serial_puts("[SEC2] Read failed at offset ");
            serial_puthex(offset, 8);
            serial_puts("\n");
            return -1;
        }
        offset += chunk;
        remaining -= chunk;
    }

    serial_puts("[SEC2] Booter loaded to RAM at 0x");
    serial_puthex((uint64_t)(uintptr_t)gsp.booter_data, 16);
    serial_puts("\n");

    gsp.booter_loaded = true;
    return 0;
}

static int gsp_sec2_booter(void)
{
    gpu_probe_t *p = gpu_get_probe();

    if (!p || !p->sec2_present) {
        serial_puts("[SEC2] SEC2 falcon not present\n");
        return -1;
    }
    if (!gsp.booter_loaded || !gsp.booter_data || gsp.booter_size == 0) {
        serial_puts("[SEC2] No booter firmware loaded\n");
        return -1;
    }
    if (!radix3.built) {
        serial_puts("[SEC2] Radix3 not built, cannot run booter\n");
        return -1;
    }

    serial_puts("[SEC2] === SEC2 Booter Load ===\n");

    /* ── Step 1: Reset SEC2 Falcon ── */
    serial_puts("[SEC2] Resetting SEC2 Falcon...\n");
    if (falcon_reset(NV_PSEC_BASE) < 0) {
        serial_puts("[SEC2] SEC2 reset failed\n");
        return -1;
    }

    /* ── Step 2: DMA-load booter to SEC2 IMEM ── */
    uint64_t booter_phys = (uint64_t)(uintptr_t)gsp.booter_data;

    serial_puts("[SEC2] DMA loading booter (");
    serial_putdec(gsp.booter_size);
    serial_puts(" bytes) to SEC2 IMEM...\n");

    if (falcon_dma_load(NV_PSEC_BASE, booter_phys, 0,
                        (uint32_t)gsp.booter_size, true) < 0) {
        serial_puts("[SEC2] DMA load to SEC2 IMEM failed\n");
        return -1;
    }

    /* ── Step 3: Set BOOTVEC to 0 ── */
    gpu_reg_write(NV_PSEC_BASE + NV_FALCON_BOOTVEC, 0);
    wmb();

    /* ── Step 4: Set MAILBOX0/1 to WPR metadata physical address ── */
    uint64_t vram_bytes = (uint64_t)p->vram_size_mb * 1024 * 1024;
    uint64_t wpr_meta_addr = vram_bytes - WPR_META_VRAM_OFFSET_FROM_END;

    gpu_reg_write(NV_PSEC_BASE + NV_FALCON_MAILBOX0,
                  (uint32_t)(wpr_meta_addr & 0xFFFFFFFF));
    gpu_reg_write(NV_PSEC_BASE + NV_FALCON_MAILBOX1,
                  (uint32_t)(wpr_meta_addr >> 32));
    wmb();

    serial_puts("[SEC2] BOOTVEC=0, MAILBOX0/1=0x");
    serial_puthex(wpr_meta_addr, 16);
    serial_puts(" (WPR meta)\n");

    /* ── Step 5: Start SEC2 ── */
    serial_puts("[SEC2] Starting SEC2 Falcon...\n");
    gpu_reg_write(NV_PSEC_BASE + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_STARTCPU);
    wmb();

    /* ── Step 6: Poll for completion (3s timeout) ── */
    /* SEC2 booter sets MAILBOX0 = 0 on success, nonzero on error.
     * It also halts when done. We check both. */
    uint64_t t0 = rdtsc();
    uint64_t timeout_cycles = 9000000000ULL;  /* ~3s @ 3GHz */
    bool completed = false;
    uint32_t mbox0 = (uint32_t)(wpr_meta_addr & 0xFFFFFFFF);

    while (1) {
        rmb();
        uint32_t cpuctl = gpu_reg_read(NV_PSEC_BASE + NV_FALCON_CPUCTL);

        if (cpuctl & NV_FALCON_CPUCTL_HALTED) {
            mbox0 = gpu_reg_read(NV_PSEC_BASE + NV_FALCON_MAILBOX0);
            completed = true;
            break;
        }

        /* Also check if mailbox changed from initial value */
        mbox0 = gpu_reg_read(NV_PSEC_BASE + NV_FALCON_MAILBOX0);
        if (mbox0 != (uint32_t)(wpr_meta_addr & 0xFFFFFFFF)) {
            completed = true;
            break;
        }

        if (rdtsc() - t0 >= timeout_cycles)
            break;
    }

    uint64_t elapsed_ms = (rdtsc() - t0) / 3000000;
    uint32_t final_cpuctl = gpu_reg_read(NV_PSEC_BASE + NV_FALCON_CPUCTL);
    uint32_t mbox1 = gpu_reg_read(NV_PSEC_BASE + NV_FALCON_MAILBOX1);

    serial_puts("[SEC2] ");
    serial_puts(completed ? "Completed" : "TIMEOUT");
    serial_puts(" after ~");
    serial_putdec(elapsed_ms);
    serial_puts(" ms\n");
    serial_puts("[SEC2] CPUCTL=0x");
    serial_puthex(final_cpuctl, 8);
    serial_puts(" MBOX0=0x");
    serial_puthex(mbox0, 8);
    serial_puts(" MBOX1=0x");
    serial_puthex(mbox1, 8);
    serial_puts("\n");

    /* MAILBOX0 == 0 indicates success (booter loaded firmware into WPR2) */
    bool success = completed && (mbox0 == 0);

    if (success) {
        serial_puts("[SEC2] Booter succeeded — GSP firmware loaded into WPR2\n");
        fb_puts(" SEC2 booter: OK\n");
    } else if (completed) {
        serial_puts("[SEC2] Booter completed with error (mbox0=0x");
        serial_puthex(mbox0, 8);
        serial_puts(")\n");
        fb_puts(" SEC2 booter: err 0x");
        fb_puthex(mbox0, 8);
        fb_puts("\n");
    } else {
        serial_puts("[SEC2] Booter timed out — may need FWSEC-SB authentication\n");
        fb_puts(" SEC2 booter: timeout\n");
    }

    gsp.sec2_boot_ok = success;
    serial_puts("[SEC2] === end ===\n");

    return success ? 0 : -1;
}

/* ── gsp_boot: full chain dispatcher ── */

int gsp_boot(void)
{
    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present || !p->gsp_present) {
        return -1;
    }

    /* Parse ELF from firmware in RAM */
    if (gsp_parse_elf() < 0) {
        serial_puts("[GSP] ELF parse failed, skipping boot\n");
        return -1;
    }

    /* ── Pre-boot: FWSEC-FRTS (create WPR2 before GSP firmware boot) ── */
    gsp_fwsec_frts();

    /* ── X29: Build radix3 PTs + extract bootloader + write WPR meta ── */
    gsp_build_radix3();
    gsp_extract_bootloader();
    gsp_write_wpr_meta();

    /* ── X31: SEC2 booter (loads firmware into WPR2 via radix3) ── */
    gsp_load_booter();
    if (gsp.booter_loaded)
        gsp_sec2_booter();

    /* ── Boot GSP: try v2 (bootloader via DMA), fallback to legacy ── */
    int ret = gsp_boot_v2();

    if (ret < 0) {
        serial_puts("[GSP] Boot v2 failed, falling back to legacy boot...\n");
        ret = gsp_boot_legacy();
    }

    /* ── Post-boot: RPC init sequence ── */
    if (gsp.queues_ready) {
        gsp_rpc_init();
        gsp_rm_init();
        gsp_channel_init();
        gsp_compute_init();
        gsp_ce_init();
    }

    /* X38: GMMU page tables (identity-map VRAM for compute) */
    gmmu_init();

    /* X37: SASS kernel infrastructure (works with or without GSP boot) */
    sass_init();

    /* X39: GPU tensor operations (dispatch layer + self-test) */
    gpu_tensor_init();

    return ret;
}

/* ══════════════════════════════════════════════════════════
 *  X27: Falcon PIO Load
 *
 *  Programmed I/O write/read to Falcon IMEM/DMEM via IMEMC/IMEMD
 *  and DMEMC/DMEMD registers. Used to load small bootstrap code
 *  (Generic Bootloader) that then DMA-loads larger payloads.
 *
 *  Register format (envytools, nova-core):
 *    Control (IMEMC/DMEMC):
 *      bits 15:0 = byte address (must be 4-byte aligned)
 *      bit 24    = auto-increment on write
 *      bit 25    = auto-increment on read
 *    Data (IMEMD/DMEMD):
 *      Write/read dwords sequentially, address auto-increments.
 *
 *  Reference: nouveau nvkm_falcon_pio_wr(), nova-core Falcon::pio_wr()
 * ══════════════════════════════════════════════════════════ */

void falcon_pio_load_imem(uint32_t base, uint32_t dst,
                          const uint32_t *data, uint32_t size)
{
    /* Set IMEMC: destination byte address | auto-increment on write */
    gpu_reg_write(base + NV_FALCON_IMEMC, (dst & 0xFFFC) | (1 << 24));
    wmb();

    uint32_t dwords = size / 4;
    for (uint32_t i = 0; i < dwords; i++)
        gpu_reg_write(base + NV_FALCON_IMEMD, data[i]);
    wmb();
}

void falcon_pio_load_dmem(uint32_t base, uint32_t dst,
                          const uint32_t *data, uint32_t size)
{
    /* Set DMEMC: destination byte address | auto-increment on write */
    gpu_reg_write(base + NV_FALCON_DMEMC, (dst & 0xFFFC) | (1 << 24));
    wmb();

    uint32_t dwords = size / 4;
    for (uint32_t i = 0; i < dwords; i++)
        gpu_reg_write(base + NV_FALCON_DMEMD, data[i]);
    wmb();
}

void falcon_pio_read_imem(uint32_t base, uint32_t src,
                          uint32_t *buf, uint32_t size)
{
    /* Set IMEMC: source byte address | auto-increment on read */
    gpu_reg_write(base + NV_FALCON_IMEMC, (src & 0xFFFC) | (1 << 25));
    wmb();
    rmb();

    uint32_t dwords = size / 4;
    for (uint32_t i = 0; i < dwords; i++)
        buf[i] = gpu_reg_read(base + NV_FALCON_IMEMD);
}

void falcon_pio_read_dmem(uint32_t base, uint32_t src,
                          uint32_t *buf, uint32_t size)
{
    /* Set DMEMC: source byte address | auto-increment on read */
    gpu_reg_write(base + NV_FALCON_DMEMC, (src & 0xFFFC) | (1 << 25));
    wmb();
    rmb();

    uint32_t dwords = size / 4;
    for (uint32_t i = 0; i < dwords; i++)
        buf[i] = gpu_reg_read(base + NV_FALCON_DMEMD);
}

int falcon_reset(uint32_t base)
{
    /* Halt the Falcon */
    gpu_reg_write(base + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_HALTED);
    wmb();

    /* Clear mailboxes */
    gpu_reg_write(base + NV_FALCON_MAILBOX0, 0);
    gpu_reg_write(base + NV_FALCON_MAILBOX1, 0);
    wmb();

    /* Verify halted */
    rmb();
    uint32_t cpuctl = gpu_reg_read(base + NV_FALCON_CPUCTL);
    if (!(cpuctl & NV_FALCON_CPUCTL_HALTED)) {
        serial_puts("[FALCON] Reset: not halted (CPUCTL=0x");
        serial_puthex(cpuctl, 8);
        serial_puts(")\n");
        return -1;
    }

    return 0;
}

int falcon_boot(uint32_t base, uint32_t boot_addr)
{
    /* Set boot vector (byte address >> 8 for some Falcon versions,
     * or direct byte address — depends on firmware. Use raw value
     * and let caller decide the encoding.) */
    gpu_reg_write(base + NV_FALCON_BOOTVEC, boot_addr);
    wmb();

    /* Start CPU */
    gpu_reg_write(base + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_STARTCPU);
    wmb();

    return 0;
}

int falcon_pio_selftest(uint32_t base)
{
    gpu_probe_t *p = gpu_get_probe();
    if (!p || !p->present || !p->gsp_present) {
        serial_puts("[FALCON] PIO self-test: no GPU/GSP\n");
        return -1;
    }

    serial_puts("[FALCON] PIO self-test on base 0x");
    serial_puthex(base, 6);
    serial_puts("...\n");

    /* Halt first */
    if (falcon_reset(base) < 0)
        return -1;

    /* Read HWCFG2 to get DMEM size */
    uint32_t hwcfg2 = gpu_reg_read(base + NV_FALCON_HWCFG2);
    uint32_t dmem_size = ((hwcfg2 & NV_FALCON_HWCFG2_DMEM_MASK) >>
                           NV_FALCON_HWCFG2_DMEM_SHIFT) * 256;

    if (dmem_size == 0) {
        serial_puts("[FALCON] DMEM size = 0, cannot test PIO\n");
        return -1;
    }

    serial_puts("[FALCON] DMEM size: ");
    serial_putdec(dmem_size);
    serial_puts(" bytes\n");

    /* Test: write pattern to DMEM, read back and verify */
    uint32_t test_data[4] = { 0xDEADBEEF, 0x05170000, 0xCAFEBABE, 0x12345678 };
    uint32_t read_buf[4]  = { 0 };

    /* Write 16 bytes at DMEM offset 0 */
    falcon_pio_load_dmem(base, 0, test_data, 16);

    /* Read back */
    falcon_pio_read_dmem(base, 0, read_buf, 16);

    int ok = 1;
    for (int i = 0; i < 4; i++) {
        if (read_buf[i] != test_data[i]) {
            serial_puts("[FALCON] DMEM PIO mismatch at dword ");
            serial_putdec(i);
            serial_puts(": wrote 0x");
            serial_puthex(test_data[i], 8);
            serial_puts(" read 0x");
            serial_puthex(read_buf[i], 8);
            serial_puts("\n");
            ok = 0;
        }
    }

    /* Restore DMEM: write zeros to test area */
    uint32_t zeros[4] = { 0 };
    falcon_pio_load_dmem(base, 0, zeros, 16);

    if (ok) {
        serial_puts("[FALCON] PIO self-test: PASS\n");
        fb_puts(" Falcon PIO: OK\n");
    } else {
        serial_puts("[FALCON] PIO self-test: FAIL\n");
        fb_puts(" Falcon PIO: FAIL\n");
    }

    return ok ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════
 *  X30: Falcon DMA Load
 *
 *  Load firmware from system RAM to Falcon IMEM/DMEM using the
 *  Falcon's DMA engine (DMATRFBASE/DMATRFCMD). Transfers in
 *  256-byte chunks with idle polling between each.
 *
 *  Reference: nova-core falcon.load(), nouveau nvkm_falcon_load()
 * ══════════════════════════════════════════════════════════ */

int falcon_dma_load(uint32_t base, uint64_t src_phys,
                    uint32_t dst_off, uint32_t size, bool to_imem)
{
    if (size == 0) return 0;

    /* Program FBIF TRANSCFG for coherent system memory DMA */
    gpu_reg_write(base + NV_PFALCON_FBIF_TRANSCFG,
                  FBIF_TRANSCFG_TARGET_COHERENT_SYSMEM);
    wmb();

    /* Set DMA base address (physical >> 8) */
    gpu_reg_write(base + NV_FALCON_DMATRFBASE,
                  (uint32_t)((src_phys >> 8) & 0xFFFFFFFF));
    gpu_reg_write(base + NV_FALCON_DMATRFBASE1,
                  (uint32_t)(src_phys >> 40));
    wmb();

    /* Transfer in 256-byte chunks */
    uint32_t cmd_flags = DMATRFCMD_SIZE_256B;
    if (to_imem)
        cmd_flags |= DMATRFCMD_IMEM;

    uint32_t chunks = (size + 255) / 256;

    for (uint32_t i = 0; i < chunks; i++) {
        uint32_t chunk_off = i * 256;

        /* Destination offset in falcon IMEM/DMEM */
        gpu_reg_write(base + NV_FALCON_DMATRFMOFFS, dst_off + chunk_off);
        /* Source offset relative to DMATRFBASE */
        gpu_reg_write(base + NV_FALCON_DMATRFFBOFFS, chunk_off);
        wmb();

        /* Trigger DMA transfer */
        gpu_reg_write(base + NV_FALCON_DMATRFCMD, cmd_flags);
        wmb();

        /* Poll for idle (timeout ~10ms per chunk @ 3GHz) */
        uint64_t t0 = rdtsc();
        uint64_t timeout = 30000000ULL;  /* ~10ms @ 3GHz */
        bool idle = false;

        while (1) {
            rmb();
            uint32_t cmd = gpu_reg_read(base + NV_FALCON_DMATRFCMD);
            if (cmd & DMATRFCMD_IDLE) {
                idle = true;
                break;
            }
            if (rdtsc() - t0 >= timeout)
                break;
        }

        if (!idle) {
            serial_puts("[FALCON] DMA load timeout at chunk ");
            serial_putdec(i);
            serial_puts("/");
            serial_putdec(chunks);
            serial_puts("\n");
            return -1;
        }
    }

    serial_puts("[FALCON] DMA load: ");
    serial_putdec(size);
    serial_puts(" bytes (");
    serial_putdec(chunks);
    serial_puts(" chunks) to ");
    serial_puts(to_imem ? "IMEM" : "DMEM");
    serial_puts("+0x");
    serial_puthex(dst_off, 4);
    serial_puts(" from phys 0x");
    serial_puthex(src_phys, 16);
    serial_puts("\n");

    return 0;
}

/* ══════════════════════════════════════════════════════════
 *  X29: Radix3 Page Tables + WPR Metadata + Bootloader
 *
 *  Build 3-level page tables mapping firmware at GSP virtual
 *  address 0, extract bootloader descriptor from gsp.bin,
 *  and write GspFwWprMeta to VRAM for the GSP bootloader.
 *
 *  Reference: nouveau nvkm_gsp_radix3_sg, nova-core map_into_lvl()
 * ══════════════════════════════════════════════════════════ */

int gsp_build_radix3(void)
{
    if (!gsp.fw_data || gsp.fw_size == 0) {
        serial_puts("[GSP] Radix3: no firmware data\n");
        return -1;
    }

    /* Calculate page counts */
    uint32_t fw_size = (uint32_t)gsp.fw_size;
    radix3.num_fw_pages = (fw_size + RADIX3_PAGE_SIZE - 1) / RADIX3_PAGE_SIZE;
    radix3.num_l2_pages = (radix3.num_fw_pages + RADIX3_PTES_PER_PAGE - 1)
                          / RADIX3_PTES_PER_PAGE;
    radix3.num_l1_pages = (radix3.num_l2_pages + RADIX3_PTES_PER_PAGE - 1)
                          / RADIX3_PTES_PER_PAGE;

    serial_puts("[GSP] Radix3: ");
    serial_putdec(radix3.num_fw_pages);
    serial_puts(" fw pages, ");
    serial_putdec(radix3.num_l2_pages);
    serial_puts(" L2 pages, ");
    serial_putdec(radix3.num_l1_pages);
    serial_puts(" L1 pages\n");

    /* Allocate page-aligned page table levels */
    uint32_t l0_size = RADIX3_PAGE_SIZE;
    uint32_t l1_size = radix3.num_l1_pages * RADIX3_PAGE_SIZE;
    uint32_t l2_size = radix3.num_l2_pages * RADIX3_PAGE_SIZE;

    radix3.lvl0 = (uint64_t *)mem_alloc_aligned(l0_size, RADIX3_PAGE_SIZE);
    radix3.lvl1 = (uint64_t *)mem_alloc_aligned(l1_size, RADIX3_PAGE_SIZE);
    radix3.lvl2 = (uint64_t *)mem_alloc_aligned(l2_size, RADIX3_PAGE_SIZE);

    if (!radix3.lvl0 || !radix3.lvl1 || !radix3.lvl2) {
        serial_puts("[GSP] Radix3: allocation failed\n");
        return -1;
    }

    memset(radix3.lvl0, 0, l0_size);
    memset(radix3.lvl1, 0, l1_size);
    memset(radix3.lvl2, 0, l2_size);

    /* Fill L2: each entry = DMA physical addr of a 4KB firmware page */
    uint8_t *fw = (uint8_t *)gsp.fw_data;
    for (uint32_t i = 0; i < radix3.num_fw_pages; i++)
        radix3.lvl2[i] = (uint64_t)(uintptr_t)(fw + i * RADIX3_PAGE_SIZE);

    /* Fill L1: each entry = DMA addr of a L2 page */
    for (uint32_t i = 0; i < radix3.num_l2_pages; i++)
        radix3.lvl1[i] = (uint64_t)(uintptr_t)(&radix3.lvl2[i * RADIX3_PTES_PER_PAGE]);

    /* Fill L0: single entry = DMA addr of L1 */
    radix3.lvl0[0] = (uint64_t)(uintptr_t)radix3.lvl1;

    radix3.built = true;

    uint32_t total_pt_bytes = l0_size + l1_size + l2_size;
    serial_puts("[GSP] Radix3: L0=0x");
    serial_puthex((uint64_t)(uintptr_t)radix3.lvl0, 16);
    serial_puts(" L1=0x");
    serial_puthex((uint64_t)(uintptr_t)radix3.lvl1, 16);
    serial_puts(" L2=0x");
    serial_puthex((uint64_t)(uintptr_t)radix3.lvl2, 16);
    serial_puts("\n");
    serial_puts("[GSP] Radix3: total PT size = ");
    serial_putdec(total_pt_bytes);
    serial_puts(" bytes (");
    serial_putdec(total_pt_bytes / 4096);
    serial_puts(" pages)\n");

    fb_puts(" Radix3: ");
    fb_putdec(radix3.num_fw_pages);
    fb_puts(" pages OK\n");

    return 0;
}

int gsp_extract_bootloader(void)
{
    if (!gsp.fw_data || gsp.fw_size < sizeof(elf64_ehdr_t)) {
        serial_puts("[GSP] Bootloader: no firmware data\n");
        return -1;
    }

    /* The RmRiscvUCodeDesc is at the end of the ELF file.
     * Nouveau locates it via: fw_size - sizeof(rm_riscv_ucode_desc_t).
     * Reference: nouveau nvkm_gsp_fwsec_sb(), nova-core gsp_fw_new() */
    if (gsp.fw_size < sizeof(rm_riscv_ucode_desc_t)) {
        serial_puts("[GSP] Bootloader: firmware too small for descriptor\n");
        return -1;
    }

    uint64_t desc_off = gsp.fw_size - sizeof(rm_riscv_ucode_desc_t);
    const rm_riscv_ucode_desc_t *desc_ptr =
        (const rm_riscv_ucode_desc_t *)((uint8_t *)gsp.fw_data + desc_off);

    /* Copy descriptor */
    memcpy(&bl_desc, desc_ptr, sizeof(bl_desc));

    serial_puts("[GSP] Bootloader desc at fw+0x");
    serial_puthex(desc_off, 8);
    serial_puts(":\n");
    serial_puts("[GSP]   bl_off=0x");
    serial_puthex(bl_desc.bootloader_offset, 8);
    serial_puts(" bl_sz=");
    serial_putdec(bl_desc.bootloader_size);
    serial_puts("\n");
    serial_puts("[GSP]   elf_off=0x");
    serial_puthex(bl_desc.riscv_elf_offset, 8);
    serial_puts(" elf_sz=");
    serial_putdec(bl_desc.riscv_elf_size);
    serial_puts("\n");
    serial_puts("[GSP]   manifest_off=0x");
    serial_puthex(bl_desc.manifest_offset, 8);
    serial_puts(" app_ver=0x");
    serial_puthex(bl_desc.app_version, 8);
    serial_puts("\n");

    /* Validate bootloader region */
    if (bl_desc.bootloader_size == 0 ||
        bl_desc.bootloader_offset + bl_desc.bootloader_size > gsp.fw_size) {
        serial_puts("[GSP] Bootloader: invalid offset/size (off=0x");
        serial_puthex(bl_desc.bootloader_offset, 8);
        serial_puts(" sz=");
        serial_putdec(bl_desc.bootloader_size);
        serial_puts(" fw_sz=");
        serial_putdec((uint32_t)gsp.fw_size);
        serial_puts(")\n");
        return -1;
    }

    /* Allocate page-aligned copy for DMA */
    bl_size = bl_desc.bootloader_size;
    bl_data = (uint8_t *)mem_alloc_aligned(bl_size, RADIX3_PAGE_SIZE);
    if (!bl_data) {
        serial_puts("[GSP] Bootloader: alloc failed (");
        serial_putdec(bl_size);
        serial_puts(" bytes)\n");
        return -1;
    }

    memcpy(bl_data, (uint8_t *)gsp.fw_data + bl_desc.bootloader_offset, bl_size);

    serial_puts("[GSP] Bootloader: extracted ");
    serial_putdec(bl_size);
    serial_puts(" bytes to 0x");
    serial_puthex((uint64_t)(uintptr_t)bl_data, 16);
    serial_puts("\n");

    fb_puts(" GSP BL: ");
    fb_putdec(bl_size);
    fb_puts("B OK\n");

    return 0;
}

int gsp_write_wpr_meta(void)
{
    gpu_probe_t *p = gpu_get_probe();

    if (!p || !p->pramin_rw_ok) {
        serial_puts("[GSP] WPR meta: PRAMIN not writable\n");
        return -1;
    }
    if (!radix3.built) {
        serial_puts("[GSP] WPR meta: radix3 not built\n");
        return -1;
    }
    if (!bl_data || bl_size == 0) {
        serial_puts("[GSP] WPR meta: bootloader not extracted\n");
        return -1;
    }

    uint64_t vram_bytes = (uint64_t)p->vram_size_mb * 1024 * 1024;

    /* Build GspFwWprMeta v2 */
    gsp_fw_wpr_meta_v2_t meta;
    memset(&meta, 0, sizeof(meta));

    meta.magic                   = WPR2_MAGIC;
    meta.revision                = 1;
    meta.sysmemAddrOfRadix3Elf   = (uint64_t)(uintptr_t)radix3.lvl0;
    meta.sizeOfRadix3Elf         = gsp.fw_size;
    meta.sysmemAddrOfBootloader  = (uint64_t)(uintptr_t)bl_data;
    meta.sizeOfBootloader        = bl_size;
    meta.bootloaderCodeOffset    = 0;
    meta.bootloaderDataOffset    = bl_desc.bootloader_param_offset;
    meta.bootloaderManifestOffset = bl_desc.manifest_offset;
    meta.fbSize                  = vram_bytes;
    meta.gspFwWprEnd             = vram_bytes - RADIX3_PAGE_SIZE;
    meta.gspFwRsvdStart          = vram_bytes - WPR_META_VRAM_OFFSET_FROM_END;

    /* Write to VRAM via PRAMIN at end-of-VRAM - 256KB */
    uint64_t meta_vram_off = vram_bytes - WPR_META_VRAM_OFFSET_FROM_END;
    uint32_t orig_window = gpu_reg_read(NV_PBUS_BAR0_WINDOW);

    uint32_t window_val = (uint32_t)(meta_vram_off >> 16);
    gpu_reg_write(NV_PBUS_BAR0_WINDOW, window_val);
    wmb();

    uint32_t pramin_off = (uint32_t)(meta_vram_off & (NV_PRAMIN_SIZE - 1));
    uint32_t *src = (uint32_t *)&meta;
    uint32_t dwords = sizeof(meta) / 4;

    for (uint32_t i = 0; i < dwords; i++)
        gpu_reg_write(NV_PRAMIN_BASE + pramin_off + (i * 4), src[i]);
    wmb();

    /* Verify magic readback */
    rmb();
    uint32_t read_magic = gpu_reg_read(NV_PRAMIN_BASE + pramin_off);

    /* Restore window */
    gpu_reg_write(NV_PBUS_BAR0_WINDOW, orig_window);
    wmb();

    bool verify_ok = (read_magic == WPR2_MAGIC);

    serial_puts("[GSP] WPR meta: written to VRAM+0x");
    serial_puthex(meta_vram_off, 16);
    serial_puts(" (");
    serial_putdec(sizeof(meta));
    serial_puts(" bytes)\n");
    serial_puts("[GSP] WPR meta: magic verify ");
    serial_puts(verify_ok ? "OK" : "FAIL");
    serial_puts(" (read 0x");
    serial_puthex(read_magic, 8);
    serial_puts(")\n");

    serial_puts("[GSP] WPR meta: radix3_l0=0x");
    serial_puthex(meta.sysmemAddrOfRadix3Elf, 16);
    serial_puts(" bl=0x");
    serial_puthex(meta.sysmemAddrOfBootloader, 16);
    serial_puts(" fbSize=");
    serial_putdec(vram_bytes / (1024 * 1024));
    serial_puts("MB\n");

    fb_puts(" WPR meta: ");
    fb_puts(verify_ok ? "OK" : "FAIL");
    fb_puts("\n");

    return verify_ok ? 0 : -1;
}

gsp_state_t *gsp_get_state(void)
{
    return &gsp;
}
