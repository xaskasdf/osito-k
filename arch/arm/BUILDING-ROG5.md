# Building & Booting Osito-K on ASUS ROG Phone 5

Target: ASUS ROG Phone 5 (I005DA), Qualcomm SM8350 (Snapdragon 888)
Last verified: 2026-10-28 on physical device. Boots through the shim to the
`shell>` prompt, shown on the framebuffer console with the heartbeat blinking. The
shell is interactive over USB: the side USB-C port enumerates on the PC as a USB
serial port (see "Console over USB").

---

## Quick Start

From the Windows dev box (Git Bash or MSYS2), with Osito-K running on the phone
(or the phone in fastboot mode, Power + Vol-Down):

```bash
arch/arm/scripts/rog5-boot.sh --usb          # build, send to the running kernel over USB, boot it (~15 s)
arch/arm/scripts/rog5-boot.sh --flash        # build + pack + flash boot_<active slot> + reboot + verify (~75 s)
arch/arm/scripts/rog5-boot.sh                # build + pack only -> arch/arm/build/rog5/osito-boot-rog5.img
arch/arm/scripts/rog5-boot.sh --virt-check   # also build PLATFORM=virt and boot it in local QEMU
```

The script ships the working tree (`arch/arm`, `gui`, `include/common`) to the Mac,
builds there, packs the boot image with the pieces in `~/phone_breaker2`, brings it
back and checks its SHA-256. Then it either:
- `--usb`: hands the raw `Image` to `rog5-kload.ps1`, which runs `kload` on the live
  kernel's USB console. This is the normal development loop: no buttons, and
  `boot_a` is untouched, so a reboot returns to the flashed kernel.
- `--flash`: flashes the image, for a build that must survive a reboot. If Osito-K
  is running, the script first sends `reboot bootloader` (see "Rebooting into
  fastboot"), so no buttons are needed. A kernel older than that needs
  Power + Vol-Down. After `fastboot reboot` it waits for the console and checks
  that `uname` reports the build stamp found in the flashed `boot.img`.

`rog5-console.ps1 -Command <line>` runs one shell command on the console and prints
the answer (`-WaitPort <s>` waits for the phone to enumerate first).

Where things live:

| What | Where |
|------|-------|
| Toolchain | Mac `cortessamuel@192.168.1.7`: Homebrew `aarch64-elf-gcc` 15.2 in `/opt/homebrew/bin` |
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
test IDs), serial `0001`. After `net up` it is a composite with a network function
instead (serial `0002`, the console then on COM4; see "USB networking"). The
scripts find the port either way by matching `VID_1209&PID_0001`. Windows 10+ binds
its inbox `usbser.sys` and creates a COM port (COM3 on the dev box); find it with
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
  they need no cache maintenance. They do need barriers: see below.
- **Barriers.** The DMA window is Normal non-cacheable memory, and the
  controller's registers are Device memory. Arm orders neither against the other.
  So `rd()` / `wr()` in `dwc3_acm.c` work as Linux's `readl` / `writel`:
  - `dmb oshld` after a register read, so the event buffer and SETUP packet are
    read only after `GEVNTCOUNT` says they are there;
  - `dmb oshst` before a register write, so a TRB is complete before `DEPCMD`
    starts it;
  - `dmb osh` before handing event slots back.

  Without them, servicing the gadget from an A55 at 403 MHz wedged EP0 on the
  second port open/close. The CPUs kept running, and the console stayed dead even
  at full speed. At 1.8 GHz, or from a big core, it never showed. Any new driver
  that DMAs needs the same.
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
the running kernel's image, BSS and stack. The old kernel's memory is simply
abandoned and goes back into the page allocator (see "High memory"). A 500 KB
transfer takes ~0.3 s with caches on (~2.5 s without).

---

## USB networking

`net up` re-enumerates the gadget as a composite, `usb_set_ncm` in
`dwc3_acm.c`. Device class EF/02/01 carries two IADs:

| Function | Interfaces | Endpoints |
|----------|------------|-----------|
| CDC-ACM (the console, as before) | 0, 1 | EP1 bulk, EP2 interrupt |
| CDC-NCM | 2 (control), 3 (data, alt 1) | EP4 interrupt IN (phys 9); EP3 bulk OUT/IN (phys 6/7) |

