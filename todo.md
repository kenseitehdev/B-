B++ compiler roadmap and target checklist
Checklist revision: 1.3
Status snapshot: 2026-10-03 (after explicit allocation locations, helper-boundary live-range splitting, and refreshed Linux x86-64 validation)
Codegen target set: macOS ARM64, Linux x86-64, Linux ARM64, Windows x86-64.
This file tracks work by roadmap stage and native platform. “Implemented” means
code exists. “Cross-assembled” means an object was produced for a foreign target.
“Native verified” means the executable was built and run on that target OS and
architecture. Those are different gates. Do not mark an OS verified because its
assembly assembled on another host or because a calling-convention bridge passed.
Platform/version matrix
Platform profile	Target triple	Codegen implementation	Cross-assembly (current snapshot)	Native release execution (current snapshot)	Native debug/sanitizers
macOS ARM64 v1	macos-arm64 / Mach-O	Implemented	Current revision pending; clang unavailable in this workspace	Prior revision passed on Apple Silicon; current allocator revision needs rerun	Pending for current revision
Linux x86-64 v1	linux-x86_64 / ELF	Implemented	Native assembly/linking passed through the current test suite; cross-target assembly pending (clang unavailable)	Passed current revision: 51 C and 51 native cases, plus differential and stress suites	Current revision: ASAN/UBSAN and TSAN suites passed; LSan exit scan disabled
Linux ARM64 v1	linux-arm64 / ELF	Implemented	Current revision pending; clang unavailable in this workspace	Pending native Linux ARM64	Pending native Linux ARM64
Windows x86-64 v1	windows-x86_64 / COFF/PE	Implemented	Current revision pending; clang unavailable in this workspace	Pending native Windows x86-64	Windows native toolchain/runtime pending; current Linux ms_abi bridge passed 51 cases


