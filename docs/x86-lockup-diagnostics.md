# Opt-in BSP lockup diagnostics

`lockup` is a temporary diagnostic for a machine that stops responding to the
keyboard, compositor and SSH together. It is not recovery, a general NMI
subsystem, or a production watchdog. It is off after every boot and kexec.

```
lockup status
lockup arm 10
lockup sample
lockup status
lockup off
```

`arm` accepts a stall threshold of 2..60 seconds (default 10), requires an
available AP, calibrated TSC and supported GOP scanout, and reserves one AP
for at most 300 seconds. The worker polls with PAUSE; it is intentionally
temporary and changes CPU usage/power while armed. `off` requests release;
`status` reports whether the observer is still running. The shell serializes
arming; only the read-only status query is admitted alongside a foreground
console application over SSH.

The AP observes timer ticks and scheduler switches independently. If either
does not advance for the threshold, it requests one BSP NMI through xAPIC.
With interrupts disabled, both counters stop; the older observation can
reach its threshold first. The reported reason identifies that observation,
not the root cause. Inspect captured FLAGS.IF to distinguish an IRQ-off
sample. Intentional long scheduler suppression, including native DOS, can
trigger this diagnostic; do not leave it armed for unrelated work.

The requested NMI uses the existing independent IST5. An assembly-only path
copies the hardware frame, saved GPRs and CR0/CR2/CR3/CR4, then restores the
original context. It bypasses scheduler bookkeeping, per-task FPU save slots,
C code, allocation, serial output and filesystem locks. No EOI is sent for
the NMI. Unrequested NMIs retain the previous dispatch behavior. The xAPIC
command uses physical BSP destination, NMI delivery, edge trigger and assert
level, following Intel's [SDM Volume 3A, sections 6.7 and 11.6.1](https://cdrdv2-public.intel.com/825758/253668-sdm-vol-3a.pdf).

The AP, not the NMI handler, formats a bounded monochrome report directly
into the previously validated permanent GOP mapping. It does not invoke the
terminal, compositor, network or filesystem while the BSP may hold their
locks. `sample` requests the same capture without drawing or ending the
observer, so an idle machine can verify delivery/return before reproducing
the real hang. Completed captures use a generation check for status reads.

ICR readiness and NMI acknowledgement waits are bounded. An unacknowledged
NMI remains marked pending, preventing another arm from recycling its
storage before a late delivery. If the BSP cannot accept an NMI, the AP
reports that instead of inventing a RIP. A stopped AP, stalled memory bus,
unmapped/broken GOP scanout, SMM, or nested NMI blocking may still prevent
useful evidence. The monitor never forces IF on, releases a kernel lock,
terminates a task, writes a crash file, or resets hardware.

The snapshot is RAM-only: photograph the report before resetting. Readable
kernel RIPs must be symbolized against the exact booted ELF, not a later
workspace rebuild. User-code RIPs also require the process/module mapping.

## Validation

- `bash arch/x86/scripts/test-lockup.sh`: GCC and Clang, normal and
  ASan/UBSan; 5,173 checks per variant. Covers admission, no-AP/invalid-GOP
  failures, timer/scheduler stalls, expiry, cancellation, BSP worker rejection,
  sample, failed/late NMI, and framebuffer clipping/pitch/guard pixels.
- `bash arch/x86/scripts/test-shell-remote.sh <fresh-output>`: 146 admission
  checks per compiler/mode, including status allowed while mutation is denied
  during foreground console use.
- `OSITO_SSH_TEST_STORAGE=usb OSITO_SSH_TEST_LOCKUP=1 bash
  arch/x86/scripts/test-ssh-qemu.sh <fresh-output>`: isolated managed disks;
  authenticated SSH before/after an explicit sample; privileged PE64 fixtures
  stop IRQs or preemption, then require unchanged FPU/SSE state and FLAGS.IF/DF
  after the NMI. A final deliberate hard stall produces a QMP screenshot and
  an IF=0 register record. The fixture gets diagnostic symbol addresses from
  the exact test ELF; no production address or application name is hardcoded.

