# B++ IR and backend contract (revision 1)

This document defines the contract between IR lowering, IR transformations, and
the C, ARM64, and x86-64 backends. It describes the compiler's current
revision-1 IR and private runtime boundary. It is not the future public static
library ABI.

## Common rules

Numeric behavior is specified once in [`numeric-semantics.md`](numeric-semantics.md).
The arithmetic opcode rows below describe which operation is performed and
where failures are reported; they defer exact integer and conversion semantics
to that document.

- Each function has numbered basic blocks. Block 0 is the entry. A block is a
  straight-line instruction sequence ending in exactly one `jump`, `branch`,
  or `return`. No instruction may follow its terminator.
- Virtual registers are function-local identifiers. A result-producing
  instruction defines its destination; no other opcode may define a
  destination. Each statically present destination has at most one defining
  instruction. A definition must reach every use along the CFG. Revision 1 has
  no phi instruction, so a value defined on only one side of a merge cannot be
  used after that merge.
- A produced `BLValue` is owned by its destination register. Re-executing a
  definition in a loop replaces the register's previous value, releasing that
  old value first. An input operand is borrowed unless the opcode says it
  consumes ownership. `release` consumes a live register and leaves it null.
  A register cannot be used after a release on any path reaching that use.
- Local/global stores and procedure/builtin arguments borrow their register
  operands; the store/callee obtains its own stored/result ownership as
  required. Calls do not implicitly release argument registers. The result of
  every result-producing call is newly owned by its destination. `return`
  transfers the selected register value to the caller; backend cleanup releases
  every other live temporary and local.
- Runtime-visible failures set the current runtime error and stop the current
  procedure. They do not unwind through arbitrary foreign frames. Each
  instruction carries a 1-based source line and column. Runtime helpers receive
  that site for diagnostics; synthetic instructions inherit their originating
  AST location where one exists, otherwise use the documented 1:1 fallback.
- Native backends may emit a fast path only when it preserves the runtime
  semantics below. A failed fast-path guard branches to a runtime helper with
  the same operands and source site. “Native” means emitted machine
  instructions/control flow; “runtime” means a call to the private B++ runtime
  helper. The C emitter is the semantic/reference backend and expresses value
  operations through the runtime.
- `BL_IR_CALL` with `a != BL_IR_NONE` is a direct B++ procedure call. With
  `a == BL_IR_NONE`, `text` names a runtime builtin. Indirect calls are not
  representable in revision 1.

## Opcode contract

