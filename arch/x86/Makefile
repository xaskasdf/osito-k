# OsitoK x86-64 — UEFI Application Build
#
# Builds an EFI binary using gnu-efi.
#
# Prerequisites:
#   sudo apt install gnu-efi gcc
#
# Usage:
#   make              Build ositok.efi
#   make clean        Clean build artifacts
#   make install      Copy to ESP (requires sudo)
#
# The EFI binary can be chainloaded from GRUB:
#   chainloader /EFI/ositok/ositok.efi

CC       = gcc
LD       = ld
OBJCOPY  = objcopy

# gnu-efi paths (Debian/Ubuntu)
EFI_INC  = /usr/include/efi
EFI_INC_ARCH = /usr/include/efi/x86_64
EFI_LIB  = /usr/lib
EFI_CRT  = /usr/lib/crt0-efi-x86_64.o
EFI_LDS  = /usr/lib/elf_x86_64_efi.lds

# Compiler flags
CFLAGS   = -ffreestanding -fno-stack-protector -fshort-wchar \
           -mno-red-zone -Wall -Wextra -O2 \
           -I$(EFI_INC) -I$(EFI_INC_ARCH) -Iinclude \
           -I../../include/common \
           -DGNU_EFI_USE_MS_ABI -fpic

# Separate flags for kernel files (no EFI headers)
KCFLAGS  = -std=c11 -ffreestanding -fno-stack-protector -mno-red-zone \
           -Wall -Wextra -O2 -fPIE -fno-jump-tables -Iinclude -I../../include/common \
           -D__KERNEL_X86__=1

# AVX2/FMA flags for tensor_avx2.c only (runtime CPUID dispatch in tensor.c)
AVXFLAGS = $(KCFLAGS) -mavx2 -mfma

LDFLAGS  = -nostdlib -shared -Bsymbolic \
           -T $(EFI_LDS) \
           -L$(EFI_LIB) \
           $(EFI_CRT)

LIBS     = -lefi -lgnuefi

# Build directory
BUILD    = build

# Source files
EFI_SRC  = boot/efi_main.c
KERN_SRC = kernel/main.c kernel/serial.c kernel/framebuffer.c \
           kernel/pci.c kernel/memory.c kernel/net.c kernel/tensor.c \
           kernel/tensor_avx2.c kernel/inference.c
DRV_SRC  = drivers/nvme.c drivers/i211.c drivers/gpu.c drivers/gsp.c drivers/sass.c drivers/gmmu.c drivers/gpu_tensor.c drivers/gpu_inference.c
FS_SRC   = fs/ositofs2.c fs/gpt.c fs/gguf.c

EFI_OBJ  = $(BUILD)/efi_main.o
KERN_OBJ = $(KERN_SRC:%.c=$(BUILD)/%.o)
DRV_OBJ  = $(DRV_SRC:%.c=$(BUILD)/%.o)
FS_OBJ   = $(FS_SRC:%.c=$(BUILD)/%.o)

ALL_OBJ  = $(EFI_OBJ) $(KERN_OBJ) $(DRV_OBJ) $(FS_OBJ)

TARGET   = $(BUILD)/ositok.efi
SO       = $(BUILD)/ositok.so

.PHONY: all clean install check-deps

all: check-deps $(TARGET)

check-deps:
	@test -f $(EFI_CRT) || (echo "ERROR: gnu-efi not installed. Run: sudo apt install gnu-efi" && exit 1)

$(TARGET): $(SO)
	$(OBJCOPY) -j .text -j .rodata -j .sdata -j .data -j .dynamic -j .dynsym \
	           -j .rel -j .rela -j .reloc \
	           --target=efi-app-x86_64 $< $@
	@echo "Built: $@ ($$(stat -c%s $@) bytes)"

$(SO): $(ALL_OBJ)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

# EFI entry point (needs gnu-efi headers)
$(BUILD)/efi_main.o: boot/efi_main.c | $(BUILD)/boot
	$(CC) $(CFLAGS) -c -o $@ $<

# Kernel files (freestanding, no libc)
$(BUILD)/kernel/%.o: kernel/%.c | $(BUILD)/kernel
	$(CC) $(KCFLAGS) -c -o $@ $<

# tensor_avx2.c needs AVX2/FMA flags (overrides pattern rule above)
$(BUILD)/kernel/tensor_avx2.o: kernel/tensor_avx2.c | $(BUILD)/kernel
	$(CC) $(AVXFLAGS) -c -o $@ $<

# Drivers
$(BUILD)/drivers/%.o: drivers/%.c | $(BUILD)/drivers
	$(CC) $(KCFLAGS) -c -o $@ $<

# Filesystem
$(BUILD)/fs/%.o: fs/%.c | $(BUILD)/fs
	$(CC) $(KCFLAGS) -c -o $@ $<

# Create build directories
$(BUILD)/boot $(BUILD)/kernel $(BUILD)/drivers $(BUILD)/fs:
	mkdir -p $@

clean:
	rm -rf $(BUILD)

# Install to ESP partition
ESP_DIR = /boot/efi/EFI/ositok

install: $(TARGET)
	sudo mkdir -p $(ESP_DIR)
	sudo cp $(TARGET) $(ESP_DIR)/
	@echo "Installed to $(ESP_DIR)/ositok.efi"
	@echo ""
	@echo "Add to /etc/grub.d/40_custom:"
	@echo '  menuentry "OsitoK x86 (bare-metal AI)" {'
	@echo '      insmod chain'
	@echo '      insmod part_gpt'
	@echo '      insmod fat'
	@echo '      chainloader /EFI/ositok/ositok.efi'
	@echo '  }'
	@echo ""
	@echo "Then run: sudo grub-mkconfig -o /boot/grub/grub.cfg"
