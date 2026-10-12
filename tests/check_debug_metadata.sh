#!/bin/sh
set -eu
if [ "${CI:-}" = true ]; then set -x; fi

bpp=$1
# LeakSanitizer cannot attach through an already ptraced debugger. Ownership
# and leak gates run separately, outside this metadata/debugger subprocess.
if [ "${BPP_SKIP_DEBUGGER_TESTS:-0}" != 1 ]; then
    ASAN_OPTIONS="${ASAN_OPTIONS:+${ASAN_OPTIONS}:}detect_leaks=0"
    export ASAN_OPTIONS
fi
work=${TMPDIR:-/tmp}/bpp-debug-metadata-$$
if command -v cygpath >/dev/null 2>&1; then work=$(cygpath -m "$work"); fi
cleanup() {
    result=$?
    if [ "$result" -ne 0 ] || [ "${BPP_KEEP_DEBUG_METADATA_TMP:-0}" = 1 ]; then
        echo "Preserved debug metadata test files at $work" >&2
        for log in "$work/gdb-variables.txt" "$work/variables.txt" "$work/state-variables.txt" "$work/verify.txt"; do
            if [ -f "$log" ]; then tail -80 "$log" >&2; fi
        done
        if [ "${CI:-}" = true ]; then
            mkdir -p build/debug-evidence
            cp -R "$work" build/debug-evidence/
        fi
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
grep -F '"representation":"tagged-value","declared_type":"byte"' "$work/map.json" >/dev/null
grep -F '"location_status":"initialized_tagged_payload"' "$work/map.json" >/dev/null

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
grep -F 'L_cleanup_0:' "$work/raw-arm64.s" >/dev/null
awk '
    /^\.cfi_startproc$/ { in_cfi = 1 }
    /^\.cfi_endproc$/ { in_cfi = 0 }
    /^L_cleanup_0:$/ && in_cfi { bad = 1 }
    END { exit bad }
' "$work/raw-arm64.s"
if grep -F '.cfi_restore x23' "$work/raw-arm64.s" >/dev/null; then
    echo 'ARM64 epilogues must retain CFA-relative saved-register rules through ret' >&2
    exit 1
fi
"$bpp" --debug-info --emit-asm --target windows-x86_64 -o "$work/raw-windows.s" tests/raw_core.bpp
grep -F '.seh_proc L_proc_0' "$work/raw-windows.s" >/dev/null
grep -F '.seh_endprologue' "$work/raw-windows.s" >/dev/null
grep -F '.seh_endproc' "$work/raw-windows.s" >/dev/null
grep -F '.seh_pushreg %r12' "$work/raw-windows.s" >/dev/null
grep -F '.seh_pushreg %rbp' "$work/raw-windows.s" >/dev/null
# A zero-offset SEH frame register must anchor the final allocated rsp, not
# the pre-allocation stack. This also covers sanitizer callback stack changes.
awk '
    /\.seh_proc / { allocated=0; framed=0; source=($2 ~ /^L_proc_/) }
    /\.seh_stackalloc / { allocated=1 }
    /\.seh_setframe / { if (!allocated) exit 1; framed=1 }
    /\.seh_endprologue/ { if (source && (!allocated || !framed)) exit 1 }
' "$work/raw-windows.s"
grep -F '.cv_file 1 ' "$work/raw-windows.s" >/dev/null
grep -F '.cv_func_id 0' "$work/raw-windows.s" >/dev/null
grep -F '.cv_loc 0 ' "$work/raw-windows.s" >/dev/null
grep -F '.cv_linetable 0, L_proc_0, L_proc_end_0' "$work/raw-windows.s" >/dev/null
grep -F '.globl bpp_source_main' "$work/raw-windows.s" >/dev/null

"$bpp" --debug-info --print-result -o "$work/program" "$work/main.bpp"
linked_program="$work/program"
if [ -f "$work/program.exe" ]; then linked_program="$work/program.exe"; fi
"$linked_program" >"$work/result.txt"
grep -Fx '42' "$work/result.txt" >/dev/null
grep -F '"source_variable_ranges"' "$work/map.json" >/dev/null

if [ "$(uname -s)" = Linux ] && command -v readelf >/dev/null 2>&1; then
    readelf --debug-dump=decodedline "$linked_program" >"$work/lines.txt"
    grep -F "library.bpp" "$work/lines.txt" >/dev/null
    readelf --debug-dump=info "$linked_program" >"$work/info.txt"

    grep -F 'DW_TAG_subprogram' "$work/info.txt" >/dev/null
    grep -F 'DW_TAG_formal_parameter' "$work/info.txt" >/dev/null
    grep -F 'DW_TAG_lexical_block' "$work/info.txt" >/dev/null
    grep -F 'DW_TAG_variable' "$work/info.txt" >/dev/null
    grep -E 'DW_AT_name[[:space:]]+: .*increment' "$work/info.txt" >/dev/null
    grep -E 'DW_AT_name[[:space:]]+: .*local' "$work/info.txt" >/dev/null
    grep -E 'DW_AT_decl_file[[:space:]]+:.*[[:space:]](0x)?2([[:space:]]|$)' "$work/info.txt" >/dev/null
    readelf --debug-dump=loc "$linked_program" >"$work/locations.txt"
    grep -F 'Contents of the .debug_loclists section' "$work/locations.txt" >/dev/null
    grep -F 'DW_OP_bregx' "$work/locations.txt" >/dev/null
    readelf --debug-dump=frames "$linked_program" >"$work/frames.txt"
    grep -F 'CFA' "$work/frames.txt" >/dev/null
    test "$(grep -c ' FDE cie=' "$work/frames.txt")" -ge 3
    if [ "$(uname -m)" = x86_64 ]; then
        grep -E 'r12 .* at cfa-' "$work/frames.txt" >/dev/null
        grep -F 'DW_CFA_restore: r12 (r12)' "$work/frames.txt" >/dev/null
        grep -F 'DW_CFA_def_cfa: r7 (rsp) ofs 8' "$work/frames.txt" >/dev/null
    fi
fi

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) have_elf_dwarf=0 ;;
    *) have_elf_dwarf=1 ;;
