# B++ native codegen: four targets

B++ lowers its hand-rolled register IR directly to ARM64 or x86-64 assembly.
Native is now the default backend. The C backend remains available for comparison.
The compiler and object runtime are written in C; native B++ programs do not pass
through generated C. A host compiler driver still assembles and links the emitted
assembly with the runtime. The custom assembler and linker remain later stages.

This is a complete source snapshot. Its Makefile has an explicit source list;
it does not build the former VM or a libbl.a archive.

## Targets

| Target flag | Architecture and format | Executable toolchain |
|---|---|---|
| `macos-arm64` | ARM64, Mach-O | Apple Silicon macOS, Homebrew LLVM |
| `linux-x86_64` | x86-64, ELF | Linux x86-64, GCC or Clang |
| `linux-arm64` | ARM64, ELF | Linux ARM64, GCC or Clang |
| `windows-x86_64` | x86-64, COFF/PE | Windows x64, MSYS2 UCRT64 Clang |

An executable requires the matching host and runtime objects. All four assembly
outputs can be generated on any host. Cross-linking, foreign SDK discovery,
MSVC syntax, Windows ARM64, and macOS Intel are not implemented.

## Build and test

On your Mac:

```sh
make BUILD_MODE=release test check-differential
make BUILD_MODE=debug test check-asan-hook
make BUILD_MODE=tsan test

./bin/release/bpp --print-result -o demo tests/demo.bpp
./demo
# prints 11, then 10
```

On Linux use the same commands; pass `CC=cc` if Clang is unavailable.
On Windows run `make CC=clang BUILD_MODE=release test check-differential` in an
MSYS2 UCRT64 shell with make and mingw-w64-ucrt-x86_64-clang installed. The compiler
path there is `bin/release/bpp.exe`. Windows sanitizer modes need a toolchain that
supplies the requested sanitizer runtimes; they are not yet a verified Windows gate.

`test` runs unit tests, the 51-case C suite, and the 51-case native suite.
`check` runs unit tests plus C execution; `check-native` runs native execution.
`check-arm64` remains an ARM64 compatibility target. `BUILD_MODE` is the mode
variable; lowercase `mode` is not an alias.

Query the registered target and opcode lowering table instead of maintaining a
separate target list in scripts:

```sh
./bin/release/bpp --list-targets
./bin/release/bpp --target-info linux-x86_64
./bin/release/bpp --emit-asm --asm-comments --target linux-x86_64 -o demo.s tests/demo.bpp
```

Target registry and unsupported-feature policy are documented in
[`docs/backend-feature-policy.md`](docs/backend-feature-policy.md). Numeric
behavior is centralized in [`docs/numeric-semantics.md`](docs/numeric-semantics.md).

```sh
./bin/release/bpp --target linux-arm64 --emit-asm -o demo-linux.s tests/demo.bpp
./bin/release/bpp --target windows-x86_64 --emit-asm -o demo-win.s tests/demo.bpp
./bin/release/bpp --emit-ir --no-opt -o demo.ir tests/demo.bpp
./bin/release/bpp --backend c --print-result -o reference tests/demo.bpp
make BUILD_MODE=release check-targets    # requires a Clang cross-assembler
make BUILD_MODE=release check-msabi     # Windows calling convention on Linux x64
make BUILD_MODE=release check-differential check-stress
make -s BUILD_MODE=release benchmark > timings.csv
```

`--check` parses, lowers, optimizes, and validates without running the program.
`--no-opt` disables the IR optimization pass, while register/frame allocation
still runs. `--print-result` prints the top-level return value, which is not the
process exit status. Runtime errors exit with status 1. The default executable
name is a.out on Unix and a.exe on Windows.

The Makefile selects Homebrew LLVM when available. `CC` selects the build tool;
`BPP_CC` overrides the assembler/link driver at compiler invocation. Both take
one executable name/path, not shell commands. The compiler records checkout and
runtime-object paths: rebuild it after moving the checkout. Generated executables
can run without the checkout. Clean when changing compiler or sanitizer overrides
within an existing build mode.

## Codegen changes

- Native integer operations, comparisons, branches, loops, slot access and
  procedure calls work through target-specific host calling conventions.
- Shared CFG liveness and weighted interval allocation use eight ARM64 or four
  x86-64 callee-saved payload registers. Excess values stay in stack cells.
- Disjoint virtual values reuse ownership cells. The previous dense quadratic
  interference graph is gone; interval scans remain conservative.
