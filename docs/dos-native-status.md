# OsitoK DOS Native Execution — Status & Architecture

Status as of 2026-04-24, after a 12-commit session on the `experiment` branch
(b967964 → fc00883). Documents what works, what's blocked, and the design.

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

## Discovery: DOOM.EXE uses Pharlap DOS/16M, not Watcom DOS/4GW

The header at offset 0xF2A4 inside DOOM.EXE has magic `0x42 0x57 = "BW"` —
the Pharlap **DOS/16M** signature. DOS/4GW uses LE/LX magic. The "DOS/4G "
substring at offset 0x25C is just a copyright string in DOS/16M's banner.

Practical consequence: our `vm->dos4gw_mode` flag is a misnomer (it covers
both DOS/16M and DOS/4GW), but the workarounds — segment-load synthesis,
GDT[3]/[4] aliases, raw-mode-switch tolerance — apply to both.

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
