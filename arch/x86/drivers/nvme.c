/*
 * OsitoK x86-64 — Minimal NVMe Driver (Read-only)
 *
 * Supports: identify, read, write. Full OsitoFS v2 I/O.
 *
 * NVMe registers at BAR0 (MMIO). Admin queue + one I/O queue.
 * Uses polling (no interrupts) for simplicity.
 */

#include "../include/types.h"
#include "../include/paging.h"

/* ── Declarations ────────────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_putc(char c);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);
extern void fb_puthex(uint64_t val, int digits);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);

/* ── NVMe Register Map (BAR0) ────────────────────────────────── */

#define NVME_REG_CAP        0x00   /* Controller Capabilities (64-bit) */
#define NVME_REG_VS         0x08   /* Version */
#define NVME_REG_CC         0x14   /* Controller Configuration */
#define NVME_REG_CSTS       0x1C   /* Controller Status */
#define NVME_REG_AQA        0x24   /* Admin Queue Attributes */
#define NVME_REG_ASQ        0x28   /* Admin SQ Base (64-bit) */
#define NVME_REG_ACQ        0x30   /* Admin CQ Base (64-bit) */

/* Doorbell stride from CAP */
#define NVME_CAP_DSTRD(cap) (((cap) >> 32) & 0xF)
#define NVME_DOORBELL_BASE  0x1000

/* CC fields */
#define NVME_CC_EN          (1 << 0)
#define NVME_CC_CSS_NVM     (0 << 4)
#define NVME_CC_MPS_4K      (0 << 7)
#define NVME_CC_AMS_RR      (0 << 11)
#define NVME_CC_SHN_NONE    (0 << 14)
#define NVME_CC_IOSQES      (6 << 16)  /* 64 bytes */
#define NVME_CC_IOCQES      (4 << 20)  /* 16 bytes */

#define NVME_CSTS_RDY       (1 << 0)

/* ── NVMe Command Structures ────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t cdw0;      /* opcode(7:0), fuse(9:8), psdt(15:14), cid(31:16) */
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_sqe_t;          /* 64 bytes */

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;    /* phase bit in bit 0, status in 1:15 */
} nvme_cqe_t;          /* 16 bytes */

/* ── NVMe Opcodes ────────────────────────────────────────────── */

#define NVME_ADMIN_IDENTIFY       0x06
#define NVME_ADMIN_CREATE_IOSQ    0x01
#define NVME_ADMIN_CREATE_IOCQ    0x05

#define NVME_IO_READ              0x02
#define NVME_IO_WRITE             0x01

/* ── Queue sizes ─────────────────────────────────────────────── */

#define ADMIN_QUEUE_SIZE    16
#define IO_QUEUE_SIZE       64

/* ── Driver state ────────────────────────────────────────────── */

typedef struct {
    volatile void *bar0;      /* MMIO base */
    uint32_t dstrd;           /* Doorbell stride (in 32-bit units) */

    /* Admin queue */
    nvme_sqe_t *asq;
    nvme_cqe_t *acq;
    uint16_t    asq_tail;
    uint16_t    acq_head;
    uint8_t     acq_phase;

    /* I/O queue (queue ID = 1) */
    nvme_sqe_t *iosq;
    nvme_cqe_t *iocq;
    uint16_t    iosq_tail;
    uint16_t    iocq_head;
    uint8_t     iocq_phase;

    /* Identify data */
    uint64_t    total_lbas;
    uint32_t    lba_size;     /* bytes per LBA */
    uint32_t    max_transfer; /* max transfer size in LBAs */

    uint16_t    cmd_id;
    bool        initialized;
} nvme_state_t;

static nvme_state_t nvme;

/* ── Register access ─────────────────────────────────────────── */

static uint32_t nvme_read32(uint32_t offset)
{
    return mmio_read32((volatile void *)((uint64_t)nvme.bar0 + offset));
}

static uint64_t nvme_read64(uint32_t offset)
{
    return mmio_read64((volatile void *)((uint64_t)nvme.bar0 + offset));
}

static void nvme_write32(uint32_t offset, uint32_t val)
{
    mmio_write32((volatile void *)((uint64_t)nvme.bar0 + offset), val);
}

static void nvme_write64(uint32_t offset, uint64_t val)
{
    mmio_write64((volatile void *)((uint64_t)nvme.bar0 + offset), val);
}

/* ── Doorbell writes ─────────────────────────────────────────── */

