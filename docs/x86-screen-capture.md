# Remote Screen Capture

`osito-screen-v1` is a read-only SSH subsystem for on-demand screenshots. It
uses the existing TinySSH service, authorized Ed25519 keys and pinned host
identity. There is no additional listener, password, RFB/VNC compatibility,
input injection or continuous video stream. Start `sshd` on the device as for
normal [remote maintenance](x86-ssh.md).

The host client requests a frozen RGB snapshot, verifies its SHA-256, then
encodes it as PNG locally. No screenshot is written to the device's filesystem.
The account can see everything on the local display, including visible secrets;
treat received PNG files as diagnostic data with the same access restrictions
as other maintenance logs.

## Usage

Python 3 and OpenSSH are required on the receiving computer; no Python imaging
package is needed. Use the current device address, not a hardcoded network.

```text
python tools/osito-screen.py --host osito@DEVICE_IP -i CLIENT_KEY --known-hosts KNOWN_HOSTS --host-key-alias osito-p1 build/p1-screen.png
```

`--bind-address` selects the source IP for link-local connections. On Windows,
`--ssh-config NUL` ignores unrelated SSH configuration; on Linux use
`--ssh-config /dev/null`. `--port` defaults to 22. Host-key checking and
noninteractive public-key authentication remain enabled.

Existing output files are not replaced unless `--force` is specified. Output
is published only after all bytes and their checksum are validated. The JSON
result includes dimensions, byte count, SHA-256, capture uptime, output path,
elapsed time, block payload bytes (`wire_bytes`) and `transfer_seconds`.
`--chunk-size` defaults to 32768 bytes. Independent zlib block compression is
enabled by default; `--raw` disables it for transport comparisons. Older kernels
that reject the first compressed request fall back to raw reads. `--timeout` is
a per-request timeout, not a total transfer deadline.

## Capture Contract

- V1 reads the primary, permanently mapped GOP framebuffer. This includes the
  physical P1 and QEMU standard VGA. It uses the saved boot mode and physical
  VRAM pointer, not the current console drawing target: opening the desktop
  redirects the terminal drawing target to a smaller offscreen surface.
- A primary native GPU scanout or VGA-compatible Virtio GPU returns an explicit
  unsupported-backend error. A secondary Virtio device does not hide GOP. No
  stale GOP image is presented as a successful capture of another backend.
- GOP RGBX/BGRX is converted to tightly packed, top-down RGB8. Source pitch is
  honored; row padding and reserved fourth bytes are never transmitted.
- At most one snapshot is held per authenticated connection, bounded to 16 Mi
  pixels (48 MiB RGB) and 16384 pixels per dimension. Allocation failure is
  reported without changing display state. Capture checks a 30-second wall
  deadline per row and yields after an elapsed four-millisecond slice rather
  than every 32 rows. A fast copy no longer waits behind the ready queue after
  its first row. Ordinary timer preemption remains enabled; the renderer is
  not stopped. Scheduling delays still count toward the wall deadline.
- Geometry, format, mapping and ownership are checked before and after copying.
  Changed state discards the snapshot. GOP is live scanout memory: copying is
  not vblank-synchronized or atomic, so moving content can tear. This limitation
  is explicit in metadata and the client's `atomic: false` result. The completed
  RGB buffer remains immutable for the rest of the transfer.
- Release, session termination and protocol failure wipe and free the snapshot.
  During a copy, service stop, changed network epoch or a TCP connection no
  longer established cancel at the per-row check. A kernel-wide hang or disabled scheduling can also
  prevent SSH from answering; this is not an out-of-band management controller.
- Capture does not execute a shell command or take its execution lock, so it can
  run while the local shell is busy. The SSH service still permits only one
  active connection at a time; close an interactive SSH session first.

## Wire Protocol

The transport is an SSH subsystem request without a PTY. Integer fields are
unsigned big-endian. Each request/response is prefixed with a u32 payload
length, bounded to 32800 bytes. A response starts with a one-byte status;
nonzero status has no additional payload. No client-supplied physical address,
stride, dimensions or memory destination is accepted.

| Request | Payload after opcode | Successful response after status |
| --- | --- | --- |
| `V` | none; first request only | `OSCR0001`, u32 block limit 32768 |
| `C` | none; no snapshot already held | 72-byte metadata |
| `R` | u64 offset, u32 count | exactly count RGB bytes |
| `Z` | u64 offset, u32 count | u8 encoding, encoded block bytes |
| `X` | none | empty; idempotent release |
| `Q` | none | empty; release and finish session |

Metadata consists of six u32 fields (width, height, format=1 RGB8, source=1
GOP, flags=1 non-atomic, reserved=0), two u64 fields (RGB byte count and capture
start uptime in milliseconds), and 32 SHA-256 bytes. Reads must be nonempty,
within the held snapshot, and no larger than the negotiated block limit.

`Z` is an optional compatible extension. Encoding 0 carries exactly count raw
bytes; encoding 1 is an independent zlib stream that must decode to exactly
count bytes. The kernel chooses compression only when the complete stream is
smaller. Incompressible blocks fall back to raw. The client bounds decompression
and rejects excess output, truncated streams and trailing data before verifying
the snapshot SHA-256. A status-2 response to the first `Z` request selects `R`
for legacy kernels; transport errors never silently trigger fallback.

