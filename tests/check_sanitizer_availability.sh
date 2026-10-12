#!/bin/sh
# Probe the actual configured host driver, linker and runtime. An unsupported
# flavor is recorded explicitly; this does not turn it into a release promise.
set -eu
cc=$1
work=${TMPDIR:-/tmp}/bpp-sanitizer-availability-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"
cat >"$work/probe.c" <<'EOF'
#include <stdlib.h>
int main(void) { volatile int *p=calloc(1,sizeof *p); if(!p)return 2; *p=42; int result=*p; free((void *)p); return result!=42; }
EOF
printf 'host=%s compiler=%s\n' "$(uname -s)/$(uname -m)" "$cc"
"$cc" --version | head -1
for flavor in address leak undefined thread memory; do
    if [ "$flavor" = memory ] && [ "$(uname -s)" != Linux ]; then
        printf '%s: unsupported_by_bpp_on_this_host\n' "$flavor"; continue
    fi
    if "$cc" -O1 -g -fPIE -pie "-fsanitize=$flavor" "$work/probe.c" -o "$work/probe" >"$work/build.log" 2>&1; then
        if "$work/probe" >"$work/run.log" 2>&1; then
            printf '%s: runtime_smoke_passed\n' "$flavor"
        else
            printf '%s: runtime_execution_failed\n' "$flavor"
            cat "$work/run.log"
        fi
    else
        printf '%s: toolchain_unavailable\n' "$flavor"
        cat "$work/build.log"
    fi
done
