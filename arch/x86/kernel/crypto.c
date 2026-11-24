/*
 * OsitoK x86-64 — Crypto Primitives
 *
 * SHA-256, HMAC-SHA-256, AES-128-GCM, X25519, TLS PRF.
 * Minimal implementations for TLS 1.2 client.
 * No side-channel protection (acceptable for bare-metal client).
 */

#include "crypto.h"

/* ── Helpers ─────────────────────────────────────────────────── */

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static inline void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void *cmemcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void cmemset(void *dst, int c, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)c;
}

/* ══════════════════════════════════════════════════════════════
 *  SHA-256 (FIPS 180-4)
 * ══════════════════════════════════════════════════════════════ */

static const uint32_t sha256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_transform(sha256_ctx *ctx, const uint8_t block[64])
{
    uint32_t W[64], a, b, c, d, e, f, g, h;

    for (int i = 0; i < 16; i++)
        W[i] = be32(block + i * 4);

    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(W[i-15], 7) ^ rotr32(W[i-15], 18) ^ (W[i-15] >> 3);
        uint32_t s1 = rotr32(W[i-2], 17) ^ rotr32(W[i-2], 19) ^ (W[i-2] >> 10);
        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_K[i] + W[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
    ctx->buf_len = 0;
}

void sha256_update(sha256_ctx *ctx, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len > 0) {
        uint32_t space = 64 - ctx->buf_len;
        uint32_t take = len < space ? len : space;

        cmemcpy(ctx->buf + ctx->buf_len, p, take);
        ctx->buf_len += take;
        p += take;
        len -= take;

        if (ctx->buf_len == 64) {
            sha256_transform(ctx, ctx->buf);
            ctx->count += 64;
            ctx->buf_len = 0;
        }
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t digest[32])
{
    uint64_t total_bits = (ctx->count + ctx->buf_len) * 8;

    /* Pad: 0x80, zeros, 8-byte big-endian bit count */
    ctx->buf[ctx->buf_len++] = 0x80;

    if (ctx->buf_len > 56) {
        cmemset(ctx->buf + ctx->buf_len, 0, 64 - ctx->buf_len);
        sha256_transform(ctx, ctx->buf);
        ctx->buf_len = 0;
    }

    cmemset(ctx->buf + ctx->buf_len, 0, 56 - ctx->buf_len);
    put_be64(ctx->buf + 56, total_bits);
    sha256_transform(ctx, ctx->buf);

    for (int i = 0; i < 8; i++)
        put_be32(digest + i * 4, ctx->state[i]);
}

void sha256(const void *data, uint32_t len, uint8_t digest[32])
{
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

/* ══════════════════════════════════════════════════════════════
 *  SHA-1 (FIPS 180-1) — for git object hashing
 * ══════════════════════════════════════════════════════════════ */

static inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_transform(sha1_ctx *ctx, const uint8_t block[64])
{
    uint32_t W[80], a, b, c, d, e;

    for (int i = 0; i < 16; i++)
        W[i] = be32(block + i * 4);
    for (int i = 16; i < 80; i++)
        W[i] = rotl32(W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16], 1);

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2];
    d = ctx->state[3]; e = ctx->state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);  k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;              k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;              k = 0xCA62C1D6;
        }
        uint32_t temp = rotl32(a, 5) + f + e + k + W[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = temp;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e;
}

void sha1_init(sha1_ctx *ctx)
{
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE; ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
    ctx->buf_len = 0;
}

void sha1_update(sha1_ctx *ctx, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        uint32_t space = 64 - ctx->buf_len;
        uint32_t take = len < space ? len : space;
        cmemcpy(ctx->buf + ctx->buf_len, p, take);
        ctx->buf_len += take;
        p += take;
        len -= take;
        if (ctx->buf_len == 64) {
            sha1_transform(ctx, ctx->buf);
            ctx->count += 64;
            ctx->buf_len = 0;
        }
    }
}

