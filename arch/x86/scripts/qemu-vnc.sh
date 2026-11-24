#!/bin/bash
set -e
BUILD_DIR="arch/x86/build"
NVME_ARGS="-drive file=$BUILD_DIR/nvme.img,format=raw,if=none,id=nvme0,cache=none -device nvme,serial=deadbeef,drive=nvme0"
ESP_IMG="$BUILD_DIR/esp.img"

# Copy VARS to be writable
cp /usr/local/Cellar/qemu/10.2.2/share/qemu/edk2-i386-vars.fd $BUILD_DIR/vars.fd
chmod +w $BUILD_DIR/vars.fd

qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/local/Cellar/qemu/10.2.2/share/qemu/edk2-x86_64-code.fd \
    -drive if=pflash,format=raw,file=$BUILD_DIR/vars.fd \
    -drive file="$ESP_IMG",format=raw,if=ide \
    $NVME_ARGS \
    -m 512M \
    -machine q35 \
    -smp 4 \
    -device e1000e,netdev=net0 \
    -netdev user,id=net0,hostfwd=udp::7779-:7777 \
    -device qemu-xhci,id=usb \
    -device usb-kbd,bus=usb.0 \
    -device usb-mouse,bus=usb.0 \
    -vnc :0,password=on \
    -serial file:"$BUILD_DIR/serial.log" \
    -monitor unix:/tmp/qemu-monitor.sock,server,nowait \
    -no-reboot &

sleep 5
if [ -S /tmp/qemu-monitor.sock ]; then
    echo "set_password vnc osito" | nc -U /tmp/qemu-monitor.sock
    echo "VNC Password set successfully"
fi
