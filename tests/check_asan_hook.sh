#!/bin/sh
set -eu
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM
if "$1" > "$task_dir/out" 2> "$task_dir/err"; then echo 'ASAN hook failed to detect poisoned global access'; exit 1; fi
grep -F 'AddressSanitizer: heap-buffer-overflow' "$task_dir/err" >/dev/null
echo 'Native global ASAN hook detected the poisoned access'
