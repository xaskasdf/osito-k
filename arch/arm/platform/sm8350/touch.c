/*
 * touch.c -- the ROG Phone 5's touchscreen: a FocalTech controller
 *
 * From the phone's device tree: "focaltech,fts" at I2C address 0x38 on
 * QUP0 SE4 (i2c@990000, qcom,i2c-geni), SDA/SCL on TLMM GPIO 20/21
 * (function qup4, pull-up), reset on GPIO 22, interrupt on GPIO 23, an IO
 * power switch on GPIO 131; supplies pm8350c_l3 (3.008 V) and pm8350c_l8
 * (1.8 V), RPMh regulators on PMIC SID 2; coordinates 1080 x 2448, ten
 * fingers.
 *
 * ABL leaves the controller unpowered and both LDOs off. They are the
 * AOP's, so `touch on` votes them on through RPMh (rpmh.c) with the device
 * tree's fixed voltages, then resets the controller and reads its IDs.
 *
 * The bus: SE4's QUP firmware has its FIFO interface disabled
 * (GENI_IF_DISABLE_RO): the SE is set up for GPI DMA, which Android shares
 * with its trusted-touch VM, so Linux drives it through the GSI. Until
 * Osito-K has a GPI driver, it bit-bangs I2C on the same two pins as GPIOs
 * (their sleep state is function gpio anyway): open drain through the
 * output enable, with the pull-ups, at about 200 kHz. A read of one or two
 * fingers takes under a millisecond.
 *
 * The touch task (started at boot) powers it up, then polls every 10 ms:
 * the TLMM latches the interrupt line's falling edge (raw status, routed
 * nowhere), and the task reads the first point when it's set or while a
 * finger is down. Presses, moves and lifts go into a queue the desktop
 * drains (platform_touch_event).
 *
 * Offsets: Linux gcc-sm8350.c, pinctrl-sm8350.c + pinctrl-msm.c (TLMM),
 * geni-se.h, the PMIC5 LDO layout; the FocalTech registers from its
 * Android driver.
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "../../include/aarch64.h"
#include "../../include/spinlock.h"
#include "pmic_arb.h"
#include "rpmh.h"
#include "touch.h"

#define TLMM_PINS           0x0F100000UL    /* TLMM + 0x100000: pin n at +0x1000 * n */
#define PIN_CFG(n)          (TLMM_PINS + 0x1000UL * (n))
#define PIN_IO(n)           (PIN_CFG(n) + 4)
#define CFG_PULL(c)         ((c) & 3)       /* 0 none, 1 down, 2 keeper, 3 up */
#define CFG_FUNC(c)         (((c) >> 2) & 0xF)
#define CFG_DRV(c)          ((((c) >> 6) & 7) * 2 + 2)  /* mA */
#define CFG_OE              (1U << 9)
#define IO_OUT_HIGH         (1U << 1)       /* IN_OUT: bit 1 drives, bit 0 reads */
#define PIN_INTR_CFG(n)     (PIN_CFG(n) + 8)
#define PIN_INTR_STATUS(n)  (PIN_CFG(n) + 0xc)  /* bit 0, cleared by writing 0 */
#define INTR_CFG_MASK       0x1FU
#define INTR_FALLING_RAW    ((1U << 4) | (2U << 2) | (1U << 1))   /* raw status,
                                               falling edge; not enabled */
#define PULL_NONE           0
#define PULL_UP             3

#define TS_SDA              20
#define TS_SCL              21
#define TS_RESET            22
#define TS_IRQ              23
#define TS_VDDIO            131
#define TS_ADDR             0x38

#define GCC                 0x00100000UL
#define GCC_QUP_VOTE        0x52008         /* APCS branch enable votes */
#define CBCR_OFF            (1U << 31)
#define CBCR_HW_CTL         (1U << 1)

#define SE4                 0x00990000UL
#define GENI_STATUS         0x040
#define GENI_IF_DISABLE_RO  0x064           /* bit 0: no FIFO, GPI DMA only */
#define GENI_FW_REVISION    0x068           /* protocol in bits 15:8 */

