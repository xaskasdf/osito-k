#!/bin/bash
set -euo pipefail
script_dir=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$script_dir/../../.." && pwd)
run=$(realpath -e "${1:?diagnostic directory required}")
kernel=$(realpath -e "${2:-$repo/arch/x86/build/kernel.elf}")
for output in kernel.elf serial.log disks.txt qemu.pid; do
    if [ -e "$run/$output" ] || [ -L "$run/$output" ]; then
        echo "Use a fresh evidence directory: $run ($output already exists)" >&2
        exit 1
    fi
done
if [ -z "${OSITO_TEST_DISK_DIR:-}" ]; then
    exec bash "$script_dir/qemu-test-disks.sh" run -- bash "$0" "$run" "$kernel" "${3:-}"
fi
cd "$repo"
test -d "$run"
disk="$OSITO_TEST_DISK_DIR/disk.img"
test ! -e "$disk"
printf 'scratch=%s\nnvme=%s\nesp=%s\n' "$OSITO_TEST_DISK_DIR" "$disk" "$QEMU_ESP_IMG" > "$run/disks.txt"
truncate -s 128M "$disk"
tools/ositofs/mkfs.ositofs3 "$disk" --label dos4gw-test --inodes 512
cp "$kernel" "$run/kernel.elf"
for probe in tsrtest tsrload tsrc31 tsrc27 tsrrom tsrmid; do
    tools/ositofs/ositofs-write3 "$disk" "arch/x86/build/test/$probe.com" --dest "$probe.com"
done
for probe in umblink xmsrm xmsp16 xmsp32 xmsps16 a20rm a20p16 a20p32 a20ps16 bmovrm bmov16 bmov32 bmovs32 dpnif16 dpnif32 dpcp16 dpcp32 dpcpc32 dpirqoff; do
    tools/ositofs/ositofs-write3 "$disk" "arch/x86/build/test/$probe.com" --dest "$probe.com"
done
for probe in dofprm dofp16 dofp32 dofpe32 dpfp16 dpfp32 dpfpr16 dpfpr32 dpfpc32 dpfd16 dpfd32 dpfdr16 dpfdr32 dpfm16 dpfm32 dpfmi32 dpmf16 dpmf32 dpdr16 dpdr32 dpdrs16 dpdrs32 dpst16 dpst32 dpsts16 dpsts32 dpsth32 dpsths16 dpsti32 dpstis16 dpdec16 dpdec32 dpdecs16 dpdecs32 dpirq32 dpirqs16 dpirqh32 dpirqhs dpirqd32 dpirqds; do
    tools/ositofs/ositofs-write3 "$disk" "arch/x86/build/test/$probe.com" --dest "$probe.com"
done
for probe in dpxd16 dpxd32 dpxdp16 dpxdp32 dpxdc; do
    tools/ositofs/ositofs-write3 "$disk" "arch/x86/build/test/$probe.com" --dest "$probe.com"
done
for probe in dxuc16 dxuc32 dxup16 dxup32 dxus16 dxus32 dxun16 dxun32 dxuns16 dxuns32 dxul16 dxul32 dxuls16 dxuls32 dxum16 dxum32 dxums16 dxums32 droc16 droc32 drop16 drop32 dros16 dros32 dron16 dron32 drons16 drons32 dprw16 dprw32 dprws16 dprws32 dxcb16 dxcb32 dxbp16 dxbp32 dxbs16 dxbs32 dxbn16 dxbn32 dxbns16 dxbns32; do
    tools/ositofs/ositofs-write3 "$disk" "arch/x86/build/test/$probe.com" --dest "$probe.com"
done
if [ -n "${QEMU_EVIDENCE_DIR:-}" ]; then
    mkdir -p "$QEMU_EVIDENCE_DIR"
    cp "$kernel" "$QEMU_EVIDENCE_DIR/kernel.elf"
