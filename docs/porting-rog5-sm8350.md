# Osito-K: Porting Guide — ROG Phone 5 (SM8350 / Lahaina)

Target device: ASUS ROG Phone 5 (ASUS_I005DA)
SoC: Qualcomm Snapdragon 888 (SM8350, codename "lahaina")
RAM: 16 GB LPDDR5
Bootloader: Unlocked (verified boot state: orange)
Partition scheme: A/B (super, boot_a/b, vendor_boot_a/b, dtbo_a/b)
Serial: M2AIKN06180585K

---

## 1. Boot Chain — What Osito-K Must Navigate

The SM8350 boot chain is:

```
PBL (ROM) → XBL (Qualcomm) → ABL (UEFI/Android) → Kernel
```

### 1.1 Primary Boot Loader (PBL)
- Hardcoded in SoC ROM, not modifiable
- Initializes DDR, loads XBL from `xbl_a`/`xbl_b` partition
- Supports EDL (Emergency Download) mode via test points or vol-down at power-on

### 1.2 XBL (Qualcomm eXtensible Boot Loader)
- Proprietary Qualcomm firmware in `xbl_a` partition (`/dev/block/sdb1`)
- Initializes SoC peripherals, DDR training, security co-processor
- Loads ABL (Android Boot Loader)
- **Do not modify** — bricking risk

### 1.3 ABL (Android Boot Loader)
- Based on Qualcomm's fork of EDK2/UEFI (`abl_a` partition, `/dev/block/sde8`)
- Loads `boot.img` (kernel + ramdisk) or `vendor_boot.img`
- Implements fastboot protocol, verified boot (AVB)
- With unlocked bootloader, it will boot unsigned images
- **This is our entry point** — ABL loads our kernel from `boot_a`

### 1.4 What ABL Expects in boot.img
The `boot_a` partition contains an Android boot image (mkbootimg format v3/v4):

```
+------------------+
| boot header      |  (page-aligned, identifies kernel offset, ramdisk size, etc.)
| kernel           |  (gzip-compressed or raw Image)
| ramdisk          |  (can be empty/minimal for our purposes)
| second stage     |  (unused, 0 bytes)
| DTB              |  (appended or in dtbo partition)
+------------------+
```

Osito-K's aarch64 kernel must be packaged as a **boot.img** using `mkbootimg`.

---

## 2. Minimum Kernel Requirements for SM8350

### 2.1 CPU Architecture
- **ISA**: ARMv8.2-A (Cortex-A78 + Cortex-A55, Kryo 680)
- **CPU topology**: 1x Cortex-X1 (prime @ 2.84GHz) + 3x Cortex-A78 (perf @ 2.4GHz) + 4x Cortex-A55 (eff @ 1.8GHz)
- **Exception level**: Kernel runs at EL1 (ABL drops from EL2/EL3 to EL1 before jumping to kernel)
- **Must support**: AArch64 state, NEON/ASIMD, CRC32, atomics (LSE)
- **Page size**: 4KB (standard) — ABL configures initial page tables
- **Endianness**: Little-endian

### 2.2 Entry Point Requirements
When ABL jumps to the kernel, the CPU state is:

```
- x0 = physical address of the Device Tree Blob (DTB)
- x1 = 0 (reserved)
- x2 = 0 (reserved)
- x3 = 0 (reserved)
- MMU may be ON with identity mapping (ABL/UEFI page tables)
- D-cache and I-cache enabled
- All secondary CPUs are held in a spin-table or PSCI
- Interrupts disabled (PSTATE.DAIF = 0b1111)
```

The kernel entry point must be at offset 0 of the uncompressed Image, following the ARM64 Linux boot protocol header:

