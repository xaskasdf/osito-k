# x86 USB storage diagnostics

## Real-hardware report, 2026-09-22

The ThinkPad P1 Gen 2 reached the desktop, but OsitoFS was not available and
`dmesg > dmesg.log` failed to create the file. The supplied log only covered
MEMSTAT and shell activity, not enumeration or mounting. The GUI terminal has
no scrollback, so repeating an unbounded `dmesg` is not sufficient diagnosis.

Read-only inspection of the returned test USB (serial `00000000CY02`) found:

- The expected keyboard-fix kernel and unchanged UEFI bootloader.
- A valid OsitoFS3 superblock and readable directory tree.
- A metadata-only sparse snapshot checked with `ositofs-fsck3`, without repair:
  15,923 regular files and 1,980 directories. The only two reported errors were
  stale superblock free-inode/free-block counters. File payloads were not
  copied or content-verified by this check.

The original physical failure is not yet attributed conclusively. None of
these checks writes to the USB, and this change does not reformat or manually
repair the data partition.

## Transfer and event contracts

`disk_read_bytes` and `disk_write_bytes` use physically contiguous 8-KiB USB
bounce buffers aligned to 4 KiB. Such a buffer can cross a physical 64-KiB
boundary. The old `bulk_xfer` described it with a single Normal TRB. xHCI
requires a chained TD instead. See Intel's
[xHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf),
sections 6.4.1, 4.11.2.4, and 4.10.1.1.2.

The bulk path now splits at those boundaries, calculates packet-based TD Size,
chains Link TRBs across a ring wrap, and publishes the first cycle bit last.
Completion matching includes the submitted TRB pointer, slot, and endpoint.
An early short-IN event records the byte count but does not complete the call
until the final IOC event arrives. Short OUT, malformed residues, and TDs too
large for the transfer ring are rejected. The API still requires physically
contiguous identity/direct-map buffers in that revision. The remapped-stack
correction below extends bulk transfers to translated physical segments.

A second bug was reproduced after opening the desktop: its poller could
consume disk completions while the disk waiter could discard HID completions.
The DMA event ring now has one guarded consumer shared by normal polling and
synchronous waiters. Non-HID completions are retained in a bounded controller
inbox and matched by their owners. Sequence numbers preserve event order even
when freed inbox slots are reused. A full/busy inbox leaves the DMA event in
place rather than dropping it. The initialization-only port-event purge runs
before endpoints and public polling are enabled.

HID decoding/callbacks run outside event-ring ownership. Each endpoint retains
its completed DMA report until dispatch ends and only then rearms the transfer.
This allows callback-driven polling without holding the event-ring guard.

## Diagnosis without a filesystem

After the boot mount attempt, `boot_diag_capture_storage` preserves up to 64
storage-related log lines in RAM, independently of persistent logs and later
klog wrap. Lines are bounded to 127 characters, with explicit truncation.

- `storage`: the last page, including the mount outcome.
- `storage 1`, `storage 2`, etc.: earlier pages, six lines per page.
- The header reports current filesystem availability and omitted old lines.

The snapshot is read-only diagnostic output. It does not retry discovery,
mount a device, or write a log file. It disappears on reboot.

## Regression tests

`bash arch/x86/scripts/test-xhci-poll.sh <fresh-output-directory>` builds the
production driver and boot-snapshot code with mocked MMIO/klog using GCC and
Clang, both normally and with ASan/UBSan. Coverage includes 1,116 bulk TD cases,
ring wrap, packet sizes 64/512/1024, early-short/final-IOC ordering, competing
desktop polls, interleaved HID/storage events, reentrant HID dispatch, control
and command completion matching, full-inbox backpressure, and bounded log
snapshot retention. The unmodified bulk path failed the 8-KiB boundary case.

These tests do not prove behavior on the P1's Intel controller. Multi-controller
device addressing, general control-transfer scatter/gather, BOT transaction
serialization/recovery, and cancellation of timed-out TDs remain separate
work. Do not describe this as complete xHCI or USB mass-storage support.

## Validated USB Kernel

The 6,721,192-byte kernel installed at 2026-09-22 03:57 UTC has SHA256
`7440ab26ba668341336574b5312b63ebb3bc7d10fa98b708d9373b1f7f0339f9`.
The final host results are in `arch/x86/build/xhci-shared-release/`.

Exact-binary QEMU runs under `arch/x86/build/realhw-usb-20260921/` passed:

- `smoke-usb-shared-ps2-final-v2`: UEFI USB boot, OsitoFS3 mount/read/write,
  persistent diagnostics, DOS probe, nine desktop keyboard cases, PS/2 mouse,
  and a USB file read after desktop focus changes. Serial SHA256:
  `65ad98caf90f0f66485ae4117993c9c1766307eca4d92b1d881b514e8fb73f87`.
- `smoke-usb-shared-usb-final-v2`: the same checks with USB HID and storage
  sharing xHCI. No bulk errors/timeouts or prerequisite-read failures appeared
  anywhere in either complete filesystem test log. Serial SHA256:
  `ae5210f2910a673a8c0e9eed6a69b287e7d9d2a1b9288a7f14aba86547395fc8`.
- `smoke-usb-shared-nofs-final-v2`: USB detection without a filesystem,
  desktop `storage` last/first pages, invalid-page handling, and a screenshot
  confirming the diagnostic fits without scrolling. File/DOS/persistent-log
  flags are intentionally false in this test. Serial SHA256:
  `539a9b687ad87a220bb3b8d684040842184ea7250e370c37d6e86bc9fc54909d`.