fi
for probe in dxrmc16 dxrmc32 dxrmp16 dxrmp32 dxrms16 dxrms32 dxrmn16 dxrmn32 dxrmns16 dxrmns32 dxc32 dxs32 dxcn16 dxcn32 dxcns16 dxcns32 dxsn16 dxsn32 dxsns16 dxsns32 dxcnew dxsnew dxcp16 dxcp32 dxcs16 dxcs32 dxsp16 dxsp32 dxss16 dxss32 dpxp16 dpxp32 dpxps16 dpxps32 dpxreal dpxleaf dpxnew dpce16 dpce32 dpces16 dpces32 dpre16 dpre32 dpres16 dpres32 dpee16 dpee32 dpees16 dpees32; do
    tools/ositofs/ositofs-write3 "$disk" "arch/x86/build/test/$probe.com" --dest "$probe.com"
done
for fixture in DOOM.EXE DOOM.WAD; do
    tools/ositofs/ositofs-write3 "$disk" "arch/x86/build/dos4gw-fixtures/$fixture" --dest "$fixture"
done
for probe in dprg16 dprg32 dprgs16 dprgs32 dpcr16 dpcr32 dpcrs16 dpcrs32 dpsr16 dpsr32 dpsrs16 dpsrs32 dprs16 dprs32 dprss16 dprss32 dprec16 dprec32 dprecs16 dprecs32 dpnseg16 dpnseg32 dpsbnp32 dpsb16 dpsb32 dpsbs16 dpsbs32 dpsw16 dpsw32 dpswn16 dpswn32 dpargs16 dpargs32 dosbits dosjit dosivt dospop dprom16 dprom32 dpcon16 dpcon32 dpio16 dpio32 dpcrit16 dpcrit32 dpbreak16 dpbreak32 dpvgamem dpmi_vbe dosvga doskey dpkey16 dpkey32 dpmi_audio dpflags16 dpflags32 dpdebug16 dpdebug32; do
    tools/ositofs/ositofs-write3 "$disk" "arch/x86/build/test/$probe.com" --dest "$probe.com"
done
for probe in dosexec.com dexchild.com dexmz.exe dexdpmi.com dpstr16.com dpstr32.com dpex16.com dpex32.com dpxf16.com dpxf32.com dpfault.com dpfalt32.com dpff16.com dpff32.com dplock16.com dplock32.com dpstate16.com dpstate32.com dpcbuf16.com dpcbuf32.com; do
    tools/ositofs/ositofs-write3 "$disk" "arch/x86/build/test/$probe" --dest "$probe"
done
tools/ositofs/ositofs-write3 "$disk" arch/x86/build/doom-audio.cfg --dest audio.cfg
for probe in seh3_pe32.exe seh4_pe32.exe; do
    tools/ositofs/ositofs-write3 "$disk" "arch/x86/build/test-pe32/$probe" --dest "$probe"
done
tools/ositofs/ositofs-write3 "$disk" arch/x86/build/dos4gw.autoload --dest .autoload
export QEMU_KERNEL_BIN="$run/kernel.elf"
export QEMU_NVME_IMG="$disk"
export QEMU_MEM=8G QEMU_SMP=4
export QEMU_VNC=${QEMU_VNC:-:12} QEMU_UDP_PORT=${QEMU_UDP_PORT:-7790}
export QEMU_USB_POINTER=mouse QEMU_PID_FILE="$run/qemu.pid"
export QEMU_MONITOR_SOCKET="$run/monitor.sock"
export QEMU_SERIAL_SOCKET="$run/serial.sock" QEMU_SERIAL_LOG="$run/serial.log"
export QEMU_AUDIO_PATH="${QEMU_EVIDENCE_DIR:-$run}/audio.wav"
export QEMU_EXTRA_ARGS="-snapshot -qmp unix:$run/qmp.sock,server=on,wait=off"
extra_args=()
if [ "${3:-}" = "--no-kvm" ]; then
    extra_args+=(--no-kvm)
fi
exec bash arch/x86/scripts/qemu-test.sh --no-build --no-gl --vnc "${extra_args[@]}"
