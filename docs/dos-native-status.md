# OsitoK DOS Native Execution — Status & Architecture

Status as of 2026-04-24, after a 12-commit session on the `experiment` branch
(b967964 → fc00883). Documents what works, what's blocked, and the design.

> Historical snapshot, not the current compatibility contract. The synthetic
> selectors and binary-specific workarounds described below have since been
> removed. See the September [VGA](win32-layer-correctness.md#436-dos4gw-vga-memory-and-native-mmio)
> and [keyboard IRQ1](win32-layer-correctness.md#437-dos4gw-keyboard-ownership-and-irq1)
> tests for current measured behavior and generic fixes. The
> [audio follow-up](win32-layer-correctness.md#438-dos-sound-dma-request-ordering-and-remaining-doom-audio-blocker)
> records the DMA investigation. The later
> [software CPU and long-run checks](win32-layer-correctness.md#441-unbounded-dos-sessions-and-explicit-interpreter-step-quotas)
> establish DOOM graphics, keyboard input, digital sound and a clean exit
> past one billion instructions using `--emulate`. The
> [ICEBP/EXEC comparison](win32-layer-correctness.md#442-icebp-return-ip-contract-and-exec-regression-normalization)
> separates the correct DPMI return contract from the current KVM/WSL native
> mismatch. The [string port-I/O follow-up](win32-layer-correctness.md#443-dos-string-port-io-and-restartable-native-mediation)
> adds bounded REP INS/OUTS, segment-fault restart and native mediation,
> with dedicated fixtures and another DOOM graphics/audio/exit check.
> The [default exception follow-up](win32-layer-correctness.md#444-dpmi-default-exception-vectors-and-dos4gw-shutdown)
> fixes null 0202h vectors and the DOS4GW shutdown cascade, with chainable
> defaults, controlled fatal exits and software/native regression fixtures.
> The [native exception bridge](win32-layer-correctness.md#445-native-dpmi-exception-delivery)
> adds CPU-to-DPMI delivery and SS16/SS32 fault/trap fixtures. Mixed tests also
> exposed and fixed [PE32 SEH fault-stack abandonment](win32-layer-correctness.md#446-pe32-seh-abandonment-must-restore-ist3).
> The [resident exception-stack follow-up](win32-layer-correctness.md#447-resident-dpmi-exception-stack)
> adds bounded, editable frames and native recovery from an exhausted SS:ESP.
> The [SS16 return fix](win32-layer-correctness.md#448-preserve-full-esp-on-native-ss16-returns)
> preserves full ESP across native interrupts and mixed DOS/PE32 sessions.
> The [shared locked-stack follow-up](win32-layer-correctness.md#449-shared-dpmi-locked-stack-ownership)
> unifies protected IRQ, special software interrupt, callback and exception
> stacks across ordinary mode simulations, with before/after fixtures and
> DOOM software/native exit checks. The
> [dormant-mode state follow-up](win32-layer-correctness.md#450-dpmi-dormant-mode-state-and-real-mode-timer-reflection)
> implements 0305h stack-state save/restore, nested real stacks, real-mode
> INT 1Ch reflection and raw-switch virtual IF preservation, with USE16/USE32
> fixtures and DOOM checks. The
> [transactional return follow-up](win32-layer-correctness.md#451-transactional-dpmi-exception-and-locked-interrupt-returns)
> validates exception and locked-interrupt return segments before committing
> CPU state, with edited-CS/SS fixtures and DOOM exit checks. The
> [record-buffer follow-up](win32-layer-correctness.md#452-checked-dpmi-records-and-alternate-callback-buffers)
> validates fixed-size host records across guest pages, supports alternate
> read-only callback return buffers and revalidates nested-call output.
> The [protected JIT follow-up](win32-layer-correctness.md#453-protected-1632-bit-jit-blocks-and-dispatch-accounting)
> adds bounded USE16/USE32 register blocks, full-EIP cache keys, source
> validation and shared instruction/IRQ accounting under `--emulate`, with
> DOOM graphics/audio/exit checks. It does not fix native PUSHFD visibility;
> the native FLAGS probe still exits 71h. The
> [cache follow-up](win32-layer-correctness.md#454-bounded-jit-cache-reuse-and-interpreter-backoff)
> replaces pressure-driven full resets with indexed LRU eviction and reusable
> native-code ranges, and backs off unsupported translation attempts. It
> includes cache-pressure contracts and a fixed-demo before/after comparison.
> The [page-walker follow-up](win32-layer-correctness.md#455-shared-guest-page-walker-and-checked-operand-faults)
> shares permission-aware 4 KiB translation between bounded DPMI records and
> checked CPU operands, with 5,654 new paging/operand checks and another
> DOOM graphics/audio/exit regression. The
> [fetch and descriptor follow-up](win32-layer-correctness.md#456-checked-instruction-fetch-and-paged-guest-descriptors)
> adds checked instruction bytes, explicit fetch-fault exits, supervisor
> GDT/LDT reads and accessed-bit writes, with 12,262 fetch/descriptor/VGA
> checks. The
> [dormant paging follow-up](win32-layer-correctness.md#457-dormant-paging-context-across-dpmi-re-entry)
> preserves CR0/CR3 across raw switches, callbacks and real-mode handler
> re-entry, including 0305h save/restore and nested simulations. It adds
> 328 assertions and another DOOM graphics/audio/exit check. The
> [ModRM follow-up](win32-layer-correctness.md#458-checked-modrm-operands-and-multi-field-memory-records)
> adds full-range scalar, moffs, XLAT and memory-record checks, preflighted
> read-modify-write accesses and transactional POP r/m operands. Its 8,748
> new checks and DOOM software/native exit runs pass. The
> [memory-string follow-up](win32-layer-correctness.md#459-checked-string-memory-and-restartable-rep)
> adds full-range MOVS/CMPS/STOS/LODS/SCAS checks, bounded REP with the real
> remaining count and software comparison-FLAGS restoration across faults.
> It passes 7,862 new checks, native/emulated VGA strings and DOOM exit runs.
> The [data-stack follow-up](win32-layer-correctness.md#460-checked-data-stack-and-restartable-enter)
> adds checked scalar pushes/pops, PUSHA/POPA and restartable ENTER/LEAVE,
> plus preservation of the ESP high word across 16-bit DPMI exceptions.
> Its 11,870 new checks include 1,352 handler returns and retries; DOOM
> software/native exits and subsequent PE32 SEH regressions pass.
> The [segment-load follow-up](win32-layer-correctness.md#461-checked-segment-loads-and-irq-shadow)
> validates MOV/POP segment loads and adds the modeled maskable-IRQ delay
> after MOV/POP SS and STI, including REP and JIT boundaries. Its 38,334
> checks include 3,440 handler returns and retries and 72 native IRQ-frame
> imports that reject stale interpreter continuations before PIC delivery.
> DOOM native/emulated exits, digital PCM output and subsequent PE32 SEH
> regressions pass; the native FLAGS probe still exits 71h.
> The [near-transfer follow-up](win32-layer-correctness.md#462-checked-near-control-transfers)
> validates same-CS CALL/RET/JMP/Jcc/LOOP before committing CPU state,
> preserves the original stack and LOOP count across target faults, and
> separates branch completion from a later destination-fetch page fault.
> Its 14,116 new checks include 3,648 repaired-instruction retries; the
> complete API suite, DOOM native/emulated exits, digital PCM output and
> subsequent PE32 SEH tests pass. Native FLAGS still exit 71h.
> The [far-transfer follow-up](win32-layer-correctness.md#463-checked-far-calls-jumps-and-returns)
> adds checked far CALL/JMP/RETF and 16/32-bit call gates, including inward
> stack changes, parameter copying, destination-CPL page accesses and outer
> returns. Its 6,176 new cases include 184 repaired page-fault retries and
> 768 actual gate returns. The complete API suite, DOOM native/emulated
> exits, digital PCM output and subsequent PE32 SEH tests pass. Native
> FLAGS still exit 71h. Task switching, IRET/interrupt stacks, hidden
> descriptors, native interruptibility and NMI/debug suppression remain open.
> The [system-descriptor follow-up](win32-layer-correctness.md#464-cached-ldtrtr-and-checked-system-selector-loads)
> adds cached LDTR/TR, checked LLDT/LTR admission and supervisor busy-bit
> writes, plus VCPI context preservation and explicit DPMI LDT ownership.
> Its 27,600 new cases pass. The 102 repaired CPL0 instructions restart
> from checked snapshots; DPMI correctly rejects returning a client to
> ring zero. That is not a completed guest IDT/IRET return. The six ordinary
> segment caches, task/interrupt returns and native FLAGS remain open;
> the complete API suite, DOOM native/emulated exits with digital PCM and
> subsequent PE32 SEH regressions pass. The linked section records evidence
> and limitations.
> The [mode-control follow-up](win32-layer-correctness.md#465-privileged-mode-control-and-table-register-instructions)
> adds checked CR/table-register operations, correct LMSW bit handling,
> non-accessing INVLPG and privilege checks that also recognize VCPI's
> real-addressed v86 state. Its 16,192 new cases, the complete API suite,
> DOOM native/emulated exits with digital PCM and subsequent PE32 probes
> pass. Native FLAGS still exit 71h; ordinary segment caches, guest
> IDT/IRET, general VM86 execution and additional privileged instructions
> remain open.
> The [loaded-CS follow-up](win32-layer-correctness.md#466-loaded-cs-cache-and-page-access-privilege)
> adds a loaded CS cache and explicit CPL for fetch, CS-relative reads,
> JIT validity and page-access privilege across PE transitions. Its 84
> new scenarios, the complete API suite, DOOM native/emulated exits with
> digital PCM and subsequent PE32 probes pass. SS/DS/ES/FS/GS still need
> independent loaded caches; native FLAGS still exit 71h.
> The [loaded-data follow-up](win32-layer-correctness.md#467-loaded-data-and-stack-segment-caches)
> adds independent SS/DS/ES/FS/GS caches, checked load commits, cached SS.B
> and outer-return data validation, plus real/VM86 reload semantics. Its
> 382 new cases and 768 expanded gate returns pass with the complete API
> suite, DOOM native/emulated exits, digital PCM and subsequent PE32 probes.
> Earlier fixture failures and their corrections are recorded explicitly.
> Native FLAGS still exit 71h. General guest paging, guest IDT/IRET, task
> switching, full VM86 execution, physical hidden state, other return paths, native
> FLAGS, x87 vector ownership, music and performance remain open; this is not complete
> DOS compatibility.
> The [interrupt-return follow-up](win32-layer-correctness.md#468-checked-architectural-interrupt-returns)
> adds checked IRET/IRETD frames, old-CPL FLAGS permissions, outer stack
> returns and VM86 return-frame loading. Its 986 new cases include 24 real
> DPMI page-fault returns and 16 explicitly separate CPL0 snapshot retries.
> The complete API suite, native/emulated DOOM exits, digital PCM output
> and subsequent PE32 probes pass. Nested-task returns stop with an explicit
> unsupported diagnostic; guest IDT delivery, general VM86 execution,
> task switching and native FLAGS remain open.
> The [guest-IDT follow-up](win32-layer-correctness.md#469-checked-guest-owned-idt-delivery)
> adds checked interrupt/trap gates, inward stacks, guest VM86 frames,
> nested delivery faults and client-local triple-fault stops, with explicit
> VCPI event ownership and JIT INT routing. Its 2,326 new cases, the complete
> API suite, native/emulated DOOM exits, digital PCM and subsequent PE32
> probes pass. Task switching, full VM86, native FLAGS and additional
> interruptibility rules remain open; this is not complete DOS compatibility.
> The [hardware-task follow-up](win32-layer-correctness.md#470-checked-1632-bit-hardware-task-switches)
> adds 16/32-bit TSS CALL/JMP, task gates and nested-task IRET, with checked
> pre/post-commit faults, incoming CR3/LDTR, VM86 tasks and separate task
> debug traps. Its 1,154 new checks, complete API suite, native/emulated
> DOOM exits, digital PCM and subsequent PE32 probes pass. Native FLAGS
> remains a known 71h result, not a passing 2Ah result. General VM86,
> interruptibility, x87, music and performance also remain open.
> The [FLAGS follow-up](win32-layer-correctness.md#471-checked-flags-privileges-and-dpmi-interrupt-ownership)
> adds checked PUSHF/POPF and CLI/STI privileges, VM86/LOCK admission and
> explicit guest versus host interrupt ownership. Its 2,623 new checks,
> complete API suite, 15 independent probes, native/emulated DOOM exits,
> emulated digital PCM and subsequent PE32 probes pass. Native FLAGS
> remains the separate legacy-profile gap; general debug interruptibility,
> VME/PVI, real-mode IDTR, x87, music and performance remain open.
> The [real-mode IDTR follow-up](win32-layer-correctness.md#472-checked-real-mode-interrupt-entry-and-rom-ownership)
> shares checked real interrupt frames and current-IDTR routing between
> interpreter and JIT, validates ROM service ownership and preserves
> DPMI's installed PM timer precedence. Its 610 new checks, complete API
> suite, 15 independent probes, native/emulated DOOM exits, digital PCM
> and subsequent PE32 probes pass. The executable timer regression and
> corrected fixtures are recorded explicitly. Instruction-level INT 23h/
> 24h reflection, general debug interruptibility, native FLAGS and the
> other remaining gaps are not claimed complete.
> The [DOS control reflection follow-up](win32-layer-correctness.md#473-checked-real-mode-ctrlc-and-critical-error-reflection)
> connects real INT 23h/24h to protected handlers, checks the mixed critical
> frame against loaded SS and both address spaces, and fixes 0300h control
> frames and interpreted argument offsets. Its 284 new checks, complete
> API suite, six extended control probes, fifteen independent probes,
> native/emulated DOOM exits, digital PCM and post-DOS PE32 checks pass.
> Arbitrary-length paged argument copying, nested raw hidden-state coverage,
> general debug interruptibility and the native FLAGS profile remain open.

> The [real-call argument follow-up](win32-layer-correctness.md#474-checked-arbitrary-length-dpmi-real-call-arguments)
> validates arbitrary-length 0300h/0301h/0302h arguments across both address
> spaces, snapshots physical overlaps and checks real return-stack balance.
> Its 324 new internal checks, complete API suite, three argument binaries,
> six control probes, fifteen independent probes, native/emulated DOOM
> exits, digital PCM and post-DOS PE32 checks pass. Native FLAGS still
> exits 71h; the linked section records remaining scope and artifacts.

> The [protected DOS buffer follow-up](win32-layer-correctness.md#475-checked-protected-mode-dos-file-and-console-buffers)
> replaces legacy file/console translation with checked page spans and
> overlap snapshots, revalidates output after real handlers and handles
> string/zero-line boundaries. Its 786 new checks, complete API suite,
> six file/console probes, argument/control regressions, native/emulated
> DOOM exits, digital PCM and post-DOS PE32 checks pass. Real-mode buffers,
> legacy descriptor records, native FLAGS and other documented gaps remain.

> The [descriptor/state record follow-up](win32-layer-correctness.md#476-checked-dpmi-descriptor-and-real-state-records)
> replaces legacy 000Bh/000Ch and real 0305h copies with checked fixed
> records. Its 1,689 new checks, complete DPMI selftest, extended callback
> binaries, file/console/argument/control regressions, native/emulated
> DOOM exits and post-DOS PE32 probes pass. `dos-dpmi-test` now runs that
> focused suite. The broader DOS API suite was not rerun on this kernel.
> Pointer-fault reflection, nested hidden state, general VM86, native
> FLAGS and the other linked gaps are still open.

> The [paged host-frame follow-up](win32-layer-correctness.md#477-checked-paged-dpmi-exception-and-locked-interrupt-frames)
> adds checked exception/locked-interrupt frames and returns, loaded-SS
> lifetime rules and an atomic private interpreter INT frame. Its 336 new
> checks, 812-check focused suite, complete DOS API suite, independent
> probes, native/emulated DOOM exits, digital PCM and post-DOS PE32 probes
> pass. `dos-dpmi-stack-test` runs the focused suite. Callback/timer legacy
> pushes, host buffer-fault continuations, native FLAGS and the linked
> remaining gaps are still open; this is not complete DOS compatibility.

> The [callback/control entry follow-up](win32-layer-correctness.md#478-paged-callback-and-reflected-real-mode-entry-frames)
> replaces legacy callback/timer pushes with prepared paged frames and
> keeps Ctrl+C/critical-error frames on the admitted SS snapshot. Callback
> outputs commit both records' paging metadata before payload, including
> circular aliases; real input and read-only return records are checked.
> Its 2,010 new checks, complete DOS API suite, independent and extended
> probes, native/emulated DOOM exits, digital PCM and post-DOS PE32 checks
> pass. Ordinary PM interrupt pushes, buffer-fault continuations, native
> FLAGS and the linked remaining gaps are still open.

> The [ordinary software-entry follow-up](win32-layer-correctness.md#479-checked-ordinary-dpmi-software-entry-and-native-continuation)
> validates code and the complete loaded-SS frame before handler entry.
> Native DPL faults retain the original prefixed INT address; completed
> native gates instead retain an exception-depth-owned entry continuation,
> resumed only after an unchanged CS:EIP return. Its 560 new checks include
> 32 repaired returns, and four independent emulated/native COM fixtures
> verify plain and 15-byte INT stack-fault repair with exactly-once handlers.
> All four print PASS and exit 2Ah. The complete DOS API suite, independent
> and extended probes, native/emulated DOOM exits, digital PCM and post-DOS
> PE32 checks pass. Host-service buffer continuations, native FLAGS and the
> linked remaining gaps are still open.

> The [real-call buffer follow-up](win32-layer-correctness.md#480-restartable-dpmi-real-call-register-buffers)
> adds typed faults and retained-result continuations for 0300h/0301h/0302h.
> A repaired buffer retries only access, not completed real-mode side effects;
> matched exception returns, edited destinations and termination are distinct.
> Its 6,476 buffer checks and 32 EXEC snapshot checks pass, along with six
> independent COM runs, the complete API suite, existing probes, emulated
> EXEC, native/emulated DOOM exits, digital PCM and post-DOS PE32 checks.
> Native EXEC still fails at the previously documented KVM ICEBP mismatch;
> native FLAGS and the separately reproduced host ES-restore fault remain
> open. General service buffers and the other linked gaps are not complete.

> The [checked native restoration follow-up](win32-layer-correctness.md#481-checked-native-dos-segment-restoration)
> closes that ES-restore failure by admitting all six client segments before
> hardware restoration. It preserves typed faults, edited return destinations
> and client DF, and accepts the absent descriptor types permitted by 0009h.
> Its 672-case matrix includes 48 repairs without service replay and verifies
> admission before pending IRQ delivery. All 65,536 rights cases, three native
> COMs, the complete API suite and the existing regression probes pass, as do
> native/emulated DOOM exits and post-DOS PE32 checks. Native FLAGS/ICEBP and
> native EXEC still fail as documented; the linked remaining gaps stay open.

> The [restartable file/console follow-up](win32-layer-correctness.md#482-restartable-protected-dos-file-and-console-copies)
> retains completed DOS output across protected-buffer repair without
> replaying file or input side effects. Its 13,648 checks include 928
> recoveries, nested calls, cancellation and multiple chunks. Six real-file
> recovery runs verify exactly-once reads; six updated file/console runs
> pass, along with the complete API suite, independent/native regressions,
> DOOM native/emulated exits, digital PCM and subsequent PE32 checks.
> Native FLAGS/ICEBP and native EXEC still fail as documented; the linked
> remaining gaps are still open.

> The [descriptor/info continuation follow-up](win32-layer-correctness.md#483-restartable-dpmi-descriptor-and-memory-information-records)
> extends typed buffer recovery to 000Bh, 000Ch and 0500h, retaining the
> original arguments and revalidating targets after repair. Its 7,392 checks,
> 496 recoveries, six independent COM runs, complete API suite and legacy
> regressions pass. The identical COMs fail against the previous kernel.
> Native/emulated DOOM exits, digital PCM and post-DOS PE32 checks also pass.
> Callback/state record continuations, native FLAGS/ICEBP and the linked
> remaining gaps are still open; this is not complete DOS compatibility.
> The [protected state continuation follow-up](win32-layer-correctness.md#484-restartable-protected-0305h-state-records)
> adds restartable 0305h records without losing the original save payload,
> FAR caller or handler-directed cancellation. Its 6,688 checks, 368
> recoveries, six independent COM runs, complete API suite, DOOM exits and
> post-DOS PE32 checks pass. The identical COMs fail against the preceding
> kernel. Callback continuations and real/VM86 record recovery remain
> separate, open work.
> The [callback return continuation follow-up](win32-layer-correctness.md#485-restartable-callback-return-records)
> adds restartable reads without losing the selected record, real-mode
> destination or active callback during nested handlers and cancellation.
> Its 3,664 checks, 208 recoveries and six independent COM runs pass,
> including the native callback-return gate. The full DOS API suite,
> native/emulated DOOM exits and post-DOS PE32 checks also pass; the same
> six COM runs fail as expected on the preceding kernel. Registration and
> real-mode callback entry still need separate continuation work, as do
> the known native FLAGS/ICEBP/EXEC failures.
> The [callback registration follow-up](win32-layer-correctness.md#486-restartable-callback-registration)
> separates recoverable pointer faults from allocation errors and rolls
> back partial descriptor reservations. Its 7,960 checks, 320 repairs and
> six register/invoke/free COM runs pass, together with the full DOS API
> suite, native/emulated DOOM exits and post-DOS PE32 checks. The identical
> COM files fail at the first inaccessible record on the preceding kernel.
> Real-mode callback entry still needs its own exception continuation;
> native FLAGS/ICEBP/EXEC and presentation gaps remain open.

> The [extended PM exception follow-up](win32-layer-correctness.md#487-extended-protected-mode-exception-frames)
> adds 0210h/0212h, 88-byte frames, old/new handler chaining and explicit
> host retry/cancellation. The frame matrix passes 4,744 checks and the
> expanded paged-frame matrix passes 640. All six independent COM runs
> pass, including native USE32 with either stack width, nested host calls
> and exit from a handler. The full DOS API suite, legacy regressions,
> native/emulated DOOM exits, digital PCM and post-DOS PE32 checks pass;
> native FLAGS/ICEBP/EXEC still fail as documented. The identical six COM
> invocations fail at unsupported 0212h on the preceding kernel. The full
> suite outlasted its monitor but completed in the same VM without being
> reissued; persistent artifacts retain both results and monitor errors.
> Real-mode 0211h/0213h and callback-entry
> continuation remain open; full DPMI 1.0 support is not advertised.

> The [real-mode exception follow-up](win32-layer-correctness.md#488-extended-real-mode-exception-delivery)
> adds separate 0211h/0213h vectors for the active DPMI client, locked-stack
> protected delivery, extended real returns and default IVT chaining.
> The 1,720-check real-frame matrix and all six independent COM runs pass,
> including mixed stack widths and nested real/protected exceptions.
> The checked round has 93 passes and the three known native failures;
> native/emulated DOOM exits, digital PCM and post-DOS PE32 probes pass.
> The same COM binaries fail at unsupported 0211h on the preceding kernel.
> This checkpoint reruns DPMI/stack and COM suites, not the full DOS API matrix.
> Suspended-primary-client routing across EXEC/RSP and restartable callback
> entry remain open; this does not advertise full DPMI 1.0 compatibility.

> The [callback-entry follow-up](win32-layer-correctness.md#489-restartable-real-mode-callback-entry)
> adds restartable code/record admission using real-mode host exceptions,
> single-consumption INT frames and explicit cancellation. Registration
> identities prevent a freed/reused slot from retargeting a pending entry.
> The new matrix passes 3,800 checks with 192 repairs; all six independent
> COM variants pass, including raw switches and native USE32 callbacks.
> The verified round has 99 passes and the same three native failures,
> plus normal native/emulated DOOM exits, digital PCM and post-DOS PE32
> probes. The same COM bytes fail entry on the previous kernel. This round
> reruns DPMI/stack and COM suites, not the exhaustive DOS API matrix.
> Suspended-primary-client and cross-mode cancellation ownership, native
> paging/hidden state, FLAGS/ICEBP/EXEC, debug retirement, x87, music,
> presentation and performance remain open. Full DOS/DPMI is not claimed.

> The [EXEC primary-client follow-up](win32-layer-correctness.md#490-dpmi-primary-client-lifetime-across-real-mode-exec-children)
> preserves the complete live DPMI client across real-mode EXEC descendants,
> including handler changes, callbacks and client-owned DOS allocations.
> Initial child entry is transactional. Native load-only dispatch depth and
> backend TLS/IDT ownership now survive child return. The 642 primary-client
> and 41 continuation checks pass, as do four emulated and two native COM
> variants. The same COM bytes fail on the preceding kernel. This round
> reruns the full DOS API suite and records 105 passes plus the same three
> known native failures, with normal native/emulated DOOM exits, digital
> PCM and successful post-DOS PE32 probes.
> This is not general multi-protected-client/RSP support; shared
> address-space, callback/stack and cancellation ownership remain open.

> Source-of-truth memory: `~/.claude/projects/-Users-pc-osito-k/memory/project_dos_native_transfer_apr24.md`
> Original plan: `~/.claude/plans/dos-native-transfer.md`

## What works today

DOOM.EXE (id Software, 1995) runs as a **ring-0 DPMI client on bare-metal
OsitoK** — not emulated, not interpreted, not translated. The CPU executes
DOOM's actual 16-bit/32-bit machine code inside its own LDT, with INT 21h
trapping into the kernel for DOS services.

Concretely:

| Capability | Status |
|---|---|
| MZ loader detects DOS4GW / DOS/16M signature | ✅ |
| Native LRETQ transfer to DOS code (CPL=0, LDT loaded) | ✅ |
| Real INT 21h dispatch via dedicated IDT gate (vec 0x21) on IST2 | ✅ |
| File handles bridged to OsitoFS (LSEEK, READ, OPEN, CLOSE, etc.) | ✅ |
| MCB allocator (AH=48/49/4A) with chain walking | ✅ |
| Surgical emulator for DOS4GW-quirky opcodes — see "Emulator" below | ✅ |
| Graceful crash recovery — long-jump back to shell on any unrecoverable fault | ✅ |
| DAC palette presenter (vm->mem[0xA0000+] → fb_vram) | ✅ |
| Hardware breakpoints (DR0–DR3) for kernel-side DOS debugging | ✅ |

**Quantitative milestone**: in a 90-second headless run, DOOM hits the surgical
emulator ~11 million times, completes 10 real DOS API calls in early init
(LSEEK / READ / DISK-RESET / MemAlloc / Get-Set Ctrl-Break), and reads itself
through INT 21h to find its embedded DOS extender header at offset 0xF2A4.

## What does NOT work

DOOM does **not** reach `INT 10h AH=00 AL=13h` (set VGA mode 13h). After our
synthetic-descriptor breakthrough (commit 628d04d) DOOM exits its segment-load
relocation loop, but it lands in a **semantically wrong state**: 99.9% of
subsequent INT 21h calls carry `BX = 0x5FD8` (out of range for file handles),
`AL = 6` (invalid SEEK whence), `DS:DX = 0x70:0x79F2` (wild offset). DOOM is
executing through uninitialized memory whose bytes happen to decode as `INT 21h`
sequences.

**Why**: our synth-base (`bad_sel * 16`) lets DOOM exit the descriptor-walk
loop, but the base it generates does NOT match what DOOM's relocation table
intended. Each `cmp es:[bx]` reads from the wrong memory; control flow
diverges; eventually DOOM lands in garbage.

## Correction: DOS/16M headers do not exclude DOS/4GW

The earlier identification of `BW` as evidence against DOS/4GW was incorrect.
The DOS/4GW configuration guide states that its protected-mode support is
based on Rational Systems' DOS/16M. The bound DOOM.EXE tested on 2026-09-05
also contains the explicit DOS/4GW Professional runtime banner; it is a valid
DOS/4GW fixture. A nested header must not become an excuse for synthetic
selectors or binary-specific execution rules. [Primary documentation](https://github.com/open-watcom/open-watcom-v2/blob/master/bld/redist/dos4gw/dos4gw.doc).

## Architecture: native DPMI host

```
┌─────────────────────────────────────────────────────────┐
│              OsitoK kernel (high-half)                  │
│                                                         │
│  ┌──────────────┐    ┌────────────────────────────┐     │
│  │  IDT vec 13  │←───│ dos_native_emulate_lretw() │     │
│  │  (#GP, IST1) │    │  — surgical opcode emu     │     │
│  └──────────────┘    └────────────────────────────┘     │
│  ┌──────────────┐    ┌────────────────────────────┐     │
│  │ IDT vec 0x21 │───→│ dos_int_native_dispatch()  │     │
│  │   (IST2)     │    │  — real DOS service        │     │
│  └──────────────┘    └────────────────────────────┘     │
│                                                         │
│  kernel_gdt[3]  = sel 0x18 → DOOM CS alias (DOS4GW)     │
│  kernel_gdt[4]  = sel 0x20 → DOOM DS alias (DOS4GW)     │
│  kernel_gdt[12] = LDT desc (16 bytes) → vm->dpmi.ldt    │
│  kernel_gdt[14] = scratch slot for synth descriptors    │
│                                                         │
└────────────────────────┬────────────────────────────────┘
                         │  LRETQ (CPL=0 → CPL=0, new CR3)
                         ▼
┌─────────────────────────────────────────────────────────┐
│            DOOM.EXE (DOS native context)                │
│                                                         │
│  CR3: dos_cr3   — virtual 0..16MB → vm->mem PA          │
│  LDT: vm->dpmi.ldt[256] (DOOM's segments)               │
│   - LDT[0]  CS = base 0x52B0, lim 0xFFFF (16-bit code)  │
│   - LDT[2]  DS = base 0x710,  lim 0xFFFF (16-bit data)  │
│   - LDT[E]  PSP = ...                                   │
│   - + dynamic entries from DPMI AX=0008/0009 SetDesc    │
│                                                         │
│  RIP: 16-bit DOOM code → faults (#GP / #DB) → kernel    │
│   handler runs on IST1/IST2 → emu / longjmp recovery    │
│   → IRETQ resumes DOOM at corrected RIP                 │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## Surgical emulator coverage

`arch/x86/dos/dos_int.c::dos_native_emulate_lretw()` handles DOS4GW-quirky
opcodes that the CPU rejects under x86_64's strict descriptor validation:

| Opcode  | Mnemonic         | Action |
|---------|------------------|--------|
| 0xCB    | RETF             | Pop IP:CS from stack, redirect RIP within current CS |
| 0xCA i16 | RETF imm16      | Same + release imm16 stack bytes |
| 0xCF    | IRET             | Pop IP:CS:FLAGS (6 bytes), redirect RIP |
| 0x66 0xCF | IRETD          | Pop EIP:CS:EFLAGS (12 bytes), redirect RIP |
| 0xEA    | JMP FAR direct   | Read seg:off from operand, redirect RIP |
| 0x66 0xEA | JMP FAR 32-bit | Same with 32-bit operand |
| 0xFF /5 | JMP FAR [mem]    | Indirect — read seg:off from disp16 in DS |
| 0xFF /3 | CALL FAR [mem]   | Push CS:IP, then jump (indirect) |
| 0x8E    | MOV Sreg, r/m16  | Synth scratch descriptor, load it instead |
| 0xC4    | LES r16, m16:16  | Same — load ES with synth scratch |
| 0xC5    | LDS r16, m16:16  | Same — load DS with synth scratch |
| (+ vec 1) | #DB             | Clear TF + DR6 bits, resume |

Prefixes absorbed: `0x66` (operand size), `0x67` (addr size),
`0x26/2E/36/3E/64/65` (segment overrides), `0xF0/F2/F3` (LOCK / REP).

When the popped/fetched CS doesn't resolve to any LDT entry **or** is a GDT
selector other than our installed aliases, the emulator falls back to keeping
the current CS and rewriting RIP relative to it. This handles DOS4GW's habit
of pushing stale/garbage CS values where the offset is still valid within the
active segment.

## Synth scratch descriptor (the breakthrough)

The biggest single change of the session (commit 628d04d). Earlier MOV Sreg
faults loaded a fixed safe selector (current DS) into the destination register,
but every iteration of DOOM's relocation loop dereferenced the SAME memory and
the comparisons never converged.

New strategy:

1. On `MOV Sreg / LES / LDS` fault, take the failing selector value
   (`f->error_code`).
2. Treat it as a real-mode segment number and compute `synth_base = sel * 16`.
3. Clamp to `[0, total_mem - 0x10000]` so the descriptor never points past
   `vm->mem`.
4. Write a 16-bit data descriptor into `kernel_gdt[14]` (a single rolling
   scratch slot) with that base, `limit = 0xFFFF`, `access = 0x92` (P, DPL=0,
   data writable).
5. Load `(14 << 3) = 0x70` into the target segreg via `movw`.
6. Advance RIP past the instruction; iretq resumes DOOM.

Different fault values produce different bases, so DOOM's per-iteration
`cmp es:[bx]` reads from different memory and the loops can actually exit.
Result: INT 21h call count jumped from ~10 to over a million in 90 seconds.

GDTR limit was extended from slot 13 to slot 16 in `dos_exec.c` so the new
scratch slot is reachable.

## DOS4GW GDT aliases

`dos_exec.c` patches two host GDT slots when entering native DOS mode (gated
on `vm->dos4gw_mode`):

- **GDT[3] = sel 0x18** → 16-bit code segment, base = LDT[0].base. DOOM hard-
  codes selector 0x18 in `LJMPW $0x18:$N` instructions; aliasing it to DOOM's
  own CS base makes the JMP FAR land in DOOM's code at the same linear address
  it would have reached via LDT[0].
- **GDT[4] = sel 0x20** → 16-bit data segment, base = LDT[2].base. Symmetric
  for DOS4GW's hardcoded DS reload sequence (`MOV SS, AX` with AX = 0x20).

The IDT [pf-ist] dispatch and the `dos_native_emulate_lretw()` gate both
accept faults from `frame->cs == 0x18` or `0x20` so the emulator runs even
after DOOM transitions through the raw-mode-switch boundary.

## Kernel ↔ DOOM transition

`arch/x86/dos/dos_exec.c::dos_transfer_to_native()`:

1. Allocate a dedicated CR3, identity-map `vm->mem` at virtual 0..16MB.
2. Map the `dos_vm_t` struct pages so kernel handlers can dereference `vm`
   under DOS CR3.
3. Build the long-mode LDT descriptor pointing at `vm->dpmi.ldt[]`, install
   it at `kernel_gdt[12+13]`, extend GDTR.
4. Install DOS INT IDT gates (vec 0x08, 0x10, 0x16, 0x20, 0x21, 0x2F, 0x31,
   0x33) with DPL=3 / IST=2.
5. Patch GDT[3] and GDT[4] aliases (DOS4GW only).
6. `LRETQ` to DOOM's entry point with CR3, LDT, DS/ES/SS, RSP set up.

Returning home: any unrecoverable fault long-jumps back to a `setjmp` buffer
saved by `dosrun` in `shell.c`, which restores the kernel CR3 and GDTR before
the prompt re-appears.

## Files touched

| File | Role |
|---|---|
| `arch/x86/dos/dos_int.c` | Surgical LRETW emulator + INT 21h native dispatcher |
| `arch/x86/dos/dos_exec.c` | LRETQ transfer, CR3/LDT/GDT setup, DOS4GW aliases |
| `arch/x86/dos/dos_loader.c` | MZ loader, DOS4GW signature scan |
| `arch/x86/dos/dos_api.c` | INT 21h DOS service handlers (file I/O, MCB, etc.) |
| `arch/x86/dos/dos_dpmi.c` | DPMI INT 31h handlers (AX=0008, 0009, 0305, 0306, …) |
| `arch/x86/dos/dos_int_stub.S` | Asm stubs for IDT-side INT 21h entry/iretq |
| `arch/x86/kernel/idt.c` | [pf-ist] probe, emulator dispatch, longjmp recovery |
| `arch/x86/dos/dos_types.h` | `dos_vm_t::dos4gw_mode` flag |

## Session commits (2026-04-24)

| Commit | Title |
|---|---|
| `b967964` | LRETW emu: 0xCF/IRETD, 0xEA, FF/5, FF/3 |
| `e20144c` | MOV Sreg with invalid sel → safe DS alias |
| `6c186ca` | LES (0xC4) + LDS (0xC5) — same strategy |
| `63a3daf` | Panic recovery via dos_native_exit_jmpbuf |
| `977b8c4` | JMP FAR with GDT selector + DOS-active fault dispatch |
| `93df38b` | Generic prefix decoder + #DB single-step recovery |
| `f265bad` | Rate-limit logs + force IF=1 in INT return |
| `75c98eb` | GDT[3] flat-32 attempted then reverted (doc only) |
| `962875c` | GDT[3]+GDT[4] aliased to DOOM CS/DS, emu accepts GDT-CS |
| `628d04d` | **Synth per-selector descriptor on segreg fault — DOOM reads WAD** |
| `6a9e0d0` | R/S diagnostics — discover DOOM uses DOS/16M not DOS/4GW |
| `fc00883` | BAD-HANDLE diagnostic — confirm DOOM is lost after synth |

Plus 9 earlier commits on the same branch (`d5c0397`..`91d98cb`) covering the
original DOS native transfer bringup and DPMI AX=0305/0306/0009 work.

## Where to go next

The surgical-emulator approach has hit a ceiling. To get DOOM past
`INT 10h AH=00` we need DOOM's relocation loop to ACTUALLY converge with the
right data, not the synth approximation. Two paths:

1. **Parse DOOM's embedded BW header at offset 0xF2A4** in DOOM.EXE, walk the
   DOS/16M descriptor table that follows it, and pre-populate `vm->dpmi.ldt[]`
   with the bases DOOM expects. Then synth descriptors aren't needed and
   `cmp es:[bx]` reads the right bytes. ~150-300 LOC plus reverse-engineering
   of DOS/16M's on-disk layout.
2. **Implement a real DOS/16M host stub** — proper PM↔RM mode-switch routines
   at fixed selectors, handle DOS/16M's specific DPMI services. ~500+ LOC,
   non-trivial.

Both are out of scope for the surgical-emulator track. DOOM-on-OsitoK is
parked at "ring-0 native execution proven, semantic correctness for the
relocation phase pending."
