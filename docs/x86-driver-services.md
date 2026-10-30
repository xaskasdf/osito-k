# x86 Driver Lifecycle

The background driver worker starts when the shell becomes available, before
application autoload. It does not block entry to the shell. Boot-critical
storage, input and display remain owned by early boot and are not reset by the
worker. Initial adapters are AX200 WLAN, IPv4 configuration, SSH and HDA audio;
GPU firmware and other drivers have not yet been migrated.

Each service has explicit state, a serialized operation owner, a retry deadline
and its own versioned configuration. Failure leaves diagnostics and the shell
available. A failed DMA shutdown quarantines WLAN instead of repeatedly resetting
hardware whose ownership is uncertain. `/.nodrivers` disables worker startup for
recovery; manual commands remain available.

## First Connection And Subsequent Boots

On a device with its firmware already installed:

```text
drivers status
wlan connect
sshd autostart on
```

Select a known network in the local assistant. A successful open/WPA2 connection
is remembered before requesting DHCP. Password entry no longer echoes. The
profile stores the SSID, security policy and derived WPA2 PMK, not the entered
passphrase. Existing installations need this one-time connection: older kernels
did not persist their in-memory credentials.

SSH autostart requires an existing host identity and authorized client keys;
provision them as described in [SSH maintenance](x86-ssh.md). Automatic startup
never creates or replaces an identity. The default remains opt-in. Once enabled,
the next boot loads the profile, starts firmware, scans, joins a compatible known
network, obtains IPv4 and starts SSH. A link change retires the old SSH listener;
the worker starts another after the link and address are ready.

```text
wlan saved
wlan forget 1
wlan auto off
wlan auto on
drivers retry wlan
drivers ipv4 auto
drivers ipv4 dhcp
drivers ipv4 apipa
sshd autostart on 22
sshd autostart off
drivers hda off
drivers hda on
```

`wlan saved` displays SSID/security, never the PMK. Up to 16 explicit profiles
are retained. A full store reports failure rather than evicting an unrelated
network. Matching requires the exact SSID and compatible security/group cipher;
a secured profile never falls back to an open network. Unknown networks require
explicit selection. A working selected Ethernet adapter is not displaced by
WLAN. Hidden SSID discovery and WPA3/Enterprise are not implemented.

IPv4 defaults to DHCP followed by APIPA on failure. The alternatives select
DHCP-only or APIPA-only. An existing address is not changed automatically; saved
policy takes effect on the next configuration. Static address persistence and
host discovery after an APIPA address change remain open.

`wlan stop` pauses automatic WLAN actions for this boot. `sshd stop` similarly
suppresses automatic restart until manually enabled again or rebooted.
Manual `wlan start <firmware>` and `wlan starttrace <firmware>` also leave WLAN
automation paused, so a subsequent `wlan connect` can select another network
before the worker rejoins the old saved profile. `wlan connect`, `wlan auto on`
or `drivers retry wlan` resumes automation; an explicit `wlan auto off` policy
still takes precedence. Status and scan commands preserve the pause.
`sshd autostart off` preserves an existing connection and disables future
automatic starts. HDA policy applies at initialization, not by tearing down
an active audio stream. No supported codec/output is a reported service failure,
not a reason to halt the shell.

## Configuration And Firmware

Each service uses `/.driver-<name>.conf`, with a versioned payload inside an
`ODCFG001` envelope and SHA-256 checksum. Files are regular, mode 0600. Writes
use private staging, flush, readback, atomic rename and a final flush. Corrupt
existing configurations are preserved and reported. These files are not
encrypted at rest: the PMK is a secret, and privileged raw filesystem access
can read it. Do not include WLAN configurations in diagnostic bundles.

AX200 verifies `iwlwifi-cc-a0-77.ucode` against the same pinned version and hash
as `scripts/fetch-ax200-firmware.sh`. If absent and another adapter provides a
routed connection, the worker fetches its Intel license and firmware via HTTPS,
verifies their pinned hashes and publishes them without replacing existing
files. Redirects, oversized bodies, wrong hashes and link changes fail cleanly.
HTTP sessions are serialized because the existing parser/TLS path has shared
state; a busy client causes a later retry. No certificate bypass was added.

Without another connection, missing WLAN firmware remains `waiting-firmware`;
the shell can receive a manually copied file. An invalid existing firmware is
preserved, not silently replaced. The live Internet download path is not yet
validated on the physical P1.

## Transfer Performance

