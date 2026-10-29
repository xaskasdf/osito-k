# Building & Booting Osito-K on ASUS ROG Phone 5

Target: ASUS ROG Phone 5 (I005DA), Qualcomm SM8350 (Snapdragon 888)
Last verified: 2026-10-28 on physical device. Boots through the shim to the
`shell>` prompt, shown on the framebuffer console with the heartbeat blinking. The
shell is interactive over USB: the side USB-C port enumerates on the PC as a USB
serial port (see "Console over USB").

---

## Quick Start

From the Windows dev box (Git Bash or MSYS2), phone in fastboot mode (Power + Vol-Down):

```bash
arch/arm/scripts/rog5-boot.sh --usb          # build, send to the running kernel over USB, boot it (~15 s)
arch/arm/scripts/rog5-boot.sh --flash        # build + pack + fastboot flash boot_<active slot> + reboot
arch/arm/scripts/rog5-boot.sh                # build + pack only -> arch/arm/build/rog5/osito-boot-rog5.img
arch/arm/scripts/rog5-boot.sh --virt-check   # also build PLATFORM=virt and boot it in local QEMU
```

The script ships the working tree (`arch/arm`, `gui`, `include/common`) to the Mac,
builds there, packs the boot image with the pieces in `~/phone_breaker2`, brings it
back and checks its SHA-256. Then it either:
- `--usb`: hands the raw `Image` to `rog5-kload.ps1`, which runs `kload` on the live
  kernel's USB console. This is the normal development loop: no buttons, and
  `boot_a` is untouched, so a reboot returns to the flashed kernel.
- `--flash`: flashes the image. Needed once, to get a kernel with `kload` onto the
  phone, or for a build that must survive a reboot. Requires fastboot mode
  (Power + Vol-Down).

Where things live:

| What | Where |
|------|-------|
| Toolchain | Mac `cortessamuel@192.168.1.4`: Homebrew `aarch64-elf-gcc` 15.2 in `/opt/homebrew/bin` |
| Shim, DTBs, `mkbootimg.py`, UEFI payload, stock backups | Mac, `~/phone_breaker2` (read-only for the script) |
| Build + pack scratch | Mac, `~/osito-arm-build` |
| fastboot | Windows, `C:\platform-tools\fastboot.exe` (phone on this machine's USB) |

- From the Windows box, port 22 on the Mac times out, so the script reaches it
  through `xasko@kerberus` (`OK_MAC_JUMP`; empty = direct).
- clang can't build the ARM kernel (no `__builtin_setjmp`/`__builtin_longjmp` on
  aarch64), so msys2's clang is not an option. Any aarch64 gcc works:
  `make PLATFORM=sm8350 CROSS=aarch64-elf-` (Mac) or the default
  `aarch64-linux-gnu-` prefix on Linux.
- Windows needs a WinUSB driver for the fastboot interface (`VID_0B05&PID_4DAF`),
  otherwise `fastboot devices` is empty: Google USB Driver, "Have Disk", pick
  **Android Bootloader Interface**.

The sm8350-boot shim menu appears on-screen and auto-boots the payload at 1.5 MB
(Osito-K) after 1 second.

---

## Console over USB

With the cable in the **side** USB-C port (the bottom one is damaged on our unit),
Osito-K presents itself as a CDC-ACM serial device: `VID_1209&PID_0001` (pid.codes
test IDs), serial `0001`. Windows 10+ binds its inbox `usbser.sys` and creates a COM
port (COM3 on the dev box); find it with
`Get-CimInstance Win32_SerialPort | ? PNPDeviceID -match VID_1209`. Open it with
PuTTY or any terminal; the baud rate doesn't matter. Everything the kernel prints
goes there as well as to the screen, and keystrokes feed the shell. Boot messages
come out before the port can be opened. `dmesg` replays them from a 16 KB log
(`kernel/serial.c`).