void sha1_final(sha1_ctx *ctx, uint8_t digest[20])
{
    uint64_t total_bits = (ctx->count + ctx->buf_len) * 8;
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 56) {
        cmemset(ctx->buf + ctx->buf_len, 0, 64 - ctx->buf_len);
        sha1_transform(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    cmemset(ctx->buf + ctx->buf_len, 0, 56 - ctx->buf_len);
    put_be64(ctx->buf + 56, total_bits);
    sha1_transform(ctx, ctx->buf);

    for (int i = 0; i < 5; i++)
        put_be32(digest + i * 4, ctx->state[i]);
}

void sha1(const void *data, uint32_t len, uint8_t digest[20])
{
    sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
}

/* ══════════════════════════════════════════════════════════════
 *  HMAC-SHA-256 (RFC 2104)
 * ══════════════════════════════════════════════════════════════ */

void hmac_sha256(const void *key, uint32_t key_len,
                 const void *data, uint32_t data_len,
                 uint8_t mac[32])
{
    uint8_t k_pad[64];
    uint8_t tk[32];
    sha256_ctx ctx;

    /* If key > 64 bytes, hash it first */
    if (key_len > 64) {
        sha256(key, key_len, tk);
        key = tk;
        key_len = 32;
    }

    /* ipad = key ^ 0x36 */
    cmemset(k_pad, 0x36, 64);
    for (uint32_t i = 0; i < key_len; i++)
        k_pad[i] ^= ((const uint8_t *)key)[i];

    sha256_init(&ctx);
    sha256_update(&ctx, k_pad, 64);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, mac);

    /* opad = key ^ 0x5c */
    cmemset(k_pad, 0x5c, 64);
    for (uint32_t i = 0; i < key_len; i++)
        k_pad[i] ^= ((const uint8_t *)key)[i];

    sha256_init(&ctx);
    sha256_update(&ctx, k_pad, 64);
    sha256_update(&ctx, mac, 32);
    sha256_final(&ctx, mac);
}

/* ══════════════════════════════════════════════════════════════
 *  TLS PRF (SHA-256 based, RFC 5246 Section 5)
 * ══════════════════════════════════════════════════════════════ */

void tls_prf_sha256(const void *secret, uint32_t secret_len,
                     const char *label,
                     const void *seed, uint32_t seed_len,
                     void *output, uint32_t output_len)
{
    /* label_seed = label || seed */
    uint32_t label_len = 0;
    while (label[label_len]) label_len++;

    uint8_t label_seed[128];
    if (label_len + seed_len > sizeof(label_seed))
        return;  /* Too long */
    cmemcpy(label_seed, label, label_len);
    cmemcpy(label_seed + label_len, seed, seed_len);
    uint32_t ls_len = label_len + seed_len;

    /* A(0) = label_seed
     * A(i) = HMAC(secret, A(i-1))
     * P(i) = HMAC(secret, A(i) || label_seed) */
    uint8_t A[32];
    hmac_sha256(secret, secret_len, label_seed, ls_len, A);

    uint8_t *out = (uint8_t *)output;
    uint32_t remaining = output_len;

    while (remaining > 0) {
        /* P = HMAC(secret, A || label_seed) */
        uint8_t a_ls[32 + 128];
        cmemcpy(a_ls, A, 32);
        cmemcpy(a_ls + 32, label_seed, ls_len);

        uint8_t P[32];
        hmac_sha256(secret, secret_len, a_ls, 32 + ls_len, P);

        uint32_t take = remaining < 32 ? remaining : 32;
        cmemcpy(out, P, take);
        out += take;
        remaining -= take;

        /* A(i+1) = HMAC(secret, A(i)) */
        hmac_sha256(secret, secret_len, A, 32, A);
    }
}

/* ══════════════════════════════════════════════════════════════
 *  AES-128 (FIPS 197)
 * ══════════════════════════════════════════════════════════════ */

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const uint8_t aes_rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
};

static uint32_t sub_word(uint32_t w)
{
    return ((uint32_t)aes_sbox[(w >> 24) & 0xFF] << 24) |
           ((uint32_t)aes_sbox[(w >> 16) & 0xFF] << 16) |
           ((uint32_t)aes_sbox[(w >> 8) & 0xFF] << 8) |
           (uint32_t)aes_sbox[w & 0xFF];
}

