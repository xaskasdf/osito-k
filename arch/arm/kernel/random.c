/*
 * random.c -- random bytes for keys and nonces (TLS)
 *
 * HMAC-DRBG over SHA-256 (NIST SP 800-90A), reseeded on every request with
 * platform_entropy (SM8350's hardware PRNG; none on virt) and the timer
 * counter read around work that takes a variable time. Before this, TLS
 * drew its ECDHE keys from a xorshift seeded with the counter alone, which
 * a peer could guess from roughly when the phone booted.
 */

#include "../include/hal.h"
#include "../include/spinlock.h"
#include "crypto.h"

static uint8_t K[32], V[32];
static int have_hw = -1;                /* -1 unknown */
static spinlock_t lock;

/* SP 800-90A's HMAC_DRBG_Update */
static void update(const uint8_t *data, uint32_t len)
{
    static uint8_t buf[32 + 1 + 96];
    for (int round = 0; round < 2; round++) {
        memcpy(buf, V, 32);
        buf[32] = (uint8_t)round;
        if (len)
            memcpy(buf + 33, data, len);
        hmac_sha256(K, 32, buf, 33 + len, K);
        hmac_sha256(K, 32, V, 32, V);
        if (!len)
            break;
    }
}

/* 96 bytes: 32 from the hardware, if any, and the timer counter's low
 * bits sampled around hashing, whose time varies with caches and IRQs */
static void gather(uint8_t out[96])
{
    memset(out, 0, 96);
    have_hw = platform_entropy(out, 32) == 0;
    sha256_ctx ctx;
    uint8_t scratch[32];
    for (int half = 0; half < 2; half++) {
        sha256_init(&ctx);
        for (int i = 0; i < 64; i++) {
            uint64_t t = read_cntpct_el0();
            sha256_update(&ctx, &t, sizeof(t));
            sha256(scratch, sizeof(scratch), scratch);
        }
        sha256_final(&ctx, out + 32 + 32 * half);
    }
}

void random_bytes(void *buf, uint32_t len)
{
    uint8_t seed[96];
    uint64_t flags = spin_lock_irqsave(&lock);
    gather(seed);
    update(seed, sizeof(seed));
    uint8_t *p = buf;
    while (len) {
        hmac_sha256(K, 32, V, 32, V);
        uint32_t n = len < 32 ? len : 32;
        memcpy(p, V, n);
        p += n;
        len -= n;
    }
    update((uint8_t *)0, 0);
    memset(seed, 0, sizeof(seed));
    spin_unlock_irqrestore(&lock, flags);
}

int random_hw(void)
{
    if (have_hw < 0) {
        uint8_t b;
        random_bytes(&b, 1);
    }
    return have_hw;
}