static void nvme_ring_sq_doorbell(uint16_t qid, uint16_t tail)
{
    uint32_t offset = NVME_DOORBELL_BASE + (2 * qid) * (4 << nvme.dstrd);
    nvme_write32(offset, tail);
}

static void nvme_ring_cq_doorbell(uint16_t qid, uint16_t head)
{
    uint32_t offset = NVME_DOORBELL_BASE + (2 * qid + 1) * (4 << nvme.dstrd);
    nvme_write32(offset, head);
}

/* ── Submit and wait (polling) ───────────────────────────────── */

static int nvme_admin_submit_wait(nvme_sqe_t *cmd)
{
    cmd->cdw0 = (cmd->cdw0 & 0xFFFF) | ((uint32_t)nvme.cmd_id++ << 16);

    nvme.asq[nvme.asq_tail] = *cmd;
    nvme.asq_tail = (nvme.asq_tail + 1) % ADMIN_QUEUE_SIZE;
    wmb();
    nvme_ring_sq_doorbell(0, nvme.asq_tail);

    /* Poll CQ */
    for (uint32_t timeout = 0; timeout < 10000000; timeout++) {
        nvme_cqe_t *cqe = &nvme.acq[nvme.acq_head];
        if ((cqe->status & 1) == nvme.acq_phase) {
            uint16_t status = cqe->status >> 1;
            nvme.acq_head = (nvme.acq_head + 1) % ADMIN_QUEUE_SIZE;
            if (nvme.acq_head == 0) nvme.acq_phase ^= 1;
            nvme_ring_cq_doorbell(0, nvme.acq_head);
            return (status == 0) ? 0 : -1;
        }
        __asm__ volatile ("pause");
    }

    serial_puts("[NVMe] Admin command timeout\n");
    return -1;
}

static int nvme_io_submit_wait(nvme_sqe_t *cmd)
{
    cmd->cdw0 = (cmd->cdw0 & 0xFFFF) | ((uint32_t)nvme.cmd_id++ << 16);

    nvme.iosq[nvme.iosq_tail] = *cmd;
    nvme.iosq_tail = (nvme.iosq_tail + 1) % IO_QUEUE_SIZE;
    wmb();
    nvme_ring_sq_doorbell(1, nvme.iosq_tail);

    /* Poll CQ */
    for (uint32_t timeout = 0; timeout < 50000000; timeout++) {
        nvme_cqe_t *cqe = &nvme.iocq[nvme.iocq_head];
        if ((cqe->status & 1) == nvme.iocq_phase) {
            uint16_t status = cqe->status >> 1;
            nvme.iocq_head = (nvme.iocq_head + 1) % IO_QUEUE_SIZE;
            if (nvme.iocq_head == 0) nvme.iocq_phase ^= 1;
            nvme_ring_cq_doorbell(1, nvme.iocq_head);
            return (status == 0) ? 0 : -1;
        }
        __asm__ volatile ("pause");
    }

    serial_puts("[NVMe] I/O command timeout\n");
    return -1;
}

/* ── Async I/O: submit without waiting ─────────────────────── */

/* Submit an I/O command and return immediately with the cmd_id.
 * Caller must later call nvme_poll_cq() or nvme_wait_cq() to
 * check/wait for completion. Max 2 in-flight for ping-pong. */

static volatile uint32_t nvme_inflight;

uint16_t nvme_io_submit_async(nvme_sqe_t *cmd)
{
    uint16_t cid = nvme.cmd_id++;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFF) | ((uint32_t)cid << 16);

    nvme.iosq[nvme.iosq_tail] = *cmd;
    nvme.iosq_tail = (nvme.iosq_tail + 1) % IO_QUEUE_SIZE;
    wmb();
    nvme_ring_sq_doorbell(1, nvme.iosq_tail);
    nvme_inflight++;
    return cid;
}

/* Non-blocking: check if a specific command completed. Returns:
 *   0 = completed OK,  -1 = not yet,  -2 = error */
int nvme_poll_cq(uint16_t expected_cid)
{
    nvme_cqe_t *cqe = &nvme.iocq[nvme.iocq_head];
    if ((cqe->status & 1) != nvme.iocq_phase)
        return -1;  /* no completion yet */

    uint16_t cid = cqe->cid;
    uint16_t status = cqe->status >> 1;
    nvme.iocq_head = (nvme.iocq_head + 1) % IO_QUEUE_SIZE;
    if (nvme.iocq_head == 0) nvme.iocq_phase ^= 1;
    nvme_ring_cq_doorbell(1, nvme.iocq_head);
    if (nvme_inflight > 0) nvme_inflight--;

    if (cid != expected_cid) {
        /* Out-of-order completion — shouldn't happen with 1-2 in-flight
         * on the same queue, but handle gracefully */
        return (status == 0) ? 0 : -2;
    }
    return (status == 0) ? 0 : -2;
}

