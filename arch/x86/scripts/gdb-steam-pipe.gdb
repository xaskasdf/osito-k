set pagination off
set confirm off
set print address on

target remote 127.0.0.1:1234
hbreak *0x00000001388d4780

commands
  silent
  set $pipe = $rdi
  set $backend = *(unsigned long long *)($pipe + 0x10)
  set $vtable = *(unsigned long long *)$backend

  printf "STEAM_PIPE_BREAK\n"
  printf "rip=%#llx rsp=%#llx pipe=%#llx backend=%#llx vtable=%#llx\n", $rip, $rsp, $pipe, $backend, $vtable
  printf "PIPE_OBJECT\n"
  x/24gx $pipe
  printf "PIPE_BACKEND\n"
  x/24gx $backend
  printf "PIPE_VTABLE\n"
  x/16gx $vtable
  printf "CALL_STACK\n"
  x/48gx $rsp

  detach
  quit
end

continue
