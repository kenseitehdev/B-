#!/bin/sh
set -eu
compiler=$1
runtime=$2
value=$3
native_runtime=$4
abi_runtime=$5
cc=${BPP_ABI_CC:-cc}
ar_tool=${AR:-ar}
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT HUP INT TERM
printf '%s\n' 'define proc increment(n){return n+1;}return 0;' > "$task_dir/provider.bpp"

for target in macos-arm64 linux-x86_64 linux-arm64 windows-x86_64; do
    "$compiler" --emit-library --library-name provider --emit-asm --target "$target" -o "$task_dir/$target.s" "$task_dir/provider.bpp"
    case "$target" in
        macos-arm64)
            grep -F '_bpp_v1_increment:' "$task_dir/$target.s" >/dev/null
            awk '/^\.section __DATA,__data/{in_data=1} in_data && /^_bpp_v1_library_provider:/{found=1} END{exit !found}' "$task_dir/$target.s"
            ;;
        *) grep -F 'bpp_v1_increment:' "$task_dir/$target.s" >/dev/null ;;
    esac
    if grep -E '\.globl (_)?main' "$task_dir/$target.s" >/dev/null; then
        echo "FAIL static ABI: $target library output retained main" >&2; exit 1
    fi
done

"$compiler" --emit-library --library-name provider --emit-asm -o "$task_dir/provider.s" "$task_dir/provider.bpp"
"$cc" -x assembler -c "$task_dir/provider.s" -o "$task_dir/provider.o"
"$ar_tool" rcs "$task_dir/libprovider.bpp.a" "$task_dir/provider.o"
"$compiler" --emit-library-manifest --library-name provider -o "$task_dir/libprovider.bppabi" "$task_dir/provider.bpp"
grep -Fx 'bppabi 1' "$task_dir/libprovider.bppabi" >/dev/null
grep -Fx 'name provider' "$task_dir/libprovider.bppabi" >/dev/null
grep -Fx 'export increment 1' "$task_dir/libprovider.bppabi" >/dev/null
awk '$1 == "archive" && $2 != "" && $3 == "libprovider.bpp.a" && $4 ~ /^[0-9]+$/ && $5 ~ /^[0-9A-Fa-f]+$/ { found=1 } END { exit !found }' "$task_dir/libprovider.bppabi"
printf '%s\n' 'import library "provider";' 'return increment(41);' > "$task_dir/bpp-consumer.bpp"
"$compiler" -L "$task_dir" --print-result -o "$task_dir/bpp-consumer" "$task_dir/bpp-consumer.bpp"
test "$("$task_dir/bpp-consumer")" = 42
"$compiler" -L "$task_dir" --emit-link-map -o "$task_dir/consumer.link-map" "$task_dir/bpp-consumer.bpp"
grep -Fx 'bpp-link-map 1' "$task_dir/consumer.link-map" >/dev/null
grep -F 'name=provider' "$task_dir/consumer.link-map" >/dev/null
grep -F 'symbol=bpp_v1_increment arity=1' "$task_dir/consumer.link-map" >/dev/null
printf '%s\n' 'import library "provider";' 'return increment();' > "$task_dir/bad-arity.bpp"
if "$compiler" -L "$task_dir" --check "$task_dir/bad-arity.bpp" >"$task_dir/bad-arity.out" 2>&1; then
    echo 'FAIL static ABI: bad imported procedure arity compiled' >&2; exit 1
fi
grep -F "static procedure 'increment' expected 1 arguments, got 0" "$task_dir/bad-arity.out" >/dev/null
# Archive ordering must be dependant-before-dependency for one-pass static
# linkers.  Build a second B++ library which imports provider and expose a
# procedure from it; the final consumer imports only the dependant.
printf '%s\n' 'import library "provider";' 'define proc twice(n){return increment(increment(n));}' 'return 0;' > "$task_dir/bridge.bpp"
"$compiler" -L "$task_dir" --emit-library --library-name bridge --emit-asm -o "$task_dir/bridge.s" "$task_dir/bridge.bpp"
"$cc" -x assembler -c "$task_dir/bridge.s" -o "$task_dir/bridge.o"
"$ar_tool" rcs "$task_dir/libbridge.bpp.a" "$task_dir/bridge.o"
"$compiler" -L "$task_dir" --emit-library-manifest --library-name bridge -o "$task_dir/libbridge.bppabi" "$task_dir/bridge.bpp"
grep -Fx 'dependency provider' "$task_dir/libbridge.bppabi" >/dev/null
printf '%s\n' 'import library "bridge";' 'return twice(40);' > "$task_dir/bpp-transitive-consumer.bpp"
"$compiler" -L "$task_dir" --print-result -o "$task_dir/bpp-transitive-consumer" "$task_dir/bpp-transitive-consumer.bpp"
test "$("$task_dir/bpp-transitive-consumer")" = 42
printf '%s\n' '#include "bpp_abi.h"' \
 '#include <stddef.h>' \
 'extern const BPPStaticLibrary bpp_v1_library_provider;' \
 'int main(void){' \
 'char e[128];BLRuntime vm={"consumer",0};BLValue a=bl_value_int(41),o=bl_value_null();' \
 'if(!bpp_static_library_validate(&bpp_v1_library_provider,e,sizeof e)||bpp_v1_library_provider.procedure_count!=1||bpp_static_invoke(&bpp_v1_library_provider.procedures[0],&vm,0,0,&a,1,&o)!=BPP_STATIC_OK||o.tag!=BLV_INT||o.as.i!=42)return 1;' \
 'bl_value_release(o);return 0;}' > "$task_dir/consumer.c"
case "$(uname -s)" in Linux) link_flags=-no-pie ;; *) link_flags= ;; esac
"$cc" -std=c11 -Iinclude $link_flags "$task_dir/consumer.c" "$runtime" "$value" "$native_runtime" "$abi_runtime" "$task_dir/libprovider.bpp.a" -o "$task_dir/consumer"
"$task_dir/consumer"
printf x >> "$task_dir/libprovider.bpp.a"
if "$compiler" -L "$task_dir" --check "$task_dir/bpp-consumer.bpp" >"$task_dir/stale-archive.out" 2>&1; then
    echo 'FAIL static ABI: stale archive matched its manifest' >&2; exit 1
fi
grep -F 'archive identity mismatch' "$task_dir/stale-archive.out" >/dev/null
echo 'Static archive producer/consumer ABI fixture passed'
