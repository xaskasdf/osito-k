# Building & Booting Osito-K on ASUS ROG Phone 5

Target: ASUS ROG Phone 5 (I005DA), Qualcomm SM8350 (Snapdragon 888)
Last verified: 2026-03-08 on physical device

---

## Quick Start

```bash
# 1. Build Osito-K kernel
cd ~/osito-k/arch/arm
make PLATFORM=sm8350

# 2. Inject into combined boot image (replaces Linux at offset 1.5MB)
dd if=build/Image of=~/phone_breaker/combined_boot.bin bs=1024 seek=1536 conv=notrunc

# 3. Repack as flashable boot.img
cd ~/phone_breaker && ./repack_boot.sh

# 4. Flash (device must be in fastboot mode)
fastboot flash boot linux-boot-rog5.img
fastboot reboot
```

The sm8350-boot shim menu will appear on-screen. Osito-K is option 1 ("Linux/Osito-K").
Press `2` on UART, or wait for the 1-second auto-boot timeout.

---

## Boot Architecture

Osito-K does NOT boot directly from ABL. It boots through a three-stage chain:

```
PBL (ROM) → XBL (Qualcomm) → ABL (Android bootloader) → sm8350-boot (shim) → Osito-K
```

### Why the shim is necessary

ABL is the factory Android bootloader. It loads a standard `boot.img` and jumps to
the kernel field. On the ROG Phone 5, we discovered through extensive testing that:

1. **`fastboot boot` does not work** — causes bootloop/black screen every time.
   You MUST flash to the boot partition.
2. **`header_version 3` causes permanent logo hang** — must use version 2.
3. **A pure kernel-in-boot.img approach** (no shim) works for simple payloads but
   loses visual debug, multi-boot menu, UEFI chainloading, and DTB injection.

The **sm8350-boot** shim (v0.8) solves all of this:
- Presents a framebuffer + UART boot menu (Osito-K, Linux, Windows UEFI, reboot)
- Provides visual telemetry (colored screen flash before jump = proof of execution)
- Includes exception handlers with ESR/ELR/FAR dump (invaluable for debugging)
- Uses libfdt to dynamically inject nodes into the live DTB
- Handles cache flush (DC CVAC + IC IALLUIS) before payload handoff
- Properly resets EL1 state (DAIF, SCTLR_EL1) before jumping

### Combined image layout

The `boot.img` kernel field contains a 45MB composite binary:

```
Offset       Size     Component
──────────   ──────   ─────────────────────────────────────
0x000000     ~15KB    sm8350-boot shim (with libfdt)
0x010000     ~780KB   Patched DTB (rog5-patched.dtb)
0x180000     varies   Kernel payload (Osito-K or Linux Image)
0x2980000    ~6MB     UEFI payload (Windows ARM, optional)
──────────   ──────   ─────────────────────────────────────
Total        45MB     Zero-padded container
```

The 0x180000 (1.5MB) offset is deliberate: ABL adds a 0x80000 (512KB) kernel_offset,
so the payload lands at 0x200000 (2MB) in physical memory — the mandatory alignment
for ARM64 kernel images.

---

## Build Instructions

### Prerequisites

```bash
# Cross-compiler (Ubuntu/Debian)
sudo apt install gcc-aarch64-linux-gnu

# Boot image tool
sudo apt install mkbootimg      # or: pip install mkbootimg

# Verify
aarch64-linux-gnu-gcc --version
mkbootimg --help
```

### Build Osito-K

```bash
cd ~/osito-k/arch/arm
make clean
make PLATFORM=sm8350
```

Output: `build/Image` (raw AArch64 binary with ARM64 header at offset 0).

The Makefile uses:
- `aarch64-linux-gnu-gcc` with `-ffreestanding -nostdlib -march=armv8.2-a -O2`
- `-mgeneral-regs-only` (except for tensor/NEON files which get FP enabled)
- Platform-specific linker script: `boot/linker_sm8350.ld`
- Platform-specific sources from `platform/sm8350/`

