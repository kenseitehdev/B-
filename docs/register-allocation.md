# Register inventory and payload allocation

This document describes the native backends' current **selective payload cache**. It is not a general machine-register allocator: IR tags and ownership state remain in canonical memory cells, and only integer-sized payloads can occupy physical registers. The source of target register data is `src/registers.c`; allocation uses `src/regalloc.c`; frame saves and reloads are emitted by the corresponding native emitter.

## Register metadata contract

Each `BPPRegisterInfo` reports an architecture-local ID, assembler name, class mask, width, role flags, save cost, and call-clobber mask. IDs are stable only within an ISA. Floating-point and vector names can be aliases of the same physical register bank (ARM64 `vN`, x86-64 `xmmN`); the class mask records both uses. The allocator asks for the integer candidate pool and currently rejects other classes.

`BPP_REG_CALLER_SAVED` and `BPP_REG_CALLEE_SAVED` describe the selected platform ABI. `BPP_REG_FRAME`, `BPP_REG_SCRATCH`, `BPP_REG_ARGUMENT`, and `BPP_REG_RETURN` describe additional roles and may coexist with a save class. `BPP_REG_ALLOCATABLE` means the register is permitted in the payload pool after the emitter's fixed roles are excluded. `call_clobbers` is a bit set over runtime, B++ user, and external calls; ABI volatile registers are marked clobbered by all three. Callee-saved registers have no call clobber bits. `save_cost` is a relative allocator cost, not a cycle estimate: volatile integer registers cost 0, selected callee-saved integer registers cost 3, and mandatory frame registers are not candidates.

The `--target-info TARGET` output prints every inventory record. `--list-targets` retains a final count column for compatibility; it is the number of integer payload candidates exposed to this allocator, not the architectural register count.

## Target inventories and current allocation pool

| Target | Integer caller-saved candidates | Integer callee-saved candidates | Payload pool order | Floating/vector inventory |
|---|---|---|---|---|
| macOS ARM64 | `x13–x15` | `x20–x25`, `x27–x28` | `x20,x21,x22,x23,x24,x25,x27,x28,x13,x14,x15` | `v0–v7`, `v16–v31` volatile; `v8–v15` callee-saved (low 64 bits under AAPCS64) |
| Linux ARM64 | `x13–x15` | `x20–x25`, `x27–x28` | same ARM64 pool | same AAPCS64 vector rules |
| Linux x86-64 / System V | `r8–r9` | `rbx`, `r13–r15` | `rbx,r13,r14,r15,r8,r9` | `xmm0–xmm15` volatile |
| Windows x86-64 | `r8–r9` | `rbx`, `r13–r15` | `rbx,r13,r14,r15,r8,r9` | `xmm0–xmm5` volatile; `xmm6–xmm15` nonvolatile |

The complete GPR inventories also include argument/return registers, reserved registers, and scratch registers. On ARM64, `x0–x7` are argument registers, `x0` is the return register, `x8` carries an indirect-result address, `x9–x12` and `x16–x17` are emitter scratch/platform temporaries, `x13–x15` are caller-saved payload candidates, `x18` is reserved as the platform register, `x19` is the runtime-frame register, `x26` is emitter scratch, `x29` is the frame pointer, and `x30` is the caller-clobbered link register. `sp`/`xzr` are the architectural `x31` aliases and are not general payload registers. `x20–x22` and `x24` stage initial arguments before the runtime frame is established; after that setup the payload allocator may use them.

On x86-64, `rsp` is the stack pointer, `rbp` anchors the generated function frame, and `r12` is the B++ runtime-frame pointer. `rax`, `r10`, and `r11` are reserved emitter scratch/return registers. System V argument registers are `rdi,rsi,rdx,rcx,r8,r9`; Windows uses `rcx,rdx,r8,r9` and the caller-provided 32-byte shadow area. The target inventory records ABI-specific volatility, including Windows' nonvolatile `rsi/rdi` and `xmm6–xmm15`; these are currently not candidates because the emitter does not need them for the payload cache.

