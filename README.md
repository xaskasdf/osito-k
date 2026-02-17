```
+============================================================================+
|                                                                            |
|                       O S I T O - K   v 0 . 1                              |
|                                                                            |
|               PREEMPTIVE MULTITASKING OPERATING SYSTEM KERNEL              |
|                                                                            |
|                   FOR THE XTENSA LX106 MICROPROCESSOR                      |
|                                                                            |
|                        OPERATOR'S REFERENCE MANUAL                         |
|                                                                            |
|                          REVISION 3 -- FEB 2026                            |
|                                                                            |
+============================================================================+
```

                            *** IMPORTANT NOTICE ***

      This manual describes the installation, configuration, and operation
      of the Osito-K Preemptive Kernel, Version 0.1. The operator should
      read this document in its entirety before attempting to initialize
      the system. Improper configuration may result in unpredictable
      behavior of the processing unit.


## TABLE OF CONTENTS

```
  SECTION                                                            PAGE
  -------                                                            ----
  I.    SYSTEM OVERVIEW ........................................      1
  II.   HARDWARE SPECIFICATIONS ................................      2
  III.  SYSTEM REQUIREMENTS ....................................      3
  IV.   INSTALLATION PROCEDURE .................................      4
  V.    BUILDING THE SYSTEM FROM SOURCE ........................      5
  VI.   LOADING THE SYSTEM INTO MEMORY .........................      6
  VII.  SYSTEM INITIALIZATION SEQUENCE .........................      7
  VIII. OPERATOR CONSOLE COMMANDS ..............................      8
  IX.   INTER-PROCESS COMMUNICATION ............................      9
  X.    FILESYSTEM (OsitoFS) ...................................     10
  XI.   3D GRAPHICS AND ELITE FLIGHT DEMO ......................     11
  XII.  ZFORTH INTERACTIVE LANGUAGE ............................     12
  XIII. SYSTEM ARCHITECTURE ....................................     13
  XIV.  MEMORY MAP .............................................     14
  XV.   SOURCE FILE DIRECTORY ..................................     15
  XVI.  KNOWN LIMITATIONS ......................................     16
  XVII. WARRANTY AND DISCLAIMER ................................     17
```

---

## I. SYSTEM OVERVIEW