### Build the shim (if modified)

```bash
cd ~/phone_breaker/sm8350-boot
make clean all
```

Note: the shim uses `aarch64-elf-gcc` (bare-metal toolchain), NOT `aarch64-linux-gnu-gcc`.
If you don't have it, install via your distro or build from source. The shim binary
lands at `build/Image` (~15KB).

### Integrate Osito-K into the combined image

**Option A: Replace kernel in existing combined_boot.bin**

```bash
# Inject Osito-K at the kernel offset (1.5MB = 1536 * 1024)
dd if=~/osito-k/arch/arm/build/Image \
   of=~/phone_breaker/combined_boot.bin \
   bs=1024 seek=1536 conv=notrunc
```

Then repack:
```bash
cd ~/phone_breaker && ./repack_boot.sh
```

**Option B: Full manual repack**

```bash
cd ~/phone_breaker

# Create 45MB zero container
dd if=/dev/zero of=combined_boot.bin bs=1M count=45

# Inject shim at offset 0
dd if=sm8350-boot/build/Image of=combined_boot.bin conv=notrunc

# Inject DTB at 64KB
dd if=rog5-patched.dtb of=combined_boot.bin bs=1024 seek=64 conv=notrunc

# Inject Osito-K at 1.5MB
dd if=~/osito-k/arch/arm/build/Image of=combined_boot.bin bs=1024 seek=1536 conv=notrunc

# (Optional) Inject UEFI at 41.5MB
dd if=windows/uefi_payload.bin of=combined_boot.bin bs=512k seek=83 conv=notrunc

# Package as boot.img
mkbootimg \
    --kernel combined_boot.bin \
    --ramdisk /dev/null \
    --dtb device-dump/live-device-tree.dtb \
    --base 0x00000000 --kernel_offset 0x00080000 \
    --ramdisk_offset 0x01000000 --dtb_offset 0x01F00000 \
    --os_version 13.0.0 --os_patch_level 2023-01 \
    --header_version 2 --pagesize 4096 \
    --cmdline "clk_ignore_unused pd_ignore_unused" \
    -o linux-boot-rog5.img
```

**Option C: Standalone boot.img (no shim)**

For a simpler setup without the multi-boot menu:

```bash
cd ~/osito-k/arch/arm
mkbootimg \
    --kernel build/Image \
    --ramdisk /dev/null \
    --dtb ~/phone_breaker/rog5-patched.dtb \
    --base 0x00000000 --kernel_offset 0x00080000 \
    --header_version 2 --pagesize 4096 \
    --cmdline "clk_ignore_unused pd_ignore_unused" \
    -o build/osito-k-rog5.img
```

This loses the shim's visual debug, exception handlers, and DTB injection.
Not recommended for development — use the shim approach until the kernel is stable.

---

## Flashing & Recovery

### Flash

```bash
# Device must be in fastboot mode (hold vol-down + power during boot)
fastboot devices                        # Verify connection
fastboot flash boot linux-boot-rog5.img # Flash to active boot slot
fastboot reboot                         # Boot
```

**NEVER use `fastboot boot`** — it causes bootloop on this device. Always flash.

### Recovery

Always keep a backup before flashing:

```bash
# Restore stock Android boot
fastboot flash boot_a ~/phone_breaker/recovery-kit/stock-boot-original.img
fastboot reboot

# Alternative: switch to slot B (if stock Android is there)
fastboot set_active b
fastboot reboot
```

### Emergency recovery (EDL)

If both slots are bricked:
- Hold vol-down during power-on to enter EDL (Emergency Download)
- Use `qdl` or Qualcomm QFIL to reflash full firmware
- ASUS firmware packages available from ASUS support site

---

## Hardware Reference (Verified on Device)

All values confirmed by running sm8350-boot v0.1–v0.8 on the physical ROG Phone 5.

### CPU State at Kernel Entry

When the shim (or ABL) jumps to Osito-K:

