/*
 * ecdsa_p256.c — minimal ECDSA-P256 verify.
 *
 * Implementation choices, all on the side of "correct + simple" over
 * "fast + clever":
 *   - 256-bit unsigned bigints as `uint64_t b[4]` little-endian.
 *   - 64×64 → 128 multiply via the x86 mulq instruction (no
 *     __int128, no libgcc helpers needed).
 *   - 512-bit-mod-256 reduction by classic shift-and-subtract.
 *     ~257 iterations of 8-word compare + conditional subtract per
 *     reduction. Way slower than Barrett/Montgomery but trivially
 *     correct, and verify is once-per-handshake.
 *   - Modular inversion via Fermat's little theorem
 *     (a^-1 ≡ a^(p-2) mod p) — needs ~256 squarings + ~128 muls.
 *   - Affine point arithmetic with an explicit "is infinity" flag.
 *     Double-and-add for scalar multiply.
 *
 * Self-test: a single known NIST CAVS test vector is verified at
 * boot. If it fails, ecdsa_p256_self_test returns -1 and the caller
 * (tls_init) refuses to upgrade SKE-verify state, falling back to
 * cert-pin-only protection.
 *
 * Not constant-time. Verify keys and signatures are public, so
 * timing leaks don't matter for SKE verification. DO NOT reuse
 * any of this for signing or any private-key operation.
 */

#include "../include/types.h"
#include "ecdsa_p256.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

/* ── 256-bit bigint primitives ─────────────────────────────── */

static inline void bn_zero(uint64_t r[4]) {
    r[0] = r[1] = r[2] = r[3] = 0;
}
static inline void bn_copy(uint64_t r[4], const uint64_t a[4]) {
    r[0] = a[0]; r[1] = a[1]; r[2] = a[2]; r[3] = a[3];
}
static inline int bn_is_zero(const uint64_t a[4]) {
    return (a[0] | a[1] | a[2] | a[3]) == 0;
}
/* Returns -1, 0, +1 for a < b, a == b, a > b */
static int bn_cmp(const uint64_t a[4], const uint64_t b[4]) {
    for (int i = 3; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}
/* c = a + b, returns carry. */
static uint64_t bn_add(uint64_t c[4], const uint64_t a[4], const uint64_t b[4]) {
    uint64_t carry = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t s = a[i] + b[i];
        uint64_t c1 = (s < a[i]);
        uint64_t s2 = s + carry;
        uint64_t c2 = (s2 < s);
        c[i] = s2;
        carry = c1 + c2;
    }
    return carry;
}
/* c = a - b, returns borrow. */
static uint64_t bn_sub(uint64_t c[4], const uint64_t a[4], const uint64_t b[4]) {
    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t d = a[i] - b[i];
        uint64_t b1 = (a[i] < b[i]);
        uint64_t d2 = d - borrow;
        uint64_t b2 = (d < borrow);
        c[i] = d2;
        borrow = b1 + b2;
    }
    return borrow;
}

/* 64x64 = 128 — uses x86 mulq. */
static inline void mul64(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi) {
    uint64_t l, h;
    __asm__("mulq %3" : "=a"(l), "=d"(h) : "a"(a), "rm"(b));
    *lo = l; *hi = h;
}

/* 256x256 = 512: c[0..7] little-endian, c[0] is least significant. */
static void bn_mul512(uint64_t c[8], const uint64_t a[4], const uint64_t b[4]) {
    for (int i = 0; i < 8; i++) c[i] = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            uint64_t lo, hi;
            mul64(a[i], b[j], &lo, &hi);
            uint64_t s1 = c[i + j] + lo;
            uint64_t k1 = (s1 < lo);
            uint64_t s2 = s1 + carry;
            uint64_t k2 = (s2 < carry);
            c[i + j] = s2;
            carry = hi + k1 + k2;
        }
        c[i + 4] = carry;
    }
}

/* ── P-256 / N Barrett reduction ────────────────────────────
 *
 * mu = floor(2^512 / m). Both moduli are exactly 256 bits, so mu
 * is exactly 257 bits and fits in uint64_t[5] with the top word
 * holding a single bit. Computed offline (Python: (1<<512)//m)
 * and pasted here as constants. */