Osito-K (from Spanish *osito*, "little bear") is a **bare-metal preemptive
multitasking kernel** engineered for the Xtensa LX106 processor.
A [naranjositos.tech](https://naranjositos.tech/) project. The system
operates without the assistance of any vendor SDK, communicating directly
with the hardware registers of the processing unit.

The kernel provides the following capabilities:

  - **Priority-based preemptive scheduling** at a rate of 100 ticks per
    second. Higher-priority tasks always preempt lower-priority ones;
    tasks at the same priority level are scheduled round-robin.
  - **Hardware interrupt-driven context switching** with full preservation
    of all 16 general-purpose registers, the Processor Status word, the
    Shift Amount Register, and the Exception Program Counter.
  - **Two-tier memory allocation**: a fixed-block pool (8 KB, 256 x 32-byte
    blocks) for fast O(1) alloc/free, and a general-purpose heap allocator
    (8 KB) with first-fit allocation and automatic coalescing.
  - **Inter-process communication** via counting semaphores, mutexes,
    and bounded message queues with blocking and non-blocking modes.
  - **Software timers** with one-shot and periodic modes, serviced by
    the kernel tick interrupt at zero additional hardware cost.
  - **General-purpose I/O** with automatic IOMUX configuration for all
    17 GPIO pins, including the special RTC-domain GPIO16.
  - **Flat filesystem (OsitoFS)** on SPI flash with contiguous allocation,
    bitmap-based sector management, and up to 128 files on ~3.8 MB of
    storage. Inspired by the BBC Micro's DFS.
  - **Interrupt-driven serial communications** with a 64-byte receive
    buffer and cooperative mutual exclusion for output operations.
  - **An interactive operator console** with 25+ built-in commands for
    system monitoring, file operations, hardware control, 3D graphics,
    and an Elite flight demo.
  - **zForth interactive language** — a minimal Forth interpreter with
    hardware syscalls for framebuffer graphics, wireframe 3D rendering,
    and task scheduling. Scripts stored on OsitoFS.
  - **3D wireframe graphics** with fixed-point math, 3x3 matrix rotation,
    perspective projection, and Elite ship models (Cobra, Sidewinder,
    Viper, Coriolis). All running at 80 MHz with no floating point.


## II. HARDWARE SPECIFICATIONS

The Osito-K kernel is designed to exploit the full capabilities of the
following high-performance computing platform:

```
+==========================================================================+
|                      WEMOS D1 COMPUTING MODULE                           |
|                      TECHNICAL SPECIFICATIONS                            |
+==========================================================================+
|                                                                          |
|  PROCESSOR                                                               |
|  .........                                                               |
|    Model ............... Tensilica Xtensa LX106                          |
|    Architecture ........ 32-bit RISC, Harvard                            |
|    Clock Speed ......... 80,000,000 cycles/second  (80 MHz)             |
|    Instruction Set ..... Xtensa ISA, CALL0 ABI                          |
|    Register File ....... 16 general-purpose, 32-bit wide                |
|    Register Windows .... None (CALL0 convention)                         |
|    Pipeline ............ In-order, single issue                          |
|    Interrupt Levels .... 2 (Level-1 maskable, NMI)                      |
|    Memory Management ... None (flat address space)                       |
|                                                                          |
|  MEMORY SUBSYSTEM                                                        |
|  ................                                                         |
|    Data RAM (DRAM) ..... 81,920 bytes    (80 KB)                        |
|    Instruction RAM ..... 32,768 bytes    (32 KB)                        |
|    Flash Storage ....... 4,194,304 bytes (4 MB)                         |
|    Flash Interface ..... Memory-mapped, cached (irom0)                   |
|    Flash Mode .......... DOUT (Dual Output)                              |
|                                                                          |
|  PERIPHERALS                                                             |
|  ...........                                                             |
|    UART Channels ....... 2 (UART0 active, 115200 baud)                  |
|    Hardware Timers ..... 2 (FRC1 assigned to kernel)                     |
|    GPIO Pins ........... 17 (active low/high, multiplexed)              |
|    SPI Controllers ..... 2                                               |
|    I2C Bus ............. 1 (software-emulated)                           |
|    WiFi Transceiver .... IEEE 802.11 b/g/n (reserved for future use)    |
|    ADC ................. 1 channel, 10-bit resolution                    |
|                                                                          |
|  POWER                                                                   |
|  .....                                                                   |
|    Operating Voltage ... 3.3V                                            |
|    Supply (USB) ........ 5.0V via Micro-USB connector                   |
|                                                                          |
|  FORM FACTOR                                                             |
|  ...........                                                             |
|    Board Dimensions .... 34.2mm x 25.6mm                                |
|    Weight .............. Approximately 3 grams                           |
|                                                                          |
+==========================================================================+
```

      NOTE: The 80 MHz clock provides ample processing power for the
      simultaneous execution of up to eight (8) independent programs.
      The operator should not be concerned about resource exhaustion
      under normal workloads.


## III. SYSTEM REQUIREMENTS

The following items are required prior to system assembly:

```
  ITEM                            PURPOSE
  ----                            -------
  Wemos D1 Mini board             Target computing hardware
  USB cable (Micro-B)             Power supply and serial link
  Host computer (Windows)         Assembly and loading station
  xtensa-lx106-elf-gcc            Cross-compilation toolchain
  esptool (Python)                Firmware loading utility
  Serial terminal program         Operator console access
```

The cross-compilation toolchain may be obtained from the Espressif Systems
distribution archive. The operator should place the toolchain binaries in
the `tools/xtensa-lx106-elf/` directory.


## IV. INSTALLATION PROCEDURE

**Step 1.** Obtain the system source distribution and place it in a
suitable working directory:

```
  C:\Users\operator\osito-k\
```

**Step 2.** Ensure the cross-compilation toolchain is accessible. Add
the toolchain to the command search path:

```
  export PATH="/c/Users/operator/osito-k/tools/xtensa-lx106-elf/bin:$PATH"
```

**Step 3.** Verify the toolchain installation by invoking:

```
  xtensa-lx106-elf-gcc --version
```

The system should respond with the compiler identification string. If no
response is received, consult the toolchain installation documentation.

**Step 4.** Ensure the Python `esptool` module is installed:

```
  py -m esptool version
```


## V. BUILDING THE SYSTEM FROM SOURCE

To assemble the kernel from its component source modules, issue the
following command from the system root directory:

```
  make
```

The assembler will process all source modules and produce the following
output files:

```
  build/osito.elf              Executable and Linkable Format image
  build/osito.map              Memory allocation map
  build/osito0x00000.bin       Binary image for flash programming
```

Upon successful assembly, the system will display a summary of memory
utilization:

```
  === Osito-K build complete ===
     text    data     bss     dec     hex filename
    24742    9992   37232   71966   1191e build/osito.elf
```

      NOTE: The .bss segment includes all task stacks, the memory pool,
      the heap arena, and the filesystem sector buffer, which accounts
      for the majority of RAM allocation.

      IMPORTANT: The image version parameter MUST be set to 1.
      Version 2 images are not compatible with this hardware.
      The flash mode MUST be DOUT. QIO mode will cause boot failure.


## VI. LOADING THE SYSTEM INTO MEMORY

**Step 1.** Connect the Wemos D1 computing module to the host computer
via the USB cable.

**Step 2.** Determine the serial port assignment (typically `COM4` on
Windows systems).

**Step 3.** Transfer the system image to the flash memory:

```
  make flash
```

Or manually:

```
  py -m esptool --chip esp8266 --port COM4 --baud 460800 \
      write_flash --flash_mode dout --flash_size 4MB \
      --flash_freq 40m 0x00000 build/osito0x00000.bin
```

**Step 4.** The loading utility will display progress indicators.
Wait for the message:

```
  Hard resetting via RTS pin...
```

The system is now loaded and will begin execution automatically upon
the next power-on or reset event.

      CAUTION: To restore the factory firmware at any time, the
      operator may execute:

        py -m esptool --port COM4 write_flash 0x0 \
            backup/wemos_d1_full_backup.bin


## VII. SYSTEM INITIALIZATION SEQUENCE

Upon power application, the kernel performs the following initialization
sequence automatically:

```
  PHASE    OPERATION                          DESCRIPTION
  -----    ---------                          -----------
    1      ROM Bootloader                     Hardware self-test, flash load
    2      _start (crt0.S)                    Set stack pointer, VECBASE, clear BSS
    3      nosdk_init                         Disable watchdog, PLL to 80 MHz
    4      uart_init                          Serial port: 115200 8N1, RX interrupts
    5      pool_init                          Memory pool: 256 blocks x 32 bytes
    6      heap_init                          Heap allocator: 8192 bytes
    7      fs_init                            Mount filesystem (if formatted)
    8      sched_init                         Scheduler: idle task created
    9      input_init                         Joystick ADC + button GPIO setup
   10      video_init                         Framebuffer 128x64 (1024 bytes)
   11      task_create (x2)                   Input (pri=2) and Shell (pri=3)
   12      timer_init                         FRC1 armed: 100 Hz, prescaler /16
   13      sched_start                        Context loaded, rfe — system live
```

The operator console will display the following banner:

```
  =============================
    Osito-K v0.1
    Bare-metal kernel for ESP8266
  =============================
  pool: initialized 256 blocks x 32 bytes = 8192 bytes
  heap: 8192 bytes
  fs: mounted, 0 files, 958 sectors
  sched: initialized, idle task created
  video: framebuffer 128x64 (1024 bytes)
  sched: created task 'input' (id=1)
  sched: created task 'shell' (id=2)
  timer: FRC1 configured at 100 Hz (load=50000)

  Starting kernel...

  sched: starting scheduler

  osito>
```

The `osito>` prompt indicates the system is operational and awaiting
operator input.

      NOTE: The serial console operates at 74880 baud during the ROM
      bootloader phase, then 115200 baud during kernel operation.
      Some terminal programs may display garbled output during the
      initial boot phase. This is normal and expected.


## VIII. OPERATOR CONSOLE COMMANDS

The Osito-K interactive shell accepts the following commands at the
`osito>` prompt. Commands are terminated by pressing the RETURN key.
The BACKSPACE key may be used to correct input errors.

```
+----------+----------------------------------------------------------+
| COMMAND  | DESCRIPTION                                              |
+----------+----------------------------------------------------------+
|          |                                                          |
| ps       | Display a listing of all active tasks in the system.     |
|          | For each task: ID, Priority, State, Ticks, Name.         |
|          |                                                          |
| mem      | Display memory pool utilization statistics.               |
|          |                                                          |
| heap     | Display heap allocator statistics: free, used, largest   |
|          | contiguous block, and fragmentation count.               |
|          |                                                          |
| heap test| Demonstrate heap allocation and coalescing by allocating |
|          | and freeing several blocks with interleaved patterns.    |
|          |                                                          |
| ticks    | Display the current system tick counter and the          |
|          | equivalent elapsed time in seconds.                      |
|          |                                                          |
| gpio     | Display the state of all safe GPIO pins: direction,     |
|          | value, and Wemos D1 board label (D0-D8).                |
|          |                                                          |
| gpio     | Read a specific pin. Returns 0 or 1.                     |
|   read N |                                                          |
|          |                                                          |
| gpio     | Set pin N as output HIGH or LOW.                         |
|  high N  |                                                          |
|  low N   |                                                          |
|          |                                                          |
| gpio     | Blink the onboard LED (GPIO2) five times.                |
|  blink   |                                                          |
|          |                                                          |
| fs       | Filesystem commands (see Section X for details).         |
|          |                                                          |
| pri N P  | Change the priority of task N to level P. Takes effect   |
|          | at the next scheduling decision. Priority 0 is lowest.   |
|          |                                                          |
| timer    | Arm a one-shot software timer for 1 second. Reports     |
|          | the actual elapsed ticks when it fires.                   |
|          |                                                          |
| forth    | Enter the zForth interactive REPL. Type Forth code       |
|          | at the prompt. Press Ctrl+C to return to the shell.      |
|          |                                                          |
| run F    | Execute a .zf Forth script stored in OsitoFS.            |
|          | Usage: run <filename>                                    |
|          |                                                          |
| joy      | Joystick live monitor. Displays ADC value, button state, |
|          | and input events in real time. Press Ctrl+C to exit.     |
|          |                                                          |
| fbtest   | Draw a test pattern on the 128x64 framebuffer:           |
|          | border, title text, and character set sample.            |
|          |                                                          |
| fixtest  | Run the fixed-point 16.16 math test suite: sin, cos,    |
|          | sqrt, div, lerp, and distance approximation.             |
|          |                                                          |
| mat3test | Run the 3D matrix/vector math test: rotations,           |
|          | projections, and matrix multiplication.                  |
|          |                                                          |
| wiretest | Render a static wireframe cube to the framebuffer.       |
|          |                                                          |
| wirespin | Animate a spinning wireframe cube (~5 seconds).          |
|          | Press Ctrl+C to stop early.                              |
|          |                                                          |
| ship [N] | Display Elite ship model (1=cobra, 2=sidewinder,         |
|          | 3=viper, 4=coriolis). No argument lists all.             |
|          |                                                          |
| shipspin | Cycle through all ship models with rotation animation.   |
|          |                                                          |
| elite    | Launch the Elite flight demo. Keyboard controls:          |
|          | a/d=yaw, w/s=pitch, n=next ship. Press Ctrl+C to exit.  |
|          |                                                          |
| uname    | Display system identification: kernel version, CPU,      |
|          | clock speed, memory sizes, tick rate, max tasks.         |
|          |                                                          |
| help     | Display a summary of available commands.                 |
|          |                                                          |
| reboot   | Perform an immediate software reset of the processor.    |
|          |                                                          |
+----------+----------------------------------------------------------+
```

**Task States:**

```
  STATE    MEANING
  -----    -------
  free     Task slot is unoccupied and available for allocation
  ready    Task is eligible for execution and awaiting its turn
  run      Task is currently in possession of the processor
  block    Task is voluntarily suspended (waiting for a resource)
  dead     Task has terminated and its slot may be reclaimed
```

**Sample session:**

```
  osito> ps
  ID  Pri  State  Ticks  Name
  0   0    ready  1  idle
  1   2    ready  0  input
  2   3    run    206  shell

  osito> uname
  Osito-K v0.1 xtensa-lx106 ESP8266 @ 80MHz DRAM:80KB IRAM:32KB tick:100Hz tasks:8

  osito> mem
  Memory pool:
    Block size:  32 bytes
    Total:       256 blocks (8192 bytes)
    Free:        256 blocks
    Used:        0 blocks

  osito> heap
  Heap:
    Total:      8192 bytes
    Free:       8188 bytes
    Used:       0 bytes
    Largest:    8188 bytes
    Fragments:  1

  osito> forth
  zf: ready (Ctrl+C exit)
  1 2 + .
  3  ok
  : sq dup * ;
   ok
  7 sq .
  49  ok
  [Ctrl+C]

  osito> fs write test.zf : cube dup dup * * ; 5 cube .
  wrote 29 bytes to 'test.zf'
  osito> run test.zf
  125  ok

  osito> timer
  timer: armed 1s one-shot... FIRED! (100 ticks)
  active timers: 0

  osito> ticks
  Tick count: 808 (8 seconds)
```


## IX. INTER-PROCESS COMMUNICATION

Osito-K provides three IPC primitives for synchronization and data
exchange between tasks:

**Counting Semaphores:**

```
  FUNCTION                 DESCRIPTION
  --------                 -----------
  sem_init(&s, count)      Initialize with given count
  sem_wait(&s)             Decrement; block if count is zero
  sem_trywait(&s)          Non-blocking attempt (returns -1 if zero)
  sem_post(&s)             Increment; wake one blocked task
  sem_count(&s)            Read current count
```

Semaphores are used for resource counting and event signaling.
A semaphore initialized to 1 functions as a mutual exclusion lock
(mutex). The UART output subsystem uses this mechanism to prevent
interleaved output from concurrent tasks.

**Message Queues:**

```
  FUNCTION                 DESCRIPTION
  --------                 -----------
  mq_init(&q, buf, sz, n) Initialize queue with buffer, message size, depth
  mq_send(&q, msg)         Enqueue message; block if full
  mq_recv(&q, msg)         Dequeue message; block if empty
  mq_trysend(&q, msg)      Non-blocking send (returns -1 if full)
  mq_tryrecv(&q, msg)      Non-blocking receive (returns -1 if empty)
  mq_count(&q)             Number of messages currently queued
```

Messages are copied by value. The queue is implemented as a circular
buffer with head and tail pointers. The `ping` shell command demonstrates
message queue operation by sending a message from the shell task to the
heartbeat task.

**Software Timers:**

```
  FUNCTION                      DESCRIPTION
  --------                      -----------
  swtimer_init(&t, cb, arg)     Initialize with callback and argument
  swtimer_start(&t, tk, mode)   Arm for tk ticks (ONESHOT or PERIODIC)
  swtimer_stop(&t)              Disarm the timer
  swtimer_active_count()        Number of currently armed timers
```

Timers are serviced by the kernel tick ISR at 100 Hz. Callbacks execute
in interrupt context and must be brief. The `timer` shell command
demonstrates one-shot timer operation.


## X. FILESYSTEM (OsitoFS)

OsitoFS is a flat filesystem on SPI flash, inspired by the BBC Micro's
Disc Filing System (DFS). It provides persistent file storage across
power cycles.

**Design Characteristics:**

```
  Property                  Value
  --------                  -----
  Maximum files             128
  Maximum filename length   23 characters (+ null terminator)
  Maximum file size         ~3.8 MB (limited by flash capacity)
  Allocation strategy       Contiguous (no fragmentation within files)
  Sector size               4,096 bytes
  Data sectors available    958 (~3,832 KB)
  Directory structure       Flat (no subdirectories)
```

**Flash Layout:**

```
  ADDRESS     SIZE     CONTENTS
  -------     ----     --------
  0x00000     256 KB   Kernel firmware image
  0x40000     4 KB     Superblock (magic, version, statistics)
  0x41000     4 KB     File table (128 entries x 32 bytes)
  0x42000     3,832KB  Data area (958 sectors)
  0x400000    ---      End of 4 MB flash
```

**Shell Commands:**

```
  COMMAND                  DESCRIPTION
  -------                  -----------
  fs format                Create a fresh filesystem (erases all files)
  fs ls                    List all files with size and sector count
  fs df                    Display free space in KB and bytes
  fs write NAME DATA       Create a file with the given text content
  fs overwrite NAME DATA   Overwrite an existing file (or create new)
  fs append NAME DATA      Append data to an existing file
  fs mv OLD NEW            Rename a file
  fs cat NAME              Print file contents to the console
  fs xxd NAME              Hex dump of file contents (up to 256 bytes)
  fs rm NAME               Delete a file and reclaim its sectors
  fs upload NAME SIZE      Receive binary file via UART (see below)
  fs help                  Show filesystem command summary
```

**Sample filesystem session:**

```
  osito> fs format
  fs: formatting...
  fs: formatted, 958 sectors (3832 KB) available

  osito> fs write hello.txt Hello from Osito-K!
  wrote 18 bytes to 'hello.txt'

  osito> fs overwrite hello.txt Goodbye!
  wrote 8 bytes to 'hello.txt'

  osito> fs cat hello.txt
  Goodbye!

  osito> fs append hello.txt  See you later.
  appended 14 bytes to 'hello.txt'

  osito> fs cat hello.txt
  Goodbye! See you later.

  osito> fs mv hello.txt message.txt
  renamed 'hello.txt' -> 'message.txt'

  osito> fs ls
  Name                     Size  Sec
  message.txt              22  1

  osito> fs rm message.txt
  deleted
```

**Binary Upload Protocol:**

The `fs upload` command enables transfer of binary files from a host
computer to OsitoFS. This is essential for loading program images that
exceed the shell's 128-byte command buffer.

```
  PROTOCOL SEQUENCE
  -----------------
  1. Host sends:   fs upload <name> <size>\r\n
  2. Device sends: READY\n
  3. For each 4096-byte sector:
     a. Host sends up to 4096 bytes of raw data
     b. Device writes sector to flash
     c. Device sends '#' (ACK)
  4. Device sends: \nOK 0x<crc16>\n
```

The host MUST wait for the '#' acknowledgment before transmitting the
next sector. This prevents overflow of the 64-byte UART receive buffer
during flash erase/write operations (~30 ms per sector).

A Python upload utility is provided at `tools/upload.py`:

```
  py tools/upload.py COM4 game.bin game.bin
  py tools/upload.py COM4 data.bin data.bin --baud 74880
```

      NOTE: The filesystem uses ROM SPI functions (SPIRead, SPIWrite,
      SPIEraseSector) for all flash operations. All buffers passed to
      these functions must be 4-byte aligned. The filesystem handles
      alignment internally using a staging buffer when necessary.

      IMPORTANT: The `fs format` command will erase all files
      irrecoverably. The operator should exercise caution.


## XI. 3D GRAPHICS AND ELITE FLIGHT DEMO

Osito-K includes a complete wireframe 3D rendering pipeline, built
entirely with integer arithmetic on a processor with no floating-point
unit. This system powers the Elite flight demo.

**Fixed-Point Math (16.16):**

All 3D computation uses `fix16_t` — a 32-bit signed integer where the
upper 16 bits represent the integer part and the lower 16 bits the
fractional part. A 256-entry sine table provides trigonometric functions
with 1.4-degree resolution. Division and square root are computed
iteratively without hardware support.

**3D Pipeline:**

```
  Model vertices (fix16 xyz)
       |
       v
  mat3_rotate_x/y/z    ← 3x3 rotation matrix, angle_t (0-255)
       |
       v
  mat3_transform        ← apply rotation to each vertex
       |
       v
  project()             ← perspective projection to 2D (128x64)
       |
       v
  fb_line()             ← Bresenham line drawing to framebuffer
       |
       v
  fb_flush()            ← stream 1024 bytes to UART/display
```

**Ship Models:**

Geometry data from the original BBC Micro Elite (bbcelite.com),
scaled to fix16 coordinates:

```
  MODEL        VERTICES  EDGES   DESCRIPTION
  -----        --------  -----   -----------
  Cube              8      12    Test model (unit cube)
  Cobra Mk III     28      38    Player ship (iconic)
  Sidewinder       10      15    Common pirate/enemy
  Viper            15      20    Police patrol ship
  Coriolis         16      28    Space station (rotating)
```

**Elite Flight Demo:**

The `elite` command launches an interactive flight demo:
- Keyboard: `a`/`d` = yaw, `w`/`s` = pitch, `n` = next ship model
- Starfield: pseudo-random dots scrolling with velocity
- HUD: speed indicator, compass, ship name
- Renders at ~15-20 FPS on the 80 MHz processor
- Press Ctrl+C to exit


## XII. ZFORTH INTERACTIVE LANGUAGE

Osito-K includes **zForth**, a minimal Forth interpreter adapted for
bare-metal embedded use. zForth replaces the earlier Tiny BASIC
interpreter and bytecode VM, saving approximately 4 KB of IRAM.

**Characteristics:**

```
  Property                  Value
  --------                  -----
  Cell size                 32-bit signed integer (int32_t)
  Dictionary                2,048 bytes
  Data stack depth          16 cells
  Return stack depth        16 cells
  Number formats            Decimal, hexadecimal (0x prefix)
  Context persistence       Yes (definitions survive between sessions)
  Source                    github.com/zevv/zForth (MIT license)
```

**Built-in Words (core.zf bootstrap):**

The core bootstrap defines essential control flow and convenience words:

```
  CATEGORY     WORDS
  --------     -----
  Arithmetic   + - * / % 1+ 1- dup drop swap rot over pick
  Comparison   = != < > <= >= <0 =0 not
  Logic        & | ^ << >>
  Control      if else fi unless begin again until do loop
  Variables    variable constant allot !  @ +!
  I/O          emit . cr br tell s" ."
  Stack        >r r> i j
```

**Hardware Syscalls:**

zForth programs can access Osito-K hardware through numbered syscalls:

```
  WORD           STACK EFFECT       DESCRIPTION
  ----           ------------       -----------
  emit           ( c -- )           Print character to UART
  .              ( n -- )           Print number
  tell           ( addr len -- )    Print string from dictionary
  fb-clear       ( -- )             Clear 128x64 framebuffer
  fb-pixel       ( x y -- )         Set pixel
  fb-line        ( x0 y0 x1 y1 -- ) Draw line (Bresenham)
  fb-flush       ( -- )             Send framebuffer to display
  fb-text        ( col row a l -- ) Draw text string
  yield          ( -- )             Yield CPU to other tasks
  ticks          ( -- n )           Push system tick count
  delay          ( n -- )           Sleep for n ticks
  wire-render    ( m rx ry rz -- )  Render 3D wireframe model
  wire-models    ( -- n )           Push number of models
```

The `wire-render` syscall accepts a model index (0=cube, 1=cobra,
2=sidewinder, 3=viper, 4=coriolis) and three rotation angles
(0-255 = 0-360 degrees).

**Example: Spinning Cobra from Forth:**

```
  osito> forth
  zf: ready (Ctrl+C exit)
  : spin 20 0 do fb-clear dup i 8 * i 8 * 0 wire-render fb-flush 3 delay loop drop ;
   ok
  1 spin
   ok
```

**Running Scripts from OsitoFS:**

```
  osito> fs write demo.zf fb-clear 1 30 45 0 wire-render fb-flush
  wrote 46 bytes to 'demo.zf'
  osito> run demo.zf
   ok
```

      NOTE: The earlier Tiny BASIC interpreter and bytecode VM have been
      removed to conserve IRAM. Their source code is preserved in git
      history (commits 434697a through 11d5a48) and may be restored as
      an optional compile-time feature if desired.


## XIII. SYSTEM ARCHITECTURE

```
                     +============================+
                     |      HARDWARE LAYER        |
                     |  Xtensa LX106 @ 80 MHz     |
                     |  DRAM 80KB / IRAM 32KB      |
                     +============================+
                                  |
                     +------------+-------------+
                     |                          |
              +------+------+          +--------+--------+
              |   VECTORS   |          |    FRC1 TIMER   |
              |  vectors.S  |          |  timer_tick.c   |
              |  crt0.S     |          |  100 Hz tick    |
              +------+------+          +--------+--------+
                     |                          |
                     v                          v
              +------+---------------------------+------+
              |         CONTEXT SWITCH ENGINE           |
              |         context_switch.S                |
              |  Save a0-a15, PS, SAR, EPC1 (80 bytes) |
              |  Switch to ISR stack (512 bytes)        |
              |  Call os_exception_handler()            |
              |  Restore next task context + rfe        |
              +-----------------+-----------------------+
                                |
                     +----------+-----------+
                     |     SCHEDULER        |
                     |     sched.cpp        |
                     |  Priority-based +    |
                     |  round-robin select  |
                     |  task_create/yield   |
                     |  task_delay_ticks    |
                     +----------+-----------+
                                |
              +-----------+-----+------+-----------+
              |           |            |           |
        +-----+---+ +----+----+ +-----+----+ +----+-----+
        |  IDLE   | |  INPUT  | |  SHELL   | | (slots  |
        | task 0  | | task 1  | | task 2   | |  3 - 7) |
        | pri=0   | | pri=2   | | pri=3    | | avail.  |
        +---------+ +---------+ +-----+----+ +----------+
                         |             |
                    +----+----+  +-----+-----+
                    |   IPC   |  |   SHELL   |
                    | sem/mq  |  | COMMANDS  |
                    | swtimer |  |  25+ cmds |
                    +---------+  +-----+-----+
                                       |
              +----------+-------------+-------------+
              |          |             |             |
        +-----+--+ +----+-----+ +----+-----+ +-----+----+
        |  UART  | | POOL     | |  HEAP    | | OsitoFS  |
        | TX/RX  | | 32Bx256  | |  8KB     | | SPI flash|
        | mutex  | | free list| | 1st fit  | | 3.8 MB   |
        +--------+ +----------+ +----------+ +----------+
```

**GPIO Subsystem:**

```
  +-----------------------------------------------------------+
  |  GPIO DRIVER (gpio.cpp)                                   |
  |                                                           |
  |  GPIO 0-15: Standard peripheral (0x60000300)              |
  |    - IOMUX auto-configuration per pin                     |
  |    - Direction via GPIO_ENABLE register                   |
  |    - Read/write via GPIO_IN / GPIO_OUT registers          |
  |                                                           |
  |  GPIO 16: RTC domain (separate registers)                 |
  |    - RTC_GPIO_OUT, RTC_GPIO_ENABLE, RTC_GPIO_IN           |
  |                                                           |
  |  Wemos D1 Pin Mapping:                                    |
  |    D0=GPIO16  D1=GPIO5   D2=GPIO4   D3=GPIO0              |
  |    D4=GPIO2   D5=GPIO14  D6=GPIO12  D7=GPIO13             |
  |    D8=GPIO15  (GPIO 6-11 reserved for SPI flash)          |
  +-----------------------------------------------------------+
```


## XIV. MEMORY MAP

```
  INSTRUCTION RAM (32 KB)
  =======================
  0x40100000  +---------------------+  VECBASE
              | Exception Vectors   |  vectors.S
              |   +0x10 Debug       |
              |   +0x20 NMI         |
              |   +0x30 Kernel      |
              |   +0x50 User  <---  |  FRC1 / INUM7 / UART land here
              |   +0x70 Double      |
  0x4010007C  +---------------------+
              | ISR Entry/Exit      |  context_switch.S
              | Scheduler hot path  |  sched.cpp (critical sections)
              | Timer handler       |  timer_tick.c
              | All kernel code     |  ~10 KB total
  0x40107FFF  +---------------------+  End of IRAM


  DATA RAM (80 KB)
  ================
  0x3FFE8000  +---------------------+  DRAM_START
              | .data (initialized) |  Global variables
              | .rodata             |  String constants
              +---------------------+
              | .bss  (zeroed)      |  Uninitialized globals
              |   task_pool[8]      |  8 x TCB structs
              |   stack_pool[8]     |  8 x 1536 bytes = 12 KB
              |   pool_memory       |  256 x 32 bytes = 8 KB
              |   heap_memory       |  8 KB
              |   sec_buf[4096]     |  Filesystem sector buffer
              |   rx_buf[64]        |  UART receive ring buffer
              |   isr_stack[512]    |  Dedicated interrupt stack
              +---------------------+
              | <<< free space >>>  |  ~46 KB available
              |                     |
  0x3FFFBFF0  +---------------------+  Initial stack pointer
  0x3FFFBFFF  +---------------------+  DRAM_END


  SPI FLASH (4 MB)
  =================
  0x00000000  +---------------------+
              | Firmware image      |  ~13 KB (kernel + data)
  0x00003FFF  +---------------------+
              | (unused)            |
  0x00040000  +---------------------+  FS_FLASH_BASE
              | OsitoFS Superblock  |  4 KB (magic, version, stats)
  0x00041000  +---------------------+
              | File Table          |  4 KB (128 entries x 32 bytes)
  0x00042000  +---------------------+
              | Data Area           |  958 sectors = 3,832 KB
              |                     |  Contiguous file storage
  0x00400000  +---------------------+  FS_FLASH_END (4 MB boundary)


  FLASH-MAPPED CODE (irom0)
  =========================
  0x40200000  +---------------------+
              | .irom0.text         |  Bulk code (if cache enabled)
              | String literals     |  Mapped through cache
  0x405FFFFF  +---------------------+
```


## XV. SOURCE FILE DIRECTORY

```
  FILE                              LINES  DESCRIPTION
  ----                              -----  -----------

  Boot sequence
  ~~~~~~~~~~~~~
  src/boot/vectors.S                  90   Exception vector table at VECBASE
  src/boot/crt0.S                     70   CPU init: SP, VECBASE, BSS, jump
  src/boot/nosdk_init.c               81   WDT off, PLL 80MHz, IOMUX setup

  Kernel core
  ~~~~~~~~~~~
  src/kernel/context_switch.S        239   Full ISR save/restore, stack switch
  src/kernel/sched.cpp               267   Priority scheduler, task management
  src/kernel/timer_tick.c            148   FRC1 config, exception dispatcher
  src/kernel/task.h                  155   TCB struct, offsets, API protos

  Synchronization and IPC
  ~~~~~~~~~~~~~~~~~~~~~~~
  src/kernel/sem.cpp                 116   Counting semaphores and mutexes
  src/kernel/sem.h                    64   Semaphore API declarations
  src/kernel/mq.cpp                  101   Bounded message queues
  src/kernel/mq.h                     61   Message queue API declarations
  src/kernel/timer_sw.cpp            113   Software timers (one-shot/periodic)
  src/kernel/timer_sw.h               65   Software timer API declarations

  Memory management
  ~~~~~~~~~~~~~~~~~
  src/mem/pool_alloc.cpp             107   Fixed-block allocator, free list
  src/mem/pool_alloc.h                32   Pool API declarations
  src/mem/heap.cpp                   178   First-fit heap, auto-coalescing
  src/mem/heap.h                      46   Heap API declarations

  Filesystem
  ~~~~~~~~~~
  src/fs/ositofs.cpp                 724   Flat filesystem on SPI flash
  src/fs/ositofs.h                   101   Filesystem API declarations

  Drivers
  ~~~~~~~
  src/drivers/uart.cpp               175   UART0: TX, RX IRQ, ring buf, mutex
  src/drivers/uart.h                  46   UART API declarations
  src/drivers/gpio.cpp               128   GPIO 0-16, IOMUX auto-config
  src/drivers/gpio.h                  46   GPIO API declarations
  src/drivers/adc.cpp                 68   SAR ADC (10-bit, A0 pin)
  src/drivers/adc.h                   21   ADC API declarations
  src/drivers/input.cpp              121   Joystick input (ADC + button)
  src/drivers/input.h                 34   Input event API declarations
  src/drivers/font.cpp               185   4x6 bitmap font (ASCII 32-126)
  src/drivers/font.h                  19   Font API declarations
  src/drivers/video.cpp              270   Framebuffer 128x64, Bresenham lines

  Math library
  ~~~~~~~~~~~~
  src/math/fixedpoint.h              120   Fixed-point 16.16 types and inlines
  src/math/fixedpoint.cpp            162   sin/cos tables, div, sqrt, print
  src/math/matrix3.h                  72   3D vector/matrix types and inlines
  src/math/matrix3.cpp               165   Rotation, multiply, transform, project

  3D graphics
  ~~~~~~~~~~~
  src/gfx/wire3d.h                    53   Wireframe model struct, render API
  src/gfx/wire3d.cpp                 140   Render pipeline: rotate→project→draw
  src/gfx/ships.h                     40   Elite ship model declarations
  src/gfx/ships.cpp                  285   Ship vertex/edge data (4 models)
  src/game/game.h                     18   Game API declarations
  src/game/game.cpp                  220   Elite flight demo (HUD, starfield)

  zForth language
  ~~~~~~~~~~~~~~~
  src/forth/zforth.c                 887   Core interpreter (adapted, MIT)
  src/forth/zforth.h                 119   API header (ctx, eval, push/pop)
  src/forth/zfconf.h                  28   Config: int32 cells, 2KB dict
  src/forth/zf_host.cpp              400   Host callbacks, REPL, file runner
  src/forth/setjmp.h                  25   jmp_buf typedef for Xtensa CALL0
  src/forth/setjmp.S                  44   setjmp/longjmp (6 registers, 24B)

  User interface
  ~~~~~~~~~~~~~~
  src/shell/shell.cpp                830   Interactive console, 25+ commands
  src/shell/shell.h                   20   Shell entry point declaration
  src/main.cpp                        70   kernel_main: init and launch

  System headers
  ~~~~~~~~~~~~~~
  include/osito.h                     27   Master include
  include/kernel/config.h             59   System constants
  include/kernel/types.h              80   Freestanding type definitions
  include/hw/esp8266_regs.h          143   Peripheral register addresses
  include/hw/esp8266_iomux.h          70   Pin multiplexing definitions
  include/hw/esp8266_rom.h            68   ROM function prototypes

  Tools
  ~~~~~
  tools/upload.py                    171   Binary upload utility (Python)

  Build system
  ~~~~~~~~~~~~
  ld/osito.ld                         84   Linker script (IRAM/DRAM/irom0)
  ld/rom_functions.ld                 76   ROM function address bindings
  Makefile                           155   Build system
  tools/flash.sh                      45   Flash utility script
  tools/monitor.sh                    17   Serial monitor script
                                   -----
  TOTAL                           ~8,000   lines of source
```


## XVI. KNOWN LIMITATIONS

The operator should be aware of the following limitations in the
current release:

  1. **ROM String Functions.** The `ets_strlen` function resident in
     the processor's mask ROM has been observed to cause processor
     exceptions when invoked from preemptible task context. The system
     uses inline string length computation to avoid this condition.

  2. **Serial Baud Rate.** The ROM bootloader transmits at 74880 baud
     before kernel initialization sets the UART to 115200 baud. Brief
     garbled output during the boot phase is expected.

  3. **WiFi Subsystem.** The IEEE 802.11 radio transceiver is present
     on the hardware but is not initialized or controlled by this
     release. WiFi support is planned for Version 0.2.

  4. **Watchdog Timer.** The hardware watchdog is disabled during
     operation. A software watchdog facility is planned for a future
     release.

  5. **Task Termination.** Tasks are designed to run indefinitely.
     No mechanism for graceful task exit and resource reclamation is
     provided in this release.

  6. **Filesystem Limitations.** Files are allocated contiguously.
     `fs_append` can extend a file only within its pre-allocated sectors;
     it cannot reallocate. `fs_overwrite` will delete and recreate if the
     new data exceeds the original sector count. External fragmentation
     may prevent allocation of large files even when sufficient total
     free space exists.

  7. **SPI Flash Alignment.** All SPI flash operations require 4-byte
     aligned buffers. The filesystem handles this internally, but
     callers of the raw ROM functions (SPIRead, SPIWrite) must ensure
     proper alignment.

  8. **Stack Size.** Task stacks are limited to 1,536 bytes. Functions
     must avoid allocating large arrays on the stack. The filesystem
     uses a shared global sector buffer to stay within this constraint.


## XVII. WARRANTY AND DISCLAIMER

```
+============================================================================+
|                                                                            |
|  THIS SOFTWARE IS PROVIDED TO THE OPERATOR "AS IS" WITHOUT WARRANTY        |
|  OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE         |
|  WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,          |
|  OR NON-INFRINGEMENT OF INTELLECTUAL PROPERTY RIGHTS.                      |
|                                                                            |
|  THE AUTHORS SHALL NOT BE HELD LIABLE FOR ANY DAMAGES ARISING FROM          |
|  THE USE OF THIS SOFTWARE, INCLUDING BUT NOT LIMITED TO DAMAGE TO          |
|  THE COMPUTING HARDWARE, LOSS OF DATA, LOSS OF PRODUCTIVITY, OR           |
|  EXISTENTIAL DREAD EXPERIENCED WHILE DEBUGGING EXCEPTION VECTORS           |
|  AT THREE O'CLOCK IN THE MORNING.                                          |
|                                                                            |
|  THE 80 MHz CLOCK SPEED IS A NOMINAL VALUE. ACTUAL PERFORMANCE            |
|  MAY VARY DEPENDING ON AMBIENT TEMPERATURE, COSMIC RAY FLUX, AND          |
|  THE GENERAL DISPOSITION OF THE HARDWARE ON ANY GIVEN DAY.                 |
|                                                                            |
|  THE FILESYSTEM STORES DATA ON SPI FLASH MEMORY WHICH HAS A FINITE        |
|  NUMBER OF WRITE CYCLES. THE AUTHORS ACCEPT NO RESPONSIBILITY FOR          |
|  DATA LOSS DUE TO FLASH WEAR, POWER INTERRUPTION DURING WRITE             |
|  OPERATIONS, OR THE OPERATOR'S FAILURE TO MAINTAIN ADEQUATE BACKUPS        |
|  OF IRREPLACEABLE FILES CONTAINING ~8,000 LINES OF KERNEL CODE.           |
|                                                                            |
|  Osito-K IS A PROJECT OF NARANJOSITOS.TECH                                 |
|  https://naranjositos.tech/                                                |
|                                                                            |
+============================================================================+
```


---

```
  Osito-K v0.1
  Copyright (C) 2026 naranjositos.tech
  All rights reserved.

  https://naranjositos.tech/

  Written for the Xtensa LX106 processor.
  Assembled and tested on the Wemos D1 Mini computing module.

  ~8,000 lines of code. 80 KB of RAM. 3.8 MB of persistent storage.
  One Forth interpreter. Four spaceships. Zero floating-point units.
  No bears were harmed in the making of this kernel.
```
