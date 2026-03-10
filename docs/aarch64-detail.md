# OsitoK AArch64/SM8350 — Port Details

> Referenced from [CLAUDE.md](../CLAUDE.md). Contains hardware reference for ASUS ROG Phone 5 port.

### AArch64/SM8350 Port (arch/arm/)
Reference bare-metal code for ASUS ROG Phone 5 (Snapdragon 888).
All code verified on real hardware via sm8350-boot bootloader (2026-03-03/04).

```
arch/arm/include/sm8350.h      SM8350 MMIO register definitions (verified)
arch/arm/include/aarch64.h     System register helpers, GICv3 ICC, timer, context frame
arch/arm/boot/start.S          EL1 entry, ARM64 image header, exception vectors
arch/arm/boot/linker.ld        Linker script (WARNING: load addr != link addr)
arch/arm/drivers/geni_uart.c   Qualcomm GENI UART (TX timeout mandatory)
arch/arm/drivers/framebuffer.c Splash FB console (ARGB8888, 1080x2448, 0xE5000000)
arch/arm/drivers/timer.c       ARM Generic Timer (19.2 MHz, scheduler tick)
arch/arm/drivers/spmi.c        SPMI PMIC buttons (WIP, data abort on observer probe)
```

**Critical rules for AArch64 SM8350**:
- ABL loads at ~0xA0080000, NOT 0x80080000. Never use stored pointers (`char *p = "str"`), always use arrays (`char p[] = "str"`)
- UART TX MUST have timeout (~100k iterations) or system hangs
- FB output BEFORE UART output in dual-output wrappers
- PSCI reboot: `ldr x0, =0x84000009` then `smc #0` (not mov — immediate too large)

See `docs/porting-rog5-sm8350.md` for full hardware reference and verified findings.