| Register      | Value                  | Notes |
|---------------|------------------------|-------|
| `x0`          | DTB physical address   | ~0xAF4FE000 from ABL, or patched DTB addr from shim |
| `x1`–`x3`    | 0                      | Reserved |
| CurrentEL     | EL1                    | ABL drops from EL2/EL3 |
| DAIF          | Masked (0b1111)        | All interrupts disabled |
| CPU           | Cortex-A55 (MIDR part 0xD05) | Little core boots first |
| CNTFRQ_EL0    | 0x124F800 (19.2 MHz)  | ARM Generic Timer frequency |
| MMU           | May be ON              | ABL identity-maps everything |
| D-Cache       | ON                     | |
| I-Cache       | ON                     | |

### Load Address Mismatch (CRITICAL)

The linker script (`linker_sm8350.ld`) uses base `0x80080000`, but ABL actually
loads the kernel at **~0xA0080000**. This is a 512MB offset.

**Consequence**: stored pointers in `.data`/`.rodata` contain the wrong address.

```c
// BROKEN — pointer stores link-time address 0x8008xxxx:
static const char *msg = "hello";
static const char *labels[] = { "one", "two" };

// SAFE — array contents accessed via PC-relative ADRP:
static const char msg[] = "hello";
static const char labels[][8] = { "one", "two" };
```

GCC at -O2 generates ADRP (PC-relative) for direct symbol references, which works
at any load address. But stored pointers in `.data` contain link-time addresses.
**Use char arrays, not char pointers. Use 2D arrays, not pointer arrays.**

This rule is mandatory until we set up MMU with proper relocation.

### Memory Map

| Address          | Description |
|------------------|-------------|
| `0x80000000`     | DDR Bank 0 start (880MB) |
| `0x85000000`     | Osito-K stack (set in linker script) |
| `0x0098C000`     | GENI UART (serial console) |
| `0x0A600000`     | USB DWC3 (side port) |
| `0x0A800000`     | USB DWC3 (bottom port) |
| `0x0AE00000`     | MDSS/SDE display controller |
| `0x0F000000`     | TLMM (GPIO/pinctrl) |
| `0x17A00000`     | GICv3 Distributor (GICD) |
| `0x17A60000`     | GICv3 Redistributor (GICR), stride 0x20000 |
| `0x17C20000`     | Timer memory-mapped block |
| `0x17980004`     | APSS Watchdog — **TZ-PROTECTED, DO NOT ACCESS** |
| `0x01D84000`     | UFS host controller |
| `0xE5000000`     | Splash framebuffer (35MB reserved) |
| `~0xA0080000`    | Actual kernel load address (from ABL) |
| `~0xAF4FE000`    | DTB address (from ABL in x0) |

### Framebuffer (Splash)

ABL leaves the display pipeline active with a persistent framebuffer. No MDSS
initialization needed — just write pixels directly.

| Property    | Value |
|-------------|-------|
| Base        | `0xE5000000` |
| Resolution  | 1080 x 2448 |
| Format      | ARGB8888 (4 bytes/pixel) |
| Stride      | 4320 bytes (1080 * 4) |
| Reserved    | 0x2300000 (35 MB) |
| Panel       | Samsung AMS678 ER2 FHD+ OLED, DSI command mode |