static const uint64_t MU_P[5] = {
    0x0000000000000003ULL, 0xFFFFFFFEFFFFFFFFULL,
    0xFFFFFFFEFFFFFFFEULL, 0x00000000FFFFFFFFULL,
    0x0000000000000001ULL
};
static const uint64_t MU_N[5] = {
    0x012FFD85EEDF9BFEULL, 0x43190552DF1A6C21ULL,
    0xFFFFFFFEFFFFFFFFULL, 0x00000000FFFFFFFFULL,
    0x0000000000000001ULL
};

/* Generic 64-bit limb add with carry-in/carry-out, used by the
 * partial multiplications below. */
static inline uint64_t limb_madd(uint64_t *acc, uint64_t hi_lo_lo, uint64_t carry) {
    uint64_t s = *acc + hi_lo_lo;
    uint64_t c1 = (s < hi_lo_lo);
    uint64_t s2 = s + carry;
    uint64_t c2 = (s2 < carry);
    *acc = s2;
    return c1 + c2;
}

/* Barrett reduce 512-bit v (8 words) mod 256-bit m (4 words) using
 * precomputed mu (5 words). Result in r (4 words). Used in place of
 * the original shift-subtract bn_mod_512 — gives roughly an order of
 * magnitude speedup on the per-mul reduction.
 *
 * Algorithm (Knuth TAOCP 4.3.1, Handbook of Applied Cryptography
 * §14.42, with k = 256):
 *   q1 = floor(v / 2^(k-1))                     ≡ top 257 bits of v
 *   q2 = q1 * mu                                ≤ 2^514
 *   q3 = floor(q2 / 2^(k+1))                    ≤ 2^257
 *   r1 = v mod 2^(k+1)                          (low 257 bits)
 *   r2 = (q3 * m) mod 2^(k+1)                   (low 257 bits)
 *   r  = (r1 - r2) mod 2^(k+1)
 *   while r >= m: r -= m   (at most 2 corrections)
 *
 * All intermediate values fit in 5 uint64_t words. The arithmetic is
 * not constant-time; verify keys are public, so this is fine. */
static void bn_mod_512_barrett(uint64_t r[4], const uint64_t v[8],
                                const uint64_t m[4], const uint64_t mu[5]) {
    /* q1 = v >> 255 — bits 255..511, which is up to 257 bits. */
    uint64_t q1[5];
    for (int i = 0; i < 5; i++) {
        uint64_t lo = v[i + 3] >> 63;
        uint64_t hi = (i + 4 < 8) ? (v[i + 4] << 1) : 0;
        q1[i] = lo | hi;
    }

    /* q2 = q1 * mu, full 5×5 schoolbook → 9 words. */
    uint64_t q2[10] = {0};
    for (int i = 0; i < 5; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 5; j++) {
            uint64_t lo, hi;
            mul64(q1[i], mu[j], &lo, &hi);
            uint64_t k = limb_madd(&q2[i + j], lo, carry);
            carry = hi + k;
        }
        q2[i + 5] = carry;
    }

    /* q3 = q2 >> 257 — bits 257..513, fits in 5 words. */
    uint64_t q3[5];
    for (int i = 0; i < 5; i++) {
        uint64_t lo = (i + 4 < 10) ? (q2[i + 4] >> 1) : 0;
        uint64_t hi = (i + 5 < 10) ? (q2[i + 5] << 63) : 0;
        q3[i] = lo | hi;
    }

    /* r1 = v mod 2^257 — low 4 words plus the LSB of v[4]. */
    uint64_t r1[5];
    for (int i = 0; i < 4; i++) r1[i] = v[i];
    r1[4] = v[4] & 1ULL;

    /* r2 = (q3 * m) mod 2^257 — low 5 words of q3 * m, masked to 257 bits. */
    uint64_t prod[10] = {0};
    for (int i = 0; i < 5; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            uint64_t lo, hi;
            mul64(q3[i], m[j], &lo, &hi);
            uint64_t k = limb_madd(&prod[i + j], lo, carry);
            carry = hi + k;
        }
        prod[i + 4] += carry;
    }
    uint64_t r2[5];
    for (int i = 0; i < 5; i++) r2[i] = prod[i];
    r2[4] &= 1ULL;

    /* r = r1 - r2 mod 2^257.  r1 ≥ r2 always (Barrett invariant),
     * but we still mask to be safe against any approximation slack. */
    uint64_t r5[5];
    {
        uint64_t borrow = 0;
        for (int i = 0; i < 5; i++) {
            uint64_t d  = r1[i] - r2[i];
            uint64_t b1 = (r1[i] < r2[i]);
            uint64_t d2 = d - borrow;
            uint64_t b2 = (d < borrow);
            r5[i] = d2;
            borrow = b1 + b2;
        }
        r5[4] &= 1ULL;
    }

    /* While r5 >= m, subtract m. The Barrett bound guarantees ≤ 2
     * corrections. */
    for (int corr = 0; corr < 3; corr++) {
        /* r5 >= m  iff  r5[4] != 0 OR (r5[4]==0 AND low 4 words >= m). */
        int ge;
        if (r5[4] != 0) {
            ge = 1;
        } else {
            ge = 0;
            for (int i = 3; i >= 0; i--) {
                if (r5[i] > m[i]) { ge = 1; break; }
                if (r5[i] < m[i]) { ge = 0; break; }
                if (i == 0) ge = 1;     /* equal across all 4 words */
            }
        }
        if (!ge) break;

        uint64_t borrow = 0;
        for (int i = 0; i < 4; i++) {
            uint64_t d  = r5[i] - m[i];
            uint64_t b1 = (r5[i] < m[i]);
            uint64_t d2 = d - borrow;
            uint64_t b2 = (d < borrow);
            r5[i] = d2;
            borrow = b1 + b2;
        }
        r5[4] -= borrow;
    }

    for (int i = 0; i < 4; i++) r[i] = r5[i];
}

