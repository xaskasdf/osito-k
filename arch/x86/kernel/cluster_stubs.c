/*
 * cluster_stubs.c — weak stubs for the agentic / OICT pipeline-parallel
 * symbols that inferconnect_rpc.c and cluster.c link against.
 *
 * osito-k's cluster client doesn't run brandon-tiny pipeline
 * parallelism (yet) — only the discovery protocol (HELLO +
 * DEVICE_COUNT + GET_DEVICE_MEMORY). These stubs let the
 * inferconnect server side compile + link cleanly. If a remote
 * peer ever sends us an OSITOA_TOKENS / LOGITS / HIDDEN /
 * BRANDON_BUNDLE op or an AGENT_TASK delegation, we just
 * return -1 and the peer falls back to local compute.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);

/* OICT pipeline-parallel handlers (RPC_CMD_OSITOA_TOKENS/LOGITS/HIDDEN/
 * BRANDON_BUNDLE). osito-a runs distributed inference across these;
 * osito-k v1 declines and lets the peer keep the layer local. */
int oict_handle_tokens(int conn,
                       const uint8_t *payload, uint32_t payload_size)
{
    (void)conn; (void)payload; (void)payload_size;
    serial_puts("[OICT] tokens handler: not implemented\n");
    return -1;
}

int oict_handle_logits(int conn,
                       const uint8_t *payload, uint32_t payload_size)
{
    (void)conn; (void)payload; (void)payload_size;
    serial_puts("[OICT] logits handler: not implemented\n");
    return -1;
}

int oict_handle_hidden(int conn,
                       const uint8_t *payload, uint32_t payload_size)
{
    (void)conn; (void)payload; (void)payload_size;
    serial_puts("[OICT] hidden handler: not implemented\n");
    return -1;
}

int oict_handle_brandon_bundle(int conn,
                               const uint8_t *payload, uint32_t payload_size)
{
    (void)conn; (void)payload; (void)payload_size;
    serial_puts("[OICT] brandon_bundle handler: not implemented\n");
    return -1;
}

/* HMAC-shared-key derivation. Used by cluster.c handshake; we don't
 * run cluster auth yet so return failure → caller refuses unknown
 * peer (safe default). */
int oict_get_shared_key(uint8_t key_out[32])
{
    (void)key_out;
    return -1;
}

/* Agent slot RPC — cross-node task delegation. osito-k has no
 * agent infrastructure yet; refuse all submissions. */
int agent_submit_slot(int conn, const void *task, uint32_t task_size,
                      uint32_t *slot_out)
{
    (void)conn; (void)task; (void)task_size; (void)slot_out;
    return -1;
}

int agent_slot_wait(uint32_t slot, uint32_t timeout_ticks)
{
    (void)slot; (void)timeout_ticks;
    return -1;
}

int agent_read_response_slot(uint32_t slot, void *buf, uint32_t cap,
                             uint32_t *out_size)
{
    (void)slot; (void)buf; (void)cap; (void)out_size;
    return -1;
}
