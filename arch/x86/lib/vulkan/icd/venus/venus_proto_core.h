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

/* W3b.3 additions — device + memory + buffer opcodes.
 * Same Mesa-25.0.0 caveat as W3b.2. */
#define VN_CMD_vkCreateDevice                           0x7FFFFFF7u
#define VN_CMD_vkDestroyDevice                          0x7FFFFFF8u
#define VN_CMD_vkAllocateMemory                         0x7FFFFFF9u
#define VN_CMD_vkFreeMemory                             0x7FFFFFFAu
#define VN_CMD_vkMapMemory                              0x7FFFFFFBu
#define VN_CMD_vkUnmapMemory                            0x7FFFFFFCu
#define VN_CMD_vkCreateBuffer                           0x7FFFFFFDu
#define VN_CMD_vkDestroyBuffer                          0x7FFFFFFEu
#define VN_CMD_vkGetBufferMemoryRequirements            0x7FFFFFFFu
#define VN_CMD_vkBindBufferMemory                       0x80000000u

/* W3b.4 additions — shader + render pass + image + framebuffer + pipeline
 * + command buffer opcodes. Same Mesa-25.0.0 caveat. */
#define VN_CMD_vkCreateShaderModule                     0x80000001u
#define VN_CMD_vkDestroyShaderModule                    0x80000002u
#define VN_CMD_vkCreateRenderPass                       0x80000003u
#define VN_CMD_vkDestroyRenderPass                      0x80000004u
#define VN_CMD_vkCreateImage                            0x80000005u
#define VN_CMD_vkDestroyImage                           0x80000006u
#define VN_CMD_vkCreateImageView                        0x80000007u
#define VN_CMD_vkDestroyImageView                       0x80000008u
#define VN_CMD_vkCreateFramebuffer                      0x80000009u
#define VN_CMD_vkDestroyFramebuffer                     0x8000000Au
#define VN_CMD_vkCreatePipelineLayout                   0x8000000Bu
#define VN_CMD_vkDestroyPipelineLayout                  0x8000000Cu
#define VN_CMD_vkCreateGraphicsPipelines                0x8000000Du
#define VN_CMD_vkDestroyPipeline                        0x8000000Eu
#define VN_CMD_vkCreateCommandPool                      0x8000000Fu
#define VN_CMD_vkDestroyCommandPool                     0x80000010u
#define VN_CMD_vkAllocateCommandBuffers                 0x80000011u
#define VN_CMD_vkFreeCommandBuffers                     0x80000012u
#define VN_CMD_vkBeginCommandBuffer                     0x80000013u
#define VN_CMD_vkEndCommandBuffer                       0x80000014u
#define VN_CMD_vkCmdBeginRenderPass                     0x80000015u
#define VN_CMD_vkCmdEndRenderPass                       0x80000016u
#define VN_CMD_vkCmdBindPipeline                        0x80000017u
#define VN_CMD_vkCmdDraw                                0x80000018u
#define VN_CMD_vkGetImageMemoryRequirements             0x80000019u
#define VN_CMD_vkBindImageMemory                        0x8000001Au

/* W3b.5 additions — queue + sync + WSI. Same Mesa-25.0.0 caveat.
 * The WSI opcodes (swapchain + present) are guest-local today; the
 * values are reserved so future wire forwarding can reuse them. */
#define VN_CMD_vkGetDeviceQueue                         0x8000001Bu
#define VN_CMD_vkQueueSubmit                            0x8000001Cu
#define VN_CMD_vkQueueWaitIdle                          0x8000001Du
#define VN_CMD_vkDeviceWaitIdle                         0x8000001Eu
#define VN_CMD_vkCreateFence                            0x8000001Fu
#define VN_CMD_vkDestroyFence                           0x80000020u
#define VN_CMD_vkResetFences                            0x80000021u
#define VN_CMD_vkWaitForFences                          0x80000022u
#define VN_CMD_vkGetFenceStatus                         0x80000023u
#define VN_CMD_vkCreateSemaphore                        0x80000024u
#define VN_CMD_vkDestroySemaphore                       0x80000025u
/* WSI — guest-local only in W3b.5. */
#define VN_CMD_vkCreateSwapchainKHR                     0x80000026u
#define VN_CMD_vkDestroySwapchainKHR                    0x80000027u
#define VN_CMD_vkGetSwapchainImagesKHR                  0x80000028u
#define VN_CMD_vkAcquireNextImageKHR                    0x80000029u
#define VN_CMD_vkQueuePresentKHR                        0x8000002Au

#endif
