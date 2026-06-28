/*
 * Encoder for vkGetPhysicalDeviceFeatures over the Mesa venus protocol.
 *
 * The older local wire command used OsitoK-private command ids and never
 * reached virglrenderer's generated decoder.  The real Venus stream uses
 * command type 3, a 64-bit "simple pointer" marker for pFeatures, and a
 * reply of cmd + pointer marker + VkPhysicalDeviceFeatures.
 */
#include "venus_wire.h"
#include "venus.h"

extern void *memcpy(void *, const void *, unsigned long);
extern int printf(const char *, ...);

#define VN_CMD_TYPE_vkGetPhysicalDeviceFeatures 3u
#define VN_CMD_GENERATE_REPLY 1u

int venus_cmd_encode_GetPhysicalDeviceFeatures(
        struct venus_wire *w, uint64_t pd_id,
        VkPhysicalDeviceFeatures *out) {
    if (!w || !out) return -22;

    uint8_t cmd[24];
    uint8_t reply[12 + sizeof(VkPhysicalDeviceFeatures)];
    uint32_t cmd_type = VN_CMD_TYPE_vkGetPhysicalDeviceFeatures;
    uint32_t flags = VN_CMD_GENERATE_REPLY;
    uint64_t present = 1;

    memcpy(cmd + 0,  &cmd_type, 4);
    memcpy(cmd + 4,  &flags, 4);
    memcpy(cmd + 8,  &pd_id, 8);
    memcpy(cmd + 16, &present, 8);

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;

    int got = venus_wire_submit_reply(w, cmd, sizeof(cmd),
                                      reply, sizeof(reply));
    if (got < (int)sizeof(reply))
        return got < 0 ? got : -5;

    uint32_t reply_cmd = 0;
    uint64_t reply_present = 0;
    memcpy(&reply_cmd, reply + 0, 4);
    memcpy(&reply_present, reply + 4, 8);
    if (reply_cmd != VN_CMD_TYPE_vkGetPhysicalDeviceFeatures ||
        !reply_present)
        return -5;

    memcpy(out, reply + 12, sizeof(*out));
    printf("[VGF] host features geom=%u tess=%u anis=%u bc=%u int64=%u f64=%u\n",
           out->geometryShader, out->tessellationShader,
           out->samplerAnisotropy, out->textureCompressionBC,
           out->shaderInt64, out->shaderFloat64);
    return 0;
}