esac
if [ "$have_elf_dwarf" = 1 ] && command -v llvm-dwarfdump >/dev/null 2>&1; then
    dwarf_input="$work/program"
    if [ "$(uname -s)" = Darwin ]; then dwarf_input="$work/program.dSYM"; fi
    if [ "$(uname -s)" = Linux ]; then
        # Verify compiler-owned DWARF independently of GCC/compiler-rt units.
        # LLVM's verifier rejects GCC's inlined sanitizer call-site records
        # and older LLVM can loop on them. Linked B++ metadata is still checked
        # with readelf below/above and exercised by the native debugger.
        "$bpp" --debug-info --emit-asm -o "$work/native-dwarf.s" "$work/main.bpp"
        clang -g -c -x assembler "$work/native-dwarf.s" -o "$work/native-dwarf.o"
        timeout 60 llvm-dwarfdump --verify "$work/native-dwarf.o" >"$work/verify.txt"
        dwarf_input="$work/native-dwarf.o"
    else
        llvm-dwarfdump --verify "$dwarf_input" >"$work/verify.txt"
    fi
    llvm-dwarfdump --debug-info --debug-loclists "$dwarf_input" >"$work/llvm-info.txt"
    grep -F 'DW_TAG_subprogram' "$work/llvm-info.txt" >/dev/null
    grep -F 'DW_OP_bregx' "$work/llvm-info.txt" >/dev/null
fi
if command -v llvm-readobj >/dev/null 2>&1; then
    llvm-readobj --sections --unwind "$linked_program" >"$work/readobj.txt"
    grep -E '\.eh_frame|__eh_frame|__compact_unwind|__unwind_info' "$work/readobj.txt" >/dev/null ||
        grep -F 'UnwindInformation' "$work/readobj.txt" >/dev/null ||
        grep -F '.pdata' "$work/readobj.txt" >/dev/null
fi

# Cross-assemble the Windows source and inspect the produced COFF CodeView
# subsections wherever LLVM's object tools are installed. This validates the
# assembler directives as object metadata instead of only matching the text.
if command -v llvm-readobj >/dev/null 2>&1 && command -v clang >/dev/null 2>&1; then
    clang -target x86_64-w64-windows-gnu -g -c -x assembler "$work/raw-windows.s" -o "$work/raw-windows.o"
    llvm-readobj --sections "$work/raw-windows.o" >"$work/windows-sections.txt"
    grep -F '.debug$S' "$work/windows-sections.txt" >/dev/null
    llvm-readobj --codeview "$work/raw-windows.o" >"$work/windows-codeview.txt"
    grep -F 'S_GPROC32' "$work/windows-codeview.txt" >/dev/null
    grep -F 'S_COMPILE3' "$work/windows-codeview.txt" >/dev/null
    grep -F 'S_BUILDINFO' "$work/windows-codeview.txt" >/dev/null
    grep -F 'S_LOCAL' "$work/windows-codeview.txt" >/dev/null
    grep -F 'S_DEFRANGE_REGISTER_REL' "$work/windows-codeview.txt" >/dev/null
    grep -F 'VarName: memory' "$work/windows-codeview.txt" >/dev/null
    grep -F 'S_UDT' "$work/windows-codeview.txt" >/dev/null
    grep -Ei 'Lines \(|Type: Lines|DEBUG_S_LINES|Line table' "$work/windows-codeview.txt" >/dev/null
