#ifndef OSITOK_IWX_H
#define OSITOK_IWX_H

#include "../include/pci.h"
#include "../include/dma_diag.h"
#include "../kernel/nic.h"
#include "../kernel/wpa2_status.h"

#define IWX_BOOT_PRPH_SAMPLES 10
typedef struct {
    bool captured, pci_valid, csr_valid, pcie_valid, pm_valid;
    const char *pci_error, *prph_error;
    uint32_t pci_id, pci_command_status;
    uint16_t pcie_offset, pcie_flags, pcie_device_control, pcie_device_status;
    uint16_t pcie_link_control, pcie_link_status;
    uint16_t pm_offset, pm_control;
    uint32_t interrupt, fh_interrupt, gp_cntrl, reset, hw_if, int_mask;
    uint64_t context_expected, context_observed, free_ring, used_ring, status;
    uint64_t lmac, umac, paging;
    uint32_t dma_bytes, rx_read, rx_producer, context_flags;
    uint16_t context_mac, context_dwords;
    unsigned prph_count;
    struct { const char *name; uint32_t address, value; bool valid; }
        prph[IWX_BOOT_PRPH_SAMPLES];
} iwx_boot_snapshot_t;

#define IWX_BOOT_TRACE_SAMPLES 16
typedef struct {
    const char *phase, *note;
    uint64_t elapsed_us, context_expected, context_observed;
    uint32_t pci_id, pci_command_status;
    uint32_t interrupt, fh_interrupt, gp_cntrl, reset, hw_if, int_mask;
    uint32_t dma_bytes, rx_read, rx_producer;
    uint16_t pcie_device_control, pcie_device_status;
    uint16_t pcie_link_control, pcie_link_status, pm_control;
    bool pci_valid, pcie_valid, pm_valid, csr_sampled, csr_valid;
} iwx_boot_trace_sample_t;

typedef struct {
    const char *stage;
    const char *error;
    uint64_t bar0;
    uint32_t hw_rev, hw_rf_id, gp_cntrl, reset, interrupt;
    uint16_t command_before, command_after;
    bool clock_ready, rfkill_known, rfkill, stopped;
    const char *stop_error;
    uint64_t retained_bytes;
    uint32_t rx_packets, lmac_major, lmac_minor, umac_major, umac_minor;
    uint16_t alive_status;
    uint8_t alive_version;
    bool rfh_ready, alive;
    iwx_boot_snapshot_t fault;
    uint64_t trace_start_ns;
    unsigned trace_count, trace_dropped, poll_count;
    iwx_boot_trace_sample_t trace[IWX_BOOT_TRACE_SAMPLES];
    dma_diag_t platform;
    pci_dma_path_t dma_path;
    const char *progress_failed_at;
    unsigned progress_count, progress_deferred;
} iwx_probe_report_t;
_Static_assert(sizeof(iwx_probe_report_t) <= 8192, "bounded WLAN report stack use");

/* Explicit, destructive diagnostic of an unowned AX200. No DMA or RF TX.
 * Attempts verified stop on every accessible-MMIO exit; failures are reported.
 * No NIC is registered. */
int iwx_probe(iwx_probe_report_t *report);

#define IWX_FW_MAX_SIZE (4U * 1024U * 1024U)
typedef struct {
    uint32_t version[3], phy_config, paging_size, commands, tlvs;
    uint32_t sections[3], bytes[3]; /* LMAC, UMAC, paging (including CSS) */
} iwx_fw_info_t;

/* Validates the API-77 TLV/container layout, not authenticity or radio SKU.
 * No pointers into input survive; info is zeroed on every error. */
const char *iwx_fw_inspect(const void *data, uint32_t size, iwx_fw_info_t *info);

