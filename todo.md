# B++ compiler roadmap and target checklist

**Checklist revision:** 1.6  
**Status snapshot:** 2026-10-05  
**Codegen target set:** macOS ARM64, Linux x86-64, Linux ARM64, Windows x86-64.

This file tracks work by roadmap stage and native platform. “Implemented” means
code exists. “Cross-assembled” means an object was produced for a foreign target.
“Native verified” means the executable was built and run on that target OS and
architecture. Those are different gates. Do not mark an OS verified because its
assembly assembled on another host or because a calling-convention bridge passed.

## Platform/version matrix

| Platform profile | Target triple | Codegen implementation | Cross-assembly | Native release execution | Native debug/sanitizers |
|---|---|---|---|---|---|
| macOS ARM64 v1 | `macos-arm64` / Mach-O | Implemented | Passed | **Passed on Apple Silicon** | Pending for this snapshot |
| Linux x86-64 v1 | `linux-x86_64` / ELF | Implemented | Passed | **Passed on Linux x86-64** | ASAN/UBSAN and TSAN passed; LSAN scan unavailable in this environment |
| Linux ARM64 v1 | `linux-arm64` / ELF | Implemented | Passed | Pending native Linux ARM64 | Pending native Linux ARM64 |
| Windows x86-64 v1 | `windows-x86_64` / COFF/PE | Implemented | Passed | Pending native Windows x86-64 | Pending toolchain/runtime check |

**macOS evidence:** Jay ran `make BUILD_MODE=release test check-differential check-targets check-stress` on Apple Silicon. The run passed unit tests, 48 C cases, 48 native cases, 64 seeded comparisons in optimized and baseline modes, cross-assembly of all four targets in both modes, and a 5,000-update large-procedure run.

**Linux x86-64 evidence:** 48 C cases and 48 native cases passed in release, ASAN/UBSAN, and TSAN builds. A 64-seed differential run, 5,000-update stress case, 4096-virtual-value allocator test, mapped-cell cleanup test, and poisoned-global ASAN hook test passed. LSAN's exit-time scan could not run in this environment. Windows `ms_abi` bridge: 48 Windows-convention cases executed on Linux; this is not a Windows OS run.

**Feature-contract update:** the capability query reports all four registered targets and an explicit lowering for every revision-1 IR opcode. Linux x86-64 release and ASAN/UBSAN suites pass at 51 cases; numeric boundary, conversion, and out-of-range literal coverage is included. The 64-seed optimized/baseline differential suite passes. Assembly-comment output assembled with the host assembler for Linux x86-64. Cross-target assembly could not be rerun in this environment because Clang is unavailable.

**2026-10-04 dirty-state flow update:** the Linux x86-64 release, ASAN/UBSAN (LSan exit scan disabled), and TSAN builds each pass 53 C-reference and 53 native cases. The poisoned-global ASAN hook passes. The optimized/baseline differential suite passes 64 seeds, the 5,000-update stress test passes, and the Windows `ms_abi` bridge passes 53 cases on Linux. Cross-target assembly could not be rerun in this sandbox because `clang` is unavailable. The new allocator regression verifies both same-owner preservation across a diamond join and canonical writeback when one predecessor crosses a helper.

**2026-10-05 MSAN update:** `BUILD_MODE=msan` is a Linux/Clang-only, PIE build mode with an intentional uninitialized-stack-load self-check. With Clang 18/compiler-rt on Linux x86-64 it passes 53 C-reference and 53 native cases, 64 optimized/baseline differential seeds, and the 5,000-update stress suite. MSAN verifies compiler and C runtime code; hand-emitted assembly does not yet maintain MSAN shadow state. The Linux x86-64 CI job retains a separate transcript; a completed CI run is still required for the hosted gate.

**2026-10-05 scalable-liveness update:** allocator planning has a checked 512 MiB dense-workspace ceiling before allocations begin. Arithmetic for register/word/block/instruction tables is checked; an over-limit or failed request retains a backend diagnostic containing the affected procedure and allocator reason. Virtual-register liveness now uses a predecessor worklist, and `--emit-map` reports worklist pops/updates plus allocator CPU time. Canonical-cell reuse uses ordered active/free heaps rather than scanning every prior cell. Unit coverage includes a deliberately rejected 5,000,000-vreg request and a 4,096-block dependency chain that converges in fewer than three worklist visits per block. On Linux x86-64 this revision passed the release unit suite, both 53-case C/native suites, 64 seeded differential cases, the 5,000-update stress fixture, and cross-assembly for all four targets.

---

# 1. Codegen (CG)

## 1.1 Shared pipeline and invariants

- [x] Keep the hand-rolled front end and IR as the common input to every backend.
- [x] Make native codegen the default on supported hosts; keep C output as a comparison backend.
- [x] Preserve the `--no-opt` path for differential testing.
- [x] Keep target selection explicit with `--target` and reject execution of a foreign-target executable on the wrong host.
- [x] Keep the current procedure/value layout private and revisioned in `include/codegen_abi.h` (private revision 1).
- [x] Add a documented backend contract for every IR opcode, including value ownership, errors, source positions, and which operations are native or runtime calls (`docs/ir-backend-contract.md`).
- [x] Validate IR before and after each transforming pass; report the pass and function/block in diagnostics.
- [x] Add a verifier for definition/use validity, CFG successors, block terminators, register-map bounds, ownership releases, and call argument counts.
- [x] Add optional assembly comments that map each emitted instruction group to IR opcode, function, block, source span, virtual register, physical register, and spill cell (`--asm-comments`).
- [x] Make backend feature/target capabilities queryable rather than duplicating target conditionals across CLI, emitter, tests, and build rules (`--list-targets`, `--target-info`).
- [x] Establish documented numeric semantics once and keep optimizer, C reference backend, native backends, and tests identical for signed overflow, division, modulo, shifts if added, and conversions (`docs/numeric-semantics.md`).
- [x] Establish a policy for unsupported IR or target features: compile-time diagnostic, explicit helper call, or target-specific lowering. No silent wrong-code fallback (`docs/backend-feature-policy.md`).

