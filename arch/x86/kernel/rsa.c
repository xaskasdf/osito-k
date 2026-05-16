/*
 * rsa.c — RSA-2048 PKCS#1 v1.5 signature verification.
 *
 * Verify-only.  No keygen, no signing, no encryption.  Public values
 * only (the signature, the public key (n, e), and the candidate
 * hash) — so we don't sweat constant-time discipline that would
 * matter for a private-key op.
 *
 * Wire format (PKCS#1 v1.5 for SHA-256):
 *
 *   Sig    = RSAEP(n, e, EM)
 *   EM     = 0x00 || 0x01 || PS || 0x00 || DigestInfo
 *   PS     = 0xFF * (k - 3 - len(DigestInfo))           (k = 256 for RSA-2048)
 *   DigestInfo = ASN.1 SHA-256 prefix (19 bytes) || hash (32 bytes)
 *
 * Caller computes the hash, hands us (sig, sig_len, n, n_len, e, e_len,
 * expected_hash).  We modexp the signature, verify the padding, and
 * compare the inner SHA-256 hash.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

/* ── Bignum: 32 × uint64 limbs, little-endian (limb[0] = low). ─── */

#define BN_LIMBS  32                /* 2048 bits / 64 */
typedef struct { uint64_t v[BN_LIMBS]; } bn_t;

static void bn_zero(bn_t *r) {
    for (int i = 0; i < BN_LIMBS; i++) r->v[i] = 0;
}
static void bn_copy(bn_t *r, const bn_t *a) {
    for (int i = 0; i < BN_LIMBS; i++) r->v[i] = a->v[i];
}
static int bn_is_zero(const bn_t *a) {
    uint64_t acc = 0;
    for (int i = 0; i < BN_LIMBS; i++) acc |= a->v[i];
    return acc == 0;
}
static int bn_cmp(const bn_t *a, const bn_t *b) {
    for (int i = BN_LIMBS - 1; i >= 0; i--) {
        if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    }
    return 0;
}
/* Read big-endian byte buffer into limb array. Buffer length up to
 * BN_LIMBS*8 bytes.  Leading-zero short buffers are zero-extended. */
static int bn_from_be(bn_t *r, const uint8_t *be, uint32_t len) {
    if (len > BN_LIMBS * 8) return -1;
    bn_zero(r);
    /* Walk from least-significant byte (end of buffer) backwards. */
    uint32_t i = 0;
    int32_t  bi = (int32_t)len - 1;
    while (bi >= 0) {
        uint64_t limb = 0;
        for (int b = 0; b < 8 && bi >= 0; b++, bi--)
            limb |= ((uint64_t)be[bi]) << (b * 8);
        r->v[i++] = limb;
        if (i >= BN_LIMBS) break;
    }
    return 0;
}
/* Write limbs into big-endian buffer. Always writes BN_LIMBS*8 bytes. */
static void bn_to_be(const bn_t *a, uint8_t *out) {
    for (int i = 0; i < BN_LIMBS; i++) {
        uint64_t limb = a->v[i];
        int off = (BN_LIMBS - 1 - i) * 8;
        for (int b = 0; b < 8; b++)
            out[off + b] = (uint8_t)(limb >> ((7 - b) * 8));
    }
}

/* Add a + b → r ; returns carry-out. */
static uint64_t bn_add(bn_t *r, const bn_t *a, const bn_t *b) {
    __uint128_t s = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        s = (__uint128_t)a->v[i] + b->v[i] + (uint64_t)(s >> 64);
        r->v[i] = (uint64_t)s;
    }
    return (uint64_t)(s >> 64);
}
/* Subtract a - b → r ; returns borrow-out (0 or 1). */
static uint64_t bn_sub(bn_t *r, const bn_t *a, const bn_t *b) {
    __int128_t s = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        s = (__int128_t)a->v[i] - b->v[i] + (int64_t)(s >> 64);
        r->v[i] = (uint64_t)s;
    }
    /* Top bit of (s >> 64) is the borrow-out flag. */
    return (uint64_t)((s >> 64) & 1);
}

