#!/bin/sh
# Execute Windows x64 argument passing on a Linux x64 CPU. OS behavior is not emulated.
set -eu
mode=compile
print_flag=
debug_flag=
opt_flag=
output=a.out
source_file=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --backend) shift 2;;
        --check|--emit-ir|--emit-c|--emit-asm|--emit-map) mode=$1; shift;;
        --print-result) print_flag=--print-result; shift;;
        --debug-info) debug_flag=--debug-info; shift;;
        --no-opt) opt_flag=--no-opt; shift;;
        -o) output=$2; shift 2;;
        *) source_file=$1; shift;;
    esac
done
if [ "$mode" = --check ]; then exec "$BPP_REAL_COMPILER" --check "$source_file"; fi
if [ "$mode" != compile ]; then
    exec "$BPP_REAL_COMPILER" "$mode" --target windows-x86_64 ${print_flag:+"$print_flag"} ${debug_flag:+"$debug_flag"} ${opt_flag:+"$opt_flag"} -o "$output" "$source_file"
fi
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM
"$BPP_REAL_COMPILER" --emit-asm --target windows-x86_64 ${print_flag:+"$print_flag"} ${debug_flag:+"$debug_flag"} ${opt_flag:+"$opt_flag"} -o "$task_dir/windows.s" "$source_file"
awk '/^[[:space:]]*\.seh_/ {next} {gsub(/\.section \.rdata,"dr"/, ".section .rodata"); print} END {print ".section .note.GNU-stack,\"\",@progbits"}' "$task_dir/windows.s" > "$task_dir/host.s"
"${BPP_MSABI_CC:-cc}" -x assembler "$task_dir/host.s" -x none "$BPP_MSABI_DIR/runtime.o" "$BPP_MSABI_DIR/value.o" "$BPP_MSABI_DIR/codegen_runtime.o" "$BPP_MSABI_DIR/probe.o" -o "$output"
