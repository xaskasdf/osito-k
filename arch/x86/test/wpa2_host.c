#include "../kernel/wpa2.h"
#include "../kernel/wpa2_rsn.h"
#include "../kernel/wpa2_selftest.h"
#include "../kernel/random_secure.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static int rng_support = 1, rng_fails, rng_break;
static unsigned rng_calls;
static uint64_t rng_sequence;
int random_secure_supported(void) { return rng_support; }
int random_secure_word(uint64_t *word)
{
    rng_calls++;
    if (rng_fails > 0) { rng_fails--; *word = UINT64_MAX; return 0; }
    if (rng_break && rng_calls >= (unsigned)rng_break) { *word = 0; return 0; }
    *word = rng_sequence++ * UINT64_C(0x0102030405060708);
    return 1;
}

static void rng_reset(void)
{
    rng_support = 1; rng_fails = 0; rng_break = 0; rng_calls = 0; rng_sequence = 1;
}

static int zero(const void *p, size_t n)
{
    const unsigned char *v = p;
    for (size_t i = 0; i < n; i++) if (v[i]) return 0;
    return 1;
}

static unsigned kat_fault;
static void kat_hmac(const void *k, size_t kn, const void *p, size_t n, uint8_t out[20])
{
    wpa2_hmac_sha1(k, kn, p, n, out);
    unsigned bit = kn == 20 ? WPA2_TEST_HMAC : kn == 32 ? WPA2_TEST_PMKID : WPA2_TEST_M2_MIC;
    if (kat_fault & (1U << bit)) out[0] ^= 1;
}
static int kat_pmk(const uint8_t *p, size_t pn, const uint8_t *s, size_t sn, uint8_t out[32])
{
    int rc = wpa2_pmk(p, pn, s, sn, out);
    if (kat_fault & (1U << WPA2_TEST_PMK)) out[0] ^= 1;
    return rc;
}
static void kat_ptk(const uint8_t p[32], const uint8_t a[6], const uint8_t s[6],
                    const uint8_t an[32], const uint8_t sn[32], uint8_t out[48])
{
    wpa2_ptk(p, a, s, an, sn, out);
    if (kat_fault & (1U << WPA2_TEST_PTK)) out[0] ^= 1;
}
static int kat_unwrap(const uint8_t k[16], const uint8_t *p, size_t n, uint8_t *out, size_t cap)
{
    int rc = wpa2_unwrap(k, p, n, out, cap);
    if (rc > 0 && (kat_fault & (1U << WPA2_TEST_UNWRAP))) out[0] ^= 1;
    return rc;
}
#define wpa2_selftest injected_selftest
#define wpa2_pmk kat_pmk
#define wpa2_ptk kat_ptk
#define wpa2_hmac_sha1 kat_hmac
#define wpa2_unwrap kat_unwrap
#include "../kernel/wpa2_selftest.c"
#undef wpa2_selftest
#undef wpa2_pmk
#undef wpa2_ptk
#undef wpa2_hmac_sha1
#undef wpa2_unwrap

static void test_kat(void)
{
    CHECK(!wpa2_selftest());
    for (kat_fault = 0; kat_fault < (1U << WPA2_TEST_COUNT); kat_fault++)
        CHECK(injected_selftest() == kat_fault);
    kat_fault = 0;
    CHECK(!wpa2_selftest());
}

static void test_rng(void)
{
    uint8_t b[80];
    rng_reset(); CHECK(!random_secure_bytes(NULL, 0)); CHECK(random_secure_bytes(NULL, 1) == -1);
    CHECK(rng_calls == 0);
    for (unsigned n = 1; n < 66; n++) for (unsigned offset = 0; offset < 8; offset++) {
        rng_reset(); memset(b, 0xa5, sizeof(b));
        CHECK(!random_secure_bytes(b + offset, n));
        CHECK(rng_calls == (n + 7) / 8);
        for (unsigned i = 0; i < offset; i++) CHECK(b[i] == 0xa5);
        for (unsigned i = offset + n; i < sizeof(b); i++) CHECK(b[i] == 0xa5);
        for (unsigned i = 0; i < n; i++)
            CHECK(b[offset + i] == (uint8_t)(((uint64_t)(i / 8 + 1) * UINT64_C(0x0102030405060708)) >> (8 * (i % 8))));
    }
    rng_reset(); rng_support = 0; memset(b, 0xa5, sizeof(b));
    CHECK(random_secure_bytes(b + 1, 32) == -1 && zero(b + 1, 32) && !rng_calls);
    CHECK(b[0] == 0xa5 && b[33] == 0xa5);
    rng_reset(); rng_fails = 9; CHECK(!random_secure_bytes(b, 8) && rng_calls == 10);
    rng_reset(); rng_fails = 10; memset(b, 0xa5, 32);
    CHECK(random_secure_bytes(b, 32) == -1 && zero(b, 32) && rng_calls == 10);
    for (int i = 1; i <= 4; i++) {
        rng_reset(); rng_break = i; memset(b, 0xa5, 32);
        CHECK(random_secure_bytes(b, 32) == -1 && zero(b, 32) && rng_calls == (unsigned)i + 9);
    }
    rng_reset(); rng_sequence = 0;
    CHECK(!random_secure_bytes(b, 8) && zero(b, 8)); /* CF, not data value, is success. */
}

static void ref_hmac(const uint8_t *key, unsigned kn, const uint8_t *data, unsigned n, uint8_t mac[20])
{
    unsigned length = 0;
    CHECK(HMAC(EVP_sha1(), key, kn, data, n, mac, &length) && length == 20);
}