```c
// ARM64 Image header (first 64 bytes)
struct arm64_image_header {
    uint32_t code0;          // Executable code (branch instruction)
    uint32_t code1;          // Executable code
    uint64_t text_offset;    // Image load offset from start of RAM
    uint64_t image_size;     // Effective image size (0 = unknown)
    uint64_t flags;          // Kernel flags (bit 0: endianness, bit 1: page size, bit 3: phys placement)
    uint64_t res2;           // Reserved
    uint64_t res3;           // Reserved
    uint64_t res4;           // Reserved
    uint32_t magic;          // 0x644d5241 ("ARM\x64")
    uint32_t res5;           // Reserved (PE/COFF offset or 0)
};
```

### 2.3 Device Tree (DTB)
- ABL passes the DTB address in x0
- The DTB describes all hardware: CPU topology, memory map, peripherals, clocks, regulators
- **Source**: Use the stock DTB from the device (extract from `dtbo_a` partition) or from the `sm8350.dtsi` in mainline Linux
- For bare-metal, you can use the stock DTB directly or create a minimal one
- If ignoring DTB: hardcode UART base address for early console output

### 2.4 Memory Map (Key Regions)
```
0x0000_0000 - 0x7FFF_FFFF    DDR Bank 0 (~2GB)
0x8000_0000 - 0xFFFF_FFFF    DDR Bank 1 (~2GB) — typical kernel load here
0x0100_0000_0000+             DDR upper banks (16GB total)

Key MMIO regions:
0x0098_C000                   GENI UART (earlycon, serial0)
0x0A60_0000                   USB DWC3 controller
0x01E0_0000                   TLMM (GPIO/pinctrl)
0x0F52_2000                   APCS/GIC distributor
0x0F52_0000                   APCS/GIC redistributor
0x0188_0000 - 0x018B_FFFF    Display controller (MDSS/DPU)
```

---

## 3. Minimal Subsystems Osito-K Needs

### 3.1 MUST HAVE (Phase 1 — Serial Console)
| Subsystem | Details | Priority |
|-----------|---------|----------|
| **AArch64 bootstrap** | EL1 entry, set up stack, clear BSS, branch to C | Critical |
| **UART driver** | Qualcomm GENI UART @ `0x0098C000`, 115200n8 | Critical |
| **Exception vectors** | VBAR_EL1 setup, basic sync/irq/fiq/serror handlers | Critical |
| **Physical memory** | Simple page allocator (bitmap), parse DTB `/memory` node or hardcode | Critical |
| **MMU setup** | 4KB granule, VA=PA identity map for MMIO + RAM | Critical |

With these, you get: boot → serial output → basic exception handling. This is your "hello world" milestone.

### 3.2 SHOULD HAVE (Phase 2 — Interactive)
| Subsystem | Details | Priority |
|-----------|---------|----------|
| **GIC-v3** | ARM GICv3 interrupt controller (GICD + GICR + ICC system regs) | High |
| **Timer** | ARM Generic Timer (CNTPCT_EL0 / CNTP_TVAL_EL0) for scheduler tick | High |
| **Scheduler** | Port existing preemptive scheduler from ESP8266 (adapt context frame) | High |
| **Shell** | Port existing shell over UART | High |
| **UFS storage** | Qualcomm UFS host controller for reading/writing flash | High |

### 3.3 NICE TO HAVE (Phase 3 — Display + USB)
| Subsystem | Details | Priority |
|-----------|---------|----------|
| **Display** | Qualcomm MDSS/DPU framebuffer — complex, consider simplefb from ABL | Medium |
| **USB** | DWC3 controller @ `0x0A600000` — USB gadget/host | Medium |
| **SMP** | Bring up secondary cores via PSCI (already held by ABL) | Medium |
| **GPU** | Adreno 660 — extremely complex, skip for now | Low |

---

## 4. AArch64 Context Frame for Scheduler

The ESP8266 context frame is 80 bytes (a0-a15 + PS + SAR + EPC1). The AArch64 equivalent:

```c
struct aarch64_context {
    uint64_t x[31];     // x0-x30 (x30 = LR)
    uint64_t sp;        // SP_EL0
    uint64_t elr;       // ELR_EL1 (return address, like EPC1)
    uint64_t spsr;      // SPSR_EL1 (saved PSTATE, like PS)
    uint64_t padding;   // Alignment to 16 bytes
    // NEON: 32 x 128-bit Q registers if needed (512 bytes extra)
};
// Size: 34 * 8 = 272 bytes minimum (without NEON)
// Size: 272 + 512 = 784 bytes with full NEON state
```

