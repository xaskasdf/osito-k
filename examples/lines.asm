; lines.asm — animated lines demo for OsitoK framebuffer
;
; Draws rotating lines from the center of the screen.
; Uses locals: 0=angle counter, 1=frame counter

  push8 0
  store 0         ; angle = 0
  push8 0
  store 1         ; frame = 0

frame_loop:
  fb_clear

  ; Draw 8 lines from center (64,32) to edge points
  ; Each line endpoint rotates based on angle counter

  load 0          ; get angle
  store 2         ; temp = angle

  push8 8
  store 3         ; line_count = 8

line_loop:
  load 3
  push8 0
  eq
  jnz frame_done

  ; Compute endpoint x: (angle + line_idx * 16) % 128
  load 2
  push8 127
  and             ; x1 = temp & 127

  ; Compute endpoint y: (angle + line_idx * 8) % 64
  load 2
  push8 63
  and             ; y1 = temp & 63

  ; Push args: x0=64, y0=32, x1, y1
  ; Stack currently has: x1, y1 on top (y1 at TOS)
  ; We need: x0 y0 x1 y1
  ; Rearrange: store endpoints, push center, push endpoints

  store 5         ; save y1
  store 4         ; save x1

  push8 64        ; x0 = center
  push8 32        ; y0 = center
  load 4          ; x1
  load 5          ; y1
  fb_line

  ; Advance temp for next line
  load 2
  push8 16
  add
  store 2

  ; line_count--
  load 3
  push8 1
  sub
  store 3

  jmp line_loop

frame_done:
  ; Draw border
  push8 0
  push8 0
  push8 127
  push8 0
  fb_line

  push8 127
  push8 0
  push8 127
  push8 63
  fb_line

  push8 127
  push8 63
  push8 0
  push8 63
  fb_line

  push8 0
  push8 63
  push8 0
  push8 0
  fb_line

  fb_flush

  ; Delay ~100ms (10 ticks at 100Hz)
  push8 10
  delay

  ; angle += 3
  load 0
  push8 3
  add
  store 0

  ; frame++
  load 1
  push8 1
  add
  dup
  store 1

  ; Run for 200 frames then halt
  push8 200
  lt
  jnz frame_loop

  halt