/* Pick the right Barrett mu for one of our two known moduli.
 * Falls back to a shift-subtract reduce for any other modulus —
 * but in practice every call site is either P256_P or P256_N. */
static const uint64_t *barrett_mu_for(const uint64_t m[4]) {
    if (m[0] == 0xFFFFFFFFFFFFFFFFULL && m[3] == 0xFFFFFFFF00000001ULL)
        return MU_P;
    if (m[0] == 0xF3B9CAC2FC632551ULL && m[3] == 0xFFFFFFFF00000000ULL)
        return MU_N;
    return 0;
}

/* Slow fallback for any modulus we don't have a Barrett mu for —
 * keeps the codepath available if a future curve needs it. */
static void bn_mod_512_slow(uint64_t r[4], const uint64_t v[8], const uint64_t m[4]) {
    uint64_t a[8];
    for (int i = 0; i < 8; i++) a[i] = v[i];
    uint64_t ms[8];
    for (int i = 0; i < 4; i++) { ms[i] = 0; ms[i + 4] = m[i]; }
    for (int k = 0; k <= 256; k++) {
        int gt = 0, lt = 0;
        for (int i = 7; i >= 0; i--) {
            if (a[i] > ms[i]) { gt = 1; break; }
            if (a[i] < ms[i]) { lt = 1; break; }
        }
        if (gt || (!lt)) {
            uint64_t borrow = 0;
            for (int i = 0; i < 8; i++) {
                uint64_t d  = a[i] - ms[i];
                uint64_t b1 = (a[i] < ms[i]);
                uint64_t d2 = d - borrow;
                uint64_t b2 = (d < borrow);
                a[i] = d2;
                borrow = b1 + b2;
            }
        }
        for (int i = 0; i < 7; i++)
            ms[i] = (ms[i] >> 1) | (ms[i + 1] << 63);
        ms[7] >>= 1;
    }
    for (int i = 0; i < 4; i++) r[i] = a[i];
}

static void bn_mod_512(uint64_t r[4], const uint64_t v[8], const uint64_t m[4]) {
    const uint64_t *mu = barrett_mu_for(m);
    if (mu) bn_mod_512_barrett(r, v, m, mu);
    else    bn_mod_512_slow   (r, v, m);
}