## Allocation and calls

The pool lists callee-saved registers first so a constrained test or future target policy can request the safe prefix. The allocator splits intervals immediately after unconditional helper operations (strings, indexing, conversions, calls, free, and delete). Each segment competes for candidates independently. That lets a value use a register in one segment and the canonical spill cell in another. Within a segment, caller-saved candidates are rejected if a possible helper/call clobber or a CFG boundary falls inside it. Conditional slow paths keep the same mapping across the fast/slow merge; the emitter flushes their pre-instruction dirty snapshot before the helper. Caller-saved registers are used where the segment is short and safe.

## Deterministic allocation costs

Allocation is deterministic and does not consume runtime profile data. The planner computes
dominators for the reachable CFG, identifies a backedge only when its destination dominates
its source, and forms one natural-loop membership set per header. A block’s static execution
weight is 8 outside loops and is multiplied by `1 + 4 × loop-depth` inside nested natural
loops. For a two-way branch, a dominance backedge is statically 88% hot; otherwise a
successor entering a deeper natural loop is 75% hot. Equal-depth branches remain 50/50.
One bounded propagation pass adds one quarter of a block's weight to its preferred successor.
This reconciles layout/loop structure with allocation without solving an unbounded cyclic
frequency equation. Exceptional helper/error paths never become a reason to discard their
required canonical homes.

Native payload uses have fixed operation costs: division/modulo 18, multiplication 12,
addition/subtraction 10, comparisons 8, unary operations 6, branch tests 5, and local/global
stores 4. The segment priority is the sum of operation cost times its block’s static weight.
Each native payload use also accounts for two units of avoided tag checking and four units of
payload traffic. Helper-visible instructions use a static path-class cost: user calls 32,
builtins 24, generic helpers 8, and possible runtime-error, return-cleanup, and sanitizer
boundaries add 4, 4, and 3 respectively. A safely rematerializable operand costs three units
at every native use, and is intentionally kept out of the payload cache: its immediate is
recreated at the use site. Helpers and rematerializable values retain their existing correctness
rules: these costs explain plans and prioritize native payload traffic; they do not permit an
omitted writeback or an effectful recomputation.
Candidates subtract eight times their documented callee-save cost; caller-saved candidates
remain available only to a segment with no possible clobber or CFG crossing. This gives short
safe spans the zero-save-cost caller registers while making long loop-heavy spans compete by
their expected native fast-path value.

`--emit-map` exports each block’s `loop_depth` and `static_weight`, plus an
`allocation_metrics` object. The metrics report natural-loop headers, weighted fast-path cost,
helper/call boundaries, selected register/spill segments, a dry-run legacy baseline, and
conservative estimated cache hits, reload sites, payload writebacks, and writeback bytes. The
baseline reruns the former numeric-backedge priority policy against the same segments and
target eligibility rules; it never changes generated code. These are diagnostic allocation
estimates, not benchmark measurements. For generated-code accounting, `--emit-asm
--asm-comments` ends every procedure with the exact static number of emitted payload spill
stores, reloads, and safe rematerializations for that target. Those totals count assembly in
the output, not dynamic executions along a runtime path. Optional profile input and measured
allocator CPU time are also reported. The map additionally reports `workspace_peak_bytes`
(the deterministic allocator-owned preflight estimate, not process RSS),
`sparse_use_entries`, and live-range piece/hole counts.

### Optional allocation profiles

`--alloc-profile FILE` is deliberately optional. Without it, the plan is determined solely by
the source, target, and static model above. The profile is plaintext: each non-comment line is
`function-name block-index positive-multiplier`; `*` matches every function. It multiplies a
block's static weight before the bounded hot-successor propagation pass. Later matching entries
override earlier entries. Invalid input is a compile-time diagnostic. The map records the number
of `profiled_blocks`, so a profile cannot silently become a required build input.

## Scalability guardrails

