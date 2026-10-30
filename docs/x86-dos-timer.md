# Native DOS Timer Deadlines

## P1 investigation, 2026-09-24

The physical P1 reported roughly one DOOM frame every 40 seconds after a local
`dosrun DOOM.EXE -nosound` launch was suggested. The user subsequently confirmed
native execution without `--emulate`, but also without `-nosound`: audio was
enabled. F10/Y was unusable at that speed and the USB was returned for update.
The original no-audio QEMU control therefore does not isolate the contribution
of native audio to this physical run. A no-audio P1 comparison is still needed.
Ping and SSH timed out during the run. That alone does not prove a crash:
the native DOS backend currently owns the BSP's GDT/LDT/IDT state and suppresses
`sched_tick()`, so background networking and SSH tasks cannot run normally.

Code inspection found a timer ownership defect:

1. Native DOS skips `sched_tick()`, including `sched_sleep_timeout_sweep()`.
2. The TSC-deadline rearm path still consulted `sched_get_next_deadline_us()`.
3. An expired scheduler sleep remains pending until the scheduler can run.
   Its deadline calculation returns one microsecond for every subsequent call.
4. Rearming against that unserviceable deadline can continuously interrupt the
   DOS client. Conversely, a scheduler idle result of zero can starve virtual
   devices when no other deadline exists.

`kernel/timer_deadline.h` now selects a 10 ms device-service interval while
native DOS owns the CPU, matching the existing 100 Hz periodic-timer backend.
It does not query scheduler deadlines in that state. Audio deadlines still
take precedence, because audio work continues to run in the interrupt handler.
Leaving native DOS immediately restores ordinary scheduler/idle selection.
The periodic backend, guest PIT/PIC semantics, VGA mediation, executable and
native task-switching restriction are unchanged. This is not a new promise
of cycle-accurate PIT delivery or native DOS multitasking.

GitNexus tools and CLI were unavailable. Manual impact review covered the
vector-32 handler, scheduler sleep expiry, native DOS ownership and the audio
deadline provider. Only the TSC-deadline selection call changes in `idt.c`.

## Verification

`scripts/test-timer-deadline.sh` exercises the production selector with recorded
scheduler/audio providers: 4,497 checks each under GCC and Clang, both normal
and ASan/UBSan. Cases include expired/future sleepers, tickless idle, no audio,
earlier/equal/later audio, state transition out of DOS and repeated rearming.
The synthetic 40-second case selects 4,000 10 ms deadlines instead of repeated
1 us deadlines; it is a deterministic policy test, not a frame-rate benchmark.

Artifacts: `arch/x86/build/timer-deadline-20260924/`.

Kernel: 7,102,936 bytes, SHA256
`e989d9d2d1effc03d4f503508d2338a399d15c3066277d523ac69887f4ff1dc9`.
The build succeeds with the existing three packed-TSS-member warnings.
`git diff --check` passes.

QEMU/KVM, four CPUs and 2 GiB, boots this exact binary with scratch ESP/OsitoFS3
disks. Native `dpnif16.com`, `dpnif32.com`, `dpirq32.com` and `dpvgamem.com`
print their PASS markers and terminate with their expected codes. Unmodified
DOOM with `-nosound` produces differing captures, receives F10/Y, exits with
code zero, and a subsequent shell echo succeeds. QEMU exits zero and the
managed scratch disks are released. Evidence is in
`arch/x86/build/dos-timer-qemu-20260924-v2/`.

The first smoke-harness attempt stopped after a passing `dpnif16` because it
expected the interpreter's exit message instead of the native termination
message. Its raw log is retained in `dos-timer-qemu-20260924/`; the second
attempt checks native PASS, exact exit code and normal shell recovery.

This host's QEMU uses the periodic timer, not TSC-deadline. It qualifies the
unchanged periodic path and kernel integration only. The timer defect is
demonstrated by source and host tests; its share of the P1 slowdown and actual
improvement still require a physical run with this kernel. VGA exception cost
and native DOS background-task starvation remain separate work.

## USB readback and deployment

The returned development USB (serial `00000000CY02`) was read without writing
the raw device. `diag/latest.txt` identifies `diag/boot2.log`, last flushed at
tick 82015 with reason `shell-exec`. That log contains the exact local command
`dosrun DOOM.EXE` and stops at its pre-execution marker. It contains no persisted
runtime timing or exit evidence for this game run; absence of a saved fault is
not proof that none occurred. The early IDT timer-mode record is also absent.

On September 24 at 13:35:41 UTC, only `EFI/BOOT/kernel.elf` was replaced with
the tested 7,102,936-byte binary above. The previous SHA256
`e03b77749bfe0a96f4708df3c1acbd851a8d3cb3435c48485ece8d796f4d5a84`
is retained as `EFI/BOOT/DOSTSAFE.ELF` and in the local artifact directory.
Readback hashes confirm the bootloader and all other existing EFI files,
primary/backup GPT and the first 33,751,040 bytes of OsitoFS3 metadata are
unchanged. EFI writes were flushed successfully. The diagnostic readback,
preflight, update and coordinator all finished with exit zero.

Evidence: `arch/x86/build/usb-dos-timer-20260924/`. No old logs were deleted
and no new whole-disk image was created. The next physical comparison is
`dosrun DOOM.EXE -nosound`; native audio has not been qualified by this update.
