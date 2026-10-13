# B++ compiler roadmap and target checklist

**Checklist revision:** 1.6  
**Status snapshot:** 2026-10-05  
**Codegen target set:** macOS ARM64, Linux x86-64, Linux ARM64, Windows x86-64.

This file tracks work by roadmap stage and native platform. “Implemented” means
code exists. “Cross-assembled” means an object was produced for a foreign target.
“Native verified” means the executable was built and run on that target OS and
architecture. Those are different gates. Do not mark an OS verified because its
assembly assembled on another host or because a calling-convention bridge passed.

**1.0.15 backend update:** the C output backend, `--emit-c`, and `--backend c` are removed. Native optimized and baseline fixtures use fixed expectations and an independent seeded evaluator. Earlier C/native suite entries below remain historical evidence.

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

**2026-10-05 scalable-liveness completion:** allocator planning has a checked 512 MiB per-function workspace ceiling before allocations begin. Arithmetic for register/word/block/instruction tables is checked; an over-limit request retains a backend diagnostic containing the affected procedure and allocator reason. Per-instruction operands now use one compact sparse use table instead of a dense instruction-by-vreg visibility bitmap. Payload allocation constructs discontiguous active pieces from definitions, sparse uses, and exact live-after sets, preserving holes through mutually exclusive branch arms; canonical ownership-cell reuse stays conservative over the enclosing span. `--emit-map` reports worklist pops/updates, allocator CPU time, deterministic allocator-owned workspace peak, sparse-use entries, and range-piece/hole counts. Unit coverage rejects a 5,000,000-vreg request, covers 4,096 and 16,384 straight-line values, a 4,096-block dependency chain, an explicit branch hole, and 4,096-register loop/branch/call fixtures with recorded CPU/workspace metrics. The supported compiler-function-size contract is the checked 512 MiB preflight, not an unbounded source-size promise.

---

# 1. Codegen (CG)

## 1.1 Shared pipeline and invariants

- [x] Keep the hand-rolled front end and IR as the common input to every backend.
- [x] Use native codegen exclusively on supported hosts; retire C output at the end of 1.0.15.
- [x] Preserve the `--no-opt` path for differential testing.
- [x] Keep target selection explicit with `--target` and reject execution of a foreign-target executable on the wrong host.
- [x] Keep the current procedure/value layout private and revisioned in `include/codegen_abi.h` (private revision 1).
- [x] Add a documented backend contract for every IR opcode, including value ownership, errors, source positions, and which operations are native or runtime calls (`docs/ir-backend-contract.md`).
- [x] Validate IR before and after each transforming pass; report the pass and function/block in diagnostics.
- [x] Add a verifier for definition/use validity, CFG successors, block terminators, register-map bounds, ownership releases, and call argument counts.
- [x] Add optional assembly comments that map each emitted instruction group to IR opcode, function, block, source span, virtual register, physical register, and spill cell (`--asm-comments`).
- [x] Make backend feature/target capabilities queryable rather than duplicating target conditionals across CLI, emitter, tests, and build rules (`--list-targets`, `--target-info`).
- [x] Establish documented numeric semantics once and keep optimizer, runtime helpers, native backends, and tests identical for signed overflow, division, modulo, shifts if added, and conversions (`docs/numeric-semantics.md`).
- [x] Establish a policy for unsupported IR or target features: compile-time diagnostic, explicit helper call, or target-specific lowering. No silent wrong-code fallback (`docs/backend-feature-policy.md`).

## 1.2 Register allocation, spills, and frames

### 1. Broader register allocation

Current implementation: up to eleven ARM64 integer payload candidates (eight callee-saved and three caller-saved), and six x86-64 candidates (four callee-saved and two caller-saved) for both System V and Windows. Caller-saved candidates are used only for short, single-block intervals that do not cross a possible helper/call barrier. Tags and ownership cells remain in memory; fixed-role/scratch registers remain reserved. This is a selective payload cache, not a full machine-register allocator. See [`docs/register-allocation.md`](docs/register-allocation.md).

- [x] Allocate selected payloads to callee-saved ARM64 registers.
- [x] Allocate selected payloads to callee-saved x86-64 registers for both System V and Windows conventions.
- [x] Inventory each target's allocatable integer, floating-point, vector, caller-saved, callee-saved, reserved, frame, and scratch registers; expose inventory through `--target-info`.
- [x] Replace hard-coded pools with target descriptions including register class, width, save cost, call-clobber behavior, and reserved status.
- [x] Add caller-saved allocation when its save/restore cost is lower than spilling; conservatively model helper, user, and external call clobbers.
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
- [x] Add tests proving heuristic changes preserve output and do not increase spills on known pressure fixtures. The allocator’s zero/one/exact/pool-plus-one and high-pressure call fixtures for every target assert that the selected plan does not exceed the legacy baseline spill count. Loop-depth/hot-edge metadata, safe rematerialization, and all-target generated-code traffic summaries are unit-tested; the 64-case C/native suite preserves output. Differential and stress checks remain regression gates.

### 5. Scalable liveness and allocation

Current implementation: the dense quadratic interference graph is gone. Block live sets use bitsets and sparse set-bit iteration; per-instruction uses use compact operand lists; virtual-register liveness uses a predecessor worklist rather than repeated full-CFG scans. Active range pieces preserve holes through mutually exclusive branch arms for payload allocation, while canonical cells use conservative enclosing spans. A checked 512 MiB ceiling bounds allocator-owned workspace before allocation.

- [x] Bound memory for functions with many blocks and many virtual registers; diagnose allocation failure without corrupting the module. The planner estimates all current dense allocator tables and rejects requests exceeding 512 MiB before allocating them. Failure preserves a reason on the plan, which every native emitter and location-map export reports with the procedure name.
- [x] Measure liveness convergence on a generated long dependency chain. `--emit-map` records worklist pops and changed block entries; the 4,096-block regression requires fewer than three visits per block. Loop/backedge and irreducible-CFG scale fixtures remain open.
- [x] Avoid rescanning every virtual register for each instruction when a sparse use/def representation is cheaper. The allocator records a compact offset/operand table for every instruction and updates block use/def, reverse liveness, needed-value propagation, spill visibility, and benefit accounting directly from those operands. The previous dense instruction-by-vreg visibility bitmap is gone; only exact instruction live-after bitsets remain where edge/error correctness needs them.
- [x] Use worklist dataflow so only predecessors affected by a changed live entry are revisited. The predecessor table is built once per function and the queue suppresses duplicate pending blocks.
- [x] Improve interval construction for values with holes across mutually exclusive branches. Definitions, sparse operands, and exact live-after sets form ordered active pieces. A hole creates separate allocation segments that can reuse a payload register; canonical ownership-cell reuse remains deliberately conservative over the enclosing value span. The branch-hole unit regression verifies the inactive arm has no register segment.
- [x] Avoid quadratic canonical-cell scans. Intervals are ordered by start; a min-heap of active cells releases finished intervals into a min-heap of free cells, selecting the lowest reusable cell in `O(log n)` per interval. The existing 4,096-value reuse regression remains green.
- [x] Make allocator size arithmetic safe for bitset word counts, block/register/instruction products, predecessor tables, segment totals, local liveness tables, and the dense-workspace preflight. Static target metadata remains validated by the target registry.
- [x] Test 4096 values (currently covered) and larger generated functions at multiple block/register scales. The allocator unit suite runs 4,096 and 16,384 straight-line virtual values, a 4,096-block dependency chain, and 4,096-register loop, branch, and call-heavy functions; every plan is verified.
- [x] Record compile time and peak memory for synthetic straight-line, loop-heavy, branch-heavy, and call-heavy programs. The scale fixtures print deterministic allocator planning CPU nanoseconds and the allocator-owned preflight workspace peak; `--emit-map` exports the same per-function fields. These are not host process-RSS measurements.
- [x] Set a supported compiler-function size based on evidence; reject pathological input with a clear diagnostic if necessary. The supported contract is a deterministic allocator preflight of at most 512 MiB per function. Evidence covers the scale fixtures above; a 5,000,000-vreg function is rejected before allocation with `register allocator workspace exceeds 512 MiB limit`, and emitters report the function name with that reason.

