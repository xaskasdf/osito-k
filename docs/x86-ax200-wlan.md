# AX200 WLAN: native diagnostic bring-up

## Scope (2026-09-24)

Latest physical result, 04:28 UTC: the P1 at `169.254.167.93` responds to
4/4 ICMP echo requests (3-11 ms) and returns an exact UDP 7777 echo token
over Wi-Fi through the repeater to the development PC's Ethernet port.
DHCP failed according to the user; APIPA was applied manually. Local IPv4
data is now physically verified, but DHCP, Internet access and SSH are not.

Current ESP deployment: temporary password-echo kernel `f8acc7da...`, verified
at 03:41 UTC. It retains native `wlan selftest` and bounded pre-WPA2 RX/EAPOL
TX traces from `f5caddec...`. The P1 readback of that earlier diagnostic kernel
at 03:16 UTC confirms native KAT 6/6 both before and after connection,
six M2 firmware SUCCESS completions (2-3 ms, no retry failures), but no
authorized port or accepted M3. ARP broadcast traffic evicted the earlier
RX samples; this diagnostic limitation is not evidence of an M3 drop.

The native connection path now includes PCI discovery, hardware preparation,
API-77 firmware/NVM, persistent RX, passive discovery, managed association,
WPA2-PSK/CCMP and bounded Ethernet queues feeding the existing IPv4 stack.
`wlan connect` opens a paginated console assistant; the direct forms remain
`wlan connect wpa2 <BSSID>` and `wlan connect open <BSSID>`. Both are opt-in and start DHCP only after the data
port is authorized. Local Wi-Fi IPv4 data has now been verified on the P1;
this is not a claim that DHCP or Internet connectivity works.
The P1 has confirmed ALIVE and cleanup in both ordinary and persistent-trace
modes. The NVM retest now passes all three commands, validates NVM/MAC and
releases DMA. It confirms the corrected INIT_COMPLETE contract on the P1;
ordinary persistent startup and passive scanning now pass on the P1: seven
BSS entries, 13/13 completed commands and verified stop/DMA release. Standalone
untraced NVM timing comparison and authorized data still need physical tests.
Earlier P1 tests confirmed open-system authentication and 802.11 association
(AID 1) without WPA2 authorization. The latest test advances to local IP data.
WPA2-PSK is wired to
the AX200 RX/TX transport and acknowledged firmware key installation through
the driver API `iwx_connect_psk()`. A simulated AP exercises that path, including
CCMP metadata/replay validation and key renewal. This is not a physical RF test.
The existing shell `wlan associate` remains a management-only diagnostic with
its data port closed, including on open networks. Credential input has temporary
visible echo for the requested P1 keyboard diagnostic, but no command-history
entry (see the password-echo checkpoint below). Boot prefers Ethernet with carrier; an absent, failed
or disconnected Ethernet NIC allows AX200 selection before starting the IP stack.
It does not automatically start firmware, scan or connect, or switch NICs later.

The connection kernel is now installed on the development USB's ESP, with
verified readback and an old-kernel backup (see the deployment checkpoint below).
Hardware NVM, persistent startup, passive scan, management TX, association
and now local IP data are validated. The earlier P1 readback, at
01:43 UTC, confirms the M1 fix: six EAPOL messages accepted, zero dropped,
six advisory PMKID mismatches, and TX 8/8/0 (two management frames plus six
M2 replies). No key is installed, no M3 is accepted and the AP disconnects
with reason 15. These counters do not prove the AP authenticated M2, or
that M3 was never received below the WPA2 parser. The earlier five-M1-drop
failure is historical, not the current failure point.
The PSK-only/no-PMKSA-cache path now treats that unauthenticated identifier as
advisory and continues using the configured PMK. M3 MIC, RSN binding, replay,
key-install and TX-completion authorization gates are unchanged. See the
physical PMKID checkpoint below for evidence, tests and deployment status.
The PSK M1 kernel is installed on the development USB's ESP, verified at
2026-09-24 01:27:45 UTC (`32c451f2...`). The diagnostic `6a75a9d6...` kernel is
backed up as `EFI/BOOT/KERNPMK.ELF`. Its P1 retest advances through M2 TX but
does not authorize data or obtain IP. No further USB update was made at readback.

The existing Ethernet backend, DHCP/APIPA, OFTP, `kdownload`, `kupdate` and
`kexec` remain the network path to reuse. See [network stack](x86-network-stack.md).
Do not create a parallel network stack or reset `net_init()` on reconnection.

## PCI and hardware contract

- Only discrete Intel AX200 `8086:2723`, class `02/80`, is selected. AX201/CNVi,
  AX210 and other controllers are not assumed register-compatible.
- PCI records the first AX200 separately from Ethernet, even when the generic
  64-entry enumeration array is full. Multiple-radio selection is not provided.
- `wlan` / `wlan status` report cached PCI discovery and software runtime state. They do not wake
  the card or infer RF-kill from power-gated registers.
- `wlan probe` is an explicit, destructive diagnostic for the currently unowned
  card, not an interface-up operation. It checks PCI identity and the bounded
  capability chain, clears bus mastering, disables INTx/MSI/MSI-X and requests
  D0. True 16-bit config writes avoid rewriting adjacent W1C Status bits.
- BAR0 must be assigned memory, page-aligned, within the kernel's shared
  512-GiB direct-map slot. No all-ones BAR-sizing write is needed. The first two
  register pages (including RX doorbell 0x1c80) are mapped, and both high-half
  translations are checked before use.
- The AX200 sequence obtains NIC ownership, handles firmware persistence,
  resets, applies the device APM settings, waits for MAC clock, then samples
  radio enable/RF-kill. A blocked radio is a valid diagnostic result, not a
  kernel crash and not an enabled link.
- Hardware waits have a monotonic deadline and an iteration bound. Runtime
  RX polls have a per-iteration buffer budget. All-ones and
  power-gated MMIO values must not satisfy readiness masks. A concurrent probe
  is rejected rather than interleaving register transactions.
- Every probe exit that reached accessible MMIO attempts STOP_MASTER, confirms
  MASTER_DISABLED, resets and clears INIT_DONE. Failures retain their original
  stage/error and separately report whether stop was confirmed. The diagnostic
  `probe` never enables DMA or submits a radio packet. It leaves D0/memory decode and
  interrupt masking configured; it does not restore an inherited active driver.

PCI legacy CF8/CFC transactions in `pci.c` are serialized across CPUs with local
IRQs saved/restored. Config accesses outside the available ECAM bus range use
the legacy path. Other code that bypasses this backend is not made safe by that
lock.