/* Blocking: spin until the command completes */
int nvme_wait_cq(uint16_t cid)
{
    for (uint32_t timeout = 0; timeout < 50000000; timeout++) {
        int r = nvme_poll_cq(cid);
        if (r >= -1 && r != -1) return r;  /* 0 or -2 */
        __asm__ volatile ("pause");
    }
    serial_puts("[NVMe] Async wait timeout\n");
    return -2;
}

/* High-level: async read of LBAs to a physical address.
 * Returns cmd_id (use nvme_wait_cq to wait). Limits to 2 pages. */
int nvme_read_async(uint64_t lba, uint32_t count, uint64_t phys_addr)
{
    if (!nvme.initialized || count == 0) return -1;
    uint32_t lbas_per_page = 4096 / nvme.lba_size;
    if (count > lbas_per_page * 2) count = lbas_per_page * 2;

    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_IO_READ;
    cmd.nsid = 1;
    cmd.prp1 = phys_addr;
    if ((uint64_t)count * nvme.lba_size > 4096)
        cmd.prp2 = phys_addr + 4096;
    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1;

    return (int)nvme_io_submit_async(&cmd);
}

/* Accessor for external code */
uint32_t nvme_get_lba_size(void) { return nvme.initialized ? nvme.lba_size : 512; }

/* ── Initialize NVMe controller ──────────────────────────────── */

