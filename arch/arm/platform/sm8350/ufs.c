/*
 * ufs.c -- the SM8350's UFS host controller (ufshc@1d84000)
 *
 * The phone's storage, every partition included: ABL boots from it and
 * leaves the controller up, link active, its request list idle. Shell `ufs`
 * reports the power domain and clocks (GCC), then, only if the interface
 * and bus clocks run (an unclocked access can hang the bus), the UFSHCI
 * registers, and whether the controller's DMA (SMMU stream 0xE0) reaches
 * the DMA window 1:1 (ABL leaves its context bank untranslated).
 *
 * `ufs luns`, `ufs gpt <lun>` and `ufs read <lun> <lba> [offset]` read the
 * device. The first command takes the transfer request list over from ABL:
 * stop it, point it at ours, start it. One slot, one command at a time,
 * polled, inline crypto off.
 *
 * Osito-K writes only to LUN 0's userdata partition: OsitoFS v3 lives there
 * (`ufs mkfs userdata`, `ufs mount`; `ufs migrate userdata` turns a v2 into
 * v3, files kept). permitted() refuses any other write, and every command
 * that isn't a read or a write into that range.
 *
 * Offsets: Linux drivers/clk/qcom/gcc-sm8350.c (the ufs_phy block at
 * 0x77000), include/ufs/ufshci.h and ufs.h, the phone's device tree (reg,
 * iommus = <&apps_smmu 0xe0 0x0>).
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "mmu.h"
#include "smmu.h"
#include "ufs.h"
#include "../../include/fs.h"
#include "../../fs/ositofs3.h"

#define GCC_BASE            0x00100000UL
#define GCC_UFS             0x77000     /* GCC_UFS_PHY_BCR */
#define GCC_UFS_GDSCR       0x004
#define GDSC_PWR_ON         (1U << 31)
#define CBCR_CLK_OFF        (1U << 31)

static const struct {
    const char *name;
    uint16_t    cbcr;                   /* in the ufs_phy block */
} clocks[] = {
    { "ahb",    0x018 },                /* iface_clk */
    { "axi",    0x010 },                /* core_clk */
    { "unipro", 0x05c },
    { "ice",    0x064 },
    { "aux",    0x09c },
    { "tx0",    0x01c },                /* lane symbol clocks, from the PHY */
    { "rx0",    0x020 },
    { "rx1",    0x0b8 },
    { "aggre",  0x0cc },                /* aggre_ufs_phy_axi: the NoC path */
};
#define NCLOCKS (int)(sizeof(clocks) / sizeof(clocks[0]))

#define UFS_BASE            0x01D84000UL
#define UFS_SID             0xE0

/* UFSHCI */
#define REG_CAP             0x00
#define REG_VER             0x08
#define REG_AHIT            0x18        /* auto-hibernate idle timer */
#define REG_IS              0x20
#define REG_IE              0x24
#define REG_HCS             0x30
#define REG_HCE             0x34
#define REG_UTRLBA          0x50
#define REG_UTRLBAU         0x54
#define REG_UTRLDBR         0x58
#define REG_UTRLCLR         0x5C
#define REG_UTRLRSR         0x60
#define REG_UTMRLBA         0x70
#define REG_UTMRLBAU        0x74
#define REG_UTMRLDBR        0x78
#define REG_UTMRLRSR        0x80

#define CAP_NUTRS(c)        (((c) & 0x1F) + 1)
#define CAP_NUTMRS(c)       ((((c) >> 16) & 0x7) + 1)
#define CAP_64AS            (1U << 24)
#define HCS_DP              (1U << 0)   /* device present */
#define HCS_UTRLRDY         (1U << 1)
#define HCS_UTMRLRDY        (1U << 2)
#define HCS_UCRDY           (1U << 3)

#define IS_UTRCS            (1U << 0)   /* a transfer request completed */

static uint32_t gcc(uint32_t off)
{
    return mmio_read32(GCC_BASE + GCC_UFS + off);
}

/* With barriers, as Linux's readl/writel (see dwc3_acm.c): descriptors and
 * data live in the Normal non-cacheable DMA window, the registers in Device
 * memory */