How it works (`platform/sm8350/usb.c`, `dwc3_acm.c`):
- **State left by ABL.** ABL leaves `usb_1` (DWC_usb31, `0x0A600000`) powered,
  clocked and in device mode after fastboot, with the USB2 PHY up. `usb_2` is off.
  So there is no PHY init, only a device-controller soft reset.
- **PIPE clock.** ABL ran fastboot at SuperSpeed and shut the USB3 PHY down on exit,
  taking the PIPE clock with it. Without it every endpoint command after
  `DEPSTARTCFG` hangs (CMDACT never clears). The fix is Linux's
  `qcom,select-utmi-as-pipe-clk` sequence in `QSCRATCH_GENERAL_CFG`, which clocks
  that logic from UTMI. The gadget is high-speed only.
- **VBUS.** There is no PMIC/extcon hookup, so `QSCRATCH_HS_PHY_CTRL` overrides
  VBUS-valid; without it the controller never sees the host.
- **DMA and the SMMU.** The controller DMAs as apps-SMMU stream `0x0`. ABL maps it
  to context bank 5 with stage-1 translation off (`SCTLR.M = 0`), so physical
  addresses work as-is. `usb.c` checks this on every boot (and can walk the stage-1
  tables if a future ABL turns translation on) and won't start the gadget otherwise.
  The DMA buffers come from the uncached DMA window (see "MMU and caches"), so
  they need no cache maintenance.
- **Polled, no IRQs.** `uart_trygetc` calls `acm_poll()`, so USB is serviced while
  the shell waits for input. Output is queued in an 8 KB ring and drains once the
  shell is back at the prompt; a long-running command's output arrives at the end.
  If the ring fills while the host has the port open (DTR), `acm_putc` services USB
  until there is room, so long output arrives whole. With nobody reading, it drops
  bytes instead of stalling. Bytes sent before the host opens the port are
  discarded by Windows; use `dmesg`.
- **Flow control.** Bulk OUT is armed only when a full 2 KB transfer fits in the
  8 KB receive ring; until then the controller NAKs the host.

**Loading kernels (`kload`, `platform/sm8350/kload.c`).** The shell command prints
`kload: ready` and expects `"OKLD"`, the Image size and its CRC-32 (u32
little-endian each), then the raw Image. It stores the Image in freshly allocated
pages, checks the CRC and the ARM64 header magic, waits for its reply to reach
the host and disconnects USB. Then `mmu_off_and_jump` stops the timer, cleans and
invalidates the whole cached range, turns the MMU and caches off, invalidates the
I-cache and TLB, and jumps with the shim's entry state. The Image runs wherever it
landed. That works because the kernel relocates itself, and `mem_init` reserves
everything from the RAM base up to the running kernel's end. The old kernel's
memory is simply abandoned. Each generation lands a little higher (e.g.
`0xA86AB000`). A 500 KB transfer takes ~0.3 s with caches on (~2.5 s without).

## MMU and caches

The shim enters with the MMU and D-cache off, so every access is
Device-nGnRnE: uncached and unmerged. `platform/sm8350/mmu.c`, called from
`kernel_main` after `mem_init`, builds a flat map (VA == PA) and turns the MMU
and both caches on:

| Range | Attributes | Use |
|-------|-----------|-----|
| `0x00000000`–`0x3FFFFFFF` | Device-nGnRE (1 GB block) | MMIO: GCC, UART, USB, SMMU, GIC… |
| `0xA0000000`–`0xB4DFFFFF` | Normal write-back | kernel, heap, page allocator, kload buffers |
| `0xB4E00000`–`0xB4FFFFFF` | Normal non-cacheable | DMA window (`dma_alloc`): USB event buffer and TRBs |
| `0xE5000000`–`0xE5BFFFFF` | Normal non-cacheable | splash framebuffer; writes merge, display sees them |

Everything else is unmapped. That matters for the firmware carve-outs below
`0xA0000000`: Normal memory may be read speculatively, and those regions belong to
other processors. The holes in the MMIO gigabyte are harmless, because Device
memory is never speculated.