int __initk nvme_init(uint64_t bar0_phys)
{
    memset(&nvme, 0, sizeof(nvme));
    /* Use the upper-half MMIO alias so register accesses work from
     * any CR3 (paging_map_mmio installs BAR pages at both identity
     * and KERNEL_VBASE+phys). */
    nvme.bar0 = (volatile void *)PHYS_TO_VIRT(bar0_phys);

    serial_puts("[NVMe] Initializing, BAR0=");
    serial_puthex(bar0_phys, 16);
    serial_puts("\n");

    /* Read capabilities */
    uint64_t cap = nvme_read64(NVME_REG_CAP);
    nvme.dstrd = NVME_CAP_DSTRD(cap);
    uint32_t mqes = (cap & 0xFFFF) + 1; /* Max queue entries */

    uint32_t vs = nvme_read32(NVME_REG_VS);
    serial_puts("[NVMe] Version ");
    serial_putdec((vs >> 16) & 0xFFFF);
    serial_puts(".");
    serial_putdec((vs >> 8) & 0xFF);
    serial_puts(", MQES=");
    serial_putdec(mqes);
    serial_puts("\n");

    /* Disable controller */
    nvme_write32(NVME_REG_CC, 0);
    /* Wait for not ready */
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY)) break;
        __asm__ volatile ("pause");
    }

    /* Allocate admin queues (page-aligned). The struct fields hold
     * upper-half virt pointers so kernel CPU code can read/write the
     * queue entries via the direct map; the hardware register writes
     * below convert back to phys via VIRT_TO_PHYS. */
    void *asq_phys = mem_alloc_aligned(ADMIN_QUEUE_SIZE * sizeof(nvme_sqe_t), 4096);
    void *acq_phys = mem_alloc_aligned(ADMIN_QUEUE_SIZE * sizeof(nvme_cqe_t), 4096);
    if (!asq_phys || !acq_phys) {
        serial_puts("[NVMe] Failed to allocate admin queues\n");
        return -1;
    }
    nvme.asq = (nvme_sqe_t *)PHYS_TO_VIRT(asq_phys);
    nvme.acq = (nvme_cqe_t *)PHYS_TO_VIRT(acq_phys);
    memset(nvme.asq, 0, ADMIN_QUEUE_SIZE * sizeof(nvme_sqe_t));
    memset(nvme.acq, 0, ADMIN_QUEUE_SIZE * sizeof(nvme_cqe_t));

    nvme.asq_tail = 0;
    nvme.acq_head = 0;
    nvme.acq_phase = 1;

    /* Configure admin queues — hardware needs the physical addresses. */
    nvme_write32(NVME_REG_AQA, ((ADMIN_QUEUE_SIZE - 1) << 16) | (ADMIN_QUEUE_SIZE - 1));
    nvme_write64(NVME_REG_ASQ, VIRT_TO_PHYS(nvme.asq));
    nvme_write64(NVME_REG_ACQ, VIRT_TO_PHYS(nvme.acq));

    /* Enable controller */
    uint32_t cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K |
                  NVME_CC_IOSQES | NVME_CC_IOCQES;
    nvme_write32(NVME_REG_CC, cc);

    /* Wait for ready */
    for (uint32_t i = 0; i < 10000000; i++) {
        if (nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY) break;
        __asm__ volatile ("pause");
    }
    if (!(nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY)) {
        serial_puts("[NVMe] Controller not ready\n");
        return -1;
    }

    serial_puts("[NVMe] Controller ready\n");

    /* Identify controller — DMA buffer (controller fills it). CPU reads
     * the model name etc. afterwards, so we keep both views handy. */
    void *identify_phys = mem_alloc_aligned(4096, 4096);
    if (!identify_phys) return -1;
    void *identify_buf  = PHYS_TO_VIRT(identify_phys);
    memset(identify_buf, 0, 4096);

    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_IDENTIFY;
    cmd.prp1 = (uint64_t)identify_phys;
    cmd.cdw10 = 1; /* Controller identify */

    if (nvme_admin_submit_wait(&cmd) < 0) {
        serial_puts("[NVMe] Identify controller failed\n");
        return -1;
    }

    /* Print model name (bytes 24-63) */
    char *model = (char *)identify_buf + 24;
    serial_puts("[NVMe] Model: ");
    for (int i = 0; i < 40 && model[i]; i++)
        serial_putc(model[i]);
    serial_puts("\n");

    fb_puts(" NVMe: ");
    for (int i = 0; i < 40 && model[i]; i++) {
        if (model[i] >= 32 && model[i] < 127) {
            char tmp[2] = { model[i], 0 };
            fb_puts(tmp);
        }
    }
    fb_puts("\n");

    /* Identify namespace 1 */
    memset(identify_buf, 0, 4096);
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_IDENTIFY;
    cmd.nsid = 1;
    cmd.prp1 = (uint64_t)identify_phys;
    cmd.cdw10 = 0; /* Namespace identify */

    if (nvme_admin_submit_wait(&cmd) < 0) {
        serial_puts("[NVMe] Identify namespace failed\n");
        return -1;
    }

    uint64_t nsze = *(uint64_t *)identify_buf; /* Namespace Size */
    uint8_t flbas = *((uint8_t *)identify_buf + 26); /* Formatted LBA Size */
    uint32_t lbaf_offset = 128 + (flbas & 0xF) * 4;
    uint32_t lbaf = *(uint32_t *)((uint8_t *)identify_buf + lbaf_offset);
    uint32_t lba_ds = (lbaf >> 16) & 0xFF; /* LBA Data Size (power of 2) */

    nvme.total_lbas = nsze;
    nvme.lba_size = 1 << lba_ds;
    nvme.max_transfer = 256; /* Conservative: 256 LBAs per command */

    serial_puts("[NVMe] NS1: ");
    serial_putdec(nsze);
    serial_puts(" LBAs x ");
    serial_putdec(nvme.lba_size);
    serial_puts(" bytes = ");
    serial_putdec(nsze * nvme.lba_size / (1024 * 1024 * 1024));
    serial_puts(" GB\n");

    /* Create I/O Completion Queue (ID=1) */
    void *iocq_phys = mem_alloc_aligned(IO_QUEUE_SIZE * sizeof(nvme_cqe_t), 4096);
    if (!iocq_phys) return -1;
    nvme.iocq = (nvme_cqe_t *)PHYS_TO_VIRT(iocq_phys);
    memset(nvme.iocq, 0, IO_QUEUE_SIZE * sizeof(nvme_cqe_t));

    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_CREATE_IOCQ;
    cmd.prp1 = (uint64_t)iocq_phys;
    cmd.cdw10 = ((IO_QUEUE_SIZE - 1) << 16) | 1;  /* QID=1, size */
    cmd.cdw11 = 1; /* Physically contiguous */

    if (nvme_admin_submit_wait(&cmd) < 0) {
        serial_puts("[NVMe] Create IOCQ failed\n");
        return -1;
    }

    /* Create I/O Submission Queue (ID=1, CQ=1) */
    void *iosq_phys = mem_alloc_aligned(IO_QUEUE_SIZE * sizeof(nvme_sqe_t), 4096);
    if (!iosq_phys) return -1;
    nvme.iosq = (nvme_sqe_t *)PHYS_TO_VIRT(iosq_phys);
    memset(nvme.iosq, 0, IO_QUEUE_SIZE * sizeof(nvme_sqe_t));

    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_CREATE_IOSQ;
    cmd.prp1 = (uint64_t)iosq_phys;
    cmd.cdw10 = ((IO_QUEUE_SIZE - 1) << 16) | 1;  /* QID=1, size */
    cmd.cdw11 = (1 << 16) | 1; /* CQID=1, physically contiguous */

    if (nvme_admin_submit_wait(&cmd) < 0) {
        serial_puts("[NVMe] Create IOSQ failed\n");
        return -1;
    }

    nvme.iosq_tail = 0;
    nvme.iocq_head = 0;
    nvme.iocq_phase = 1;
    nvme.initialized = true;

    serial_puts("[NVMe] I/O queues created, driver ready\n");

    /* Register as a block device. NVMe driver currently uses one global
     * `nvme` so multi-controller setups overwrite each other; the most
     * recent successful nvme_init wins both nvme_read AND the blkdev
     * read function. main.c iterates controllers sequentially during
     * boot to find OsitoFS, which is consistent with this single-active
     * model. */
    extern int  blkdev_register(const char *name, uint8_t type,
                                uint32_t sector_size, uint64_t sector_count,
                                int (*read)(uint64_t, uint32_t, void *),
                                int (*write)(uint64_t, uint32_t, const void *));
    extern int  nvme_read(uint64_t lba, uint32_t count, void *buf);
    extern int  nvme_write(uint64_t lba, uint32_t count, const void *buf);
    blkdev_register("nvme0", 0 /* BLKDEV_NVME */,
                    nvme.lba_size, nvme.total_lbas,
                    nvme_read, nvme_write);

    return 0;
}