## 1.2 Register allocation, spills, and frames

### 1. Broader register allocation

Current implementation: up to eleven ARM64 integer payload candidates (eight callee-saved and three caller-saved), and six x86-64 candidates (four callee-saved and two caller-saved) for both System V and Windows. Caller-saved candidates are used only for short, single-block intervals that do not cross a possible helper/call barrier. Tags and ownership cells remain in memory; fixed-role/scratch registers remain reserved. This is a selective payload cache, not a full machine-register allocator. See [`docs/register-allocation.md`](docs/register-allocation.md).

- [x] Allocate selected payloads to callee-saved ARM64 registers.
- [x] Allocate selected payloads to callee-saved x86-64 registers for both System V and Windows conventions.
- [x] Inventory each target's allocatable integer, floating-point, vector, caller-saved, callee-saved, reserved, frame, and scratch registers; expose inventory through `--target-info`.
- [x] Replace hard-coded pools with target descriptions including register class, width, save cost, call-clobber behavior, and reserved status.
- [x] Add caller-saved allocation when its save/restore cost is lower than spilling; conservatively helper, user, and external call clobbers.
- [x] Decide that tags and ownership stay canonical in memory while payloads may be cached; document writeback, helper visibility, and reload rules for tag/payload coherence.
- [x] Add explicit allocation classes for canonical tagged pairs, integer/null immediates, and same-block integer expressions proven safe to rematerialize. Checked overflow, division by zero, `INT64_MIN / -1`, dynamic inputs, and effectful/helper operations stay unclassified. Proven values are metadata used by native payload lowering: each native use recreates its immediate instead of consuming a payload-cache register. Dead pure values may be omitted under section 1.3, while observable values retain canonical cells.
- [x] Generate a per-function save set from actual allocations instead of always saving the current fixed register set.
- [ ] Verify stack alignment, callee-saved restoration, scratch-register ownership, and call-clobber behavior in nested and recursive calls on every native target. This revision passes Linux x86-64 release, ASAN/UBSAN debug (LSan scanning disabled in this sandbox), and TSAN. The Windows `ms_abi` bridge passes 53 cases here. Native macOS ARM64, Linux ARM64, and Windows x86-64 execution still need matching CI runner results. CI rejects runners with the wrong architecture and uploads a per-run transcript containing the commit SHA, runner identity, toolchain versions, and every test command; this item stays open until those native jobs pass.
- [x] Test zero/one capacity, exact callee-save capacity, pool-plus-one spills, and high-pressure live values across runtime/user calls, joins, loop backedges, returns, and checked-operation error/helper paths for all four target descriptions. Runtime recursion and cleanup-error cases run through the host test suite and Windows MS ABI suite when available.

### 2. Actual spill management

Current implementation: intervals compete for a cache register; evicted intervals use mapped stack cells. The plan records exact instruction live-after sets, helper/call/error/cleanup/sanitizer boundary kinds, and successor-specific live-in sets. Emitters write dirty payloads selectively before helpers and through edge-specific branch stubs; block-entry payload reloads are lazy at their first native payload use. Every virtual IR register still has an ownership cell.

- [x] Define per-position allocator locations explicitly: physical register, canonical spill cell, integer/null constant, rematerializable integer expression, and dead/unallocated. `bpp_reg_location_at` is the authoritative query; `physical[]` remains only a legacy summary when every segment uses the same register. `--emit-map` exports allocation segments, instruction positions, and optimized-out instructions. Observable values retain canonical cells; dead pure values can omit them.
- [x] Split live ranges at unconditional helper/call boundaries and allocate each resulting segment independently. A value may stay cached in a high-use segment and spill only in another segment; emitters flush before the helper and reload into the next segment's register when needed. This does not yet split at arbitrary hot/cold CFG regions or place edge-specific spills.
- [x] Place spill stores only where a value is live across a clobber or must be visible to a helper/cleanup path. Exact instruction live-after and operand/destination sets drive helper writeback. Potential object values remain visible for runtime cleanup even after their final language use.
- [x] Place reloads immediately before first use, avoid duplicate reloads, and hoist only when correctness and profiling justify it. Block-entry preloads are removed. Allocation-segment transitions invalidate the loaded-register map and reload at first payload use; duplicate reloads are suppressed per value and physical register. Canonical homes are synchronized before fast/helper splits. Helper-defined destinations now also use lazy reloads: fast-path results write through to the canonical home before the shared continuation, slow-path results already have a canonical home, and the destination cache is invalidated at the join. Regression coverage checks helper-result execution and verifies ARM64 emits the reload at first use. There is no reload-hoisting optimization today, so reloads are not moved earlier without profile evidence; any future hoisting must preserve helper/error-edge validity.
- [x] Track dirty state by allocation location and CFG edge; remove conservative flushes when the value is dead or the edge proves it unnecessary. The allocator computes must-cache input/output owners across reachable CFG edges. A dirty payload crosses a join only when every reachable predecessor has the same virtual value in the same physical register and the successor entry allocation agrees; otherwise only the necessary predecessor edges write canonical homes. Helper boundaries, cleanup roots, dead values, instrumentation callbacks, and register transitions retain their existing visibility rules. Tests cover matching and conflicting incoming states.
- [x] Model helper calls, user calls, builtins, runtime errors, return cleanup, and sanitizer callbacks as explicit clobber/visibility points. `BPPRegPlan` exposes per-instruction flags, and the emitter uses those flags to choose which values must be canonical before a helper. Sanitizer access hooks are marked at instrumentable global accesses; function tracing is a prologue/epilogue boundary.
- [x] Handle critical edges and split blocks when a spill/reload belongs on only one successor path. Native branch edges now have dedicated assembly stubs that write back only that successor's live values and any possible object cleanup roots. Reload is deferred to the successor's first payload use.
- [x] Verify that every error edge observes a valid tag, payload, and ownership state before cleanup. Plan validation checks that every live-after value, helper operand/destination, and potentially owned object at runtime-error/return-cleanup boundaries has a canonical value-cell home. Tags and ownership stay memory-resident; only integer payloads can be marked dirty. Native `cleanup_error`, recursive cleanup, and sanitizer suites exercise the emitted failure path.
- [x] Add allocator diagnostics that identify why each spill/reload was inserted. `--emit-map` reports spill segment causes (no native use, no candidate register, call clobber, or pressure), per-instruction clobber flags, and spill candidates with vreg, physical register, cell, and reason. `--asm-comments` reports the actual stores and reloads with source instruction, register, cell, and reason (live-across, helper operand/destination, cleanup root, successor edge, first use, or segment transition).
- [x] Expand split-allocation tests to cover values selectively live on one branch, reused after a loop, and replaced by an owned object. Unit tests assert successor-specific spill reasons and physical-register reuse across a loop before an owned string; `edge_loop_owned` exercises the combined path on C and native execution. Existing suites retain call pressure, recursion, joins/backedges, and cleanup-error coverage.

