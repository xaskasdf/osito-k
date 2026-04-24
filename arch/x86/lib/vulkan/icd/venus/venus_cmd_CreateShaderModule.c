/*
 * Encoder for vkCreateShaderModule over venus.
 *
 * Request payload:
 *   dev_id                (u64)
 *   flags                 (u32)
 *   pad                   (u32)
 *   codeSize              (u64)    raw byte count
 *   code[]                (u32 *N) SPIR-V words, padded to 8 bytes
 *   pAllocator_present    (u32)  always 0
 *   pShaderModule_present (u32)  always 1
 *
 * Reply:
 *   VkResult              (u32)
 *   pad                   (u32)
 *   host VkShaderModule id (u64)
 *
 * See master plan §W3b.4 T1.
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);

#define VENUS_SHADER_MAX_BYTES 65536u

int venus_cmd_encode_CreateShaderModule(
        struct venus_wire *w, uint64_t dev_id,
        const VkShaderModuleCreateInfo *pCreateInfo,
        uint64_t *out_shader_id) {
    if (!w || !pCreateInfo || !out_shader_id) return -22;

    uint64_t code_bytes = pCreateInfo->codeSize;
    if (code_bytes > VENUS_SHADER_MAX_BYTES) return -22;

    /* Pad code section to 8-byte multiple. */
    uint64_t code_padded = (code_bytes + 7ull) & ~7ull;
    uint32_t payload = 8u + 4u + 4u + 8u + (uint32_t)code_padded + 4u + 4u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCreateShaderModule,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            payload, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;                      off += 8;
    *(uint32_t *)(p + off) = pCreateInfo->flags;          off += 4;
    *(uint32_t *)(p + off) = 0u;                          off += 4;
    *(uint64_t *)(p + off) = code_bytes;                  off += 8;
    if (code_bytes && pCreateInfo->pCode) {
        memcpy(p + off, pCreateInfo->pCode, code_bytes);
        if (code_padded > code_bytes)
            memset(p + off + code_bytes, 0, code_padded - code_bytes);
    } else {
        memset(p + off, 0, code_padded);
    }
    off += code_padded;
    *(uint32_t *)(p + off) = 0u; off += 4; /* pAllocator null */
    *(uint32_t *)(p + off) = 1u; off += 4; /* pShaderModule present */

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    *out_shader_id = *(uint64_t *)(reply + 8);
    return (int)vk_result;
}