#define I2C_HALF_US         2
#define I2C_ERR_NACK        (-1)
#define I2C_ERR_STUCK       (-2)            /* SCL held low */

#define FTS_REG_POINTS      0x00            /* 0x02 count, 6 bytes a point from 0x03 */
#define FTS_REG_CHIP_ID2    0x9F
#define FTS_REG_CHIP_ID     0xA3
#define FTS_REG_FW_VER      0xA6
#define FTS_REG_VENDOR_ID   0xA8
#define FTS_POINTS          10
#define FTS_POINTS_LEN      (3 + 6 * FTS_POINTS)
#define FTS_EV_DOWN         0               /* a point's event, bits 7:6 */
#define FTS_EV_UP           1
#define FTS_EV_CONTACT      2

#define POLL_MS             10
#define QUEUE               32

#define PM8350C             2
#define LDO(n)              (0xC000 + 0x100 * (n))  /* type 0x04 (`pmic list 2`) */
#define LDO_STATUS1         0x08            /* bit 7: VREG_READY */
#define LDO_VSET_LB         0x40            /* mV, little-endian */
#define LDO_EN_CTL          0x46            /* bit 7: enabled */

/* QUP0's branches, for the survey. The AHB ones may be hardware-gated
 * (CBCR bit 1): then they read off while idle. */
static const struct {
    const char *name;
    uint32_t    cbcr;
    uint32_t    vote_bit;
    int         hwcg;
} qup_clocks[] = {
    { "m-ahb", 0x17004, 6,  1 },
    { "s-ahb", 0x17008, 7,  1 },
    { "core",  0x23000, 8,  0 },
    { "2x",    0x23008, 9,  0 },
    { "se4",   0x174cc, 14, 0 },
};
#define NQUP (int)(sizeof(qup_clocks) / sizeof(qup_clocks[0]))

static spinlock_t bus_lock;
static volatile int touch_ready;
static int task_started;
static uint32_t presses, reads;
static int32_t last_x = -1, last_y = -1;

/* Touch events for the desktop: one producer (the task), one consumer */
static struct {
    int16_t x, y;
    uint8_t down;
} queue[QUEUE];
static volatile uint32_t q_head, q_tail;

static void put_pin(const char *what, int n)
{
    uint32_t cfg = mmio_rd32(PIN_CFG(n)), io = mmio_rd32(PIN_IO(n));
    static const char *const pulls[] = { "none", "down", "keeper", "up" };
    serial_puts("  gpio");
    serial_putdec(n);
    serial_puts(n < 100 ? (n < 10 ? "   " : "  ") : " ");
    serial_puts(what);
    serial_puts(": func ");
    serial_putdec(CFG_FUNC(cfg));
    serial_puts(", pull ");
    serial_puts(pulls[CFG_PULL(cfg)]);
    serial_puts(", ");
    serial_putdec(CFG_DRV(cfg));
    serial_puts(" mA, ");
    serial_puts(cfg & CFG_OE ? "output " : "input ");
    serial_putc(io & 2 ? '1' : '0');
    serial_puts(", reads ");
    serial_putc(io & 1 ? '1' : '0');
    serial_puts("\n");
}

/* A GPIO (function 0); outputs get their level before the enable */
static void pin_set(int n, uint32_t pull, uint32_t ma, int out, int high)
{
    if (out)
        mmio_wr32(PIN_IO(n), high ? IO_OUT_HIGH : 0);
    mmio_wr32(PIN_CFG(n), pull | (ma / 2 - 1) << 6 | (out ? CFG_OE : 0));
}

static void pin_drive(int n, int high)
{
    mmio_wr32(PIN_IO(n), high ? IO_OUT_HIGH : 0);
}

static int pin_level(int n)
{
    return mmio_rd32(PIN_IO(n)) & 1;
}

