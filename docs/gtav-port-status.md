# GTA V Single-Player Port — Status

Port of GTA V SP to OsitoK (bare-metal x86-64 OS). Full source access
to both RAGE engine and OsitoK kernel. Target: playable SP from Prologue
to credits.

## Current State (Jun 20 2026)

**Build**: `gtav-app-shell` is a 94.0 MiB static x86-64 SysV ELF, compiled
with the OsitoK GCC/musl toolchain. The production link now consumes the
audited retail frontiers for Core, Audio, Graphics, Physics, Creature,
Security, Network, Framework, SuiteCreature and Script plus the required
RageMisc closures. The 263-TU `game4_lib` frontier also builds, passes the
combined whole-archive audit and is present in the production link lazily.

**Runtime**: FSM reaches RunGame (state 2), game loop ran 1.5M+ ticks with
CSystem::BeginUpdate/EndUpdate active. Compositor shows GTA5 window via
virtio-gpu. On macOS, the patched QEMU 11.0.1 SDL/OpenGL core build boots the
virtio-gpu-gl/VIRGL path to shell with VG3D selftests T2-T9 passing. A QEMU
8GB + `-display sdl,gl=core` run now reaches DXVK swapchain creation, repeated
successful presents, shader/effect loading, and passes the previous
`dialoguecharacters.meta` inflate crash point.

**Assets**: 24 RPFs (39GB) loaded from 60GB NVMe image. common.rpf + x64a-x64w
all RPF7-valid. Real assets rendered (icon.jpg, hires_lrg2.bmp from common.rpf).

**QEMU Venus presentation fix (Jun 20 2026)**: macOS QEMU runs use the patched
render-server virglrenderer plus `-display sdl,gl=core`. `GTA5.elf` must be
relinked after `make vulkan-user`; rebuilding only `kernel.elf` leaves the old
Venus ICD embedded in the game binary. Verified run `osito-renderfix24` creates
one compositor target (`h=2`, `flags=0x07`) and three CPU-only swapchain
backbuffers (`h=3..5`, `flags=0x03`) with no extra `[COMP]` windows, eliminating
the debug-pattern/flicker path caused by scanout backbuffers.

## Architecture

```
gtav-app-shell (static x86-64 SysV ELF, musl libc)
├── Manifest-derived RAGE retail archives
│   ├── Core / Audio / Graphics / Physics / Creature / Sec / Net
│   ├── Framework / SuiteCreature / Script / required RageMisc clusters
│   └── game4_lib (Vehicles / vehicleAi / weapons / VFX / text / tools)
├── Osito platform closures
│   ├── pthread, VFS, socket and asynchronous task backends
│   └── D3D11 -> DXVK -> Vulkan/Venus graphics backend
└── Deterministic CApp FSM and subsystem probes

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
| Task scheduler (task_ositok.cpp) | DONE | Pthread worker queues, 512 reusable handles, prepared/local dispatch, aligned scratch, same/cross-scheduler wait assistance and phased draining shutdown. The standalone lifecycle smoke and concurrent retail collider, NaturalMotion and force-solver app-shell probes pass Venus. |
| RPF loading | DONE | 24 packs, StreamingInstall override |
| CApp FSM | DONE | InitSystem→InitGame→RunGame |
| CFileMgr | DONE | Real impl compiled, 24 RPFs mounted |
| CGame::Init | PARTIAL | Compiles, InitWidgets/DLC/LoadingScreens guarded |
| Parser (attribute.h) | DONE | Real header from Windows source |
| GPU backend | PARTIAL | virtio-gpu 2D + VIRGL negotiate; VG3D host ctx lifecycle passes; real Venus submit stream pending |
| Audio backend | STUB | HDA driver exists, no RAGE bridge |
| Input | STUB | Kernel has xHCI+evdev, no RAGE bridge |
| Streaming (pgStreamer) | STUB | Thread creation works, real streaming pending |
| Script VM (.ysc) | PARTIAL | The complete seven-TU RageScript retail library compiles, links and passes whole-audit; game `.ysc` mounting, native registration and execution are pending. |
| Save/Load | NOT STARTED | Needs OsitoFS file write |
| Network transport/Social | PARTIAL | RageNet transport and wolfSSL are real and validated offline; native Rockstar Social Club login/UI/entitlements remain unavailable outside Win32. |

## Current Blocker

**Current runtime barrier**: After loading `dialoguecharacters.meta` and
`hudcolor.dat`, the QEMU run remains alive in a high-rate DXVK/D3D11 present
loop. There is no `#PF`, `#GP`, futex corruption log, or process crash in the
serial output. The next investigation target is whether the black frame is
expected loading-screen behavior, missing assets, or a render/presenter state
machine issue.

**Presentation state**: The previous QEMU flicker/pattern was not real GTA
rendering. It came from Venus allocating each swapchain image through
`SYS_SHM_MKSURFACE`, which created compositor windows for backbuffers. Swapchain
images now use `SYS_SHM_CREATE` CPU-visible SHM only; `vkQueuePresentKHR` copies
the selected backbuffer into the OsitoK compositor surface SHM and flips that
surface. `DXGI_PRESENT_TEST` still returns without presenting, so a real frame
requires a non-test `Present`.

**Hardware `sysBuddyHeap` crash triage**: Hardware logs
`hw-logs/20260620-012805-usb/diag/cr0_00.txt` and
`hw-logs/20260620-022854-usb/diag/cr3_00.txt` faulted at `0x09b4d74b`,
`rage::sysBuddyHeap::Init+0x8b`, with `CR2=0x1f0`. The bytes at that RIP are
`41 0f 11 87 58 01 00 00`: in 64-bit mode `0x41` is a REX prefix and the store
uses `r15+0x158`; in compat/legacy mode it is `inc ecx`, then `movups` stores
through `edi+0x158`, matching `edi=0x98 -> 0x1f0`. The failure was therefore
a bad `CS=0x10` SYSCALL selector, not heap corruption. SYSCALL is now pinned to
private 64-bit GDT entries `0x90/0x98`.

