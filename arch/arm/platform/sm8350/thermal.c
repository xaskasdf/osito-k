/*
 * thermal.c -- SM8350 temperature sensors (TSENS) and a CPU thermal limit
 *
 * Two TSENS v2 controllers (qcom,tsens24xx in the phone's device tree):
 * tsens0 (SROT 0x0C222000, TM 0x0C263000) has the CPU sensors, tsens1
 * (SROT 0x0C223000, TM 0x0C265000) the GPU, NPU, video, DDR, modem and
 * camera ones. Each sensor's status register (TM + 0xA0 + 4n) holds its
 * last reading in 0.1 C, 12-bit signed, and a valid bit, as Linux's
 * tsens-v2 reads it. Names are the device tree's thermal zones.
 *
 * The limit: every 100 ms the "thermal" task takes the hottest big-core
 * sensor and moves the cpufreq caps of both big clusters one level down
 * above 95 C, one level up below 85 C. Both together: they share a voltage
 * rail, so capping one alone changes nothing (cpufreq.c). The A55s stay
 * at their top level (cpufreq.c too) and barely warm up anyway. Android's die trips for these zones are 108-125
 * C; its everyday control works from skin temperature, which Osito-K
 * doesn't read, so these are lower. The limits hardware (LMh) still
 * throttles on its own.
 */

#include "../../include/hal.h"
#include "../../include/aarch64.h"
#include "../../include/types.h"
#include "cpufreq.h"

#define SROT_CTRL           0x004   /* bit 0 enabled, bit 3 + n sensor n */
#define TM_SN_STATUS(n)     (0x0A0 + 4 * (n))
#define SN_VALID            (1u << 21)
#define TM_TRDY             0x0E4   /* bit 0: first round of readings done */

#define TEMP_NONE           (-10000)    /* deci-C: no valid reading */
#define TRIP_CAP            950         /* deci-C, defaults; `temp limit` */
#define TRIP_RELEASE        850
#define CHECK_MS            100

struct tsens {
    uint64_t srot, tm;
    int nsensors;
    const char *const *names;
};

static const char *const tsens0_names[] = {
    "aoss-0", "cpu-0-0", "cpu-0-1", "cpu-0-2", "cpu-0-3", "cpuss-0", "cpuss-1",
    "cpu-1-0", "cpu-1-1", "cpu-1-2", "cpu-1-3", "cpu-1-4", "cpu-1-5", "cpu-1-6",
    "cpu-1-7",
};
static const char *const tsens1_names[] = {
    "aoss-1", "gpuss-0", "gpuss-1", "nspss-0", "nspss-1", "nspss-2", "video",
    "ddr", "mdmss-0", "mdmss-1", "mdmss-2", "mdmss-3", "camera-0", "camera-1",
};

static const struct tsens tsens[] = {
    { 0x0C222000UL, 0x0C263000UL, 15, tsens0_names },
    { 0x0C223000UL, 0x0C265000UL, 14, tsens1_names },
};
#define NTSENS (int)(sizeof(tsens) / sizeof(tsens[0]))

/* The tsens0 sensors of each cpufreq domain. cpu-1-0..7 cover the four big
 * cores, two each in zone order: cpu-1-6/7 is the X1 (cpu7). */
static const struct {
    const char *core;
    int first, last;
} clusters[CPUFREQ_DOMAINS] = {
    { "A55", 1, 4 },
    { "A78", 7, 12 },
    { "X1",  13, 14 },
};

/* The domains the limit moves: A78 and X1, one rail */
#define BIG_FIRST   1
#define BIG_LAST    2

static int peak[CPUFREQ_DOMAINS];   /* hottest reading, deci-C */
static int caps;                    /* times the big cores went over trip_cap */

/* deci-C; `temp limit` changes them, e.g. low to watch the limit work */
static volatile int trip_cap = TRIP_CAP, trip_release = TRIP_RELEASE;

static int sensor_on(const struct tsens *t, int n)
{
    uint32_t ctrl = mmio_rd32(t->srot + SROT_CTRL);
    return (ctrl & 1) && (ctrl & (1u << (3 + n)));
}

/* deci-C, or TEMP_NONE. A reading can be caught mid-update; try again. */
static int read_temp(const struct tsens *t, int n)
{
    for (int tries = 0; tries < 3; tries++) {
        uint32_t v = mmio_rd32(t->tm + TM_SN_STATUS(n));
        if (v & SN_VALID) {
            int d = (int)(v & 0xFFF);
            return d & 0x800 ? d - 0x1000 : d;
        }
    }
    return TEMP_NONE;
}

static int hottest(int dom)
{
    int hot = TEMP_NONE;
    for (int n = clusters[dom].first; n <= clusters[dom].last; n++) {
        int t = read_temp(&tsens[0], n);
        if (t > hot)
            hot = t;
    }
    return hot;
}

static void print_temp(int d)
{
    if (d == TEMP_NONE) {
        serial_puts("  -  ");
        return;
    }
    if (d < 0) {
        serial_puts("-");
        d = -d;
    }
    serial_putdec(d / 10);
    serial_puts(".");
    serial_putdec(d % 10);
}