static void bn_mod_add(uint64_t r[4], const uint64_t a[4], const uint64_t b[4],
                       const uint64_t m[4]) {
    uint64_t carry = bn_add(r, a, b);
    if (carry || bn_cmp(r, m) >= 0) {
        bn_sub(r, r, m);
    }
}
static void bn_mod_sub(uint64_t r[4], const uint64_t a[4], const uint64_t b[4],
                       const uint64_t m[4]) {
    uint64_t borrow = bn_sub(r, a, b);
    if (borrow) {
        bn_add(r, r, m);
    }
}
static void bn_mod_mul(uint64_t r[4], const uint64_t a[4], const uint64_t b[4],
                       const uint64_t m[4]) {
    uint64_t prod[8];
    bn_mul512(prod, a, b);
    bn_mod_512(r, prod, m);
}

/* r = a^e mod m via square-and-multiply, walking e LSB→MSB. */
static void bn_mod_pow(uint64_t r[4], const uint64_t a[4],
                       const uint64_t e[4], const uint64_t m[4]) {
    uint64_t result[4] = {1, 0, 0, 0};
    uint64_t base[4]; bn_copy(base, a);

    for (int i = 0; i < 256; i++) {
        uint64_t bit = (e[i / 64] >> (i % 64)) & 1;
        if (bit) {
            uint64_t tmp[4];
            bn_mod_mul(tmp, result, base, m);
            bn_copy(result, tmp);
        }
        uint64_t sq[4];
        bn_mod_mul(sq, base, base, m);
        bn_copy(base, sq);
    }
    bn_copy(r, result);
}

/* a^-1 mod prime m via Fermat's little theorem. */
static void bn_mod_inv(uint64_t r[4], const uint64_t a[4], const uint64_t m[4]) {
    uint64_t two[4] = {2, 0, 0, 0};
    uint64_t exp[4];
    bn_sub(exp, m, two);
    bn_mod_pow(r, a, exp, m);
}

/* ── Big-endian 32-byte ↔ bigint helpers ───────────────────── */

static void be32_to_bn(const uint8_t in[32], uint64_t out[4]) {
    /* Big-endian input: in[0] is MSB of the 256-bit number.
     * Our little-endian bigint: out[3] holds the high word. */
    for (int i = 0; i < 4; i++) {
        uint64_t w = 0;
        for (int j = 0; j < 8; j++) w = (w << 8) | in[i * 8 + j];
        out[3 - i] = w;
    }
}

/* ── P-256 curve constants ─────────────────────────────────── */

/* p = 2^256 - 2^224 + 2^192 + 2^96 - 1 */
static const uint64_t P256_P[4] = {
    0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL,
    0x0000000000000000ULL, 0xFFFFFFFF00000001ULL
};
/* n (curve order) */
static const uint64_t P256_N[4] = {
    0xF3B9CAC2FC632551ULL, 0xBCE6FAADA7179E84ULL,
    0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF00000000ULL
};
/* G = (Gx, Gy) */
static const uint64_t P256_GX[4] = {
    0xF4A13945D898C296ULL, 0x77037D812DEB33A0ULL,
    0xF8BCE6E563A440F2ULL, 0x6B17D1F2E12C4247ULL
};
static const uint64_t P256_GY[4] = {
    0xCBB6406837BF51F5ULL, 0x2BCE33576B315ECEULL,
    0x8EE7EB4A7C0F9E16ULL, 0x4FE342E2FE1A7F9BULL
};

/* ── Affine point arithmetic ───────────────────────────────── */

typedef struct {
    uint64_t x[4];
    uint64_t y[4];
    int      infinity;
} ec_point_t;