static void ref_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                     const uint8_t an[32], const uint8_t sn[32], uint8_t out[48])
{
    uint8_t b[100], digest[20];
    unsigned n = sizeof("Pairwise key expansion");
    memcpy(b, "Pairwise key expansion", n);
    int m = memcmp(aa, spa, 6), a = memcmp(an, sn, 32);
    memcpy(b + n, m < 0 ? aa : spa, 6); n += 6;
    memcpy(b + n, m < 0 ? spa : aa, 6); n += 6;
    memcpy(b + n, a < 0 ? an : sn, 32); n += 32;
    memcpy(b + n, a < 0 ? sn : an, 32); n += 32;
    for (unsigned i = 0; i < 3; i++) {
        b[n] = i; ref_hmac(pmk, 32, b, n + 1, digest);
        memcpy(out + i * 20, digest, i == 2 ? 8 : 20);
    }
}

static unsigned wrap(const uint8_t key[16], const uint8_t *plain, unsigned n, uint8_t *out)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    CHECK(c != NULL);
    EVP_CIPHER_CTX_set_flags(c, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
    int len = 0, tail = 0;
    CHECK(EVP_EncryptInit_ex(c, EVP_aes_128_wrap(), NULL, key, NULL) == 1);
    CHECK(EVP_EncryptUpdate(c, out, &len, plain, n) == 1);
    CHECK(EVP_EncryptFinal_ex(c, out + len, &tail) == 1);
    EVP_CIPHER_CTX_free(c);
    return len + tail;
}