### 3. Smaller stack frames

Current implementation: non-overlapping virtual-value intervals reuse physical cells; possible object owners occupy a root prefix and primitive-only cells follow it. CFG-wide local-slot reuse uses definite-initialization and conservative live spans across reachable blocks. Outgoing argument cells are allocated once per function at the maximum call arity.

- [x] Reuse local slots when declaration lifetimes cannot overlap and shadowing/initialization semantics permit it. Reachable-CFG definite-initialization uses predecessor intersection (parameters start initialized; `INIT_LOCAL` establishes initialization), and local liveness extends conservative linear spans over CFG live-in/live-out sets. Locals accessed on a path before a guaranteed initialization remain unique. Multi-block straight-line and diamond-join tests cover safe reuse and conditional initialization; shared-slot `INIT_LOCAL` uses the runtime store path to release the previous owner. The interval projection is conservative and may miss mutually exclusive reuse opportunities.
- [x] Reuse outgoing argument cells between calls and size outgoing areas from the maximum simultaneously needed arguments. Both native layouts compute the function's maximum IR call arity once and reuse that area; Windows x64's 32-byte ABI shadow area remains a separate call-frame reserve.
- [x] Remove homes and native definitions for unused pure, infallible values. This includes integer/null literals, `NOT`, equality/inequality, and checked integer operations already proven safe for rematerialization; dead pure chains are pruned transitively. Effectful, helper-visible, cleanup-visible, and potentially failing results keep homes. Debug locations are omitted for eliminated instructions; variable-location tracking is not implemented yet.
- [x] Separate GC/ownership roots from ordinary spills so only values requiring runtime visibility need root cells. The native frame places cells that can contain owned/dynamic values first; cleanup scans that root prefix only. Proven integer/null cells remain canonical tagged spill cells outside the root prefix. There is no tracing GC yet.
- [x] Allocate frame regions by alignment/lifetime: header, roots, spills, locals, flags, outgoing arguments, and target-required shadow space. Private ABI revision 2 describes a 128-byte header, root-prefix and ordinary value-cell regions, reusable physical local cells and flags, then a 16-byte-aligned outgoing argument region. Windows shadow space stays in the ABI's caller reserve rather than the B++ argument-value area. x86-64 frame padding and ARM64 save/frame areas preserve call alignment.
- [x] Remove unused layout-map entries or encode identity mappings compactly when that is cheaper. Native metadata omits register/local arrays for identity maps; `--emit-map` reports identity versus explicit maps and physical local offsets.
- [x] Preserve required stack alignment for every call and each target's ABI. x86-64 keeps 16-byte call alignment and Windows shadow space; ARM64 rounds save and local areas to 16 bytes. Linux x86-64 execution and the Windows `ms_abi` bridge pass; ARM64 assembly emission is implemented, but this environment lacks `clang`/`llvm-mc` to assemble it.
- [x] Check frame-size arithmetic for overflow and produce a source-located diagnostic before emission. Shared frame arithmetic rejects overflow; both native emitters now report the module path, source line/column, backend, and function during frame preflight. The diagnostic points at the largest-arity call when present, otherwise the first IR instruction in the function.
- [x] Centralize frame-region arithmetic and assert exact byte counts, overflow rejection, outgoing argument sizing, x86-64 save/padding totals, and ARM64 save-area totals. The map reports frame-data bytes, saved-register bytes, stack allocation, and total procedure stack consumption. A deterministic emitted-map test checks the Linux x86-64 144-byte data frame / 232-byte total stack case; broader exact emitted-map snapshots for recursive and large source fixtures remain useful follow-up coverage.
- [x] Report frame regions, outgoing-call cell count, ABI shadow bytes, root/spill counts, and compact/explicit map forms in `--emit-map`.

### 4. Better allocation decisions

Current implementation: deterministic weighted interval allocation computes dominance-based natural loops and loop depth. A reachable non-loop block has weight 8; each natural-loop nesting level multiplies it by five. Native operand uses have fixed static operation costs (division/modulo 18, multiply 12, add/subtract 10, comparison 8, unary 6, branch 5, store 4), plus tag-check and spill/reload costs. Helper visibility and safe-rematerialization costs are tracked separately. Candidate selection applies the target register’s documented callee-save cost, while caller-saved candidates remain ineligible across a possible helper/call clobber or CFG boundary. Optional `--alloc-profile FILE` multiplies named block weights but is never required; the default remains source/target-deterministic. `--emit-map` exports block weights, branch estimates, profile use, allocator CPU time, and the complete allocation-cost accounting.