enum iwx_radio_abi {
    IWX_ABI_ANT, IWX_ABI_BT, IWX_ABI_SOC, IWX_ABI_DQA, IWX_ABI_TEMP,
    IWX_ABI_POWER, IWX_ABI_MCC, IWX_ABI_SCAN_CFG, IWX_ABI_SCAN_REQ,
    IWX_ABI_SCAN_ABORT, IWX_ABI_BEACON_FILTER, IWX_ABI_RX_MPDU, IWX_ABI_LTR,
    IWX_ABI_PHY_CTX, IWX_ABI_RLC, IWX_ABI_MAC_CTX, IWX_ABI_BINDING,
    IWX_ABI_STA, IWX_ABI_TX, IWX_ABI_TX_QUEUE, IWX_ABI_SESSION,
    IWX_ABI_SESSION_NOTIF, IWX_ABI_STA_KEY, IWX_ABI_COUNT
};
typedef struct {
    uint32_t api[3], capa[4], phy_config, scan_channels, seen;
    uint8_t command[IWX_ABI_COUNT], notification[IWX_ABI_COUNT];
} iwx_radio_profile_t;
_Static_assert(IWX_ABI_COUNT <= 32, "radio ABI presence bitmap capacity");

typedef struct {
    iwx_fw_info_t info;
    struct { uint32_t offset, length; } section[3][64];
    uint8_t alive_version;
    uint8_t nvm_version, nvm_get_version, nvm_complete_version;
    bool nvm_v4;
    iwx_radio_profile_t radio;
} iwx_fw_plan_t;

/* Offsets refer only to validated section payloads, excluding destination words. */
const char *iwx_fw_plan(const void *data, uint32_t size, iwx_fw_plan_t *plan);
/* Bounded DMA/ALIVE diagnostic, always attempts shutdown. A failed stop pins
 * DMA allocations until iwx_stop succeeds. This does not establish a link. */
int iwx_fw_boot(const void *data, uint32_t size, iwx_probe_report_t *report);
/* Explicit slow diagnostic. The synchronous observer may persist each marker;
 * it must not access the radio or retain report pointers. False aborts startup,
 * but never skips verified shutdown/quarantine. No observer runs while this
 * driver may hold MAC access. The ordinary API above installs no observer. */
typedef bool (*iwx_progress_fn)(void *arg, const char *phase,
                                const iwx_probe_report_t *report);
int iwx_fw_boot_traced(const void *data, uint32_t size, iwx_probe_report_t *report,
                       iwx_progress_fn progress, void *arg);

#define IWX_NVM_CHANNELS 110
typedef struct {
    bool attempted, init_complete, valid, mac_from_otp;
    uint8_t mac[6], response_version, board_type, n_hw_addrs;
    uint16_t nvm_version, response_bytes;
    uint32_t general_flags, mac_sku, tx_chains, rx_chains, lar_enabled, n_channels;
    uint32_t channel_profile[IWX_NVM_CHANNELS];
    unsigned commands_sent, commands_completed, command_polls;
    unsigned init_notifications;
    uint16_t init_sequence, init_payload_bytes;
    uint8_t last_group, last_opcode, last_index;
} iwx_nvm_report_t;

/* Bounded initialization/NVM read, then verified shutdown. No NVM writes,
 * scan, association, RF frame transmission or NIC registration. Raw channel
 * profiles are evidence, not permission to transmit. Optional slow observer. */
int iwx_fw_nvm(const void *data, uint32_t size, iwx_probe_report_t *report,
               iwx_nvm_report_t *nvm, iwx_progress_fn progress, void *arg);

enum iwx_runtime_state { IWX_OFF, IWX_ACTIVE, IWX_FAILED, IWX_QUARANTINED };
typedef struct {
    enum iwx_runtime_state state;
    bool worker_started;
    uint64_t generation, polls, rx_buffers, rx_reposted;
    unsigned dma_bytes, rx_read, rx_write, rx_outstanding, rx_pending;
    uint32_t rx_packets;
    const char *error, *stop_error;
    iwx_nvm_report_t nvm;
} iwx_status_t;

/* Opt-in persistent firmware/NVM transport, NOT a network link. The driver
 * owns all retained state; callers may release the firmware and reports on
 * return. Requires scheduling, starts one process-context RX worker. */
