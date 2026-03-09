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
           kernel/tensor_avx2.c kernel/inference.c kernel/idt.c \
           kernel/paging.c kernel/heap.c kernel/syscall.c \
           kernel/elf.c kernel/process.c \
           kernel/keyboard.c kernel/terminal.c kernel/shell.c \
           kernel/crypto.c kernel/tls.c kernel/http.c kernel/claude.c \
           kernel/tokenizer.c kernel/smp.c kernel/dynlink.c \
           kernel/zlib.c kernel/git.c
ASM_SRC  = kernel/isr_stubs.S kernel/syscall_entry.S kernel/setjmp.S
DRV_SRC  = drivers/nvme.c drivers/i211.c drivers/gpu.c drivers/gsp.c drivers/sass.c drivers/gmmu.c drivers/gpu_tensor.c drivers/gpu_inference.c
FS_SRC   = fs/ositofs2.c fs/gpt.c fs/gguf.c
WIN32_SRC = win32/win32_init.c win32/compat32.c win32/pe.c win32/winexec.c \
            win32/handle.c win32/ntsyscall.c win32/ntprocess.c win32/ntsync.c \
            win32/dllloader.c \
            win32/kernel32_shim.c win32/ntdll_shim.c win32/msvcrt_shim.c \
            win32/user32_shim.c win32/gdi32_shim.c win32/advapi32_shim.c \
            win32/ddraw_shim.c win32/dsound_shim.c \
            win32/ole32_shim.c win32/shell32_shim.c \
            win32/comctl32_shim.c win32/comdlg32_shim.c \
            win32/winmm_shim.c win32/wsock32_shim.c
WIN32_ASM = win32/int2e_stub.S

EFI_OBJ  = $(BUILD)/efi_main.o
KERN_OBJ = $(KERN_SRC:%.c=$(BUILD)/%.o)
ASM_OBJ  = $(ASM_SRC:%.S=$(BUILD)/%.o)
DRV_OBJ  = $(DRV_SRC:%.c=$(BUILD)/%.o)
FS_OBJ   = $(FS_SRC:%.c=$(BUILD)/%.o)
WIN32_OBJ = $(WIN32_SRC:%.c=$(BUILD)/%.o)
WIN32_ASM_OBJ = $(WIN32_ASM:%.S=$(BUILD)/%.o)

ALL_OBJ  = $(EFI_OBJ) $(KERN_OBJ) $(ASM_OBJ) $(DRV_OBJ) $(FS_OBJ) $(WIN32_OBJ) $(WIN32_ASM_OBJ)

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

# Assembly files
$(BUILD)/kernel/%.o: kernel/%.S | $(BUILD)/kernel
	$(CC) -c -o $@ $<

# Drivers
$(BUILD)/drivers/%.o: drivers/%.c | $(BUILD)/drivers
	$(CC) $(KCFLAGS) -c -o $@ $<

# Filesystem
$(BUILD)/fs/%.o: fs/%.c | $(BUILD)/fs
	$(CC) $(KCFLAGS) -c -o $@ $<

# Win32 compatibility layer
$(BUILD)/win32/%.o: win32/%.c | $(BUILD)/win32
	$(CC) $(KCFLAGS) -Iwin32 -c -o $@ $<

# Win32 assembly
$(BUILD)/win32/%.o: win32/%.S | $(BUILD)/win32
	$(CC) -c -o $@ $<

# Create build directories
$(BUILD)/boot $(BUILD)/kernel $(BUILD)/drivers $(BUILD)/fs $(BUILD)/win32:
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