Context switch flow:
1. Exception entry (IRQ from generic timer) → save x0-x30, SP, ELR_EL1, SPSR_EL1
2. Call scheduler → pick next task
3. Restore next task's registers → `eret` (returns to ELR_EL1 with SPSR_EL1)

### 4.1 Task Stack Size
- ESP8266 uses 1536 bytes — for AArch64 with 64-bit registers, recommend **4096 bytes minimum** (8KB preferred)
- Stack must be 16-byte aligned (AArch64 ABI requirement)

---

## 5. Boot Image Creation

### 5.1 Building the Kernel
```bash
# Cross-compile for AArch64
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
    -march=armv8.2-a -mtune=cortex-a78 \
    -T linker.ld \
    -o osito-k.elf start.S main.c uart.c ...

aarch64-linux-gnu-objcopy -O binary osito-k.elf Image
```

### 5.2 Packaging as boot.img
```bash
# Create Android boot image
mkbootimg \
    --kernel Image \
    --ramdisk /dev/null \
    --dtb device-tree.dtb \
    --base 0x00000000 \
    --kernel_offset 0x00008000 \
    --ramdisk_offset 0x01000000 \
    --dtb_offset 0x01f00000 \
    --os_version 13.0.0 \
    --os_patch_level 2023-01 \
    --header_version 3 \
    --pagesize 4096 \
    --cmdline "console=ttyMSM0,115200n8 earlycon=msm_geni_serial,0x98c000" \
    -o boot.img
```

### 5.3 Flashing
```bash
# From recovery fastboot (fastbootd):
fastboot flash boot_a boot.img

# Or from Android with root (dd method):
adb push boot.img /sdcard/
adb shell su -c "dd if=/sdcard/boot.img of=/dev/block/sde11 bs=4096"

# Or boot without flashing (test first):
fastboot boot boot.img
```

---

## 6. UART / Serial Console Access

The primary debug interface. Without display drivers, this is how you'll see Osito-K output.

- **Hardware UART**: Qualcomm GENI Serial Engine @ `0x0098C000`
- **Baud**: 115200
- **Access method**: USB serial via the phone's USB-C port (requires Android kernel to expose it) OR headphone jack UART adapter (ROG Phone 5 supports debug UART on 3.5mm jack — check device-specific docs)
- **Alternative**: Use `adb shell` with a serial forwarder if booting a minimal Android-based shim first

### 6.1 GENI UART Register Map (minimal)
```c
#define GENI_UART_BASE      0x0098C000

#define SE_GENI_STATUS       (GENI_UART_BASE + 0x40)
#define SE_GENI_TX_FIFOn     (GENI_UART_BASE + 0x700)
#define SE_GENI_RX_FIFOn     (GENI_UART_BASE + 0x780)
#define SE_GENI_TX_FIFO_STATUS (GENI_UART_BASE + 0x800)

// Minimal TX: poll TX FIFO status, write byte to TX_FIFOn
static void uart_putc(char c) {
    while (*(volatile uint32_t*)(SE_GENI_TX_FIFO_STATUS) & TX_FIFO_FULL);
    *(volatile uint32_t*)(SE_GENI_TX_FIFOn) = c;
}
```

**Important**: The GENI serial engine requires clock and pinmux setup. If ABL already configured earlycon (which it does — see kernel cmdline), the UART should be ready to use at kernel entry. Do NOT reinitialize clocks or the UART will break.

---

## 7. Differences from x86 Port