- Cached integer payload writes are deferred until a helper or block boundary
  needs them. Proven integer operands skip repeated tag checks inside a block.
- Safe integer constants fold; constant branches and unreachable blocks disappear.
  Effectful operations and operations that must report overflow/zero division remain.
- ARM64 internal branches/calls use page-relative address materialization for
  long distances. x86-64 uses its normal relative branches and RIP-relative data.
- Large frames use page probing on Unix and chkstk on Windows. Unix CFI and
  Windows SEH describe saved registers and stack allocation.
- Native instructions have source line locations; `--emit-map` reports local
  frame offsets and IR locations. Debug snapshots preserve expanded import text.
- Sanitized native builds check global accesses through ASAN/TSAN hooks and track
  native procedure entry/exit for TSAN. Runtime object operations stay instrumented C.

The private procedure/value boundary is **revision 1**, defined in
include/codegen_abi.h. This is an ABI placeholder, not a public static-library ABI.
See [CODEGEN.md](CODEGEN.md), [IR-NOTES.md](IR-NOTES.md), and
[CODEGEN-CHECKLIST.md](CODEGEN-CHECKLIST.md) for contracts and remaining work.

## Debugging and build modes

| Mode | Compiler/runtime flags | Compiler path on Unix |
|---|---|---|
| debug (default) | ASAN + LSAN + UBSAN, O1, debug information | bin/debug/bpp |
| tsan | TSAN, O1, debug information | bin/tsan/bpp |
| release | O2, no sanitizers | bin/release/bpp |

Debug and TSAN builds save `<output>.bpp-source` and emit assembler line directives.
Use `--debug-info` to enable this in release too. Preserve the snapshot next to
its executable for source stepping. `--emit-map -o demo.json` records the selected
target's local offsets relative to x19 or r12. It is an inspection manifest,
not DWARF local-variable DIEs or a standalone debugger.

ASAN hooks cover native global fast paths. Explicit signed-overflow guards provide
integer safety; C runtime helpers cover strings, arrays, buffers and bounds.
Native stack redzones, complete per-instruction instrumentation, use-after-return
tracking, and native local-variable debug records are still unfinished.
Sanitizer runtime availability varies by platform. No threading syntax is added;
TSAN does not make the reference-counted object runtime thread-safe.

## Verification in this snapshot

| Check | Result |
|---|---|
| Linux x86-64 release | Unit tests and 51 cases per backend pass |
| Linux x86-64 ASAN/UBSAN | Unit tests and 51 cases per backend pass; leak scanning disabled |
| Linux x86-64 TSAN | Unit tests and 48 cases per backend pass |
| Seeded differential execution | 64 seeds, optimized and baseline native outputs match C |
| Windows x64 calling-convention bridge | 48 cases pass on Linux x64; Windows OS behavior is not emulated |
| ARM64 instruction model | 43 execution/error cases pass; not ARM64 hardware execution |
| Cross-target assembly | Mach-O, both ELF targets and COFF assemble in both optimization modes |
| Large procedure | 5000 updates execute on Linux x64; large ARM64 output assembles |
| Debug/unwind metadata | ELF line table and COFF unwind sections inspected |
| Actual macOS, Linux ARM64 and Windows execution | Pending this updated snapshot's native CI runs |

The GitHub workflow runs the matching native host for each target. Unix jobs cover
release/debug/TSAN; Windows currently has a release execution gate. The workflow
is included as source and has not been run remotely from this session. A completed
LSAN run is not claimed: this host cannot perform its exit-time process scan.

## Language and roadmap

Procedures use `define proc name(args) { ... }`; fn is no longer a keyword.
Forward calls and recursion work. Blocks do not introduce scopes. Locals shadow
globals throughout their procedure, and declarations skipped at runtime remain
uninitialized. Duplicate declarations, unknown identifiers, nested procedures,
and incorrect direct-call arity are compilation errors.

The supplied dynamic value model remains: String declarations create C-string
buffers; indexing is buffer-only; imports resolve local files and legacy .bl
names under ~/.bytelang/lib. Diagnostics refer to expanded-root locations.
Parsed macro/native quoted snippets remain discarded. Reference counting does
not collect object cycles, and no static type system is introduced.

The roadmap remains: codegen, ABI, assembler, linker, buildFile (build tool and
recipe format), xf static library for bl, debugger, sanitizers. This snapshot
stays in codegen; the remaining codegen gates are listed explicitly before ABI work.
