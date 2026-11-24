#!/bin/bash
L=/root/osito-run/serial.log
A=${1:-21540}
B=${2:-21720}
sed -n "${A},${B}p" "$L"
