# Repository Guidelines

## Project Structure & Module Organization

Osito-K is a bare-metal OS with architecture-specific code under `arch/<arch>/`.
The active trees are `arch/xtensa/` for ESP8266 firmware, `arch/x86/` for the
x86-64 UEFI/kernel target, `arch/arm/` for AArch64 SM8350, and `arch/wasm/` for
Emscripten. Shared GUI code lives in `gui/`; shared headers and on-disk format
definitions live in `include/common/`. Host filesystem tools are in
`tools/ositofs/`, design notes are in `docs/`, and x86 in-OS tests are in
`arch/x86/test/*.c`. Build outputs and local disk images belong in ignored
`build/` directories or ignored `nvme_*.img` assets.

## Build, Test, and Development Commands

- `make` builds the default CI target, Xtensa ESP8266.
- `make x86`, `make arm`, and `make wasm` delegate to the architecture
  Makefiles.
- `make -C arch/x86` builds `arch/x86/build/{boot.efi,kernel.elf}`.
- `make -C arch/x86 CLANG=1` uses clang/lld for the Windows/MSYS2 workflow.
- `make -C tools/ositofs` builds host tools for creating and editing OsitoFS
  images.
- `arch/x86/scripts/qemu-test.sh [--no-build] [--no-gl] [--kill]` runs the x86
  QEMU loop. Use `arch/x86/build/serial.log` as the primary debug output.
- `make clean-all` removes all architecture build directories.

## Coding Style & Naming Conventions

Keep LF line endings; `.gitattributes` enforces this and CRLF creates noisy
diffs. Follow the existing C style in each architecture subtree and keep
hardware-facing code explicit: prefer register names, small helpers, and local
patterns over broad abstractions. Put architecture-specific code under
`arch/<arch>/`; only move reusable formats or APIs into `include/common/`.

## Testing Guidelines

There is no host unit-test harness. x86 tests compile to ELFs from
`arch/x86/test/*.c` and run inside the booted OS after being copied to an
NVMe/ESP image. Network behavior can be smoke-tested through the UDP inference
server, for example: `echo "hola osito" | nc -u localhost 7777`.

## Commit & Pull Request Guidelines

Recent commits use concise, subsystem-prefixed subjects such as
`x86/win32: ...` or `paging/smp: ...`. Keep the first line action-oriented and
specific. Pull requests should describe the target architecture, commands run,
serial-log findings for QEMU work, and any required images, WADs, or toolchains.

## Agent-Specific Instructions

Before editing a function, class, or method, run GitNexus impact analysis for
the symbol and report the blast radius. Before committing, run
`gitnexus_detect_changes()` to verify the affected scope.
