#ifndef OSITO_IWX_LINK_H
#define OSITO_IWX_LINK_H
#include "iwx.h"
#include "iwxreg.h"

/* CC API-77 non-MLD context/TX layouts. Intel BSD notice in iwxreg.h;
 * reference: OpenBSD if_iwxreg.h and Linux iwlwifi fw/api. */
typedef struct __attribute__((packed)) {
    uint32_t id_color, action;
    uint32_t channel;
    uint8_t band, width, control, reserved_channel;
    uint32_t lmac, rxchain, dsp, reserved;
} iwx_link_phy_t;
typedef struct __attribute__((packed)) {
    uint16_t cw_min, cw_max;
    uint8_t aifs, fifo;
    uint16_t txop;
} iwx_link_ac_t;
typedef struct __attribute__((packed)) {
    uint32_t associated, dtim_time;
    uint64_t dtim_tsf;
    uint32_t beacon_interval, reserved, dtim_interval, policy, listen_interval, aid, beacon_time;
} iwx_link_sta_mac_t;
typedef struct __attribute__((packed)) {
    uint32_t id_color, action, type, tsf_id;
    uint8_t local[6]; uint16_t reserved_local;
    uint8_t bssid[6]; uint16_t reserved_bssid;
    uint32_t cck, ofdm, protection, preamble, slot, filter, qos;
    iwx_link_ac_t ac[5];
    iwx_link_sta_mac_t sta;
    uint32_t reserved_union;
} iwx_link_mac_t;
typedef struct __attribute__((packed)) {
    uint32_t id_color, action, macs[3], phy, lmac;
} iwx_link_binding_t;
typedef struct __attribute__((packed)) {
    uint8_t modify, awake; uint16_t tid_disable;
    uint32_t mac_color;
    uint8_t address[6]; uint16_t reserved2;
    uint8_t id, modify_mask; uint16_t reserved3;
    uint32_t flags, mask;
    uint8_t add_ba, remove_ba; uint16_t ba_ssn, sleep_count;
    uint8_t sleep_flags, type;
    uint16_t aid, beamform;
    uint32_t queues;
    uint16_t ba_window; uint8_t sp, uapsd;
} iwx_link_station_t;
typedef struct __attribute__((packed)) {
    uint32_t operation, sta_mask;
    uint8_t tid, reserved[3];
    uint32_t flags, cb_size;
    uint64_t byte_count, descriptors;
} iwx_link_queue_t;
_Static_assert(sizeof(iwx_link_phy_t) == 32, "PHY context v4, UHB channel ABI");
_Static_assert(sizeof(iwx_link_mac_t) == 148, "MAC context v5");
_Static_assert(__builtin_offsetof(iwx_link_mac_t, sta) == 100, "MAC station offset");
_Static_assert(sizeof(iwx_link_binding_t) == 28, "binding v2");
_Static_assert(sizeof(iwx_link_station_t) == 48, "station v12");
_Static_assert(sizeof(iwx_link_queue_t) == 36, "SCD queue v3");

typedef struct {
    iwx_link_status_t status;
    iwx_bss_t ap;
    uint8_t local[6], rates[12], nrates;
    uint8_t data_rates[12], data_rate_index, data_successes, data_cooldown;
    uint16_t capability, sequence;
    uint32_t rate_flags, cck, ofdm;
    bool session_started, session_ended, request_sent, ofdm_supported;
    uint64_t deadline;
} iwx_link_t;

const char *iwx_link_profile_check(const iwx_radio_profile_t *p);
const char *iwx_link_select(iwx_link_t *link, const iwx_bss_t *ap,
                           const iwx_nvm_report_t *n, const iwx_radio_profile_t *p,
                           const iwx_regulatory_t *reg, bool rsn);
bool iwx_link_tx_allowed(const iwx_regulatory_t *reg, unsigned channel);
void iwx_link_mac_build(iwx_link_mac_t *cmd, const iwx_link_t *link, bool associated);
uint32_t iwx_link_data_rate(const iwx_link_t *link);
void iwx_link_rate_feedback(iwx_link_t *link, bool acknowledged, unsigned retries);
/* Open System authentication is also the first step of WPA2. No keys, no data
 * authorization. These routines only construct/validate management frames. */
unsigned iwx_link_request(iwx_link_t *link, uint8_t frame[256]);
int iwx_link_receive(iwx_link_t *link, const uint8_t *mpdu, unsigned size);
const char *iwx_link_rsn_check(const uint8_t *ie, unsigned length);
const char *iwx_link_tx_build(iwx_tfh_tfd_t *tfd, uint8_t page[4096], uint64_t phys,
                             unsigned queue, unsigned index, uint32_t rate,
                             const uint8_t *frame, unsigned length, uint16_t *byte_count);

#endif
