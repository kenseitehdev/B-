# Native codegen and private boundary revision 1

## Lowering and target conventions

The custom mutable-register IR lowers to textual assembly. There is no runtime
IR dispatcher. ARM64 instruction selection is shared between Mach-O and ELF;
the ELF adapter changes sections, symbol decoration and page relocations.
x86-64 uses separate argument lowering for System V and Windows x64.

Each private procedure has five pointer arguments. It returns through the final
pointer rather than a host aggregate return. These rules are compiler-private.

| Argument | Value | ARM64 | Linux x64 | Windows x64 |
|---|---|---|---|---|
| 0 | Runtime context | x0 | rdi | rcx |
| 1 | Global value cells | x1 | rsi | rdx |
| 2 | Global initialization flags | x2 | rdx | r8 |
| 3 | Borrowed argument cells | x3 | rcx | r9 |
| 4 | Caller-owned output cell | x4 | r8 | caller stack +40 at entry |

The output is initially null. The callee clones arguments into initialized local
parameter cells. A successful return transfers its value and clears that cell;
cleanup releases every remaining physical ownership cell and local. Errors
return null and retain the first source diagnostic. Globals live until program
teardown. The host convention governs calls to C runtime helpers, including
Windows shadow space and stack arguments beyond the fourth argument.

BLValue remains 16 bytes: a 32-bit null/int/object tag at byte 0 and an 8-byte
integer or object pointer at byte 8. Build-time assertions check this layout and
the 112-byte header. The ABI module is a placeholder; it supplies no export
naming, public type layouts, compatibility promise, library ABI, or symbol policy.

## Stack and allocation

The frame header stores runtime/globals/register/local/argument pointers and
initialization flags. Its call-output scratch is at byte 72, virtual-to-cell map
pointer at 80, cell count at 88, saved caller output at 96, and primitive-result
scratch at 104. Value cells follow the header, then local cells, initialization
bytes, padding to 16 bytes, and borrowed argument cells.

Layout metadata supplies parameter count, virtual register count, local count,
maximum call argument count and physical value-cell count, followed by a mapping
for every virtual register. The runtime uses the same map. Cleanup scans physical
cells once, so several disjoint virtual registers cannot double-release an object.

ARM64 saves x29/x30 and x19 through x28 in 96 bytes. x19 is the frame base; x26
is primitive-result scratch; x20/x21/x22/x23/x24/x25/x27/x28 are eight payload
caches. Scratch registers are caller-saved, and x18 is unused. Stack reservation
is 16-byte aligned and probes each 4096-byte step. Internal control transfers
materialize page-relative addresses in x16 before br/blr; inverse conditional
branches skip only the nearby transfer sequence. This avoids ARM64 short branch
range failures. Page-relative addresses retain their architecture range limit.

x86-64 saves rbp/rbx/r12/r13/r14/r15. r12 is the frame base, with rbx/r13/r14/r15
as four payload caches. The allocation includes 64 outgoing bytes, sufficient
for the eight-argument runtime helper under either convention, plus alignment
padding. Linux probes pages and uses rbp-based CFI. Windows invokes chkstk and
emits SEH directives; it does not depend on a guessed Windows guard-page size.
x86-64 frames larger than 2 GiB are rejected; branches/data use the usual signed
relative displacement range. Extreme multi-gigabyte text sections are not supported.

The allocator computes block use-before-definition and iterative live-in/out
bitsets, including backedges. Release is a use followed by a null definition.
Conservative intervals cover register uses/definitions and live block boundaries.
Disjoint intervals reuse ownership cells. Cached register assignment considers
native operand-use weights and boosts backedge blocks. Low-weight intervals can
be evicted to cells when the cache pool is full. Sparse bit iteration visits set bits in operand/liveness sets. There is no dense N-by-N graph;
block bitsets and interval/cell scans still have worst-case time costs.

This is an ownership-cell allocator plus callee-saved payload caching. It does
not yet split intervals, promote locals across loops, use caller-saved payloads,
or remove all stack homes. Save sets remain fixed rather than per-function minimal.

## Fast paths and synchronization

### Static value facts

`value_facts.c` is an optimization-only forward analysis. Its lattice keeps
kind (`unknown`, `null`, `integer`, `string`, `buffer`, `array`, `object`), a
separate optional integer constant, and separate truthiness (`unknown`, false,
true). CFG joins retain identical facts, join differing object subkinds to
`object`, and otherwise become unknown. This is intentionally not a language
type system.