- [x] Count uses by operation cost, including native fast-path benefit, helper-call cost, tag checks, spill traffic, and rematerialization cost. The deterministic planner charges fixed native fast-path/tag avoidance and payload traffic, classifies weighted helper paths (user call, builtin, generic helper, runtime-error, cleanup, sanitizer), and charges every native use of a proven-safe rematerializable value. Rematerializable values no longer occupy payload-cache registers: the emitter recreates their immediate at each use. `--emit-asm --asm-comments` reports exact static emitted payload spill-store, reload, and rematerialization counts per procedure and target; these are generated-code totals, not sampled runtime-path frequencies.
- [x] Compute natural loops/backedges and loop depth rather than treating every backward edge equally. Reachable-CFG dominators identify a backedge only when the target dominates its source. Multiple latches share one natural-loop set per header; nested membership determines depth.
- [x] Weight hot loops, call-heavy paths, error paths, and cold blocks separately; establish how weights are selected without runtime profiling. The static policy is documented: base 8 outside loops, `8 × (1 + 4 × depth)` inside loops, fixed operation costs, equal non-loop branch likelihood, and canonical handling for helper/error paths. Helper/call boundaries are reported in the allocation metrics. It is intentionally deterministic and profile-free.
- [x] Include call-crossing cost and caller/callee-save cost in register selection. Caller-saved registers are prohibited for spans that can cross helper/call/CFG boundaries; candidate utility subtracts eight times the target inventory’s save cost. The current ABI gives caller-saved candidates zero prologue save cost.
- [x] Prefer values with long hot live ranges while avoiding cache occupancy for cold/dead spans. A segment with no native payload use remains spilled; repeated high-cost uses in nested loops accumulate priority, while dead values are eliminated before allocation.
- [x] Reconcile allocation weights with block layout and branch probability estimates. Two-way branches use a documented static model: dominance backedges are 88% hot, successors entering deeper loop nesting are 75% hot, and equal-depth branches are 50/50. One bounded propagation pass contributes one quarter of a block's weight to its preferred successor, avoiding an unbounded cyclic-frequency solve. The map records the preferred successor and probability for inspection.
- [x] Add optional profile data without making profile data required for deterministic builds. `--alloc-profile FILE` accepts documented plaintext `function-name block-index positive-multiplier` entries; `*` matches all functions. No profile means exactly the static deterministic plan. Invalid profiles fail before compilation, and the map reports how many blocks were profiled.
- [x] Compare allocation choices against a simple baseline and record spills, reloads, cache hits, flush bytes, and compile time. `--emit-map` contains a dry-run former-heuristic baseline under the same target eligibility rules, selected register/spill segments, estimated cache hits/reload sites/payload writebacks/writeback bytes, and allocator CPU nanoseconds. These are allocation-plan measurements; `--emit-asm --asm-comments` additionally reports exact static emitted payload spill-store/reload/rematerialization counts for each procedure and target.
- [x] Add tests proving heuristic changes preserve output and do not increase spills on known pressure fixtures. The allocator’s zero/one/exact/pool-plus-one and high-pressure call fixtures for every target assert that the selected plan does not exceed the legacy baseline spill count. Loop-depth/hot-edge metadata, safe rematerialization, and all-target generated-code traffic summaries are unit-tested; the 54-case C/native suite preserves output. Differential and stress checks remain regression gates.

### 5. Scalable liveness and allocation

Current implementation: the dense quadratic interference graph is gone. Block live sets use bitsets and sparse set-bit iteration; virtual-register liveness uses a predecessor worklist rather than repeated full-CFG scans. Conservative intervals and linear cell reuse remain. A checked 512 MiB ceiling bounds allocator-owned dense workspace before allocation.

- [x] Bound memory for functions with many blocks and many virtual registers; diagnose allocation failure without corrupting the module. The planner estimates all current dense allocator tables and rejects requests exceeding 512 MiB before allocating them. Failure preserves a reason on the plan, which every native emitter and location-map export reports with the procedure name.
- [x] Measure liveness convergence on a generated long dependency chain. `--emit-map` records worklist pops and changed block entries; the 4,096-block regression requires fewer than three visits per block. Loop/backedge and irreducible-CFG scale fixtures remain open.
- [x] Avoid rescanning every virtual register for each instruction when a sparse use/def representation is cheaper.
- [x] Use worklist dataflow so only predecessors affected by a changed live entry are revisited. The predecessor table is built once per function and the queue suppresses duplicate pending blocks.
- [x] Improve interval construction for values with holes across mutually exclusive branches.
- [x] Avoid quadratic canonical-cell scans. Intervals are ordered by start; a min-heap of active cells releases finished intervals into a min-heap of free cells, selecting the lowest reusable cell in `O(log n)` per interval. The existing 4,096-value reuse regression remains green.
- [x] Make allocator size arithmetic safe for bitset word counts, block/register/instruction products, predecessor tables, segment totals, local liveness tables, and the dense-workspace preflight. Static target metadata remains validated by the target registry.
- [x] Test 4096 values (currently covered) and larger generated functions at multiple block/register scales.
- [x] Record compile time and peak memory for synthetic straight-line, loop-heavy, branch-heavy, and call-heavy programs.
- [x] Set a supported compiler-function size based on evidence; reject pathological input with a clear diagnostic if necessary.

### 6. Static value information

Current implementation: block-local known result kinds and integer slot constants eliminate some tag checks and permit some folding. There is no static type system.

- [x] Define a value-kind lattice for unknown, null, integer, string/buffer, array, and other objects, with merge rules at CFG joins.
- [x] Distinguish “known kind” from “known constant” and from “known truthiness.”
- [x] Track kinds through every IR opcode, builtin, user call, assignment, release, delete, free, and runtime fallback.
- [x] Invalidate global facts across calls; preserve local facts only where aliasing and language semantics prove safety.
- [x] Add interprocedural summaries for known argument/result kinds only if recursion and dynamic returns can be modeled soundly.
- [x] Use known kinds to remove tag tests only when all incoming control-flow paths prove the same kind.
- [x] Simplify truthiness tests for known null/integer/object values without changing empty-object semantics.
- [x] Keep error behavior identical for dynamic type errors and uninitialized loads.
- [x] Test loops, joins, recursion, object-to-integer reassignment, self-assignment, deletion, free aliases, and calls that mutate globals.