static uint32_t rot_word(uint32_t w)
{
    return (w << 8) | (w >> 24);
}

void aes128_init(aes128_ctx *ctx, const uint8_t key[16])
{
    /* First 4 words from key */
    for (int i = 0; i < 4; i++)
        ctx->rk[i] = be32(key + i * 4);

    /* Key expansion */
    for (int i = 4; i < 44; i++) {
        uint32_t temp = ctx->rk[i - 1];
        if ((i & 3) == 0)
            temp = sub_word(rot_word(temp)) ^ ((uint32_t)aes_rcon[i / 4] << 24);
        ctx->rk[i] = ctx->rk[i - 4] ^ temp;
    }
}

void aes128_encrypt_block(const aes128_ctx *ctx,
                           const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    cmemcpy(s, in, 16);

    /* AddRoundKey (round 0) */
    for (int i = 0; i < 4; i++) {
        uint32_t rk = ctx->rk[i];
        s[i*4+0] ^= (uint8_t)(rk >> 24);
        s[i*4+1] ^= (uint8_t)(rk >> 16);
        s[i*4+2] ^= (uint8_t)(rk >> 8);
        s[i*4+3] ^= (uint8_t)rk;
    }

    for (int round = 1; round <= 10; round++) {
        uint8_t t[16];

        /* SubBytes */
        for (int i = 0; i < 16; i++)
            t[i] = aes_sbox[s[i]];

        /* ShiftRows */
        s[0]  = t[0];  s[1]  = t[5];  s[2]  = t[10]; s[3]  = t[15];
        s[4]  = t[4];  s[5]  = t[9];  s[6]  = t[14]; s[7]  = t[3];
        s[8]  = t[8];  s[9]  = t[13]; s[10] = t[2];  s[11] = t[7];
        s[12] = t[12]; s[13] = t[1];  s[14] = t[6];  s[15] = t[11];

        /* MixColumns (skip in last round) */
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[c*4], a1 = s[c*4+1], a2 = s[c*4+2], a3 = s[c*4+3];
                #define xtime(x) (((x)<<1) ^ ((((x)>>7)&1) * 0x1b))
                s[c*4+0] = xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3;
                s[c*4+1] = a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3;
                s[c*4+2] = a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3;
                s[c*4+3] = xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3);
                #undef xtime
            }
        }

        /* AddRoundKey */
        for (int i = 0; i < 4; i++) {
            uint32_t rk = ctx->rk[round * 4 + i];
            s[i*4+0] ^= (uint8_t)(rk >> 24);
            s[i*4+1] ^= (uint8_t)(rk >> 16);
            s[i*4+2] ^= (uint8_t)(rk >> 8);
            s[i*4+3] ^= (uint8_t)rk;
        }
    }

    cmemcpy(out, s, 16);
}

/* ══════════════════════════════════════════════════════════════
 *  AES-128-GCM (NIST SP 800-38D)
 * ══════════════════════════════════════════════════════════════ */

/* GF(2^128) multiplication for GHASH */
static void ghash_mult(const uint8_t X[16], const uint8_t H[16], uint8_t out[16])
{
    uint8_t V[16], Z[16];
    cmemcpy(V, H, 16);
    cmemset(Z, 0, 16);

    for (int i = 0; i < 128; i++) {
        /* If bit i of X is set, Z ^= V */
        if (X[i / 8] & (0x80 >> (i & 7))) {
            for (int j = 0; j < 16; j++)
                Z[j] ^= V[j];
        }
        /* V = V >> 1 in GF(2^128), with reduction polynomial x^128 + x^7 + x^2 + x + 1 */
        uint8_t carry = V[15] & 1;
        for (int j = 15; j > 0; j--)
            V[j] = (V[j] >> 1) | (V[j-1] << 7);
        V[0] >>= 1;
        if (carry)
            V[0] ^= 0xE1;  /* x^7 + x^2 + x + 1 = 0xE1 in the MSB position */
    }

    cmemcpy(out, Z, 16);
}