/* r ← (a * b) mod 2^2048 (i.e. the low 32 limbs of the product). */
static void bn_mul_low(bn_t *r, const bn_t *a, const bn_t *b) {
    uint64_t buf[BN_LIMBS] = {0};
    for (int i = 0; i < BN_LIMBS; i++) {
        __uint128_t carry = 0;
        for (int j = 0; j + i < BN_LIMBS; j++) {
            __uint128_t prod = (__uint128_t)a->v[i] * b->v[j] + buf[i + j] + (uint64_t)carry;
            buf[i + j] = (uint64_t)prod;
            carry = prod >> 64;
        }
    }
    for (int i = 0; i < BN_LIMBS; i++) r->v[i] = buf[i];
}

/* Full 64-limb product a*b → c[0..64].  Output limbs little-endian. */
static void bn_mul_full(uint64_t c[BN_LIMBS * 2], const bn_t *a, const bn_t *b) {
    for (int i = 0; i < BN_LIMBS * 2; i++) c[i] = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        __uint128_t carry = 0;
        for (int j = 0; j < BN_LIMBS; j++) {
            __uint128_t prod = (__uint128_t)a->v[i] * b->v[j] + c[i + j] + (uint64_t)carry;
            c[i + j] = (uint64_t)prod;
            carry = prod >> 64;
        }
        c[i + BN_LIMBS] = (uint64_t)carry;
    }
}

/* Schoolbook modulo: divide 64-limb dividend by 32-limb modulus.
 *
 * For verify-only this is fine — slow (O(limbs^2)) but works.  We're
 * called once per cert signature; no perf budget concerns. */
static void bn_mod_full(bn_t *r, const uint64_t prod[BN_LIMBS * 2], const bn_t *m) {
    /* Take the top half of the product, then shift in 32 more limbs
     * (worth of zero limbs) until we land within m.  We use a
     * shift-and-subtract loop on the whole 4096-bit value, treating
     * `prod` as the high-half and gradually pulling in bits.
     *
     * Simpler implementation that works: maintain a 4096-bit
     * accumulator (high|low), shift left by 1 each step, and
     * conditionally subtract m.  Iterate 2048 times.  Slow (~2048 ×
     * O(32-limb add/sub)) but solid.
     *
     * For a verify of 1 cert chain (≤4 sigs) this is fine. */
    uint64_t hi[BN_LIMBS] = {0};      /* upper 2048 bits */
    uint64_t lo[BN_LIMBS];             /* lower 2048 bits (input) */
    for (int i = 0; i < BN_LIMBS; i++) lo[i] = prod[i];
    for (int i = 0; i < BN_LIMBS; i++) hi[i] = prod[i + BN_LIMBS];

    for (int bit = 2047; bit >= 0; bit--) {
        (void)bit;
        /* Shift {hi:lo} left by 1. */
        uint64_t carry = 0;
        for (int i = 0; i < BN_LIMBS; i++) {
            uint64_t nc = lo[i] >> 63;
            lo[i] = (lo[i] << 1) | carry;
            carry = nc;
        }
        uint64_t hi_carry = 0;
        for (int i = 0; i < BN_LIMBS; i++) {
            uint64_t nc = hi[i] >> 63;
            hi[i] = (hi[i] << 1) | carry;
            carry = nc;
            (void)hi_carry;
        }
        /* If hi >= m or there was an overflow out of hi, subtract m. */
        bn_t hi_bn;
        for (int i = 0; i < BN_LIMBS; i++) hi_bn.v[i] = hi[i];
        if (carry || bn_cmp(&hi_bn, m) >= 0) {
            bn_t after;
            bn_sub(&after, &hi_bn, m);
            for (int i = 0; i < BN_LIMBS; i++) hi[i] = after.v[i];
        }
    }
    for (int i = 0; i < BN_LIMBS; i++) r->v[i] = hi[i];
}

/* r = (a * b) mod m */
static void bn_mod_mul(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *m) {
    uint64_t prod[BN_LIMBS * 2];
    bn_mul_full(prod, a, b);
    bn_mod_full(r, prod, m);
}

/* r = base ^ exp mod m.  exp is at most 64 bits (RSA verify uses
 * small e — typically 65537). */