static void test_crypto(void)
{
    uint8_t key[100], data[512], mac[20], reference[64], out[520], ct[520];
    memset(key, 0x0b, 20);
    const uint8_t known[] = {0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,
                             0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00};
    wpa2_hmac_sha1(key, 20, "Hi There", 8, mac); CHECK(!memcmp(mac, known, 20));
    for (unsigned i = 0; i < sizeof(key); i++) key[i] = i;
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = i * 19;
    for (unsigned k = 0; k <= 100; k++) for (unsigned n = 0; n < 130; n += 7) {
        ref_hmac(key, k, data, n, reference);
        wpa2_hmac_sha1(key, k, data, n, mac); CHECK(!memcmp(mac, reference, 20));
    }
    const char *passwords[] = {"password", "ThisIsAPassword", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    const char *ssids[] = {"IEEE", "ThisIsASSID", "01234567890123456789012345678901"};
    for (unsigned i = 0; i < 3; i++) {
        CHECK(PKCS5_PBKDF2_HMAC_SHA1(passwords[i], strlen(passwords[i]), (const uint8_t *)ssids[i], strlen(ssids[i]), 4096, 32, reference));
        CHECK(!wpa2_pmk((const uint8_t *)passwords[i], strlen(passwords[i]), (const uint8_t *)ssids[i], strlen(ssids[i]), out));
        CHECK(!memcmp(out, reference, 32));
    }
    CHECK(wpa2_pmk(key, 7, data, 8, out) == -1 && zero(out, 32));
    memset(key, 'p', 64); CHECK(wpa2_pmk(key, 64, data, 8, out) == -1);
    key[2] = 0; CHECK(wpa2_pmk(key, 8, data, 8, out) == -1);
    key[2] = 'p'; CHECK(wpa2_pmk(key, 8, data, 0, out) == -1);
    CHECK(wpa2_pmk(key, 8, data, 33, out) == -1);
    CHECK(!wpa2_pmk(key, 8, data, 32, out)); /* SSID is binary, not a C string. */
    CHECK(PKCS5_PBKDF2_HMAC_SHA1((const char *)key, 8, data, 32, 4096, 32, reference));
    CHECK(!memcmp(out, reference, 32));
    for (unsigned i = 0; i < 256; i++) {
        data[0] = i; data[30] = 255 - i;
        ref_ptk(key, data, data + 6, data + 20, data + 60, reference);
        wpa2_ptk(key, data, data + 6, data + 20, data + 60, out);
        CHECK(!memcmp(out, reference, 48));
    }
    const uint8_t rfc_key[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    const uint8_t rfc_ct[24] = {0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,0xae,0xf3,0x4b,0xd8,
                               0xfb,0x5a,0x7b,0x82,0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5};
    CHECK(wpa2_unwrap(rfc_key, rfc_ct, 24, out, sizeof(out)) == 16);
    for (unsigned i = 0; i < 16; i++) CHECK(out[i] == i * 17);
    for (unsigned n = 16; n <= 504; n += 8) {
        unsigned cn = wrap(rfc_key, data, n, ct);
        CHECK(cn == n + 8);
        CHECK(wpa2_unwrap(rfc_key, ct, cn, out, n) == (int)n && !memcmp(data, out, n));
        for (unsigned j = 0; j < cn; j++) {
            ct[j] ^= 1; memset(out, 0x5a, sizeof(out));
            CHECK(wpa2_unwrap(rfc_key, ct, cn, out, sizeof(out)) == -1);
            for (unsigned k = 0; k < sizeof(out); k++) CHECK(out[k] == 0x5a);
            ct[j] ^= 1;
        }
        CHECK(wpa2_unwrap(rfc_key, ct, cn, out, n - 1) == -1);
        CHECK(wpa2_unwrap(rfc_key, ct, cn, ct + 1, n) == (int)n && !memcmp(ct + 1, data, n));
    }
    CHECK(wpa2_unwrap(key, ct, 513, out, sizeof(out)) == -1);
    CHECK(wpa2_unwrap(key, ct, 23, out, sizeof(out)) == -1);
}

static uint8_t pmk[32], ptk[48], anonce[32], gtk[32];
static bool tkip_test;
/* Public RSN advertisement captured from both CortesRojas APs. */
static const uint8_t mixed_rsn[] = {1,0,0,15,172,2,2,0,0,15,172,2,0,15,172,4,1,0,0,15,172,2,0,0};
static const uint8_t local[6] = {2,0,0,0,0,1}, peer[6] = {2,0,0,0,0,2};
static uint64_t now;
static void put16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
static void counter(uint8_t *p, uint64_t n)
{
    for (unsigned i = 0; i < 8; i++) { p[7 - i] = n; n >>= 8; }
}
static void frame_mic(uint8_t *p, unsigned n)
{
    uint8_t digest[20]; memset(p + 81, 0, 16);
    ref_hmac(ptk, 16, p, n, digest); memcpy(p + 81, digest, 16);
}
static void base(uint8_t *p, unsigned info, uint64_t replay)
{
    memset(p, 0, WPA2_EAPOL_MAX);
    p[0] = 2; p[1] = 3; p[4] = 2;
    put16(p + 2, 95); put16(p + 5, info); put16(p + 7, 16); counter(p + 9, replay);
    if (info & 8) memcpy(p + 17, anonce, 32);
}
static unsigned message(uint8_t *p, bool pairwise, uint64_t replay, int group_id)
{
    uint8_t plain[512]; unsigned n = 0;
    base(p, pairwise ? 0x13ca : 0x1382, replay);
    if (pairwise) {
        if (tkip_test) { plain[0] = 48; plain[1] = sizeof(mixed_rsn); memcpy(plain + 2, mixed_rsn, sizeof(mixed_rsn)); n = 26; }
        else { memcpy(plain, wpa2_assoc_rsn, 22); n = 22; }
    } else if (tkip_test) put16(p + 7, 32);
    if (group_id >= 0) {
        uint8_t kde[] = {221,22,0,15,172,1,(uint8_t)group_id,0};
        unsigned key_len = tkip_test ? 32 : 16; kde[1] = key_len + 6;
        memcpy(plain + n, kde, sizeof(kde)); n += sizeof(kde);
        memcpy(plain + n, gtk, key_len); n += key_len;
    }
    if (n & 7) { plain[n++] = 221; while (n & 7) plain[n++] = 0; }
    unsigned size = wrap(ptk + 16, plain, n, p + 99);
    put16(p + 97, size); put16(p + 2, 95 + size); p[65] = 47;
    frame_mic(p, 99 + size);
    return 99 + size;
}
static unsigned keyframe(uint8_t *p, unsigned info, uint64_t replay, const uint8_t *data, unsigned n)
{
    base(p, info, replay);
    unsigned size = n;
    if (info & 0x1000) size = wrap(ptk + 16, data, n, p + 99);
    else memcpy(p + 99, data, n);
    put16(p + 97, size); put16(p + 2, 95 + size);
    frame_mic(p, 99 + size);
    return 99 + size;
}
static void setup(wpa2_t *s)
{
    rng_reset(); now = 12345;
    wpa2_reset(s);
    for (unsigned i = 0; i < 32; i++) { pmk[i] = i + 1; anonce[i] = i * 17; }
    for (unsigned i = 0; i < 32; i++) gtk[i] = i * 7;
    CHECK(!wpa2_start(s, pmk, local, peer, tkip_test ? mixed_rsn : wpa2_assoc_rsn + 2, tkip_test ? 24 : 20, now));
    CHECK(s->state == WPA2_WAIT_M1 && !s->authorized && rng_calls == 4);
    ref_ptk(pmk, peer, local, anonce, s->snonce, ptk);
}
static uint32_t transmit(wpa2_t *s, unsigned expected, unsigned info, bool protected_frame)
{
    uint8_t f[WPA2_EAPOL_MAX], digest[20], saved[16]; uint32_t token = 0; bool enc = !protected_frame;
    int n = wpa2_take_tx(s, f, sizeof(f), &token, &enc, now);
    CHECK(n == (int)expected && token && enc == protected_frame);
    if (n < 99) return token;
    CHECK(((unsigned)f[5] << 8 | f[6]) == info && f[7] == 0 && f[8] == 0);
    memcpy(saved, f + 81, 16); memset(f + 81, 0, 16);
    ref_hmac(ptk, 16, f, n, digest); CHECK(!memcmp(saved, digest, 16));
    if (info == 0x010a) {
        uint8_t rsn[22]; memcpy(rsn, wpa2_assoc_rsn, 22); if (tkip_test) rsn[7] = 2;
        CHECK(!memcmp(f + 99, rsn, 22) && !memcmp(f + 17, s->snonce, 32));
    }
    CHECK(!wpa2_take_tx(s, f, sizeof(f), &token, &enc, now));
    return token;
}
static uint32_t m1(wpa2_t *s)
{
    uint8_t p[WPA2_EAPOL_MAX]; base(p, 0x8a, 1);
    CHECK(wpa2_receive(s, peer, p, 99, now) == 1 && s->state == WPA2_WAIT_M3);
    CHECK(!memcmp(s->ptk, ptk, 48));
    return transmit(s, 121, 0x010a, false);
}
static void establish(wpa2_t *s)
{
    setup(s); uint32_t token = m1(s); CHECK(!wpa2_tx_complete(s, token, true));
    uint8_t p[WPA2_EAPOL_MAX]; unsigned n = message(p, true, 2, 1);
    CHECK(wpa2_receive(s, peer, p, n, now) == 1 && s->install_pending);
    CHECK(!wpa2_keys_installed(s, s->keys.generation, true));
    token = transmit(s, 99, 0x030a, false);
    CHECK(!s->authorized && !wpa2_tx_complete(s, token, true));
    CHECK(s->authorized && s->state == WPA2_ESTABLISHED && s->installs == 1);
}

static void test_handshake(void)
{
    wpa2_t s; setup(&s);
    uint32_t m2token = m1(&s);
    uint8_t p[WPA2_EAPOL_MAX], bad[WPA2_EAPOL_MAX]; unsigned n = message(p, true, 2, 1);
    CHECK(wpa2_receive(&s, local, p, n, now) == 0);
    for (unsigned i = 0; i < n; i++) {
        memcpy(bad, p, n); bad[i] ^= 1;
        CHECK(wpa2_receive(&s, peer, bad, n, now) == 0 && !s.authorized && !s.install_pending);
    }
    for (unsigned len = 0; len < n; len++) CHECK(!wpa2_receive(&s, peer, p, len, now));
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && s.state == WPA2_INSTALL && !s.authorized);
    CHECK(s.keys.set_pairwise && s.keys.set_group && s.keys.group_id == 1);
    CHECK(!memcmp(s.keys.pairwise, ptk + 32, 16) && !memcmp(s.keys.group, gtk, 16) && s.keys.rsc[0] == 47);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && s.keys.generation == 1 && s.duplicates == 1);
    CHECK(wpa2_keys_installed(&s, 2, true) == -1 && s.install_pending);
    CHECK(!wpa2_keys_installed(&s, 1, true) && !s.authorized && !s.install_pending);
    CHECK(zero(s.keys.pairwise, 16) && zero(s.keys.group, 16));
    CHECK(!wpa2_tx_complete(&s, m2token, true) && !s.authorized);
    uint32_t m4token = transmit(&s, 99, 0x030a, false);
    CHECK(wpa2_tx_complete(&s, m2token, true) == -1 && !s.authorized);
    CHECK(!wpa2_tx_complete(&s, m4token, true) && s.authorized);
    for (unsigned replay = 2; replay <= 5; replay++) {
        n = message(p, true, replay, 1);
        CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && !s.install_pending && s.installs == 1);
        m4token = transmit(&s, 99, 0x030a, false); CHECK(!wpa2_tx_complete(&s, m4token, true));
    }
    n = message(p, true, 4, 1); CHECK(!wpa2_receive(&s, peer, p, n, now));
    gtk[0] ^= 17; n = message(p, true, 6, 1);
    CHECK(!wpa2_receive(&s, peer, p, n, now) && s.authorized);
    gtk[0] ^= 17; n = message(p, false, 6, 2); /* Same GTK, another slot. */
    CHECK(!wpa2_receive(&s, peer, p, n, now) && !s.install_pending);
    gtk[0] ^= 17; n = message(p, false, 6, 2);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && s.state == WPA2_GROUP_INSTALL && !s.authorized);
    CHECK(!s.keys.set_pairwise && s.keys.set_group && s.keys.generation == 2);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && s.keys.generation == 2);
    CHECK(!wpa2_keys_installed(&s, 2, true));
    uint32_t g2token = transmit(&s, 99, 0x0302, true);
    gtk[1] ^= 5; n = message(p, false, 7, 3);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && s.keys.generation == 3);
    CHECK(!wpa2_keys_installed(&s, 3, true));
    CHECK(!wpa2_tx_complete(&s, g2token, true) && !s.authorized);
    g2token = transmit(&s, 99, 0x0302, true);
    CHECK(!wpa2_tx_complete(&s, g2token, true) && s.authorized && s.installs == 3);
    n = message(p, false, 8, 3);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && !s.install_pending && s.installs == 3);
    g2token = transmit(&s, 99, 0x0302, true); CHECK(!wpa2_tx_complete(&s, g2token, true));
    wpa2_reset(&s); CHECK(zero(&s, sizeof(s)));
}