Status codes: 0 success; 1 framebuffer unavailable; 2 invalid request/geometry;
3 allocation failed; 4 unsupported scanout; 5 snapshot already held;
6 scanout changed; 7 capture cancelled; 8 capture wall deadline exceeded.
Older kernels combine all three causes in status 6; the client keeps an
explicit legacy diagnostic for that ambiguous response.

SSH subsystem semantics follow [RFC 4254 section 6.5](https://www.rfc-editor.org/rfc/rfc4254.html#section-6.5).
The host encoder writes RGB8 IHDR/IDAT/IEND chunks with CRC32 and zlib according
to the [PNG specification](https://www.w3.org/TR/png-3/).

## Validation

```sh
bash arch/x86/scripts/test-screen-capture.sh build/screen-unit
CC=clang SANITIZE=1 bash arch/x86/scripts/test-screen-capture.sh build/screen-sanitize
python3 tools/test-osito-screen.py
SCREEN_CAPTURE_TEST=1 FILE_SYNC_TEST=1 bash arch/x86/scripts/build-ssh-host.sh build/screen-ssh
python3 arch/x86/scripts/test-screen-ssh.py arch/x86/build/screen-ssh
python3 arch/x86/scripts/test-file-sync-ssh.py arch/x86/build/screen-ssh
bash arch/x86/scripts/test-file-sync.sh build/screen-sync-regression
make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf
bash arch/x86/scripts/test-ssh-qemu.sh arch/x86/build/screen-qemu-new
```

Core tests cover RGB/BGR, padded rows, immutable reads, overflow/range checks,
allocation failure, cancellation/time limits, geometry changes and wipe/free.
The 2026-09-27 extension passes 292 checks with GCC and Clang ASan/UBSan,
including a slow ready-queue fixture which must not be entered for sub-slice
work, distinct timeout/cancel/geometry statuses and cancellation after yield.
The two live PE32/PE64 snapshots in `build/ssh-independent-qemu-20260927-r1/`
were received while four application workers remained runnable. Physical
Steam-load capture still requires verification.
Client tests decode generated PNG independently and cover malformed metadata,
checksum mismatch, short blocks, existing destinations and symlink rejection.
OpenSSH tests exercise fragmented binary transfers over 1 MiB, rekey, repeated
captures, disconnect cleanup, PTY rejection and console reconnection.
All five OpenSSH gates also pass with the 2026-09-27 changes in
`build/ssh-screen-independent-host-20260927-r1/`.

QEMU checks exact console RGB equality against an independent QMP screendump,
capture while a local command is busy, and full-desktop geometry after terminal
redirection. The desktop comparison allows changing clock/FPS counters, unlike
the exact console comparison. Existing SSH, file-sync, cold-boot and kexec
regressions run in the same suite. Physical AX200/GOP validation is separate.

Validated on 2026-09-25: 119 core checks with GCC and Clang ASan/UBSan; four
client tests on Windows and Linux; five OpenSSH screen gates with and without
sanitizers; 12 file-sync unit tests, four file-sync SSH gates and 11 SSH console
gates. The production QEMU run passed all 19 gates. Its console pixels matched
QMP exactly; the desktop differed in 867 of 3072000 channel bytes while its
live counters advanced during transfer. Both received PNGs were inspected.
Artifacts are under ignored `arch/x86/build/screen-desktop-qemu-20260925/`.
This records VM/host validation, not a completed physical P1 capture.

## Physical P1 Validation

On 2026-09-25 the P1 reconnected at `192.168.1.20` after an explicit
`kexec kernel-screen.elf`. Its existing SSH host identity was retained. The
downloaded 7130128-byte kernel was subsequently read back through authenticated
file-sync hashes and matched local SHA-256
`bf649cbf931bbb38ad5c017a180a5ec1bc10cdb12ffac6da1defd602edf9b3aa`.
The USB UEFI boot kernel was not replaced.

Two screenshots of the running desktop were received over AX200 Wi-Fi,
checksum-verified and visually inspected at 1920x1080. Both include the complete
desktop, terminal window, cursor, top status bar and bottom dock, rather than
only the redirected terminal surface. Each snapshot contains 6220800 RGB bytes.

| Read size | End-to-end time | Saved PNG bytes |
| --- | --- | --- |
| 4096 bytes | 137.157 s | 14967 |
| 32768 bytes | 104.454 s | 15194 |

These are two smoke measurements, not a controlled throughput benchmark. The
larger read size works on this physical receive path. These measurements used
the original raw-RGB kernel, before the optional `Z` extension. They are not
measurements of compressed transfer or a claim of interactive remote video.
Delta updates and higher transport throughput remain performance work, not
completed VNC functionality.

`mem` reported the same 143768 KiB used after both captures; this checks no
additional physical-memory growth between these two operations, not a general
proof of allocator reclamation. Normal SSH commands and file-sync still worked
after screenshot sessions closed. Evidence, PNGs and deployment state are in
ignored `arch/x86/build/p1-screen-20260925/`. The temporary bootstrap servers,
firewall rules and old APIPA helper were removed after deployment.
