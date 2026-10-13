#!/bin/sh
set -eu

bpp=$1
tmp=${TMPDIR:-/tmp}/bpp-native-runtime-$$
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# Control flow and user calls must remain native. Dynamic value and checked
# memory leaves are allowed; the retired VM/C-output routes are not.
source=tests/raw_core.bpp
for target in linux-x86_64 macos-arm64 linux-arm64 windows-x86_64; do
    asm="$tmp/$target.s"
    "$bpp" --target "$target" --emit-asm -o "$asm" "$source"
    if grep -E 'bl_vm_|bpp_a_run' "$asm" >/dev/null; then
        printf '%s\n' "native boundary regression: $target retained an interpreter entry point" >&2
        exit 1
    fi
    grep -E 'L_proc_[0-9]+' "$asm" >/dev/null
    grep -E '\.globl (_)?main' "$asm" >/dev/null
    grep -E 'bpp_a_op' "$asm" >/dev/null
    grep -E 'bpp_n_allocate_zero' "$asm" >/dev/null
done

printf '%s\n' 'return "not a raw value";' > "$tmp/quoted.bpp"
if "$bpp" --check "$tmp/quoted.bpp" >"$tmp/out" 2>"$tmp/err"; then
    printf '%s\n' 'raw native boundary regression: quoted runtime value compiled' >&2
    exit 1
fi
grep -F 'quoted text is only valid in compile-time directives' "$tmp/err" >/dev/null

printf '%s\n' 'Native control-flow/runtime boundary checks passed'
