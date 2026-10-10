#!/bin/sh
set -eu

compiler=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
project_compiler=${BPP_REAL_COMPILER:-$compiler}
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM
count=0
progress() { count=$((count + 1)); printf '[%02d] %s\n' "$count" "$1" >&2; }
bpp() { "$compiler" --backend "${BPP_TEST_BACKEND:-native}" "$@"; }

case_run() {
    progress "$1"
    printf '%s\n' "$2" > "$task_dir/case.bpp"
    bpp --print-result -o "$task_dir/program" "$task_dir/case.bpp"
    actual=$("$task_dir/program")
    if [ "$actual" != "$3" ]; then
        printf 'FAIL %s: got <%s>, expected <%s>\n' "$1" "$actual" "$3" >&2
        exit 1
    fi
}

compile_error() {
    progress "$1"
    printf '%s\n' "$2" > "$task_dir/error.bpp"
    if bpp --check "$task_dir/error.bpp" >"$task_dir/out" 2>"$task_dir/err"; then
        printf 'FAIL %s: expected compilation failure\n' "$1" >&2; exit 1
    fi
    grep -F "$3" "$task_dir/err" >/dev/null
}

runtime_failure() {
    progress "$1"
    printf '%s\n' "$2" > "$task_dir/error.bpp"
    bpp -o "$task_dir/program" "$task_dir/error.bpp"
    if "$task_dir/program" >"$task_dir/out" 2>"$task_dir/err"; then
        printf 'FAIL %s: expected non-zero native exit\n' "$1" >&2; exit 1
    fi
}

case_run recursion 'define proc fact(n){if(n<=1){return 1;}return n*fact(n-1);} define proc main(){return fact(6);}' '720'
case_run conventional_main 'new byte seed=40; define proc main(){return seed+2;}' '42'
case_run loop 'define proc main(){new byte n=1000;new byte x=0;while(n>0){x=x+1;n=n-1;}return x;}' '1000'
case_run shadow 'new byte x=8;define proc add(x){return x+1;}define proc main(){return add(4)+x;}' '13'
case_run signed_arithmetic 'define proc main(){return -7/3 + (-7%3) + 12;}' '9'
case_run comparisons 'define proc main(){return (3==3)+(3!=4)+(2<3)+(3<=3)+(4>3)+(4>=4);}' '6'
case_run branches 'define proc main(){new byte x=1;if(!x){x=9;}else{x=3;}return x;}' '3'
case_run many_arguments 'define proc sum(a,b,c,d,e,f,g,h,i){return a+b+c+d+e+f+g+h+i;}define proc main(){return sum(1,2,3,4,5,6,7,8,9);}' '45'
case_run recursive_pressure 'define proc f(n,a,b,c,d,e,f,g,h){if(n==0){return a+b+c+d+e+f+g+h;}return (a+b)*(c+d)+(e+f)*(g+h)+f(n-1,a,b,c,d,e,f,g,h);}define proc main(){return f(4,1,2,3,4,5,6,7,8);}' '780'
case_run raw_memory 'define proc main(){new ptr p=malloc 2;p[0]=65;p[1]=42;p=remalloc p 8;new byte r=p[1];print p,1;print r;free p;delete p;return r;}' 'A*42'
case_run raw_alias_call 'define proc write(p){p[0]=66;return 0;}define proc main(){new ptr p=malloc 1;p[0]=65;new byte before=p[0];write(p);new byte after=p[0];free p;delete p;return before+after;}' '131'
printf '%s\n' 'define proc twice(n){return n*2;}' > "$task_dir/lib.bpp"
case_run imports 'import "lib.bpp";
define proc main(){return twice(21);}' '42'

compile_error quoted_runtime_value 'define proc main(){return "x";}' 'quoted text is only valid in compile-time directives'
compile_error legacy_realloc 'define proc main(){new ptr p=realloc 4;return 0;}' "expected ';' after variable declaration"
compile_error old_keyword 'define fn f(){return 1;}' 'expected macro or proc'
compile_error main_requires_zero_args 'define proc main(x){return x;}' 'must take zero arguments'
compile_error unknown_identifier 'define proc main(){return missing;}' 'undefined variable'
compile_error duplicate_local 'define proc main(){new byte x=1;new byte x=2;return x;}' 'duplicate declaration'
compile_error integer_literal_range 'define proc main(){return 9223372036854775808;}' 'outside the signed 64-bit range'
runtime_failure division_by_zero 'define proc main(){return 1/0;}'
runtime_failure uninitialized 'define proc main(){if(0){new byte x=1;}return x;}'

progress 'output modes'
printf '%s\n' 'define proc main(){return 7;}' > "$task_dir/modes.bpp"
bpp --emit-ir -o "$task_dir/out.ir" "$task_dir/modes.bpp"
grep -F 'function' "$task_dir/out.ir" >/dev/null
bpp --emit-c -o "$task_dir/out.c" "$task_dir/modes.bpp"
grep -F 'int main' "$task_dir/out.c" >/dev/null
bpp --emit-asm -o "$task_dir/out.s" "$task_dir/modes.bpp"
grep -E '\.globl (_)?main' "$task_dir/out.s" >/dev/null
if grep -E 'BL(Value|Runtime)|bpp_[an]_' "$task_dir/out.s" >/dev/null; then
    echo 'FAIL output modes: raw assembly retained legacy runtime boundary' >&2; exit 1
fi
bpp --emit-map -o "$task_dir/out.json" "$task_dir/modes.bpp"
grep -F '"source_files"' "$task_dir/out.json" >/dev/null

printf '%s\n' "Passed $count raw native and diagnostic cases, plus output-mode checks"
