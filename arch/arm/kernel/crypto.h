/*
 * OsitoK x86-64 — Minimal Crypto Primitives
 *
 * SHA-256, HMAC-SHA-256, AES-128-GCM, X25519.
 * For TLS 1.2 client (ECDHE-RSA-AES128-GCM-SHA256).
 */

#ifndef OSITOK_CRYPTO_H
#define OSITOK_CRYPTO_H

#include "../include/types.h"

/* ── SHA-1 ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t state[5];
    uint64_t count;          /* Total bytes processed */
    uint8_t  buf[64];
    uint32_t buf_len;
} sha1_ctx;

void sha1_init(sha1_ctx *ctx);
void sha1_update(sha1_ctx *ctx, const void *data, uint32_t len);
void sha1_final(sha1_ctx *ctx, uint8_t digest[20]);

/* One-shot */
void sha1(const void *data, uint32_t len, uint8_t digest[20]);

/* ── SHA-256 ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t state[8];
    uint64_t count;          /* Total bytes processed */
    uint8_t  buf[64];
    uint32_t buf_len;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, uint32_t len);
void sha256_final(sha256_ctx *ctx, uint8_t digest[32]);

/* One-shot */
void sha256(const void *data, uint32_t len, uint8_t digest[32]);

/* ── HMAC-SHA-256 ────────────────────────────────────────────── */

void hmac_sha256(const void *key, uint32_t key_len,
                 const void *data, uint32_t data_len,
                 uint8_t mac[32]);

/* ── AES-128 ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t rk[44];         /* Round keys (11 × 4 words) */
} aes128_ctx;

void aes128_init(aes128_ctx *ctx, const uint8_t key[16]);
void aes128_encrypt_block(const aes128_ctx *ctx,
                           const uint8_t in[16], uint8_t out[16]);

/* ── AES-128-GCM ─────────────────────────────────────────────── */

/* Encrypt + authenticate. Returns 0 on success.
 * iv: 12 bytes (explicit nonce).
 * aad: additional authenticated data.
 * out: ciphertext (same length as plaintext).
 * tag: 16-byte authentication tag. */
int aes128_gcm_encrypt(const uint8_t key[16],
                        const uint8_t iv[12],
                        const void *aad, uint32_t aad_len,
                        const void *plaintext, uint32_t pt_len,
                        void *ciphertext,
                        uint8_t tag[16]);

/* Decrypt + verify. Returns 0 on success, -1 on tag mismatch. */
int aes128_gcm_decrypt(const uint8_t key[16],
                        const uint8_t iv[12],
                        const void *aad, uint32_t aad_len,
                        const void *ciphertext, uint32_t ct_len,
                        void *plaintext,
                        const uint8_t tag[16]);

/* ── X25519 (ECDHE) ──────────────────────────────────────────── */

/* Generate keypair. private_key should be 32 random bytes.
 * Clamps private_key in-place, computes public_key. */
void x25519_public(uint8_t public_key[32], const uint8_t private_key[32]);

/* Compute shared secret: result = scalar * point */
void x25519(uint8_t result[32], const uint8_t scalar[32],
            const uint8_t point[32]);

/* ── TLS PRF (SHA-256) ───────────────────────────────────────── */

/* TLS 1.2 PRF: P_SHA256(secret, label || seed) */
void tls_prf_sha256(const void *secret, uint32_t secret_len,
                     const char *label,
                     const void *seed, uint32_t seed_len,
                     void *output, uint32_t output_len);

/* ── Self-test ──────────────────────────────────────────────── */

/* Returns 0 on success, -1 on failure. Tests SHA-256, HMAC, AES-GCM. */
int crypto_selftest(void);

#endif /* OSITOK_CRYPTO_H */
