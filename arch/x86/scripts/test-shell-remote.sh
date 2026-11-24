#!/bin/bash
set -euo pipefail
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/shell-remote-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/shell_remote_host.c \
    arch/x86/kernel/{shell.c,shell_remote.h,sshd.h,driver_manager.h,kexec.h,kernel_update.h,rag.h,oftp_rx.h,shell_redirect.h,nic.h,wpa2_status.h,wpa2_crypto.h,wpa2_selftest.h,wpa2_rsn.h} \
    arch/x86/drivers/{ps2.h,iwx.h,iwx_scan.h,iwxreg.h,intel_gfx_driver.h} \
    arch/x86/win32/{d3dkmt_registry.h,d3dkmt_abi.h,nttypes.h} \
    arch/x86/fs/{vfs.h,ositofs_metadata.h} \
    arch/x86/include/{types.h,stdint.h,boot_info.h,input_events.h,pci.h,dma_diag.h} \
    arch/x86/include/sys/display_syscalls.h "$out/"
out=$(realpath "$out")
cd "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $compiler == gcc ]]; then flags+=(-Wno-clobbered); fi
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-unused-function \
            -Iarch/x86/include -ffunction-sections -fdata-sections -Wl,--gc-sections \
            "${flags[@]}" arch/x86/test/shell_remote_host.c -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
