#!/bin/sh
set -eu

bpp=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_dir="$root/build/strict-native-runtime-test"
rm -rf "$task_dir"
mkdir -p "$task_dir"
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM

source="$root/tests/raw_core.bpp"

case "$(uname -s):$(uname -m)" in
    Darwin:arm64|Darwin:aarch64) host_target=macos-arm64 ;;
    Linux:x86_64|Linux:amd64) host_target=linux-x86_64 ;;
    Linux:arm64|Linux:aarch64) host_target=linux-arm64 ;;
    MINGW*:x86_64|MSYS*:x86_64|CYGWIN*:x86_64) host_target=windows-x86_64 ;;
    *)
        printf '%s\n' "strict native runtime: unsupported host $(uname -s)/$(uname -m)" >&2
        exit 1
        ;;
esac

# Assemble and run only the executable format of this host. Apple Clang's
# integrated assembler accepts Mach-O ARM64, not ELF x86-64, so treating it as
# a Linux cross-assembler turns a valid strict profile into a false failure.
host_asm="$task_dir/$host_target.s"
host_exe="$task_dir/strict-native-host"
"$bpp" --strict-native-runtime --emit-asm --target "$host_target" -o "$host_asm" "$source"
! grep -E 'bpp_(a|n)_' "$host_asm"
cc -x assembler "$host_asm" -o "$host_exe"
"$host_exe" >"$task_dir/output"
grep -Fx 'A*' "$task_dir/output"

for target in macos-arm64 linux-x86_64 linux-arm64 windows-x86_64; do
    [ "$target" = "$host_target" ] && continue
    "$bpp" --strict-native-runtime --emit-asm --target "$target" -o "$task_dir/$target.s" "$source"
    ! grep -E 'bpp_(a|n)_' "$task_dir/$target.s"
done

"$bpp" --strict-native-runtime --print-result -o "$task_dir/with-result" "$source"
"$task_dir/with-result" >"$task_dir/result"
grep -Fx 'A*42' "$task_dir/result"

printf 'return "dynamic";\n' > "$task_dir/dynamic.bpp"
if "$bpp" --strict-native-runtime -o "$task_dir/nope" "$task_dir/dynamic.bpp" >"$task_dir/dynamic.out" 2>"$task_dir/dynamic.err"; then
    echo "strict native runtime unexpectedly accepted a dynamic value" >&2
    exit 1
fi
grep -F 'quoted text is only valid in compile-time directives' "$task_dir/dynamic.err"

printf '%s\n' 'Strict raw native-runtime profile passed'
