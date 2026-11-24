/*
 * arch/x86/kernel/cluster.h — osito-a cluster mode public surface.
 *
 * Layered on top of inferconnect.c's UDP-multicast peer discovery and
 * inferconnect_rpc.c's TCP RPC framing.  Adds: per-peer liveness state,
 * stale-eviction with epoch tracking, HMAC-PSK authenticated heartbeats,
 * and an optional HTTP rendezvous server for nodes that can't share a
 * multicast segment.  See docs/cluster.md / the plan file for design.
 */
#ifndef OSITOA_CLUSTER_H
#define OSITOA_CLUSTER_H

#include "../include/types.h"

typedef enum {
    PEER_UNKNOWN = 0,
    PEER_ALIVE,    /* heartbeat within 15s */
    PEER_STALE,    /* 15s ≤ silence < 30s */
    PEER_DEAD      /* ≥30s silence — slot reclaimable */
} peer_state_t;

typedef struct {
    bool         in_use;          /* slot mirrors a valid ic_peers[] entry */
    uint8_t      ip[4];
    uint16_t     rpc_port;
    peer_state_t state;
    uint64_t     last_seen_tick;          /* udp mcast or rpc reply */
    uint64_t     last_rpc_heartbeat_tick;
    uint32_t     rpc_heartbeat_sent;
    uint32_t     rpc_heartbeat_ok;
    uint32_t     rpc_heartbeat_authfail;

    /* Rope-arc allocation (V2 hook).  On any membership-change epoch
     * the surviving ALIVE peers are sorted by IP and given equal-
     * size arcs in [0, 2π).  Q16.16 fixed-point so we can stay in
     * the kernel without pulling libm in.  A future MoE backend or
     * pipeline-parallel layer scheduler picks "responsible peer" via
     * cluster_nearest_alive_peer(angle).  For dense brandon-tiny
     * these fields are computed but otherwise unused. */
    uint32_t     rope_arc_lo_q16;
    uint32_t     rope_arc_hi_q16;
} cluster_peer_meta_t;

typedef struct {
    uint64_t epoch;                /* bumps on any state transition */
    uint64_t last_transition_tick;
    char     rendezvous_url[160];  /* empty = no rendezvous client */
    bool     rendezvous_is_server; /* true = run /register endpoint */
    bool     debug_verbose;
    bool     psk_loaded;           /* false = HMAC verify rejects all */
    uint32_t hmac_rejects_total;
    uint32_t hmac_rejects_since_log;
} cluster_state_t;

/* ── Public API ─────────────────────────────────────────────── */

/* Read /cluster.json (best effort) + initialise meta[] table. */
void cluster_init(void);

/* Spawn cluster-tick kthread; optionally spawn rendezvous server. */
int  cluster_start(void);

/* Operator surface (shell). */
void cluster_dump_status(void);
int  cluster_render_peers(char *buf, uint32_t cap);
void cluster_dump_stats(void);
void cluster_force_reconfigure(void);
void cluster_set_rendezvous(const char *url);
void cluster_set_debug(bool on);

/* Write 32 random bytes to osfs2:/oict-key.txt; returns 0 on success. */
int  cluster_keygen(void);

/* ── RPC opcode handlers (called from inferconnect_rpc.c) ───── */

/* Returns 0 on accept (HMAC ok), -1 on auth fail / malformed.
 * Body wire-format: [u8 reserved | u64 sender_epoch | u32 mac_len | mac] */
int  cluster_handle_heartbeat(int conn, uint64_t in_size,
                              const uint8_t sender_ip[4]);
int  cluster_handle_info_req(int conn, uint64_t in_size);
int  cluster_handle_epoch_req(int conn, uint64_t in_size);

/* ── RPC opcode emitters (called from cluster_tick or shell) ── */

/* Open a one-shot TCP conn, send authenticated HEARTBEAT, wait ack. */
int  cluster_send_heartbeat(int peer_idx);
int  cluster_query_epoch(int peer_idx, uint64_t *epoch_out);

/* Read the cluster state head (for shell + tests). */
const cluster_state_t *cluster_state(void);
uint32_t               cluster_count_state(peer_state_t st);

/* Rope-arc routing.  Q16.16 fixed-point full circle = 2π ≈ 411775
 * (rounded).  Returns the peer index whose arc contains `angle_q16`,
 * or -1 if no ALIVE peer.  Used by future MoE / pipeline-parallel
 * routing; the framework recomputes arcs on every epoch advance. */
#define CLUSTER_ROPE_FULL_Q16 ((uint32_t)411775)
int  cluster_nearest_alive_peer(uint32_t angle_q16);
int  cluster_render_ring(char *buf, uint32_t cap);

/* RPC opcode numbers — kept in OsitoA-native range (>=200). */
#define RPC_CMD_OSITOA_CLUSTER_HEARTBEAT 205
#define RPC_CMD_OSITOA_CLUSTER_INFO      206
#define RPC_CMD_OSITOA_CLUSTER_EPOCH     207

#endif
