#!/bin/sh
# Native instruction-selection benchmark. It compares normal selection with
# legal generic/materialized sequences forced only for this compiler process.
set -eu

compiler=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
clock=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
samples=${BPP_BENCHMARK_SAMPLES:-11}
runs=${BPP_BENCHMARK_RUNS:-1}
case "$samples:$runs" in *[!0-9:]*|0:*|*:0) echo "BPP_BENCHMARK_SAMPLES and BPP_BENCHMARK_RUNS must be positive integers" >&2; exit 2;; esac
task_dir=$(mktemp -d)
if command -v cygpath >/dev/null 2>&1; then task_dir=$(cygpath -m "$task_dir"); fi
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM

clean_field() { printf '%s' "$1" | tr '\r\n,' '   '; }
host_os=$(clean_field "$(uname -s 2>/dev/null || echo unknown)")
host_arch=$(clean_field "$(uname -m 2>/dev/null || echo unknown)")
case "$host_os:$host_arch" in
    Darwin:arm64|Darwin:aarch64) target=macos-arm64 ;;
    Linux:x86_64|Linux:amd64) target=linux-x86_64 ;;
    Linux:arm64|Linux:aarch64) target=linux-arm64 ;;
    MINGW*:x86_64|MSYS*:x86_64|CYGWIN*:x86_64) target=windows-x86_64 ;;
    *) target=unknown ;;
esac
if [ "$(uname -s 2>/dev/null || true)" = Darwin ]; then
    cpu=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || sysctl -n hw.model 2>/dev/null || echo unknown)
elif [ -r /proc/cpuinfo ]; then
    cpu=$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p;s/^Hardware[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -n 1)
else
    cpu=unknown
fi
cpu=$(clean_field "$cpu")
host_cc=${BPP_BENCHMARK_CC:-${CC:-cc}}
compiler_info=$(clean_field "$("$host_cc" --version 2>/dev/null | head -n 1 || echo unknown)")

median() { sort -n | awk '{a[NR]=$1} END { if (!NR) exit 1; print a[int((NR+1)/2)] }'; }
mad() { center=$1; awk -F, -v center="$center" '{ d=$1-center; if (d<0) d=-d; print d }' | median; }
measure() {
    executable=$1
    output=$2
    : > "$output"
    i=0
    while [ "$i" -lt "$samples" ]; do
        "$clock" "$runs" "$executable" >> "$output"
        i=$((i + 1))
    done
}
text_bytes() {
    file=$1
    if size -A "$file" >/dev/null 2>&1; then
        size -A "$file" | awk '$1 == ".text" || $1 == "__text" { print $2; exit }'
    elif size -m "$file" >/dev/null 2>&1; then
        size -m "$file" | awk '/Section __text:/ { print $3; exit }'
    fi
}

printf 'schema,case,selection,target,host_os,host_arch,cpu,compiler_identity,executable_bytes,text_bytes,assembly_source_bytes,sample_count,runs_per_sample,median_wall_ms,mad_wall_ms,median_cycles,mad_cycles,cycle_source\n'
for name in immediate_add immediate_compare calls; do
    case "$name" in
        immediate_add) code='new int i=0;new int sum=0;while(i<400000){sum=sum+4096;i=i+1;}assert(sum==1638400000);';;
        immediate_compare) code='new int i=0;new int sum=0;while(i<400000){if(i<4096){sum=sum+1;}else{sum=sum+2;}i=i+1;}assert(sum==795904);';;
        calls) code='define proc step(a,b,c,d){return (a+b)*(c+d);}new int i=0;new int sum=0;while(i<10000){sum=sum+step(i,2,3,4);i=i+1;}assert(sum==350105000);';;
    esac
    printf '%s\n' "$code" > "$task_dir/case.bpp"
    for selection in selected generic; do
        assembly="$task_dir/$name-$selection.s"
        executable="$task_dir/$name-$selection"
        if [ "$selection" = generic ]; then
            BPP_ISEL_FORCE_GENERIC=1 "$compiler" --backend native --emit-asm -o "$assembly" "$task_dir/case.bpp"
            BPP_ISEL_FORCE_GENERIC=1 "$compiler" --backend native -o "$executable" "$task_dir/case.bpp"
        else
            "$compiler" --backend native --emit-asm -o "$assembly" "$task_dir/case.bpp"
            "$compiler" --backend native -o "$executable" "$task_dir/case.bpp"
        fi
        [ -f "$executable" ] || executable="$executable.exe"
        measurements="$task_dir/$name-$selection.csv"
        measure "$executable" "$measurements"
        median_ms=$(cut -d, -f1 "$measurements" | median)
        mad_ms=$(cut -d, -f1 "$measurements" | mad "$median_ms")
        cycle_source=$(cut -d, -f3 "$measurements" | sort -u | tr '\n' '+' | sed 's/+$//')
        cycles=$(cut -d, -f2 "$measurements" | sed '/^$/d')
        if [ -n "$cycles" ]; then
            median_cycles=$(printf '%s\n' "$cycles" | median)
            mad_cycles=$(printf '%s\n' "$cycles" | awk -v center="$median_cycles" '{ d=$1-center; if(d<0)d=-d; print d }' | median)
        else
            median_cycles=
            mad_cycles=
        fi
        executable_bytes=$(wc -c < "$executable" | tr -d ' ')
        text_size=$(text_bytes "$executable" || true)
        assembly_bytes=$(wc -c < "$assembly" | tr -d ' ')
        printf '1,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$name" "$selection" "$target" "$host_os" "$host_arch" "$cpu" "$compiler_info" \
            "$executable_bytes" "$text_size" "$assembly_bytes" "$samples" "$runs" "$median_ms" "$mad_ms" \
            "$median_cycles" "$mad_cycles" "$cycle_source"
    done
done