### 6. Static value information

Current implementation: a flow-sensitive, optimization-only value-fact pass runs before each native emitter. There is no static type system and unknown facts always retain the runtime path.

- [x] Define a value-kind lattice for unknown, null, integer, string, buffer, array, and other objects, with merge rules at CFG joins. Equal kinds survive a join; differing object subkinds merge to object; all other disagreements become unknown.
- [x] Distinguish known kind, known integer constant, and known truthiness. Null is false; known zero/nonzero integers are false/true; every object kind is true; facts otherwise stay independent and unknown.
- [x] Track facts through every IR opcode, builtin, user call, assignment, release, delete, free, and runtime fallback. Operations that can only continue with an integer result establish integer kind even when their dynamic operands require a helper/error check.
- [x] Invalidate global facts across user calls; preserve local kind/truth facts only where mutation cannot alter them. Builtins keep only documented result facts; buffer/array mutation and free preserve the object's kind, never contents or ownership detail.
- [x] Apply the sound interprocedural policy for recursive and dynamic returns: this revision derives no user-procedure result/argument summary. Every direct user call produces unknown and clears global facts, which is deterministic and sound for recursion, dynamic returns, and global mutation. A future summary must prove a call graph/SCC and effects before changing this rule.
- [x] Use known kinds to remove native integer tag tests only when the flow fact proves integer on every incoming path. Fact storage is block-entry dataflow, not the former block-local emitter guess.
- [x] Simplify native truthiness tests for proven null/known integers/object values without changing empty-object semantics. All objects remain true; assembly comments mark `B++ truthiness-elided` when this fires.
- [x] Keep error behavior identical for dynamic type errors and uninitialized loads. Facts never remove initialization guards, overflow/divide checks, dynamic operation fallbacks, or source diagnostics.
- [x] Test loops/joins, recursion policy, object-to-integer reassignment, self-assignment, deletion, free aliases, and global mutation across a direct call. The value-fact unit test covers the lattice transitions; native/C execution, differential, and stress suites retain dynamic/error coverage.

### 7. Redundant-access elimination

Current implementation: a conservative redundant-access pass runs after folding and CFG cleanup. It performs block-local value numbering only for ownership-free primitive expressions, forwards only proven integer slot values, and treats all memory/ownership/error operations as barriers. It is deliberately not a general alias optimizer.

- [x] Add local value numbering or a similar pass for safe repeated pure loads/expressions. The pass value-numbers null, integer literals, `!`, `==`, and `!=` inside one block. Definitions used by another block are retained, and an eliminated primitive's `release` is dropped rather than redirected to its representative.
- [x] Eliminate repeated local/global slot loads only when calls, stores, aliasing, volatile rules, and error behavior cannot change the result. Only initialized slots with a locally proven integer value forward to an integer IR value. Calls invalidate global facts; every object load, unknown value, mutation, and potential diagnostic retains its normal access.
- [x] Remove redundant stores when the value is unchanged and the store has no required initialization/error effect. A second write of the same proven integer to an already initialized slot is removed; declarations and initialization transitions are never removed.
- [x] Remove duplicate tag loads and initialization checks after preserving the first check's diagnostic path. Forwarded primitive slot values lower as integers, so native emitters omit their duplicate tag/initialization fast-path checks. The first load/store check, all dynamic/object accesses, and all error paths remain.
- [x] Track memory effects by operation: globals, locals, objects, buffer/array contents, allocation, output, ownership, calls, sanitizers, and possible error. `ir_effects.c` provides the shared classification; any non-pure effect clears the value-numbering window.
- [x] Reduce dirty-payload flushes using liveness at individual edges and helpers, not only block-level boundaries. Existing edge liveness, helper visibility, cleanup-root, and spill-reason logic controls native flushes; this pass does not introduce an extra synchronization point.
- [x] Keep sanitizer hooks, debug visibility, and runtime ownership synchronization as observable effects where required. `--debug-info` retains source accesses. Native sanitizer builds retain global accesses for ASAN/TSAN hooks while still folding local proven-integer accesses that have no hook; the pass never forwards owned objects.
- [x] Count emitted loads/stores/branches/helper calls per target and compare before/after on fixed fixtures. `--emit-map` exports target-independent redundant-access counts. `--asm-comments` emits per-function `B++ access-traffic` groups for all four targets; the fixed redundant-store fixture verifies optimized `0/1` load/store groups against baseline `1/2` on every target.
- [x] Add tests with calls between loads, side-effecting builtins, aliasing arrays/buffers, and failures on the second access. The 57-case execution suite includes direct global mutation, buffer alias mutation through a call, repeated output, bounds after a successful access, and existing array/buffer/free/cleanup coverage; unit tests cover value-numbering, store removal, and effect classification.

### 8. Constant folding and propagation

Current implementation: folding and propagation use the same flow-sensitive value-fact lattice as the native emitters. Exact signed integers survive a CFG merge only when every reachable predecessor agrees; unknown facts retain ordinary runtime IR and checks.

- [x] Specify folding rules per opcode and value kind, including signed boundaries, equality, truthiness, strings, and conversions. `docs/numeric-semantics.md` defines the contract: exact integer unary/binary operations and comparisons fold only when defined; proven truthiness folds `!` and branches; string/buffer/array/object operations and conversions stay dynamic in this revision.
- [x] Implement a constant lattice and merge facts across predecessor edges. `BPPValueFact` carries kind, exact integer, and truthiness. Its fixed-point solver merges only reachable predecessors; identical integer constants remain exact, compatible object kinds merge to object, and disagreements become unknown.
- [x] Propagate constants through loop preheaders and loop-invariant expressions without assuming a loop executes. Facts flow over preheader and backedge to a fixed point. An invariant survives only when the backedge agrees; no fact from a loop body is used after a possibly-zero-trip loop unless every path proves it.
- [x] Keep potentially failing constant operations at runtime when folding would suppress or move a language-visible error. The shared integer fold routine refuses signed overflow, zero division/modulo, and `INT64_MIN / -1` or `% -1`; those original IR operations retain their source diagnostic.
- [x] Preserve source location for folded operations so a diagnostic still points to the correct expression. Folding changes opcode/operands in place and retains each instruction's original line and column; the optimized-IR regression checks that source annotation.
- [x] Fold safe unary/binary operations and comparisons for every target consistently. The target-independent IR pass folds `-`, `!`, `+`, `-`, `*`, `/`, `%`, and all integer comparisons through `bpp_integer_fold`; C, ARM64, and x86-64 consume that same optimized IR.
- [x] Add branch and switch folding once those IR forms exist. The existing truthiness branch folds when the incoming fact is true/false and unreachable CFG arms are removed. Revision 1 has no switch IR opcode; numeric semantics documents that a future switch must add its own exhaustive fold tests.
- [x] Test INT64_MIN/MAX, multiply overflow, zero division/modulo, minimum/-1, nested branches, loops, and calls that mutate globals. Unit tests cover constant reachable-merge and invariant-loop facts; the 64-case native/reference suite retains the signed boundary, error, nested branch, loop, and global-call regressions.
- [x] Run every differential fixture with optimization enabled and disabled. `check_differential.sh` builds native executables in optimized and `--no-opt` modes for every seed, then compares each against an independent test evaluator; C emission was retired in 1.0.15.

