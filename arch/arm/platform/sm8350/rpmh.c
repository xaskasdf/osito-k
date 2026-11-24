/*
 * rpmh.c -- votes to the AOP through the apps RSC, and the command DB
 *
 * Most of the phone's power rails aren't the apps processor's to set: the
 * PMIC arbiter lets EE 0 write none of PM8350C's LDOs (`pmic list 2`). The
 * AOP owns them and sets each from the votes of every subsystem, which
 * reach it through that subsystem's RSC (Resource State Coordinator). Ours
 * is rsc@18200000 in the phone's device tree, DRV 2 (qcom,drv-id), with its
 * TCSes (Trigger Command Sets: lists of address/data writes) at +0xd00:
 * tcs-config ACTIVE 2, SLEEP 3, WAKE 3, CONTROL 0, so TCS 0 and 1 are the
 * AMCs that send a vote at once.
 *
 * Resource addresses come from the command DB the AOP leaves in DDR
 * (cmd-db@80860000, mapped by mmu.c): "ldoc3" -> PM8350C LDO3's VRM.
 *
 * Only active-only votes, one command per AMC, polled; sleep and wake sets
 * would only matter if the CPU cluster power-collapsed, and Osito-K never
 * does. Layouts from Linux's rpmh-rsc.c (RSC v2.7 registers; other majors
 * are refused) and cmd-db.c.
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "../../include/aarch64.h"
#include "../../include/spinlock.h"
#include "rpmh.h"

#define CMD_DB              0x80860000UL
#define CMD_DB_SIZE         0x20000
#define CMD_DB_MAGIC        0x0C0330DB      /* bytes db 30 03 0c at +4 */
#define CMD_DB_SLAVES       8               /* rsc_hdr, 16 bytes each, at +8 */
#define CMD_DB_DATA         0x90            /* entries start here + header_offset */
#define ENTRY_SIZE          24              /* id[8], priority[2], addr, len, offset */
#define SLAVE_VRM           4               /* address bits 18:16 */

#define RSC_DRV             0x18220000UL    /* drv-2 */
#define RSC_DRV_NUM         2
#define DRV_ID              0x00            /* version in bits 23:8 */
#define DRV_SOLVER_CONFIG   0x04
#define DRV_PRNT_CHLD_CONFIG 0x0C           /* TCS count (6 bits per DRV), commands per TCS */
#define TCS_BASE            (RSC_DRV + 0xD00)
#define IRQ_ENABLE          0x00            /* at TCS_BASE, one bit per TCS */
#define IRQ_STATUS          0x04
#define IRQ_CLEAR           0x08
#define TCS(n)              (TCS_BASE + 672UL * (n))
#define TCS_CONTROL         0x14
#define TCS_STATUS          0x18            /* 0 while the TCS is busy */
#define TCS_CMD_ENABLE      0x1C
#define CMD(n, c)           (TCS(n) + 20UL * (c))
#define CMD_MSGID           0x30
#define CMD_ADDR            0x34
#define CMD_DATA            0x38
#define CMD_STATUS          0x3C

#define AMC_COUNT           2
#define AMC_ENABLE          (1U << 16)
#define AMC_TRIGGER         (1U << 24)
#define MSGID_WRITE_ACK     ((1U << 16) | (1U << 8) | 8)    /* write, response required */
#define STATUS_ISSUED       (1U << 8)
#define STATUS_COMPL        (1U << 16)

#define VRM_VOLTAGE         0x0             /* mV */
#define VRM_ENABLE          0x4
#define VRM_MODE            0x8

static spinlock_t rsc_lock;

/* The command DB is mapped Device: byte reads only, never unaligned */
static uint32_t db(uint32_t off, int bytes)
{
    uint32_t v = 0;
    for (int i = bytes - 1; i >= 0; i--)
        v = v << 8 | *(volatile uint8_t *)(CMD_DB + off + (uint32_t)i);
    return v;
}

static int cmd_db_ok(void)
{
    return db(4, 4) == CMD_DB_MAGIC;
}

