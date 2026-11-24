#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
mkdir -p "$out"
flags=()
if [[ ${GDI_HOST_FIXTURE:-extpen} == polygon ]]; then flags+=(-DGDI_HOST_POLYGON); fi
if [[ ${GDI_HOST_FIXTURE:-extpen} == arc ]]; then flags+=(-DGDI_HOST_ARC); fi
if [[ ${GDI_HOST_SANITIZE:-0} == 1 ]]; then
    # GetProcAddress exposes the Win32 ABI using the PE fixture's private
    # struct declarations. Their layouts match; C function type names do not.
    flags+=(-fsanitize=address,undefined -fno-sanitize=function -fno-sanitize-recover=all -fno-omit-frame-pointer)
fi
clang "${flags[@]}" -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-address \
    -ffunction-sections -fdata-sections -Iarch/x86/include -Iarch/x86/win32 \
    -Iarch/x86/kernel -Iinclude/common arch/x86/test/extpen_host.c \
    -Wl,--gc-sections -o "$out/extpen-host"
"$out/extpen-host" > "$out/output.log"
