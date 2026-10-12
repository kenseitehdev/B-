#!/bin/sh
set -eu

bpp=$1
work=${TMPDIR:-/tmp}/bpp-debug-metadata-$$
cleanup() {
    if [ "${BPP_KEEP_DEBUG_METADATA_TMP:-0}" = 1 ]; then
        echo "Preserved debug metadata test files at $work" >&2
    else
        rm -rf "$work"
    fi
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$work"

cat >"$work/library.bpp" <<'EOF'
define proc increment(n) {
    new byte local = n;
    local = local + 1;
    return local;
}
EOF
cat >"$work/main.bpp" <<EOF
import "$work/library.bpp"

define proc main() {
    return increment(41);
}
EOF

"$bpp" --emit-map -o "$work/map.json" "$work/main.bpp"
# TMPDIR on macOS may include a trailing slash while the import resolver
# canonicalizes the child path.  Validate the recorded files structurally;
# do not require an incidental doubled slash to survive that normalization.
grep -F '/main.bpp"' "$work/map.json" >/dev/null
grep -F '/library.bpp"' "$work/map.json" >/dev/null
grep -F '"parent":1,"import_line":1' "$work/map.json" >/dev/null
grep -F '"file":2,"line":2' "$work/map.json" >/dev/null
grep -F '"variable_write"' "$work/map.json" >/dev/null
grep -F '"value_location"' "$work/map.json" >/dev/null
grep -F '"value_location_role":"assigned_value_at_this_instruction"' "$work/map.json" >/dev/null
grep -F '"representation":"raw-word","declared_type":"byte"' "$work/map.json" >/dev/null
grep -F '"location_status":"exact_raw_frame_slot_when_initialized"' "$work/map.json" >/dev/null

"$bpp" --debug-info --emit-asm --target macos-arm64 -o "$work/macho.s" "$work/main.bpp"
grep -F '.section __DWARF,__debug_info,regular,debug' "$work/macho.s" >/dev/null
grep -F 'L_bpp_debug_start' "$work/macho.s" >/dev/null
grep -F 'L_bpp_debug_end' "$work/macho.s" >/dev/null
grep -F '0x49, 0x13' "$work/macho.s" >/dev/null # DW_AT_type / DW_FORM_ref4
grep -F 'DW_TAG_formal_parameter' "$work/macho.s" >/dev/null 2>&1 || grep -F '.byte 3, 0x05, 0' "$work/macho.s" >/dev/null
grep -F 'DW_TAG_variable' "$work/macho.s" >/dev/null 2>&1 || grep -F '.byte 4, 0x34, 0' "$work/macho.s" >/dev/null
grep -F 'B++ value (parameter type unspecified)' "$work/macho.s" >/dev/null
grep -F '.asciz "byte"' "$work/macho.s" >/dev/null
if grep -E '^    \.long (\.L_bpp_debug_str_begin|\.L_bpp_abbrev_begin|\.debug_line)([[:space:]]|$)' "$work/macho.s" >/dev/null; then
    echo 'Mach-O DWARF section offsets must be relative to their debug section, not object addresses' >&2
    exit 1
fi
awk '
    /^\.L_bpp_debug_loclists_begin:$/ {
        if (getline <= 0 || $1 != ".long" || $2 !~ /^[0-9]+$/) exit 1
        loclists = 1
    }
    /^\.L_bpp_debug_cu_begin:$/ {
        if (getline <= 0 || $1 != ".long" || $2 !~ /^[0-9]+$/) exit 1
        cu = 1
    }
    END { exit !(cu && loclists) }
' "$work/macho.s"
if grep -F '.long .L_bpp_type_' "$work/macho.s" >/dev/null; then
    echo 'Mach-O DWARF type references must not require section-difference relocations' >&2
    exit 1
fi

"$bpp" --debug-info --emit-asm --target macos-arm64 -o "$work/raw-arm64.s" tests/raw_core.bpp
grep -F '.cfi_def_cfa x29, 16' "$work/raw-arm64.s" >/dev/null
grep -F '.cfi_def_cfa sp, 0' "$work/raw-arm64.s" >/dev/null
grep -F 'L_bpp_raw_proc_0_done:' "$work/raw-arm64.s" >/dev/null
awk '
    /^\.cfi_startproc$/ { in_cfi = 1 }
    /^\.cfi_endproc$/ { in_cfi = 0 }
    /^bpp_raw_proc_0_done:$/ && in_cfi { bad = 1 }
    END { exit bad }
' "$work/raw-arm64.s"
if grep -F '.cfi_restore x23' "$work/raw-arm64.s" >/dev/null; then
    echo 'ARM64 epilogues must retain CFA-relative saved-register rules through ret' >&2
    exit 1
fi
"$bpp" --debug-info --emit-asm --target windows-x86_64 -o "$work/raw-windows.s" tests/raw_core.bpp
grep -F '.seh_proc bpp_raw_proc_0' "$work/raw-windows.s" >/dev/null
grep -F '.seh_endprologue' "$work/raw-windows.s" >/dev/null
grep -F '.seh_endproc' "$work/raw-windows.s" >/dev/null

"$bpp" --debug-info --print-result -o "$work/program" "$work/main.bpp"
"$work/program" >"$work/result.txt"
grep -Fx '42' "$work/result.txt" >/dev/null

if command -v readelf >/dev/null 2>&1; then
    readelf --debug-dump=decodedline "$work/program" >"$work/lines.txt"
    grep -F "$work/library.bpp" "$work/lines.txt" >/dev/null
    readelf --debug-dump=info "$work/program" >"$work/info.txt"
    grep -F 'DW_TAG_subprogram' "$work/info.txt" >/dev/null
    grep -F 'DW_TAG_formal_parameter' "$work/info.txt" >/dev/null
    grep -F 'DW_TAG_lexical_block' "$work/info.txt" >/dev/null
    grep -F 'DW_TAG_variable' "$work/info.txt" >/dev/null
    grep -E 'DW_AT_name[[:space:]]+: .*increment' "$work/info.txt" >/dev/null
    grep -E 'DW_AT_name[[:space:]]+: .*local' "$work/info.txt" >/dev/null
    grep -E 'DW_AT_decl_file[[:space:]]+: 2' "$work/info.txt" >/dev/null
    readelf --debug-dump=loc "$work/program" >"$work/locations.txt"
    grep -F 'Contents of the .debug_loclists section' "$work/locations.txt" >/dev/null
    grep -F 'DW_OP_bregx' "$work/locations.txt" >/dev/null
    readelf --debug-dump=frames "$work/program" >"$work/frames.txt"
    grep -F 'CFA' "$work/frames.txt" >/dev/null
    test "$(grep -c ' FDE cie=' "$work/frames.txt")" -ge 3
    if [ "$(uname -m)" = x86_64 ]; then
        grep -F 'r12 (r12) at cfa-24' "$work/frames.txt" >/dev/null
        grep -F 'rbx) at cfa-56' "$work/frames.txt" >/dev/null
        grep -F 'DW_CFA_restore: r12 (r12)' "$work/frames.txt" >/dev/null
        grep -F 'DW_CFA_def_cfa: r7 (rsp) ofs 8' "$work/frames.txt" >/dev/null
    fi
fi

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) have_elf_dwarf=0 ;;
    *) have_elf_dwarf=1 ;;