fi

# When the LLVM Windows linker and PDB inspector are installed, ensure the
# generated COFF CodeView line records survive a real link into a PDB. The
# small stubs satisfy the raw runtime's external references; this image is
# inspected, never executed.
if command -v clang >/dev/null 2>&1 && command -v lld-link >/dev/null 2>&1 && command -v llvm-pdbutil >/dev/null 2>&1; then
    cat >"$work/windows-stubs.s" <<'EOF'
.text
.globl calloc
calloc:
    ret
.globl printf
printf:
    ret
.globl puts
puts:
    ret
.globl strlen
strlen:
    ret
.globl write
write:
    ret
.globl perror
perror:
    ret
.globl bpp_a_call_begin
bpp_a_call_begin:
    ret
.globl bpp_a_call_end
bpp_a_call_end:
    ret
.globl bpp_a_op
bpp_a_op:
    ret
.globl bpp_a_builtin
bpp_a_builtin:
    ret
.globl bpp_a_truth_enable
bpp_a_truth_enable:
    ret
.globl bpp_a_clone_parameters
bpp_a_clone_parameters:
    ret
.globl bpp_a_destination
bpp_a_destination:
    ret
.globl bpp_a_release_object
bpp_a_release_object:
    ret
.globl bpp_a_print_result
bpp_a_print_result:
    ret
.globl bpp_a_trace_enter
bpp_a_trace_enter:
    ret
.globl bpp_a_trace_exit
bpp_a_trace_exit:
    ret
.globl bpp_a_access
bpp_a_access:
    ret
.globl malloc
malloc:
    ret
.globl realloc
realloc:
    ret
.globl free
free:
    ret
.globl exit
exit:
    ret
.globl _write
_write:
    ret
