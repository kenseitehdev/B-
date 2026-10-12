#!/bin/sh
set -eu
bpp=$1
runtime_test=$2
compile() {
    if [ "${BPP_NATIVE_CHECKS:-0}" = 1 ]; then "$bpp" --native-sanitizers "$@"; else "$bpp" "$@"; fi
}
work=${TMPDIR:-/tmp}/bpp-native-sanitizer-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"
"$runtime_test"
"$runtime_test" root
status=0
"$bpp" --native-sanitizers --strict-native-runtime --check tests/raw_core.bpp >"$work/out" 2>"$work/err" || status=$?
test "$status" = 2
fault() {
    mode=$1 category=$2
    status=0
    "$runtime_test" "$mode" >"$work/out" 2>"$work/err" || status=$?
    test "$status" = 86
    grep -F "intentional-native-fault.bpp:7:3: B++ sanitizer: $category" "$work/err" >/dev/null
}
fault uninitialized uninitialized-read
fault invalid-pointer invalid-pointer
fault allocation-overflow allocation-size-overflow
fault invalid-realloc invalid-realloc
fault output-bounds heap-buffer-overflow
fault negative-output negative-output-size
fault grown-uninitialized uninitialized-read
fault bounds heap-buffer-overflow
fault underflow pointer-underflow
fault use-after-free heap-use-after-free
fault double-free double-free
fault invalid-free invalid-free
fault negative-size negative-allocation-size
fault stack-bounds stack-buffer-overflow
fault stack-return stack-use-after-return
fault overflow integer-overflow
fault leak memory-leak
fault cycle memory-leak

for target in macos-arm64 linux-arm64 linux-x86_64 windows-x86_64; do
    compile --emit-asm --target "$target" -o "$work/$target.s" tests/raw_core.bpp
    if [ "${BPP_TEST_SANITIZED:-0}" = 0 ]; then
        if grep -F 'bpp_s_' "$work/$target.s" >/dev/null; then
            echo 'Release assembly contains native sanitizer instrumentation' >&2; exit 1
        fi
    else
        for hook in read write site malloc realloc free byte_load byte_store frame_enter frame_leave globals finish; do
            grep -F "bpp_s_$hook" "$work/$target.s" >/dev/null
        done
    fi
    if command -v clang >/dev/null 2>&1; then
        case "$target" in
            macos-arm64) triple=arm64-apple-macos11 ;;
            linux-arm64) triple=aarch64-linux-gnu ;;
            linux-x86_64) triple=x86_64-linux-gnu ;;
            windows-x86_64) triple=x86_64-w64-windows-gnu ;;
        esac
        clang -target "$triple" -c -x assembler "$work/$target.s" -o "$work/$target.o"
    fi
done
if [ "${BPP_TEST_SANITIZED:-0}" = 1 ]; then
    cat >"$work/root.bpp" <<'EOF'
new ptr root = malloc 8;
define proc main() { root[0] = 42; return root[0]; }
EOF
    compile --print-result -o "$work/root" "$work/root.bpp"
    "$work/root" >"$work/root.out"
    grep -Fx '42' "$work/root.out" >/dev/null
    native_fault() {
        category=$1 body=$2
        cat >"$work/fault.bpp" <<EOF
define proc main() {
    new ptr p = malloc 8;
    p[0] = 42;
    $body
    free p;
    return 0;
}
EOF
        for mode in optimized baseline; do
            if [ "$mode" = baseline ]; then flag=--no-opt; else flag=; fi
            compile $flag -o "$work/program" "$work/fault.bpp"
            status=0
            "$work/program" >"$work/out" 2>"$work/err" || status=$?
            test "$status" = 86
            grep -E 'fault.bpp:[0-9]+:[0-9]+: B\+\+ sanitizer:' "$work/err" >/dev/null
            grep -F "$category" "$work/err" >/dev/null
        done
    }
    native_fault heap-buffer-overflow 'return p[8];'
    native_fault uninitialized-read 'return p[1];'
    native_fault heap-use-after-free 'free p; return p[0];'
    native_fault double-free 'free p; free p;'
    native_fault pointer-underflow 'return p[0 - 1];'
    native_fault memory-leak 'return 0;'
    native_fault integer-overflow 'return 9223372036854775807 + p[0];'
    native_fault integer-overflow 'return (0 - 9223372036854775807 - 1) - p[0];'
    native_fault integer-overflow 'return 9223372036854775807 * p[0];'
    native_fault integer-overflow 'return -(0 - 9223372036854775807 - 1);'
    native_fault invalid-division 'return p[0] / 0;'
    native_fault invalid-division 'return (0 - 9223372036854775807 - 1) / (0 - 1);'
    native_fault invalid-division 'return (0 - 9223372036854775807 - 1) % (0 - 1);'
fi
echo 'Native sanitizer checks passed'
