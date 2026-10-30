#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-arch/x86/build/kexec-image-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/{kexec_image,kexec_memory,pci_dma}_host.c \
    arch/x86/kernel/{kexec_image.h,pci.c,memory.c,smp.h} \
    arch/x86/include/{types,boot_info,pci,paging,sys_caps}.h "$out/"
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
for compiler in gcc clang; do
    for variant in normal sanitized; do
        extra=()
        if [[ $variant == sanitized ]]; then
            extra=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        for suite in kexec_image kexec_memory pci_dma; do
            "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror \
                -ffunction-sections -fdata-sections "${extra[@]}" \
                "arch/x86/test/${suite}_host.c" -Wl,--gc-sections -o "$out/$suite-$compiler-$variant"
            "$out/$suite-$compiler-$variant" | tee "$out/$suite-$compiler-$variant.log"
        done
    done
done