It uses serial `0002`, a device instance of its own. Windows 11 binds `usbccgp`,
then `usbser` (COM4) and its inbox `UsbNcm` (an adapter named "naranjositos
Osito-K", "Ethernet 4" here) with no driver to install. `net down` goes back
to the serial port alone, on COM3. If the host hasn't configured the
composite within 20 s, the driver goes back by itself, because the console is
the only way in.

**USB side (`dwc3_acm.c`).**
- **Class requests to interface 2** (CDC NCM 1.0 §6.2) are answered:
  `GET_NTB_PARAMETERS` (NTB16; IN up to 16 KB, OUT up to 16 KB, 4-byte
  alignment), the NTB format, input size and CRC mode getters, and the packet
  filter and format setters, acknowledged.
- **`SET_INTERFACE`** on interface 3, alt 1, arms a 16 KB bulk OUT transfer.
  It then sends a `CONNECTION_SPEED_CHANGE` notification (120 Mbit/s) and
  `NETWORK_CONNECTION`, as Linux's `f_ncm` does.
- **Memory.** The DMA area grew to 40 KB.
- **MAC addresses.** The `iMACAddress` string, `024F4B000001`, is the host
  adapter's MAC; the phone's own is `02:4F:4B:00:00:02`.
- Windows also asks for string `0xEE` (the Microsoft OS descriptor) and is
  told to stall; that's harmless.

**Framing (`ncm.c`).**
- **Receiving.** An OUT NTB is unpacked datagram by datagram (NTH16, then
  the NDP16 chain) into a 32-frame RX queue.
- **Sending.** Up to 8 queued TX frames go out per IN NTB. The block length
  is never a multiple of 512, so the transfer ends with a short packet.
- **The stack's NIC.** `net.c` now takes its NIC through `net_set_nic`, with
  virtio-net as the default for QEMU virt. The NCM NIC polls the gadget
  itself, on receive when its queue is empty and on send after queueing, so
  any user polling the stack also moves the USB side.

**Addressing.** When the host selects the data interface, the `net` task asks
for an address by DHCP (`kernel/dhcp.c`, broadcast replies; `net.c` now takes
broadcasts and anything while it has no address). Windows' Internet
Connection Sharing answers, and routes the phone to the internet. With no
answer within 3 tries of 2 s, the phone takes a link-local 169.254.79.x, as
Windows does after its own DHCP gives up, and the two reach each other.

**Ownership.** The stack isn't reentrant. Commands hold it for their whole
run (`net_hold`: `ping`, `resolve`, DHCP). The `net` task polls it every 10 ms
only when nobody holds it (`net_trypoll`), so the phone answers ARP and ping
in the background. `spin_trylock` is new in `include/spinlock.h`.

**Measured (2026-10-30).**
- Windows enumerated the composite on the first try: COM4 plus an adapter in
  state Up, with MAC `02-4F-4B-00-00-01`.
- PC to phone: `ping -S 169.254.122.0 169.254.79.70` gets 4/4 replies,
  1–7 ms.
- Phone to PC: the new `ping` (4 echoes, 1 s each) gets 0–1 ms.
- Without `-S`, Windows sent the ping through Tailscale, whose adapter also
  has a 169.254 address and an equal-metric route.
- Windows put the network in the Public category.

### Internet and HTTPS

With Internet Connection Sharing on (the PC's internet adapter shared with
"naranjositos Osito-K"), Windows answers DHCP with 192.168.137.x, gateway and
DNS 192.168.137.1. DHCP logs the DNS server it got, `net` shows it, and
`net dns <ip>` replaces it. Then:
- `ping 1.1.1.1` gets 6–8 ms, and `resolve` works.
- `https <host> [path]` runs a GET through `http.c` and `tls.c` (x86's, ported).
  It prints the status and the first 600 bytes of the body.
- The TLS client is the old 1.2 one: ECDHE-RSA-AES128-GCM, with the
  **certificate not verified**. Don't send secrets (an API key) through it
  until x86's TLS 1.3 / X.509 stack is ported.
- Measured (2026-10-30): example.com and `www.google.com/robots.txt` answered
  200, three times in a row.

Three bugs in the ported stack stood in the way:
- **Timeouts.** `net.c` and `tls.c` count x86's 100 Hz ticks, but they read
  `timer_get_ticks()`, which on AArch64 is the 19.2 MHz counter. A "5 s" TCP
  connect lasted 26 µs. They now read `timer_get_tick_count()`, the
  scheduler's 100 Hz count.
- **Stacks.** Task stacks were 8 KB, but `tls.c` put 16 KB record buffers on
  the stack. The first `https` overflowed into the neighbouring tasks' stacks,
  and the phone froze with no console. The big TLS and HTTP buffers are now
  static: there is one user at a time, under `net_hold`. `TASK_STACK_SIZE` is
  now 32 KB (`kernel/task.h`), 1 MB for the 32 tasks.
- **DNS.**
  - A query sent before the gateway's MAC was known was only an ARP request
    and never went out. Each of 3 tries now resends for up to a second,
    waiting up to 200 ms for the ARP answer each time.
  - Every `resolve` added another UDP listener, until the table was full. A
    second listen on a port now replaces the handler.

On QEMU virt, virtio-net had never received anything: `net_poll` passed
`len = 0`, which `virtio_net_recv` reads as the buffer's size. The `net_nic_t`
contract now says `*len` is the buffer's size going in. With that,
`resolve` and `https` work under SLIRP too.

## MMU and caches

The shim enters with the MMU and D-cache off, so every access is
Device-nGnRnE: uncached and unmerged. `platform/sm8350/mmu.c`, called from
`kernel_main` after `mem_init`, builds a flat map (VA == PA) and turns the MMU
and both caches on:

| Range | Attributes | Use |
|-------|-----------|-----|
| `0x00000000`–`0x3FFFFFFF` | Device-nGnRE (1 GB block) | MMIO: GCC, UART, USB, SMMU, GIC… |
| `0x80800000`–`0x809FFFFF` | Device-nGnRE | the AOP's command DB (`0x80860000`, read by `rpmh.c`) |
| `0xA0000000`–`0xB4DFFFFF` | Normal write-back | kernel, heap, page allocator, kload buffers |
| `0xB4E00000`–`0xB4FFFFFF` | Normal non-cacheable | DMA window (`dma_alloc`): USB event buffer and TRBs |
| `0xE5000000`–`0xE5BFFFFF` | Normal non-cacheable | splash framebuffer; writes merge, display sees them |
| `0x800000000`–`0x9FFFFFFFF`, `0xC0000000`–`0x27FFFFFFF` less carve-outs | Normal write-back, XN | high memory, 15 GB (models, `put` buffers, inference state) |

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
- **High memory (2026-10-30).** The device tree's memory node has three
  banks:

  | Bank | Range | Size | Use |
  |------|-------|------|-----|
  | 0 | `0x80000000`–`0xB5000000` | 848 MB | carve-outs below `0xA0000000` |
  | 1 | `0x800000000`–`0xA00000000` | 8 GB | no carve-outs |
  | 2 | `0xC0000000`–`0x280000000` | 7 GB | three carve-outs (below) |

  Bank 2's static reserved-memory regions: `removed_region`
  `0xD8800000`–`0xE3000000` (168 MB), `splash_region` `0xE5000000`–`0xE7300000`
  (35 MB, the framebuffer), `dfps_data_region` `0xE7400000`–`0xE7500000`.
  - **Mapping.** `mmu.c`'s `high_ram[]` is banks 1 and 2 less those regions:
    8 GB, then 392 + 32 + 394 MB in the framebuffer's gigabyte, then 6 GB above
    4 GB. They're mapped Normal WB and XN, in 2 MB blocks inside that gigabyte
    and 1 GB blocks elsewhere.
  - **Probe and handover.** A word at each range's start and at each gigabyte
    boundary is written and read back. Each range then goes to `memory.c` as
    a high region (`mem_add_high`, one bitmap for all). `mem` shows 15362 MB.
  - **Who uses it.** Only `mem_alloc_aligned_high` allocates there: models,
    `put`'s buffer, and the inference state (KV cache, scratch).
    `mem_alloc_pages` never does, so `kload`'s kernels stay in bank 0, where
    `mem_init` expects to find them.
  - **`kload` and bank 0.** A `kload`ed kernel lands wherever its predecessor
    had free pages. `mem_init` reserves just the running image (`_start` to
    `_stack_top`); what lies below is the predecessor's abandoned memory.
    Until 2026-10-30 it reserved from the bank's base up, so each `kload` in a
    row shrank bank 0: one chain was down to 7 MB free, now 205 MB.
  - **The handoff doesn't clean high memory.** Nothing writes it uncached and
    nothing DMAs into it, so a line left dirty holds what the next kernel would
    read anyway.
- **Effect:** `help` round trip over USB went from 315 ms to 31 ms, and kload
  uploads from 2.5 s to 0.3 s.

---

## SMP scheduling

`smp_boot_aps` starts the other seven cores with PSCI `CPU_ON` (see "PSCI"); they
park until `sched_start`, then each one sets up its GIC redistributor (found by
matching `GICR_TYPER` affinity to its MPIDR) and its virtual timer, and joins the
scheduler in its own idle task.

- **One task pool, one lock.** `kernel/sched.c` keeps `task_pool` under `sched_lock`.
  Each CPU reschedules on its own tick or yield SGI and takes the best READY task
  it may run. A RUNNING task belongs to the CPU running it, so two CPUs never pick
  the same one.
- **Placement.** `task_create` still puts tasks on the boot CPU, so code written
  for one core keeps running serialized there; the shell is one of them. Use
  `task_create_on(..., cpu)` to put a task on another core, or `TASK_ANY_CPU` to
  let any idle CPU take it (it may move between CPUs).
- **Placement by load.** Busy unpinned tasks go to the fastest idle CPU, light
  ones to the slowest. Each CPU has a capacity, set by `cpufreq` as
  `capacity-dmips-mhz` (device tree: A55 1024, A78 1946, X1 2048) × its MHz, the way
  Linux computes it. Each task has a utilization (`util`, `ps` shows it) sampled per
  tick. The task a CPU is running moves toward 100% on that CPU's ticks, and
  sleeping tasks move toward 0% on the boot CPU's, with an 80 ms time constant.
  READY tasks keep theirs. New tasks start at 100%, on a big core.
  - Under ~19% a task counts as light, over ~31% as busy; in between it stays put.
  - A CPU leaves busy tasks to idle CPUs faster than itself, and light ones to
    slower idle CPUs, unless there are more such tasks than CPUs. So tasks move up
    or down at the next tick once a CPU of the right kind is free.
  - This is sampling, not Linux's PELT: no frequency invariance and no energy
    model.
- **Per-CPU state.** `TPIDR_EL1` points at the CPU's `struct cpu_sched` (`task.h`):
  current task, IRQ stack, idle task. `irq_vector_entry` saves the context on the
  task's stack, then runs the handler on the CPU's own IRQ stack. Once
  `schedule()` makes the task READY, another CPU may resume it on its own stack.
- **Locks.** The scheduler, heap, page allocator and console take spinlocks
  (`include/spinlock.h`: `ldaxr`/`stxr` + `wfe`; GCC's `__atomic` would emit LSE
  atomics, which virt's Cortex-A72 doesn't have). They are held with IRQs off and
  skipped until secondary CPUs run tasks (`smp_running`). The console lock nests on
  one CPU, so a CPU that faults while printing can still report it. It also covers
  `uart_trygetc` and kload's reads, since USB polling sends queued output too.
- **SGIs.** SGI 0 = yield, SGI 1 = stop: `kload` calls `smp_stop_aps`, which turns
  the scheduling CPUs off (they take the SGI as soon as IRQs are on, so never while
  holding a lock) before jumping into memory they might still be running from.
- **Wakeups.** When a task becomes READY (created, woken, or preempted without
  being picked again) and a CPU that may run it sits in its idle task, the
  scheduler sends that CPU SGI 0, so it reschedules at once instead of on its next
  tick. Starting a task on another CPU went from 5.7 ms on average to 4 µs.
- **Ticks.** Every CPU ticks at 100 Hz, but only the boot CPU's ticks count as time.
- **Try it:** `cpus` shows what each CPU runs. `cpubench` starts one timed task
  pinned to each CPU, `cpubench any [n]` starts n unpinned tasks (default two per
  CPU), and `cpubench mix` shows where busy and light unpinned tasks settle:

```
task  cpu   core          time    ~MHz          (cpubench, 2026-10-29)
   0   0    Cortex-A55  177 ms  1802
   4   4    Cortex-A78  132 ms  2418
   7   7    Cortex-X1   112 ms  2840
wall 192 ms, sum of the tasks 1219 ms, 8 tasks, 0 changed CPU
```

**GIC MMIO goes through asm.** The hypervisor traps GIC accesses and emulates
them from the abort syndrome, which only single loads and stores without
writeback have. A post-indexed `str` that gcc chose for a loop came back as a
synchronous external abort. So the GIC is accessed only through `mmio_rd32` /
`mmio_wr32` / `mmio_rd64` from `include/aarch64.h`, one-line `ldr`/`str` asm (as
Linux's `readl`/`writel` do). New drivers for regions the hypervisor may trap
should use them too.

---

## CPU frequency

Each cluster is an EPSS frequency domain (`qcom,cpufreq-hw-epss` in the phone's
device tree): domain 0 at `0x18591000` = cpu0-3 (A55), 1 at `0x18592000` = cpu4-6
(A78), 2 at `0x18593000` = cpu7 (X1). The hardware holds the table of levels and
sequences the voltage itself; `platform/sm8350/cpufreq.c` reads the table
(`0x100` frequencies, `0x200` voltages) and writes the index of the level it
wants to `0x320`, as Linux's `qcom-cpufreq-hw` does. `0x020` reports what the
domain actually runs at: bits 7:0 the L value (× 19.2 MHz), 12:8 the level,
31:20 the voltage in mV. That can differ from the level it is set to: the limits
hardware (LMh) throttles hot silicon, and the big clusters share a rail (below).

The A55s' level 0 runs off the alternate clock (LUT source 0, GPLL0 / 2 =
300 MHz) instead of a PLL. Its L value reads 15 (288 MHz), so on that row the
frequency comes from the table instead. `cpubench` measures 298–299 MHz there.
Levels 0–4 (300–691 MHz) share 568 mV, so below 691 MHz only the clock drops.

The bootloader leaves the A78s at 710 MHz and the X1 at 844 MHz, their lowest
levels. Boot moves every domain to its top level: A55 1804 MHz, A78 2419 MHz,
X1 2841 MHz. A domain with all its cores off reports the new level only once
they run, so this happens after `smp_boot_aps`. The setting survives a `kload`.

**The A78s and the X1 share a voltage rail.** The rail follows whichever domain
needs the higher voltage, and the EPSS runs the other one at its fastest level
that voltage allows, whatever it was set to. Measured with the A78s set to
710 MHz:

| X1 set to | rail | A78s run at |
|---|---|---|
| 1900 MHz | 720 mV | 1440 MHz (708 mV) |
| 2496 MHz | 884 mV | 1996 MHz (856 mV) |
| 2841 MHz | 1008 mV | 2419 MHz (1000 mV) |

It works both ways: A78s at 1670 MHz (764 mV) keep the X1 at 2035 MHz (740 mV).
So lowering one big cluster alone does nothing; this looked like a drop that
"took seconds" until the X1 came down too.

**Lowering the A55s once "hung the machine".** Twice, the USB console died a
few seconds after the A55s went down (to the alternate clock, then to 403 MHz),
with no panic band. `cpufreq try` (below) showed what was actually happening:
- With the shell moved to a big core, all four A55s ran 10 s at 403 MHz, idle
  or busy, without missing a tick.
- With the shell, and so the USB polling, left on cpu0, the console wedged on the
  second port open/close. Meanwhile, every A55 kept ticking.

The bug was in the USB gadget, which lacked barriers (see "Console over USB").
With them, the A55s at 403 MHz took 48 shell commands in a row, each with its own
port open/close, including `cpubench` (402 MHz) and `dmesg`. The L3 cache, which
has its own EPSS block (`0x18590000`, `cpufreq l3`), stayed at 1248 MHz / 712 mV
in every `cpufreq try` run, whatever the A55s did. Level 0, on the alternate
clock, was the same story: 20 s with the console on cpu0, 38 commands in a row.

- `cpufreq` lists each domain's frequency and voltage now, and its levels as
  MHz@mV.
- `cpufreq max` / `cpufreq min` set every domain to its top / lowest level
  (300 / 710 / 844 MHz).
- `cpufreq <domain> <level>` sets one and reports every domain on its rail, e.g.
  `cpu4-6: 1440 MHz, set to 710 MHz (the other cluster holds their shared rail up)`.
- `cpufreq try <domain> <level> <seconds> [stay | busy]` runs a level that might take
  its CPUs down, under a watcher.
  - A task pinned outside the domain samples every 50 ms, with its IRQs off: the
    domain's state, the L3's, and each of the domain's CPUs' timer ticks.
  - If a CPU goes 1 s without a tick, the watcher puts the level back at once.
  - It prints a table: per 0.5 s, the MHz / mV / level and the ticks (50 = none
    missed).
  - The shell (and the USB console) moves out of the domain for the run, unless
    `stay` is given.
  - `busy` keeps the domain's CPUs busy meanwhile.
  - In screen debug mode, a bar under the heartbeat squares, drawn without the
    console lock, gives the verdict even if the console dies: yellow while it runs,
    green if every CPU kept ticking, red if one stopped.
- `cpufreq l3` dumps the L3 block read-only: its level table (`0x100`, 300–1593
  MHz) and the words around the state (`0x020`, same layout as a CPU domain's)
  and vote registers.

Each level change also updates the scheduler's capacity for those CPUs (see
"Placement by load"). `cpus` shows it.

### Temperature

`platform/sm8350/thermal.c` reads the two TSENS v2 controllers
(`qcom,tsens24xx`): tsens0 (SROT `0x0C222000`, TM `0x0C263000`) has the CPU
sensors, tsens1 (SROT `0x0C223000`, TM `0x0C265000`) the GPU, NPU, video, DDR,
modem and camera ones. Each sensor's status register (TM + `0xA0` + 4n) holds the
last reading in 0.1 °C, 12-bit signed, with a valid bit (21). The names come from
the device tree's thermal zones. cpu-0-0..3 are the A55s, cpu-1-0..7 the four big
cores, two sensors each in order: `burn 7` heats cpu-1-6/7 and `burn 4` heats
cpu-1-0/1.

A "thermal" task checks every 100 ms. Above 95 °C on any big-core sensor, it lowers
the caps of **both** big clusters one level (the shared rail makes one alone
useless); below 85 °C it raises them one level. A capped domain runs at the lower
of the level asked for and its cap. Android's die trips for these zones are
108–125 °C; its everyday control works from skin temperature, which Osito-K
doesn't read, so these trips are lower. LMh still throttles on its own.

- `temp` shows every sensor, then per cluster the hottest one, its peak, and the
  frequency it runs at (and its cap).
- `temp limit <cap> <release>` moves the trips, in °C, until the next boot.
- `burn <cpu|all> <seconds>` keeps CPUs busy in the background, to make heat.

At rest everything reads 33–38 °C, and one core busy for 25 s reaches ~42 °C. With
`temp limit 45 41` and `burn all 45`, the cap went from 2419 / 2841 MHz down to
1996 / 2496 MHz within 22 s and held the big cores at 43–44 °C. Within ~2 s of the
burn ending, it released them to the top levels.

---

## Storage (UFS)

The phone's storage is a **Samsung KLUEG8UHDC-B0E1** (256 GB, UFS 3.1) on the
`ufshc@1d84000` controller (UFSHCI 3.0, 32 request slots, 64-bit DMA). It holds
every partition the phone has, ABL and both boot slots included. So
`platform/sm8350/ufs.c` **writes only to LUN 0's `userdata`**, which belongs to
Osito-K since 2026-10-29 (Android's data there, encrypted, was given up):
- `permitted()` decides every command. Reads (TEST UNIT READY, INQUIRY, READ
  CAPACITY(10), READ(10)) may go anywhere.
- WRITE(10) passes only if its whole LBA range lies inside `userdata`, found by
  name in LUN 0's GPT (LBA 2 352 680–61 865 978). SYNCHRONIZE CACHE goes only
  to LUN 0. Any other opcode is refused.
- Until that lookup has run, nothing writes. Inline crypto (ICE) stays off.

ABL boots from UFS and leaves the controller up: the power domain
(`gcc_ufs_phy_gdsc`, GCC `0x77004`) and all ten UFS clocks are on, the link is
active (`HCS` = `0x10F`: device present, both lists and UIC ready), and its
transfer request list (at `0x9EA59000`) is running and idle. The controller DMAs
as SMMU stream `0xE0`, which ABL puts in context bank 0 with translation off, so
it's 1:1 like USB's (`smmu.c` checks both).

The first read takes the request list over: stop it (`UTRLRSR` = 0), point
`UTRLBA` at a list in the DMA window, and start it again. Those are the only
three writes, all to the controller. Then one slot, one command at a time,
polled, with the same `readl`/`writel` barriers as the USB gadget. A `kload`ed
kernel finds the list where the previous kernel left it, and takes it over the
same way.

- `ufs` shows the power domain and clocks. If the interface and bus clocks run
  (an unclocked access can hang the bus), it adds the UFSHCI registers and the
  SMMU check.
- `ufs luns` shows the device and each LUN's size: LUN 0 236 GB (`userdata`,
  `super`, `persist`…), 1 and 2 `xbl_a` / `xbl_b`, 3 `cdt`/`ddr`, 4 2.3 GB
  (`tz`, `hyp`, `modem`…), 5 and 6 32 MB. There is no LUN 7.
- `ufs gpt <lun>` lists a LUN's partitions from its GPT (header at LBA 1, blocks
  of 4096 bytes). LUN 1 matches Android's `sgdisk` dump. The `last_parti` entry
  Android also shows has a zero type GUID, which the UEFI spec calls unused.
- `ufs read <lun> <lba> [offset]` hexdumps 256 bytes of a block (numbers in
  decimal or `0x` hex).

**OsitoFS on `userdata`.**
- **The device:** OsitoFS (`fs/ositofs2.c`) runs on a selectable backend
  (`osfs2_set_backend`), VirtIO-blk on virt. On the phone it's `userdata` as a
  byte device: commands of up to 16 blocks of 4 KB, and a write that covers a
  block only partly reads it first.
- **Format:** `ufs mkfs userdata` (the name typed as confirmation) formats it
  with the in-kernel `osfs2_format`, a port of `tools/ositofs/mkfs.c`: 1 MB
  blocks, 4096 file slots, the expanded layout. It writes only the first 4 MB:
  232 469 data blocks, 227 GB.
- **Mount:** boot mounts it (`ufs_mount_at_boot`, ~0.6 s), and `ufs mount`
  does it by hand.
- **Files:** the shell's `ls`, `cat`, `hexdump`, `df` and `exec` work on it.
- **Upload:** `put <name>` takes a file from the host with `kload`'s framing
  (`"OKLD"`, size, CRC-32, bytes; `arch/arm/scripts/rog5-put.ps1 -File <path>
  [-Name <name>]`, up to 1 GB, received in high memory). It replaces a file
  with that name, flushes the device's cache, reads the file back from UFS and
  checks the CRC before saying "stored and verified". It runs at the USB
  console's pace, ~1.67 MB/s: 5 MB of random bytes took 3 s, a 92 MB model
  54 s.
- **Download:** `get <name>` sends a file back in the same framing
  (`arch/arm/scripts/rog5-get.ps1 -Name <name> [-Out <path>]`, which checks
  the CRC). Other tasks may print during the transfer. While it runs,
  `console_host_hold` (`kernel/serial.c`) keeps their output on the screen
  and in the log, but off the USB link, so it can't land inside the binary
  data. It runs at ~2.9 MB/s: 21.6 MB in 7.3 s, over COM3 or COM4 alike.
- **Screenshots:** `screenshot [name]` writes the panel as a 24-bit BMP,
  `screen.bmp` by default: 7.9 MB for 1080×2448.
  - The panel's pixels are `0x00RRGGBB`, so B, G, R in memory, which is
    already BMP's order.
  - `arch/arm/scripts/rog5-screenshot.ps1 [-Out <file.png>]` runs it, fetches
    the BMP and saves a PNG, all in ~4 s. The console comes out at ~80 KB as
    a PNG, the desktop at ~150 KB.
- **Apps:** `exec <name> [args]` runs a program stored there (see "Apps").

---

## Apps

Programs are C files in `arch/arm/apps/`, linked against a small libc in
`arch/arm/libc/` into static PIEs:
- **Build and copy:** `arch/arm/scripts/rog5-app.sh [app...]` builds them on the
  Mac (`make -C arch/arm/libc CROSS=aarch64-elf-`). It fetches the ELFs into
  `arch/arm/build/apps/` and copies them onto the phone's OsitoFS with `put`.
- **Run:** `rog5-app.sh --run hello uno dos` does that and runs
  `exec hello.elf uno dos`.
- **Check:** `make check` makes sure each ELF is a PIE whose relocations are
  all `R_AARCH64_RELATIVE`.

**The libc:**
- startup (`_start(argc, argv)` → `main` → `exit`);
- Linux AArch64 syscalls over `svc #0`;
- `string.h`, `ctype.h`;
- `stdio.h`: a full `printf` family with flags, width, precision and
  `hh`/`h`/`l`/`ll`/`z`, `%d %u %x %o %p %s %c %f %e %g`; line-buffered stdout;
  `fgets`/`getchar` from the console; read-only `fopen`/`fread`/`fseek`;
- `stdlib.h`: first-fit `malloc` over `brk`, `strtol`/`strtod`, `qsort`,
  `bsearch`, `rand`, `atexit`/`exit`;
- `time.h`, `sleep`/`usleep`, `errno`, `assert`.

**How a program runs** (`kernel/process.c`, `elf.c`, `syscall.c`):
- **Placement:** one at a time, in the shell's task, at EL1 in the kernel's
  address space. `elf.c` puts every PT_LOAD into one contiguous allocation at
  its original distances, and applies a PIE's RELATIVE relocations (as
  `start.S` does for the kernel), so pointers in its data work wherever it
  lands. It also cleans the D-cache and invalidates the I-cache over the
  code.