AX200 data transmission no longer uses the fixed authentication/basic rate.
An ACK/retry-based controller starts conservatively, moves through the AP's
negotiated common legacy rates after clean completions, and backs off after
failure/excessive retries. Authentication/EAPOL retain the basic rate. The RX
worker briefly polls while TX is pending instead of sleeping a full timer tick
between every active frame; its bounded burst remains preemptible.

This is not a 600 Mbps implementation. HT/VHT/HE negotiation, aggregation and
multiple outstanding TX slots remain open. The current maximum legacy PHY rate
is 54 Mbps, not a promised application throughput. TCP buffering and synchronous
file RPCs are additional limits. Physical before/after measurements are separate
from host and VM correctness tests.

[Screen captures](x86-screen-capture.md) can now use independently compressed
32 KiB blocks, avoiding transmission of a full raw framebuffer for a mostly
static desktop. Raw mode remains available to measure transport separately.

## Validation

```sh
bash arch/x86/scripts/test-driver-services.sh build/driver-services-host
bash arch/x86/scripts/test-wlan-shell.sh build/wlan-auto-shell
bash arch/x86/scripts/test-iwx.sh build/iwx-auto
bash arch/x86/scripts/test-screen-capture.sh build/screen-compressed
python3 tools/test-osito-screen.py
make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf
bash arch/x86/scripts/test-ssh-qemu.sh arch/x86/build/drivers-qemu
```

On 2026-09-25 the service, store and firmware fixtures passed 119, 51 and 35
checks respectively with GCC/Clang and ASan/UBSan.
HTTP session ownership passed 151 checks with both compilers and sanitizers,
including synchronous/async failure cleanup and rejection of overlapping opens.
The WLAN shell suite passed 158680 checks in each configuration. AX200 frame,
DMA, crypto, rate selection
and network integration suites passed. Compressed capture passed 280 core checks,
six client tests and five real-OpenSSH gates.

The production QEMU suite passed all 20 gates, including cold-boot SSH autostart
and automatic reconnect after remote `kexec`, with unchanged pinned host key
and persistent files. Captured console pixels matched QMP. Artifacts are in
ignored `arch/x86/build/drivers-rate-qemu-20260925/`, kernel SHA-256
`62308b7ce3abc9559883f1e16f6542b0f3388195b3f668ba1e29a741a055190e`.
QEMU uses virtio-net; physical AX200 reconnection and throughput require their
own validation. A remote `kexec` does not update the USB UEFI boot kernel.

