#!/bin/sh
set -eu
compiler=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
generator=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
task_dir=$(mktemp -d)
if command -v cygpath >/dev/null 2>&1; then task_dir=$(cygpath -m "$task_dir"); fi
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM
seed=1
while [ "$seed" -le "${BPP_DIFFERENTIAL_CASES:-64}" ]; do
    "$generator" "$seed" > "$task_dir/case.bpp"
    "$generator" --expected "$seed" > "$task_dir/expected"
    for optimize in optimized baseline; do
        if [ "$optimize" = baseline ]; then set -- --no-opt; else set --; fi
        "$compiler" --backend native --print-result "$@" -o "$task_dir/native" "$task_dir/case.bpp"
        "$task_dir/native" > "$task_dir/actual"
        if ! cmp "$task_dir/expected" "$task_dir/actual"; then
            printf 'Differential failure seed=%s mode=%s\n' "$seed" "$optimize"; cat "$task_dir/case.bpp"; exit 1
        fi
    done
    seed=$((seed+1))
done
printf 'Passed %s independently evaluated seeds in optimized and baseline native modes\n' "$((seed-1))"
