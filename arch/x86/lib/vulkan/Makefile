# Build libvulkan.a — OsitoK SDK static archive.
#
# Invoked from arch/x86/Makefile. Expects OSITOK_ROOT to be set by parent,
# but defaults to a relative layout so `make -C lib/vulkan` also works.

VULKAN_ROOT := $(CURDIR)
BUILD_ROOT  ?= $(VULKAN_ROOT)/../../build/lib/vulkan
OUT         := $(BUILD_ROOT)/libvulkan.a

CC       ?= gcc
AR       ?= ar
GCC_INC  := $(shell $(CC) -print-file-name=include)
CFLAGS   := -std=gnu99 -ffreestanding -nostdinc -fno-stack-protector \
            -mno-red-zone -Wall -Wextra -O2 -fno-pic \
            -I$(VULKAN_ROOT)/include -I$(VULKAN_ROOT)/loader \
            -I$(VULKAN_ROOT)/../../include \
            -isystem $(GCC_INC)

SRC := \
  loader/icd_table.c \
  loader/loader_instance.c \
  loader/loader_enum.c \
  loader/loader_dispatch.c \
  icd/nvk_stub/nvk_stub.c \
  icd/venus/venus_instance.c \
  icd/venus/venus_device.c \
  icd/venus/venus_entry.c \
  icd/venus/venus_wire.c \
  icd/venus/venus_cmd_CreateInstance.c \
  icd/venus/venus_cmd_GetPhysicalDeviceProperties.c \
  icd/venus/venus_cmd_GetPhysicalDeviceFeatures.c \
  icd/venus/venus_cmd_GetPhysicalDeviceQueueFamilyProperties.c \
  icd/venus/venus_cmd_GetPhysicalDeviceMemoryProperties.c

OBJ := $(patsubst %.c,$(BUILD_ROOT)/%.o,$(SRC))

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
