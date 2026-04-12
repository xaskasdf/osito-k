# OsitoK - Project Status

> Two parallel platforms: x86-64 bare-metal AI OS (the active one) and the
> original ESP8266 dev board. The x86 line is where the kernel grew up;
> the ESP8266 line is preserved as the original substrate.

## x86-64 Platform (active)

UEFI-booted bare-metal kernel for QEMU/q35 and real hardware. ELF64,
preemptive scheduler, 4-level paging, 100+ Linux syscalls, three filesystem
drivers (osfs2/osfs3 native + FAT32/ext2/NTFS host), full TCP/IP+TLS, SMP,
Win32 PE compat, GUI compositor, GPU compute (RTX), self-hosting via TCC.

### Last stabilization sweep — 2026-04-12

Eight commits (`be59c00..207900d`) brought the kernel from "loads small
TCC binaries" to "runs static musl binaries up to 6.5 MB end-to-end". Full
detail in [`x86-vfs-demand-paging.md`](x86-vfs-demand-paging.md).

| Subsystem | Before | After |
|-----------|--------|-------|
| FS namespace | Flat (osfs2) | Hierarchical (osfs3) + flat (osfs2) via VFS |
| ELF loader | `kmalloc(file_size)` eager (~10 MB ceiling) | Header-only read + demand paging |
| `vma_t` | base/pages/prot only | + type / file_node / file_offset / file_size |
| `demand_page_fault` | Stub: alloc-and-map any high addr | VMA-validated + file-backed page-in |
| `proc_fd_t` per-process | Dead code (67 lines) | Removed |
| Pipes | Always returned `EAGAIN` | Blocking by default, `O_NONBLOCK` honored |
| Self-write of executing binary | Allowed (silent corruption) | `-ETXTBSY` |
| Stdio FDs across exec | Could leak file handles to parent | Force-reset to console |
| Kernel paging structs | Allocated bottom-up (collide with ELF loads) | High-memory allocator |
| `getrusage`/`rt_sigsuspend`/`setitimer` | Unknown syscall (zsh hung) | Stubs |
| `kthread_wrapper` | Empty stub | Reads func+data from kthreads table |
| `io_uring` READ/WRITE | Hardcoded `-ENOSYS` | Wired to syscall dispatch |

### Verified workloads

- **`hello_ositok.elf`** (Zig 0.13.0, 9.6 KB static) — boots and prints
  via two `write()` syscalls
- **`zsh.elf`** (musl 5.9, 1.4 MB static) — boots, prints prompt, accepts
  USB keyboard, fork+pipe subshell coordination, exits clean
- **`GTA5.elf`** (RAGE engine, 6.5 MB static) — demand-pages instantly into
  4476 KB R+X + 1188 KB R+W VMAs, runs to expected SIGSEGV in RAGE init
  (no asset RPFs loaded)
- All three: `md5(nvme.img/binary)` identical pre/post run thanks to ETXTBSY

### x86-64 build / run

```bash
cd /Users/pc/osito-k/arch/x86
make build/kernel.elf -j$(sysctl -n hw.ncpu) \
     CC=x86_64-elf-gcc LD=x86_64-elf-ld OBJCOPY=x86_64-elf-objcopy
bash scripts/qemu-test.sh --no-build
```

Full `make all` (including `boot.efi`) needs gnu-efi installed at
`/private/tmp/gnu-efi/`. The qemu-test.sh script handles ESP image creation
and pflash setup on macOS automatically.

### Known issues (after this sweep)

1. **Auxv setup loop crash** — `elf_setup_stack` can fault on a write to
   address `-8` when GCC vectorizes the auxv build loop with a backward
   iteration. Stack-frame-layout sensitive. zsh hits it in some builds,
   GTA5 didn't. See `x86-vfs-demand-paging.md` for workaround candidates.
2. **fork+execve of same binary on demand path** — child page faults
   rewrite parent PTEs. Static binaries that fork+exec themselves should
   link with `PT_DYNAMIC` to use the eager path.
3. **Single shared address space** — no per-process PML4. Two processes
   loading at the same vaddr (e.g. zsh and GTA5 both at 0x20000000) cannot
   coexist concurrently.

### Roadmap files

- [`x86-features-detail.md`](x86-features-detail.md) — full feature catalog
  (X9..X42, X-OS*, X-NET*, X-CL*, X-WIN32, …)
- [`x86-vfs-demand-paging.md`](x86-vfs-demand-paging.md) — this sweep
- [`ositofs2-spec.md`](ositofs2-spec.md) / [`ositofs3-spec.md`](ositofs3-spec.md) — FS specs
- [`x86-gpu-roadmap.md`](x86-gpu-roadmap.md) — GPU compute (X27..X40)
- [`os-selfhost-roadmap.md`](os-selfhost-roadmap.md) — Tier 0..9
- [`binary-compat-roadmap.md`](binary-compat-roadmap.md) — Linux/Win32/PE/DOS
- [`win32-improvements.md`](win32-improvements.md) — PE compat layer
- [`game-rendering-guide.md`](game-rendering-guide.md) — porting guide

---

## ESP8266 Platform (preserved)

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