### 7. Redundant-access elimination

Current implementation: cached integer payloads defer stores; some known integer tag checks and repeated initialization-flag writes are omitted. Slot accesses and many synchronization flushes remain.

- [x] Add local value numbering or a similar pass for safe repeated pure loads/expressions.
- [x] Eliminate repeated local/global slot loads only when calls, stores, aliasing, volatile rules, and error behavior cannot change the result.
- [x] Remove redundant stores when the value is unchanged and the store has no required initialization/error effect.
- [x] Remove duplicate tag loads and initialization checks after preserving the first check's diagnostic path.
- [x] Track memory effects by operation: globals, locals, objects, buffer/array contents, allocation, output, and possible error.
- [x] Reduce dirty-payload flushes using liveness at individual edges and helpers, not only block-level boundaries.
- [x] Keep sanitizer hooks, debug visibility, and runtime ownership synchronization as observable effects where required.
- [x] Count emitted loads/stores/branches/helper calls per target and compare before/after on fixed fixtures.
- [x] Add tests with calls between loads, side-effecting builtins, aliasing arrays/buffers, and failures on the second access.

### 8. Constant folding and propagation

Current implementation: overflow-safe integer folding, safe division/modulo handling, constant-branch simplification, and integer slot propagation within a block. Operations that must report overflow or division-by-zero remain executable.

- [x] Specify folding rules per opcode and value kind, including signed boundaries, equality, truthiness, strings, and conversions.
- [x] Implement a constant lattice and merge facts across predecessor edges.
- [x] Propagate constants through loop preheaders and loop-invariant expressions without assuming a loop executes.
- [x] Keep potentially failing constant operations at runtime when folding would suppress or move a language-visible error.
- [x] Preserve source location for folded operations so a diagnostic still points to the correct expression.
- [x] Fold safe unary/binary operations and comparisons for every target consistently.
- [x] Add branch and switch folding once those IR forms exist.
- [x] Test INT64_MIN/MAX, multiply overflow, zero division/modulo, minimum/-1, nested branches, loops, and calls that mutate globals.
- [x] Run every differential fixture with optimization enabled and disabled.

### 9. Dead-code elimination

Current implementation: constant unreachable branches/blocks and unreferenced private procedures are removed; some unused primitive literal writes/releases are removed. There is no general effect-aware DCE.

- [ ] Define an effect table for all IR operations: output, allocation, retain/release, mutation, global access, potential runtime error, and control flow.
- [ ] Remove dead pure computations only when dropping them cannot suppress overflow, division, bounds, uninitialized-variable, or type errors.
- [ ] Remove dead stores only when initialization semantics and error behavior remain intact.
- [ ] Remove unused cleanup/release operations only with ownership proof; do not leak objects or release aliases early.
- [ ] Remove dead blocks and repair branch targets, source maps, and function IDs safely.
- [ ] Keep exported/address-taken procedures once a public ABI or library model introduces them.
- [ ] Add tests for effectful dead-looking calls, errors in unused expressions, recursive-but-unreachable procedures, and object cleanup.

### 10. Instruction selection

Current implementation: direct ARM64 and x86-64 integer operations, immediate arithmetic, local/global fast paths, branches, and calls. Dynamic operations use C helpers.

- [ ] Define an instruction-selection table for every IR opcode on every target.
- [ ] Add legality/cost checks for immediate forms, extensions, shifts, multiply-add, compare-branch, address generation, and load/store addressing modes.
- [ ] Select instructions using register constraints and operand locations rather than emitter-local special cases.
- [ ] Add target-specific lowering for useful operations while keeping fallback semantics and errors identical.
- [ ] Model flags/condition-code lifetime so instruction combines do not overwrite live conditions.
- [ ] Model scratch-register demand before choosing a sequence; avoid hidden scratch conflicts.
- [ ] Add assembly golden tests and executable tests for each selected form, including negative/overflow boundaries.
- [ ] Measure code size and cycles on each native CPU before retaining a more complex sequence.

### 11. Control-flow optimization

Current implementation: constant branches simplify and jumps to the next block are omitted. Blocks are otherwise emitted in existing order.

- [ ] Lay out blocks to favor fall-through on likely edges while keeping branch targets and source locations correct.
- [ ] Add branch inversion to remove unconditional jumps after conditional branches.
- [ ] Merge compatible blocks and remove empty jump-only blocks.
- [ ] Simplify chained jumps and redundant branches after CFG mutation.
- [ ] Preserve loop headers, call return sites, unwind ranges, and debug locations during reordering.
- [ ] Separate cold error/cleanup paths where doing so improves hot code without making branches exceed range.
- [ ] Add CFG validation after every block-reordering/merging pass.
- [ ] Test nested loops, diamonds, early returns, error paths, and long cold blocks on every emitter.

### 12. Large-program branch and address handling

Current implementation: ARM64 internal transfers materialize page-relative addresses; x86-64 uses relative branches/calls and RIP-relative data references. Architecture and object-format range limits remain.

- [ ] Enumerate range limits for ARM64 ADRP relocations, literal/data references, x86-64 rel32 branches/calls, and each object format.
- [ ] Add a post-layout range checker that reports the source instruction, target label, and exceeded range.
- [ ] Add ARM64 branch veneers/islands or a documented code model when a target exceeds branch/page range.
- [ ] Add x86-64 branch islands or long-call sequences for text beyond rel32 range.
- [ ] Coordinate symbol, relocation, section layout, and veneer rules with the future assembler/linker.
- [ ] Test at boundaries using generated object layouts; do not require multi-gigabyte CI fixtures.
- [ ] Keep short local conditional branches short and long cross-block transfers range-safe.
- [ ] Test Mach-O, ELF ARM64, ELF x86-64 and COFF relocations independently.