- **Before enabling**, `dc ivac` runs over the whole cached range. Everything there
  was written with the D-cache off, so no stale line may survive. Under the
  hypervisor this behaves as clean+invalidate. That's safe only because whatever
  ran before us left no dirty lines: ABL cleans on exit, and `kload` cleans before
  its jump.
- **The page allocator** stops at `0xB5000000`, the end of DDR bank 0. The next
  bank starts at `0xC0000000`. Until 2026-10-29 it claimed 384 MB up to
  `0xC0000000`, which ran 176 MB into the hole; no allocation had gone that high.
- **Effect:** `help` round trip over USB went from 315 ms to 31 ms, and kload
  uploads from 2.5 s to 0.3 s.

---

## Reading the screen (no UART)

The GENI UART TX FIFO does not drain once Osito-K runs, and there is no serial cable.
Until the USB console is up (and whenever the kernel dies before or inside it), the
framebuffer is the only output. `uart_putc` gives up on the first TX timeout
and drops all later output (before that, ~100k MMIO polls per character made the
banner take minutes).

**Text console.** `platform_init` points `kernel/framebuffer.c` at the splash
framebuffer and confines it to pixel rows 700 and below. `serial.c` writes every
console byte there before the UART, so the boot log and the shell show up on screen.
Glyphs are the 8×16 font at 2× (67 columns × 54 rows). The console doesn't scroll:
with the D-cache off, copying the text area took ~1 s per line and stalled the shell,
USB included. At the bottom it wraps to the top and always blanks the row after
the current one, so look for the line just above the empty one.

Above the console, `kernel_main` paints 30-px bars as it advances:

| Rows | Color | Meaning |
|------|-------|---------|
| 20–49 | red | `kernel_main` reached |
| 60–89 | green | `platform_init` done |
| 100–129 | blue | banner + CPU info printed |
| 140–169 | yellow | GIC up |
| 180–209 | cyan | timer programmed |
| 220–249 | magenta | page allocator up |
| 260–289 | white | MMU and caches on |
| 300–329 | grey | device probing done (virt-only probes skipped on SM8350) |
| 340–369 | dark green | scheduler starting |
| 420–449 | white square, blinking every 0.5 s | timer IRQs + context switches alive |

The tiny lines at the very top come from `start.S` (red/green/cyan/white = entry,
EL1, stack, BSS cleared).

**Panic telemetry.** A synchronous exception paints an **orange** band at rows 500–529
(an SError paints a **purple** one), followed by three rows of 32 cells, MSB first,
white = 1, grey = 0:

1. `ESR_EL1`
2. `ELR_EL1[31:0]`
3. runtime address of `_start` (so far always `0xA0200000`)

The kernel is linked at 0, so `ELR - _start` is the faulting address in
`kernel.elf`. Look it up with `aarch64-elf-addr2line -f -e kernel.elf <addr>` on the
Mac (`~/osito-arm-build/src/arch/arm/build/kernel.elf`). Example from 2026-10-28,
when the link base was still `0x80080000`: ESR `0x96000010` (data abort, synchronous
external abort) at ELR `0xA020603C` was offset `0x603C`, a config-space read in
`pci_scan`.

### Bring-up log (2026-10-28)

