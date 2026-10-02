#!/bin/sh
set -eu

BIN="$1"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

pass() {
    printf 'ok - %s\n' "$1"
}

fail() {
    printf 'not ok - %s\n' "$1" >&2
    exit 1
}

out=$("$BIN" --print-result "$ROOT/tests/basic_return.bl")
[ "$out" = "5" ] || fail "basic return prints 5"
pass "basic return"

out=$("$BIN" "$ROOT/string_array_memory.bl")
expected='alpha
beta
gamma'
[ "$out" = "$expected" ] || fail "string_array_memory output"
pass "string array memory"

"$BIN" --check "$ROOT/tests/basic_return.bl" >/dev/null
pass "check mode"

if "$BIN" "$ROOT/tests/parse_fail.bl" >/tmp/bl_parse_out.txt 2>/tmp/bl_parse_err.txt; then
    fail "parse failure should return non-zero"
fi
grep -q "expected ')'" /tmp/bl_parse_err.txt || fail "parse error message"
pass "parse error"

if "$BIN" "$ROOT/tests/runtime_assert_fail.bl" >/tmp/bl_runtime_out.txt 2>/tmp/bl_runtime_err.txt; then
    fail "assert failure should return non-zero"
fi
grep -q "assertion failed" /tmp/bl_runtime_err.txt || fail "assert error message"
pass "runtime assert error"

if "$BIN" "$ROOT/tests/runtime_type_fail.bl" >/tmp/bl_type_out.txt 2>/tmp/bl_type_err.txt; then
    fail "type failure should return non-zero"
fi
grep -q "array_len expects array" /tmp/bl_type_err.txt || fail "type error message"
pass "runtime type error"