/* ── Read LBAs ───────────────────────────────────────────────── */

int nvme_read(uint64_t lba, uint32_t count, void *buf)
{
    if (!nvme.initialized) return -1;
    if (count == 0 || count > nvme.max_transfer) return -1;

    /* For reads > 1 page, need PRP list — for now limit to 1 page per command */
    /* (4096 / lba_size) LBAs per page */
    uint32_t lbas_per_page = 4096 / nvme.lba_size;
    uint8_t *dst = (uint8_t *)buf;

    while (count > 0) {
        uint32_t this_count = count;
        if (this_count > lbas_per_page * 2) this_count = lbas_per_page * 2;

        uint64_t bytes = (uint64_t)this_count * nvme.lba_size;

        nvme_sqe_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cdw0 = NVME_IO_READ;
        cmd.nsid = 1;
        /* NVMe PRP fields are PHYSICAL addresses.  Callers may pass
         * either lower-half identity virts (UEFI-era boot stacks) or
         * upper-half kernel mirror virts (kmalloc'd buffers, per-process
         * kernel stacks). kvirt_to_phys handles both correctly.  Without
         * this translation, kmalloc'd `sys_buf` in NtReadFile would be
         * DMA-written-to using an upper-half VIRT treated as PHYS — the
         * data lands at the wrong physical address and the buffer stays
         * zero (caught when UT99 localization reads returned all-NUL). */
        cmd.prp1 = kvirt_to_phys(dst);
        if (bytes > 4096)
            cmd.prp2 = kvirt_to_phys(dst + 4096);
        cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = this_count - 1; /* 0-based */

        if (nvme_io_submit_wait(&cmd) < 0) return -1;

        dst += bytes;
        lba += this_count;
        count -= this_count;
    }

    return 0;
}

/* ── Read bytes at arbitrary offset ──────────────────────────── */

