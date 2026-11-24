#!/bin/bash
LOG="${1:-/root/osito-run/serial.log}"
echo "lines=$(wc -l < "$LOG" 2>/dev/null)"
echo "crashes=$(grep -c 'Process crashed' "$LOG" 2>/dev/null)"
echo "exceptions=$(grep -c 'EXCEPTION:' "$LOG" 2>/dev/null)"
echo "max_int2e_depth=$(grep -oE 'depth=[0-9]+' "$LOG" 2>/dev/null | grep -oE '[0-9]+' | sort -n | tail -1)"
echo "max_seh_frame=$(grep -oE 'SEH32\] frame [0-9]+' "$LOG" 2>/dev/null | grep -oE '[0-9]+$' | sort -n | tail -1)"
echo "reached_menu=$(grep -c 'KeyEvent\|RegisterClassExW\|PeekMessage' "$LOG" 2>/dev/null)"
