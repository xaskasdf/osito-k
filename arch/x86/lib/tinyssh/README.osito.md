# TinySSH port

Upstream: https://github.com/janmojzis/tinyssh
Release: 20260906
Commit: 50c1e9903b866ff320cba667013a83ebe5cfcc83
License choice: 0BSD (upstream also offers CC0, MIT-0 and MIT).
See upstream/LICENSE.md and upstream/cryptoint/README.md.

The selected upstream C files and headers are vendored without modification.
The OS adaptation lives outside upstream/: transport, entropy, session-local
failure unwinding, host signing, authorization and the console channel.
No POSIX sockets, fork, password authentication or emulated user database.
Only one connection runs at a time: upstream's protocol state is process-global.

The build uses the portable crypto implementations, -fwrapv and no SIMD.
Generated feature headers in port/ deliberately disable optional host libraries.
Protocol errors must unwind to the connection boundary, never halt the kernel.