static void test_delayed_group(void)
{
    wpa2_t s; setup(&s); uint32_t token = m1(&s); CHECK(!wpa2_tx_complete(&s, token, true));
    uint8_t p[WPA2_EAPOL_MAX]; unsigned n = message(p, true, 2, -1);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && s.keys.set_pairwise && !s.keys.set_group);
    CHECK(!wpa2_keys_installed(&s, 1, true)); token = transmit(&s, 99, 0x030a, false);
    CHECK(!wpa2_tx_complete(&s, token, true) && !s.authorized && s.state == WPA2_WAIT_GROUP);
    n = message(p, false, 3, 1);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && s.keys.set_group);
    CHECK(!wpa2_keys_installed(&s, 2, true)); token = transmit(&s, 99, 0x0302, true);
    CHECK(!wpa2_tx_complete(&s, token, true) && s.authorized);
}

static void test_failure(void)
{
    wpa2_t s; setup(&s);
    CHECK(!wpa2_tick(&s, now + WPA2_TIMEOUT_NS - 1));
    CHECK(wpa2_tick(&s, now + WPA2_TIMEOUT_NS) == -1 && !s.authorized && s.state == WPA2_FAILED);
    CHECK(zero(s.pmk, 32) && zero(s.snonce, 32));
    setup(&s); uint32_t token = m1(&s);
    CHECK(wpa2_tx_complete(&s, token, false) == -1 && zero(s.ptk, 48) && !s.authorized);
    setup(&s); token = m1(&s); CHECK(!wpa2_tx_complete(&s, token, true));
    uint8_t p[WPA2_EAPOL_MAX]; unsigned n = message(p, true, 2, 1);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1);
    CHECK(wpa2_keys_installed(&s, 1, false) == -1 && zero(s.keys.group, 16) && zero(s.ptk, 48));
    establish(&s); CHECK(!wpa2_tick(&s, now + WPA2_TIMEOUT_NS * 2));
    n = message(p, true, 3, 1); CHECK(wpa2_receive(&s, peer, p, n, now) == 1);
    token = transmit(&s, 99, 0x030a, false);
    CHECK(wpa2_tick(&s, now + WPA2_TIMEOUT_NS) == -1 && zero(s.gtk, sizeof(s.gtk)));
    setup(&s); wpa2_reset(&s); rng_support = 0;
    CHECK(wpa2_start(&s, pmk, local, peer, wpa2_assoc_rsn + 2, 20, now) == -1 && s.state == WPA2_FAILED);
    CHECK(zero(s.snonce, 32) && zero(s.pmk, 32));
    wpa2_reset(&s); CHECK(wpa2_start(&s, pmk, local, local, wpa2_assoc_rsn + 2, 20, now) == -1);
}

