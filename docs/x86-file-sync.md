# Incremental File Transfer Over SSH

`tools/osito-sync.py` copies regular files and directory trees in either
direction, using OpenSSH and Python's standard library on the host. The kernel
offers the `osito-sync-v1` SSH subsystem. It reuses the existing TinySSH
authentication, encryption, host identity, TCP transport and flow control.
No new port, network service, crypto implementation or GPL dependency is added.
The transport is an SSH subsystem as specified in
[RFC 4254 section 6.5](https://www.rfc-editor.org/rfc/rfc4254.html#section-6.5).

This is rsync-like functionality, **not rsync wire compatibility or SFTP**.
Both the host tool and a kernel containing `file_sync.c`/`file_sync_fs.c` are
needed. An older kernel rejects the subsystem; changing the host tool alone
does not upgrade the guest. Enable the opt-in service with `sshd start` after
booting the new kernel. Keep the known host key pinned as described in
[SSH provisioning](x86-ssh.md).

## Usage

From PowerShell in the repository, with the existing development key/host pin:

```powershell
$peer = @('--host', 'osito@192.168.1.20',
          '-i', "$HOME\.ssh\osito_p1_ed25519",
          '--known-hosts', "$HOME\.ssh\osito_p1_known_hosts",
          '--host-key-alias', 'osito-p1')

python tools/osito-sync.py @peer pull /dmesg-dosrun.log arch/x86/build/dmesg-dosrun.log
python tools/osito-sync.py @peer -r pull /diag arch/x86/build/p1-diag
python tools/osito-sync.py @peer -n push arch/x86/build/kernel.elf /kernel-next.elf
python tools/osito-sync.py @peer push arch/x86/build/kernel.elf /kernel-next.elf
```

These addresses and key paths are examples, not kernel constants. `--host`
also accepts a configured OpenSSH host alias. `--port`, `--ssh`, `--timeout`,
`--identity`, `--known-hosts` and `--host-key-alias` are configurable. Host-key
checking is strict; passwords and interactive prompts are not used.

The destination is the **exact** file or directory path. A directory transfer
copies its contents into that destination and requires `--recursive`/`-r`.
`--dry-run`/`-n` compares content but performs no destination writes or mkdirs.
Nothing deletes extra destination files. `--json` suppresses per-path output
and reports files copied/skipped, file-data bytes sent/received, bytes reused,
directories created and dry-run status. These byte counters exclude hashes,
SSH headers and other protocol traffic; dry-run counters are estimates.

The kernel supports only OsitoFS3 through this adapter. It does not expose the
ESP, raw disks, devices, symlinks or host paths. The authenticated maintenance
user remains privileged over OsitoFS3, including dotfiles; this is not an
unprivileged account or a restricted export directory.

## Correctness And Cost

- Comparisons use SHA-256, never size/mtime alone. Entire unchanged files are
  skipped. Changed files use fixed aligned 32 KiB block signatures; identical
  blocks are read from the old destination instead of sent across the network.
- Pushes assemble a same-directory, exclusively created random `.osync-*.part`
  file. The server enforces sequential coverage of the announced size, reads
  back written blocks, and checks the complete SHA-256 before flush and rename.
  Empty files follow the same verification/commit path. Existing target
  permission bits are preserved; new remote files use 0644. Other metadata,
  ownership, extended attributes and timestamps are not replicated.
- Pulls verify block hashes, assemble a random same-directory temporary file,
  flush/fsync, verify its full readback digest and replace the destination.
  New host files use the temporary file's restrictive permissions; existing
  host permission bits are preserved. POSIX hosts also fsync the parent.
- Disconnects, incomplete uploads and checksum failures before rename never
  publish the staged file. Ordinary teardown releases handles and deletes the
  session's own temporary file. A reset/power loss can leave a `.osync-*.part`
  file; it is not auto-deleted or reused on the next transfer. Interrupted
  trees can be rerun: already committed files are skipped, but partial-file
  resume is not implemented.
- Replacement uses OsitoFS3's existing namespace rename. This is not a claim
  of power-loss-atomic filesystem transactions. I/O/flush failure during or
  after rename can leave the new name visible without a success response.
  Rerun the comparison after recovery. A device rejecting cache flush still
  has the durability limitations documented in the USB storage notes.
- Retained read handles and file revisions reject detected concurrent source
  changes. Directory cursors walk the child index, not the entire inode table,
  and fail if the directory generation changes. Destination identity/revision
  is checked before replacement; busy OsitoFS3 targets are rejected. There is
  no filesystem compare-and-swap rename, so callers must not concurrently
  modify/replace the same destination during synchronization.
- Frames are bounded to 32 KiB plus 32 bytes; file offsets/sizes are 64-bit.
  The kernel uses fixed buffers, independent of file size. Host signatures
  spill to a temporary file beyond 1 MiB; directory entries are held per
  directory. The shell's 4 KiB `cat` cap and 1 MiB SSH console capture cap do
  not apply. The binary session does not have the console's 1 GiB traffic cap;
  TinySSH rekey and inactivity/cancellation limits still apply.
- Both ends hash all compared bytes, and a changed file needs staging disk
  space for its entire new contents. Requests are sequential, not pipelined.
  This saves network traffic but is not a zero-copy or minimum-disk-I/O path.
  No rolling checksum, compression, sparse-file preservation, hard links,
  `--delete`, partial-file resume or parallel connections are implemented.
- Paths are bounded by OsitoFS3's 259-byte path limit. The client rejects
  non-portable filenames, case-fold collisions, symlinks and Windows reparse
  points rather than allowing traversal, ADS or device-name interpretation.
  Same-directory staging also needs room for the temporary basename in that
  path limit. No filename or data is interpolated into a remote shell command.

## Protocol

One request and response at a time. Each frame is `u32be length` followed by
that many bytes (1 through 32800). A request starts with an ASCII opcode; a
response starts with status (0 success, 1 missing, 2 end, 3 invalid, 4 I/O,
5 changed, 6 busy, 7 checksum). Error responses contain only the status byte.
All integers below are big-endian. Paths/names are UTF-8 without NUL or an
additional length prefix; the frame determines their length.

| Opcode | Request after opcode | Success payload |
| --- | --- | --- |
| V | empty; must be first and only once | `OSYNC001`, u32 block size |
| S | path | kind (1 file, 2 directory), u64 size |
| L | directory path | empty; opens/replaces one directory cursor |
| N | empty | kind, basename; status 2 ends enumeration |
| O | regular file path | u64 size; one retained reader |
| H | u64 offset, u32 length | SHA-256 of that reader range |
| R | u64 offset, u32 length | exact range bytes |
| X | empty | validates reader revision and closes it |
| B | u64 final size, destination path | empty; one staged writer |
| W | bytes, 1 through 32768 | empty; appends after readback |
| C | u64 reader offset, u32 length | empty; appends from retained reader |
| F | full 32-byte SHA-256 | empty; validates complete coverage and commits |
| A | empty | aborts writer only |
| M | directory path | empty; creates or verifies directory |
| Q | empty | releases state and ends subsystem session |

Malformed framing closes the channel. The PTY is rejected for this subsystem;
normal console commands retain their existing behavior. Upstream TinySSH
sources are unchanged. Application framing lives in the OS port, and the file
protocol has a separate storage adapter so the same engine is tested on the
host and inside the kernel.

## Tests

```sh
bash arch/x86/scripts/test-file-sync.sh build/file-sync-host
CC=clang SANITIZE=1 bash arch/x86/scripts/test-file-sync.sh build/file-sync-sanitize
FILE_SYNC_TEST=1 bash arch/x86/scripts/build-ssh-host.sh build/file-sync-ssh
python3 arch/x86/scripts/test-file-sync-ssh.py arch/x86/build/file-sync-ssh
make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf
bash arch/x86/scripts/test-ssh-qemu.sh arch/x86/build/file-sync-qemu
```

Host tests exercise large binary files, same-size edits, empty/shrunk/grown
files, 64-bit offsets, trees, Unicode/spaces, dry-run, no deletion, invalid paths
and frames, symlinks, concurrent changes, bad checksums, write/flush failures
and interrupted uploads. OpenSSH tests exercise fragmentation, backpressure,
rekey, unknown subsystems and reconnect/console isolation. QEMU gates exercise
the actual TCP and OsitoFS3 adapter, replacement, delta copies, disconnect
cleanup, persistence across cold boot and the existing SSH/kexec regressions.
None of these substitutes for a physical P1 transfer over AX200.

### 2026-09-24 Qualification

The 7,112,656-byte kernel has SHA-256
`4d24574f4b0bd84ee2bdc2799e3429cc2bc456fbeb6cd2a1152df07ece7f8619`.
All 12 host cases pass with GCC and Clang ASan/UBSan. OpenSSH sync gates pass
normally and with sanitizers; the 11 existing host SSH regression gates pass
with the subsystem enabled. The 16 production-kernel QEMU gates pass in
`arch/x86/build/file-sync-qemu-20260924-v2/results.json`, pinned to that hash.
The first QEMU run caught an adapter permission-mask error when replacing
files; the v2 run includes its fix. Test disks use the managed scratch area and
are released by the runner. This kernel has not been deployed to the P1 yet.
