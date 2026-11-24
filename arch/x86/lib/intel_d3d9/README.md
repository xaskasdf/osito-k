# Native Intel Direct3D 9 driver

`osito-intel9.dll` implements the user-mode **driver** DDI. Applications still
need an independently installed original Microsoft Direct3D runtime. The DLL
exports `OpenAdapter` and imports Windows system/CRT services. It does not
export Direct3D factory functions or get installed as `d3d9.dll`.

The current interface is the WDDM 1.0 D3D9 DDI (`DriverVersion=0x000c`).
Build it against local SDK, MSVC and WDK headers; none are copied into the OS
or the repository. First build `compiler.mk` for each PE architecture with
`umd api-test` targets, the MinGW paths described in the compiler documentation,
and `SDK_INCLUDE`, `MSVC_INCLUDE`, `WDK_INCLUDE` variables. Then package the
DLLs and build the diagnostic clients. From the repository root, in WSL:

```sh
bash arch/x86/scripts/build-native-umd.sh BUILD_OUTPUT SDK_INCLUDE MSVC_INCLUDE WDK_UM_INCLUDE COMPILER_PE32_OUTPUT COMPILER_PE64_OUTPUT
```

`WDK_UM_INCLUDE` must contain `d3dumddi.h`. The tested reference is the header
from Microsoft.Windows.WDK.x64 10.0.26100.6584, with SDK 10.0.26100.0 and MSVC
14.44.35207 headers. Outputs are separate PE32/PE64 driver DLLs and diagnostic
clients. The normal kernel build does not embed these files.

The implemented subset creates driver devices and native contexts, creates
A8R8G8B8/X8R8G8B8 video-memory surfaces and explicit 2D mip chains, locks
whole levels or bounded areas, copies equal-size rectangles through `pfnBlt`,
and destroys the associated objects. A texture owns one page-rounded Gen9
allocation containing all levels. Concurrent mip locks share one synchronized
read/write CPU mapping until the final Unlock; GPU submissions are rejected
while any level owns that mapping. Callback failures preserve ownership for
retry. Copies include each mip's native origin and retain whole allocations.
It also creates vertex buffers and explicit render
targets, binds float4 POSITIONT declarations, stream zero and pixel shaders,
and submits triangle lists, strips and fans through `DrawPrimitive` and
`DrawPrimitive2`. Viewport and scissor determine the clipped draw rectangle.
The new single-target blend path captures source/destination factors, color
and separate-alpha operations, constant blend color and RGBA write masks in
the draw command. Gen9 performs the pixel blending; the CPU only validates and
packs state. ARGB and XRGB targets have distinct native formats when blending,
so XRGB destination alpha is one regardless of its unused stored byte. This
path passes physical P1 pixel comparisons against the independent Microsoft
D3D9 HAL oracle. Depth, alpha testing, application texture sampling, vertex
programs and additional layouts remain open;
unsupported state calls return an error. Commands and allocations use runtime callbacks; the
driver never accepts user GPU addresses or executes kernel helpers directly.
The KMD owns validated private commands, memory, GPU execution and completion.

Adapter-private information must match the native Intel copy ABI before open
succeeds. Unsupported versions, layouts and operations are rejected. Object
identities do not wrap or become valid again when their slots are reused.
Callbacks run outside the metadata lock; reentrant operations encounter a busy
device. Failed resource/context callbacks leave handles available for retry.

`gpu-native on` selects the BCS copy engine; `gpu-native render` selects native
RCS textured copies using a fixed driver-owned shader. The private capability
and node must match exactly. Stop the driver before changing engines. Both use
the same DDI `Blt` and KMT ownership path; RCS additionally owns a full render
save area and a private batch/shader mapping per hardware slot. Closed client
contexts can reuse a slot after completion and acknowledged TLB invalidation.
This does not yet schedule both engines simultaneously. The fixed RCS copy
program is independent of shader selection. UMD drawing uses the selected
compiler program's owned KMT allocation and the public three-resource draw
command. Drawing is available only on the render node.

`GetCaps` currently returns `D3DERR_NOTAVAILABLE`, and unsupported entries in
the device table are null. This DLL is therefore a diagnostic driver subset,
not an advertised Direct3D render device. General capabilities, rendering,
broader shader support, presentation, resource layouts and full threading support
must be implemented before publishing its UMD path/render capability in KMT.

Packed texture storage is qualified in `arch/x86/build/native-packed-texture`:
14 QEMU cases plus PE32/PE64 physical suites on RCS and BCS. Five full mip
copies and one partial cross-level copy per ABI/engine verify complete source
and destination allocations, including padding. The suite also exercises
shared mapping, mixed read/write locks, reordered unlocks, renamed allocation
handles (model), callback failures and orphan cleanup. Existing draw, constant,
clear and blend suites pass with the same DLLs. This is diagnostic DDI/KMT
qualification; original-runtime device creation and Braid remain pending.

`shader.c` is the bounded SM2/SM3 bytecode reader for the native shader
compiler. It decodes operands, predicates, declarations and immediate constants
without importing SDK headers or executing shader instructions. It is currently
used by the compiler behind the UMD's pixel shader creation callback.
Syntax acceptance does not imply semantic validity or hardware support. See
[shader compiler input and validation](../../../../docs/native-d3d9-shaders.md)
for the exact boundary, reproduction commands and remaining compiler work.