### 8. Dead-code elimination

Current implementation: a conservative effect-aware backward mark pass removes unused ownership-free primitive computation chains and releases of values proven primitive. It preserves all potential language errors, output, allocation, calls, loads/stores, mutation, and ownership effects. CFG and private-procedure reachability cleanup run before native lowering.

- [x] Define an effect table for all IR operations: output, allocation, retain/release, mutation, global access, potential runtime error, and control flow. `ir_effects.c` classifies every revision-1 opcode with local/global/object reads and writes, allocation, output, ownership, call, sanitizer, error, and control flags; the backend contract documents the table and DCE rule for each class.
- [x] Remove dead pure computations only when dropping them cannot suppress overflow, division, bounds, uninitialized-variable, or type errors. DCE marks backwards from every effect/control root and removes only unused `NULL`, `INT`, `NOT`, `EQ`, and `NE` chains. Dynamic arithmetic, indexed access, and all load paths remain roots because their diagnostics are observable.
- [x] Remove dead stores only when initialization semantics and error behavior remain intact. The existing redundant-access pass removes only a second identical proven-integer store to an already initialized slot. DCE retains all other stores and every initialization transition because they may affect later reads, ownership, or diagnostics.
- [x] Remove unused cleanup/release operations only with ownership proof; do not leak objects or release aliases early. A release is discarded only when its defining result is proven primitive on normal continuation; release of any unknown, string/buffer, array, or other object remains a root.
- [x] Remove dead blocks and repair branch targets, source maps, and function IDs safely. CFG reachability removes blocks after constant branch folding and remaps surviving jump/branch targets. Private-procedure reachability then removes unused functions, remaps direct-call IDs, and runs the verifier after both transformations.
- [x] Keep exported/address-taken procedures once a public ABI or library model introduces them. Revision 1 has neither exported nor address-taken procedures: all calls are private/direct, so only the module entry is a procedure root. The documented policy requires future ABI exports, addresses, callbacks, and static-library entry points to be roots before procedure pruning.
- [x] Add tests for effectful dead-looking calls, errors in unused expressions, recursive-but-unreachable procedures, and object cleanup. Optimizer units prove effect roots and primitive-release pruning; the execution suite retains output calls, unused divide-by-zero, recursive private-procedure pruning, and owned-object cleanup coverage.

### 9. Instruction selection

Current implementation: `isel.c` is a shared target-aware selector between optimized IR/register locations and the ARM64/x86-64 emitters. It chooses legal immediate and address forms with an explicit cost, scratch-GPR demand, and flags lifetime. Dynamic/error paths retain their private runtime fallback.

- [x] Define an instruction-selection table for every IR opcode on every target. `BPPISelRule` has one exhaustive revision-1 opcode entry per registered target; `docs/instruction-selection.md` records native, mixed, and runtime-only lowering. Object format changes syntax/relocations, not ISA form legality.
- [x] Add legality/cost checks for immediate forms, extensions, shifts, multiply-add, compare-branch, address generation, and load/store addressing modes. ARM64 selects unsigned 12-bit or shifted-12-bit add/sub/compare immediates; x86-64 selects signed-32-bit add/sub/multiply/compare immediates; address selection distinguishes direct, immediate, displacement, and materialized forms. Revision 1 has no shift/extension/vector/FMA IR shape, so those are explicitly unavailable rather than silently selected; checked ARM64 modulo uses its defined `msub` reconstruction.
- [x] Select instructions using register constraints and operand locations rather than emitter-local special cases. Both native emitters ask the selector using target, opcode, and flow-proven RHS constant. The selector exposes scratch demand; emitters use allocator-provided payload/cell locations and reject any form exceeding their documented scratch budget.
- [x] Add target-specific lowering for useful operations while keeping fallback semantics and errors identical. ARM64 now emits shifted add/sub and signed negative compare immediates (`cmn`); x86-64 emits immediate32 arithmetic/comparisons. Integer guards, overflow, zero-divide, `INT64_MIN / -1`, dynamic type checks, and helper fallback paths are unchanged.
- [x] Model flags/condition-code lifetime so instruction combines do not overwrite live conditions. Every selected flag-defining form declares a local consumer; emitters output the overflow branch or `cset` directly next to it. No selection keeps flags live across an IR instruction, reload, address calculation, or helper setup.
- [x] Model scratch-register demand before choosing a sequence; avoid hidden scratch conflicts. Each selection reports required scratch GPRs (zero for immediate forms, four for generic scalar/divmod, at most one for materialized addressing). Emitters refuse a form above their four reserved scratch GPRs and take the safe runtime path.
- [x] Add assembly golden tests and executable tests for each selected form, including negative/overflow boundaries. `test_isel` covers all opcode rules, immediate/address bounds, form costs, flags, and scratch demand. The execution/golden suite verifies ARM64 shifted/negative forms and x86-64 immediate32 output; signed overflow, divide/modulo boundaries, and dynamic fallback cases remain execution regressions.
- [x] Provide repeatable instruction-selection size/timing measurements and require host evidence before retaining a complex sequence. `make benchmark` compares selected forms with legal generic/materialized lowering for immediate arithmetic, immediate comparisons, and calls; it records executable, `.text` (when available), and assembly-source bytes; median wall time/MAD; sample count; target/CPU/OS/compiler identity; and available hardware cycles. Linux uses per-process user-mode `perf_event` cycles when permitted and Windows uses `QueryProcessCycleTime`; macOS records cycles as unavailable rather than falsely treating timer ticks as cycles. Run the harness on native macOS ARM64, Linux ARM64, Linux x86-64, and Windows x86-64 and archive each CSV under the matching target profile before using that profile's result to retain a new complex form.

### 10. Control-flow optimization

Current implementation: after folding/DCE, the CFG pass threads jump chains, removes unreachable jump-only blocks, merges a jump with its sole-predecessor target, then trace-orders blocks. A backwards successor is statically preferred as a loop-hot edge; otherwise the source-order true edge is preferred. Native emitters omit adjacent jumps and use a fall-through-oriented conditional sequence when either branch successor is physically next. IR source locations stay attached to their instructions; native error/cleanup labels remain out of line after procedure blocks and use range-safe transfers.

