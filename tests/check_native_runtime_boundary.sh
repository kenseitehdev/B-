#!/bin/sh
set -eu

bpp=$1
tmp=${TMPDIR:-/tmp}/bpp-native-runtime-$$
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# This is deliberately raw source: native output must contain neither the old
# tagged-value bridge nor any object management helper on every target.
source=tests/raw_core.bpp
for target in linux-x86_64 macos-arm64 linux-arm64 windows-x86_64; do
    asm="$tmp/$target.s"
    "$bpp" --target "$target" --emit-asm -o "$asm" "$source"
    if grep -E 'BL(Value|Runtime)|bpp_[an]_' "$asm" >/dev/null; then
        printf '%s\n' "raw native boundary regression: $target retained a legacy runtime symbol" >&2
        exit 1
    fi
    grep -E 'malloc|_malloc' "$asm" >/dev/null
    grep -E 'realloc|_realloc' "$asm" >/dev/null
    grep -E 'free|_free' "$asm" >/dev/null
    grep -E 'write|_write' "$asm" >/dev/null
done

printf '%s\n' 'return "not a raw value";' > "$tmp/quoted.bpp"
if "$bpp" --check "$tmp/quoted.bpp" >"$tmp/out" 2>"$tmp/err"; then
    printf '%s\n' 'raw native boundary regression: quoted runtime value compiled' >&2
    exit 1
fi
grep -F 'quoted text is only valid in compile-time directives' "$tmp/err" >/dev/null

printf '%s\n' 'Raw native runtime boundary checks passed'
