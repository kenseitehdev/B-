# B++ native codegen: four targets

B++ lowers its hand-rolled register IR directly to ARM64 or x86-64 assembly.
Native is the only executable backend as of 1.0.15. `--backend c` and
`--emit-c` have been removed.
The compiler and object runtime are written in C; native B++ programs do not pass
through generated C. A host compiler driver still assembles and links the emitted
assembly with the runtime. The custom assembler and linker remain later stages.

Release 1.0.16 adds signed truth (-1 false, +1 true), binary64 uncertainty,
state AND/OR/XOR/NOT, 64-bit bitwise operators and TAU (`start => end`).
Propagation conditions explicitly compare `velocity(start,end,dt)` with a
program-supplied threshold. Recoverable errors return U=0 with retained
context. See [truth and TAU](docs/truth-and-tau.md).

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
path there is `bin/release/bpp.exe`. Windows sanitizer modes require the requested runtime. CI executes the
available UBSAN profile with `BUILD_MODE=debug DEBUG_SANITIZERS=undefined`;
unavailable ASAN/LSAN/TSAN runtimes are recorded explicitly.

`test` runs unit, native execution, signed-state/bitwise oracle, strict-profile
and portable diagnostic tests. `check` runs unit, metadata and native execution
tests; `check-native` runs the native fixture suite. `check-static-abi` verifies
separate archives, fractional/error ownership and incompatible manifests.
Bounded differential, fuzz, stress and reproducibility gates are described in
[verification](docs/verification.md).
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
Native relocation limits, code models, range diagnostics, and the future
assembler/linker boundary are documented in
[`docs/branch-address-model.md`](docs/branch-address-model.md).
The static-only B++ library ABI v2, export naming, ownership rules, and archive
fixture are documented in [`docs/static-abi.md`](docs/static-abi.md).