int iwx_start(const void *data, uint32_t size, iwx_probe_report_t *report,
              iwx_nvm_report_t *nvm, iwx_progress_fn progress, void *arg);
/* Nonblocking ownership attempt; at most 64 RX buffers per successful call.
 * Process context only (failure cleanup may wait for hardware shutdown).
 * Returns processed buffers, zero when idle/busy, or -1 after a failure. */
int iwx_poll(void);
/* Consistent software snapshot, no hardware accesses; -1 while busy. */
int iwx_get_status(iwx_status_t *status);

#define IWX_SCAN_CHANNELS 51
#define IWX_SCAN_BSS_MAX 128
enum iwx_scan_state {
    IWX_SCAN_IDLE, IWX_SCAN_RUNNING, IWX_SCAN_ABORTING,
    IWX_SCAN_COMPLETE, IWX_SCAN_ABORTED, IWX_SCAN_FAILED
};
typedef struct {
    bool valid;
    uint16_t mcc, capabilities, geo;
    uint8_t source;
    unsigned count;
    struct { uint8_t number; uint32_t flags; } channel[IWX_SCAN_CHANNELS];
} iwx_regulatory_t;
typedef struct {
    uint8_t bssid[6], ssid[32], ssid_len, channel;
    int16_t signal_dbm;
    uint16_t capability, beacon_interval;
    bool rsn, wpa;
    uint32_t observations;
    uint8_t rates[16], rates_len, dtim_count, dtim_period, erp;
    uint8_t rsn_ie[255], rsn_len;
    uint32_t system_time;
    uint64_t tsf;
} iwx_bss_t;
typedef struct {
    enum iwx_scan_state state;
    bool radio_ready;
    uint64_t generation;
    unsigned count, dropped, malformed, ignored, notifications;
    uint8_t completion_status, ebs_status;
    const char *error;
    iwx_regulatory_t regulatory;
} iwx_scan_status_t;

/* Passive discovery only. No authentication, association or IP interface.
 * Commands return NULL on success or a stable error string. Single scan slot;
 * loss of command/completion ownership requires verified transport shutdown. */
const char *iwx_scan_start(void);
const char *iwx_scan_abort(void);
/* Atomic bounded snapshot; BSS entries are untrusted observations, not identities. */
int iwx_scan_results(iwx_scan_status_t *status, iwx_bss_t *bss, unsigned capacity);

enum iwx_link_state {
    IWX_LINK_IDLE, IWX_LINK_PREPARING, IWX_LINK_SESSION, IWX_LINK_AUTH,
    IWX_LINK_ASSOC, IWX_LINK_CONFIGURE, IWX_LINK_ASSOCIATED,
    IWX_LINK_FAILED, IWX_LINK_STOPPED
};

enum iwx_rx_reason {
    IWX_RX_ACCEPTED, IWX_RX_BOUNDS, IWX_RX_STATE, IWX_RX_STATUS,
    IWX_RX_CHANNEL, IWX_RX_AMSDU, IWX_RX_FORMAT, IWX_RX_FRAGMENT,
    IWX_RX_PEER, IWX_RX_DESTINATION, IWX_RX_CCMP, IWX_RX_IV,
    IWX_RX_REPLAY, IWX_RX_PLAIN_CIPHER, IWX_RX_LLC, IWX_RX_EAPOL_ADDRESS,
    IWX_RX_PORT, IWX_RX_DUPLICATE, IWX_RX_TKIP, IWX_RX_MIC, IWX_RX_OFFLOAD, IWX_RX_REASON_COUNT
};
#define IWX_LINK_TRACE_MAX 8U
/* Header metadata only. Never add payload, MACs, handshake nonces, MICs or keys.
 * addresses: bit 0 receiver=local, bit 1 transmitter=AP, bit 2 source=AP,
 * bit 3 group destination, bit 4 source=local, bit 5 source=SNAP prefix.
 * Packet numbers are public replay counters, not key material.
 * Candidates are observations, not authenticated/validated IP input. */
