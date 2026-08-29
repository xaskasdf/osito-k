# GTA V Single-Player Port — Status

Port of GTA V SP to OsitoK (bare-metal x86-64 OS). Full source access
to both RAGE engine and OsitoK kernel. Target: playable SP from Prologue
to credits.

## Current State (Aug 1 2026)

**Build**: `gtav-app-shell` is a 94.0 MiB static x86-64 SysV ELF, compiled
with the OsitoK GCC/musl toolchain. The production link now consumes the
audited retail frontiers for Core, Audio, Graphics, Physics, Creature,
Security, Network, Framework, SuiteCreature and Script plus the required
RageMisc closures. The 263-TU `game4_lib` frontier also builds, passes the
combined whole-archive audit and is present in the production link lazily.

**Runtime**: The deterministic app-shell FSM reaches `RunGame`, shuts down
cleanly, and passes the complete 4 GiB/4-vCPU TCG Venus regression. The suite
now includes a standalone asynchronous task-scheduler smoke, concurrent retail
collider/NaturalMotion/force-solver jobs, an endpoint-free RageSec generic worker,
Framework config/pool checks and an isolated retail FSM probe in addition to the
existing Creature, Network and DXVK probes.

**Assets**: 24 RPFs (39GB) loaded from 60GB NVMe image. common.rpf + x64a-x64w
all RPF7-valid. Real assets rendered (icon.jpg, hires_lrg2.bmp from common.rpf).

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
| RageCore retail frontier | DONE | All 404 compile entries from `RageCore_2015.vcxproj` build with one-for-one Osito platform backends; the resulting full archive replaces the 229-source bootstrap in `gtav-app-shell`, passes a whole-archive link audit, and passes the complete Venus app-shell regression. The task backend queues asynchronously on pthread workers and is validated by a dedicated Venus lifecycle/concurrency smoke. |
| RageGraphics retail frontier | PARTIAL | All 318 C++ entries from `RageGraphics_2015.vcxproj` compile. The production archive keeps 301 retail TUs, assigns 17 conflicting members to the existing 30-TU DXVK/input/runtime closure, links normally into `gtav-app-shell`, and passes the complete Venus regression. Platform fixes cover GCC EDGE assertions, Win32-only Logitech/XInput/monitor/focus paths, resource aliases and bounded input-event key tracking. DXVK vertex/index buffers implement factories, resource placement, CPU locks, partial GPU writeback, `Set`, clone lifetime and read-only transitions; canonical immediate triangles and quad batching use the existing dynamic draw transaction. The canonical portable image and viewport owners replace their reduced Scaleform subsets, publishing world/view/projection globals and providing window/scissor transforms plus frustum culling. Split state stores own the complete retail default state lifecycle. The texture owner preserves the `0x90` Win64 `grcTextureDX11` ABI, fixes RSC7 virtual/physical pointers, realizes serialized 2D/cube backing stores as DXVK textures and SRVs, and supplies base references, dictionary TLS, BPP tables and heap-safe factory lifecycle. The device maps legacy vertex registers through a 4 KiB DXVK constant buffer; mouse/pad lifecycle publishes a deterministic neutral state until Osito exposes those devices. Model-facing effect support now owns technique groups/maps, annotations, material lookup, instance clone/load, Color32 conversion, pass copies and preload lists. Venus validates `default -> draw`, a nine-local clone, normalized `64,128,191` Color32 data, pass-selective copy and annotation defaults alongside all prior probes. No RageGraphics-owned symbol remains unresolved in the whole-archive audit; its only four imports are Sony EDGE host-tool helpers absent from the tree. |
| RagePhysics retail frontier | DONE | All 296 C++ entries from `RagePhysics_2015.vcxproj` compile into `rage_ositok_ragephysics_full_archive`, link normally into `gtav-app-shell`, and pass a whole-archive audit with no unresolved symbols. The frontier preserves the complete curve, collision, articulated-body, bound, vendored Bullet, material, cloth/wind and generated solver source matrix. Portable fixes canonicalize two generated mixed-case paths, preserve the archetype save destination, use a GCC-valid intrusive-union offset, normalize an included SPU path and enable the existing host shape-test/force-solver branches for Osito without pretending to be Win32. Venus validates `phMaterial`, `phBoundSphere`, asynchronous collider gravity integration from `(1,2,3)` to `(1,-3,3)`, and a retail one-phase fixed/fixed `SolveConstraintTask` ending with allocation/release atoms `2/1`. Moving-body contact solving remains pending; frag-cache pointer validation remains gated until the game fragment allocator enters the runtime boundary. |
| NaturalMotion task runtime | PARTIAL | The retail `NmRsCBU_DBMPelvisControl` DynamicBalancer job runs concurrently with the collider task using its canonical read-only/body/pelvis packet ABI. Venus validates pelvis sway zero, hip pitch `0.125` and twist decay to `0.1875/-0.1875`. Foot placement and the full two-leg balance solver still need non-degenerate synthetic rigs or live character state. |
| RageCreature retail frontier | DONE | All 51 C++ entries from `RageCreature_2015.vcxproj` compile one-for-one into `rage_ositok_ragecreature_full_archive`, link normally into `gtav-app-shell`, and pass a whole-archive audit with no unresolved symbols. The frontier preserves animation streams/channels, frame data, weights, IK and all creature components without platform substitutions. The Osito force-include restores the retail 16/128-byte compile-time alignment contracts, and Creature alone disables strict aliasing to preserve its packed animation codecs under GCC. Its actual dependencies resolve through RageCore and the completed skeleton/metadata/viewport/shader owners in RageGraphics, not NaturalMotion or game stubs. Venus validates the 32-byte `crBlockStream`, mask table, raw interpolation at 15, compressed linear reconstruction at 25, step/skew modifiers and a fixed-position IK goal before the Physics and Graphics regressions. Retail `.ycd`/`RTLF` fixtures remain pending for persistent-format validation; the three historical `'RTLF'` multi-character warnings are non-fatal. |
| RageSec retail frontier | DONE | All 13 compile entries from `RageSec_2015.vcxproj` (nine C++ and four Yubico C files) compile into `rage_ositok_ragesec_full_archive`, link normally into `gtav-app-shell`, and pass a whole-archive audit with no unresolved symbols. Portable obfuscated types, engine, generic tasks and plugin scheduling remain active while PAPI, RageSecWinAPI, PE scanning and YubiKey paths stay behind real platform capabilities. Venus validates the obfuscated-data bridge and a canonical `rageSecGenericWorkItem` on a local retail thread pool: `crcRange` returns `0xC9E3F40C` from a distinct pthread without initializing `netTask`, plugin manager or endpoints. |
| RageNet retail frontier | DONE | All 219 entries from `RageNet_2015.vcxproj` compile one-for-one into `rage_ositok_ragenet_full_archive`: 61 core network, 133 RLINE, 14 miniupnpc, eight libpcp and three Portcullis units. The archive replaces RageSec's temporary task/time dependency and passes a whole-archive audit with no unresolved symbols. The external `wolfssl_2015.vcxproj` dependency contributes all 89 retail C units; Osito uses portable SP math and wolfSSL's POSIX `/dev/urandom` entropy path instead of absent Win64 assembly or BCrypt. Native Social Club, WinSock, TFIT launcher, PCP and UPnP paths require real Win32 services and retain their retail fallbacks on Osito. The two-TU Osito backend provides IPv4 UDP, ephemeral bind, non-blocking I/O, queue/buffer queries and interface address/MAC/MTU. The kernel now wires Linux x86-64 socket syscalls, `select`/`poll`, `FIONREAD` and interface ioctls into its existing BSD socket table. Venus obtains 32 cryptographic bytes and round-trips `OSITOK-RAGENET-UDP` between two real RAGE sockets before passing the full regression. |
| RageFramework retail frontier | DONE | All 364 compile entries from `RageFramework_2015.vcxproj` build one-for-one after canonicalizing 11 generated mixed-case paths. The full archive replaces the remaining bootstrap owners for FSM/config, packfiles, instancing and pools, links lazily in production, and passes both an isolated and a combined whole-archive audit. Osito keeps the PC ABI while Win32-only focus, multimedia, commerce/Social Club and D3D paths remain behind real `__WIN32PC`/D3D feature guards. The port also preserves the 96-byte PC `CNavMeshQuadTree` layout, adds the required DXVK device/effect/texture closure and supplies platform parameters, decal callbacks and native-hash audit hooks. Venus validates configured pools and limits plus a deterministic retail FSM sequence (`enter -> message -> exit -> enter -> update/quit`) before all existing subsystem probes. |
| RageSuiteCreature retail frontier | DONE | All 153 entries from `RageSuiteCreature_2015.vcxproj` compile into `rage_ositok_ragesuitecreature_full_archive`, link lazily and pass the combined whole-archive audit with Framework and Script. This closes Framework's suite-level animation/behavior dependencies without substituting NaturalMotion or game stubs. Runtime validation currently remains at the existing base Creature animation/IK probe; retail SuiteCreature asset fixtures are still pending. |
| RageScript retail frontier | DONE | All seven entries from `RageScript_2015.vcxproj` compile into `rage_ositok_ragescript_full_archive`, link lazily and pass the combined whole-archive audit. The MSVC-vtable anti-cheat capture is disabled only for Osito because GCC/SysV does not expose that ABI. Native registration, game `.ysc` mounting and representative script execution remain the next runtime boundary. |
| RageMisc Framework closure | DONE | Framework's required RageMisc owners are explicit manifest-derived archives: glass 14, fragment 18, softrasterizer 7, event/cloth 15, RMPTFX 50 and grrope 1; Physics retains the canonical `ptxrandomtable.cpp` owner. Every TU compiles, the combined whole-link audit passes, and production returns to lazy archive extraction. Osito follows the existing Win32/Orbis CPU path for the three softrasterizer fragments, defining `boxoccluder_frag`, `boxselector_frag` and `modelselector_frag` in the canonical `boxoccluder.cpp` owner rather than pretending to generate SPU binaries. The shared pthread scheduler can now execute its queued jobs asynchronously. |
| game4 retail frontier | DONE | All 263 entries from `game4_lib_2015.vcxproj` compile into `gtav_ositok_game4_full_archive`. GCC portability fixes cover Windows include separators, explicit PCH dependencies, Win32-only chat, D3D11 depth/DOF/particle paths and strict pointer tests. The duplicate generated `VehiclePopulationTuning` parser owner was removed from `vehiclepopulation.cpp` in favor of its dedicated metadata TU. The archive passes a combined whole-audit with every completed RAGE archive, links lazily into `gtav-app-shell`, and passes the complete Venus regression. The lightweight app-shell owner of `SmokeTests::PARAM_smoketest` remains until `game1`-`game3` provide the transitive gameplay closure required by retail `tools/SmokeTest.cpp`. |
| GPU backend | PARTIAL | Real D3D11 to DXVK to Vulkan/Venus indexed draw with RAGE vertex declarations and buffers; canonical `grcStateBlock` stores drive observable culling, depth, blending, and deduplicated sampler ownership; public `grcEffect::Create`/private `Init` resolves the synthetic profile plus unmodified retail `rage_bink.fxc` and `scaleform_shaders.fxc`, including all nine `sfTech*` switches and real `b1`/`b2`/`b11` bindings; retained VS DXBC, declaration/program input-layout caching and dynamic `BeginVertices`/`EndVertices` execute solid and texture readbacks; all 191 C++ units from the retail `ScaleformGfx_2015.vcxproj` compile into one full archive and replace the former selected SDK archive in the app link; original release-378 `scaleform/renderer.cpp` uses that SDK and an aligned Osito `GMemoryHeap`; `sfRendererBase::CreateRenderer()` completes constructor/destructor plus `BeginFrame`/`BeginDisplay`/`DrawIndexedTriList`/`DrawBitmaps`/`EndDisplay`/`EndFrame`; the Osito texture factory creates DXVK-backed RGBA, A8 and DXT1/3/5 textures with SRVs, mip upload and partial updates; original `GFxResourceLib`, `GFxImageResource`, `GImageInfo`, `GThreads` and `GTimer` provide the higher-level image cache over Osito pthread mutex, condition and joinable `GThread` implementations; Venus verifies resolver/waiter synchronization, available hits, qualified-key separation, cancellation, weak eviction, `gthread=1` and cached texture byte 32, plus `CreateTextureFromImage`/`FillStyleBitmap` at `32,192,96,255`, dynamic A8 font-cache data 137, RGB conversion, DXT1 upload and `sfTechAlphaSprite` blending at `122,110,87,192`; global defaults commit only after exact profile, connection and instance validation; full movie binding, `CScaleformStore` TXD dependencies, dynamic `img://` resources, additional retail layouts/formats, MSAA, and WSI remain pending |
| Audio backend | PARTIAL | All 289 compile entries from `RageAudio_2015.vcxproj` build with `AUD_IMPL`, replacing only XAudio2 with `audMixerDeviceOsitoK`; the full archive supplies retail `waveslot.cpp` to `gtav-app-shell` and passes the complete Venus regression. The current 5.1 backend is a headless sink that processes triggered mixer frames but has no timed worker or HDA PCM output. The RageGraphics archive now owns and validates the debug-draw/immediate symbols reached by `effectmonitor.cpp`. |
| Input | STUB | Kernel has xHCI+evdev, no RAGE bridge |
| Streaming (pgStreamer) | STUB | Thread creation works, real streaming pending |
| Script VM (.ysc) | PARTIAL | The complete seven-TU RageScript retail library compiles, links and passes whole-audit; game `.ysc` mounting, native registration and execution are pending. |
| Save/Load | NOT STARTED | Needs OsitoFS file write |
| Network transport/Social | PARTIAL | RageNet transport and wolfSSL are real and validated offline; native Rockstar Social Club login/UI/entitlements remain unavailable outside Win32. |

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
| `GTAV Source/CMakeLists.txt` | Manifest-derived retail frontiers, archive ownership and production app-shell link |
| `GTAV Source/ports/ositok/game/core/app_shell_ositok.cpp` | Deterministic subsystem and Framework runtime probes |
| `GTAV Source/ports/ositok/forceinclude/ositok_original_platform.h` | Osito platform identity and ABI feature selection |
| `GTAV Source/ports/ositok/rage/` | VFS, networking, graphics and other Osito platform owners |
| `GTAV Source/ports/ositok/rage/system/task_ositok.cpp` | Asynchronous RAGE task scheduler and handle lifecycle |
| `osito-k/arch/x86/fs/ositofs2.c` | Guest OsitoFS implementation used by GTA mounts |
| `osito-k/arch/x86/scripts/test-venus-user.sh` | Automated Venus/DXVK/app-shell regression |