### 13. Formal ABI call lowering — depends on stage 2

Current implementation: private revision-1 procedure frames and per-target host calling conventions. Do not turn this private frame into a public promise before the ABI is designed.

- [ ] Stage 2 defines calling convention, value representation, alignment, ownership, error propagation, symbol names, visibility, versioning, and compatibility rules.
- [ ] Define argument classification for the deliberately small set of B++ types exposed by static libraries; add aggregates only when the language has a concrete need.
- [ ] Define return classification and error/status behavior for supported B++ values; defer multiple returns and complex aggregates unless required by the language.
- [ ] Define stack alignment, stack arguments, callee/caller-saved registers, and target-required shadow/home space for B++ library/runtime calls. Variadic calls are out of scope for the public ABI.
- [ ] Define the B++-to-B++ static-library call convention and B++ symbol/export rules. Keep system entry points in the private runtime boundary.
- [ ] Replace private call lowering only after the ABI spec and conformance tests are accepted.
- [ ] Add separately compiled B++ library producer/consumer fixtures for macOS ARM64, Linux x86-64/ARM64 and Windows x86-64.
- [ ] Add ABI compatibility tests across compiler versions before static library work.

### 14. Further runtime independence

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

### 15. Debug metadata

Current implementation: assembler source/line directives, host unwind data, and `--emit-map` local offsets/IR positions. It does not yet describe optimized variable locations in standard debug records.

- [ ] Preserve original source spans through imports, macros, lowering, folding, DCE and block reordering.
- [ ] Represent source files and include/import stacks instead of mapping all expanded code to one sidecar.
- [ ] Emit procedure names, parameter names/types, lexical scopes, and source-file tables.
- [ ] Emit DWARF location lists/register locations and Windows CodeView/PDB-compatible information as appropriate.
- [ ] Track a variable that moves between register and spill slot over time.
- [ ] Describe optimized-out variables honestly instead of reporting stale frame cells.
- [ ] Keep unwind rules correct through prologue/epilogue, stack probes, veneers, helper calls, and every return/error path.
- [ ] Add metadata inspection tests with `llvm-dwarfdump`, `readelf`, `llvm-readobj`, and platform-native tools.
- [ ] Test source stepping through loops, calls, recursive frames, imports and runtime errors on each supported debugger.

### 16. Native sanitizer instrumentation

Current implementation: host compiler sanitizer modes instrument C components/runtime. Native global fast paths call ASAN/TSAN hooks; TSAN procedure entry/exit hooks expose native frames. Linux/Clang `BUILD_MODE=msan` adds MemorySanitizer plus PIE and an intentional uninitialized-load runtime self-check; the local Linux x86-64 suite passes and the matching CI job is queued as a separate evidence gate. Native stack accesses and hand-emitted assembly are not fully shadow-memory instrumented. UBSAN-relevant integer behavior uses explicit language checks.

- [ ] Define which sanitizer checks are language guarantees versus compiler debugging instrumentation.
- [ ] Specify shadow-memory mapping and runtime hooks independently of Clang/GCC sanitizer ABI where portability requires it.
- [ ] Instrument native loads/stores, pointer arithmetic, array/buffer bounds, integer overflow, invalid tags, and allocation/free transitions.
- [ ] Add stack redzones, frame poisoning/unpoisoning, return poisoning and use-after-return detection.
- [ ] Avoid duplicate or missing reports when native access transitions through C helpers.
- [ ] Define TSAN memory events, atomic semantics, synchronization operations, and limitations until B++ threads exist.
- [ ] Define leak tracking across native and runtime allocations, including cycles and global roots.
- [ ] Verify ASAN, LSAN, UBSAN and TSAN runtime availability for each compiler/OS/architecture combination.
- [x] Add a Linux/Clang MSAN mode with PIE, an intentional uninitialized-read self-check, and a dedicated Linux x86-64 CI transcript. MSAN is rejected on macOS, Windows, and non-Clang toolchains rather than silently degrading.
- [ ] Verify the Linux/Clang MSAN runtime on the native CI runner and document it separately from ASAN/LSAN/UBSAN/TSAN. Local Clang 18/compiler-rt execution passes; MSAN currently covers the C compiler/runtime path, not full hand-emitted assembly shadow state.
- [ ] Add intentional-fault tests for each check and assert source location, category and exit behavior.
- [ ] Keep production/release code free of sanitizer hooks unless explicitly enabled.

### 17. Broader verification and release gates

Current implementation: unit/runtime tests, 53-case C/native suites, 64-seed optimized/baseline differential tests, register pressure, mapped-cell ownership, 4096-value reuse, large frames, 5000-update large procedure, ASAN and MSAN self-checks, cross-assembly where a Clang assembler is available, and native CI jobs that retain per-run verification transcripts. Linux/Clang MSAN passes locally and has a separate CI job; matching-host ARM64/Windows results remain pending.

- [ ] Run all tests on every supported native OS/architecture; cross-assembly is a separate gate.
- [ ] Keep the C backend as a temporary semantic oracle and compare exit status, stdout, stderr category, and returned value.
- [ ] Extend deterministic randomized programs across arithmetic, branch, loop, call, recursion, string, buffer, array and error behavior.
- [ ] Add seed reporting and save the smallest reproducer for every generated failure.
- [ ] Add deep recursion, stack exhaustion, many parameters, high live-value pressure, many locals, huge block count, large text section, and frame-probe boundary tests.
- [ ] Test both optimized and `--no-opt` native modes for every fixture.
- [ ] Test debug metadata builds and sanitizer builds separately from release.
- [ ] Run the Linux/Clang MSAN job and retain its transcript; do not report MSAN as a macOS/Windows sanitizer mode or as full hand-emitted-assembly instrumentation.
- [ ] Run native CI on macOS ARM64, Linux x86-64, Linux ARM64 and Windows x86-64 and retain logs/artifacts.
- [ ] Add compiler warning-as-error builds on Clang and GCC-compatible toolchains where supported.
- [ ] Fuzz lexer/parser/IR validator/optimizer/emitter with resource limits and sanitizer builds.
- [ ] Test reproducible assembly/object output or document nondeterministic fields.
- [ ] Gate a target release on its native runtime suite, ABI conformance, unwind, debug, and supported sanitizer checks.

