#include "wpa2_crypto.h"
#include "../lib/bearssl/inc/bearssl.h"
#include <string.h>

/* BearSSL 0.6 uses function-pointer casts in its generic hash vtable. Keep
 * the upstream primitives unchanged but call them through exact C prototypes. */
static const br_hash_class sha1_class;
static void hash_init(const br_hash_class **ctx)
{
    br_sha1_init((br_sha1_context *)ctx);
    *ctx = &sha1_class;
}
static void hash_update(const br_hash_class **ctx, const void *data, size_t n)
{
    br_sha1_update((br_sha1_context *)ctx, data, n);
}
static void hash_out(const br_hash_class *const *ctx, void *out)
{
    br_sha1_out((const br_sha1_context *)ctx, out);
}
static uint64_t hash_state(const br_hash_class *const *ctx, void *out)
{
    return br_sha1_state((const br_sha1_context *)ctx, out);
}
static void hash_set_state(const br_hash_class **ctx, const void *state, uint64_t count)
{
    br_sha1_set_state((br_sha1_context *)ctx, state, count);
}
static const br_hash_class sha1_class = {
    sizeof(br_sha1_context),
    BR_HASHDESC_ID(br_sha1_ID) | BR_HASHDESC_OUT(20) | BR_HASHDESC_STATE(20) |
        BR_HASHDESC_LBLEN(6) | BR_HASHDESC_MD_PADDING | BR_HASHDESC_MD_PADDING_BE,
    hash_init, hash_update, hash_out, hash_state, hash_set_state
};

void wpa2_clear(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

int wpa2_equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) diff |= x[i] ^ y[i];
    return diff == 0;
}

void wpa2_hmac_sha1(const void *key, size_t key_len,
                    const void *data, size_t data_len, uint8_t mac[20])
{
    br_hmac_key_context k;
    br_hmac_context c;
    br_hmac_key_init(&k, &sha1_class, key, key_len);
    br_hmac_init(&c, &k, 20);
    br_hmac_update(&c, data, data_len);
    br_hmac_out(&c, mac);
    wpa2_clear(&c, sizeof(c)); wpa2_clear(&k, sizeof(k));
}

int wpa2_pmk(const uint8_t *password, size_t password_len,
             const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32])
{
    if (!pmk) return -1;
    wpa2_clear(pmk, 32);
    if (!password || !ssid || password_len < 8 || password_len > 63 || !ssid_len || ssid_len > 32)
        return -1;
    for (size_t i = 0; i < password_len; i++) if (password[i] < 32 || password[i] > 126) return -1;
    br_hmac_key_context k;
    br_hmac_context c;
    uint8_t u[20], t[20], count[4] = {0};
    br_hmac_key_init(&k, &sha1_class, password, password_len);
    for (unsigned block = 1; block <= 2; block++) {
        count[3] = block;
        br_hmac_init(&c, &k, 20);
        br_hmac_update(&c, ssid, ssid_len);
        br_hmac_update(&c, count, 4);
        br_hmac_out(&c, u);
        memcpy(t, u, 20);
        for (unsigned iteration = 1; iteration < 4096; iteration++) {
            br_hmac_init(&c, &k, 20);
            br_hmac_update(&c, u, 20);
            br_hmac_out(&c, u);
            for (unsigned i = 0; i < 20; i++) t[i] ^= u[i];
        }
        memcpy(pmk + (block - 1) * 20, t, block == 1 ? 20 : 12);
    }
    wpa2_clear(u, sizeof(u)); wpa2_clear(t, sizeof(t));
    wpa2_clear(&c, sizeof(c)); wpa2_clear(&k, sizeof(k));
    return 0;
}

void wpa2_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
              const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48])
{
    const char label[] = "Pairwise key expansion";
    const uint8_t *mac1 = aa, *mac2 = spa, *nonce1 = anonce, *nonce2 = snonce;
    if (memcmp(aa, spa, 6) > 0) { mac1 = spa; mac2 = aa; }
    if (memcmp(anonce, snonce, 32) > 0) { nonce1 = snonce; nonce2 = anonce; }
    br_hmac_key_context k;
    br_hmac_context c;
    uint8_t digest[20];
    br_hmac_key_init(&k, &sha1_class, pmk, 32);
    for (uint8_t i = 0; i < 3; i++) {
        br_hmac_init(&c, &k, 20);
        br_hmac_update(&c, label, sizeof(label));
        br_hmac_update(&c, mac1, 6); br_hmac_update(&c, mac2, 6);
        br_hmac_update(&c, nonce1, 32); br_hmac_update(&c, nonce2, 32);
        br_hmac_update(&c, &i, 1);
        br_hmac_out(&c, digest);
        memcpy(ptk + i * 20, digest, i == 2 ? 8 : 20);
    }
    wpa2_clear(digest, sizeof(digest));
    wpa2_clear(&c, sizeof(c)); wpa2_clear(&k, sizeof(k));
}

int wpa2_unwrap(const uint8_t kek[16], const uint8_t *in, size_t length,
                uint8_t *out, size_t capacity)
{
    if (!kek || !in || !out || length < 24 || length > WPA2_KEY_DATA_MAX ||
        (length & 7) || capacity < length - 8) return -1;
    uint8_t plain[WPA2_KEY_DATA_MAX], b[16], iv[16];
    br_aes_ct_cbcdec_keys aes;
    memcpy(plain, in, length);
    br_aes_ct_cbcdec_init(&aes, kek, 16);
    unsigned blocks = (length - 8) / 8;
    for (int j = 5; j >= 0; j--) for (unsigned i = blocks; i; i--) {
        memcpy(b, plain, 8);
        unsigned t = blocks * j + i;
        for (unsigned k = 0; k < 8; k++) { b[7 - k] ^= t & 255; t >>= 8; }
        memcpy(b + 8, plain + i * 8, 8);
        memset(iv, 0, sizeof(iv));
        br_aes_ct_cbcdec_run(&aes, iv, b, 16);
        memcpy(plain, b, 8); memcpy(plain + i * 8, b + 8, 8);
    }
    const uint8_t initial[8] = {0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6};
    int result = -1;
    if (wpa2_equal(plain, initial, 8)) {
        memcpy(out, plain + 8, length - 8);
        result = length - 8;
    }
    wpa2_clear(plain, sizeof(plain)); wpa2_clear(b, sizeof(b));
    wpa2_clear(iv, sizeof(iv)); wpa2_clear(&aes, sizeof(aes));
    return result;
}
