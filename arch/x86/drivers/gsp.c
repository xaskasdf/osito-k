/*
 * OsitoK x86-64 — GSP Falcon Driver (Phase 4-7)
 *
 * Phase 4: Deep probe (HWCFG2, CPUCTL, mailboxes), firmware load + VRAM upload.
 * Phase 5: ELF64 parse, boot sequence (BOOTVEC, CPUCTL start), mailbox handshake.
 * Phase 6: Shared memory message queues (host↔GSP bidirectional).
 * Phase 7: RPC protocol (function IDs, poll with timeout, init sequence).
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
    }

    return gsp.boot_ack ? 0 : -1;
}

gsp_state_t *gsp_get_state(void)
{
    return &gsp;
}
