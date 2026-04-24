/*
 * Encoder for vkUnmapMemory over venus.
 *
 * Request payload:
 *   dev_id  (u64)
 *   mem_id  (u64)
 *
 * No reply expected. In W3b.3 guest-side Unmap is a no-op at the ICD
 * level (see venus_device.c::venus_UnmapMemory); the wire command is
 * still issued so the host side can drop its own mapping reference. */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_UnmapMemory(struct venus_wire *w,
                                 uint64_t dev_id, uint64_t mem_id) {
    if (!w) return -22;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkUnmapMemory,
            0u /* no reply */,
            8u + 8u,
            &reply_id);
    if (!p) return -12;

    *(uint64_t *)(p + 0) = dev_id;
    *(uint64_t *)(p + 8) = mem_id;

    return venus_wire_submit(w);
}
