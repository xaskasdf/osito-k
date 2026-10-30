# SoftFloat extended-precision arithmetic

Selected files from Bochs' Berkeley SoftFloat Release 3e fork, pinned to
`dafbd06bf89fcd15b34cefcdf345fd0f30870de0`:
https://github.com/bochs-emu/Bochs/tree/dafbd06bf89fcd15b34cefcdf345fd0f30870de0/bochs/cpu/softfloat3e

`upstream-files.txt` lists the verbatim imported files, relative to that
directory. `upstream.sha256` records their hashes. Keep the BSD-3-Clause
notice in `COPYING.txt` and each file, including in binary distributions.
No Bochs CPU/FPU (LGPL) instruction implementation is imported here.

Local additions are this README, the manifests and `config.h`. The C++
interface is upstream's; it uses integer arithmetic and explicit per-call
status, not the kernel's floating-point environment or global rounding state.
It is built freestanding, without exceptions, RTTI or a C++ runtime.

The DOS adapter is `../../dos/dos_fpu_softfloat.cc`. It selects decoded
arithmetic, including `extF80_extract` for FXTRACT, and comparisons through
`extF80_compare`. FXAM uses the existing `extF80_class` helper, with the DOS
adapter preserving the distinction between unsupported and empty registers.
The adapter also handles real/integer/packed-BCD memory loads and stores,
including FISTTP, using exact widening and explicit destination rounding.
The 74 original files remain verbatim. The adapter preserves the OS's stack,
memory and exception ownership. Raw 80-bit transfers do not classify or quiet
their payload. Invalid FBLD digits have an undefined architectural result;
the adapter's weighted-digit policy is not claimed as portable conformance.
The remaining instructions still use the existing isolated x87 host path;
this import alone is not complete software x87 emulation.
