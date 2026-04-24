/*
 * Encoder for vkQueueWaitIdle. Guest-local no-op: with no wire
 * forwarding of submits, the queue is always idle by the time the
 * app can observe it.
 *
 * See master plan §W3b.5 T2.
 */
#include "venus.h"
#include "venus_wire.h"
#include "venus_proto_core.h"

int venus_cmd_encode_QueueWaitIdle(struct venus_device *dev) {
    (void)dev;
    return 0;
}