| Opcode | Operands and result | Ownership | Errors and source site | Native/runtime lowering |
|---|---|---|---|---|
| `NULL` | No operands; defines `dst` as null. | `dst` owns the null value. | Cannot fail. | Native immediate/tag write; C reference backend constructs null. |
| `INT` | `integer`; defines `dst`. | `dst` owns the integer. | Cannot fail. | Native immediate/tag write; C reference backend constructs integer. |
| `STRING` | `text`; defines `dst`. | `dst` owns a newly allocated string value. | Allocation failure follows runtime allocation policy; instruction site is retained. | Private runtime string-copy helper on native backends; C reference backend uses the same value constructor. |
| `LOAD_LOCAL` | Local slot `a`; defines `dst`. | `dst` owns a clone of the initialized slot value. | Uninitialized local is a runtime error at this instruction. | Native integer fast path; runtime fallback for null/object values and checks; C reference backend uses runtime load. |
| `STORE_LOCAL` | Slot `a`, borrowed value `b`. | Slot receives its own stored value; `b` remains owned by its register. | Writing an uninitialized slot is a runtime error at this site. | Native integer fast path; runtime fallback; C reference backend uses runtime store. |
| `LOAD_GLOBAL` | Global slot `a`; defines `dst`. | `dst` owns a clone of the initialized slot value. | Uninitialized global is a runtime error at this instruction. | Native integer fast path; runtime fallback for null/object values and checks, plus optional sanitizer access hook; C reference backend uses runtime load. |
| `STORE_GLOBAL` | Slot `a`, borrowed value `b`. | Global receives its own stored value; `b` remains owned. | Writing an uninitialized global is a runtime error at this site. | Native integer fast path; runtime fallback and optional sanitizer access hook; C reference backend uses runtime store. |
| `INIT_LOCAL` | Slot `a`, borrowed initializer `b`. | Establishes/replaces the slot value with its own copy; does not consume `b`. | No recoverable language error in the current runtime; allocation failure follows runtime allocation policy. | Native integer fast path; runtime fallback; C reference backend uses runtime store in initialize mode. |
| `INIT_GLOBAL` | Slot `a`, borrowed initializer `b`. | Establishes/replaces the global value with its own copy; does not consume `b`. | No recoverable language error in the current runtime; allocation failure follows runtime allocation policy. | Native integer fast path; runtime fallback and optional sanitizer access hook; C reference backend uses runtime store in initialize mode. |
| `NEG` | Integer-like operand `a`; defines `dst`. | Borrows `a`; `dst` owns the result. | Non-integer and signed overflow are errors at this site. | Native checked integer fast path; runtime fallback; C reference backend uses runtime unary operation. |
| `NOT` | Truthiness operand `a`; defines integer `dst` (0 or 1). | Borrows `a`; `dst` owns an integer. | Current truthiness rules do not fail. | Native null/integer fast path and runtime fallback for remaining tags; C reference backend uses runtime unary operation. |
| `ADD` | `a`, `b`; defines `dst`. | Borrows both; `dst` owns integer sum or concatenated string/buffer result. | Integer overflow and unsupported operand combinations are errors at this site. | Native checked integer fast path; runtime fallback handles dynamic addition/concatenation; C reference backend uses runtime binary operation. |
| `SUB` | Integer operands `a`, `b`; defines `dst`. | Borrows both; `dst` owns integer result. | Non-integers and signed overflow are errors at this site. | Native checked integer fast path; runtime fallback; C reference backend uses runtime binary operation. |
| `MUL` | Integer operands `a`, `b`; defines `dst`. | Borrows both; `dst` owns integer result. | Non-integers and signed overflow are errors at this site. | Native checked integer fast path; runtime fallback; C reference backend uses runtime binary operation. |
| `DIV` | Integer operands `a`, `b`; defines `dst`. | Borrows both; `dst` owns integer quotient. | Non-integers, zero divisor, and `LLONG_MIN / -1` are errors at this site. | Native checked integer fast path; runtime fallback; C reference backend uses runtime binary operation. |
| `MOD` | Integer operands `a`, `b`; defines `dst`. | Borrows both; `dst` owns integer remainder. | Non-integers, zero divisor, and `LLONG_MIN % -1` are errors at this site. | Native checked integer fast path; runtime fallback; C reference backend uses runtime binary operation. |
| `EQ` | Values `a`, `b`; defines integer `dst`. | Borrows both; `dst` owns 0 or 1. | Uses B++ value equality; no language error is currently raised. | Native integer fast path; runtime fallback for general values; C reference backend uses runtime equality. |
| `NE` | Values `a`, `b`; defines integer `dst`. | Borrows both; `dst` owns 0 or 1. | Logical inverse of B++ value equality; no language error is currently raised. | Native integer fast path; runtime fallback for general values; C reference backend uses runtime inequality. |
| `LT` | Integer operands `a`, `b`; defines integer `dst`. | Borrows both; `dst` owns 0 or 1. | Non-integers are errors at this site. | Native checked integer comparison; runtime fallback; C reference backend uses runtime binary operation. |
| `LE` | Integer operands `a`, `b`; defines integer `dst`. | Borrows both; `dst` owns 0 or 1. | Non-integers are errors at this site. | Native checked integer comparison; runtime fallback; C reference backend uses runtime binary operation. |
| `GT` | Integer operands `a`, `b`; defines integer `dst`. | Borrows both; `dst` owns 0 or 1. | Non-integers are errors at this site. | Native checked integer comparison; runtime fallback; C reference backend uses runtime binary operation. |
| `GE` | Integer operands `a`, `b`; defines integer `dst`. | Borrows both; `dst` owns 0 or 1. | Non-integers are errors at this site. | Native checked integer comparison; runtime fallback; C reference backend uses runtime binary operation. |
| `INDEX_GET` | Buffer value `a`, integer index `b`; defines `dst`. | Borrows object/index; `dst` owns the loaded integer byte. | Wrong operand types and out-of-bounds index are errors at this site. | Private runtime helper; no direct native implementation yet. |
| `INDEX_SET` | Buffer `a`, integer index `b`, integer byte value `c`; no result. | Borrows all operands; mutates the buffer. | Wrong operand types and out-of-bounds index are errors at this site. | Private runtime helper; no direct native implementation yet. |
| `TO_STRING_BUFFER` | Value `a`; defines `dst`. | Borrows `a`; `dst` owns a new buffer representation. | Conversion is total for current values (unsupported values become an empty buffer); allocation failure follows runtime policy. The source site stays attached to the IR instruction but is not currently used for a language diagnostic. | Private runtime value conversion helper; C reference backend uses the runtime conversion helper. |
| `CALL` | Direct function id in `a`, or builtin name in `text`; argument register vector; defines `dst`. | Arguments are borrowed for the duration of the call; result is owned by `dst`. | Procedure argument count is verified statically. Builtin type/arity errors are runtime errors at the call site. Errors from user procedures propagate through the shared runtime state. | User procedure is a direct native call using the compiler-private frame/call convention; builtin is a private runtime call. C reference backend calls procedure/runtime helper functions. |
| `RELEASE` | Owned register `a`; no result. | Consumes the register value and leaves it null. | Invalid, duplicate, or path-inconsistent release is rejected by IR verification; it should not fail at runtime. | Native null/integer fast path; runtime release helper for object values; C reference backend nulls/releases the register. |
| `FREE_LOCAL` | Local slot `a`; no result. | Releases the buffer/array storage owned by the slot and resets its storage. | Uninitialized slot or non-buffer/non-array value is an error at this site. | Private runtime helper. |
| `FREE_GLOBAL` | Global slot `a`; no result. | Releases buffer/array storage owned by the global and resets its storage. | Uninitialized slot or non-buffer/non-array value is an error at this site. | Private runtime helper. |
| `DELETE_LOCAL` | Local slot `a`; no result. | Releases the slot value and stores null while keeping the slot initialized. | Deleting an uninitialized slot is a runtime error at this site. | Private runtime helper. |
| `DELETE_GLOBAL` | Global slot `a`; no result. | Releases the global value and stores null while keeping the global initialized. | Deleting an uninitialized global is a runtime error at this site. | Private runtime helper. |
| `JUMP` | Successor block `a`; no result. | No value effect. | Invalid successor is an IR verifier error, not a runtime error. | Native unconditional branch/fall-through when possible. |
| `BRANCH` | Truthiness register `a`; true successor `b`, false successor `c`. | Borrows the condition; it remains owned until explicit release or function cleanup. | No semantic runtime failure; malformed successors/registers are verifier errors. | Native tag-aware truthiness test and conditional branch. |
| `RETURN` | Register `a`; terminates the function. | Transfers `a` to the caller and releases other live temporaries/locals during frame cleanup. | Runtime error state already set in the procedure determines failure; return itself does not add a new error. | Native epilogue/return; C reference backend transfers then cleans up. |