static void bn_mod_pow_small_e(bn_t *r, const bn_t *base, uint64_t exp, const bn_t *m) {
    bn_t result;
    bn_zero(&result); result.v[0] = 1;            /* 1 */

    bn_t b; bn_copy(&b, base);

    while (exp) {
        if (exp & 1) bn_mod_mul(&result, &result, &b, m);
        exp >>= 1;
        if (exp) bn_mod_mul(&b, &b, &b, m);
    }
    bn_copy(r, &result);
}

/* ── Public API ─────────────────────────────────────────────── */

/* Verify a PKCS#1 v1.5 SHA-256 signature.
 *
 *   sig, sig_len: the raw signature bytes (typically 256 = RSA-2048).
 *   n,   n_len  : modulus, big-endian.
 *   e,   e_len  : public exponent, big-endian (typically 0x01 0x00 0x01).
 *   hash[32]    : the SHA-256 digest of the signed data.
 *
 * Returns 0 on success, -1 on any failure (bad length, padding,
 * digest mismatch). */
int rsa_pkcs1_v15_sha256_verify(const uint8_t *sig, uint32_t sig_len,
                                 const uint8_t *n, uint32_t n_len,
                                 const uint8_t *e, uint32_t e_len,
                                 const uint8_t hash[32])
{
    /* Step 1: integer ranges. */
    if (sig_len != n_len) return -1;
    if (n_len == 0 || n_len > BN_LIMBS * 8) return -1;
    if (e_len == 0 || e_len > 8)            return -1;

    bn_t S, N, M;
    if (bn_from_be(&S, sig, sig_len) < 0) return -1;
    if (bn_from_be(&N, n,   n_len)   < 0) return -1;

    uint64_t exp = 0;
    for (uint32_t i = 0; i < e_len; i++) exp = (exp << 8) | e[i];
    if (exp == 0) return -1;

    /* Step 2: S < N? */
    if (bn_cmp(&S, &N) >= 0) return -1;

    /* Step 3: M = S^e mod N. */
    bn_mod_pow_small_e(&M, &S, exp, &N);

    /* Step 4: serialise M as a sig_len-byte big-endian buffer.  This
     * is the EM (encoded message) we'd PKCS#1 v1.5 decode. */
    uint8_t em_full[BN_LIMBS * 8];
    bn_to_be(&M, em_full);
    /* The high (BN_LIMBS*8 - sig_len) bytes are MSB-side padding
     * zeros from the limb representation; the real EM is in the low
     * sig_len bytes. */
    const uint8_t *em = em_full + (BN_LIMBS * 8 - sig_len);

    /* Step 5: PKCS#1 v1.5 padding check.
     *
     *   EM = 0x00 || 0x01 || 0xFF…0xFF || 0x00 || T
     *
     * where T = ASN.1 DigestInfo(SHA-256, hash).  For SHA-256, T is
     * exactly 51 bytes:
     *
     *   30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20 || H(32)
     *
     * So sig_len = 2 + (k-3-51) + 1 + 51 = k.  For k=256 (RSA-2048),
     * PS = 0xFF × 202. */
    if (em[0] != 0x00 || em[1] != 0x01) return -1;
    static const uint8_t sha256_digestinfo_prefix[19] = {
        0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,
        0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
    };
    /* T occupies the last 51 bytes; PS occupies em[2..sig_len-52]. */
    uint32_t t_offset = sig_len - 51;
    if (t_offset < 11) return -1;          /* RFC 8017 §9.2 (PS ≥ 8) */
    /* PS check: all 0xFF, then 0x00 separator. */
    for (uint32_t i = 2; i < t_offset - 1; i++)
        if (em[i] != 0xFF) return -1;
    if (em[t_offset - 1] != 0x00) return -1;
    /* DigestInfo prefix check. */
    for (uint32_t i = 0; i < 19; i++)
        if (em[t_offset + i] != sha256_digestinfo_prefix[i]) return -1;
    /* Hash check (the actual point of all this). */
    uint8_t diff = 0;
    for (uint32_t i = 0; i < 32; i++)
        diff |= (uint8_t)(em[t_offset + 19 + i] ^ hash[i]);
    if (diff != 0) return -1;

    return 0;
}

