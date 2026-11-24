/*
 * random_drbg.c -- random bytes for keys and nonces (the TLS clients)
 *
 * HMAC-DRBG over SHA-256 (NIST SP 800-90A), reseeded on every request with
 * RDRAND (random_secure_bytes) and the TSC read around work whose time
 * varies. random_get_bytes falls back to an xorshift when the CPU has no
 * RDRAND (QEMU's default CPU), which keys mustn't come from; this doesn't.
 * The same as arch/arm/kernel/random.c, over the x86 sources.
 */

#include "../include/types.h"
#include "crypto.h"
#include "random_secure.h"
#include "smp.h"

extern void serial_puts(const char *s);

static uint8_t K[32], V[32];
static spinlock_t lock;
static int warned;

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

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

/* 96 bytes: 32 from RDRAND, if any, and the TSC sampled around hashing */
static void gather(uint8_t out[96])
{
    memset(out, 0, 96);
    if (random_secure_bytes(out, 32) < 0 && !warned) {
        warned = 1;
        serial_puts("[RNG] no RDRAND: keys from TSC jitter only (QEMU: -cpu max)\n");
    }
    sha256_ctx ctx;
    uint8_t scratch[32];
    memset(scratch, 0, sizeof(scratch));
    for (int half = 0; half < 2; half++) {
        sha256_init(&ctx);
        for (int i = 0; i < 64; i++) {
            uint64_t t = rdtsc();
            sha256_update(&ctx, &t, sizeof(t));
            sha256(scratch, sizeof(scratch), scratch);
        }
        sha256_final(&ctx, out + 32 + 32 * half);
    }
}

void random_bytes(void *buf, uint32_t len)
{
    uint8_t seed[96];
    spin_lock(&lock);
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
    spin_unlock(&lock);
}
