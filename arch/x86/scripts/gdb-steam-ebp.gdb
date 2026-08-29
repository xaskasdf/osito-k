set pagination off
set confirm off
set architecture i386:x86-64
set logging file /root/osito-steam/gdb-ebp.log
set logging overwrite on
set logging enabled on
target remote localhost:1234

hbreak *0x1005025c
commands 1
  silent
  printf "AFTER_CALL rip=%#lx rbp=%#lx rsp=%#lx rax=%#lx rcx=%#lx\n", $rip, $rbp, $rsp, $rax, $rcx
  continue
end

hbreak *0x10050263
continue
printf "BEFORE_STORE rip=%#lx rbp=%#lx rsp=%#lx rax=%#lx rcx=%#lx\n", $rip, $rbp, $rsp, $rax, $rcx
x/12wx $rsp
detach
quit
