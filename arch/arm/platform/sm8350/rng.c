/*
 * rng.c -- the SM8350's hardware random number generator (prng@10d3000)
 *
 * Linux: drivers/crypto/qcom-rng.c, compatible "qcom,prng-ee" (the device
 * tree's rng node; Android's lahaina.dtsi has it as "qcom,msm-rng" with
 * qcom,no-qrng-config). The secure world configures and enables it; the
 * HLOS only reads it: PRNG_STATUS bit 0 says a word is ready in
 * PRNG_DATA_OUT, and a word of 0 means it failed.
 *
 * Its clock is RPMH_HWKM_CLK, the "HK0" BCM in the command DB: voted on
 * once (rpmh.c) before the first read, as clk-rpmh.c does.
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "../../include/aarch64.h"
#include "rpmh.h"

#define PRNG_BASE           0x010D3000UL
#define PRNG_DATA_OUT       0x0
#define PRNG_STATUS         0x4
#define PRNG_DATA_AVAIL     (1U << 0)

/* clk-rpmh.c's BCM_TCS_CMD(commit, valid, vote_x, vote_y): on, level 1 */
#define BCM_VOTE_ON         ((1U << 30) | (1U << 29) | 1U)

static int state;                       /* 0 unknown, 1 working, -1 not */

static int prng_up(void)
{
    if (state)
        return state > 0;
    uint32_t addr;
    if (cmd_db_addr("HK0", &addr) || rpmh_write(addr, BCM_VOTE_ON)) {
        serial_puts("[RNG ] no vote for the HWKM clock: no hardware entropy\n");
        state = -1;
        return 0;
    }
    state = 1;
    return 1;
}

int platform_entropy(void *buf, uint32_t len)
{
    if (!prng_up())
        return -1;
    uint8_t *p = buf;
    while (len) {
        uint32_t w = 0;
        int i;
        for (i = 0; i < 10000; i++) {       /* ~10 ms, as Linux waits */
            if (mmio_rd32(PRNG_BASE + PRNG_STATUS) & PRNG_DATA_AVAIL) {
                w = mmio_rd32(PRNG_BASE + PRNG_DATA_OUT);
                break;
            }
            udelay(1);
        }
        if (!w) {
            serial_puts("[RNG ] the PRNG gave no data: no hardware entropy\n");
            state = -1;
            return -1;
        }
        uint32_t n = len < 4 ? len : 4;
        for (uint32_t k = 0; k < n; k++)
            p[k] = (uint8_t)(w >> (8 * k));
        p += n;
        len -= n;
    }
    return 0;
}