static void print_mhz(uint32_t khz)
{
    serial_putdec(khz / 1000);
    serial_puts(" MHz");
}

static int big_capped(void)
{
    for (int dom = BIG_FIRST; dom <= BIG_LAST; dom++)
        if (cpufreq_capped(dom))
            return 1;
    return 0;
}

static void big_step(int dir)
{
    for (int dom = BIG_FIRST; dom <= BIG_LAST; dom++)
        cpufreq_cap_step(dom, dir);
}

/* "[TEMP] big cores at 95.2 C: capping, A78 2342 MHz, X1 2764 MHz" */
static void big_log(int hot, const char *what)
{
    serial_puts("[TEMP] big cores at ");
    print_temp(hot);
    serial_puts(" C: ");
    serial_puts(what);
    for (int dom = BIG_FIRST; dom <= BIG_LAST; dom++) {
        serial_puts(dom == BIG_FIRST ? " " : ", ");
        serial_puts(clusters[dom].core);
        serial_puts(" ");
        print_mhz(cpufreq_cap_khz(dom));
    }
    serial_puts("\n");
}

static void thermal_task(void *arg)
{
    (void)arg;
    for (;;) {
        int hot = TEMP_NONE;
        for (int dom = 0; dom < CPUFREQ_DOMAINS; dom++) {
            int t = hottest(dom);
            if (t > peak[dom])
                peak[dom] = t;
            if (dom >= BIG_FIRST && t > hot)
                hot = t;
        }
        int was = big_capped();
        if (hot != TEMP_NONE && hot >= trip_cap) {
            big_step(-1);
            if (!was) {
                caps++;
                big_log(hot, "capping,");
            }
        } else if (hot != TEMP_NONE && hot <= trip_release && was) {
            big_step(+1);
            if (!big_capped())
                big_log(hot, "back to");
        }
        task_delay_ms(CHECK_MS);
    }
}

void platform_start_tasks(void)
{
    if (!(mmio_rd32(tsens[0].srot + SROT_CTRL) & 1)) {
        serial_puts("[TEMP] tsens0 is off: no thermal limit\n");
        return;
    }
    for (int i = 0; i < 100 && !(mmio_rd32(tsens[0].tm + TM_TRDY) & 1); i++)
        mdelay(1);
    for (int dom = 0; dom < CPUFREQ_DOMAINS; dom++)
        peak[dom] = TEMP_NONE;
    /* Unpinned and mostly asleep: it settles on an A55 */
    task_create_on("thermal", thermal_task, (void *)0, 1, TASK_ANY_CPU);
}

static int parse_c(const char *s)
{
    int v = 0;
    if (*s < '0' || *s > '9')
        return -1;
    for (; *s >= '0' && *s <= '9'; s++)
        v = v * 10 + (*s - '0');
    return *s ? -1 : v;
}

/* Shell `temp`: every sensor, then per cluster the hottest one and the
 * clock it runs at. `temp limit <cap C> <release C>` moves the limit. */
void platform_temp_cmd(int argc, char **argv)
{
    if (argc > 1) {
        int cap = argc == 4 && strcmp(argv[1], "limit") == 0 ? parse_c(argv[2]) : -1;
        int rel = argc == 4 ? parse_c(argv[3]) : -1;
        if (cap <= 0 || rel < 0 || rel >= cap || cap > 115) {
            serial_puts("usage: temp [limit <cap C> <release C>], release < cap <= 115\n");
            return;
        }
        trip_release = rel * 10;
        trip_cap = cap * 10;
    }
    for (int c = 0; c < NTSENS; c++) {
        const struct tsens *t = &tsens[c];
        serial_puts("tsens");
        serial_putdec(c);
        serial_puts(":");
        for (int n = 0; n < t->nsensors; n++) {
            serial_puts(n % 4 ? "  " : "\n  ");
            serial_puts(t->names[n]);
            serial_puts(" ");
            print_temp(sensor_on(t, n) ? read_temp(t, n) : TEMP_NONE);
        }
        serial_puts("\n");
    }
    for (int dom = 0; dom < CPUFREQ_DOMAINS; dom++) {
        serial_puts(clusters[dom].core);
        serial_puts(dom == 2 ? " " : "");
        serial_puts("  hottest ");
        print_temp(hottest(dom));
        serial_puts(" C, peak ");
        print_temp(peak[dom]);
        serial_puts(" C, runs at ");
        print_mhz(cpufreq_now_khz(dom));
        if (cpufreq_capped(dom)) {
            serial_puts(" (capped to ");
            print_mhz(cpufreq_cap_khz(dom));
            serial_puts(")");
        }
        serial_puts("\n");
    }
    serial_puts("limit: cap the big cores over ");
    serial_putdec(trip_cap / 10);
    serial_puts(" C, release under ");
    serial_putdec(trip_release / 10);
    serial_puts(" C, checked every ");
    serial_putdec(CHECK_MS);
    serial_puts(" ms; capped ");
    serial_putdec(caps);
    serial_puts(" times\n");
}