static void test_keydata(void)
{
    wpa2_t s; setup(&s); uint32_t token = m1(&s); CHECK(!wpa2_tx_complete(&s, token, true));
    uint8_t p[WPA2_EAPOL_MAX], data[128];
    memcpy(data, wpa2_assoc_rsn, 22);
    const uint8_t kde[] = {221,22,0,15,172,1,1,0};
    memcpy(data + 22, kde, 8); memcpy(data + 30, gtk, 16); data[46] = 221; data[47] = 0;
    /* Valid MIC is not sufficient: RSN, KDE structure and encryption matter. */
    unsigned n = keyframe(p, 0x03ca, 2, data, 46);
    CHECK(!wpa2_receive(&s, peer, p, n, now) && !s.install_pending);
    for (unsigned i = 0; i < 22; i++) {
        data[i] ^= 1;
        n = keyframe(p, 0x13ca, 2, data, 48);
        CHECK(!wpa2_receive(&s, peer, p, n, now) && !s.install_pending);
        data[i] ^= 1;
    }
    data[23] = 255; n = keyframe(p, 0x13ca, 2, data, 48);
    CHECK(!wpa2_receive(&s, peer, p, n, now)); data[23] = 22;
    data[28] = 0x80; n = keyframe(p, 0x13ca, 2, data, 48);
    CHECK(!wpa2_receive(&s, peer, p, n, now)); data[28] = 1;
    data[29] = 1; n = keyframe(p, 0x13ca, 2, data, 48);
    CHECK(!wpa2_receive(&s, peer, p, n, now)); data[29] = 0;
    memcpy(data + 46, data + 22, 24); data[70] = 221; data[71] = 0;
    n = keyframe(p, 0x13ca, 2, data, 72);
    CHECK(!wpa2_receive(&s, peer, p, n, now));
    n = keyframe(p, 0x13ca, 2, data + 22, 24);
    CHECK(!wpa2_receive(&s, peer, p, n, now)); /* Missing RSN binding. */
    /* Plaintext M3 may contain only the RSN; GTK must arrive encrypted later. */
    n = keyframe(p, 0x03ca, 2, wpa2_assoc_rsn, 22);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && !s.keys.set_group);
    CHECK(!wpa2_keys_installed(&s, 1, true)); token = transmit(&s, 99, 0x030a, false);
    CHECK(!wpa2_tx_complete(&s, token, true) && s.state == WPA2_WAIT_GROUP && !s.authorized);
    n = message(p, false, 3, 1);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1);
    CHECK(!wpa2_keys_installed(&s, 2, true)); token = transmit(&s, 99, 0x0302, true);
    CHECK(!wpa2_tx_complete(&s, token, true) && s.authorized);
}

