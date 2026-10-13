# Truth states and TAU: 1.0.16 language contract

The native compiler implements these user-supplied rules. `make check-truth`
checks the semantic kernel; `make check-native-truth` executes the independent
125-row reference table, fractional gates, velocity conditions, recoverable
errors and checked memory accesses in optimized and unoptimized native programs.
Static libraries use ABI 2.0/runtime revision 3 for the changed error contract.

Truth is a signed state in [-1, 1]. False is -1, true is +1, and uncertainty
occupies the open interval between them. The five reference states are F=-1,
LF=-0.5, U=0, LT=+0.5 and T=+1; they are reference points rather than a rule
restricting all uncertainty to those five values.

For states a and b, the accepted logical rules are:

| Operation | Result |
|---|---|
| Negation | -a |
| AND | min(a,b) |
| OR | max(a,b) |
| XOR | max(min(a,-b),min(-a,b)) |
| Implication | max(-a,b) |
| TAU `a => b` | sign(b-a) |

TAU's left operand is the starting state and its right operand is the ending
state. Its result is +1 for movement toward truth, -1 for movement toward
falsehood, and 0 for no net displacement. Thus `0 => 0` is 0, `0 => 1` is +1,
`1 => 0` is -1, and `1 => 1` is 0. Zero here means uncertainty, not falsehood.
TAU returns direction, not implication or the destination state.

The supplied table contains 125 cases: five start states times five end states
times five partner states. Classical logical operations are evaluated at both
ends against the partner. Mean velocity is (b-a)/dt, but neither dt nor path
history is needed for the chosen direction result. Equal endpoints cannot
distinguish a stationary path from an excursion; `=>` alone makes no claim
to measure path activity or perform hierarchical predictive propagation.

The signed truth model applies to all Boolean results and conditions. Conditions
use a configurable threshold on transition velocity, not on the destination
state. The accepted condition form is `if (velocity(start, end, dt) >= threshold)`.
The program supplies explicit start/end/dt and threshold operands. Velocity is
(end-start)/dt and dt must be positive; its units follow the supplied time unit.
The same explicit comparison can be used in a while condition. Validation
(`υ`) and the propagation threshold (`φ`) are distinct from the derived
construction (`κ`) that supplies state change and velocity. The interval
helper does not implement the entire abstract κ reconstruction/validation
pipeline. A distinct `state` value uses 64-bit floating-point storage; ordinary signed
64-bit integer values and bitwise operations retain their integer representation.
Create a state with `state(0.5)` or `new state current = 0.5;`.
Decimal/exponent literals use binary64 values. Velocity results may exceed the
truth range; `state(...)` validates a truth value against [-1,1]. Declarations
retain the existing language's dynamic assignment behavior.

Runtime errors enter uncertainty propagation. When no valid computed state exists, an error produces U=0 and retains the
error context and transition inputs so propagation can reason about the failure. Failed accesses must not perform invalid memory operations. This
changes the externally visible error contract and therefore requires deliberate
static-ABI versioning rather than silently changing existing libraries.

Integer bitwise `&`, `|`, `^` and `~` act independently on all 64 bits. In
particular, bitwise complement is distinct from logical state negation:
`~1` is -2, whereas logical negation of true is -1. State gates are `AND(a,b)`, `OR(a,b)`, `XOR(a,b)`, `NOT(a)` and
`IMPLIES(a,b)`; infix `a AND b`, `a OR b`, and `a XOR b` are also accepted.
`!a` performs signed logical negation. Word operators keep their existing
precedence; state AND/XOR/OR follow them in that order, and `=>` is lower
precedence and right associative. Parentheses make compound transitions clear.

Recoverable errors carry a fault mask and source location. `faults(value)`,
`error_line(value)` and `error_file(value)` expose that context; `err` carries
the most recent error. Native buffer operations validate bounds, lifetime and
byte definedness before accessing memory. Structural compiler failures remain
compilation errors. The explicit `--native-sanitizers` diagnostic profile and
strict bootstrap profile retain their separate fail-fast execution contracts;
state programs require the default native value runtime.
