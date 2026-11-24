#ifndef OSITO_WPA2_H
#define OSITO_WPA2_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "wpa2_crypto.h"
#include "wpa2_status.h"
#include "wpa2_rsn.h"

#define WPA2_EAPOL_MAX (99 + WPA2_KEY_DATA_MAX)
#define WPA2_TIMEOUT_NS 10000000000ULL
#define WPA2_MIC_WINDOW_NS 60000000000ULL
enum wpa2_tx_kind { WPA2_TX_NONE, WPA2_TX_M2, WPA2_TX_M4, WPA2_TX_GROUP, WPA2_TX_MIC_REPORT };

/* Per-radio state survives disconnects; it contains no key material. */
typedef struct {
    uint64_t first_ns, blocked_until;
    bool have_first;
} wpa2_michael_t;

typedef struct {
    uint8_t pairwise[16], group[32], rsc[6], group_id;
    enum wpa2_cipher group_cipher;
    bool set_pairwise, set_group;
    uint32_t generation;
} wpa2_keys_t;

/* Private per-link state. Never print, persist, or copy to a status interface.
 * A caller serializes all accesses and wipes this on every disconnect. */
typedef struct {
    enum wpa2_state state;
    const char *error;
    uint8_t local[6], peer[6], ap_rsn[255], ap_rsn_len;
    uint8_t pmk[32], ptk[48], snonce[32], anonce[32];
    uint8_t gtk[4][32], gtk_set, group_len, assoc_rsn[22];
    enum wpa2_cipher group_cipher;
    uint8_t m1_counter[8], replay[8];
    bool have_replay, pairwise_installed, authorized;
    wpa2_keys_t keys;
    bool install_pending;
    uint8_t last_rx[WPA2_EAPOL_MAX], tx[121];
    unsigned last_rx_len, tx_len;
    enum wpa2_tx_kind tx_kind, inflight_kind;
    uint32_t next_tx, inflight_tx, inflight_generation;
    uint64_t started, tx_started;
    unsigned dropped, bad_mic, duplicates, installs;
    unsigned pmkid_mismatches;
    unsigned mic_reports;
    bool countermeasures;
    uint64_t request_counter, mic_deadline;
    wpa2_rx_diagnostic_t last_drop;
} wpa2_t;

void wpa2_reset(wpa2_t *s);
int wpa2_start(wpa2_t *s, const uint8_t pmk[32], const uint8_t local[6],
               const uint8_t peer[6], const uint8_t *ap_rsn, unsigned rsn_len, uint64_t now);
/* 1: accepted (inspect pending TX/keys); 0: dropped; -1: fatal, port closed.
 * src is the verified 802.11 transmitter, not untrusted LLC payload metadata. */
int wpa2_receive(wpa2_t *s, const uint8_t src[6], const uint8_t *eapol, unsigned length, uint64_t now);
/* Keys are immutable while pending. Hardware ACKs, not submission, commit them. */
int wpa2_keys_installed(wpa2_t *s, uint32_t generation, bool success);
int wpa2_take_tx(wpa2_t *s, uint8_t *frame, unsigned capacity, uint32_t *token,
                 bool *protected_frame, uint64_t now);
int wpa2_tx_complete(wpa2_t *s, uint32_t token, bool success);
int wpa2_tick(wpa2_t *s, uint64_t now);
/* Only for firmware-confirmed ICV-valid, fresh-TSC TKIP Michael failures. */
int wpa2_michael_failure(wpa2_t *s, wpa2_michael_t *cm, uint64_t now);
#endif