static void pt_double(ec_point_t *R, const ec_point_t *P) {
    if (P->infinity) { *R = *P; return; }
    if (bn_is_zero(P->y)) { R->infinity = 1; bn_zero(R->x); bn_zero(R->y); return; }

    /* a = -3 ≡ p - 3 */
    uint64_t three[4] = {3, 0, 0, 0};
    uint64_t a[4]; bn_sub(a, P256_P, three);

    /* lambda = (3*x^2 + a) / (2*y) */
    uint64_t x2[4], num[4], two_y[4], two_y_inv[4], lambda[4];
    bn_mod_mul(x2, P->x, P->x, P256_P);
    bn_mod_add(num, x2, x2, P256_P);
    bn_mod_add(num, num, x2, P256_P);
    bn_mod_add(num, num, a,  P256_P);

    bn_mod_add(two_y, P->y, P->y, P256_P);
    bn_mod_inv(two_y_inv, two_y, P256_P);
    bn_mod_mul(lambda, num, two_y_inv, P256_P);

    /* x_R = lambda^2 - 2x */
    uint64_t lam_sq[4], two_x[4], rx[4], rx_diff[4], ry_term[4];
    bn_mod_mul(lam_sq, lambda, lambda, P256_P);
    bn_mod_add(two_x, P->x, P->x, P256_P);
    bn_mod_sub(rx, lam_sq, two_x, P256_P);

    /* y_R = lambda * (x - x_R) - y */
    bn_mod_sub(rx_diff, P->x, rx, P256_P);
    bn_mod_mul(ry_term, lambda, rx_diff, P256_P);

    R->infinity = 0;
    bn_copy(R->x, rx);
    bn_mod_sub(R->y, ry_term, P->y, P256_P);
}

static void pt_add(ec_point_t *R, const ec_point_t *P, const ec_point_t *Q) {
    if (P->infinity) { *R = *Q; return; }
    if (Q->infinity) { *R = *P; return; }
    if (bn_cmp(P->x, Q->x) == 0) {
        if (bn_cmp(P->y, Q->y) == 0) { pt_double(R, P); return; }
        R->infinity = 1; bn_zero(R->x); bn_zero(R->y); return;
    }
    /* lambda = (Qy - Py) / (Qx - Px) */
    uint64_t dy[4], dx[4], dx_inv[4], lambda[4];
    bn_mod_sub(dy, Q->y, P->y, P256_P);
    bn_mod_sub(dx, Q->x, P->x, P256_P);
    bn_mod_inv(dx_inv, dx, P256_P);
    bn_mod_mul(lambda, dy, dx_inv, P256_P);

    /* x_R = lambda^2 - Px - Qx */
    uint64_t lam_sq[4], rx[4], rx_diff[4], ry_term[4];
    bn_mod_mul(lam_sq, lambda, lambda, P256_P);
    bn_mod_sub(rx, lam_sq, P->x, P256_P);
    bn_mod_sub(rx, rx, Q->x, P256_P);

    /* y_R = lambda * (Px - x_R) - Py */
    bn_mod_sub(rx_diff, P->x, rx, P256_P);
    bn_mod_mul(ry_term, lambda, rx_diff, P256_P);

    R->infinity = 0;
    bn_copy(R->x, rx);
    bn_mod_sub(R->y, ry_term, P->y, P256_P);
}

/* R = k * P. */
static void pt_mul(ec_point_t *R, const uint64_t k[4], const ec_point_t *P) {
    R->infinity = 1; bn_zero(R->x); bn_zero(R->y);
    ec_point_t Q = *P;

    for (int i = 0; i < 256; i++) {
        uint64_t bit = (k[i / 64] >> (i % 64)) & 1;
        if (bit) {
            ec_point_t tmp;
            pt_add(&tmp, R, &Q);
            *R = tmp;
        }
        ec_point_t tmp;
        pt_double(&tmp, &Q);
        Q = tmp;
    }
}

/* ── ASN.1 sig parser ──────────────────────────────────────── */

/* Read a DER length field at *off; advance *off past it. */
static int der_read_len(const uint8_t *der, uint32_t len, uint32_t *off, uint32_t *out) {
    if (*off >= len) return -1;
    uint8_t b = der[(*off)++];
    if ((b & 0x80) == 0) { *out = b; return 0; }
    uint8_t nb = b & 0x7F;
    if (nb == 0 || nb > 4 || *off + nb > len) return -1;
    uint32_t v = 0;
    for (uint32_t i = 0; i < nb; i++) v = (v << 8) | der[(*off)++];
    *out = v;
    return 0;
}

