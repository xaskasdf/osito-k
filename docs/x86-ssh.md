# x86 SSH Maintenance Console

The former `kernel/sshd.c` stub is replaced by an opt-in SSHv2 service using
TinySSH 20260906, commit `50c1e9903b866ff320cba667013a83ebe5cfcc83`, under its
0BSD license option. Selected upstream protocol and crypto files are unchanged;
see `arch/x86/lib/tinyssh/README.osito.md`. This is not wolfSSH and adds no GPL
dependency. It uses the existing TCP backend, secure RNG, shell and OsitoFS3.

## Provisioning

Generate a dedicated Ed25519 client key with OpenSSH on the development host.
Keep the private key there. Copy only its public `.pub` file to the root of
the guest filesystem as `.ssh_authorized_keys`. The file accepts up to 32 plain
OpenSSH `ssh-ed25519` lines, comments and blank lines, within 16 KiB. Options,
other key types and malformed lines fail the entire load instead of being
silently accepted. The only remote user is `osito`; passwords and `none`
authentication are disabled.

On Osito-K:

```text
sshd keygen
sshd key
sshd start
sshd status
sshd stop
```

`sshd start [port]` defaults to TCP 22 and creates the host identity if missing.
`.ssh_host_ed25519` stores 96 bytes (Ed25519 public key and secret key), mode
600. Creation uses a staging file, flush, readback and rename. Existing invalid
keys and unfinished `.ssh_host_ed25519.new` are reported, never silently
replaced. Stop the service before reading/reloading keys. Automatic startup is
disabled by default. With existing host and authorized keys, `sshd autostart on`
persists opt-in startup after network configuration; `sshd autostart off`
disables future automatic starts without closing the current session. See
[driver lifecycle and saved WLAN profiles](x86-driver-services.md). Automatic
reconnect does not generate or replace host keys.

Compare `sshd key` through the local console before trusting the host identity
in OpenSSH. Do not disable host-key checking. For the current P1 test network:

```text
ssh -b 169.254.167.94 -i C:\Users\xasko\.ssh\osito_p1_ed25519 osito@169.254.167.93
```

This address pair is development configuration, not a kernel constant.
Authorized-key access is privileged maintenance access to the kernel and its
filesystem, not an isolated multi-user account.

## Contract And Limits

- One connection and one session channel at a time. TinySSH's protocol state
  is process-global, so the service serializes access and stops on link changes.
- Ed25519 authentication, Curve25519 and upstream hybrid KEX, ChaCha20-Poly1305,
  rekey and channel flow control use the vendored implementation. Partial writes
  and a temporarily full TCP transmit queue are retried, not treated as EOF.
- Initial handshake/authentication and rekey have 30-second deadlines; idle
  sessions expire after ten minutes. Console sessions close at 1 GiB total
  traffic; the binary file subsystem uses rekey without that traffic cap.
- Protocol/entropy failures unwind to the connection boundary, not a kernel
  halt. Credentials, packet buffers and command buffers are cleared on teardown.
- `exec` and a basic interactive line console support maintenance commands,
  diagnostics, pipes and file redirection. Output belongs to the remote task;
  it does not appear on the hardware console or in the local command history.
  Existing builtins can still explicitly log diagnostics to the kernel log.
- The remote session owns its input, capture buffers and output, independently
  of the local shell. Diagnostics, text filters, `ls`, `hexdump`, `touch`, `cp`,
  `mv`, `rm`, `sync`, pipes and file redirection can run while a foreground
  application retains the local prompt. Filesystem concurrency uses the same
  filesystem operations as the binary transfer subsystem. A read of a live
  application log is not an atomic snapshot.
- Individual machine-wide commands still try the exclusive command gate:
  `kexec`, `kupdate`, reboot, network reconfiguration and non-audited builtins
  reject contention with status 126 before opening any redirect. Nothing is
  deferred. The independent set is `shell_remote_independent_command`; it does
  not grant remote access to commands outside `shell_remote_supported`.
  Ordinary remote diagnostics do not acquire the gate even when the local
  prompt is idle. Screen and file subsystems do not use shell execution.
- Direct remote execution of local keyboard/graphics programs (`desktop`, DOS,
  Win32, ELF launchers) is rejected with status 126. The explicit `console launch`
  handoff below runs supported launchers on the local shell task instead.
  WLAN remote commands are limited to diagnostics;
  association/password entry remain local. The allowlist lives in
  `shell_remote_supported`. No SFTP, forwarding or general POSIX PTY. The
  `win32-loader` query, `clear`, `flush`, and `strict on|off` are supported with
  exact argument validation. All require the exclusive command gate, including
  the query; they cannot run alongside a foreground console application. Import
  policy is global, so diagnostic clients must restore its previous setting.
  The [file transfer subsystem](x86-file-sync.md) accepts `osito-sync-v1` for
  authenticated binary/incremental copies separately from the shell.
  The [screen capture subsystem](x86-screen-capture.md) accepts `osito-screen-v1`
  for read-only GOP snapshots, also independently of local shell execution.
- PTY requests enable the basic editor and accept window size, not a terminal
  emulator or full POSIX termios/signal contract. Ctrl-C clears an input line;
  it does not interrupt an executing builtin. Builtin error statuses are not
  yet a general POSIX shell status contract.
- Console commands are bounded to 32 KiB and captured output to 1 MiB. Overflow is
  reported explicitly and closes the command with an error, never a partial
  success. Output is sent after a synchronous builtin finishes. Binary file
  transfers stream bounded frames and are not console captures.

The exception-diagnostic hardening in section 4.163 of
`win32-layer-correctness.md` was also checked against this admission contract.
Frozen kernel `6b97c814c7e776448bab59330854bf77c1bd29f9dea28fa3d3f9174028484442`
passes all 28 production SSH gates in `build/ssh-seh-full-20260927-r1/` and
155 shell host checks in each compiler/sanitizer variant. The PE32/PE64 malformed
exception fixtures no longer fault while logging and leave SSH usable. These
checks do not establish availability through an arbitrary kernel deadlock or
loss of the physical P1's WLAN connection.

On the same image, `build/steam-seh-diagnostic-20260927-r1/` passes twenty
maintenance probes during ten minutes of real Steam, an authenticated GOP
capture (0.669 seconds total), exclusive-command rejection and cooperative
stop returning the prompt on the third status poll. Post-stop echo/dmesg also
work. The store, Library and Pixel Dungeon details render, and all six Steam
logs are retained. This is QEMU evidence, not deployment to the unreachable
P1 or proof that earlier intermittent startup stalls cannot recur.

The fifteen-minute repeat `build/steam-stock-pixel-20260927-r1/` on that same
6b97c814 kernel passes 24 of 25 scheduled maintenance connections. The
360-second console probe fails during SSH banner exchange in 38.996 seconds;
the four subsequent probes at that checkpoint and all later ones succeed.
This failure occurs before shell command dispatch, not at the exclusive gate.
The authenticated snapshot takes 0.720 seconds, and cooperative stop returns
the console on its third status poll. Steam's main content remains black and
WebHelper reports an unresponsive browser; this is not a successful game test.
No fatal CPU exception is logged. The P1's UI is reported responsive, but the
host still cannot resolve ARP for 192.168.1.20 after Steam is closed. Neither
that reachability failure nor the intermittent banner failure is attributed
to a shell lock without further evidence.

## Failure Diagnostics

The OS adapter now logs a bounded line when a protocol session fails:

```text
[SSHD] session failed phase=banner-receive reason=deadline rx=0 tx=24
```

The stage distinguishes banner send/receive, key negotiation, authentication,
channel processing and console/file/screen callbacks. The reason distinguishes
deadline, cancellation, traffic limit, entropy, transport read/write closure
and otherwise-unspecified protocol failure. RX counts bytes consumed from TCP;
TX counts bytes accepted by the TCP send callback, not peer acknowledgements
or proof of delivery over WLAN. Only static labels and byte counts are added;
no keys, command strings or packet payloads are logged. Inspect with
`dmesg | grep SSHD` after a failed connection.

