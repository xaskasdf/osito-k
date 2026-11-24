#!/bin/bash
# Open the UT99 console and type "open <map>"+Enter via the QEMU monitor.
# Usage: ut_console_open.sh <mapname>  (default DM-Deck16, a map not in the image)
MAP="${1:-DM-Deck16}"
MON=/mnt/c/Users/xasko/osito-k/arch/x86/scripts/qemu_mon.py
H=127.0.0.1; P=55555

# map a single char to a qemu sendkey keyname
keyname() {
  case "$1" in
    [a-z]) echo "$1" ;;
    [0-9]) echo "$1" ;;
    "-") echo "minus" ;;
    "_") echo "shift-minus" ;;
    ".") echo "dot" ;;
    " ") echo "spc" ;;
    *) echo "" ;;
  esac
}

# 1) open console (grave/backtick). Send twice in case first toggles a small bar.
python3 "$MON" "$H" "$P" "sendkey grave"
sleep 0.4

# 2) type "open <MAP>" lowercased (UT console is case-insensitive)
STR=$(echo "open $MAP" | tr 'A-Z' 'a-z')
for (( i=0; i<${#STR}; i++ )); do
  ch="${STR:$i:1}"
  kn=$(keyname "$ch")
  [ -n "$kn" ] && python3 "$MON" "$H" "$P" "sendkey $kn"
done

# 3) Enter
python3 "$MON" "$H" "$P" "sendkey ret"
echo "typed: open $MAP"
