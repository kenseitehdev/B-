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
ASAN_OPTIONS=detect_leaks=0 make BUILD_MODE=debug test
make BUILD_MODE=debug check-asan-hook
make BUILD_MODE=tsan test

./bin/release/bpp --print-result -o demo tests/demo.bpp
./demo
# prints 11, then 10
```

On Linux use the same commands; pass `CC=cc` if Clang is unavailable.
MemorySanitizer is a separate Linux/Clang-only gate because it requires the
Clang MSAN runtime and a compatible libc environment:

```sh
make CC=clang BUILD_MODE=msan test check-differential check-stress
```

The Makefile rejects `BUILD_MODE=msan` on macOS, Windows, and non-Clang
toolchains instead of silently producing a partially instrumented build.
On Windows run `make CC=clang BUILD_MODE=release test check-differential` in an
MSYS2 UCRT64 shell with make and mingw-w64-ucrt-x86_64-clang installed. The compiler
path there is `bin/release/bpp.exe`. Windows sanitizer modes need a toolchain that
supplies the requested sanitizer runtimes; they are not yet a verified Windows gate.

`test` runs unit tests, the 64-case C suite, and the 64-case native suite.
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
Per-target register inventories, payload-cache rules, and dynamic save frames are
documented in [`docs/register-allocation.md`](docs/register-allocation.md).
Instruction forms, legality, scratch demand, and flags lifetime are documented
in [`docs/instruction-selection.md`](docs/instruction-selection.md).

```sh
./bin/release/bpp --target linux-arm64 --emit-asm -o demo-linux.s tests/demo.bpp
./bin/release/bpp --target windows-x86_64 --emit-asm -o demo-win.s tests/demo.bpp
./bin/release/bpp --emit-ir --no-opt -o demo.ir tests/demo.bpp
./bin/release/bpp --backend c --print-result -o reference tests/demo.bpp
./bin/release/bpp --alloc-profile hot-blocks.txt --emit-map -o demo.json tests/demo.bpp
make BUILD_MODE=release check-targets    # requires a Clang cross-assembler
make BUILD_MODE=release check-msabi     # Windows calling convention on Linux x64
make BUILD_MODE=release check-differential check-stress
make -s BUILD_MODE=release benchmark > instruction-selection-bench.csv
```

`--alloc-profile` is optional and never affects builds that omit it. Its plaintext
format is `function-name block-index positive-multiplier` per non-comment line;
use `*` for every function. The map identifies the blocks affected by a profile,
so profile data cannot become an implicit build dependency.

`benchmark` compares normal instruction selection with legal generic/materialized
forms for immediate arithmetic, immediate comparisons, and calls. It emits a
versioned CSV with executable, `.text` (when the host size tool exposes it), and
assembly-source bytes; 11 samples by default; median and
median absolute deviation of startup-inclusive wall time, and host metadata.
Linux records per-process user-mode hardware cycles through `perf_event_open`
when permitted; Windows records process cycles through `QueryProcessCycleTime`.
macOS intentionally writes `cycle_source=unavailable`: ordinary user processes
cannot portably read Apple Silicon PMU cycles, and timer ticks are not mislabeled
as cycles. Run this on each matching native host and retain a complex selected
form only when its size/timing result is recorded. Set `BPP_BENCHMARK_SAMPLES`
and `BPP_BENCHMARK_RUNS` for longer runs; `make benchmark-smoke` validates the
harness with one sample per variant.

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
- Shared CFG liveness and weighted interval allocation use the target-described
  ARM64 or x86-64 payload pools. Natural-loop depth and fixed native-operation
  costs prioritize hot payload uses deterministically; excess values stay in
  canonical stack cells. Optional profiles can scale selected block weights without
  making the default plan nondeterministic. `--emit-map` exposes block weights,
  branch estimates, baseline comparison, and allocator metrics. `--emit-asm
  --asm-comments` ends each procedure with its target-specific static payload
  spill-store, reload, and safe-rematerialization totals.
- Disjoint virtual values reuse ownership cells. The previous dense quadratic
  interference graph is gone. Ordered active/free-cell heaps make canonical-cell
  reuse `O(n log n)` after interval ordering rather than scanning prior cells.
- Virtual-register liveness uses a reverse predecessor worklist: a block is
  revisited only when a successor's live entry changes. Per-instruction operands
  use a compact sparse table, and live ranges retain holes across mutually
  exclusive branch arms for payload allocation. Allocator workspace is preflighted
  with checked arithmetic and capped at 512 MiB per function; failure is reported
  with the procedure and allocator reason instead of partial output. `--emit-map`
  includes worklist pops, liveness updates, CPU time, allocator-owned workspace
  peak, sparse-use entries, and live-range pieces/holes. The tested envelope is
  16,384 straight-line vregs, 4,096 CFG blocks, and 4,096-register
  loop/branch/call fixtures; the enforced limit is the 512 MiB preflight.
- Cached integer payload writes are deferred until a helper or block boundary
  needs them. A flow-sensitive value-fact lattice carries kind, integer-constant,
  and truthiness proofs across CFG joins; only proofs common to every incoming
  edge remove native integer tag checks or truthiness branches. Unknown facts keep
  the exact runtime check/error path. User calls deliberately invalidate global
  facts because recursion and dynamic returns are not summarized yet.
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

The private procedure/value boundary is **revision 2**, defined in
`include/codegen_abi.h`. Keep this header and the native emitters from the same
source snapshot; shared overflow-checked frame-layout types and helpers live in
this header. This is an ABI placeholder, not a public static-library ABI.
See [CODEGEN.md](CODEGEN.md), [IR-NOTES.md](IR-NOTES.md), and
[CODEGEN-CHECKLIST.md](CODEGEN-CHECKLIST.md) for contracts and remaining work.

## Debugging and build modes

| Mode | Compiler/runtime flags | Compiler path on Unix |
|---|---|---|
| debug (default) | ASAN + LSAN + UBSAN, O1, debug information | bin/debug/bpp |
| tsan | TSAN, O1, debug information | bin/tsan/bpp |
| msan (Linux/Clang only) | MSAN, O1, PIE, debug information | bin/msan/bpp |
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
MSAN validates the compiler and C runtime path for uninitialized-memory use and
includes a runtime self-check. Hand-emitted assembly is not yet shadow-memory
instrumented, so MSAN is not evidence of complete native-instruction coverage.

## Verification in this snapshot

| Check | Result |
|---|---|
| Linux x86-64 release | Unit tests and 54 cases per backend pass |
| Linux x86-64 ASAN/UBSAN | Unit tests and 53 cases per backend pass; leak scanning disabled |
| Linux x86-64 TSAN | Unit tests and 53 cases per backend pass |
| Linux x86-64 MSAN | Clang 18: 53 C/native cases, 64 differential seeds, 5,000-update stress case, and intentional-fault self-check pass |
| Seeded differential execution | 64 seeds, optimized and baseline native outputs match C |
| Windows x64 calling-convention bridge | 53 cases pass on Linux x64; Windows OS behavior is not emulated |
| ARM64 instruction model | 43 execution/error cases pass; not ARM64 hardware execution |
| Cross-target assembly | Mach-O, both ELF targets and COFF assemble in both optimization modes |
| Large procedure | 5000 updates execute on Linux x64; large ARM64 output assembles |
| Allocator scalability | Rejects a preflighted 5,000,000-vreg request; a 4,096-block dependency chain converges through the liveness worklist |
| Debug/unwind metadata | ELF line table and COFF unwind sections inspected |
| macOS ARM64 debug/native test | Passed locally on Apple Silicon with `ASAN_OPTIONS=detect_leaks=0`; leak scanning was disabled for this run |
| Linux ARM64 and Windows native execution | Pending this updated snapshot's native CI runs |

The GitHub workflow runs the matching native host for each target. Unix jobs cover
release/debug/TSAN; Windows currently has a release execution gate. Every job
uploads a 30-day verification transcript recording its commit SHA, runner
architecture, toolchain versions, and test output, so the remaining platform gates
can be closed with specific evidence rather than an inferred green check. A completed
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
