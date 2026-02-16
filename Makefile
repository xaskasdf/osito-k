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

# Python + esptool (use Windows py launcher by default)
PYTHON  ?= py
ESPTOOL  = $(PYTHON) -m esptool
PORT    ?= COM4
BAUD    ?= 460800

# Directories
SRCDIR   = src
INCDIR   = include
BUILDDIR = build
LDDIR    = ld

# Flash parameters (Wemos D1: 4MB, DOUT mode, image v1)
FLASH_MODE  = dout
FLASH_SIZE  = 4MB
FLASH_FREQ  = 40m
IMAGE_VER   = 1

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
	$(SRCDIR)/kernel/sem.cpp \
	$(SRCDIR)/kernel/mq.cpp \
	$(SRCDIR)/kernel/timer_sw.cpp \
	$(SRCDIR)/mem/pool_alloc.cpp \
	$(SRCDIR)/drivers/uart.cpp \
	$(SRCDIR)/shell/shell.cpp \
	$(SRCDIR)/main.cpp

# Object files
ASM_OBJS = $(patsubst $(SRCDIR)/%.S,$(BUILDDIR)/%.o,$(ASM_SRCS))
C_OBJS   = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(C_SRCS))
CXX_OBJS = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(CXX_SRCS))
OBJS     = $(ASM_OBJS) $(C_OBJS) $(CXX_OBJS)

# Output files
# esptool elf2image with -o build/osito produces build/osito0x00000.bin
ELF     = $(BUILDDIR)/osito.elf
BIN_PFX = $(BUILDDIR)/osito
BIN     = $(BIN_PFX)0x00000.bin

# =============================================================================

.PHONY: all clean flash monitor dump size

all: $(BIN)
	@echo ""
	@echo "=== OsitoK build complete ==="
	@$(SIZE) $(ELF)

# Link
$(ELF): $(OBJS)
	@echo "  LD    $@"
	@$(LD) $(LDFLAGS) -o $@ $^ -lgcc

# Generate flash binary using esptool
$(BIN): $(ELF)
	@echo "  BIN   $@"
	@$(ESPTOOL) --chip esp8266 elf2image \
		--flash-mode $(FLASH_MODE) --flash-size $(FLASH_SIZE) \
		--flash-freq $(FLASH_FREQ) --version $(IMAGE_VER) \
		-o $(BIN_PFX) $<

# Compile assembly
$(BUILDDIR)/%.o: $(SRCDIR)/%.S
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	@$(AS) $(ASFLAGS) -c -o $@ $<

# Compile C
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Compile C++
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@$(CXX) $(CXXFLAGS) -c -o $@ $<

# Flash to board
flash: $(BIN)
	$(ESPTOOL) --chip esp8266 --port $(PORT) --baud $(BAUD) write-flash \
		--flash-mode $(FLASH_MODE) --flash-size $(FLASH_SIZE) --flash-freq $(FLASH_FREQ) \
		0x00000 $(BIN)

# Serial console
monitor:
	$(PYTHON) tools/console.py $(PORT)

# Disassembly dump
dump: $(ELF)
	$(OBJDUMP) -d -S $< > $(BUILDDIR)/osito.dis

# Size info
size: $(ELF)
	$(SIZE) -A $<

# Clean
clean:
	rm -rf $(BUILDDIR)
