# B++ hand-rolled IR lowering

The IR layer provides an owned register IR, AST-to-IR lowering, a structural
validator, and a readable dump. It uses the exact `BLNode` and `BLTokenKind`
layouts from the supplied parser and lexer headers. The native integration is documented in README.md.

## Instruction format

IDs are size_t, with BL_IR_NONE as the unused sentinel. Register IDs and local
slot IDs are function-specific. Block IDs are also function-specific. Global
slot and function IDs are module-specific. Function 0 is the module entry;
all other function IDs are direct user-call targets.

| Opcode | dst | a | b | c / other |
|---|---|---|---|---|
| null | result | — | — | — |
| int | result | — | — | integer field |
| string | result | — | — | owned text field |
| load.local/global | result | slot | — | — |
| init.local/global | — | slot | value register | — |
| store.local/global | — | slot | value register | — |
| neg, not, to.string.buffer | result | input register | — | — |
| arithmetic, comparison, index.get | result | left/object register | right/index register | — |
| index.set | — | object register | index register | value register |
| call | result | function ID, or NONE for builtin | — | args array; text identifies builtin |
| release | — | register | — | — |
| free.local/global, delete.local/global | — | slot | — | — |
| jump | — | target block | — | — |
| branch | — | condition register | true block | false block |
| return | — | return register | — | — |

Every block ends in a terminator. Lowering after a return stops for that block.
An if whose two arms return can leave an unreachable merge block with a null
return; this is valid and can be eliminated by a later optimization pass.
The validator checks opcode shapes, ID ranges, call arity, and terminators.
It is not a dominance, definite-assignment, type, or ownership proof.

## Backend and runtime contract

Use BLValue as the value representation for the initial backend. This preserves
the VM's dynamic `+`, object equality, string conversions, and truthiness while
making control flow compiled. Registers are mutable frame locations, not SSA.
Loops execute their register definitions repeatedly.

- Initialize all frame registers to BLValue null. All result-producing writes
  release any old destination value before storing the newly owned value.
- Keep an initialized flag for each local/global slot. Parameters start
  initialized; all other slots start uninitialized. `init` releases the previous
  value if present, clones its input, and marks the slot initialized. `store`
  requires an initialized slot and clones its input before releasing the old
  slot value. `load` requires an initialized slot and returns a clone.
- Uninitialized slot access must report an error using the instruction's line
  and column. A declared null value differs from an uninitialized slot.
- Arithmetic, comparisons, indexing, and conversions borrow operands. Each
  result operation returns an owned BLValue. `add` must preserve integer
  addition versus the existing string/buffer concatenation behavior.
- Preserve buffer-only indexing, integer indices, bounds checks, byte masking
  on index assignment, and integer comparison restrictions from the VM.
- `to.string.buffer` preserves the existing String declaration conversion:
  buffer clone, string-to-buffer copy, integer decimal conversion, otherwise an
  empty C-string buffer.
- Calls borrow argument registers; the caller emits releases after the call.
  User functions clone arguments into their parameter slots. Call results are
  owned. Builtin aliases are canonicalized to existing builtin names.
- `release` releases the register value and sets it to null.
- `branch` inspects truthiness without consuming the value. Each successor
  releases the condition before executing its statements.
- `return` transfers ownership of its register to the caller, clears that
  register, then releases all remaining frame registers and initialized locals.
  Runtime errors perform the same cleanup without returning an owned result.
  Globals live across user calls and are cleaned up at program teardown.
- `delete` releases a slot's value and replaces it with initialized null.
  `free` invalidates the shared buffer/array payload, preserving alias-visible
  invalidation; it must release array elements before releasing their storage.
  Honor payload ownership flags when deciding whether to free external storage.
- Preserve runtime diagnostics rather than compiling unsafe C arithmetic.
  Decide and enforce signed-overflow semantics, including negating LLONG_MIN
  and dividing/modulo LLONG_MIN by -1; the old VM has undefined C behavior here.

The pasted VM's array_grow shrinks before releasing truncated values, accessing
freed/out-of-range storage. Release removed elements before reallocating. Its
array free also omits releases of contained values. Fix both during extraction.
The value implementation is needed to verify the exact ownership rules.

## Explicit language changes from the VM

These rules are intentional in this first compiler lowering:

1. Direct top-level functions are declared before bodies are lowered. Forward
   calls and recursion work. The VM registered functions only when execution
   reached their definition. Definitions remain known even after a top-level
   return; compiled declarations are not executable statements.
2. Function definitions inside blocks, branches, loops, or functions are
   rejected. The VM registered them dynamically in a global function table.
3. Duplicate variable declarations within one function/module, duplicate
   parameters/functions, and functions named after builtins are rejected.
   The VM accumulated duplicate variables and looked up the first matching
   variable; it replaced duplicate function entries.
4. Locals are resolved for the whole function and shadow globals throughout
   that function. Blocks introduce no scope, matching the supplied VM. Before
   a local declaration executes, accessing that local reports uninitialized
   access rather than falling back to a same-named global. Re-executing a
   declaration in a loop reinitializes its one slot.
5. Unknown variables/functions and user-call arity errors are diagnosed at
   compilation, even if the offending expression is on an untaken branch.
   Statements after a return are not lowered, though declarations are collected.