typedef struct {
    uint64_t time_ns;
    uint64_t pn, previous_pn;
    enum iwx_rx_reason reason;
    uint32_t status, dhcp_xid;
    uint16_t frame_control, frame_bytes, ether_type, header_bytes;
    uint16_t eapol_bytes, key_info, sequence, udp_source, udp_destination;
    uint8_t channel, flags, addresses, version, packet_type, key_slot;
} iwx_rx_sample_t;

typedef struct {
    uint64_t submitted_ns, completed_ns;
    uint32_t command_flags, rate, completion_rate;
    uint16_t queue, index, frame_bytes, byte_count, tb_bytes[3];
    uint16_t frame_control, key_info, status;
    uint8_t kind, version, rts_failures, ack_failures, bt_kills;
    bool completed, dma_match;
} iwx_tx_sample_t;

typedef struct {
    enum iwx_link_state state;
    uint8_t bssid[6], channel;
    bool rsn, port_open;
    uint16_t aid, peer_status, reason;
    uint64_t generation;
    unsigned tx_sent, tx_completed, tx_failed, rx_ignored, rx_malformed;
    bool queue_response;
    unsigned queue_bytes;
    uint16_t queue_id, queue_flags, queue_write_pointer, queue_reserved;
    unsigned eapol_received, eapol_dropped, key_installs;
    unsigned eapol_ingress_dropped;
    unsigned pmkid_mismatches;
    wpa2_rx_diagnostic_t eapol_last_drop;
    unsigned rx_reasons[IWX_RX_REASON_COUNT];
    unsigned rx_trace_count, rx_trace_next, rx_trace_overwritten;
    iwx_rx_sample_t rx_trace[IWX_LINK_TRACE_MAX];
    unsigned data_trace_count, data_trace_skipped;
    iwx_rx_sample_t data_trace[IWX_LINK_TRACE_MAX];
    unsigned eapol_tx_count, eapol_tx_next, eapol_tx_overwritten;
    iwx_tx_sample_t eapol_tx[IWX_LINK_TRACE_MAX];
    unsigned data_tx, data_tx_dropped, data_rx, data_rx_dropped;
    unsigned data_rate_flags, data_rate_kbps;
    const char *error;
} iwx_link_status_t;
_Static_assert(sizeof(iwx_link_status_t) <= 2048, "bounded link diagnostic snapshot");
/* Explicit 802.11 association to an observed BSSID. security must match the
 * advertised network (open or WPA2-PSK/CCMP); diagnostic association only.
 * The controlled port remains closed, including on open networks. */
const char *iwx_associate(const uint8_t bssid[6], bool rsn);
/* Associate and run WPA2 with an already-derived PMK. No password logging or
 * persistence. Caller wipes its PMK copy after return. NIC binding is separate. */
const char *iwx_connect_psk(const uint8_t bssid[6], const uint8_t pmk[32]);
const char *iwx_connect_open(const uint8_t bssid[6]);
/* Pin the scan identity used for PMK derivation; a changed SSID/security
 * observation is rejected before transmission. NULL PMK selects open Wi-Fi. */
const char *iwx_connect_network(const iwx_bss_t *network, const uint8_t *pmk);
/* Nonblocking Ethernet callbacks. recv requires a buffer of at least 1514
 * bytes; len is output only, matching the existing NIC contract. */
int iwx_send(const void *data, uint32_t len);
int iwx_recv(void *buf, uint32_t *len);
void iwx_get_mac(uint8_t mac[6]);
bool iwx_link_up(void);
int iwx_read_link(nic_link_state_t *state);
int iwx_link_status(iwx_link_status_t *status);
int iwx_stop(iwx_probe_report_t *report);
uint64_t iwx_retained_bytes(void);
/* Serialize kexec staging/handoff against probes and stop active/quarantined DMA.
 * A successful begin holds the driver lock until end (only on aborted kexec). */
int iwx_handoff_begin(void);
void iwx_handoff_end(void);

#endif
