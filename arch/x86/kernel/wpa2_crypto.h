#ifndef OSITO_WPA2_CRYPTO_H
#define OSITO_WPA2_CRYPTO_H
#include <stdint.h>
#include <stddef.h>

#define WPA2_KEY_DATA_MAX 512
void wpa2_clear(void *p, size_t n);
int wpa2_equal(const void *a, const void *b, size_t n);
void wpa2_hmac_sha1(const void *key, size_t key_len,
                    const void *data, size_t data_len, uint8_t mac[20]);
int wpa2_pmk(const uint8_t *password, size_t password_len,
             const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32]);
void wpa2_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
              const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]);
/* RFC 3394, AES-128 KEK only. Returns unwrapped length, or -1. Output is not
 * touched unless integrity succeeds. Buffers may overlap. */
int wpa2_unwrap(const uint8_t kek[16], const uint8_t *in, size_t length,
                uint8_t *out, size_t capacity);
#endif