The allocator performs an overflow-checked preflight of its remaining dense work tables before
allocating them. Per-instruction operands are held in one compact sparse use table rather than
an instruction-by-vreg visibility bitmap. The current supported planning workspace is
**512 MiB** per function.
The estimate includes virtual-register arrays, block and instruction bitsets, local
liveness, CFG dominance/worklist structures, allocation segments, and allocator
scratch space. An over-limit request fails cleanly; the native emitters and
`--emit-map` report the procedure name plus the allocator reason instead of emitting
partial output.

Virtual-register liveness uses a reverse predecessor worklist. A block re-enters the
queue only when its live-in set changes, and pending entries are de-duplicated. The
location map exposes `liveness_worklist_pops`, `liveness_updates`, allocator CPU time, and
the preflight workspace peak so generated CFG fixtures can assert convergence and memory
behavior. The checked workspace ceiling is the supported compiler-function-size contract: a
function is supported when its deterministic preflight is at most 512 MiB; larger requests are
rejected before allocation with `register allocator workspace exceeds 512 MiB limit`. This
revision exercises 16,384 straight-line virtual values, 4,096 CFG blocks, and
4,096-register loop/branch/call fixtures.

Canonical ownership-cell reuse is `O(n log n)` after the existing interval sort:
an active min-heap is ordered by interval end and moves expired cells to a second
min-heap ordered by cell number. The allocator therefore preserves deterministic
lowest-numbered-cell reuse without the old scan over every allocated cell for each
new interval. Canonical-cell reuse intentionally stays conservative over a value's
enclosing span, which keeps cleanup/visibility behavior stable. Payload allocation is
not: range construction records exact active instruction pieces from definitions,
sparse uses, and live-after sets, so a virtual value can have a hole through a
mutually exclusive branch arm and its remaining pieces compete independently for
cache registers.

The allocator records instruction-level live-after sets, each instruction's borrowed operands and destination, and explicit helper, user-call, builtin, runtime-error, return-cleanup, and sanitizer boundary flags. At a helper point, the emitters write only dirty payloads needed after the point, consumed by the helper, or potentially owning an object that cleanup must release. A value whose final language use occurs at the helper is included in the operand set. Integer/null immediates and safely rematerializable integers do not create dirty payloads. Generated B++ procedures preserve callee-saved payloads by the platform ABI. On a conditional slow path, the emitter uses the pre-instruction dirty snapshot so stores are emitted only along that path.

Block entries no longer preload every live-in payload. Each emitter tracks which virtual value currently occupies each physical cache register and emits a cell-to-register reload at the first actual native payload use. Repeated uses of that value in the same mapping do not reload it again. A helper-defined destination is reloaded when its next allocation segment uses a register; if the helper's result is immediately consumed, the helper return is its definition point. Some helper joins still reload after the call where a path merge requires a common register state; there is no profile-guided reload hoisting.

Control-flow writeback is successor-specific. A jump writes the values live on its edge; a conditional branch emits two edge stubs, each writing only its successor's live-in values plus possible object roots needed by frame cleanup. These stubs split critical assembly edges without mutating the verified IR CFG. The allocator now computes a must-cache state for reachable block entries: a dirty value can cross an edge without a store only when every reachable predecessor carries that value in the same physical register and the successor begins with that allocation. Matching states are restored at the target block; conflicting or helper-cleared states are written to canonical cells on only the edges that need it. Dead values still produce no stores. Native sanitizer-instrumented output disables this preservation and uses canonical writeback around callbacks. The flow state is represented as virtual-value-to-physical-owner pairs, so allocation segment changes remain explicit.

The allocator verifier checks error and return-cleanup points: every value live afterward, visible to the helper (including arguments and destinations), or potentially owning an object across that point must have a canonical cell location. Tags and ownership state always remain in the cell; only integer payloads can be marked dirty by the emitter. The runtime cleanup-error and recursive-cleanup tests exercise actual failure edges.

