# Backend feature and target policy

## Queryable target records

`include/target.h` is the authoritative target registry. Each record declares
the target name, assembler triple, object format, ISA, pointer and language
integer widths, and payload register count. Add a target there before exposing
it to users. Do not maintain separate target-name or triple lists in the CLI,
emitter, tests, or build rules.

Use `bpp_target_count`, `bpp_target_at`, and `bpp_target_capabilities` to query
records. `bpp_target_ir_lowering(target, opcode)` reports `native`, `runtime`,
`mixed`, or `unsupported`. `bpp_target_is_host` is the single host compatibility
check. CLI tooling can query the same information with:

```sh
bpp --list-targets
bpp --target-info linux-x86_64
```

`--list-targets` prints tab-separated name, assembler triple, object format,
ISA, pointer width, and payload register count. `--target-info` prints those
fields and one lowering classification per IR opcode.

## Unsupported operations

Every opcode introduced to IR must receive an explicit lowering classification
for each registered backend before that backend can accept it. `unsupported`
must fail before assembly is emitted, with target, opcode,
function, block, instruction, and source location in the diagnostic. A new
operation can be enabled by one of three explicit choices:

1. implement a native lowering that preserves the documented opcode contract;
2. call a B++ runtime helper with the same value ownership, errors, and source
   location; or
3. add a target-specific lowering and test it against independent expected results in optimized and baseline native modes.

No default switch arm may fabricate a value, omit an effect, or silently
translate an unknown opcode to a generic operation. New backend code must keep
the table classifier, emitter switch, and conformance tests in agreement. See
[`ir-backend-contract.md`](ir-backend-contract.md) and
[`numeric-semantics.md`](numeric-semantics.md).