static void test_diagnostics(void)
{
    wpa2_t s;
    uint8_t p[WPA2_EAPOL_MAX];
    const char *reasons[] = {
        "short EAPOL-Key frame", "unsupported EAPOL version",
        "not an EAPOL-Key packet", "unsupported EAPOL-Key descriptor",
        "invalid negotiated key length", "invalid EAPOL-Key data length",
        "invalid EAPOL-Key length", "unsupported EAPOL-Key flags",
        "EAPOL-Key source is not associated AP",
        "pairwise key message not expected in current state"
    };
    for (unsigned fault = 0; fault < sizeof(reasons) / sizeof(reasons[0]); fault++) {
        setup(&s); base(p, 0x8a, 1);
        unsigned n = 99; const uint8_t *source = peer;
        switch (fault) {
        case 0: n = 98; break;
        case 1: p[0] = 4; break;
        case 2: p[1] = 0; break;
        case 3: p[4] = 254; break;
        case 4: put16(p + 7, 0); break;
        case 5: put16(p + 97, 22); break;
        case 6: put16(p + 2, 94); break;
        case 7: put16(p + 5, 0x88); break;
        case 8: source = local; break;
        case 9: put16(p + 5, 0x13ca); break;
        }
        CHECK(!wpa2_receive(&s, source, p, n, now));
        CHECK(s.last_drop.reason && !strcmp(s.last_drop.reason, reasons[fault]));
        CHECK(s.last_drop.state == WPA2_WAIT_M1 && s.last_drop.bytes == n);
        CHECK(s.last_drop.version == p[0] && s.last_drop.packet_type == p[1]);
        CHECK(s.last_drop.descriptor == p[4] && s.last_drop.body_bytes == (unsigned)(p[2] * 256 + p[3]));
        CHECK(s.last_drop.key_info == (unsigned)(p[5] * 256 + p[6]));
        CHECK(s.last_drop.key_bytes == (unsigned)(p[7] * 256 + p[8]));
        CHECK(s.last_drop.data_bytes == (n >= 99 ? (unsigned)(p[97] * 256 + p[98]) : 0));
        CHECK(s.dropped == 1 && !s.authorized && !s.install_pending && !s.tx_len);
        wpa2_rx_diagnostic_t saved = s.last_drop;
        base(p, 0x8a, 1);
        CHECK(wpa2_receive(&s, peer, p, 99, now) == 1);
        CHECK(!memcmp(&s.last_drop, &saved, sizeof(saved))); /* Success keeps the last rejection. */
        wpa2_reset(&s); CHECK(zero(&s, sizeof(s)));
    }
    setup(&s); base(p, 0x8a, 1);
    for (unsigned n = 0; n < 99; n++) {
        uint8_t *short_frame = malloc(n ? n : 1);
        CHECK(short_frame != NULL);
        if (!short_frame) break;
        memcpy(short_frame, p, n);
        CHECK(!wpa2_receive(&s, peer, short_frame, n, now));
        CHECK(s.last_drop.reason && !strcmp(s.last_drop.reason, "short EAPOL-Key frame"));
        CHECK(s.last_drop.bytes == n && !s.last_drop.data_bytes);
        CHECK(s.last_drop.version == (n >= 1 ? 2 : 0));
        CHECK(s.last_drop.packet_type == (n >= 2 ? 3 : 0));
        CHECK(s.last_drop.body_bytes == (n >= 4 ? 95 : 0));
        CHECK(s.last_drop.descriptor == (n >= 5 ? 2 : 0));
        CHECK(s.last_drop.key_info == (n >= 7 ? 0x8a : 0));
        CHECK(s.last_drop.key_bytes == (n >= 9 ? 16 : 0));
        free(short_frame);
    }
    CHECK(!wpa2_receive(&s, peer, NULL, 99, now));
    CHECK(s.last_drop.reason && !strcmp(s.last_drop.reason, "short EAPOL-Key frame"));
    CHECK(!s.last_drop.version && !s.last_drop.key_info && !s.last_drop.data_bytes);
    CHECK(!wpa2_receive(&s, NULL, p, 99, now));
    CHECK(s.last_drop.reason && !strcmp(s.last_drop.reason, "invalid EAPOL source"));
    setup(&s); uint32_t token = m1(&s); CHECK(!wpa2_tx_complete(&s, token, true));
    unsigned n = message(p, true, 2, 1);
    p[81] ^= 1;
    CHECK(!wpa2_receive(&s, peer, p, n, now) && s.bad_mic == 1);
    CHECK(s.last_drop.reason && !strcmp(s.last_drop.reason, "EAPOL-Key MIC mismatch"));
    CHECK(s.last_drop.state == WPA2_WAIT_M3 && !s.install_pending && !s.authorized);
    p[99] ^= 1; frame_mic(p, n);
    CHECK(!wpa2_receive(&s, peer, p, n, now));
    CHECK(s.last_drop.reason && !strcmp(s.last_drop.reason, "EAPOL-Key unwrap failed"));
    wpa2_reset(&s); CHECK(zero(&s, sizeof(s)));
}

static void test_m1(void)
{
    wpa2_t s; setup(&s);
    uint8_t p[WPA2_EAPOL_MAX], snonce[32], name[20], digest[20];
    memcpy(snonce, s.snonce, 32);
    memcpy(name, "PMK Name", 8); memcpy(name + 8, peer, 6); memcpy(name + 14, local, 6);
    ref_hmac(pmk, 32, name, 20, digest);
    base(p, 0x8a, 1);
    uint8_t kde[] = {221,20,0,15,172,4};
    memcpy(p + 99, kde, sizeof(kde)); memcpy(p + 105, digest, 16);
    put16(p + 2, 117); put16(p + 97, 22);
    p[106] ^= 1;
    CHECK(wpa2_receive(&s, peer, p, 121, now) == 1 && s.state == WPA2_WAIT_M3);
    CHECK(!s.last_drop.reason && !s.dropped && s.tx_len == 121);
    CHECK(!s.authorized && !s.install_pending && !s.installs);
    CHECK(s.pmkid_mismatches == 1);
    p[106] ^= 1;
    CHECK(wpa2_receive(&s, peer, p, 121, now) == 1 && !memcmp(s.ptk, ptk, 48));
    CHECK(wpa2_receive(&s, peer, p, 121, now) == 1 && !memcmp(s.snonce, snonce, 32) && rng_calls == 4);
    p[17] ^= 1; CHECK(!wpa2_receive(&s, peer, p, 121, now));
    p[17] ^= 1;
    counter(p + 9, 0); CHECK(!wpa2_receive(&s, peer, p, 121, now));
    counter(p + 9, 2); CHECK(wpa2_receive(&s, peer, p, 121, now) == 1 && rng_calls == 4);
    CHECK(s.pmkid_mismatches == 1);
    uint32_t token = 0; bool enc;
    CHECK(wpa2_take_tx(&s, p, 120, &token, &enc, now) == -1 && s.tx_len == 121 && !s.inflight_tx);
    token = transmit(&s, 121, 0x010a, false); CHECK(!wpa2_tx_complete(&s, token, true));
    CHECK(wpa2_tick(&s, now + WPA2_TIMEOUT_NS) == -1); /* M1 cannot extend the deadline. */
    establish(&s); base(p, 0x8a, 99);
    CHECK(!wpa2_receive(&s, peer, p, 99, now) && s.authorized); /* No unsolicited PTK replacement. */
}