## Next Steps

1. Import and close the release-378 `game1`-`game3` manifests, then retire the lightweight `PARAM(smoketest)` owner and activate the complete retail SmokeTest closure.
2. Integrate the completed engine archives into the full `CGame::Init` path and revalidate allocator bring-up.
3. Mount scripts from `script.rpf`, register game natives and execute a representative `.ysc`.
4. Extend solver coverage to a moving-body contact and isolate the memory-only script-variable RageSec verifier without enabling Win32/PAPI or endpoint-dependent reactions.
5. Connect `GFxLoaderImpl` movie binding to `CScaleformStore` TXD dependencies and dynamic `img://` resources, then WSI/presentation.
6. Bridge the HDA driver to the RAGE audio mixer and xHCI/evdev to RAGE input.

## Build Commands

```bash
# Production app-shell build
cmake --build /root/gtav-dxvk-smoke --target gtav_ositok_app_shell -j4

# Deploy to NVMe
tools/ositofs/ositofs-write /root/osito/nvme_gcc.img \
    /root/gtav-dxvk-smoke/gtav-app-shell --name gtav --overwrite

# Run the validated Venus regression
QEMU_BIN=/root/qemu-venus-build/qemu-system-x86_64 \
QEMU_NVME_IMG=/root/osito/nvme_gcc.img \
QEMU_BOOT_TIMEOUT=300 \
QEMU_TEST_KEYS='e x e c spc g t a v ret' \
QEMU_PASS_PATTERN='GTAV-OSITOK-APP-SHELL: OK' \
arch/x86/scripts/test-venus-user.sh
```

## QEMU Configuration

The currently validated Venus baseline is intentionally limited to 4 GiB,
4 vCPUs, 512 MiB of virtio-gpu host memory, and TCG. Larger RAM/SMP and KVM
configurations currently break NVMe/OsitoFS, while WSL Vulkan exposes only
llvmpipe rather than the RTX 3090. See `docs/dev-environment.md`, section
"Current WSL Venus resource limits", for the measured matrix and the criteria
for treating memory or GPU availability as a rendering blocker.

```
-m 4G -smp 4 -device virtio-vga -device e1000e -device qemu-xhci
-device usb-kbd -device usb-mouse -device intel-hda -device nvme
```
