#!/bin/bash
#
# rog5-app.sh -- build Osito-K programs on the Mac and copy them to the phone
#
# The programs are arch/arm/apps/<name>.c, linked against arch/arm/libc into
# static PIEs (see arch/arm/libc/Makefile). The Mac has the aarch64
# toolchain, like for rog5-boot.sh. The ELFs land in arch/arm/build/apps/,
# then go onto the phone's OsitoFS over USB (rog5-put.ps1).
#
# Usage (Git Bash / MSYS2 on Windows):
#   arch/arm/scripts/rog5-app.sh                    # build every app, copy them
#   arch/arm/scripts/rog5-app.sh hello primes       # only those
#   arch/arm/scripts/rog5-app.sh --run hello a b    # build and copy hello, then
#                                                   # run `exec hello.elf a b`
#   arch/arm/scripts/rog5-app.sh --build-only       # no phone needed
#
# Environment: OK_MAC, OK_MAC_JUMP, OK_MAC_DIR as for rog5-boot.sh.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ARCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO="$(cd "$ARCH_DIR/../.." && pwd)"
OUT_DIR="$ARCH_DIR/build/apps"

MAC="${OK_MAC:-cortessamuel@192.168.1.7}"
JUMP="${OK_MAC_JUMP-xasko@kerberus}"
MAC_DIR="${OK_MAC_DIR:-osito-arm-build}"

info() { printf '\033[1;36m[apps]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[apps] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

RUN=0; PUT=1; APPS=()
case "$1" in
    --run)        RUN=1; shift; [ -n "$1" ] || die "--run needs an app"; APPS=("$1"); shift ;;
    --build-only) PUT=0; shift; APPS=("$@"); set -- ;;
    -h|--help)    sed -n '3,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *)            APPS=("$@"); set -- ;;
esac
RUN_ARGS=("$@")
for a in "${APPS[@]}"; do
    [ -f "$ARCH_DIR/apps/$a.c" ] || die "no arch/arm/apps/$a.c"
done

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=15)
QUIET_KEX=()
ssh -G -o WarnWeakCrypto=no localhost >/dev/null 2>&1 && QUIET_KEX=(-o WarnWeakCrypto=no)
SSH_OPTS+=("${QUIET_KEX[@]}")
[ -n "$JUMP" ] && SSH_OPTS+=(-o "ProxyCommand=ssh ${QUIET_KEX[*]} -o BatchMode=yes -W %h:%p $JUMP")
mac() { ssh "${SSH_OPTS[@]}" "$MAC" "$@"; }

# --- Build on the Mac ---------------------------------------------------------
cd "$REPO"
info "Building ${APPS[*]:-every app} on $MAC..."
tar czf - arch/arm/libc arch/arm/apps |
    mac "rm -rf ~/$MAC_DIR/apps-src && mkdir -p ~/$MAC_DIR/apps-src && tar xzf - -C ~/$MAC_DIR/apps-src"
BUILT="$(mac "export PATH=/opt/homebrew/bin:/usr/local/bin:\$PATH
    cd ~/$MAC_DIR/apps-src/arch/arm/libc &&
    make -s CROSS=aarch64-elf- READELF=aarch64-elf-readelf ${APPS:+APPS=\"${APPS[*]}\"} > ../build.log 2>&1 &&
    make -s CROSS=aarch64-elf- ${APPS:+APPS=\"${APPS[*]}\"} check >> ../build.log 2>&1 &&
    ls ../build/apps" || { mac "cat ~/$MAC_DIR/apps-src/arch/arm/build.log" >&2; die "build failed"; })"

mkdir -p "$OUT_DIR"
for f in $BUILT; do
    mac "cat ~/$MAC_DIR/apps-src/arch/arm/build/apps/$f" > "$OUT_DIR/$f"
done
info "Built: $BUILT (arch/arm/build/apps/)"
[ "$PUT" = 1 ] || exit 0

# --- Copy to the phone, run ------------------------------------------------------
winpath() { cygpath -w "$1" 2>/dev/null || echo "$1"; }
for f in $BUILT; do
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(winpath "$SCRIPT_DIR/rog5-put.ps1")" \
        -File "$(winpath "$OUT_DIR/$f")" || die "copying $f failed"
done
if [ "$RUN" = 1 ]; then
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(winpath "$SCRIPT_DIR/rog5-console.ps1")" \
        -Command "exec ${APPS[0]}.elf ${RUN_ARGS[*]}" -Expect 'shell> $' -Timeout 30
fi
