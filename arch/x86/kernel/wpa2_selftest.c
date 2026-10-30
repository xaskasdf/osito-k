#include "wpa2_selftest.h"
#include "wpa2_crypto.h"
#include "wpa2_rsn.h"
#include <string.h>

unsigned wpa2_selftest(void)
{
    /* RFC 2202 case 1, WPA password/IEEE, RFC 3394 section 4.1.
     * The synthetic PTK/PMKID/M2 vectors use the public PMK below, AP ...02,
     * station ...01, ANonce=00..1f, SNonce=20..3f; checked against OpenSSL. */
    static const uint8_t hmac_expected[20] = {
        0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,
        0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00
    };
    static const uint8_t pmk[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e
    };
    static const uint8_t ptk[48] = {
        0xdd,0xbd,0x69,0x93,0xdd,0xce,0xf2,0xe5,0x5f,0x87,0xa6,0x20,0xa1,0x90,0xea,0x20,
        0xf5,0xcc,0x99,0x9d,0x44,0xa9,0x95,0x9e,0xe8,0x83,0xd5,0xbd,0xf5,0xe1,0x79,0x8e,
        0x74,0x95,0xc0,0xaa,0x22,0x58,0x82,0x88,0xf3,0xb7,0x58,0xfb,0x43,0x18,0xd0,0x0a
    };
    static const uint8_t pmkid[16] = {
        0x6e,0xd2,0x46,0x2d,0x3f,0xc2,0x66,0x19,0xbd,0xc3,0xb7,0x4c,0x54,0x40,0x8c,0x1d
    };
    static const uint8_t mic[16] = {
        0x2b,0x9b,0xb9,0xfb,0xad,0xe2,0xa1,0x64,0x72,0x34,0xfd,0x41,0x94,0x64,0x0c,0x4f
    };
    static const uint8_t wrapped[24] = {
        0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,0xae,0xf3,0x4b,0xd8,
        0xfb,0x5a,0x7b,0x82,0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5
    };
    const uint8_t aa[6] = {2,0,0,0,0,2}, spa[6] = {2,0,0,0,0,1};
    uint8_t key[20], an[32], sn[32], out[48], frame[121] = {0};
    unsigned failed = 0;
    memset(key, 0x0b, sizeof(key));
    wpa2_hmac_sha1(key, sizeof(key), "Hi There", 8, out);
    if (!wpa2_equal(out, hmac_expected, 20)) failed |= 1U << WPA2_TEST_HMAC;
    if (wpa2_pmk((const uint8_t *)"password", 8, (const uint8_t *)"IEEE", 4, out) ||
        !wpa2_equal(out, pmk, 32)) failed |= 1U << WPA2_TEST_PMK;
    for (unsigned i = 0; i < 32; i++) { an[i] = i; sn[i] = i + 32; }
    wpa2_ptk(pmk, aa, spa, an, sn, out);
    if (!wpa2_equal(out, ptk, 48)) failed |= 1U << WPA2_TEST_PTK;
    /* Exercise the inverse ordering too; each KAT uses fixed inputs so an
     * earlier failure cannot hide which later primitive also failed. */
    wpa2_ptk(pmk, spa, aa, sn, an, out);
    if (!wpa2_equal(out, ptk, 48)) failed |= 1U << WPA2_TEST_PTK;
    memcpy(frame, "PMK Name", 8); memcpy(frame + 8, aa, 6); memcpy(frame + 14, spa, 6);
    wpa2_hmac_sha1(pmk, sizeof(pmk), frame, 20, out);
    if (!wpa2_equal(out, pmkid, 16)) failed |= 1U << WPA2_TEST_PMKID;
    memset(frame, 0, sizeof(frame));
    frame[0] = 2; frame[1] = 3; frame[3] = 117; frame[4] = 2;
    frame[5] = 1; frame[6] = 10; frame[16] = 1;
    memcpy(frame + 17, sn, 32); frame[98] = sizeof(wpa2_assoc_rsn);
    memcpy(frame + 99, wpa2_assoc_rsn, sizeof(wpa2_assoc_rsn));
    wpa2_hmac_sha1(ptk, 16, frame, sizeof(frame), out);
    if (!wpa2_equal(out, mic, 16)) failed |= 1U << WPA2_TEST_M2_MIC;
    for (unsigned i = 0; i < 16; i++) key[i] = i;
    int n = wpa2_unwrap(key, wrapped, sizeof(wrapped), out, sizeof(out));
    if (n != 16) failed |= 1U << WPA2_TEST_UNWRAP;
    for (unsigned i = 0; i < 16; i++)
        if (out[i] != i * 17) failed |= 1U << WPA2_TEST_UNWRAP;
    memcpy(frame, wrapped, sizeof(wrapped)); frame[0] ^= 1;
    memset(out, 0x5a, sizeof(out));
    if (wpa2_unwrap(key, frame, sizeof(wrapped), out, sizeof(out)) != -1)
        failed |= 1U << WPA2_TEST_UNWRAP;
    for (unsigned i = 0; i < sizeof(out); i++)
        if (out[i] != 0x5a) failed |= 1U << WPA2_TEST_UNWRAP;
    wpa2_clear(key, sizeof(key)); wpa2_clear(an, sizeof(an)); wpa2_clear(sn, sizeof(sn));
    wpa2_clear(out, sizeof(out)); wpa2_clear(frame, sizeof(frame));
    return failed;
}
