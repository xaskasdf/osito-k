```
+============================================================================+
|                                                                            |
|                        O S I T O K   v 0 . 1                               |
|                                                                            |
|               PREEMPTIVE MULTITASKING OPERATING SYSTEM KERNEL              |
|                                                                            |
|                   FOR THE XTENSA LX106 MICROPROCESSOR                      |
|                                                                            |
|                        OPERATOR'S REFERENCE MANUAL                         |
|                                                                            |
|                          REVISION 1 -- FEB 2026                            |
|                                                                            |
+============================================================================+
```

                            *** IMPORTANT NOTICE ***

      This manual describes the installation, configuration, and operation
      of the OsitoK Preemptive Kernel, Version 0.1. The operator should
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
  IX.   SYSTEM ARCHITECTURE ....................................      9
  X.    MEMORY MAP .............................................     10
  XI.   SOURCE FILE DIRECTORY ..................................     11
  XII.  KNOWN LIMITATIONS ......................................     12
  XIII. WARRANTY AND DISCLAIMER ................................     13
```

---

## I. SYSTEM OVERVIEW

OsitoK (from Spanish *osito*, "little bear") is a **bare-metal preemptive
multitasking kernel** engineered for the Xtensa LX106 processor. The system
operates without the assistance of any vendor SDK, communicating directly
with the hardware registers of the processing unit.

The kernel provides the following capabilities:

  - **Preemptive round-robin task scheduling** at a rate of 100 operations
    per second, ensuring fair distribution of the processor's considerable
    computational resources among all active programs.
  - **Hardware interrupt-driven context switching** with full preservation
    of all 16 general-purpose registers, the Processor Status word, the
    Shift Amount Register, and the Exception Program Counter.
  - **Fixed-block memory allocation** providing 8,192 bytes of dynamically
    assignable storage organized in 256 blocks of 32 bytes each.
  - **Interrupt-driven serial communications** with a 64-byte receive
    buffer and cooperative mutual exclusion for output operations.
  - **An interactive operator console** with built-in diagnostic commands
    for system monitoring and administration.


## II. HARDWARE SPECIFICATIONS

The OsitoK kernel is designed to exploit the full capabilities of the
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
  build/osito.elf         Executable and Linkable Format image
  build/osito.map         Memory allocation map
```

Upon successful assembly, the system will display a summary of memory
utilization:

```
  === OsitoK build complete ===
     text    data     bss     dec     hex filename
     3546     968   21824   26338    66e2 build/osito.elf
```

      NOTE: The .bss segment includes all task stacks and the memory
      pool, which accounts for the majority of RAM allocation.

To convert the ELF image into a format suitable for the flash memory,
execute:

```
  py -m esptool --chip esp8266 elf2image \
      --flash_mode dout --flash_size 4MB \
      --flash_freq 40m --version 1 \
      -o build/osito build/osito.elf
```

This produces the binary image file `build/osito0x00000.bin`.

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
    6      sched_init                         Scheduler: idle task created
    7      task_create (x2)                   Heartbeat and Shell tasks registered
    8      timer_init                         FRC1 armed: 100 Hz, prescaler /16
    9      sched_start                        Context loaded, rfe — system live
```

The operator console will display the following banner:

```
  =============================
    OsitoK v0.1
    Bare-metal kernel for ESP8266
  =============================
  timer: FRC1 configured at 100 Hz (load=50000)
  pool: initialized 256 blocks x 32 bytes = 8192 bytes
  sched: initialized, idle task created
  sched: created task 'heartbeat' (id=1)
  sched: created task 'shell' (id=2)
  sched: starting scheduler

  Starting kernel...

  osito>
```

The `osito>` prompt indicates the system is operational and awaiting
operator input. The `[heartbeat N]` messages will appear at
approximately 2-second intervals, confirming that the preemptive
scheduler is functioning correctly.

      NOTE: The serial console operates at 74880 baud during the ROM
      bootloader phase, then 115200 baud during kernel operation.
      Some terminal programs may display garbled output during the
      initial boot phase. This is normal and expected.


## VIII. OPERATOR CONSOLE COMMANDS

The OsitoK interactive shell accepts the following commands at the
`osito>` prompt. Commands are terminated by pressing the RETURN key.
The BACKSPACE key may be used to correct input errors.

```
+--------+----------------------------------------------------------+
|COMMAND | DESCRIPTION                                              |
+--------+----------------------------------------------------------+
|        |                                                          |
| ps     | Display a listing of all active tasks in the system.     |
|        | For each task, the following information is shown:       |
|        |   ID     - Unique task identification number (0-7)       |
|        |   State  - Current execution state (see table below)     |
|        |   Ticks  - Number of timer ticks consumed by the task    |
|        |   Name   - Human-readable task designation               |
|        |                                                          |
| mem    | Display memory pool utilization statistics, including:   |
|        |   Block size, total blocks, free blocks, used blocks.    |
|        |                                                          |
| ticks  | Display the current system tick counter and the          |
|        | equivalent elapsed time in seconds.                      |
|        |                                                          |
| help   | Display a summary of available commands.                 |
|        |                                                          |
| reboot | Perform an immediate software reset of the processor.    |
|        | All task state will be lost. The system will reinitialize |
|        | from the boot sequence.                                  |
|        |                                                          |
+--------+----------------------------------------------------------+
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
  ID  State  Ticks  Name
  0   ready  1  idle
  1   ready  293  heartbeat
  2   run    222  shell

  osito> mem
  Memory pool:
    Block size:  32 bytes
    Total:       256 blocks (8192 bytes)
    Free:        256 blocks
    Used:        0 blocks

  osito> ticks
  Tick count: 808 (8 seconds)
