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
 * Field/order products use Montgomery reduction and scalar multiplication
 * uses Jacobian coordinates, so certificate checks fit network deadlines.
 * Not constant-time: verification only handles public values. Do not reuse
 * this implementation for signing or private scalar operations.
 */

#include "../include/types.h"
#include "ecdsa_p384.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

#define BL 6

/* 384-bit unsigned arithmetic, little-endian 64-bit limbs. */
static inline void bn_zero(uint64_t r[BL])
{
    for (int i = 0; i < BL; i++) r[i] = 0;
}

static inline void bn_copy(uint64_t r[BL], const uint64_t a[BL])
{
    for (int i = 0; i < BL; i++) r[i] = a[i];
}

static inline int bn_is_zero(const uint64_t a[BL])
{
    uint64_t bits = 0;
    for (int i = 0; i < BL; i++) bits |= a[i];
    return bits == 0;
}

static int bn_cmp(const uint64_t a[BL], const uint64_t b[BL])
{
    for (int i = BL - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

static uint64_t bn_add(uint64_t out[BL], const uint64_t a[BL],
                       const uint64_t b[BL])
{
    uint64_t carry = 0;
    for (int i = 0; i < BL; i++) {
        __uint128_t sum = (__uint128_t)a[i] + b[i] + carry;
        out[i] = (uint64_t)sum;
        carry = (uint64_t)(sum >> 64);
    }
    return carry;
}

static uint64_t bn_sub(uint64_t out[BL], const uint64_t a[BL],
                       const uint64_t b[BL])
{
    uint64_t borrow = 0;
    for (int i = 0; i < BL; i++) {
        uint64_t d = a[i] - b[i];
        uint64_t b1 = a[i] < b[i];
        uint64_t d2 = d - borrow;
        uint64_t b2 = d < borrow;
        out[i] = d2;
        borrow = b1 | b2;
    }
    return borrow;
}

typedef struct {
    const uint64_t *mod;
    const uint64_t *r2;
    const uint64_t *one;
    uint64_t n0_inv;
} mont_ctx_t;

/* out = a * b * R^-1 mod m, R = 2^384. */
static void mont_mul(uint64_t out[BL], const uint64_t a[BL],
                     const uint64_t b[BL], const mont_ctx_t *ctx)
{
    uint64_t t[BL * 2 + 1];
    for (int i = 0; i < BL * 2 + 1; i++) t[i] = 0;

    for (int i = 0; i < BL; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < BL; j++) {
            __uint128_t sum = (__uint128_t)a[i] * b[j] +
                              t[i + j] + carry;
            t[i + j] = (uint64_t)sum;
            carry = (uint64_t)(sum >> 64);
        }
        int k = i + BL;
        while (carry != 0) {
            __uint128_t sum = (__uint128_t)t[k] + carry;
            t[k] = (uint64_t)sum;
            carry = (uint64_t)(sum >> 64);
            k++;
        }
    }

    for (int i = 0; i < BL; i++) {
        uint64_t q = t[i] * ctx->n0_inv;
        uint64_t carry = 0;
        for (int j = 0; j < BL; j++) {
            __uint128_t sum = (__uint128_t)q * ctx->mod[j] +
                              t[i + j] + carry;
            t[i + j] = (uint64_t)sum;
            carry = (uint64_t)(sum >> 64);
        }
        int k = i + BL;
        while (carry != 0) {
            __uint128_t sum = (__uint128_t)t[k] + carry;
            t[k] = (uint64_t)sum;
            carry = (uint64_t)(sum >> 64);
            k++;
        }
    }

    for (int i = 0; i < BL; i++) out[i] = t[i + BL];
    if (t[BL * 2] != 0 || bn_cmp(out, ctx->mod) >= 0) {
        (void)bn_sub(out, out, ctx->mod);
    }
}

static void mont_encode(uint64_t out[BL], const uint64_t in[BL],
                        const mont_ctx_t *ctx)
{
    mont_mul(out, in, ctx->r2, ctx);
}

static void mont_decode(uint64_t out[BL], const uint64_t in[BL],
                        const mont_ctx_t *ctx)
{
    static const uint64_t one[BL] = {1, 0, 0, 0, 0, 0};
    mont_mul(out, in, one, ctx);
}

static void mont_pow(uint64_t out[BL], const uint64_t base[BL],
                     const uint64_t exp[BL], const mont_ctx_t *ctx)
{
    uint64_t acc[BL], tmp[BL];
    bn_copy(acc, ctx->one);
    for (int limb = BL - 1; limb >= 0; limb--) {
        for (int bit = 63; bit >= 0; bit--) {
            mont_mul(tmp, acc, acc, ctx);
            bn_copy(acc, tmp);
            if ((exp[limb] >> bit) & 1ULL) {
                mont_mul(tmp, acc, base, ctx);
                bn_copy(acc, tmp);
            }
        }
    }
    bn_copy(out, acc);
}

static void mont_inv(uint64_t out[BL], const uint64_t in[BL],
                     const mont_ctx_t *ctx)
{
    uint64_t two[BL] = {2, 0, 0, 0, 0, 0};
    uint64_t exp[BL];
    (void)bn_sub(exp, ctx->mod, two);
    mont_pow(out, in, exp, ctx);
}

static void mod_add(uint64_t out[BL], const uint64_t a[BL],
                    const uint64_t b[BL], const uint64_t mod[BL])
{
    uint64_t carry = bn_add(out, a, b);
    if (carry || bn_cmp(out, mod) >= 0) (void)bn_sub(out, out, mod);
}

static void mod_sub(uint64_t out[BL], const uint64_t a[BL],
                    const uint64_t b[BL], const uint64_t mod[BL])
{
    if (bn_sub(out, a, b)) (void)bn_add(out, out, mod);
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

static const uint64_t P384_P_R[BL] = {
    0xffffffff00000001ULL, 0x00000000ffffffffULL, 0x0000000000000001ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
};
static const uint64_t P384_P_R2[BL] = {
    0xfffffffe00000001ULL, 0x0000000200000000ULL, 0xfffffffe00000000ULL,
    0x0000000200000000ULL, 0x0000000000000001ULL, 0x0000000000000000ULL,
};
static const uint64_t P384_N_R[BL] = {
    0x1313e695333ad68dULL, 0xa7e5f24db74f5885ULL, 0x389cb27e0bc8d220ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
};
static const uint64_t P384_N_R2[BL] = {
    0x2d319b2419b409a9ULL, 0xff3d81e5df1aa419ULL, 0xbc3e483afcb82947ULL,
    0xd40d49174aab1cc5ULL, 0x3fb05b7a28266895ULL, 0x0c84ee012b39bf21ULL,
};

static const mont_ctx_t P384_FIELD = {
    P384_P, P384_P_R2, P384_P_R, 0x0000000100000001ULL,
};
static const mont_ctx_t P384_ORDER = {
    P384_N, P384_N_R2, P384_N_R, 0x6ed46089e88fdc45ULL,
};

/* Field values below stay in Montgomery form. */
static void fe_add(uint64_t out[BL], const uint64_t a[BL], const uint64_t b[BL])
{
    mod_add(out, a, b, P384_P);
}

static void fe_sub(uint64_t out[BL], const uint64_t a[BL], const uint64_t b[BL])
{
    mod_sub(out, a, b, P384_P);
}

static void fe_mul(uint64_t out[BL], const uint64_t a[BL], const uint64_t b[BL])
{
    mont_mul(out, a, b, &P384_FIELD);
}

static void fe_sqr(uint64_t out[BL], const uint64_t a[BL])
{
    mont_mul(out, a, a, &P384_FIELD);
}

static void fe_mul2(uint64_t out[BL], const uint64_t a[BL])
{
    fe_add(out, a, a);
}

static void fe_mul3(uint64_t out[BL], const uint64_t a[BL])
{
    uint64_t twice[BL];
    fe_mul2(twice, a);
    fe_add(out, twice, a);
}

static void fe_mul4(uint64_t out[BL], const uint64_t a[BL])
{
    uint64_t twice[BL];
    fe_mul2(twice, a);
    fe_mul2(out, twice);
}

static void fe_mul8(uint64_t out[BL], const uint64_t a[BL])
{
    uint64_t four[BL];
    fe_mul4(four, a);
    fe_mul2(out, four);
}

typedef struct {
    uint64_t x[BL], y[BL];
    int infinity;
} ec_affine_t;

typedef struct {
    uint64_t x[BL], y[BL], z[BL];
    int infinity;
} ec_jacobian_t;

static void jac_set_inf(ec_jacobian_t *p)
{
    bn_zero(p->x);
    bn_zero(p->y);
    bn_zero(p->z);
    p->infinity = 1;
}

static void jac_from_affine(ec_jacobian_t *out, const ec_affine_t *p)
{
    if (p->infinity) {
        jac_set_inf(out);
        return;
    }
    bn_copy(out->x, p->x);
    bn_copy(out->y, p->y);
    bn_copy(out->z, P384_FIELD.one);
    out->infinity = 0;
}

/* Jacobian doubling specialized for a = -3. */
static void jac_double(ec_jacobian_t *out, const ec_jacobian_t *p)
{
    if (p->infinity || bn_is_zero(p->y)) {
        jac_set_inf(out);
        return;
    }

    uint64_t delta[BL], gamma[BL], beta[BL], alpha[BL];
    uint64_t t1[BL], t2[BL], x3[BL], y3[BL], z3[BL];

    fe_sqr(delta, p->z);
    fe_sqr(gamma, p->y);
    fe_mul(beta, p->x, gamma);
    fe_sub(t1, p->x, delta);
    fe_add(t2, p->x, delta);
    fe_mul(alpha, t1, t2);
    fe_mul3(alpha, alpha);

    fe_sqr(x3, alpha);
    fe_mul8(t1, beta);
    fe_sub(x3, x3, t1);

    fe_add(z3, p->y, p->z);
    fe_sqr(z3, z3);
    fe_sub(z3, z3, gamma);
    fe_sub(z3, z3, delta);

    fe_mul4(t1, beta);
    fe_sub(t1, t1, x3);
    fe_mul(y3, alpha, t1);
    fe_sqr(t2, gamma);
    fe_mul8(t2, t2);
    fe_sub(y3, y3, t2);

    bn_copy(out->x, x3);
    bn_copy(out->y, y3);
    bn_copy(out->z, z3);
    out->infinity = 0;
}

/* Add an affine point to a Jacobian point. */
static void jac_add_mixed(ec_jacobian_t *out, const ec_jacobian_t *p,
                          const ec_affine_t *q)
{
    if (p->infinity) {
        jac_from_affine(out, q);
        return;
    }
    if (q->infinity) {
        *out = *p;
        return;
    }

    uint64_t z1z1[BL], u2[BL], s2[BL], h[BL], rr[BL];
    uint64_t hh[BL], i4[BL], j[BL], v[BL];
    uint64_t x3[BL], y3[BL], z3[BL], t1[BL], t2[BL];

    fe_sqr(z1z1, p->z);
    fe_mul(u2, q->x, z1z1);
    fe_mul(t1, p->z, z1z1);
    fe_mul(s2, q->y, t1);
    fe_sub(h, u2, p->x);
    fe_sub(rr, s2, p->y);

    if (bn_is_zero(h)) {
        if (bn_is_zero(rr)) jac_double(out, p);
        else jac_set_inf(out);
        return;
    }

    fe_sqr(hh, h);
    fe_mul4(i4, hh);
    fe_mul(j, h, i4);
    fe_mul2(rr, rr);
    fe_mul(v, p->x, i4);

    fe_sqr(x3, rr);
    fe_sub(x3, x3, j);
    fe_mul2(t1, v);
    fe_sub(x3, x3, t1);

    fe_sub(t1, v, x3);
    fe_mul(y3, rr, t1);
    fe_mul(t2, p->y, j);
    fe_mul2(t2, t2);
    fe_sub(y3, y3, t2);

    fe_add(z3, p->z, h);
    fe_sqr(z3, z3);
    fe_sub(z3, z3, z1z1);
    fe_sub(z3, z3, hh);

    bn_copy(out->x, x3);
    bn_copy(out->y, y3);
    bn_copy(out->z, z3);
    out->infinity = 0;
}

/* Full Jacobian addition, used once to combine u1*G and u2*Q. */
static void jac_add(ec_jacobian_t *out, const ec_jacobian_t *p,
                    const ec_jacobian_t *q)
{
    if (p->infinity) { *out = *q; return; }
    if (q->infinity) { *out = *p; return; }

    uint64_t z1z1[BL], z2z2[BL], u1[BL], u2[BL], s1[BL], s2[BL];
    uint64_t h[BL], rr[BL], i4[BL], j[BL], v[BL];
    uint64_t x3[BL], y3[BL], z3[BL], t1[BL], t2[BL];

    fe_sqr(z1z1, p->z);
    fe_sqr(z2z2, q->z);
    fe_mul(u1, p->x, z2z2);
    fe_mul(u2, q->x, z1z1);
    fe_mul(t1, q->z, z2z2);
    fe_mul(s1, p->y, t1);
    fe_mul(t1, p->z, z1z1);
    fe_mul(s2, q->y, t1);
    fe_sub(h, u2, u1);
    fe_sub(rr, s2, s1);

    if (bn_is_zero(h)) {
        if (bn_is_zero(rr)) jac_double(out, p);
        else jac_set_inf(out);
        return;
    }

    fe_mul2(t1, h);
    fe_sqr(i4, t1);
    fe_mul(j, h, i4);
    fe_mul2(rr, rr);
    fe_mul(v, u1, i4);

    fe_sqr(x3, rr);
    fe_sub(x3, x3, j);
    fe_mul2(t1, v);
    fe_sub(x3, x3, t1);

    fe_sub(t1, v, x3);
    fe_mul(y3, rr, t1);
    fe_mul(t2, s1, j);
    fe_mul2(t2, t2);
    fe_sub(y3, y3, t2);

    fe_add(z3, p->z, q->z);
    fe_sqr(z3, z3);
    fe_sub(z3, z3, z1z1);
    fe_sub(z3, z3, z2z2);
    fe_mul(z3, z3, h);

    bn_copy(out->x, x3);
    bn_copy(out->y, y3);
    bn_copy(out->z, z3);
    out->infinity = 0;
}

static void ec_scalar_mul(ec_jacobian_t *out, const uint64_t scalar[BL],
                          const ec_affine_t *point)
{
    ec_jacobian_t acc;
    jac_set_inf(&acc);
    for (int limb = BL - 1; limb >= 0; limb--) {
        for (int bit = 63; bit >= 0; bit--) {
            ec_jacobian_t tmp;
            jac_double(&tmp, &acc);
            acc = tmp;
            if ((scalar[limb] >> bit) & 1ULL) {
                jac_add_mixed(&tmp, &acc, point);
                acc = tmp;
            }
        }
    }
    *out = acc;
}

static int ec_affine_on_curve(const ec_affine_t *p)
{
    uint64_t lhs[BL], x2[BL], rhs[BL], three_x[BL], b[BL];
    fe_sqr(lhs, p->y);
    fe_sqr(x2, p->x);
    fe_mul(rhs, x2, p->x);
    fe_mul3(three_x, p->x);
    fe_sub(rhs, rhs, three_x);
    mont_encode(b, P384_B, &P384_FIELD);
    fe_add(rhs, rhs, b);
    return bn_cmp(lhs, rhs) == 0;
}

static int jac_get_x(uint64_t x_normal[BL], const ec_jacobian_t *p)
{
    if (p->infinity || bn_is_zero(p->z)) return -1;
    uint64_t z_inv[BL], z2_inv[BL], x_mont[BL];
    mont_inv(z_inv, p->z, &P384_FIELD);
    fe_sqr(z2_inv, z_inv);
    fe_mul(x_mont, p->x, z2_inv);
    mont_decode(x_normal, x_mont, &P384_FIELD);
    return 0;
}

/* ── DER signature parsing ──────────────────────────────────── */

/* Parse SEQUENCE { INTEGER r, INTEGER s } into padded big-endian scalars. */
static int parse_ecdsa_sig(const uint8_t *sig, uint32_t sig_len,
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
    if (parse_ecdsa_sig(sig_der, sig_der_len, r_be, s_be) < 0) return -1;

    uint64_t r[BL], s[BL], z[BL];
    be48_to_bn(r_be, r);
    be48_to_bn(s_be, s);
    be48_to_bn(hash, z);

    /* Range check: r, s ∈ [1, n−1]. */
    if (bn_is_zero(r) || bn_cmp(r, P384_N) >= 0) return -1;
    if (bn_is_zero(s) || bn_cmp(s, P384_N) >= 0) return -1;

    if (bn_cmp(z, P384_N) >= 0) (void)bn_sub(z, z, P384_N);

    /* Compute the ECDSA scalars in the order's Montgomery domain. */
    uint64_t r_m[BL], s_m[BL], z_m[BL], w_m[BL];
    uint64_t u1_m[BL], u2_m[BL], u1[BL], u2[BL];
    mont_encode(r_m, r, &P384_ORDER);
    mont_encode(s_m, s, &P384_ORDER);
    mont_encode(z_m, z, &P384_ORDER);
    mont_inv(w_m, s_m, &P384_ORDER);
    mont_mul(u1_m, z_m, w_m, &P384_ORDER);
    mont_mul(u2_m, r_m, w_m, &P384_ORDER);
    mont_decode(u1, u1_m, &P384_ORDER);
    mont_decode(u2, u2_m, &P384_ORDER);

    uint64_t pub_x_n[BL], pub_y_n[BL];
    be48_to_bn(pub_x, pub_x_n);
    be48_to_bn(pub_y, pub_y_n);
    if (bn_cmp(pub_x_n, P384_P) >= 0 || bn_cmp(pub_y_n, P384_P) >= 0)
        return -1;

    ec_affine_t generator, pub;
    mont_encode(generator.x, P384_GX, &P384_FIELD);
    mont_encode(generator.y, P384_GY, &P384_FIELD);
    generator.infinity = 0;
    mont_encode(pub.x, pub_x_n, &P384_FIELD);
    mont_encode(pub.y, pub_y_n, &P384_FIELD);
    pub.infinity = 0;
    if (!ec_affine_on_curve(&pub)) return -1;

    ec_jacobian_t u1g, u2p, sum;
    ec_scalar_mul(&u1g, u1, &generator);
    ec_scalar_mul(&u2p, u2, &pub);
    jac_add(&sum, &u1g, &u2p);

    uint64_t x[BL];
    if (jac_get_x(x, &sum) < 0) return -1;
    if (bn_cmp(x, P384_N) >= 0) (void)bn_sub(x, x, P384_N);
    return bn_cmp(x, r) == 0 ? 0 : -1;
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
