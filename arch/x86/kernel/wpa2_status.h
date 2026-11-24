#ifndef OSITO_WPA2_STATUS_H
#define OSITO_WPA2_STATUS_H

enum wpa2_state { WPA2_OFF, WPA2_WAIT_M1, WPA2_WAIT_M3, WPA2_INSTALL,
                  WPA2_WAIT_M4_TX, WPA2_WAIT_GROUP, WPA2_ESTABLISHED, WPA2_GROUP_INSTALL,
                  WPA2_WAIT_GROUP_TX, WPA2_FAILED };

/* Safe status snapshot: fixed reason and public header fields only. Never
 * include keys, nonces, MICs, PMKIDs, replay counters or key-data bytes.
 * No private WPA2 state or host/kernel stdint dependency in this interface. */
typedef struct {
    const char *reason;
    enum wpa2_state state;
    unsigned bytes;
    unsigned short body_bytes, key_info, key_bytes, data_bytes;
    unsigned char version, packet_type, descriptor;
} wpa2_rx_diagnostic_t;

#endif