```


## IX. SYSTEM ARCHITECTURE

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
                     |  Round-robin select  |
                     |  task_create()       |
                     |  task_yield()  [sw]  |
                     |  task_delay_ticks()  |
                     +----------+-----------+
                                |
              +-----------+-----+------+-----------+
              |           |            |           |
        +-----+---+ +----+----+ +-----+----+ +----+-----+
        |  IDLE   | |HEARTBEAT| |  SHELL   | | (slots  |
        | task 0  | | task 1  | | task 2   | |  3 - 7) |
        | 512B stk| | 1536B   | | 1536B    | | avail.  |
        +---------+ +---------+ +-----+----+ +----------+
                                       |
                                 +-----+-----+
                                 |   UART    |
                                 | uart.cpp  |
                                 | TX polled |
                                 | RX irq    |
                                 | mutex     |
                                 +-----------+
                                       |
                                 +-----+-----+
                                 | POOL ALLOC|
                                 | 32B x 256 |
                                 | free list |
                                 +-----------+
```


## X. MEMORY MAP

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
              |   rx_buf[64]        |  UART receive ring buffer
              |   isr_stack[512]    |  Dedicated interrupt stack
              +---------------------+
              | <<< free space >>>  |  ~56 KB available
              |                     |
  0x3FFFBFF0  +---------------------+  Initial stack pointer
  0x3FFFBFFF  +---------------------+  DRAM_END


  FLASH (4 MB, memory-mapped)
  ===========================
  0x40200000  +---------------------+
              | .irom0.text         |  Bulk code (shell, drivers)
              | String literals     |  Mapped through cache
  0x405FFFFF  +---------------------+
```


## XI. SOURCE FILE DIRECTORY

```
  FILE                              LINES  DESCRIPTION
  ----                              -----  -----------
  src/boot/vectors.S                  90   Exception vector table at VECBASE
  src/boot/crt0.S                     70   CPU init: SP, VECBASE, BSS, jump
  src/boot/nosdk_init.c               81   WDT off, PLL 80MHz, IOMUX setup
  src/kernel/context_switch.S        239   Full ISR save/restore, stack switch
  src/kernel/sched.cpp               253   Round-robin scheduler, task mgmt
  src/kernel/timer_tick.c            131   FRC1 config, exception dispatcher
  src/kernel/task.h                  154   TCB struct, offsets, API protos
  src/mem/pool_alloc.cpp             107   Fixed-block allocator, free list
  src/mem/pool_alloc.h                32   Pool API declarations
  src/drivers/uart.cpp               188   UART0: TX, RX IRQ, ring buf, mutex
  src/drivers/uart.h                  46   UART API declarations
  src/shell/shell.cpp                185   Interactive console, 5 commands
  src/shell/shell.h                   20   Shell entry point declaration
  src/main.cpp                        76   kernel_main: init and launch
  include/osito.h                     27   Master include
  include/kernel/config.h             49   System constants
  include/kernel/types.h              80   Freestanding type definitions
  include/hw/esp8266_regs.h          143   Peripheral register addresses
  include/hw/esp8266_iomux.h          70   Pin multiplexing definitions
  include/hw/esp8266_rom.h            68   ROM function prototypes
  ld/osito.ld                         84   Linker script (IRAM/DRAM/irom0)
  ld/rom_functions.ld                 76   ROM function address bindings
  Makefile                           143   Build system
  tools/flash.sh                      45   Flash utility script
  tools/monitor.sh                    17   Serial monitor script
                                   -----
  TOTAL                            2,563   lines of source
```


## XII. KNOWN LIMITATIONS

The operator should be aware of the following limitations in the
current release:

  1. **ROM String Functions.** The `ets_strlen` function resident in
     the processor's mask ROM has been observed to cause processor
     exceptions when invoked from preemptible task context. The system
     uses inline string length computation to avoid this condition.

  2. **Build System.** The Makefile invokes `python` for the esptool
     image generation step. On systems where the Python interpreter
     is registered as `py`, the operator must execute the image
     conversion command manually (see Section V).

  3. **Serial Baud Rate.** The ROM bootloader transmits at 74880 baud
     before kernel initialization sets the UART to 115200 baud. Brief
     garbled output during the boot phase is expected.

  4. **WiFi Subsystem.** The IEEE 802.11 radio transceiver is present
     on the hardware but is not initialized or controlled by this
     release. WiFi support is planned for Version 0.2.

  5. **Watchdog Timer.** The hardware watchdog is disabled during
     operation. A software watchdog facility is planned for a future
     release.

  6. **Task Termination.** Tasks are designed to run indefinitely.
     No mechanism for graceful task exit and resource reclamation is
     provided in this release.


## XIII. WARRANTY AND DISCLAIMER

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
+============================================================================+
```


---

```
  OsitoK v0.1
  Copyright (C) 2026
  All rights reserved.

  Written for the Xtensa LX106 processor.
  Assembled and tested on the Wemos D1 Mini computing module.

  This document was prepared using only 80 KB of RAM.
  No bears were harmed in the making of this kernel.
```