EOF
    for symbol in bpp_s_read bpp_s_write bpp_s_site bpp_s_fail bpp_s_malloc bpp_s_realloc bpp_s_free bpp_s_byte_load bpp_s_byte_store bpp_s_frame_enter bpp_s_frame_leave bpp_s_globals bpp_s_finish bpp_s_print_range ___chkstk_ms; do
        printf ".globl %s\n%s:\n    ret\n" "$symbol" "$symbol" >>"$work/windows-stubs.s"
    done
    clang -target x86_64-w64-windows-gnu -c -x assembler "$work/windows-stubs.s" -o "$work/windows-stubs.o"
    MSYS2_ARG_CONV_EXCL='*' lld-link /debug /nodefaultlib /entry:main /subsystem:console \
        "/out:$work/raw-windows.exe" "/pdb:$work/raw-windows.pdb" \
        "$work/raw-windows.o" "$work/windows-stubs.o"
    llvm-pdbutil dump -modules -files -l -symbols -types "$work/raw-windows.pdb" >"$work/windows-pdb.txt"
    grep -F 'raw_core.bpp' "$work/windows-pdb.txt" >/dev/null
    grep -F 'Lines' "$work/windows-pdb.txt" >/dev/null
    grep -F 'S_GPROC32' "$work/windows-pdb.txt" >/dev/null
    grep -F 'S_LOCAL' "$work/windows-pdb.txt" >/dev/null
    grep -F 'memory' "$work/windows-pdb.txt" >/dev/null
    llvm-pdbutil dump -publics "$work/raw-windows.pdb" >"$work/windows-pdb-publics.txt"
    grep -F 'bpp_source_main' "$work/windows-pdb-publics.txt" >/dev/null
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
        "$bpp" --native-sanitizers --debug-info -o "$work/lldb-runtime-error" tests/lldb_runtime_error.bpp
        "${PYTHON:-python3}" tests/run_bounded.py 60 lldb --batch -o 'settings set use-color false' \
            -o 'breakpoint set --file lldb_runtime_error.bpp --line 6' \
            -o 'breakpoint set --file lldb_runtime_error.bpp --line 15' \
            -o 'breakpoint set --file lldb_runtime_error.bpp --line 7' \
            -o run -o 'breakpoint disable 1' -o continue -o 'thread step-over' \
            -o 'breakpoint disable 3' -o continue -o bt \
            -o 'process kill' -o quit \
            "$work/lldb-runtime-error" >"$work/lldb.txt" 2>&1 || true
        grep -F 'lldb_runtime_error.bpp:6' "$work/lldb.txt" >/dev/null
        grep -E 'lldb_runtime_error.bpp:7|^7[[:space:]]' "$work/lldb.txt" >/dev/null
        # LLDB versions differ in the location suffix (for example 3.1 or
        # 3.2) when one source breakpoint resolves to multiple addresses.
        grep -F 'stop reason = breakpoint' "$work/lldb.txt" >/dev/null
        grep -F 'read_invalid_address(value=' "$work/lldb.txt" |
            grep -F ' at lldb_runtime_error.bpp:15' >/dev/null
        grep -F 'main at lldb_runtime_error.bpp:21' "$work/lldb.txt" >/dev/null

        "$bpp" --native-sanitizers --debug-info -o "$work/lldb-import-recursion" tests/lldb_import_recursion.bpp
        "${PYTHON:-python3}" tests/run_bounded.py 60 lldb --batch -o 'settings set use-color false' \
            -o 'breakpoint set --file lldb_recursive_helper.bpp --line 3' \
            -o 'breakpoint set --file lldb_recursive_helper.bpp --line 10' \
            -o run -o continue -o bt -o 'breakpoint disable 1' -o continue -o bt \
            -o 'process kill' -o quit "$work/lldb-import-recursion" >"$work/lldb-import.txt" 2>&1 || true
        test "$(grep -Ec 'recursive_calls(\([^)]*\))? at lldb_recursive_helper.bpp:3' "$work/lldb-import.txt")" -ge 2
        grep -E 'imported_fault(\([^)]*\))? at lldb_recursive_helper.bpp:10' "$work/lldb-import.txt" >/dev/null
        # main calls imported_fault on line 6 (line 5 initializes result from
        # recursive_calls); the imported fault's caller frame should point at
        # the call site.
        grep -F 'main at lldb_import_recursion.bpp:6' "$work/lldb-import.txt" >/dev/null
    fi
fi

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        test -f "$work/program.pdb"
        llvm-pdbutil dump -symbols -types "$work/program.pdb" >"$work/native-pdb.txt"
        grep -F 'increment' "$work/native-pdb.txt" >/dev/null
        grep -F 'S_LOCAL' "$work/native-pdb.txt" >/dev/null
        if [ "${BPP_REQUIRE_WINDOWS_DEBUGGER:-0}" = 1 ]; then command -v lldb >/dev/null; fi
        ;;
esac
if [ "${BPP_SKIP_DEBUGGER_TESTS:-0}" != 1 ] && command -v lldb >/dev/null 2>&1; then
    program="$work/program"
    if [ -f "$work/program.exe" ]; then program="$work/program.exe"; fi
    if [ "${BPP_REQUIRE_WINDOWS_DEBUGGER:-0}" = 1 ]; then
        # NativePDB resolves source breakpoints against the primary source.
        # Stop at the call site, then run to the imported procedure and its return line.
        "${PYTHON:-python3}" tests/run_bounded.py 60 lldb --batch -o 'settings set use-color false' \
            -o 'breakpoint set --file main.bpp --line 4' -o run \
            -o 'breakpoint set --name increment' -o continue -o 'thread step-over' \
            -o 'frame variable n local' -o 'thread step-over' -o bt \
            -o 'process kill' -o quit "$program" >"$work/variables.txt" 2>&1
    else
    "${PYTHON:-python3}" tests/run_bounded.py 60 lldb --batch -o 'settings set use-color false' \
        -o 'breakpoint set --file library.bpp --line 4' -o run \
        -o 'frame variable n local' -o 'thread step-over' -o bt \
        -o 'process kill' -o quit "$program" >"$work/variables.txt" 2>&1
    fi
    grep -E 'n = 41' "$work/variables.txt" >/dev/null
    grep -E 'local = 42' "$work/variables.txt" >/dev/null
    grep -F 'increment' "$work/variables.txt" >/dev/null
