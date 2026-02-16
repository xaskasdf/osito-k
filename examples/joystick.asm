; joystick.asm - Joystick input test for OsitoVM
;
; Reads joystick events and raw ADC in a loop.
; Prints direction and button events.
; Press Ctrl+C to exit (handled by VM runtime).

; local 0 = loop counter

    push8 0
    store 0         ; counter = 0

loop:
    ; Print loop counter
    push8 35        ; '#'
    putc
    load 0
    put_dec
    push8 32        ; ' '
    putc

    ; Read and print raw ADC value
    push8 88        ; 'X'
    putc
    push8 61        ; '='
    putc
    adc_read
    put_dec
    push8 32        ; ' '
    putc

    ; Read packed state: x_raw[15:0] | button[16]
    input_state     ; → [state]
    push32 65536    ; → [state, 65536]
    div             ; → [state/65536] = button bit (0 or 1)
    jz btn_up       ; consumes button bit → []

    ; Button is pressed
    push8 80        ; 'P'
    putc
    jmp btn_done

btn_up:
    push8 45        ; '-'
    putc

btn_done:
    ; Check for events
    push8 32        ; ' '
    putc

poll_events:
    input_poll
    dup
    jz no_more_events

    ; Print event type
    dup
    push8 1
    eq
    jnz print_left
    dup
    push8 2
    eq
    jnz print_right
    dup
    push8 3
    eq
    jnz print_press
    dup
    push8 4
    eq
    jnz print_release
    drop
    jmp poll_events

print_left:
    drop
    push8 76        ; 'L'
    putc
    jmp poll_events

print_right:
    drop
    push8 82        ; 'R'
    putc
    jmp poll_events

print_press:
    drop
    push8 68        ; 'D'
    putc
    jmp poll_events

print_release:
    drop
    push8 85        ; 'U'
    putc
    jmp poll_events

no_more_events:
    drop            ; drop the 0 from input_poll

    ; Newline
    push8 10
    putc

    ; counter++
    load 0
    push8 1
    add
    store 0

    ; Delay ~200ms (20 ticks)
    push8 20
    delay

    jmp loop