int cmd_db_addr(const char *name, uint32_t *addr)
{
    if (!cmd_db_ok())
        return RPMH_ERR_VERSION;
    uint8_t id[8] = { 0 };
    for (int k = 0; k < 8 && name[k]; k++)
        id[k] = (uint8_t)name[k];
    for (int s = 0; s < CMD_DB_SLAVES; s++) {
        uint32_t hdr = 8 + 16 * (uint32_t)s;
        if (!db(hdr, 2))
            break;
        uint32_t ent = CMD_DB_DATA + db(hdr + 2, 2), cnt = db(hdr + 6, 2);
        for (uint32_t j = 0; j < cnt && ent + ENTRY_SIZE <= CMD_DB_SIZE; j++, ent += ENTRY_SIZE) {
            int k = 0;
            while (k < 8 && db(ent + (uint32_t)k, 1) == id[k])
                k++;
            if (k == 8) {
                *addr = db(ent + 16, 4);
                return 0;
            }
        }
    }
    return RPMH_ERR_NO_RES;
}

static uint32_t rd(uint64_t a) { return mmio_rd32(a); }
static void wr(uint64_t a, uint32_t v) { mmio_wr32(a, v); }

/* A write the RSC must have taken before the next one (write_tcs_reg_sync) */
static void wr_sync(uint64_t a, uint32_t v)
{
    wr(a, v);
    for (int i = 0; i < 100 && rd(a) != v; i++)
        udelay(1);
}

static int rsc_ok(void)
{
    return ((rd(RSC_DRV + DRV_ID) >> 16) & 0xFF) == 2;
}

int rpmh_write(uint32_t addr, uint32_t data)
{
    if (!rsc_ok())
        return RPMH_ERR_VERSION;
    uint64_t flags = spin_lock_irqsave(&rsc_lock);
    int tcs = -1;
    for (int t = 0; t < AMC_COUNT && tcs < 0; t++)
        if (rd(TCS(t) + TCS_STATUS))
            tcs = t;
    int err = RPMH_ERR_BUSY;
    if (tcs >= 0) {
        uint32_t bit = 1U << tcs;
        /* The completion shows in IRQ_STATUS; no GIC line is enabled for it */
        wr(TCS_BASE + IRQ_ENABLE, rd(TCS_BASE + IRQ_ENABLE) | bit);
        wr(TCS_BASE + IRQ_CLEAR, bit);
        wr(TCS(tcs) + TCS_CMD_ENABLE, 0);
        wr(CMD(tcs, 0) + CMD_MSGID, MSGID_WRITE_ACK);
        wr(CMD(tcs, 0) + CMD_ADDR, addr);
        wr(CMD(tcs, 0) + CMD_DATA, data);
        wr(TCS(tcs) + TCS_CMD_ENABLE, 1);
        /* Trigger as __tcs_set_trigger does: clear the trigger, then the
         * AMC mode, set the mode again, then trigger */
        uint32_t ctl = rd(TCS(tcs) + TCS_CONTROL) & ~AMC_TRIGGER;
        wr_sync(TCS(tcs) + TCS_CONTROL, ctl);
        wr_sync(TCS(tcs) + TCS_CONTROL, ctl & ~AMC_ENABLE);
        wr_sync(TCS(tcs) + TCS_CONTROL, AMC_ENABLE);
        wr(TCS(tcs) + TCS_CONTROL, AMC_ENABLE | AMC_TRIGGER);
        err = RPMH_ERR_TIMEOUT;
        for (int i = 0; i < 20000; i++) {
            if (rd(TCS_BASE + IRQ_STATUS) & bit) {
                uint32_t st = rd(CMD(tcs, 0) + CMD_STATUS);
                err = (st & STATUS_ISSUED) && (st & STATUS_COMPL) ? 0 : RPMH_ERR_NACK;
                break;
            }
            udelay(1);
        }
        wr(TCS(tcs) + TCS_CMD_ENABLE, 0);
        wr(TCS_BASE + IRQ_CLEAR, bit);
    }
    spin_unlock_irqrestore(&rsc_lock, flags);
    return err;
}

