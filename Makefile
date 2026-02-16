#
# OsitoK v0.1 - Makefile
# Bare-metal preemptive kernel for ESP8266 (Wemos D1)
#

# Toolchain
TOOLCHAIN ?= xtensa-lx106-elf
CC      = $(TOOLCHAIN)-gcc
CXX     = $(TOOLCHAIN)-g++
AS      = $(TOOLCHAIN)-gcc
LD      = $(TOOLCHAIN)-gcc
OBJCOPY = $(TOOLCHAIN)-objcopy
OBJDUMP = $(TOOLCHAIN)-objdump
SIZE    = $(TOOLCHAIN)-size

# esptool
ESPTOOL ?= python -m esptool
PORT    ?= COM4
BAUD    ?= 460800

# Directories
SRCDIR   = src
INCDIR   = include
BUILDDIR = build
LDDIR    = ld

# Flash parameters (Wemos D1: 4MB, DOUT mode)
FLASH_MODE  = dout
FLASH_SIZE  = 4MB
FLASH_FREQ  = 40m

# Compiler flags
COMMON_FLAGS = \
	-mlongcalls \
	-mtext-section-literals \
	-nostdlib \
	-ffreestanding \
	-Os \
	-Wall -Wextra -Wno-unused-parameter \
	-I$(INCDIR) \
	-I$(SRCDIR) \
	-DICACHE_FLASH_ATTR='__attribute__((section(".irom0.text")))' \
	-DIRAM_ATTR='__attribute__((section(".iram0.text")))'

CFLAGS = $(COMMON_FLAGS) -std=c11
CXXFLAGS = $(COMMON_FLAGS) -std=c++17 -fno-exceptions -fno-rtti
ASFLAGS = -mlongcalls -mtext-section-literals -I$(INCDIR) -I$(SRCDIR)

LDFLAGS = \
	-mlongcalls \
	-nostdlib \
	-T$(LDDIR)/osito.ld \
	-L$(LDDIR) \
	-Wl,--no-check-sections \
	-Wl,--gc-sections \
	-Wl,-Map=$(BUILDDIR)/osito.map

# Source files
ASM_SRCS = \
	$(SRCDIR)/boot/vectors.S \
	$(SRCDIR)/boot/crt0.S \
	$(SRCDIR)/kernel/context_switch.S

C_SRCS = \
	$(SRCDIR)/boot/nosdk_init.c \
	$(SRCDIR)/kernel/timer_tick.c

CXX_SRCS = \
	$(SRCDIR)/kernel/sched.cpp \
	$(SRCDIR)/mem/pool_alloc.cpp \
	$(SRCDIR)/drivers/uart.cpp \
	$(SRCDIR)/shell/shell.cpp \
	$(SRCDIR)/main.cpp

# Object files
ASM_OBJS = $(patsubst $(SRCDIR)/%.S,$(BUILDDIR)/%.o,$(ASM_SRCS))
C_OBJS   = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(C_SRCS))
CXX_OBJS = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(CXX_SRCS))
OBJS     = $(ASM_OBJS) $(C_OBJS) $(CXX_OBJS)

# Output
ELF = $(BUILDDIR)/osito.elf
BIN = $(BUILDDIR)/osito.bin

# =============================================================================

.PHONY: all clean flash monitor dump size

all: $(BIN)
	@echo ""
	@echo "=== OsitoK build complete ==="
	@$(SIZE) $(ELF)

# Link
$(ELF): $(OBJS)
	@echo "  LD    $@"
	$(LD) $(LDFLAGS) -o $@ $^ -lgcc

# Generate flash binary using esptool
$(BIN): $(ELF)
	@echo "  BIN   $@"
	$(ESPTOOL) --chip esp8266 elf2image --flash_mode $(FLASH_MODE) --flash_size $(FLASH_SIZE) \
		--flash_freq $(FLASH_FREQ) --version 2 -o $@ $<

# Compile assembly
$(BUILDDIR)/%.o: $(SRCDIR)/%.S
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	$(AS) $(ASFLAGS) -c -o $@ $<

# Compile C
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c -o $@ $<

# Compile C++
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Flash
flash: $(BIN)
	$(ESPTOOL) --chip esp8266 --port $(PORT) --baud $(BAUD) write_flash \
		--flash_mode $(FLASH_MODE) --flash_size $(FLASH_SIZE) --flash_freq $(FLASH_FREQ) \
		0x00000 $(BIN)

# Serial monitor
monitor:
	python -m serial.tools.miniterm $(PORT) 115200

# Disassembly dump
dump: $(ELF)
	$(OBJDUMP) -d -S $< > $(BUILDDIR)/osito.dis

# Size info
size: $(ELF)
	$(SIZE) -A $<

# Clean
clean:
	rm -rf $(BUILDDIR)