- [x] Lay out blocks to favor fall-through on likely edges while keeping branch targets and source locations correct. The deterministic trace builder keeps entry first, prefers a backedge as loop-hot, otherwise prefers the source-order true successor, and retargets every terminator after reordering. Instruction source spans remain unchanged.
- [x] Add branch inversion to remove unconditional jumps after conditional branches. ARM64 and x86-64 select a true- or false-fall-through truthiness sequence when either successor is adjacent, leaving the other outcomes as range-safe conditional transfers; they no longer emit the former two terminal unconditional edge jumps. Adjacent unconditional jumps are omitted too.
- [x] Merge compatible blocks and remove empty jump-only blocks. A jump block with a sole-predecessor target absorbs that target's instructions; jump-only blocks are threaded then removed by reachability compaction.
- [x] Simplify chained jumps and redundant branches after CFG mutation. Target resolution follows jump chains to a fixed point; a branch whose successors converge becomes a direct jump, then the compact/merge loop repeats.
- [x] Preserve loop headers, call return sites, unwind ranges, and debug locations during reordering. The pass stays within one procedure, retargets all loop/header labels, does not move instructions across calls, retains individual instruction source spans, and leaves procedure-level CFI/SEH plus out-of-line cleanup unchanged.
- [x] Separate cold error/cleanup paths where doing so improves hot code without making branches exceed range. Native emitters already place failure/cleanup labels after normal procedure blocks; the CFG pass never folds them into IR traces, and ARM64 keeps page-relative far transfers.
- [x] Add CFG validation after every block-reordering/merging pass. The optimizer validates after each simplify round and after final layout, with the pass/function/block diagnostic context supplied by the IR verifier.
- [x] Test nested loops, diamonds, early returns, error paths, and long cold blocks on every emitter. The optimizer unit covers jump threading, block merging, diamond trace reordering, and verifier validity. The execution suite retains nested loop/branch/early-return/error/large-frame cases; output-mode checks require all four emitters to contain the fall-through branch sequence, while native execution validates it on the host backend.

### 11. Large-program branch and address handling

Current implementation: ARM64 uses a checked small ADRP-page model for internal labels; x86-64 uses a large internal-label model (`movabs` followed by indirect `jmp`/`call`). The C runtime/toolchain boundary remains host-linker-owned until the assembler/linker stage.

- [x] Enumerate range limits for ARM64 ADRP relocations, literal/data references, x86-64 rel32 branches/calls, and each object format. `bpp_target_range_limits` records Mach-O `ARM64_RELOC_*`, ELF `R_AARCH64_*`/`R_X86_64_*`, and COFF `IMAGE_REL_AMD64_*` names with their byte ranges. `--target-info` exposes the selected target's table. See `docs/branch-address-model.md`.
- [x] Add a post-layout range checker that reports the source instruction, target label, and exceeded range. Before native emission, the ARM64 checker lays out conservative block/procedure/data labels after optimization and validates every generated ADRP reference. Its diagnostic includes the IR source location, source label, target label, signed distance, relocation, and allowed range. The public single-site checker verifies synthetic boundary layouts too.
- [x] Add ARM64 branch veneers/islands or a documented code model when a target exceeds branch/page range. Revision 1 explicitly supports one generated text/data image inside the ADRP page range; conditional transfers skip around `ADRP+ADD+BR/BLR`, so only that local skip needs a conditional-branch range. The compiler rejects an exceeded generated range. Linker-owned islands and literal pools remain part of the assembler/linker stage, not an implicit unsafe fallback.
- [x] Add x86-64 branch islands or long-call sequences for text beyond rel32 range. Every B++ internal cross-block jump and user-procedure call now materializes the absolute target in `r11` and transfers indirectly. String and layout addresses use the same `movabs` model. ELF native output uses `-no-pie` for these internal `R_X86_64_64` references until the B++ linker owns a PIE-capable large model.
- [x] Coordinate symbol, relocation, section layout, and veneer rules with the future assembler/linker. The branch/address model documents which relocations remain host-toolchain-owned (runtime/PLT/GOT), which are B++ internal, the current code-model contract, and the linker work that must replace it: final section layout, islands, literal pools, relocations, and final range validation.
- [x] Test at boundaries using generated object layouts; do not require multi-gigabyte CI fixtures. `test_ranges` accepts exact legal and one-byte-over ARM64 ADRP and x86-64 rel32 synthetic layouts, asserting the complete diagnostic instead of allocating multi-gigabyte source/object files.
- [x] Keep short local conditional branches short and long cross-block transfers range-safe. ARM64 uses local inverse conditional skips around page-relative indirect transfers. x86-64 uses local inverse `jcc` skips around `movabs`/indirect transfers; all non-adjacent B++ CFG edges and user calls are range-independent.
- [x] Test Mach-O, ELF ARM64, ELF x86-64 and COFF relocations independently. The four-target output checks assert the correct relocation table and transfer model in each assembly output. `check-targets` assembles Mach-O ARM64, ELF ARM64, ELF x86-64, and COFF x86-64 independently in optimized and baseline modes when a Clang cross-assembler is available; matching-host CI remains the final native/link verification gate.

### 12. Formal ABI call lowering — depends on stage 2

Current implementation: static-only ABI v1 is formal. `BPPStaticProc` is the public five-argument procedure boundary; local and archive procedure calls use that exact type. A target-specific `libNAME.bpp.a` plus `libNAME.bppabi` manifest carries ABI/runtime revision, export arities, dependencies, and archive byte-count/FNV-1a identity. Private per-procedure frames remain internal to code generation.

- [x] Stage 2 defines calling convention, value representation, alignment, ownership, error propagation, symbol names, visibility, versioning, and compatibility rules. `include/bpp_abi.h` and `docs/static-abi.md` define ABI v1: `BPP1`, major/minor validation, static-only/no-FFI flags, value-layout checks, error status, target calling conventions, `bpp_v1_` symbols, target archive selection, and archive identity validation.
- [x] Define argument classification for the deliberately small set of B++ types exposed by static libraries; add aggregates only when the language has a concrete need. Every v1 argument is one borrowed 16-byte tagged `BLValue`: null, signed i64, or reference-counted object. There are no aggregate, FP, vector, callback, or variadic classes.
- [x] Define return classification and error/status behavior for supported B++ values; defer multiple returns and complex aggregates unless required by the language. A procedure produces one owned `BLValue` through `owned_result`; the caller releases it. Descriptor arity errors are checked before entry and runtime failures surface as `BPP_STATIC_RUNTIME_ERROR`/`runtime->error`. Multiple returns and aggregates are excluded.
- [x] Define stack alignment, stack arguments, callee/caller-saved registers, and target-required shadow/home space for B++ library/runtime calls. Variadic calls are out of scope for the public ABI. The ABI uses the target C ABI: AAPCS64 x0-x4, SysV rdi/rsi/rdx/rcx/r8, or Windows rcx/rdx/r8/r9 plus the fifth stack argument and shadow space. Existing prologue/epilogue rules remain authoritative.
- [x] Define the B++-to-B++ static-library call convention and B++ symbol/export rules. Keep system entry points in the private runtime boundary. `--emit-library --library-name NAME --emit-asm` omits `main`, retains named procedures, and emits `bpp_v1_<procedure>`, `bpp_v1_library_<name>`, and its procedure table. `--emit-library-manifest` writes the target-specific `libNAME.bppabi` companion for `libNAME.bpp.a`, including the exact byte count and FNV-1a identity generated after `ar`; the runtime entry point remains private.
- [x] Replace private call lowering only after the ABI spec and conformance tests are accepted. `BPPNativeProc` is now an alias of `BPPStaticProc`; native internal calls and exported procedures share the verified signature, while private frame layout stays behind it.
- [x] Add separately compiled B++ library producer/consumer fixtures for macOS ARM64, Linux x86-64/ARM64 and Windows x86-64. `check-static-abi` emits a provider library for all four targets, packages the matching-host archive, generates `libprovider.bppabi`, and executes a separate B++ consumer using `import library "provider";`. It checks direct and transitive archive order, import arity, stale archive rejection, and a deterministic `--emit-link-map`. The release CI jobs invoke this gate on macOS ARM64, Linux x86-64/ARM64, and Windows x86-64; a green run on each runner is the release evidence.
- [x] Add ABI compatibility tests across compiler versions before static library work. Descriptor validation rejects magic/major/value-layout mismatches and newer library minor revisions; `test_static_abi` freezes the v1 layout constants, ownership/arity behavior, and major-version rejection. Future ABI changes must add a new major/prefix plus a fixture built by the previous released compiler.

