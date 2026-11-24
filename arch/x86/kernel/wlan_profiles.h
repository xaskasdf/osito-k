#ifndef OSITO_WLAN_PROFILES_H
#define OSITO_WLAN_PROFILES_H
#define WLAN_PROFILE_MAX 16
#define WLAN_PROFILE_BYTES 68
#define WLAN_PROFILES_BYTES (4 + WLAN_PROFILE_MAX * WLAN_PROFILE_BYTES)
typedef struct {
    unsigned char length, secure, group_cipher, reserved;
    unsigned char ssid[32], pmk[32];
} wlan_profile_t;
typedef struct {
    unsigned char version, enabled, count, reserved;
    wlan_profile_t network[WLAN_PROFILE_MAX];
} wlan_profiles_t;
int wlan_profiles_valid(const wlan_profiles_t *, unsigned);
int wlan_profiles_remember(wlan_profiles_t *, const wlan_profile_t *);
int wlan_profiles_match(const wlan_profile_t *, const unsigned char *, unsigned,
                        int secure, unsigned group_cipher);
void wlan_profiles_forget(wlan_profiles_t *, unsigned);
#endif
