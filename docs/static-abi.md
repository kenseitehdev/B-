# B++ static-library ABI v2

This is the B++-to-B++ static-library ABI. It is deliberately small: there is
no dynamic loading, general foreign-function interface, callbacks, variadics,
or public object layout beyond the tagged `BLValue` call boundary below.

## Library output

Emit an assembly-only library unit with an explicit collision-resistant library
name:

```sh
./bin/release/bpp --emit-library --library-name math \
  --target linux-x86_64 --emit-asm -o math.s math.bpp
cc -x assembler -c math.s -o math.o
ar rcs libmath.bpp.a math.o
./bin/release/bpp --emit-library-manifest --library-name math \
  --target linux-x86_64 -o libmath.bppabi math.bpp
```

`--emit-library` omits `main`, retains every named procedure as an external
root, and emits:

- `bpp_v2_<procedure>` — exported procedure symbol;
- `bpp_v2_library_<library>` — `BPPStaticLibrary` descriptor;
- `bpp_v2_procedures_<library>` — descriptor procedure table.

On Mach-O the assembler spelling has the normal leading underscore; the object
symbol seen by the linker still has the names above. Library names and source
procedure names are ASCII identifiers. The module top-level body is not an
exported procedure.

## Versioning and descriptor

`BPPStaticLibrary` is declared in `include/bpp_abi.h`. It contains the magic
`BPP1`, major/minor revision, mandatory `static-only`, `no-ffi`, and
`value-ownership` policy bits; the `BLValue` size/field offsets; and an array
of `(name, arity, entry)` procedure descriptors.

Consumers call `bpp_static_library_validate` before use. Major revisions must
match exactly. A library minor revision must not be newer than the consuming
runtime's minor revision. Changing the value layout, calling convention,
ownership rules, or externally visible error contract requires a new major
revision and symbol prefix, not a silent replacement.

Revision 2.0 requires runtime revision 3. The 16-byte tagged value boundary
is retained, with a new owned state object carrying a binary64 payload and
error provenance. Signed Boolean results and recoverable runtime errors change
the public semantics, so v1 archives and manifests are rejected. Recompile
producers and consumers together; a symbol-prefix change prevents accidental
linking against the old contract.

## Calling convention

Every exported procedure has this target-C-ABI signature:

```c
void bpp_v2_name(BLRuntime *runtime,
                 BLValue *globals,
                 unsigned char *global_initialized,
                 const BLValue *borrowed_arguments,
                 BLValue *owned_result);
```

| Target | Argument placement |
|---|---|
| macOS ARM64 / Linux ARM64 | AAPCS64: `x0`–`x4` |
| Linux x86-64 | System V: `rdi`, `rsi`, `rdx`, `rcx`, `r8` |
| Windows x86-64 | Microsoft x64: `rcx`, `rdx`, `r8`, `r9`, then caller stack/home space |

The caller maintains the platform's C ABI stack alignment and required
shadow/home space. Procedure code follows the normal platform callee-saved
rules; the B++ private frame remains an implementation detail behind this
entry convention. No variadic or aggregate classification exists in v1.

## Values, ownership, and errors

All B++ arguments and results are the one 16-byte tagged `BLValue` class:
null, signed 64-bit integer, or reference-counted object (string/buffer/array/state).
Binary64 states travel as owned tagged objects, rather than a separate platform
floating-point calling class. There are no vector or multiple-return classes.

- Argument values are borrowed by the caller. The generated procedure clones
  each parameter into its local frame before it can retain, replace, or release
  it.
- `owned_result` is null on entry and receives one owned value on successful
  return. The caller releases it exactly once.
- Recoverable language failures return owned U=0 states with retained fault
  context. Structural descriptor/arity failures still return an ABI error status.
- Arity is checked by the descriptor before the generated procedure is entered.

The ABI may be exercised through `bpp_static_invoke`; this is an ABI validator
and host harness, not an invitation to treat B++ objects as a general C FFI.

## Runtime linkage

A library is a target-specific artifact pair:

```
libmath.bpp.a       # normal static archive for one target
libmath.bppabi      # text manifest, checked before lowering/linking
```

The manifest is deliberately small and deterministic:

```text
bppabi 1
name math
abi 1.1
runtime 2
archive linux-x86_64 libmath.bpp.a 123456 01ab23cd45ef6789
dependency numbers
export add 2
```

`archive` may occur once for each supported target. Its final two fields are
the archive byte count and a 64-bit FNV-1a identity fingerprint, generated
after `ar` creates the archive. `export` carries the source name and exact
arity. The manifest must use ABI 2.0 and runtime revision 3; unknown
directives, duplicate exports, a missing selected target archive, target
mismatch, stale/substituted archive identity, manifest-name mismatch,
dependency cycle, or ambiguous imported export is a compile-time diagnostic.

Use a static archive from B++ source with a compile-time-only import:

```bpp
import library "math";
return add(20, 22);
```

The source directory is searched first, then every `-L directory`, then
`~/.bytelang/lib`. The compiler resolves transitive `dependency` records,
checks call arity while lowering, emits direct `bpp_v2_<procedure>` calls, and
adds archives to the host-link command in dependant-before-dependency order.
It preserves ordinary textual `import "file.bpp";` and global
`import {name};` imports unchanged. Static imports follow native code generation and the validated archive link
boundary. The C output backend was removed in 1.0.15.

Inspect the exact resolved inputs without assembling or linking:

```sh
./bin/release/bpp -L . --emit-link-map -o app.link-map app.bpp
```

The deterministic text map records target, manifest and archive paths, archive
identity, imported procedure symbol, arity, and owning library index. It is the
handoff format for future buildFile/link-map work, not an object-file map.

A final executable still links the matching `runtime.o`, `value.o`,
`codegen_runtime.o`, and `bpp_abi.o` from the same B++ build revision. On ELF
x86-64 it uses the current non-PIE model required by the large internal-label
code model. This is a manifest resolver and host-link stage, not the later
custom object linker: archive indexing, object parsing, duplicate-symbol
analysis, relocations, section layout, veneers, and foreign libraries remain
linker-stage work.

`make check-static-abi` verifies a separate B++ producer assembly object,
packages it with `ar`, generates its manifest, compiles a separate B++
consumer using `import library`, links it through the resolver, and checks an
owned integer return. It also checks imported-call arity diagnostics and emits
library-mode output for all four registered targets. Matching macOS ARM64,
Linux ARM64, and Windows x64 archive execution remains a native CI gate.

## XF bridge library

`Libraries/xf` is a curated static bridge for the supplied XF runtime. Build
and exercise it with:

```sh
make BUILD_MODE=release check-xf-static-lib
```

It exports `xf_version()`, `xf_run_string(source)`, and `xf_run_file(path)`.
The bridge uses the same descriptor and manifest validation as a B++-produced
archive. It embeds XF core only: the legacy XF LambdaScript and former
ByteLang sub-bridges are intentionally excluded because they target a
different runtime boundary. Runtime failures become B++ diagnostics.

The supplied XF snapshot declares but does not define `interp_init` and
`interp_free`; the bridge carries a narrowly scoped compatibility source file
until XF provides those definitions itself.
