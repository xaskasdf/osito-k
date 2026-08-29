set pagination off
set confirm off
set remotetimeout 15
set logging file /root/osito-steam/gdb-tier0-aa-read.log
set logging overwrite on
set logging enabled on

target remote 127.0.0.1:1234
set $hits = 0
hbreak *0xffff80003d47a156
commands
  silent
  set $hits = $hits + 1
  set $raw = *(unsigned long long *)$rax
  if $raw == 0xaaaaaaaaaaaaaaaa
    printf "\n=== tier0 reads poisoned page-map slot ===\n"
    printf "hits=%llu slot=%p raw=0x%llx\n", $hits, $rax, $raw
    info registers rip rsp rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15
    x/8gx $rax-32
    x/32gx $rsp
    x/20i $rip-40
    detach
    quit
  end
  continue
end
continue
