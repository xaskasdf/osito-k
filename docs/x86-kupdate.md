# Persistent USB kernel updates

`kexec` only changes the running kernel. UEFI reads `EFI/BOOT/kernel.elf`
from the FAT EFI System Partition, not `/kernel.elf` in OsitoFS.
`kupdate` now updates both copies before invoking `kexec`.

## Commands

```
kupdate --status
kupdate --file /uploaded-kernel.elf --no-kexec
kupdate --file /uploaded-kernel.elf
kupdate [host] [/path] [--channel stable] [--no-kexec]
```

The default HTTPS URL remains
`https://wasm.naranjositos.tech/k/x86_64/stable/kernel.elf`.
`--file` uses an already downloaded/transferred file. `--no-kexec` still
updates the USB, but leaves the current session running. `--status` only
checks the target and existing ELF. Mutating commands retain the shell's
exclusive remote-command policy. The running kernel must contain this
implementation; bootstrap an older system with one ordinary `kexec` first.

## Target and publication

- Only the writable USB device currently backing mounted OsitoFS is eligible.
  The updater does not switch the active disk or scan internal EFI volumes.
- GPT header/entry CRCs, geometry, partition ranges and overlaps are checked.
  Exactly one ESP and the mounted data partition's start must be present.
  Invalid primary GPT metadata is rejected, not repaired automatically.
- FAT16 and mirrored FAT32 with 512/4096-byte sectors are supported. Existing
  `EFI/BOOT/BOOTX64.EFI` and `KERNEL.ELF` must exist. This does not install a
  bootloader or create a filesystem. FAT12 and non-mirrored FAT32 are refused.
- Both the old EFI kernel and incoming kernel pass the full x86-64 ELF
  validator shared with `kexec`, not merely a magic-number check. HTTPS also
  requires status 200, a bounded exact Content-Length and a complete body.
- The old EFI payload is saved as `/kernel.previous.elf` in OsitoFS before
  changing the ESP. `/kernel.elf` receives the new payload. Each OsitoFS
  publication uses a unique staging file, flush, full readback and rename.
- All reachable FAT chains are checked for loops/cross-links. New contents
  use free clusters, with contiguous I/O coalesced up to 64 KiB. The old
  contents are not overwritten in place. Data, FAT mirrors and the final
  directory sector are flushed and read back. Only then is the old chain
  freed. Valid FSInfo hints are invalidated; absent/invalid optional hints
  are left invalid, as FAT readers must ignore them.
- On an I/O/validation failure `kexec` is cancelled. A failed directory
  publication is restored and verified before attempting FAT rollback. If
  rollback cannot be confirmed, both chains remain allocated and the error
  explicitly reports uncertainty. Repair the volume before retrying.
- Reapplying the same EFI payload does not replace the previous-kernel
  backup or rewrite EFI data. It can still refresh OsitoFS and hotreload.

Limits: 32 MiB kernel, 16 MiB per FAT, 128 GPT entries (128-1024 bytes each),
16 directory levels and 65,536 directory entries. These are explicit
validation/resource bounds for boot media, not general filesystem limits.

This is **not** a journaled FAT transaction, an A/B boot selector, a signed
update protocol or an automatic fallback bootloader. A torn sector, loss of
power between FAT mirror writes, a lying device cache, or an OsitoFS rename
failure can require repair. In particular the existing unsupported-flush
policy for inexpensive USB devices cannot guarantee power-loss durability.
The backup in OsitoFS is a recovery artifact, not a UEFI-loadable fallback.
An EFI failure after OsitoFS publication can leave `/kernel.elf` newer than
the boot copy; the command reports failure rather than claiming success.

