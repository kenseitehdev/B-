# B++ numeric and conversion semantics (revision 1)

This is the normative numeric contract for the parser, optimizer, compiler/runtime
helpers, native backends, and their tests. Language integers are signed 64-bit
two's-complement values. A decimal integer literal must be in
`0..9223372036854775807`; larger literal magnitudes are compile-time errors.
The minimum value is expressible as `-9223372036854775807 - 1`.

## Arithmetic

- `+`, `-`, `*`, and unary `-` are checked. A result outside
  `[-9223372036854775808, 9223372036854775807]` raises `integer overflow` at
  the source location of the operation. `+` also concatenates values if either
  operand is not an integer; concatenation uses the runtime's string
  representation and yields a buffer if either operand is a buffer.
- `/` and `%` require integer operands. Division truncates toward zero and
  remainder has the sign of the dividend, matching C's signed quotient/remainder
  rule. A zero divisor raises `division by zero` or `modulo by zero` as
  appropriate. `INT64_MIN / -1` and `INT64_MIN % -1` raise `integer overflow`.
- Ordered comparisons require integers. Equality compares integers by value,
  null only with null, strings and buffers by byte contents across those two
  object types, and other objects by identity. Comparisons produce integer 0 or
  1.
- Truthiness is false for null and integer zero; all other integers and all
  objects are true.

The optimizer may fold an operation only when the result is defined by these
rules. If folding would cause a language error, it must leave the operation in
the IR so execution raises the same error at the same source site. Folding
must not use C signed overflow as its semantics.

## Constant folding and propagation

The optimizer uses the same forward value-fact lattice as native lowering:
unknown, null, integer (optionally an exact signed 64-bit constant), string,
buffer, array, and other object. At a control-flow join a fact survives only
when every reachable predecessor agrees; equal integer constants remain exact.
An unreachable arm of a proven branch does not participate in the merge.

- Safe folds are integer literals; unary `-` except `INT64_MIN`; `!` when
  truthiness is proven; and integer `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`,
  `<=`, `>`, and `>=` when all operands are exact and the result is defined.
- Proven local/global integer values can propagate through reachable CFG edges
  and loop headers. A loop backedge must agree with the preheader fact; the
  optimizer never assumes a loop executes or runs only once.
- Strings, buffers, arrays, object equality, indexing, conversions, calls,
  allocation, output, ownership operations, and dynamic fallback operations
  are not folded by this revision. Direct user calls invalidate global facts.
- A fold replaces only the opcode and operands. It retains the original IR
  instruction's source line and column, so later diagnostics and maps still
  identify the source expression.

There is no `switch` IR opcode in revision 1. Branch folding applies to the
existing truthiness branch opcode; any future switch lowering must add its own
exhaustive constant-folding and diagnostic-preservation tests.

## Conversions

- The language's `String` declaration conversion (`TO_STRING_BUFFER`) clones a
  buffer, copies a string's bytes, formats an integer in base 10, and converts
  null or unsupported values to an empty buffer.
- The `string_new` builtin has its own documented conversion: strings and
  buffers become strings with the same bytes; an integer becomes the single
  byte given by its low eight bits. It is not integer-to-decimal formatting.
- Runtime `bl_value_to_int` accepts an integer directly or parses the complete
  decimal text representation of a string/buffer; parsing must fit in signed
  64-bit range. Other inputs fail conversion.

## Shifts and future numeric opcodes

Revision 1 has no shift opcode or shift token. If shifts are added, counts must
be integers in `0..63`; right shift must be arithmetic (sign-extending), and
left shift must be checked for signed overflow. Implementations must avoid C's
undefined behavior for oversized shifts and signed left shift. Until an opcode
and these semantics are implemented and tested, the compiler must diagnose it
as unsupported instead of emitting an approximation.

## Backend conformance

The C runtime helpers define reference behavior. Native fast paths must match
the rules above and route dynamic or failing cases to a helper that preserves
the same source site. Every arithmetic optimization and native opcode change
must be covered by boundary tests and differential execution in optimized and
baseline modes. See `ir-backend-contract.md` for ownership and lowering rules.
