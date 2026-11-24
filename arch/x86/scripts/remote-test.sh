#!/bin/bash
#
# OsitoK x86-64 — corre los tests de host (scripts/test-*.sh) en una WSL remota.
#
# Los tests de host compilan su arnés (test/*_host.c) junto con fuentes reales
# del kernel usando gcc y clang de Linux, casi siempre con ASan/UBSan, cosa que
# msys2 no da. Esta máquina no tiene WSL: sincronizamos el árbol por SSH a la
# WSL de otra máquina Windows (kerberus) y los corremos ahí.
#
# El shell SSH de kerberus es PowerShell 5.1, que rompe comillas y `&&`. Por eso
# cada comando remoto viaja como un script en base64 a `wsl -e bash`; stdin y
# stdout pasan intactos (binary-safe), así que el árbol viaja como un tar.
#
# Uso (Git Bash o MSYS2):
#   arch/x86/scripts/remote-test.sh boot-diag gdi-screen   # test-boot-diag.sh + test-gdi-screen.sh
#   arch/x86/scripts/remote-test.sh --sync                 # solo sincroniza
#   arch/x86/scripts/remote-test.sh --full boot-diag       # borra la copia remota y la manda entera
#   arch/x86/scripts/remote-test.sh --no-sync boot-diag    # no sincroniza (re-run rápido)
#   arch/x86/scripts/remote-test.sh --exec 'make -C arch/x86 serial-host-test'
#
# Variables:
#   OK_REMOTE=xasko@kerberus   OK_REMOTE_DISTRO=Ubuntu-24.04
#   OK_REMOTE_DIR=osito-k      (relativo al $HOME de la WSL remota)
#
# Se sincronizan los archivos trackeados y los no ignorados de arch/x86,
# include/common, gui y tools/ositofs. Después de la primera vez solo viaja lo
# modificado desde la última sincronización (marca en arch/x86/build/). Los
# borrados locales no se propagan: usar --full.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
REMOTE="${OK_REMOTE:-xasko@kerberus}"
DISTRO="${OK_REMOTE_DISTRO:-Ubuntu-24.04}"
RDIR="${OK_REMOTE_DIR:-osito-k}"
STAMP="$ROOT/arch/x86/build/.remote-sync-${REMOTE//[^A-Za-z0-9]/_}"
SYNC_PATHS=(arch/x86 include/common gui tools/ositofs)

info() { printf '\033[1;36m[remote-test]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[remote-test] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[[ $RDIR =~ ^[A-Za-z0-9._/-]+$ ]]   || die "OK_REMOTE_DIR inválido: $RDIR"
[[ $DISTRO =~ ^[A-Za-z0-9._-]+$ ]]  || die "OK_REMOTE_DISTRO inválido: $DISTRO"

# Corre el script bash $1 en la WSL remota. stdin pasa tal cual; el exit code
# vuelve vía $LASTEXITCODE de PowerShell.
remote() {
    local b64
    b64=$(printf '%s\n' "$1" | base64 -w0)
    ssh -o BatchMode=yes -o WarnWeakCrypto=no "$REMOTE" \
        "wsl -d $DISTRO -e bash -c 'f=\$(mktemp); echo $b64 | base64 -d > \$f; bash \$f; rc=\$?; rm -f \$f; exit \$rc'; exit \$LASTEXITCODE"
}

sync_tree() {
    local full=$1 list marker prep="mkdir -p ~/$RDIR" incremental=1 n f
    list=$(mktemp); marker=$(mktemp)    # marker: hora de inicio, antes de listar
    if [ "$full" = 1 ] || [ ! -f "$STAMP" ]; then
        incremental=0
        [ "$full" = 1 ] && prep="rm -rf ~/$RDIR && mkdir -p ~/$RDIR"
        info "Sincronización completa -> $REMOTE:~/$RDIR"
    fi
    # Saltea los trackeados que no existen en el working tree y, en modo
    # incremental, los que no cambiaron desde la última marca.
    (cd "$ROOT" && git ls-files -z -co --exclude-standard -- "${SYNC_PATHS[@]}" |
        while IFS= read -r -d '' f; do
            [ -e "$f" ] || continue
            [ $incremental = 1 ] && ! [ "$f" -nt "$STAMP" ] && continue
            printf '%s\0' "$f"
        done) > "$list"
    n=$(tr -cd '\0' < "$list" | wc -c)
    if [ "$n" -eq 0 ] && [ $incremental = 1 ]; then
        info "Nada que sincronizar"
    else
        info "Enviando $n archivos..."
        (cd "$ROOT" && tar -c --null -T "$list" | gzip -1) |
            remote "$prep && tar -xz --warning=no-timestamp -C ~/$RDIR" ||
            die "falló la sincronización"
    fi
    mkdir -p "$(dirname "$STAMP")"
    touch -r "$marker" "$STAMP"
    rm -f "$list" "$marker"
}

full=0; do_sync=1; exec_cmd=""; tests=(); targs=()
while [ $# -gt 0 ]; do
    case $1 in
        --)        shift; targs=("$@"); break ;;
        --sync)    exec_cmd=":" ;;
        --full)    full=1 ;;
        --no-sync) do_sync=0 ;;
        --exec)    [ $# -ge 2 ] || die "--exec necesita un comando"; exec_cmd=$2; shift ;;
        -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
        -*)        die "opción desconocida: $1" ;;
        *)
            case $1 in
                test-*.sh|test-*.py) f=$1 ;;
                *) f=test-$1.sh; [ -f "$ROOT/arch/x86/scripts/$f" ] || f=test-$1.py ;;
            esac
            [[ $f =~ ^[A-Za-z0-9._-]+$ ]] && [ -f "$ROOT/arch/x86/scripts/$f" ] ||
                die "no existe arch/x86/scripts/$f"
            tests+=("$f") ;;
    esac
    shift
done
[ -n "$exec_cmd" ] || [ ${#tests[@]} -gt 0 ] || die "nada que hacer (ver --help)"
[ ${#targs[@]} -eq 0 ] || [ ${#tests[@]} -eq 1 ] || die "los argumentos después de -- son para un solo test"

[ "$do_sync" = 1 ] && sync_tree "$full"

if [ -n "$exec_cmd" ]; then
    [ "$exec_cmd" = ":" ] && exit 0
    exec_rc=0
    remote "cd ~/$RDIR || exit 2
$exec_cmd" || exec_rc=$?
    exit "$exec_rc"
fi

args_q=""
[ ${#targs[@]} -eq 0 ] || args_q=$(printf ' %q' "${targs[@]}")
calls=""
for f in "${tests[@]}"; do calls+="run_test $f$args_q"$'\n'; done

remote "cd ~/$RDIR || exit 2
make -s -C tools/ositofs >/dev/null 2>&1 || echo '[remote-test] aviso: falló make -C tools/ositofs' >&2
fail=0; total=0
run_test() {
    local f=\$1 s=\$SECONDS run=bash rc=0
    shift
    case \$f in *.py) run=python3 ;; esac
    total=\$((total + 1))
    printf '\n=== %s\n' \"\$f\"
    \$run \"arch/x86/scripts/\$f\" \"\$@\" || rc=\$?
    if [ \$rc -eq 0 ]; then
        echo \"=== PASS \$f (\$((SECONDS - s))s)\"
    else
        echo \"=== FAIL \$f (rc=\$rc, \$((SECONDS - s))s)\"
        fail=\$((fail + 1))
    fi
}
$calls
echo
echo \"=== \$((total - fail)) ok, \$fail fallaron\"
[ \$fail -eq 0 ]"