All three QEMU processes exited with status 0. The update retained the previous
keyboard kernel as `EFI/BOOT/KERNKEY.ELF`, as well as `KERNPS2.ELF` and all older
boot files. The EFI volume cache was flushed and file hashes verified. Both GPT
copies and all 33,751,040 bytes of OsitoFS3 fixed metadata matched before/after;
the raw device was opened read-only. Deployment evidence is in
`usb-storage-input/update-result.json`. Confirmation on the physical P1 remains
pending; use `storage` there if discovery or mounting still fails.

## P1 Unsupported Cache Flush

The subsequent P1 report identifies SYNCHRONIZE CACHE(10), CDB 0x35, current
sense key 0x05 / ASC 0x24 / ASCQ 0x00, followed by metadata recovery failure.
The new per-device policy logs unsupported flush once and continues without a
durability guarantee, as requested for this test pendrive. It does not suppress
media errors, timeouts, malformed CSWs, failed REQUEST SENSE, deferred sense,
or other ASC/ASCQ values. Cache/WP/FUA capabilities come from MODE SENSE; FUA
is not blindly forced. BOT phase/tag/residue and mandatory read lengths are
validated, with bounded endpoint STALL recovery in xHCI.

`test-usb-storage.sh` passed 1,178 checks with GCC/Clang and ASan/UBSan;
`test-xhci-poll.sh` also passed the new bulk-halt recovery cases. Evidence is
under `build/usb-cache-v3/` and `build/usb-cache-xhci-v1/`. The QEMU run
`smoke-usb-cache-normal` mounted OsitoFS3 but failed its literal README-output
assertion (duplicated character in serial output); it is not a passing full
USB smoke. That failed run remains recorded rather than being promoted to PASS.