- **Memory:** the program gets its own 256 KB stack, a contiguous 32 MB heap
  region for `brk`, and anonymous `mmap`.
- **When it ends:** all of it is freed, open files included. The IRQ state is
  restored too: `exit` comes back through the SVC exception with IRQs masked,
  and on cpu0 that stopped the tick every sleeping task waits on. That was a
  bug in the old `exec` until 2026-10-30.
- **Waits:** a console `read` and `nanosleep` unmask IRQs while they wait, for
  the same reason.
- **Faults:** a fault in the program's own code ends the program with exit code
  139 and the fault's ESR and offset; the shell goes on. A fault anywhere else
  is still a kernel panic.

Examples, all run on the phone on 2026-10-30:

| App | Run on the phone |
|---|---|
| `hello` | arguments, a table of strings, `printf` formats |
| `primes [n]` | sieve over `malloc`'d memory: 664 579 primes up to 10 M in 0.28 s on an A55 |
| `cat <file>...` | OsitoFS files through `fopen`/`fread`, `perror` on a missing one |
| `guess` | reads guesses with `fgets`, echoed (found 10 in 6 tries) |
| `countdown [s]` | `sleep(1)` in a loop, measured 2.99 s for 3 |
| `crash` | writes through a null pointer: exit code 139, the kernel lives |

