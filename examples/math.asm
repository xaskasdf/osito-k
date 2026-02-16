; math.asm — 7 * 6 = 42
  push8 7
  push8 6
  mul
  syscall 2    ; put_dec -> "42"
  push8 10
  syscall 0    ; '\n'
  halt