### 13. Further runtime independence

Current implementation: arithmetic, comparisons, core branches, user-procedure control flow, private-frame allocation/zeroing/layout, parameter-cell cloning and object retains, outgoing borrowed-argument copies, return-value transfer, primitive root/local clearing, runtime-error probes, executable startup/exit, literal-string construction, allocator leaves, and recursive object destruction are native for ordinary executable output. The emitted launcher owns the runtime record, globals, initialization flags, result/error path, and object-reference cleanup; it never calls `bpp_a_run`. Dynamic string operations, buffers, arrays, builtins, and general dynamic operations still use deliberately narrow C helpers.

- [x] Inventory every emitted helper symbol and map it to runtime source, ownership contract, sanitizer behavior, and target calling convention. `BPPNativeRuntimeHelper` is compiled with the runtime and tested for complete non-null records. `check-runtime-boundary` rejects generated ARM64/x86-64 assembly that reintroduces the retired argument-copy, argument-base, or error-probe helpers.
- [x] Decide which helpers remain in the runtime and which become direct native sequences. The helper inventory marks retired direct sequences. Native emitters now copy borrowed `BLValue` argument pairs directly, load the outgoing argument base from the private frame directly, and test `BLRuntime.error` directly. String allocation, object release, dynamic operations, builtins, frame setup/teardown, and process startup remain runtime calls.
- [x] Design a B++ runtime boundary compatible with the formal ABI and assembler/linker object model. `codegen_abi.h` names and static-asserts the private frame/runtime offsets used by assembly; `native_runtime.h` describes every callable bridge helper. Both are compiler-private revision-2 boundaries and remain distinct from the public static-library ABI v1.
- [x] Implement native startup/exit, global initialization, frame entry/cleanup, and error propagation. The x86-64 and ARM64 launchers allocate global `BLValue`/initialization arrays, build the private runtime record on their stack, call module procedure zero, print either the result or recorded error, release the result and every global object, free process state, and return its status. `bpp_a_run` is legacy/unit-only and rejected from emitted assembly.
- [x] Implement or retain value clone/release, allocation, strings, buffers, arrays, indexing, and builtin calls with identical semantics. Native code handles parameter clone/retain, borrowed argument copies, value transfer, primitive clearing, and object-reference call sites. ABI-decorated allocation/free, refcount destruction, dynamic values, strings, buffers, arrays, indexing, and builtins remain C leaves until B++ runtime replacements exist.
- [x] Define allocator failure behavior, integer-size conversion, alignment, and object layout before replacing C allocation. `bpp_a_allocate_zero` rejects count×size overflow with `ENOMEM`, normalizes zero dimensions to one byte, and returns `calloc` storage aligned for `BLValue`; null routes to the native fatal path. Object payload/destruction remains private C runtime representation and outside static ABI v1.
- [x] Add ownership/refcount tests for aliases, nested arrays, self-assignment, free/delete, errors, recursion and returned values. Native ownership unit tests cover nested aliases, shared slots, return transfer, and error cleanup; the 66-case native suite covers self-assignment, free/delete, arrays/buffers, recursion, calls, and return pressure. Allocation overflow/zero-initialization/alignment is unit-tested at the launcher boundary.
- [x] Run ASAN/LSAN/UBSAN/TSAN against both old and replacement runtime paths where available. Linux x86-64 `debug` (ASAN/UBSAN with LSan exit-time scanning disabled in this sandbox) and `tsan` both pass unit tests plus the 66-case C-reference and native suites after the native launcher/runtime boundary changes. `check-asan-hook` detects a poisoned native global access. MSAN remains Linux/Clang-only and separately covers the C compiler/runtime path; it does not claim full hand-emitted assembly shadow instrumentation.
- [x] Remove C runtime dependency only when all required program behaviors pass on all targets. A strict `--strict-native-runtime` bootstrap profile emits and links a constant-result executable without `runtime.o`, `value.o`, `codegen_runtime.o`, or `bpp_abi.o`; it cross-emits helper-free assembly for Mach-O ARM64, both ELF targets, and COFF x86-64. Normal native executables now emit their own allocation/free/fatal leaves, recursive object release/destruction for strings/buffers/arrays, runtime-error reporting, destination clearing, integer/null result formatting, and direct `BLString` literal allocation/copy/installation. The boundary test executes a host-native returned string literal and rejects `bpp_a_string` in generated Linux x86-64 and macOS ARM64 output. Those leaves use only the host allocator/process/output ABI; destination clearing and cleanup no longer call `codegen_runtime.c` or `value.c`. ARM64 object result formatting deliberately remains a narrow C fallback pending native string/buffer/array formatting. The profile intentionally rejects output formatting, globals, procedures, imports, and dynamic values rather than silently falling back to C. Remaining work is native frame replacement, dynamic string operations, buffers, arrays, builtins, dynamic operations, and all target test suites.

### 14. Debug metadata

**Target release:** 1.0.14. Complete the remaining debug metadata implementation and native debugger/unwind verification gates below for this release.

Current implementation: raw-native DWARF5 procedure, parameter, local, type, address-table and initialized-slot location-list records on Mach-O ARM64 and both ELF targets. Source `byte` locals use their actual signed 64-bit frame representation; pointers use pointer DIEs, and untyped parameters expose raw words under an explicitly unspecified source-type name. Windows x64 emits CodeView signatures, source-type aliases, procedure/parameter/local names, initialized-slot defranges, source files and line tables in COFF debug sections; debug linking uses LLD to produce a PDB. `--emit-map` includes full canonical source-variable ranges. Raw stack probes, CFA rules and Windows nonvolatile save/restore paths are covered by assembly checks. Local macOS ARM64 LLDB stepping, imports/recursion, displayed parameter/local values and dSYM inspection pass; LLVM verifies the dSYM and decodes COFF records. Remaining native Linux GDB and Windows LLDB/PDB verification is assigned to 1.0.16. See `docs/debug-metadata.md`.

