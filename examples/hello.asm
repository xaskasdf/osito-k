; hello.asm — Hello World para OsitoVM
  push8 72    ; 'H'
  syscall 0
  push8 101   ; 'e'
  syscall 0
  push8 108   ; 'l'
  syscall 0
  push8 108   ; 'l'
  syscall 0
  push8 111   ; 'o'
  syscall 0
  push8 10    ; '\n'
  syscall 0
  halt