The first physical attempt reported firmware file-read failure despite a
mounted filesystem. USB readback confirmed the installed firmware and license
hashes. A concurrent USB BOT ownership defect was reproduced and corrected;
see [storage ownership and regression evidence](x86-usb-storage.md#concurrent-firmware-reads-and-diagnostic-writes-2026-09-25).
The final 7,152,832-byte kernel, SHA-256
`7b798fdb1cfecedb1a7d2b3d6983a93fd66240105b62adfc7e72556523199841`,
passed all 20 integration gates again with xHCI USB storage. Artifacts are in
`arch/x86/build/drivers-usb-qemu-20260925/`. This does not replace physical
AX200 reconnection or throughput validation.

## Manual Network Selection Regression

The first driver-manager kernel resumed automatic selection immediately after
`wlan start`. With a saved network, the worker could associate before the user
entered `wlan connect`, which correctly rejects an already active link. Manual
`start` and `starttrace` now retain the pause until an explicit connection or
resume command. Status and scan preserve ownership policy; no radio, cipher or
band-selection behavior was changed.

The regression exercises the shell's production release policy and the real
manager scheduler across stop/start/status/scan/connect and explicit resume.
The WLAN suite passed 158,743 checks with GCC/Clang, normal and ASan/UBSan,
in `arch/x86/build/wlan-manual-owner-20260925/`. Kernel SHA-256
`010cbc9f63e14a5ae4d5bbf5db102390ea5400320acd3f0d44359a5300966095`
(7,152,872 bytes) passed all 20 USB QEMU gates in
`arch/x86/build/drivers-switch-qemu-20260925/`. This kernel is built and tested,
not yet deployed to the physical P1.

On the previously installed kernel, temporarily run `wlan auto off`,
`wlan stop`, `wlan start iwlwifi-cc-a0-77.ucode`, then `wlan connect`.
After selecting and connecting to the desired network, `wlan auto on` restores
the saved automatic policy. The P1's reported successful router ping but failed
PC ping on 5 GHz is a separate peer-reachability problem; this ownership fix
does not establish or repair its cause.

## Post-DHCP Startup Diagnostics (2026-09-26)

The P1's saved `diag/boot2.log` from a reportedly frozen physical reboot
contains an accepted DHCP OFFER and ACK, a configured `192.168.1.20`, and
`dhcp-done`. The next, manually restarted boot also acquired DHCP and SSH.
Both logs originally ended at that checkpoint: it was not evidence that
DHCP, the final filesystem flush, SSH initialization, or CPU idle caused the
freeze. The four original ring slots were pulled before any further reboot
into ignored `arch/x86/build/p1-dhcp-freeze-20260926/diag/`.

Automatic WLAN selection now logs the sanitized SSID and channel, then
`wlan-auto-selected` and `wlan-auto-linked`. IPv4 initialization records
`driver-ipv4-ready` or `driver-ipv4-failed` after returning from DHCP/APIPA.
Actual SSH startup records `sshd-start`, `sshd-keys-ready`, and `sshd-ready`,
or `sshd-start-failed` with its existing error. No password, PMK, or private
key is logged; ready-state polling does not repeat these checkpoints.

Boot diagnostics now create the `diag` directory on a fresh OsitoFS3 volume.
Previously only volumes with an existing directory saved the ring logs.
An existing non-directory entry or failed mkdir is preserved and diagnosed,
not deleted. The flat OsitoFS2 namespace is unchanged. This was reproduced
in QEMU, not identified as the P1 freeze's cause.

Each marker is also a serial/klog line. If another diagnostic flush is in
progress, a later snapshot may persist that line under another `reason=`;
`latest.txt` describes the last flush, not necessarily the last service event.
Inspect the log contents, not only its final reason. Shell `cat` displays
only the first 4096 bytes of a file; use `grep`, `tail`, or `osito-sync pull`
for complete boot diagnostics.

```sh
bash arch/x86/scripts/test-boot-diag.sh build/boot-diag-host
bash arch/x86/scripts/test-driver-services.sh build/driver-startup-host
OSITO_SSH_TEST_STORAGE=usb OSITO_SSH_TEST_STARTUP_ONLY=1 \
    bash arch/x86/scripts/test-ssh-qemu.sh arch/x86/build/startup-usb-qemu
```

Boot-log initialization/checkpoint tests passed 156 checks, and service tests
passed 170 checks with GCC/Clang, normal and ASan/UBSan. DHCP/APIPA regressions
also passed. USB QEMU passed authenticated SSH, rejection of an unauthorized
key, persisted startup markers, and automatic reconnect with the same host
key after `kexec`. Evidence: `arch/x86/build/driver-startup-qemu-20260926-r4/`,
kernel SHA-256 `2e38259f8bab57cfb3389d08832836ac70845016c079e30cb7780eaeb385fe56`.

The physical P1 was still on the previous kernel when it stopped responding
to ARP/SSH during this investigation, before the upload could start. The PC
could still ping the gateway. The user subsequently confirmed that the battery
had discharged; this interruption is not evidence of another kernel or WLAN
hang. SSH returned on the same IP after charging/restarting.

The diagnostic kernel above was then uploaded with SHA-256 verification and
installed using `kupdate --file /kernel-startup-20260926.elf --no-kexec`.
The P1 reported verified OsitoFS and EFI kernels, with the previous EFI kernel
saved as `/kernel.previous.elf`. Before the update, the console was idle and
WLAN was associated on channel 6. A subsequent remote `reboot` did not restore
SSH during a two-minute retry window; ICMP also timed out and the PC's neighbor
entry became unreachable. This alone cannot distinguish a boot stall from a
different address, network, or power issue. Local screen/keyboard state and the
new boot ring still need to be collected; no fix for the original reported
freeze is claimed. Upload, installation, reboot and reconnect evidence are in
`arch/x86/build/p1-dhcp-freeze-20260926/`.

After the user confirmed the frozen keyboard and manually power-cycled once,
SSH returned at 33 seconds. The recovered ring (`diag-after-frozen-reboot/`
in that directory) advanced only to slot 1, which contains the new kernel's
WLAN selection on channel 6, `driver-ipv4-ready`, and all three SSH startup
markers. Slot 0 still belongs to the earlier, working boot on the old kernel.
There is no separate boot slot for the intervening remote `reboot` attempt.
The displayed DHCP address can therefore be stale console output, not proof
of a new boot reaching DHCP. This redirects the investigation to the reset
path; see [ACPI reboot validation](x86-kupdate.md#acpi-reboot-path).
