# Porting OsitoK to Arduino Due R3

## Hardware: Arduino Due R3

| Spec | Arduino Due (SAM3X8E) | Wemos D1 (ESP8266) |
|------|----------------------|---------------------|
| CPU | ARM Cortex-M3 @ 84MHz | Xtensa LX106 @ 80MHz |
| SRAM | 96KB (64K + 32K) | ~50KB usable |
| Flash | 512KB internal | 4MB external SPI |
| ADC | 12-bit, 12 channels | 10-bit, 1 channel (TOUT) |
| DAC | 2x 12-bit | none |
| UART | 4x hardware | 1x (UART0) |
| SPI | 1x + USART-SPI | 1x (shared with flash) |
| GPIO | 54 pins | ~11 usable |
| USB | Native + ATmega16U2 | none |
| Voltage | 3.3V logic | 3.3V logic |

The ATmega16U2 handles USB-to-serial for programming/debug.
The main MCU is the ATSAM3X8E (ARM Cortex-M3).

## Why Due is better for Elite

1. **96KB SRAM** - Elite BBC Micro used ~32KB. Due has room for framebuffer + game state
2. **12-bit ADC, 12 channels** - Both joystick axes (VRx + VRy) with proper resolution, no ROM hacks
3. **2x DAC** - Sound output without PWM tricks
4. **54 GPIO** - Enough for buttons (fire, thrust, hyperspace, etc.) without shift registers
5. **SPI for display** - Dedicated SPI bus, not shared with boot flash
6. **Well-documented registers** - Atmel SAM3X datasheet is public, no reverse engineering needed

## What's portable (reuse as-is)

These modules are pure C/C++ with no hardware dependencies:

| Module | Files | Notes |
|--------|-------|-------|
| OsitoVM | `src/vm/vm.cpp`, `vm.h` | Bytecode interpreter, syscall dispatch |
| Pool allocator | `src/mem/pool_alloc.cpp` | Fixed-block allocator (adjust pool size for 96KB) |
| Shell logic | `src/shell/shell.cpp` | Command parsing (swap UART calls to Due UART) |
| OsitoFS logic | `src/fs/fs.cpp` | File operations (swap SPI flash backend) |
| Assembler | `tools/asm.py` | Cross-platform Python tool |
| Upload tool | `tools/upload.py` | Serial protocol (works with any UART) |

## What needs rewriting (hardware-specific)

### 1. Boot / Startup

**ESP8266**: Custom `crt0.S` → set SP, VECBASE, clear BSS, `nosdk_init`, `Cache_Read_Enable`.

**Due**: ARM Cortex-M3 standard boot:
```
Vector table at 0x00080000 (flash bank 0):
  [0] Initial SP (top of SRAM = 0x20088000)
  [1] Reset_Handler
  [2] NMI_Handler
  ...
  [14] PendSV_Handler    ← context switch goes here
  [15] SysTick_Handler   ← timer tick goes here
```

ARM startup is simpler: the hardware loads SP and jumps to Reset_Handler automatically.
No VECBASE register, no cache enable, no PLL dance.

```c
// startup.c (Due)
void Reset_Handler(void) {
    // Copy .data from flash to SRAM
    // Zero .bss
    // Enable FPU if needed (Cortex-M3 has no FPU)
    // SystemInit(): configure flash wait states, PLL for 84MHz
    // Call kernel_main()
}
```

Toolchain: `arm-none-eabi-gcc` (standard ARM GCC).

### 2. Context Switch

**ESP8266**: Xtensa exception vectors, manual save of a0-a15 + PS/SAR/EPC1 (80-byte frame), `rfe` to return.

**Due**: ARM Cortex-M3 PendSV pattern (standard RTOS technique):

```c
// PendSV_Handler (in assembly)
// Hardware auto-saves: R0-R3, R12, LR, PC, xPSR (32 bytes)
// Software saves: R4-R11 (32 bytes)
// Total context: 64 bytes

PendSV_Handler:
    // Get current PSP
    mrs   r0, psp
    // Save R4-R11
    stmdb r0!, {r4-r11}
    // Save PSP to current TCB
    // Load next TCB's PSP
    // Restore R4-R11
    ldmia r0!, {r4-r11}
    msr   psp, r0
    // Return (hardware restores R0-R3, PC, etc.)
    bx    lr
```