esac
if [ "$have_elf_dwarf" = 1 ] && command -v llvm-dwarfdump >/dev/null 2>&1; then
    llvm-dwarfdump --verify "$work/program" >"$work/verify.txt"
    llvm-dwarfdump --debug-info --debug-loclists "$work/program" >"$work/llvm-info.txt"
    grep -F 'DW_TAG_subprogram' "$work/llvm-info.txt" >/dev/null
    grep -F 'DW_OP_bregx' "$work/llvm-info.txt" >/dev/null
fi
if command -v llvm-readobj >/dev/null 2>&1; then
    llvm-readobj --sections --unwind "$work/program" >"$work/readobj.txt"
    grep -F '.eh_frame' "$work/readobj.txt" >/dev/null ||
        grep -F 'UnwindInformation' "$work/readobj.txt" >/dev/null ||
        grep -F '.pdata' "$work/readobj.txt" >/dev/null
fi

if [ "$(uname -s)" = Darwin ]; then
    clang -g -c -x assembler "$work/raw-arm64.s" -o "$work/raw-arm64.o"
    test -d "$work/program.dSYM"
    dwarf="$work/program.dSYM/Contents/Resources/DWARF/program"
    xcrun dwarfdump --debug-info "$dwarf" >"$work/info.txt"
    if ! grep -F 'DW_TAG_compile_unit' "$work/info.txt" >/dev/null; then
        echo "dSYM contains no compile unit; linker debug map follows:" >&2
        xcrun dsymutil --dump-debug-map "$work/program" >&2
        exit 1
    fi
    grep -F 'DW_TAG_formal_parameter' "$work/info.txt" >/dev/null
    grep -F 'DW_TAG_variable' "$work/info.txt" >/dev/null
    grep -E 'DW_AT_name[[:space:]]+\("increment"\)' "$work/info.txt" >/dev/null
    grep -E 'DW_AT_name[[:space:]]+\("n"\)' "$work/info.txt" >/dev/null
    grep -E 'DW_AT_name[[:space:]]+\("local"\)' "$work/info.txt" >/dev/null
    grep -E 'DW_AT_name[[:space:]]+\("byte"\)' "$work/info.txt" >/dev/null
    xcrun dwarfdump --debug-line "$dwarf" >"$work/lines.txt"
    grep -F 'library.bpp' "$work/lines.txt" >/dev/null

    if [ "${BPP_SKIP_DEBUGGER_TESTS:-0}" != 1 ] && command -v lldb >/dev/null 2>&1; then
        "$bpp" --debug-info -o "$work/lldb-runtime-error" tests/lldb_runtime_error.bpp
        lldb --batch \
            -o 'breakpoint set --file lldb_runtime_error.bpp --line 6' \
            -o 'breakpoint set --file lldb_runtime_error.bpp --line 15' \
            -o 'breakpoint set --file lldb_runtime_error.bpp --line 7' \
            -o run -o 'breakpoint disable 1' -o continue -o 'thread step-over' \
            -o 'breakpoint disable 3' -o continue -o bt \
            -o 'process kill' -o quit \
            "$work/lldb-runtime-error" >"$work/lldb.txt" 2>&1 || true
        grep -F 'lldb_runtime_error.bpp:6' "$work/lldb.txt" >/dev/null
        grep -F 'lldb_runtime_error.bpp:7' "$work/lldb.txt" >/dev/null
        grep -F 'stop reason = breakpoint 3.1' "$work/lldb.txt" >/dev/null
        grep -E 'read_invalid_address(\([^)]*\))? at lldb_runtime_error.bpp:15' "$work/lldb.txt" >/dev/null
        grep -F 'main at lldb_runtime_error.bpp:21' "$work/lldb.txt" >/dev/null

        "$bpp" --debug-info -o "$work/lldb-import-recursion" tests/lldb_import_recursion.bpp
        lldb --batch \
            -o 'breakpoint set --file lldb_recursive_helper.bpp --line 3' \
            -o 'breakpoint set --file lldb_recursive_helper.bpp --line 10' \
            -o run -o continue -o bt -o 'breakpoint disable 1' -o continue -o bt \
            -o 'process kill' -o quit "$work/lldb-import-recursion" >"$work/lldb-import.txt" 2>&1 || true
        test "$(grep -Ec 'recursive_calls(\([^)]*\))? at lldb_recursive_helper.bpp:3' "$work/lldb-import.txt")" -ge 2
        grep -E 'imported_fault(\([^)]*\))? at lldb_recursive_helper.bpp:10' "$work/lldb-import.txt" >/dev/null
        grep -F 'main at lldb_import_recursion.bpp:5' "$work/lldb-import.txt" >/dev/null
    fi
