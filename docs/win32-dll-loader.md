# Win32 DLL Loader

The Win32 layer models loaded modules as process-owned PE images. Every process
has a PEB-like linked list used by `GetModuleHandle`, `LoadLibrary`, export
resolution, address lookup, thread notifications, and teardown. This avoids a
system-wide 1024-slot scan for each lookup while preserving case-insensitive
base-name matching and separate PE32/PE32+ shim facades.

Image mapping and `DllMain` execution are serialized by a recursive loader lock
per Win32 process. Independent CEF helper processes can therefore load DLLs in
parallel, while callbacks and dependency loads inside one process retain
Windows-style serialization. Built-in shims serve the role of KnownDLLs;
already-loaded modules are checked before filesystem search, followed by the
application directory and normalized OsitoFS paths.

Each real DLL keeps a private mapped image, relocations, IAT, TLS, and section
protections. Immutable source bytes are retained in a system-wide 512 MiB LRU
cache, keyed by the stable OsitoFS file object, size, and per-file revision.
OsitoFS revisions use an odd/even mutation protocol so a concurrent write can
never publish a torn cache entry. This mirrors the useful part of Windows file
caching without sharing writable PE state. Sharing mapped image pages still
requires prototype PTEs, physical-page references, and copy-on-write.

Large read-only file mappings use a second Windows-style ownership layer. Each
`SECTION_OBJECT` still owns its handles, ACL, identity, and views, while a
system-wide control area owns clean sparse pages for the backing file. Control
areas are keyed by file object, revision, logical length, and rounded section
size, and remain cached after the final view is unmapped. The cache has 32 LRU
entries and a 256 MiB soft limit. Pagefile sections, writable mappings, and
`SEC_IMAGE` are deliberately excluded until copy-on-write and writeback can be
modeled correctly.

The module record and mapped `ImageBase` are published before import walking,
which permits circular dependency resolution. Required imports are recorded in
a bounded diagnostic ledger. Compatibility mode remains the default while the
shim surface is incomplete; `win32-loader strict on` switches to NT-style
load failure for an unresolved required import.

Named PE exports follow the native loader's snap algorithm: try the import
table hint first, then binary-search the lexically sorted export name pointer
table. Ordinal imports remain direct table lookups, including `DLL.#ordinal`
forwarders. This avoids repeatedly scanning large export tables during CEF
helper startup.

Thread teardown follows the loader lifetime contract as well. `ExitThread` and
`FreeLibraryAndExitThread` never return: they run `DLL_THREAD_DETACH`, release
the current thread's static TLS, and transfer control to the scheduler. PE64
workers unwind to a saved scheduler-stack continuation; the common return path
releases the Win32 stack before signaling the NT thread object. Compat32 keeps
the context active for the reaper until its `INT 0x2E` callback gate is safe to
retire. Direct `TerminateThread` intentionally skips DLL detach callbacks.

## SEC_IMAGE Roadmap

The remaining startup-memory gap is mapped-image sharing. It must not be built
on the current contiguous `pe_alloc()` buffer: the physical allocator has no
page references, and the existing `PTE_COW` helper has no control-area owner.
Implement this in dependency order:

1. Add physical-page retain/release accounting and explicit shared-page tests.
2. Add an image control area keyed by file identity, revision, machine, and PE
   layout; cache clean prototype pages independently of process views.
3. Map clean header, `.text`, and `.rdata` pages read-only into each process.
4. Map IAT, relocation-touched pages, TLS, and writable sections privately or
   as write-copy views; a write fault materializes a private page.
5. Make unload remove the view and release page references only after loader,
   callback, and asynchronous users are quiescent.

Until those invariants exist, the source-byte LRU avoids repeated OsitoFS I/O
while every process retains a private relocated image. This costs memory, but
does not risk freeing or modifying a page still executable in another CR3.

The in-OS regression test is:

```text
win32-module-test
win32-loader
```

`win32-loader` reports DLL-source and section-control hits, export hint/binary
lookups, materialized pages, disk bytes, avoided bytes, and unresolved imports.
Use `win32-loader flush` to
evict inactive entries from both caches and `win32-loader clear` before a
focused import trace.

For realistic startup measurements, boot QEMU and run
`.debug/profile-cef-startup.sh`; compare time to the first Crypt32 chain event
and inspect `arch/x86/build/serial.log` for loader faults or stale-lock recovery.

The intended behavior follows the Windows
[DLL search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order),
[load-time linking](https://learn.microsoft.com/en-us/windows/win32/dlls/load-time-dynamic-linking),
[section and view](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/section-objects-and-views),
[mapped-view coherence](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile),
and [PE IAT](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
contracts.