Key differences:
- ARM has **two stack pointers**: MSP (kernel) and PSP (tasks) - cleaner separation
- PendSV is the **lowest priority exception** - ideal for context switch
- Hardware saves half the context automatically
- No EXCM/INTLEVEL complexity

### 3. Timer Tick

**ESP8266**: FRC1 hardware timer → edge interrupt → INUM 9.

**Due**: SysTick (built into all Cortex-M):

```c
void timer_init(uint32_t hz) {
    SysTick->LOAD = (84000000 / hz) - 1;  // 84MHz / 100 = 100Hz
    SysTick->VAL  = 0;
    SysTick->CTRL = 7;  // enable, interrupt, use CPU clock
}

void SysTick_Handler(void) {
    os_tick++;
    if (need_reschedule()) {
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;  // trigger PendSV
    }
}
```

No need to clear interrupt flags manually. SysTick auto-clears on read.

### 4. UART Driver

**ESP8266**: Direct register access to `0x60000000`, ROM `uart_div_modify`.

**Due**: SAM3X UART (UART, USART0-3):

```c
// UART on pins 0/1 (RX0/TX0) = UART peripheral (not USART)
#define UART  ((Uart*)0x400E0800)

void uart_init(uint32_t baud) {
    // Enable peripheral clock for UART (PID 8)
    PMC->PMC_PCER0 = (1 << 8);
    // Disable TX/RX, reset
    UART->UART_CR = UART_CR_RSTRX | UART_CR_RSTTX;
    // Set baud: CD = MCK / (16 * baud) = 84000000 / (16 * 115200) = 45
    UART->UART_BRGR = 84000000 / (16 * baud);
    // 8N1, no parity
    UART->UART_MR = UART_MR_PAR_NO;
    // Enable TX/RX
    UART->UART_CR = UART_CR_TXEN | UART_CR_RXEN;
    // Enable RX interrupt
    UART->UART_IER = UART_IER_RXRDY;
    NVIC_EnableIRQ(UART_IRQn);
}
```

### 5. ADC Driver

This is dramatically simpler on the Due:

```c
void adc_init(void) {
    PMC->PMC_PCER1 = (1 << (37 - 32));  // Enable ADC clock
    ADC->ADC_CR = ADC_CR_SWRST;          // Reset
    ADC->ADC_MR = ADC_MR_PRESCAL(4)      // ADC clock = MCK/10
               | ADC_MR_STARTUP_SUT96;    // Startup time
    ADC->ADC_CHER = (1 << 7) | (1 << 6); // Enable CH7 (A0) + CH6 (A1)
}

uint16_t adc_read(uint8_t channel) {
    ADC->ADC_CR = ADC_CR_START;            // Start conversion
    while (!(ADC->ADC_ISR & (1 << channel)))
        ;
    return ADC->ADC_CDR[channel] & 0xFFF; // 12-bit result, 0-4095
}
```

No ROM hacks, no I2C bus, no calibration needed. Both joystick axes at once.

### 6. GPIO (Buttons)

```c
// Configure GPIO as input with pull-up
void gpio_init_button(Pio *port, uint32_t pin) {
    port->PIO_PER  = pin;   // PIO control (not peripheral)
    port->PIO_ODR  = pin;   // Output disable (input mode)
    port->PIO_PUER = pin;   // Pull-up enable
}

// Read button (active low)
bool gpio_read(Pio *port, uint32_t pin) {
    return !(port->PIO_PDSR & pin);
}

// Example: 6 buttons for Elite
// Pin 2 (PB25) = Fire
// Pin 3 (PC28) = Thrust
// Pin 4 (PC26) = Hyperspace
// Pin 5 (PC25) = ECM
// Pin 6 (PC24) = Missile
// Pin 7 (PC23) = Docking
```

### 7. Flash Storage for OsitoFS

Options:
- **External SPI flash** (W25Q32, same as ESP8266) - connect to SPI bus, reuse OsitoFS
- **Internal flash** (512KB, bank 1 = 256KB free) - use SAM3X EFC (Embedded Flash Controller)
- **SD card** via SPI - more storage, FAT compatible