Spill reasons are queryable in `--emit-map`. A spilled allocation segment identifies whether it had no native payload uses, no available candidate register, was excluded by call clobbers, or lost to register pressure. Per-instruction spill candidates identify the vreg, physical register, canonical cell, and boundary reason. With `--asm-comments`, the emitted assembly records actual stores and reloads, including whether each reload serves the first payload use, a helper-defined result, or an allocation-segment transition.

Each used virtual value has a reusable canonical cell. Register assignment is a cache on top of that cell, not a transfer of ownership. Spill victims, helper-visible immediates, and other noncached values use their cell. Possible owners are partitioned into a contiguous root prefix so cleanup visits only cells that may own an object; ordinary cells remain available to helpers without entering the release scan.

## Native frame layout and slot reuse

Native codegen's private frame contract is revision 2. The 128-byte header is followed by a contiguous root prefix, ordinary canonical value/spill cells, physical local-value cells, local initialization bytes, alignment padding, and an outgoing `BLValue` argument area. The root prefix contains each cell that can hold a dynamic/owned value; cleanup releases that prefix once and then releases each physical local slot once. Primitive-only cells still carry canonical tags/payloads for helper visibility but are not scanned for ownership cleanup. There is no tracing collector in this runtime.

Outgoing argument cells are reserved to the maximum call arity in the function and reused by every call. The shared `bpp_native_frame_layout` calculation drives both emitters and `--emit-map`; its tests assert every region offset/size, outgoing bytes, alignment padding, and overflow rejection. The map reports `frame_data_bytes`, saved-register bytes, the emitter's stack allocation, and total `frame_bytes` including saves and the ARM64 frame record. On Windows x64 the 32-byte shadow area is a separate caller ABI reserve; it is not included in B++'s argument-value cells. ARM64 rounds its save area and frame data to 16 bytes. x86-64 chooses its frame padding from save-set parity so `rsp` is aligned at every call.

Local-value cells and initialization flags share the same physical local map. CFG-wide reuse uses forward definite-initialization with predecessor intersection and backward liveness across reachable blocks. A non-parameter local can share only when every reachable access is preceded by a guaranteed `INIT_LOCAL`; parameters begin initialized. Liveness is conservatively projected to linear spans, allowing reuse across block boundaries and diamond joins while retaining distinct slots for path-conditional initialization. Initialization into a shared slot uses the runtime store path, which releases the previous owner. The linear projection can miss reuse between mutually exclusive ranges. Identity register/local maps are omitted from emitted metadata; non-identity maps are emitted explicitly. The map report records each local's physical slot and byte offsets.

Unused pure, infallible values have no canonical cell and their native definition instruction is omitted. The allocator prunes dead chains of integer/null literals, `NOT`, equality/inequality, and integer expressions it has already proven safe to rematerialize. It retains effectful producers and checked operations whose runtime error behavior must remain observable. Eliminated instructions have no source location entry because debug variable/location tracking is not implemented.

## Tag/payload coherence and allocation classes

Tags and ownership bits remain memory-resident and authoritative. A cached physical register mirrors only the 64-bit payload. When an operation replaces an object value, the emitter performs the old-object destination cleanup before writing the new tag and payload. Dirty primitive payloads are written to the canonical cell before any helper or edge that can inspect or clean the value; helper-produced object payloads are written to the cell by the runtime helper. Reloads never infer a tag from a payload register.

`BPPRegPlan.alloc_class` distinguishes dead values, canonical tagged pairs, integer immediates, null immediates, and proven-safe rematerializable integer expressions. A dynamic tagged value is treated as one ownership unit: its tag and ownership state stay canonical in the value cell while its payload may be cached. The allocator never allocates an integer/null literal or a rematerializable expression to a payload register.

Used immediate-class literals still receive canonical cells and their payload is written there. Helper fallbacks, error cleanup, branches, and return cleanup can inspect the canonical value frame. Dead pure results and their unused pure dependencies are omitted. `BPPRegLocation` reports register, spill, constant, rematerializable, or dead at an instruction position; the map exporter includes allocation segments, position numbers, and an `optimized_out` flag. This location model does not yet remove homes for effectful values whose results are unused or select immediate operands.