**Crash diagnostics**: Native ELF faults may arrive with startup `CS=0x28`, the
fixed SYSCALL-return `CS=0x90`, or legacy crash dumps using the old buggy
`CS=0x10`. The crash reporter handles all three and normalizes low stack
aliases against the high direct-map mirror before walking RBP frames.

**Hardware render-thread diagnostics**: `hw-logs/20260620-030043-usb` reached
the GTA render-thread spawn path on serial but persisted only through
`gta-before-commonmain`, with no `cr*.txt`. OsitoK now exposes private syscall
`522` (`SYS_BOOT_DIAG_MARK`) so userspace can call `boot_diag_mark()` directly.
The GTA port marks `CommonMain`, `CSystem::Init`, `grcSetup::Init`,
`gRenderThreadInterface.Init`, `RenderThreadInit`, and render-thread loop entry
so a hardware hang without an exception still leaves the last reached stage in
`diag/latest.txt` / `diag/boot*.log`.

**Hardware futex crash fixed in test**: Real hardware logs from
`/private/tmp/boot0.log` and `/private/tmp/cr0_00.txt` captured `GTA5.elf`
crashing in kernel mode at `futex_requeue_locked`. The kernel now cleans stale
futex waiters when a process/thread exits, validates futex addresses before
wait/wake/requeue, guards futex bucket chains against corrupt indices, and
implements six-argument futex dispatch for `FUTEX_CMP_REQUEUE` and
`FUTEX_WAIT_BITSET`.

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
| `GTAV Source/CMakeLists.txt` | Manifest-derived retail frontiers, archive ownership and production app-shell link |
| `GTAV Source/ports/ositok/game/core/app_shell_ositok.cpp` | Deterministic subsystem and Framework runtime probes |
| `GTAV Source/ports/ositok/forceinclude/ositok_original_platform.h` | Osito platform identity and ABI feature selection |
| `GTAV Source/ports/ositok/rage/` | VFS, networking, graphics and other Osito platform owners |
| `GTAV Source/ports/ositok/rage/system/task_ositok.cpp` | Asynchronous RAGE task scheduler and handle lifecycle |
| `osito-k/arch/x86/fs/ositofs2.c` | Guest OsitoFS implementation used by GTA mounts |
| `osito-k/arch/x86/scripts/test-venus-user.sh` | Automated Venus/DXVK/app-shell regression |

## Next Steps

1. Inspect the present loop after `hudcolor.dat`: capture the QEMU window and
   correlate it with D3D11/DXVK state.
2. Resolve missing optional asset probes such as `platformcrc:/data/startup.meta`
   and `update2:/x64/data/lang/american_rel.rpf` if they block progression.
3. Validate partial `mprotect` with `arch/x86/test/mmap_test.c`.
4. Remove the GTA `sysMemVirtualAllocate(size, bool)` workaround after kernel VM validation.
5. GPU backend: virtio-gpu-gl 3D -> Vulkan ICD -> DXVK -> RAGE D3D11
6. Audio backend: HDA driver -> RAGE audiosystem bridge
7. Input: xHCI/evdev -> RAGE ioKeyboard/ioMouse
8. Script VM: compile rage/script/, load .ysc from script.rpf
9. Loading screens: Scaleform + grcDevice integration

## Build Commands

```bash
# Production app-shell build
cmake --build /root/gtav-dxvk-smoke --target gtav_ositok_app_shell -j4

# Deploy to NVMe
tools/ositofs/ositofs-write /root/osito/nvme_gcc.img \
    /root/gtav-dxvk-smoke/gtav-app-shell --name gtav --overwrite

# Refresh embedded Vulkan ICD before deploying GTA after Venus edits
cd ~/ok-ported/GTAV_Source
make vulkan-user
make -W GTA5_ositok.o GTA5.elf

# Run with macOS HVF + SDL GL core/VIRGL.
# Requires the patched QEMU 11.0.1 build that creates a 4.1 core context.
PATH=/private/tmp/qemu-core-src/qemu-11.0.1/build:$PATH \
  bash arch/x86/scripts/qemu-test.sh --no-build --hvf
# In OsitoK shell: exec GTA5.elf
```

## QEMU Configuration

The currently validated Venus baseline is intentionally limited to 4 GiB,
4 vCPUs, 512 MiB of virtio-gpu host memory, and TCG. Larger RAM/SMP and KVM
configurations currently break NVMe/OsitoFS, while WSL Vulkan exposes only
llvmpipe rather than the RTX 3090. See `docs/dev-environment.md`, section
"Current WSL Venus resource limits", for the measured matrix and the criteria
for treating memory or GPU availability as a rendering blocker.

```
-m 8G -smp 4 -machine q35,accel=hvf -cpu host
-device virtio-vga-gl,hostmem=256M,blob=on,venus=on -display sdl,gl=core
-device e1000e -device qemu-xhci -device usb-kbd -device usb-mouse
-device intel-hda -device nvme
```

Set `DYLD_LIBRARY_PATH` to the patched virglrenderer build and
`RENDER_SERVER_EXEC_PATH` to its `virgl_render_server` binary before launching
QEMU. On macOS, QEMU must request the OpenGL core profile; compatibility GL can
crash during virtio-gpu/virgl scanout reset.