**Limits:**
- There is no memory protection: a stray pointer can corrupt the kernel.
- Files are read-only from programs.
- The shell doesn't parse quotes.
- The context switch doesn't save FP/SIMD registers. A program may use them
  as long as no other task on its CPU does (the kernel is built with
  `-mgeneral-regs-only`, NEON inference aside).

---

## Local LLM

The inference stack is x86's, ported, replacing an older copy that arm had
compiled but never used:
- `fs/gguf.c` reads the hyperparameters from the GGUF itself.
- `kernel/tokenizer.c` + `tokenizer_trie.c` do BPE or SentencePiece, chosen
  by `tokenizer.ggml.model`.
- `kernel/inference.c` runs llama-style models and brandon-tiny, picking the
  chat template from the vocabulary.
- `kernel/tensor.c` holds the kernels.

`kernel/llm.c` is the arm side:
- It loads a model from OsitoFS into high memory and sets up the tokenizer.
  `inference.c` puts the model's state (KV cache, scratch) in high memory
  too.
- It holds the shell commands.
- It splits every matvec over four CPUs (below).
- It stubs what x86's code expects of its kernel: x86's own SMP paths
  (`ap_worker_count = 0`), NVMe layer streaming and the profiler.

```
put (host): arch/arm/scripts/rog5-put.ps1 -File smollm2-135m-q4_0.gguf -Name smollm2.gguf
model smollm2.gguf        # load it (model: the loaded one)
chat What is the capital of France?
llm selftest              # NEON against the scalar code, the split, the math
llm simd off              # the scalar path, for comparison
llm smp off               # the X1 alone, for comparison
```