fi

# Linux native debugger coverage mirrors the LLDB smoke test above. Keep the
# breakpoints on source locations so this checks the line table, not symbols
# supplied by the host C compiler. The second fixture verifies imported source
# files and distinct recursive frames before stopping at an imported fault.
if [ "$(uname -s)" = Linux ] && [ "${BPP_SKIP_DEBUGGER_TESTS:-0}" != 1 ] && command -v gdb >/dev/null 2>&1; then
    "$bpp" --debug-info -o "$work/gdb-runtime-error" tests/lldb_runtime_error.bpp
    gdb --batch \
        -ex 'set pagination off' \
        -ex 'set confirm off' \
        -ex 'break lldb_runtime_error.bpp:6' \
        -ex 'break lldb_runtime_error.bpp:15' \
        -ex run -ex 'bt' -ex 'disable 1' -ex next -ex 'bt' \
        -ex continue -ex 'bt' -ex 'disable 2' -ex continue -ex 'bt' \
        -ex quit "$work/gdb-runtime-error" >"$work/gdb.txt" 2>&1 || true
    grep -F 'lldb_runtime_error.bpp:6' "$work/gdb.txt" >/dev/null
    grep -F 'lldb_runtime_error.bpp:7' "$work/gdb.txt" >/dev/null
    grep -F 'read_invalid_address' "$work/gdb.txt" >/dev/null
    grep -F 'main' "$work/gdb.txt" >/dev/null
    grep -E 'SIGSEGV|SIGBUS|Program received signal' "$work/gdb.txt" >/dev/null

    "$bpp" --debug-info -o "$work/gdb-import-recursion" tests/lldb_import_recursion.bpp
    gdb --batch \
        -ex 'set pagination off' \
        -ex 'set confirm off' \
        -ex 'break lldb_recursive_helper.bpp:3' \
        -ex 'break lldb_recursive_helper.bpp:10' \
        -ex run -ex continue -ex 'bt' -ex 'disable 1' -ex continue -ex 'bt' \
        -ex quit "$work/gdb-import-recursion" >"$work/gdb-import.txt" 2>&1 || true
    test "$(grep -Ec 'recursive_calls.*lldb_recursive_helper.bpp:3' "$work/gdb-import.txt")" -ge 2
    grep -E 'imported_fault.*lldb_recursive_helper.bpp:10' "$work/gdb-import.txt" >/dev/null
    grep -F 'main' "$work/gdb-import.txt" >/dev/null
fi

echo 'Debug provenance checks passed'