Previous macOS evidence: Jay reported a release run on Apple Silicon with 48 C/native cases, 64 optimized/baseline differential cases, cross-assembly for all four targets, and the 5,000-update stress case. That run predates the current explicit-location and split-allocation revision and must be repeated before marking this snapshot native-verified.
Current Linux x86-64 evidence: release `make test`, current debug ASAN/UBSAN `make test`, and current TSAN `make test` each passed unit tests and 51 C plus 51 native cases. Debug used `ASAN_OPTIONS=detect_leaks=0`; the LSan exit scan is therefore unverified. The 64-seed optimized/baseline differential suite, 5,000-update stress case, current 51-case Windows `ms_abi` bridge, and 4096-virtual-value allocator test passed. A focused allocator test confirms physical-register, spill-cell, immediate, rematerializable, and dead locations, including one value switching from register to spill across a helper boundary. `tests/run_tests.sh` now prints each case name while running, so `make check` gives progress instead of remaining silent until completion.
Current cross-target status: assembly text was emitted for macOS ARM64, Linux x86-64, Linux ARM64, and Windows x86-64. This workspace has no `clang`, so `check-targets` could not produce cross-target objects for this revision; previously recorded cross-assembly applies to the prior revision. Linux x86-64 native assembly and linking are covered by the passing native suite.
1. Codegen (CG)
1.1 Shared pipeline and invariants
- [x] Keep the hand-rolled front end and IR as the common input to every backend.
- [x] Make native codegen the default on supported hosts; keep C output as a comparison backend.
- [x] Preserve the --no-opt path for differential testing.
- [x] Keep target selection explicit with --target and reject execution of a foreign-target executable on the wrong host.
- [x] Keep the current procedure/value layout private and revisioned in include/codegen_abi.h (private revision 1).
- [x] Add a documented backend contract for every IR opcode, including value ownership, errors, source positions, and which operations are native or runtime calls.
- [x] Validate IR before and after each transforming pass; report the pass and function/block in diagnostics.
- [x] Add a verifier for definition/use validity, CFG successors, block terminators, register-map bounds, ownership releases, and call argument counts.
- [x] Add optional assembly comments that map each emitted instruction group to IR opcode, function, block, source span, virtual register, physical register, and spill cell.
- [x] Make backend feature/target capabilities queryable rather than duplicating target conditionals across CLI, emitter, tests, and build rules.
- [x] Establish documented numeric semantics once and keep optimizer, C reference backend, native backends, and tests identical for signed overflow, division, modulo, shifts if added, and conversions.
- [x] Establish a policy for unsupported IR or target features: compile-time diagnostic, explicit helper call, or target-specific lowering. No silent wrong-code fallback.
1.2 Register allocation, spills, and frames
1. Broader register allocation
Current implementation: up to eleven ARM64 integer payload candidates (eight callee-saved and three caller-saved), and six x86-64 candidates (four callee-saved and two caller-saved) for both System V and Windows. Integer and null literals are classified as immediates and remain in canonical cells; dynamic tag/payload values are classified as paired values. Tags and ownership cells remain in memory; fixed-role/scratch registers remain reserved. This is a selective payload cache, not a full machine-register allocator.
- [x] Allocate selected payloads to callee-saved ARM64 registers.
- [x] Allocate selected payloads to callee-saved x86-64 registers for both System V and Windows conventions.
- [x] Inventory each target's allocatable integer, floating-point, vector, caller-saved, callee-saved, reserved, frame, and scratch registers.
- [x] Replace hard-coded pools with target descriptions including register class, width, save cost, call-clobber behavior, and reserved status.
- [x] Add caller-saved allocation where its save/restore cost is lower than spilling; model helper and user-call clobbers explicitly.
- [x] Decide whether tags remain memory-resident or can be cached with payloads; document coherence rules for tag/payload pairs.
- [x] Classify dynamic tag/payload values as canonical tagged pairs and integer/null literals as immediates. Literals remain in canonical cells for helper/error visibility and no longer take payload registers.
- [x] Classify proven-safe rematerializable expressions. Same-block integer expressions with known constant inputs are marked only when checked evaluation succeeds; operations that could overflow, divide by zero, evaluate `INT64_MIN / -1`, depend on dynamic inputs, or have effects remain unclassified. The computed result is allocator metadata and may be reported as a rematerializable location when spilled. The emitter still executes the original IR and every value retains its canonical cell; recomputation and stack-home removal remain open.
- [x] Generate each function's additional callee-save set from its allocated physical registers; allocator tests check the derived set at zero, exact-capacity, and over-capacity pressure.
- [ ] Verify stack alignment, callee-saved restoration, scratch ownership, and call-clobber behavior on every native target. Current Linux x86-64 release, ASAN/UBSAN debug (with LSan scanning disabled), and TSAN suites exercise calls, recursion, frames, and errors; the current Windows `ms_abi` bridge passes 51 cases. These do not close native macOS ARM64, Linux ARM64, or Windows x86-64 verification. The matching native runs remain pending.
- [x] Test zero/one capacity, exact callee-save capacity, pool-plus-one spills, and high-pressure values across calls, CFG joins, loop backedges, returns, and checked-operation error/helper paths for all four target descriptions. Runtime recursion, cleanup-error, and call-pressure cases pass on Linux x86-64; the Windows calling-convention bridge also passes.
2. Actual spill management
Current implementation: each virtual value has a canonical ownership cell, reused across nonoverlapping value lifetimes. Allocation is represented as per-instruction-position segments: a segment may use a physical register, spill cell, or rematerializable expression; integer/null constants are reported as immediate locations and dead values as unallocated. The legacy `physical[]` field is only a summary when a value uses one register for all of its segments. `--emit-map` exports the segments and instruction positions. Both native emitters query this map, flush before unconditional helpers/calls, and reload live values into the location selected for the following segment. Splitting currently occurs at unconditional helper boundaries; arbitrary hot/cold CFG splitting and edge-specific spill placement remain open.
- [x] Define allocator locations explicitly: physical register, spill cell, constant/immediate, rematerializable expression, and unallocated/dead. `bpp_reg_location_at` is the per-position query, and `--emit-map` reports allocation segments. Canonical ownership cells remain allocated for all virtual values.
- [x] Split live ranges at unconditional helper/call boundaries and allocate each segment independently. A focused pressure fixture verifies one value is register-resident in one segment and spilled in another. This is helper-boundary splitting, not general hot/cold CFG splitting; arbitrary split points and edge-specific transitions remain open.
- [ ] Place spill stores only where a value is live across a clobber or must be visible to a helper/cleanup path.
- [ ] Place reloads immediately before first use, avoid duplicate reloads, and hoist only when correctness and profiling justify it.
- [ ] Track dirty state by allocation location and CFG edge; remove conservative flushes when the value is dead or the edge proves it unnecessary.
- [ ] Model helper calls, user calls, builtins, runtime errors, return cleanup, and sanitizer callbacks as explicit clobber/visibility points.
- [ ] Handle critical edges and split blocks when a spill/reload belongs on only one successor path.
- [ ] Verify that every error edge observes a valid tag, payload, and ownership state before cleanup.
- [ ] Add allocator diagnostics that identify why each spill/reload was inserted.
- [ ] Expand split-allocation tests to cover values selectively live on one branch, reused after a loop, and replaced by an owned object. Current tests cover helper/call boundaries, register-to-spill transitions, call pressure, recursion, CFG joins/backedges, and owned objects in the broader execution suite; edge-specific split placement is not implemented.
3. Smaller stack frames
Current implementation: non-overlapping virtual-value intervals reuse physical ownership cells. All virtual values still receive a stack home, and local slots remain separately allocated.
- [ ] Reuse local slots when declaration lifetimes cannot overlap and shadowing/initialization semantics permit it.
- [ ] Reuse outgoing argument cells between calls and size outgoing areas from the maximum simultaneously needed arguments.
- [ ] Avoid homes for values proven never to reach a helper, call, cleanup scan, debug location, or spill.
- [ ] Separate GC/ownership roots from ordinary spills so only values requiring runtime visibility need root cells.
- [ ] Allocate frame regions by alignment/lifetime: header, roots, spills, locals, flags, outgoing arguments, and target-required shadow space.
- [ ] Remove unused layout-map entries or encode identity mappings compactly when that is cheaper.
- [ ] Preserve required stack alignment for every call and each target's ABI.
- [ ] Check frame-size arithmetic for overflow and produce a source-located diagnostic before emission.
- [ ] Add tests that assert frame-byte counts for sequential expressions, nested calls, many locals, recursion, and large frames.
- [ ] Report frame bytes and peak outgoing-call bytes in --emit-map or a dedicated frame report.
4. Better allocation decisions
Current implementation: a conservative weighted-interval heuristic considers native operand use and gives a backedge block a boost. This is not profile-guided allocation.
- [ ] Count uses by operation cost, including native fast-path benefit, helper-call cost, tag checks, spill traffic, and rematerialization cost.
- [ ] Compute natural loops/backedges and loop depth rather than treating every backward edge equally.
- [ ] Weight hot loops, call-heavy paths, error paths, and cold blocks separately; establish how weights are selected without runtime profiling.
- [ ] Include call-crossing cost and caller/callee-save cost in register selection.
- [ ] Prefer values with long hot live ranges while avoiding cache occupancy for cold/dead spans.
- [ ] Reconcile allocation weights with block layout and branch probability estimates.
- [ ] Add optional profile data later without making profile data required for deterministic builds.
- [ ] Compare allocation choices against a simple baseline and record spills, reloads, cache hits, flush bytes, and compile time.
- [ ] Add tests proving heuristic changes preserve output and do not increase spills on known pressure fixtures.
5. Scalable liveness and allocation
Current implementation: the dense quadratic interference graph is gone. Block live sets use bitsets and sparse set-bit iteration; conservative intervals and cell reuse remain.
- [ ] Bound memory for functions with many blocks and many virtual registers; diagnose allocation failure without corrupting the module.
- [ ] Measure liveness convergence on loops with many backedges and irreducible CFGs.
- [ ] Avoid rescanning every virtual register for each instruction when a sparse use/def representation is cheaper.
- [ ] Consider worklist dataflow so only predecessor blocks affected by a changed live set are revisited.
- [ ] Improve interval construction for values with holes across mutually exclusive branches.
- [ ] Avoid quadratic interval/cell scans with free-cell heaps, ordered active intervals, or another measured data structure.
- [ ] Make size arithmetic safe for bitset word counts, block/register products, interval arrays, and target metadata.
- [ ] Test 4096 values (currently covered) and larger generated functions at multiple block/register scales.
- [ ] Record compile time and peak memory for synthetic straight-line, loop-heavy, branch-heavy, and call-heavy programs.
- [ ] Set a supported compiler-function size based on evidence; reject pathological input with a clear diagnostic if necessary.
6. Static value information
Current implementation: block-local known result kinds and integer slot constants eliminate some tag checks and permit some folding. There is no static type system.
- [ ] Define a value-kind lattice for unknown, null, integer, string/buffer, array, and other objects, with merge rules at CFG joins.
- [ ] Distinguish “known kind” from “known constant” and from “known truthiness.”
- [ ] Track kinds through every IR opcode, builtin, user call, assignment, release, delete, free, and runtime fallback.
- [ ] Invalidate global facts across calls; preserve local facts only where aliasing and language semantics prove safety.
- [ ] Add interprocedural summaries for known argument/result kinds only if recursion and dynamic returns can be modeled soundly.
- [ ] Use known kinds to remove tag tests only when all incoming control-flow paths prove the same kind.
- [ ] Simplify truthiness tests for known null/integer/object values without changing empty-object semantics.
- [ ] Keep error behavior identical for dynamic type errors and uninitialized loads.
- [ ] Test loops, joins, recursion, object-to-integer reassignment, self-assignment, deletion, free aliases, and calls that mutate globals.
7. Redundant-access elimination
Current implementation: cached integer payloads defer stores; some known integer tag checks and repeated initialization-flag writes are omitted. Slot accesses and many synchronization flushes remain.
- [ ] Add local value numbering or a similar pass for safe repeated pure loads/expressions.
- [ ] Eliminate repeated local/global slot loads only when calls, stores, aliasing, volatile rules, and error behavior cannot change the result.
- [ ] Remove redundant stores when the value is unchanged and the store has no required initialization/error effect.
- [ ] Remove duplicate tag loads and initialization checks after preserving the first check's diagnostic path.
- [ ] Track memory effects by operation: globals, locals, objects, buffer/array contents, allocation, output, and possible error.
- [ ] Reduce dirty-payload flushes using liveness at individual edges and helpers, not only block-level boundaries.
- [ ] Keep sanitizer hooks, debug visibility, and runtime ownership synchronization as observable effects where required.
- [ ] Count emitted loads/stores/branches/helper calls per target and compare before/after on fixed fixtures.
- [ ] Add tests with calls between loads, side-effecting builtins, aliasing arrays/buffers, and failures on the second access.
8. Constant folding and propagation
Current implementation: overflow-safe integer folding, safe division/modulo handling, constant-branch simplification, and integer slot propagation within a block. Operations that must report overflow or division-by-zero remain executable.
- [ ] Specify folding rules per opcode and value kind, including signed boundaries, equality, truthiness, strings, and conversions.
- [ ] Implement a constant lattice and merge facts across predecessor edges.
- [ ] Propagate constants through loop preheaders and loop-invariant expressions without assuming a loop executes.
- [ ] Keep potentially failing constant operations at runtime when folding would suppress or move a language-visible error.
- [ ] Preserve source location for folded operations so a diagnostic still points to the correct expression.
- [ ] Fold safe unary/binary operations and comparisons for every target consistently.
- [ ] Add branch and switch folding once those IR forms exist.
- [ ] Test INT64_MIN/MAX, multiply overflow, zero division/modulo, minimum/-1, nested branches, loops, and calls that mutate globals.
- [ ] Run every differential fixture with optimization enabled and disabled.
9. Dead-code elimination
Current implementation: constant unreachable branches/blocks and unreferenced private procedures are removed; some unused primitive literal writes/releases are removed. There is no general effect-aware DCE.
- [ ] Define an effect table for all IR operations: output, allocation, retain/release, mutation, global access, potential runtime error, and control flow.
- [ ] Remove dead pure computations only when dropping them cannot suppress overflow, division, bounds, uninitialized-variable, or type errors.
- [ ] Remove dead stores only when initialization semantics and error behavior remain intact.
- [ ] Remove unused cleanup/release operations only with ownership proof; do not leak objects or release aliases early.
- [ ] Remove dead blocks and repair branch targets, source maps, and function IDs safely.
- [ ] Keep exported/address-taken procedures once a public ABI or library model introduces them.
- [ ] Add tests for effectful dead-looking calls, errors in unused expressions, recursive-but-unreachable procedures, and object cleanup.
10. Instruction selection
Current implementation: direct ARM64 and x86-64 integer operations, immediate arithmetic, local/global fast paths, branches, and calls. Dynamic operations use C helpers.
- [ ] Define an instruction-selection table for every IR opcode on every target.
- [ ] Add legality/cost checks for immediate forms, extensions, shifts, multiply-add, compare-branch, address generation, and load/store addressing modes.
- [ ] Select instructions using register constraints and operand locations rather than emitter-local special cases.
- [ ] Add target-specific lowering for useful operations while keeping fallback semantics and errors identical.
- [ ] Model flags/condition-code lifetime so instruction combines do not overwrite live conditions.
- [ ] Model scratch-register demand before choosing a sequence; avoid hidden scratch conflicts.
- [ ] Add assembly golden tests and executable tests for each selected form, including negative/overflow boundaries.
- [ ] Measure code size and cycles on each native CPU before retaining a more complex sequence.
11. Control-flow optimization
Current implementation: constant branches simplify and jumps to the next block are omitted. Blocks are otherwise emitted in existing order.
- [ ] Lay out blocks to favor fall-through on likely edges while keeping branch targets and source locations correct.
- [ ] Add branch inversion to remove unconditional jumps after conditional branches.
- [ ] Merge compatible blocks and remove empty jump-only blocks.
- [ ] Simplify chained jumps and redundant branches after CFG mutation.
- [ ] Preserve loop headers, call return sites, unwind ranges, and debug locations during reordering.
- [ ] Separate cold error/cleanup paths where doing so improves hot code without making branches exceed range.
- [ ] Add CFG validation after every block-reordering/merging pass.
- [ ] Test nested loops, diamonds, early returns, error paths, and long cold blocks on every emitter.
12. Large-program branch and address handling
Current implementation: ARM64 internal transfers materialize page-relative addresses; x86-64 uses relative branches/calls and RIP-relative data references. Architecture and object-format range limits remain.
- [ ] Enumerate range limits for ARM64 ADRP relocations, literal/data references, x86-64 rel32 branches/calls, and each object format.
- [ ] Add a post-layout range checker that reports the source instruction, target label, and exceeded range.
- [ ] Add ARM64 branch veneers/islands or a documented code model when a target exceeds branch/page range.
- [ ] Add x86-64 branch islands or long-call sequences for text beyond rel32 range.
- [ ] Coordinate symbol, relocation, section layout, and veneer rules with the future assembler/linker.
- [ ] Test at boundaries using generated object layouts; do not require multi-gigabyte CI fixtures.
- [ ] Keep short local conditional branches short and long cross-block transfers range-safe.
- [ ] Test Mach-O, ELF ARM64, ELF x86-64 and COFF relocations independently.
13. Formal ABI call lowering — depends on stage 2
Current implementation: private revision-1 procedure frames and per-target host calling conventions. Do not turn this private frame into a public promise before the ABI is designed.
- [ ] Stage 2 defines calling convention, value representation, alignment, ownership, error propagation, symbol names, visibility, versioning, and compatibility rules.
- [ ] Define argument classification for the deliberately small set of B++ types exposed by static libraries; add aggregates only when the language has a concrete need.
- [ ] Define return classification and error/status behavior for supported B++ values; defer multiple returns and complex aggregates unless required by the language.
- [ ] Define stack alignment, stack arguments, callee/caller-saved registers, and target-required shadow/home space for B++ library/runtime calls. Variadic calls are out of scope for the public ABI.
- [ ] Define the B++-to-B++ static-library call convention and B++ symbol/export rules. Keep system entry points in the private runtime boundary.
- [ ] Replace private call lowering only after the ABI spec and conformance tests are accepted.
- [ ] Add separately compiled B++ library producer/consumer fixtures for macOS ARM64, Linux x86-64/ARM64 and Windows x86-64.
- [ ] Add ABI compatibility tests across compiler versions before static library work.
14. Further runtime independence
Current implementation: arithmetic, comparisons, core branches and user-procedure control flow are native. Startup, value ownership, strings, buffers, arrays, builtins, dynamic operations, and error reporting still use C runtime objects/helpers.
- [ ] Inventory every emitted helper symbol and map it to runtime source, ownership contract, sanitizer behavior, and target calling convention.
- [ ] Decide which helpers remain in the runtime and which become direct native sequences.
- [ ] Design a B++ runtime boundary compatible with the formal ABI and assembler/linker object model.
- [ ] Implement native startup/exit, global initialization, frame entry/cleanup, and error propagation.
- [ ] Implement or retain value clone/release, allocation, strings, buffers, arrays, indexing, and builtin calls with identical semantics.
- [ ] Define allocator failure behavior, integer-size conversion, alignment, and object layout before replacing C allocation.
- [ ] Add ownership/refcount tests for aliases, nested arrays, self-assignment, free/delete, errors, recursion and returned values.
- [ ] Run ASAN/LSAN/UBSAN/TSAN against both old and replacement runtime paths where available.
- [ ] Remove C runtime dependency only when all required program behaviors pass on all targets.
15. Debug metadata
Current implementation: assembler source/line directives, host unwind data, and --emit-map local offsets/IR positions. It does not yet describe optimized variable locations in standard debug records.
- [ ] Preserve original source spans through imports, macros, lowering, folding, DCE and block reordering.
- [ ] Represent source files and include/import stacks instead of mapping all expanded code to one sidecar.
- [ ] Emit procedure names, parameter names/types, lexical scopes, and source-file tables.
- [ ] Emit DWARF location lists/register locations and Windows CodeView/PDB-compatible information as appropriate.
- [ ] Track a variable that moves between register and spill slot over time.
- [ ] Describe optimized-out variables honestly instead of reporting stale frame cells.
- [ ] Keep unwind rules correct through prologue/epilogue, stack probes, veneers, helper calls, and every return/error path.
- [ ] Add metadata inspection tests with llvm-dwarfdump, readelf, llvm-readobj, and platform-native tools.
- [ ] Test source stepping through loops, calls, recursive frames, imports and runtime errors on each supported debugger.
16. Native sanitizer instrumentation
Current implementation: host compiler sanitizer modes instrument C components/runtime. Native global fast paths call ASAN/TSAN hooks; TSAN procedure entry/exit hooks expose native frames. Native stack accesses are not fully instrumented. UBSAN-relevant integer behavior uses explicit language checks.
- [ ] Define which sanitizer checks are language guarantees versus compiler debugging instrumentation.
- [ ] Specify shadow-memory mapping and runtime hooks independently of Clang/GCC sanitizer ABI where portability requires it.
- [ ] Instrument native loads/stores, pointer arithmetic, array/buffer bounds, integer overflow, invalid tags, and allocation/free transitions.
- [ ] Add stack redzones, frame poisoning/unpoisoning, return poisoning and use-after-return detection.
- [ ] Avoid duplicate or missing reports when native access transitions through C helpers.
- [ ] Define TSAN memory events, atomic semantics, synchronization operations, and limitations until B++ threads exist.
- [ ] Define leak tracking across native and runtime allocations, including cycles and global roots.
- [ ] Verify ASAN, LSAN, UBSAN and TSAN runtime availability for each compiler/OS/architecture combination.
- [ ] Add intentional-fault tests for each check and assert source location, category and exit behavior.
- [ ] Keep production/release code free of sanitizer hooks unless explicitly enabled.
17. Broader verification and release gates
Current implementation: unit/runtime tests; 51-case C/native suites on Linux x86-64; 64-seed optimized/baseline differential tests; register pressure including exact callee-save capacity and CFG cases; mapped-cell ownership; 4096-value reuse; large frames; 5,000-update large procedure; and sanitizer hooks. Cross-assembly passed for the prior source revision. Current assembly text emits for all four targets, but this environment lacks clang, so current cross-assembly and native CI remain outstanding.
- [ ] Run all tests on every supported native OS/architecture; cross-assembly is a separate gate.
- [ ] Keep the C backend as a temporary semantic oracle and compare exit status, stdout, stderr category, and returned value.
- [ ] Extend deterministic randomized programs across arithmetic, branch, loop, call, recursion, string, buffer, array and error behavior.
- [ ] Add seed reporting and save the smallest reproducer for every generated failure.
- [ ] Add deep recursion, stack exhaustion, many parameters, high live-value pressure, many locals, huge block count, large text section, and frame-probe boundary tests.
- [ ] Test both optimized and --no-opt native modes for every fixture.
- [ ] Test debug metadata builds and sanitizer builds separately from release.
- [ ] Run native CI on macOS ARM64, Linux x86-64, Linux ARM64 and Windows x86-64 and retain logs/artifacts.
- [ ] Add compiler warning-as-error builds on Clang and GCC-compatible toolchains where supported.
- [ ] Fuzz lexer/parser/IR validator/optimizer/emitter with resource limits and sanitizer builds.
- [ ] Test reproducible assembly/object output or document nondeterministic fields.
- [ ] Gate a target release on its native runtime suite, ABI conformance, unwind, debug, and supported sanitizer checks.
18. Performance and resource benchmarks
Current implementation: a CSV harness compares C, native optimized and native --no-opt execution time plus executable size for loop/call/object workloads. Existing measurements are a smoke check, not an isolated baseline.
- [ ] Separate compile time, assembly time, link time, startup time and steady-state execution time.
- [ ] Add warmup, repeated samples, median/dispersion, environment metadata and stable machine identifiers.
- [ ] Measure code size, read-only data, frame size, maximum stack depth, spill/reload count, helper-call count, and compilation peak memory.
- [ ] Include integer-heavy, branch-heavy, recursive, call-heavy, object-heavy, string/buffer/array, and large-source programs.
- [ ] Compare C reference, optimized native, unoptimized native, and later assembler/linker builds.
- [ ] Run on macOS ARM64, Linux x86-64, Linux ARM64 and Windows x86-64 native hosts.
- [ ] Keep results in versioned CSV/JSON with compiler revision, flags, target triple, CPU/OS and sanitizer mode.
- [ ] Set performance expectations only after repeatable runs; avoid pass/fail thresholds sensitive to shared CI noise.
19. Target support and platform-specific integration
The four emitters exist. macOS ARM64 has a user-reported native release run from the prior allocation revision; Linux x86-64 has the current 51-case native release run. Linux ARM64 and Windows x86-64 still lack native OS release runs. Keep each target's object, runtime, linker, debugger, and sanitizer checks separate.
- [ ] Keep target IDs, triples, object formats, symbol prefixes, sections, relocations, stack rules and helper conventions in one target descriptor.
- [ ] Validate host-target detection on Apple Silicon, Linux x86-64/ARM64, and Windows x86-64; reject unsupported hosts clearly.
- [ ] Test spaces, Unicode, drive letters, case-insensitive paths, temp files, sidecar paths and source-overwrite protection on Windows.
- [ ] Validate Mach-O relocations/unwind/debug info on macOS tooling.
- [ ] Validate ELF relocations, GNU-stack markings, CFI, stack probes and debug info on Linux x86-64/ARM64.
- [ ] Validate PE/COFF sections, SEH unwind data, chkstk, startup, imports, CRT and debug information on Windows x86-64.
- [ ] Decide minimum OS/SDK/toolchain versions and document supported compiler drivers.
- [ ] Keep macOS Intel, Windows ARM64 and other architectures explicitly unsupported until separately designed and tested.
1.20 Platform-specific CG checklist
macOS ARM64 — target profile v1
- [x] Emit ARM64 Mach-O assembly.
- [ ] Re-run assembly, linking, and execution of the release compiler and generated program on Apple Silicon after the explicit-location/live-range-splitting update; the prior revision passed.
- [ ] Re-run unit, C/native execution, differential, cross-target assembly and large-procedure checks for this revision. The prior Apple Silicon run passed 48 cases and predates the current allocator revision.
- [ ] Run make BUILD_MODE=debug test check-asan-hook on the Mac and record the exact sanitizer runtimes available.
- [ ] Run make BUILD_MODE=tsan test on the Mac.
- [ ] Run and archive repeated native benchmarks and frame/code-size results.
- [ ] Inspect Mach-O unwind and debug output with Apple/LLVM tools; test backtraces through B++ calls/errors.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.
Linux x86-64 — target profile v1
- [x] Emit System V x86-64 ELF assembly.
- [x] Pass the current Linux x86-64 release suite (51 C/native cases), 64-seed differential suite, 5,000-update stress test and runtime/unit tests.
- [x] Run TSAN on the current Linux x86-64 source revision; the complete unit, C, and native suites passed. ASAN/UBSAN debug `make test` also passed with LSAN exit scanning disabled; a host run with LSan scanning enabled remains open.
- [x] Exercise the Windows calling convention with the Linux ms_abi bridge; keep it classified as a bridge test only.
- [ ] Run LSAN on a host/container where its exit-time process scan works.
- [ ] Run repeated isolated benchmarks and inspect CFI/source-line/backtrace behavior.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.
Linux ARM64 — target profile v1
- [x] Emit Linux ARM64 ELF assembly using the shared ARM64 instruction selector.
- [ ] Re-run current-revision cross-assembly in optimized and baseline modes; prior revision passed.
- [ ] Run the compiler and generated executables natively on Linux ARM64.
- [ ] Pass unit tests, the C suite, the native suite, optimized/baseline differential suite and 5000-update stress case.
- [ ] Run ASAN/UBSAN/TSAN/LSAN where the actual compiler runtime supports them; record unsupported combinations explicitly.
- [ ] Inspect ELF relocations, CFI, stack probing, source lines and backtraces on ARM64.
- [ ] Run isolated benchmarks and compare with Linux x86-64 and macOS ARM64.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.
Windows x86-64 — target profile v1
- [x] Emit Windows x64 COFF assembly and SEH prolog/epilog directives.
- [ ] Re-run current-revision cross-assembly in optimized and baseline modes; prior revision passed.
- [x] Pass the current 51-case Linux `ms_abi` calling-convention bridge suite; this does not test Windows OS behavior.
- [ ] Build and run compiler and generated executables on native Windows x64.
- [ ] Pass unit tests, the C suite, native suite, optimized/baseline differential suite and 5000-update stress case.
- [ ] Verify Windows CRT startup, symbol entry point, shadow space, stack arguments, ___chkstk_ms, guard pages, PE linking and SEH unwinding.
- [ ] Validate paths containing spaces, mixed separators, drive letters, Unicode, case-only aliases, temp files and source/debug-snapshot overwrite protection.
- [ ] Determine MinGW/UCRT/Clang toolchain support and whether MSVC syntax/tooling is in scope.
- [ ] Run debug/sanitizer tests only for runtimes that exist; document ASAN/LSAN/UBSAN/TSAN gaps without treating unsupported runtimes as passing.
- [ ] Inspect CodeView/PDB or chosen debug format and Windows backtraces.
- [ ] Run isolated benchmarks and record binary/frame sizes.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.
1.21 CG exit criteria before moving to ABI
- [ ] Native release test matrix passes on macOS ARM64, Linux x86-64, Linux ARM64 and Windows x86-64.
- [ ] Every target has passed semantic differential tests, large procedure/frame tests, recursion, pressure, error cleanup, target calling-convention tests, unwind tests and debug-line tests.
- [ ] Debug/sanitizer results are recorded per compiler/runtime/OS, including unsupported combinations and LSAN scan limitations.
- [ ] Allocator/spill/frame design is documented and stress results show bounded behavior for project-sized code.
- [ ] Branch/relocation limits and diagnostics are documented for each current object format.
- [ ] Native runtime dependencies and the C components that remain are listed for the ABI/runtime design.
- [ ] Benchmarks are collected for all four host profiles and saved with reproducible environment metadata.
- [ ] The unresolved shared tasks in sections 1.2–1.19 are reviewed explicitly; no “partial” item is silently promoted to complete.
- [ ] Only after these gates are reviewed, begin roadmap stage 2 and freeze the public ABI contract.
2. ABI
Scope: a functional ABI for statically linking B++ libraries into B++ programs. Keep the public boundary small and stable. Broad FFI, general C ABI compatibility, arbitrary callbacks into foreign runtimes, shared libraries and dynamic loading are out of scope. C code may remain inside the compiler/runtime as an implementation detail; that alone does not make C interoperability part of the B++ ABI.
Dependency: CG call/frame requirements must be known. Public libraries must not depend on the temporary private revision-1 frame layout.
ABI goals
- [ ] A library author can publish B++ procedures for use by another B++ program.
- [ ] A separately compiled B++ consumer can resolve symbols, call procedures, exchange supported B++ values, handle errors and link without a dynamic loader.
- [ ] The library/linker can check target, ABI revision, runtime requirement, dependencies, imports and exports before creating an executable.
- [ ] Compiler/runtime internals such as BLValue, refcounts, allocator structures and the current private frame remain hidden behind the public boundary.
- [ ] The supported public type set is deliberately small. Unsupported types are diagnosed instead of implicitly exposing a C or compiler-specific layout.
ABI design and implementation
- [ ] Write an ABI goals/non-goals specification that explicitly excludes broad FFI, dynamic loading, shared-library lifecycle, arbitrary foreign callbacks and a general-purpose C ABI.
- [ ] Decide which language entities cross a static-library boundary in version 1: procedures first; add public globals, constants or types only when the language/library feature needs them.
- [ ] Define public value representation or opaque handles. Do not expose C struct layout, private frame headers, refcount fields or allocator internals accidentally.
- [ ] Define the initial supported parameter and return types. Specify integer widths, signedness, pointer size, alignment, endianness, string encoding/length, nullability, and array/buffer ownership if those types are included.
- [ ] Define ownership for parameters and results: borrowed inputs, retained inputs, moved results, object lifetime, cleanup on normal return, cleanup on error and aliasing.
- [ ] Define how B++ errors cross a library call and how callers observe diagnostics. Prevent unwinding through code without the same runtime contract.
- [ ] Define argument/return lowering for macOS ARM64, Linux x86-64, Linux ARM64 and Windows x86-64, including stack alignment, preserved registers, stack arguments and Windows shadow space.
- [ ] Define B++ symbol names, module/namespace rules, exported/private visibility, duplicate handling, version tags and dead-stripping rules.
- [ ] Define static archive metadata: ABI revision, compiler/library format revision, target triple, object format, runtime dependency, dependent archives, imports, exports and link requirements.
- [ ] Decide whether a library needs saved IR/codegen metadata or consists of target object files plus a small B++ manifest.
- [ ] Define compatibility rules across compiler versions and diagnostic behavior for mismatched ABI revisions or target triples.
- [ ] Keep platform/system calls behind a narrow runtime boundary. Add a specific host hook only when B++ startup/runtime requires it; do not turn it into general user-facing FFI.
- [ ] Implement B++ library imports/exports and call lowering after the contract is stable.
- [ ] Add link checks for unresolved and duplicate B++ symbols, wrong target, wrong ABI revision, missing runtime/dependency, and unsupported public types.
- [ ] Build library producer and consumer as separate B++ projects to catch accidental dependence on shared compiler state.
- [ ] Add conformance fixtures for multiple arguments, recursion, supported object returns, ownership transfer, runtime errors and private symbols on each native target.
- [ ] Add negative fixtures for every unsupported type/call form and every artifact mismatch.
- [ ] Keep private runtime bridge symbols unexported; document migration from private revision 1.
Explicit ABI non-goals for v1
- [ ] No general-purpose C ABI or automatic C type marshalling.
- [ ] No arbitrary calls from B++ into foreign libraries through source-level FFI.
- [ ] No callbacks from arbitrary foreign code into B++ procedures.
- [ ] No shared-library lifecycle, dynamic loader, plugin ABI or symbol interposition.
- [ ] No public dependency on the current C BLValue layout or private runtime frame.
- [ ] Revisit a narrow interop feature only when a concrete system/runtime need cannot be met by B++ libraries and the private platform runtime boundary.
3. Assembler
Dependency: finalized target instruction forms, relocation model, ABI symbols, unwind and debug metadata.
- [ ] Define a versioned B++ assembly language or structured instruction format.
- [ ] Specify lexical grammar, labels, expressions, sections, alignment, visibility, directives and comments.
- [ ] Specify relocations for branches, calls, symbols, constants, TLS, imports and veneers on each object format.
- [ ] Implement parsing, validation, diagnostics with line/column, and deterministic encoding.
- [ ] Implement ARM64 and x86-64 instruction encoders for every opcode emitted by codegen.
- [ ] Implement Mach-O ARM64, ELF ARM64/x86-64, and COFF/PE x86-64 sections/symbol tables/relocations.
- [ ] Implement unwind and debug section emission or a documented later stage if a first assembler omits them.
- [ ] Add golden byte tests, disassembly round trips, relocation boundary tests and malformed-input tests.
- [ ] Cross-check emitted objects against LLVM/Apple/MinGW tool readers.
- [ ] Replace Clang/GCC assembly invocation only when object compatibility tests pass.
4. Linker
Dependency: assembler object format, static B++ ABI symbol rules, runtime object format and executable entry points.
- [ ] Define B++ linker inputs and output modes: executable, object, and static B++ library. Shared-library output is outside the initial ABI scope.
- [ ] Parse object files and validate machine, format, ABI version, sections, symbols and relocations.
- [ ] Resolve strong/weak/import/export symbols and diagnose duplicates/unresolved references.
- [ ] Implement relocation application, section layout, alignment, address-space limits and long-branch veneers.
- [ ] Link B++ startup/runtime code and the minimum platform runtime components required by each target.
- [ ] Emit macOS Mach-O, Linux ELF x86-64/ARM64 and Windows PE/COFF executables.
- [ ] Implement entry point, stack/process initialization, imports, relocations, unwind/debug sections and platform metadata.
- [ ] Add object/link map output and deterministic builds where possible.
- [ ] Test linking multiple B++ objects and static B++ libraries, platform runtime objects, missing symbols and incompatible ABI versions. General C interoperability is outside the ABI scope.
- [ ] Remove external host linker dependency after native executable conformance passes on all four platforms.
5. buildFile
Dependency: target/ABI/library metadata and linker command model.
- [ ] Define a readable make/makefile-like B++ project file syntax and version.
- [ ] Define sources, include paths, imports, modules, generated files, libraries, target, build mode and output.
- [ ] Define dependency discovery, timestamps/content hashes, incremental rebuild and parallel scheduling semantics.
- [ ] Define debug/release/sanitizer profiles and target-specific settings.
- [ ] Define environment variables, toolchain discovery, overrides and reproducible-build controls.
- [ ] Define clean, check/test, run, package and install actions.
- [ ] Diagnose cycles, missing inputs, duplicate outputs, unsupported target options and stale ABI artifacts.
- [ ] Add example projects and migrate this compiler's current Makefile workflow.
- [ ] Test paths with spaces/unicode and deterministic results on macOS, Linux and Windows.
6. XF static library for ByteLang
Dependency: ABI, assembler, linker and buildFile library metadata. XF’s public archive interface should be B++-to-B++; internal C components may remain behind the private runtime boundary.
- [ ] Inventory XF public procedures, data, runtime entry points and current C archive dependencies.
- [ ] Resolve existing unresolved ByteLang/LambdaScript/interpreter/SIMD symbols before packaging.
- [ ] Decide which XF parts become B++ code and which C pieces remain private runtime/build dependencies; do not expand the public ABI into general C interoperability to accommodate internals.
- [ ] Build xf as a target-specific static library with ABI/version/dependency metadata.
- [ ] Link it from ByteLang programs using the B++ linker and buildFile.
- [ ] Preserve bindata/reverse-column-index storage behavior and document ownership/lifetime.
- [ ] Test ingestion, transformations, table returns, errors, internal runtime hooks, large input and concurrent operations if supported.
- [ ] Build/test the library on macOS ARM64, Linux x86-64/ARM64 and Windows x86-64.
7. Debugger
Dependency: stable source maps, ABI frame rules, unwind records and variable locations.
- [ ] Define debugger protocol/CLI and target connection model.
- [ ] Read line tables, import maps, procedure metadata and variable/register location ranges.
- [ ] Implement launch/attach, breakpoints, conditional breakpoints, step-in/over/out, continue and terminate.
- [ ] Inspect call stack, arguments, locals, globals, registers, spill cells, tagged values, arrays, strings and buffers.
- [ ] Show source-level runtime errors and distinguish compiler/runtime frames.
- [ ] Handle optimized-out variables and remapped virtual registers accurately.
- [ ] Integrate target-native process control and symbol readers for macOS, Linux and Windows.
- [ ] Test recursion, optimized code, B++ static-library imports, runtime errors and unwinding on every target.
8. Sanitizers
Dependency: public ABI/runtime allocation hooks, assembler/linker metadata, debugger source maps.
- [ ] Specify B++ sanitizer modes and diagnostics independently of compiler-brand flag names.
- [ ] Implement memory bounds/use-after-free checks for heap, arrays, buffers, globals and stack frames.
- [ ] Implement leak tracking and define object-cycle treatment.
- [ ] Implement undefined-behavior checks for arithmetic, casts, alignment, shifts, tags and invalid control flow.
- [ ] Implement thread race detection only after thread/synchronization semantics are defined; instrument atomics and runtime locks correctly.
- [ ] Add stack redzones, frame lifetime poisoning, use-after-return checks and native access instrumentation.
- [ ] Connect generated native instructions and B++ runtime calls to sanitizer reporting with source locations.
- [ ] Test intentional faults per category and ensure one fault produces one actionable report.
- [ ] Validate availability and semantics on macOS ARM64, Linux x86-64/ARM64 and Windows x86-64.
- [ ] Document unsupported sanitizer/target combinations and keep release behavior free of debug-only overhead.