# External Direct3D9 backend

**Status, 2026-10-08: superseded experiment.** The required architecture is
the original Microsoft Direct3D runtime running against compatible Osito-K
driver interfaces and the physical GPU. The user explicitly rejected
WineD3D/DXVK translation and a replacement Direct3D implementation. Do not
extend, install or tune this package as the solution to that requirement.
The instructions and results below are retained as historical evidence.
See [the current graphics contract](win32-graphics-latency.md#original-microsoft-runtime-contract-2026-10-08).

The Win32 `d3d9.dll` shim forwards `Direct3DCreate9` and `Direct3DCreate9Ex`
to an installed `d3d9-provider.dll`. WineD3D 3.8 implements the D3D9 device;
the separately built Mesa 25.0.0 module provides OpenGL through softpipe.
Rendering runs on the CPU. These binaries are installed in the filesystem,
not linked into `kernel.elf`.

The Microsoft DirectX installer still supplies its original auxiliary DLLs,
including D3DX and shader compiler versions. This backend package does not
contain or replace those files. Its selected DLLs also do not replace the
kernel's `d3d9.dll` or `opengl32.dll` entry points.

## Prepare a package

Requirements: Python 3.10 or later, `tar` with zstd support, and the module
toolchain described by `arch/x86/lib/opengl/Makefile`. GNU tar needs `zstd`
on PATH; Windows' libarchive-based tar supports the retained MSYS2 packages.

Build the external module in Linux/WSL:

```sh
make -C arch/x86/lib/opengl module
```

Then prepare a new installation tree from the repository root:

```sh
python tools/win32/prepare-d3d9-backend.py prepare \
  --module arch/x86/build/lib/opengl-module/libGL-osito.so \
  --cache build/d3d9-backend-cache \
  --output build/d3d9-backend-root
python tools/win32/prepare-d3d9-backend.py verify build/d3d9-backend-root
```

Use one line in PowerShell, or PowerShell continuation syntax. `--module`
can point to a separately qualified module build. Preparation rejects an
existing output directory. `--offline` requires all pinned inputs in the
cache; it never substitutes a DLL from the host's PATH or MSYS2 installation.

[The source lock](../tools/win32/d3d9-backend-sources.json) pins the archive
URLs, sizes, SHA-256 values, selected members and destinations. Archives and
individual members are verified before output creation. Only the selected
members are extracted to memory, never archive directory trees. The output
includes package metadata, license notices and a manifest with every file's
size and SHA-256. `verify` rejects missing, changed or extra files and reparse
points. It verifies consistency with the checked-in source lock; the manifest
is not a signed release or a claim of compatibility with arbitrary kernels.

| Component | Installed path |
| --- | --- |
| 32-bit WineD3D and its GCC/pthread dependencies | `/Windows/SysWOW64/` |
| 64-bit WineD3D and its GCC/pthread dependencies | `/Windows/System32/` |
| Osito-K Mesa module, used by both Win32 ABIs | `/libGL-osito.so` |
| Manifest, package provenance and license notices | `/System/Backends/d3d9/` |

Each DLL directory contains `d3d9-provider.dll`, `wined3d.dll`, `libwine.dll`,
`libwinpthread-1.dll`, and its architecture's `libgcc_s_*-1.dll`.
The MSYS2 `d3d9.dll` member is renamed only at installation; its bytes remain
unchanged. The runtime versions differ between ABIs because this locks the
exact combinations qualified by the Win32 tests.

## Install over SSH

Use a kernel containing the external-provider bridge, the graphics dependency
loader fixes, shared WGL objects, `GetDCEx(DCX_USESTYLE)` and message-only WGL
drawables. The qualified development kernel is r48 (see the evidence below).
Stop graphical applications before replacing backend files; leave the P1 in
the shell. Installation does not update/reboot the kernel or launch Steam.

Use the host/key options in [file synchronization](x86-file-sync.md). For
example, from PowerShell:

```powershell
$peer = @('--host', 'osito@192.168.1.12',
          '-i', "$HOME/.ssh/osito_p1_ed25519",
          '--known-hosts', "$HOME/.ssh/osito_p1_known_hosts",
          '--host-key-alias', 'osito-p1')
python tools/win32/prepare-d3d9-backend.py verify build/d3d9-backend-root
if ($LASTEXITCODE -ne 0) { throw 'Backend verification failed' }
python tools/osito-sync.py @peer -n -r push build/d3d9-backend-root /
python tools/osito-sync.py @peer -r push build/d3d9-backend-root /
```

Before upgrading an existing backend, retain its prior package or pull its
files to a separate backup with `osito-sync.py`. The transfer compares hashes,
verifies replacement files, and preserves unrelated destination files. An
interrupted tree transfer can be rerun while applications remain stopped;
the entire tree is not one atomic transaction. A final dry run must report
zero files to copy before launching applications. Restore the previous
package through the same transport if rollback is required.

Keep only the rollback files needed by the current update. Development runs
must not accumulate a kernel, full source snapshot or backup archive for every
revision. Preserve the working tree and compact test evidence; remove obsolete
build outputs after qualification. A single reusable QEMU base is sufficient
for the graphics tests.

## WineD3D command-stream setting

The external utility `wined3d-config.exe` reads or changes the global
`HKCU\Software\Wine\Direct3D\csmt` DWORD. WineD3D reads this setting when
its DLL loads; existing application processes keep their current mode.
Per-application Wine registry overrides, if present, take precedence.
The setting is implemented by the external renderer, not a kernel default.

Build the utility and optional comparison probes:

```sh
bash arch/x86/scripts/build-wined3d-config.sh build/wined3d-config
bash arch/x86/scripts/build-d3d9-benchmark.sh build/d3d9-benchmark
```

Copy `build/wined3d-config/pe64/wined3d-config.exe` to the guest, for example
`/Diagnostics/Win32/wined3d-config.exe`, with `tools/osito-sync.py` and the
same SSH options used above. Run from the Osito-K shell:

```text
winexec /Diagnostics/Win32/wined3d-config.exe csmt
winexec /Diagnostics/Win32/wined3d-config.exe csmt 0
winexec /Diagnostics/Win32/wined3d-config.exe csmt 1
winexec /Diagnostics/Win32/wined3d-config.exe csmt default
```

The first command queries without creating a key. `0` selects synchronous
execution, `1` selects a separate command-stream thread, and `default` removes
the value (WineD3D 3.8 defaults to `1`). Writes are flushed and read back; errors
return exit code 1 and invalid arguments return 2. Run configuration commands
on the guest, not on the Windows development host. The normal backend package
does not change this setting or install the optional utility automatically.

With the same r48 kernel, Mesa module and provider DLLs, three QEMU repetitions
per mode and ABI measured these medians for 16 alternating fixed-function and
shader frames, including per-frame pixel readback and `Present` calls:

| ABI | `csmt=1` | `csmt=0` |
| --- | ---: | ---: |
| PE32 | 5,940 ms | 220 ms |
| PE64 | 2,010 ms | 240 ms |

All 12 runs verified the expected triangle/background pixels and restored the
previous registry value. Context-creation thread traces confirm the selected
execution modes in both ABIs; PE32 also emits WineD3D's setting diagnostic.
The benchmark excludes setup/teardown and does not measure display completion
or application frame rate. In particular, the first PE32 multithreaded capture
still shows the preceding green frame; both synchronous captures show the
final blue frame. These results do not establish a Braid or P1 speedup.
The native benchmark variants do not write Wine registry settings; the
`csmt0`/`csmt1` variants are intended for an isolated test guest and restore
the prior value on normal completion.

Evidence: `arch/x86/build/msgwait-20261003/d3d9-csmt-qemu-r48-r2/qualification.json`,
its per-run logs, and `framebuffer-pixels.json`.

## Validated coverage and remaining work

The r48 kernel and module SHA-256 values are:

```text
kernel: bf8bcf1616dd945ef2370279997071014d7eca18809fcb4926c4a3339f1f16d6
module: 7df8a3b466285d19658dbc522621a81d5a0144aa9240036fd271a5172202371d
```

The native Windows and guest probes cover both PE32 and PE32+, ordinary and
message-only windows, base/Ex factories, shader drawing, readback, and visible
presentation. QEMU r48 also displays the Steam login UI with both provider
architectures installed and without a new GPU-process disable/restart.
Details and retained evidence paths are in sections 4.354 and 4.355 of
[Win32 correctness](win32-layer-correctness.md).

This qualifies the exercised D3D9 path. D3D10/11, DXGI, GLES3 and GPU hardware
acceleration are not qualified by these results and are not added by this
package, even though the WineD3D archives contain other DLLs. On 2026-10-08,
r48 was persisted and hotreloaded on P1, and the OpenGL and system D3D9 drawing
probes passed in both ABIs without exceptions. Braid gameplay and performance
still require physical validation. Microsoft's auxiliary DirectX installation
is a separate prerequisite for applications that import those DLLs.