Format references: [UEFI GPT layout](https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html)
and [Microsoft FAT specification, sections 3-6](https://www.scs.stanford.edu/~zyedidia/docs/_other/fat.pdf).

## Validation

```
bash arch/x86/scripts/test-kernel-update.sh build/kernel-update-host
bash arch/x86/scripts/test-kernel-update-qemu.sh arch/x86/build/kernel-update-qemu
```

The host suite runs with GCC/Clang, normally and under ASan/UBSan. It covers
both FAT types and sector sizes, fragmented chains, malformed GPT/FAT,
cross-links, exhausted space, repeated replacement without chain leaks,
failed and silently corrupt I/O, failed writes that actually reached disk,
persistent transport failure, HTTP/ELF validation, staging/rename/flush
failure, unchanged updates, and no-hot-reload-on-error behavior.

The QEMU suite uses a managed disposable GPT USB disk containing both ESP
and OsitoFS. It updates, resets through UEFI, reapplies the file and performs
a hotreload, then extracts and byte-compares the EFI payload and runs
`fsck.fat -n`. It requires KVM, OVMF, mtools, gdisk and dosfstools. Scratch
images are removed by the existing test-disk manager; evidence stays under
the requested output directory.

### 2026-09-26 evidence

`kernel-update-host-20260926-r5`: 7,022 FAT/GPT/I/O and 740 command checks
per compiler/sanitizer variant, all passed. The shell admission suite passed
114 checks and WLAN shell regression passed 158,743 checks per variant.
`kernel-update-qemu-20260926-r3` passed install, UEFI reset, repeat update,
hotreload, exact extracted-payload comparison and `fsck.fat -n` (exit 0;
free-space summary intentionally unknown, no chain errors).

Tested/deployed kernel: 7,146,592 bytes, SHA-256
`253016171266b8c76af4c4b54fe7984b1a84cfcb633bffb1c85a9c0eeb6ab183`.
On the physical P1, the SSH sync upload completed with SHA verification.
An initial kexec loaded this implementation; WLAN/DHCP and SSH returned
automatically. `kupdate --file /kernel-kupdate-20260926.elf --no-kexec`
then backed up the existing EFI kernel and verified both new copies on
`usb0`, completing in approximately 8 seconds including SSH setup. A fresh
`kupdate --status` succeeded and the driver manager reported WLAN/IPv4/SSH
ready. Logs are `p1-upload.log`, `p1-bootstrap-kexec.log` (expected transport
timeout during reboot), and `p1-persistent-update.log` in the QEMU evidence
directory. The initial deployment left the P1 running at the shell; physical
reboot qualification followed separately, as recorded below.

### Physical reboot follow-up

An SSH `reboot` was issued with the local console confirmed idle (uptime
11m42s). SSH and ARP did not return at the previous address. The user
reported the P1 had reached DHCP but appeared frozen, and manually restarted
it. The failed attempt's volatile kernel log was not recovered; its cause
is unconfirmed and this is not evidence that the intermittent freeze is fixed.

After that manual restart, SSH returned automatically at `192.168.1.20`
with uptime 41 seconds. `p1-uefi-boot-dmesg.log` in the same evidence directory
records OsitoFS mounted from `usb0`, completed boot, WLAN association, and
DHCP DISCOVER/OFFER/REQUEST/ACK without retries. The lease is 86,400 seconds.
WLAN status shows channel 6 (2.4 GHz), an open data port and 24/24 firmware
commands completed. The driver manager reports HDA/IPv4/SSH/WLAN ready.
`kupdate --status` executes in the running kernel and validates the existing
EFI kernel without writes, confirming the new implementation survived a
physical firmware boot. Subsequent SSH diagnostics remain responsive.
The successful manual restart validates persistence, but unattended restart
reliability still needs investigation of the first attempt.

### ACPI reboot path

Subsequent diagnostic-kernel deployment and a second remote `reboot` left
the same DHCP address on a frozen local screen. After a manual power cycle,
the ring advanced only one slot, to a successful boot with all post-DHCP SSH
markers. The remote attempt did not leave its own boot log. The old command
sent an 8042 reset pulse, then disabled normal execution with a triple fault;
it did not use the firmware's ACPI reset register. Its SSH output was also
hidden from the physical console. Thus an old DHCP message was not evidence
of a new boot hanging in DHCP.

`reboot` now prepares the FADT reset register before disabling interrupts.
It checks the table signature, revision, bounded length, checksum, support
flag, GAS byte width/offset/access size and address range. System I/O,
mapped MMIO and segment-zero/bus-zero PCI configuration are supported. PCI
uses an exact byte write, preserving adjacent status/W1C fields. Unsupported
or malformed descriptors are diagnosed; 8042 polling is bounded and a
triple fault remains only a last resort. No laptop-specific reset address
is hardcoded. The format follows
[ACPI Reset Register](https://uefi.org/specs/ACPI/6.6/04_ACPI_Hardware_Specification.html#reset-register)
and the [FADT definition](https://uefi.org/specs/ACPI/6.6/05_ACPI_Software_Programming_Model.html#fixed-acpi-description-table-fadt).

Before reset, the selected method and `reboot-request` are checkpointed to
the current boot log and storage is flushed; a reported flush failure cancels
the request. The USB unsupported-flush durability limitation still applies.
Remote requests use the existing local-console queue, with rejection on a
busy/editing console, allocation failure or redirected input/output. This
keeps reset diagnostics visible locally and avoids SSH transport calls after
interrupts are disabled. A queued acknowledgement is not proof of reset:
local input racing the request can cancel it under the existing queue policy.

```sh
bash arch/x86/scripts/test-acpi-reset.sh build/acpi-reset-host
bash arch/x86/scripts/test-shell-remote.sh arch/x86/build/reboot-shell-host
OSITO_SSH_TEST_STORAGE=usb OSITO_SSH_TEST_REBOOT_ONLY=1 \
    bash arch/x86/scripts/test-ssh-qemu.sh arch/x86/build/acpi-reset-qemu
```

Host checks cover malformed/truncated FADT, all reset address spaces, MMIO
mapping failure, exact-width PCI writes and invalid/extended offsets,
IRQ/PCI-lock ownership, bounded busy/absent 8042 handling and queue admission,
cancellation and allocation failure. GCC/Clang normal and ASan/UBSan pass.
The new QEMU gate sends actual SSH `reboot` commands twice, rather than asking
QMP to reset the VM. It checks a new firmware boot, automatic authenticated
SSH return with pinned identity, and the previous boot's persisted method
and request marker. Pending UART bytes can be lost during reset, so serial
output alone is not the oracle for the final checkpoint.

The host suites passed 100,826 reset checks (including each bounded 8042 poll),
50 PCI byte/config checks, 170 PCI DMA regressions and 126 shell admission
checks in each of the four compiler/sanitizer variants. Evidence directories:
`acpi-reset-host-20260926`, `reboot-shell-host-20260926` and
`acpi-reset-qemu-20260926-r3` under `arch/x86/build/`. QEMU passed both actual
remote resets, using its advertised I/O register `0xcf9` and value `0x0f`,
with persisted checkpoints and automatic SSH return. Tested kernel:
7,150,888 bytes, SHA-256
`114552dafbda0ab78481cf48b968ddc32d7a3d0ed8c8df63dc2dcd495ecb8ede`.
On the P1, the SHA-verified upload completed, `kupdate --file
/kernel-reset-20260926.elf` installed and hotreloaded the kernel, and pinned
SSH returned at uptime 35 seconds. A repeat `--no-kexec` verification reported
`EFI kernel already current`; HDA/IPv4/SSH/WLAN were ready and the local console
was idle. The first corrected remote `reboot` acknowledged its local queue,
but SSH did not return at `192.168.1.20` during a two-minute retry window.
The user subsequently confirmed that the remote `reboot` alone displayed
Lenovo/UEFI and reached the normal shell, without touching the power button
or performing a manual restart. Pinned SSH was reachable again at the same
address; the recovered ring advanced from slot 2 to slot 3. Slot 3 selected
`CortesRojas-2.4G` on
channel 6, completed DHCP at tick 3216, and reached `sshd-ready` at tick 3220
(about 32.2 seconds of kernel uptime, not end-to-end reset latency). All four
managed services were ready and the local console was idle. Follow-up SSH
queries remained responsive through uptime 36m51s. Verification with
`kupdate --file /kernel-reset-20260926.elf --no-kexec` again reported verified
OsitoFS and an already-current EFI kernel after this firmware boot.

The preceding slot 2 contains `[SSH console] reboot` but ends at `shell-exec`
tick 10203, without the expected reset method or `reboot-request` checkpoint.
Together with the user's observation, this validates one unattended remote
reboot through firmware and the persistent kernel. It does not establish
which reset mechanism ran, explain the missing final checkpoint or earlier
SSH absence, or establish repeatable unattended reboot reliability. The
original freeze is not yet conclusively resolved.
Evidence is in `p1-dhcp-freeze-20260926/diag-after-acpi-reset/` and
`p1-dhcp-freeze-20260926/reset-*.log` under `arch/x86/build/`; the installed
kernel remains the tested hash above.

A second remote `reboot` was sent on 2026-09-26 at 19:39:30.149 local time
(UTC-03), after confirming uptime 59m08s and an idle console. The queued
request returned success. TCP/SSH initially timed out; the first successful
pinned SSH probe completed at 19:40:17.273, 47.1 seconds after the command,
with uptime 34.08 seconds. ICMP then returned 2/2 replies and the neighbor
entry matched the P1's MAC. The boot ring advanced from slot 3 to slot 0;
the new log selected the saved 2.4 GHz network on channel 6, completed DHCP
at tick 3374 and reached `sshd-ready` at tick 3379. HDA/IPv4/SSH/WLAN were
ready, the console was idle, and the retrieved current log contained no
page fault, general-protection fault or panic. This repeat demonstrates
automatic remote access returning after another fresh boot; the 47.1-second
measurement is a polling bound, not the exact time SSH first became available.
The user also confirmed seeing Lenovo/UEFI in this second attempt and returning
to the shell at the same IP without a manual restart. Follow-up authenticated
SSH checks at uptime 4m16s still reported all four services ready and an idle
console; the newly retrieved log contained no page fault or panic. These are
two observed unattended firmware boots, with measured automatic reconnection
on the second, not a claim that the diagnostic gap below is resolved.

The previous slot 3 again ends at `shell-exec` (tick 357229), following
`[SSH console] reboot`, with no persisted `reboot-request` or method. That
diagnostic gap is reproducible on the P1 even when reboot/reconnect completes;
do not infer a particular ACPI or fallback method from the successful return.
Evidence: `p1-dhcp-freeze-20260926/reset-physical-2*.log` and
`p1-dhcp-freeze-20260926/diag-after-acpi-reset-2/` under `arch/x86/build/`.

### Non-resetting diagnostics

`reboot --check` performs the same ACPI target preparation as `reboot` and
writes a `reboot-check` diagnostic checkpoint, but never writes the reset
register, uses the legacy reset fallbacks, or queues a console reboot.
It reports the selected register and whether the log checkpoint was accepted
by the storage API. An unavailable ACPI target or failed checkpoint returns a
nonzero SSH status. Unknown options and extra arguments are rejected without
resetting. This is an exclusive diagnostic command, not one of the read-only
queries permitted while a foreground program owns the local console.

`console launch reboot --check` queues exactly this diagnostic on the local
console, so it also exercises local framebuffer output and kernel logging
without needing a person to type it. The existing idle/editing/busy admission
rules apply. Bare `console launch reboot`, unknown flags and extra arguments
are rejected. Use `console status` to confirm that the request returned, then
retrieve `dmesg` or the persisted boot log for its result. Direct SSH
`reboot --check` instead runs under remote output capture; its checkpoint is
saved, but the selected-method output goes to SSH rather than the kernel log.

Real reboot now also checkpoints `reboot-enter` before table lookup, then
`reboot-request` after selecting the method. Checkpoint failures are reported
instead of silently ignored. This instrumentation does not change USB cache
policy or promise power-loss durability: the P1's test stick reports unknown
write-cache mode, no FUA, and unsupported `SYNCHRONIZE CACHE(10)`. A successful
checkpoint only confirms the existing block driver's best-effort contract.

Shell admission checks pass 138 cases in each GCC/Clang normal/ASan+UBSan run.
The ACPI/PCI suites remain green. USB QEMU checks `--check` without a new boot,
its saved checkpoint, invalid-option rejection, the queued local diagnostic
returning with its method saved, and two actual reboots with both
entry/request markers and SSH autostart. Evidence is in
`reboot-check-shell-20260926-r2`, `reboot-check-acpi-20260926-r1` and
`reboot-check-qemu-20260926-r2` under `arch/x86/build/`. Tested kernel:
7,150,968 bytes, SHA-256
`68d5e40f256d876a6a11a98977574402a4180ad6657ad659ec6789abd5d9bde0`.

On the P1, the first diagnostic revision (`d3e62ff28d3c18cd07cde3edb0b17ed6f5f7b2228b3ccb5cdbc25bca8c6c61cc`)
completed direct SSH `reboot --check`: the FADT selected System I/O `0xcf9`,
value `0x06`, and the checkpoint was accepted and readable in slot 1. A real
remote reboot requested at 20:49:39.573 local time returned pinned SSH at
20:50:34.919, a 55.3-second polling bound, with uptime 42.55 seconds. The new
slot 2 reached `sshd-ready` at tick 3940. Previous slot 1 now contains
`reboot-enter` at tick 9655, but still lacks the selected method and
`reboot-request`. Thus the new entry marker narrows the missing interval;
it does not establish whether the final checkpoint was reached or lost.

The user's locally typed check is preserved in that same cold boot's slot 2:
`reboot-check` at tick 53920 includes the FADT method, followed by
`Checkpoint accepted by storage`, `shell-exec-done` and the prompt. It survived
the subsequent `kexec`. After installing and verifying revision 2 in both
OsitoFS and EFI, the hotreload advanced the ring to slot 3. An SSH-queued
`console launch reboot --check` also returned to idle (request 1); its slot 3
checkpoint at tick 17256 contains the same method and successful return.
The local preparation and checkpoint path therefore worked both after a
firmware boot and after a hotreload. This does not prove that the final write
before a real reset is durable, or rule out an intermittent failure on that
path. No delay or USB cache-policy change was added on this evidence alone.

Physical evidence is under `arch/x86/build/p1-dhcp-freeze-20260926/`:
`reboot-check-*.log`, `diag-reboot-check-before/`, `diag-reboot-check-after/`,
`diag-reboot-check-local-r2/` and `diag-reboot-check-queued-local-r2/`. The
revision-2 install receipt verifies the persistent EFI kernel; the local
queued check and authenticated SSH verify the running hotloaded kernel.
