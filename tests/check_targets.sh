#!/bin/sh
# Assemble, but do not link: no foreign SDK/runtime libraries are required.
set -eu
compiler=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
task_dir=$(mktemp -d)
if command -v cygpath >/dev/null 2>&1; then task_dir=$(cygpath -m "$task_dir"); fi
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM
for target in macos-arm64 linux-x86_64 linux-arm64 windows-x86_64; do
    case "$target" in
        macos-arm64) triple=arm64-apple-macosx13.0.0;;
        linux-x86_64) triple=x86_64-unknown-linux-gnu;;
        linux-arm64) triple=aarch64-unknown-linux-gnu;;
        windows-x86_64) triple=x86_64-w64-windows-gnu;;
    esac
    for optimize in optimized baseline; do
        if [ "$optimize" = baseline ]; then set -- --no-opt; else set --; fi
        "$compiler" --emit-asm --target "$target" --debug-info "$@" -o "$task_dir/$target.s" tests/arm64_coverage.bpp
        "${BPP_ASSEMBLER:-clang}" -target "$triple" -g -x assembler -c "$task_dir/$target.s" -o "$task_dir/$target.o"
    done
    "$compiler" --emit-map --target "$target" -o "$task_dir/$target.json" tests/arm64_coverage.bpp
    grep -F "\"target\":\"$target\"" "$task_dir/$target.json" >/dev/null
done
echo 'Cross-assembled all four targets in optimized and baseline modes'