This is instrumentation, not a timeout increase or a change to command
admission, authentication or cancellation. An arbitrary blocked kernel
callback still cannot be preempted by these diagnostics. Upstream TinySSH
remains unchanged. GitNexus was unavailable; manual scope review covered
osito_ssh_session, its I/O/cancellation boundary, the SSH worker caller and
the real-OpenSSH host fixture.

The fixture injects six first-session failures (blocked banner write/read,
closed write/read, cancellation and unavailable entropy), checks the stage,
reason and byte counts, then authenticates a new session successfully. Its
clock acceleration is host-test-only. All 17 interoperability gates pass in
`build/ssh-phase-host-20260927-r1/` (GCC) and
`build/ssh-phase-host-clang-san-20260927-r2/` (Clang with ASan/UBSan).
The first sanitizer run passed the original ten gates but timed out waiting
five seconds for host fixture termination; it is retained as an incomplete
run, not counted as a pass. The identical instrumented binary passes the
second run without changing protocol deadlines or test assertions.

Kernel SHA-256
`56da67b9cfa10066233fc8126ce9defea305277525087e31d4fa6c7b298f2ef7`
(7,258,496 bytes) passes all 28 production SSH gates in
`build/ssh-phase-full-20260927-r1/`, including busy-shell diagnostics, files,
capture, cold boot and automatic SSH reconnect after kexec. The fifteen-minute
Steam run above predates this instrumentation; it is the motivation, not a
post-change end-to-end verification. Physical deployment follows below using
the integrated kernel with the Virtio correction.

The subsequent ten-minute run `build/steam-ssh-phase-qemu-20260927-r1/`
passes 24/60 scheduled requests, then loses virtual NIC communication while
the scheduler remains active. QEMU reports an invalid descriptor and Virtio
NEEDS_RESET; the failure is below SSH, not in shell command admission.
The production queue ownership correction and negative/positive tests are
recorded in [network stack, section 11](x86-network-stack.md).
The corrected kernel passes all 60 requests in the repeat
`build/steam-virtio-tx-qemu-20260927-r1/`, with the Store visible in its final
capture, successful cooperative cancellation and working post-stop SSH.

The P1 separately became unreachable through ARP after closing Steam; the
user reported live timers but no local command input. Its AX200 path is not
validated by the Virtio correction, and that boot's RAM logs were not recovered.
After the P1 returned from a restart, the console was idle. Uploading the
7,262,624-byte tested kernel succeeded, but a subsequent 4,209,929-byte
`webhelper.txt` download timed out while Steam was closed. A new SSH connection
then succeeded. This establishes a separate transient physical transfer
failure that does not require a foreground application, not its root cause.

`build/p1-ssh-recovery-20260927-r1/` records the persistent update of both
OsitoFS and the USB EFI kernel, followed by kexec of
`/kernel-maint-20260927.elf` (SHA-256
`be78fb60a6d273a6abb6decaddb205a2797b3c283a1b71d3017fad529b675431`).
The P1 returned automatically at `192.168.1.20` with the pinned SSH identity.
The driver manager reports WLAN/IPv4/SSH ready, on WLAN channel 6. The same
webhelper download then succeeded, as did a verified 1920x1080 capture in
5.609 seconds. One successful retry does not establish transfer reliability;
physical Steam-load qualification is recorded separately.

`build/p1-steam-maint-20260927-r1/` then launches desktop (request 1) and
Steam (request 2) over SSH. All fifteen scheduled uptime/console/dmesg queries
through 240 seconds complete, but the later requests become slow and all
three queries at 360 seconds time out. ARP subsequently resolves the same
P1 MAC again. A fresh authenticated `console stop 2` is accepted, and the
next status confirms `console: idle`, `last request: 2 returned`, without a
restart. Full dmesg, ps, WLAN status, echo and a verified 1920x1080 capture
are recovered. The capture takes 16.156 seconds and still shows the Steam
profile-selection window: the main owner's stop does not terminate all child
processes. `after-stop-ps.log` retains the remaining Win32 tasks; this is not
a complete Steam shutdown or a local-input rendering/focus qualification.

The recovered session diagnostics report `phase=authentication
reason=write-closed rx=1573 tx=552` and `phase=kex-receive reason=deadline
rx=33 tx=336`. These failed sessions have not reached command execution;
they are not waiting for the foreground shell's command lock. They do not
establish why transport/key negotiation stalled or imply invalid credentials.
WLAN remains associated on channel 6 with all 6,383 TX completions reported,
but its Ethernet RX queue drop counter reaches 706 (zero in the idle baseline).
That counter denotes a full 16-frame software receive queue, not a CCMP
authentication rejection. Further physical RX scheduling and TCP evidence
are needed before attributing the SSH timeouts to those drops.

## Local Console Handoff

Authenticated SSH can request one local foreground launch at an idle main
prompt, without occupying the SSH service worker:

```text
console status
console launch desktop
console launch win32-process-test
console launch sched-signal-test
console launch winexec Games/UT99/System/UnrealTournament.exe -safe
console launch dosrun DOOM.EXE -nosound
console stop 2
```

Paths containing spaces can be quoted as ordinary shell arguments. Only
`desktop`, `winexec`, `dosrun`, `reboot --check` and the argument-free
`win32-process-test` and `sched-signal-test` diagnostics are admitted. The copied request is bounded to
4096 bytes including quoting; this does not restrict the local line editor.
Redirections, nested command strings and command separators are not supported.
The main prompt polls for the handoff through interruptible keyboard input;
Wi-Fi, password and heredoc readers do not opt in. Locally typed input takes
priority when observed, cancelling a request racing the idle/editing edge.

The launch response confirms **queued**, not application success. `console status`
reports the last request ID and `queued`, `running`, `returned` or
`cancelled-local-input`, plus prompt availability. `returned` only means the
launcher returned, not that the application initialized correctly. Command
output and input stay on the device; the remote session gets no Win32 handles
or keyboard ownership. A busy or editing prompt rejects new launches without
leaving a deferred command. A second job is never accepted while one is queued
or running. Diagnostic SSH, file commands, redirection, screen and binary file
transfers remain available during the foreground job.

`console stop <request-id>` is an explicit termination request for a running
Win32 job launched through this mailbox. Use the ID returned by launch or
reported by status; the `2` above is only an example. The ID, running phase,
Win32 kind and owning kernel PID are checked while the mailbox is held. Old
IDs, queued jobs, DOS/desktop jobs and unavailable exit checkpoints are
rejected. It does not kill the SSH worker or local-shell task, inject a return
address, or bypass Win32 teardown. The main owner consumes the request at its
existing safe termination checkpoints, releases its resources and owned
threads, then returns to the prompt. Unsaved application state can be lost.
Acceptance is not completion: wait for `console: idle` and `returned` before
updating or reloading the kernel. A guest spinning without entering a safe
checkpoint or a kernel deadlock is not forcibly recovered by this mechanism.
Child processes have their existing independent lifetime; this is not a
recursive process-tree kill, general signal/PTY support or concurrent `kexec`.

Validation of this extension (2026-09-27):
`build/ssh-independent-qemu-20260927-r1/` uses frozen kernel SHA-256
`efe84a73de5fa79e0cd0a40e0a7a89c79e21d9b051597e69edb62a1ca2acf9c8`
(7,202,992 bytes). PE32 and PE64 fixtures keep a foreground owner asleep and
four runnable worker threads active. SSH writes/reads/copies/removes files,
captures and verifies a GOP snapshot, rejects stale stop IDs and exclusive
kernel/reboot requests, then stops the owner and verifies four owned threads
retired, prompt return and another authenticated SSH command. All 14 gates pass.
The shell host suite passes 155 checks with GCC/Clang, normal and ASan/UBSan.
The general production suite in `build/ssh-independent-full-20260927-r1/`
passes all 28 gates on the same ELF, including busy-shell redirection and
output isolation, rejected exclusive requests without redirect side effects,
authenticated file transfer, cold boot and automatic SSH recovery after kexec.
This is not yet a physical P1 or arbitrary hung-application recovery claim.
GitNexus was unavailable; manual scope review covered session I/O, per-command
admission, mailbox ownership and the existing Win32 termination checkpoints.