The native emitters use an integer fact only to suppress a redundant tag test;
they use a truth fact only to select a branch or lower `!` directly. They do not
remove initialization checks, checked arithmetic, dynamic helper fallbacks, or
error diagnostics. Null is false, known integer zero/nonzero is false/true, and
all object kinds are true, including empty strings, buffers, and arrays.

Direct user calls invalidate all global facts and return an unknown fact. Local
kind/truth facts survive only because calls cannot change an object's kind; no
contents, ownership, alias, or initialization fact is inferred. The compiler
does not publish interprocedural summaries: recursion, dynamic returns, and
global effects require a proven SCC/effect model first. This conservative rule
is part of the current codegen contract.

Integer constants, arithmetic, comparisons, slot loads/stores and truthiness
lower to instructions. Add/subtract/negate guard signed overflow; ARM64 multiply
checks the high product, while x86-64 checks overflow. Division and remainder
guard zero and minimum-integer divided by -1. A failed guard enters the runtime
for its existing diagnostic or dynamic operation.

Within a block, known integer result kinds omit redundant operand tag checks.
Literal right operands select encodable immediate arithmetic. Kind state resets
at block boundaries; this is not static typing or interprocedural analysis.
The optimizer folds safe integer operations, simplifies constant branches,
removes unreachable blocks/private procedures, propagates integer slot constants
inside a block, and removes unused primitive literal writes/releases.
It preserves effectful operations and potentially failing arithmetic.

Cached fast results write their tags immediately and defer payload stores.
Before a conditional runtime fallback, the emitter flushes the pre-instruction
dirty snapshot: the helper sees the original operands/destination. Old-object
cleanup preserves the new primitive result in scratch. Cache reuse flushes any
old dirty owner. Calls, unconditionally slow operations, and block terminators
flush cells; block entries reload live caches. Runtime helpers and user procedures
preserve the payload caches through recursion. Primitive release clears tags
without clearing unused cell payloads.

Object creation, retention, conversion, buffers, arrays, indexing, builtins,
program startup, dynamic operations, frame initialization and cleanup use the C
runtime bridge. That runtime is still a dependency. Replacing it with B++/native
runtime code needs its own work; native integer lowering alone does not remove it.

## Debugging, unwind and sanitizers

When debug information is enabled, the compiler saves the expanded source and
emits .file/.loc directives. Host assembly produces source line tables. Unix
CFI and Windows SEH describe stack allocation and saved registers. --emit-map
provides procedure labels, local frame offsets, and IR source locations. It is
not a standard debugger variable-location record; source stepping and unwinding
are the current integration boundary. Imports lack original-file source maps.

The compiler and runtime use host ASAN/LSAN/UBSAN or TSAN build modes. Native
integer overflow and runtime bounds checks are explicit. Native global fast
paths call ASAN/TSAN access hooks, including initialization flags. TSAN procedure
entry/exit hooks preserve native user frames in its shadow call stack. Native
stack accesses are not fully ASAN instrumented; there are no stack redzones or
use-after-return instrumentation. A deliberate poisoned-global unit check
verifies the ASAN hook. Windows native sanitizer coverage remains unverified.

## Verification

IR validation checks instruction shapes, ID ranges, terminators and arity.
It does not prove dominance, types, initialization, or ownership. Unit checks
cover checked folding/unreachable blocks, CFG joins/backedges, cache pressure,
4096 sequential values sharing a cell, mapped-cell cleanup, borrowed calls,
nested object return transfer and error cleanup. Execution tests cover recursive
pressure, more than eight user parameters, dynamic kind joins, large frames,
objects, aliases and runtime errors. Seeded differential tests compare optimized
and baseline native output against C on the same host.

The Linux Windows-convention test compiles bridge entry points and callbacks
with ms_abi and executes the real Windows-emitted x64 instructions after changing
only object-format/unwind directives for ELF. Its chkstk stub is a Linux-only
adaptation. It validates call/stack/register behavior, not SEH execution, Windows
startup, guard pages, CRT behavior, or PE linking. Separate COFF assembly checks
validate directives and relocations. Native Windows CI remains the OS gate.

External Clang/LLVM is used to assemble test outputs; it does not select B++
native instructions. The host assembler/linker dependency remains until the
later assembler/linker roadmap stages.

## References

- [Arm AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)
- [System V x86-64 ABI](https://gitlab.com/x86-psABIs/x86-64-ABI)
- [Windows x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention)
- [Windows x64 prologue/epilogue](https://learn.microsoft.com/en-us/cpp/build/prolog-and-epilog)
