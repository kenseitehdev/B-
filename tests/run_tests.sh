#!/bin/sh
set -eu
compiler=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
project_compiler=${BPP_REAL_COMPILER:-$compiler}
project_dir=$(cd "$(dirname "$project_compiler")/../.." && pwd)
task_dir=$(mktemp -d)
if command -v cygpath >/dev/null 2>&1; then task_dir=$(cygpath -m "$task_dir"); fi
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM
count=0
progress() { printf '[%02d] %s\n' "$count" "$1" >&2; }
bpp() { "$compiler" --backend "${BPP_TEST_BACKEND:-c}" "$@"; }
native_case() {
    count=$((count + 1))
    progress "$1"
    printf '%s\n' "$2" > "$task_dir/case.bpp"
    bpp --print-result -o "$task_dir/program" "$task_dir/case.bpp"
    actual=$("$task_dir/program")
    if [ "$actual" != "$3" ]; then printf 'FAIL %s: got <%s>, expected <%s>\n' "$1" "$actual" "$3"; exit 1; fi
}
compile_error() {
    count=$((count + 1))
    progress "$1"
    printf '%s\n' "$2" > "$task_dir/error.bpp"
    if bpp --check "$task_dir/error.bpp" > "$task_dir/out" 2> "$task_dir/err"; then echo "FAIL $1: expected compilation error"; exit 1; fi
    if ! LC_ALL=C grep -F "$3" "$task_dir/err" >/dev/null; then cat "$task_dir/err"; echo "FAIL $1"; exit 1; fi
}
runtime_error() {
    count=$((count + 1))
    progress "$1"
    printf '%s\n' "$2" > "$task_dir/error.bpp"
    bpp -o "$task_dir/program" "$task_dir/error.bpp"
    if "$task_dir/program" > "$task_dir/out" 2> "$task_dir/err"; then echo "FAIL $1: expected runtime error"; exit 1; fi
    if ! LC_ALL=C grep -F "$3" "$task_dir/err" >/dev/null; then cat "$task_dir/err"; echo "FAIL $1"; exit 1; fi
}
native_case recursion 'return fact(6); define proc fact(n) { if(n <= 1) {return 1;} else {return n * fact(n - 1);} }' '720'
native_case loop 'new int x=0; while(x<1000){x=x+1;} return x;' '1000'
native_case shadow 'new int x=8; define proc f(x){return x+1;} return f(4)+x;' '13'
expected_local_slot=$(printf 'old\n7\n0')
native_case local_slot_reuse 'define proc f(){new String a="old";println(a);new int b=7;println(b);return 0;} return f();' "$expected_local_slot"
native_case strings 'new String s="ab"; new String t="cd"; new String u=String.concat(s,t); assert(String.cmp(u,"abcd")==0); return String.length(u);' '4'
native_case integer_to_decimal_buffer 'new String value=12345; assert(String.cmp(value,"12345")==0); return String.length(value);' '5'
native_case string_new_integer_byte 'return string_new(65);' 'A'
native_case buffer 'new ptr b=malloc 4; b[0]=65; new ptr alias=b; free b; assert(len(alias)==0); b=realloc b 4; b[0]=66; return b[0];' '66'
native_case arrays 'new Array a=Array.new(3); Array.set(a,0,"a"); Array.set(a,1,"b"); Array.set(a,2,"c"); Array.grow(a,1); assert(Array.get(a,0)=="a"); Array.grow(a,4); Array.set(a,3,"d"); new Array alias=a; free a; assert(Array.length(alias)==0); Array.grow(a,2); Array.set(a,0,"e"); return Array.get(a,0);' 'e'
native_case selfassign 'new String s="x"; s=s; new Array a=Array.new(1); Array.set(a,0,s); Array.set(a,0,Array.get(a,0)); delete s; return string_new(Array.get(a,0));' 'x'
native_case branch 'new int x=1; if(!x){x=9;} else {x=3;} return (x%2)+(x/2)+(x!=0)+(x>=3)+(x>2)+(x==3);' '6'
native_case edge_loop_owned 'define proc grow(n){new String s="start";while(n>0){s=s+"x";n=n-1;}s=s+"owned";return s;} new int edge_value=5;new int result=0;if(1){result=String.length(grow(3))+edge_value;}else{result=0;}return result;' '18'
native_case cleanup 'define proc f(n){new String s="x"; while(n>0){s=s+"y"; n=n-1;} return s;} return string_new(f(100));' "x$(printf '%100s' '' | tr ' ' y)"
native_case signed_arithmetic 'assert(-7/3 == -2); assert(-7%3 == -1); assert(7%-3 == 1); assert(-4*-3 == 12); return -7-3;' '-10'
native_case many_arguments 'return many(1,2,3,4,5,6,7,8,9); define proc many(a,b,c,d,e,f,g,h,i){return a+b+c+d+e+f+g+h+i;}' '45'
native_case object_truth 'new String s=""; new ptr b=malloc 0; assert(!s == 0); if(b){return 7;} return 0;' '7'
native_case overwrite_kind 'new int x=0; while(x<5){new int value="object"; value=x; x=x+1;} return value;' '4'
native_case live_across_call 'define proc f(n){println(n);return n*2;} return (1+2)+f(3);' '3
9'
native_case live_across_builtin 'return (2+3)+len("abc"+"de");' '10'
native_case register_pressure 'return (1+2)*(3+4)+(5+6)*(7+8);' '186'
native_case rematerialization 'return -(9+9)+1;' '-17'
native_case mixed_call_results 'define proc f(n){if(n){return 7;}return "ab";} new int i=0; new int n=0; while(i<10){n=n+len(string_new(f(i%2)));i=i+1;} return n;' '15'
native_case wide_call_pressure 'define proc sum(a,b,c,d,e){return a+b+c+d+e;} return sum((1+2)*(3+4),(5+6)*(7+8),(9+10)*(11+12),(13+14)*(15+16),(17+18)*(19+20));' '2825'
native_case deferred_return 'return ((10-3)*6)/2;' '21'
native_case deferred_dynamic 'new int x=0; new String s="a"; while(x<6){s=s+"b";x=x+1;} assert(s=="abbbbbb"); return string_new(s);' 'abbbbbb'
runtime_error deferred_error 'define proc f(a,b){new String s="owned";return (a+1)/(b-1);} return f(2,1);' 'division by zero'
# Local imports still expand before parsing, and duplicate imports load once.
printf '%s\n' 'define proc imported(n){return n*2;}' > "$task_dir/lib.bl"
native_case imports 'import "lib.bl"; 
import "lib.bl";
return imported(4);' '8'
compile_error old_keyword 'define fn f(){return 1;}' 'expected macro or proc'
compile_error integer_literal_range 'return 9223372036854775808;' 'outside the signed 64-bit range'
compile_error unknown 'return missing;' 'undefined variable'
compile_error arity 'define proc f(a){return a;} return f();' 'expected 1 arguments'
compile_error duplicate 'new int x=1; new int x=2;' 'duplicate declaration'
compile_error nested 'if(1){define proc f(){return 1;}}' 'direct program children'
runtime_error division 'return 1/0;' 'division by zero'
runtime_error overflow 'return 9223372036854775807 + 1;' 'integer overflow'
runtime_error minimum 'return (-9223372036854775807 - 1)/-1;' 'integer overflow'
runtime_error subtract_overflow 'return (-9223372036854775807 - 1) - 1;' 'integer overflow'
runtime_error multiply_overflow 'return 9223372036854775807 * 2;' 'integer overflow'
runtime_error negate_overflow 'return -(-9223372036854775807 - 1);' 'integer overflow'
runtime_error modulo_overflow 'return (-9223372036854775807 - 1)%-1;' 'integer overflow'
runtime_error bounds 'new ptr b=malloc 1; return b[1];' 'buffer index out of bounds'
runtime_error uninitialized 'if(0){new int x=1;} return x;' 'uninitialized variable'
runtime_error cleanup_error 'define proc f(){new String s="allocated"; new Array a=Array.new(1); Array.set(a,0,s); return 1/0;} return f();' 'division by zero'
native_case recursive_pressure 'define proc f(n,a,b,c,d,e,f,g,h){if(n==0){return a+b+c+d+e+f+g+h;} return (a+b)*(c+d)+(e+f)*(g+h)+f(n-1,a,b,c,d,e,f,g,h);} return f(4,1,2,3,4,5,6,7,8);' '780'
native_case pressure_calls 'define proc id(n){return n;} define proc f(a,b,c,d,e,f,g,h){return ((a+b)*(c+d))+id((e+f)*(g+h))+((a+c)*(b+d));} return f(1,2,3,4,5,6,7,8);' '210'
native_case object_return_pressure 'define proc id(s){return s;} define proc f(a,b){new String x=id(a+"x");new String y=id(b+"y");return id(x+y);} return string_new(f("a","b"));' 'axby'
native_case joined_objects 'define proc f(n){new int x=0;if(n){x="yes";}else{x="no";}return x;} return f(1)+f(0);' 'yesno'
native_case empty_procedure 'define proc empty(){} return empty()==empty();' '1'
native_case slot_propagation 'new int x=6;new int y=7;define proc modify(){x=9;return 1;}assert(x*y==42);modify();return x+y;' '16'
native_case repeated_pure_access 'define proc f(n){new int x=!n;new int y=!n;return x+y;}return f(0)+f(3);' '2'
native_case call_mutates_buffer 'define proc write(p){p[0]=66;return 0;}new ptr p=malloc 1;p[0]=65;new int before=p[0];write(p);return before+p[0];' '131'
native_case repeated_output_effect 'println("left");println("right");return 3;' 'left
right
3'
native_case constant_nested_branch 'new int x=0;new int k=1;if(k){x=6;}else{x=8;}return x+1;' '7'
native_case constant_loop_invariant 'define proc f(c){new int x=7;while(c){return x+2;}return x+3;}return f(0)+f(1);' '19'
native_case dead_procedure 'define proc unused(){return "unused";}define proc f(n){return n+1;}return f(9);' '10'
native_case dead_effectful_call 'define proc noise(){println("still");return 1;}new int unused=noise();return 2;' 'still
2'
runtime_error dead_error_expression 'new int unused=1/0;return 2;' 'division by zero'
native_case recursive_unreachable 'define proc dead(n){return dead(n-1);}return 3;' '3'
native_case dead_owned_cleanup 'define proc f(){new String s="owned";return 3;}return f();' '3'
runtime_error typed_compare 'define proc f(x){return x<1;} return f("x");' 'integer operands'
runtime_error typed_negate 'define proc f(x){return -x;} return f("x");' 'integer'
runtime_error recursive_cleanup 'define proc f(n){new Array a=Array.new(1);Array.set(a,0,"owned");if(n){return f(n-1);}return 1/0;} return f(30);' 'division by zero'
# Large local frames exercise Unix page probing and Windows chkstk lowering.
large='define proc large(n){'
j=0
while [ "$j" -lt 320 ]; do large="$large new int v$j=n+$j;"; j=$((j+1)); done
large="$large return v0+v319;} return large(2);"
native_case large_frame "$large" '323'
# Output modes and output-path protection.
progress "output-mode checks"
bpp --emit-ir -o "$task_dir/out.ir" "$task_dir/case.bpp"
grep -F 'call f1' "$task_dir/out.ir" >/dev/null
printf '%s\n' 'define proc dead(n){return dead(n-1);}return 3;' > "$task_dir/dead-recursive.bpp"
"$project_compiler" --emit-ir -o "$task_dir/dead-recursive.ir" "$task_dir/dead-recursive.bpp"
if grep -E '^function .*"dead"' "$task_dir/dead-recursive.ir" >/dev/null; then
    echo 'FAIL DCE: retained unreachable recursive procedure'; exit 1