The same frozen image was SHA-256-verified by the P1's file subsystem as
`/kernel-ssh-independent-20260927.elf`. With `console status` reporting idle,
`kupdate --file ... --no-kexec` saved the previous EFI kernel, verified the
OsitoFS kernel and wrote/read-back-verified `usb0:EFI/BOOT/kernel.elf`.
Evidence is in `build/p1-ssh-independent-update-20260927.log`.
The following SSH kexec ended the connection; reconnection at 192.168.1.20
has not yet succeeded. This records a persistent update, not a confirmed
post-kexec boot or physical Steam-load validation. The current IP/state must
be established before continuing the hardware test.

A real Steam workload was also exercised in isolated QEMU with the later
[passive TCP retry](x86-network-stack.md#10-passive-tcp-cold-arp-retry-2026-09-27)
kernel, SHA-256
`45fa5bccf3c823cadd67e41fd5fcdb5c4f58facaf7a84d18fa25077736394cf9`.
`build/steam-ssh-independent-qemu-20260927-r3/` records fifteen successful
status/uptime/dmesg/write/read requests at 60, 180 and 360 seconds while the
foreground job remains active. Each completes in 0.150-3.963 seconds. The
1280x800 authenticated snapshot completes in 0.556 seconds (3,072,000 RGB
bytes, 62,476 compressed wire bytes, verified digest). A redirected exclusive
reboot request is rejected. `console stop 1` returns the foreground console
to idle/returned; another SSH command succeeds afterwards. This image has
not yet been deployed to the P1.

The captured desktop shows two WebHelper page faults and no usable Steam UI.
Their instruction pointers resolve to `SystemParametersInfoW` and
`NtAllocateVirtualMemory`; the faults precede the stop request. Thus the
maintenance-path result must not be presented as a Steam correctness fix.
The isolated disk used for that run did not contain the Segoe UI font already
installed on the P1. Its captured SystemParametersInfoW guard-page fault
matches the earlier missing-font recursion described in section 4.152 of
`win32-layer-correctness.md`, rather than establishing a new regression in
remote maintenance. Subsequent comparisons import the same 959,752-byte font
from the development host into the disposable disk only.
The general suite's first run on this ELF also exposes a separate intermittent
session-admission timeout after a post-cold-boot file pull, before kexec is
sent. It passes 26 gates, not all 28. Eight immediate binary-transfer/exec
reuse rounds pass in `build/ssh-session-reuse-20260927-r1/`, including 64 KiB
rekeys; packet tracing is available with `OSITO_SSH_TEST_TRACE=1` and the
focused reuse mode with `OSITO_SSH_TEST_SESSION_REUSE=1`.
Two subsequent traced full runs, `build/ssh-passive-open-full-20260927-r2/`
and `-r3/`, pass all 28 gates without changing the kernel or relaxing the
admission timeout. Both execute kexec and automatically reconnect with the
same pinned identity and persisted files. The isolated first-run timeout
remains unexplained, rather than being labelled fixed by successful retries.

### Runtime UART Backpressure (2026-09-27)

The font-complete Steam workload exposed another source of admission delay.
With the same 45fa5bcc kernel, `build/steam-ssh-admission-qemu-20260927-r1/`
records fourteen failed SSH probes out of twenty at 30/60/120/180 seconds.
The first failed probe exchanged banners but stalled during KEX; later ones
timed out before a banner. TCP tracing shows delayed acknowledgement of the
client's KEX data and delayed SYN processing. GDB repeatedly finds `net-rx`
inside `uart_write` through `serial_tx_flush`, including two samples on the
running CPU. SSH is either waiting for protocol input or blocked in accept,
not waiting for the local shell's command lock. The kernel and application
continue running, and later probes, capture and console stop succeed.

Each ordinary serial record previously called a synchronous flush with a
20 ms deadline. During normal scheduled execution with interrupts enabled,
records now enqueue and perform only the existing bounded non-waiting poll.
The timer continues draining the queue. Early boot and interrupt-disabled
paths retain the synchronous bounded flush, and explicit `serial_flush`
retains its existing contract. The memory log and remote task's captured
output are unchanged. UART overflow is still counted; this is not lossless
output under unlimited producer load or a serial throughput guarantee.

The intermediate runtime-only change built as 7,203,032 bytes, SHA-256
`e0de98d04ed22dc482072daa2e83ca9eeb4dfbc71ffdb2899302dd6f56094481`.
In `build/steam-ssh-serial-qemu-20260927-r1/`, the first fifteen probes pass,
but all five at 180 seconds fail, as do capture and the stop request. GDB
repeatedly samples the timer interrupt draining the UART. This result is
not an end-to-end fix: even a non-waiting 64-byte poll performs many PIO VM
exits with interrupts disabled.

`serial_poll_irq` now limits the timer's drain to one byte, using the same
consumer lock and FIFO checks. Ordinary producers retain the larger bounded
poll. The core budget API clamps to the normal limit and treats zero as no
work. Manual impact review covered `serial_record`, both poll paths, timer
dispatch and their fixtures; GitNexus was unavailable. The queue suite
passes 118 checks and the UART fixture passes 27 with GCC/Clang, normal and
ASan/UBSan, using separate output directories. Tests cover a slow/stuck runtime
UART, limited IRQ drain, deferred byte order, explicit flush timeout/recovery,
early boot, absent hardware and unchanged formatting. Before the IRQ extension,
the synchronous-record control failed five of the 26 UART fixture checks.

The resulting kernel is 7,203,112 bytes, SHA-256
`212e581798e7b096d50bac285bb8dd7372e40c73673eb362efe593be7623c487`.
`build/ssh-serial-irq-full-20260927-r1/` passes all 29 gates, including the
optional `OSITO_SSH_TEST_SERIAL=1` gate. That gate checks the raw UART core
matrix, formatting and 256 ordered/checksummed records from multiple CPUs,
then checks another authenticated SSH command. The full run also verifies
busy-shell I/O and capture, file transfer, cold boot and kexec reconnection.
`build/ssh-serial-irq-maintenance-20260927-r1/` passes all 14 focused gates
on the same ELF: PE32 and PE64 foreground fixtures with four runnable workers,
remote files and diagnostics, snapshots, rejected exclusive/stale requests,
cooperative stop, owned-thread retirement and continued authenticated SSH.
This image has not been deployed to the P1; the latest connection attempt at
192.168.1.20 still timed out before establishing TCP.

### Remaining Load-Dependent Stalls

The same final ELF in `build/steam-ssh-serial-irq-qemu-20260927-r1/` passes
17 of 20 probes. Three initial probes time out; all fifteen at 60/120/180
seconds pass. The later screenshot connection closes after keepalive timeouts,
and the exclusive-operation and stop probes also time out. A subsequent echo
succeeds, but the application was not stopped. These are failures, not successful
exclusive rejection or cancellation, and are separate from shell admission.

The packet trace records a SYN at 1790518255.460298 and its SYN-ACK only at
1790518268.247240. The failed initial probes find SSH blocked in TCP accept
and `net-rx` runnable while the CPU polls xHCI from filesystem operations.
At the 180-second checkpoint the BSP is in `bulk_xfer` under `NtReadFile`,
with RFLAGS `0x200002` (IF clear), while the APs are idle workers. Later traces
also find active TCP processing and a runnable SSH worker. Thus the workload
does not establish a single remaining cause, but it does expose system-wide
dispatch starvation beyond the prompt gate.

`usb_io_lock_irqsave` currently masks interrupts over whole BOT transactions,
and `osfs3_spin_lock` does so over filesystem mutation operations, including
disk I/O. Win32's compatibility stack also defers asynchronous preemption.
Removing these guards or inserting an unconditional yield is unsafe: another
task can spin on a lock held by the suspended owner, or interleave CBW/data/CSW.
The next storage change needs scheduler-aware ownership/waiting, bounded
completion waits and concurrency tests preserving BOT and metadata integrity.
Neither the storage locks nor scheduler policy were changed by this UART fix.
Reliable maintenance under sustained Steam disk/network load remains open.

### Cooperative Storage Boundaries (2026-09-27)

`sched_io_checkpoint` now offers a scheduling opportunity after a completed
USB operation or filesystem operation has released its lock and restored IF.
This permits a long sequence of synchronous Win32 reads to yield through the
existing private-stack scheduler path, without depending on timer preemption
inside an INT 2E shim. It runs only on the BSP, with a runnable peer, outside
the scheduler/transition critical regions, and at most once per 10 ms. Boot,
interrupt-disabled and AP contexts do not yield. It changes neither priority
policy nor the existing compatibility-stack preemption guard.

The USB CBW/data/CSW/sense sequence and filesystem metadata operations retain
their original locks. A nested USB operation entered under the filesystem's
IRQ-off lock cannot yield; the outer unlock supplies the checkpoint. This is
not a conversion to sleeping locks, bounded hardware waits or preemption of a
single long metadata transaction. Those remaining stalls are still possible.
Manual impact review covered both unlock helpers, their read/write/flush and
mutation callers, and scheduler context restrictions; GitNexus was unavailable.

The kernel is 7,203,216 bytes, SHA-256
`4ebe64f66c9ea6d14acf1f903e950af46aae09af834e21e5cee2e26475196e63`.
The USB host suite passes 31,491 checks in each GCC/Clang and ASan/UBSan
variant, including concurrent transaction integrity and a checkpoint assertion
after lock release only for callers that entered with interrupts enabled.
Evidence is in `build/usb-checkpoint-host-20260927-r1/`.

`OSITO_SSH_TEST_REMOTE_IO=1` adds PE32 and PE64 fixtures issuing four 64 MiB
reads each without sleeps between reads. Every byte is checked. SSH status,
uptime, log and file operations must complete while the workload is unfinished,
then both fixtures must stop cooperatively and return the local prompt. The
production USB run in `build/ssh-io-checkpoint-positive-20260927-r1/` passes
all 12 gates, verifying 512 MiB across the two architectures. Its individual
diagnostic/file queries complete in 0.299-3.619 seconds.

The frozen 212e5817 UART-only kernel is the negative control in
`build/ssh-io-checkpoint-negative-20260927-r1/`. Its six base gates pass, but
the PE32 overlap assertion fails: the first status query takes 7.150 seconds,
and authentication follows `REMOTE-IO PASS` in the serial log. The requests
succeed only after the read workload has finished; this is not a TCP timeout.
The test stops there, so this control does not establish a PE64 failure.

The same ELF passes all 29 general gates in
`build/ssh-io-checkpoint-full-20260927-r1/`, including the UART fixture,
concurrent local/remote I/O, binary transfer, cold boot and automatic reconnect
after kexec with pinned identity and persisted files. The focused PE32/PE64
run in `build/ssh-io-checkpoint-maintenance-20260927-r1/` passes all 14 gates:
four runnable workers per foreground owner, remote file operations, snapshot,
exclusive/stale-request rejection, cooperative stop and SSH afterwards.

The font-complete Steam run in `build/steam-io-checkpoint-qemu-20260927-r1/`
passes 19 of 20 SSH probes at 30/60/120/180 seconds while the foreground job
remains running. The first status request closes after keepalive timeouts;
the following nineteen complete in 0.120-3.655 seconds. The remote snapshot
and digest check pass, the exclusive reboot request is rejected with 126,
and `console stop 1` returns idle/returned. The 1280x800 snapshot takes 1.851
seconds total, including 0.810 seconds for 61,119 compressed wire bytes.
Subsequent echo and dmesg requests also succeed. This improves maintenance
availability but is not an all-pass
Steam stress test or proof that the startup timeout has been fixed.
The captured desktop has no usable Steam library/profile window. Physical P1
deployment is pending: the latest SSH attempt at 192.168.1.20 timed out and
the development host's neighbor entry was unreachable.

The subsequent font-complete run in
`build/steam-layered-qemu-20260927-r1/` uses the layered-window extension
described in section 4.157 of `win32-layer-correctness.md`, kernel SHA-256
`0dea08ae69cae94f52e1a19ae0768365e3ab01509061f32e0bf43ee61a352e14`.
All fifteen SSH status/uptime/dmesg/write/read probes at 60/180/360 seconds
pass while the local foreground job remains running (0.319-3.857 seconds).
The authenticated 1280x800 GOP capture passes its digest check but takes
24.768 seconds, including 23.309 seconds for 364,329 compressed wire bytes.
Exclusive reboot is rejected with 126, cooperative stop returns the prompt,
and echo/dmesg work afterwards. The image shows Steam navigation/profile and
an empty store view. This is an all-pass maintenance run, not a transfer-speed
fix, proof against intermittent stalls or functional Steam validation. No
post-restart P1 connection has yet been recovered at 192.168.1.20.

The final layered input-bounds follow-up, SHA-256
`9bae99f703f8008ab82fcacb7fbebbf7e3740aa98095bc1983ebec94b185a47d`
(7,215,944 bytes), passes all 29 general SSH gates in
`build/ssh-layered-full-20260927-r1/`. These include independent commands,
redirects and output isolation while the local shell is busy, UART recovery,
binary transfer/deltas/rekey, cold-boot persistence and automatic pinned-key
reconnection after remote kexec. The shell host policy suite was rerun with
155 checks passing under GCC/Clang, normal and ASan/UBSan. The 13-gate layered
ABI/pixel/input suite also passes on this final image. The six-minute Steam
stress above predates only that input-bounds/test follow-up; it is not a
separate full Steam stress run on this final hash.

The follow-up `build/steam-library-ui-qemu-20260927-r2/` exercises that final
hash for ten minutes with USB keyboard/tablet input and real Steam. Fourteen
of fifteen maintenance probes pass. The first 120-second status probe times
out during SSH banner exchange (16.826 seconds); subsequent probes at that
checkpoint and all probes at 360/600 seconds succeed. The final five complete
in 0.275-0.332 seconds while `console status` still reports busy/running.
The authenticated GOP capture shows Steam's spinner and passes its digest
check: 1280x800, 46,377 compressed wire bytes, 14.906 seconds total. Exclusive
reboot is rejected, `console stop 1` is accepted, the second status poll reports
idle/returned, and echo/dmesg still succeed. No reboot is used for this recovery.
This supports shell independence, not a bounded SSH admission/transfer latency
guarantee or successful Steam initialization. The P1 still times out at its
last known address, so physical deployment of this final hash remains pending.

The repeat in `build/steam-task-trace-qemu-20260927-r1/` uses the same final
9bae99f7 kernel for ten minutes. All thirty status/uptime/dmesg/write/read
probes at 90/180/300/420/510/600 seconds pass while the foreground job remains
busy/running. The authenticated 1280x800 GOP capture shows the populated
Pixel Dungeon library page, verifies its digest and transfers 812,183 compressed
wire bytes in 0.323 seconds (0.634 seconds total). Exclusive reboot returns 126;
`console stop 1` is accepted, the second status poll reports idle/returned,
and subsequent echo/dmesg requests succeed. The runner exits successfully.
This does not erase the admission/transfer stalls in the preceding runs or
prove bounded latency. No production code changed between these two runs.

Read-only GDB task samples accompany this repeat, with ABI offsets derived
from the exact source/compiler and checked against the frozen ELF hash. The
90/180/300-second samples used an unvalidated scheduler-slot thread cache:
their Win32 thread names/identities are not reliable and must not be used to
diagnose ownership. Samples at 420/510/600 seconds use active contexts indexed
by their actual kernel PID (`identity_version: 2`). The final sample includes
bounded frame-pointer chains; raw stack candidates are not backtraces.
Each corrected sample pauses the VM for approximately 0.4 seconds. The final
browser main thread is in MsgWaitForMultipleObjectsEx and CEF I/O threads are
in GetQueuedCompletionStatus; these observations alone are not deadlock
evidence. The physical P1 still times out before SSH negotiation at its last
known address; deployment of this final kernel there remains unverified.

The twenty-minute follow-up in
`build/steam-pixel-launch-qemu-20260927-r1/` retains the same `9bae99f7` ELF.
Forty-eight of fifty status/uptime/dmesg/write/read probes succeed; the first
checkpoint has two SSH banner timeouts and a 10.773-second status request.
A manual binary-transfer attempt overlapped that checkpoint, so it is not
an isolated admission-latency experiment. The single service worker does not
promise concurrent client sessions. All forty-five probes at 180 seconds and
later pass; the last five complete in 0.144-0.198 seconds. The authenticated
1280x800 snapshot verifies its digest in 0.569 seconds, including 0.276 seconds
for 185,499 compressed wire bytes. Exclusive reboot is rejected; cooperative
stop is accepted and the third status poll reports idle/returned. Echo and
dmesg then succeed. No reboot is needed. The final image has navigation and
an empty marketing popup, not a functional game. The CEF fault and browser
restarts are retained separately in `win32-layer-correctness.md`.

The finite IOCP deadline fix in section 4.158 of that document builds as
7,220,032 bytes, SHA-256
`1ccbee9a6c353180281901148c44a3427385bb2561d0441eea403aa664cbc894`.
All 28 general SSH gates pass on this ELF in
`build/ssh-iocp-deadline-full-20260927-r1/`, including busy-shell diagnostics,
redirects, output isolation, screen/file subsystems and automatic reconnection
after cold boot and remote kexec. The 155-check shell policy host suite passes
under GCC/Clang, normal and ASan/UBSan. The twenty-minute Steam result above
uses the pre-IOCP-fix ELF; it is not a real-Steam test of this newer hash.
The P1 remains unreachable at 192.168.1.20, so this image is not deployed there.

The fifteen-minute real-Steam follow-up on that same `1ccbee9a` ELF is preserved
in `build/steam-iocp-fault-qemu-20260927-r1/`. All forty maintenance probes at
90/180/300/420/540/660/780/900 seconds pass. The final five take 0.123-0.152
seconds. The authenticated GOP snapshot verifies its digest in 0.517 seconds,
including 0.258 seconds for 61,168 compressed wire bytes. Exclusive reboot
is rejected, cooperative stop is accepted, the first subsequent status poll
reports idle/returned, and echo/dmesg work afterwards. This confirms maintenance
availability in this run, not functional Steam initialization: the final image
shows only the desktop and terminal. The browser remains alive after GPU
fallback without producing its main window. The fatal-fault tracer recorded no
fatal fault before it was manually detached at approximately six minutes;
that limited interval is not a no-fault claim for the whole run. Later read-only
task samples and the final six extracted Steam logs are retained.

The append-only file fix in section 4.159 of `win32-layer-correctness.md`
builds as 7,224,376 bytes, SHA-256
`9f6e1ef736dd268b86de629324ff2a76aabc5483530e443455569b5759b5da97`.
`build/ssh-append-full-20260927-r2/` passes all 28 general gates and exits
successfully, including busy-shell diagnostics/files/capture, isolated output,
binary transfers, cold boot and automatic pinned-key reconnection after kexec.
The preceding `-r1` run on intermediate `4268dbfd` passed its 28 guest gates
but its shell runner exited nonzero after a live edit changed the script's
read position. It is retained as evidence, not counted as a clean runner pass.
The final repeat used an unchanged runner. Physical P1 installation of the
new image remains pending: SSH at 192.168.1.20 is still unreachable.

The same final ELF completes a six-minute real-Steam test in
`build/steam-append-qemu-20260927-r1/`: all twenty status/uptime/dmesg/write/read
requests at 90/180/300/360 seconds succeed while the foreground job stays
busy/running. The final five take 0.126-0.149 seconds. The authenticated GOP
snapshot verifies its digest in 0.540 seconds (61,233 compressed wire bytes,
0.270 seconds transfer). Exclusive reboot returns 126, `console stop 1` is
accepted, the first subsequent status poll reports idle/returned, and another
echo and dmesg succeed. The runner exits successfully and its scratch disks
are pruned. Read-only task samples and six Steam logs are preserved. This is
a maintenance/log-integrity result, not successful Steam initialization:
its main window is absent in the final image. The complete prior CEF log is
now preserved as a byte-identical prefix, as detailed in section 4.159.

The fresh-cache follow-up in `build/steam-freshcache-qemu-20260927-r1/`
passes 25 maintenance probes and its authenticated snapshot, but an accepted
stop never completes. Read-only GDB identifies a zombie child retaining the
global Win32 heap lock, blocking both browser shutdown and console cleanup.
Section 4.160 of `win32-layer-correctness.md` records the evidence and fix:
child exit is delivered at safe checkpoints instead of replacing an arbitrary
interrupted instruction. The final `594ede27` image passes both ABI regressions
and all 14 focused maintenance gates in
`build/ssh-child-exit-maintenance-20260927-r1/`. Cancellation is cooperative;
SSH availability does not promise recovery from every host lockup. P1 deployment
is still pending because 192.168.1.20 does not accept a connection.
The same final ELF passes all 28 general gates in
`build/ssh-child-exit-full-20260927-r1/`, including busy-shell isolation,
binary/delta transfers, cold boot and automatic reconnection after kexec.

The real-Steam repeat `build/steam-child-exit-qemu-20260927-r1/` keeps SSH
responsive for all 25 commands over ten minutes and verifies an authenticated
snapshot in 0.544 seconds. Only the first 15 commands observe a busy foreground
job: the owner returns before the 420-second checkpoint with children still
alive. The runner therefore fails two busy-state predicates, accepts its
exclusive diagnostic while idle and rejects the already-finished stop request.
Do not count those results as successful cancellation. The UI reaches the
store/profile before its content turns black after a Library click; that
remaining application-lifetime/rendering problem is separate from SSH admission.

The focused real-Steam repeat in `build/steam-child-exit-stop-qemu-20260927-r1/`
does retain a busy owner until cancellation. It passes nine of ten probes at
90/180 seconds; the first connection times out before the SSH banner after
22.766 seconds. The authenticated capture succeeds in 36.525 seconds, not a
latency improvement. Exclusive reboot returns 126, stop is accepted, the second
status poll reports idle/returned, and subsequent echo/dmesg work. This confirms
the foreground recovery path in this workload while preserving evidence of
intermittent admission/transfer stalls. No P1 deployment is claimed.

### Worker Retirement And P1 Deployment (2026-09-27)

The next regression isolates a worker killed while it owns the global Win32
heap, beyond the earlier primary-task fix. Process-wide worker cancellation
now uses the guarded checkpoints and waits for ordinary reaper cleanup before
freeing the image. Cleanup starts the reaper even when desktop has never run.
Section 4.161 of `win32-layer-correctness.md` preserves the native Windows,
old-kernel negative control, failed intermediate and final positive evidence.
This does not make explicit TerminateThread or every host-lock teardown safe.

The final kernel is 7,212,752 bytes, SHA-256
`5d8f2ac2e66512ee2f87f73f8bc1427fb5006c89e3a96013ae0c6c6ed3210f7d`.
It passes eight child/worker-exit gates, fourteen foreground-maintenance gates
and all 28 general SSH gates, including automatic reconnection after kexec.
Evidence directories are `build/worker-exit-positive-20260927-r2/`,
`build/ssh-worker-exit-maintenance-20260927-r1/` and
`build/ssh-worker-exit-full-20260927-r1/`.

The final real-Steam run `build/steam-worker-exit-qemu-20260927-r1/` passes
fourteen of fifteen probes; its first status request times out. Authenticated
capture succeeds but takes 57.010 seconds using 4 KiB requests. Exclusive
reboot is rejected, cooperative stop reaches idle/returned on its ninth status
poll and subsequent echo/dmesg work. Earlier 594ede27 controls include both
a failed long-run stop and a successful shorter stop. These results do not
establish a fix for every Steam lockup or a latency bound; the captured Steam
workload still has no usable library.

The final image is now installed on the physical P1, superseding the pending
deployment notes above. File-sync upload used 4 KiB writes and a complete remote
hash comparison. `kupdate --file /kernel-ssh-workers-20260927.elf --no-kexec`
verified the persistent OsitoFS and USB EFI kernel copies while retaining the
previous EFI image. After remote kexec, saved WLAN and SSH settings restored
192.168.1.20 automatically with the same pinned key. A physical cold boot of
this final hash has not been tested.

Both PE32 and PE64 foreground workloads pass diagnostics, independent text
files, exclusive/stale-request rejection, authenticated full-HD capture,
cooperative stop, prompt recovery and post-stop SSH. Repeating the two captures
with the normal 32 KiB client setting takes 5.593 and 6.031 seconds including
client startup, versus 48.375 and 32.125 seconds with 4 KiB requests. Small
logical raw chunks require many round trips even when the image compresses
well; the upload workaround need not be applied to every screenshot. This is
not a general TCP/WLAN throughput fix or a controlled identical-image benchmark.
Evidence is in `build/p1-ssh-maintenance-20260927-r1/tests/` and `tests-32k/`.
The P1 was left at its idle local prompt, request 4 returned; Steam was not
launched during these physical fixture tests.

### Real Steam Follow-Up (2026-09-27)

A subsequent P1 launch queues desktop as request 5 and Steam as request 6.
`build/p1-steam-workers-20260927-r1/` records successful status/uptime/dmesg
at 30 and 120 seconds while the foreground owner is running. At 240 seconds
SSH stops responding, and the host later cannot resolve the peer through ARP.
There is no authoritative post-failure stack or screen yet. The decoupled
maintenance fixtures pass, but they do not establish sustained availability
under this real workload; do not treat this as a demonstrated shell-lock cause.

The longer QEMU repeat `build/steam-p1-repeat-20260927-r1/` reaches the store
and profile, passes eighteen of twenty maintenance probes and its authenticated
capture, rejects exclusive reboot, and successfully stops the foreground job
with post-stop SSH. The first two connections time out before later probes
recover. Section 4.162 of `win32-layer-correctness.md` records a separately
reproduced asynchronous-file cursor defect and its fix. That new full kernel
passes all 28 general SSH gates, but has not yet been deployed to the P1.

On that new kernel, `build/steam-file-position-20260927-r1/` passes the fifteen
probes at 180/360/600 seconds, but all five first-minute probes fail. Capture
and cooperative cancellation succeed, with idle/returned on the seventh
status poll and working SSH afterwards. Steam's content region is black after
an attempted Library click. Shell admission is independent, but real-workload
latency and application rendering are still unresolved.

Diagnostics/files/captures no longer wait for the foreground prompt. kexec,
kupdate and other exclusive commands still require that owner to finish or
accept cooperative cancellation. A noncooperative guest can defer cancellation,
and the single SSH service worker still serializes connections.

The manual diagnostic commands passed through PowerShell/WSL are not valid
filesystem evidence: a local argv probe reproduces lost embedded quotes and
argument splitting, and the default WSL shell can interpret pipes locally.
The automated probe runner invokes OpenSSH directly with a Python argv list
and is not affected. Ad-hoc queries should use the same approach, with their
exact command recorded before execution. A log changing during authenticated
file pull is correctly rejected as an inconsistent snapshot; it does not by
itself establish filesystem corruption.

```sh
OSITO_SSH_TEST_REMOTE_IO=1 OSITO_SSH_TEST_STORAGE=usb \
  bash arch/x86/scripts/test-ssh-qemu.sh <new-output-directory>
```

```sh
OSITO_SSH_TEST_REMOTE_MAINTENANCE=1 OSITO_SSH_TEST_STORAGE=usb \
  bash arch/x86/scripts/test-ssh-qemu.sh <new-output-directory>
```

Validation on 2026-09-26: the host shell policy/mailbox suite passes 100 checks;
the keyboard and terminal suites pass 4073 and 4049 checks respectively, each
with GCC, Clang and ASan/UBSan. The production QEMU SSH suite passes all 27
gates, including partial local input, rejected redirects without file creation,
remote desktop handoff, busy admission, diagnostic queries, cold boot and kexec.
The PE32/PE64 controls suites still pass 70 checks each. A separate UT99 run
launches both desktop and the recovery dialog via SSH, then reads `console
status`, `ps` and `dmesg` while the dialog remains open; its four recovery
buttons are visible and the safe-mode button opens the options page.

Evidence: `arch/x86/build/console-launch-qemu-20260926-r2/` and
`arch/x86/build/ut-remote-launch-20260926-r1/`. Frozen kernel: 7,153,160 bytes,
SHA-256 `5614fa4178b36655e197e51b687be293ae24421f20913768673a5e18f0056692`.

The same image was uploaded with the SHA-256-checked file subsystem as
`/kernel-console-launch-20260926.elf` and hot-loaded on the physical P1. After
reconnect at `192.168.1.20`, SSH launched `desktop` (request 1, returned), then
`winexec Games/UT99/System/UnrealTournament.exe -safe` (request 2, running).
The remote 1920x1080 snapshot shows all four recovery buttons; `ps` was retrieved
with the dialog still open. A full `dmesg` request timed out; a later bounded
`dmesg | tail -n 100` succeeded while the foreground application remained active.
Evidence is in
`arch/x86/build/p1-unreal-20260925-223601/ut-remote-launch-physical-1.png` and
the accompanying `console-launch-physical-*.log` files. This verifies remote
launch and diagnostics, not gameplay, window decorations or physical UI input.
The USB's EFI cold-boot kernel was not replaced by this hot load.

After local interaction, `ut-remote-launch-physical-3.png` shows the UT title
screen and "Press ESC to begin", with incorrect logo colors and an unframed
640x480 view at the screen origin. The safe-mode options explicitly request
640x480 windowed software rendering; the size alone is not a failed mode change.
The loader log confirms `SoftDrv.dll`, and the INI selects
`WindowedRenderDevice=SoftDrv.SoftwareRenderDevice`. Physical input and gameplay
are not established by this still image. Transfer reliability is still open:
the first 32 KiB-chunk snapshot took 5.234 s, the second stopped at offset 32768,
and a 4 KiB-chunk retry completed in 213.985 s (140,344 compressed wire bytes).
Small chunks are a diagnostic workaround, not a throughput fix.

### Wakeup And Run-Queue Promotion Race (2026-09-27)

The real-Steam QEMU run `build/steam-tcp-tx-font-qemu-20260927-r1/` uses the
TX ownership kernel from network-stack section 12 and the same local system
font installed on the P1. It passes the five maintenance queries at 180
seconds, but none at 60 or 360 seconds. The early admission snapshot is in
USB I/O and does not establish a permanent stall. The late snapshots are
different: ticks advance from 39398 to 41713 while the idle task remains
current, `ready_count` is 27 and all five `runq_head` entries are -1. The
process table still contains READY tasks. The local shell and net-rx are
among them, while SSH has no active connection and is waiting in accept.
This is an inconsistent scheduler queue, not a remote command blocked on
foreground shell ownership. The retained 180-second image shows the Steam
spinner, not a completed Library load. Capture and cooperative stop fail.

`sched_unblock` called the IRQ-protected `proc_transition(..., PROC_READY)`,
then manipulated the same queue without protection in `runq_promote_front`.
A timer could run the awakened task, which could block or exit, before the
original waker resumed. Promotion would then remove a non-member and insert
a non-READY task. Preemption during the linked-list writes was also allowed.
This is a demonstrated defect consistent with the observed corruption, not
proof that every lost queue entry or earlier physical failure has this cause.

Eligibility, state transition, queue promotion and slice expiry now share
one outer IRQ guard. The existing transition guard nests and preserves an
already-masked caller; no scheduler policy, priority, queue capacity or
recovery-by-rebuilding was added. `runq_irq_save/restore` centralize the same
existing instructions for these two paths and provide host-only test hooks.
No hook is present in the production image. The scheduler remains BSP-owned;
this does not introduce cross-CPU task scheduling or solve arbitrary table
corruption elsewhere.

`test-runq.sh` includes production `process.c`. Its controlled timer model
runs and blocks the newly awakened task at the first enabled-IRQ boundary,
and validates membership/state, uniqueness, links, tails and ready counts.
The instrumented but behaviorally unchanged pre-fix source fails both the
interleaving and repeated-churn groups in all four compiler configurations
in `build/runq-negative-20260927-r5/`. The fixed source passes normal wakeups
across all five QoS classes, that interleaving, ineligible/duplicate/invalid
wakes, preserved IRQ state and 10,000 blocking/wakeup cycles in
`build/runq-positive-20260927-r1/`, with GCC/Clang and ASan/UBSan. The host
fixture suppresses only the existing unused boot-code/attribute warnings
inside its included kernel source, not warnings in the test itself.

Manual impact review, with GitNexus unavailable: the behavior change affects
`sched_unblock`, used by network waiters/timeouts and Win32 synchronization;
the transition helper retains its previous IRQ semantics for other callers.
The combined TX/RX/wakeup image is 7,262,664 bytes, SHA-256
`cfb16973b80ff051de1eb66791b3fea17eed853a059c2a5a2bfd9ff5352da513`.
Build succeeds with the three pre-existing process.c warnings (unused vec,
unused fpu_state_init and Clang ignoring GCC's optimize attribute). All 28
full-kernel SSH gates pass in `build/runq-ssh-qemu-20260927-r1/`, including
diagnostics, files and capture while the local console is busy, cold boot
and remote kexec. The eight private-address-space gates also pass in
`build/runq-private-qemu-20260927-r1/`, including exact external TCP echoes
from both PE32 and PE64. Sustained real-Steam and physical qualification
remain pending at this point.

The ten-minute combined-kernel run in
`build/steam-runq-qemu-20260927-r1/` uses the same local system font, an
isolated USB disk, and frozen kernel/layout hashes. All ten maintenance
queries at 60/180 seconds time out; all ten at 360/600 seconds pass while
the foreground remains running (0.301-1.004 seconds). It is therefore not
an all-pass maintenance test. Early admission samples alternate between USB
polling and serial/network work; the packet capture shows multi-second
delays before SYN-ACK/banner/data ACKs, before remote command dispatch.
This retains the previously documented startup starvation/latency issue,
without establishing one unique cause or changing timeout thresholds.

The four stopped-VM queue audits at ticks 10986, 23402, 38314 and 62868 show
14, 7, 4 and 24 READY members respectively, matching `ready_count` and the
process states. Each has valid links/tails, no duplicates, missing READY
tasks or non-READY queue members. These are point-in-time invariants, not
proof that no other interleaving can corrupt a queue. The final QMP and
authenticated GOP images show a populated Steam Store and profile. The
current-run connection log records a successful logon response. Library,
game startup and physical P1 transport are not established by these images.

The authenticated 1280x800 capture verifies its digest, but takes 63.061
seconds total (61.152 transfer, 1,639,173 compressed wire bytes with the
existing 4 KiB diagnostic chunks). This is not a transfer-speed fix.
Exclusive reboot is rejected with 126; cooperative stop is accepted and
the fourth status poll reports idle/returned. Echo and dmesg then succeed
without reboot. The runner exits successfully and removes its scratch disk.
One fatal SteamService entry-point fault remains in the serial log; see
Win32 correctness section 4.165. That failure is not hidden by the successful
store image or maintenance recovery. Physical deployment is still pending:
the P1 answers ping at its known MAC/IP but TCP 22 continues to time out.

### Deadline Waiter Reuse And P1 Redeployment (2026-09-27)

The next audit finds a separate lost-wakeup race in the unguarded deadline
sweep, after the guarded `sched_unblock` returns. A woken task can install
a new wait in the same slot before the sweep clears the old entry. The
production-code negative/positive tests and bounded IRQ guard are described
in network-stack section 14. This defect can strand accept/receive waiters,
but no physical trace yet proves it caused a particular earlier SSH outage.

The combined image, SHA-256
`4bb6918040253ee1c915b03315313e30b9754179c47b10313d30d08b2de7923b`,
passes all 28 full SSH gates in `build/net-waiter-ssh-qemu-20260927-r1/`
and all eight private-address-space gates in
`build/net-waiter-private-qemu-20260927-r1/`. No SSH timeout or retry limit
is increased for these tests.

After the user's reboot, SSH to the P1 responds and `console status` is idle.
Authenticated sync installs the frozen image, and `kupdate --file ...
--no-kexec` verifies both the OsitoFS and EFI copies, preserving the previous
kernel. The kexec session disconnects without delivering its buffered output;
a new authenticated connection reports uptime 53 seconds. The new boot log
records automatic channel-6 association to the saved 2.4 GHz profile, DHCP
OFFER/ACK for 192.168.1.20 and successful SSH public-key authentication. The
console is idle without local intervention. Evidence is in
`build/p1-net-waiter-20260927-r1/`; this establishes persistence and hotreload
reconnection, not yet sustained Steam qualification.

The subsequent physical run launches desktop request 1 and real Steam request
2. Eleven of twelve uptime/console/dmesg queries at 30, 90, 180 and 300 seconds
complete. The 180-second console query times out in SSH negotiation; the next
dmesg query succeeds without stopping Steam or restarting SSH. The retained
log records `session failed phase=key-exchange reason=read-closed`. This is
not an all-pass latency test and does not locate the delay within WLAN/TCP
or scheduling.

The 4 KiB-block screen request exceeds the runner's 180-second total bound.
No orphan host SSH process remains afterward, and a fresh `wlan status`
request succeeds. A later 32 KiB-block request verifies the full 1920x1080
GOP snapshot: 6,220,800 raw bytes, 407,320 compressed wire bytes, 42.625 seconds
total and 35.391 seconds transferring. `steam-screen-32k.png` still shows
"Loading user data...", with an intact terminal and no repeated screen copies.
These sequential captures occur at different times/load and are not a
controlled throughput comparison or a claim of a transfer-speed fix.

With the foreground still busy, remote file write, readback and removal all
succeed. The Wi-Fi snapshot stays associated on channel 6 with its data port
open. Ethernet TX queue rejections grow from 0 to 31 and RX queue drops from
0 to 565. The latter counter is specifically the full 16-frame software
queue in `iwx.c:link_notification`, not a firmware TX failure or proof of
TCP byte corruption. The verified 16 KiB connection-log tail retains logon
successes followed by "Encryption Failure" disconnects and reconnects; this
transport lead and the spinner remain unresolved.

`console stop 2` is accepted; the second status poll reports idle/returned.
Full dmesg, ps and echo then succeed over SSH. No fatal CPU exception marker
appears in the retained full run/after-stop dmesg. Foreground cleanup reports
27 owned threads and 218 handles stopped/closed, but ps still contains many
Win32 child tasks: this is console recovery, not verified whole-process-tree
cleanup. No reboot or local shell input is needed for this recovery.

## Validation

```sh
make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf
bash arch/x86/scripts/build-ssh-host.sh build/ssh-host
python3 arch/x86/scripts/test-ssh-host.py arch/x86/build/ssh-host
CC=clang SANITIZE=1 bash arch/x86/scripts/build-ssh-host.sh build/ssh-host-sanitize
python3 arch/x86/scripts/test-ssh-host.py arch/x86/build/ssh-host-sanitize
bash arch/x86/scripts/test-ssh-qemu.sh arch/x86/build/ssh-qemu-new
bash arch/x86/scripts/test-shell-remote.sh arch/x86/build/shell-remote-new
bash arch/x86/scripts/test-klog.sh arch/x86/build/klog-new
python3 tools/test-oftp-server.py
```

Host gates use real OpenSSH: fragmented I/O, simulated TX backpressure, entropy
failure including failure after keypair generation, wrong key/user/none auth,
malformed peer recovery, rekey, Curve25519, interactive console, exit status,
output bounds and stable identity after restart. The same core also runs with
ASan/UBSan. QEMU uses isolated scratch ESP/OsitoFS3 disks, virtio-net and the
production kernel; it tests real TCP, file redirection, console isolation,
rejection of local-only commands, stop/start and identity after a cold boot of
the same disks. The warm QMP reset gate did not restart this guest setup;
the persistence gate uses a cold boot, not a claim that warm reset is fixed.
Passing QEMU is not physical AX200 SSH validation.

### Busy Local Shell Regression (2026-09-25)

A local Win32 launch retains the command lock for the lifetime of the program.
Previously an authenticated remote command could wait indefinitely for that
lock, even after its client disconnected. Since the maintenance service has
one worker, subsequent connections could complete TCP setup but time out before
the SSH banner. The first correction tried to acquire ownership before setting
up I/O and rejected contention without changing the current owner. The
2026-09-26 extension below permits independent diagnostic queries as well.

The QEMU regression keeps a local pipeline active while checking remote
rejection and then taking a screen snapshot. It verifies that a rejected file
redirection did not create its destination later. The original single ping was
too short to cover both SSH handshakes and capture; the test uses four pipeline
stages and still asserts that no local prompt returned before either check.
`OSITO_SSH_TEST_KERNEL` can select a frozen kernel for a regression control
without replacing the normal build artifact.

On the P1, the first screen transfer reset before the later shell request
stalled. The command-lock fix addresses the stall, not an established cause
for the initial screen reset. It also does not establish a fix for UT99 color,
geometry or input. Keep these separate until physical logs are recovered.

The production `QUIET=1 SKIP_MODEL=1` image passed all 22 QEMU gates, including
busy-shell rejection, screen transfer while busy, cold-boot persistence and
automatic reconnect after remote kexec. The frozen image is 7,152,920 bytes,
SHA-256 `42c667c016d283f3cd167e1e42ce7ced951b1c9840baf550222c1142ce6e59c3`.
Results are in `arch/x86/build/ssh-busy-qemu-20260925-quiet/results.json`.
The old-kernel negative control failed the new busy-command gate: it returned
success and created the redirected file after the local command finished.

The same tested image was transferred to the P1 as
`/kernel-ssh-busy-20260925.elf` and loaded with kexec. Saved WLAN and SSH
configuration restored connectivity automatically with the pinned host key.
This hot load did not update the USB's EFI kernel. A post-kexec 1920x1080
desktop capture completed in 4.766 seconds, transferring 51,285 compressed
bytes for 6,220,800 RGB bytes with the final digest verified.

Physical transfer reliability remains open: two attempts to upload the image
with normal 32 KiB file-write requests reset the SSH connection; one traced
attempt had acknowledged 2,293,760 bytes. Splitting writes into 4 KiB requests
completed the full upload and its SHA-256-checked commit. This diagnostic
workaround changes request size and timing together, so it does not establish
a maximum supported size or identify the underlying TCP/WLAN failure. The
default client protocol and block size are unchanged. Traces are under
`arch/x86/build/p1-unreal-20260925-223601/`.

### Diagnostics During A Foreground Program (2026-09-26)

Rejecting every remote command avoided starvation but still prevented `dmesg`
and process inspection during a stuck game. The existing task-specific remote
I/O now supports a diagnostic execution mode when the exclusive lock is busy.
The actual shell parser checks each stage before redirect setup or dispatch;
the mode cannot decrement or acquire the local owner's recursive lock. Unknown
commands, mutations and redirects remain rejected, including a mutating later
stage in an otherwise read-only pipeline. The one-remote-session reservation
still applies. `kexec` and reboot are intentionally not concurrent diagnostics.

The host suite exercises the real parser/policy, lock depth and I/O selection:
59 checks pass with GCC and Clang, both normal and ASan/UBSan. The production
QEMU suite additionally runs read-only pipelines while a local pipeline retains
its capture buffers, rejects mutations and redirects, checks that no destination
appears later and verifies console isolation. Its 25 gates passed in
`arch/x86/build/ssh-diagnostics-qemu-20260926-r1/`. An isolated UT99 recovery
dialog also remained open and interactive while `ps` and
`dmesg | tail -n 30` completed over SSH; evidence is in
`arch/x86/build/ut-recovery-qemu-20260926-r5/`.

The same diagnostic work corrects `klog_read` to select the newest requested
suffix before the ring fills, not the oldest prefix. The separate 59-check
host suite covers empty, partial, exactly full and wrapped rings, bounds and
the unchanged incremental cursor, with both compilers and sanitizers. This
does not make concurrent kernel-log capture an atomic snapshot or expand the
shell's 512 KiB `dmesg` limit.

The final image including that log fix passes all 25 production QEMU gates in
`arch/x86/build/ssh-diagnostics-qemu-20260926-r2/results.json`. The frozen kernel
is 7,144,688 bytes, SHA-256
`7ad7da20b7405afbd9aba2ffb3baf6664dc8a60d765bd01830f5f368a0b9fbec`.
The older group-box-only image is a negative control: the same suite fails on
`mem` during local execution with status 126, as expected. Results are under
`arch/x86/build/ssh-diagnostics-negative-20260926/`.

The final image was transferred completely to the P1 as
`/kernel-diagnostics-20260926.elf`, using 4 KiB write requests and the file-sync
SHA-256 commit check. After SSH `kexec`, 192.168.1.20 no longer resolved through
ARP. The P1 had in fact booted and automatically fallen back to 169.254.200.150.
A temporary peer-only Ethernet route and IP Helper alias on the PC restored SSH
with the same pinned identity, without changing the PC's DHCP configuration.
`drivers status` reports WLAN, IPv4 and SSH ready. A 1920x1080 GOP capture also
completed with its digest verified.

The recovered `diagnostics-kernel-apipa-dmesg.log` shows an accepted OFFER for
192.168.1.20, three queued REQUESTs and no received ACK, followed by successful
APIPA. This proves boot and fallback, not DHCP correctness or the source of the
missing ACK. Physical live-game validation of the final image is still pending;
the EFI boot kernel was not updated.

## Temporary Local Kernel Transfer

`tools/serve-kernel-local.ps1` verifies a pinned kernel hash and dedicated public
key. Its default mode only validates. With elevated `-Serve`, it creates a
temporary IP Helper alias and peer-only Ethernet route without disabling DHCP,
a peer-scoped UDP 7781 firewall rule, and a hidden OFTP server in read-only mode.
Only copies of `kernel.elf` and `osito-dev.pub` are served. It expires after two
hours by default; creating `stop` in its output directory ends it early.
Cleanup removes the server, firewall rule, route and alias; `status.json`
records readiness, expiry, hashes and cleanup results. Existing aliases/routes
or a busy port are not overwritten. OFTP itself is not authenticated: use this
bootstrap only on the trusted development LAN, and verify the kernel artifact
before executing it. SSH does not make the OFTP transfer authenticated.

```text
kdownload 169.254.167.94 7781 osito-dev.pub .ssh_authorized_keys
kdownload 169.254.167.94 7781 kernel.elf kernel-ssh.elf
kexec kernel-ssh.elf
```

This hot-loads a test kernel; it does not overwrite the USB's UEFI boot kernel.
The running kernel must already contain the [kexec ownership fix](x86-kexec.md).
The old fixed-address loader can corrupt itself even when loading a corrected
image; install the first corrected build via UEFI, not through that loader.
Before the first migration from a kernel without saved profiles, reconnect
with `wlan connect` once and enable `sshd autostart on`. Subsequent boots of the
new kernel restore these policies. Keep the working USB boot kernel as recovery
until physical validation passes.

The QEMU SSH suite now also issues `kexec next.elf` through OpenSSH, expects
the old connection to end and reconnects to the automatically started listener
with strict host-key checking to verify identity and file persistence. Cold-boot
and kexec gates do not issue a local `sshd start`.
