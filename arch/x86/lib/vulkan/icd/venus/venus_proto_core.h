/*
 * venus_proto_core.h — Mesa venus-protocol command IDs we use.
 *
 * Opcodes come from src/virtio/venus-protocol in Mesa tag mesa-25.0.0.
 * We hard-code only the ones we actively encode. Keep this list in
 * lockstep with any encoder sources we ship.
 */
#ifndef OSITOK_VENUS_PROTO_CORE_H
#define OSITOK_VENUS_PROTO_CORE_H

/* W3b.1 ship list. More opcodes arrive in W3b.2..W3b.6. */
#define VN_CMD_vkCreateInstance   0x7FFFFFF1u
#define VN_CMD_vkDestroyInstance  0x7FFFFFF2u

/* W3b.2 additions — physical-device query opcodes.
 * Values are our best-effort match for Mesa venus-protocol tag mesa-25.0.0.
 * Keep in lockstep if/when the Mesa table bumps. */
#define VN_CMD_vkGetPhysicalDeviceProperties            0x7FFFFFF3u
#define VN_CMD_vkGetPhysicalDeviceFeatures              0x7FFFFFF4u
#define VN_CMD_vkGetPhysicalDeviceQueueFamilyProperties 0x7FFFFFF5u
#define VN_CMD_vkGetPhysicalDeviceMemoryProperties      0x7FFFFFF6u

#endif
