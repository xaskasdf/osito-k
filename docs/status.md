# OsitoK - Project Status

## Current Platform
Wemos D1 Mini (ESP8266EX, Xtensa LX106 @ 80MHz, 4MB SPI flash)

## Implemented Features

### Feature 1: OsitoFS (Flat Filesystem)
- Flat filesystem on SPI flash (sectors 0x200-0x3FF, 512 sectors)
- Superblock at sector 0x200 with file table (max 64 files)
- Commands: `ls`, `cat`, `rm`, `rename`
- Binary upload via `tools/upload.py` over serial
- Overwrite, append, rename support

### Feature 2: OsitoVM (Bytecode Stack Machine)
- 16-bit stack-based VM with 256-byte stack
- 32 opcodes: arithmetic, stack, control flow, I/O, syscalls
- Local variables (8 slots), labels, conditional jumps
- Assembler: `tools/asm.py` (Python, generates .vm files)
- Shell: `run <file.vm>` executes bytecode from OsitoFS
- Syscalls: putc, put_dec, getc, delay, fs_read, fs_write, halt,
  input_poll, input_state, adc_read

### Feature 3: Joystick Input + ADC
- Bare-metal SAR ADC driver via ROM I2C functions
- Calibrated linear mapping (11-bit range 398-474 → 0-1023)
- Input subsystem: 50Hz polling, ring buffer (16 events), debounce
- GPIO12 button with 3-tick debounce
- Dead zone detection (LEFT < 400, RIGHT > 600)
- Shell commands: `adc` (raw + scaled readings), `joy` (live monitor)
- VM syscalls: input_poll (11), input_state (12), adc_read (13)
- **Note**: ADC works but joystick hardware needs wiring fix

### Kernel
- Preemptive round-robin scheduler (8 task slots)
- FRC1 timer at 100Hz
- Context switch via Xtensa exception vectors
- Software yield via INUM 7
- Task creation with configurable stack (1536 bytes default)

### Drivers
- **UART0**: Polled TX, interrupt RX, ring buffer, mutex
- **ADC**: SAR ADC via ROM functions, calibrated for no-PHY-init operation
- **GPIO**: Input with pull-up (GPIO12 for button)

### Memory
- Pool allocator: 256 blocks x 32 bytes = 8KB
- Static allocation for task stacks and kernel structures

### Shell
- Interactive command line over UART0 (74880 baud actual)
- Commands: `help`, `ps`, `mem`, `ticks`, `uname`, `reboot`,
  `ls`, `cat`, `rm`, `rename`, `upload`, `run`, `adc`, `joy`

## Build Stats
```
text    data    bss     total
16366   4476    34468   55310 bytes
```

## Examples
| File | Description |
|------|-------------|
| `examples/hello.asm` | Hello world (putc loop) |
| `examples/math.asm` | Arithmetic demo |
| `examples/blink.asm` | Delay loop demo |
| `examples/joystick.asm` | ADC + input events |

## Known Limitations
- ADC reference is ~2.2V without PHY init (calibration compensates)
- Single ADC channel (TOUT only, no VRy for joystick Y-axis)
- 74880 baud actual (PLL not fully configured, 52MHz APB)
- No display output (serial terminal only)
- No sound

## Next Steps (for Elite port)
1. Acquire SPI display (ST7735 TFT or SSD1306 OLED)
2. Implement SPI display driver + framebuffer
3. Add line drawing (Bresenham) and text rendering
4. Fixed-point math library (8.8, 16.16)
5. Consider porting to Arduino Due for better ADC/GPIO/DAC
   (see `docs/porting-arduino-due.md`)