## Optimization effect model

`bpp_ir_effects()` is the exhaustive optimizer classification for the opcode
table above.  A pass must keep an instruction whenever any listed effect is
observable.  The table is deliberately conservative: `can-error` is an
observable effect, so DCE must not remove an unused expression merely because
its result is unused.

| Effect class | IR operations | DCE rule |
|---|---|---|
| None | `NULL`, `INT`, `NOT`, `EQ`, `NE` | May be removed if no retained instruction needs `dst`. |
| Read | Local/global loads; buffer/array reads | Retain: loads can diagnose and may clone an owned value. |
| Write/mutation | Local/global init/store, index write, free/delete | Retain except for a separately proven redundant primitive store; initialization transitions are never generally dead. |
| Allocation/output/call | String conversion/construction and calls | Retain. Calls conservatively include output, global/object mutation, allocation, ownership, and failure. |
| Ownership | Release/free/delete/load/store/call | Retain unless the released value is proven primitive, so its release is a no-op. |
| Can error | Dynamic arithmetic, loads/stores, indexing, calls, frees/deletes, allocation | Retain even when `dst` is dead, preserving the original diagnostic and source site. |
| Control | `JUMP`, `BRANCH`, `RETURN` | Always retain; CFG cleanup may remove only blocks proven unreachable and remaps all surviving branch targets. |

Successful `NEG`, integer `SUB`/`MUL`/`DIV`/`MOD`, integer comparisons, and
`INDEX_GET` have primitive results, but are not pure: their type, overflow,
zero-division, or bounds errors keep them live.  `ADD` is not primitive because
it may return a string/buffer.  Debug/sanitizer observability barriers remain
effects and prevent access-elision passes from hiding source accesses.

Revision 1 has only private direct procedures.  Procedure reachability begins
at the module entry and follows direct calls; unreachable private procedures
are removed and surviving direct-call IDs are remapped.  Once a public ABI,
export, address-taking, callback, or static-library model exists, those
procedures must be treated as DCE roots before this pruning runs.

## Verifier requirements

Every validation failure must identify the validation stage, function, block,
instruction when available, and source location when the instruction has one.
Validation checks:

1. Opcode range, required/forbidden fields, destination shape, local/global
   indices, register bounds, call target and procedure arity.
2. Exactly one terminator at each block end, no earlier terminator, and valid
   jump/branch successors.
3. Every register operand has a definition, definitions are unique, and data
   flow reaches uses without crossing an invalid path. Revision 1 has no phi
   values or implicit cross-block value merges.
4. Ownership at every use. A use requires a live owned register on every path;
   a release must be reached with a live register and changes it to released.
   The verifier checks unreachable CFG components as well as the entry CFG.
5. After allocation, every virtual register's cell-map entry is below
   `cell_count`, payload-register identifiers are supported, and the plan's
   register/block dimensions and liveness-map sizes match the function.

Run the verifier immediately before and after each IR transformation pass.
Pass names are supplied by the driver so a failure distinguishes input IR,
constant/CFG simplification, procedure elimination, and future passes.

## Adding or changing an opcode

Before a new opcode is considered implemented, update this table and the opcode
enum/name table, constructor/lowering, verifier shape/use/ownership rules, IR
dump, optimizer operand/effect model, register allocator liveness/type model,
all emitters, runtime helpers if needed, and focused positive/negative tests.
An emitter must fail explicitly for an opcode it does not implement; it must
not silently emit a placeholder instruction.