1. **Stack in a secure carve-out.** `_stack_top` was fixed at `0x85000000`, inside
   `cdsp_secure_heap` (`0x80C00000–0x85200000` in the device's reserved-memory). It is
   now `_image_end + 64 KB`, loaded PC-relative in `start.S`.
2. **UART stall.** See above: the green → blue step took minutes.
3. **QEMU virt devices probed on the phone.** `pci_scan` (ECAM), virtio-blk/net
   (virtio-mmio) and `smp_boot_aps` (virt MPIDR numbering) caused a synchronous
   external abort. `kernel_main` now runs them only when `platform_ram_base()` is
   virt's `0x40000000`.
4. **IRQ before the first task.** The timer is programmed long before
   `sched_start`, so an IRQ is pending as soon as `irq_enable()` runs.
   `irq_vector_entry` loaded `current_task->sp` on exit without checking for NULL
   and read physical address 0. It now returns on the current stack when there is
   no task.
5. **Framebuffer console.** The shell only talked to the UART. The console above
   made the full boot log visible: Cortex-A55 r2p0 (MIDR `0x412FD050`, MPIDR
   `0x81000000`), GICv3 with 1024 IRQs, 19.2 MHz timer at 100 Hz, 384 MB page
   allocator at `0xA8000000`, then the scheduler and `shell>`. There were no
   unhandled IRQs.
6. **USB serial console.** See "Console over USB". Getting there took the PIPE-clock
   switch, which fixed the endpoint commands that hung, and the VBUS override. The
   console scroll also had to go, because it delayed shell output by ~1 s per line.
7. **Kernels over USB.** `kload` plus `rog5-boot.sh --usb` replaced flashing for
   every test build. The edit-build-boot loop went from fastboot plus buttons
   (~1 min) to ~15 s, hands-off.

8. **Self-relocating kernel (2026-10-29).** Stored pointers were wrong at the real
   load address. Before this, `drivers/gpu.o` (86), `gui/gui_desktop.o` (19),
   `fs/gguf.o` (14), `kernel/claude.o` (7), `kernel/shell.o` (4: the task-state
   names in `ps`) and `drivers/gsp.o` (4) all had them. The kernel is now a PIE
   linked at 0 that relocates itself (see "Load address and self-relocation").
   Verified with `ps` on the device and with two chained `kload`s at different
   addresses.
9. **MMU and caches (2026-10-29).** See "MMU and caches": a flat map with cached
   RAM, an uncached DMA window and an uncached framebuffer. Shell round trips got
   ~10× faster. Verified by chain-loading from an MMU-on kernel, which exercises
   the clean-and-disable handoff. This also fixed the page allocator's range,
   which extended past the end of DDR bank 0.

Still open:
- **The console still wraps instead of scrolling.** Framebuffer reads are uncached,
  so scrolling may still be slow; worth re-measuring now that writes merge.
- **Secondary CPUs aren't started.** `smp_boot_aps` assumes virt's MPIDR numbering.
- `PLATFORM=virt` needs `-m 4G` in QEMU because `start.S` paints `0xE5000000`
  before it knows the platform.

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

`arch/arm/scripts/rog5-boot.sh` does all of this; the manual steps are here for
reference or for building on another machine.

### Prerequisites

```bash
# Cross-compiler: any aarch64 gcc (not clang, see Quick Start)
sudo apt install gcc-aarch64-linux-gnu      # Linux: default CROSS=aarch64-linux-gnu-
brew install aarch64-elf-gcc                # macOS: make ... CROSS=aarch64-elf-

# Boot image tool: phone_breaker2 ships mu_aloha_platforms/ImageResources/mkbootimg.py
```

### Build Osito-K

```bash
cd ~/osito-k/arch/arm
make clean
make PLATFORM=sm8350                        # add CROSS=aarch64-elf- on the Mac
```

Output: `build/Image` (raw AArch64 binary with ARM64 header at offset 0).

The Makefile uses:
- `aarch64-linux-gnu-gcc` with `-ffreestanding -nostdlib -march=armv8.2-a -O2`
- `-mgeneral-regs-only` (except for tensor/NEON files which get FP enabled)
- Platform-specific linker script: `boot/linker_sm8350.ld`
- Platform-specific sources from `platform/sm8350/`

### Build the shim (if modified)

```bash
cd ~/phone_breaker2/sm8350-boot
make clean all
```

Note: the shim uses `aarch64-elf-gcc` (bare-metal toolchain), NOT `aarch64-linux-gnu-gcc`.
If you don't have it, install via your distro or build from source. The shim binary
lands at `build/Image` (~27KB, v0.8).

### Integrate Osito-K into the combined image

`phone_breaker2/repack_boot.sh` rebuilds `combined_boot.bin` from scratch with the
**Linux** kernel (`linux-sm8350/arch/arm64/boot/Image`) at 1.5 MB, so injecting
Osito-K into `combined_boot.bin` and then running it just packs Linux again. Use the
full manual repack below (what `rog5-boot.sh` runs on the Mac).

**Full manual repack**

```bash
cd ~/phone_breaker2

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
    -o osito-boot-rog5.img
```

**Standalone boot.img (no shim)**

For a simpler setup without the multi-boot menu:

```bash
cd ~/osito-k/arch/arm
mkbootimg \
    --kernel build/Image \
    --ramdisk /dev/null \
    --dtb ~/phone_breaker2/rog5-patched.dtb \
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
fastboot getvar current-slot            # a, as of 2026-10-28
fastboot flash boot_a osito-boot-rog5.img
fastboot reboot                         # Boot
```

**NEVER use `fastboot boot`** — it causes bootloop on this device. Always flash.

- `fastboot set_active` is refused ("Slot Change is not allowed in Lock State")
  even though `getvar unlocked` says yes; slot changes need the critical unlock.
- Osito-K never marks a boot successful, so ABL decrements
  `slot-retry-count:a` on every boot. Flashing resets it (to 3 on 2026-10-28).
- To get back to fastboot from a hung kernel: hold Power + Vol-Down.

### Recovery

`boot_a` can't be read back over fastboot, so keep the images to restore:

```bash
# Restore stock Android boot (also copied to arch/arm/build/rog5/ on the Windows box)
fastboot flash boot_a ~/phone_breaker2/recovery-kit/stock-boot-original.img
fastboot reboot

# Linux through the shim: ~/phone_breaker2/linux-boot-rog5.img
```

Full firmware backup: `~/phone_breaker2/backups/` (WW_ZS673KS_18.0840.2106.83).

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

### Load address and self-relocation

The kernel doesn't know its load address in advance. Through the shim it lands at
`0xA0200000`; with `kload` it lands wherever free pages were. It used to be linked
at a fixed `0x80080000`, so every pointer stored in `.data`/`.rodata` pointed into
reserved memory. That included GCC's own `switch` tables (`ps` faulted).

Since 2026-10-29 the kernel is a **PIE linked at 0** (`-fpie`, `-pie
--no-dynamic-linker -z text`):
- The linker emits only `R_AARCH64_RELATIVE` entries in `.rela.dyn` (37 of them
  at the time).
- Before any C code, `start.S` adds the load address to each of them. Stored
  pointers, function-pointer tables and string tables work normally.
- Linker-script symbols that C reads through the GOT must be section-relative.
  An absolute symbol gets no relocation. That's why `_stack_top` is defined
  inside a `.stack` section.
- Check after linking: `aarch64-elf-readelf -rW kernel.elf` should list only
  `R_AARCH64_RELATIVE`, and `aarch64-elf-nm kernel.elf | grep " A "` should be
  empty.

### Memory Map

| Address          | Description |
|------------------|-------------|
| `0x80000000`     | DDR Bank 0 start (880MB) |
| `0x80000000`–`0x9BC00000` | Reserved-memory carve-outs (hyp, smem, `cdsp_secure_heap` `0x80C00000–0x85200000`, PIL regions, modem…). Don't touch |
| `0x80000000`–`0xB5000000` | DDR bank 0 per the DTB memory node (next bank: `0xC0000000`) |
| `0xA8000000`–`0xB5000000` | Osito-K page allocator; its top 2 MB (`0xB4E00000`) is the uncached DMA window |
| `_image_end` + 64 KB | Osito-K boot stack (~`0xA0340000`; follows the actual load address) |
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

**The framebuffer must not be cached write-back**, or writes sit in the D-cache and
never reach the display controller. Osito-K maps it Normal non-cacheable (see
"MMU and caches"). Before the MMU is on, every access is uncached anyway.

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

1. **Relocation**: the kernel relocates itself, so pointers are fine. Linker-script
   symbols that C reads must be section-relative, not absolute. After linking,
   `readelf -r` must show only `R_AARCH64_RELATIVE`. See "Load address and
   self-relocation" above.

2. **No write-back caching for anything a device reads.** That covers the
   framebuffer and DMA buffers. Both are mapped Normal non-cacheable in `mmu.c`; take
   DMA memory from `dma_alloc()`, not the page allocator.

3. **UART TX timeout**: Never infinite-poll. Use `for (int i = 0; i < 100000; i++)`.

4. **FB before UART**: Always `fb_puts()` then `uart_puts()`, never the reverse.

5. **No button input**: SPMI observer probe causes data abort. Use UART for interaction
   or implement timeout-based auto-boot.

6. **header_version 2**: boot.img must be version 2. Version 3 = permanent logo hang.

7. **ARM64 Image header**: The first 64 bytes of the kernel binary must be a valid
   ARM64 Image header (branch instruction, magic "ARM\x64" at offset 0x38).
   `start.S` already provides this.

8. **Stack placement**: AArch64 ABI requires 16-byte stack alignment. The linker
   script sets `_stack_top = _image_end + 0x10000` (`_image_end` is 16-aligned) and
   `start.S` loads it PC-relative. Never use a fixed address in DDR bank 0: most of
   `0x80000000–0x9BC00000` is reserved (the old `0x85000000` was inside
   `cdsp_secure_heap`).

9. **Do NOT touch WDT**: Register `0x17980004` is TZ-protected. Instant crash.

10. **`clk_ignore_unused pd_ignore_unused`**: These cmdline params prevent ABL from
    shutting down clocks/power domains it thinks are unused. Important for keeping
    the display and UART alive.

---

## Development Workflow

```
Edit code → rog5-boot.sh --virt-check (QEMU regression) → put phone in fastboot
          → rog5-boot.sh --flash → read the bars / panic cells → Power + Vol-Down
```

Output appears on:
- **Phone screen**: progress bars, heartbeat and panic cells (see "Reading the screen")
- **UART** (if you have a serial cable — ROG Phone 5 supports debug UART on 3.5mm jack).
  As of 2026-10-28 TX stops draining once Osito-K runs.

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
| `arch/arm/boot/linker_sm8350.ld` | Linker script (PIE linked at 0, `.rela.dyn`, 64 KB `.stack` past the image) |
| `arch/arm/scripts/rog5-boot.sh` | Build on the Mac, pack, fetch, optionally flash |
| `arch/arm/kernel/mem.S` | Out-of-line memset/memcpy/memmove (GCC and gui/ need the symbols) |
| `arch/arm/platform/sm8350/usb.c` | USB state report, SMMU 1:1 check, gadget start |
| `arch/arm/platform/sm8350/dwc3_acm.c` | Polled DWC3 device driver + CDC-ACM serial function |
| `arch/arm/platform/sm8350/kload.c` | `kload`: receive a kernel Image over USB and jump to it |
| `arch/arm/platform/sm8350/mmu.c` | Flat MMU map, caches on, uncached DMA window, clean-and-disable handoff |
| `arch/arm/scripts/rog5-kload.ps1` | Host side of `kload` (used by `rog5-boot.sh --usb`) |
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
| `~/phone_breaker2/sm8350-boot/` (Mac) | Boot shim (v0.8) |
| `~/phone_breaker2/repack_boot.sh` (Mac) | Packs the **Linux** image; Osito-K uses `rog5-boot.sh` |
| `~/phone_breaker2/rog5-patched.dtb` (Mac) | Stock DTB + simplefb node |
| `~/phone_breaker2/recovery-kit/` (Mac) | Stock boot backup |