fi

# Binary64 states/rates must expose payload values, not their object address bits.
if [ "${BPP_SKIP_DEBUGGER_TESTS:-0}" != 1 ]; then
    cat >"$work/states.bpp" <<'EOF'
define proc main(){
    new state current=0.5;
    new rate speed=velocity(-1.0,current,3);
    return current+speed;
}
EOF
    "$bpp" --debug-info -o "$work/states" "$work/states.bpp"
    state_program="$work/states"
    if [ -f "$work/states.exe" ]; then state_program="$work/states.exe"; fi
    if command -v lldb >/dev/null 2>&1; then
        "${PYTHON:-python3}" tests/run_bounded.py 60 lldb --batch -o 'settings set use-color false' \
            -o 'breakpoint set --file states.bpp --line 4' -o run \
            -o 'frame variable -P 1 current speed' -o 'process kill' -o quit \
            "$state_program" >"$work/state-variables.txt" 2>&1
        if [ "${BPP_REQUIRE_WINDOWS_DEBUGGER:-0}" = 1 ]; then
            test "$(grep -Ec 'value = 0.5' "$work/state-variables.txt")" -ge 2
        else
            grep -E 'current = 0.5' "$work/state-variables.txt" >/dev/null
            grep -E 'speed = 0.5' "$work/state-variables.txt" >/dev/null
        fi
    elif command -v gdb >/dev/null 2>&1; then
        "${PYTHON:-python3}" tests/run_bounded.py 60 gdb --batch -ex 'set pagination off' -ex 'set confirm off' \
            -ex 'break states.bpp:4' -ex run -ex 'print current' -ex 'print speed' \
            -ex quit "$state_program" >"$work/state-variables.txt" 2>&1
        test "$(grep -Ec '= 0.5$' "$work/state-variables.txt")" -ge 2
    fi
fi

# Linux native debugger coverage mirrors the LLDB smoke test above. Keep the
# breakpoints on source locations so this checks the line table, not symbols
# supplied by the host C compiler. The second fixture verifies imported source
# files and distinct recursive frames before stopping at an imported fault.
if [ "$(uname -s)" = Linux ] && [ "${BPP_SKIP_DEBUGGER_TESTS:-0}" != 1 ] && command -v gdb >/dev/null 2>&1; then
    "${PYTHON:-python3}" tests/run_bounded.py 60 gdb --batch -ex 'set pagination off' -ex 'set confirm off' \
        -ex 'break library.bpp:4' -ex run -ex 'print n' -ex 'print local' \
        -ex next -ex bt -ex quit "$linked_program" >"$work/gdb-variables.txt" 2>&1
    grep -E '= 41$' "$work/gdb-variables.txt" >/dev/null
    grep -E '= 42$' "$work/gdb-variables.txt" >/dev/null
    grep -F 'increment' "$work/gdb-variables.txt" >/dev/null
    "$bpp" --native-sanitizers --debug-info -o "$work/gdb-runtime-error" tests/lldb_runtime_error.bpp
    "${PYTHON:-python3}" tests/run_bounded.py 60 gdb --batch \
        -ex 'set pagination off' \
        -ex 'set confirm off' \
        -ex 'break lldb_runtime_error.bpp:6' \
        -ex 'break lldb_runtime_error.bpp:15' \
        -ex run -ex 'bt' -ex 'disable 1' -ex next -ex 'bt' \
        -ex continue -ex 'bt' -ex 'disable 2' -ex continue -ex 'bt' \
        -ex quit "$work/gdb-runtime-error" >"$work/gdb.txt" 2>&1 || true
    cat "$work/gdb.txt"
    grep -F 'lldb_runtime_error.bpp:6' "$work/gdb.txt" >/dev/null
    grep -E 'lldb_runtime_error.bpp:7|^7[[:space:]]' "$work/gdb.txt" >/dev/null
    grep -F 'read_invalid_address' "$work/gdb.txt" >/dev/null
    grep -F 'main' "$work/gdb.txt" >/dev/null
    grep -E 'SIGSEGV|SIGBUS|Program received signal|B\+\+ sanitizer:' "$work/gdb.txt" >/dev/null

    "$bpp" --native-sanitizers --debug-info -o "$work/gdb-import-recursion" tests/lldb_import_recursion.bpp
    "${PYTHON:-python3}" tests/run_bounded.py 60 gdb --batch \
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
