#ifndef OSITO_WPA2_SELFTEST_H
#define OSITO_WPA2_SELFTEST_H

enum wpa2_selftest_case {
    WPA2_TEST_HMAC, WPA2_TEST_PMK, WPA2_TEST_PTK,
    WPA2_TEST_PMKID, WPA2_TEST_M2_MIC, WPA2_TEST_UNWRAP, WPA2_TEST_COUNT
};
/* Independent public vectors only. No radio, RNG, credentials or global state.
 * Returns a failure bitmap, one bit per case; zero means all passed. */
unsigned wpa2_selftest(void);
#endif
