#
# Makefile -- Osito-K AArch64 build system
#
# Usage:
#   make                     # default: PLATFORM=virt
#   make PLATFORM=sm8350     # SM8350 (ROG Phone 5)
#   make run                 # build + launch QEMU
#   make clean
#

PLATFORM ?= virt

# Toolchain
CROSS   := aarch64-linux-gnu-
CC      := $(CROSS)gcc
AS      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

# Directories
BUILD   := build
PLAT_DIR := platform/$(PLATFORM)

# Platform-specific linker script
ifeq ($(PLATFORM),virt)
  LDSCRIPT := boot/linker_virt.ld
  STACK_ADDR := 0x44000000
else ifeq ($(PLATFORM),sm8350)
  LDSCRIPT := boot/linker_sm8350.ld
  STACK_ADDR := 0x85000000
else
  $(error Unknown PLATFORM=$(PLATFORM). Use virt or sm8350.)
endif

# Compiler flags
CFLAGS := -ffreestanding -nostdlib -nostartfiles \
          -march=armv8.2-a -mtune=cortex-a78 -O2 \
          -mgeneral-regs-only \
          -Wall -Wextra -Wno-unused-parameter \
          -Iinclude -I$(PLAT_DIR) -I../../gui -D__KERNEL_ARM__=1

ASFLAGS := -ffreestanding -nostdlib -nostartfiles \
           -march=armv8.2-a -D__ASSEMBLY__

LDFLAGS := -nostdlib -T $(LDSCRIPT)

# Sources
ASM_SRCS := boot/start.S kernel/context_switch.S
C_SRCS   := kernel/main.c kernel/serial.c kernel/gic.c kernel/timer.c kernel/heap.c kernel/sched.c \
            kernel/framebuffer.c kernel/term.c kernel/shell.c kernel/gui_task.c \
            $(wildcard $(PLAT_DIR)/*.c)
GUI_SRCS := ../../gui/gui_draw.c ../../gui/gui_text.c \
            ../../gui/gui_panel.c ../../gui/gui_dock.c \
            ../../gui/gui_desktop.c ../../gui/gui_window.c

# Objects
ASM_OBJS := $(patsubst %.S,$(BUILD)/%.o,$(ASM_SRCS))
C_OBJS   := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
GUI_OBJS := $(patsubst ../../gui/%.c,$(BUILD)/gui/%.o,$(GUI_SRCS))
OBJS     := $(ASM_OBJS) $(C_OBJS) $(GUI_OBJS)

# Outputs
ELF   := $(BUILD)/kernel.elf
IMAGE := $(BUILD)/Image

# ========================================================================
# Targets
# ========================================================================

.PHONY: all clean run

all: $(IMAGE)
	@echo "[OK] $(IMAGE) ($(PLATFORM))"
	@ls -la $(IMAGE)

$(IMAGE): $(ELF)
	@echo "[OBJCOPY] $@"
	@$(OBJCOPY) -O binary $< $@

$(ELF): $(OBJS) $(LDSCRIPT)
	@echo "[LD] $@"
	@mkdir -p $(dir $@)
	@$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Assembly
$(BUILD)/%.o: %.S
	@echo "[AS] $<"
	@mkdir -p $(dir $@)
	@$(AS) $(ASFLAGS) -c $< -o $@

# C sources
$(BUILD)/%.o: %.c
	@echo "[CC] $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# GUI shared library
$(BUILD)/gui/%.o: ../../gui/%.c
	@echo "[CC] $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

# ========================================================================
# QEMU run target (virt only)
# ========================================================================

run: all
ifeq ($(PLATFORM),virt)
	@bash scripts/qemu-test.sh --no-build
else
	@echo "Run target only supported for PLATFORM=virt"
	@exit 1
endif
