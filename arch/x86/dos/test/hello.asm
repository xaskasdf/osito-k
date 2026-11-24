; HELLO.COM — minimal DOS test program
; Assemble: nasm -f bin -o HELLO.COM hello.asm
;
; Uses INT 21h/09h to print a string and INT 21h/4Ch to exit.

org 0x100

    mov dx, msg      ; DS:DX → string
    mov ah, 0x09     ; DOS: print string
    int 0x21

    mov ah, 0x4C     ; DOS: exit
    mov al, 0        ; exit code 0
    int 0x21

msg db 'Hello from DOS on OsitoK!$'
