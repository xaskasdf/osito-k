# Build libvulkan.a — OsitoK SDK static archive.
#
# Invoked from arch/x86/Makefile. Expects OSITOK_ROOT to be set by parent,
# but defaults to a relative layout so `make -C lib/vulkan` also works.

VULKAN_ROOT := $(CURDIR)
BUILD_ROOT  ?= $(VULKAN_ROOT)/../../build/lib/vulkan
OUT         := $(BUILD_ROOT)/libvulkan.a
.DEFAULT_GOAL := all

CC       ?= gcc
AR       ?= ar
GCC_INC  := $(shell $(CC) -print-file-name=include)

# Refuse to build with a host compiler — produces wrong object format
# (Mach-O on macOS) which the cross-ld silently drops, leaving NULL
# function pointers in the ICD table. CC must be the x86_64-elf-* cross.
ifeq ($(findstring elf,$(shell $(CC) -dumpmachine 2>/dev/null)),)
$(error CC=$(CC) is not a freestanding ELF cross-compiler. \
        Set CC=x86_64-elf-gcc (or equivalent). Building with a host \
        compiler silently produces unusable object files — see the \
        nvk_stub Mach-O bug fixed Apr 28.)
endif
CFLAGS   := -std=gnu99 -ffreestanding -nostdinc -fno-stack-protector \
            -mno-red-zone -Wall -Wextra -O2 -fno-pic \
            -I$(VULKAN_ROOT)/include -I$(VULKAN_ROOT)/loader \
            -I$(VULKAN_ROOT)/../../include \
            -isystem $(GCC_INC) \
            -MMD -MP