/* ── Self-test (RFC 8017 / NIST CAVP-style fixed vector) ─────────
 *
 * Vector source: a known-good RSA-2048 SHA-256 sig generated by
 * openssl for the message "abc".  Hash("abc") = ba7816bf... 9b934ca4
 * 95991b78 52b85515.  We hard-code n, e, and the signature here so
 * the self-test catches a bigint regression without pulling in
 * /dev/urandom or external infrastructure.
 *
 * Returns 0 on PASS, -1 on FAIL. */
int rsa_self_test(void)
{
    /* OpenSSL-generated key + signature.  Public values only — nothing
     * secret in the kernel image.  Generated 2026-05-16 as:
     *   openssl genrsa 2048 → priv.pem
     *   printf abc | openssl dgst -sha256 -sign priv.pem > sig.bin
     *   openssl rsa -in priv.pem -modulus -noout  → n
     * Verified: openssl dgst -sha256 -verify pub.pem ... → "Verified OK". */
    static const uint8_t n[256] = {
        0xc0,0xeb,0x59,0x3b,0x67,0x28,0xb7,0x10,0xe1,0x5e,0x9d,0x00,
        0x66,0x8e,0xd5,0x4e,0xa6,0xe2,0x4e,0xcd,0xd7,0x8a,0xdc,0x81,
        0x01,0x9d,0x52,0x96,0x57,0x30,0xbf,0xf4,0x69,0xd9,0x41,0xe6,
        0xe3,0xe0,0x19,0x64,0x98,0xe3,0xa1,0xd4,0x85,0xa1,0xa8,0xab,
        0x3f,0x06,0x3e,0xde,0xab,0x9d,0x4e,0xa1,0xf3,0x2a,0x13,0x16,
        0xcf,0x2d,0x7c,0x17,0x97,0xdd,0x96,0x16,0xb1,0x65,0x16,0x29,
        0xaa,0xa3,0x15,0x12,0xf3,0x31,0xd9,0x4d,0x37,0x28,0xcb,0x84,
        0x98,0xcf,0x34,0x47,0xeb,0x6a,0xa3,0x0c,0x23,0x2a,0xe1,0xb0,
        0xd5,0xf8,0x75,0xc7,0xbf,0xf8,0x64,0x74,0x0a,0xb0,0x23,0xaf,
        0x42,0xe1,0x75,0x76,0x3a,0x00,0xa9,0x1d,0x80,0x36,0x4e,0x99,
        0x80,0x8d,0xa6,0xdb,0x7b,0x8b,0xf7,0xd9,0x95,0x4a,0xa7,0xdb,
        0x5e,0xd8,0xee,0x71,0x79,0x59,0x66,0x97,0x36,0xaf,0xb1,0x36,
        0x56,0xa0,0x5e,0x47,0x3c,0x00,0xd3,0xa1,0x28,0x9c,0xf4,0x06,
        0x44,0x61,0x7a,0x10,0xe7,0x3e,0x89,0xb0,0x1e,0x68,0x2b,0xbb,
        0xc6,0x0f,0x11,0x25,0xba,0x5c,0xd5,0x92,0x7c,0xbc,0x9c,0xda,
        0xea,0xb3,0x66,0x54,0x57,0x26,0x92,0xfb,0xc5,0x5f,0x78,0x55,
        0x21,0x70,0xd7,0x16,0x17,0x3f,0xe9,0xff,0xc8,0xd4,0xc2,0xfd,
        0x6f,0x7a,0x36,0x69,0x3a,0x9e,0x2a,0xf1,0xdc,0xe1,0x03,0xae,
        0xa0,0x91,0x15,0x81,0xd5,0x8d,0xf3,0xeb,0x4d,0x61,0xda,0x66,
        0x9a,0xcd,0xc0,0x7e,0x4d,0x20,0x8a,0xff,0x2e,0x63,0xc9,0x22,
        0xfa,0x00,0xc7,0x46,0xea,0x5a,0xb6,0x04,0xd4,0xb0,0xbd,0xe9,
        0xca,0xac,0xef,0x59,
    };
    static const uint8_t e[3] = { 0x01, 0x00, 0x01 };

    /* Signature over SHA-256("abc"). */
    static const uint8_t sig[256] = {
        0x77,0x1e,0xc4,0x16,0x2c,0x44,0x3f,0x0c,0x0a,0x9f,0x3b,0x86,
        0x7f,0x81,0x1c,0x76,0xe3,0xf8,0x84,0xc2,0x50,0x0c,0xc4,0xe1,
        0x83,0x52,0xa1,0xc4,0x1a,0x90,0xbc,0x00,0x6b,0x0f,0xa9,0x2a,
        0x73,0x91,0xdc,0x92,0xc3,0xd4,0x5d,0x7e,0x27,0xf4,0x9e,0xf0,
        0x0f,0x41,0xe6,0x47,0xc5,0xff,0xfa,0xd4,0x0e,0x3b,0x9c,0x8c,
        0xa6,0x30,0x6f,0x3e,0xd6,0xe2,0xad,0x6c,0xc1,0x5b,0xbc,0x79,
        0x1b,0x0a,0x58,0x2a,0x6e,0x11,0x77,0xb9,0x8f,0xed,0x02,0xd1,
        0x8e,0x16,0x7c,0x8c,0x20,0x31,0x32,0xf0,0x26,0x13,0x79,0x8c,
        0xbc,0xef,0x9f,0x4b,0x5d,0xbc,0x6c,0x2f,0x98,0xb5,0x8f,0x3f,
        0xc0,0xe0,0xa4,0x14,0xf9,0x2d,0x82,0x84,0x5f,0x33,0xfc,0x13,
        0x4b,0x06,0x0f,0x80,0xc1,0x2c,0x7e,0x39,0x23,0x90,0x6e,0x39,
        0x61,0x3b,0x9c,0x68,0x19,0x58,0xee,0xc1,0xea,0xb1,0x78,0x9e,
        0x74,0x02,0x8a,0x28,0x42,0x53,0xd4,0xba,0x2f,0x3a,0x82,0x64,
        0x25,0x6b,0x3b,0xc7,0x50,0x54,0x80,0xd5,0x94,0xad,0x76,0xaa,
        0xf5,0x0e,0xd5,0x8c,0xa3,0x97,0xae,0xd8,0x71,0xbe,0xd2,0x60,
        0xa0,0x1a,0x71,0xec,0x67,0xeb,0x42,0xa7,0xd7,0x34,0x8d,0xd5,
        0xcc,0x73,0x5d,0x54,0x25,0x1c,0x4a,0xbc,0x73,0xa6,0xe9,0x42,
        0xa1,0xc5,0x47,0x71,0x07,0x05,0x06,0xc7,0x8d,0x14,0x5c,0xa7,
        0x2a,0xf0,0xd9,0xe7,0xc9,0x32,0xa5,0x21,0xbe,0xd1,0x98,0x36,
        0x42,0x01,0x82,0x02,0x43,0x9a,0xe5,0x08,0xb9,0xea,0x5d,0xd8,
        0x35,0x0b,0xe6,0xd7,0xa2,0x38,0x38,0xc2,0xfd,0x62,0x61,0x2b,
        0x61,0x1f,0xdf,0x68,
    };
    /* SHA-256("abc"). */
    static const uint8_t hash_abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
    };

    /* Run verify.  This vector was generated against the n above —
     * if the bigint maths or padding check has a regression, this
     * test surfaces it cleanly at boot. */
    int rc = rsa_pkcs1_v15_sha256_verify(sig, 256, n, 256, e, 3, hash_abc);

    /* Negative test: flip a single bit in the hash and re-verify;
     * MUST fail. */
    uint8_t bad_hash[32];
    for (int i = 0; i < 32; i++) bad_hash[i] = hash_abc[i];
    bad_hash[0] ^= 0x01;
    int rc_neg = rsa_pkcs1_v15_sha256_verify(sig, 256, n, 256, e, 3, bad_hash);

    if (rc == 0 && rc_neg != 0) return 0;
    return -1;
}