**Stride is 1080 pixels, NOT 1088**. Using 1088 (Qualcomm's usual 64-pixel alignment)
causes diagonal "Matrix" skewing. Verified empirically.

**D-cache must be OFF** (or flushed) when writing to the framebuffer without MMU,
because cached writes won't reach the display controller.

### UART (GENI Serial Engine)

| Property    | Value |
|-------------|-------|
| Base        | `0x0098C000` |
| Baud        | 115200 |
| Config      | 8N1 |
| Pre-init    | YES (ABL configures clocks + pinmux) |

Key registers:
```c
#define SE_GENI_TX_FIFOn         (GENI_UART_BASE + 0x700)
#define SE_GENI_RX_FIFOn         (GENI_UART_BASE + 0x780)
#define SE_GENI_TX_FIFO_STATUS   (GENI_UART_BASE + 0x800)
#define SE_GENI_RX_FIFO_STATUS   (GENI_UART_BASE + 0x804)
```

**TX timeout is mandatory**. Polling TX FIFO with an infinite loop WILL hang. Use
~100K iteration timeout and silently drop characters.

**Output order**: always write to framebuffer FIRST, then UART. If UART hangs
(even with timeout), it blocks FB updates. First boot with uart-first = black screen.

### GICv3

| Property    | Value |
|-------------|-------|
| GICD        | `0x17A00000` |
| GICR        | `0x17A60000` (per-CPU stride 0x20000) |
| Timer PPI   | IRQ 30 (non-secure physical timer) |

Uses ICC system register interface (no memory-mapped ICC).

### Timer

| Property    | Value |
|-------------|-------|
| Type        | ARM Generic Timer |
| Frequency   | 19.2 MHz (CNTFRQ_EL0 = 0x124F800) |
| Counter     | CNTPCT_EL0 (monotonic) |
| Scheduler   | CNTP_TVAL_EL0 / CNTP_CTL_EL0 |

### Watchdog

**The APSS watchdog register at `0x17980004` is TrustZone-protected.**
Any read or write from EL1 causes an immediate Synchronous External Abort
(ESR DFSC=0x10). Do NOT access this register. If UEFI watchdog management is
needed, use SCM calls (SMC to EL3).

### PSCI (Power State Coordination Interface)

```asm
// Reboot to bootloader
ldr    x0, =0x84000009    // PSCI SYSTEM_RESET (SMC64)
smc    #0
```

Must use `ldr x0, =imm` (not `mov`) — the immediate is too large for MOV encoding.

---

## Graceful Boot Path for Osito-K

### Recommended: Via sm8350-boot shim (Option 1 in menu)

The shim's boot menu (`sm8350-boot/src/main.c`) has Osito-K as option 0 (index-based).
When selected (UART key `1`, or auto-boot after timeout):

1. Shim calculates kernel address: `base_addr + 0x180000` (1.5MB offset from shim start)
2. Shim calculates DTB address: `base_addr + 0x10000` (64KB offset)
3. Calls `jump_to_linux(kernel_addr, dtb_addr)`:
   - Masks DAIF (all interrupts)
   - Disables MMU (`SCTLR_EL1.M = 0`)
   - Disables D-Cache (`SCTLR_EL1.C = 0`)
   - Sets `x0 = DTB address`, `x1 = x2 = x3 = 0`
   - Branches to kernel entry (`br`)

Osito-K's `start.S` takes over from there.

### What the shim gives you during development

- **Colored screen flash** before jump — if the screen flashes blue, the shim reached
  the jump point. If it stays blue, your kernel crashed before touching the FB.
- **Exception dump** — if Osito-K crashes and returns to the shim's exception vectors
  (it won't if VBAR_EL1 is overwritten, but useful for very early crashes).
- **UART menu** — press `6` to reboot to fastboot without reflashing.
- **DTB injection** — the shim uses libfdt to add reserved-memory nodes to the live
  DTB for UEFI and display regions.

### Entry contract

When Osito-K's `_start` executes (via shim or directly from ABL):

```
x0          = DTB physical address
x1-x3       = 0
EL           = EL1
MMU          = OFF (shim disables it) or ON (ABL identity-maps)
D-Cache      = OFF (shim disables it) or ON (ABL leaves it on)
I-Cache      = ON or OFF (implementation-defined)
DAIF         = masked (interrupts disabled)
SP           = undefined (must set our own)
VBAR_EL1     = shim's vectors (must set our own)
```

`start.S` handles: save x0 (DTB) → verify EL1 → set VBAR_EL1 → set SP → clear BSS → call `kernel_main(dtb)`.

---

## Key Constraints & Gotchas

1. **Position-independent code**: Use `char arr[]` not `char *ptr`. Use `char labels[][N]`
   not `char *labels[]`. See "Load Address Mismatch" above.

2. **D-Cache OFF for framebuffer**: Without MMU, cached writes to `0xE5000000` won't
   reach the display. Either disable D-Cache before FB writes or use DC CVAC to flush.

3. **UART TX timeout**: Never infinite-poll. Use `for (int i = 0; i < 100000; i++)`.

4. **FB before UART**: Always `fb_puts()` then `uart_puts()`, never the reverse.

5. **No button input**: SPMI observer probe causes data abort. Use UART for interaction
   or implement timeout-based auto-boot.

6. **header_version 2**: boot.img must be version 2. Version 3 = permanent logo hang.

7. **ARM64 Image header**: The first 64 bytes of the kernel binary must be a valid
   ARM64 Image header (branch instruction, magic "ARM\x64" at offset 0x38).
   `start.S` already provides this.

8. **Stack alignment**: AArch64 ABI requires 16-byte stack alignment. The linker
   script sets `_stack_top = 0x85000000` which is aligned.

9. **Do NOT touch WDT**: Register `0x17980004` is TZ-protected. Instant crash.

10. **`clk_ignore_unused pd_ignore_unused`**: These cmdline params prevent ABL from
    shutting down clocks/power domains it thinks are unused. Important for keeping
    the display and UART alive.

---

## Development Workflow

```
Edit code → make PLATFORM=sm8350 → dd into combined_boot.bin → repack → flash → observe
```

Typical iteration cycle:

```bash
# Build
cd ~/osito-k/arch/arm && make PLATFORM=sm8350

# Inject (1 command — overwrites kernel slot in existing image)
dd if=build/Image of=~/phone_breaker/combined_boot.bin bs=1024 seek=1536 conv=notrunc

# Repack
cd ~/phone_breaker && ./repack_boot.sh

# Flash + boot
fastboot flash boot linux-boot-rog5.img && fastboot reboot
```

Output appears on:
- **Phone screen** (framebuffer console, if FB driver is active)
- **UART** (if you have a serial cable — ROG Phone 5 supports debug UART on 3.5mm jack)

### Debugging a crash

If the screen stays on the shim's blue flash and never shows Osito-K output:
- Kernel crashed before FB init
- Check start.S: is BSS clear correct? Stack address valid?
- Try adding an immediate FB write (pixel at 0xE5000000) as first thing in kernel_main

If the screen goes black:
- Kernel overwrote the framebuffer with zeros
- Or kernel set up MMU incorrectly and lost access to FB region

If nothing happens (stays on ROG boot logo):
- The shim didn't even start — boot.img packaging issue
- Verify `header_version 2`, verify DTB is included

---

## File Reference

| File | Description |
|------|-------------|
| `arch/arm/Makefile` | Build system (`make PLATFORM=sm8350`) |
| `arch/arm/boot/start.S` | Entry point + exception vectors |
| `arch/arm/boot/linker_sm8350.ld` | Linker script (base 0x80080000, stack 0x85000000) |
| `arch/arm/include/sm8350.h` | Hardware register definitions (verified) |
| `arch/arm/include/aarch64.h` | System register helpers |
| `arch/arm/platform/sm8350/` | Platform-specific code |
| `arch/arm/kernel/main.c` | Kernel entry (kernel_main) |
| `arch/arm/kernel/framebuffer.c` | Splash FB console |
| `arch/arm/kernel/serial.c` | GENI UART driver |
| `arch/arm/kernel/timer.c` | ARM Generic Timer |
| `arch/arm/kernel/gic.c` | GICv3 interrupt controller |
| `arch/arm/kernel/sched.c` | Preemptive scheduler |
| `arch/arm/kernel/context_switch.S` | AArch64 context frame (272B min) |
| `~/phone_breaker/sm8350-boot/` | Boot shim (v0.8) |
| `~/phone_breaker/repack_boot.sh` | Image packing script |
| `~/phone_breaker/rog5-patched.dtb` | Stock DTB + simplefb node |
| `~/phone_breaker/recovery-kit/` | Stock boot backup |