static void put_ldo(int n, const char *use)
{
    uint8_t type = 0, sub = 0, st = 0, en = 0, lo = 0, hi = 0;
    int err = pmic_read(PM8350C, LDO(n) + 0x04, &type) | pmic_read(PM8350C, LDO(n) + 0x05, &sub) |
              pmic_read(PM8350C, LDO(n) + LDO_STATUS1, &st) | pmic_read(PM8350C, LDO(n) + LDO_EN_CTL, &en) |
              pmic_read(PM8350C, LDO(n) + LDO_VSET_LB, &lo) | pmic_read(PM8350C, LDO(n) + LDO_VSET_LB + 1, &hi);
    serial_puts("  pm8350c_l");
    serial_putdec(n);
    serial_puts(" (");
    serial_puts(use);
    serial_puts("): ");
    if (err) {
        serial_puts("can't read it\n");
        return;
    }
    serial_puts("type ");
    serial_puthex(type, 2);
    serial_puts("/");
    serial_puthex(sub, 2);
    serial_puts(en & 0x80 ? ", enabled" : ", disabled");
    serial_puts(st & 0x80 ? ", ready, " : ", not ready, ");
    serial_putdec((uint32_t)lo | (uint32_t)hi << 8);
    serial_puts(" mV set\n");
}

static int qup_clock_on(int i)
{
    uint32_t cbcr = mmio_rd32(GCC + qup_clocks[i].cbcr);
    return !(cbcr & CBCR_OFF) || (qup_clocks[i].hwcg && (cbcr & CBCR_HW_CTL));
}

/* --- I2C, bit-banged. A line is released (input, pulled up) or driven low
 * (output enable, the output level is always 0). Reading the config back
 * makes sure the TLMM has the write before the delay starts. */

static void line(int pin, int high)
{
    mmio_wr32(PIN_CFG(pin), PULL_UP | (high ? 0 : CFG_OE));
    (void)mmio_rd32(PIN_CFG(pin));
}

static void half(void)
{
    udelay(I2C_HALF_US);
}

/* Release SCL and wait for it to rise: the controller may stretch it */
static int scl_high(void)
{
    line(TS_SCL, 1);
    for (int i = 0; i < 1000; i++) {
        if (pin_level(TS_SCL))
            return 0;
        udelay(1);
    }
    return I2C_ERR_STUCK;
}

static int i2c_start(void)
{
    line(TS_SDA, 1);
    int err = scl_high();
    half();
    line(TS_SDA, 0);
    half();
    line(TS_SCL, 0);
    return err;
}

static void i2c_stop(void)
{
    line(TS_SDA, 0);
    half();
    scl_high();
    half();
    line(TS_SDA, 1);
    half();
}

static int i2c_put(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        line(TS_SDA, (b >> i) & 1);
        half();
        if (scl_high())
            return I2C_ERR_STUCK;
        half();
        line(TS_SCL, 0);
    }
    line(TS_SDA, 1);
    half();
    if (scl_high())
        return I2C_ERR_STUCK;
    int nack = pin_level(TS_SDA);
    half();
    line(TS_SCL, 0);
    return nack ? I2C_ERR_NACK : 0;
}

static int i2c_get(uint8_t *b, int ack)
{
    uint8_t v = 0;
    line(TS_SDA, 1);
    for (int i = 0; i < 8; i++) {
        half();
        if (scl_high())
            return I2C_ERR_STUCK;
        v = (uint8_t)(v << 1 | pin_level(TS_SDA));
        half();
        line(TS_SCL, 0);
    }
    line(TS_SDA, !ack);
    half();
    if (scl_high())
        return I2C_ERR_STUCK;
    half();
    line(TS_SCL, 0);
    line(TS_SDA, 1);
    *b = v;
    return 0;
}

/* Free a bus a reset left mid-byte: clock until the slave lets SDA go */
static void i2c_recover(void)
{
    mmio_wr32(PIN_IO(TS_SDA), 0);
    mmio_wr32(PIN_IO(TS_SCL), 0);
    line(TS_SDA, 1);
    line(TS_SCL, 1);
    for (int i = 0; i < 9 && !pin_level(TS_SDA); i++) {
        half();
        line(TS_SCL, 0);
        half();
        scl_high();
    }
    i2c_stop();
}

