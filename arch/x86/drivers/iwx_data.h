#ifndef OSITO_IWX_DATA_H
#define OSITO_IWX_DATA_H
#include "iwx_link.h"
#include "../kernel/wpa2_rsn.h"

#define IWX_ETH_MAX 1514U
#define IWX_DATA_MAX (24U + 8U + 1500U)
#define IWX_CCMP_PN_MAX 0xffffffffffffULL
#define IWX_RX_RADA (1U << 26)

/* CC API-77 ADD_STA_KEY v3. Intel BSD notice in iwxreg.h. */
typedef struct __attribute__((packed)) {
    uint8_t station, offset;
    uint16_t flags;
    uint8_t key[32], receive_counter[16];
    uint64_t rx_mic, tx_mic, transmit_counter;
} iwx_key_cmd_t;
_Static_assert(sizeof(iwx_key_cmd_t) == 76, "ADD_STA_KEY v3 size");
_Static_assert(__builtin_offsetof(iwx_key_cmd_t, receive_counter) == 36, "key RX counter offset");
_Static_assert(__builtin_offsetof(iwx_key_cmd_t, transmit_counter) == 68, "key TX counter offset");

typedef struct {
    /* Slot 0: PTK, slots 1..4: GTK IDs 0..3, all owned by AP station 0. */
    uint32_t generation[5];
    enum wpa2_cipher cipher[5];
    uint64_t pn[5];
    uint16_t last_sequence;
    bool have_sequence;
} iwx_data_t;

typedef struct {
    uint8_t ethernet[IWX_ETH_MAX];
    unsigned length;
    bool eapol;
} iwx_data_rx_t;

const char *iwx_key_profile_check(const iwx_radio_profile_t *p);
int iwx_key_build(iwx_key_cmd_t *cmd, unsigned slot, enum wpa2_cipher cipher,
                    const uint8_t *key, unsigned key_len);
/* Only after firmware acknowledgement. Repeated generations never reset PN. */
int iwx_data_key_commit(iwx_data_t *s, unsigned slot, uint32_t generation,
                         enum wpa2_cipher cipher, const uint8_t rsc[6]);
int iwx_data_receive(iwx_data_t *s, const iwx_link_t *l, const uint8_t *mpdu,
                      unsigned size, iwx_data_rx_t *out);
/* Same acceptance/output contract; optional metadata is set even on rejection. */
int iwx_data_receive_diag(iwx_data_t *s, const iwx_link_t *l, const uint8_t *mpdu,
                           unsigned size, iwx_data_rx_t *out, iwx_rx_sample_t *diag,
                           uint32_t packet_flags);
int iwx_data_frame(iwx_data_t *s, iwx_link_t *l, const uint8_t *ethernet,
                    unsigned length, bool protect, uint8_t *out, unsigned capacity);
#endif
