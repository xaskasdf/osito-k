; blink.asm — blink GPIO2 (LED) 5 times
  push8 2
  push8 1       ; GPIO_MODE_OUTPUT
  syscall 7     ; gpio_mode(2, 1)
  push8 5       ; counter
loop:
  dup
  push8 0
  eq
  jnz done
  push8 2
  push8 0
  syscall 8     ; gpio_write(2, 0) -> LED on
  push8 25
  syscall 5     ; delay 25 ticks (250ms)
  push8 2
  push8 1
  syscall 8     ; gpio_write(2, 1) -> LED off
  push8 25
  syscall 5     ; delay 25 ticks
  push8 1
  sub           ; counter--
  jmp loop
done:
  drop
  halt
