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
else ifeq ($(PLATFORM),sm8350)
  LDSCRIPT := boot/linker_sm8350.ld
else
  $(error Unknown PLATFORM=$(PLATFORM). Use virt or sm8350.)
endif

# SCREEN_DEBUG=1: the SM8350 screen boots in debug mode (start.S markers,
# progress bars, heartbeat squares, the console from pixel row 700); 0, the
# console on the whole panel. The shell's `screen` switches it at runtime.
SCREEN_DEBUG ?= 0

# Compiler flags. The kernel is a PIE linked at 0 that relocates itself in
# start.S (R_AARCH64_RELATIVE only): ABL loads it ~0x20180000 above where
# a fixed link address would put it, and kload runs it wherever it lands.
CFLAGS := -ffreestanding -nostdlib -nostartfiles -fpie \
          -march=armv8.2-a -mtune=cortex-a78 -O2 \
          -mgeneral-regs-only \
          -Wall -Wextra -Wno-unused-parameter \
          -Iinclude -I$(PLAT_DIR) -I../../gui -D__KERNEL_ARM__=1 \
          -DSCREEN_DEBUG=$(SCREEN_DEBUG)

ASFLAGS := -ffreestanding -nostdlib -nostartfiles \
           -march=armv8.2-a -D__ASSEMBLY__ -DSCREEN_DEBUG=$(SCREEN_DEBUG)

LDFLAGS := -nostdlib -pie --no-dynamic-linker -z text -T $(LDSCRIPT)

# Sources
ASM_SRCS := boot/start.S kernel/context_switch.S kernel/mem.S
C_SRCS   := kernel/main.c kernel/serial.c kernel/gic.c kernel/timer.c \
            kernel/memory.c kernel/paging.c kernel/heap.c kernel/sched.c \
            kernel/syscall.c kernel/elf.c kernel/process.c \
            kernel/framebuffer.c kernel/term.c kernel/shell.c kernel/fs.c kernel/gui_task.c \
            kernel/pci.c kernel/net.c kernel/dhcp.c \
            kernel/crypto.c kernel/tls.c kernel/http.c kernel/smp.c \
            kernel/claude.c kernel/tokenizer.c kernel/tokenizer_trie.c \
            kernel/tensor.c kernel/tensor_neon.c kernel/tensor_arena.c \
            kernel/inference.c kernel/dispatch.c kernel/llm.c \
            kernel/git.c kernel/zlib.c kernel/memcompress.c kernel/input_events.c \
            kernel/stubs.c \
            drivers/hda.c drivers/virtio_blk.c drivers/virtio_net.c \
            drivers/gpu.c drivers/gsp.c drivers/sass.c drivers/gmmu.c \
            drivers/gpu_tensor.c drivers/gpu_inference.c drivers/gpu_display.c \
            fs/ositofs2.c fs/ositofs3.c fs/gguf.c fs/gpt.c \
            $(wildcard $(PLAT_DIR)/*.c)
GUI_SRCS := ../../gui/gui_draw.c ../../gui/gui_text.c \
            ../../gui/gui_panel.c ../../gui/gui_dock.c \
            ../../gui/gui_desktop.c ../../gui/gui_window.c \
            ../../gui/gui_anim.c ../../gui/gui_theme.c ../../gui/gui_font.c \
            ../../gui/gui_widgets.c

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

# Float-using files need FP registers enabled (no -mgeneral-regs-only). The
# scheduler saves no FP/SIMD state, so their code must stay on one CPU that
# no other FP user shares (see kernel/llm.c).
FP_CFLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS))
FP_SRCS   := kernel/tensor.c kernel/tensor_neon.c kernel/inference.c \
             kernel/tokenizer.c kernel/llm.c fs/gguf.c \
             drivers/gpu_inference.c drivers/gpu_tensor.c
$(patsubst %.c,$(BUILD)/%.o,$(FP_SRCS)): $(BUILD)/%.o: %.c
	@echo "[CC] $< (FP)"
	@mkdir -p $(dir $@)
	@$(CC) $(FP_CFLAGS) -c $< -o $@

# GUI shared library
$(BUILD)/gui/%.o: ../../gui/%.c ../../gui/gui.h ../../gui/gui_theme.h
	@echo "[CC] $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/gui/gui_font.o: ../../gui/gui_font_data.h

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