The driver sequence/register subset follows the primary reference
[OpenBSD iwx](https://github.com/openbsd/src/blob/master/sys/dev/pci/if_iwx.c)
(`if_iwx.c` revision 1.230, `if_iwxreg.h` revision 1.75); ISC/BSD notices are
retained in `iwx.c` and `iwxreg.h`. Completion buffer IDs and LTR field encoding
were also checked against Intel's BSD-licensed
[Linux RX transport](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/pcie/rx.c)
and [Gen2 startup](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/pcie/trans-gen2.c).
The kernel integration and tests are native
OsitoK code, not a Linux/NDIS compatibility wrapper.

## Firmware

The Lenovo `n2ow901w.exe` package was extracted without executing its installer.
`NETwtw08.sys` needs NT/NDIS/WDF, not the Win32 application ABI. Its
`Netwfw08.dat` is not the native Intel TLV container. Do not rename that blob
or claim it has been loaded by this driver.

The inspection fixture is the unmodified `iwlwifi-cc-a0-77.ucode` from
[linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware/-/tree/797d34e622b2262ca0777e98fd40b1d29034169d/intel/iwlwifi):

- 1,368,100 bytes, version `77.8dbafb52.0`.
- SHA256 `94f5fb915f2074f98059365e11cca313e25a6fa3b7cd6772e80b3ba5edfb966b`.
- 14 LMAC sections, 15 UMAC sections, 19 paging sections including CSS;
  declared paging payload 585,728 bytes; 192 command-version records.
- The command-version table declares ALIVE notification ABI 6, not ABI 4.
- Keep `LICENCE.iwlwifi_firmware` alongside any staged/distributed binary.
  The firmware remains an external binary under its own license, not kernel
  source or a binary committed to this repository.

Reproducible fetch, with pinned revision and hashes for binary and license:

```sh
bash arch/x86/scripts/fetch-ax200-firmware.sh arch/x86/build/ax200-firmware
```

`wlan firmware <path>` reads through VFS and rejects missing/short/oversized
files, wrong magic/API, malformed lengths/padding, duplicate required metadata,
bad separator order, excessive/overflowing sections and inconsistent paging.
Unknown optional/debug TLVs are opaque but still bounds-checked. Reads are
bounded to 4 MiB; the buffer is freed after inspection, including every error.
The parser retains no pointer into freed input. This is structural validation,
not firmware authentication or a guarantee that a different API-77 image fits
the radio SKU. It deliberately does not write any firmware to the device.

## Firmware boot diagnostic

`wlan fwboot <path>` uses the same checked TLV parser to build a section plan.
It additionally requires exactly one supported ALIVE ABI record (4, 5, 6 or 7).
Section offsets reference only payloads, not their destination words. No PCI
configuration or allocation occurs for an invalid plan.

- Context-info is the packed 1792-byte 22000 ABI. LMAC/UMAC/paging buffers are
  copied to separate page-aligned physical allocations. The context order is
  UMAC, LMAC, paging; unused entries are zero.
- The default RX queue has 512 4-KiB buffers, 64-bit free descriptors, 32-bit
  completion descriptors and a status block. A zeroed 256-entry long-TFD
  command queue is supplied, but no host command is submitted yet.
- Allocations must be mapped, contiguous within each block and below 4 GiB.
  This conservative DMA aperture avoids 4-GiB crossings; there is no bounce
  allocator or IOMMU setup in this checkpoint. Exhaustion is an explicit error.
- PCI bus mastering is enabled only after all buffers and firmware copies are
  ready. INTx, MSI, MSI-X and CSR delivery remain masked; the bounded boot uses
  polling, not a registered IRQ handler. Two 32-bit writes publish context-info,
  and a locked peripheral write releases the firmware CPU.
- The CSR ALIVE cause means RFH initialization, NOT successful firmware boot.
  Only then are RX descriptors published (initial doorbell 8, then 504). This
  one-shot diagnostic does not recycle buffers: its notification budget is 504
  buffers, leaving a ring gap. Exhaustion fails explicitly. Streaming RX and
  recycling belong to the later persistent transport.
- RX uses completion buffer IDs, not an assumed correspondence between buffer
  ID and ring position. Cursor bounds, duplicate IDs, packet/queue bounds,
  notification header, exact ABI length and `0xCAFE` status are checked.
  SRAM pointers returned by firmware are not dereferenced.
- A firmware/hardware error, radio/thermal block, MMIO disappearance, missing
  RFH or missing notification terminates the test. Waiting is bounded by five
  seconds and iteration limits; a broken clock also has a bounded failure.
- Even after a valid ALIVE, ALL firmware, paging and ring allocations remain
  pinned until STOP_MASTER/MASTER_DISABLED, reset and PCI bus-master disable
  succeed. The command always attempts shutdown and never reports a link.
- If shutdown cannot be confirmed, the buffers are quarantined. The original
  boot failure and separate shutdown failure remain visible. New probes/boots
  are refused. `wlan stop` retries shutdown, verifying the saved PCI identity
  and BAR before touching MMIO; it never force-frees DMA-reachable memory.
- `kexec` prepares allocator-owned staging, then acquires the same ownership
  lock and stops active/quarantined DMA before overwriting final destinations.
  The lock remains held through kernel handoff; an aborted preparation releases
  its resources. See [kexec ownership](x86-kexec.md). The AX200 guard is not a
  general device shutdown registry or a claim that all drivers are kexec-safe.

Guest commands after staging firmware on a test filesystem:

```text
wlan
wlan probe
wlan firmware /iwlwifi-cc-a0-77.ucode
wlan probe > wlan-probe.log
wlan fwboot /iwlwifi-cc-a0-77.ucode > wlan-fwboot.log
wlan stop
```

The last command uses the existing shell redirection. If storage is unavailable,
the visible stage/error/register values still help; firmware inspection and the
hardware probe are independent operations. The development USB deployment below
includes the firmware in OsitoFS3, where VFS can actually read it, not just on ESP.

## Validation

```sh
bash arch/x86/scripts/test-iwx.sh arch/x86/build/iwx-host-v8 \
    arch/x86/build/ax200-firmware/iwlwifi-cc-a0-77.ucode
make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf
bash arch/x86/scripts/test-oftp-qemu.sh arch/x86/build/iwx-qemu-v3 \
    arch/x86/build/ax200-firmware/iwlwifi-cc-a0-77.ucode
```

Use new output directories for subsequent runs. Host tests include production
code with mocked PCI/MMIO/time, not a second implementation of the driver:
the initial ALIVE checkpoint had 2,191 hardware/transport checks, 44,717 firmware
checks and 23 PCI checks per compiler/mode; current runtime results are below.
GCC and Clang pass in normal and ASan/UBSan modes with halt-on-error enabled.
Firmware tests include every truncation of the synthetic image, deterministic
mutations, and the real firmware fixture. PCI tests cover config access width,
IRQ restoration, ECAM range fallback, full enumeration and Ethernet isolation.
Transport tests inject each allocation failure, valid ALIVE ABIs 4 through 7,
malformed/absent notifications, wrong queue/cursor/IDs, RF-kill, hardware errors,
failed PCI BME transitions and MMIO loss. They assert lifetime of firmware and
paging through failed stops, quarantine/retry, repeated cleanup without leaks,
and mutual exclusion with kernel handoff. These are simulated hardware events,
not evidence of a successful physical boot.

The optional second argument to the existing OFTP QEMU test stages firmware
on a fresh managed OsitoFS3 scratch disk and exercises the shell commands.
QEMU has virtio-net, not an AX200: its pass cannot validate physical MMIO,
firmware boot or Wi-Fi reception. Real hardware validation on the P1 is pending.

The OFTP integration test additionally checks `fwboot` rejection without a
radio, no false ALIVE confirmation, `wlan stop`, and release of the handoff lock
after `kexec` fails to find its file. Its result JSON and copied kernel identify
the exact run; the kernel can be hashed independently. It does not perform a
successful kernel handoff. Managed scratch disks are removed when QEMU exits.

`build/iwx-qemu-v3/result.json` passed all WLAN checks and five OFTP cases,
including byte-exact readback and rejection of the changed-size transfer;
`server_errors` is empty. The managed scratch inventory is empty after exit.
Tested kernel: 6,755,280 bytes, SHA256
`92057155ae76533c6748b3cd2d13179bca0a20fb06519a13fa164585121cdb2d`.
The build emitted only the existing unused `sh_reboot`/`sh_halt` warnings.

## Development USB deployment (2026-09-22)

At 14:38 UTC the identified development USB, `iOS devi Y08`, serial
`00000000CY02`, 125,085,155,328 bytes, was updated without formatting. The
exact tested kernel above is installed both at `EFI/BOOT/kernel.elf` and at
OsitoFS3 `/kernel.elf`; the latter no longer points default kexec at an older
kernel. This does not itself qualify a successful hardware kexec handoff.

OsitoFS3 now also contains `/iwlwifi-cc-a0-77.ucode`,
`/LICENCE.iwlwifi_firmware` and `/WLANTEST.txt`. Physical readback of all four
files matched SHA256. The previous EFI kernel (`7440ab26...339f9`) remains at
`EFI/BOOT/KERNUSB.ELF`. Both GPT copies, the bootloader (`feec6ce0...94038`),
older boot backups and all unrelated filesystem objects were preserved.

The repository's `ositofs-write3` prepared the changes on a sparse copy of
the USB's current metadata and directory blocks. Scope checks allowed only
the kernel inode, root directory and three new files; all 301 preexisting
root entries remained. Deployment applied 133 changed 64-KiB blocks
(8,716,288 bytes), adding three inodes and 24 data blocks. Every overwritten
block was backed up locally before any raw write, with data written before
directory/metadata and the superblock last. Guard hashes were rechecked under
a volume lock; written blocks, metadata, file payloads and GPT were verified
afterward, and caches were flushed. No full filesystem image was written.

Evidence is under the ignored directory
`arch/x86/build/realhw-usb-20260921/wlan-deploy-20260922/`:

- `patch.json`, `before-fsck.txt`, `after-fsck.txt`: bounded offline preparation.
  Both fscks report only the two preexisting free-counter mismatches. They are
  not clean-fsck results and were not silently repaired by the host writer.
- `qemu-v2/result.json`: exact-bundle UEFI/xHCI USB boot, mount, firmware TLV
  inspection, honest no-AX200 fwboot rejection, DMA cleanup, persistent
  diagnostic writes and byte-exact guest file-copy readback all passed.
  Original staged files also remained byte-identical after guest activity.
- `physical-update-v2/result.json`: successful physical deployment and hashes;
  `rollback.json` plus `before-blocks/` preserve every changed block, including
  the old OsitoFS3 kernel. The first deployment attempt stopped before raw
  writes because its local JSON backup index used non-string dictionary keys.

The earlier `qemu/` attempt failed a literal console-text assertion after a
duplicated character. Its raw log is retained; the subsequent test uses
independent on-disk readback as well as serial output. This is not a claim that
the intermittent serial duplication is fixed. All scratch images were removed
by the managed disk runner. The physical AX200 still needs testing on the P1;
QEMU has no such radio. This kernel also includes the unsupported USB cache-flush
policy described in [USB storage](x86-usb-storage.md).

The subsequent P1 run failed the USB cache probe before filesystem mounting,
so it could not reach the firmware file. At 16:37 UTC the development USB was
updated again with the [short-response transport correction](x86-usb-storage.md#p1-cache-probe-transport-failure),
kernel SHA256 `5c2d9f6b9050414a8866568ec1e43b7bad07f48c5dd1f133edb7cd4f53eff7cb`.
Both kernel copies and `/WLANTEST.txt` were updated, firmware/license hashes
were verified unchanged, and `EFI/BOOT/KERNBOT.ELF` preserves the previous
kernel. No successful physical AX200 firmware boot or Wi-Fi link has been
confirmed yet.

## P1 Physical Firmware Attempt

The 2026-09-22 17:01 UTC read-only USB extraction is retained at
`build/realhw-usb-20260921/hardware-readback-20260922-170154/`. Its `result.json`
records individual log hashes and the installed EFI kernel, still
`5c2d9f6b9050414a8866568ec1e43b7bad07f48c5dd1f133edb7cd4f53eff7cb`.
The USB now mounts and persists logs; its metadata snapshot passes read-only
fsck with zero errors. No USB or kernel update was performed during extraction.

`root-logs/wlan-probe.log` confirms:

- AX200 `8086:2723`, PCI `82:0.0` (decimal), BAR0 `0xCEB00000`.
- HW revision `0x340`, RF ID `0x0010A100`, GP control `0x08040005`.
- MAC clock ready, radio switch enabled, and verified device stop.
- PCI command changes from `0x0000` to `0x0402`: memory decode on,
  bus mastering off and INTx disabled after the diagnostic.

`root-logs/wlan-fwboot.log` reaches `firmware RFH initialization`, but reports
`INT observed: 0x20000000`, no RFH confirmation and zero RX packets. The TLV
plan selects ALIVE ABI 6, but no ALIVE notification arrives. Bit 29 is the
hardware/DMA error cause, distinct from the bit-25 firmware software error in
[Intel's CSR definitions](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/iwl-csr.h).
That kernel's combined error text does not distinguish these sources. Shutdown
is confirmed, PCI command is again `0x0402`, and DMA allocations are released.

This establishes an early transport failure, not a Wi-Fi authentication,
association, DHCP or APIPA failure. It does **not** establish its root cause:
the report does not preserve FH interrupt/error registers, context address
readback or PCI error state before reset. The next diagnostic should capture
that state before verified shutdown, retaining the original error and the
existing DMA quarantine rules. Investigate platform DMA remapping only with
evidence; do not disable it or relax firmware/descriptor validation speculatively.

## Pre-reset Boot Snapshot

Boot errors after DMA publication now copy an inline report before
`stop_transport()` can reset the device or release its allocations. There is
no allocation, filesystem I/O or console output in the capture path. The shell
prints the saved report only after shutdown, including when output is redirected:

```text
wlan fwboot /iwlwifi-cc-a0-77.ucode > wlan-dma.log
```

The `Boot snapshot: captured BEFORE shutdown/reset` section includes:

- Raw CSR INT/FH_INT, interrupt mask, GP control, reset and hardware config.
- Expected and observed context address, context size/MAC/flags, firmware and
  RX-ring DMA addresses, pinned byte count and host RX cursors.
- PCI identity/command/status and endpoint PCIe device/link control/status.
  Capability traversal is bounded and rejects malformed chains or types.
- RFH config/status, FH TX status/error, secure-boot CPU status and firmware
  load status, using the register selection/access paths in Intel's
  [iwl-io.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/iwl-io.c),
  [iwl-fh.h](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/iwl-fh.h)
  and [iwl-prph.h](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/iwl-prph.h).

Snapshot reads do not acknowledge W1C errors, change PCI configuration or
reset the device. Peripheral access requires a bounded MAC-access handshake;
its request bit is restored to the preceding state. An inaccessible register
or diagnostic timeout is marked separately and never replaces the original
boot error. PCI identity/BAR/memory-decode mismatch skips MMIO; power-gated or
inaccessible CSR state skips peripheral reads. Shutdown/quarantine rules are
unchanged. The report is not an atomic hardware freeze, does not dereference
firmware-supplied pointers and does not inspect or disable the IOMMU.

`build/iwx-host-snapshot-v3/` passes 2,356 transport, 44,717 firmware and 23 PCI
checks per GCC/Clang normal/sanitized run. Tests cover the observed bit-29
failure, independent FH/software errors, values surviving reset/free, no W1C
acknowledgements, PCIe chain errors, peripheral timeout/sentinels, missing MMIO,
quarantine and repeat capture. Successful boots and pre-publication failures
do not trigger a snapshot. Kernel build succeeds with only the preexisting
unused `sh_reboot`/`sh_halt` warnings. These simulations do not identify the
P1's root cause; a new physical log is required.

At 17:27 UTC the snapshot kernel (6,759,432 bytes, SHA256
`7341902b4474a9946e0942292e6f8076b60c15f5d958a8d73f8fbd209850f3a3`)
was installed in EFI and OsitoFS3 on development USB `00000000CY02`.
`EFI/BOOT/KERNDMA.ELF` preserves the previous `5c2d9f6b...eff7cb` kernel.
Only the kernel and `/WLANTEST.txt` were replaced in OsitoFS3, using 107
backed-up blocks with no allocation or directory changes. GPT, bootloader,
firmware/license and all six prior root/diag log files passed unchanged
physical SHA256 readback. No physical AX200 run of this kernel is claimed.

Evidence and rollback blocks are under
`build/realhw-usb-20260921/dma-snapshot-deploy-20260922/`. Before/after offline
fsck both report zero errors. `qemu-v2/result.json` passes exact-kernel USB
boot/mount, firmware inspection, no-radio rejection/cleanup and byte-exact
guest copy/log persistence. `serial_text_exact` is false because the existing
intermittent serial text issue recurred; on-disk readback passed independently.
The physical updater exited 0 and all managed QEMU scratch images were removed.

## AX200 PRPH Address Contract

The 17:52 UTC P1 readback is retained at
`build/realhw-usb-20260921/hardware-readback-20260922-175158/`.
`root-logs/wlan-dma.log` captures bit-29 HW_ERR before reset, with FH_INT zero,
no RFH/ALIVE and zero RX packets. Context expected/readback both equal
`0x108FE000`, with 448 dwords and flags `0x990`. This verifies the CSR context
address, not that the device successfully read the DMA payload. PCIe device
status is `0x001F`; without a pre-attempt baseline its sticky errors cannot
be attributed exclusively to this attempt. Several PRPH samples are
`0xA5A5A5A2`, not valid register contents. Shutdown and DMA release succeeded.
The filesystem metadata/directory snapshot passes read-only fsck, zero errors.

Inspection found an independent driver contract violation: AX200/22000 PRPH
addresses must be masked to 20 bits before adding the dword access field,
unlike AX210's 24-bit addressing. See
[Intel's Linux transport](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/pcie/trans.c)
and `iwx_prph_addr_mask()` in
[OpenBSD iwx](https://github.com/openbsd/src/blob/master/sys/dev/pci/if_iwx.c).
All peripheral reads/writes now go through local helpers applying this
encoding, including persistence, CPU release and pre-reset diagnostics.
For example, CPU_INIT_RUN is encoded as `0x03005C44`, not `0x03A05C44`.
MAC-access ownership, firmware validation, DMA lifetime and reset policies
are unchanged. Do not add AX210 UMAC offsets to AX200.

The mock now checks the full bus command and independently decodes 20-bit
register addresses. The original driver fails 788 assertions in
`build/iwx-prph-red/`; the corrected driver passes 3,184 transport, 44,717
firmware and 23 PCI checks per GCC/Clang normal/sanitized run in
`build/iwx-prph-green/`. This proves the encoding regression is covered,
not that it is the sole cause of the P1 failure. Actual ALIVE still needs
verification on the AX200.

This readback also exposed old bytes after the new 20,118-byte prefix of
`dmesg.log`; use `diag/boot1.log` for the complete new boot. The shell's
[redirection correction](x86-shell-redirection.md) addresses this separately.

At 18:28 UTC the corrected kernel (6,759,496 bytes, SHA256
`ed5640a63074d743f5622cc9a8ef802bbc3d4baf5cd5cd7bec10f1ae0cc52342`)
was installed in both EFI and OsitoFS3 on development USB `00000000CY02`.
`EFI/BOOT/KERNPRPH.ELF` preserves the preceding snapshot kernel. Physical
readback verified both copies and all eight prior diagnostic files, firmware,
license, existing boot files and GPT. The 107-block update changes only the
kernel and `/WLANTEST.txt`, with no inode/block allocations or directory edits.
All changed blocks were backed up before writing; the updater exited 0 and
closed all handles. This is not a physical AX200 ALIVE result.

Evidence is under
`build/realhw-usb-20260921/prph-redirection-deploy-20260922-v2/`:
`physical-update/result.json`, `rollback.json`, `before-blocks/`, and
`qemu-v2/result.json`. Before/after prepared filesystem checks both passed
with zero errors. Exact-kernel QEMU verified boot, USB mount, firmware parsing,
honest no-radio rejection, DMA cleanup and byte-exact persisted shell tests.
The initial `prph-redirection-deploy-20260922/` trial is retained: its empty
output fixture incorrectly assumed `cat` omitted its display newline.
The corrected trial uses the no-output `clear` command. The intermittent
serial-text issue remains (`serial_text_exact=false`); independent file
readback passed. Managed scratch disks were cleaned after both trials.

## Read-only Boot Phase Trace

The fresh P1 readback at
`build/realhw-usb-20260921/hardware-readback-20260922-185735/` confirms
identical first/second fwboot reports: HW_ERR `0x20000000`, FH_INT zero,
no RFH/ALIVE and no RX packets. Both attempts stop and release their DMA;
the explicit stop has no pending allocations. The boot log records `sync: ok`
and the metadata/directory snapshot passes read-only fsck, zero errors.
This is a new boot after diagnostic cleanup, not an old redirection tail.

`iwx_fw_boot()` now retains at most 16 in-memory phase samples. The successful
path uses 14: initial PCI state, before card reset, after preparation, DMA
ready, before/after BME, context publication, before/after CPU_INIT_RUN, after
MAC release, first poll, RFH interrupt, validated ALIVE and before stop.
Failure paths retain only reached stages. Poll count is separate; timeout
loops do not fill the trace. Overflow is counted without further hardware
reads. The whole report is statically limited to 4 KiB.

Each sample retains elapsed microseconds, PCI command/status, bounded PCIe
capability reads, PMCSR, CSR interrupt/FH/clock/reset/config/mask/context,
allocated context address and DMA/RX counters. Before MMIO is mapped only
configuration space is read. Identity, memory decode, BAR and available
power-state checks gate CSR access; invalid diagnostics stay separate from
the original error. No PRPH handshake, register write, W1C acknowledgement,
allocation or output was added to the capture path. Samples are not atomic
and the additional reads can affect timing. The shell prints them after
the shutdown attempt, followed by the existing pre-reset peripheral snapshot.

The PCIe baseline preserves sticky error bits, never clears them. Newly set
bits can be distinguished, but unchanged bits cannot exclude recurrence.
Context address readback still does not establish a successful DMA fetch.
The trace does not inspect bridge AER, ACPI DMAR or IOMMU fault records;
those remain separate platform diagnostics if the trace points there.
It does not change interrupt masks, power policy or the firmware sequence.

`build/iwx-trace-v3/` passes 3,501 transport, 44,717 firmware and 23 PCI
checks per GCC/Clang normal/sanitized run with the real API-77 image.
Injection at BME, context publication, CPU start and MAC release verifies the
first observed error stage, PCIe deltas and survival across reset/free.
Other coverage includes preexisting errors, D3/BAR/identity guards, malformed
capabilities, timeout bounds, partial allocation, vanished MMIO, quarantine,
trace overflow and absent/invalid firmware. Context field offsets now have
additional compile-time ABI checks. Kernel build passes with only the existing
unused `sh_reboot`/`sh_halt` warnings. This is not a physical ALIVE result.

At 19:34 UTC the phase-trace kernel (6,771,864 bytes, SHA256
`e251331f18d39215bcdd469e33753bed9f7083b24511a56aae1acd9b179569e5`)
was installed in EFI and OsitoFS3 on development USB `00000000CY02`.
`EFI/BOOT/KERNTRCE.ELF` preserves the previous `ed5640a6...cc52342` kernel.
Only `/kernel.elf` and `/WLANTEST.txt` changed in OsitoFS3, through 107 backed-up
blocks with no inode/block allocations or directory edits. Physical readback
verified both kernels, firmware/license, all eight preceding root/diag files,
the bootloader, existing boot files and GPT. The updater exited zero.

Evidence is under `build/realhw-usb-20260921/trace-deploy-20260922/`:
`physical-update/result.json`, `physical-update/rollback.json`,
`physical-update/before-blocks/`, and `qemu-v2/result.json`.
Prepared filesystem checks before/after report zero
errors. The exact kernel passed QEMU USB boot/mount, firmware parsing,
no-radio rejection and cleanup, persisted diagnostics and independent
byte-exact guest file verification. The existing intermittent serial-text
issue remains (`serial_text_exact=false`); on-disk data passed. All managed
scratch images were removed. The actual AX200 phase trace still needs the P1.

## P1 Phase Trace Readback (2026-09-22)

Read-only extraction at 19:56 UTC is preserved under
`build/realhw-usb-20260921/hardware-readback-20260922-195605/`.
The reader exited zero and confirmed the installed `e251331f...569e5` EFI
kernel. `diag/latest.txt` identifies the new, untruncated `diag/boot1.log`;
it records both trace captures, their exact 7,096-byte lengths and `sync: ok`.
Read-only fsck of the metadata/directory snapshot reports clean, zero errors
(15,940 files, 1,980 directories, 455,752 free blocks). The USB was not changed.

The first attempt localizes a new PCIe error before the CSR hardware error:

| Sample | Elapsed us | PCIe Device Status | CSR INT |
| --- | ---: | --- | --- |
| before-prepare | 0 | 0x0019 | not sampled |
| after-context | 7206 | 0x0019 | 0 |
| after-cpu-run | 7269 | 0x0019 | 0 |
| after-mac-release | 7303 | 0x003D | 0 |
| first-poll | 7339 | 0x003D | 0 |
| before-stop | 44687 | 0x001F | 0x20000000 |

Device Status is the high half of the combined control/status value in the
report. Relative to `0x0019`, `0x003D` adds Fatal Error Detected (`0x0004`)
and Transactions Pending (`0x0020`). At the final sample pending has cleared
and Non-Fatal Error Detected (`0x0002`) is also set. Correctable error and
unsupported-request flags were already set in the initial baseline; they
cannot be attributed to this launch. Decoding follows
[Linux's PCIe register definitions](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/pci_regs.h#L490-L496).
The sampling interval associates the new fatal bit with CPU start/MAC release;
it does not prove that the release write caused it.

The second attempt begins at `0x001F`, retaining the first attempt's errors.
It reaches `0x003F` after MAC release at 7,279 us and CSR HW_ERR at 47,767 us,
after 374 polls (first attempt: 345). Its pending-transaction bit is new, but
the sticky fatal/nonfatal bits cannot identify a new occurrence by themselves.
Both attempts retain PMCSR `0x0008` (D0), link status `0x1012`, valid CSR
reads and matching context-register readback at `0x10901000`. Neither receives
RFH confirmation or an ALIVE packet; FH_INT stays zero. Both report verified
stop and release of all 3,551,232 DMA bytes, and the intervening explicit stop
reports no pending WLAN DMA. Register readback is not proof of a DMA fetch.

The boot enumeration identifies the AX200 at `52:00.0` (bus 82 decimal), behind
`00:1d.6` (`8086:a336`, secondary/subordinate bus 82). The next capture should
include bounded, read-only AER status/mask/severity/first-error/header registers
on both functions before reset, plus DMAR translation state and applicable
IOMMU fault records. No such data exists in this readback, so neither a
completion timeout nor an IOMMU rejection is established. Do not disable
translation, alter ASPM or substitute firmware based only on these flags.

## AER and DMAR Evidence Capture

`kernel/dma_diag.c` adds a separate bounded read-only platform report to
`wlan fwboot`: once before device preparation, once immediately before
shutdown and the existing PRPH snapshot. It neither changes the firmware
sequence nor replaces its original error. Preparation may map pages; the
post-launch capture does not allocate, map, print or write hardware registers.
Capture timestamps describe the sampling duration, not atomic bus events.

The PCI path is discovered from enumerated bridges, not P1 BDF constants.
At most four functions are retained, with explicit cycle/depth/incomplete
topology notes. The new strict `pci_diag_read32()` rejects unaligned/out-of-
range accesses and extended configuration without ECAM instead of wrapping
onto CF8/CFC. Existing driver configuration-access behavior is unchanged.
Both conventional and extended capability walks check bounds and cycles;
overlapping/truncated AER ranges are rejected. Reports retain UNC/COR status,
masks, severity, first-error control, header log, and root-port error/source
registers. Newly set bits are compared only against a valid baseline;
unchanged sticky flags never establish that an error did not recur.

The DMAR reader validates RSDP, XSDT/RSDT and DMAR checksums/lengths, with
64-KiB tables, 256 root entries/subtables and 32 DRHD units as explicit limits.
Tables must be in retained UEFI memory; firmware-nominated MMIO ranges may
occupy holes but cannot overlap RAM/ACPI/runtime memory. Address overflow,
mapping failure and unsupported spans produce notes without dereferencing
unmapped memory. DRHD scope resolution handles endpoint and bridge PCI paths,
selects an explicit owner before INCLUDE_ALL, and rejects ambiguous ownership.
Other PCI segments and unrelated DRHDs are not read as this device's IOMMU.

The applicable unit contributes VER/CAP/ECAP, GSTS translation state, raw
RTADDR, FSTS before/after, supported protected-memory ranges, and primary
fault records. CAP bounds the walk to 256 slots; at most 16 stable records
are retained, prioritizing the target requester. Raw fault information,
source ID, reason, active/stored/target/unstable counts and changing-state
markers remain available. Faults are not acknowledged, masks are not cleared,
and neither translation tables nor advanced fault logs are followed. Missing
or unchanged faults are not proof of successful DMA. Firmware-described
memory and captured PCIe/IOMMU registers can still change during capture.

Register/layout references: [PCIe AER](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/pci_regs.h),
[ACPI DMAR](https://github.com/torvalds/linux/blob/v6.12/include/acpi/actbl1.h),
[Intel VT-d registers](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/intel/iommu.h)
and [fault record access](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/intel/dmar.c).

`build/iwx-aer-v3/` passes all GCC/Clang normal/ASan+UBSan variants: 3,505
transport, 44,717 firmware, 39 PCI and 108 platform checks per variant.
Coverage includes real API-77 parsing, pre-stop retention, legacy config
rejection, parent ambiguity, AER cycles/overlap/truncation, invalid ACPI
checksums/lengths, RSDT fallback, DRHD ownership/scopes, RAM overlap, MMIO
failure, all 256 fault slots, target retention on overflow, unstable records
and bit-for-bit unchanged device registers. The platform report is 2,032
bytes; the complete WLAN report has an 8-KiB compile-time stack bound.
An initial Clang UBSan failure was in host-fixture pointer arithmetic and was
fixed before the complete v3 rerun; it did not require a driver workaround.

At 20:50 UTC on 2026-09-22 the diagnostic kernel (6,792,744 bytes, SHA256
`4a447102f880c709e51503f5003d19d064753ec6b3c0e99f21a0bbb4992982ee`)
was installed in EFI and OsitoFS3 on development USB `00000000CY02`.
`EFI/BOOT/KERNAER.ELF` preserves the preceding phase-trace kernel.
Only `/kernel.elf` and `/WLANTEST.txt` changed in OsitoFS3, through 107
backed-up blocks with no inode/block allocations or directory edits.
Physical readback verified both kernel copies; all 14 preceding root/diag
files, firmware/license, bootloader, existing boot files and GPT were
preserved byte-for-byte. The updater exited zero and its handles are closed.

Evidence is under `build/realhw-usb-20260921/aer-deploy-20260922/`:
`physical-update/result.json`, `physical-update/rollback.json`,
`physical-update/before-blocks/`, and `qemu-v2/result.json`.
Prepared filesystem checks before/after report zero errors. The exact
kernel passed QEMU USB boot/mount, firmware parsing, no-radio rejection
and cleanup, persisted diagnostics and independent byte-exact guest file
verification. The existing intermittent serial-text issue remains
(`serial_text_exact=false`); on-disk data passed. All managed scratch images
were removed. QEMU has no AX200; actual AER/DMAR evidence still needs the P1.

## P1 AER and DMAR Readback (2026-09-22)

Read-only extraction at 21:25 UTC is preserved under
`build/realhw-usb-20260921/hardware-readback-20260922-212508/`.
The reader exited zero and closed its disk handle. The EFI kernel matches
the deployed `4a447102...982ee` diagnostic. The metadata/directory snapshot
passes read-only fsck with zero errors (15,945 files, 1,980 directories,
455,684 free blocks). No USB, driver or kernel changes were made in this review.
`diag/latest.txt` identifies untruncated `diag/boot2.log`, including `sync: ok`.

There is one actual firmware attempt, not two. `wlan-aer.log` has 10,766
bytes (SHA256 `587079e4efd2f6bcd28b3f08eacf8eb5ebe71ef363c1262eaa51c838278b5282`).
The second command was entered as `wlan fwbot ...`, so `wlan-aer2.log`
contains only the 130-byte usage message. The fresh dmesg was saved as
`dmesg.log` (22,031 bytes, SHA256
`99417cadbc65e72ad0521ec16c7675d860cce15302ec6a8847dd23c48d098389`),
not `dmesg-aer.log`. Neither is a missing-write or truncated-capture failure.

The AX200 reaches CPU start/context publication but not RFH or ALIVE. Its
context-register readback matches `0x01FB3000`; PCIe fatal/pending flags
appear at 7,475 us, and CSR HW_ERR is captured at 43,196 us after 330 polls.
Shutdown again verifies device stop and releases 3,551,232 DMA bytes.

The new evidence isolates an unprepared upstream DMA path:

| Register | Baseline | Before stop |
| --- | --- | --- |
| AX200 `52:00.0` PCI Command | `0x0402` | `0x0406` |
| Root port `00:1d.6` PCI Command | `0x0003` | `0x0003` |
| AX200 AER uncorrectable status | `0` | `0x00044000` |
| AX200 AER first-error pointer | `0` | `0x12` |
| DRHD `0xFED91000` GSTS | `0x40000000` | `0x40000000` |
| DRHD FSTS / active fault records | `0 / 0` | `0 / 0` |

Endpoint bus mastering is enabled, but its root port never has Command bit
2 set. The new AER bits decode as Completion Timeout (bit 14) and Malformed
TLP (bit 18); the first-error pointer names bit 18. These are distinct from
the Unsupported Request status encoded in the logged completion header.
See the [AER register definitions](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/pci_regs.h).
The header `0A002000 00EE2040 52000000 00000000` decodes as a completion
without data, status 1 (Unsupported Request), completer `00:1d.6`, requester
`52:00.0`, tag 0. Field decoding follows
[go-pcie-tlp's completion parser](https://github.com/google/go-pcie-tlp/blob/master/pcie/tlp.go).
It does not identify the address of the rejected request or explain which
condition the endpoint classified as malformed.

Intel documents that clearing root-port BME prevents upstream memory/I/O
requests and results in Unsupported Requests; this matches the captured
completion. See [Intel's root-port Command register](https://edc.intel.com/content/www/us/en/design/publications/800-series-chipset-family-platform-controller-hub-pch-volume-2/001/device-command-cmd-offset-4/)
for that PCIe contract, not for P1-specific register programming.
Locally, `iwx.c:launch_firmware()` changes only the endpoint Command, and
`pci_enable_bus_master()` likewise does not prepare parents. Linux's
[PCI enable path](https://github.com/torvalds/linux/blob/v6.12/drivers/pci/pci.c)
recursively enables upstream bridges and their bus mastering. The missing
bridge preparation is established; successful firmware startup after fixing
it still requires a physical retest and may expose another issue.

DMAR ownership resolves successfully to the include-all DRHD for segment 0.
Translation is disabled in both samples (GSTS.TES clear), PMEN is zero and
the only primary fault slot is inactive. There is no evidence here of an
IOMMU translation rejection; disabling VT-d is not a justified fix. The
non-atomic snapshots do not prove that all DMA addresses are otherwise valid.

## Topology-Based DMA Preparation

`pci_prepare_dma_path()` validates the enumerated endpoint and its complete
upstream path before any bridge write. The endpoint must have bus mastering
disabled, with DMA buffers prepared by its owning driver. The bounded walk
rejects missing/ambiguous parents, cycles, overlapping unrelated bus windows,
identity/class/header changes, secondary reset, disabled memory decode and
incomplete inventory. The current PCI enumerator exposes one MCFG allocation
or legacy access and only establishes root bus 0. An MCFG start bus is not
proof of a root bridge; unproven additional roots are explicitly rejected.
The path limit is 32 bridges, and a full 64-entry inventory is conservatively
treated as potentially truncated. This does not implement hotplug or resource
assignment; no rescan may run concurrently with preparation.

After validation, only bridge Command bit 2 is enabled, root first, using
16-bit writes and readback. Adjacent W1C Status and unrelated Command bits
are preserved. The path is rechecked before returning success. Concurrent
preparations return busy without touching configuration. No endpoint BME,
BAR/window, power, AER, ASPM or IOMMU programming is performed by this API.

Bridge enables are bus-owned infrastructure, not a per-endpoint lease.
They deliberately persist after endpoint shutdown and partial failure;
blind rollback could stop DMA for another child. A failed preparation is
reported with the per-bridge write/readback state and does not authorize
endpoint DMA. `iwx_fw_boot()` calls the API after allocating its buffers and
before publishing context or enabling endpoint BME. Its existing verified
stop/quarantine/kexec rules remain unchanged. `wlan fwboot` reports the
path result, bridge Command values and the retained ownership policy.
Legacy users of `pci_enable_bus_master()` are not migrated by this change.

`build/iwx-bridge-v2/` passes all GCC/Clang normal/ASan+UBSan variants:
4,035 transport, 44,717 firmware, 39 PCI access, 141 PCI DMA and 108 AER/DMAR
checks per variant. Coverage includes real API-77 parsing, root-first order,
ECAM and CF8/CFC access, shared children, idempotent/reentrant calls, full-depth
paths, malformed/missing topology, W1C preservation, failed writes, partial
enables retained, retry, and AX200 failure cleanup before context publication.
The kernel builds successfully with only the existing unused shell-helper
warnings. This is not yet a physical AX200 ALIVE result.

At 21:52 UTC on 2026-09-22 the final kernel (6,801,112 bytes, SHA256
`7a55f21c257b03668a4d74ab46b3354a029d9a843b3624cb1aa1abfd7c8ec63d`)
was installed in EFI and OsitoFS3 on development USB `00000000CY02`.
`EFI/BOOT/KERNBME.ELF` retains the preceding `4a447102...982ee` kernel.
The 107 backed-up blocks change only `/kernel.elf` and `/WLANTEST.txt`,
without inode/block allocations or directory changes. Physical readback
verified both kernels and all 19 preceding root/diag files; bootloader,
existing boot files, firmware/license and GPT were unchanged. The updater
exited zero, flushed caches and closed its handles.

Evidence is in `build/realhw-usb-20260921/bridge-deploy-20260922-v2/`:
`physical-update/result.json`, rollback/block backups, clean before/after
fsck reports and `qemu-v2/result.json`. The exact kernel passed QEMU USB
boot/mount, firmware parsing, absent-radio rejection/cleanup and byte-exact
persisted file checks. The existing serial-text duplication issue remains
(`serial_text_exact=false`), independently of successful on-disk verification.
Managed scratch images were removed. This does not test a physical AX200.

## P1 Freeze Readback (2026-09-22)

The user reported that the second command stopped returning at the native
shell, without the compositor. Keyboard/mouse responsiveness was not tested,
so this is not yet evidence that every CPU or the whole machine stopped.
Read-only extraction at 23:04 UTC is retained in
`build/realhw-usb-20260921/hardware-readback-20260922-230406/`; the reader
exited zero and closed the physical disk. The ESP kernel matches the deployed
`7a55f21c...c63d` build, and the bootloader hash is unchanged. No USB writes
were performed during this investigation.

- `wlan-probe-bme.log` contains the complete 498-byte successful probe:
  MAC clock ready, radio enabled, PCI Command `0x0402`, and verified stop.
- Replaying terminal backspaces confirms the exact second command was
  `wlan fwboot /iwlwifi-cc-a0-77.ucode > wlan-bme.log`, without a typo or
  truncated firmware path.
- `wlan-bme.log` exists but is empty. Shell redirection creates/truncates the
  target before dispatch, buffers output in RAM, and writes it on return.
  An empty target therefore does not locate the stall inside the driver or
  prove that firmware file reading completed.
- `diag/latest.txt` names slot 3, `reason=shell-exec`, tick 219336.
  `diag/boot3.log` is 23,710 bytes, untruncated, SHA256
  `23d76978b41da2349ae0ed67877d63d5557d21f0f7b25c442d13ad672f91e3d4`.
  Its last persisted event is entry into that shell command. No subsequent
  driver trace, ALIVE result, exception or shutdown result was recovered.
- Read-only fsck of the bounded metadata/directory snapshot reports zero
  errors: 15,948 files, 1,980 directories, 455,619 free blocks and 113,143
  free inodes. This checks filesystem structure, not every file payload.

This run cannot confirm whether bridge enable, endpoint BME, firmware start,
RX or shutdown was reached. It neither establishes DMA corruption nor rules
it out. The next diagnostic needs persisted milestones before firmware file
reading and around PCI preparation, DMA publication, firmware wait and
cleanup. Merely repeating the same buffered command would retain the same
observability gap. Synchronous persistence can affect hardware timing and
must be explicit, bounded and kept out of MAC-access critical sections.

## Explicit Persistent Progress

`wlan fwtrace <path>` is the opt-in, timing-perturbing variant of `fwboot`.
It records milestones in the current `diag/bootN.log`, independently of shell
stdout redirection. The screen shows the phase before checkpoint I/O, then
`wlan trace: saved` only after the boot-log append, logical size update and
block-driver flush succeed. If a checkpoint itself stalls, the last live
line may lack `saved`; a photograph complements the previous persisted mark.
This is not a claim of power-loss durability beyond the USB driver's cache
contract, nor a guarantee that `diag/latest.txt` was updated successfully.

The shell brackets firmware lookup, allocation and reading. The driver
observer brackets validation, platform diagnostics, card preparation, DMA
allocation, bridge enable, firmware launch, RFH/first RX/ALIVE, snapshots,
verified stop and memory release. State output uses the existing report and
last captured register sample; these values are labeled as prior samples,
not fresh atomic snapshots. Bridge Command/readback results are included.
No per-poll filesystem writes occur, and driver observer calls are limited
to 64. This does not impose a separate deadline on an already-stuck MMIO or
filesystem operation.

`iwx_fw_boot_traced()` retains driver ownership throughout observation.
The old `iwx_fw_boot()` API supplies no observer. A failed observer aborts
startup, suppresses further observer calls, and still attempts the existing
hardware cleanup. The original hardware error takes precedence over a
persistence error. DMA allocations remain quarantined if stop cannot be
verified. Markers are deferred while MAC access may be held: launch uses its
existing post-release sample to confirm the request cleared; an observed
error snapshot checks the release before allowing another callback. Verified
stop ends the critical section. Deferral is reported, not silently described
as persisted progress. The context/BME/CPU-start critical section remains one
bracket, not a series of synchronous disk writes.

`boot_diag_checkpoint()` is a checked wrapper over existing boot diagnostics.
It rejects unavailable, failed, full or reentrant logging. The normal boot
flush callers keep their interface and behavior; the new result additionally
checks append completion, the file-size update and `disk_flush()`. Logging
failure is never interpreted as a successful firmware startup.

`build/iwx-persist-v3/` passes GCC/Clang normal and ASan+UBSan runs: 4,999
transport, 44,717 firmware, 39 PCI access, 141 PCI DMA, 108 AER/DMAR and 107
checkpoint checks per variant. New cases cover milestone order, observer
failure at every success-path milestone, unchanged ordinary-mode register
access, reentrant ownership rejection, timeout/no-ALIVE paths, MAC release
not latched, failed stop/quarantine, append/size/flush failures, log exhaustion
and stale/reentrant persistence success rejection. These host tests do not
reproduce the physical AX200 freeze.

At 23:25 UTC on 2026-09-22, kernel SHA256
`7ccbad5b4b2a686d3295776f9facf32f192413016beeab8472c9fd0672486c3f`
(6,805,456 bytes) was installed in EFI and OsitoFS3 on USB `00000000CY02`.
`EFI/BOOT/KERNPRG.ELF` preserves the preceding `7a55f21c...c63d` kernel.
Evidence resides in `build/realhw-usb-20260921/progress-deploy-20260922-v2/`:
the physical updater exited zero, read back all changed files/blocks,
verified all 22 prior diagnostic files unchanged, flushed caches and closed
its handles. Bootloader, GPT, existing EFI files, firmware/license, directory
blocks and allocation maps were preserved. The 107-block backed-up update
only replaces `/kernel.elf` and `/WLANTEST.txt`; it allocates no blocks/inodes.

The same bundle passed UEFI/xHCI USB boot/mount and firmware/shell regression
checks in QEMU. On-disk extraction verified all 11 expected shell and driver
milestones in order despite redirected stdout, along with absent-radio
cleanup and the missing-file error. `qemu-progress/result.json` records
`persistent_progress=true`, byte-exact readback and QEMU exit zero. The first
checker run wrongly required LF instead of accepting the klog's CRLF; that
failed evidence was retained separately and never deployed. The corrected
checker uses complete logical lines. The preexisting serial character
duplication remains (`serial_text_exact=false`), independently of the verified
file contents. Managed scratch disks were removed after both runs.

Physical retest commands (wait for the prompt between commands):

```text
wlan probe > wlan-probe-p.log
wlan fwtrace /iwlwifi-cc-a0-77.ucode
```

If the second command stalls, photograph its last phase and whether `saved`
appeared, then power off and reconnect the USB. Do not repeatedly reboot or
retry, because boot logs rotate across four slots. If it returns, run
`dmesg > wlan-progress.log` and `sync`, wait for `sync: ok`, then power off
and reconnect. The physical result from this test follows below.

## P1 ALIVE Readback (2026-09-22)

Read-only extraction at 23:40 UTC is retained in
`build/realhw-usb-20260921/hardware-readback-20260922-233959/`.
The reader exited zero and closed its handles without writing the USB. The
ESP kernel matches `7ccbad5b...6c3f`, and the bootloader is unchanged.
`diag/latest.txt` names slot 0, `reason=readline-prompt`, tick 51055, with no
truncation. Evidence includes:

- `diag/boot0.log`: 50,568 bytes, SHA256
  `15f8849c1e12ffc51cf31448fb5b8356988ce7640089a7ed22cb48b436e82a77`.
- `wlan-progress.log`: 43,783 bytes, SHA256
  `c7320a7551866bcdc7a700e9879e03cb4622e5b98f7222f4cfc4910473e48ad6`.
- `wlan-probe-p.log`: complete successful 498-byte probe, radio enabled,
  MAC clock ready and device stopped.

The first `fwtrace` command had `=77` in the filename and returned file not
found before driver entry. The corrected `-77` command performed the only
firmware startup in this boot. It initialized RFH and validated ALIVE ABI 6,
status `0xCAFE`, with two RX notifications and 471 polls. The root port
`00:1d.6` Command changed from `0x0003` to `0x0007`, with BME readback
verified. AX200 `52:00.0` Command changed from `0x0402` to `0x0406` during
startup and returned to `0x0402` after shutdown. The published context
register matched its allocation at `0x10909000`.

All 19 driver checkpoints completed, in addition to four shell file-loading
checkpoints; none were deferred during MAC access. The final state records
`alive=1`, `stopped=1`, `retained=0`, `error=none`: the diagnostic verified
hardware stop and released its 3,551,232 bytes of DMA allocations. It returned
to the prompt, and the subsequent dmesg export and `sync: ok` are persisted.

Baseline/pre-stop AER snapshots show no newly set UNC or COR bits at either
endpoint or root port. Endpoint COR `0x2000` already existed at baseline and
remained unchanged; the captured AER headers are zero. DMAR translation was
disabled, FSTS remained zero, and no active fault records were captured.
These bounded, non-atomic observations are not a general absence-of-faults
guarantee. Read-only fsck of the metadata/directory snapshot reports zero
errors: 15,950 files, 1,980 directories, 455,617 free blocks and 113,141 free
inodes. It does not verify every file payload.

This confirms one physical firmware startup and cleanup, not WLAN association
or closure of the earlier ordinary-mode freeze. `fwtrace` changes timing and
allocation activity through synchronous filesystem checkpoints. ALIVE was
sampled at +272,717 us, but this is not an uninstrumented performance result.
No ordinary `fwboot` ran on this kernel in this boot. The next control uses
the same USB/kernel, on a fresh boot, before any `fwtrace` invocation:

```text
wlan probe > wlan-probe-normal.log
wlan fwboot /iwlwifi-cc-a0-77.ucode > wlan-normal.log
```

Wait for each command to return. If both return, run
`dmesg > wlan-normal-dmesg.log`, then `sync`, and wait for `sync: ok` before
powering off and reconnecting. If a command stalls, record which one and
whether keyboard input still responds; do not run further firmware attempts
or rotate away the boot evidence. Do not add an arbitrary delay to make the
ordinary path resemble the traced path without locating a missing contract.

## P1 Ordinary-Mode Control (2026-09-22)

The 23:59 UTC read-only extraction is retained in
`build/realhw-usb-20260921/hardware-readback-20260922-235901/`.
The kernel remains `7ccbad5b...6c3f`. The reader exited zero without USB
writes, and metadata/directory fsck reports clean: 15,953 files, 1,980
directories, 455,614 free blocks, 113,138 free inodes.

`wlan-normal.log` (10,611 bytes, SHA256
`d9f39201ab43cf07c230e71d3ba06642a8a22530785e4a8f8fc70bb40ef4e482`)
confirms RFH and ALIVE ABI 6/status `0xCAFE`, two RX notifications, and
verified shutdown with DMA released. ALIVE was sampled at +137,459 us with
598 polls, without persistent progress callbacks. Bridge BME changed
`0x0003 -> 0x0007`, endpoint BME was cleared after stop, and no newly set AER
UNC/COR bits or active DMAR faults were captured. Endpoint COR `0x2000`
was already set before startup. `diag/boot1.log` contains the returned prompt
and `sync: ok`. The dmesg export is 20,786 bytes, SHA256
`48be9e9fc1e03f850518b5ffb250e2cbd307d534597e9636d8f85a86bdc8f416`.

This permits the next bounded firmware-command step. It is one successful
ordinary boot, not a stress/reliability result or a retrospective explanation
of the earlier freeze on a different kernel.

## Command And NVM Diagnostic

`wlan nvm <path>` and the explicitly slow `wlan nvmtrace <path>` use the same
boot/ownership/shutdown path as `fwboot`. After ALIVE they submit
`INIT_EXTENDED_CFG` with INIT_NVM, then `NVM_ACCESS_COMPLETE`, wait for the
unsolicited `INIT_COMPLETE`, and issue `NVM_GET_INFO`. They do not write NVM
or submit RF frames. The sequence and TFH/wide-header layout follow
[OpenBSD iwx](https://github.com/openbsd/src/blob/master/sys/dev/pci/if_iwx.c);
API-bit selection and NVM layouts were cross-checked against Intel's
[NVM parser](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/iwl-nvm-parse.c)
and [NVM ABI](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/fw/api/nvm-reg.h).

- The firmware plan records API bit 48 and NVM command/response versions.
  Command v1 and response v3/v4 must agree with the API flag before any device
  start. Missing, conflicting or unsupported layouts fail explicitly. The
  ordinary ALIVE diagnostic does not require the extra NVM capabilities.
- The 256-entry TFH command queue uses packed, size-checked 256-byte
  descriptors, 8-byte wide headers and the 20-byte first transfer-buffer
  boundary. A separate mapped DMA page holds the command. The descriptor and
  command are published before the queue-0 write pointer. The v1 commands
  use the upstream default zero wire-version field.
- Only one command may be outstanding; a matching group/opcode/queue/index
  completion must arrive before reusing its data page. No descriptor is
  reused within this bounded session. Unsolicited packets never acknowledge
  a command. Duplicate, mismatched, failed or oversized responses abort.
- These diagnostics drain a one-shot 504-buffer budget, with validated producer,
  buffer IDs and packet extents. Command
  waits have one-second deadlines; INIT_COMPLETE has two seconds, with
  independent iteration bounds. INIT_COMPLETE can precede or follow its
  command ACK; neither substitutes for the other.
- NVM v3/v4 lengths are exactly 128/468 bytes. Channel counts, chain masks
  and LAR representation are checked before publishing the report. Raw
  profiles, SKU, board and NVM version are retained, without inventing a
  regulatory domain or treating those profiles as transmit permission.
- MAC is read under the MAC-access handshake from the OEM strap, falling
  back to OTP when invalid. Multicast, zero and the documented placeholder
  address are rejected. No synthetic MAC is assigned; no-address remains an
  explicit error. The handshake release is checked before more persistence.
- Error and success paths use verified master-stop/BME-clear before freeing
  DMA. A failed stop retains all pages and blocks another diagnostic/kexec;
  it never retains a pointer to a caller's report. The original hardware
  failure is preserved separately from shutdown/persistence failures.

`build/iwx-nvm-v1/` passes all GCC/Clang normal and ASan+UBSan variants:
10,428 transport, 44,741 firmware, 39 PCI access, 141 PCI DMA, 108 platform
diagnostic and 107 persistence checks per variant. Coverage includes both
NVM ABIs, real API-77 metadata, command encoding/split/size limits, response
ownership and malformed input, reordered/duplicate/missing INIT_COMPLETE,
radio/hardware/firmware errors, stopped clocks, MAC fallback, every DMA
allocation failure, retry and every observer failure phase. The normal boot
tests still pass with their existing register-access counts. These mocks
cannot prove that physical firmware accepts the new commands.

The kernel builds with clang/lld (only the existing unused shell-helper
warnings). `build/realhw-usb-20260921/nvm-deploy-20260923/` contains the
6,817,992-byte kernel, SHA256
`a604179ba6825f6f6db1eb5a64bc7a943aa44d4e66a9ec387ded6dddf07eca26`,
the prepared block patch and `qemu-progress/result.json`. That exact bundle
passed UEFI/xHCI boot, OsitoFS3 mount, actual firmware metadata, `nvm` and
`nvmtrace` rejection without a radio, missing-file errors, persistent progress
and byte-exact file/redirection checks. QEMU exited zero and its managed
scratch disks were removed. It has no emulated AX200 command engine.

The prepared filesystem update replaces only `/kernel.elf` and
`/WLANTEST.txt`. The larger kernel requires one additional 64-KiB block
(68674); the host OsitoFS3 writer performs the allocation. Before/after fsck
are clean. The preparer verifies exactly that bitmap bit, the matching
superblock free-count/CRC, no removed/moved kernel blocks, unchanged inode
bitmap and directories, and unchanged other inodes. The physical updater
checks the baseline metadata/GPT/identity, backs up every changed block, then
verifies the written data and preserved logs. Its planned EFI backup is
`EFI/BOOT/KERNNVM.ELF`; preparation/QEMU alone are not proof of deployment.
The Windows elevation request for the read-only physical preflight was
cancelled before the helper started. No physical update was performed and
the USB still contains the preceding `7ccbad5b...6c3f` kernel. The prepared
bundle must pass a fresh physical baseline check before installation; do not
run the NVM retest commands on the old kernel.

## Persistent Transport (Opt-In, Hardware Validation Pending)

`wlan start <path>` and `wlan starttrace <path>` run the same ALIVE/NVM
initialization, but retain the firmware transport after success. These are
not `connect` commands. They do not register a NIC, modify Ethernet, configure
IP, scan, associate or submit RF frames. The ordinary `fwboot` and `nvm`
diagnostics still shut down on success and retain their one-shot RX behavior.

The driver owns the live report and NVM storage, so the shell can release its
firmware buffer and return. A single `wlan-rx` kernel task polls in process
context, sleeping between iterations. Each poll processes at most 64 buffers;
there is no receive-time allocation. A scheduler or worker-creation failure
aborts startup through verified shutdown, rather than leaving an unserviced
live transport. Diagnostic/start/stop/poll/status/kexec operations share the
same nonblocking ownership guard. Status reads are software-only.

RX uses independent circular free/completion cursors. Buffer IDs are owned
by either the driver or hardware, with a full/empty gap and publication in
eight-entry batches, following the AX200 encoding in
[Intel's PCIe RX transport](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/pcie/rx.c).
The driver copies/parses notifications before invalidating and reposting a
buffer. An absolute completion boundary also rejects duplicate IDs in an
older, partially drained snapshot after those IDs have been republished.
Only persistent mode wraps the command queue, and only after a matching ACK;
a timeout poisons the session and cannot be bypassed by sending another
command. IRQ-driven RX and network data-plane delivery remain unimplemented.

`wlan status` distinguishes off, active-but-unassociated, failed, and DMA
quarantined. It reports session generation, polling, packet/buffer/repost
counts, cursors and outstanding/pending buffers. The last counters remain
available after shutdown; they do not imply current device ownership. Active
DMA is separate from quarantined DMA. Runtime failures stop further RX and
attempt verified shutdown, preserving the error. Failed master-stop/BME-clear
pins the allocations, blocks restart/kexec and permits an explicit stop retry.
`wlan stop` also stops an active runtime; the idle worker is reused on restart.

Host coverage exercises both NVM ABIs, caller-report lifetime, one-worker
restart, handoff serialization, circular RX under bounded draining, multiple
ring wraps, partial batches, full outstanding sets, 600 additional commands,
duplicate IDs within and across budgets, malformed DMA data, unsolicited
errors, RF/thermal switches, worker creation failure, observer failures and
DMA quarantine/retry. These mocks do not validate the physical AX200 engine.

The frozen `nvm-deploy-20260923` bundle remains the physical NVM control; it
does not contain this persistent mode. First establish that firmware commands
and NVM work on the P1, then validate persistent start/status/stop across
repeated sessions before enabling scanning or network traffic.

Validation artifacts for the persistent implementation:

- `build/iwx-runtime-v2/`: all 24 GCC/Clang normal and ASan+UBSan executables
  exit zero. Per variant: 1,785,196 transport assertions (including repeated
  per-buffer sentinel checks), 44,741 firmware, 39 PCI access, 141 PCI DMA,
  108 platform and 107 persistence checks. Frozen driver/parser/test sources
  match the working files. The large assertion count is not a count of
  distinct hardware scenarios.
- The clang/lld kernel is 6,830,728 bytes, SHA256
  `310f7237ddfe80034126042691aa241811e01fefe0581a9247549ab1e6254acf`.
  `build/iwx-runtime-qemu-v2/` boots this exact kernel with UEFI, OsitoFS3 and
  virtio-net. NVM/runtime commands correctly reject absent hardware and
  invalid/missing firmware, with no worker, DMA or WLAN link. Persistent
  redirected files are checked byte-for-byte from the stopped VM's disk.
- Existing OFTP tests still pass after the WLAN commands: normal download,
  missing-middle recovery, EOF-before-data, lost-request retry and rejecting
  a changed total size. Downloaded data match the expected 4,097 bytes on disk;
  the incomplete transfer is not saved. QEMU/test runner exited zero and
  managed scratch disks were removed.
- The first runtime QEMU attempt timed out recognizing `ositoo> ` on serial;
  it also lacked `/diag`, so trace checkpoints correctly refused to proceed.
  The test image now creates `/diag`. Only prompt recognition tolerates UART
  duplicate characters; diagnostic/payload file checks remain exact. This is
  not a UART-driver fix or a physical AX200 success.

The repeated Windows UAC request was cancelled before the physical preflight
helper started. Neither preflight/update evidence directory exists and no
USB writes occurred. The USB still needs the frozen NVM kernel installed;
the runtime kernel above is not deployed. No elevated helper remains running.

## Passive Discovery (Opt-In, Hardware Validation Pending)

The running transport accepts `wlan scan`, `wlan networks` and `wlan abort`.
Startup remains separate: `wlan start` alone does not configure or scan the
radio. Scanning initializes the antenna masks from PHY/NVM, coexistence and
discrete-AX200 SoC settings, firmware-owned thermal control, awake device
power, current regulatory profile, reduced scan configuration and disabled
beacon filtering. LTR is configured only if PCIe Device Control 2 enables it.
DQA uses group 5, opcode 0, and is sent only when CAPA_DQA is set. This specific
CC API-77 image neither sets that capability nor lists the legacy DQA command;
PHY group 4, opcode 0 is a different command and must not be mistaken for DQA.

The firmware parser now owns copies of API/capability words, the scan-channel
limit and targeted command/notification versions. Duplicate recognized words
and command IDs are rejected. Scanning rejects unsupported or missing ABI
metadata before issuing radio commands. The implemented layouts are scan
request v15 (1,940 bytes), reduced configuration v5, MCC response v5 and AX200
RX MPDU notification v4 with a 48-byte descriptor. These were checked against
Intel's primary [scan ABI](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/fw/api/scan.h)
and [regulatory ABI](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/fw/api/nvm-reg.h),
along with the OpenBSD initialization reference above. Selected payload
versions do not change the default-zero wide command header version.

MCC GET_CURRENT/ZZ queries the firmware/NVM profile, rather than choosing a
country. The fixed AX200 51-entry channel-number table describes the index
encoding, not permission: only firmware VALID entries in NVM-enabled bands
are selected. Raw restriction bits and returned country/source are retained.
No six-GHz channels, active probe requests or passive-to-active promotion are
enabled. A CHUB regulatory update invalidates the cached profile; an active
scan is cancelled and another scan must apply the notified MCC/source and
validate the resulting firmware profile. Reading back the old profile alone
does not acknowledge a CHUB transition. A query
racing a change is retried once, not accepted as a stable policy snapshot.

After bounded radio-command acknowledgements, the scan returns to the shell.
The worker receives beacons/probe responses and reports running, complete,
aborting, aborted or failed. UID zero owns the single firmware scan slot; it
is not reused while a scan or abort is pending. The final completion is the
legacy-group-zero notification, distinct from the group-one scan command ACK.
Completion before its ACK is handled. Iteration notifications alone are not
proof that the slot is free. A 30-second scan deadline submits abort; an
unacknowledged command or absent abort completion within two seconds stops
the transport. Abort NOT_FOUND explicitly releases firmware slot ownership.
Failed hardware stop still quarantines every DMA allocation.

The bounded 128-BSS cache deduplicates by BSSID/channel, counts overflow and
observations, and preserves a previously visible SSID when a later beacon
hides it. RX length/CRC, management-frame type, padding, fragmentation and
information-element bounds are checked before copying. Malformed over-the-air
frames are dropped/counted without crashing the driver. SSIDs are binary data:
shell output escapes control characters, quotes, backslashes and non-ASCII
bytes. RSN/WPA/privacy labels describe advertised IEs, not authenticated
security or support for a key exchange. No RX allocation or NIC registration
is performed by discovery.

After deploying this kernel and establishing the preceding NVM/runtime gates:

```text
wlan start /iwlwifi-cc-a0-77.ucode > wlan-start.log
wlan scan > wlan-scan.log
wlan networks
wlan networks > wlan-networks.log
wlan status > wlan-scan-status.log
wlan stop > wlan-scan-stop.log
dmesg > wlan-scan-dmesg.log
sync
```

Use `wlan networks` again after the state changes from running; an immediate
zero-result snapshot is not a failed scan. `wlan abort` cancels an active scan.
Status includes the last firmware command ID/index, response length and ACK
counts. These commands are **not present on the currently installed USB
kernel**; the frozen NVM control bundle and old physical kernel remain unchanged.

`build/iwx-scan-v2/` contains 28 successful GCC/Clang normal and ASan+UBSan
executables with frozen source/firmware inputs. Per variant: 2,612,558
transport assertions, 44,741 firmware checks and 41,061 scan/parser checks,
plus the existing PCI/platform/persistence suites. The transport assertion
total includes repeated buffer-sentinel checks, not millions of distinct
scenarios. Coverage includes actual firmware ABI gating, optional DQA/LTR,
radio command wire formats, early/duplicate/stale completion, cancellation,
regulatory churn, malformed MPDUs, all radio-command failure points, abort
status validation, timeouts, quarantine and stop retry. Host tests and QEMU
without an AX200 cannot establish RF reception or WLAN Internet access.

The current clang/lld kernel is 6,856,128 bytes, SHA256
`2ae99316befcb7cf6b0a135bf422373ae541d5349088e42cc8e15908c403532f`.
`build/iwx-scan-qemu-v3/` boots that exact kernel and passes the existing
OFTP/download matrix, missing/invalid firmware, absent AX200, scan/results/abort
without an active radio, and byte-exact redirected-file readback. All result
gates are true, server/serial error lists are empty, and the runner exits zero.
The driver and host-test sources match the frozen `iwx-scan-v2` inputs.

The first two QEMU runs are failures, retained as such: serial input/prompt
progress stopped. The second run's timeout QMP snapshot finds CPU0 in
`uart_status`, with the other CPUs idle. The runner had no socket-output
consumer; merely keeping the per-command connection open was insufficient.
It now maintains one serial connection and continuously drains/captures its
output while observing the QEMU log for prompts. Timeout captures also retain
register/IRQ evidence. The successful third run uses this corrected peer;
file/payload assertions were not weakened and no UART kernel code was changed.
This does not certify the physical UART or erase the failed runs.

No elevation was retried and no physical USB update was made in this stage.
The next physical gate remains the frozen NVM control, followed by persistent
RX ownership and passive scan reception on the P1.

## Managed association implementation (2026-09-23)

`wlan associate wpa2 <BSSID>` (or `wlan associate open <BSSID>`) is an opt-in management-frame operation, not
`connect` and not an Internet test. The BSSID must occur exactly once in the
completed scan cache. The driver rereads the firmware regulatory profile
before building the association. This first implementation requires VALID and
ACTIVE on the selected channel; scan reception alone does not grant transmit
permission. Passive/DFS association policy is not generalized by guessing a
country or overriding firmware restrictions.

The supported baseline is an infrastructure BSS with a visible SSID, common
legacy basic rates, 20 MHz, SISO and no QoS/aggregation. `open` rejects every
advertised privacy/WPA/RSN network. `wpa2` selects PSK and CCMP for both pairwise
and group cipher, rejecting missing suites, malformed RSN/PMKID lengths and
required management-frame protection. WPA1/TKIP, enterprise authentication,
SAE/WPA3-only and required PMF are not silently downgraded. No password is
accepted or logged by this command. Open System authentication is also the
802.11 step used before WPA2's separate four-way handshake; it does not mean
that secured data can be sent unencrypted.

Firmware layouts are gated against the actual API-77 metadata: PHY context v4
with the UHB-sized **channel structure** (not 6 GHz radio support), RLC v2,
MAC context v5, binding v2, ADD_STA v12, SCD queue v3/response v2, TX v9/response
v7 and session protection v1/notification v2. The RLC command carries wire
header version 2; the other commands retain their documented default header
version. Sizes and critical offsets have compile-time assertions. Primary
references are [OpenBSD's AX200 driver](https://github.com/openbsd/src/blob/master/sys/dev/pci/if_iwx.c),
[Intel station ABI](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/fw/api/sta.h)
and [Intel TX/queue ABI](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/fw/api/txq.h).

Association allocates 72 KiB of tracked DMA: a 256-descriptor TFH ring, byte
count table and one management-frame page. Queue ID comes from firmware; zero,
out-of-range IDs, failure flags or a nonzero initial cursor were rejected at
this checkpoint (superseded by the later TX queue and payload-flags fixes). One
frame is in flight at a time. TX has its own legacy header, queue and completion
cursor, separate from command ACKs. Matching AP responses can arrive before or
after TX completion but cannot recycle a DMA buffer. Wrong queue/index, stale
completion, impossible scheduler cursor, a failed transmission or missing
completion causes verified device shutdown; failed shutdown quarantines the
whole transport including the three new allocations. `wlan stop` and the
existing `kexec` guard cover these allocations too.

MAC/PHY/binding/station creation precedes session protection and the first
authentication frame. The driver waits for the firmware's session-start
notification, then for the selected AP's authentication response and association
response. Each stage has a one-second deadline; firmware session protection is
requested for 3000 TU. Peer address, destination, channel, CRC, frame class,
fragmentation, length, status and AID are checked. Rejections retain the AP's
status/reason. Malformed or unrelated RF traffic is counted/dropped, not trusted
as a command completion. Disassociation/deauthentication from the selected AP
closes the transport. A regulatory change or RF-kill also stops it.

Only after a valid AP response and the MAC-context update ACK is the state
`associated (data port closed)`. This is **not** authorization, DHCP or Internet.
The path deliberately has no Ethernet send/receive callback yet. Scan and a
second association are rejected while the link owns the context; stop/start
is the explicit reset path. The cache now retains supported rates, TIM, ERP,
TSF/system timestamps and RSN bytes. ACK rate masks are computed per modulation
class; DCF contention, ERP protection and Barker-preamble restrictions are
applied without advertising unimplemented HT/QoS features.

After physical NVM/runtime/scan validation and deployment, use a BSSID returned
by `wlan networks` on a network you control:

```text
wlan associate wpa2 <BSSID>
wlan status > wlan-association.log
wlan stop > wlan-association-stop.log
dmesg > wlan-association-dmesg.log
sync
```

Do not enter credentials here. A successful test should show AID nonzero,
management TX sent/completed `2/2`, no failed TX and the closed-port association
state. The AP may subsequently disconnect because key exchange is still
unimplemented. None of these commands have been deployed to the physical USB.

`build/iwx-link-v2/` contains 32 successful GCC/Clang normal and ASan+UBSan
executables with frozen source/firmware inputs. Per variant: 4,131,266 transport
assertions, 44,741 firmware checks, 41,072 scan checks and 577,559 association/
frame checks, plus the existing PCI/platform/checkpoint suites. Counts include
repeated buffer, cursor and field assertions, not distinct scenarios. Coverage
includes real firmware ABI selection, RSN suite/length rejection, every 16-bit
AID value, deterministic input mutations, command/queue/status failures, all
three allocation failures, notification ordering, TX wrap, RF-kill, regulatory
invalidation, deadlines, quarantine and kexec exclusion. The earlier `iwx-link-v1`
also passed, before the ACK/ERP and response-capability review refinements.

The final clang/lld kernel is 6,873,184 bytes, SHA256
`f773c977be79d50aa07e4e1525a77232da21c9a8c1a45eaa346eadab626d6a67`.
`build/iwx-link-qemu-v2/` boots that exact artifact and all 11 result gates pass:
OFTP retry/reordering/readback, absent-radio NVM/runtime/scan/association,
invalid/missing firmware and unchanged Ethernet networking. No WLAN NIC is
advertised. Server/serial error lists are empty and the runner exits zero.
The kernel, bootloader, shell and runner match their frozen inputs; all 27 host
matrix source inputs match `iwx-link-v2`. Both QEMU scratch-disk runs were
cleaned. `iwx-link-qemu-v1` also passed before correcting two stale shell
messages; neither run emulates AX200 radio or certifies over-the-air association.
No physical USB update or elevation retry was performed.

## WPA2 protocol core (2026-09-23)

`kernel/wpa2.c` implements the station-side initial four-way exchange and group
key renewal for WPA2-PSK/CCMP. Its first checkpoint was a standalone engine;
the driver integration added afterward is described below. It is still **not
an IP-connected WLAN interface**. No password, PMK, key or
nonce is printed or persisted by the core. The caller must serialize access,
honor the controlled-port state, and wipe the context on every disconnect.

The association and EAPOL paths now share `kernel/wpa2_rsn.c` and the exact
selected RSN IE. The core binds message 3 to the selected AP's advertised RSN,
checks the peer, descriptor type/version, lengths, key-info flags, ANonce,
replay counter and HMAC before parsing key material. PMKID in message 1 is
checked when present. AES-wrapped key data must pass its integrity check;
plaintext GTK, duplicate/malformed KDEs and unnegotiated PMF/key-ID material
are rejected. A plaintext RSN-only message 3 is allowed, but no data is
authorized until a later encrypted group exchange installs the GTK.

The driver-facing contract explicitly separates receipt, key installation and
TX completion. Keys have a generation token; the caller must report actual
firmware ACKs for all requested keys before acknowledging installation. TX
has a separate token and remembers that generation. An old TX completion
cannot authorize a newer key generation. Matching retransmissions only
regenerate acknowledgements: they never reinstall a PTK/GTK or reset a packet
number. A GTK cannot be reused in a different slot. Reordered message 3 versus
message 2 completion, and a new group exchange before the previous response's
TX completion, are covered. Handshake/TX deadlines close the port and wipe the
owned secret buffers on failure. The CCMP adapter below implements host RX PN
checks; actual radio encryption/decryption and firmware TX PN behavior still
require physical validation.

`random_secure_bytes()` is a separate, checked API using the CPU's hardware
DRBG: capability gate, carry-flag check, at most ten attempts per word and
all-output clearing on failure. It never falls back to TSC/xorshift and does
not change existing callers of `random_get_bytes()` or `hw_random64()`. A CPU
without usable RDRAND fails this API; a general software entropy pool/CSPRNG
is still separate work. This follows the failure/iteration contract in
[Intel's DRNG guide](https://www.intel.com/content/www/us/en/developer/articles/guide/intel-digital-random-number-generator-drng-software-implementation-guide.html).

`kernel/wpa2_crypto.c` uses a pinned, unmodified subset of
[BearSSL 0.6](https://bearssl.org/bearssl-0.6.tar.gz) for SHA-1/HMAC and constant-time
AES decryption. It implements the WPA2-specific PBKDF2/PRF composition and
[RFC 3394 unwrap](https://www.rfc-editor.org/rfc/rfc3394.html). It does not replace
the kernel TLS implementation or use BearSSL's RNG. The archive hash, license
and exact selected sources are recorded under `lib/bearssl/`. Protocol
cross-checks use [wpa_supplicant 2.11](https://w1.fi/releases/wpa_supplicant-2.11.tar.gz)
(`src/rsn_supp/wpa.c`, `src/crypto/sha1-pbkdf2.c`).

`bash arch/x86/scripts/test-wpa2.sh arch/x86/build/wpa2-v3`, run from the
repository root, passes all four GCC/Clang normal and ASan+UBSan variants:
8,705,816 assertions each. Counts include repeated output-canary checks, not
distinct test cases. It needs host OpenSSL development headers/library solely
as an independent oracle; the kernel has no OpenSSL dependency. Coverage includes
known-answer HMAC/AES-wrap vectors, PBKDF2 and PTK comparisons, all supported
unwrap lengths and one-bit corruption at every byte, complete EAPOL exchanges,
bad MIC/RSN/KDE/PMKID, replay/reordering, duplicate keys, delayed GTK, group
renewal, stale completions, output bounds, RNG absence/retry/partial failure,
deadlines and explicit key-install/TX failures. All 35 frozen inputs match the
working sources. The 32 `iwx-wpa2-rsn-v1` transport/parser/PCI test variants also
pass after sharing the RSN parser.

The first `wpa2-v1` run failed Clang's function-type sanitizer on BearSSL 0.6's
generic hash-vtable casts; it is retained as a failure. Signature-correct SHA-1
adapters in the consumer resolve it without modifying upstream primitives or
disabling any sanitizer. `wpa2-v2` and the expanded `wpa2-v3` then pass. The main
clang/lld kernel builds with integer-only WPA2/crypto objects. The TCC make path
reuses those compiled objects; the complete TCC and minimal kernels were not
validated in this stage.

The built kernel is 6,900,200 bytes, SHA256
`0fb11c4570abc1a35afb82e254f12ce3e2084808372fa008c992142966f024a8`.
`build/wpa2-qemu-v1/` boots that exact artifact: all 11 existing result gates
pass, error lists are empty and the runner exits zero. Its managed scratch
disk was cleaned. This proves the boot/absent-radio/Ethernet/OFTP regression,
not a WLAN handshake; QEMU does not emulate AX200 and at this earlier checkpoint
the driver did not call the WPA2 core. All 23 imported BearSSL source/header/license
files were verified byte-identical to the pinned archive.

Remaining integration after the driver work below: secure credential input
outside command history/logging, bounded Ethernet TX/RX queues and NIC/IP
activation. The core does
not implement an in-place PTK rekey, roaming, PMF, WPA3 or enterprise EAP; an
unsolicited message 1 after installation does not replace active keys. PTK
renewal needs an explicit reconnect or a separately validated rekey transaction.
No physical USB write/elevation retry was performed.

## AX200 WPA2 transport integration (2026-09-23)

`iwx_connect_psk(bssid, pmk)` starts association and the real WPA2 engine under
the transport ownership lock. It consumes a caller-derived 32-byte PMK, never
a shell password; the caller must erase its copy after return. The checked
RDRAND nonce path must succeed before management TX starts. The old
`iwx_associate()` API remains useful for credential-free diagnostics and does
not authorize data. There is no user-facing connect command yet.

EAPOL RX validates the selected AP transmitter/source, station destination,
channel, frame shape and LLC before copying into a four-entry bounded queue.
Message 1 arriving between the association response and MAC-context ACK is
retained. The RX callback never performs crypto or sends a firmware command:
the process-context pump handles the queue after response draining. Overflow
drops frames and lets the peer retransmit; it cannot extend the handshake
deadline or allocate unbounded memory. Link status exposes EAPOL accepted/dropped
and completed key-transaction counts, never key material.

The pinned CC firmware advertises ADD_STA_KEY command v3. Its 76-byte layout
is checked with size/offset assertions and the TLV version is required before
starting WPA2. See Intel's [station ABI](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/wireless/intel/iwlwifi/fw/api/sta.h)
and [key-command caller](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/wireless/intel/iwlwifi/mvm/sta.c).
Only CCMP-128 is installed, on AP station zero: slot zero for PTK and slots
one through four for GTK IDs zero through three. RX replay enforcement is in
the host, as in the legacy key path of [OpenBSD iwx](https://raw.githubusercontent.com/openbsd/src/master/sys/dev/pci/if_iwx.c);
firmware receive-counter bytes remain zero. New firmware TX counters start
at zero only for a new key. A retransmitted EAPOL message does not reinstall
keys or reset either direction's PN.

Every required key must receive the exact firmware command ACK and success
status before committing the generation. Partial installation, malformed
status, timeout, AP disconnect or changed regulatory permissions abort the
transport. The matching EAPOL TX completion, not command submission or an AP
response, opens the controlled port. A group rekey closes it until the new
transaction is confirmed. A single owned TX buffer serves management and
non-QoS EAPOL; M2/M4 are clear, G2 is encrypted by firmware using the PTK.

`iwx_data.c` supplies bounded RFC1042 Ethernet/802.11 conversion for the later
NIC adapter, limited to non-QoS, unfragmented managed traffic and a 1500-byte
payload. CCMP reception requires firmware CRC/overflow/MIC/decryption success,
an installed matching key and a strictly increasing 48-bit PN for that key.
The adapter handles padding after the MAC header and CCMP IV. It uses the
AX200 RX contract where the IV remains but MIC is stripped; compare the
[iwlwifi RX crypto path](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/wireless/intel/iwlwifi/mvm/rxmq.c).
Frames outside the negotiated baseline, plaintext ordinary data on an RSN
link, forged-source EAPOL and stale PN are dropped. The current driver dispatches
only EAPOL from this adapter; ordinary Ethernet delivery is not wired yet.

Failure, stop, restart and kexec handoff erase the software WPA context and
queued EAPOL. Key-command DMA is erased after its exact ACK. A timed-out DMA
buffer is not touched while device ownership is uncertain: it remains pinned
and is erased only after verified quiescence, before being freed. No successful
shutdown is invented to obtain cleanup.

Validation artifacts:

- `build/iwx-wpa2-link-v4/`: all 36 GCC/Clang normal and ASan+UBSan suites pass.
  Transport has 4,786,716 assertions per variant, data ABI 161,361, firmware
  44,742, scan 41,072 and association 577,559; these include repeated checks,
  not that many distinct cases. All 65 frozen inputs match working sources.
  The simulated AP uses OpenSSL in a separate host translation unit to wrap
  GTKs and compute EAPOL MICs; kernel primitives remain BearSSL. It exercises
  early M1, M3 before M2 completion, M4 gating, repeated M3/G1, GTK renewal,
  descriptor flags, rejection/timeout/disconnect during either key command,
  missing/failed M4 TX completion, queue overflow, kexec and secret-buffer
  erasure with DMA quarantine. The earlier v3 also passed all 36 variants.
- The data contract tests cover every payload length through MTU with clear/
  protected, padded/unpadded and all group-key IDs, plus truncated frames,
  address/LLC/security failures, PN saturation and TX descriptor bounds.
- The runner now compiles its frozen source tree, not the mutable working tree.
  It requires host OpenSSL development headers and `libcrypto` for the simulated
  AP only. The first integration runs are retained: v1 caught a wrong BSS field
  and hosted/freestanding header collision; v2 caught a test bounds warning.
- The main clang/lld kernel builds at 6,908,976 bytes, SHA256
  `8b68dd81c027950e75628839042f718d0a74df182466c222671fbdde81d02535`.
  Minimal/TCC object lists include the new dependencies but those complete
  build variants have not been validated. The main build reports the two
  pre-existing unused shell helper warnings.
- `build/iwx-wpa2-qemu-v1/result.json` confirms that exact kernel hash and all
  11 existing boot/absent-radio/firmware/Ethernet/OFTP/readback gates. Server
  and serial error lists are empty, the runner exits zero and its managed
  scratch directory is removed. It does not emulate AX200 or validate RF/CCMP
  in hardware. Scoped `git diff --check` and the new-source LF/whitespace
  audit pass.

No USB update or physical WPA2 success is claimed by this checkpoint.

## Ethernet/IP and console connection checkpoint (2026-09-23)

The driver now exposes `iwx_send/recv`, coherent link snapshots and a bounded
16-packet queue in each direction. A separate EAPOL queue retains authentication
priority. NIC callbacks do not wait on firmware or recursively enter `net_poll`.
They reject busy/full/closed-port sends, unsupported Ethernet lengths/types,
foreign source MACs and caller-injected EAPOL. Normal data TX without an ACK
is packet loss, not a driver failure. Missing/invalid completions still require
verified shutdown or DMA quarantine. Stop/failure wipes both software queues;
DMA ownership rules are unchanged.

`main.c` initializes the same network services for a selected, initially-down
AX200. Existing Ethernet keeps priority. There is still one active NIC, not
runtime interface switching, routing across interfaces or concurrent WLAN and
Ethernet. `net_init` runs once. Polled link generation/MAC/carrier changes clear
IPv4 configuration, ARP and pending DNS, close old TCP connections and fail
old async requests. They preserve UDP/TCP listeners and claimed socket handles
until their owners release them. Busy snapshot reads are not link-down events;
GTK rotation does not create a new IP network.

The local connection command pins the observed SSID/channel/RSN identity before
PBKDF2 and rechecks it under driver ownership. Credentials are not command-line
arguments. `term_read_secret` uses a caller-owned buffer, no echo/history, drains
overflow through Enter and clears cancelled/invalid input. The caller wipes the
password and its PMK copy; WPA state is erased on stop/failure. Consumed keyboard
ring bytes are cleared too. This is not a guarantee that all input-device event
queues or an external serial-console recorder have been scrubbed. Use a trusted
console. Noninteractive/redirected `wlan connect` is rejected.

DHCP now validates reply ports, transaction/client identity, selected server,
message phase, option bounds and lease/mask fields before publication. It handles
the option-overload file/sname fields, retries lost selection REQUESTs and commits
configuration only against its live link epoch. Missing router/DNS values clear
old values. Changes during DHCP/APIPA cancel their wait/commit paths. APIPA
commits an explicitly empty gateway/DNS; it is local-only, not an Internet
fallback. The parsing/phase rules follow the relevant portions of
[RFC 2131](https://www.rfc-editor.org/rfc/rfc2131) and
[RFC 2132](https://www.rfc-editor.org/rfc/rfc2132), not a claim of full conformance.
`ifconfig` identifies the selected backend and carrier; `route` prints actual
IPv4 values rather than an unconditional placeholder default route.

### Verification artifacts

All paths below are relative to `arch/x86/build/`. Each host suite passed with
GCC and Clang, both normally and with ASan/UBSan and halt-on-error enabled:

- `wlan-network-v2/`: 40 suite executions. Per variant, transport has 4,951,222
  checks and integrated AX200/NIC/DHCP/ARP/DNS/ICMP has 73,664. The AP simulator
  exercises open and WPA2 data, stop/address cleanup and cancellation of pending
  DNS/TCP/ARP operations exactly once. Firmware, scan, association, data ABI,
  PCI, DMA and boot-checkpoint suites also pass. Firmware decryption/RF remain
  simulated, not hardware evidence.
- `dhcp-link-v3/`: eight executions; DHCP has 203,856 checks and APIPA 43 per
  variant, including malformed replies, retries and link changes before commit.
- `net-link-v1/`: four executions with 36 checks each for generation changes,
  busy snapshots, address/DNS/TCP cleanup and listener/handle preservation.
- `keyboard-secret-v1/`: eight executions; keyboard has 4,067 checks and secret
  terminal input 2,398 per variant, including cancellation, overflow and history.
- `wlan-nic-qemu-v2/result.json`: all 12 gates pass with no serial/server errors,
  including Ethernet diagnostics, honest AX200 absence and exact OFTP readback
  under loss/retry. The managed runner removed its scratch disks.

The frozen inputs for these runs match the working sources. The clang x86
kernel build (`CLANG=1 QUIET=1 SKIP_MODEL=1`) succeeds; the tested kernel is
6,926,536 bytes, SHA256
`41c341dad6af854888728d1a0ac6c19cb0b82096d829bcbf5a42407edc379819`.
Minimal/TCC/WASM builds were not validated in this checkpoint. The kernel is
available at `arch/x86/build/kernel.elf`; USB installation is recorded below.

### ESP-only deployment bundle

`arch/x86/build/wlan-connect-deploy-20260923/` contains the frozen kernel,
QEMU result/serial log, `README.txt`, `apply-kernel.ps1` and `selftest.ps1`.
The updater requires an already-elevated PowerShell session and defaults to
read-only preflight; `-Write` performs that preflight and installs the kernel.
It matches the development USB by serial/bus/geometry/volume, not disk number,
and pins the old/new kernel, bootloader, GPT, OsitoFS metadata and test evidence.
Only ESP files are written. It preserves `KERNWLAN.ELF` and a local backup,
checks flush/readback, and attempts a verified rollback after rename failures.
Unknown files or changed media stop the update; they are never overwritten.
Two FAT renames are not power-loss atomic. The USB must not be unplugged during
installation, and a Windows readback is not a subsequent successful hardware boot.

The local helper test passed 120 checks without opening a physical device.
Evidence is in `selftest-ac4f91b783b54f90abdbf33bed18d0eb/result.json` inside the
bundle; helper SHA256 is
`e9ea99046297705bdf2358e17b90523b43fce91859ce4ec3352e7858dfe9a6b9`.
The original unelevated attempts could not access the ESP. On the user's retry,
UAC allowed the pinned updater to run. It exited zero at 2026-09-23 20:17:32 UTC;
evidence is in `20260923-171722-ffafcc06a16d4cdfa08afc35b3368334/` inside the bundle.
`update-result.json` records `Passed=true`, `Wrote=true`, the expected installed
kernel hash and a flushed cache. The old kernel is backed up locally and as
`EFI/BOOT/KERNWLAN.ELF`. Other boot files, GPT and OsitoFS metadata were unchanged
(`b235de960524d7df1d99e908f9225ec1c2c6456086f30e7536190febf7ce3efa`).
The helper's 120 local tests were rerun before installation, with evidence in
`selftest-7eebd40a667646e4990f7875168cd81d/`.
A separate `verify-installed.ps1` read-only postflight was requested, but its
UAC elevation was cancelled before the child process started. No independent
postflight is claimed; `postflight-uac-cancelled.txt` records that outcome.
The completed installer's own hash/readback/flush and preservation checks remain
valid, and no rollback was performed. Neither updater nor verifier is running.

The bundle is now installed, not just prepared. It leaves the old OsitoFS
`/kernel.elf` and `WLANTEST.txt` unchanged: boot through UEFI and use the bundle's
test steps, not a `kexec` of that old filesystem copy. The firmware file is
already present. Do not rerun the installer against this updated USB: its
old-kernel/absent-backup guards intentionally reject a second installation.

### Physical test sequence for the new kernel

First complete the NVM/runtime/scan hardware gates below. When those succeed:

```text
wlan start /iwlwifi-cc-a0-77.ucode
wlan scan
wlan networks
wlan connect wpa2 <observed-BSSID>
ifconfig
route
ping <DHCP-gateway>
resolve example.com
curl example.com /
wlan status > wlan-connect.log
dmesg > wlan-connect-dmesg.log
sync
```

Wait for scan completion before choosing a BSSID; the password is entered only
at the separate prompt. `wlan connect open <observed-BSSID>` is for intentionally
open networks. Stop/restart/scan is currently required before another connection.
Record the status and first failing layer; an association is not WPA authorization,
a DHCP address is not a successful DNS/HTTPS test, and a mock AP is not real RF.
The new kernel is installed in the USB ESP. Boot through UEFI for this sequence;
the old ALIVE-only OsitoFS kernel was intentionally not replaced. Successful
installation is not evidence that NVM, association or Internet work on the P1.

### Remaining network work

Physical NVM, regulatory/scan, association and encrypted data have not been
validated. Automatic reconnection, multi-NIC selection, higher rates/HT/QoS,
WPA3/enterprise authentication and hidden-SSID selection remain outside this
connection path. Initial DHCP is usable; background lease renewal/rebinding,
server-provided T1/T2, general option concatenation and full address-conflict
defense still need work. The existing renewal routine is not scheduled
automatically. APIPA's original simplified conflict/probe policy is not a full
RFC 3927 implementation. General stack TX-buffer/SMP serialization and IPv6
link lifecycle are not solved by this IPv4 checkpoint.

## Physical INIT_COMPLETE rejection and contract correction (2026-09-23)

The reconnect was read without writing the device at 20:56:36 UTC. Evidence is
`build/realhw-usb-20260921/hardware-readback-20260923-205634/` (relative to
`arch/x86/`). The ESP still contained the tested `41c341da...` kernel;
`root-logs/wlan-nvm-dmesg.log` is 42,265 bytes, SHA256
`a9ab8459943ea10787bf8ee8fe3f7eeb6dbf43f0d4191bfc499262f745fa945f`.
`diag/latest.txt` selects `boot2.log`; the older `wlan-progress.log` is not the
new NVM run. Metadata-only fsck was clean: 15,954 files, 1,980 directories,
zero errors. Metadata SHA256 is now
`675b40bd80718de5e23993b0fbb32a0440bffbed576c90443295c38dc0a02d97`.

Observed on the P1: upstream bridge BME verified, firmware ALIVE ABI 6/status
0xCAFE, INIT_EXTENDED and NVM_ACCESS_COMPLETE ACKs (2/2), then the fifth RX
packet failed with `invalid/unexpected INIT_COMPLETE notification`. Shutdown
completed and released DMA; the shell returned and saved its log. NVM_GET
was never sent. The old diagnostic did not record this notification's sequence
or payload length, so neither an exact sequence nor a four-byte body is claimed
as an observed hardware fact.

The host was requiring an exact high sequence byte of 0x80 and an empty body.
The [Intel command-header contract in Linux](https://raw.githubusercontent.com/torvalds/linux/v6.6/drivers/net/wireless/intel/iwlwifi/fw/api/cmdhdr.h)
identifies firmware-originated notifications by bit 15, not by the other bits.
The [Linux INIT_COMPLETE waiter](https://raw.githubusercontent.com/torvalds/linux/v6.6/drivers/net/wireless/intel/iwlwifi/mvm/fw.c)
does not decode or require an empty body. The correction treats the bounded body
as opaque, records sequence/length/count, and arms an explicit initialization
wait before sending NVM_ACCESS_COMPLETE. Missing origin, early/duplicate/late
INIT, bad framing and an unconfigured RX queue still fail. Notifications cannot
complete command ACKs or release TX ownership. Scan/MCC/session/MPDU events use
the same origin-bit contract; their payload and state validation remain intact.

The physical log also shows Ethernet `8086:15BB` initialized without carrier,
then selected for DHCP/APIPA. That prevented `wlan connect` from using AX200.
Boot now keeps connected Ethernet priority, but falls through to AX200 when
Ethernet has no carrier. Selection occurs before `net_init`, IRQ/service
startup and DHCP; it neither reinitializes a running stack nor switches back
when a cable is inserted. Without AX200, the existing Ethernet behavior remains.

Regression evidence starts at `build/iwx-init-contract-red/`: the new INIT
variants failed 20 assertions against the old parser. Positive cases cover
notification-before/after-ACK and opaque bodies; negative cases preserve
origin, phase, duplicate, queue and extent guards. The parser tests all 65,536
sequence values for five event types while asserting pending ACK state is
unchanged. Boot-policy tests cover absent/present AX200 and wired carrier up,
down or unavailable. These tests model ABI inputs, not a captured P1 packet.

Validation: `build/iwx-init-contract-v1/` passed all 40 GCC/Clang normal and
ASan/UBSan executions. Transport has 5,869,193 checks and network integration
73,671 per variant. All 74 frozen source files match the working tree.
`build/iwx-init-contract-qemu-v1/result.json` passed all 12 gates without
serial/server errors, retaining Ethernet and OFTP readback under loss/retry.
The managed runner cleaned up its scratch disks. The clang x86 build succeeds
with the existing unused-helper warnings; other architectures were not built.
The tested kernel is 6,926,584 bytes, SHA256
`04bd9ed786f7f8e32fbcf1be095c9689bff91a03be4f4866dca9ff0c6a80f3c0`.

`build/iwx-init-deploy-20260923/` freezes this kernel and QEMU evidence. Its
ESP-only updater uses the current physical readback as its baseline, preserves
the previous kernel as `KERNNVM.ELF` and leaves `KERNWLAN.ELF`, the bootloader
and OsitoFS intact. Local helper self-tests passed 120 checks without opening
a physical device (`selftest-e046ecb83b254244b418ef60fb58c07a/`).

The elevated updater (PID 36568) exited zero at 21:17:43 UTC. Installation
evidence is `20260923-181732-c88a4f2dbcde415890cd3862004f6b68/` within that
bundle. It reports the expected installed hash, cache flush, matching backup,
unchanged other boot files/GPT/data metadata and raw-device read-only access.
The local previous-kernel backup independently matches `41c341da...`.
Updater SHA256:
`e11a1f01f4e3140f16d95dba4e8808bc085a30eed9f0364db727fba522063d79`.
No updater/test VM remains running. This is an installed correction with host
and QEMU evidence, not a successful NVM or RF result on the P1. Boot via UEFI;
the OsitoFS kernel copy remains older. Preserve the existing logs and save the
next run as `wlan-nvm2-dmesg.log`.

## Physical NVM pass (2026-09-23, 21:26 UTC)

The read-only capture is
`build/realhw-usb-20260921/hardware-readback-20260923-212622/`, relative to
`arch/x86/`. `root-logs/wlan-nvm2-dmesg.log` contains 43,083 bytes, SHA256
`516a99c9734d0744c3e0d455ff2299525b6a0234489e038eff9478f4aacfe2da`.
The ESP kernel still matches `04bd9ed7...`. `diag/latest.txt` selects
`boot3.log`, with the completed diagnostic, exported dmesg and `sync: ok`.

Physical results:

- The boot path selected AX200 after detecting Ethernet without carrier.
- ALIVE ABI 6/status 0xCAFE, six RX packets and three matching NVM command ACKs.
- One INIT_COMPLETE notification, sequence **0xC002**, **four payload bytes**.
  These are now observed values, not merely test inputs. Both old assumptions
  (exact high byte 0x80 and empty body) rejected this packet.
- NVM response ABI 4, 468 bytes, version 0x1235, OTP MAC validated, TX/RX masks
  0x3, location-aware regulatory enabled and 51 raw channel profiles.
- 25 progress checkpoints, no deferred MAC-held callbacks, verified device
  stop and all DMA allocations released. No exception or quarantine appears
  in this run. This proves traced NVM read/cleanup, not RF or Internet access.
- Read-only fsck of the metadata/directory snapshot: 15,955 files, 1,980
  directories, **zero errors**. Current metadata SHA256:
  `26da039faeff301922680298a2eeebb3da3f392e3581b8b0ef2e351012e16ad3`.

While checking the next test commands, the shell's connection selection was
found to treat `iwx_scan_results()` as zero-on-success, although it returns
the number of copied entries. Any nonempty successful scan would therefore
be rejected before prompting for a password. The correction accepts a
nonnegative count, requires a completed scan and iterates only the returned
entries, not the potentially larger total in the status snapshot. Unique
BSSID, security, local-console and driver revalidation checks remain intact.

`test/wlan_shell_host.c` includes the real `shell.c` connection function and
simulates the radio/console/network services. It covers 1..128 entries with
the selected BSSID last, open/WPA2 paths, busy/empty/running scans, duplicates,
unknown BSSID, truncated snapshots, noninteractive calls, allocation failures,
security mismatch, hidden SSID, cancelled/invalid credentials, changed driver
identity, authorization timeout/failure and DHCP failure. It does not simulate
RF or replace the separate crypto/driver integration tests. The old shell
fails 778 assertions in `build/wlan-shell-red-v2/`; the earlier `red-v1` directory
records initial test-harness compilation errors, not a production regression.

The corrected shell passes 14,505 checks in each of the four GCC/Clang normal
and ASan/UBSan runs in `build/wlan-shell-v1/`. Its 20 frozen source inputs match
the working tree, as do all 74 inputs of the previous full AX200 transport
suite. No driver, ABI or network-core change was needed. The host runner ignores
GCC's pre-existing clobbered-variable warnings in the unused Win32 shell path;
other warnings remain errors. The clang kernel build succeeds (only the existing
unused reboot/halt helpers warn), producing 6,926,584 bytes, SHA256
`c18995056d0142192b34ad0eb636045ed6b5b308363124329f9f1340f78b6945`.

QEMU passed all 12 gates in `build/wlan-select-qemu-v1/result.json`, with no
serial/server errors and exact OFTP readback. The managed runner removed its
scratch disks. This retains Ethernet/absence diagnostics, not AX200 RF coverage.

Deployment bundle: `build/wlan-select-deploy-20260923/`. Its pinned ESP-only
updater passed 120 local checks without opening a physical disk
(`selftest-e582e54216314a888cb1ff2af8910242/`), then installed successfully via
UAC (PID 2928, exit zero) at 21:38:02 UTC. Evidence:
`20260923-183751-225d62d5f8fd47328c481df52dd18a55/update-result.json`.
Installed readback matches `c1899505...`, with cache flushed. `KERNCON.ELF`
and the local backup preserve `04bd9ed7...`, the kernel that passed physical
NVM. Existing boot files, GPT and OsitoFS metadata remained unchanged. Helper
SHA256: `f92220a5eb5eaaa542a53c20b7e42d52f18c29c0d48f4b5b5b7175ad1c1dcc74`.
No updater or test VM remains running. The on-OsitoFS kernel is still the older
copy; boot the new ESP kernel through UEFI, not `kexec /kernel.elf`.

## Physical passive scan pass (2026-09-23, 21:48 UTC)

Read-only USB capture:
`build/realhw-usb-20260921/hardware-readback-20260923-214803/`. The installed
ESP kernel is still `c1899505...`, the previous connection-selector build.
The following files were exported by the P1, not generated by a mock AP:

| File in root-logs | Bytes | SHA256 |
| --- | ---: | --- |
| wlan-networks2.log | 570 | 91b3ff3164ce5c1ed198dfc8ea458d5b7e380a2d86f8fea571147e6660c9f87d |
| wlan-runtime2.log | 860 | c4fbdde6af2802f4d82788f47c5669222fa831b38c049b7170fb116baab1d0dd |
| wlan-scan2-dmesg.log | 42826 | 17e78849bfa08b3835531e211b57ad7ef5f45b7097ccfb22362ff0d3f67c93a2 |

The ordinary persistent runtime has its RX worker started and owns 3,555,328
DMA bytes. All 13 firmware commands completed. RX counters are 29 consumed
buffers, 24 reposted and 31 packets. Scan generation 1 completed with seven
networks on 2.4/5 GHz, zero malformed/ignored/overflow entries, completion 1
and EBS status 3. Two entries have hidden SSIDs. No network is selected or
hardcoded from this capture.

The report ends with `regulatory profile not current`. A firmware MCC-change
notification invalidates that cached profile and records a pending update.
`associate()` drains RX, acknowledges/requeries the pending MCC and checks
active-channel permissions before TX. Discovery is not permission to transmit;
neither the assistant nor the driver forces a country or clears no-TX flags.
Whether the firmware will authorize the selected channel is still a physical
connection checkpoint, not something the scan result proves.

Link state is idle, data port closed, no EAPOL/key installs or Ethernet data.
The later stop confirms PCI command 0x0402, quiescence, DMA release and
`sync: ok`. No exception or DMA quarantine is recorded in this run.
Read-only fsck of the metadata/directory snapshot is clean: 15,958 files,
1,980 directories, zero errors. Metadata SHA256:
`75cc6f878eb9eaa4d79fb35b3f76da6a61bac477a934a06f0dd2a4273822ed90`.

## Console connection assistant (2026-09-23)

Start the firmware explicitly, then open the assistant:

```text
wlan start /iwlwifi-cc-a0-77.ucode
wlan connect
```

The assistant uses a completed scan when available, waits for one already in
progress, or starts passive discovery if there is no completed scan. The scan
wait yields to the scheduler and is bounded to 35 seconds. Failure/aborted
scans never expose partial entries as selectable results. A busy transport,
an inactive radio or an existing link is reported without tearing it down.
Changing networks currently requires `wlan stop`, then `wlan start <firmware>`.

Each page shows at most six entries with escaped SSID, BSSID, channel, signal
and security. Select a number, `n`/`p` for pages, `r` for a new scan, or `q`/
empty input to cancel. Ctrl-C at the selection prompt also cancels. The empty
list still permits rescan/cancel. Hidden, legacy WPA/WEP, enterprise-only or
required-PMF networks remain visible but are rejected with a reason before
asking for a password. Security classification uses the same RSN parser as
the driver, not just the presence of an RSN information element.

### Security visibility for unavailable networks (2026-09-24)

The P1 user confirmed `CortesRojas-2.4G` (channel 6, BSSID
`60:d2:dd:37:49:d9`) and `CortesRojas-5G` (channel 161, BSSID
`60:d2:dd:37:49:d4`) are the upstream networks to test. The live
`wlan networks` snapshot at APIPA `169.254.67.96` detects both as RSN;
the old assistant shows only `unavailable`. The old listing does not expose
the rejected suite/capability or its raw RSN element. This is not evidence of
a wrong password, nor proof that either AP uses TKIP, SAE or required PMF.
The unchanged policy accepts PSK/CCMP with optional PMF, not required PMF,
enterprise-only AKMs or unsupported group/pairwise ciphers.

The assistant now displays the precise `wlan_network_error` reason alongside
each unavailable entry. `wlan networks` adds the same connection eligibility,
the capability word and the saved RSN IE payload in bounded 16-byte hex lines.
These are public scan advertisements, not PMK/PTK/GTK material. It uses the
existing copied scan snapshot, remains read-only while connected, and works
over SSH without changing the association or asking for a secret. `wlan status`
does not dump the full scan. No scan/parser, key derivation, negotiation,
cryptography or data-plane checks were relaxed or changed.

`build/wlan-security-report-v2-20260924` passes 146,457 checks in each of four
GCC/Clang normal and ASan/UBSan variants. Coverage includes the existing
PSK/CCMP path, group/pairwise TKIP rejection, enterprise and SAE-only rejection,
required PMF, malformed RSN, hidden/privacy-inconsistent APs, open/legacy modes,
all 0..255 payload lengths, 128 maximum-length entries, SSID/control escaping,
redirection with an active link and non-console caller, and allocation/busy
cleanup. At that checkpoint the physical APs' detailed RSN output was still
required; the subsequent TKIP-group capture and implementation are recorded
below.

The clang kernel build succeeds (7,121,064 bytes, SHA256
`f878e893b3d8e9c8e95ae284a03d744e1b8134e27fd34ab07edacb18657e40e1`).
`build/wlan-security-qemu-20260924/results.json` passes all 16 production
SSH gates, including binary/delta sync, cold-boot persistence and remote
kexec reconnect with the same identity. This validates the diagnostic and
delivery path, not compatibility with either physical CortesRojas AP.

The tested snapshot was committed over authenticated SSH to
`/kernel-wlan-security.elf` on the P1 at `169.254.67.96`. The sync used the
unchanged `/kernel-dhcp-backoff.elf` as a read-only basis: 6,727,848 bytes
transferred, 393,216 bytes reused, 123.97 seconds, with the final protocol
digest accepted. Data writes used 4 KiB requests. Logs are under
`build/p1-wlan-security-20260924/`. The subsequent remote kexec request ended
in an SSH timeout; this alone did not confirm the new kernel booted. A later
authenticated SSH connection at `169.254.243.99` retrieved the new RSN listing,
confirming that the diagnostic kernel and local reconnection worked.
The USB ESP's cold-boot kernel was not replaced.

The direct form remains available for a BSSID from a completed scan:

```text
wlan scan
wlan networks
wlan connect wpa2 aa:bb:cc:dd:ee:ff
wlan connect open aa:bb:cc:dd:ee:ff
```

The two direct lines are alternatives, not a script to execute together.
Use exactly one security argument: `wpa2` for WPA2-PSK, or `open` for an
unsecured network. Never type a pipe between them: `|` is the shell's pipeline
operator, not part of the WLAN syntax. BSSID means the AP address printed by
`wlan networks`; replace the example address, without angle brackets.
`wlan connect` without arguments is also a shell command, not a GUI entry.
The assistant needs the newer kernel; the previous installed kernel only
accepts the direct forms and prints usage for the argument-free command.
Both direct and assistant entry points require the owning interactive console,
no redirected input/output and the selected AX200 NIC. No password argument,
environment variable, file or command-string interpolation is accepted. The
WPA2 password is read without echo/history, used to derive the PMK, then wiped;
the caller's PMK copy is also wiped after the driver call. Network choices
use the normal line editor, so only nonsecret selection text can enter history.

The menu owns a bounded snapshot and never remaps a numeric selection through
a newer cache. It copies the selected BSS and frees the list before prompting
for credentials. The driver revalidates the observed identity before TX.
Selections are parsed from the complete input line, not a truncated prefix.
Progress distinguishes association, WPA2 authorization and DHCP. A DHCP failure
leaves an authorized link available for `dhcp` or explicit `apipa`; it does not
report Internet access or silently switch to link-local addressing.

Regression tests include the real `shell.c`, SSID escaping and RSN parser,
with radio/console/DHCP mocks. `build/wlan-assistant-host-v2/` passes 103,251
checks in each GCC/Clang normal and ASan/UBSan variant. Coverage includes all
1..128 selections, pagination, rescan, empty/failed/aborted/busy scans, timeout,
snapshot races, full-line rejection, cancellation, allocation failures,
malicious SSID display, credential guards, changed identity and DHCP failure.
These tests do not establish RF connectivity. GUI work is explicitly a plan:
[desktop Wi-Fi interface](x86-wlan-ui-plan.md).

Validation also passed all eight keyboard/terminal executions in
`build/wlan-assistant-input-v1/` (4,067 keyboard and 2,398 terminal checks per
compiler/mode). The 24 frozen shell inputs, nine input-suite files and all 74
files of the prior full AX200 regression still match the working tree. The
full 40-run driver suite was not rerun for this shell-only change.

The clang kernel build succeeds with only the existing unused reboot/halt
helper warnings: 6,934,952 bytes, SHA256
`c47a4d3e91bf08b0f00ab5338fcd3d33085b01211562289aa2646b50fe51b66e`.
`build/wlan-assistant-qemu-v1/result.json` passes all 12 gates, including
assistant/direct commands on the unselected AX200 path, malformed arguments,
existing Ethernet and byte-exact OFTP under loss/retry. Serial/server errors
are empty, frozen QEMU inputs match, and managed scratch disks were removed.
No AX200 RF is emulated by this test.

Deployment bundle: `build/wlan-assistant-deploy-20260923/`. Its ESP-only updater
is pinned to the readback above and preserves the previous `c1899505...` kernel
as `KERNWIZ.ELF`. Local updater self-tests passed 120 checks without opening a
physical disk (`selftest-4a97417bc10e474e90ef2048c8e9dd82/`). Updater SHA256:
`84a1d66468308e1240926bf9a6b7ccb73dc74275b6ae6712536b67902d3907b7`.
The per-run `update-result.json` is the installation evidence; passing the
self-test alone does not prove a physical update.

The attempted elevated launch returned Windows' operation-cancelled error
before `Start-Process` returned a child PID. The updater did not start: there
is no installation result directory and no USB write occurred. This assistant
kernel was ready in the bundle but **not installed at that point**. The USB
retained the previous `c1899505...` kernel from the physical scan. The resumed
physical deployment below supersedes that cancelled attempt.

### CLI help clarification (2026-09-23)

Connection/association usage now prints one complete command per alternative,
without the pipe notation. The parser and connection semantics are unchanged:
choose `wpa2` or `open`, never concatenate them. No compositor is required for
the assistant; its absence on the last confirmed USB kernel is a deployment
version difference, not a GUI dependency.

`build/wlan-cli-help-host-v1/` passes 103,349 checks per GCC/Clang normal and
ASan/UBSan variant, with all 24 frozen inputs matching. The tests explicitly
cover both single-mode commands and reject a literal combined security token;
usage has separate assistant/WPA2/open lines. The rebuilt kernel is 6,939,096
bytes, SHA256
`459c80fbf25e4a7f4c5f083233bc46a80d196578416d186cd0cf5fc5bdcc7844`.
Only the existing unused reboot/halt helper warnings remain. The previous
deployment bundle is immutable and still contains `c47a4d3e...`, not this help
follow-up. No additional UAC launch or USB write is performed in this step.

`build/wlan-cli-help-qemu-v1/result.json` passes all 12 gates, including the
separate usage lines emitted by the real shell. No serial/server errors;
OFTP readback remains exact and the runner removes its managed scratch disks.

## Physical assistant deployment (2026-09-23, 22:50 UTC)

After the user reconnected the USB, a fresh read-only capture was taken at
`build/realhw-usb-20260921/hardware-readback-20260923-224802/` (reader PID 24488,
exit zero). The ESP still contained `c1899505...`, confirming why the P1 showed
general WLAN usage for `wlan connect` without arguments. `diag/latest.txt`
selects `boot1.log`; its two usage outputs match the user's photo.
All 26 exported root-log hashes remain unchanged. The metadata/directory
snapshot passes read-only fsck: 15,958 files, 1,980 directories, zero errors.
Fresh metadata SHA256:
`c9fe9de67c578b7e964eeb6481f0ad3512a626c0ba93e804774592663950eeec`.

The latest help/assistant kernel `459c80fb...` and its QEMU evidence were frozen
in `build/wlan-cli-deploy-20260923/`. Only artifact/baseline hash pins changed
in the reviewed ESP updater; its target guards and write/rollback logic did not.
It passed 120 local checks without opening a physical device
(`selftest-66e620063c804fe186144c1f53624550/`). Updater SHA256:
`766d97a8c28c7296d746f39a74e896e2d5db6dab8f6881ae95dddfe20e509234`.

The elevated installer (PID 2772) exited zero. At **22:50:31 UTC**, installed
readback matched
`459c80fbf25e4a7f4c5f083233bc46a80d196578416d186cd0cf5fc5bdcc7844`.
The result is
`20260923-195019-7050222abc574f89b95b232ad807f3fa/update-result.json` within the
bundle. ESP caches were flushed, the old `c1899505...` kernel was preserved as
`EFI/BOOT/KERNWIZ.ELF` and in a local backup, and other boot files, GPT and
OsitoFS metadata remained unchanged. No updater is left running.

The assistant is now **installed on the USB ESP**, not merely compiled.
Boot through UEFI on the P1; do not `kexec /kernel.elf`, since the OsitoFS copy
was deliberately left untouched. Run:

```text
wlan start /iwlwifi-cc-a0-77.ucode
wlan connect
```

Choose the intended network and enter its password only in the hidden prompt.
Before returning the USB for diagnostics, preserve the link/IP state first:

```text
wlan status > wlan-connect-status.log
ifconfig > wlan-connect-ip.log
wlan stop
dmesg > wlan-connect-dmesg.log
sync
```

Require `sync: ok`. Physical WPA2 authorization, data traffic, DHCP and Internet
access still need testing; successful kernel installation does not prove them.

## Physical connection attempts and TX queue contract (2026-09-23)

Read-only USB capture:
`build/realhw-usb-20260921/hardware-readback-20260923-231208/`.
The ESP kernel is the intended `459c80fb...` assistant/help build, so this is
not the previous usage-only shell. `diag/latest.txt` selects `boot2.log`
(62,905 bytes, no truncation); the user's export is `dmesg-2010.log`
(57,610 bytes, SHA256
`eabcbf816dc55fc142a1406d2a971b33dc6d8d05e99a89e2081e7644828f8554`).
The final sync succeeded. Read-only metadata/directory fsck is clean:
15,959 files, 1,980 directories, zero errors. Metadata SHA256 is
`749144059523200ed3aa16475a363c6b5e8f30d7c59af7041b9d05ca48df77a7`.

All three selected-network attempts (two BSSIDs) report
`invalid management queue allocation response` at `boot2.log` lines 862,
1307 and 1732. PHY/MAC/binding/station setup precedes this stage; session
protection and authentication TX do not. These attempts did not test password
validity or DHCP. Separate `only CCMP group cipher is supported` rejections
are security-capability checks, not the TX allocation failure.

Review against Intel's Linux firmware ABI found two invalid assumptions:
AX200 has 512 scheduler queue IDs, not 32; the allocation response carries an
initial write pointer that need not be zero. The echoed command header keeps
only five queue-tag bits, while TX_RSP contains the full queue ID. The scheduler
sequence in a completion must also be reduced to the 256-slot ring before
reclaiming its descriptor. References:
[22000 configuration](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/intel/iwlwifi/cfg/22000.c),
[queue allocation and TX header](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/pcie/tx-gen2.c),
[allocation response ABI](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/intel/iwlwifi/fw/api/txq.h).

The driver now keeps the full assigned queue ID, initializes its cursor from
the firmware, and validates full-ID/index/next-cursor ownership on completion.
Queue zero is still command-owned; out-of-range queues and short responses
cause verified transport shutdown without transmitting. Payload flags were
also rejected here, but that additional check is superseded below.
No AP-specific queue, SSID, credential, regulatory override or fabricated
firmware success was added. The existing single-outstanding-TX design remains.

The link snapshot retains response length, queue ID, flags, initial write
pointer and reserved word across failure cleanup. `wlan status` and immediate
connection failures print them, without credentials. The old physical log
does not identify which response field triggered rejection: these are proven
contract defects, not yet a confirmed explanation of the physical response.

Regression fixtures use an independent scheduler cursor and include queue IDs
1..511, nonzero/full-width initial pointers, ring rollover, a completion for a
different full queue ID sharing the same five-bit tag, failure flags and
DMA cleanup. The pre-fix build rejects the valid allocation fixtures and fails
ring rollover (`build/iwx-txq-contract-red-v2/`); continued negative execution
also aborts when later fixtures reference the already-stopped mock transport.

`build/iwx-txq-contract-v2/` passes all 40 GCC/Clang normal and ASan/UBSan
executions. Each transport run has 7,624,873 checks; NIC/DHCP/ARP/DNS/ICMP has
73,687. The open-network integration uses queue 511 and initial cursor 0x12ff,
while the existing real-crypto WPA2/key-installation integration also passes.
All 74 frozen source inputs match the working tree. The shell suite
`build/wlan-txq-status-v1/` passes 109,326 checks in each of four variants,
including immediate error diagnostics and no credential echo; all 24 inputs
match. The kernel build succeeds with only the pre-existing unused reboot/halt
warnings. Hardware association, WPA2 and IP traffic remain unvalidated.

## TX queue kernel deployment (2026-09-23, 23:32 UTC)

The rebuilt 6,939,096-byte kernel is SHA256
`7effbceddb0925b2f7f0c7edb7bb5b2d2bf1405899ab977220b55b9209109a9e`.
`build/wlan-txq-qemu-v1/` passes all 12 boot/network/OFTP gates, with no serial
or server errors and byte-exact readback. The runner cleaned its scratch disks;
no QEMU or test session remains running. This tests Ethernet and absent-WLAN
behavior, not AX200 RF. QEMU result SHA256:
`ffc19f8a5788d67da45199c913be00805383a93c5001f54a3571b418c2b406f6`;
serial SHA256:
`79a26900d9d575373717eb7f6e9b0261911769d95e52f2035553367a9c93922b`.

The immutable bundle is `build/wlan-txq-deploy-20260923/`. The existing ESP
updater changed only artifact/baseline pins and backup/stage names. Its 120
local guard/rollback tests passed without opening a physical disk
(`selftest-57ab732759684675afe6e3b2a94e7695/`). Updater SHA256:
`93adc583569c8f02d85e70e7fb9ad85dfff0d8dee8e8e0193aae15c1d507da4b`.

Elevated PID 41628 exited zero and was reaped. At **23:32:24 UTC**, installed
readback matched `7effbced...`; the authoritative result within the bundle is
`20260923-203213-1e2469919883451ab7829b015f6f0e72/update-result.json`.
The verified USB serial is `00000000CY02` (disk 4 at installation). The updater
opened the raw device read-only, changed only ESP files, flushed its cache and
verified GPT, OsitoFS metadata and other boot files unchanged. The previous
`459c80fb...` kernel is preserved as `EFI/BOOT/KERNTXQ.ELF` and locally in the
result directory. Historical backups and logs were not deleted.

Safely eject and boot via UEFI, not `kexec /kernel.elf` (the data-partition
kernel is intentionally unchanged). Retest with:

```text
wlan start /iwlwifi-cc-a0-77.ucode
wlan connect
wlan status > wlan-connect-status.log
ifconfig > wlan-connect-ip.log
wlan stop
dmesg > wlan-connect-dmesg.log
sync
```

Enter the intended network and password locally. Require `sync: ok` before
returning the USB. If setup fails again, the link report now includes the raw
TX queue response; do not infer authentication failure from an allocation
failure. Successful Wi-Fi authorization and IP traffic still need the P1 test.

## Physical payload-flags rejection (2026-09-23, 23:40 UTC)

Fresh read-only capture:
`build/realhw-usb-20260921/hardware-readback-20260923-234002/`.
The ESP still contains the intended `7effbced...` kernel. `diag/latest.txt`
selects `boot3.log`: 34,359 bytes, no truncation, SHA256
`31a843494e9172e6b6005dad4e5f2423b9fc97675df2b49b75cf6331aabff547`.
`wlan-connect-status.log` is 1,058 bytes, SHA256
`6b43d7f29182913a32e4187a11f910ee1ad80d5ad72e98bf406098493cc9fbcc`.
The manual dmesg export contains `Unknown command: dmesh`; the automatic
checkpoint retained the full attempt, so no hardware-log information was lost.
The final sync succeeded. Read-only metadata fsck is clean: 15,962 files,
1,980 directories, zero errors. Current metadata SHA256:
`3b5595afbb75daac370b3012fef031c03919e3d7629b909abc33111eed1c1a57`.

The transport completed 20/20 commands and received the 8-byte SCD allocation
response. Link diagnostics show `8/1/1/0/0`: size=8, queue=1, payload flags=1,
initial cursor=0, reserved=0. There were zero management TXs, EAPOL frames or
key installations. Link/IP stayed down and verified cleanup released all DMA.
This narrows the actual rejection to our `if (queue_flags)` check, not the
queue-ID/cursor corrections made in the previous build. The error text
`firmware rejected management queue allocation` was our interpretation;
the command header did not report CMD_FAILED.

Intel's [current allocation implementation](https://github.com/intel/backport-iwlwifi/blob/master/iwlwifi-stack-dev/drivers/net/wireless/intel/iwlwifi/queue/tx.c)
and [Linux v6.12](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/pcie/tx-gen2.c)
consume response length, assigned queue and initial write pointer without
testing the payload `flags`. [OpenBSD](https://github.com/openbsd/src/blob/master/sys/dev/pci/if_iwx.c)
checks CMD_FAILED in the packet header and also does not reject payload flags.
The legacy ABI header comment says "set on failure", but does not specify
bit values and does not match these allocators' acceptance path. We do not
invent a meaning for bit zero or add an exception for flags=1: the driver now
follows that acceptance path and retains the opaque word for diagnostics.

`command_response()` still rejects CMD_FAILED in the header, unexpected IDs or
sequence, malformed RX and timeout. Allocation still validates the exact
8-byte extent, queue bounds and command-queue collision. Authentication,
regulatory gating, actual TX ACKs, WPA2 proofs and port authorization are
unchanged; accepting a queue does not mark the network connected.

The new regression replays the physical `8/1/1/0/0` response, plus zero and
other 16-bit payload flag values. For each, a real header CMD_FAILED or wrong
echoed index must still stop before TX and release DMA. The pre-fix run
`build/iwx-txq-flags-red-v1/` fails five acceptance cases (634,825 checks)
and exits cleanly without cascading into later tests. All happy-path transport
mocks now return payload flags=1, including WPA2 and Ethernet integration.

`build/iwx-txq-flags-v1/` passes all 40 GCC/Clang normal and ASan/UBSan
executions. Transport: 8,262,573 checks per run; NIC/DHCP/ARP/DNS/ICMP:
73,687. All 74 frozen source inputs match. The exact physical response passes
while header failure/sequence corruption, bad allocation extent/queue and
invalid TX completion still shut down with verified DMA cleanup. The full
kernel rebuild succeeds (6,948,176 bytes); warnings remain in unrelated
kernel/Win32/DOS sources, with no new AX200 warning. This is not an RF test.

## Payload-flags bundle and canceled first deployment (2026-09-23)

New kernel SHA256:
`99af84a359699f60c9de88782b250346f5794d31a84382e90d2964fbc223a702`.
`build/wlan-txq-flags-qemu-v1/` passes all 12 boot/network/OFTP gates, with no
serial or server errors and byte-exact readback. Its managed scratch disks
were removed; the QEMU/test sessions finished. Result SHA256:
`e96e07582a78e5cd0ad02f0998bdfa6adf71418efabd2ce70ebe85f1c9040eeb`;
serial SHA256:
`b3f532f4676cc4c24111208e248e2aea0a464151f9b3721acd039249264967a5`.

`build/wlan-txq-flags-deploy-20260923/` freezes those artifacts and a guarded
ESP-only updater. Only pins and backup/stage names changed from the previous
helper. Its local selftest passes 120 checks without opening a physical device
(`selftest-63478b297fea47649e42be450fcf273d/`). Helper SHA256:
`553bb38d26c3ea4a57d6c12daad9497b7b81b501c6d747e9b44a8b64f6074490`.
The backup name is `KERNFLG.ELF`; staging name is `OSFLG.TMP`.

The first elevation request was canceled before `Start-Process` returned a child
process. No helper PID or deployment result directory was created, and no USB
write occurred. The outer shell misleadingly exited zero after its null-process
errors; that is **not** successful installation evidence. That exec session
finished without an automatic retry. The ESP still held `7effbced...` at that
point. The user explicitly requested the successful retry recorded below.

## Payload-flags kernel deployment (2026-09-24, 00:15 UTC)

The retry used terminating launch errors, rechecked the tested helper hash and
USB identity, and required a single successful deployment result. The guarded
updater revalidated geometry, GPT, the old kernel and the metadata baseline
before writing. Elevated PID 37548 exited zero and was reaped. Evidence inside
`build/wlan-txq-flags-deploy-20260923/`:
`20260923-211526-ed11b9201dc84167aad8ccb955f1b87b/update-result.json`.

At **2026-09-24 00:15:37 UTC**, `Passed=true` and `Wrote=true`; installed kernel
readback matches `99af84a359699f60c9de88782b250346f5794d31a84382e90d2964fbc223a702`.
The cache was flushed. The verified USB serial is `00000000CY02` (disk 4).
Raw-device access remained read-only; only ESP files were changed. Bootloader,
other boot files, GPT and OsitoFS metadata stayed unchanged. The metadata hash
remains `3b5595afbb75daac370b3012fef031c03919e3d7629b909abc33111eed1c1a57`.
The previous `7effbced...` kernel is backed up as `EFI/BOOT/KERNFLG.ELF` and
`previous-efi-kernel.elf` in the result directory. Logs were not deleted.

Do not rerun this pinned updater against the updated USB: the old-kernel and
absent-backup guards intentionally reject a repeat. No updater remains running.
Safely eject and boot through UEFI, not `kexec /kernel.elf`; the OsitoFS kernel
copy is intentionally unchanged. The connection test and log commands above
remain valid. Spell the export `dmesg > wlan-connect-dmesg.log`, not `dmesh`;
preserve link status and IP before `wlan stop`, then require `sync: ok`.
Installation does not establish physical association, WPA2 or IP success;
those still require the P1 retest.

## Physical association pass, WPA2 rejection (2026-09-24, 00:32 UTC)

Read-only USB capture (reader PID 29280, exit zero):
`build/realhw-usb-20260921/hardware-readback-20260924-003211/`.
The ESP readback still matches `99af84a3...`. `diag/latest.txt` selects
`diag/boot0.log`, 35,824 bytes, not truncated, SHA256
`2f969c74f9548d296ff7f6e8f981e7e4281831895e1fb4b57c2f52f9d5a7918e`.
The manual dmesg export is now valid (31,397 bytes). The 961-byte status log is
SHA256 `cbe0c01ee27760fb337eb4bddda9e05dc5c5ed9c98df9e92295e78ca5a7bc1d7`.

The assistant scans four networks and reaches authentication, association and
the WPA2 key-exchange wait. Channel 1, AID 1, AP association status zero; TX
sent/completed/failed is 2/2/0. Allocation remains 8/1/1/0/0, now accepted.
Firmware commands complete 22/22. EAPOL accepted/dropped is 0/6 with zero key
installs. The AP disconnects with reason 15, the four-way-handshake timeout
in [hostap's reason codes](https://w1.fi/wpa_supplicant/devel/ieee802__11__defs_8h_source.html).
The port stays closed and `wlan0` has 0.0.0.0. These counters do not identify
the rejected packet fields or prove that the password was wrong.

Shutdown releases all DMA (owned bytes zero); final `sync: ok` is recorded.
The read-only fsck of the sparse local metadata/directory snapshot is clean:
15,962 files, 1,980 directories, zero errors. Current metadata SHA256:
`d0304889c8ef34a6bf0528fbf4afb1377034e6cfdd1f644cf2b65e4413518701`.
All 30 root logs are retained. No writes to the USB were made during capture.

### Bounded WPA2 rejection diagnostics

`wpa2_receive()` now records the last rejection's fixed reason, protocol state,
received/body lengths, version/type/descriptor, key-info bits and key/data
lengths. It does not export keys, nonces, MICs, PMKIDs, replay counters or raw
payloads. The standalone `wpa2_status.h` exposes only this safe snapshot,
without pulling private key state or host/kernel stdint types into the driver
status ABI. No acceptance rule, replay/MIC check or authorization gate changes.

The AX200 copies this bounded snapshot before private WPA2 state is wiped.
It survives AP disconnect and `wlan stop`, but a new session clears it.
Ingress queue/size drops have a separate counter, so they are distinguishable
from parser rejections. Both the immediate connection-failure report and
redirected `wlan status` include the diagnostic.

Validation:

- `build/wpa2-rx-diag-red-v1/`: the pre-instrumentation parser fails the new
  diagnostic assertions (1,196 checks, 828 failures), without changing its
  acceptance behavior.
- `build/wpa2-rx-diag-v2/`: 8,707,014 checks in each GCC/Clang normal and
  ASan/UBSan run. Includes exact-size truncated buffers, descriptor/version/
  flags/length/state failures, PMKID, MIC and unwrap rejection, preserving the
  last rejection across valid input, and wiping private state on reset.
- `build/iwx-rx-diag-v2/`: all 40 runs pass. Transport: 8,371,190 checks per
  variant; NIC/DHCP/ARP/DNS/ICMP: 73,687. Synthetic descriptor and PMKID failures
  exercise six drops followed by reason-15 disconnect; both diagnostics persist
  after DMA/private-state cleanup. These are synthetic cases, not captured
  physical EAPOL payloads. Queue overflow is tested separately.
- `build/wlan-rx-diag-v1/`: 111,235 checks in each of four variants, including
  failure output, redirection, invalid state bounds and no password echo.
- All 75 driver, 36 WPA2 and 25 shell frozen inputs match the working tree.
  The initial driver run exposed a host/kernel stdint include conflict; the
  separate public status header resolves it, and the complete v2 run passes.
- Kernel build: 6,952,320 bytes, SHA256
  `6a75a9d6bccea19aaeb46f930cc60b68982362915caf7b41dd210a485ed8d501`.
  Only existing unused `sh_reboot`/`sh_halt` warnings were emitted.

This is diagnostic instrumentation, not a demonstrated fix for physical WPA2.

## Rejection-diagnostic kernel deployment (2026-09-24, 00:53 UTC)

`build/wlan-rx-diag-qemu-v1/` passes all 12 boot/network/OFTP gates, including
byte-exact disk readback and rejecting an incomplete/changing-size transfer.
No serial or server errors; the managed runner removed its scratch disks.
This is Ethernet/absent-WLAN regression coverage, not AX200 RF validation.
QEMU result SHA256:
`c8cbc44b8381828fdef042e46dec53d42c648ffef0c07619431704c3d9f83ce8`;
serial SHA256:
`c21b9efffe1bb2d7457babaaf32aaf8b85b4bc5ea8e36cd7f77ab71e55b3f9f0`.

Deployment bundle: `build/wlan-rx-diag-deploy-20260924/`. Only pins and backup/
stage names changed in the guarded ESP updater. Its local selftest passes
120 checks without opening a physical device
(`selftest-3104764b394145b0aa0996fb93e63243/`). Helper SHA256:
`3151c0564de37b33cc941cf41c904511e32a031e3f6503337728bfcc546a4562`.

Elevated PID 32696 exited zero and was reaped. At **00:53:02 UTC**, the result
`20260923-215246-61792fa6b3c1448da0b50b6285f75e78/update-result.json` records
`Passed=true`, `Wrote=true` and installed readback matching `6a75a9d6...`.
The cache was flushed; USB serial `00000000CY02` (disk 4) was verified.
Raw-device access was read-only; only ESP files changed. GPT, bootloader,
other boot files and OsitoFS metadata match the preflight. The old `99af84a3...`
kernel is preserved as `EFI/BOOT/KERNEAP.ELF` and the local verified
`previous-efi-kernel.elf`. No logs or earlier backups were deleted.

Do not rerun this pinned updater after success. No updater or test VM remains
running. Safely eject and boot through UEFI, not the old OsitoFS `/kernel.elf`.
Run `wlan start /iwlwifi-cc-a0-77.ucode`, then `wlan connect`; save
`wlan status > wlan-connect-status.log` and `ifconfig > wlan-connect-ip.log`,
then `wlan stop`, `dmesg > wlan-connect-dmesg.log`, and `sync`. Require
`sync: ok`. The next readback should distinguish queue drops from the precise
WPA2 rejection; no physical WPA2 success or password diagnosis is claimed yet.

## Physical M1 PMKID rejection (2026-09-24, 01:00 UTC)

Read-only capture (reader PID 39720, exit zero):
`build/realhw-usb-20260921/hardware-readback-20260924-010055/`.
Installed ESP kernel readback matches `6a75a9d6...`. `diag/latest.txt` selects
`diag/boot1.log`, 34,844 bytes, not truncated, SHA256
`d3a6358b8a28afbcdfe8e22ce3da11f3178d7e5a2fd4ce97d1ab5f90ebd4ff93`.
The 1,165-byte status log is SHA256
`689226d674366316ccb0cf6e77b3f7f199383019c2ea71307646b8444159312f`.
All 30 root logs and six diagnostic files are preserved locally.

Firmware commands complete 22/22. Authentication and association succeed
(channel 1, AID 1, AP status zero), TX 2/2/0. Five EAPOL frames are dropped,
zero accepted, zero key installs and zero ingress queue/size drops. The exact
last rejection is `M1 PMKID mismatch` in `wait M1`; bytes/body are 121/117,
version/type/descriptor 1/3/2, key-info 0x008a, key/data lengths 16/22. The AP
disconnects with reason 15 and IPv4 remains 0.0.0.0. Stop releases all DMA;
the final sync succeeds. Read-only fsck of the local metadata snapshot is
clean (15,962 files, 1,980 directories, zero errors). Metadata SHA256:
`e4af82f8bf139635572af02fa68f00063c71b086ab0b699405c1b320e8b2bcd5`.

### PSK M1 contract

The primary reference is the pinned
[wpa_supplicant RSN implementation](https://android.googlesource.com/platform/external/wpa_supplicant_8/+/ef0b8fa1163a4b136de5d7b88edc37b3abb73398/src/rsn_supp/wpa.c).
`wpa_supplicant_get_pmk()` uses M1 PMKID for PMKSA-cache selection; a cache
miss takes the EAP fallback only for 802.1X. PSK with its configured PMK can
continue. `wpa_supplicant_process_1_of_4()` derives a temporary PTK; M3
verification authenticates it. OsitoK currently supports PSK only, with no
PMKSA cache. It must not apply the enterprise/cache failure rule as a PSK
authorization gate. Adding PMKSA caching or another AKM will need its own
explicit selection and fallback contract.

M1 now records PMKID-content mismatch as a counter, not a drop, after its
structural/state/replay checks. It does not change the configured PMK. The
driver preserves the advisory count on disconnect/stop and resets it on a
new connection. `wlan status` exports only the count, not identifier bytes,
passwords, keys, nonces or MICs. Malformed/duplicate PMKID KDEs still fail.
No AP/vendor/SSID exception or password-validation bypass is introduced.

The real mismatch remains a credential/identity/interoperability diagnostic,
not proof of which side is wrong. A wrong PSK still cannot authenticate M2/M3
or authorize data. A mismatching PMKID followed by timeout is not success.
Physical WPA2 and IP validation still require the P1 retest.

### PSK M1 validation and prepared bundle

- `build/wpa2-pmkid-red-v1/`: the old gate fails the new regression (1,512
  checks, 33 failures). `build/wpa2-pmkid-v1/`: 8,707,372 checks in each of
  four GCC/Clang normal and ASan/UBSan runs, all passing. Absent, matching and
  mismatching PMKID cases cover EAPOL versions 1-3 and the observed 121-byte
  shape. Independent OpenSSL AP keys verify M2/M3; a wrong local PMK fails
  MIC authentication and times out with keys wiped. Malformed/duplicate KDE,
  replay, bad MIC, delayed key/TX ACK and reinstallation checks remain covered.
- `build/iwx-pmkid-v2/`: all 40 executions pass, 8,479,940 transport checks
  and 73,687 NIC/integration checks per variant. Synthetic M1 retransmissions
  with a mismatching hint send M2 without opening data; valid M3 plus key/TX
  ACKs completes the handshake. Timeout cleans DMA/private keys and preserves
  the advisory count; a new connection clears it. The v1 run stopped on a
  maybe-uninitialized local test snapshot warning; v2 initializes that fixture.
- `build/wlan-pmkid-v1/`: 112,842 checks per variant, all four passing.
  Includes direct and redirected diagnostics and no password echo.
- All 36 WPA2, 75 driver and 25 shell frozen inputs match the working tree;
  LF/trailing-whitespace and `git diff --check` pass. GitNexus was unavailable;
  manual impact review covers `message1`/`wpa2_receive`, `security_pump`, the
  driver status structure and `wlan_link_report` plus their existing tests.
- Kernel build: 6,956,416 bytes, SHA256
  `32c451f27e9b78b5b155b509ac4c90d7c8ec30d488150af5b7bafcdddb8ad799`.
  Only existing unused `sh_reboot`/`sh_halt` warnings were emitted.
- `build/wlan-pmkid-qemu-v1/`: all 12 boot/network/OFTP gates pass; no serial
  or server errors. Transfers are byte-exact; incomplete/changing-size
  transfers are rejected. Managed scratch disks were cleaned. Result SHA256
  `c65f3c7d99d9bfb75c14b287c4d0aeca4a5affa077f6a2806260c03fd5a5dda8`;
  serial SHA256
  `3d23ffa9e4da60e73c80a59729a43d4b7d062f9138b4e9c74cc2ea364d90374f`.

`build/wlan-pmkid-deploy-20260924/` freezes these artifacts. Only pins and
backup/stage names change in the guarded updater. Its local selftest passes
120 checks without opening a physical device
(`selftest-7d9a5853833449aca913962a5655e44a/`). Helper SHA256:
`c49a87d39ed6beedb1d061f48de003ea5fffde1ed3dc41a2aae389a173b27ef3`.
The target is uniquely identified development USB serial `00000000CY02`;
the old `6a75a9d6...` kernel must match before it can become `KERNPMK.ELF`.
Do not infer installation from preparation/selftest; require the guarded
updater's successful exit and installed-kernel readback evidence.

The first deployment elevation was canceled: Windows returned
`El usuario ha cancelado la operacion` before `Start-Process` returned a child
PID. The launcher exited 1 and was reaped; no deployment result directory was
created and no USB write occurred. At that point the PSK M1 fix was not
installed; the pendrive retained the diagnostic `6a75a9d6...` kernel. The user
then explicitly requested the successful retry recorded below.

### PSK M1 kernel deployment (2026-09-24, 01:27 UTC)

The retry rechecked the tested helper and artifact hashes before elevation,
then required the guarded updater's complete verification. Elevated PID 44008
exited zero and was reaped. Evidence within `build/wlan-pmkid-deploy-20260924/`:
`20260923-222733-a66e1775fbdf48ad97693e954c1aee3b/update-result.json`.
At **01:27:45 UTC**, `Passed=true`, `Wrote=true` and the installed kernel hash
matches `32c451f27e9b78b5b155b509ac4c90d7c8ec30d488150af5b7bafcdddb8ad799`.
The cache was flushed. USB serial `00000000CY02` (disk 4), geometry and GPT
were verified. Raw-device access remained read-only; only ESP files changed.
Bootloader, older backups, other boot files and OsitoFS metadata remain
unchanged; metadata hash is still `e4af82f8...`. Logs were not deleted.
The old `6a75a9d6...` kernel is preserved in `EFI/BOOT/KERNPMK.ELF` and the
local result directory's verified `previous-efi-kernel.elf`.

Do not rerun this pinned updater after success. No updater remains running.
Safely eject and boot through UEFI, not the old OsitoFS `/kernel.elf`.
Run `wlan start /iwlwifi-cc-a0-77.ucode`, then `wlan connect`; export
`wlan status > wlan-connect-status.log` and `ifconfig > wlan-connect-ip.log`,
then `wlan stop`, `dmesg > wlan-connect-dmesg.log` and `sync`. Require
`sync: ok` before reconnecting. The PMKID count is advisory; physical
authorization, authenticated data and IP connectivity remain unverified.

## Physical M2 progress, authorization still closed (2026-09-24, 01:43 UTC)

Read-only capture (reader PID 25420, exit zero, reaped):
`build/realhw-usb-20260921/hardware-readback-20260924-014336/`.
The installed ESP kernel still hashes to `32c451f2...`; bootloader unchanged.
`diag/latest.txt` selects `diag/boot2.log`, 35,658 bytes, not truncated, SHA256
`c0992cede0791ee1020dcaa08e8f5cf08c96003b0df82305edf90096a6560597`.
All six diagnostic files and 30 matching root logs were copied locally.
`wlan-connect-status.log` is 1,029 bytes, SHA256
`7a43a9c7d2d1b3b2a8954db6ea93dd9d838d075d158e19d3f98abf174a6d7cd8`.
The corrected dmesg export is 31,239 bytes, SHA256
`6b5a92c6387a60c1e46ad2565cc30f64061d65cd7a7e4870460ec9939eb7c093`.
An earlier mistyped `.loh` export also exists; it was not removed.

Eight BSS entries were discovered. Firmware commands complete 22/22 and
association succeeds on channel 1, AID 1, status zero. TX sent/completed/failed
is now 8/8/0, versus 2/2/0 before the fix. The six accepted EAPOL messages
all produce an advisory M1 PMKID mismatch; no EAPOL/ingress drops or key
installs occur. The AP disconnects with reason 15. The port stays closed and
`wlan0` retains 0.0.0.0. Successful TX completion is not proof of M2 MIC
acceptance by the AP. No accepted M3 is visible; counters alone cannot tell
whether an M3 was absent over the air or rejected before the WPA2 parser.

The pinned [hostap authenticator](https://android.googlesource.com/platform/external/wpa_supplicant_8/+/ef0b8fa1163a4b136de5d7b88edc37b3abb73398/src/ap/wpa_auth.c)
derives candidate PTKs and validates the M2 MIC before progressing. Failure
returns without advancing the handshake. The observed result is consistent
with different effective key material, but is not a capture of the AP's
decision and does not prove an incorrectly entered password.

Inspected the shell's secret-input/SSID-to-PMK path, network-snapshot identity
checks, PMK ownership/wiping, local/AP MAC binding, EAPOL encoding and Gen2 TX
descriptor layout. No evidence justified another acceptance-rule change.
The user confirms the password is alphanumeric, so symbol-layout differences
are not the current leading explanation. The console currently has a fixed
US mapping; case/modifier input can be checked using harmless sample text,
never by echoing a real password into persistent shell logs.

Additional local validation: `build/wpa2-native-objects-v1/` freezes the
deployed kernel and its existing WPA2/BearSSL objects, then links those
objects (not recompiled crypto) against the existing OpenSSL-oracle host
tests. RNG remains the test's mocked source. All 8,707,372 checks pass,
including PBKDF2, HMAC, PTK, M2 MIC and authentication-failure cases. This
reduces a native compilation/ABI hypothesis but does not emulate hardware
input, kernel scheduling, DMA or the AP. Test executable SHA256:
`4138c0149c2e4a9835972b267a50ba6baef594aa19898713e29bf617a790d522`.

Stop releases all DMA and the final `sync: ok` is recorded. OsitoFS mounts
from `usb0`; the earlier superblock-read failure is the unsuccessful probe
of another block device, not failure to mount the USB. Read-only fsck of the
local snapshot is clean: 15,963 files, 1,980 directories, zero errors.
Metadata SHA256 is now
`8faee468c1d1395d8478de7afe0005f640673abc298b51ba66d7215b8327b938`.
No USB writes, log deletion, kernel changes or deployment attempts were made
during this review. The old deployment helper's metadata baseline is stale;
do not reuse it for a future update.

The user subsequently confirmed the same failure on two networks with
alphanumeric passwords. Do not repeat the second-network/password advice as
the next step. Instrument the native crypto and pre-WPA2 RX/TX boundaries;
AP-side diagnostics remain the independent way to establish M2 acceptance.
No Wi-Fi/IP success is claimed.

## Native crypto and handshake diagnostics (2026-09-24)

`wlan selftest` executes six public known-answer tests inside the kernel:
HMAC-SHA1, PBKDF2-PMK, PTK (both input orderings), PMKID, a synthetic M2 MIC,
and AES unwrap (valid and tampered input). It never accesses the radio,
credentials, RNG, active WPA2 state or key slots. Each case has independent
fixed inputs, so one failure cannot mask another. It reports names and
pass/fail only; ordinary file redirection works without the connection
assistant's secret-input restrictions. This tests native execution, not the
correctness of a typed password, the live session, DMA or RF.

Primitive references: [RFC 2202 case 1](https://www.rfc-editor.org/rfc/rfc2202),
[RFC 3394 section 4.1](https://www.rfc-editor.org/rfc/rfc3394).
The synthetic vectors use the public password/IEEE PMK, AP `02:00:00:00:00:02`,
station `02:00:00:00:00:01`, ANonce bytes `00..1f`, SNonce `20..3f`, EAPOL
version 2, replay 1 and the association RSN IE. Their expected bytes were
computed independently with OpenSSL-backed Python hashlib/hmac, not the
kernel functions. The recipe is `build/wlan-diag-vectors.py`.

`wlan status` now includes saturating pre-WPA2 RX reason counters and at most
eight recent AP-data observations while the port is closed. Reasons identify
bounds, state, firmware validity, channel, aggregation, frame format,
fragmentation, peer/destination, CCMP/IV/replay, LLC, EAPOL source and port
gates. The original RX return/output contract and acceptance gates are
unchanged. A plaintext QoS EAPOL candidate can be identified diagnostically
but is still rejected by the non-QoS association contract. Ciphertext and
aggregates are never guessed as LLC/EAPOL headers.

RX samples contain time, length, FC, channel, firmware status/flags and an
address-match bitmap (1 receiver=local, 2 transmitter=AP, 4 source=AP,
8 group destination). Any EtherType/EAPOL header is explicitly a candidate,
not authentication evidence. Beacons and unrelated peers cannot evict these
samples. Overwrite counts expose truncation of the fixed-size history.

Eight recent EAPOL TX submissions retain time, M2/M4/group kind, queue/index,
frame/byte-count/TB lengths, flags/rate/FC and public EAPOL version/key-info.
The driver compares the copied DMA frame against the source before submission
and retains only a match bit. Validated completion attaches raw status,
completion time/rate and RTS/no-ACK/BT counters using the
[TX_RSP layout](https://raw.githubusercontent.com/openbsd/src/master/sys/dev/pci/if_iwxreg.h).
SUCCESS and DIRECT_DONE keep their existing behavior; neither is reported as
AP acceptance of the M2 MIC. Stale/malformed completions still fail ownership
validation before touching the trace. There is no serial output or file I/O
in the RX/TX path. Snapshots stay below 2 KiB, survive stop/private-key wiping,
and reset on a new link. No passwords, keys, MACs, replay values, nonces,
MICs, PMKIDs or packet bodies are added to exported diagnostics.

Manual impact review (GitNexus unavailable): `iwx_data_receive` keeps its
existing wrapper/contract; only the driver opts into the diagnostic variant.
`link_notification` and `link_transmit` update the bounded software report
under existing ownership. Shell changes are confined to WLAN reporting and
selftest dispatch. No Win32, DOS, filesystem, DMA lifecycle or authentication
policy changes are part of this diagnostic patch.

Host verification:

- `build/wpa2-linkdiag-v1/`: GCC/Clang normal and ASan/UBSan, 8,707,438 checks
  per run. Includes all 64 injected KAT-failure combinations, without changing
  the kernel build's crypto functions.
- `build/iwx-linkdiag-v1/`: 40 passing suite executions. Transport 8,625,068,
  NIC integration 73,687 and data/CCMP 161,952 checks per compiler/mode.
  New cases cover gate reasons, exact-length/truncated allocations under
  sanitizers, plaintext QoS observation without acceptance, no ciphertext
  header export, raw TX statuses 1/2/failure, ring wrap and persistence/reset.
- `build/wlan-linkdiag-v1/`: 116,930 checks per compiler/mode, including
  selftest success/failure reporting, redirection and bounded trace output.

Physical procedure after installing the tested diagnostic kernel via UEFI:

```text
wlan selftest > wlan-selftest.log
cat wlan-selftest.log
wlan start /iwlwifi-cc-a0-77.ucode
wlan connect
wlan status > wlan-connect-status.log
ifconfig > wlan-connect-ip.log
wlan selftest > wlan-selftest-after.log
wlan stop
dmesg > wlan-connect-dmesg.log
sync
```

Require `WPA2 selftest: PASS (6/6)` before starting the radio. If a KAT fails,
skip start/connect, retain the log and sync instead. After connect returns,
export status before stopping. Require `sync: ok` before disconnecting USB.
The before/after KAT logs and bounded TX/RX history are the next evidence;
do not infer working Wi-Fi from a selftest or firmware TX completion.

### Diagnostic kernel validation and USB installation (02:58 UTC)

Build: `make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf`,
exit zero; existing unrelated warnings remain. An initial default-flags build
was replaced by the intended quiet/skip-model build before QEMU/deployment.
Selftest and WPA2 crypto are compiled with `-mgeneral-regs-only`.
Final kernel size 6,981,528 bytes, SHA256
`f5caddece032c5d3ee27898fb092869803029379f791e58af8ba0f992f9d3295`.
All frozen non-vendor inputs in the three host-test directories matched the
working tree after testing (13/50/26 files); no post-test code changes.

`build/wlan-linkdiag-qemu-v1/`: regression runner exited zero and reaped QEMU.
All 13 gates pass, including the real `wlan selftest` in console and redirected
file form, byte-exact OsitoFS3 readback, missing AX200 behavior and OFTP loss /
resume / changed-size rejection. No server or serial-reader errors. No real
USB or persistent application disk is attached to this smoke test.

- Result SHA256: `827bf9556cdbd18574590c9713879a7830f3c8ea1df53e91c2965b0272c63e56`.
- Serial SHA256: `b567178a186d4607623b5b0b618cac935e6510a56960e93d9f529b36064f0733`.
- `readback/wlan-selftest.log`: all six cases and overall selftest pass.

Deployment bundle: `build/wlan-linkdiag-deploy-20260924/`. Updater SHA256
`2d19e6c20904787e842f9f040088ab2bec50e9e055b680218b9e74a0930bb1dc`.
Its selftest passed 121 checks without opening a physical device, including
the new native-KAT evidence gate, identity failures and rollback injection.
Elevated PID 16032 exited zero and was reaped. Result:
`20260923-235817-a7f2ba9646e94ebc980c7e5ea7d13c76/update-result.json`,
timestamp `2026-09-24T02:58:30.2166150Z`.

The helper revalidated USB serial `00000000CY02`, size, partition geometry,
ESP GUID, primary/backup GPT, old kernel, bootloader and latest OsitoFS3
metadata before creating the staged FAT file. It flushed, verified, preserved
the old kernel and renamed the staged kernel; final installed hash matches
the QEMU-tested binary. The physical device was opened read-only for guard
hashes; writes were confined to the existing ESP filesystem.

Previous kernel `32c451f2...` is preserved as `EFI/BOOT/KERNKAT.ELF` and the
result directory's verified `previous-efi-kernel.elf`. Bootloader hash remains
`feec6ce013b760c6e2b6f71192c01eb00ed183a8139ac73b61b218233bd94038`;
GPT and other boot files are unchanged. OsitoFS3 metadata remains
`8faee468c1d1395d8478de7afe0005f640673abc298b51ba66d7215b8327b938`.
No logs were deleted. `OSKAT.TMP` was consumed; do not rerun this one-shot
updater. Eject safely and boot via UEFI, not the old OsitoFS `/kernel.elf`.

## Physical native KAT and M2 trace (2026-09-24, 03:16 UTC)

Read-only reader PID 300, exit zero, reaped. Capture:
`build/realhw-usb-20260921/hardware-readback-20260924-031656/`, timestamp
`2026-09-24T03:16:58.5286422Z`. USB identity/geometry verified; six diagnostic
files and 32 matching root logs copied locally. Kernel remains `f5caddec...`
(6,981,528 bytes); bootloader unchanged. Latest slot is `diag/boot3.log`,
41,181 bytes, not truncated, SHA256
`3342a8a32fa2d0eeacd7199f18f3cbcbc57e0dc28bea6b30ff1099374296012b`.

`wlan-selftest.log` and `wlan-selftest-after.log` are each 220 bytes with SHA256
`b0cf833a5edd29511c5a7b79a7a43c3f966ac18d29597c6a4f8163f92445cd16`.
Both report all six public KATs passing on the physical P1. This lowers the
likelihood of a generic crypto/native-ABI failure; it does not validate live
credentials, session inputs or frame contents against the AP.

Status: `wlan-connect-status.log`, 4,632 bytes, SHA256
`f2e44b2501fe8243e47960246a431f8dad5066e0b80453b9e9ad42c353b15091`.
Dmesg: 36,187 bytes, SHA256
`eed18eecc226b4ecfdeb0a3f2448aa045b6632ccff05d1c0c794247cfe2a7803`.

- Six BSS entries, 22/22 firmware commands, association channel 1 / AID 1 /
  status zero; AP disconnect reason 15. Port closed, `wlan0` 0.0.0.0.
- TX 8/8/0: two management frames and six M2 replies. All six have DMA-copy
  match, 153 frame bytes, BC 39, TB lengths 20/28/129, FC 0x0108, flags 3,
  rate 0x4000, EAPOL version 1, key info 0x010a, queue 1 / indices 2..7.
- M2 submission times 435138, 436127, 437127, 438127, 439127, 440127 ms.
  Each completion follows in 2-3 ms, raw status 0x0001, RTS/no-ACK/BT 0/0/0.
  This is firmware transmission evidence, not acceptance of the MIC by the AP.
- EAPOL accepted/dropped 6/0, ingress drops zero, six advisory M1 PMKID
  mismatches, zero key installs. No additional accepted M3 is visible.
- Pre-WPA2 reasons: accepted 6, format 90, closed-port 22. Other counters
  are zero, including firmware-status/channel/bounds/CCMP/replay failures.
  Do not call all 90 format drops M3: that counter also includes management
  frames and unrelated frame formats.
- RX history retains eight frames and reports 20 overwritten. All retained
  entries are ARP (0x0806), 82 bytes, address mask 10 (group destination,
  transmitter=AP), dropped at the correctly closed data port. The eight-slot
  history currently admits broadcast ARP and therefore loses earlier EAPOL.
  This is an observability bug, not a justification to open the data port.

The 28 retained-plus-overwritten RX observations equal six accepted EAPOL
plus 22 closed-port frames. Under this trace's selected-AP/data/closed-port
eligibility rules, there is no extra observed AP data-format rejection. This
is a code/counter inference, not an over-the-air capture; lower firmware
filtering and actual AP validation are still unobserved. Preserve dedicated
EAPOL/candidate evidence independently of ordinary broadcast traffic before
another diagnostic deployment. For the remaining M2 rejection hypothesis,
an AP-side log/capture is independent evidence; the
[reference authenticator](https://android.googlesource.com/platform/external/wpa_supplicant_8/+/ef0b8fa1163a4b136de5d7b88edc37b3abb73398/src/ap/wpa_auth.c)
checks the M2 MIC before advancing. Do not infer a mistyped password from
these logs or weaken the security gates. Two alphanumeric networks have
already been tested according to the user.

Stop releases all DMA; `sync: ok` is present. Local metadata snapshot fsck is
clean: 15,965 files, 1,980 directories, 3,580,547,522 bytes, 455,602 free blocks,
113,126 free inodes, zero errors. Current metadata SHA256:
`d1e367f42a03c2bf728564915ef34ef4095bbb71bf8c063fc781023c3a1ebaa0`.
No USB writes, new kernel deployment or log deletion in this readback turn.
The previous updater's metadata pin is now stale; do not rerun it.

## Temporary password echo (2026-09-24)

At the user's request, `wlan_connect_selected()` sets `echo_password = true`.
Both the assistant and direct WPA2 command now prompt with
`WPA2 password (visible): ` and echo accepted input while typing, through the
normal framebuffer/serial output. Backspace/DEL and Ctrl+U erase displayed
characters. Serial/boot/dmesg logs can contain the password in this diagnostic
build; the user explicitly accepted that local diagnostic behavior. No separate
log filtering or credential store was added. Set this local flag to `false`
after the keyboard check to restore the existing no-echo prompt and behavior.

Input bounds, escape-sequence handling, cancellation, buffer wiping, command
history isolation and all WPA2 acceptance rules remain unchanged. This patch
does not fix the RX-history ARP eviction or establish the authentication cause.
Manual impact review (GitNexus unavailable): the reader has one production
caller, the WLAN shell helper; the other updated signatures are host-test stubs.

Host tests passed in GCC/Clang normal and ASan/UBSan modes:
`build/wlan-password-echo-keyboard-v2/` (4,067 keyboard and 3,995 terminal
checks per mode) and `build/wlan-password-echo-shell-v1/` (117,202 per mode).
The terminal tests cover both echo settings, editing, cancellation, invalid
input and the 63-character boundary. The first test attempt used an unavailable
`strncmp` declaration in the host harness; it was corrected to the existing
`memcmp` helper before the successful rerun. Kernel build with
`CLANG=1 QUIET=1 SKIP_MODEL=1` passed; existing unused shell-helper warnings
remain. Kernel SHA256:
`f8acc7da5fbd9317bb2408ad9062e775cdf29b78044a90145069ff1316e28f0e`.

`build/wlan-password-echo-qemu-v1/` passed all 13 regression gates, including
the native crypto selftest and OFTP byte-exact readback; QEMU exited normally.
This does not exercise the physical AX200 or its password prompt in QEMU.
The existing pinned ESP updater was reused in
`build/wlan-password-echo-deploy-20260924/`, with current evidence/metadata
hashes and unique backup/stage names. Its local 121-check selftest passed.
Elevated PID 42556 exited zero and was reaped. Installation completed at
`2026-09-24T03:41:47.0625291Z`, with the installed kernel hash matching the
tested binary. The previous `f5caddec...` kernel is preserved as
`EFI/BOOT/KERNECHO.ELF` and in the local result directory. Bootloader, other
boot files, GPT and OsitoFS3 metadata are unchanged; no logs were deleted.
Boot via UEFI, not the older OsitoFS `/kernel.elf`. Physical echo verification
and successful WPA2 authorization remain pending.

## Next checkpoints

1. Traced NVM passed on the P1. Compare ordinary `wlan nvm` to exclude dependence
   on persistent-trace I/O timing; keep the successful `wlan-nvm2-dmesg.log`.
2. Local IPv4 now passes on the physical P1 after the password-echo retest.
   The earlier M2-only failures remain historical evidence, not the current
   state. Visible echo is diagnostic, not a demonstrated authentication fix;
   disable it after confirming input. Correct the pending ARP eviction of RX
   history without changing WPA2 acceptance rules. Do not regress to the old
   M1-drop/TX-allocation diagnosis if a later failure has different evidence.
3. Validate the pending MCC acknowledgement and association across networks.
   No hardcoded MAC, country, SSID, credentials or permissive channel list.
   A discovered AP or an association response is not an authorized link.
4. Physical ICMP/UDP over APIPA passed; investigate the reported DHCP failure.
   Still require a lease, gateway reachability, DNS and HTTPS,
   followed by OFTP readback and stop/restart without stale addresses/listeners.
   Use the checked `random_secure_bytes()` path for handshake nonces, not
   `random_get_bytes()` or `random_is_ready()`. Keep the core's generation/
   completion and replay contracts intact when mapping them to hardware.
5. Stress RX replenishment on hardware; add interrupt delivery, disconnect/reconnect,
   RF-kill and firmware-error recovery. Generalize the AX200 kexec guard into
   a registered device DMA/IRQ shutdown path. Never free DMA-reachable memory before
   hardware quiescence is proven; do not invent a successful stop on timeout.

## Physical local IPv4 and SSH probe (2026-09-24, 04:28 UTC)

The development PC is wired to the repeater; the P1 uses Wi-Fi. Windows has
`192.168.1.31/24` on Ethernet and an overlapping `169.254.0.0/16` route on
Tailscale. The initial unconstrained ping used Tailscale and failed. A direct
Ethernet ARP request resolved the P1's `169.254.167.93` to
`6c:6a:77:eb:bd:15`, matching its known AX200 MAC.

Local helper `build/probe-p1-linklocal.ps1` created a temporary Ethernet
`169.254.167.94/32` address through IP Helper plus a target-only on-link route.
Both had 180-second lifetimes; the helper removed them in `finally`, without
changing DHCP, the normal IP, default route or Tailscale. Duplicate-address
detection completed before use. The first attempt failed before creating an
address (native error 87); assigning the nested socket-address value explicitly
corrected the PowerShell value-type marshalling issue. Both elevated processes
(34504 and 45292) exited and were reaped; no temporary address/route remains.

Successful capture: `build/p1-linklocal-20260924-012834/`.
`result.json` SHA256 `9d9129c292e56829e8ba105c035d25817c6ac93a51af6a842aea41d0304ed6ad`;
`probe.log` SHA256 `59f6d82f380d23eca98558f2559ce24b09991717328439f0a6787a52f34aec4d`.

- Ping: 4 sent, 4 received, 0 lost; 11/3/5/4 ms, TTL 64.
- UDP 7777: production no-model prefix followed by the exact 49-byte unique
  test token, received from `169.254.167.93:7777`. This is an echo service,
  not a command shell. No credentials were involved in the probe.
- TCP 22: timed out after three seconds. Source inspection confirms no caller
  registers a listener/accept loop for `sshd_handle_connection`; its KEX stops
  at a TODO and `sshd_poll` has no packet processing. The header's advertised
  encryption/authentication/shell support is not implemented. A real SSH
  server integration is required; do not label the UDP echo as remote shell.

No kernel or USB changes in this live network test. The user-reported external
ping failures do not contradict working local IP: APIPA has no default route.

The subsequent SSH implementation replaces that stub using TinySSH under its
0BSD option; see [SSH maintenance console](x86-ssh.md). OpenSSH host/QEMU tests
do not establish physical AX200 SSH success. A new kernel must be downloaded,
booted, associated and tested on the P1 before updating that claim.

## Mixed CCMP pairwise / TKIP group support (2026-09-25)

The read-only SSH capture at `169.254.243.99`, saved as
`build/p1-wlan-security-20260924/networks-after.log`, resolves the earlier
unknown-security diagnosis. Both CortesRojas BSSIDs advertise this 24-byte
RSN payload:

```text
0100000fac020200000fac02000fac040100000fac020000
```

It selects TKIP as the group cipher, offers TKIP and CCMP as pairwise choices,
and offers PSK authentication with RSN capabilities zero. The 2.4 GHz entry
was on channel 6 at -59 dBm; the 5 GHz entry was on channel 161 at -76 dBm.
The old parser rejected the group selector before any password exchange.
This is not evidence of a bad password, SAE-only authentication or required
PMF. MOVISTAR_2A6A continues to advertise CCMP for both cipher roles.

Implemented contract:

- The shared RSN parser accepts either CCMP or TKIP group keys while still
  requiring CCMP pairwise encryption and PSK. Association and M2 preserve the
  AP's group selector and select CCMP pairwise. M3 still binds the complete
  observed AP RSN element; no SSID, BSSID or channel exception was added.
- The WPA2 core handles 16-byte CCMP and 32-byte TKIP GTK KDEs, late GTKs,
  group rekeys and retransmissions. Key generations and firmware acknowledgments
  remain authoritative. Duplicate installation does not reset replay state;
  reusing a temporal key with different MIC subkeys or another slot is rejected.
- AX200 ADD_STA_KEY v3 installs TKIP group keys with station-oriented TX/RX
  Michael subkeys. Firmware APIs 29 and 41 are required for this path; CCMP
  pairwise keys and the existing all-CCMP path retain their prior contracts.
- TKIP RX requires the installed cipher/slot, valid firmware key/decryption,
  ICV and Michael MIC results, and RADA offload metadata. IV layout, firmware
  padding/trailer bounds and strictly increasing TSC are checked before delivery.
  Missing offload evidence is an explicit rejection, not a guessed plaintext
  fallback. Rejected frames do not advance the accepted replay counter.
- A fresh, otherwise eligible Michael failure queues an authenticated EAPOL-Key
  error report using the CCMP pairwise key. Two failures within 60 seconds close
  the data port, report the failures, deauthenticate with reason 14 and block
  reassociation for 60 seconds. The per-radio hold survives security reset and
  stop/start; reports and teardown are completion-driven and bounded.
- The shell labels eligible mixed networks `WPA2-PSK/CCMP (TKIP group)` and
  distinguishes TKIP, Michael MIC and offload RX failures. The temporary
  user-requested visible password prompt is unchanged.

Firmware layout and RX ownership follow the Intel interfaces in
[`sta.h`](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/wireless/intel/iwlwifi/fw/api/sta.h)
and [`rxmq.c`](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/wireless/intel/iwlwifi/mvm/rxmq.c).
GTK subkey direction and MIC-report framing were checked against the
[RSN supplicant](https://raw.githubusercontent.com/freebsd/freebsd-src/main/contrib/wpa/src/rsn_supp/wpa.c).
This does not add legacy WPA, TKIP pairwise encryption, SAE, enterprise
authentication, required PMF, general QoS/fragmentation or software TKIP crypto.

Validation completed with GCC and Clang, each normal and ASan/UBSan:

- `build/wpa2-tkip-v1-20260925/`: 8,709,577 checks per variant, zero failures;
  independent OpenSSL MIC checks, both GTK lengths, rekeys, replay, malformed
  KDEs, subkey reuse, countermeasure reports, timing and key wiping.
- `build/iwx-tkip-v2-20260925/`: all 40 runs pass. Per variant: transport
  8,877,828; data ABI 222,758; association 585,706; network integration 343,336;
  firmware 44,743; scan 41,072; PCI/DMA/boot checks also pass. Network cases
  exercise broadcast/unicast DHCP and previous APIPA configuration for open,
  CCMP-only and mixed TKIP-group links, with simulated radio/firmware traffic.
  Real `iwlwifi-cc-a0-77.ucode` parsing confirms the required capabilities,
  not live TKIP execution on hardware.
- `build/wlan-tkip-v1-20260925/`: 146,455 shell checks per variant, zero failures.
- The clang kernel build succeeds with `QUIET=1 SKIP_MODEL=1`; only the existing
  unused shell helper warnings remain. The 7,125,344-byte tested ELF is
  `build/wlan-tkip-qemu-20260925/kernel.elf`, SHA256
  `2572ee971c3e880914454530afb43e0ab3918e8dc56840fda477cd091d165e80`.
  Its `results.json` passes all 16 SSH/sync/cold-boot/kexec regression gates.

An SSH retry to the old address `169.254.243.99` timed out while the PC's
temporary source was Preferred and the P1 neighbor unresolved. The user then
reported `169.254.194.99`. The old temporary IP/route helper stopped cleanly
and a new two-hour, peer-scoped helper restored access without changing
Windows DHCP. Authenticated `ifconfig` confirmed the expected AX200 MAC and
an active link; `wlan status` showed an authorized channel-1 CCMP connection.

The exact tested ELF above was transferred over authenticated SSH to the new
path `/kernel-wlan-tkip.elf`. The sync protocol accepted its final SHA-256 and
confirmed the 7,125,344-byte destination: 7,092,576 bytes sent in 4 KiB write
requests, 32,768 bytes reused from the read-only `/kernel-wlan-security.elf`
basis, 129.88 seconds. Neither the previous kernel nor the USB ESP was replaced.
Evidence is under `build/p1-wlan-tkip-20260925/`: `status-before.log`,
`upload.log` and `kexec.log`.

The subsequent SSH command `kexec /kernel-wlan-tkip.elf` ended with a server
timeout (exit 255), without remote command output. The request was sent, but
the transport ending alone did not prove that the new kernel booted. The
requested local retest was `wlan start /iwlwifi-cc-a0-77.ucode` followed by
`wlan connect`, with CortesRojas now eligible as
`WPA2-PSK/CCMP (TKIP group)`. Host/QEMU success alone did not establish the
physical AP handshake or resolve the repeater DHCP issue.

### Physical 5 GHz result and peer reachability (2026-09-25)

The user connected to CortesRojas-5G and reported `192.168.1.20`, successful
ping to `192.168.1.1`, and failed ping to the wired development PC at
`192.168.1.31`. Restarting sshd did not restore PC-to-P1 access. The supplied
`wlan status` photo shows channel 161 and the new `Key slot/PN/previous` label,
confirming execution of the updated receive path after the remote kexec request.

The visible trace contains accepted group packets with firmware status
`0x00001B7F` (TKIP in this driver's security field). GTK slot 2 advances through
PN 3, 4 and 5. Two accepted UDP 67-to-68 frames carry DHCP transaction
`0x7AC32948`, interleaved with reflected 68-to-67 client traffic. Together with
the reported address and gateway ping, this establishes physical TKIP-group
reception and useful IPv4 traffic on the direct 5 GHz network. It does not
establish peer connectivity or explain why DHCP failed through MOVISTAR_2A6A.

The later visible CCMP drop has FC `0x4A08`, slot 0 and PN/previous 72/72:
the Retry bit is set and the packet number is repeated. Rejection is consistent
with the replay contract; this sample is not evidence to relax replay checks or
change the keys.

The subsequent summary photo shows the data port open, 364/364/0 link TX
sent/completed/failed, and Ethernet TX 360/0 and RX 4315/0 queued/dropped.
RX reasons are accepted=4317, format=15345 and replay=2, with no other nonzero
reasons displayed. The accepted total includes two EAPOL frames. The format
counter includes management frames and unsupported data formats such as QoS;
it does not identify 15,345 corrupted peer packets. Likewise, management-parser
`Link RX ignored` includes data frames later handled by the data parser. The
existing short RX trace excludes QoS and preserves its earliest samples, so
these counters do not establish why peer traffic is missing.

Windows selects source `192.168.1.31` and the on-link Ethernet /24 for the P1.
The router responds to host ping, but the P1 neighbor remains unresolved
(`00-00-00-00-00-00`, Unreachable), and direct SSH times out. A bounded helper
also tried a temporary `192.168.1.20/32` route through `192.168.1.1`: both
pings and strict-host-key SSH timed out. Helper PID 27944 exited, its route
was removed, and Windows DHCP remained Enabled. Results are in
`build/p1-router-probe-20260925-002702/`. No radio state or kernel code was changed
for these reachability probes. Router/repeater forwarding and the driver's
handling of peer traffic remain hypotheses, not established causes.

A repeated packet capture covered the user's confirmed P1-to-PC and
P1-to-router pings from 00:44:32 to 00:46:26 local time. The recovered evidence
is in `build/p1-peer-capture-20260925-004432/`: `peer.etl`,
`peer-recovered.txt` and `ethernet.pcapng`. The ETL SHA256 is
`57312862e821630f99e248fae1dc1dd063b75a5e22f4d8b658725cb481ef055f`.
The earlier capture was missed by the user and yielded no usable packet
export; its empty export is not negative network evidence.

At the Intel I211 NIC boundary (PktMon component 13, Windows interface 14),
the valid capture contains 21 packets with no ETW events lost:

- Four PC-to-router ICMP requests and four matching replies provide controls
  before and after the peer test.
- Seven broadcast requests ask for `192.168.1.20` from `192.168.1.31`, without
  a matching ARP response from the P1.
- Three unicast ARP requests from the router ask for the PC, and the PC replies.
  Their timing alone does not establish proxy ARP or a causal link to P1 ping.
- No captured ARP/ICMP frame arriving at the PC identifies the P1 source IP or
  MAC. This observation cannot distinguish AX200 handling from AP/repeater
  forwarding and does not capture the P1's successful router traffic.

This Windows PktMon version wrote the memory-mode dump to its working
directory rather than the requested start filename. The timestamp-matching
dump was recovered from System32; the capture helper now sets its working
directory inside `build/` and performs cleanup independently of conversion.
`recovery.json` confirms capture stopped and all owned filters removed;
the earlier helper's stale `status.json` is not authoritative. No kernel,
radio or firewall configuration was changed by these captures.

The next P1 photo confirms `192.168.1.0/255.255.255.0` directly on `wlan0`,
with default gateway `192.168.1.1`. Router ping is 4/4 while PC ping is 0/4.
The last twelve ARP log lines show requests from the router and calls returning
from `eth_send`; no PC-origin request appears in that short excerpt. This does
not prove the absence of earlier PC requests or delivery of every ARP reply.

A bounded unicast probe then temporarily installed only the known P1 neighbor
`192.168.1.20 -> 6c:6a:77:eb:bd:15` in Windows ActiveStore. Router ping worked,
P1 ping lost all three packets, and strict-host-key SSH timed out with exit
255. The entry was removed, helper PID 48508 exited, and DHCP stayed Enabled.
Evidence: `build/p1-unicast-probe-20260925-010200/`. Bypassing host ARP resolution
alone did not restore reachability. A direct PC cable to the main router is
not available, per the user. The next feasible control is to preserve the
current WLAN status and reconnect the P1 to MOVISTAR_2A6A, the repeater network
where SSH previously worked, leaving the PC unchanged. This changes both the
AP path and cipher configuration, so success would not by itself distinguish
forwarding from a driver issue. No isolation or driver diagnosis is yet
established.

### Repeater control and restored SSH (2026-09-25)

After the user reconnected the P1 to the repeater, its APIPA address was
`169.254.247.236`. The previous PC helper for `169.254.194.99` stopped with
successful address/route cleanup and DHCP unchanged. A new 30-minute helper
installed only source `169.254.167.94/32` and the new peer's on-link route,
with SkipAsSource and duplicate-address detection. Strict-host-key SSH then
succeeded immediately and confirmed AX200 MAC `6c:6a:77:eb:bd:15`.

Read-only results are in `build/p1-repeater-20260925-014241/`; all six SSH
commands returned zero. They include current interface/routes, WLAN status,
filtered DHCP/ARP logs and the saved 5 GHz WLAN snapshot. The shell's `cat`
returned only the first 4096 of 4377 bytes of that saved file, explicitly
reported in the output; this is not a complete file transfer.

- The earlier direct 5 GHz transaction received an OFFER and ACK from
  `192.168.1.1`, assigned `192.168.1.20`, and acquired an 86400-second lease.
- The repeater transaction sent four DISCOVER requests with XID `0xE2355B9B`
  and logged no received OFFER. Its radio trace contains reflected client
  UDP 68-to-67 traffic, not a server response.
- Current session 2 is associated on channel 1 with the data port open;
  link TX is 103/103/0 sent/completed/failed, Ethernet RX 802/0 queued/dropped,
  and RX reasons accepted=804, format=2908, replay=5. Local IP/SSH works with
  APIPA even though DHCP did not acquire a lease.
- The later saved 5 GHz summary has accepted=10933, format=53380 and
  replay=5746. Its earlier two replay samples do not describe the whole
  session; the limited trace cannot explain the later growth or establish a
  peer-specific drop cause.

These controls separate DHCP acquisition from working local IP/SSH. They do
not prove where server offers disappear through the repeater or why peer
reachability fails on the main AP. No kernel update, Wi-Fi security change or
replay-check relaxation was made during this read-only comparison.