| Aspect | x86-64 (current) | AArch64/SM8350 |
|--------|-------------------|----------------|
| Boot | UEFI → EFI stub | ABL → boot.img (mkbootimg) |
| Console | GOP framebuffer + COM1 | GENI UART (no easy framebuffer) |
| Interrupts | APIC + IDT | GICv3 + VBAR_EL1 |
| Timer | APIC timer / HPET | ARM Generic Timer (CNTPCT) |
| Storage | NVMe | UFS (Qualcomm variant) |
| Page tables | 4-level x86 | 4-level ARM (TCR_EL1/TTBR0/TTBR1) |
| GPU | NVIDIA (full driver) | Adreno 660 (skip for now) |
| Network | Intel I211 | N/A (WiFi/modem only, both broken on this unit) |

---

## 8. Recovery Plan

If a bad flash bricks the boot, these are the recovery options:

1. **Slot B**: Flash Osito-K to `boot_a`, keep stock Android on `boot_b`. Switch active slot:
   ```bash
   fastboot set_active b   # revert to stock Android
   ```

2. **EDL Mode**: Qualcomm Emergency Download mode. Requires:
   - `qdl` or QFIL tool
   - Programmers/firehose for SM8350 (device-specific, may need ASUS firmware package)
   - Access via test points on the motherboard or USB while holding vol-down during PBL

3. **Stock firmware reflash**: Download ASUS ROG Phone 5 firmware from ASUS support, flash via fastboot or ASUS flash tool

---

## 9. VERIFIED ON REAL HARDWARE (sm8350-boot, 2026-03-03/04)

The following was discovered by building and booting a custom bare-metal bootloader
(`sm8350-boot` v0.1 through v0.4) on the actual device. These findings supersede
any theoretical assumptions above.

### 9.1 Load Address Mismatch (CRITICAL)
ABL loads the kernel binary at **~0xA0080000**, NOT 0x80080000 as the boot.img
header and linker script suggest. DTB was observed at ~0xAF4FE000.

**Consequence**: Any stored pointer in `.data` or `.rodata` that contains a
link-time address will be wrong at runtime. GCC aarch64 at -O2 uses ADRP
(PC-relative) for direct symbol references, which works at any address. But
stored pointers break:

```c
// BROKEN — pointer in .data stores link-time 0x8008xxxx address:
static const char *msg = "hello";
static const char *labels[] = { "one", "two" };

// SAFE — array contents accessed via PC-relative ADRP:
static const char msg[] = "hello";
static const char labels[][8] = { "one", "two" };
```

**Fix**: Use `char arr[] = "str"` instead of `char *ptr = "str"` everywhere.
Use 2D arrays `char labels[][N]` instead of pointer arrays `char *labels[]`.

### 9.2 UART TX Hangs
`uart_putc()` with an infinite polling loop HANGS the system. ABL may not leave
UART TX fully functional. Fix: timeout loop (~100k iterations), silently drop
the character rather than blocking.

