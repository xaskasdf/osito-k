#!/bin/bash
set -euo pipefail
export UBSAN_OPTIONS=halt_on_error=1
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/iwx-host}
firmware=${2:-}
test ! -e "$out"
mkdir -p "$out"
cp --parents arch/x86/drivers/iwx{.c,.h,reg.h,_fw.c,_scan.c,_scan.h,_link.c,_link.h,_data.c,_data.h} \
    arch/x86/include/{types,pci,paging,stdint}.h arch/x86/test/iwx{,_fw,_scan,_link,_data,_crypto}_host.c \
    arch/x86/test/pci_wlan_host.c arch/x86/test/pci_dma_host.c arch/x86/kernel/pci.c \
    arch/x86/include/{boot_info,dma_diag}.h arch/x86/kernel/dma_diag.c \
    arch/x86/test/dma_diag_host.c \
    arch/x86/kernel/boot_diag.c arch/x86/test/boot_checkpoint_host.c \
    arch/x86/drivers/{gpu,intel_gfx}.h "$0" "$out/"
cp --parents arch/x86/kernel/wpa2{,_rsn,_crypto}.{c,h} arch/x86/kernel/wpa2_status.h arch/x86/kernel/random_secure.{c,h} "$out/"
cp --parents arch/x86/kernel/nic.h "$out/"
cp --parents arch/x86/kernel/{nic.c,net.c,net.h,dhcp.c,smp.h} \
    arch/x86/drivers/{i211.h,rtl8111.h} arch/x86/test/iwx_net_host.c "$out/"
cp -r --parents arch/x86/lib/bearssl "$out/"
out=$(realpath "$out")
fw_args=()
if [[ -n $firmware ]]; then
    cp "$firmware" "$out/firmware.ucode"
    sha256sum "$out/firmware.ucode" | tee "$out/firmware.sha256"
    fw_args=("$out/firmware.ucode")
fi
cd "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        for suite in iwx iwx_net iwx_fw iwx_scan iwx_link iwx_data pci_wlan pci_dma dma_diag boot_checkpoint; do
            extra=()
            if [[ $suite == iwx || $suite == iwx_net || $suite == iwx_link || $suite == iwx_data ]]; then
                extra=(arch/x86/kernel/wpa2_rsn.c)
            fi
            if [[ $suite == iwx || $suite == iwx_net ]]; then
                "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
                    -c arch/x86/test/iwx_crypto_host.c -o "$out/crypto-$compiler-$mode.o"
                extra+=(-Iarch/x86/include -Iarch/x86/lib/bearssl/port -DRANDOM_SECURE_HOST_TEST
                    -DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0
                    -Iarch/x86/lib/bearssl/inc -Iarch/x86/lib/bearssl/src
                    arch/x86/kernel/{wpa2,wpa2_crypto,random_secure}.c
                    arch/x86/lib/bearssl/src/hash/sha1.c arch/x86/lib/bearssl/src/mac/hmac.c
                    arch/x86/lib/bearssl/src/codec/{dec32be,enc32be}.c
                    arch/x86/lib/bearssl/src/symcipher/{aes_ct,aes_ct_dec,aes_ct_cbcdec}.c
                    "$out/crypto-$compiler-$mode.o" -lcrypto)
            fi
            "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
                -ffunction-sections -fdata-sections -Wl,--gc-sections \
                "arch/x86/test/${suite}_host.c" "${extra[@]}" -o "$out/$suite-$compiler-$mode"
            "$out/$suite-$compiler-$mode" "${fw_args[@]}" | tee "$out/$suite-$compiler-$mode.log"
        done
    done
done