/* Parses SEQUENCE { INTEGER r, INTEGER s }.  INTEGERs may have a
 * leading 0x00 padding to keep them positive in DER; we strip it.
 * Outputs r/s as 32-byte right-aligned big-endian. */
static int parse_ecdsa_sig(const uint8_t *der, uint32_t len,
                            uint8_t r_be[32], uint8_t s_be[32]) {
    if (len < 8 || der[0] != 0x30) return -1;
    uint32_t off = 1, seq_len;
    if (der_read_len(der, len, &off, &seq_len) < 0) return -1;
    if (off + seq_len > len) return -1;

    /* INTEGER r */
    if (off >= len || der[off++] != 0x02) return -1;
    uint32_t r_len;
    if (der_read_len(der, len, &off, &r_len) < 0) return -1;
    if (off + r_len > len) return -1;
    const uint8_t *r_p = der + off;
    off += r_len;
    if (r_len > 0 && r_p[0] == 0x00) { r_p++; r_len--; }
    if (r_len > 32) return -1;
    for (uint32_t i = 0; i < 32 - r_len; i++) r_be[i] = 0;
    for (uint32_t i = 0; i < r_len; i++) r_be[32 - r_len + i] = r_p[i];

    /* INTEGER s */
    if (off >= len || der[off++] != 0x02) return -1;
    uint32_t s_len;
    if (der_read_len(der, len, &off, &s_len) < 0) return -1;
    if (off + s_len > len) return -1;
    const uint8_t *s_p = der + off;
    if (s_len > 0 && s_p[0] == 0x00) { s_p++; s_len--; }
    if (s_len > 32) return -1;
    for (uint32_t i = 0; i < 32 - s_len; i++) s_be[i] = 0;
    for (uint32_t i = 0; i < s_len; i++) s_be[32 - s_len + i] = s_p[i];
    return 0;
}

/* ── DER encoder for {r, s} → SEQUENCE { INTEGER, INTEGER } ── */

static void bn_to_be32(const uint64_t a[4], uint8_t out[32])
{
    for (int i = 0; i < 4; i++) {
        uint64_t w = a[3 - i];
        for (int j = 0; j < 8; j++)
            out[i * 8 + j] = (uint8_t)(w >> (56 - 8 * j));
    }
}

/* Encode a 32-byte big-endian unsigned int as DER INTEGER.
 * Strips leading zeros, prepends 0x00 if MSB is set.  Returns
 * bytes written starting at out (TLV: 02 LEN CONTENT). */
static uint32_t der_emit_int(uint8_t *out, const uint8_t be[32])
{
    uint32_t skip = 0;
    while (skip < 31 && be[skip] == 0) skip++;
    int needs_pad = (be[skip] & 0x80) ? 1 : 0;
    uint32_t content_len = (32 - skip) + (uint32_t)needs_pad;
    out[0] = 0x02;
    out[1] = (uint8_t)content_len;
    uint32_t off = 2;
    if (needs_pad) out[off++] = 0x00;
    for (uint32_t i = skip; i < 32; i++) out[off++] = be[i];
    return off;
}

extern void random_get_bytes(void *buf, uint32_t len);

/* ECDSA P-256 sign.  d_be is the 32-byte big-endian private key;
 * hash is the 32-byte message digest (caller hashes with SHA-256).
 * Writes a DER-encoded signature to sig_out (cap should be ≥ 72 B
 * for safety; typical output is 70-72).  Returns bytes written, or
 * -1 on error.  Uses random_get_bytes for k. */
