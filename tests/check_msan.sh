#!/bin/sh
set -eu

task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM

set +e
MSAN_OPTIONS='halt_on_error=1:exit_code=86' "$1" >"$task_dir/out" 2>"$task_dir/err"
status=$?
set -e

if [ "$status" -eq 0 ]; then
    echo 'MSAN runtime failed to detect an uninitialized stack load' >&2
    exit 1
fi
if [ "$status" -ne 86 ]; then
    cat "$task_dir/err" >&2
    echo "MSAN runtime exited with $status, expected 86" >&2
    exit 1
fi

grep -F 'MemorySanitizer: use-of-uninitialized-value' "$task_dir/err" >/dev/null
echo 'MemorySanitizer runtime detected the intentional uninitialized stack load'