int nvme_read_bytes(uint64_t byte_offset, void *buf, uint64_t len)
{
    if (!nvme.initialized) return -1;

    uint64_t lba = byte_offset / nvme.lba_size;
    uint64_t lba_offset = byte_offset % nvme.lba_size;

    /* Allocate a temporary aligned DMA buffer for the read.
     * temp_phys is what nvme_read hands to the controller as PRP1;
     * temp_virt is the kernel CPU view used for the memcpy below. */
    void *temp_phys = mem_alloc_aligned(4096 * 2, 4096);
    if (!temp_phys) return -1;
    uint8_t *temp_virt = (uint8_t *)PHYS_TO_VIRT(temp_phys);

    uint8_t *dst = (uint8_t *)buf;
    uint64_t remaining = len;
    uint64_t cur_lba = lba;
    uint64_t cur_offset = lba_offset;

    while (remaining > 0) {
        uint32_t read_lbas = (uint32_t)((cur_offset + remaining + nvme.lba_size - 1) / nvme.lba_size);
        if (read_lbas > 8) read_lbas = 8;  /* 4KB at a time */

        if (nvme_read(cur_lba, read_lbas, temp_phys) < 0) {
            mem_free_pages(temp_phys, 2);
            return -1;
        }

        uint64_t avail = (uint64_t)read_lbas * nvme.lba_size - cur_offset;
        uint64_t copy = remaining < avail ? remaining : avail;
        memcpy(dst, temp_virt + cur_offset, copy);

        dst += copy;
        remaining -= copy;
        cur_lba += read_lbas;
        cur_offset = 0;
    }

    mem_free_pages(temp_phys, 2);
    return 0;
}

/* ── Write LBAs ──────────────────────────────────────────────── */

int nvme_write(uint64_t lba, uint32_t count, const void *buf)
{
    if (!nvme.initialized) return -1;
    if (count == 0 || count > nvme.max_transfer) return -1;

    uint32_t lbas_per_page = 4096 / nvme.lba_size;
    const uint8_t *src = (const uint8_t *)buf;

    while (count > 0) {
        uint32_t this_count = count;
        if (this_count > lbas_per_page * 2) this_count = lbas_per_page * 2;

        uint64_t bytes = (uint64_t)this_count * nvme.lba_size;

        nvme_sqe_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cdw0 = NVME_IO_WRITE;
        cmd.nsid = 1;
        cmd.prp1 = (uint64_t)src;
        if (bytes > 4096)
            cmd.prp2 = (uint64_t)(src + 4096);
        cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = this_count - 1; /* 0-based */

        if (nvme_io_submit_wait(&cmd) < 0) return -1;

        src += bytes;
        lba += this_count;
        count -= this_count;
    }

    return 0;
}

/* ── Write bytes at arbitrary offset ─────────────────────────── */

int nvme_write_bytes(uint64_t byte_offset, const void *buf, uint64_t len)
{
    if (!nvme.initialized) return -1;
    if (len == 0) return 0;

    uint64_t lba = byte_offset / nvme.lba_size;
    uint64_t lba_offset = byte_offset % nvme.lba_size;

    void *temp_phys = mem_alloc_aligned(4096 * 2, 4096);
    if (!temp_phys) return -1;
    uint8_t *temp_virt = (uint8_t *)PHYS_TO_VIRT(temp_phys);

    const uint8_t *src = (const uint8_t *)buf;
    uint64_t remaining = len;
    uint64_t cur_lba = lba;
    uint64_t cur_offset = lba_offset;

    while (remaining > 0) {
        uint32_t rw_lbas = (uint32_t)((cur_offset + remaining + nvme.lba_size - 1) / nvme.lba_size);
        if (rw_lbas > 8) rw_lbas = 8;

        /* Read-modify-write if not aligned */
        if (cur_offset != 0 || remaining < (uint64_t)rw_lbas * nvme.lba_size) {
            if (nvme_read(cur_lba, rw_lbas, temp_phys) < 0) {
                mem_free_pages(temp_phys, 2);
                return -1;
            }
        }

        uint64_t avail = (uint64_t)rw_lbas * nvme.lba_size - cur_offset;
        uint64_t copy = remaining < avail ? remaining : avail;
        memcpy(temp_virt + cur_offset, src, copy);

        if (nvme_write(cur_lba, rw_lbas, temp_phys) < 0) {
            mem_free_pages(temp_phys, 2);
            return -1;
        }

        src += copy;
        remaining -= copy;
        cur_lba += rw_lbas;
        cur_offset = 0;
    }

    mem_free_pages(temp_phys, 2);
    return 0;
}

/* ── Flush ───────────────────────────────────────────────────── */

int nvme_flush(void)
{
    if (!nvme.initialized) return -1;

    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = 0x00; /* Flush opcode */
    cmd.nsid = 1;

    return nvme_io_submit_wait(&cmd);
}

/* ── Accessors ───────────────────────────────────────────────── */

bool nvme_is_ready(void)  { return nvme.initialized; }
uint32_t nvme_lba_size(void) { return nvme.lba_size; }
uint64_t nvme_total_lbas(void) { return nvme.total_lbas; }

