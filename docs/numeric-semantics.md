# B++ numeric and conversion semantics (1.0.16)

The parser, optimizer and native backends share this contract. Integers are
signed 64-bit two's-complement words. Integer literal magnitudes must fit in
0..9223372036854775807; write the minimum as `-9223372036854775807 - 1`.
Decimal/exponent literals use finite binary64 storage. `state(value)` validates
a truth value against [-1,1]; interval velocities and other numeric rates may
exceed that range. See [truth and TAU](truth-and-tau.md).

## Arithmetic and errors

Integer `+`, `-`, `*` and unary `-` check overflow. Integer `/` truncates toward
zero, and `%` has the dividend's sign. Zero divisors and `INT64_MIN / -1` or
`INT64_MIN % -1` are errors. Mixed state/integer arithmetic promotes the integer
to binary64; state arithmetic supports `+`, `-`, `*`, unary `-` and `/`.
Nonfinite results and invalid operand kinds are errors.

Recoverable language errors produce U=0 with a fault mask, source location and
diagnostic context. Failed rate arithmetic retains its explicit transition
inputs. Gates and numeric comparisons retain incoming fault context; a Boolean
payload alone does not validate a faulted value. Applications can combine
`faults(rate)==0` with their velocity threshold as an explicit validation rule.
Structural compiler/ABI errors and process-level failures remain failures.
Explicit diagnostic and strict bootstrap profiles have their separately
documented execution contracts.

Native buffer operations validate bounds, lifetime, generation and byte
definedness before accessing memory. Reallocation preserves defined bytes and
invalidates older offset views. Native buffer equality compares handle identity,
generation and offset. Runtime string/array source constructors remain outside
the supported frontend; curated library bridges use the private owned-value
boundary and validate their input conversions.

## Boolean results and conditions

Comparisons return +1 for true and -1 for false. Fractional comparisons preserve
context in owned state values. Logical state negation is `-s`; integer `!`
returns -1 for positive words, +1 for negative words and 0 for U=0. Non-state
owned objects remain truthy, with logical negation returning false (-1).
Null is treated as U=0.

A branch enters on a positive predicate payload. Propagation decisions use the
explicit accepted form `if (velocity(start,end,dt) >= threshold)`, with a
positive finite dt and a program-supplied threshold. Direct scalar conditions
use their sign and do not infer elapsed time, previous state, or validation.
Use the explicit velocity predicate for decisions about uncertain transitions.

State gates use min/max/negation, including
`XOR(a,b)=max(min(a,-b),min(-a,b))`; TAU returns sign(end-start). Integer
`&`, `|`, `^` and `~` operate independently on all 64 bits. In particular,
`~1` is -2, whereas logical `!1` is -1. No shift opcode is implemented.

## Optimization and conformance

Only defined integer results may fold. Overflow and invalid division remain
runtime operations with their original source sites. A value fact survives a
join only when every reachable predecessor agrees. Deleted or uninitialized
slots cannot become known values merely through a failed store. Direct calls
invalidate global facts; dynamic state/ownership/memory operations retain their
runtime boundary. Debug builds preserve canonical source-variable homes.

Native assembly and C value leaves are verified against independent expected
results in both optimization modes. The source frontend has no generated C
backend. See [verification](verification.md), [static ABI](static-abi.md) and
[IR contract](ir-backend-contract.md).