### 9.3 Dual Output Order
When outputting to both framebuffer and UART, **always call fb_puts() BEFORE
uart_puts()**. If UART TX hangs (even with timeout, it's slow), it blocks
framebuffer updates. First boot with uart-first showed black screen.

### 9.4 Splash Framebuffer — Confirmed Working
- Address: `0xE5000000` (splash_region in DTB, 35MB reserved)
- Format: **ARGB8888** (confirmed with R/G/B/W color bar test)
- Resolution: 1080 x 2448 (Samsung AMS678 ER2 OLED)
- Stride: 1080 pixels (linear, no row padding)
- ABL leaves display pipeline active — just write pixels, no MDSS init
- Font: 8x16 VGA bitmap works great (135 cols x 153 rows)

### 9.5 CPU State at Entry (Confirmed)
- CPU: Cortex-A55 (little core, MIDR part 0xD05), r4p0
- Exception Level: EL1
- Timer: 19.2 MHz (CNTFRQ_EL0 = 0x124F800)
- Stack: set our own at 0x85000000
- x0 = DTB address (~0xAF4FE000)
- Caches ON, interrupts disabled

### 9.6 SPMI / Buttons — NOT YET WORKING
- PON (power + vol_down): periph 0x13, SPMI SID 0
- Vol Up: PMIC GPIO6 (periph 0x8D)
- **SPMI observer probe causes Data Abort** (Synchronous External Abort)
- Channel mapping unknown — need to extract from ABL or kernel sources
- Workaround: timeout-only menu, UART input for selection

### 9.7 PSCI Reboot
Works. Use SMC with `x0 = 0x84000009` (SYSTEM_RESET). Must use `ldr x0, =imm`
not `mov x0, imm` because the immediate is too large for MOV.

### 9.8 Camera Sensors (for future OCR/vision)
5 sensors identified:
- Cam0: Sony IMX686, 64MP (4624x3472 binned to 15.7MP)
- Cam1: 24MP front camera
- Cam2: 13MP ultrawide
- Cam3: 5MP macro
- Cam4: IMX686 full-res mode (63MP, 9248x6944)
- CCI buses: cci0 (sensors 0,1,2,4), cci1 (sensor 3)

### 9.9 Build & Boot Recipe (tested)
```bash
# Build
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
    -march=armv8.2-a -mtune=cortex-a78 -O2 -mgeneral-regs-only \
    -I include -c -o build/main.o src/main.c
# ... (compile all .c and .S files)
aarch64-linux-gnu-ld -nostdlib -T linker.ld -o build/kernel.elf build/*.o
aarch64-linux-gnu-objcopy -O binary build/kernel.elf build/Image

# Package boot.img (v2 for fastboot boot, includes DTB)
mkbootimg --kernel build/Image --ramdisk /dev/null \
    --dtb stock.dtb --base 0x00000000 --kernel_offset 0x00080000 \
    --header_version 2 --pagesize 4096 \
    --cmdline "console=ttyMSM0,115200n8 earlycon=msm_geni_serial,0x98c000" \
    -o build/boot.img

# Boot without flashing (safe)
fastboot boot build/boot.img
```

Binary size: ~14KB. Boot.img: ~800KB (mostly DTB).

---

## 10. Recommended Development Workflow

1. ~~Extract stock boot.img~~ Done, backed up
2. ~~Create minimal "hello world" kernel~~ Done (sm8350-boot)
3. ~~Package as boot.img~~ Done (mkbootimg v2)
4. ~~Test with `fastboot boot boot.img`~~ Done, multiple iterations
5. **Next**: Port Osito-K kernel (scheduler, shell) to AArch64
6. Add GICv3 + timer interrupt for preemptive scheduler
7. Port context_switch.S (ESP8266 → AArch64 context frame)
8. Port shell over UART
9. Add framebuffer console
10. UFS storage for OsitoFS

---

## 11. Osito-K ARM Files

Reference code from sm8350-boot has been ported to `arch/arm/`:

```
arch/arm/
  include/sm8350.h      Hardware registers (verified)
  include/aarch64.h     System register helpers, context frame
  boot/start.S          Entry point + exception vectors
  boot/linker.ld        Linker script (with relocation warnings)
  drivers/geni_uart.c   UART TX/RX (with timeout fix)
  drivers/framebuffer.c Splash FB console (ARGB8888)
  drivers/timer.c       ARM Generic Timer (19.2 MHz)
  drivers/spmi.c        SPMI button input (WIP, data abort)
```

---

## 12. Key Resources

- **Qualcomm SM8350 TRM**: Not publicly available; use Linux kernel sources as reference
- **Linux kernel sm8350.dtsi**: `arch/arm64/boot/dts/qcom/sm8350.dtsi` in mainline Linux — the definitive hardware reference
- **PostmarketOS wiki**: ROG Phone 5 device page for community kernel builds
- **Qualcomm GENI driver**: `drivers/tty/serial/qcom_geni_serial.c` in Linux kernel
- **ARM Architecture Reference Manual (ARMv8-A)**: Exception model, MMU, system registers
- **GICv3 specification**: ARM IHI 0069 — interrupt controller programming
- **sm8350-boot repo**: `~/phone_breaker/sm8350-boot/` — working bootloader code
- **Device dump**: `~/phone_breaker/device-dump/` — DTB, iomem, configs, hardware ref
