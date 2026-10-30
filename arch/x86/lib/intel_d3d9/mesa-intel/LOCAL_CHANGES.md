# Local portability changes

`source.json` stores the hash of every current imported file. Its
`local_changes` object also records the original upstream hash for each
modified file, so the Mesa 25.0.0 release remains independently verifiable.

- `src/intel/compiler/brw_opt_bank_conflicts.cpp`: include `util/u_memory.h`
  and use `align_malloc`/`align_free` for `weight_vector_type`. This preserves
  the existing alignment requirement and pairs allocation/free correctly on
  both POSIX and Windows. No register-allocation or shader arithmetic change.