/* Write w (if any), then read r (if any) after a repeated start */
static int i2c_xfer(uint8_t addr, const uint8_t *w, int wlen, uint8_t *r, int rlen)
{
    uint64_t flags = spin_lock_irqsave(&bus_lock);
    int err = 0;
    if (wlen) {
        err = i2c_start();
        if (!err)
            err = i2c_put((uint8_t)(addr << 1));
        for (int i = 0; i < wlen && !err; i++)
            err = i2c_put(w[i]);
    }
    if (!err && rlen) {
        err = i2c_start();
        if (!err)
            err = i2c_put((uint8_t)(addr << 1 | 1));
        for (int i = 0; i < rlen && !err; i++)
            err = i2c_get(&r[i], i < rlen - 1);
    }
    i2c_stop();
    spin_unlock_irqrestore(&bus_lock, flags);
    return err;
}

static const char *i2c_err(int err)
{
    return err == I2C_ERR_NACK ? "no ACK" : err == I2C_ERR_STUCK ? "SCL held low" : "?";
}

static int fts_read(uint8_t reg, uint8_t *buf, int len)
{
    return i2c_xfer(TS_ADDR, &reg, 1, buf, len);
}

/* Power up, reset, and ask the controller who it is. verbose: the shell's
 * `touch on`, with the supplies' state; else one line for the boot log. */
static void touch_on(int verbose)
{
    touch_ready = 0;
    /* Held in reset while the supplies come up (fts_power_source_ctrl) */
    pin_set(TS_RESET, PULL_UP, 8, 1, 0);
    pin_set(TS_IRQ, PULL_UP, 8, 0, 0);
    int err = rpmh_vrm_vote("ldoc3", 3008, RPMH_LDO_HPM, 1);
    if (!err)
        err = rpmh_vrm_vote("ldoc8", 1800, RPMH_LDO_HPM, 1);
    if (err) {
        serial_puts("touch: supply vote: ");
        serial_puts(rpmh_err(err));
        serial_puts("\n");
        return;
    }
    pin_set(TS_VDDIO, PULL_NONE, 2, 1, 1);
    mdelay(2);
    if (verbose) {
        serial_puts("supplies:\n");
        put_ldo(3, "vdd");
        put_ldo(8, "i2c");
    }

    i2c_recover();
    /* fts_reset_proc: out of reset, then 200 ms for its firmware */
    mdelay(1);
    pin_drive(TS_RESET, 1);
    mdelay(200);
    /* Latch the interrupt line's falling edges for the task; RAW_STATUS_EN
     * may set the status as it turns on (pinctrl-msm.c), so clear after */
    mmio_wr32(PIN_INTR_CFG(TS_IRQ),
              (mmio_rd32(PIN_INTR_CFG(TS_IRQ)) & ~INTR_CFG_MASK) | INTR_FALLING_RAW);
    mmio_wr32(PIN_INTR_STATUS(TS_IRQ), 0);

    uint8_t id = 0, id2 = 0, fw = 0, vendor = 0;
    err = fts_read(FTS_REG_CHIP_ID, &id, 1);
    if (!err)
        err = fts_read(FTS_REG_CHIP_ID2, &id2, 1);
    if (!err)
        err = fts_read(FTS_REG_FW_VER, &fw, 1);
    if (!err)
        err = fts_read(FTS_REG_VENDOR_ID, &vendor, 1);
    if (err) {
        serial_puts("touch: controller at 0x38: ");
        serial_puts(i2c_err(err));
        serial_puts("\n");
        return;
    }
    serial_puts("touch: FocalTech chip ");
    serial_puthex((uint32_t)id << 8 | id2, 4);
    serial_puts(", firmware ");
    serial_puthex(fw, 2);
    serial_puts(", vendor ");
    serial_puthex(vendor, 2);
    serial_puts(verbose ? "\n" : " (supplies over RPMh, I2C bit-banged)\n");
    touch_ready = 1;
}