The allocator marks same-block integer expressions as rematerializable only when their inputs are known integer constants and checked evaluation succeeds: negation, logical not, integer arithmetic, division/modulo, and comparisons. Overflowing arithmetic, division by zero, `INT64_MIN / -1`, dynamic inputs, and operations that can call helpers or allocate objects remain dynamic. The computed integer is retained as allocator metadata and exposed as a rematerializable location. The emitter still emits the original checked operation and canonical value write, but recreates the known immediate rather than loading its payload cell at each native use.

## Per-function saves and stack invariants

Each function saves mandatory frame/runtime registers plus only the callee-saved payload registers present in its allocation plan. `bpp_reg_saved_set` derives this list from physical assignments; allocation tests compare the result with actual assignments for zero, exact-capacity, and overflowing pressure. ARM64 stores the save set below the `x29/x30` frame record and rounds the save area to 16 bytes. It restores the exact set before restoring `x29/x30`. Caller-saved payload registers require no prologue save.

x86-64 pushes `rbp` and `r12` for every generated procedure, followed by the selected callee-saved payload registers, and pops them in reverse order. The outgoing frame size is a multiple of 16 bytes plus 72 or 80 bytes depending on save-count parity. Since function entry has `rsp mod 16 = 8`, this makes `rsp` 16-byte aligned before every helper or generated-procedure call. The call reserve includes the native 128-byte runtime frame beginning at `r12 = rsp + 64`, Windows shadow space where required, and the incoming fifth-argument stash. The SysV fifth argument is stashed just below the pushed save set; the Windows fifth argument is addressed above the dynamic save and allocation areas. Large frames retain the existing stack-probe path.

This design preserves scratch ownership: allocated registers never overlap the documented scratch set, and x86-64 register names come from the target inventory rather than a second allocator pool table. The ARM64 emitter uses architecture register IDs directly after allocation. Tests exercise values across runtime and user calls, recursive calls, checked operations with error/helper paths, joins, and loop backedges. `check-targets` cross-assembles generated code for all four targets; `check-msabi` executes the Windows x64 calling convention on Linux x86-64; the ordinary native suite executes on the current host. CI includes architecture-checked native runners for macOS ARM64, Linux x86-64, Linux ARM64, and Windows x86-64. Cross-assembly and the Linux Windows-ABI bridge do not replace native Windows execution. This local Linux x86-64 revision passed release, ASAN/UBSAN debug with leak scanning disabled, and TSAN suites; native ARM64 and Windows pass logs are still required to close platform verification.

## Current limits

- Float/vector registers are inventoried but not allocated.
- Used integer and null literals are explicitly classified as immediates but retain canonical stack cells. Dead pure expressions can be omitted; effectful or potentially failing operations retain their required result homes. Immediate operands and broader elimination for unused-but-effectful results are not implemented.
- The allocator marks safe same-block integer expressions with checked constant inputs and stores their result as metadata. Native payload uses recreate the immediate instead of consuming a cache register; checked failures, dynamic values, and effectful operations are excluded, and used values retain canonical cells.
- The allocator splits only at unconditional helper boundaries. It does not yet split arbitrary ranges by hot/cold CFG regions or profile data, and it retains one canonical ownership cell for every IR value.
- Live ranges remain conservative across CFG joins and loop backedges. They are not a full interference graph or a general spill-placement solution.
- Reload placement at helper joins is not fully lazy, and dirty-state merging is per virtual value rather than per physical location and CFG edge.
- Tags and object ownership remain memory-based. Every used value has a reusable canonical cell; unused pure immediates have none. Only possible-owner cells enter the release scan, while primitive-only cells remain available to helpers as ordinary spills.
- Platform assembly and ABI execution tests are required for each host/toolchain. Linux x86-64 and Windows x64 ABI execution are covered where their test environments are available; macOS ARM64 and Linux ARM64 still need native-host execution runs. Cross-assembly checks validate syntax and unwind directives but do not replace running on each target OS.
