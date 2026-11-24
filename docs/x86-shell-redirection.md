# Shell Output Redirection

The P1 USB readback at 2026-09-22 17:52 UTC exposed a shell bug, not a
filesystem structural error. `dmesg > dmesg.log` reported 20,118 bytes written,
but the inode still had its old 400,788-byte size. The entire tail after byte
20,118 matched the previous dump. The old shell overwrote from offset zero
without truncation, skipped empty output and ignored write/flush errors.

Output preparation now uses the existing OsitoFS API before dispatch:

- `>` creates or truncates the target, without deleting/recreating its inode.
- `>>` creates a missing file, or writes at the existing end without truncation.
- Empty command output still creates/truncates/flushes as appropriate.
- A target-open/type/truncate/allocation error prevents command execution.
- Failed writes or flushes produce an explicit error, never `(synced)`.

`shell_dispatch()` contains the unchanged builtin selection. Returning early
from a builtin now returns to `shell_exec()`, which saves output, restores
the previous capture/stdin and frees buffers. Nested output redirections
restore the enclosing capture; status messages do not contaminate pipe data.
The file operations and bounded capture helper are in `shell_redirect.h`.

The existing 1-MiB capture allocation is retained (1 MiB minus one byte of
payload). Exceeding it now reports an incomplete file instead of success;
a pipeline does not pass a silently shortened capture to its next stage.
This remains a buffered mini-shell, not a claim of complete POSIX shell
parsing, streaming, atomic replacement or concurrent O_APPEND semantics.
Only output routed through the shell's capture hooks is redirected.

Host regression:

```sh
bash arch/x86/scripts/test-shell-redirection.sh arch/x86/build/shell-redirection-host
```

`build/shell-redirection-v1/` passes 1,199 checks per GCC/Clang normal/sanitized
run: short/empty overwrite, append/new/empty files, mode/create/truncate/write/
flush failures, append overflow and capture bounds. These tests use the actual
helper with mocked filesystem errors; they do not emulate USB hardware.

The real shell also passed QEMU UEFI/xHCI USB integration in
`build/realhw-usb-20260921/prph-redirection-deploy-20260922-v2/qemu-v2/`.
After QEMU exited, on-disk readback verified short overwrite, three zero-byte
files, append preservation, an early `touch` usage return, the next command's
restored capture, `echo | cat`, and nested redirection under `time`. An output
target failure prevented the test command's file-creation side effect.
Expected creation failure for directory `/diag` is checked separately from
unexpected storage errors. Console text alone is not the test oracle.

The first trial's empty fixture used `cat`, which currently adds a display
newline even for an empty file. Its failure is retained; the corrected trial
uses `clear` for genuinely empty output. `cat` behavior was not changed.