static unsigned pmkid_m1(uint8_t *p, unsigned version, unsigned hint)
{
    base(p, 0x8a, 1); p[0] = version;
    if (!hint) return 99;
    uint8_t name[20], digest[20];
    memcpy(name, "PMK Name", 8); memcpy(name + 8, peer, 6); memcpy(name + 14, local, 6);
    ref_hmac(pmk, 32, name, sizeof(name), digest);
    memcpy(p + 99, "\xdd\x14\x00\x0f\xac\x04", 6);
    memcpy(p + 105, digest, 16);
    if (hint == 2) p[105] ^= 1;
    put16(p + 2, 117); put16(p + 97, 22);
    return 121;
}

static void test_pmkid_psk(void)
{
    wpa2_t s;
    uint8_t p[WPA2_EAPOL_MAX], tx[WPA2_EAPOL_MAX] = {0}, digest[20], saved[16];
    /* An M1 hint cannot authorize the port. M3 MIC and driver/TX ACKs must. */
    for (unsigned version = 1; version <= 3; version++) for (unsigned hint = 0; hint < 3; hint++) {
        setup(&s);
        unsigned n = pmkid_m1(p, version, hint);
        CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && s.state == WPA2_WAIT_M3);
        CHECK(!s.authorized && !s.install_pending && !s.installs && !s.dropped);
        CHECK(!memcmp(s.ptk, ptk, 48) && !memcmp(s.pmk, pmk, 32));
        CHECK(s.pmkid_mismatches == (unsigned)(hint == 2));
        uint32_t token = transmit(&s, 121, 0x010a, false);
        CHECK(!wpa2_tx_complete(&s, token, true) && !s.authorized);
        n = message(p, true, 2, 1);
        p[0] = version; frame_mic(p, n);
        p[81] ^= 1;
        CHECK(!wpa2_receive(&s, peer, p, n, now) && !s.install_pending && !s.authorized);
        p[81] ^= 1;
        CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && s.install_pending && !s.authorized);
        CHECK(wpa2_keys_installed(&s, s.keys.generation + 1, true) == -1 && !s.authorized);
        CHECK(!wpa2_keys_installed(&s, s.keys.generation, true) && !s.authorized);
        token = transmit(&s, 99, 0x030a, false);
        CHECK(!s.authorized && !wpa2_tx_complete(&s, token, true) && s.authorized);
        wpa2_reset(&s); CHECK(zero(&s, sizeof(s)));
    }
    /* Independent AP key: a wrong local PSK must fail M2/M3 authentication. */
    setup(&s); s.pmk[0] ^= 1;
    unsigned n = pmkid_m1(p, 1, 1);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && s.state == WPA2_WAIT_M3);
    CHECK(s.pmkid_mismatches == 1);
    uint32_t token = 0; bool enc = true;
    CHECK(wpa2_take_tx(&s, tx, sizeof(tx), &token, &enc, now) == 121 && token && !enc);
    CHECK(tx[0] == 1 && tx[5] == 1 && tx[6] == 10);
    memcpy(saved, tx + 81, 16); memset(tx + 81, 0, 16);
    ref_hmac(ptk, 16, tx, 121, digest); CHECK(memcmp(saved, digest, 16));
    CHECK(!wpa2_tx_complete(&s, token, true) && !s.authorized);
    n = message(p, true, 2, 1);
    CHECK(!wpa2_receive(&s, peer, p, n, now) && s.bad_mic == 1);
    CHECK(!s.install_pending && !s.installs && !s.tx_len && !s.authorized);
    CHECK(s.last_drop.reason && !strcmp(s.last_drop.reason, "EAPOL-Key MIC mismatch"));
    CHECK(wpa2_tick(&s, now + WPA2_TIMEOUT_NS) == -1 && s.state == WPA2_FAILED);
    CHECK(zero(s.pmk, 32) && zero(s.ptk, 48) && zero(s.snonce, 32) && !s.authorized);

    for (unsigned fault = 0; fault < 3; fault++) {
        setup(&s); n = pmkid_m1(p, 1, 2);
        if (fault == 0) p[100] = 19; /* Wrong KDE size. */
        if (fault == 1) { /* Duplicate PMKID, even if identical. */
            memcpy(p + 121, p + 99, 22); n = 143;
            put16(p + 2, 139); put16(p + 97, 44);
        }
        if (fault == 2) { n--; put16(p + 2, 116); put16(p + 97, 21); }
        CHECK(!wpa2_receive(&s, peer, p, n, now) && s.state == WPA2_WAIT_M1);
        CHECK(s.last_drop.reason && !strcmp(s.last_drop.reason, "invalid M1 key data"));
        CHECK(!s.tx_len && !s.authorized && !s.install_pending);
        CHECK(!s.pmkid_mismatches);
    }
}

