# GTA V Single-Player Port — Status

Port of GTA V SP to OsitoK (bare-metal x86-64 OS). Full source access
to both RAGE engine and OsitoK kernel. Target: playable SP from Prologue
to credits.

## Current State (Jun 18 2026)

**Build**: GTA5.elf 7MB, compiled with x86_64-ositok-gcc 14.2.0 + musl libc.
1199 RAGE .o files in rage_core.a. Game core files (main.cpp, app.cpp,
system.cpp, game.cpp, filemgr.cpp) compile and link.

**Runtime**: FSM reaches RunGame (state 2), game loop ran 1.5M+ ticks with
CSystem::BeginUpdate/EndUpdate active. Compositor shows GTA5 window via
virtio-gpu. On macOS, the patched QEMU 11.0.1 SDL/OpenGL core build boots the
virtio-gpu-gl/VIRGL path to shell with VG3D selftests T2-T9 passing.

**Assets**: 24 RPFs (39GB) loaded from 60GB NVMe image. common.rpf + x64a-x64w
all RPF7-valid. Real assets rendered (icon.jpg, hires_lrg2.bmp from common.rpf).

## Architecture

```
GTA5.elf (userspace, musl libc)
├── RAGE Engine (rage_core.a, 1199 objects)
│   ├── ipc_ositok.cpp — real threading via syscall (clone, futex, nanosleep)
│   ├── device_ositok.cpp — file I/O via OsitoK VFS
│   ├── ositok_stubs.cpp — controlled stubs (StreamingInstall, singletons)
│   └── ositok_phase1_stubs.cpp — 1049 auto-generated long-return stubs
├── Game Core (main.cpp, app.cpp, system.cpp, game.cpp)
└── PSC-generated headers (gen_psc_headers.py v4.5, 543 .psc files)

OsitoK Kernel
├── sys_clone (CLONE_THREAD) + sys_futex (WAIT/WAKE, 256 slots)
├── sys_mmap (anonymous demand-paged, 4096 VMAs)
├── sys_nanosleep + sys_sched_yield + sys_gettid + sys_clock_gettime
├── NVMe + OsitoFS (60GB image, 33 files)
├── Virtio-GPU 2D (1024x768 BGRA scanout)
└── SMP 4-core (AP workers, parallel matvec 4x)
```

## Subsystem Status

| Subsystem | Status | Notes |
|-----------|--------|-------|
| Platform identity (RSG_OSITO_K) | DONE | platform.h, forceinclude, Makefile |
| File I/O (fiDeviceOsitoK) | DONE | VFS mount game:/, RPF7 read+inflate |
| Heap (InitGameHeap) | DONE | 256MB via mmap at 0x504001000 |
| Threading (ipc_ositok.cpp) | DONE | Real clone/futex/nanosleep |
| RPF loading | DONE | 24 packs, StreamingInstall override |
| CApp FSM | DONE | InitSystem→InitGame→RunGame |
| CFileMgr | DONE | Real impl compiled, 24 RPFs mounted |
| CGame::Init | PARTIAL | Compiles, InitWidgets/DLC/LoadingScreens guarded |
| Parser (attribute.h) | DONE | Real header from Windows source |
| GPU backend | PARTIAL | virtio-gpu 2D + VIRGL negotiate; VG3D T2-T9 pass; D3D11→DXVK→Vulkan pending |
| Audio backend | STUB | HDA driver exists, no RAGE bridge |
| Input | STUB | Kernel has xHCI+evdev, no RAGE bridge |
| Streaming (pgStreamer) | STUB | Thread creation works, real streaming pending |
| Script VM (.ysc) | NOT STARTED | rage/script/ not compiled |
| Save/Load | NOT STARTED | Needs OsitoFS file write |
| Network/Social | EXCLUDED | Stubs, offline-only |

## Current Blocker

**Allocator bring-up**: GTA now reaches RAGE allocator initialization with a
multi-allocator graph: game heap plus growable buddy allocators for resource
virtual and physical memory. The previous invalid-cast crash in
`pgRscBuilder::ComputeLeafSize()` is resolved by using real resource buddy
allocators.

**Kernel VM fix**: RAGE's PC `sysMemVirtualAllocate(size, bool)` creates a
one-page guard with `mprotect(PROT_NONE)`. OsitoK now splits VMAs for partial
`mprotect` ranges, so the guard page no longer changes protection on the whole
allocator workspace. Demand paging also refuses to fault in `PROT_NONE` VMAs
and updates present pages through the active process CR3. `munmap` now accepts
ranges spanning split VMAs so callers can still release the original allocation.

**Port workaround**: `GTAV_Source/ositok_stubs.cpp` still overrides both
`sysMemVirtualAllocate` overloads with a simple 64KB-aligned mmap path. Keep
that until the kernel VM change has been validated with non-GTA mmap tests.

## Key Files

| File | Purpose |
|------|---------|
| `GTAV_Source/Makefile` | Build rules, source lists, cross-compiler flags |
| `GTAV_Source/GTA5_ositok.cpp` | Entry point, heap init, VFS mount |
| `GTAV_Source/ositok_stubs.cpp` | Controlled stubs (singletons, StreamingInstall) |
| `GTAV_Source/ositok_phase1_stubs.cpp` | 1049 auto-generated function stubs |
| `GTAV_Source/ositok_auto_stubs.cpp` | C++ mangled symbol stubs |
| `GTAV_Source/gen_psc_headers.py` | PSC XML → C++ header generator |
| `GTAV_Source/src/dev_ng/rage/base/src/system/ipc_ositok.cpp` | Threading layer |
| `GTAV_Source/src/dev_ng/rage/base/src/file/device_ositok.cpp` | File I/O |
| `GTAV_Source/src/dev_ng/rage/base/src/forceinclude/ositok_beta.h` | Platform defines |

## Next Steps

1. Rebuild kernel and GTA5.elf, then retest `exec GTA5.elf` from QEMU.
2. Validate partial `mprotect` with `arch/x86/test/mmap_test.c`.
3. Remove the GTA `sysMemVirtualAllocate(size, bool)` workaround after kernel VM validation.
4. GPU backend: virtio-gpu-gl 3D → Vulkan ICD → DXVK → RAGE D3D11
5. Audio backend: HDA driver → RAGE audiosystem bridge
6. Input: xHCI/evdev → RAGE ioKeyboard/ioMouse
7. Script VM: compile rage/script/, load .ysc from script.rpf
8. Loading screens: Scaleform + grcDevice integration

## Build Commands

```bash
# Full rebuild
cd ~/ok-ported/GTAV_Source && make clean && make -j4

# Single file
make src/dev_ng/rage/base/src/system/ipc_ositok.o

# Deploy to NVMe
cd ~/osito-k
tools/ositofs/ositofs-delete arch/x86/build/nvme.img GTA5.elf
tools/ositofs/ositofs-write arch/x86/build/nvme.img ~/ok-ported/GTAV_Source/GTA5.elf --name GTA5.elf

# Run with macOS HVF + SDL GL core/VIRGL.
# Requires the patched QEMU 11.0.1 build that creates a 4.1 core context.
PATH=/private/tmp/qemu-core-src/qemu-11.0.1/build:$PATH \
  bash arch/x86/scripts/qemu-test.sh --no-build --hvf
# In OsitoK shell: exec GTA5.elf
```

## QEMU Configuration

```
-m 4G -smp 4 -machine q35,accel=hvf -cpu host
-device virtio-gpu-gl-pci,hostmem=256M,blob=on -display sdl,gl=core
-device e1000e -device qemu-xhci -device usb-kbd -device usb-mouse
-device intel-hda -device nvme
```
