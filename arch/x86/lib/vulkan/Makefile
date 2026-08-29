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
CC_MACHINE := $(shell $(CC) -dumpmachine 2>/dev/null)
ifeq ($(or $(findstring elf,$(CC_MACHINE)),$(findstring linux,$(CC_MACHINE))),)
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
  loader/loader_dispatch_real.c \
  loader/syscall_compat.c \
  icd/venus/venus_instance_real.c \
  icd/venus/venus_physical_device_real.c \
  icd/venus/venus_device_real.c \
  icd/venus/venus_descriptor_real.c \
  icd/venus/venus_command_real.c \
  icd/venus/venus_resource_real.c \
  icd/venus/venus_pipeline_real.c \
  icd/venus/venus_surface_real.c \
  icd/venus/venus_entry_real.c \
  icd/venus/venus_wire.c

OBJ := $(patsubst %.c,$(BUILD_ROOT)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

-include $(DEP)

.PHONY: all clean
all: $(OUT)

$(BUILD_ROOT):
	@mkdir -p $(BUILD_ROOT)/loader $(BUILD_ROOT)/icd/venus

$(BUILD_ROOT)/%.o: %.c | $(BUILD_ROOT)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT): $(OBJ)
	@rm -f $@
	$(AR) rcs $@ $(OBJ)

clean:
	@rm -rf $(BUILD_ROOT)
