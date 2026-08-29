set pagination off
set confirm off
set remotetimeout 15
set logging file /root/osito-steam/gdb-closehandle-invalid.log
set logging overwrite on
set logging enabled on

target remote 127.0.0.1:1234
set $close_hits = 0
hbreak CloseHandle
condition $bpnum (($rcx & 3) != 0) || (($rcx > 0x3ffc) && ($rcx < 0xfffffffffffffff0))
commands
  silent
  set $close_hits = $close_hits + 1
  printf "\n=== invalid CloseHandle argument ===\n"
  printf "hits=%llu handle=0x%llx wrapper_return=%p\n", $close_hits, $rcx, *(void **)$rsp
  info registers rip rsp rbp rax rbx rcx rdx rsi rdi r8 r9 r10 r11 r12 r13 r14 r15
  printf "\n--- raw call stack ---\n"
  x/96gx $rsp
  printf "\n--- CloseHandle entry ---\n"
  x/12i $rip
  detach
  quit
end
continue
