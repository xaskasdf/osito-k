/*
 * Encoder for vkDeviceWaitIdle. Guest-local no-op — same rationale as
 * QueueWaitIdle. See master plan §W3b.5 T2.
 */
#include "venus.h"

int venus_cmd_encode_DeviceWaitIdle(struct venus_device *dev) {
    (void)dev;
    return 0;
}