SRC := \
  loader/icd_table.c \
  loader/loader_instance.c \
  loader/loader_enum.c \
  loader/loader_dispatch.c \
  icd/nvk_stub/nvk_stub.c \
  icd/venus/venus_instance.c \
  icd/venus/venus_device.c \
  icd/venus/venus_w3b6_objects.c \
  icd/venus/venus_w3b5_objects.c \
  icd/venus/venus_w3b4_objects.c \
  icd/venus/venus_entry.c \
  icd/venus/venus_wire.c \
  icd/venus/venus_cmd_CreateInstance.c \
  icd/venus/venus_cmd_GetPhysicalDeviceProperties.c \
  icd/venus/venus_cmd_GetPhysicalDeviceFeatures.c \
  icd/venus/venus_cmd_GetPhysicalDeviceQueueFamilyProperties.c \
  icd/venus/venus_cmd_GetPhysicalDeviceMemoryProperties.c \
  icd/venus/venus_cmd_CreateDevice.c \
  icd/venus/venus_cmd_DestroyDevice.c \
  icd/venus/venus_cmd_AllocateMemory.c \
  icd/venus/venus_cmd_FreeMemory.c \
  icd/venus/venus_cmd_MapMemory.c \
  icd/venus/venus_cmd_UnmapMemory.c \
  icd/venus/venus_cmd_CreateBuffer.c \
  icd/venus/venus_cmd_DestroyBuffer.c \
  icd/venus/venus_cmd_GetBufferMemoryRequirements.c \
  icd/venus/venus_cmd_BindBufferMemory.c \
  icd/venus/venus_cmd_CreateShaderModule.c \
  icd/venus/venus_cmd_DestroyShaderModule.c \
  icd/venus/venus_cmd_CreateRenderPass.c \
  icd/venus/venus_cmd_DestroyRenderPass.c \
  icd/venus/venus_cmd_CreateImage.c \
  icd/venus/venus_cmd_DestroyImage.c \
  icd/venus/venus_cmd_CreateImageView.c \
  icd/venus/venus_cmd_DestroyImageView.c \
  icd/venus/venus_cmd_GetImageMemoryRequirements.c \
  icd/venus/venus_cmd_BindImageMemory.c \
  icd/venus/venus_cmd_CreateFramebuffer.c \
  icd/venus/venus_cmd_DestroyFramebuffer.c \
  icd/venus/venus_cmd_DescriptorObjects.c \
  icd/venus/venus_cmd_CreateDescriptorSetLayout.c \
  icd/venus/venus_cmd_DestroyDescriptorSetLayout.c \
  icd/venus/venus_cmd_CreatePipelineLayout.c \
  icd/venus/venus_cmd_DestroyPipelineLayout.c \
  icd/venus/venus_cmd_CreateGraphicsPipelines.c \
  icd/venus/venus_cmd_DestroyPipeline.c \
  icd/venus/venus_cmd_CreateCommandPool.c \
  icd/venus/venus_cmd_DestroyCommandPool.c \
  icd/venus/venus_cmd_AllocateCommandBuffers.c \
  icd/venus/venus_cmd_FreeCommandBuffers.c \
  icd/venus/venus_cmd_BeginCommandBuffer.c \
  icd/venus/venus_cmd_EndCommandBuffer.c \
  icd/venus/venus_cmd_CmdBeginRenderPass.c \
  icd/venus/venus_cmd_CmdEndRenderPass.c \
  icd/venus/venus_cmd_CmdBeginRendering.c \
  icd/venus/venus_cmd_CmdEndRendering.c \
  icd/venus/venus_cmd_CmdBindPipeline.c \
  icd/venus/venus_cmd_CmdDraw.c \
  icd/venus/venus_cmd_CreateOsitokCompositorSurface.c \
  icd/venus/venus_cmd_DestroySurface.c \
  icd/venus/venus_cmd_GetDeviceQueue.c \
  icd/venus/venus_cmd_QueueSubmit.c \
  icd/venus/venus_cmd_QueueWaitIdle.c \
  icd/venus/venus_cmd_DeviceWaitIdle.c \
  icd/venus/venus_cmd_CreateFence.c \
  icd/venus/venus_cmd_DestroyFence.c \
  icd/venus/venus_cmd_ResetFences.c \
  icd/venus/venus_cmd_WaitForFences.c \
  icd/venus/venus_cmd_GetFenceStatus.c \
  icd/venus/venus_cmd_CreateSemaphore.c \
  icd/venus/venus_cmd_DestroySemaphore.c \
  icd/venus/venus_cmd_CreateSwapchainKHR.c \
  icd/venus/venus_cmd_DestroySwapchainKHR.c \
  icd/venus/venus_cmd_GetSwapchainImagesKHR.c \
  icd/venus/venus_cmd_AcquireNextImageKHR.c \
  icd/venus/venus_cmd_QueuePresentKHR.c \
  icd/venus/venus_cmd_GetPhysicalDeviceSurfaceCapabilitiesKHR.c \
  icd/venus/venus_cmd_GetPhysicalDeviceSurfaceFormatsKHR.c \
  icd/venus/venus_cmd_GetPhysicalDeviceSurfacePresentModesKHR.c \
  icd/venus/venus_cmd_GetPhysicalDeviceSurfaceSupportKHR.c \
  icd/venus/venus_cmd_CmdBindVertexBuffers.c \
  icd/venus/venus_cmd_CmdSetViewport.c \
  icd/venus/venus_cmd_CmdSetScissor.c \
  icd/venus/venus_cmd_present_software_raster.c

OBJ := $(patsubst %.c,$(BUILD_ROOT)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

-include $(DEP)

.PHONY: all clean
all: $(OUT)

$(BUILD_ROOT):
	@mkdir -p $(BUILD_ROOT)/loader $(BUILD_ROOT)/icd/nvk_stub $(BUILD_ROOT)/icd/venus

$(BUILD_ROOT)/%.o: %.c | $(BUILD_ROOT)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT): $(OBJ)
	$(AR) rcs $@ $(OBJ)

clean:
	@rm -rf $(BUILD_ROOT)