static void push(int32_t x, int32_t y, int down, int droppable)
{
    uint32_t used = q_head - q_tail;
    if (used >= QUEUE || (droppable && used >= QUEUE / 2))
        return;
    uint32_t i = q_head % QUEUE;
    queue[i].x = (int16_t)x;
    queue[i].y = (int16_t)y;
    queue[i].down = (uint8_t)down;
    __asm__ volatile("dmb ish" : : : "memory");
    q_head = q_head + 1;
}

/* The desktop's side of the queue: 1 and the next event, or 0 */
int platform_touch_event(int32_t *x, int32_t *y, int *down)
{
    if (q_tail == q_head)
        return 0;
    __asm__ volatile("dmb ish" : : : "memory");
    uint32_t i = q_tail % QUEUE;
    *x = queue[i].x;
    *y = queue[i].y;
    *down = queue[i].down;
    __asm__ volatile("dmb ish" : : : "memory");
    q_tail = q_tail + 1;
    return 1;
}

/* Read the first point when the line fell, and while a finger is down (to
 * see it lift). Moves may be dropped when the desktop falls behind; a
 * press or a lift never is. */
static void touch_task(void *arg)
{
    if (arg)
        touch_on(0);
    int down = 0;
    int32_t lx = 0, ly = 0;
    for (;;) {
        int fell = touch_ready && (mmio_rd32(PIN_INTR_STATUS(TS_IRQ)) & 1);
        if (fell)
            mmio_wr32(PIN_INTR_STATUS(TS_IRQ), 0);
        uint8_t b[3 + 6];
        if ((fell || down) && touch_ready && !fts_read(FTS_REG_POINTS, b, sizeof(b))) {
            reads++;
            int n = b[2] & 0x0F, ev = b[3] >> 6;
            int now = n >= 1 && n <= FTS_POINTS && (ev == FTS_EV_DOWN || ev == FTS_EV_CONTACT);
            int32_t x = (int32_t)((b[3] & 0x0F) << 8 | b[4]);
            int32_t y = (int32_t)((b[5] & 0x0F) << 8 | b[6]);
            if (now && !down)
                presses++;
            if (now && (!down || x != lx || y != ly)) {
                push(x, y, 1, down);
                lx = last_x = x;
                ly = last_y = y;
            } else if (!now && down) {
                push(lx, ly, 0, 0);
            }
            down = now;
        }
        task_delay_ms(POLL_MS);
    }
}

/* At boot: the power-up runs in the task, so its 200 ms reset doesn't hold
 * the boot up */
void touch_start(void)
{
    if (!task_started)
        task_started = task_create_on("touch", touch_task, (void *)1, 1, TASK_ANY_CPU) >= 0;
}

static void touch_off(void)
{
    touch_ready = 0;
    pin_drive(TS_RESET, 0);
    pin_drive(TS_VDDIO, 0);
    int err = rpmh_vrm_vote("ldoc3", 0, 0, 0);     /* ldoc8 stays: always-on in Android */
    serial_puts("touch: off (");
    serial_puts(rpmh_err(err));
    serial_puts(")\n");
}