- [x] Preserve original source spans through imports, macros, lowering, folding, DCE and block reordering.
- [x] Represent source files and include/import stacks instead of mapping all expanded code to one sidecar.
- [x] Emit DWARF5 procedure, parameter and variable DIE names/types with function scopes and source-file tables on Mach-O ARM64 and ELF x86-64. Parameters use an explicitly unspecified B++ value type because the language has no parameter type syntax; local declarations retain their source type. B++ locals are function-scoped, so there are no nested lexical blocks.
- [x] Emit DWARF5 location lists for raw-native source locals on Mach-O ARM64 and ELF x86-64, using frame locations only while definitely initialized.
- [x] Emit CodeView source-file and line-table records for Windows x64 raw and optimized assembly output; cross-assemble the generated COFF object and inspect its CodeView line subsection when LLVM tools are available.
- [x] Emit CodeView procedure/parameter/local records with source-level names and types, including raw source-type aliases and definitely initialized register-relative frame ranges.
- [x] Produce and inspect a linked Windows PDB containing CodeView source-file and line records when `lld-link` and `llvm-pdbutil` are available; the Windows CI job now requires both tools.
- [x] Expose allocator register/spill segments over IR positions in `--emit-map`; the allocator regression suite includes a value that transitions from a register to a spill slot. Source-variable write events identify their assigned IR value and its location at the write. [x] Track current raw source-variable values across their full initialized ranges in DWARF, CodeView and `--emit-map`. These locals have canonical frame homes, with no source-local register promotion; allocator IR cache segments are not reported as named-local register/spill locations.
- [x] Leave optimized-backend source-variable location lists empty when no trustworthy location is available, so debuggers report the variable unavailable instead of reading stale frame cells. `--emit-map` labels these locations `unavailable_no_trustworthy_location`; raw-backend map entries identify their exact slot only while initialized. An empty DWARF location description is the standard representation for a variable with no executable location.
- [x] Emit unwind rules for raw x86-64 and ARM64 frames and Windows x64 SEH prologues; x86-64 CFI includes saved-register transitions through the shared epilogue. ARM64 rules keep CFA anchored to x29 across stack probes and helper calls.
- [x] Add unwind inspection for ELF procedure FDEs, saved-register restores, and canonical CFA restoration; cross-target assembly tests cover ARM64 CFI and Windows x64 SEH directives. Remaining native-unwinder coverage is tracked under 1.0.16.
- [x] Add metadata inspection checks for generated assembly and ELF DWARF info/location/unwind metadata with `readelf`; Mach-O DWARF/dSYM is checked with `dwarfdump` on macOS. CI installs `llvm-dwarfdump` and `llvm-readobj` on Unix/Windows runners and invokes them when present. Remaining matching-runner evidence is tracked under 1.0.16.
- [x] Add debugger stepping checks for loops, calls, recursive frames, imports, and runtime faults in the macOS LLDB test and Linux GDB test. [x] Add Windows LLDB stepping and parameter/local value inspection with native linked PDBs; CI requires LLDB/LLD/PDB tools. Local macOS LLDB passes. Remaining matching-runner debugger validation is tracked under 1.0.16.

### 15. Native sanitizer instrumentation

**Target release:** 1.0.15 for the native sanitizer implementation and C-backend retirement. Remaining cross-platform verification fixes and evidence gates are assigned to 1.0.16.

Current implementation: sanitized raw-native word/byte memory operations, pointer offsets, output ranges and allocation/free transitions use a portable checked C boundary. It tracks per-byte definedness, allocation/frame lifetimes, guarded frame storage, retired-region tombstones and conservative global-root/cycle leak reachability. Host sanitizers instrument the C boundary's actual memory accesses, and TSAN traces native procedure entry/exit. Diagnostics retain original source locations and portable faults exit 86. Default release output and the explicitly helper-free strict bootstrap profile omit the boundary. `--native-sanitizers` opts a release compiler into portable checks without requiring host sanitizer runtimes; Windows CI exercises this profile. macOS ARM64 ASAN/UBSAN and TSAN intentional-fault suites pass locally; all four instrumented targets assemble. Hosted Linux/Clang MSAN native tests, differential execution and stress pass; remaining platform verification is assigned to 1.0.16. See `docs/native-sanitizers.md`.

