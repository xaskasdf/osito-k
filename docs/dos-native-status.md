# OsitoK DOS Native Execution — Status & Architecture

Status as of 2026-04-24, after a 12-commit session on the `experiment` branch
(b967964 → fc00883). Documents what works, what's blocked, and the design.

> The [P1 timer investigation](x86-dos-timer.md) records the September 24
> TSC-deadline correction and its physical-validation limits.
>
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

> The [client-resource follow-up](win32-layer-correctness.md#491-dpmi-per-client-locked-stacks-and-callback-addresses)
> isolates protected locked-stack pages and callback addresses across EXEC
> clients, releases child resources before restoring the parent, and checks
> allocation failure without overwriting ancestor state. Its 2,452 resource
> checks and 24 executable variants pass for USE16/USE32 children and mixed
> parent stack widths; the same 24 variants fail on the previous kernel.
> Children in these host-call paths still use the EXEC interpreter, even
> with a native parent. The completed rounds contain 144 passes and the
> three known native failures, including the full DOS API suite, normal
> native/emulated DOOM exits, digital PCM and post-DOS PE32 checks. Suspended
> client routing, real-mode host-stack ownership, shared paging/cancellation
> and the existing native FLAGS/ICEBP/EXEC failures remain open.

> The [real-stack follow-up](win32-layer-correctness.md#492-per-client-real-mode-host-stacks)
> separates real-mode host stacks across protected EXEC clients, bounds default
> argument copies and releases only the departing client's allocation. The
> ownership matrix passes 487 checks, the argument matrix passes 432 checks,
> and all 12 new executable variants pass while failing on the preceding
> kernel with identical fixture bytes. The completed round has 141 passes and
> the same three native failures, including the full DOS API and stack suites.
> Native/emulated DOOM exits, 659 million emulated instructions, digital PCM
> and subsequent PE32 probes also pass. Suspended-client routing, shared
> paging/cancellation, native FLAGS/ICEBP/EXEC, text glyphs, scaling and the
> other listed gaps remain open; this is not complete DOS compatibility.

> The [VM-page follow-up](win32-layer-correctness.md#493-vm-wide-extended-page-reservations)
> moves physical reservations out of private EXEC client snapshots, preserves
> them across client initialization/restoration and explicitly releases only
> departing clients' blocks. The new C context-boundary matrix passes 10,282
> checks, including resize/free interleavings and EMS/VCPI coexistence. This is
> not an executable test of older-client callback routing and has no new
> standalone old-kernel negative control. The existing COM bytes are unchanged.
> The completed full DOS API/stack and executable round has 141 passes and the
> same three native failures. Both DOOM exits, 663 million emulated instructions,
> digital PCM and subsequent PE32 probes pass. The VM is stopped and its scratch
> disks were removed automatically; evidence remains. Shared page tables,
> callback routing/cancellation, native FLAGS/ICEBP/EXEC and the other listed
> gaps remain open. This is not complete DOS/DPMI compatibility.

> The [client-routing follow-up](win32-layer-correctness.md#494-suspended-client-callbacks-and-primary-interrupt-routing)
> permits callbacks into suspended EXEC clients without changing the primary.
> IRQs and real exceptions retain primary ownership; protected software events
> use the current client. The new C matrix passes 640 checks, including redirected
> real returns. Twelve executable variants pass and fail on the preceding kernel
> with identical inputs. The scoped DPMI/stack regression and post-DOOM checks
> total 95 passes and one known native FLAGS failure; the full DOS API suite was
> not rerun. Both DOOM exits, 660 million emulated instructions and digital PCM
> pass. Shared guest paging, raw-switch/RSP identity, cross-mode cancellation and
> the other listed gaps remain open. This is not complete DOS/DPMI compatibility.

> The [raw-entry follow-up](win32-layer-correctness.md#495-recoverable-raw-mode-switch-admission)
> admits complete raw-switch destinations with repairable real-mode host
> exceptions, exactly-once private frames and handler-selected cancellation.
> Its C matrix passes 9,112 checks with 432 recoveries. Six executable variants
> pass and fail on the preceding kernel with identical fixture bytes. The scoped
> DPMI/stack regression totals 101 passes and one known native FLAGS failure;
> the exhaustive DOS API suite was not rerun. Both DOOM exits, 245 million
> emulated instructions, digital PCM and post-DOS PE32 checks pass. Shared paging,
> raw-switch/RSP identity and the other listed gaps remain open.

> The [raw-client follow-up](win32-layer-correctness.md#496-per-client-raw-entry-and-identity-based-restoration)
> gives live EXEC clients distinct real-mode raw entries and restores suspended
> host/EXEC callers by identity after raw switches permute their saved contexts.
> Current software events remain separate from primary hardware/real exceptions.
> The new matrix passes 2,416 checks; all 12 executable variants pass and fail on
> the preceding kernel with identical fixture bytes. The complete DOS API/stack
> and executable regression has 116 passes and the same three native failures.
> Both DOOM exits, 243 million emulated instructions, digital PCM and post-DOS
> PE32 checks pass. Scratch disks were removed automatically; evidence remains.
> This does not complete shared paging, cross-client fault/cancellation coverage,
> RSP lifetime or the other listed gaps, and does not advertise full DPMI support.

> The [host-unwind follow-up](win32-layer-correctness.md#497-owner-aware-host-activation-unwind)
> cleans synchronous host activation state by client identity after raw switches,
> preserving previous private frames, exit status and failed-call output records.
> Its C matrix passes 4,184 checks in 168 scenarios; the original 80-case matrix
> exposed 252 failures in the unchanged preceding implementation. The scoped
> DPMI/stack and executable regression has 174 passes and the three known native
> FLAGS/ICEBP/EXEC failures. Both DOOM exits, 242 million emulated instructions,
> digital PCM and post-DOS PE32 checks pass. The full DOS API suite was not rerun.
> Asynchronous cancellation, RSP lifetime, shared paging and the other listed
> gaps remain open. This does not advertise complete DOS/DPMI compatibility.

> The [EXEC-boundary follow-up](win32-layer-correctness.md#498-exec-boundary-activation-cleanup)
> retires abandoned older-client callbacks at child exit, preserves resources
> and existing private frames, and rejects ancestor frame-depth loss.
> Its C matrix passes 1,356 checks; 40 executable cases pass and fail on the
> preceding kernel with identical fixtures, including 4B01h and exception-stack
> parents. The full DOS API/DPMI/stack and executable regression finishes with
> 215 passes and three unchanged native gaps; both DOOM backends render and
> return normally, with PCM activity in the emulated run. This does not complete
> asynchronous cancellation, RSP lifetime, shared paging or DOS compatibility.
>
> The [debug routing follow-up](win32-layer-correctness.md#499-native-software-int-01h-versus-processor-debug-exceptions)
> separates native software INT 01h from processor #DB through the host gate's
> privilege level. Ten dedicated executions pass; the unchanged preceding
> kernel fails both native cases with the same fixture bytes. The KVM regression
> has 106 passes and three unchanged known results; all 16 TCG checks pass,
> including native ICEBP and post-DOS PE32. This does not fix the KVM ICEBP
> next-IP mismatch or complete interruptibility/tracing.
>
> The [single-step follow-up](win32-layer-correctness.md#4100-dos-single-step-retirement-and-native-handoff-ordering)
> adds TF retirement, DPMI debug-cause reporting and ordered native handoffs,
> including IRET clearing TF. Eighteen executable KVM checks and the new
> retirement/FLAGS/task matrices pass. The broader regression found an EXEC
> teardown kernel halt, not a complete pass;
> TCG 8.2.2 and 10.2.0 expose separate SS-load and REP debug-boundary failures.
> Full RF/watchpoint behavior and native interruptibility remain open.
>
> The [EXEC debug-state follow-up](win32-layer-correctness.md#4101-exec-ownership-of-native-host-debug-state)
> saves and restores native DR6 ownership with the parent instead of letting
> child cleanup clear its LDTR. The expanded continuation matrix fails 112
> checks without the fix and passes all 137 with it; all twelve raw-client
> executions pass in both backends. KVM completes with 126 passes and three
> unchanged known results. TCG completes 47 commands with 41 passes and the
> six native trace failures retained as failures. Full API and post-DOS PE32
> recovery pass in KVM; the earlier halted run remains archived separately.
>
> The [RF boundary follow-up](win32-layer-correctness.md#4102-rf-instruction-entry-and-repeated-string-continuation)
> implements instruction-entry RF consumption and REP continuation state in the
> interpreter/JIT. All 1,236 focused checks pass in KVM and TCG; the TCG audit
> retains six native trace failures among 48 calls. KVM completes 130 commands
> with 127 passes and three unchanged known results; full API and post-DOS PE32
> recovery pass. Synthetic DPMI exception frames and native port-I/O RF return behavior
> still needed direct coverage at that point; the following work addresses those paths.
>
> The [DPMI RF follow-up](win32-layer-correctness.md#4103-dpmi-rf-frames-and-locked-interrupt-continuation)
> adds fault/trap frame classification, scalar native-I/O RF consumption and
> transparent interpreted locked-interrupt returns. All 2,992 focused checks
> pass in KVM and TCG; independent negative controls fail 1,272 exception/I/O
> cases and 56 locked-interrupt cases. TCG retains its six native trace failures.
> The full KVM regression completed 142 commands, with 139 passes and three
> unchanged known results; TCG completed 60, with 54 passes and six native trace
> failures. Native locked-IRQ RF return and native service retries remain open.
>
> The [native decode follow-up](win32-layer-correctness.md#4104-bounded-native-instruction-decoding)
> bounds prefix/opcode/immediate fetches together and rejects EIP overflow.
> The 632 new cases and six COM executions pass in KVM and TCG; the old native
> decoder fails 116 matrix cases and both native COMs. Full KVM validation
> completes 136 commands, with 133 passes and three unchanged known results;
> TCG completes 54, with 48 passes and the same six native trace failures.
> All 187 original fixture hashes are unchanged; four probes were added.
>
> The [native IRQ follow-up](win32-layer-correctness.md#4105-native-locked-irq-return-before-flags-consumption)
> intercepts the locked handler's IRET before a private INT can consume its
> restored RF. The 296 new matrix cases and four native COMs pass in KVM and
> TCG, including actual PIT interrupts and SS16 callers. The focused total is
> 3,920; both basic COMs fail against the previous kernel. Broad KVM validation
> retains three known results among 130 commands; TCG retains six trace failures
> among 58. Native service retry/admission RF and the wider DOS scope remain open.
>
> The [software-entry RF follow-up](win32-layer-correctness.md#4106-rf-when-retrying-a-native-software-interrupt-entry)
> consumes RF when a completed native INT finishes its repaired handler entry,
> without losing RF on an exception-handler cancellation or replaying the INT.
> All 384 new checks and the reinforced COMs pass in KVM and TCG; the matching
> control fails 48 cases and both native COMs. The focused suite totals 4,304
> checks. KVM completes 140 selected commands with three unchanged known
> results; TCG completes 62 with the same six native single-step failures.
> Host-service retry/admission RF and the broader DOS scope remain unverified.
>
> The [host-service FLAGS verification](win32-layer-correctness.md#4107-dpmi-host-service-and-native-return-flags-verification)
> confirms selected return FLAGS without changing production behavior: 864
> native-dispatch frame cases and 10,628 service-buffer assertions pass in KVM
> and TCG. A separate RF-clearing mutation fails exactly 288 new cases. KVM
> completes 45 commands and TCG 44, each with the existing native FLAGS result;
> the full API also passes in KVM. Native paged-buffer RF and the wider DOS
> scope remain open; the linked section distinguishes the tested paths.
>
> The [native x87 routing fix](win32-layer-correctness.md#4108-native-x87-exceptions-and-bios-int-10h-routing)
> separates hardware #MF from BIOS INT 10h, including unregistered translators.
> The final probes pass in KVM/TCG and after DOOM; both fail on the old kernel.
> KVM completes 64 selected commands and TCG 58, each retaining the known FLAGS
> result. DOOM renders and exits normally. VBE's missing PASS text also occurs
> on the old kernel and is not counted as success. Full x87 ownership remains open.
>
> The [native FP preservation work](win32-layer-correctness.md#4109-native-dos-floating-point-state-across-services-and-exec)
> saves x87/MMX/SSE/AVX state around DOS service gates and retains it across raw
> PM/RM transitions and EXEC. Twelve FP probe executions pass in KVM/TCG and
> after DOOM; the four matching old-kernel controls fail. KVM completes 70
> selected commands and TCG 62, each retaining the known FLAGS result. DOOM
> renders and exits normally. Interpreter x87 and general nested kernel-ISR
> FP ownership remain open; this is not a complete coprocessor implementation.
>
> The [interpreter x87 implementation](win32-layer-correctness.md#4110-interpreter-x87-and-realprotected-mode-ownership)
> now executes checked x87 operands through an isolated host FPU, retains the
> register image across interpreter/JIT/native boundaries, and delivers real-mode
> IRQ13 or protected-mode #MF. KVM passes all 1,856 new checks; the 20 arithmetic,
> mode-transition and routing COM executions pass across KVM/TCG. TCG still fails
> 12 edge assertions, with direct-native old-kernel controls reproducing the
> underlying host limitations. KVM completes 67 selected commands with the known
> native FLAGS result; TCG completes 59 with that result and the failing FPU
> matrix. DOOM renders and exits zero. DPMI coprocessor enable/emulation controls,
> full CPUID feature coherence and general nested ISR FP ownership remain open.
>
> The [native ISR FP bridge](win32-layer-correctness.md#4111-native-dos-isr-floating-point-ownership-and-data-mediation)
> now gives each interrupt during a native DOS session a private x87/SSE/AVX
> image, and synchronizes that image with checked x87 data mediation on VGA.
> The new native probes pass with both stack widths; old-kernel and no-bridge
> mutation controls fail. KVM passes 1,858 FPU assertions and 69 of 70 selected
> commands, retaining native FLAGS/71h. TCG passes the new assertions but keeps
> its 12 x87 edge failures (59 of 61 commands pass). DOOM renders and exits zero;
> post-exit FP and PE32 probes pass. DPMI 0E00h/0E01h and native CR0 control,
> non-DOS ISR/NMI audits, CPUID coherence, music and performance remain open.
>
> The [DPMI coprocessor controls](win32-layer-correctness.md#4112-dpmi-coprocessor-controls-and-native-host-entry-policy)
> now implement 0E00h/0E01h per client, including native CR0 policy, protected
> and real-mode #NM reflection, raw switches and EXEC. Host entries clear EM/TS
> before using FP; other ABI gates and fast system calls are isolated during
> native DOS ownership and restored on detach. KVM passes all 1,931 FPU checks
> and 74 of 75 commands; TCG passes the new controls but retains its 12 x87
> edge failures and native FLAGS (63 of 65 commands pass). DOOM renders and
> exits normally, with CR0/EFER restoration measured and post-exit FP/PE32
> probes passing. CPUID coherence, broader ISR/NMI ownership, native FLAGS,
> paging/hidden-state audits, scaling, music and performance remain open.
>
> The [native virtual-interrupt contract audit](win32-layer-correctness.md#4113-native-dpmi-virtual-interrupt-contract-versus-virtual-iopl3)
> corrects the interpretation of the earlier native FLAGS/71h result: DPMI
> explicitly permits physical PUSHF and IF-insensitive POPF/IRET. The unchanged
> software-CPU probe tests a different, stronger virtual-IOPL3 profile. New
> native COMs cover both stack widths, USE16/USE32 FLAGS, 0900h-0902h and actual
> held/delivered IRQ0 with nested software handlers; six paired executions
> pass in each of KVM and TCG. Cross-profile failures are recorded separately,
> not turned into passes. The kernel is unchanged; the broader DOS scope remains
> open, including programs that require complete CPU FLAGS virtualization.
>
> The [BIOS extended-memory move follow-up](win32-layer-correctness.md#4114-bios-extended-memory-block-moves)
> replaces INT 15h/87h's successful no-op with checked physical copies. The
> 160-case matrix and six real/DPMI COM executions pass in KVM and TCG; the
> previous kernel fails all six data checks. DOOM renders and exits normally.
> Two KVM SS16 hardware-IRQ probe failures remain recorded despite a passing
> post-DOOM repetition; this is not an all-green regression audit or a claim
> that A20, the remaining BIOS services, or the full DOS layer are complete.
>
> The [native IRQ timing audit](win32-layer-correctness.md#4115-native-irq-probes-use-clock-deadlines)
> captures a timeout before one PIT period, with no bad return frame. The
> probe now waits on BIOS ticks without service calls inside its native loop;
> 80 hardware executions pass across KVM, TCG and the preceding kernel. Three
> IRQ-blocked controls reach their four-tick deadline and remain classified
> as intentional failures. The kernel is unchanged; the original failed logs
> are retained. This fixes the test's wait, not the outstanding DOS scope.
>
> The [A20 follow-up](win32-layer-correctness.md#4116-a20-bus-gating-and-dos-transfer-buffers)
> connects BIOS/8042/port 92h controls to CPU bus accesses, preserves DPMI's
> unmasked protected-mode policy, and fixes wrapped file/console buffers.
> Ninety kernel checks and eleven real/DPMI program executions pass on each
> final backend; the full DOS API suite passes on KVM. XMS/HMA management,
> guest reset, additional A20 transition coverage and the broader DOS scope
> remain open.
>
> Final A20 regression evidence records 57 positive commands on KVM and 55
> on TCG, with one FLAGS-profile observation and one expected IRQ-blocked
> failure per backend kept separate. All 221 preceding inputs are unchanged.
> DOOM renders and exits normally on KVM; scaling remains open. Both final
> VMs are stopped and their disposable disk leases released.
>
> The [XMS follow-up](win32-layer-correctness.md#4117-xms-control-entry-and-shared-extended-memory-ownership)
> adds a real far-call control entry, HMA reservation, counted A20 requests,
> dynamic handles, physical moves and shared-pool allocations. Its 2,201 internal
> checks report zero failures and eleven COM variants pass on KVM and TCG.
> Standalone serial monitors lost their result suffixes; klog/console evidence
> and the full KVM API suite confirm completion, but those monitor failures
> remain recorded and are excluded from the 67 KVM/66 TCG positive commands.
> XMS UMB functions work independently; DOS upper MCB linking, nested-client
> coverage and the remaining DOS scope are still open. DOOM renders and exits
> normally, and disposable test disks are released after evidence extraction.
>
> The [DOS UMB follow-up](win32-layer-correctness.md#4118-dos-upper-memory-arenas-and-process-owned-cleanup)
> connects DOS allocation/linking to protected XMS reservations and cleans up
> process-owned upper blocks even when the arena is unlinked. Short COM stacks
> stay inside their allocations. The 371-check C matrix and three real-mode
> EXEC program variants pass on KVM and TCG; TCG's failed serial monitor and
> reader-overlap error remain archived separately from console/klog recovery.
> A per-VM checker lease prevents competing readers. CONFIG.SYS/LOADHIGH,
> TSR/session residency, further nested-client tests and the broader DOS
> scope are still open.
>
> The [buffered UART follow-up](win32-layer-correctness.md#4119-buffered-uart-output-and-trustworthy-dos-test-captures)
> fixes silent transmitter timeouts and unsynchronized emitters. Host fault
> injection and 1,536 checksummed multi-CPU records pass; repeated UMB/XMS/BIOS
> summaries arrive intact without klog recovery on KVM and TCG. The full KVM
> API suite and DOOM exit pass, while TCG retains twelve x87 failures. The old
> control's incomplete captures and QEMU logfile discrepancies remain explicit;
> this is not complete DOS or lossless terminal-panic logging.
>
> The [x87 transfer follow-up](win32-layer-correctness.md#4120-mediated-x87-transfer-stack-faults-and-inhibited-stores)
> removes the recorded TCG failures from the mediated path: 33,731 checks
> and the full DOS API pass on KVM and TCG. Loads/stores/comparisons now
> enforce physical stack tags and inhibited stores retain their operands.
> Each backend passes 65 selected commands; KVM also renders and exits DOOM.
> Old-production controls still fail (504 KVM / 10,296 TCG), with raw log
> anomalies preserved. Native x87 differences, arithmetic-stack contracts,
> broader FP ownership and the remaining DOS work are not declared complete.
>
> The [x87 arithmetic-stack follow-up](win32-layer-correctness.md#4121-mediated-x87-arithmetic-stack-faults)
> extends tag mediation to arithmetic, FXCH, FCMOV and double-result
> operations. The 1,099,358-check matrix passes three times on KVM and TCG,
> including the full API and post-regression checks. Old production passes
> on this KVM host but fails 179,559 checks on TCG. Each backend passes 65
> selected commands; KVM also renders and exits DOOM. Initial out-of-domain
> fixture failures and raw-log anomalies remain explicit. Non-stack numeric
> exceptions, native differences and broader FP ownership are still open.
>
> The [x87 numerical-exception follow-up](win32-layer-correctness.md#4122-mediated-x87-numerical-exception-inhibition)
> preserves registers/TOP on reported unmasked #I/#D/#Z while respecting
> denormal FLD and completed #P operations. The expanded 1,192,286-check
> matrix passes three times on KVM. With identical tests, TCG improves from
> 4,704 to 2,256 failures: 18 cases corrected, 10 still failing. The full TCG
> API remains failed; unmasked exponent-biased results and transcendental
> exceptions are not declared complete. KVM passes 65 selected commands,
> TCG the other 62; KVM renders and exits DOOM normally. Raw log anomalies
> and failed checks are retained, all VM processes stopped and scratch
> disks released. The disk lifecycle host tests also pass.
>
> The [software-arithmetic follow-up](win32-layer-correctness.md#4123-mask-aware-extended-precision-dos-arithmetic)
> replaces host-dependent evaluation for 171 mediated x87 forms with a pinned,
> integer-only BSD SoftFloat core and per-call mask/rounding state. Clang and
> GCC pass 6,952,176 sanitized host comparisons. The unchanged guest matrix
> passes three times on KVM; TCG improves from 2,256 to 1,344 failures, fixing
> seven exponent-adjustment cases. Its remaining FPTAN/FSINCOS failures keep
> the numerical/API suites failed. KVM passes 65 selected commands, TCG the
> other 62; DOOM renders and exits normally. Failed attempts and raw anomalies
> remain archived, all VMs stopped and scratch disks released. Direct native
> FP differences and the broader DOS/DPMI scope are still open.
>
> The [software-trigonometry follow-up](win32-layer-correctness.md#4124-mediated-x87-trigonometric-execution)
> covers mediated FSIN/FCOS/FSINCOS/FPTAN with integer-only arithmetic,
> correct pair-stack effects and mask-aware tiny results. Both Clang and GCC
> pass 598,272 MPFR-backed evaluations and the existing 6,952,176 arithmetic
> comparisons. KVM and TCG pass the expanded 2,086,238-check guest matrix
> three times and 65 selected commands each; TCG's remaining 1,344 mediated
> numerical failures reach zero. Native CPU approximation differences remain
> explicitly separate from conformance passes. DOOM renders and exits normally;
> evidence is archived and both test VMs/scratch disks are released. Other
> transcendental instructions, native FP ownership and broader DOS/DPMI work
> remain open. The additional Bochs math directory retains its separate
> SoftFloat-2b/LGPL notices; it is not part of the uniformly BSD subset.
>
> The [elementary-math follow-up](win32-layer-correctness.md#4125-mediated-x87-exponential-logarithmic-and-arctangent-execution)
> adds mediated F2XM1/FYL2X/FYL2XP1/FPATAN, fixes tiny rounding and a
> freestanding constant-initialization defect, and audits the integer-only
> objects for runtime dependencies. Clang and GCC each pass 66,756,396 MPFR
> assertions plus the unchanged arithmetic/trig matrices. KVM and TCG pass
> the expanded 3,185,246-check guest matrix three times and 65 selected
> commands each; DOOM renders and exits normally. Original failures, two raw
> serial heading anomalies and native profile differences remain explicit.
> Sources, fixtures and evidence are archived; both VMs and scratch disks
> are released. FPREM/FPREM1, FXTRACT, direct-native FP ownership and the
> broader DOS/DPMI scope are still open.
>
> The [remainder/extraction follow-up](win32-layer-correctness.md#4126-mediated-x87-remainder-and-exponent-extraction)
> adds mediated FPREM/FPREM1/FXTRACT with exact remainder and quotient bits,
> partial-reduction progress, exponent/significand stack effects and
> mask-aware subnormal identity paths. Clang and GCC each pass 52,049,582
> new assertions and the unchanged arithmetic/trig/elementary matrices.
> KVM and TCG pass the expanded 10,696,286-check guest matrix three times
> and 65 selected positive commands each; DOOM renders and exits normally.
> Sources, fixtures and evidence are archived, both VMs stopped and scratch
> disks released. The new numerical cases use the interpreter entry point;
> native/JIT transport regressions remain separate. Other unconverted x87
> instructions, direct-native FP ownership and broader DOS/DPMI gaps remain.
>
> The [comparison/classification follow-up](win32-layer-correctness.md#4127-mediated-x87-comparisons-and-classification)
> adds 76 mediated comparison/FXAM forms, mask-aware result flags and exact
> zero/one/two-pop handling. Clang and GCC pass 793,901,617 assertions each;
> KVM and TCG pass the 34,943,582-check matrix three times and 65 selected
> positive commands each. Sources, fixtures, raw logs and captures are
> verified and archived; both VMs stopped and scratch disks released.
> Native CPU differences remain separately labeled; transfers/control,
> native ownership and the broader DOS/DPMI scope remain open.
>
> The [memory-conversion follow-up](win32-layer-correctness.md#4128-mediated-x87-memory-conversions-and-transfers)
> adds 21 mediated real/integer/BCD load/store forms, exact widening, FISTTP,
> destination rounding and mask-correct write/pop inhibition. Clang and GCC
> each pass 232,122,912 new assertions and the five previous numerical families;
> independently generated guest vectors match. KVM and TCG pass 188,224,598
> checks three times and 65 selected positive commands each, including all
> conversion widths crossing protected pages. DOOM renders and exits normally.
> Original failures and a byte-identical GCC type correction remain explicit
> in the archived evidence. Both VMs stopped and scratch disks were released.
> Register transfers/control, direct-native FP ownership and the broader
> DOS/DPMI scope remain open; the new matrix is not a hot-JIT benchmark.
>
> The [register/control follow-up](win32-layer-correctness.md#4129-mediated-x87-register-and-control-state)
> adds the remaining 129 mediated state/control forms. All 408 admitted forms
> now have a software handler; the kernel no longer links the native fallback.
> Clang and GCC each pass 93,174,649 assertions. The full 188,900,658-check
> guest matrix passes four times on KVM and three on TCG, with 66/65 selected
> positive commands. DOOM renders and exits; both VMs and scratch disks were
> released. Repeated malformed KVM serial headings are preserved explicitly,
> not normalized or described as fixed. Direct-native FP ownership/profile
> differences and the broader DOS/DPMI scope remain open.
>
> The [native FP diagnostic follow-up](win32-layer-correctness.md#4130-native-x87-diagnostic-state-and-backend-limits)
> preserves the client legacy environment through the native gate without
> truncating host 64-bit pointers. The 188,900,662-check matrix passes three
> times on KVM and once on TCG; an old-gate control fails exactly the new
> selector assertion. Strict diagnostic probes remain separate: KVM passes
> both raw-switch probes but fails both long-wait probes; TCG fails all four.
> IRQ-disabled and independent Linux/WSL controls expose backend diagnostic
> loss, not a completed compatibility contract. DOOM renders and exits,
> final evidence is verified, and all owned scratch disks/VMs are released.
> Native backend qualification/fallback and the broader DOS scope remain open.
>
> The [real-mode TSR follow-up](win32-layer-correctness.md#4131-real-mode-tsr-lifetime-and-global-dos-file-ownership)
> implements AH=31h and INT 27h, including the ROM-call path. EXEC no longer
> rolls back the global SFT: ordinary children close their own handles,
> while resident descendants keep memory and files after intermediate
> parents return. The new COM fixtures cover both 4B00 and 4B01, and the
> kernel adds 330 lifetime/edge checks. Both KVM and TCG pass the full API
> suite and all six TSR commands; the existing KVM native EXEC debug
> failure remains explicitly classified, not passed. Evidence is verified
> and scratch disks are released. Persistent host DOS sessions and DPMI
> 1.0 protected resident providers remain open.
>
> The [independent KVM debug diagnosis](win32-layer-correctness.md#4132-independent-native-debug-diagnosis-and-wsl-kvm-candidate)
> reproduces the ICEBP RIP mismatch without QEMU or Osito-K: all 60 ICEBP
> cases fail, while 60 single-step and hardware-breakpoint controls pass.
> Linux userspace ICEBP passes outside KVM. The SVM trace exposes the bad
> RIP before guest delivery. A host-kernel backport applies and its isolated
> translation unit compiles, but it has not been linked, installed or booted.
> Native ICEBP/EXEC therefore remain open; no guest RIP workaround was added.
>
> The [nested-kernel validation](win32-layer-correctness.md#4133-nested-kernel-validation-of-the-svm-icebp-backport)
> builds and boots that backport in a disposable Linux VM. The old kernel
> has 60 ICEBP failures; the candidate passes all 120 KVM cases, both with
> NextRIP and with the decoder fallback. The payload is byte-identical.
> The outer WSL is unchanged, so its separate ICEBP control still fails
> and the probe's real exit remains 1. Host deployment and native DOS
> regressions remain pending; this is not complete DOS compatibility.
>
> The [real DOS follow-up](win32-layer-correctness.md#4134-real-dos-validation-under-the-candidate-kvm-host)
> runs Osito-K itself under that candidate host. Native ICEBP and EXEC now
> meet their strict return contracts, with the original host failures kept
> as negative controls. Software EXEC also exposed locked-IRQ-stack
> exhaustion, and the full API run overflowed UART while retaining results
> in klog. These remain separate from the host fix and from full compatibility.
>
> The [atomic IRQ-return follow-up](win32-layer-correctness.md#4135-atomic-software-dpmi-interrupt-return)
> fixes software locked-stack retirement at IRET. The direct-host RF suite
> passes, a return-helper mutation produces 132 failures, and software DOOM
> reaches animated gameplay, PCM output and normal exit. Candidate-host
> validation of this newer binary and the remaining broad regressions are
> still incomplete; the follow-up records the pre-kernel boot failures.
>
> The [diagnostic transport follow-up](win32-layer-correctness.md#4136-candidate-host-boot-selection-and-diagnostic-transport)
> boots the corrected binary with explicit ESP priority, but retains a failed
> RF capture due to UART overflow and records an independent SMP klog-counter
> inconsistency. [Opt-in tracing](win32-layer-correctness.md#4137-opt-in-dos-instruction-diagnostics)
> moves verbose INT/CS/SetAccess diagnostics behind `dosrun --trace`; errors
> and test assertions stay enabled. Its new binary is validated separately.
>
> The [native service-boundary follow-up](win32-layer-correctness.md#4138-native-service-boundary-irq-state-and-retained-broad-failures)
> records the completed broad run as 241 passes and six failures, including
> intermittent native IF, x87 diagnostic pointers and missing VBE output.
> Thirty-two new RF cases expose stale interpreter IRQ/REP state at native
> service entry; the unchanged dispatcher fails exactly 16 negative-control
> cases. The corrected binary passes all 5,252 RF checks and 89 commands in
> 18 focused groups, including ten unchanged native IF repetitions and
> post-DOS Win32 recovery. Native DOOM with sound
> separately stalls after CLI/POPF, with virtual IF disabled; that is not
> qualified gameplay or evidence that the IRQ-shadow fix repairs DMX.
>
> The [candidate-host game controls](win32-layer-correctness.md#4139-candidate-host-game-controls-and-software-timer-stall)
> qualify native DOOM without sound through gameplay, input and normal exit.
> Software DOOM with sound instead times out with virtual IF enabled, a live
> PIT and IRQ0 stuck in the PIC's in-service state. Four used DMX stack slots
> point to nested delivery; its initiating cause is not yet established.
> The failure, silent WAV and read-only snapshots are retained. The VM is
> closed and its managed scratch disk released; post-hang recovery is not
> claimed from the earlier successful fixture sequence.
>
> The [IRQ timeline/input-device control](win32-layer-correctness.md#4140-opt-in-irq-timeline-and-reproduced-dmx-nesting)
> confirms four nested software IRQ0 entries and a missing fourth EOI in the
> captured DMX path. With identical binaries and only USB devices removed,
> the PS/2 control reaches gameplay, input, nonzero PCM and normal exit;
> post-game SEH3/SEH4/FLAGS checks pass. This points to the synchronous USB
> polling path for further timing investigation, not an automatic-EOI fix.
> The diagnostic ring is opt-in, the normal build is restored, and both
> disposable VMs are closed. USB and the full remaining DOS suite are not
> qualified by this control.
>
> The [common xHCI polling correction](win32-layer-correctness.md#4141-event-driven-xhci-polling-and-usb-doom-recovery)
> removes empty-ring MMIO from the hot path and fixes event acknowledgements.
> With USB retained, software DOOM now reaches gameplay, input, nonzero PCM
> and normal exit; three post-game recovery probes and ten native IF runs
> pass. All 240 prepared disk files match the earlier USB control. The
> diagnostic VM is closed and the default build restored. Physical AMD USB,
> the complete API/broad suite, native audio, x87 and VBE gaps remain open.
>
> The [default-kernel qualification and VBE allocation diagnosis](win32-layer-correctness.md#4142-default-kernel-regressions-and-vbe-console-allocation)
> completes the 47-group run: RF's 11 matrices and API's 184 matrices pass;
> the 44 broad groups report 242 passes and five retained failures (four
> strict x87 pointer probes and VBE output). Software DOOM on the default
> binary reaches verified gameplay, input, nonzero PCM and normal exit after
> two retained observer/menu-sequence timeouts; post-game recovery passes.
> Six pressure comparisons and hardware-breakpoint inspection prove that
> VBE's missing output comes from AH=09h translation allocating temporary
> storage from the COM program's exhausted DOS arena. Host-managed transfer
> storage is the next production fix, not a relaxed VBE oracle. Both VMs are
> closed and their managed scratch disks released. Native audio, x87,
> physical AMD USB, music and general performance remain open.
>
> The [host-owned DOS transfer fix](win32-layer-correctness.md#4143-host-owned-dos-transfer-storage)
> removes the console/file translator's dependency on free application MCB
> memory. All 12 pressure executions and the unchanged VBE test pass; API
> passes 185 exact matrices, including 63 new reservation checks. All 47
> regression groups finish: 248 verified command passes and four remaining
> x87 failures. The original checker also retains a NET-interleaving failure,
> independently resolved by replaying its unchanged oracle over decoded logs.
> Native DOOM without sound reaches reviewed gameplay, input and normal exit;
> three post-game recovery commands pass. Closed byte/hash audits pass, both
> VMs exit 0 and no scratch disks remain. Native audio, strict x87 diagnostics,
> physical AMD USB, music and performance are still open.
>
> The [cross-route x87 controls](win32-layer-correctness.md#4144-cross-route-x87-diagnostic-regression-controls)
> keep that kernel unchanged and add five integer/x87-only COMs. Interpreter
> and JIT each pass four cases with 135 complete-state snapshots per case,
> including raw switches and EXEC. Native execution fails all four; the
> original strict probes also remain failing. Independent GCC/Clang Linux
> controls reproduce the diagnostic loss outside Osito-K. The FPU matrix,
> four numerical state probes and three recovery commands still pass. Both
> VMs are closed and scratch disks released. Native backend qualification
> and selection remain open; this does not qualify the original SIMD probes.
>
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
