#!/bin/sh
set -eu
case "$(uname -s):$(uname -m)" in Linux:x86_64) ;; *) echo 'check-msabi requires Linux x86-64'; exit 1;; esac
BPP_REAL_COMPILER=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
BPP_MSABI_DIR=$(mktemp -d)
trap 'rm -rf "$BPP_MSABI_DIR"' EXIT HUP INT TERM
export BPP_REAL_COMPILER BPP_MSABI_DIR
for name in runtime value codegen_runtime; do
    "${BPP_MSABI_CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror '-DBPP_HOSTCALL=__attribute__((ms_abi))' -c "src/$name.c" -o "$BPP_MSABI_DIR/$name.o"
done
# Linux has a different stack-growth mechanism. The real COFF path retains chkstk.
printf '%s\n' '.text' '.globl ___chkstk_ms' '___chkstk_ms:' 'ret' '.section .note.GNU-stack,"",@progbits' > "$BPP_MSABI_DIR/probe.s"
"${BPP_MSABI_CC:-cc}" -c "$BPP_MSABI_DIR/probe.s" -o "$BPP_MSABI_DIR/probe.o"
cp tests/msabi_driver.sh "$BPP_MSABI_DIR/bpp"
chmod +x "$BPP_MSABI_DIR/bpp"
BPP_TEST_BACKEND=native sh tests/run_tests.sh "$BPP_MSABI_DIR/bpp"
