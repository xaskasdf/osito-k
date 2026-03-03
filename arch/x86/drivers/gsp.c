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

/* Step 1: SET_SYSTEM_INFO (func 70) — fire-and-forget */
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

/* Step 2: SET_REGISTRY (func 69) — fire-and-forget */
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

/* Step 5: INIT_POST_OBJGPU (func 71) — no payload, poll response */
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

int gsp_rm_init(void)
{
    if (!gsp.queues_ready) {
        serial_puts("[GSP-RM] Queues not ready, skipping RM init\n");
        return -1;
    }

    serial_puts("[GSP-RM] === GSP-RM Init Sequence ===\n");

    serial_puts("[GSP-RM] Step 1/5: SET_SYSTEM_INFO\n");
    gsp_rm_send_system_info();

    serial_puts("[GSP-RM] Step 2/5: SET_REGISTRY\n");
    gsp_rm_send_registry();

    serial_puts("[GSP-RM] Step 3/5: ALLOC_ROOT\n");
    gsp_rm_alloc_root();

    serial_puts("[GSP-RM] Step 4/5: ALLOC_DEVICE\n");
    gsp_rm_alloc_device();

    serial_puts("[GSP-RM] Step 5/5: INIT_POST_OBJGPU\n");
    gsp_rm_init_post_objgpu();

    gsp.rm_init_done = true;

    serial_puts("[GSP-RM] === Init sequence complete ===\n");

    fb_puts(" GSP-RM: init sequence done\n");

    return 0;
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

    /* ── Pre-boot: FWSEC-FRTS (create WPR2 before GSP firmware boot) ── */
    gsp_fwsec_frts();

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

    /* ── Step 2: Write queue init args to VRAM (before clearing mailboxes) ── */
    if (gsp.queues_ready)
        gsp_queue_write_args();

    /* ── Step 3: Set mailboxes to shared memory address (for GSP to find queues) ── */
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

    /* ── Step 4: Set DMATRFBASE (firmware location in VRAM, >> 8) ── */
    uint32_t dma_base = (uint32_t)(gsp.vram_offset >> 8);
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_DMATRFBASE, dma_base);
    wmb();

    /* ── Step 5: Set BOOTVEC (entry point >> 8) ── */
    uint32_t bootvec = (uint32_t)(gsp.elf_entry >> 8);
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_BOOTVEC, bootvec);
    wmb();

    serial_puts("[GSP] Boot: DMATRFBASE=0x");
    serial_puthex(dma_base, 8);
    serial_puts(" BOOTVEC=0x");
    serial_puthex(bootvec, 8);
    serial_puts("\n");

    /* ── Step 6: Start CPU ── */
    serial_puts("[GSP] Boot: starting CPU...\n");
    gpu_reg_write(NV_PGSP_BASE + NV_FALCON_CPUCTL, NV_FALCON_CPUCTL_STARTCPU);
    wmb();
    gsp.booted = true;

    /* ── Step 7: Poll mailbox (timeout ~1 second @ 3GHz) ── */
    serial_puts("[GSP] Boot: polling mailbox (timeout 1s)...\n");

    /* Remember what we wrote so we detect GSP changing it */
    uint32_t mbox0_initial = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX0);

    uint64_t t0 = rdtsc();
    uint64_t timeout_cycles = 3000000000ULL;  /* ~1s @ 3GHz */
    uint32_t mbox0 = mbox0_initial;

    while (1) {
        rmb();
        mbox0 = gpu_reg_read(NV_PGSP_BASE + NV_FALCON_MAILBOX0);
        if (mbox0 != mbox0_initial)
            break;

        uint64_t elapsed = rdtsc() - t0;
        if (elapsed >= timeout_cycles)
            break;
    }

    uint64_t t1 = rdtsc();
    uint64_t elapsed_cycles = t1 - t0;
    uint64_t elapsed_ms = elapsed_cycles / 3000000;

    gsp.boot_status = mbox0;
    gsp.boot_ack = (mbox0 != mbox0_initial);

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

    /* ── Post-boot: RPC init sequence (poll INIT_DONE, query static info) ── */
    if (gsp.queues_ready) {
        gsp_rpc_init();
        gsp_rm_init();    /* X23: RM init sequence */
    }

    return gsp.boot_ack ? 0 : -1;
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

gsp_state_t *gsp_get_state(void)
{
    return &gsp;
}
