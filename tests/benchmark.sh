#!/bin/sh
set -eu
compiler=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
clock=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
task_dir=$(mktemp -d)
if command -v cygpath >/dev/null 2>&1; then task_dir=$(cygpath -m "$task_dir"); fi
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM
printf 'case,backend,executable_bytes,mean_ms_including_startup\n'
for name in loop calls objects; do
    case "$name" in
        loop) code='new int i=0;new int sum=0;while(i<200000){sum=sum+i;i=i+1;}assert(sum==19999900000);';;
        calls) code='define proc step(a,b,c,d){return (a+b)*(c+d);}new int i=0;new int sum=0;while(i<10000){sum=sum+step(i,2,3,4);i=i+1;}assert(sum==350105000);';;
        objects) code='new int i=0;while(i<10000){new Array a=Array.new(2);Array.set(a,0,"a");Array.set(a,1,"b");assert(Array.get(a,0)=="a");delete a;i=i+1;}';;
    esac
    printf '%s\n' "$code" > "$task_dir/case.bpp"
    for backend in c native native-baseline; do
        if [ "$backend" = native-baseline ]; then set -- --backend native --no-opt; else set -- --backend "$backend"; fi
        "$compiler" "$@" -o "$task_dir/program" "$task_dir/case.bpp"
        program="$task_dir/program";if [ -f "$program.exe" ]; then program="$program.exe"; fi
        bytes=$(wc -c < "$program" | tr -d ' ')
        ms=$("$clock" "${BPP_BENCHMARK_RUNS:-5}" "$program")
        printf '%s,%s,%s,%s\n' "$name" "$backend" "$bytes" "$ms"
    done
done