### 18. Performance and resource benchmarks

Current implementation: a CSV harness compares C, native optimized and native `--no-opt` execution time plus executable size for loop/call/object workloads. Existing measurements are a smoke check, not an isolated baseline.

- [ ] Separate compile time, assembly time, link time, startup time and steady-state execution time.
- [ ] Add warmup, repeated samples, median/dispersion, environment metadata and stable machine identifiers.
- [ ] Measure code size, read-only data, frame size, maximum stack depth, spill/reload count, helper-call count, and compilation peak memory.
- [ ] Include integer-heavy, branch-heavy, recursive, call-heavy, object-heavy, string/buffer/array, and large-source programs.
- [ ] Compare C reference, optimized native, unoptimized native, and later assembler/linker builds.
- [ ] Run on macOS ARM64, Linux x86-64, Linux ARM64 and Windows x86-64 native hosts.
- [ ] Keep results in versioned CSV/JSON with compiler revision, flags, target triple, CPU/OS and sanitizer mode.
- [ ] Set performance expectations only after repeatable runs; avoid pass/fail thresholds sensitive to shared CI noise.

### 19. Target support and platform-specific integration

The four emitters exist; only macOS ARM64 and Linux x86-64 have native release runs recorded. Keep each target's object, runtime, linker, debugger, and sanitizer checks separate.

- [ ] Keep target IDs, triples, object formats, symbol prefixes, sections, relocations, stack rules and helper conventions in one target descriptor.
- [ ] Validate host-target detection on Apple Silicon, Linux x86-64/ARM64, and Windows x86-64; reject unsupported hosts clearly.
- [ ] Test spaces, Unicode, drive letters, case-insensitive paths, temp files, sidecar paths and source-overwrite protection on Windows.
- [ ] Validate Mach-O relocations/unwind/debug info on macOS tooling.
- [ ] Validate ELF relocations, GNU-stack markings, CFI, stack probes and debug info on Linux x86-64/ARM64.
- [ ] Validate PE/COFF sections, SEH unwind data, chkstk, startup, imports, CRT and debug information on Windows x86-64.
- [ ] Decide minimum OS/SDK/toolchain versions and document supported compiler drivers.
- [ ] Keep macOS Intel, Windows ARM64 and other architectures explicitly unsupported until separately designed and tested.

## 1.20 Platform-specific CG checklist

### macOS ARM64 — target profile v1

- [x] Emit ARM64 Mach-O assembly.
- [x] Assemble and link/run the release compiler and generated program on Apple Silicon.
- [x] Pass unit, C/native execution, differential, cross-target assembly and large-procedure checks in the user-reported release run.
- [ ] Run `make BUILD_MODE=debug test check-asan-hook` on the Mac and record the exact sanitizer runtimes available.
- [ ] Run `make BUILD_MODE=tsan test` on the Mac.
- [ ] Run and archive repeated native benchmarks and frame/code-size results.
- [ ] Inspect Mach-O unwind and debug output with Apple/LLVM tools; test backtraces through B++ calls/errors.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.

### Linux x86-64 — target profile v1

- [x] Emit System V x86-64 ELF assembly.
- [x] Pass release suite, 64-seed differential suite, large-procedure test and runtime/unit tests.
- [x] Pass ASAN/UBSAN and TSAN test suites on the available Linux x86-64 host.
- [x] Exercise the Windows calling convention with the Linux `ms_abi` bridge; keep it classified as a bridge test only.
- [ ] Run LSAN on a host/container where its exit-time process scan works.
- [ ] Run repeated isolated benchmarks and inspect CFI/source-line/backtrace behavior.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.

### Linux ARM64 — target profile v1

- [x] Emit Linux ARM64 ELF assembly using the shared ARM64 instruction selector.
- [x] Cross-assemble optimized and baseline objects.
- [ ] Run the compiler and generated executables natively on Linux ARM64.
- [ ] Pass unit tests, the C suite, the native suite, optimized/baseline differential suite and 5000-update stress case.
- [ ] Run ASAN/UBSAN/TSAN/LSAN where the actual compiler runtime supports them; record unsupported combinations explicitly.
- [ ] Inspect ELF relocations, CFI, stack probing, source lines and backtraces on ARM64.
- [ ] Run isolated benchmarks and compare with Linux x86-64 and macOS ARM64.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.

### Windows x86-64 — target profile v1

- [x] Emit Windows x64 COFF assembly and SEH prolog/epilog directives.
- [x] Cross-assemble optimized and baseline objects.
- [x] Pass the Linux `ms_abi` calling-convention bridge suite; retain the limitation that this does not test Windows OS behavior.
- [ ] Build and run compiler and generated executables on native Windows x64.
- [ ] Pass unit tests, the C suite, native suite, optimized/baseline differential suite and 5000-update stress case.
- [ ] Verify Windows CRT startup, symbol entry point, shadow space, stack arguments, `___chkstk_ms`, guard pages, PE linking and SEH unwinding.
- [ ] Validate paths containing spaces, mixed separators, drive letters, Unicode, case-only aliases, temp files and source/debug-snapshot overwrite protection.
- [ ] Determine MinGW/UCRT/Clang toolchain support and whether MSVC syntax/tooling is in scope.
- [ ] Run debug/sanitizer tests only for runtimes that exist; document ASAN/LSAN/UBSAN/TSAN gaps without treating unsupported runtimes as passing.
- [ ] Inspect CodeView/PDB or chosen debug format and Windows backtraces.
- [ ] Run isolated benchmarks and record binary/frame sizes.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.

