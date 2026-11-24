#ifndef OSITO_WPA2_RSN_H
#define OSITO_WPA2_RSN_H
enum wpa2_cipher { WPA2_CIPHER_NONE, WPA2_CIPHER_TKIP = 2, WPA2_CIPHER_CCMP = 4 };
/* RSN payload, without the element ID and length octets. */
const char *wpa2_rsn_check(const unsigned char *ie, unsigned length);
const char *wpa2_rsn_select(const unsigned char *ie, unsigned length, enum wpa2_cipher *group);
/* Select CCMP pairwise/PSK and preserve the AP's group cipher. */
int wpa2_rsn_assoc(unsigned char out[22], const unsigned char *ie, unsigned length);
extern const unsigned char wpa2_assoc_rsn[22];
#endif