fi
bpp --emit-c -o "$task_dir/out.c" "$task_dir/case.bpp"
grep -F 'bpp_f1' "$task_dir/out.c" >/dev/null
bpp --emit-asm -o "$task_dir/out.s" "$task_dir/case.bpp"
grep -E '\.globl (_)?main' "$task_dir/out.s" >/dev/null
"$project_compiler" --asm-comments --emit-asm -o "$task_dir/placement.s" "$project_dir/tests/arm64_coverage.bpp"
grep -F 'B++ reload' "$task_dir/placement.s" >/dev/null
grep -F 'reason=' "$task_dir/placement.s" >/dev/null
printf '%s\n' 'define proc g(n){return n+1;} define proc f(n){new int a=n+1; return a+g(n);} return f(4);' > "$task_dir/lazy-reload.bpp"
"$project_compiler" --target macos-arm64 --asm-comments --emit-asm -o "$task_dir/lazy-reload.s" "$task_dir/lazy-reload.bpp"
if grep -F 'reason=segment-transition-after-clobber' "$task_dir/lazy-reload.s" >/dev/null; then
    echo 'FAIL lazy reload: allocation transition was reloaded eagerly'; exit 1
fi
if ! awk '/B\+\+ reload .*reason=first-payload-use/ { if (getline && $0 ~ /add x12/ && getline && $0 ~ /ldr x[0-9]+, \[x12, #8\]/) found=1 } END { exit !found }' "$task_dir/lazy-reload.s"; then
    echo 'FAIL lazy reload: no first-use reload group was emitted'; exit 1
