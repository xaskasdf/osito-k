set pagination off
set confirm off
set $event_a = 0x1d8
set $event_b = 0x1dc
set $event_obj_a = 0x204d40700
set $event_obj_b = 0x204d40710
set $wait_results = 0

file /mnt/c/Users/xasko/osito-k/arch/x86/build/kernel.elf
target remote 127.0.0.1:1234

break WaitForSingleObject if $rcx == $event_a || $rcx == $event_b
commands
  silent
  printf "STEAM_EVENT wait handle=%#llx timeout=%u caller=%#llx\n", $rcx, $rdx, *(unsigned long long *)$rsp
  continue
end

break SetEvent if $rcx == $event_a || $rcx == $event_b
commands
  silent
  printf "STEAM_EVENT set handle=%#llx caller=%#llx\n", $rcx, *(unsigned long long *)$rsp
  continue
end

break ResetEvent if $rcx == $event_a || $rcx == $event_b
commands
  silent
  printf "STEAM_EVENT reset handle=%#llx caller=%#llx\n", $rcx, *(unsigned long long *)$rsp
  continue
end

hbreak *0x000000013f014246 if $rbx == $event_obj_a || $rbx == $event_obj_b
commands
  silent
  printf "STEAM_EVENT result object=%#llx status=%#x\n", $rbx, $eax
  set $wait_results = $wait_results + 1
  if $wait_results >= 32
    detach
    quit
  end
  continue
end

continue
