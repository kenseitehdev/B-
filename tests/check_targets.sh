#!/bin/sh
# Assemble, but do not link: no foreign SDK/runtime libraries are required.
set -eu
compiler=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
task_dir=$(mktemp -d)
if command -v cygpath >/dev/null 2>&1; then task_dir=$(cygpath -m "$task_dir"); fi
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM
"$compiler" --list-targets > "$task_dir/targets.tsv"
target_count=0
while IFS="$(printf '\t')" read -r target triple format isa bits registers; do
    [ -n "$target" ] || continue
    target_count=$((target_count + 1))
    for optimize in optimized baseline; do
        if [ "$optimize" = baseline ]; then set -- --no-opt; else set --; fi
        "$compiler" --emit-asm --target "$target" --debug-info "$@" -o "$task_dir/$target.s" tests/raw_core.bpp
        "${BPP_ASSEMBLER:-clang}" -target "$triple" -g -x assembler -c "$task_dir/$target.s" -o "$task_dir/$target.o"
    done
    "$compiler" --emit-map --target "$target" -o "$task_dir/$target.json" tests/raw_core.bpp
    grep -F "\"target\":\"$target\"" "$task_dir/$target.json" >/dev/null
    "$compiler" --target-info "$target" > "$task_dir/$target.info"
    ! grep -F 'ir.' "$task_dir/$target.info" | grep -F '=unsupported' >/dev/null
done < "$task_dir/targets.tsv"
[ "$target_count" -gt 0 ]
IFS="$(printf '\t')" read -r comment_target _rest < "$task_dir/targets.tsv"
"$compiler" --emit-asm --target "$comment_target" -o "$task_dir/raw.s" tests/raw_core.bpp
if grep -E 'BL(Value|Runtime)|bpp_[an]_' "$task_dir/raw.s" >/dev/null; then
    echo 'cross-target raw assembly retained legacy runtime symbols' >&2
    exit 1
fi
echo "Cross-assembled $target_count queried targets in optimized and baseline modes"