fi
printf '%s\n' 'define proc g(n){return n+1;} define proc f(n){new int a=g(n); return a+1;} return f(4);' > "$task_dir/helper-result-lazy.bpp"
"$project_compiler" --print-result -o "$task_dir/helper-result-lazy" "$task_dir/helper-result-lazy.bpp"
if [ "$("$task_dir/helper-result-lazy")" != 6 ]; then
    echo 'FAIL lazy reload: helper-defined result changed before first use'; exit 1
fi
"$project_compiler" --target macos-arm64 --asm-comments --emit-asm -o "$task_dir/helper-result-lazy.s" "$task_dir/helper-result-lazy.bpp"
if ! awk '/B\+\+ opcode=add function=f / { in_add=1 } in_add && /reason=first-payload-use/ { found=1 } in_add && /adds x26/ { exit !found } END { if (!in_add) exit 1 }' "$task_dir/helper-result-lazy.s"; then
    echo 'FAIL lazy reload: helper result was not reloaded at its first native use'; exit 1
fi
"$project_compiler" --emit-map -o "$task_dir/placement.json" "$project_dir/tests/arm64_coverage.bpp"
grep -F '"spill_candidates"' "$task_dir/placement.json" >/dev/null
grep -F '"spill_reason"' "$task_dir/placement.json" >/dev/null
grep -F '"redundant_access_metrics"' "$task_dir/placement.json" >/dev/null
grep -E '"effect_barriers":[1-9][0-9]*' "$task_dir/placement.json" >/dev/null
printf '%s\n' 'new int x=0;new int k=1;if(k){x=6;}else{x=8;}return x+1;' > "$task_dir/constant-branch.bpp"
"$project_compiler" --emit-ir -o "$task_dir/constant-branch.ir" "$task_dir/constant-branch.bpp"
grep -E 'int 7 +; 1:' "$task_dir/constant-branch.ir" >/dev/null
if grep -F ' add ' "$task_dir/constant-branch.ir" >/dev/null; then echo 'FAIL constant folding: retained safe branch result addition'; exit 1; fi
grep -E '"workspace_peak_bytes":[1-9][0-9]*' "$task_dir/placement.json" >/dev/null
grep -E '"sparse_use_entries":[1-9][0-9]*' "$task_dir/placement.json" >/dev/null
grep -F '"live_range_pieces"' "$task_dir/placement.json" >/dev/null
"$project_compiler" --no-opt --target linux-x86_64 --asm-comments --emit-asm -o "$task_dir/rematerialization.s" "$project_dir/tests/rematerialization.bpp"
grep -F 'B++ rematerialize' "$task_dir/rematerialization.s" >/dev/null
grep -E 'allocation-traffic function=<module> .*rematerializations=[1-9][0-9]*' "$task_dir/rematerialization.s" >/dev/null
"$project_compiler" --no-opt --target linux-x86_64 --emit-map -o "$task_dir/rematerialization.json" "$project_dir/tests/rematerialization.bpp"
grep -E '"rematerialization_cost":[1-9][0-9]*' "$task_dir/rematerialization.json" >/dev/null
printf '%s\n' 'new int x=1; if(x){return !x;} return 5;' > "$task_dir/value-facts.bpp"
"$project_compiler" --no-opt --target linux-x86_64 --asm-comments --emit-asm -o "$task_dir/value-facts.s" "$task_dir/value-facts.bpp"
grep -F 'B++ truthiness-elided' "$task_dir/value-facts.s" >/dev/null
printf '%s\n' 'define proc id(n){return n;} define proc f(a,b,c,d,e,f,g,h){return ((a+b)*(c+d))+id((e+f)*(g+h))+((a+c)*(b+d));} return f(1,2,3,4,5,6,7,8);' > "$task_dir/allocation-traffic.bpp"
for target in macos-arm64 linux-x86_64 linux-arm64 windows-x86_64; do
    "$project_compiler" --no-opt --target "$target" --asm-comments --emit-asm -o "$task_dir/allocation-$target.s" "$task_dir/allocation-traffic.bpp"
    grep -F 'B++ allocation-traffic function=f' "$task_dir/allocation-$target.s" >/dev/null
    grep -E 'allocation-traffic function=f .*reloads=[1-9][0-9]*' "$task_dir/allocation-$target.s" >/dev/null
    grep -F 'B++ access-traffic function=f' "$task_dir/allocation-$target.s" >/dev/null
