#!/bin/bash
# OsitoK — Compile PTX kernels to embeddable C arrays
#
# Requires: CUDA Toolkit (ptxas, nvdisasm)
# Run on a machine with CUDA installed (e.g., the ntransformer dev machine).
#
# Usage:
#   ./scripts/compile_kernels.sh              # compile all
#   ./scripts/compile_kernels.sh gemv_q4_0    # compile one
#
# Output: arch/x86/kernels/generated/*.h (C byte arrays)
#
# The generated headers are checked into git so the bare-metal build
# doesn't need CUDA tools.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="$SCRIPT_DIR/../kernels"
GEN_DIR="$KERNEL_DIR/generated"
GPU_ARCH="${GPU_ARCH:-sm_75}"    # Default: Turing (RTX 2080/2070)

mkdir -p "$GEN_DIR"

# Find ptxas
PTXAS="${PTXAS:-$(which ptxas 2>/dev/null || echo "")}"
NVDISASM="${NVDISASM:-$(which nvdisasm 2>/dev/null || echo "")}"

if [ -z "$PTXAS" ]; then
    echo "ERROR: ptxas not found. Install CUDA Toolkit or set PTXAS=/path/to/ptxas"
    echo "  apt install nvidia-cuda-toolkit"
    echo "  or: export PATH=/usr/local/cuda/bin:\$PATH"
    exit 1
fi

echo "=== OsitoK PTX Kernel Compiler ==="
echo "ptxas:    $PTXAS"
echo "nvdisasm: ${NVDISASM:-not found (optional, for disassembly)}"
echo "target:   $GPU_ARCH"
echo ""

compile_kernel() {
    local name="$1"
    local ptx="$KERNEL_DIR/${name}.ptx"
    local cubin="$GEN_DIR/${name}.cubin"
    local header="$GEN_DIR/${name}_code.h"
    local disasm="$GEN_DIR/${name}.sass"

    if [ ! -f "$ptx" ]; then
        echo "SKIP: $ptx not found"
        return
    fi

    echo "── Compiling $name ──"

    # Step 1: PTX → cubin
    echo "  ptxas --gpu-name $GPU_ARCH -o $cubin $ptx"
    $PTXAS --gpu-name "$GPU_ARCH" -o "$cubin" "$ptx" 2>&1 | sed 's/^/  /'

    if [ ! -f "$cubin" ]; then
        echo "  ERROR: cubin not generated"
        return 1
    fi

    local cubin_size=$(stat -c%s "$cubin")
    echo "  cubin: $cubin_size bytes"

    # Step 2: Extract .text sections from ELF cubin
    # Detect entry points: multi-kernel PTX files have multiple .entry directives
    local entries
    entries=$(grep -oP '(?<=\.entry )\w+' "$ptx" 2>/dev/null || echo "$name")

    for entry in $entries; do
        local entry_header="$GEN_DIR/${entry}_code.h"
        python3 "$SCRIPT_DIR/cubin2array.py" "$cubin" "$entry" > "$entry_header"
        local entry_size=$(stat -c%s "$entry_header")
        echo "  header: $entry_header ($entry_size bytes)"
    done

    # Step 3: Optional disassembly
    if [ -n "$NVDISASM" ]; then
        echo "  nvdisasm -b $GPU_ARCH $cubin"
        $NVDISASM -b "$GPU_ARCH" "$cubin" > "$disasm" 2>/dev/null || true
        if [ -f "$disasm" ]; then
            local insn_count=$(grep -c '^\s' "$disasm" 2>/dev/null || echo "?")
            echo "  disasm: $disasm ($insn_count lines)"
        fi
    fi

    echo "  OK"
    echo ""
}

# Determine which kernels to compile
if [ $# -gt 0 ]; then
    for name in "$@"; do
        compile_kernel "$name"
    done
else
    # Compile all .ptx files in kernels/
    for ptx in "$KERNEL_DIR"/*.ptx; do
        name=$(basename "$ptx" .ptx)
        compile_kernel "$name"
    done
fi

echo "=== Done ==="
echo "Generated headers in $GEN_DIR/"
echo "Include them in sass.c and register in sass_init()."