The first QEMU run stopped on a test expectation that CLI must always report
the timer before the scheduler (both had stopped). The second passed the NMI
and register checks but lacked optional Pillow at PNG conversion. The runner
now uses the repository's existing PNG writer; neither harness failure is
counted as a passing run. The third run passed before a final xAPIC assert-bit
and display-clearing cleanup. Final build/physical evidence is recorded with
the deployed kernel below.

The final QEMU run, `arch/x86/build/lockup-qemu-20260926-r4/`, passes all
SSH/sample/CLI/preemption/hard-stall gates. `lockup.png` was visually checked:
the register report is legible and clears the previous text behind it.
`build/lockup-owner-qemu-20260926-r1/` also passes the same-PEB-VA parent/child
thread ownership regression with this exact kernel on USB storage.
The final host rerun is `build/lockup-host-20260926-r2/`; shell admission
evidence is `build/lockup-shell-remote-20260926-r1/`. Owned QEMU instances
exit and release their managed scratch disks after each run.

Tested kernel: 7,164,112 bytes, SHA-256
`4202d369d566208b07e0ad7239f28a064af8a80c8a92a26be30dee230d5ffc5d`.
It was uploaded as `/kernel-lockup-20260926.elf` and installed with
`kupdate --file ... --no-kexec`; both OsitoFS and EFI verification succeeded.
Host deployment logs are `build/p1-lockup-{upload,install,kexec}-20260926.log`.

After kexec, the P1 rejoined `192.168.1.20` automatically. `lockup status`
confirmed the default off state. `lockup arm 10` followed by `lockup sample`
returned a real NMI snapshot (`RIP=FFFF800002071060`, `FLAGS=286`) while SSH
remained usable; the saved response is `build/p1-lockup-sample-20260926.log`.
Steam was then launched with the observer still armed. SSH stopped responding
again. Host evidence in `build/p1-steam-lockup-retest-20260926-r1/` includes
three complete dmesg responses; the last reaches a scheduled
`SteamServiceTmp.exe /Update` child. No final exception appears there.
The physical stall report still needs to be read from the P1 screen. A
successful sample proves NMI delivery/return in that idle context, not that
every later hard lockup remains NMI-recoverable or that Steam is fixed.

GitNexus was unavailable. Manual impact covered the selective branch in
`isr_common`, shell admission/dispatch/help, build lists and the new diagnostic
module. Normal timer/fault dispatch, scheduler and driver implementations
are unchanged by this diagnostic.

## Physical Steam lockup identified

The user supplied the P1 photograph before resetting. The saved local evidence
is `build/p1-lockup-photo-20260926.png` (under `arch/x86/`). Transcription against
the exact `4202d369...` kernel above:

```text
BSP timer stopped
state=3 observer_running=1 ticks=38CB switches=E26A
RIP=FFFF8000021C1D12 CS=38 RSP=FFFF800022337D70 FLAGS=200002
CR3=981F6000 CR2=4000A2E778 CR0=80010013 CR4=40768
RAX=1 RBX=6 RCX=200282 RDX=1 RSI=6 RDI=0
RBP=FFFF800022337DC0 R8=0 R9=0 R10=FFFF8000021C1CC0 R11=0
R12=FFFF80000495CCD8 R13=6 R14=0 R15=0
```

The RIP is `file_lock_cancel_waiters+0x52`, the IRQ-disabled acquisition loop
for `g_file_range_lock` (R12). The original exception frame is not in this
snapshot; CR2 is the last fault address, not necessarily an access by the
sampled instruction. RCX contains this acquisition's saved incoming flags.

The global I/O guard list published addresses from private PE64 stacks.
An isolated old-kernel QEMU probe with separate process CR3s reproduces a
fault on that list, followed by the same cleanup deadlock at **the identical
RIP**, with IF=0. See section 4.146 of
[Win32 correctness](win32-layer-correctness.md) for the resident-record fix,
negative control and regression evidence. This report is a concrete use of
the NMI diagnostic, not a claim that arbitrary lockups can be recovered.
