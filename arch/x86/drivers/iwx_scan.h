#ifndef OSITOK_IWX_SCAN_H
#define OSITOK_IWX_SCAN_H

#include "iwx.h"

/* Intel API-77 scan request v15; x86 little endian. The ABI notices and
 * sources used for these layouts are in iwxreg.h and docs/x86-ax200-wlan.md. */
typedef struct __attribute__((packed)) {
    uint16_t offset, length;
} iwx_probe_segment_t;
typedef struct __attribute__((packed)) {
    uint32_t uid, priority;
    struct __attribute__((packed)) {
        uint16_t flags;
        uint8_t reserved, start_mac, active_dwell[2];
        uint8_t adwell_2g, adwell_5g, adwell_social, flags2;
        uint16_t adwell_budget;
        uint32_t max_out[2], suspend[2], priority;
        uint8_t passive_dwell[2], fragments[2];
    } general;
    struct __attribute__((packed)) {
        uint8_t flags, count, aps_override[2];
        struct __attribute__((packed)) {
            uint32_t flags;
            uint8_t channel, band, iterations, interval;
        } channel[67];
    } channels;
    struct __attribute__((packed)) {
        struct __attribute__((packed)) {
            uint16_t interval;
            uint8_t iterations, reserved;
        } schedule[2];
        uint16_t delay, reserved;
    } periodic;
    struct __attribute__((packed)) {
        iwx_probe_segment_t mac, band[3], common;
        uint8_t data[512];
        uint8_t short_ssid_count, bssid_count;
        uint16_t reserved;
        struct { uint8_t id, length, ssid[32]; } ssid[20];
        uint32_t short_ssid[8];
        uint8_t bssid[16][6];
    } probe;
} iwx_scan_request_t;
_Static_assert(sizeof(iwx_scan_request_t) == 1940, "scan request v15 ABI");
_Static_assert(__builtin_offsetof(iwx_scan_request_t, channels) == 44, "scan channel ABI");
_Static_assert(__builtin_offsetof(iwx_scan_request_t, periodic) == 584, "scan schedule ABI");
_Static_assert(__builtin_offsetof(iwx_scan_request_t, probe) == 596, "scan probe ABI");

bool iwx_radio_api(const iwx_radio_profile_t *p, unsigned bit);
bool iwx_radio_capability(const iwx_radio_profile_t *p, unsigned bit);
const char *iwx_scan_profile_check(const iwx_radio_profile_t *p, const iwx_nvm_report_t *n);
const char *iwx_mcc_decode(iwx_regulatory_t *out, const iwx_nvm_report_t *n,
                          const uint8_t *data, unsigned length);
const char *iwx_scan_build(iwx_scan_request_t *out, const iwx_regulatory_t *reg,
                          const iwx_nvm_report_t *n);
/* 1 = valid beacon/probe response, 0 = unrelated/bad CRC, -1 = malformed.
 * Neither firmware RX data nor information elements are trusted. */
int iwx_scan_decode_mpdu(iwx_bss_t *out, const uint8_t *data, unsigned length,
                         const iwx_regulatory_t *reg);
void iwx_scan_cache(iwx_scan_status_t *s, iwx_bss_t *cache, const iwx_bss_t *bss);
/* At most 128 printable bytes plus NUL; escapes controls, quotes and backslashes. */
void iwx_ssid_text(char out[129], const uint8_t *ssid, unsigned length);

#endif
