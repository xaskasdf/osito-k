#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
bash arch/x86/scripts/build-dll-lifecycle-test.sh "$out"
for bits in 32 64; do
    dir="$out/pe$bits"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc;machine=x86;dll_machine=i386;entry='dependency_main@12'
    else
        target=x86_64-pc-windows-msvc;machine=x64;dll_machine=i386:x86-64;entry=dependency_main
    fi
    printf 'CreateEventA@16\nOpenEventA@12\nSetEvent@4\nResetEvent@4\n' >> "$dir/kernel32.def"
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    printf 'int dependency_forwarder_marker;\n' > "$dir/forward.c"
    clang --target="$target" -ffreestanding -c "$dir/forward.c" -o "$dir/forward.obj"
    # /export preserves the forwarder target on x86; a .def alias is decorated.
    lld-link /dll /noentry /nodefaultlib /machine:"$machine" /base:0x32000000 /export:lifecycle_value=lifecycle-fail.lifecycle_value "/out:$dir/dependency-forwarder.dll" "$dir/forward.obj"
    printf 'LIBRARY dependency-huge.dll\nEXPORTS\nlifecycle_value\n' > "$dir/huge.def"
    llvm-dlltool -m "$dll_machine" -d "$dir/huge.def" -l "$dir/dependency-huge.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
        -c arch/x86/test/load_dependency_dll.c -o "$dir/parent.obj"
    for kind in good fail forward oom; do
        case "$kind" in
            good|fail) dependency="$dir/lifecycle-$kind.lib";;
            forward) dependency="$dir/dependency-forwarder.lib";;
            oom) dependency="$dir/dependency-huge.lib";;
        esac
        # Keep dependencies at their preferred base. The x64 lifecycle fixture
        # is position independent and has no relocation table.
        lld-link /dll /nodefaultlib /machine:"$machine" /base:0x31000000 "/entry:$entry" \
            "/out:$dir/dependency-$kind.dll" "$dir/parent.obj" "$dir/kernel32.lib" "$dependency"
    done
    for variant in normal oom direct; do
        options=()
        [[ $variant == oom ]] && options=(-DLOADDEP_OOM)
        [[ $variant == direct ]] && options=(-DLOADDEP_OOM -DLOADDEP_NO_FORWARD)
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
            "${options[@]}" -c arch/x86/test/load_dependency_pe.c -o "$dir/probe-$variant.obj"
        lld-link /entry:load_dependency_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
            "/out:$dir/loaddep$bits-$variant.exe" "$dir/probe-$variant.obj" "$dir/kernel32.lib"
    done
done
python3 arch/x86/scripts/make-load-dependency-oom.py "$out"