```sh
./bin/release/bpp --target linux-arm64 --emit-asm -o demo-linux.s tests/demo.bpp
./bin/release/bpp --target windows-x86_64 --emit-asm -o demo-win.s tests/demo.bpp
./bin/release/bpp --emit-ir --no-opt -o demo.ir tests/demo.bpp
./bin/release/bpp --no-opt --print-result -o baseline tests/demo.bpp
./bin/release/bpp --alloc-profile hot-blocks.txt --emit-map -o demo.json tests/demo.bpp
make BUILD_MODE=release check-targets    # requires a Clang cross-assembler
make BUILD_MODE=release check-msabi     # Windows calling convention on Linux x64
make BUILD_MODE=release check-static-abi # B++ producer -> archive -> consumer
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
- CFG cleanup threads jump chains, merges sole-predecessor blocks, and trace-orders
  blocks with a deterministic loop-backedge/true-edge policy. Adjacent jumps
  become fall-through; native branch lowering chooses an inverted conditional
  sequence when a successor is adjacent. Failure and cleanup labels remain cold
  out-of-line procedure paths, and the IR verifier runs after each CFG mutation.
- ARM64 internal branches/calls use an `ADRP` page-relative small code model;
  the post-layout guard diagnoses an out-of-range generated label before
  assembly. x86-64 internal B++ calls, cross-block jumps, string references,
  and frame-layout references use `movabs` plus an indirect transfer/address,
  while short conditional branches skip only that nearby sequence. Generated
  x86-64 ELF programs link non-PIE until the B++ linker owns relocation and
  veneer placement.
- `--emit-library --library-name NAME --emit-asm` omits `main`, retains named
  procedures as library roots, and exports static ABI v2 procedure and library
  descriptor symbols. `--emit-library-manifest --library-name NAME` writes the
  matching target archive manifest. Native source may use
  `import library "name";`; `-L directory` adds manifest search paths, checks
  ABI/target/archive identity/export arity before lowering, and passes the
  resolved static archives to the host link driver in dependency order.
  `--emit-link-map` records the resolved archive graph. This is static-only;
  object parsing, relocation, and final link layout remain later linker work.
- Large frames use page probing on Unix and chkstk on Windows. Unix CFI and
  Windows SEH describe saved registers and stack allocation.
- Native instructions have source line locations; `--emit-map` reports local
  frame offsets and IR locations. Debug snapshots preserve expanded import text.
- Sanitized native builds check global accesses through ASAN/TSAN hooks and track
  native procedure entry/exit for TSAN. Runtime object operations stay instrumented C.
- Native assembly allocates private frames, copies borrowed outgoing values,
  performs control flow and integer fast paths, and transfers owned results.
  C leaves handle dynamic states, ownership, checked memory and recoverable
  errors; they do not interpret IR. `check-runtime-boundary` verifies native
  procedure/control-flow output and absence of interpreter entry points.

`--strict-native-runtime` explicitly selects the raw bootstrap profile. It
runs supported word, procedure, memory and output operations without the tagged
value runtime. It rejects state operations and static imports rather than
silently changing their semantics. `--native-sanitizers` selects the portable
fail-fast diagnostic profile. The default profile returns recoverable errors as
uncertainty. These profiles are documented in [native sanitizers](docs/native-sanitizers.md).

The private procedure/value boundary is **revision 2**, defined in
`include/codegen_abi.h`. Keep this header and the native emitters from the same
source snapshot; shared overflow-checked frame-layout types and helpers live in
this header. This is an ABI placeholder, not a public static-library ABI.
See [CODEGEN.md](CODEGEN.md), [IR-NOTES.md](IR-NOTES.md), and
[CODEGEN-CHECKLIST.md](CODEGEN-CHECKLIST.md) for contracts and remaining work.

`Libraries/xf` is the first curated runtime library. It is a static B++ ABI
bridge to XF core—not a general C FFI. Run
`make BUILD_MODE=release check-xf-static-lib` to build it, validate its
manifest, and run a B++ consumer.

## Debugging and build modes

| Mode | Compiler/runtime flags | Compiler path on Unix |
|---|---|---|
| debug (default) | ASAN + LSAN + UBSAN, O1, debug information | bin/debug/bpp |
| tsan | TSAN, O1, debug information | bin/tsan/bpp |
| msan (Linux/Clang only) | MSAN, O1, PIE, debug information | bin/msan/bpp |
| release | O2, no sanitizers | bin/release/bpp |

Debug and TSAN builds save `<output>.bpp-source` and emit assembler line directives.
On macOS, native debug builds also create `<output>.dSYM` so LLDB can resolve the
assembler line table after linking. Use `--debug-info` to enable this in release
too. Preserve the snapshot next to its executable for source stepping.
`--emit-map -o demo.json` records canonical source-variable frame ranges.
DWARF5 exposes procedure, parameter and local records on Mach-O/ELF;
Windows CodeView exposes source names, types and local ranges, and debug
linking writes `<output>.pdb` with LLD. See
[debug metadata](docs/debug-metadata.md) for verification and remaining native gates.

Sanitized raw-native output checks word/byte accesses, pointer offsets,
allocation lifetime, undefined heap bytes and leaks through a portable checked
boundary. Frames have guarded storage and return-lifetime checks. Release output
contains no native sanitizer hooks unless `--native-sanitizers` is explicitly
requested. This opt-in also works without host sanitizer runtimes. The helper-free `--strict-native-runtime`
profile excludes this instrumentation. See [native sanitizers](docs/native-sanitizers.md)
for the contract, TSAN/MSAN limits and platform evidence. Run
`make BUILD_MODE=debug check-native-sanitizer`,
`make BUILD_MODE=release check-native-checks`, and
`make check-sanitizer-availability` to verify the configured toolchain.

## Verification in this snapshot

The 1.0.15 native suites use fixed expected results and an independent seeded
test evaluator. Earlier Linux C/native suite results below are historical
evidence; the C output backend is retired.

| Check | Result |
|---|---|
| Linux x86-64 release | Unit tests and 54 cases per backend pass |
| Linux x86-64 ASAN/UBSAN | Unit tests and 53 cases per backend pass; leak scanning disabled |
| Linux x86-64 TSAN | Unit tests and 53 cases per backend pass |
| Linux x86-64 MSAN | Clang 18: 53 C/native cases, 64 differential seeds, 5,000-update stress case, and intentional-fault self-check pass |
| Seeded differential execution | 64 seeds, optimized and baseline native outputs independently match the test evaluator |
| Windows x64 calling-convention bridge | 53 cases pass on Linux x64; Windows OS behavior is not emulated |
| ARM64 instruction model | 43 execution/error cases pass; not ARM64 hardware execution |
| Cross-target assembly | Mach-O, both ELF targets and COFF assemble in both optimization modes |
| Large procedure | 5000 updates execute on Linux x64; large ARM64 output assembles |
| Allocator scalability | Rejects a preflighted 5,000,000-vreg request; a 4,096-block dependency chain converges through the liveness worklist |
| Debug/unwind metadata | macOS LLDB values/stepping/recursion and dSYM verification pass; COFF source symbols and SEH inspected |
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
`define proc main() { ... }` is the executable entry when present: the module
performs top-level setup first and then calls it. Invoking `bpp` without a
source path compiles `./main.bpp`, which must declare that zero-argument
procedure. Explicit legacy source files without `main` retain top-level-return
behavior during the migration.
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