- [x] Define language guarantees versus debugging instrumentation in `docs/native-sanitizers.md`, including the explicit helper-free bootstrap exception.
- [x] Specify a portable `native_sanitizer.h` boundary and range/definedness metadata; host shadow mapping and ASAN/TSAN hooks stay inside C.
- [x] Instrument raw native word/byte reads/writes, pointer offsets, output ranges, integer faults and allocation/free transitions. Compiler-owned frame initialization/argument/result housekeeping stays trusted. Raw source values have no tags, managed arrays or buffers; the transitional tagged backend retains its existing instrumentation.
- [x] Give sanitized raw frames guarded heap storage with ASAN-poisoned redzones, unpoison backing storage before free, and retire frame regions on every return. Checked stale frame accesses report `stack-use-after-return`; direct runtime tests cover bounds and lifetime faults.
- [x] Check native application accesses before one actual C load/store; host instrumentation observes that access once. Clang MSAN parameter/return TLS checking is disabled only for the assembly boundary object, whose portable byte-definedness map checks native inputs.
- [x] Define non-atomic C memory events and native frame tracing, plus the current single-threaded registry/no-thread/no-FFI limitation. Future atomics and synchronization require a separate language and metadata design.
- [x] Track raw native allocations, scan conservative global roots and initialized aligned words, and report unreachable allocations/cycles at normal completion. Root/cycle/reallocation tests cover the policy; fatal exits do not perform the completion leak scan.
**Verification follow-up:** all remaining compiler/OS/architecture availability, native execution, metadata and debugger gates are consolidated under 1.0.16 below.
- [x] Add a Linux/Clang MSAN mode with PIE, an intentional uninitialized-read self-check, and a dedicated Linux x86-64 CI transcript. MSAN is rejected on macOS, Windows, and non-Clang toolchains rather than silently degrading.
- [x] Verify the Linux/Clang MSAN native checked boundary on Linux x86-64 CI. [Native tests, 64-seed differential execution and stress passed](https://github.com/kenseitehdev/B-/actions/runs/38204275312/job/114664184601) for `a58a999edbb2c855a277c8c492666feb7d588cc5`. The portable definedness map complements host MSAN C accesses; arbitrary assembly/FFI shadow propagation is not claimed. Reverify the final 1.0.16 revision with the full matrix.
- [x] Add portable runtime faults for bounds, undefined bytes, growth, invalid/double free, use-after-free/return, frame overflow, negative sizes, arithmetic overflow, leaks and cycles; native fixtures assert source/category/exit 86 in optimized and baseline modes on the host. Cross-assemble instrumented output for all four targets.
- [x] Verify all four release assemblies contain no `bpp_s_*` hooks; default release raw executables omit the checked boundary object. `--native-sanitizers` explicitly enables portable checks in a release compiler; sanitized builds enable them automatically, except the explicitly requested helper-free strict bootstrap profile.

- [x] Remove the C output backend and capability API, delete the C emitter, reject `--backend c`/`--emit-c`, and use native code generation exclusively. Replace C-reference comparisons with independent seeded expected results for optimized/baseline native execution.

### 16. Broader verification and release gates

**Target release:** 1.0.16. All remaining cross-platform verification fixes from 1.0.14/1.0.15 belong here. Native generation and C-backend retirement are implemented; an incomplete or unsupported runtime probe is not a passed validation gate.

Current evidence: all 18 jobs / 19 execution profiles pass on implementation `2c817e176897d0addfd7df624b01d4e136ac1f57` in [native CI run 47](https://github.com/kenseitehdev/B-/actions/runs/38329351396). Matching hosts cover macOS ARM64, Linux x86-64/ARM64 and Windows x86-64. Exact toolchains, sanitizer availability, job links and transcript artifact digests are recorded in `docs/verification-1.0.16.json`. The closing documentation commit does not change the tested implementation.

Priority carryover:

- [x] Add AND, OR, XOR and bitwise NOT at the end of 1.0.16, with parser/IR/optimizer/native/static-ABI coverage.
- [x] Define and implement TAU (`=>`) from the supplied table/equations, including fractional uncertainty/error propagation across all Boolean results and conditions. Accepted: `a => b = sign(b-a)`, signed truth in [-1,1], logical AND=min/OR=max/NOT=negation/XOR=max(min(a,-b),min(-a,b)), and conditions that threshold transition velocity rather than final state. Binary64 state representation, explicit velocity thresholds and retained error provenance are implemented with ABI 2.0/runtime revision 3; see `docs/truth-and-tau.md`.

- [x] Finish Linux ELF metadata and native GDB verification on x86-64/ARM64, including source stepping, imported recursion, variable values and unwinding. Validate the extra DWARF-terminator fix, readelf formatting checks and x86 callback trap-flag masking on matching runners.
- [x] Finish Windows x86-64 native portable sanitizer checks, CodeView/PDB inspection, LLDB stepping/variable values and SEH/stack-probe execution. Validate Windows temp/import paths, executable suffix handling, and ELF-only inspection guards.
- [x] Resolve unusually long GCC debug/TSAN jobs on both Linux architectures with indexed predecessor queries and incremental value-fact propagation; retain toolchain/runtime probes and complete native intentional-fault/differential execution. Keep unsupported sanitizer combinations explicit.
- [x] Verify existing static linking and ABI v2 on each native runner: separately compiled archive producers/consumers, transitive dependencies and archive order, exports/arity, manifest/archive identity, and incompatible ABI/target rejection. Retain `check-static-abi` evidence alongside native executable tests.
- [x] Complete the final Clang/GCC/OS/architecture matrix with exact compiler revisions and retained transcripts; rerun Linux/Clang MSAN after the final verification fixes. Cross-assembly alone does not satisfy execution gates.
- [x] Keep CI runs bounded, cancel superseded runs and avoid duplicate push/PR verification jobs; remove invalid AppleDouble workflow metadata from the repository.

- [x] Run all tests on every supported native OS/architecture; cross-assembly is a separate gate.
- [x] Retire the temporary C backend at 1.0.15. Native modes compare fixed stdout/return expectations and independent seeded results; intentional-fault fixtures compare source location, category and exit behavior in both optimization modes.
- [x] Extend deterministic randomized programs across supported arithmetic, branches, loops, calls, recursion and byte buffers; independently verify state/error propagation and reject unsupported runtime string/array source constructs.
- [x] Add seed reporting and retain original inputs plus bounded minimized reproducers for generated failures; minimization is best effort within a fixed budget.
- [x] Add deep recursion, stack exhaustion, many parameters, high live-value pressure, many locals, huge block count, large text section, and frame-probe boundary tests.
- [x] Test both optimized and `--no-opt` native modes for semantic and positive stress fixtures; intentional stack exhaustion uses baseline native recursion.
- [x] Test debug metadata builds and sanitizer builds separately from release.
- [x] Run the Linux/Clang MSAN job and retain its transcript; do not report MSAN as a macOS/Windows sanitizer mode or as full hand-emitted-assembly instrumentation.
- [x] Run native CI on macOS ARM64, Linux x86-64, Linux ARM64 and Windows x86-64 and retain logs/artifacts.
- [x] Add compiler warning-as-error builds on Clang and GCC-compatible toolchains where supported.
- [x] Fuzz lexer/parser/IR validator/optimizer/emitter with resource limits and sanitizer builds.
- [x] Test reproducible assembly/object output or document nondeterministic fields.
- [x] Gate a target release on its native runtime suite, ABI conformance, unwind, debug, and supported sanitizer checks.

### 17. Bake build tooling, static linking and performance

**Planned target release:** 1.0.17, after the 1.0.16 verification work. Bake is a make-like build tool using a `Bakefile`; the initial contract is defined in `docs/bake.md` and implemented by `tools/bake.py` (portable Python bootstrap). Native cross-platform execution remains a release gate. Sanitizer integration belongs to the later debugger and is excluded from Bake's built-in commands, configuration and build profiles.

- [x] Define `Bakefile` syntax for named/default targets, dependencies, source files, compiler options, output paths, target platforms and executable/static-library outputs.
- [x] Define and implement the `bake` command and make-like target selection, alternate Bakefile selection, build, clean and ordinary test commands; document invocation and failure/exit behavior.
- [x] Implement dependency ordering and incremental rebuilding, including imported source files, changed options/toolchain/target, and changed static-library manifests or archives.
- [ ] Drive the existing native compiler, assembler, archiver and linker with portable path/argument handling on all supported hosts; keep build outputs separated by target and profile.
- [x] Keep Bake builds free of sanitizer targets, flags, options and profiles, including implicit sanitizer activation inherited from compiler build defaults. Ordinary debug metadata is separate; sanitizers will be integrated with the debugger later.
- [x] Support creating and consuming target-specific static B++ archives and their `.bppabi` manifests through Bake, with manifest generation after archive creation and correct transitive archive ordering.
- [ ] Preserve and validate the existing static-only ABI v2 contract: export symbols, arity, value layout, alignment/ownership, ABI/runtime revisions, target compatibility and archive identity. Broad FFI and dynamic loading remain outside this scope.
- [ ] Add Bakefile examples and end-to-end builds for an executable, a static-library producer/consumer and transitive static dependencies; verify incremental rebuilds and ABI/target mismatch diagnostics on native runners.


Benchmark status: schema-2 `tools/benchmark.py` separates native compile/assembly/link/startup phases from in-process ABI work, with raw samples, median/MAD, per-process compile peak RSS and separate untimed native profiling. The local macOS ARM64 16-case smoke run passes. Matching-host retained CI measurements remain a gate; shared CI is not an isolated hardware baseline. Earlier schema-1/C measurements remain historical evidence; see `docs/benchmarking.md`.

- [x] Separate compile time, assembly time, link time, startup time and steady-state execution time.
- [x] Add warmup, repeated samples, median/dispersion, environment metadata and stable machine identifiers.
- [x] Measure generated code/read-only data and frame sizes, dynamic native call depth/frame-stack span and boundary-helper calls, actual emitted spill/reload sites, and per-process compile peak memory. Native frame span excludes deeper C-helper stack usage; static spill/reload sites are not dynamic traffic counts.
- [x] Include integer, branch, recursion, call, owned-state, byte-string/buffer and large-source workloads; record retired managed string/Array source constructs as unsupported rather than benchmarking nonexistent features.
- [x] Compare native optimized and `--no-opt` builds; retain older C measurements as historical evidence and record the future custom assembler/linker as unavailable until implemented.
- [ ] Run on macOS ARM64, Linux x86-64, Linux ARM64 and Windows x86-64 native hosts.
- [x] Keep results in versioned CSV/JSON with compiler revision, flags, target triple, CPU/OS and sanitizer mode.
- [x] Set performance expectations only after repeatable runs; avoid pass/fail thresholds sensitive to shared CI noise.


### 18. Target support and platform-specific integration

All four emitters have matching-host native release evidence in the 1.0.16 matrix: macOS ARM64, Linux x86-64, Linux ARM64 and Windows x86-64. Keep each target's object, runtime, linker, debugger, and sanitizer checks separate. Broader platform policy and isolated benchmarks below remain future work.

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
- [x] Run `make BUILD_MODE=debug test check-asan-hook` on the Mac and record the exact sanitizer runtimes available.
- [x] Run `make BUILD_MODE=tsan test` on the Mac.
- [ ] Run and archive repeated native benchmarks and frame/code-size results.
- [x] Inspect Mach-O unwind and debug output with Apple/LLVM tools; test backtraces through B++ calls/errors.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.

### Linux x86-64 — target profile v1

- [x] Emit System V x86-64 ELF assembly.
- [x] Pass release suite, 64-seed differential suite, large-procedure test and runtime/unit tests.
- [x] Pass ASAN/UBSAN and TSAN test suites on the available Linux x86-64 host.
- [x] Exercise the Windows calling convention with the Linux `ms_abi` bridge; keep it classified as a bridge test only.
- [x] Run LSAN on a host/container where its exit-time process scan works.
- [ ] Run repeated isolated benchmarks and inspect CFI/source-line/backtrace behavior.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.

### Linux ARM64 — target profile v1

- [x] Emit Linux ARM64 ELF assembly using the shared ARM64 instruction selector.
- [x] Cross-assemble optimized and baseline objects.
- [x] Run the compiler and generated executables natively on Linux ARM64.
- [x] Pass unit tests, the native suite, optimized/baseline differential suite and 5000-update stress case. The retired C suite remains historical evidence.
- [x] Run ASAN/UBSAN/TSAN/LSAN where the actual compiler runtime supports them; record unsupported combinations explicitly.
- [x] Inspect ELF relocations, CFI, stack probing, source lines and backtraces on ARM64.
- [ ] Run isolated benchmarks and compare with Linux x86-64 and macOS ARM64.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.

### Windows x86-64 — target profile v1

- [x] Emit Windows x64 COFF assembly and SEH prolog/epilog directives.
- [x] Cross-assemble optimized and baseline objects.
- [x] Pass the Linux `ms_abi` calling-convention bridge suite; retain the limitation that this does not test Windows OS behavior.
- [x] Build and run compiler and generated executables on native Windows x64.
- [ ] Pass unit tests, the C suite, native suite, optimized/baseline differential suite and 5000-update stress case.
- [x] Verify Windows CRT startup, symbol entry point, shadow space, stack arguments, `___chkstk_ms`, guard pages, PE linking and SEH unwinding.
- [ ] Validate paths containing spaces, mixed separators, drive letters, Unicode, case-only aliases, temp files and source/debug-snapshot overwrite protection.
- [ ] Determine MinGW/UCRT/Clang toolchain support and whether MSVC syntax/tooling is in scope.
- [x] Run debug/sanitizer tests only for runtimes that exist; document ASAN/LSAN/UBSAN/TSAN gaps without treating unsupported runtimes as passing.
- [x] Inspect CodeView/PDB or chosen debug format and Windows backtraces.
- [ ] Run isolated benchmarks and record binary/frame sizes.
- [ ] Finish shared CG tasks required by the exit criteria before stage 2.

## 1.21 CG exit criteria before moving to ABI

- [x] Native release test matrix passes on macOS ARM64, Linux x86-64, Linux ARM64 and Windows x86-64.
- [x] Every target has passed semantic differential tests, large procedure/frame tests, recursion, pressure, error cleanup, target calling-convention tests, unwind tests and debug-line tests.
- [x] Debug/sanitizer results are recorded per compiler/runtime/OS, including unsupported combinations and LSAN scan limitations.
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

# 6. XF static library for B++

**Dependency:** ABI, assembler, linker and buildFile library metadata. XF’s public archive interface should be B++-to-B++; internal C components may remain behind the private runtime boundary.

- [x] Inventory XF public procedures, data, runtime entry points and current C archive dependencies. The supplied XF public API exposes state lifecycle/configuration, string/file execution, streaming, errors, and versioning. The initial B++ surface deliberately exposes only `xf_version()`, `xf_run_string(source)`, and `xf_run_file(path)`; stateful streaming and XF values/tables are not yet public B++ values.
- [x] Resolve existing unresolved ByteLang/LambdaScript/interpreter/SIMD symbols before packaging. `Libraries/xf` builds XF source rather than consuming the stale standalone `libxf.a`, includes `simd.c`, and excludes the old embedded LambdaScript/ByteLang modules. The supplied XF snapshot declares but omits `interp_init`/`interp_free`; a separate compatibility source provides only those lifecycle definitions until XF supplies them.
- [x] Decide which XF parts become B++ code and which C pieces remain private runtime/build dependencies; do not expand the public ABI into general C interoperability to accommodate internals. XF core remains a private C implementation behind three B++ ABI procedures. The public boundary remains borrowed/owned `BLValue` and does not expose XF structs, callbacks, variadics, raw handles, or arbitrary native calls.
- [x] Build `xf` as a target-specific static library with ABI/version/dependency metadata. `Libraries/xf/Makefile` builds `libxf.bpp.a` and `libxf.bppabi`, including target, ABI/runtime revision, exports, FNV-1a archive identity, and Linux `m`/`pthread` requirements. The manifest's closed `link TARGET m|pthread|dl` vocabulary adds no raw host-driver input.
- [x] Link it from B++ programs using the B++ resolver/host-link stage. `import library "xf";` with `-L Libraries/xf` resolves the descriptor, validates the archive identity, emits direct `bpp_v1_xf_*` calls, and links XF after the generated program. `make BUILD_MODE=release check-xf-static-lib` builds and runs the consumer fixture. buildFile integration remains stage 5 work.
- [ ] Preserve bindata/reverse-column-index storage behavior and document ownership/lifetime. The initial bridge intentionally does not expose XF tables/maps/RCI data or storage handles.
- [ ] Test ingestion, transformations, table returns, errors, internal runtime hooks, large input and concurrent operations if supported. Current fixture validates manifest resolution, `xf_version`, and a one-shot XF `BEGIN` execution. Stateful execution, errors, ingestion, transformations, table results, large inputs, and concurrent calls remain unverified.
- [ ] Build/test the library on macOS ARM64, Linux x86-64/ARM64 and Windows x86-64. Linux x86-64 core build/execution passes locally. Jay’s macOS ARM64 run is required next; Linux ARM64 requires its runner. Windows is not yet ported because the supplied XF core uses POSIX threading/regex/process APIs.

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

**Integration:** implement sanitizer controls and workflows in the debugger later. Bake/Bakefile has no built-in sanitizer configuration or commands.

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