int ecdsa_p256_sign(const uint8_t d_be[32], const uint8_t hash[32],
                     uint8_t *sig_out, uint32_t sig_cap)
{
    if (sig_cap < 72) return -1;
    uint64_t d[4], z[4];
    be32_to_bn(d_be, d);
    be32_to_bn(hash, z);
    if (bn_is_zero(d) || bn_cmp(d, P256_N) >= 0) return -1;

    uint64_t r[4], s[4], k[4], kinv[4];
    ec_point_t G, kG;
    bn_copy(G.x, P256_GX); bn_copy(G.y, P256_GY); G.infinity = 0;

    for (int retry = 0; retry < 16; retry++) {
        /* Pick k ∈ [1, n-1].  Reject if outside range — extremely
         * rare with 256-bit n. */
        uint8_t kb[32];
        random_get_bytes(kb, 32);
        be32_to_bn(kb, k);
        if (bn_is_zero(k) || bn_cmp(k, P256_N) >= 0) continue;

        /* (x, y) = k*G ; r = x mod n */
        pt_mul(&kG, k, &G);
        if (kG.infinity) continue;
        bn_copy(r, kG.x);
        if (bn_cmp(r, P256_N) >= 0) bn_sub(r, r, P256_N);
        if (bn_is_zero(r)) continue;

        /* s = k^-1 * (z + d*r) mod n */
        bn_mod_inv(kinv, k, P256_N);
        uint64_t dr[4], zdr[4];
        bn_mod_mul(dr, d, r, P256_N);
        bn_mod_add(zdr, z, dr, P256_N);
        bn_mod_mul(s, kinv, zdr, P256_N);
        if (bn_is_zero(s)) continue;

        /* Encode DER */
        uint8_t r_be[32], s_be[32];
        bn_to_be32(r, r_be);
        bn_to_be32(s, s_be);
        uint8_t inner[80];
        uint32_t off = 0;
        off += der_emit_int(inner + off, r_be);
        off += der_emit_int(inner + off, s_be);
        sig_out[0] = 0x30;            /* SEQUENCE */
        sig_out[1] = (uint8_t)off;    /* total inner length, always ≤ 70 */
        for (uint32_t i = 0; i < off; i++) sig_out[2 + i] = inner[i];
        return (int)(2 + off);
    }
    return -1;
}

/* ── ECDSA verify entry point ──────────────────────────────── */

int ecdsa_p256_verify(const uint8_t pub_x[32], const uint8_t pub_y[32],
                      const uint8_t hash[32],
                      const uint8_t *sig_der, uint32_t sig_der_len) {
    uint8_t r_be[32], s_be[32];
    if (parse_ecdsa_sig(sig_der, sig_der_len, r_be, s_be) < 0) return -1;

    uint64_t r[4], s[4], z[4];
    be32_to_bn(r_be, r);
    be32_to_bn(s_be, s);
    be32_to_bn(hash, z);

    /* Range check r, s ∈ [1, n-1] */
    if (bn_is_zero(r) || bn_cmp(r, P256_N) >= 0) return -1;
    if (bn_is_zero(s) || bn_cmp(s, P256_N) >= 0) return -1;

    /* w = s^-1 mod n */
    uint64_t w[4]; bn_mod_inv(w, s, P256_N);

    uint64_t u1[4], u2[4];
    bn_mod_mul(u1, z, w, P256_N);
    bn_mod_mul(u2, r, w, P256_N);

    ec_point_t G, Pub, u1G, u2P, sum;
    bn_copy(G.x, P256_GX); bn_copy(G.y, P256_GY); G.infinity = 0;
    be32_to_bn(pub_x, Pub.x);
    be32_to_bn(pub_y, Pub.y);
    Pub.infinity = 0;

    pt_mul(&u1G, u1, &G);
    pt_mul(&u2P, u2, &Pub);
    pt_add(&sum, &u1G, &u2P);

    if (sum.infinity) return -1;

    /* Verify: sum.x mod n == r */
    uint64_t sxn[4]; bn_copy(sxn, sum.x);
    if (bn_cmp(sxn, P256_N) >= 0) bn_sub(sxn, sxn, P256_N);

    return (bn_cmp(sxn, r) == 0) ? 0 : -1;
}

/* ── Self-test (NIST CAVS P-256 SHA-256 sample vector) ─────── */
/*
 *  Msg  = "sample"  → SHA-256:
 *  AF2BDBE1AA9B6EC1E2ADE1D694F41FC71A831D0268E9891562113D8A62ADD1BF
 *  Public key:
 *    Qx = 60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6
 *    Qy = 7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299
 *  Signature:
 *    r  = EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716
 *    s  = F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8
 */

