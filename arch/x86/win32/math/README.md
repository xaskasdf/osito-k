# CRT double-precision math

Selected generic C routines from musl 1.2.5, fetched from the official release.
`upstream.json` records the archive and original/local file hashes. The original
per-file notices and full `COPYRIGHT` are retained.

The upstream algorithms are unchanged. Includes for `math.h`, `features.h`, and
`endian.h` select the adjacent freestanding headers. `math.h` namespaces every
linked symbol with `osito_libm_`; `fp_arch.h` selects no architecture overrides.
Compile without fast-math or floating-point contraction. Only double precision
is used here. The Win32 entry points, errno policy and PE32 ABI adapters live
outside this directory in `crt_math.c` and `compat32.c`.