done
printf '%s\n' 'new int x=1;x=1;return x;' > "$task_dir/redundant-store.bpp"
"$project_compiler" --emit-map -o "$task_dir/redundant-store.json" "$task_dir/redundant-store.bpp"
grep -E '"eliminated_slot_stores":[1-9][0-9]*' "$task_dir/redundant-store.json" >/dev/null
"$project_compiler" --debug-info --emit-map -o "$task_dir/redundant-store-debug.json" "$task_dir/redundant-store.bpp"
grep -F '"eliminated_slot_stores":0' "$task_dir/redundant-store-debug.json" >/dev/null
for target in macos-arm64 linux-x86_64 linux-arm64 windows-x86_64; do
    "$project_compiler" --target "$target" --asm-comments --emit-asm -o "$task_dir/redundant-store-$target.s" "$task_dir/redundant-store.bpp"
    "$project_compiler" --no-opt --target "$target" --asm-comments --emit-asm -o "$task_dir/redundant-store-noopt-$target.s" "$task_dir/redundant-store.bpp"
    grep -F 'access-traffic function=<module> slot-loads=0 slot-stores=1' "$task_dir/redundant-store-$target.s" >/dev/null
    grep -F 'access-traffic function=<module> slot-loads=1 slot-stores=2' "$task_dir/redundant-store-noopt-$target.s" >/dev/null
