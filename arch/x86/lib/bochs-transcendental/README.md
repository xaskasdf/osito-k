# Bochs transcendental arithmetic for OsitoK

This is a derivative of the Bochs x87 transcendental arithmetic library,
pinned to commit `dafbd06bf89fcd15b34cefcdf345fd0f30870de0`:
https://github.com/bochs-emu/Bochs/tree/dafbd06bf89fcd15b34cefcdf345fd0f30870de0/bochs/cpu/fpu

Imported files are `f128_67.cc`, `fsincos.cc`, `fsincos_constants.h`,
`f2xm1.cc`, `f2xm1_constants.h`, `fyl2x.cc`, `fyl2x_constants.h`,
`fpatan.cc`, `fpatan_constants.h`, `fprem.cc`,
`fpu_constant.h`, `fpu_trans.h`, `softfloat-helpers.h`, and
`softfloat-specialize.h`. `upstream.sha256` identifies their original bytes;
`COPYING` is the upstream Bochs license text. Keep the individual notices:
several files carry the SoftFloat Release 2b derivative-work terms, while
`fpu_trans.h` carries LGPL-2.0-or-later. This directory is not the uniformly
BSD-3-Clause subset in `../softfloat3e`. Preserve source and relinkable build
materials when distributing linked binaries under the applicable licenses.

Local changes:

- `port.h` supplies fixed-width Bochs integer aliases without its CPU runtime.
- `fpu_trans.h` includes that port header.
- `f2xm1.cc` includes the same port header for its fixed-width aliases and
  initializes minus one as static data, without requiring C++ startup.
- `softfloat-helpers.h` retains its product invariant using a compiler trap
  instead of a hosted libc assertion. No check is compiled out.
- `fsincos.cc` preserves the direction of tiny nonzero corrections through
  SoftFloat's extended round/pack operations, including subnormal and biased
  underflow results. Zero remains exact. Cosine is evaluated before sine for
  the two-result instruction, retaining the sine result's final C1.
- `fprem.cc` returns exact signed zero before the nonzero normalizer, avoiding
  an invalid 64-bit shift. Identity shortcuts use mask-aware round/pack too:
  an exact subnormal remainder still needs exponent bias when underflow is
  unmasked. The helper supports both FPATAN's reduction and the DOS adapter's
  FPREM/FPREM1 operations; the adapter owns quotient condition codes.
- `fpatan.cc` preserves the negative infinitesimal correction of tiny exact
  quotients, or the division residue of nonexact ones. Exponents remain
  separate until the final mask-aware rounding.
- `fyl2x.cc` takes the exact logarithm of powers of two directly from their
  normalized exponent, avoiding table error at the underflow boundary.

The existing table/polynomial and argument-reduction algorithms are retained.
This import does not promise bit-identical transcendental approximations on
all CPU families. Guest stack effects and exception delivery remain in the
DOS layer, outside this library.