/* GHASH: iterative multiply-accumulate */
static void ghash(const uint8_t H[16], const void *data, uint32_t len,
                   uint8_t tag[16])
{
    const uint8_t *p = (const uint8_t *)data;

    while (len >= 16) {
        for (int i = 0; i < 16; i++)
            tag[i] ^= p[i];
        ghash_mult(tag, H, tag);
        p += 16;
        len -= 16;
    }

    if (len > 0) {
        uint8_t block[16];
        cmemset(block, 0, 16);
        cmemcpy(block, p, len);
        for (int i = 0; i < 16; i++)
            tag[i] ^= block[i];
        ghash_mult(tag, H, tag);
    }
}

/* Increment 32-bit counter (last 4 bytes of 16-byte block) */
static void gcm_inc32(uint8_t ctr[16])
{
    for (int i = 15; i >= 12; i--) {
        if (++ctr[i] != 0)
            break;
    }
}

int aes128_gcm_encrypt(const uint8_t key[16],
                        const uint8_t iv[12],
                        const void *aad, uint32_t aad_len,
                        const void *plaintext, uint32_t pt_len,
                        void *ciphertext,
                        uint8_t tag[16])
{
    aes128_ctx aes;
    aes128_init(&aes, key);

    /* H = AES(K, 0^128) */
    uint8_t H[16], zero[16];
    cmemset(zero, 0, 16);
    aes128_encrypt_block(&aes, zero, H);

    /* J0 = IV || 0x00000001 */
    uint8_t J0[16];
    cmemcpy(J0, iv, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    /* Counter starts at J0 + 1 for encryption */
    uint8_t ctr[16];
    cmemcpy(ctr, J0, 16);

    /* Encrypt plaintext with CTR mode */
    const uint8_t *pt = (const uint8_t *)plaintext;
    uint8_t *ct = (uint8_t *)ciphertext;
    uint32_t remaining = pt_len;

    while (remaining > 0) {
        gcm_inc32(ctr);
        uint8_t keystream[16];
        aes128_encrypt_block(&aes, ctr, keystream);

        uint32_t chunk = remaining < 16 ? remaining : 16;
        for (uint32_t i = 0; i < chunk; i++)
            ct[i] = pt[i] ^ keystream[i];

        pt += chunk;
        ct += chunk;
        remaining -= chunk;
    }

    /* GHASH(H, AAD || CT || len(AAD) || len(CT)) */
    cmemset(tag, 0, 16);
    ghash(H, aad, aad_len, tag);
    ghash(H, ciphertext, pt_len, tag);

    /* Lengths block: bits of AAD and CT, big-endian 64-bit */
    uint8_t len_block[16];
    put_be64(len_block, (uint64_t)aad_len * 8);
    put_be64(len_block + 8, (uint64_t)pt_len * 8);
    for (int i = 0; i < 16; i++)
        tag[i] ^= len_block[i];
    ghash_mult(tag, H, tag);

    /* Tag = GHASH ^ AES(K, J0) */
    uint8_t S[16];
    aes128_encrypt_block(&aes, J0, S);
    for (int i = 0; i < 16; i++)
        tag[i] ^= S[i];

    return 0;
}

int aes128_gcm_decrypt(const uint8_t key[16],
                        const uint8_t iv[12],
                        const void *aad, uint32_t aad_len,
                        const void *ciphertext, uint32_t ct_len,
                        void *plaintext,
                        const uint8_t tag[16])
{
    /* Compute expected tag */
    uint8_t computed_tag[16];

    /* First decrypt (GCM decryption = same CTR operation) */
    aes128_ctx aes;
    aes128_init(&aes, key);

    uint8_t H[16], zero[16];
    cmemset(zero, 0, 16);
    aes128_encrypt_block(&aes, zero, H);

    uint8_t J0[16];
    cmemcpy(J0, iv, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    uint8_t ctr[16];
    cmemcpy(ctr, J0, 16);

    const uint8_t *ct = (const uint8_t *)ciphertext;
    uint8_t *pt = (uint8_t *)plaintext;
    uint32_t remaining = ct_len;

    while (remaining > 0) {
        gcm_inc32(ctr);
        uint8_t keystream[16];
        aes128_encrypt_block(&aes, ctr, keystream);

        uint32_t chunk = remaining < 16 ? remaining : 16;
        for (uint32_t i = 0; i < chunk; i++)
            pt[i] = ct[i] ^ keystream[i];

        ct += chunk;
        pt += chunk;
        remaining -= chunk;
    }

    /* Compute GHASH over AAD and ciphertext (not plaintext!) */
    cmemset(computed_tag, 0, 16);
    ghash(H, aad, aad_len, computed_tag);
    ghash(H, ciphertext, ct_len, computed_tag);

    uint8_t len_block[16];
    put_be64(len_block, (uint64_t)aad_len * 8);
    put_be64(len_block + 8, (uint64_t)ct_len * 8);
    for (int i = 0; i < 16; i++)
        computed_tag[i] ^= len_block[i];
    ghash_mult(computed_tag, H, computed_tag);

    uint8_t S[16];
    aes128_encrypt_block(&aes, J0, S);
    for (int i = 0; i < 16; i++)
        computed_tag[i] ^= S[i];

    /* Verify tag (constant-time compare) */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= computed_tag[i] ^ tag[i];

    return diff ? -1 : 0;
}

/* ══════════════════════════════════════════════════════════════
 *  X25519 (RFC 7748)
 *  Adapted from TweetNaCl approach — field elements as 16 limbs
 * ══════════════════════════════════════════════════════════════ */

typedef int64_t fe[16];  /* Field element: 16 × 16-bit limbs */

static void fe_set(fe o, int64_t v)
{
    o[0] = v;
    for (int i = 1; i < 16; i++) o[i] = 0;
}

static void fe_copy(fe o, const fe a)
{
    for (int i = 0; i < 16; i++) o[i] = a[i];
}

static void fe_add(fe o, const fe a, const fe b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void fe_sub(fe o, const fe a, const fe b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void fe_mul(fe o, const fe a, const fe b)
{
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];

    /* Reduce mod 2^255 - 19: t[16..30] fold back with factor 38 */
    for (int i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];
    /* t[15] doesn't get a fold from t[31] because it doesn't exist */

    /* Carry propagation */
    for (int i = 0; i < 16; i++)
        o[i] = t[i];

    /* Two rounds of carry */
    for (int round = 0; round < 2; round++) {
        int64_t carry = 0;
        for (int i = 0; i < 16; i++) {
            o[i] += carry;
            carry = o[i] >> 16;
            o[i] &= 0xFFFF;
        }
        o[0] += carry * 38;
    }
}

static void fe_sq(fe o, const fe a) { fe_mul(o, a, a); }

/* Fermat inverse: a^(p-2) where p = 2^255 - 19 */
static void fe_inv(fe o, const fe a)
{
    fe c;
    fe_copy(c, a);

    /* a^(2^255 - 21) via square-and-multiply */
    for (int i = 253; i >= 0; i--) {
        fe_sq(c, c);
        if (i != 2 && i != 4)
            fe_mul(c, c, a);
    }
    fe_copy(o, c);
}

static void fe_pack(uint8_t out[32], const fe n)
{
    fe t, m;
    fe_copy(t, n);

    /* Three rounds of carry normalization */
    for (int round = 0; round < 3; round++) {
        int64_t carry = 0;
        for (int i = 0; i < 16; i++) {
            t[i] += carry;
            carry = t[i] >> 16;
            t[i] &= 0xFFFF;
        }
        t[0] += carry * 38;
    }

    /* Conditional subtraction of p = 2^255 - 19 (twice for full reduction).
     * p in limbs: [0xFFED, 0xFFFF, ..., 0xFFFF, 0x7FFF] */
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xFFED;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xFFFF - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xFFFF;
        }
        m[15] = t[15] - 0x7FFF - ((m[14] >> 16) & 1);
        m[14] &= 0xFFFF;
        int64_t b = (m[15] >> 16) & 1; /* borrow: 1 if t < p */
        m[15] &= 0xFFFF;

        /* Select: if no borrow (b==0), use m; else keep t */
        for (int i = 0; i < 16; i++) {
            t[i] = t[i] ^ ((t[i] ^ m[i]) & ~(-b));
        }
    }

    for (int i = 0; i < 16; i++) {
        out[2 * i]     = (uint8_t)(t[i] & 0xFF);
        out[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void fe_unpack(fe o, const uint8_t in[32])
{
    for (int i = 0; i < 16; i++)
        o[i] = (int64_t)in[2 * i] | ((int64_t)in[2 * i + 1] << 8);
    o[15] &= 0x7FFF;  /* Clear top bit */
}

void x25519(uint8_t result[32], const uint8_t scalar[32],
            const uint8_t point[32])
{
    uint8_t e[32];
    cmemcpy(e, scalar, 32);

    /* Clamp */
    e[0] &= 0xF8;
    e[31] = (e[31] & 0x7F) | 0x40;

    fe x1, x2, x3, z2, z3, tmp0, tmp1;
    fe_unpack(x1, point);
    fe_set(x2, 1);
    fe_set(z2, 0);
    fe_copy(x3, x1);
    fe_set(z3, 1);

    int swap = 0;

    for (int pos = 254; pos >= 0; pos--) {
        int bit = (e[pos / 8] >> (pos & 7)) & 1;
        swap ^= bit;

        /* Conditional swap */
        for (int i = 0; i < 16; i++) {
            int64_t d;
            d = swap * (x2[i] - x3[i]); x2[i] -= d; x3[i] += d;
            d = swap * (z2[i] - z3[i]); z2[i] -= d; z3[i] += d;
        }
        swap = bit;

        /* Montgomery ladder step */
        fe a, b, aa, bb, e_fe, c, d, da, cb;
        fe_add(a, x2, z2);
        fe_sq(aa, a);
        fe_sub(b, x2, z2);
        fe_sq(bb, b);
        fe_sub(e_fe, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);

        fe_add(tmp0, da, cb);
        fe_sq(x3, tmp0);
        fe_sub(tmp0, da, cb);
        fe_sq(tmp1, tmp0);
        fe_mul(z3, tmp1, x1);
        fe_mul(x2, aa, bb);

        fe a121665;
        fe_set(a121665, 121665);
        fe_mul(tmp0, a121665, e_fe);
        fe_add(tmp0, tmp0, aa);
        fe_mul(z2, e_fe, tmp0);
    }

    /* Final conditional swap */
    for (int i = 0; i < 16; i++) {
        int64_t d;
        d = swap * (x2[i] - x3[i]); x2[i] -= d; x3[i] += d;
        d = swap * (z2[i] - z3[i]); z2[i] -= d; z3[i] += d;
    }

    /* result = x2 / z2 */
    fe_inv(tmp0, z2);
    fe_mul(tmp1, x2, tmp0);
    fe_pack(result, tmp1);
}

void x25519_public(uint8_t public_key[32], const uint8_t private_key[32])
{
    /* Base point = 9 */
    static const uint8_t base[32] = { 9 };
    x25519(public_key, private_key, base);
}

/* ══════════════════════════════════════════════════════════════
 *  Self-test
 * ══════════════════════════════════════════════════════════════ */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

int crypto_selftest(void)
{
    int fail = 0;

    /* SHA-256("abc") = ba7816bf 8f01cfea 414140de 5dae2223
     *                  b00361a3 96177a9c b410ff61 f20015ad */
    {
        uint8_t digest[32];
        sha256("abc", 3, digest);

        static const uint8_t expected[32] = {
            0xba,0x78,0x16,0xbf, 0x8f,0x01,0xcf,0xea,
            0x41,0x41,0x40,0xde, 0x5d,0xae,0x22,0x23,
            0xb0,0x03,0x61,0xa3, 0x96,0x17,0x7a,0x9c,
            0xb4,0x10,0xff,0x61, 0xf2,0x00,0x15,0xad,
        };

        int ok = 1;
        for (int i = 0; i < 32; i++)
            if (digest[i] != expected[i]) { ok = 0; break; }

        serial_puts("[CRYPTO] SHA-256(\"abc\"): ");
        if (ok) {
            serial_puts("OK\n");
        } else {
            serial_puts("FAIL got=");
            for (int i = 0; i < 32; i++)
                serial_puthex(digest[i], 2);
            serial_puts("\n");
            fail = -1;
        }
    }

    /* HMAC-SHA-256 test vector (RFC 4231 Test Case 2):
     * Key  = "Jefe"
     * Data = "what do ya want for nothing?"
     * HMAC = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843 */
    {
        uint8_t mac[32];
        hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, mac);

        static const uint8_t expected[32] = {
            0x5b,0xdc,0xc1,0x46, 0xbf,0x60,0x75,0x4e,
            0x6a,0x04,0x24,0x26, 0x08,0x95,0x75,0xc7,
            0x5a,0x00,0x3f,0x08, 0x9d,0x27,0x39,0x83,
            0x9d,0xec,0x58,0xb9, 0x64,0xec,0x38,0x43,
        };

        int ok = 1;
        for (int i = 0; i < 32; i++)
            if (mac[i] != expected[i]) { ok = 0; break; }

        serial_puts("[CRYPTO] HMAC-SHA-256: ");
        if (ok) {
            serial_puts("OK\n");
        } else {
            serial_puts("FAIL got=");
            for (int i = 0; i < 32; i++)
                serial_puthex(mac[i], 2);
            serial_puts("\n");
            fail = -1;
        }
    }

    /* AES-128 ECB test vector (FIPS 197 Appendix B):
     * Key = 2b7e1516 28aed2a6 abf71588 09cf4f3c
     * PT  = 3243f6a8 885a308d 313198a2 e0370734
     * CT  = 3925841d 02dc09fb dc118597 196a0b32 */
    {
        static const uint8_t key[16] = {
            0x2b,0x7e,0x15,0x16, 0x28,0xae,0xd2,0xa6,
            0xab,0xf7,0x15,0x88, 0x09,0xcf,0x4f,0x3c
        };
        static const uint8_t pt[16] = {
            0x32,0x43,0xf6,0xa8, 0x88,0x5a,0x30,0x8d,
            0x31,0x31,0x98,0xa2, 0xe0,0x37,0x07,0x34
        };
        static const uint8_t expected[16] = {
            0x39,0x25,0x84,0x1d, 0x02,0xdc,0x09,0xfb,
            0xdc,0x11,0x85,0x97, 0x19,0x6a,0x0b,0x32
        };

        aes128_ctx aes;
        aes128_init(&aes, key);
        uint8_t ct[16];
        aes128_encrypt_block(&aes, pt, ct);

        int ok = 1;
        for (int i = 0; i < 16; i++)
            if (ct[i] != expected[i]) { ok = 0; break; }

        serial_puts("[CRYPTO] AES-128-ECB: ");
        if (ok) {
            serial_puts("OK\n");
        } else {
            serial_puts("FAIL got=");
            for (int i = 0; i < 16; i++)
                serial_puthex(ct[i], 2);
            serial_puts("\n");
            fail = -1;
        }
    }

    /* AES-128-GCM NIST test vector (Test Case 3 from SP 800-38D):
     * Key = 00000000...0 (16)
     * IV  = 00000000...0 (12)
     * PT  = (empty)
     * AAD = (empty)
     * CT  = (empty)
     * Tag = 58e2fccefa7e3061367f1d57a4e7455a */
    {
        static const uint8_t key[16] = {0};
        static const uint8_t iv[12] = {0};
        uint8_t tag[16];

        aes128_gcm_encrypt(key, iv, NULL, 0, NULL, 0, NULL, tag);

        static const uint8_t expected_tag[16] = {
            0x58,0xe2,0xfc,0xce,0xfa,0x7e,0x30,0x61,
            0x36,0x7f,0x1d,0x57,0xa4,0xe7,0x45,0x5a
        };

        int ok = 1;
        for (int i = 0; i < 16; i++)
            if (tag[i] != expected_tag[i]) { ok = 0; break; }

        serial_puts("[CRYPTO] AES-128-GCM (NIST): ");
        if (ok) {
            serial_puts("OK\n");
        } else {
            serial_puts("FAIL tag=");
            for (int i = 0; i < 16; i++)
                serial_puthex(tag[i], 2);
            serial_puts("\n");
            fail = -1;
        }
    }

    /* AES-128-GCM round-trip test */
    {
        static const uint8_t key[16] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
            0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
        };
        static const uint8_t iv[12] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
            0x08,0x09,0x0a,0x0b
        };
        static const uint8_t aad[] = "additional data";
        static const uint8_t pt[] = "hello, GCM world";

        uint8_t ct[16], tag[16], dec[16];
        aes128_gcm_encrypt(key, iv, aad, sizeof(aad)-1, pt, 16, ct, tag);
        int r = aes128_gcm_decrypt(key, iv, aad, sizeof(aad)-1, ct, 16, dec, tag);

        int ok = (r == 0);
        for (int i = 0; i < 16 && ok; i++)
            if (dec[i] != pt[i]) ok = 0;

        /* Tamper test: flip a bit in tag */
        uint8_t bad_tag[16];
        cmemcpy(bad_tag, tag, 16);
        bad_tag[0] ^= 1;
        int r2 = aes128_gcm_decrypt(key, iv, aad, sizeof(aad)-1, ct, 16, dec, bad_tag);
        if (r2 != -1) ok = 0;

        serial_puts("[CRYPTO] AES-128-GCM (roundtrip): ");
        serial_puts(ok ? "OK\n" : "FAIL\n");
        if (!ok) fail = -1;
    }

    /* X25519 test vector (RFC 7748 Section 6.1):
     * Alice's private key = 77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a
     * Alice's public key  = 8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a
     * Bob's private key   = 5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb
     * Bob's public key    = de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f
     * Shared secret       = 4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742 */
    {
        static const uint8_t alice_priv[32] = {
            0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,
            0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
            0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,
            0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a
        };
        static const uint8_t alice_pub_expected[32] = {
            0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,
            0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
            0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,
            0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a
        };
        static const uint8_t bob_pub[32] = {
            0xde,0x9e,0xdb,0x7d,0x7b,0x7d,0xc1,0xb4,
            0xd3,0x5b,0x61,0xc2,0xec,0xe4,0x35,0x37,
            0x3f,0x83,0x43,0xc8,0x5b,0x78,0x67,0x4d,
            0xad,0xfc,0x7e,0x14,0x6f,0x88,0x2b,0x4f
        };
        static const uint8_t shared_expected[32] = {
            0x4a,0x5d,0x9d,0x5b,0xa4,0xce,0x2d,0xe1,
            0x72,0x8e,0x3b,0xf4,0x80,0x35,0x0f,0x25,
            0xe0,0x7e,0x21,0xc9,0x47,0xd1,0x9e,0x33,
            0x76,0xf0,0x9b,0x3c,0x1e,0x16,0x17,0x42
        };

        /* Test 1: Public key derivation */
        uint8_t alice_pub[32];
        x25519_public(alice_pub, alice_priv);

        int ok1 = 1;
        for (int i = 0; i < 32; i++)
            if (alice_pub[i] != alice_pub_expected[i]) { ok1 = 0; break; }

        serial_puts("[CRYPTO] X25519 pubkey: ");
        if (ok1) {
            serial_puts("OK\n");
        } else {
            serial_puts("FAIL got=");
            for (int i = 0; i < 32; i++)
                serial_puthex(alice_pub[i], 2);
            serial_puts("\n");
            fail = -1;
        }

        /* Test 2: Shared secret */
        uint8_t shared[32];
        x25519(shared, alice_priv, bob_pub);

        int ok2 = 1;
        for (int i = 0; i < 32; i++)
            if (shared[i] != shared_expected[i]) { ok2 = 0; break; }

        serial_puts("[CRYPTO] X25519 shared: ");
        if (ok2) {
            serial_puts("OK\n");
        } else {
            serial_puts("FAIL got=");
            for (int i = 0; i < 32; i++)
                serial_puthex(shared[i], 2);
            serial_puts("\n");
            fail = -1;
        }
    }

    return fail;
}

/* SMP wrapper for boot-time parallel crypto selftest on AP */
void boot_crypto_worker(void *arg, void *result)
{
    (void)arg; (void)result;
    crypto_selftest();
}