/* One read of the touch points */
static void touch_read(void)
{
    if (!touch_ready) {
        serial_puts("touch: not on (touch on)\n");
        return;
    }
    uint8_t b[FTS_POINTS_LEN];
    uint64_t t0 = read_cntpct_el0();
    int err = fts_read(FTS_REG_POINTS, b, FTS_POINTS_LEN);
    uint64_t us = (read_cntpct_el0() - t0) * 1000000 / read_cntfrq_el0();
    if (err) {
        serial_puts("touch: ");
        serial_puts(i2c_err(err));
        serial_puts("\n");
        return;
    }
    serial_puts("touch: ");
    serial_putdec(b[2] & 0x0F);
    serial_puts(" points (read in ");
    serial_putdec(us);
    serial_puts(" us), irq line ");
    serial_putc(pin_level(TS_IRQ) ? '1' : '0');
    serial_puts(", registers 0-8:");
    for (int i = 0; i < 9; i++) {
        serial_puts(" ");
        serial_puthex(b[i], 2);
    }
    serial_puts("\n");
    for (int i = 0; i < FTS_POINTS; i++) {
        const uint8_t *p = b + 3 + 6 * i;
        if ((p[0] >> 6) == 3)               /* no event: an empty slot */
            continue;
        serial_puts("  id ");
        serial_putdec(p[2] >> 4);
        serial_puts(" event ");
        serial_putdec(p[0] >> 6);
        serial_puts(" x ");
        serial_putdec(((uint32_t)(p[0] & 0x0F) << 8) | p[1]);
        serial_puts(" y ");
        serial_putdec(((uint32_t)(p[2] & 0x0F) << 8) | p[3]);
        serial_puts("\n");
    }
}

/* Shell `touch`: what the touchscreen's wiring looks like now; `touch on`
 * powers it up and probes it, `touch read` reads the points once, `touch
 * off` powers it down */
void platform_touch_cmd(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        touch_on(1);
        if (!task_started)
            task_started = task_create_on("touch", touch_task, (void *)0, 1, TASK_ANY_CPU) >= 0;
        return;
    }
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        touch_off();
        return;
    }
    if (argc == 2 && strcmp(argv[1], "read") == 0) {
        touch_read();
        return;
    }
    if (argc != 1) {
        serial_puts("usage: touch [on | read | off]\n");
        return;
    }
    serial_puts("controller: ");
    serial_puts(touch_ready ? "on" : "off");
    serial_puts(", task ");
    serial_puts(task_started ? "running, " : "not started, ");
    serial_putdec(presses);
    serial_puts(" presses, ");
    serial_putdec(reads);
    serial_puts(" reads");
    if (last_x >= 0) {
        serial_puts(", last at ");
        serial_putdec((uint32_t)last_x);
        serial_puts(",");
        serial_putdec((uint32_t)last_y);
    }
    serial_puts("\n");
    serial_puts("pins:\n");
    put_pin("i2c sda", TS_SDA);
    put_pin("i2c scl", TS_SCL);
    put_pin("reset", TS_RESET);
    put_pin("irq", TS_IRQ);
    put_pin("io power", TS_VDDIO);

    serial_puts("QUP0 clocks:");
    uint32_t vote = mmio_rd32(GCC + GCC_QUP_VOTE);
    int all_on = 1;
    for (int i = 0; i < NQUP; i++) {
        uint32_t cbcr = mmio_rd32(GCC + qup_clocks[i].cbcr);
        all_on &= qup_clock_on(i);
        serial_puts(" ");
        serial_puts(qup_clocks[i].name);
        serial_puts(!(cbcr & CBCR_OFF) ? "=on" : qup_clock_on(i) ? "=hw-gated" : "=off");
        if (vote & (1U << qup_clocks[i].vote_bit))
            serial_puts("(voted)");
    }
    serial_puts("\n");

    serial_puts("supplies:\n");
    put_ldo(3, "vdd 3.0 V");
    put_ldo(8, "i2c 1.8 V");

    if (!all_on) {
        serial_puts("SE4: clocks off, not reading it\n");
        return;
    }
    uint32_t fw = mmio_rd32(SE4 + GENI_FW_REVISION);
    static const char *const protos[] = { "none", "SPI", "UART", "I2C" };
    uint32_t proto = (fw >> 8) & 0xFF;
    serial_puts("SE4: firmware ");
    serial_puthex(fw, 8);
    serial_puts(" (");
    serial_puts(proto < 4 ? protos[proto] : "?");
    serial_puts(mmio_rd32(SE4 + GENI_IF_DISABLE_RO) & 1 ? ", no FIFO: GPI DMA only" : ", FIFO");
    serial_puts("), status ");
    serial_puthex(mmio_rd32(SE4 + GENI_STATUS), 8);
    serial_puts("\n");
}
