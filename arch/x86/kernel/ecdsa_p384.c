/*
 * ecdsa_p384.c — minimal ECDSA P-384 verify.
 *
 * Same shape as ecdsa_p256.c but 6 × uint64 limbs (384 bits) and
 * with the P-384 (secp384r1, NIST FIPS 186-4) curve parameters.
 * Verify-only.  Used to close the WE1 → GTS Root R4 chain link
 * (the WE1 intermediate's signature is ECDSA-P256-SHA384 — wait,
 * that's the OID; the actual curve is determined by the *issuer's*
 * public key, which for GTS Root R4 is P-384).
 *
 * Correctness > speed.  Verify runs once per chain link, so the
 * O(n²) shift-and-subtract modular reduction and Fermat's little
 * theorem inversion are fine.  Not constant-time — verify operates
 * on public values; do NOT reuse for signing.
 */

#include "../include/types.h"
#include "ecdsa_p384.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

#define BL 6   /* limbs */

/* ── 384-bit unsigned bignum primitives ───────────────────────── */

static inline void bn_zero(uint64_t r[BL]) { for (int i = 0; i < BL; i++) r[i] = 0; }
static inline void bn_copy(uint64_t r[BL], const uint64_t a[BL]) {
    for (int i = 0; i < BL; i++) r[i] = a[i];
}
static inline int bn_is_zero(const uint64_t a[BL]) {
    uint64_t s = 0; for (int i = 0; i < BL; i++) s |= a[i]; return s == 0;
}
static int bn_cmp(const uint64_t a[BL], const uint64_t b[BL]) {
    for (int i = BL - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static uint64_t bn_add(uint64_t c[BL], const uint64_t a[BL], const uint64_t b[BL]) {
    uint64_t carry = 0;
    for (int i = 0; i < BL; i++) {
        __uint128_t s = (__uint128_t)a[i] + b[i] + carry;
        c[i] = (uint64_t)s;
        carry = (uint64_t)(s >> 64);
    }
    return carry;
}
static uint64_t bn_sub(uint64_t c[BL], const uint64_t a[BL], const uint64_t b[BL]) {
    __int128_t s = 0;
    for (int i = 0; i < BL; i++) {
        s = (__int128_t)a[i] - b[i] + (int64_t)(s >> 64);
        c[i] = (uint64_t)s;
    }
    return (uint64_t)((s >> 64) & 1);
}

/* Full 12-limb product a*b. */
static void bn_mul_full(uint64_t c[BL * 2], const uint64_t a[BL], const uint64_t b[BL])
{
    for (int i = 0; i < BL * 2; i++) c[i] = 0;
    for (int i = 0; i < BL; i++) {
        __uint128_t carry = 0;
        for (int j = 0; j < BL; j++) {
            __uint128_t prod = (__uint128_t)a[i] * b[j] + c[i + j] + (uint64_t)carry;
            c[i + j] = (uint64_t)prod;
            carry = prod >> 64;
        }
        c[i + BL] = (uint64_t)carry;
    }
}

/* prod (12 limbs) mod m (6 limbs).  Shift-and-subtract — 384 iters. */
static void bn_mod_full(uint64_t r[BL], const uint64_t prod[BL * 2], const uint64_t m[BL])
{
    uint64_t hi[BL], lo[BL];
    for (int i = 0; i < BL; i++) lo[i] = prod[i];
    for (int i = 0; i < BL; i++) hi[i] = prod[i + BL];

    for (int bit = 0; bit < BL * 64; bit++) {
        (void)bit;
        uint64_t carry = 0;
        for (int i = 0; i < BL; i++) {
            uint64_t nc = lo[i] >> 63;
            lo[i] = (lo[i] << 1) | carry;
            carry = nc;
        }
        for (int i = 0; i < BL; i++) {
            uint64_t nc = hi[i] >> 63;
            hi[i] = (hi[i] << 1) | carry;
            carry = nc;
        }
        if (carry || bn_cmp(hi, m) >= 0) {
            uint64_t tmp[BL];
            bn_sub(tmp, hi, m);
            for (int i = 0; i < BL; i++) hi[i] = tmp[i];
        }
    }
    for (int i = 0; i < BL; i++) r[i] = hi[i];
}

static void bn_mod_add(uint64_t r[BL], const uint64_t a[BL], const uint64_t b[BL],
                       const uint64_t m[BL])
{
    uint64_t carry = bn_add(r, a, b);
    if (carry || bn_cmp(r, m) >= 0) {
        uint64_t tmp[BL];
        bn_sub(tmp, r, m);
        for (int i = 0; i < BL; i++) r[i] = tmp[i];
    }
}
static void bn_mod_sub(uint64_t r[BL], const uint64_t a[BL], const uint64_t b[BL],
                       const uint64_t m[BL])
{
    uint64_t borrow = bn_sub(r, a, b);
    if (borrow) {
        uint64_t tmp[BL];
        bn_add(tmp, r, m);
        for (int i = 0; i < BL; i++) r[i] = tmp[i];
    }
}
static void bn_mod_mul(uint64_t r[BL], const uint64_t a[BL], const uint64_t b[BL],
                       const uint64_t m[BL])
{
    uint64_t prod[BL * 2];
    bn_mul_full(prod, a, b);
    bn_mod_full(r, prod, m);
}

/* r = base ^ exp mod m.  exp is a BL-limb bignum.  Square-and-
 * multiply over the bits of exp, MSB first. */
static void bn_mod_pow(uint64_t r[BL], const uint64_t base[BL],
                       const uint64_t exp[BL], const uint64_t m[BL])
{
    uint64_t res[BL], b[BL];
    bn_zero(res); res[0] = 1;
    bn_copy(b, base);
    for (int limb = 0; limb < BL; limb++) {
        uint64_t e = exp[limb];
        for (int bit = 0; bit < 64; bit++) {
            if (e & 1) bn_mod_mul(res, res, b, m);
            e >>= 1;
            bn_mod_mul(b, b, b, m);
        }
    }
    bn_copy(r, res);
}

/* Modular inverse via Fermat: a^(p-2) mod p. */
static void bn_mod_inv(uint64_t r[BL], const uint64_t a[BL], const uint64_t m[BL])
{
    /* exp = m - 2 */
    uint64_t two[BL]; bn_zero(two); two[0] = 2;
    uint64_t exp[BL]; bn_sub(exp, m, two);
    bn_mod_pow(r, a, exp, m);
}

/* ── P-384 curve constants (FIPS 186-4 D.2.4) ────────────────── */

/* p = 2^384 − 2^128 − 2^96 + 2^32 − 1 */
static const uint64_t P384_P[BL] = {
    0x00000000ffffffffULL, 0xffffffff00000000ULL, 0xfffffffffffffffeULL,
    0xffffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL,
};
/* Group order n */
static const uint64_t P384_N[BL] = {
    0xecec196accc52973ULL, 0x581a0db248b0a77aULL, 0xc7634d81f4372ddfULL,
    0xffffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL,
};
/* a = -3 mod p */
static const uint64_t P384_A[BL] = {
    0x00000000fffffffcULL, 0xffffffff00000000ULL, 0xfffffffffffffffeULL,
    0xffffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL,
};
/* b */
static const uint64_t P384_B[BL] = {
    0x2a85c8edd3ec2aefULL, 0xc656398d8a2ed19dULL, 0x0314088f5013875aULL,
    0x181d9c6efe814112ULL, 0x988e056be3f82d19ULL, 0xb3312fa7e23ee7e4ULL,
};
/* Base point Gx */
static const uint64_t P384_GX[BL] = {
    0x3a545e3872760ab7ULL, 0x5502f25dbf55296cULL, 0x59f741e082542a38ULL,
    0x6e1d3b628ba79b98ULL, 0x8eb1c71ef320ad74ULL, 0xaa87ca22be8b0537ULL,
};
/* Base point Gy */
static const uint64_t P384_GY[BL] = {
    0x7a431d7c90ea0e5fULL, 0x0a60b1ce1d7e819dULL, 0xe9da3113b5f0b8c0ULL,
    0xf8f41dbd289a147cULL, 0x5d9e98bf9292dc29ULL, 0x3617de4a96262c6fULL,
};

/* ── EC point operations (affine) ─────────────────────────────── */

typedef struct {
    uint64_t x[BL], y[BL];
    int      infinity;
} ec_point_t;

static void ec_set_inf(ec_point_t *p) { p->infinity = 1; bn_zero(p->x); bn_zero(p->y); }

/* Point doubling on y² = x³ + ax + b mod p.
 *   λ = (3·x² + a) / (2·y)
 *   x3 = λ² − 2·x
 *   y3 = λ·(x − x3) − y
 */
static void ec_double(ec_point_t *r, const ec_point_t *a)
{
    if (a->infinity) { ec_set_inf(r); return; }
    /* λ = (3x² + a) / (2y) */
    uint64_t x_sq[BL], three_x_sq[BL], num[BL], two_y[BL], inv[BL], lam[BL];
    bn_mod_mul(x_sq, a->x, a->x, P384_P);
    bn_mod_add(three_x_sq, x_sq, x_sq, P384_P);
    bn_mod_add(three_x_sq, three_x_sq, x_sq, P384_P);
    bn_mod_add(num, three_x_sq, P384_A, P384_P);
    bn_mod_add(two_y, a->y, a->y, P384_P);
    bn_mod_inv(inv, two_y, P384_P);
    bn_mod_mul(lam, num, inv, P384_P);

    /* x3 = λ² − 2x */
    uint64_t lam_sq[BL], two_x[BL], x3[BL];
    bn_mod_mul(lam_sq, lam, lam, P384_P);
    bn_mod_add(two_x, a->x, a->x, P384_P);
    bn_mod_sub(x3, lam_sq, two_x, P384_P);

    /* y3 = λ·(x − x3) − y */
    uint64_t x_minus_x3[BL], lam_dx[BL], y3[BL];
    bn_mod_sub(x_minus_x3, a->x, x3, P384_P);
    bn_mod_mul(lam_dx, lam, x_minus_x3, P384_P);
    bn_mod_sub(y3, lam_dx, a->y, P384_P);

    bn_copy(r->x, x3);
    bn_copy(r->y, y3);
    r->infinity = 0;
}

/* Point addition (a != b, neither infinity, x_a != x_b).
 * λ = (y_b − y_a) / (x_b − x_a)
 * x3 = λ² − x_a − x_b
 * y3 = λ·(x_a − x3) − y_a
 */
static void ec_add(ec_point_t *r, const ec_point_t *a, const ec_point_t *b)
{
    if (a->infinity) { bn_copy(r->x, b->x); bn_copy(r->y, b->y); r->infinity = b->infinity; return; }
    if (b->infinity) { bn_copy(r->x, a->x); bn_copy(r->y, a->y); r->infinity = a->infinity; return; }
    if (bn_cmp(a->x, b->x) == 0) {
        /* Same x: either doubling or P + (-P) = infinity. */
        if (bn_cmp(a->y, b->y) == 0) { ec_double(r, a); return; }
        ec_set_inf(r); return;
    }
    uint64_t dy[BL], dx[BL], inv[BL], lam[BL];
    bn_mod_sub(dy, b->y, a->y, P384_P);
    bn_mod_sub(dx, b->x, a->x, P384_P);
    bn_mod_inv(inv, dx, P384_P);
    bn_mod_mul(lam, dy, inv, P384_P);

    uint64_t lam_sq[BL], x3[BL];
    bn_mod_mul(lam_sq, lam, lam, P384_P);
    bn_mod_sub(x3, lam_sq, a->x, P384_P);
    bn_mod_sub(x3, x3, b->x, P384_P);

    uint64_t x_minus_x3[BL], lam_dx[BL], y3[BL];
    bn_mod_sub(x_minus_x3, a->x, x3, P384_P);
    bn_mod_mul(lam_dx, lam, x_minus_x3, P384_P);
    bn_mod_sub(y3, lam_dx, a->y, P384_P);

    bn_copy(r->x, x3);
    bn_copy(r->y, y3);
    r->infinity = 0;
}

/* Scalar multiply: r = k · P.  Double-and-add, MSB-first. */
static void ec_scalar_mul(ec_point_t *r, const uint64_t k[BL], const ec_point_t *P)
{
    ec_point_t acc; ec_set_inf(&acc);
    for (int limb = BL - 1; limb >= 0; limb--) {
        uint64_t v = k[limb];
        for (int bit = 63; bit >= 0; bit--) {
            ec_point_t tmp;
            ec_double(&tmp, &acc);
            if ((v >> bit) & 1) {
                ec_add(&acc, &tmp, P);
            } else {
                bn_copy(acc.x, tmp.x); bn_copy(acc.y, tmp.y);
                acc.infinity = tmp.infinity;
            }
        }
    }
    bn_copy(r->x, acc.x);
    bn_copy(r->y, acc.y);
    r->infinity = acc.infinity;
}

/* ── DER signature parsing ──────────────────────────────────── */

/* SEQUENCE { r INTEGER, s INTEGER } where r, s are <= 48 bytes big-
 * endian (leftmost zero pad allowed; sign byte stripped). */
static int parse_ecdsa_sig(const uint8_t *sig, uint32_t sig_len,
                           uint8_t r_be[48], uint8_t s_be[48])
{
    if (sig_len < 8 || sig[0] != 0x30) return -1;
    uint32_t i = 1;
    uint32_t seq_len;
    if ((sig[i] & 0x80) == 0) { seq_len = sig[i]; i++; }
    else {
        uint8_t nb = sig[i++] & 0x7F;
        if (nb == 0 || nb > 2 || i + nb > sig_len) return -1;
        seq_len = 0;
        for (uint8_t k = 0; k < nb; k++) seq_len = (seq_len << 8) | sig[i++];
    }
    if (i + seq_len > sig_len) return -1;

    /* r INTEGER */
    if (sig[i] != 0x02) return -1;
    i++;
    uint32_t r_len = sig[i++];
    if (i + r_len > sig_len) return -1;
    const uint8_t *r_p = sig + i;
    if (r_len > 1 && r_p[0] == 0x00) { r_p++; r_len--; }
    if (r_len > 48) return -1;
    for (int k = 0; k < 48; k++) r_be[k] = 0;
    for (uint32_t k = 0; k < r_len; k++) r_be[48 - r_len + k] = r_p[k];
    i += sig[i - 1];      /* advance past r */
    /* fix the cursor: we read r_len from sig[i-1] which was already
     * past the value if we'd already consumed it.  Recompute: */
    /* the above is brittle — let's just do it cleanly: */
    return 0;  /* fall through; cursor below */
}

/* Cleaner re-do — the above was getting tangled.  Parse from scratch
 * with a proper cursor.  Returns 0 + fills r_be / s_be with 48-byte
 * big-endian-padded scalars. */
static int parse_ecdsa_sig_clean(const uint8_t *sig, uint32_t sig_len,
                                  uint8_t r_be[48], uint8_t s_be[48])
{
    if (sig_len < 8 || sig[0] != 0x30) return -1;
    uint32_t p = 1;
    uint32_t seq_len;
    if ((sig[p] & 0x80) == 0) { seq_len = sig[p]; p++; }
    else {
        uint8_t nb = sig[p++] & 0x7F;
        if (nb == 0 || nb > 2 || p + nb > sig_len) return -1;
        seq_len = 0;
        for (uint8_t k = 0; k < nb; k++) seq_len = (seq_len << 8) | sig[p++];
    }
    if (p + seq_len > sig_len) return -1;
    uint32_t end = p + seq_len;

    for (int round = 0; round < 2; round++) {
        if (p >= end || sig[p] != 0x02) return -1;
        p++;
        if (p >= end) return -1;
        uint32_t int_len = sig[p++];
        if (p + int_len > end) return -1;
        const uint8_t *ip = sig + p;
        uint32_t il = int_len;
        if (il > 1 && ip[0] == 0x00) { ip++; il--; }
        if (il > 48) return -1;
        uint8_t *dst = (round == 0) ? r_be : s_be;
        for (int k = 0; k < 48; k++) dst[k] = 0;
        for (uint32_t k = 0; k < il; k++) dst[48 - il + k] = ip[k];
        p += int_len;
    }
    return 0;
}

/* ── Bn ↔ bytes ────────────────────────────────────────────── */

static void be48_to_bn(const uint8_t be[48], uint64_t bn[BL])
{
    for (int i = 0; i < BL; i++) bn[i] = 0;
    for (int i = 0; i < 48; i++) {
        bn[(47 - i) / 8] |= ((uint64_t)be[i]) << ((47 - i) % 8 * 8);
    }
}

/* ── Public API ─────────────────────────────────────────────── */

int ecdsa_p384_verify(const uint8_t pub_x[48], const uint8_t pub_y[48],
                      const uint8_t hash[48],
                      const uint8_t *sig_der, uint32_t sig_der_len)
{
    uint8_t r_be[48], s_be[48];
    if (parse_ecdsa_sig_clean(sig_der, sig_der_len, r_be, s_be) < 0) return -1;
    (void)parse_ecdsa_sig;   /* silence unused-function warning */

    uint64_t r[BL], s[BL], z[BL];
    be48_to_bn(r_be, r);
    be48_to_bn(s_be, s);
    be48_to_bn(hash, z);

    /* Range check: r, s ∈ [1, n−1]. */
    if (bn_is_zero(r) || bn_cmp(r, P384_N) >= 0) return -1;
    if (bn_is_zero(s) || bn_cmp(s, P384_N) >= 0) return -1;

    /* w = s^-1 mod n */
    uint64_t w[BL]; bn_mod_inv(w, s, P384_N);

    /* u1 = z · w mod n,  u2 = r · w mod n */
    uint64_t u1[BL], u2[BL];
    bn_mod_mul(u1, z, w, P384_N);
    bn_mod_mul(u2, r, w, P384_N);

    /* (x1, y1) = u1·G + u2·Pub */
    ec_point_t G, Pub, u1G, u2P, sum;
    bn_copy(G.x, P384_GX); bn_copy(G.y, P384_GY); G.infinity = 0;
    be48_to_bn(pub_x, Pub.x);
    be48_to_bn(pub_y, Pub.y);
    Pub.infinity = 0;

    ec_scalar_mul(&u1G, u1, &G);
    ec_scalar_mul(&u2P, u2, &Pub);
    ec_add(&sum, &u1G, &u2P);

    if (sum.infinity) return -1;

    /* Verify: r ≡ sum.x mod n. */
    uint64_t sum_x_mod_n[BL];
    bn_copy(sum_x_mod_n, sum.x);
    if (bn_cmp(sum_x_mod_n, P384_N) >= 0) {
        uint64_t tmp[BL];
        bn_sub(tmp, sum_x_mod_n, P384_N);
        for (int i = 0; i < BL; i++) sum_x_mod_n[i] = tmp[i];
    }
    if (bn_cmp(sum_x_mod_n, r) != 0) return -1;

    /* Suppress "unused but useful" b parameter — it's only conceptual. */
    (void)P384_B;
    return 0;
}

/* ── Self-test ──────────────────────────────────────────────── */

int ecdsa_p384_self_test(void)
{
    /* Offline-generated 2026-05-16 with openssl secp384r1.
     * Signing "abc" with SHA-384 → DER signature.
     * Public values only; replay-safe. */
    static const uint8_t pub_x[48] = {
        0xca,0x4d,0xe0,0x44,0xb8,0xf9,0xe2,0x34,0xae,0x4b,0xed,0x61,
        0x4c,0xb4,0x88,0x57,0xa3,0x1d,0xdd,0x27,0xae,0x49,0xcf,0x0c,
        0xa7,0x08,0x83,0x11,0x9a,0x8d,0xff,0xe5,0x35,0xd6,0x1e,0x3a,
        0xff,0x00,0x53,0x10,0xb9,0x3b,0x22,0xe3,0x04,0xcb,0xf7,0x5e,
    };
    static const uint8_t pub_y[48] = {
        0x4e,0xd0,0x90,0x6b,0x6a,0xa1,0x9f,0x82,0x44,0x67,0x66,0x75,
        0xfc,0x83,0xf6,0x5e,0x77,0xc0,0xcd,0xe7,0x43,0xa8,0x7e,0x34,
        0x7a,0xf1,0x1f,0x5b,0x5c,0xe8,0xb4,0x26,0xca,0x99,0x0d,0xb4,
        0x34,0x37,0xc7,0x8c,0xe9,0xae,0x82,0x34,0x52,0xdd,0x73,0x1f,
    };
    static const uint8_t hash_abc_sha384[48] = {
        0xcb,0x00,0x75,0x3f,0x45,0xa3,0x5e,0x8b,0xb5,0xa0,0x3d,0x69,
        0x9a,0xc6,0x50,0x07,0x27,0x2c,0x32,0xab,0x0e,0xde,0xd1,0x63,
        0x1a,0x8b,0x60,0x5a,0x43,0xff,0x5b,0xed,0x80,0x86,0x07,0x2b,
        0xa1,0xe7,0xcc,0x23,0x58,0xba,0xec,0xa1,0x34,0xc8,0x25,0xa7,
    };
    static const uint8_t sig_der[103] = {
        0x30,0x65,0x02,0x30,0x0f,0xc5,0xaf,0x9f,0xe4,0xe3,0xee,0xec,
        0x12,0x88,0x59,0xc0,0x31,0xf8,0x71,0x43,0xdc,0x71,0x09,0x61,
        0x99,0xe9,0x5e,0x72,0x8c,0x92,0xa8,0x73,0x71,0x12,0xdf,0x8a,
        0x77,0x2f,0x59,0x4f,0x50,0x07,0xbb,0x5d,0xdf,0xf8,0x45,0x34,
        0x0b,0x4d,0x01,0x6b,0x02,0x31,0x00,0xb3,0xd3,0x7d,0xe4,0xbd,
        0x90,0xc6,0x52,0xbb,0x32,0x6c,0x11,0x81,0x8b,0x53,0x71,0x8c,
        0x21,0x9e,0xb6,0x14,0x8e,0x70,0xb5,0xf1,0x03,0x56,0x90,0xcd,
        0xdd,0x7e,0x0c,0x88,0xb4,0x4c,0x44,0x27,0xe5,0x72,0x62,0x79,
        0x24,0xe4,0xb0,0x4d,0x94,0xaa,0x2e,
    };
    int rc_pos = ecdsa_p384_verify(pub_x, pub_y, hash_abc_sha384,
                                    sig_der, sizeof sig_der);
    /* Negative: flip a bit in the hash. */
    uint8_t bad[48]; for (int i = 0; i < 48; i++) bad[i] = hash_abc_sha384[i];
    bad[0] ^= 0x01;
    int rc_neg = ecdsa_p384_verify(pub_x, pub_y, bad, sig_der, sizeof sig_der);
    if (rc_pos == 0 && rc_neg != 0) return 0;
    return -1;
}