done
printf '%s\n' '<module> 2 3' > "$task_dir/allocation.profile"
"$project_compiler" --alloc-profile "$task_dir/allocation.profile" --emit-map -o "$task_dir/profiled.json" "$project_dir/tests/demo.bpp"
grep -F '"profiled_blocks":1' "$task_dir/profiled.json" >/dev/null
grep -F '"index":2,"loop_depth":1,"static_weight":130' "$task_dir/profiled.json" >/dev/null
printf '%s\n' 'not a valid profile' > "$task_dir/bad-allocation.profile"
if "$project_compiler" --alloc-profile "$task_dir/bad-allocation.profile" --check "$project_dir/tests/demo.bpp" >/dev/null 2>&1; then
    echo 'FAIL allocation profile: accepted malformed profile'; exit 1
fi
printf '%s\n' 'define proc f(){new String a="old";println(a);new int b=7;println(b);return 0;} return f();' > "$task_dir/frame.bpp"
"$project_compiler" --emit-map -o "$task_dir/frame.json" "$task_dir/frame.bpp"
grep -F '"local_slots":1' "$task_dir/frame.json" >/dev/null
grep -F '"local_map":"explicit"' "$task_dir/frame.json" >/dev/null
grep -F '"root_cells"' "$task_dir/frame.json" >/dev/null
grep -F '"outgoing_arguments_offset"' "$task_dir/frame.json" >/dev/null
printf '%s\n' 'return 12;' > "$task_dir/exact-frame.bpp"
"$project_compiler" --emit-map --target linux-x86_64 -o "$task_dir/exact-frame.json" "$task_dir/exact-frame.bpp"
grep -F '"frame_data_bytes":144' "$task_dir/exact-frame.json" >/dev/null
grep -F '"saved_register_bytes":16,"stack_allocation_bytes":216,"frame_bytes":232' "$task_dir/exact-frame.json" >/dev/null
if bpp --emit-c -o "$task_dir/./case.bpp" "$task_dir/case.bpp" >/dev/null 2>&1; then echo 'FAIL source overwrite'; exit 1; fi
mkdir -p "$task_dir/space dir"
printf '%s\n' 'return 12;' > "$task_dir/space dir/input source.bpp"
bpp --print-result -o "$task_dir/space dir/output program" "$task_dir/space dir/input source.bpp"
if [ "$("$task_dir/space dir/output program")" != 12 ]; then echo 'FAIL paths with spaces'; exit 1; fi
printf '%s\n' 'return 7;' > "$task_dir/snapshot.bpp-source"
cp "$task_dir/snapshot.bpp-source" "$task_dir/original"
if bpp --debug-info --emit-asm -o "$task_dir/snapshot" "$task_dir/snapshot.bpp-source" >/dev/null 2>&1; then echo 'FAIL debug snapshot overwrite'; exit 1; fi
cmp "$task_dir/original" "$task_dir/snapshot.bpp-source"
printf 'Passed %s native and diagnostic cases, plus output-mode checks\n' "$count"
