#!/bin/sh
set -eu
compiler=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
generator=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
task_dir=$(mktemp -d)
if command -v cygpath >/dev/null 2>&1; then task_dir=$(cygpath -m "$task_dir"); fi
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM
n=${BPP_STRESS_COUNT:-5000}
"$generator" --stress "$n" > "$task_dir/large.bpp"
"$compiler" --backend native --print-result -o "$task_dir/program" "$task_dir/large.bpp"
actual=$("$task_dir/program")
if [ "$actual" != "$((n+3))" ]; then printf 'Stress output got %s, expected %s\n' "$actual" "$((n+3))"; exit 1; fi
"$compiler" --emit-map -o "$task_dir/map.json" "$task_dir/large.bpp"
grep -F '"name":"wide"' "$task_dir/map.json" >/dev/null
printf 'Passed large-procedure test with %s updates\n' "$n"