`nir_frontend.c` now lowers the initial straight-line pixel arithmetic subset
to NIR. `compiler.mk` builds it with the native Intel compiler from Mesa 25.0.0
and the matching NIR already in the tree. The Intel subset is kept under
`mesa-intel/`, with per-file hashes, license text and recorded local portability
changes. This compiler emits
Intel machine instructions; it does not render via OpenGL/Vulkan or interpret
shader programs on the CPU. The build/probe runs on Linux and both Windows/
Osito-K PE ABIs. Each P1 ABI passes 34 supported compiler fixtures with output
identical to Windows, plus 20 explicit rejections, after the CRT prerequisite
fixes. `PLATFORM=win32` and `PLATFORM=win64` select the PE compiler targets.
`compiler_api.c` embeds that compiler in the PE driver through a versioned C
interface. Each device lazily creates its own compiler context. Successful
programs own their bytecode, Intel instructions and dispatch metadata, with no
Mesa pointers in the public result; they survive destruction of the compiler.
The metadata retains Gen9's dummy layer input for header-only pixel shaders.
The driver implements `CreatePixelShader`, `SetPixelShader` and
`DeletePixelShader`, rejects foreign/stale handles, and releases outstanding
programs at device teardown. Failed compilation preserves the caller's handle.
Selection now uploads each program once into a device-owned KMT allocation
through the runtime's Allocate/Lock/Unlock callbacks. The packed header and
bytecode precede a page-aligned instruction region; unused bytes are cleared.
Repeated selection reuses that storage. Failed callbacks preserve ownership
for retry, including a pending unlock, and deletion frees the allocation before
discarding the shader handle. Device teardown releases shader allocations
before its context; a failed context destruction permits reupload on reuse.
Selection itself does not submit work. The Draw callbacks reference that
uploaded program alongside the bound vertex buffer and render target.
External constants, vertex inputs/shaders, textures and control flow remain
required for broader application shaders. Native
code upload and dispatch state are qualified through the internal RCS
diagnostic for 34 authored fixtures in SIMD8, SIMD16 and combined mode, with
102 successful GPU draws. This does not advertise a runtime rendering device.

The host `compiler.mk export` target produces `compiler-export`, which writes
an owned compiler program from authored bytecode for that diagnostic. Programs
and fixture files stay outside the kernel image. The diagnostic file reader
checks metadata and trusts compiler-validated instructions; it is not a public
KMT interface for arbitrary shader binaries. See the compiler document for
test commands, physical results and the remaining driver integration.

The internal Gen9 builder also consumes a separate vertex allocation for
screen-space triangle lists, strips and fans, with D3D9 integer pixel centers
and scissor. Its physical diagnostic passes 96 draws against an independent
coverage oracle. `DrawPrimitive`/`DrawPrimitive2` now submit through KMT to
that native primitive pipeline. The
diagnostic does not install Microsoft files or transform/rasterize application
vertices on the CPU.

The native queue now snapshots primitive descriptors and program metadata and
retains per-job resource ownership through completion or confirmed engine
retirement. Render contexts also support a separate read-only shader mapping
alongside vertices and target. Public KMT capture now connects to this queue
through the private `INTEL_UMD_DRAW32` command. UMD Draw/state callbacks now
connect runtime resources and shader selection to that command. `VStart` is
converted from vertices to bytes; `FirstVertexOffset` is already byte-based.
Both include stream and declaration offsets with checked bounds. The driver
uses replacement staging buffers returned by Render and preserves state on a
failed callback. Destroying bound resources or declarations clears their
bindings; failed destruction retains them. A null Lock mapping is retired,
and failed retirement can be retried by Lock or resource destruction.

That geometry diagnostic now uses the shared three-resource context, including
activation after engine startup and 95 acknowledged replacements of shader,
vertices and target between 96 draws on the P1. All 92,556 expected pixels pass,
inactive targets retain their sentinel, and all 143 pages are freed after engine
retirement. This physically qualifies the shared mapping/lifetime path with
trusted compiler fixtures. At the application boundary, the kernel captures
three owned allocations and validates the native instructions under exclusive
CPU ownership. It validates them again after acquiring GPU ownership and
excludes CPU mappings until retirement. The Gen9 verifier accepts the current
compiler's constant-output pixel programs: initialized float payloads and one
headerless BTI0 render-target write ending the thread. Other instruction and
message forms are rejected. This is a bounded initial contract, not a general
shader validator; compiler features require corresponding validation support.

`umd*-model.exe` exercises the DLL with diagnostic callbacks and modeled
memory (the `rcs-model` variant also tests render-node negotiation).
`umd*-physical.exe` supplies those callbacks by translating to public
Osito-K KMT calls and requires the enabled native prototype adapter on the P1.
Neither client is the Microsoft runtime; passing them does not establish that
an original runtime can create a device or that Braid renders. The DLL copies
metadata and compiled programs into owned storage; the physical surface copy
uses the native GPU.

`compiler-api-test.exe` exercises 34 positive and 20 rejected fixtures over
three compiler lifetimes, retaining programs after their input and compiler
are gone. The UMD clients additionally check 140 live shader handles, device
isolation, selection/deletion, exact uploaded bytes and padding, callback
failure retries and balanced allocation/mapping ownership.

`draw*-model.exe` checks Draw packets, resource lifetime and callback retries
without rendering. `draw*-physical.exe` uses KMT to verify GPU pixels for
triangle lists, strips and fans with viewport/scissor and alternating resource
banks. Both ABIs pass on the P1: 12 draws / 6,144 pixels in total. The same DLLs
pass eight Windows/QEMU cases and the existing physical copy/storage suites.
Evidence is in `arch/x86/build/native-umd-draw`; original-runtime device
creation and application performance are still unverified.

The driver boundary follows Microsoft's
[OpenAdapter contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddiarg_openadapter)
and [D3D9 driver functions](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/direct3d-functions-implemented-by-user-mode).
Pixel shader creation follows the
[driver callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_createpixelshader)
and its [byte-sized input length](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddiarg_createpixelshader).
