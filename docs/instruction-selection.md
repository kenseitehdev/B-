# B++ instruction selection (revision 1)

Instruction selection is a shared, target-aware legality and cost decision in
`include/isel.h` and `src/isel.c`.  It sits after IR optimization and register
allocation, and before architecture-specific assembly syntax.  The selector
does not change B++ semantics: every mixed operation retains its existing
slow runtime/helper arm when a tag guard, overflow check, bounds check, or
unsupported dynamic value requires it.

## Complete opcode table

| IR class | Opcodes | Selected lowering |
|---|---|---|
| Native values/control | `NULL`, `INT`, `JUMP`, `BRANCH`, `RETURN` | Native generic sequence. Literals use an immediate materialization. |
| Runtime only | `STRING`, `INDEX_GET`, `INDEX_SET`, `TO_STRING_BUFFER`, `FREE_*`, `DELETE_*` | Private runtime helper; no speculative native encoding. |
| Mixed slot/ownership | `LOAD_*`, `STORE_*`, `INIT_*`, `RELEASE` | Native checked fast path with the existing runtime fallback. Address selection chooses a direct, immediate-address, or materialized-address sequence. |
| Mixed scalar | `NEG`, `NOT`, `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `EQ`, `NE`, `LT`, `LE`, `GT`, `GE` | Native integer fast path when tags permit, otherwise runtime fallback. |
| Mixed calls | `CALL` | Direct private-procedure ABI sequence or builtin runtime call. |

The table is represented by exactly one `BPPISelRule` for each revision-1
`BLIROp` on every registered target.  Object format changes directives and
symbol spelling, not instruction legality; macOS/Linux ARM64 share ARM64
selection, and Linux/Windows x86-64 share x86-64 selection.

## Current forms and legality

| Form | Legal when | Cost / scratch demand | Flags |
|---|---|---:|---|
| ARM64 add/sub immediate | Right operand is a proven integer and its magnitude is `0..4095` or a 12-bit value shifted left 12 bits | 1 / 0 | `adds`/`subs` flags are consumed immediately by overflow branch. |
| ARM64 compare immediate | Same immediate range; negative RHS uses `cmn` so flags equal `lhs - rhs` | 1 / 0 | `cmp`/`cmn` flags are consumed immediately by `cset`. |
| x86-64 immediate32 | Right operand is a proven signed 32-bit integer for add/sub/mul/comparison | 1 / 0 | Arithmetic flags feed `jo`; compare flags feed `setcc`, immediately. |
| Generic scalar | Any guarded integer operands | 3 / up to 4 reserved scratch GPRs | Flags never survive the selected sequence. |
| Division/modulo | Guarded integers | 9 / 4 reserved scratch GPRs | Zero/`INT64_MIN / -1` checks consume their flags before `sdiv`/`idiv`. |
| ARM64 address | Zero displacement, encodable 12-bit/shifted immediate, otherwise materialize | 0, 1, or 3 / at most 1 | No live condition flags. |
| x86-64 address | Zero/32-bit displacement, otherwise materialize | 0, 1, or 3 / at most 1 | No live condition flags. |

All current language integers are signed 64-bit.  There are no IR shift,
zero/sign-extension, vector, floating-point, fused multiply-add, or
compare-and-branch opcodes yet.  The selector therefore records those feature
classes as unavailable for a concrete selection rather than fabricating an
encoding. ARM64 `msub` remains the defined modulo reconstruction after checked
division; it is not an algebraic fusion across IR instructions.

## Register and flags contract

The register allocator supplies physical locations or canonical cells; the
selector supplies only the number of reserved scratch GPRs required by a form.
Emitters reject a form that asks for more than their four documented scratch
registers and use the runtime fallback. No selected form carries condition
flags across an IR instruction: a flag-defining instruction and its `b.vs`,
`jo`, or `setcc` consumer are emitted as one indivisible group. This prevents a
later address calculation, reload, or helper setup from silently clobbering a
live condition code.

## Verification and measurement

`tests/test_isel.c` verifies every opcode has a rule on all four targets,
immediate/address legality boundaries, form costs, scratch demand, and flag
lifetimes. The normal execution suite also checks selected ARM64 and x86-64
assembly text for shifted ARM immediates, negative compare immediates, and
x86-64 immediate32 forms, alongside overflow and dynamic-type regressions.

`make -s BUILD_MODE=release benchmark` records executable bytes and repeated
wall-clock startup-inclusive time. It is a regression harness, not a completed
per-CPU cycle study. Retaining a future more complex form requires native
macOS ARM64, Linux ARM64, Linux x86-64, and Windows x86-64 measurements with
the compiler revision, flags, CPU model, and OS recorded.
