#!/bin/sh
set -eu

bpp=$1
work=${TMPDIR:-/tmp}/bpp-raw-core-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

"$bpp" --print-result tests/raw_core.bpp -o "$work/native"
"$work/native" >"$work/native.out"
grep -Fx 'A*42' "$work/native.out" >/dev/null

"$bpp" --emit-asm --target linux-x86_64 -o "$work/raw.s" tests/raw_core.bpp
if grep -E 'BL(Value|Runtime)|bpp_[an]_' "$work/raw.s" >/dev/null; then
    echo 'raw native assembly retained a legacy runtime boundary' >&2
    exit 1
fi

cat >"$work/control_flow.bpp" <<'EOF'
define proc sum(n) {
    new byte total = 0;
    while (n > 0) { total = total + n; n = n - 1; }
    return total;
}
define proc fact(n) {
    if (n == 0) { return 1; }
    return n * fact(n - 1);
}
define proc main() { return sum(10) + fact(8); }
EOF
"$bpp" --print-result -o "$work/control_flow" "$work/control_flow.bpp"
"$work/control_flow" >"$work/control_flow.out"
grep -Fx '40375' "$work/control_flow.out" >/dev/null

"$bpp" --backend c --print-result tests/raw_core.bpp -o "$work/reference"
"$work/reference" >"$work/reference.out"
grep -Fx 'A*42' "$work/reference.out" >/dev/null

printf '%s\n' 'define proc main() { new ptr p = realloc 4; return 0; }' > "$work/legacy.bpp"
if "$bpp" --check "$work/legacy.bpp" >"$work/out" 2>"$work/err"; then
    echo 'legacy realloc spelling unexpectedly compiled' >&2
    exit 1
fi
# `realloc` is not a keyword or callable primitive.  Parsing must fail rather
# than silently turning it into the old dynamic-buffer operation.
test -s "$work/err"

printf '%s\n' 'define proc main() { return "runtime string"; }' > "$work/quoted.bpp"
if "$bpp" --check "$work/quoted.bpp" >"$work/out" 2>"$work/err"; then
    echo 'runtime quoted text unexpectedly compiled' >&2
    exit 1
fi
grep -F 'quoted text is only valid in compile-time directives' "$work/err" >/dev/null

echo 'Raw byte/ptr core checks passed'