static const uint8_t cavs_qx[32] = {
    0x60,0xFE,0xD4,0xBA,0x25,0x5A,0x9D,0x31, 0xC9,0x61,0xEB,0x74,0xC6,0x35,0x6D,0x68,
    0xC0,0x49,0xB8,0x92,0x3B,0x61,0xFA,0x6C, 0xE6,0x69,0x62,0x2E,0x60,0xF2,0x9F,0xB6
};
static const uint8_t cavs_qy[32] = {
    0x79,0x03,0xFE,0x10,0x08,0xB8,0xBC,0x99, 0xA4,0x1A,0xE9,0xE9,0x56,0x28,0xBC,0x64,
    0xF2,0xF1,0xB2,0x0C,0x2D,0x7E,0x9F,0x51, 0x77,0xA3,0xC2,0x94,0xD4,0x46,0x22,0x99
};
static const uint8_t cavs_hash[32] = {
    0xAF,0x2B,0xDB,0xE1,0xAA,0x9B,0x6E,0xC1, 0xE2,0xAD,0xE1,0xD6,0x94,0xF4,0x1F,0xC7,
    0x1A,0x83,0x1D,0x02,0x68,0xE9,0x89,0x15, 0x62,0x11,0x3D,0x8A,0x62,0xAD,0xD1,0xBF
};
/* DER-encoded {r,s}:
 *   30 46 02 21 00 EFD48B2A...EAF3716   02 21 00 F7CB1C94...843ACDA8 */
static const uint8_t cavs_sig[] = {
    0x30, 0x46,
    0x02, 0x21, 0x00,
    0xEF,0xD4,0x8B,0x2A,0xAC,0xB6,0xA8,0xFD, 0x11,0x40,0xDD,0x9C,0xD4,0x5E,0x81,0xD6,
    0x9D,0x2C,0x87,0x7B,0x56,0xAA,0xF9,0x91, 0xC3,0x4D,0x0E,0xA8,0x4E,0xAF,0x37,0x16,
    0x02, 0x21, 0x00,
    0xF7,0xCB,0x1C,0x94,0x2D,0x65,0x7C,0x41, 0xD4,0x36,0xC7,0xA1,0xB6,0xE2,0x9F,0x65,
    0xF3,0xE9,0x00,0xDB,0xB9,0xAF,0xF4,0x06, 0x4D,0xC4,0xAB,0x2F,0x84,0x3A,0xCD,0xA8
};

/* RFC 6979 §A.2.5 P-256 private key matching cavs_qx / cavs_qy. */
static const uint8_t rfc6979_d[32] = {
    0xC9,0xAF,0xA9,0xD8,0x45,0xBA,0x75,0x16, 0x6B,0x5C,0x21,0x57,0x67,0xB1,0xD6,0x93,
    0x4E,0x50,0xC3,0xDB,0x36,0xE8,0x9B,0x12, 0x7B,0x8A,0x62,0x2B,0x12,0x0F,0x67,0x21
};

int ecdsa_p256_self_test(void)
{
    int rc = ecdsa_p256_verify(cavs_qx, cavs_qy, cavs_hash,
                                 cavs_sig, sizeof cavs_sig);
    if (rc != 0) {
        serial_puts("[ECDSA] P-256 verify self-test FAILED\n");
        return rc;
    }
    serial_puts("[ECDSA] P-256 verify self-test OK\n");

    /* Sign roundtrip: pick random k, sign cavs_hash with rfc6979_d,
     * verify against (cavs_qx, cavs_qy).  Confirms the sign primitive
     * is self-consistent with the verify primitive. */
    uint8_t sig[72];
    int sl = ecdsa_p256_sign(rfc6979_d, cavs_hash, sig, sizeof sig);
    if (sl < 0) {
        serial_puts("[ECDSA] P-256 sign self-test FAILED (sign returned -1)\n");
        return -1;
    }
    rc = ecdsa_p256_verify(cavs_qx, cavs_qy, cavs_hash,
                            sig, (uint32_t)sl);
    if (rc == 0) {
        serial_puts("[ECDSA] P-256 sign roundtrip OK\n");
    } else {
        serial_puts("[ECDSA] P-256 sign roundtrip FAILED — verify rejected our signature\n");
    }
    return rc;
}