static uint32_t ufs(uint32_t off)
{
    uint32_t v = mmio_read32(UFS_BASE + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

static void ufs_wr(uint32_t off, uint32_t v)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    mmio_write32(UFS_BASE + off, v);
}

static int clocked(void)
{
    return (gcc(GCC_UFS_GDSCR) & GDSC_PWR_ON) && !(gcc(clocks[0].cbcr) & CBCR_CLK_OFF) &&
           !(gcc(clocks[1].cbcr) & CBCR_CLK_OFF);
}

static void put_reg(const char *name, uint32_t val)
{
    serial_puts(" ");
    serial_puts(name);
    serial_puts("=");
    serial_puthex(val, 8);
}

static void put_flag(const char *name, int on)
{
    if (on) {
        serial_puts(" ");
        serial_puts(name);
    }
}

/* ── The device ───────────────────────────────────────────────── */

/* UTP transfer request descriptor, word 0 */
#define UTRD_CT_UFS         (1U << 28)  /* command type: UFS storage */
#define UTRD_DD_NONE        (0U << 25)
#define UTRD_DD_WRITE       (1U << 25)  /* host to device */
#define UTRD_DD_READ        (2U << 25)  /* device to host */
#define OCS_INVALID         0x0F        /* word 2, until the controller is done */

/* UTP command descriptor: command UPIU, response UPIU, PRDT */
#define UCD_RSP             512
#define UCD_PRDT            1024
#define UCD_SIZE            1040        /* one PRDT entry */

#define UPIU_COMMAND        0x01
#define UPIU_RESPONSE       0x21
#define UPIU_FLAG_READ      0x40
#define UPIU_FLAG_WRITE     0x20
#define SCSI_GOOD           0x00
#define SCSI_CHECK          0x02

#define DATA_MAX            65536       /* one command: 16 blocks of 4 KB */
#define CMD_TIMEOUT_MS      1000

/* Reading commands, sent anywhere */
#define TEST_UNIT_READY     0x00
#define INQUIRY             0x12
#define READ_CAPACITY_10    0x25
#define READ_10             0x28
static const uint8_t allowed[] = { TEST_UNIT_READY, INQUIRY, READ_CAPACITY_10, READ_10 };

/* Writing commands, only into the one partition Osito-K owns: LUN 0's
 * userdata (Android's, given up on 2026-10-29), found by name in the GPT.
 * Until ufs_attach() has found it, rw_blocks is 0 and nothing writes. */
#define WRITE_10            0x2A
#define SYNC_CACHE_10       0x35
#define RW_LUN              0
#define RW_PART             "userdata"
static uint32_t rw_first, rw_blocks, rw_bsize;

static volatile uint32_t *utrl;         /* 32 descriptors of 8 words, slot 0 used */
static volatile uint8_t *ucd;
static volatile uint8_t *data;
static int taken;
static uint8_t sense_key, asc, ascq;    /* from the last CHECK CONDITION */
static uint8_t fail_ocs, fail_response; /* from the last failed transfer */

static void *dma_aligned(uint32_t bytes, uint32_t align)
{
    uint8_t *p = dma_alloc(bytes + align);
    if (!p)
        return (void *)0;
    return (void *)(((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1));
}

/* Point the controller's transfer request list at ours. ABL's (0x9EA59000
 * on 2026-10-29) lives below 0xA0000000, which Osito-K doesn't map; it's
 * idle (doorbell 0). */
static int take_over(void)
{
    if (taken)
        return 0;
    if (!clocked()) {
        serial_puts("ufs: the controller is powered or clocked down\n");
        return -1;
    }
    uint32_t hcs = ufs(REG_HCS);
    if (!(ufs(REG_HCE) & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
        serial_puts("ufs: controller off, no device, or no request list ready\n");
        return -1;
    }
    if (ufs(REG_UTRLDBR)) {
        serial_puts("ufs: ABL left requests in flight\n");
        return -1;
    }
    if (!utrl) {
        utrl = dma_aligned(32 * 32, 1024);
        ucd = dma_aligned(UCD_SIZE, 128);
        data = dma_aligned(DATA_MAX, 128);
        if (!utrl || !ucd || !data) {
            serial_puts("ufs: no DMA memory\n");
            return -1;
        }
    }
    if (!smmu_dma_identity("ufs", UFS_SID, DMA_WINDOW_BASE, DMA_WINDOW_SIZE))
        return -1;
    uint64_t abl = ((uint64_t)ufs(REG_UTRLBAU) << 32) | ufs(REG_UTRLBA);
    uint64_t ours = (uint64_t)(uintptr_t)utrl;
    ufs_wr(REG_UTRLRSR, 0);
    for (int i = 0; i < 1000 && ufs(REG_UTRLRSR); i++)
        udelay(10);
    ufs_wr(REG_UTRLBA, (uint32_t)ours);
    ufs_wr(REG_UTRLBAU, (uint32_t)(ours >> 32));
    ufs_wr(REG_UTRLRSR, 1);
    taken = 1;
    /* From ABL's, or after a kload the previous kernel's (the same place) */
    serial_puts("[UFS ] request list moved from ");
    serial_puthex(abl, 10);
    serial_puts(" to ");
    serial_puthex(ours, 10);
    serial_puts("\n");
    return 0;
}

static void put_be32(volatile uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t be32(const volatile uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t le64(const volatile uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* May this command go out? Reads anywhere; WRITE(10) and SYNCHRONIZE
 * CACHE only on RW_LUN, a write only inside the userdata partition. */
static int permitted(uint8_t lun, const uint8_t cdb[16], uint32_t len, int write)
{
    if (!write) {
        if (cdb[0] == SYNC_CACHE_10)
            return lun == RW_LUN && rw_blocks && !len;
        for (uint32_t i = 0; i < sizeof(allowed); i++)
            if (cdb[0] == allowed[i])
                return 1;
        return 0;
    }
    if (cdb[0] != WRITE_10 || lun != RW_LUN || !rw_blocks)
        return 0;
    uint64_t lba = (uint64_t)cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5];
    uint32_t count = (uint32_t)cdb[7] << 8 | cdb[8];
    return count && len == count * rw_bsize &&
           lba >= rw_first && lba + count <= (uint64_t)rw_first + rw_blocks;
}

/* One SCSI command to `lun`, moving `len` bytes through data[] (to the
 * device if `write`). Returns the SCSI status (SCSI_GOOD, or SCSI_CHECK
 * with sense_key/asc/ascq set), or -1 if refused or the transfer failed. */
static int scsi(uint8_t lun, const uint8_t cdb[16], uint32_t len, int write)
{
    if (len > DATA_MAX || !permitted(lun, cdb, len, write))
        return -1;
    if (take_over())
        return -1;

    for (int i = 0; i < UCD_SIZE; i++)
        ucd[i] = 0;
    ucd[0] = UPIU_COMMAND;
    ucd[1] = !len ? 0 : write ? UPIU_FLAG_WRITE : UPIU_FLAG_READ;
    ucd[2] = lun;
    put_be32(&ucd[12], len);            /* expected data transfer length */
    for (int i = 0; i < 16; i++)
        ucd[16 + i] = cdb[i];
    volatile uint32_t *prdt = (volatile uint32_t *)(ucd + UCD_PRDT);
    uint64_t buf = (uint64_t)(uintptr_t)data;
    prdt[0] = (uint32_t)buf;
    prdt[1] = (uint32_t)(buf >> 32);
    prdt[2] = 0;
    prdt[3] = len ? len - 1 : 0;        /* byte count - 1 */

    uint64_t cd = (uint64_t)(uintptr_t)ucd;
    utrl[0] = UTRD_CT_UFS |                                         /* crypto off */
              (!len ? UTRD_DD_NONE : write ? UTRD_DD_WRITE : UTRD_DD_READ);
    utrl[1] = 0;
    utrl[2] = OCS_INVALID;
    utrl[3] = 0;
    utrl[4] = (uint32_t)cd;
    utrl[5] = (uint32_t)(cd >> 32);
    utrl[6] = (UCD_RSP / 4) << 16 | (512 / 4);      /* response offset | length, dwords */
    utrl[7] = (UCD_PRDT / 4) << 16 | (len ? 1 : 0); /* PRDT offset | entries */

    ufs_wr(REG_UTRLDBR, 1);
    uint64_t t0 = timer_ms();
    while (ufs(REG_UTRLDBR) & 1) {
        if (timer_ms() - t0 > CMD_TIMEOUT_MS) {
            ufs_wr(REG_UTRLCLR, ~1U);   /* 0 clears slot 0 */
            serial_puts("ufs: command timed out\n");
            return -1;
        }
    }
    ufs_wr(REG_IS, IS_UTRCS);
    uint32_t ocs = utrl[2] & 0xFF;
    volatile uint8_t *rsp = ucd + UCD_RSP;
    if (ocs || rsp[0] != UPIU_RESPONSE || rsp[6]) {
        /* e.g. response 1, target failure: a LUN that doesn't exist */
        fail_ocs = (uint8_t)ocs;
        fail_response = rsp[6];
        return -1;
    }
    if (rsp[7] == SCSI_CHECK) {
        volatile uint8_t *sense = rsp + 32 + 2;     /* after its length */
        sense_key = sense[2] & 0xF;
        asc = sense[12];
        ascq = sense[13];
    }
    return rsp[7];
}

static int read_capacity(uint8_t lun, uint32_t *blocks, uint32_t *bsize)
{
    uint8_t cdb[16] = { READ_CAPACITY_10 };
    /* The first command after a reset gets a UNIT ATTENTION: once more */
    for (int tries = 0; tries < 2; tries++) {
        int st = scsi(lun, cdb, 8, 0);
        if (st == SCSI_GOOD) {
            *blocks = be32(&data[0]) + 1;
            *bsize = be32(&data[4]);
            return 0;
        }
        if (st != SCSI_CHECK || sense_key != 6)
            return -1;
    }
    return -1;
}

/* READ(10) or WRITE(10) of `count` blocks between the device and data[] */
static int rw_blocks_io(uint8_t lun, uint32_t lba, uint32_t count, uint32_t bsize, int write)
{
    uint8_t cdb[16] = { write ? WRITE_10 : READ_10, 0,
                        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                        (uint8_t)(lba >> 8), (uint8_t)lba,
                        0, (uint8_t)(count >> 8), (uint8_t)count };
    return scsi(lun, cdb, count * bsize, write) == SCSI_GOOD ? 0 : -1;
}

static int read_blocks(uint8_t lun, uint32_t lba, uint32_t count, uint32_t bsize)
{
    return rw_blocks_io(lun, lba, count, bsize, 0);
}

static void put_mb(uint64_t bytes)
{
    serial_putdec(bytes >> 20);
    serial_puts(" MB");
}

static void put_failure(const char *what)
{
    serial_puts("ufs: ");
    serial_puts(what);
    serial_puts(" (last transfer: ocs ");
    serial_puthex(fail_ocs, 2);
    serial_puts(", response ");
    serial_puthex(fail_response, 2);
    serial_puts("; last sense ");
    serial_puthex(sense_key, 1);
    serial_puts("/");
    serial_puthex(asc, 2);
    serial_puts("/");
    serial_puthex(ascq, 2);
    serial_puts(")\n");
}

/* `ufs luns`: the device's identity, then each LUN's size */
static void cmd_luns(void)
{
    uint8_t inq[16] = { INQUIRY, 0, 0, 0, 36 };
    if (scsi(0, inq, 36, 0) == SCSI_GOOD) {
        serial_puts("device: ");
        for (int i = 8; i < 36; i++)
            serial_putc(data[i] >= 32 && data[i] < 127 ? (char)data[i] : ' ');
        serial_puts("\n");
    }
    for (int lun = 0; lun < 8; lun++) {
        uint32_t blocks, bsize;
        serial_puts("lun ");
        serial_putdec(lun);
        if (read_capacity((uint8_t)lun, &blocks, &bsize)) {
            serial_puts(": -\n");
            continue;
        }
        serial_puts(": ");
        serial_putdec(blocks);
        serial_puts(" blocks of ");
        serial_putdec(bsize);
        serial_puts(", ");
        put_mb((uint64_t)blocks * bsize);
        serial_puts("\n");
    }
}

/* A used GPT entry: its name (ASCII from UTF-16LE), first and last LBA */
typedef struct {
    char     name[37];
    uint64_t first, last;
} gpt_part_t;

/* Calls fn for each used entry of the LUN's GPT (header at LBA 1). fn must
 * not send commands: the entries live in data[]. Returns the number of
 * entries, or -1 with the reason printed. */
static int gpt_scan(uint8_t lun, uint32_t *bsize_out,
                    void (*fn)(const gpt_part_t *p, uint32_t bsize, void *ctx), void *ctx)
{
    uint32_t blocks, bsize;
    if (read_capacity(lun, &blocks, &bsize) || bsize > DATA_MAX || read_blocks(lun, 1, 1, bsize)) {
        put_failure("can't read that LUN");
        return -1;
    }
    static const char sig[] = "EFI PART";
    for (int i = 0; i < 8; i++)
        if (data[i] != (uint8_t)sig[i]) {
            serial_puts("ufs: no GPT at LBA 1\n");
            return -1;
        }
    uint64_t entries_lba = le64(&data[72]);
    uint32_t count = (uint32_t)le64(&data[80]) & 0xFFFFFFFF;
    uint32_t esize = (uint32_t)(le64(&data[84]) & 0xFFFFFFFF);
    if (esize < 128 || esize > bsize || entries_lba >= blocks) {
        serial_puts("ufs: odd GPT header\n");
        return -1;
    }
    uint32_t per_block = bsize / esize;
    int used = 0;
    for (uint32_t blk = 0; blk * per_block < count; blk++) {
        if (read_blocks(lun, (uint32_t)entries_lba + blk, 1, bsize)) {
            put_failure("read failed");
            return -1;
        }
        for (uint32_t e = 0; e < per_block && blk * per_block + e < count; e++) {
            volatile uint8_t *p = data + e * esize;
            if (!le64(p) && !le64(p + 8))
                continue;                       /* zero type GUID: unused */
            gpt_part_t part = { .first = le64(p + 32), .last = le64(p + 40) };
            int n = 0;
            for (; n < 36 && p[56 + 2 * n]; n++)
                part.name[n] = p[56 + 2 * n] < 127 ? (char)p[56 + 2 * n] : '?';
            part.name[n] = 0;
            fn(&part, bsize, ctx);
            used++;
        }
    }
    if (bsize_out)
        *bsize_out = bsize;
    return used;
}

static void print_part(const gpt_part_t *p, uint32_t bsize, void *ctx)
{
    (void)ctx;
    serial_puts("  ");
    serial_puts(p->name);
    for (int n = (int)strlen(p->name); n < 20; n++)
        serial_putc(' ');
    serial_puts(" ");
    serial_putdec(p->first);
    serial_puts("-");
    serial_putdec(p->last);
    serial_puts("  ");
    put_mb((p->last - p->first + 1) * bsize);
    serial_puts("\n");
}

/* `ufs gpt <lun>`: the partitions in its GPT */
static void cmd_gpt(uint8_t lun)
{
    int n = gpt_scan(lun, (uint32_t *)0, print_part, (void *)0);
    if (n < 0)
        return;
    serial_puts("lun ");
    serial_putdec(lun);
    serial_puts(": ");
    serial_putdec((uint32_t)n);
    serial_puts(" partitions\n");
}

/* `ufs read <lun> <lba> [offset]`: 256 bytes of the block, from offset */
static void cmd_read(uint8_t lun, uint32_t lba, uint32_t off)
{
    uint32_t blocks, bsize;
    if (read_capacity(lun, &blocks, &bsize) || lba >= blocks || bsize > DATA_MAX ||
        read_blocks(lun, lba, 1, bsize)) {
        put_failure("can't read that block");
        return;
    }
    off &= ~15U;
    for (uint32_t row = off; row < off + 256 && row < bsize; row += 16) {
        serial_puthex(row, 3);
        serial_puts(":");
        for (int i = 0; i < 16; i++) {
            serial_puts(" ");
            uint8_t b = data[row + i];
            serial_putc("0123456789abcdef"[b >> 4]);
            serial_putc("0123456789abcdef"[b & 15]);
        }
        serial_puts("  ");
        for (int i = 0; i < 16; i++) {
            uint8_t b = data[row + i];
            serial_putc(b >= 32 && b < 127 ? (char)b : '.');
        }
        serial_puts("\n");
    }
}

/* ── userdata, OsitoFS's device ───────────────────────────────── */

static void find_rw_part(const gpt_part_t *p, uint32_t bsize, void *ctx)
{
    (void)bsize;
    if (strcmp(p->name, RW_PART) == 0)
        *(gpt_part_t *)ctx = *p;
}

/* Find userdata on LUN 0 and open it to writes. 0 if ready. */
static int ufs_attach(void)
{
    if (rw_blocks)
        return 0;
    gpt_part_t part = { .last = 0 };
    uint32_t bsize;
    if (gpt_scan(RW_LUN, &bsize, find_rw_part, &part) < 0)
        return -1;
    if (!part.last || part.last < part.first || part.last >= 0xFFFFFFFFULL || bsize != 4096) {
        serial_puts("ufs: no " RW_PART " partition on LUN 0\n");
        return -1;
    }
    rw_first = (uint32_t)part.first;
    rw_bsize = bsize;
    rw_blocks = (uint32_t)(part.last - part.first + 1);
    serial_puts("[UFS ] " RW_PART ": LBA ");
    serial_putdec(rw_first);
    serial_puts("-");
    serial_putdec(part.last);
    serial_puts(", ");
    put_mb((uint64_t)rw_blocks * rw_bsize);
    serial_puts(", writable\n");
    return 0;
}

/* Bytes from userdata's start, through data[] in commands of up to 16
 * blocks. A write that covers a block only partly reads it first. */
static int part_io(uint64_t off, uint8_t *buf, uint64_t len, int write)
{
    uint64_t size = (uint64_t)rw_blocks * rw_bsize;
    if (!rw_blocks || off > size || len > size - off)
        return -1;
    uint32_t bs = rw_bsize, max = DATA_MAX / bs;
    while (len) {
        uint32_t skip = (uint32_t)(off % bs);
        uint64_t nblk = (skip + len + bs - 1) / bs;
        if (nblk > max)
            nblk = max;
        uint64_t span = nblk * bs - skip;
        if (span > len)
            span = len;
        uint32_t lba = rw_first + (uint32_t)(off / bs);
        if (!write || skip || (skip + span) % bs) {
            if (read_blocks(RW_LUN, lba, (uint32_t)nblk, bs))
                return -1;
        }
        if (write) {
            memcpy((uint8_t *)data + skip, buf, span);
            if (rw_blocks_io(RW_LUN, lba, (uint32_t)nblk, bs, 1))
                return -1;
        } else {
            memcpy(buf, (const uint8_t *)data + skip, span);
        }
        off += span;
        buf += span;
        len -= span;
    }
    return 0;
}

static int part_read(uint64_t off, void *buf, uint64_t len)
{
    return part_io(off, buf, len, 0);
}

static int part_write(uint64_t off, const void *buf, uint64_t len)
{
    return part_io(off, (uint8_t *)buf, len, 1);
}

static int part_flush(void)
{
    uint8_t cdb[16] = { SYNC_CACHE_10 };
    return scsi(RW_LUN, cdb, 0, 0) == SCSI_GOOD ? 0 : -1;
}

static uint64_t part_size(void)
{
    return (uint64_t)rw_blocks * rw_bsize;
}

/* userdata becomes OsitoFS's device (v3, and v2 for `ufs migrate`) */
static int attach_fs(void)
{
    if (ufs_attach())
        return -1;
    osfs2_set_backend(part_read, part_write, part_flush);
    osfs3_set_backend(part_read, part_write, part_flush, part_size);
    return 0;
}

/* `ufs mkfs userdata`: an empty OsitoFS v3 over it, then mounted. The name
 * must be typed: it erases what was there. */
static void cmd_mkfs(const char *confirm)
{
    if (strcmp(confirm, RW_PART) != 0) {
        serial_puts("ufs mkfs: type the partition's name to erase it: ufs mkfs " RW_PART "\n");
        return;
    }
    if (attach_fs())
        return;
    serial_puts("[UFS ] formatting " RW_PART " as OsitoFS v3 (its first 32 GB; writes 4 MB)\n");
    osfs2_unmount();
    if (osfs3_format(part_size(), "rog5", 0)) {
        put_failure("format failed");
        return;
    }
    osfs3_mount(0);
}

/* `ufs mount`: mount the OsitoFS v3 on userdata, or say how to convert a v2 */
static void cmd_mount(void)
{
    if (osfs3_is_mounted()) {
        serial_puts("ufs: already mounted\n");
        return;
    }
    if (attach_fs())
        return;
    if (osfs3_mount(0) == 0)
        return;
    if (osfs2_is_mounted() || osfs2_mount(0) == 0)
        serial_puts("ufs: " RW_PART " holds OsitoFS v2: `ufs migrate " RW_PART
                    "` turns it into v3, files kept\n");
    else
        serial_puts("ufs: no OsitoFS on " RW_PART " (ufs mkfs " RW_PART " makes one)\n");
}

/* `ufs migrate userdata`: the v2 volume as v3, files kept. Every file is
 * read into high memory, the partition formatted as v3, and each written
 * back into the root, read back and checked against its CRC. */
static void cmd_migrate(const char *confirm)
{
    if (strcmp(confirm, RW_PART) != 0) {
        serial_puts("ufs migrate: type the partition's name: ufs migrate " RW_PART "\n");
        return;
    }
    if (osfs3_is_mounted()) {
        serial_puts("ufs migrate: " RW_PART " is OsitoFS v3 already\n");
        return;
    }
    if (attach_fs())
        return;
    if (!osfs2_is_mounted() && osfs2_mount(0) != 0) {
        serial_puts("ufs migrate: no OsitoFS v2 on " RW_PART "\n");
        return;
    }
    struct mfile {
        char     name[OSFS2_NAME_LEN + 1];
        uint8_t *data;
        uint64_t size;
        uint32_t crc;
    };
    uint32_t n = osfs2_file_count();
    uint64_t list_bytes = (uint64_t)(n ? n : 1) * sizeof(struct mfile);
    struct mfile *files = mem_alloc_aligned_high(list_bytes, 4096);
    if (!files) {
        serial_puts("ufs migrate: out of memory\n");
        return;
    }
    memset(files, 0, list_bytes);
    uint64_t total = 0;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        osfs2_file_t *f = osfs2_file_at(i);
        struct mfile *m = &files[kept];
        if (!f)
            break;
        memcpy(m->name, f->name, OSFS2_NAME_LEN);
        m->size = f->size;
        if (m->size) {
            m->data = mem_alloc_aligned_high(m->size, 4096);
            if (!m->data || osfs2_read(f, 0, m->data, m->size) != (int)m->size) {
                serial_puts("ufs migrate: can't read ");
                serial_puts(m->name);
                serial_puts(": nothing changed\n");
                goto out;
            }
        }
        m->crc = osfs3_crc32(m->data, m->size);
        total += m->size;
        kept++;
    }
    serial_puts("[UFS ] migrate: ");
    serial_putdec(kept);
    serial_puts(" files, ");
    serial_putdec(total >> 20);
    serial_puts(" MB read; formatting as OsitoFS v3\n");

    osfs2_unmount();
    if (osfs3_format(part_size(), "rog5", 0) || osfs3_mount(0)) {
        put_failure("format failed; the files are lost");
        goto out;
    }
    uint32_t ok = 0;
    for (uint32_t i = 0; i < kept; i++) {
        struct mfile *m = &files[i];
        char path[FS_PATH_MAX];
        path[0] = '/';
        strcpy(path + 1, m->name);
        int good = fs_write_file(path, m->data, m->size) == 0;
        if (good && m->size) {
            /* Read back over the copy itself, and check */
            memset(m->data, 0, m->size);
            void *f = osfs3_find(path);
            good = f && osfs3_read_file(f, 0, m->data, m->size) == (int)m->size &&
                   osfs3_crc32(m->data, m->size) == m->crc;
        }
        ok += good;
        if (!good) {
            serial_puts("ufs migrate: ");
            serial_puts(m->name);
            serial_puts(" failed\n");
        }
    }
    serial_puts("[UFS ] migrate: ");
    serial_putdec(ok);
    serial_puts(" of ");
    serial_putdec(kept);
    serial_puts(" files written and verified\n");
out:
    for (uint32_t i = 0; i < n; i++)
        if (files[i].data)
            mem_free_pages(files[i].data, (files[i].size + 4095) / 4096);
    mem_free_pages(files, (list_bytes + 4095) / 4096);
}

/* At boot: mount the OsitoFS on userdata if it has one */
void ufs_mount_at_boot(void)
{
    if (!clocked()) {
        serial_puts("[UFS ] controller powered down: no filesystem\n");
        return;
    }
    cmd_mount();
}

/* Decimal, or hex after 0x */
static int parse_num(const char *s, uint32_t *out)
{
    uint32_t v = 0, base = 10;
    if (s[0] == '0' && s[1] == 'x') {
        base = 16;
        s += 2;
    }
    if (!*s)
        return -1;
    for (; *s; s++) {
        uint32_t d = *s >= '0' && *s <= '9' ? (uint32_t)(*s - '0') :
                     *s >= 'a' && *s <= 'f' ? (uint32_t)(*s - 'a' + 10) : 99;
        if (d >= base)
            return -1;
        v = v * base + d;
    }
    *out = v;
    return 0;
}

static void survey(void)
{
    int gdsc = (gcc(GCC_UFS_GDSCR) & GDSC_PWR_ON) != 0;
    int on[NCLOCKS];
    serial_puts("[UFS ] gdsc=");
    serial_putc(gdsc ? '1' : '0');
    for (int i = 0; i < NCLOCKS; i++) {
        on[i] = !(gcc(clocks[i].cbcr) & CBCR_CLK_OFF);
        serial_puts(" ");
        serial_puts(clocks[i].name);
        serial_putc('=');
        serial_putc(on[i] ? '1' : '0');
    }
    serial_puts("\n");
    if (!gdsc || !on[0] || !on[1]) {
        serial_puts("[UFS ] powered or clocked down: not touching the controller\n");
        return;
    }

    uint32_t cap = ufs(REG_CAP), ver = ufs(REG_VER), hcs = ufs(REG_HCS);
    serial_puts("[UFS ]");
    put_reg("cap", cap);
    serial_puts(" (");
    serial_putdec(CAP_NUTRS(cap));
    serial_puts(" request slots, ");
    serial_putdec(CAP_NUTMRS(cap));
    serial_puts(cap & CAP_64AS ? " task slots, 64-bit)" : " task slots, 32-bit)");
    put_reg("ver", ver);
    serial_puts(" (UFSHCI ");
    serial_putdec((ver >> 8) & 0xFF);
    serial_puts(".");
    serial_putdec((ver >> 4) & 0xF);
    serial_puts(")\n[UFS ]");
    put_reg("hcs", hcs);
    serial_puts(" (");
    put_flag("device-present", hcs & HCS_DP);
    put_flag("utrl-ready", hcs & HCS_UTRLRDY);
    put_flag("utmrl-ready", hcs & HCS_UTMRLRDY);
    put_flag("uic-ready", hcs & HCS_UCRDY);
    serial_puts(" )");
    put_reg("hce", ufs(REG_HCE));
    serial_puts("\n[UFS ]");
    put_reg("is", ufs(REG_IS));
    put_reg("ie", ufs(REG_IE));
    put_reg("ahit", ufs(REG_AHIT));
    serial_puts("\n[UFS ] utrl ");
    serial_puthex(((uint64_t)ufs(REG_UTRLBAU) << 32) | ufs(REG_UTRLBA), 10);
    put_reg("run", ufs(REG_UTRLRSR));
    put_reg("doorbell", ufs(REG_UTRLDBR));
    serial_puts("\n[UFS ] utmrl ");
    serial_puthex(((uint64_t)ufs(REG_UTMRLBAU) << 32) | ufs(REG_UTMRLBA), 10);
    put_reg("run", ufs(REG_UTMRLRSR));
    put_reg("doorbell", ufs(REG_UTMRLDBR));
    serial_puts("\n");

    if (smmu_dma_identity("ufs", UFS_SID, DMA_WINDOW_BASE, DMA_WINDOW_SIZE))
        serial_puts("[UFS ] DMA to the DMA window is 1:1\n");
    else
        serial_puts("[UFS ] DMA to the DMA window is NOT 1:1\n");
}

/* Shell `ufs`: the controller's state. `ufs luns`, `ufs gpt <lun>` and
 * `ufs read <lun> <lba> [offset]` read the device; nothing writes to it. */
void platform_ufs_cmd(int argc, char **argv)
{
    uint32_t lun, lba, off = 0;
    if (argc == 1)
        survey();
    else if (argc == 2 && strcmp(argv[1], "luns") == 0)
        cmd_luns();
    else if (argc == 2 && strcmp(argv[1], "mount") == 0)
        cmd_mount();
    else if (argc == 3 && strcmp(argv[1], "mkfs") == 0)
        cmd_mkfs(argv[2]);
    else if (argc == 3 && strcmp(argv[1], "migrate") == 0)
        cmd_migrate(argv[2]);
    else if (argc == 3 && strcmp(argv[1], "gpt") == 0 && !parse_num(argv[2], &lun) && lun < 8)
        cmd_gpt((uint8_t)lun);
    else if ((argc == 4 || argc == 5) && strcmp(argv[1], "read") == 0 &&
             !parse_num(argv[2], &lun) && lun < 8 && !parse_num(argv[3], &lba) &&
             (argc == 4 || !parse_num(argv[4], &off)))
        cmd_read((uint8_t)lun, lba, off);
    else
        serial_puts("usage: ufs [luns | gpt <lun> | read <lun> <lba> [offset] | mount |\n"
                    "            mkfs " RW_PART " | migrate " RW_PART "]\n");
}