The fix was subsequently deployed on 2026-09-22 at 14:38 UTC as part of the
[AX200 development USB update](x86-ax200-wlan.md#development-usb-deployment-2026-09-22).
The installed kernel is now 6,755,280 bytes, SHA256
`92057155ae76533c6748b3cd2d13179bca0a20fb06519a13fa164585121cdb2d`;
the preceding `7440ab26...339f9` kernel is preserved as `EFI/BOOT/KERNUSB.ELF`.
The bootloader and GPT are unchanged. Both EFI and OsitoFS3 kernel copies,
firmware and license passed physical readback verification.

`build/realhw-usb-20260921/wlan-deploy-20260922/qemu-v2/result.json` passes
exact-kernel UEFI USB boot, OsitoFS3 recovery/mount, firmware reads, persisted
diagnostics and byte-exact guest file-copy readback through xHCI. The sparse
test image contains the USB's metadata/directories and the deployment files,
not its unrelated application payloads; this is not a full application test.
The QEMU storage device supports cache flush. The unsupported-command branch
is covered by host injection tests, not by this QEMU run. Confirmation that
the real pendrive now mounts on the P1 remains pending.

## P1 Cache Probe Transport Failure

The next P1 trace reaches INQUIRY and READ CAPACITY, then reports a BOT
transport error during the optional cache probe and never registers `usb0`.
Only `nvme0` is registered when the filesystem scan starts. Its GPT and the
fallback superblock attempt are therefore on the internal NVMe, not evidence
that the pendrive lost OsitoFS3. Physical readback on 2026-09-22 at 16:33 UTC
confirmed that the USB fixed metadata still matched the preceding deployment
(`9cbcc47f...56a61`), and the installed EFI kernel still matched `92057155...cdb2d`.
The generic error did not identify the failing CDB, status stage or residue, so
the exact device response is not established by that trace alone.

A host regression reproduces the same initialization failure when MODE SENSE
returns fewer bytes than the allocation length but its otherwise valid CSW
reports residue zero. The prior driver treated this as lost BOT synchronization
and permanently blocked the device. The corrected transport:

- Bounds usable IN data by both the xHCI actual byte count and CSW residue.
  Understated residue produces a once-per-device warning, not invented bytes.
  Variable-length replies can be parsed; mandatory INQUIRY, READ CAPACITY and
  sector reads still fail when incomplete. Such a short command with a valid
  CSW does not by itself lose transport synchronization.
- Permits one surplus zero-length packet before CSW, and one clearable CSW
  STALL, with at most three status reads total. Repeated ZLP/STALL still fails.
- Recognizes a CSW returned instead of the data phase. Signature, tag, status,
  residue and missing-data checks remain enforced; a failure still triggers
  REQUEST SENSE. This does not accept a successful command with missing data.
- Logs CDB, phase, transfer result, requested/received/CSW byte counts and CSW
  fields on failure. Invalid wrappers, phase errors, failed halt clearing and
  transport timeouts still fail closed; no write is automatically replayed.

The distinctions between data, status and reset follow the
[USB-IF BOT specification](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
sections 5.3 and 6.3-6.7. Handling an extra ZLP, an early CSW and reconciling
residue with actual transfers was cross-checked against
[Linux v6.12 USB storage transport](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/storage/transport.c).
There is no vendor/product allowlist or unconditional MODE SENSE bypass.
Full BOT reset/re-enumeration, transaction serialization and safe cancellation
of timed-out DMA remain separate work; this patch does not implement them.

Validation:

- `build/usb-bot-short-before/`: the old production driver fails 13 checks with
  the initial new regression fixture; the failed run is retained.
- `build/usb-bot-short-v2/`: 16,905 checks per GCC/Clang normal/sanitized run,
  all passing. Includes all 512 undersized READ(10) lengths, mandatory reply
  truncation, early-CSW wrong tag/missing data, repeated ZLP/STALL, failed halt
  clearing and existing media/flush/FUA error cases.
- `build/realhw-usb-20260921/smoke-usb-bot-short-v1/`: exact-kernel UEFI USB
  boot, mount/read/write, storage snapshot, persistent diagnostics and a DOS
  probe passed with the complete original test payload.
- `build/realhw-usb-20260921/bot-short-deploy-20260922/qemu-v2/`: the prepared
  USB metadata and deployment files passed boot, firmware inspection, honest
  no-radio rejection/cleanup and byte-exact guest copy/persistent-log readback.
  QEMU's compliant storage device does not reproduce the P1's cache-probe error.

At 16:37 UTC the new 6,755,280-byte kernel, SHA256
`5c2d9f6b9050414a8866568ec1e43b7bad07f48c5dd1f133edb7cd4f53eff7cb`,
was installed in both EFI and OsitoFS3 on development USB `00000000CY02`.
`EFI/BOOT/KERNBOT.ELF` preserves the preceding `92057155...cdb2d` kernel;
older backups, bootloader and GPT remain unchanged. Only `/kernel.elf` and
`/WLANTEST.txt` changed in OsitoFS3: 107 backed-up/verified blocks, no new inode
or block allocations, and no directory edits. Firmware and license readback
hashes remain unchanged. Both offline fscks retain the same two preexisting
free-counter mismatches, not new structural errors. Deployment evidence and
rollback blocks are in `bot-short-deploy-20260922/physical-update/`.
All managed scratch images were removed after QEMU exited. P1 validation of
this kernel remains pending; use `storage` to collect any remaining failure.

## P1 Readback: Mount and Persistence Confirmed

On 2026-09-22 at 17:01 UTC the same development USB was read without writing
to either partition. Its EFI kernel still matches `5c2d9f6b...eff7cb` and the
bootloader remains `feec6ce0...94038`. Evidence is retained under
`build/realhw-usb-20260921/hardware-readback-20260922-170154/`.

The P1's `root-logs/dmesg.log` records the once-per-device short-IN/residue
warning, then successful USB storage registration. The USB is identified as
`058f:6387`, `iOS devi Y08`. The filesystem scan first rejects the internal
NVMe, then finds partition 2, `osito`, on the USB at LBA `0x00100800`.
OsitoFS3 repairs its free counters with zero orphans and mounts `OsitoK-USB`.
The pendrive rejects SYNCHRONIZE CACHE(10) with sense `05/24/00`; the driver
records the missing flush guarantee and continues, as intended.

The readback contains `diag/boot0.log`, `diag/latest.txt`, `wlan-probe.log`,
`wlan-fwboot.log` and the 400,788-byte `dmesg.log`. Read-only `ositofs-fsck3`
on the extracted metadata/directory snapshot exits 0: 15,932 files, 1,980
directories, 455,817 free blocks, 113,159 free inodes, **zero errors**. The
snapshot omits unrelated regular-file payloads; this is a metadata consistency
check and evidence of persisted diagnostic files, not a full payload scrub or
proof of power-loss durability on a device that rejects flush.

The current blocker is now the AX200 firmware/DMA startup, not mounting or
reading the firmware file. See [physical WLAN evidence](x86-ax200-wlan.md#p1-physical-firmware-attempt).

## Concurrent Firmware Reads And Diagnostic Writes, 2026-09-25

After enabling background driver initialization, the P1 reported failures in
both automatic and manual firmware loading after `kexec`. Read-only inspection
of development USB `00000000CY02` found the AX200 firmware and Intel license
intact, matching their pinned SHA-256 hashes. The latest persisted checkpoint
records a successful OsitoFS3 mount but precedes the reported read failure.
There is no captured failing BOT transaction from that physical attempt.

The new worker exposed an existing ownership gap: OsitoFS3 can release its
metadata lock before file data I/O, allowing firmware reads and persistent
diagnostic writes to overlap. xHCI transfer ownership did not serialize the
complete storage transaction. Interleaved CBW/data/CSW sequences could mark the
transport failed, also preventing subsequent manual reads.

USB storage now holds an IRQ-save SMP lock across public initialization,
read, write and flush operations, including REQUEST SENSE, FUA fallback and
cache bookkeeping. Diagnostic summary/reset and readiness checks share that
owner. The prior interrupt state is restored on every public return. Internal
locked helpers avoid recursive ownership. This uses the existing polled block
driver model; it is not an asynchronous USB request scheduler. Full BOT reset
recovery and cancellation of timed-out DMA remain separate work.

Validation artifacts:

- `build/usb-bot-owner-20260925/`: 27,431 checks with GCC/Clang, normal and
  ASan/UBSan, all passing. Four concurrent threads read, write, flush and reset
  diagnostics while the mock yields between BOT phases. Coverage includes
  REQUEST SENSE, unsupported FUA/flush and interrupt-state restoration.
- The same fixture with inter-core ownership removed fails 219 checks and
  reproduces transport failure. This demonstrates the code defect, not the
  exact physical failure sequence absent from the saved log.
- `build/drivers-usb-qemu-20260925/`: all 20 SSH integration gates passed
  using xHCI USB storage and virtio-net. Includes filesystem operations,
  compressed screenshots, cold-boot SSH autostart and remote `kexec` with
  automatic reconnection, unchanged host identity and persistent files.

The exact tested kernel is 7,152,832 bytes, SHA-256
`7b798fdb1cfecedb1a7d2b3d6983a93fd66240105b62adfc7e72556523199841`.
The USB mode is selected with `OSITO_SSH_TEST_STORAGE=usb` when running
`arch/x86/scripts/test-ssh-qemu.sh`; its default remains NVMe. Physical AX200
autostart/reconnection still needs validation on the P1.

At 20:42 UTC the tested kernel was installed as `EFI/BOOT/kernel.elf` on USB
`00000000CY02`. Readback matched `7b798fdb...99841`; the previous kernel,
SHA-256 `e989d9d2d1effc03d4f503508d2338a399d15c3066277d523ac69887f4ff1dc9`,
was preserved as `EFI/BOOT/DRVSAFE.ELF` and in the host deployment directory.
The EFI volume cache was flushed. Bootloader, existing boot files, both GPT
copies and all 33,751,040 bytes of fixed OsitoFS3 metadata matched before and
after. The raw device was opened read-only; only EFI files were updated.
Evidence is in `build/p1-drivers-20260925/usb-update/update-result.json`.

Boot the P1 from USB UEFI to use this kernel. The earlier OsitoFS3
`/kernel-drivers.elf` was not replaced and must not be reused for this test.
No saved WLAN profile was present at inspection: one successful `wlan connect`
and `sshd autostart on` are still needed before testing automatic reconnection.

## Remapped Win32 Stack DMA, 2026-09-26

Launching Steam PE64 on the P1 stopped xHCI with `USBSTS=0x15`, followed by
`READ(10)` (`0x28`) failing at the CBW stage, registry read/write failures and
Steam exiting with `0xffffff97`. The complete volatile log was recovered over
SSH as `arch/x86/build/p1-steam-failed-dmesg-20260926.log`; the later snapshot
is `p1-xhci-dead-dmesg-20260926.log`. This is not the earlier unsupported
SYNCHRONIZE CACHE command: HCH/HSE report a stopped controller with a host
system error. See the Intel xHCI specification linked above, sections 5.4.2
and 4.10.2.6; the status alone does not identify the offending DMA address.

The code has a reproducible DMA translation defect on this path. The BOT
CBW/CSW live on the synchronous caller's stack. PE64 shims can execute on a
private mapped stack (this launch recorded VA `0x4000930000`), but bulk
transfers used `kvirt_to_phys`, which only handles identity/direct-map
arithmetic. A low-half private VA was therefore submitted unchanged as a
physical address. The small-object alignment did not make it identity-mapped.

Bulk TD preparation now snapshots the active CR3 and translates each page
with the existing paging walker. Adjacent physical spans are coalesced up to
the 64-KiB boundary; discontinuities become chained Normal TRBs. The full
range is checked before touching the DMA ring or doorbell. Unmapped,
noncanonical, overflowing or overly fragmented ranges are rejected rather
than submitted. First-cycle-last publication, short-IN matching and ring-wrap
handling remain unchanged. No app, vendor or fixed stack address is special
cased. Synchronous callers must retain their buffers and mappings; this is
not an asynchronous page-pinning or IOMMU API.

EP0's single Data TRB also uses a translated physical span. It rejects ranges
needing multiple physical segments; general control-transfer scatter/gather
and its existing ring-wrap limitations remain separate work. Bulk waiters
return immediately when polling detects a stopped controller, and do not
submit subsequent work to it. This does not reset the controller or replay
writes after a fatal error. Unsupported-flush policy is unchanged.

Validation artifacts under `arch/x86/build/`:

- `xhci-dma-before-20260926/`: new fixture against the old driver fails 24
  assertions, including private-stack addresses and pre-submission rejection.
- `xhci-dma-after-20260926-r2/`: GCC/Clang normal and ASan/UBSan all pass.
  Includes the existing 1,116 bulk TD cases, remapped small CBW-size buffers,
  discontiguous pages, short IN across segments, upper-half mappings, invalid
  ranges, EP0 mapping, stopped-controller rejection and event/HID regressions.
- `xhci-dma-usb-bot-20260926/`: 27,431 BOT checks per compiler/sanitizer variant,
  all passing, including concurrency, sense, residue and flush policy.
- `xhci-dma-qemu-20260926-r1/`: exact-kernel USB SSH integration passes file
  I/O/delta replacement, captures, cold boot and remote kexec with automatic
  reconnection. This run does not launch Steam.
- `xhci-dma-pe64-qemu-20260926/`: a no-CRT PE64 probe uses its private Win32
  stack above 4 GiB for a 17-KiB write/read round trip through the real shims,
  OsitoFS3 and xHCI USB storage. All bytes match, the process exits with zero,
  and a subsequent authenticated SSH command succeeds.
- `xhci-dma-pe64-before-qemu-20260926/`: the same probe against the previous
  kernel (`68d5e40f...bde0`) fails at the first filesystem operation. Its
  31-byte CBW gets xHCI completion code 6, BOT becomes unusable, and the probe
  reports `USB-STACK64 FAIL`. This is an intentional negative baseline, not
  a passing run. QEMU's STALL and the P1's HSE differ, but both fail on the
  private-stack CBW path; the P1's exact rejected DMA address remains unknown.

Run the focused guest regression with
`OSITO_SSH_TEST_STORAGE=usb OSITO_SSH_TEST_USB_STACK=1 bash arch/x86/scripts/test-ssh-qemu.sh <fresh-output-dir>`.
The runner builds `test/usb_stack_pe64.c` with Clang/lld and stages it in an
isolated managed test disk. Normal SSH integration remains a separate run.

Tested kernel: 7,155,104 bytes, SHA-256
`8c9272720200baf3243c793de538c6a52305bbaa0af14e520566adf1df9a9d4d`.
The P1 was manually restarted after preserving its log to restore the stopped
controller. This exact candidate was uploaded with SHA-256 verification and
installed with `kupdate --file /kernel-xhci-dma-20260926.elf --no-kexec`.
Readback verified both the OsitoFS kernel and `usb0:EFI/BOOT/kernel.elf`, with
the previous EFI image retained as `/kernel.previous.elf`. Remote `kexec`
then returned to SSH at an uptime of 40 seconds. Deployment evidence is in
`p1-xhci-dma-upload-20260926.log`, `p1-xhci-dma-install-20260926.log` and
`p1-xhci-dma-reconnected-20260926.log`.

The physical Steam retest advanced beyond the old failure: it copied
2,963,096 bytes of SteamService successfully, verified Authenticode, saved
the registry and scheduled `SteamServiceTmp.exe /Update`. The last recovered
tail is `p1-steam-dma-retest-tail-20260926.log`. Before a screenshot could be
retrieved, the P1 stopped answering ping and SSH. That tail has no controller
fatal error, but the subsequent log is unavailable. The cause of the new
loss of connectivity is not established; Steam startup is not yet validated
end to end on physical hardware. Preserve the local screen/fault evidence
before another reset.

## Bounded USB Read Batches, 2026-10-02

The physical profiler in section 4.197 of `win32-layer-correctness.md`
finds timer samples concentrated immediately after the USB read IRQ-restore
boundary, with 99.48% BSP C0 residency during Steam startup. The timer cannot
sample inside the IRQ-masked transaction; those sample percentages are not
wall-time accounting. Inspection also finds an independent batching limit:
`disk_read_bytes` forces USB reads through an 8 KiB bounce buffer even when
the filesystem requests a full 64 KiB block and the device supports it.

The byte-read layer now sizes USB bounce storage from 8 KiB to at most 64 KiB,
in powers of two according to the requested span and initial sector offset.
It retains physically contiguous direct-map DMA storage, never submits a
caller/user buffer directly, and frees the exact number of allocated pages.
Small requests still allocate 8 KiB. Failure to allocate a larger buffer
falls back once to the old 8 KiB size; failed I/O is not retried. Each rounded
sector transfer fits in the allocation. Partial boundaries and a failure
after earlier successful chunks preserve the existing byte-read contract.
NVMe's aligned direct path, other devices' 8 KiB buffers, writes, flushes,
BOT ownership and interrupt policy are not changed.

This deliberately does not enable scheduling inside the current BOT owner:
its synchronous DMA buffers may belong to a private Win32 stack, and task
termination can discard that stack/address space. An interruptible request
path needs explicit buffer lifetime and cancellation ownership before the
whole-transaction IRQ mask can be removed. Batching reduces command overhead
but is not that concurrency fix, nor a bound on individual IRQ-off latency.

Validation artifacts under `arch/x86/build/`:

- `usb-read-batch-before-20261002-r1/`: the new byte-layer fixture against
  the previous source fails 27 assertions for the larger batch/fallback and
  the expected successful prefix before a second read failure. This is the
  retained negative control, not a passing test.
- `usb-read-batch-after-20261002-r1/`: GCC/Clang normal and ASan/UBSan each
  pass 16,733 assertions. Coverage includes 512/4096/8192-byte sectors,
  unaligned and end-of-device reads, zero length, guards, allocation failure,
  read failure, exact frees and unchanged NVMe/other-device/write behavior.
- `usb-read-batch-xhci-20261002-r1/`: all four driver fixture variants pass,
  including 1,116 bulk TD cases, 64 KiB boundaries, ring wrap, remapped stacks,
  short completions, shared event ownership and halted-controller handling.
  `usb-read-batch-bot-20261002-r1/` passes 31,491 BOT assertions per variant.
- `usb-read-fixture-20261002-r1/`: the new no-CRT public Win32 fixture passes
  on native Windows x86/x64. It creates/reuses a named 2 MiB test file,
  validates every byte for three rounds of 8/64 KiB reads, then checks EOF,
  an unaligned large read and a short final read. QPC times only ReadFile
  calls; pattern generation and verification are outside the timed interval.
- `usb-read-batch-qemu-20261002-r1/`: both ABIs pass the same file fixture
  through OsitoFS3 and xHCI USB storage, with SSH responding after each exit.
  `usb-read-batch-ssh-20261002-r1/` passes all 28 full SSH integration gates
  on USB, including binary sync, capture, busy-console operations, cold boot
  and kexec reconnect. Build, Python compilation and shell syntax checks pass.

The physical comparison uses the same PE64 fixture before and after updating,
with Steam stopped cooperatively and the desktop running in both cases:

| Request size | Previous kernel, ms per 2 MiB | Batched kernel, ms per 2 MiB |
| --- | --- | --- |
| 8 KiB | 176.772 / 192.810 / 178.624 | 147.272 / 147.533 / 147.309 |
| 64 KiB | 173.309 / 175.307 / 173.205 | 89.285 / 89.605 / 96.831 |

All bytes and boundary checks pass. The 64 KiB median before/after ratio is
about 1.93, but the 8 KiB control also improves across the reboot, so this
must not be claimed as a completely isolated 1.93x batching gain. Within the
new boot, 64 KiB requests take about 39% less time than 8 KiB requests; in
the previous boot their medians were nearly equal. New-kernel PE32 also
passes all six rounds and final checks on the P1. Raw QPC results are in
`p1-baseline64.log`, `p1-batched64.log` and `p1-batched32.log` in the before/
after directories; `p1-fixtures-dmesg.log` identifies the actual ABI launches.

The tested 7,342,648-byte kernel has SHA-256
`138ec3830d1c9dbb34f8573a8871d538f32a33a67bb3a91220d63735b2cd629b`.
Verified sync and `kupdate --file /kernel-usb-read-batch-20261002.elf --no-kexec`
update OsitoFS and EFI usb0, retaining `/kernel.previous.elf`. Hotreload
returns to automatic Wi-Fi/DHCP/pinned SSH; a physical cold boot is not tested.
Deployment output is retained in the QEMU artifact directory's `p1-kupdate.log`.

Natural Steam request 4 remains open after the two physical fixtures. The
new `p1-usb-batch-steam-start-20261002-r1/` profile still has 1923/1946 samples
at the USB read restore boundary and 99.48% C0. The later profile has
1318/1868 samples there, with user execution and more loaded helper tasks.
The phases are not precisely time-aligned with the old run, so this is not
an application launch-time benchmark. The authenticated 1920x1080 capture
shows the complete login/QR, not a library. Current logs are preserved in
`p1-usb-batch-steam-login-20261002-r1/`; no fatal/controller/BOT timeout marker
appears in the saved dmesg. Post-login qualification awaits local sign-in.
Profiling is stopped. IRQ-masked USB waits and Steam's earlier Encryption
Failure remain open; neither is claimed fixed by larger read batches.

## Direct USB Wait Accounting, 2026-10-02

`usb-perf` is a read-only, cumulative snapshot, admitted over SSH even while
the local console runs Win32. It does not reset counters, issue disk I/O or
flush a device. The BOT owner protects a bounded snapshot; output occurs
after releasing ownership and restoring the caller's interrupt state.

Serialized TSC reads measure public init/read/write/flush calls. `wait_ticks`
starts after CLI and ends at ownership acquisition; `hold_ticks` covers the
operation under that owner, excluding the final accounting/IRQ-restore and
scheduler checkpoint. `entry_irq_off` identifies callers already inside an
outer IRQ-masked region. This is elapsed reference-clock time, not CPU work
or the complete outer filesystem critical section. CBW/data/CSW phase times
include the synchronous xHCI call and are subsets of operation hold time;
do not add them to that time. Stall retries are separate phase calls.

Counts include failed and no-op API calls. `ok_bytes` at operation level
counts only wholly successful reads/writes; phase byte counts describe
transport completion and are not proof of SCSI or file-read success. Size
buckets are upper bounds at 512/4096/8192/16384/32768/65536 bytes and a final
overflow bucket. `bad_clock` excludes backward intervals from timing totals.
Raw TSC totals and the current calibration allow interval subtraction on the
host without overflow-prone microsecond scaling in the I/O path. Maximums
are since boot, not interval maximums. A zero frequency must not be scaled.
The existing synchronous ownership, timeouts, errors and cache policy are
unchanged; this is instrumentation, not interruptible I/O.

Validation in `arch/x86/build/`:

- `usb-perf-host-20261002-r1/`: 32,579 checks per GCC/Clang normal and
  ASan/UBSan build. Includes exact clock arithmetic, two-command reads,
  partial/failing reads, skipped flush, size boundaries, backward clocks,
  cumulative/non-I/O reporting, restored output context and concurrent owners.
- `usb-perf-shell-20261002-r1/`: 176 admission checks per four variants;
  only the exact read-only command is independently admitted, not `reset`.
- `usb-perf-qemu-20261002-r2/`: nine gates, both public Win32 ABIs reading
  and verifying all bytes through real QEMU xHCI/OsitoFS3; counters account
  for both workloads and SSH survives. The earlier `r1` run stopped on a
  host parser evaluation-order bug, before the fixture, not a kernel fault.
- `usb-perf-busy-20261002-r1/`: 16 gates, including metrics during PE32/PE64
  foreground execution, hardware profiling, capture and cooperative stop.
- `usb-perf-ssh-20261002-r1/`: all 28 full SSH integration gates on USB,
  including cold boot, kexec, file sync and busy-console routing. Clang kernel
  build, Python compilation and scoped diff checks also pass.

The frozen kernel in the `r2` directory is 7,346,936 bytes, SHA-256
`a86d4d9302feb7c00167962781effaad7031ab04d78e6ab0e6dea609d5ac42b2`.
On the P1, request 4 of the previous boot returned after `console stop 4`.
Verified binary sync and `kupdate --file /kernel-usb-perf-20261002.elf --no-kexec`
updated OsitoFS and EFI usb0, preserving the previous EFI kernel. Deployment
logs are in the `r2` directory. After `kexec`, automatic pinned SSH returned
at an observed uptime of 49 seconds. Physical cold boot remains untested.

### Physical Startup Measurement

Idle measurements in `p1-usb-perf-idle-20261002-r1/` show zero USB calls over
3.30 seconds. Then desktop request 1 returned and Steam request 2 launched
without arguments. `p1-usb-perf-start-20261002-r1/` records three consecutive
intervals, without concurrent log downloads or screenshots:

| Interval seconds | Read calls | Read bytes | Read hold seconds | Total USB owner hold |
| --- | --- | --- | --- | --- |
| 20.914 | 46,819 | 103,233,024 | 17.810 | 85.57% |
| 20.292 | 49,928 | 102,252,544 | 18.599 | 91.66% |
| 20.328 | 50,034 | 102,459,392 | 18.632 | 91.66% |

The middle interval has only reads, all in the 513-4096-byte bucket, averaging
exactly 2048 bytes per call. This aggregate proves small-request pressure,
not an individually traced 2048-byte length for every call. It spends only
1.238 ms acquiring ownership, versus 18.599 s holding it: lock contention is
not the measured cause. The data phase takes 16.145 s, CSW 2.173 s and CBW
0.267 s. There are no failed operations, failed phases or bad-clock samples.
Every read in the second and third intervals enters with IRQs enabled, so
the USB driver itself masks the measured hold time, not an incoming FS lock.

A verified binary pull of `bootstrap_log.txt` locates executable checksum
verification from 02:59:47 to 03:01:32 (105 seconds) in this launch. The initial
shell `tail` capture returned unusable binary-looking output; use the retained
`bootstrap-raw.log` copy for this evidence. The small-read burst temporally
coincides with verification; the exact native call site/file is not traced.
`osfs3_read_file` snapshots cached metadata then reads requested data directly
from storage, and NtReadFile supplies its requested length to that path.
Larger byte-layer bounce buffers cannot combine separate small API calls.

The later `p1-usb-perf-later-20261002-r1/` interval has only eight reads and
eight writes over 20.426 seconds: 20.422 ms owner hold, or 0.100%. Its capture
shows the login form, not the library. Thus the startup bottleneck must not
be used to explain all subsequent network/login failures. The prior boot's
`Loading user data` capture and saved Encryption Failure reconnect records
remain in the `p1-usb-wait-live` and `p1-usb-batch-live` artifact directories.
After this capture, a connection-log pull and a bounded SSH retry could not
connect to TCP 22; ping then timed out/reported destination unreachable from
the development PC. No remote reboot was sent. Request 2 running at the login
is the last confirmed state, not proof that the P1 remains responsive now.

Next work is coherent buffered read caching/read-ahead for small file reads:
preserve offsets, EOF, revisions, writes/truncation visibility, no-buffering
semantics, bounded memory and owner teardown. Do not bypass Steam's checksum
verification. Interruptible USB waits additionally require DMA lifetime and
cancellation ownership; enabling preemption under the current BOT/FS locks
alone is unsafe. Neither change is implemented by this diagnostic patch.

## Buffered NT Read Windows, 2026-10-02

The next patch addresses the measured small-read pressure at `NtReadFile`,
not with a Steam-specific verification bypass. `win32/filecache.c` keeps up
to 32 immutable 64 KiB windows (2 MiB of fixed kernel storage plus metadata).
Each live open has at most one window. A non-recycled open cookie, backing
file identity, revision, file size and byte range identify cached data.
Duplicate handles share the open; last close, including normal process handle
cleanup, invalidates its cookie. No cache entry owns a process stack, dynamic
allocation, in-progress disk request or loading reservation.

Ordinary reads learn consecutive offsets; the second consecutive request can
fill an aligned window, bounded by EOF. Explicit sequential opens can prefetch
immediately. Random-access opens cache only the actual requested range; no-
buffering opens bypass the data cache. `CreateFileA/W` now preserve all three
hints in NT create options, instead of silently discarding them. This is not
a new implementation of unbuffered alignment validation or asynchronous I/O.

The existing kernel staging buffer is enlarged only on eligible misses. If
that allocation fails, the requested-size allocation is tried. Speculative
read failure falls back to an ordinary read of the original requested range.
Short/failed reads and odd/changing revisions are not published. Revisions
are checked after cache copies and fills; writes/truncations from another
open invalidate old contents without adding callbacks to filesystem writers.
Every caller still passes the existing byte-range guard, EOF/offset handling,
position update and private-CR3 user-buffer copy in `NtReadFile`.

Cache locks protect only bounded metadata/memory copies, never backend I/O.
USB serialization, DMA lifetime, flush behavior and IRQ-masked BOT waits are
unchanged. The existing temporary I/O buffer lifecycle is retained; this does
not establish arbitrary thread cancellation safety for the rest of NT I/O.

Validation artifacts under `arch/x86/build/`:

- `filecache-host-20261002-r6/`: GCC/Clang normal and ASan/UBSan, each 557,773
  checks and zero failures. Covers sequential/random/no-buffering behavior,
  bounded eviction, close/reuse and late old-cookie publication, mutations
  during fill/hit, truncation/growth, size/overflow boundaries, partial/errors,
  requested-size fallback, IRQ restoration and eight concurrent reader threads.
  The prior `r4` single-thread harness passed; `r5` failed compilation because
  host includes searched freestanding headers. `r6` uses `-iquote` correctly.
- `filecache-lock-host-20261002-r1/`: 18,550 checks per four build variants.
- `filecache-qemu-20261002-r2/`: nine gates with QEMU xHCI and OsitoFS3, both
  Win32 ABIs, all bytes verified. Includes a peer-handle write, cached-range
  lock denial, truncate/grow/zero-fill, seek/EOF, duplicate close and process
  exit with an open file. `r1` also passed the workload; `r2` fixes a host-only
  results-dictionary naming collision in the new runner branch.
- `filecache-negative-20261002-r1/`: same fixture and USB path against the
  previous frozen `usb-perf-qemu-20261002-r2/kernel.elf`, nine gates. It retains
  one backend read per small request; byte/position/lock contracts still pass.
- Both fixture executables also pass all seven modes on native Windows; logs
  are in `filecache-qemu-20261002-r2/file-cache/native-*.log`.
- `filecache-position-20261002-r1/`: nine gates, including public PE32/PE64
  overlapped/explicit offsets and the 63-check in-kernel NT file contract.
- `filecache-ssh-20261002-r1/`: all 28 full USB SSH gates, including binary
  delta sync, busy-console diagnostics/capture, cold boot and automatic kexec
  reconnection. Clang kernel build, Python compilation and diff checks pass.

For a 2 MiB file read in 2048-byte requests, complete-launch USB read counts
are as follows. Counts include loading the fixture and unrelated metadata,
not only the 1024 application requests:

| Mode | Previous PE32 / PE64 | New PE32 / PE64 |
| --- | --- | --- |
| Unbuffered | 1068 / 1068 | 1068 / 1068 |
| Random access | 1068 / 1069 | 1068 / 1069 |
| Default buffered | 1068 / 1068 | 77 / 77 |
| Wide-path buffered | 1068 / 1068 | 77 / 77 |
| Buffered, exit without explicit close | 1068 / 1068 | 77 / 77 |

Within the new-kernel run, USB read hold time falls from 342.632 to 25.568 ms
for PE32 and from 345.610 to 26.213 ms for PE64 between the unbuffered and
default-buffered modes. These are TSC-scaled USB-owner intervals including
fixture loading; they exclude cache-copy lock time and are not total launch
times. They are QEMU measurements, not estimates for the physical pendrive.

This is about 92.8% fewer USB read operations in the controlled fixture, not
a claim of 13.9x Steam speed or a physical launch-time improvement. The frozen
new ELF is 7,351,400 bytes, SHA-256
`d0d0d7cb83d57f5a1db1ce3c8fd9bad9b97d3c9ec57da67de66ce3b8bb3d3c81`.
It is not installed on the P1 yet: TCP 22 still times out. The user reported
a breakpoint after opening a Steam link; its address and module are not yet
available. No reboot was sent. Post-login Encryption Failure, SSH loss, actual
Steam cache effectiveness and physical IRQ-off latency still need retesting.

### Physical Buffered-Read Deployment and Startup

After the user restarted the P1, `console status` reported idle/request 0.
Authenticated binary sync transferred the combined cache/TCP-checksum kernel
from `tcp-checksum-ssh-qemu-20261002-r1/kernel.elf`: 7,351,448 bytes, SHA-256
`dca976418625b3043878081a5f6a9e9f7b1aebab24e6a7446c24dceb606d8587`.
`kupdate --file /kernel-cache-tcp-20261002.elf --no-kexec` verified the OsitoFS
and EFI usb0 copies. The kexec session timed out during disconnection, then
pinned SSH returned automatically with uptime 78 seconds. Saved configuration
selected channel 6 on CortesRojas-2.4G, DHCP and sshd both became ready. The
logs are in `build/p1-filecache-tcp-20261002-r1/`. This validates hotreload
and persistent payload readback, not a physical cold boot of this image.

Steam again launched without arguments as console request 2 after desktop
request 1 returned. Five consecutive intervals in
`p1-filecache-steam-start-20261002-r1/` exclude concurrent physical screenshot
and log downloads (a separate QEMU diagnosis ran on the development host):

| Interval seconds | Read calls | Read bytes | Read hold seconds | Total USB owner hold |
| --- | --- | --- | --- | --- |
| 21.093 | 6,505 | 413,981,696 | 17.267 | 82.26% |
| 20.586 | 6,348 | 390,849,536 | 16.639 | 84.78% |
| 21.355 | 4,824 | 197,468,160 | 10.135 | 63.59% |
| 20.484 | 2,557 | 91,671,552 | 5.019 | 30.22% |
| 20.697 | 3,167 | 166,744,064 | 7.520 | 40.36% |

The first interval has 6,311 reads in the 32769-65536-byte bucket instead of
the previous mostly 2048-byte requests. All intervals have zero USB failures
or bad-clock samples. The current bootstrap log records executable checksum
verification from 04:25:50 to 04:26:15: 25 seconds versus the earlier 105,
with the same installed Steam build 1788652215 and no verification bypass.
That phase is 4.2 times faster in these physical runs; this is not a 4.2x
claim for total startup, interactive rendering or network throughput.

The 1920x1080 screenshot at uptime 313 seconds shows the complete login form
and QR. SSH remains responsive after the measured launch. A connection-log
binary pull rejected a source that changed during transfer; it is not counted
as a verified log snapshot. After local sign-in, `screen-after-signin.png`
shows `Loading user data...`, not the library. A single revision-checked 16 KiB
tail read of the 3,999,186-byte connection log succeeds; whole-file pulls had
rejected concurrent changes, and shell tail's full scan timed out. The saved
tail records logon OK at 04:43:40 and 04:45:59, followed by `Encryption Failure`
at 04:45:43 and 04:47:45, then another OK at 04:48:10. These are current-run
failures, not invalid initial credentials. Read batching does not remove the synchronous BOT
IRQ-off intervals: the cumulative longest write reached 241 ms in this run.
Post-login encryption failures, sustained SSH/network stability and the
help-link breakpoint are still open. `dmesg-after-signin.log` has no fatal
exception or bad-TCP-checksum drop; its thread-name exceptions are consumed
compatibility notifications. Short SSH diagnostics still succeed despite the
single long tail command timing out.

A separate 20-second BSP profile in `p1-filecache-kprof-20261002-r1/` has 1,392
samples, 1,037 in `gui_fill_rect_alpha`, 235 in `display_wait_vblank` and only
four in USB reads. BSP C0 snapshots are 95.62% and 94.50%, with no profiler
drops. This identifies repeated desktop shadow blending after the I/O-heavy
startup phase, not the encryption error's cause. See section 4.202 of
`win32-layer-correctness.md` for the bounded shadow optimization and its tests.