static void test_tkip_group(void)
{
    tkip_test = true;
    test_handshake(); test_delayed_group();
    wpa2_t s;
    uint8_t p[WPA2_EAPOL_MAX];
    for (unsigned pos = 16; pos < 32; pos++) {
        establish(&s);
        CHECK(s.group_len == 32 && s.group_cipher == WPA2_CIPHER_TKIP && !memcmp(s.gtk[1], gtk, 32));
        gtk[pos] ^= 1;
        unsigned n = message(p, false, 3, 1);
        CHECK(!wpa2_receive(&s, peer, p, n, now) && s.authorized && !s.install_pending && s.installs == 1);
        CHECK(strstr(s.last_drop.reason, "temporal key reused"));
    }
    establish(&s);
    unsigned n = message(p, false, 3, 1);
    put16(p + 7, 16); frame_mic(p, n);
    CHECK(!wpa2_receive(&s, peer, p, n, now) && s.authorized && !s.install_pending);
    put16(p + 7, 32); frame_mic(p, n);
    CHECK(wpa2_receive(&s, peer, p, n, now) == 1 && !s.install_pending);
    uint32_t token = transmit(&s, 99, 0x302, true);
    CHECK(!wpa2_tx_complete(&s, token, true));
    /* M3 must bind the full AP advertisement, not just our selected suites. */
    setup(&s); token = m1(&s); CHECK(!wpa2_tx_complete(&s, token, true));
    uint8_t plain[88] = {48,24};
    memcpy(plain + 2, mixed_rsn, 24); plain[7] = 4;
    n = keyframe(p, 0x13ca, 2, plain, 32);
    CHECK(!wpa2_receive(&s, peer, p, n, now) && !s.install_pending);
    /* Wrong GTK lengths, including truncation inside the MIC subkeys. */
    for (unsigned length = 0; length <= 40; length++) if (length != 32) {
        setup(&s); token = m1(&s); CHECK(!wpa2_tx_complete(&s, token, true));
        memset(plain, 0, sizeof(plain)); plain[0] = 48; plain[1] = 24; memcpy(plain + 2, mixed_rsn, 24);
        uint8_t kde[8] = {221,6 + length,0,15,172,1,1,0};
        memcpy(plain + 26, kde, 8);
        unsigned bytes = 34 + length;
        if (bytes & 7) { plain[bytes++] = 221; while (bytes & 7) plain[bytes++] = 0; }
        n = keyframe(p, 0x13ca, 2, plain, bytes);
        CHECK(!wpa2_receive(&s, peer, p, n, now) && !s.install_pending && !s.authorized);
    }
    tkip_test = false;
}

static void test_michael(void)
{
    wpa2_t s; wpa2_michael_t cm = {0};
    establish(&s);
    CHECK(!wpa2_michael_failure(&s, &cm, now) && !cm.have_first && !s.mic_reports);
    tkip_test = true; establish(&s);
    CHECK(wpa2_michael_failure(&s, &cm, now) == 1 && s.authorized && !s.countermeasures);
    uint32_t token = transmit(&s, 99, 0xf02, true);
    CHECK(s.request_counter == 1 && !wpa2_tx_complete(&s, token, true));
    CHECK(!s.mic_deadline && !s.mic_reports);
    now += WPA2_MIC_WINDOW_NS - 1;
    CHECK(wpa2_michael_failure(&s, &cm, now) == 2 && !s.authorized && s.countermeasures);
    CHECK(cm.blocked_until == now + WPA2_MIC_WINDOW_NS && s.mic_reports == 1);
    CHECK(!wpa2_michael_failure(&s, &cm, now) && s.mic_reports == 1);
    uint8_t p[WPA2_EAPOL_MAX]; unsigned n = message(p, false, 3, 1);
    CHECK(!wpa2_receive(&s, peer, p, n, now) && !s.install_pending);
    token = transmit(&s, 99, 0xf02, true);
    CHECK(s.request_counter == 2 && !wpa2_tx_complete(&s, token, true) && !s.authorized);
    uint64_t blocked = cm.blocked_until;
    wpa2_reset(&s); CHECK(zero(&s, sizeof(s)) && cm.blocked_until == blocked);
    memset(&cm, 0, sizeof(cm)); establish(&s);
    CHECK(wpa2_michael_failure(&s, &cm, now) == 1);
    token = transmit(&s, 99, 0xf02, true); CHECK(!wpa2_tx_complete(&s, token, true));
    now += WPA2_MIC_WINDOW_NS;
    CHECK(wpa2_michael_failure(&s, &cm, now) == 1 && !s.countermeasures);
    CHECK(wpa2_tick(&s, now + WPA2_TIMEOUT_NS) == -1 && s.state == WPA2_FAILED && zero(s.ptk, 48));
    memset(&cm, 0, sizeof(cm)); establish(&s);
    CHECK(wpa2_michael_failure(&s, &cm, now) == 1);
    CHECK(wpa2_michael_failure(&s, &cm, now + 1) == 2 && s.mic_reports == 2);
    token = transmit(&s, 99, 0xf02, true); CHECK(!wpa2_tx_complete(&s, token, true));
    token = transmit(&s, 99, 0xf02, true); CHECK(!wpa2_tx_complete(&s, token, true));
    CHECK(!s.mic_reports && !s.mic_deadline && !s.authorized);
    tkip_test = false;
}

int main(void)
{
    test_kat();
    test_diagnostics(); test_pmkid_psk();
    if (failures) {
        printf("WPA2 diagnostics/PMKID contracts: %u checks, %u failures\n", checks, failures);
        return 1;
    }
    test_rng(); test_crypto(); test_handshake(); test_delayed_group(); test_failure();
    test_keydata(); test_m1();
    test_tkip_group(); test_michael();
    printf("WPA2 crypto/EAPOL/random contracts: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