For OsitoFS, external SPI flash is the easiest port:
```c
// SPI flash on Due SPI bus (pins 74-77 or ICSP header)
// Replace ESP8266 ROM SPIRead/SPIWrite/SPIEraseSector with:
void spi_flash_read(uint32_t addr, void *dst, uint32_t len);
void spi_flash_write(uint32_t addr, const void *src, uint32_t len);
void spi_flash_erase_sector(uint32_t sector);
```

### 8. SPI Display

The Due's SPI bus is dedicated (not shared with boot flash like ESP8266):

```c
// SPI display on standard SPI pins
// MOSI = pin 75 (SPI header)
// SCK  = pin 76
// CS   = any GPIO
// DC   = any GPIO

void spi_init(void) {
    PMC->PMC_PCER0 = (1 << 24);  // SPI clock
    SPI0->SPI_CR = SPI_CR_SPIDIS;
    SPI0->SPI_MR = SPI_MR_MSTR | SPI_MR_PS;
    SPI0->SPI_CSR[0] = SPI_CSR_SCBR(2) | SPI_CSR_BITS_8_BIT;
    SPI0->SPI_CR = SPI_CR_SPIEN;
}
```

SPI clock can reach 42MHz (MCK/2), fast enough for display refresh.

## Porting plan

### Phase 1: Boot + UART
1. Set up `arm-none-eabi-gcc` toolchain
2. Write linker script for SAM3X8E (flash at 0x80000, SRAM at 0x20070000)
3. Write startup code (vector table, Reset_Handler, SystemInit)
4. Port UART driver
5. Verify: serial output at 115200

### Phase 2: Kernel
1. Port context switch to PendSV
2. Port SysTick timer (100Hz)
3. Port scheduler (minimal changes - just swap interrupt primitives)
4. Verify: `ps`, `ticks` commands working

### Phase 3: Peripherals
1. Port ADC (trivial on Due)
2. Port GPIO buttons
3. Port SPI flash or add SD card for OsitoFS
4. Copy VM, shell, pool allocator (no changes needed)
5. Verify: full shell with `joy`, `run`, `ls`

### Phase 4: Display (new)
1. Add SPI display driver (ST7735 or SSD1306)
2. Implement framebuffer + line drawing
3. Add display syscalls to VM
4. Verify: draw wireframe shapes from VM programs

## Toolchain setup

```bash
# Install ARM GCC (Windows)
# Download from: https://developer.arm.com/downloads/-/gnu-rm
# Or via package manager:
# scoop install gcc-arm-none-eabi

# Verify
arm-none-eabi-gcc --version

# Build
arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -O2 -c startup.c
arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -T sam3x8e.ld -o osito.elf *.o
arm-none-eabi-objcopy -O binary osito.elf osito.bin

# Flash via USB (native port, not programming port)
# Use bossac (comes with Arduino IDE) or openocd
bossac --port=COMx -U true -e -w -b osito.bin -R
```

## Memory map (SAM3X8E)

```
0x00080000 - 0x000BFFFF  Flash Bank 0 (256KB) - kernel code
0x000C0000 - 0x000FFFFF  Flash Bank 1 (256KB) - OsitoFS or game data
0x20070000 - 0x20087FFF  SRAM0 (64KB) - heap, stacks, framebuffer
0x20088000 - 0x2008FFFF  SRAM1 (32KB) - pool allocator, game state
0x400xxxxx                Peripherals (UART, SPI, ADC, PIO, etc.)
```

## Pin mapping for Elite

```
Arduino Due Pin  | Function        | SAM3X Pin
-----------------|-----------------|----------
A0 (AD7)         | Joystick VRx    | PA16 (ADC CH7)
A1 (AD6)         | Joystick VRy    | PA24 (ADC CH6)
D2               | Button: Fire    | PB25
D3               | Button: Thrust  | PC28
D4               | Button: Hyper   | PC26
D5               | Button: ECM     | PC25
D6               | Button: Missile | PC24
D7               | Button: Dock    | PC23
D10 (SPI CS)     | Display CS      | PA28
D9               | Display DC      | PC21
ICSP MOSI        | Display MOSI    | PA26
ICSP SCK         | Display SCK     | PA27
D0 (RX0)         | Serial RX       | PA8 (URXD)
D1 (TX0)         | Serial TX       | PA9 (UTXD)
DAC0             | Sound output    | PB15 (DACC CH0)
```
