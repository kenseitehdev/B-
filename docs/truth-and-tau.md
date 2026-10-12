# Truth states and TAU: 1.0.16 language contract

This records the user-supplied rules. The fractional truth model and TAU are
pending implementation; the current compiler still uses its previous Boolean
behavior. Passing bitwise tests does not certify these logical operations.

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
State creation syntax remains to be implemented.

Runtime errors enter uncertainty propagation. When no valid computed state exists, an error produces U=0 and retains the
error context and transition inputs so propagation can reason about the failure. Failed accesses must not perform invalid memory operations. This
changes the externally visible error contract and therefore requires deliberate
static-ABI versioning rather than silently changing existing libraries.

Integer bitwise `&`, `|`, `^` and `~` act independently on all 64 bits. In
particular, bitwise complement is distinct from logical state negation:
`~1` is -2, whereas logical negation of true is -1. Source spellings for state
AND/OR/XOR and state creation remain to be finalized.