## 1.21 CG exit criteria before moving to ABI

- [ ] Native release test matrix passes on macOS ARM64, Linux x86-64, Linux ARM64 and Windows x86-64.
- [ ] Every target has passed semantic differential tests, large procedure/frame tests, recursion, pressure, error cleanup, target calling-convention tests, unwind tests and debug-line tests.
- [ ] Debug/sanitizer results are recorded per compiler/runtime/OS, including unsupported combinations and LSAN scan limitations.
- [ ] Allocator/spill/frame design is documented and stress results show bounded behavior for project-sized code.
- [ ] Branch/relocation limits and diagnostics are documented for each current object format.
- [ ] Native runtime dependencies and the C components that remain are listed for the ABI/runtime design.
- [ ] Benchmarks are collected for all four host profiles and saved with reproducible environment metadata.
- [ ] The unresolved shared tasks in sections 1.2–1.19 are reviewed explicitly; no “partial” item is silently promoted to complete.
- [ ] Only after these gates are reviewed, begin roadmap stage 2 and freeze the public ABI contract.

---

# 2. ABI

**Scope:** a functional ABI for statically linking B++ libraries into B++ programs. Keep the public boundary small and stable. Broad FFI, general C ABI compatibility, arbitrary callbacks into foreign runtimes, shared libraries and dynamic loading are out of scope. C code may remain inside the compiler/runtime as an implementation detail; that alone does not make C interoperability part of the B++ ABI.

**Dependency:** CG call/frame requirements must be known. Public libraries must not depend on the temporary private revision-1 frame layout.

### ABI goals

- [ ] A library author can publish B++ procedures for use by another B++ program.
- [ ] A separately compiled B++ consumer can resolve symbols, call procedures, exchange supported B++ values, handle errors and link without a dynamic loader.
- [ ] The library/linker can check target, ABI revision, runtime requirement, dependencies, imports and exports before creating an executable.
- [ ] Compiler/runtime internals such as `BLValue`, refcounts, allocator structures and the current private frame remain hidden behind the public boundary.
- [ ] The supported public type set is deliberately small. Unsupported types are diagnosed instead of implicitly exposing a C or compiler-specific layout.

### ABI design and implementation

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

### Explicit ABI non-goals for v1

- [ ] No general-purpose C ABI or automatic C type marshalling.
- [ ] No arbitrary calls from B++ into foreign libraries through source-level FFI.
- [ ] No callbacks from arbitrary foreign code into B++ procedures.
- [ ] No shared-library lifecycle, dynamic loader, plugin ABI or symbol interposition.
- [ ] No public dependency on the current C `BLValue` layout or private runtime frame.
- [ ] Revisit a narrow interop feature only when a concrete system/runtime need cannot be met by B++ libraries and the private platform runtime boundary.

# 3. Assembler

**Dependency:** finalized target instruction forms, relocation model, ABI symbols, unwind and debug metadata.

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

# 4. Linker

**Dependency:** assembler object format, static B++ ABI symbol rules, runtime object format and executable entry points.

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

# 5. buildFile

**Dependency:** target/ABI/library metadata and linker command model.

- [ ] Define a readable make/makefile-like B++ project file syntax and version.
- [ ] Define sources, include paths, imports, modules, generated files, libraries, target, build mode and output.
- [ ] Define dependency discovery, timestamps/content hashes, incremental rebuild and parallel scheduling semantics.
- [ ] Define debug/release/sanitizer profiles and target-specific settings.
- [ ] Define environment variables, toolchain discovery, overrides and reproducible-build controls.
- [ ] Define clean, check/test, run, package and install actions.
- [ ] Diagnose cycles, missing inputs, duplicate outputs, unsupported target options and stale ABI artifacts.
- [ ] Add example projects and migrate this compiler's current Makefile workflow.
- [ ] Test paths with spaces/unicode and deterministic results on macOS, Linux and Windows.

# 6. XF static library for ByteLang

**Dependency:** ABI, assembler, linker and buildFile library metadata. XF’s public archive interface should be B++-to-B++; internal C components may remain behind the private runtime boundary.

- [ ] Inventory XF public procedures, data, runtime entry points and current C archive dependencies.
- [ ] Resolve existing unresolved ByteLang/LambdaScript/interpreter/SIMD symbols before packaging.
- [ ] Decide which XF parts become B++ code and which C pieces remain private runtime/build dependencies; do not expand the public ABI into general C interoperability to accommodate internals.
- [ ] Build `xf` as a target-specific static library with ABI/version/dependency metadata.
- [ ] Link it from ByteLang programs using the B++ linker and buildFile.
- [ ] Preserve bindata/reverse-column-index storage behavior and document ownership/lifetime.
- [ ] Test ingestion, transformations, table returns, errors, internal runtime hooks, large input and concurrent operations if supported.
- [ ] Build/test the library on macOS ARM64, Linux x86-64/ARM64 and Windows x86-64.

# 7. Debugger

**Dependency:** stable source maps, ABI frame rules, unwind records and variable locations.

- [ ] Define debugger protocol/CLI and target connection model.
- [ ] Read line tables, import maps, procedure metadata and variable/register location ranges.
- [ ] Implement launch/attach, breakpoints, conditional breakpoints, step-in/over/out, continue and terminate.
- [ ] Inspect call stack, arguments, locals, globals, registers, spill cells, tagged values, arrays, strings and buffers.
- [ ] Show source-level runtime errors and distinguish compiler/runtime frames.
- [ ] Handle optimized-out variables and remapped virtual registers accurately.
- [ ] Integrate target-native process control and symbol readers for macOS, Linux and Windows.
- [ ] Test recursion, optimized code, B++ static-library imports, runtime errors and unwinding on every target.

# 8. Sanitizers

**Dependency:** public ABI/runtime allocation hooks, assembler/linker metadata, debugger source maps.

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