int rpmh_vrm_vote(const char *name, uint32_t mv, uint32_t mode, int on)
{
    uint32_t a;
    int err = cmd_db_addr(name, &a);
    if (err)
        return err;
    if (((a >> 16) & 7) != SLAVE_VRM)
        return RPMH_ERR_NO_RES;
    if (on) {
        err = rpmh_write(a + VRM_VOLTAGE, mv);
        if (!err)
            err = rpmh_write(a + VRM_MODE, mode);
    }
    if (!err)
        err = rpmh_write(a + VRM_ENABLE, on ? 1 : 0);
    return err;
}

const char *rpmh_err(int err)
{
    switch (err) {
    case 0:                 return "ok";
    case RPMH_ERR_VERSION:  return "no RSC v2 / command DB";
    case RPMH_ERR_NO_RES:   return "no such VRM in the command DB";
    case RPMH_ERR_BUSY:     return "both AMCs busy";
    case RPMH_ERR_TIMEOUT:  return "no completion";
    case RPMH_ERR_NACK:     return "not acknowledged";
    }
    return "?";
}

static const char *slave_name(uint32_t id)
{
    return id == 3 ? "ARC" : id == SLAVE_VRM ? "VRM" : id == 5 ? "BCM" : "?";
}

/* Shell `rpmh`: the RSC and the command DB; `rpmh <name>` looks one
 * resource up. Read-only: votes only come from drivers (touch.c), with the
 * voltages the device tree fixes. */
void platform_rpmh_cmd(int argc, char **argv)
{
    if (argc == 2) {
        uint32_t a;
        int err = cmd_db_addr(argv[1], &a);
        serial_puts(argv[1]);
        if (err) {
            serial_puts(": ");
            serial_puts(rpmh_err(err));
        } else {
            serial_puts(" at ");
            serial_puthex(a, 5);
            serial_puts(" (");
            serial_puts(slave_name((a >> 16) & 7));
            serial_puts(")");
        }
        serial_puts("\n");
        return;
    }
    if (argc != 1) {
        serial_puts("usage: rpmh [<resource name>]\n");
        return;
    }
    uint32_t id = rd(RSC_DRV + DRV_ID), cfg = rd(RSC_DRV + DRV_PRNT_CHLD_CONFIG);
    serial_puts("apps RSC v");
    serial_putdec((id >> 16) & 0xFF);
    serial_puts(".");
    serial_putdec((id >> 8) & 0xFF);
    serial_puts(", DRV 2: ");
    serial_putdec((cfg >> (6 * RSC_DRV_NUM)) & 0x3F);
    serial_puts(" TCS, ");
    serial_putdec((cfg >> 27) & 0x1F);
    serial_puts(" commands each, ");
    serial_puts((rd(RSC_DRV + DRV_SOLVER_CONFIG) >> 24) & 1 ? "hw solver\n" : "no hw solver\n");
    for (int t = 0; t < AMC_COUNT; t++) {
        serial_puts("  AMC ");
        serial_putdec((uint32_t)t);
        serial_puts(rd(TCS(t) + TCS_STATUS) ? ": idle" : ": busy");
        serial_puts(", control ");
        serial_puthex(rd(TCS(t) + TCS_CONTROL), 8);
        serial_puts(", commands ");
        serial_puthex(rd(TCS(t) + TCS_CMD_ENABLE), 4);
        serial_puts("\n");
    }
    if (!cmd_db_ok()) {
        serial_puts("command DB: bad magic at 0x80860000\n");
        return;
    }
    serial_puts("command DB v");
    serial_putdec(db(0, 4));
    serial_puts(":");
    for (int s = 0; s < CMD_DB_SLAVES; s++) {
        uint32_t hdr = 8 + 16 * (uint32_t)s, slv = db(hdr, 2);
        if (!slv)
            break;
        serial_puts(" ");
        serial_puts(slave_name(slv));
        serial_puts(" ");
        serial_putdec(db(hdr + 6, 2));
    }
    serial_puts(" resources\n");
}