**SIMD.** x86 calls its AVX2 kernels by name when `tensor_has_avx2()` says
so. Here `tensor_has_avx2()` returns `tensor_simd`, and `kernel/tensor_neon.c`
defines those names with the same signatures, in NEON:
- `matvec_q4_0_avx2`, `matvec_f16_avx2`, `rmsnorm_avx2`, `vec_add_avx2`,
  `vec_mul_avx2`;
- plus `matvec_q4_1_neon`.

`llm selftest` compares each against the scalar code on random data, with
largest relative differences of 0 to 1e-6.

**What had to change from x86:**
- **Math.** The x87 code became double-precision C: `fsqrt` for the square
  root, then range reduction plus a short series for exp/log/sin/cos. The
  selftest checks it against known values, including cos(1000) for RoPE.
- **Timing.** `rdtsc` reads CNTPCT, with `TSC_PER_MS` 19200.
- **Q4_1.** `matvec`/`embed_token` gained it. x86 lacks it too, and
  `smollm2-135m-q4_0.gguf` has some Q4_1 tensors, which x86 zeroes ("output
  zeroed" warning).
- **The old NEON kernels** had the wrong Q4_0 nibble order (2j / 2j+1 instead
  of j / j+16). They had never run: `tensor_has_avx2()` returned 0, so arm
  inference was all scalar.

**Cores.** `inference.c`'s `matvec` splits every matvec of 64 rows a part
or more by rows, for any tensor type. It runs on four parts:
- the X1 does one;
- three `llm` tasks pinned to the A78s (CPUs 4–6) do the others
  (`llm_par_run` in `llm.c`).

While `model` or `chat` runs the workers spin waiting for work, because a
matvec takes a fraction of a millisecond and a sleep takes a tick.
Otherwise they sleep. The A55s are left out: an even split would wait on
them. Attention, the norms and the small K/V matvecs still run on the X1
alone.

**FP state.** The scheduler saves no FP/SIMD registers. FP code (the
Makefile's `FP_SRCS`) runs only in pinned tasks, one per CPU:
- the shell, which `model` and `chat` move to the X1 (CPU 7, `task_pin`) and
  back;
- the three workers.

Nothing else there uses FP: GUI, touch, keys and IRQ code are built with
`-mgeneral-regs-only`.

**Measured on the phone (NEON):**

| Model | Size | Load (UFS + parse) | X1 alone: prefill, per token | X1 + 3 A78s: prefill, per token |
|-------|------|--------------------|------------------------------|---------------------------------|
| `brandon-tiny-10m-instruct.f16.gguf` | 21 MB | 356 ms | 71 ms (18 tok), 9 ms | 55 ms (13 tok), 6 ms (~165 tok/s) |
| `smollm2-135m-q4_0.gguf` (BPE, ChatML) | 92 MB | 1.5 s | 681 ms (16 tok), 45 ms | 271 ms (16 tok), 20 ms (~50 tok/s) |

SmolLM2's answer to "What is the capital of France?" began "The capital of
France is Paris, a city steeped in history, culture, and romance." Models are
on `factory.naranjositos.tech/models/` (also `tinystories-llama2-20m.Q4_K_M`,
`llama-3.2-1b-instruct-q4_k_m`, whose K-quants run scalar).

---

## Desktop (phablet layout)

`desktop` puts the shared GUI (`gui/`, the dark glass desktop x86 runs) on the
panel in its phablet layout: status bar on top, navigation bar below, one
maximized window at a time (see `gui/README.md`, "Phablet layout"). At the
panel's physical size (70 x 158 mm, `platform_panel`) the theme picks 300%,
360 x 816 dp.

`kernel/gui_task.c` runs it as a task on whichever CPU is free:
- **The console** moves into the Terminal window. `framebuffer.c` is pointed
  at an off-screen surface the size of the window's content (64 columns), the
  desktop composes it, and the text is laid out again from the log
  (`console_redraw`), so nothing is lost. The volume keys still page its
  history.
- **Frames** are painted into a back buffer in cached RAM and copied to the
  panel, only when something changed: console text (`fb_take_dirty`), an
  animation, the status bar's minute, a key, a finger. The first frame takes
  39 ms on an A55 (20 ms once the wallpaper is cached).
- **The status bar** shows memory and, with no RTC on the phone, the uptime as
  its clock.
- **Power** brings the window behind to the front: Terminal or System, with
  the Appearance pane.
- **`desktop off`** gives the panel back to the console (`platform_console_to_panel`,
  redrawn from the log). `screen` refuses to change modes while the desktop is
  up, and the heartbeat squares stay off it.

- **Fingers** (see "Touchscreen"): a tap acts where it started, on the lift.
  The navigation bar's items bring their window to the front; anywhere else it
  is a click in the front window (the Appearance pane's controls). A finger
  that travels more than 8 dp is a swipe instead, and scrolls what is under it
  one wheel notch per 40 dp. The window title bar's buttons don't react yet:
  taps there go to the window as clicks.

---

## Touchscreen

From the phone's device tree: a FocalTech controller (`focaltech,fts`, an
FT3658U: chip `0x5652`, firmware `0x36`, vendor `0x8d`) at I2C address `0x38`
on QUP0 SE4 (`i2c@990000`), SDA/SCL on TLMM GPIO 20/21, reset on 22, interrupt on
23 (low), an IO power switch on 131. Coordinates are panel pixels
(1080 x 2448), up to ten fingers. `platform/sm8350/touch.c` drives it.

**Power.** ABL leaves both supplies off: `pm8350c_l3` (vdd, 3.008 V) and
`pm8350c_l8` (I/O, 1.8 V). They aren't the apps processor's to switch over SPMI:
`pmic list 2` marks none of PM8350C's LDOs writable by EE 0. The AOP owns them
and sets each from the votes of every subsystem, which arrive through that
subsystem's RSC. `platform/sm8350/rpmh.c` is the apps side, after Linux's
`rpmh-rsc.c` and `cmd-db.c`:
- **Command DB:** the AOP leaves it in DDR at `0x80860000` (magic `0x0C0330DB`,
  mapped as Device by `mmu.c`). It turns a resource name into its address:
  `ldoc3` is the VRM at `0x41100`, `ldoc8` at `0x42800`.
- **Apps RSC:** DRV 2 at `0x18220000`, TCSes from `+0xd00` (v2.3: 8 TCSes, 16
  commands each). The device tree's `tcs-config` is ACTIVE 2, SLEEP 3, WAKE 3,
  CONTROL 0, so TCS 0 and 1 are the AMCs, which send at once.
- A vote is one command per AMC: MSGID write + response required, address,
  data, then the trigger sequence of `__tcs_set_trigger`. The completion is
  polled in IRQ_STATUS; no GIC line is enabled for it. Only active-only votes:
  sleep and wake sets matter only when the CPU cluster power-collapses, and
  Osito-K never does.
- A regulator vote (`rpmh_vrm_vote`) sends voltage (mV, offset 0), mode (`0x8`;
  7 = HPM) and enable (`0x4`), and refuses names the command DB doesn't list as
  VRMs. Only drivers vote, with the device tree's fixed voltages: the shell's
  `rpmh` shows the RSC and the command DB, and `rpmh <name>` looks a resource
  up, both read-only.

**The bus.** SE4's QUP firmware has its FIFO interface disabled
(`GENI_IF_DISABLE_RO` bit 0, firmware `0x303`, I2C). The SE is set up for GPI
DMA, which Android shares with its trusted-touch VM, so Linux drives it through
the GSI. Until there's a GPI driver, `touch.c` bit-bangs I2C on the same two
pins as GPIOs (their sleep state is function gpio anyway). It works open drain
through the output enable, with the pull-ups, at about 150 kHz. A read of 63
bytes (every point) takes 4 ms, so the task's 9 bytes of the first point take
about 0.8 ms.

**The driver.** A `touch` task, started at boot:
1. Holds reset low, votes both supplies, raises GPIO 131, and releases reset.
2. Waits 200 ms for the controller's firmware, then reads its IDs.
3. Has the TLMM latch the interrupt line's falling edges (raw status in the
   pin's INTR_CFG; not enabled, so it is routed nowhere).
4. Every 10 ms, reads the first point when an edge was latched, and again while
   a finger is down so the lift isn't missed. Register 2 is the point count;
   each point is 6 bytes from register 3: event (0 down, 1 up, 2 contact) and X
   in the first two, ID and Y in the next two.

Presses, moves and lifts go into a 32-event queue the desktop drains
(`platform_touch_event`). When the desktop falls behind, moves may be dropped,
but presses and lifts never are.

- `touch` shows the controller's state, presses and reads, the last point, the
  pins, the QUP clocks, both supplies and SE4.
- `touch read` reads every point once, with the raw registers 0–8.
- `touch on` powers it up and probes it again (verbose); `touch off` drops the
  reset and the vdd vote (`ldoc8` stays on: always-on in Android).

Still open: GPI DMA for SE4 (rings, events and an SMMU stream), more than one
finger, the window buttons.

---

## Keys

`platform/sm8350/keys.c` reads the three keys from the PMICs over SPMI
(`pmic_arb.c`), at the registers Linux's `pm8941-pwrkey` and
`pinctrl-spmi-gpio` use:

| Key | Register | Pressed |
|-----|----------|---------|
| Power | PMK8350 (SID 0) PON_HLOS `0x1300`, RT_STS `0x1310` | bit 7 (kpdpwr) set |
| Vol-Down | the same register | bit 6 (resin) set |
| Vol-Up | PM8350 (SID 1) GPIO 6 (`0x8D00`), RT_STS `0x8D10` | bit 0 clear (pull-up) |

ABL leaves GPIO 6 an input with pull-up: `0x8D40` = 0 (digital in), `0x8D42` = 0
(30 µA pull-up), `0x8D46` = `0x80` (enabled). The keys' interrupts go through the
arbiter, which Osito-K doesn't route, so a task polls every 20 ms. A level counts
once two polls in a row agree (the device tree debounces 15 ms). `pmic_arb.c`
serializes the shell, the reboot path and this task with a lock, and caches the
channel each peripheral maps to (a lookup scans up to 512 registers).

- **Vol-Up / Vol-Down** page the screen back / forward 24 lines through the
  console history (see "Reading the screen"). Held, they repeat every 150 ms after
  0.5 s.
- **Both volume keys held 2 s:** `reboot bootloader`, into fastboot. Pressing
  both first returns to the live console and says so; letting go earlier cancels.
- **Power, shorter than 1 s:** two `[STAT]` lines: kernel uptime, CPUs online,
  free memory, USB state, and each cluster's MHz and hottest sensor. Holding Power
  ~10 s still resets the phone; the PMIC does that on its own.
- Every press and release also enters the GUI's input queue
  (`input_post_key`) as its PS/2 key: `E0 5E` Power, `E0 2E` / `E0 30` volume.
- `keys` shows each key's state and press count.

---

## Reading the screen (no UART)

The GENI UART TX FIFO does not drain once Osito-K runs, and there is no serial cable.
Until the USB console is up (and whenever the kernel dies before or inside it), the
framebuffer is the only output. `uart_putc` gives up on the first TX timeout
and drops all later output (before that, ~100k MMIO polls per character made the
banner take minutes).

**Two screen modes.**
- **Normal (the default):** the text console takes the whole panel (67 columns ×
  76 rows), on a cleared screen.
- **Debug:** the console starts at pixel row 700, and the rows above it show
  how far the boot got and whether each CPU is alive (below).
- **Choosing:** `SCREEN_DEBUG=1` at build time (`rog5-boot.sh --screen-debug`)
  boots in debug mode. The shell's `screen debug` / `screen normal` switches
  modes live; the console is laid out again from the log, so nothing on it is
  lost.
- **Use debug** for anything that might die before USB is up: in normal mode,
  the only clues are the text console and the panic band.

**Text console.** `platform_init` points `kernel/framebuffer.c` at the splash
framebuffer: the whole panel, or pixel rows 700 and below in debug mode.
`serial.c` writes every console byte there before the UART, so the boot log and
the shell show up on screen. Glyphs are the 8×16 font at 2× (67 columns; 76 rows,
54 in debug mode). The console never reads
the framebuffer back, because reads from it are uncached. It keeps the text in a
cached cell array (character + color), and a scroll redraws only the cells that
change. A scroll costs ~4.5 ms however far it moves, bound by writes to the
framebuffer. So at the bottom the text jumps up a quarter of the area (13 lines)
at a time, which keeps the cost close to plain drawing: `dmesg` (~50 lines) takes
~35 ms, against ~250 ms scrolling line by line.

**History.** What scrolls off the screen is still in `serial.c`'s log (`dmesg`'s
16 KB ring), and the volume keys page through it (see "Keys"):
- `fb_history` lays the log out the way `fb_put` lays out live text (wrapping,
  `\r`, `\b`), in the default color. The last row, in yellow, says how far back
  the view is.
- The view is anchored to its top line. While it's up, new output only reaches
  the log, and the status row says so.
- Paging to the end, or any shortcut, redraws the live console from the log's
  tail, with the cursor after it.

In debug mode, `kernel_main` paints 30-px bars above the console as it advances:

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
| 420–449 | one square per CPU, blinking every 0.5 s | that CPU's timer IRQs + scheduler alive |

The heartbeat row has a 60-px square per CPU at x = 70 × cpu (cpu0 on the left).
While a secondary CPU starts, its square shows the stage: **blue** up and parked,
**yellow** GIC redistributor up (**red**: it failed), **cyan** timer started,
**green** entering the scheduler. Then it blinks white/black on its own ticks, so a
square stuck on a color or frozen points at the CPU that died and where.

The tiny lines at the very top come from `start.S` (red/green/cyan/white = entry,
EL1, stack, BSS cleared), only in a `SCREEN_DEBUG=1` build. They're drawn before
the platform is known.

**Panic telemetry**, in both modes: it needs no lock, and the console's may be held
by another CPU. A synchronous exception paints an **orange** band at rows 500–529
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
10. **Console scrolls again (2026-10-29).** It had wrapped to the top since item 6.
    Scrolling now redraws changed cells from a cached copy of the text (see "Text
    console"). The scale-2 glyph path writes two pixels per 64-bit store with no
    per-pixel divide. That cut a scroll from 8 to ~4.5 ms, and jump scrolling does
    the rest.
11. **All eight cores start (2026-10-29).** `smp_boot_aps` uses PSCI `CPU_ON` over
    SMC with the MT-style MPIDRs (see "PSCI"). Each AP turns on its MMU with the
    BSP's tables and parks; `cpus` lists them. Before handing over to another
    kernel, `kload` turns them off again with `CPU_OFF`. That was verified by
    chain-loading: the next kernel's `CPU_ON` succeeds for all seven, and it would
    return ALREADY_ON if they were still running.
12. **Tasks run on all eight cores (2026-10-29).** See "SMP scheduling". Two
    bugs only showed up on the device:
    - The APs entered with SP_EL0 selected, so their first IRQ would have used an
      uninitialized SP_EL1; `ap_entry` now does `msr spsel, #1`.
    - The GIC refactor made gcc emit a post-indexed `str w0, [x1], #4` for the
      `GICR_IPRIORITYR` loop. The hypervisor traps GIC MMIO and emulates it from
      the abort syndrome, which a write-back access doesn't have, so it came back
      as a synchronous external abort (ESR `0x96000010`, FAR `0x17A70400`) before
      USB was up. GIC accesses are now single `ldr`/`str` in asm.

    `cpubench` measured the clocks the bootloader leaves: A55s at ~1.5 GHz, but the
    A78s at ~710 MHz and the X1 at ~845 MHz, so the big cores are the slow ones until
    something sets CPU frequencies.
13. **CPU frequency (2026-10-29).** Boot sets every cluster to its top level through
    EPSS (see "CPU frequency"). `cpubench` now reads 1802 / 2418 / 2840 MHz, and the
    pinned run went from 460 to 192 ms.
14. **Reschedule IPI (2026-10-29).** Idle CPUs get SGI 0 when there is a task they
    could run (see "SMP scheduling"). `cpubench` measures the delay from
    `task_create_on` to the task running on another CPU: 5.7 ms on average (7 ms
    max) waiting for ticks, now 4 µs (5 µs max). With `cpubench any` it stays in
    milliseconds, which is queueing, not wakeup: 16 tasks for 8 CPUs.
15. **Big cores first (2026-10-29).** Unpinned tasks take the fastest idle CPU (see
    "SMP scheduling"). `cpubench any 1` runs on the X1, `any 3` on the X1 and two
    A78s, `any 4` on all four big cores. With `any 8`, the four that start on A55s
    move up as the big cores free: 172 ms wall, against 183–192 ms pinned one per
    CPU. Before this change the IPI went to the lowest-numbered idle CPU, an A55.
16. **Flashing without buttons (2026-10-29).** `reboot bootloader` sets the restart
    reason in the PMIC over SPMI (see "Rebooting into fastboot"), and
    `rog5-boot.sh --flash` uses it. From a running Osito-K to the new build
    answering from `boot_a` takes ~75 s, with the build stamp checked. The same run
    was the first cold boot of the SMP kernel: all eight cores came up, and cpufreq
    moved the A78s from 710 to 2419 MHz.

17. **Light tasks on the A55s (2026-10-29).** Per-task utilization (see "Placement by
    load"). `cpubench mix` runs 2 busy and 4 light unpinned tasks; light ones do 1 ms
    of work every ~10 ms. After the first 300 ms, the busy ones were on the X1 and an
    A78 and the light ones only on cpu0-3, the same in three runs.
    `cpubench any 3` and the pinned run didn't change.
18. **Temperature limit (2026-10-29).** TSENS sensors, the `temp` command and a
    thermal task that caps the big clusters (see "Temperature"). Two findings on
    the way, both in "CPU frequency". First, the A78s and the X1 share a voltage
    rail, so the governor lowers them together. Second, lowering the A55s seemed
    to hang the machine, so for a while they stayed at their top level (item 19).
19. **USB barriers (2026-10-29).** The A55 "hang" was the USB gadget. Its driver
    had no barriers between the DMA window (Normal non-cacheable) and the
    controller's registers (Device). It was written while the D-cache was off,
    when every access was Device memory and ordered. With the shell polling USB
    from an A55 at 403 MHz, EP0 wedged on the second port open/close, while all
    four A55s kept ticking. With `readl`/`writel`-style barriers, 48 commands in a
    row went through at 403 MHz, and the A55s can be lowered again (see "Console
    over USB" and "CPU frequency"). `cpufreq try` found it: it moves the shell to
    a big core and watches the lowered CPUs' ticks from another.
20. **Keys (2026-10-29).** Power, Vol-Down and Vol-Up read over SPMI (see
    "Keys"). The first presses came back as 120, 100 and 460 ms. The older
    `spmi.c` guessed a v7 arbiter and a fixed channel, and data-aborted;
    `pmic_arb.c` looks the channel up. The volume keys page the screen through the
    console log, which `dmesg` already kept: the screen only ever had the lines
    it showed.
21. **Screen modes (2026-10-29).** The progress bars, heartbeat squares, `start.S`
    markers and the `cpufreq try` bar became debug mode (`SCREEN_DEBUG`, shell
    `screen`). By default the console has the whole panel.
22. **UFS, read-only (2026-10-29).** ABL leaves the UFS controller up with its
    SMMU stream untranslated, so taking over its request list was enough (see
    "Storage (UFS)"). The first `ufs luns` identified the Samsung part and its
    seven LUNs, and `ufs gpt 0` listed 23 partitions, `super` and `userdata`
    among them. The A55s' 300 MHz level came back the same day: it was the USB
    bug too.
23. **Apps from UFS (2026-10-29).** `userdata` became Osito-K's: OsitoFS on it,
    mounted at boot, writes refused anywhere else on the device. Files arrive
    over USB with `put` and are checked against their CRC after reading them
    back. `exec hello.elf` printed "Hello from userspace!" from a file stored on
    UFS, the first app loaded that way. It needed I-cache maintenance in
    `elf.c`, which predated the caches.
24. **Phablet desktop (2026-10-30).** The shared GUI gained a third layout,
    `GUI_LAYOUT_PHABLET`, and `desktop` runs it on the panel at 300% with the
    console inside the Terminal window (see "Desktop"). A photo of the phone
    showed the status bar, the maximized Terminal with `cpus`/`temp`/`ls` in it,
    and the navigation bar with both apps.
25. **C programs (2026-10-30).** A libc and `rog5-app.sh` (see "Apps"). Building
    them turned up three bugs in the old `exec`:
    - segments were loaded apart and never relocated, so stored pointers broke;
    - `brk` handed out memory that wasn't the program's;
    - `exit` left cpu0 with IRQs masked, which stopped the tick and with it the
      keys and the thermal task until the next reboot.
26. **Touchscreen (2026-10-30).** See "Touchscreen". Three surprises:
    - the supplies were off and not writable over SPMI, which took the first
      RPMh client;
    - SE4's FIFO is disabled (GPI DMA only), which led to bit-banged I2C;
    - the QUP AHB clocks are hardware-gated and read "off" while idle (Linux
      skips their halt check).

    The desktop takes taps and swipes. Confirmed on the phone: the navigation
    bar switches windows and the Appearance pane scrolls and takes clicks.
27. **Local LLM and high memory (2026-10-30).** See "Local LLM" and "MMU and
    caches". The 8 GB bank at 32 GB is usable as is: all eight probe words read
    back, and the hypervisor maps it. x86's inference stack runs with NEON
    kernels. SmolLM2-135M answers correctly at ~23 tok/s on the X1, once Q4_1
    was added; before that, the zeroed tensors gave word salad.
28. **All the RAM, and four cores for the LLM (2026-10-30).** Bank 2 joined the
    high memory around its three carve-outs: 15362 MB in all. Matvecs are
    split over the X1 and the A78s, which took SmolLM2 from 45 to 20 ms a token.
    The inference state moved to high memory, so bank 0 stays for the kernel.
29. **USB networking (2026-10-30).** See "USB networking". The gadget became an
    ACM + NCM composite on demand (`net up`), with NTB16 framing, a NIC
    interface in `net.c`, DHCP, a link-local fallback, and a background poller.
    Windows bound its inbox drivers on the first try, and pings cross both
    ways in 0–7 ms. `mem_init` stopped losing bank 0 to `kload` chains.
30. **Internet and HTTPS (2026-10-30).** See "Internet and HTTPS". Through
    Windows ICS, the phone resolves names and fetches pages over TLS 1.2
    (`https`). On the way, three bugs had to go:
    - x86's tick-based timeouts read the 19.2 MHz counter;
    - 16 KB TLS buffers overflowed the 8 KB task stacks and froze the phone;
      stacks are now 32 KB;
    - virt's virtio-net dropped every frame.

Still open:
- With `SCREEN_DEBUG=1`, `PLATFORM=virt` needs `-m 4G` in QEMU, because `start.S`
  paints `0xE5000000` before it knows the platform.

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

### Rebooting into fastboot

`reboot bootloader` in the Osito-K shell writes ABL's restart reason and reboots;
ABL comes up in fastboot ~16 s later. Android does the same (`qcom,reboot-reason`
in the phone's device tree). The reason is an nvmem cell in the PMK8350 PMIC
(SPMI SID 0): SDAM `0x7100`, byte `0x48`, bits 7:1, where 2 = bootloader. The PMIC
keeps it through any reset. ABL clears it once used, so the next boot is normal.

The PMIC is reached through the SPMI PMIC arbiter (`platform/sm8350/pmic_arb.c`,
v5.2 here: version register `0x50020000`):

| Region | Address | Use |
|--------|---------|-----|
| core | `0x0C440000` | APID map at `0x900 + 4n`: PPID (SID:peripheral) in bits 19:8 |
| cnfg | `0x0C40A000` | owner table at `0x700 + 4n`: the EE that may write through APID n |
| chnls | `0x0C600000` | write channel of APID n at `+ 0x10000 n` |
| obsrvr | `0x0E600000` | read channel of APID n at `+ 0x80 n` (EE 0's page) |

A one-byte access writes `(op << 27) | (reg << 4)` to the channel's CMD (`0x00`)
and polls STATUS (`0x08`); data is in WDATA0 (`0x10`) / RDATA0 (`0x18`). Writes
need an APID owned by EE 0 (HLOS), as Linux requires. `pmic` in the shell prints
the arbiter version and the restart reason, and `pmic <sid> <hex addr>` reads any
register: `pmic 0 0105` returns `0x2f`, the PMK8350 subtype. `pmic list <sid>`
lists one PMIC's peripherals with type/subtype, marking with `*` those EE 0 may
write. On PM8350C (SID 2) that is its GPIOs and PWM channels, not one LDO: the
AOP owns those, and they are reached through RPMh (see "Touchscreen"). `spmi.c` is an
earlier attempt that guessed a v7 layout with a fixed channel and data-aborted;
nothing uses it.

- `fastboot set_active` is refused ("Slot Change is not allowed in Lock State")
  even though `getvar unlocked` says yes; slot changes need the critical unlock.
- Osito-K never marks a boot successful (`slot-successful:a:no`), but this ABL
  doesn't count that against the slot: `slot-retry-count:a` stayed at 3 over four
  boots without flashing on 2026-10-29 (`fastboot reboot`, two plain `reboot`s,
  `reboot bootloader`), and slot a stayed bootable. Slot b: retry count 0.
- To get back to fastboot from a hung kernel: hold Power + Vol-Down. From a live
  one, `reboot bootloader` (see "Rebooting into fastboot").

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
| `0x800000000`–`0xA00000000` | DDR bank 1 (8 GB, no carve-outs): Osito-K's high memory region |
| `0xC0000000`–`0x280000000` | DDR bank 2 (7 GB): carve-outs `0xD8800000`–`0xE3000000`, `0xE5000000`–`0xE7300000` (splash framebuffer), `0xE7400000`–`0xE7500000`; the rest is high memory |
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

The conduit is SMC (QEMU virt uses HVC). `kernel/smp.c` also uses `CPU_ON`
(`0xC4000003`), `CPU_OFF` (`0x84000002`) and `AFFINITY_INFO` (`0xC4000004`). MPIDR
has MT set, so Aff0 is the thread (always 0) and the core number is Aff1:

| Core | MPIDR        | Type                |
|------|--------------|---------------------|
| 0–3  | `0x81000000`–`0x81000300` | Cortex-A55 r2p0 (cpu0 is the boot CPU) |
| 4–6  | `0x81000400`–`0x81000600` | Cortex-A78 r1p0 |
| 7    | `0x81000700` | Cortex-X1 r1p0      |

`CPU_ON` takes the target MPIDR, a physical entry address and a context ID that
arrives in `x0`. The core enters at EL1 with the MMU and caches off, so whatever
it reads before turning its MMU on has to be cleaned to PoC first. `smp.c` passes
a 64-byte block holding the BSP's MAIR/TCR/TTBR/CPACR/SCTLR and a stack, and the
entry is assembly because gcc ignores `naked` on aarch64.

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
| `arch/arm/platform/sm8350/cpufreq.c` | EPSS CPU frequency: levels, shared-rail report, `cpufreq try`, `cpufreq l3` |
| `arch/arm/platform/sm8350/thermal.c` | TSENS sensors (`temp`) and the big clusters' thermal cap |
| `arch/arm/platform/sm8350/pmic_arb.c` | SPMI PMIC arbiter v5: single-byte PMIC reads and writes, `pmic list` |
| `arch/arm/platform/sm8350/rpmh.c` | Command DB lookups and active-only RPMh votes through the apps RSC (`rpmh`) |
| `arch/arm/platform/sm8350/touch.c` | FocalTech touchscreen: RPMh supplies, bit-banged I2C, polling task, event queue |
| `arch/arm/platform/sm8350/ncm.c` | USB networking: NTB16 framing, frame queues, the stack's NIC, `net` task and command |
| `arch/arm/kernel/dhcp.c` | DHCP client (DISCOVER/OFFER/REQUEST/ACK) |
| `arch/arm/kernel/http.c`, `tls.c` | x86's HTTP/1.1 and TLS 1.2 clients, ported (`https`; certificates not verified) |
| `arch/arm/kernel/llm.c` | `model` / `chat` / `llm`: GGUF from OsitoFS into high memory, tokenizer, x86 glue stubs, NEON selftest |
| `arch/arm/kernel/tensor_neon.c` | NEON kernels under x86's AVX2 names (Q4_0, Q4_1, F16 matvec, rmsnorm, vec ops) |
| `arch/arm/kernel/{inference,tensor,tokenizer,tokenizer_trie}.c`, `fs/gguf.c` | x86's inference stack, ported (CNTPCT timing, double-precision math, Q4_1) |
| `arch/arm/platform/sm8350/keys.c` | Power / volume keys: console history, fastboot and status shortcuts |
| `arch/arm/platform/sm8350/ufs.c` | UFS host controller: LUNs, GPT, block dumps; writes only into `userdata` (OsitoFS) |
| `arch/arm/scripts/rog5-put.ps1` | Host side of `put`: copy a file onto the phone's OsitoFS |
| `arch/arm/scripts/rog5-get.ps1` | Host side of `get`: copy a file from the phone's OsitoFS |
| `arch/arm/scripts/rog5-screenshot.ps1` | `screenshot` on the phone, then the BMP fetched and saved as a PNG |
| `arch/arm/scripts/rog5-app.sh` | Build `arch/arm/apps` on the Mac, copy them to the phone, run one |
| `arch/arm/libc/` | The programs' libc: startup, syscalls, stdio/stdlib/string/time; `make check` |
| `arch/arm/apps/` | Example programs (hello, primes, cat, guess, countdown, crash) |
| `arch/arm/kernel/elf.c`, `process.c` | Load a static PIE contiguously and relocate it; run it on its own stack, free it after |
| `arch/arm/platform/sm8350/smmu.c` | Checks a DMA master's stream is 1:1 through the apps SMMU |
| `arch/arm/scripts/rog5-kload.ps1` | Host side of `kload` (used by `rog5-boot.sh --usb`) |
| `arch/arm/include/sm8350.h` | Hardware register definitions (verified) |
| `arch/arm/include/aarch64.h` | System register helpers |
| `arch/arm/platform/sm8350/` | Platform-specific code |
| `arch/arm/kernel/main.c` | Kernel entry (kernel_main) |
| `arch/arm/kernel/framebuffer.c` | Splash FB console and its history view of the log |
| `arch/arm/kernel/gui_task.c` | `desktop`: the shared GUI's phablet layout on the panel, console in its Terminal |
| `arch/arm/kernel/serial.c` | GENI UART driver |
| `arch/arm/kernel/timer.c` | ARM Generic Timer |
| `arch/arm/kernel/gic.c` | GICv3 interrupt controller |
| `arch/arm/kernel/sched.c` | Preemptive scheduler |
| `arch/arm/kernel/context_switch.S` | AArch64 context frame (272B min) |
| `~/phone_breaker2/sm8350-boot/` (Mac) | Boot shim (v0.8) |
| `~/phone_breaker2/repack_boot.sh` (Mac) | Packs the **Linux** image; Osito-K uses `rog5-boot.sh` |
| `~/phone_breaker2/rog5-patched.dtb` (Mac) | Stock DTB + simplefb node |
| `~/phone_breaker2/recovery-kit/` (Mac) | Stock boot backup |
