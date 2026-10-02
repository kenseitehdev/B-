# B++ compiler migration

This source bundle replaces the supplied AST execution path with:

B++ source → import expansion → AST → hand-rolled register IR → generated C → native executable.

The C backend is hand-written and consumes the custom IR. The host C compiler
performs machine-code generation and linking. This is not a direct assembly
or machine-code emitter. Generated executables do not contain an IR interpreter
or the AST VM. There is no libbl.a target.

## Install and build

Use this directory as a complete source snapshot, or copy its Makefile,
include/*.h, and src/*.c into the matching locations in your repository. Commit
or back up your current checkout first. Existing vm.c, interpreter.c, vm.h,
interpreter.h, and bl.h are retired by this compiler path; this Makefile uses an
explicit source list and does not build them. Any other repository source not
supplied in this conversation, including asm.c, is not integrated by this bundle.
The public BLInterpreter/bl_run_* API is not part of this compiler snapshot.

The lexer and parser must be updated together with ir.c: TOK_FN is renamed
TOK_PROC; AST_FN_DEF is renamed AST_PROC_DEF; the AST union member fn_def is
renamed proc_def. All definitions now use the proc keyword:

```text
define proc add(a, b) {
    return a + b;
}
return add(2, 3);
```

The existing `define` prefix is preserved. `fn` is now an ordinary identifier;
`define fn ...` produces a parser error. `#define proc` and quoted native
procedure declarations use the new keyword too. As in the original parser,
quoted native snippets and macro definitions are parsed and discarded; arbitrary
native snippets are not compiled and macros are not expanded. Runtime builtins
continue to supply their actual behavior.

```sh
make release
./bin/release/bpp --print-result -o demo tests/demo.bpp
./demo
# prints 11, then the top-level return value 10

./bin/release/bpp --check tests/demo.bpp
./bin/release/bpp --emit-ir -o demo.ir tests/demo.bpp
./bin/release/bpp --emit-c -o demo.c tests/demo.bpp
make BUILD_MODE=release check
```

Without -o, native compilation creates a.out. --emit-ir and --emit-c default to
stdout. --check performs parsing and IR lowering without producing an executable
or running the source. --print-result embeds final-result printing into the
compiled executable. It preserves the old CLI's string/int output and
buffer/array descriptors. A successful top-level return is a value, not the
process exit status; runtime errors return status 1.

Use CC=cc to build with GCC, or the default Clang/Homebrew LLVM selection.
BPP_CC can override the host compiler executable at invocation time. It must be
one executable name/path, not a shell command or a string of arguments. Compiler
invocation uses fork/execvp with individual arguments, not a shell command.

The compiler records absolute paths to the include directory and runtime/value
object files at build time. Keep the built compiler with its checkout and object
files. After moving the checkout, run make clean and rebuild. The generated
native executable is independent of those build-time paths.

## Sanitizer modes

| Build | Compiler and generated executable | Compiler path |
|---|---|---|
| make debug (default make) | ASAN + LSAN + UBSAN | bin/debug/bpp |
| make tsan | TSAN | bin/tsan/bpp |
| make release | O2, no sanitizers | bin/release/bpp |

Modes have separate object/output directories. The runtime is compiled into
matching per-mode object files, then linked into generated executables. There
is no static library. The compiler itself does not link the value runtime.

```sh
make BUILD_MODE=debug check
make BUILD_MODE=tsan check
make BUILD_MODE=release check
```

The default debug flags request -fsanitize=address,leak,undefined explicitly.
Support depends on your selected compiler/target. If it rejects standalone
leak instrumentation, you can explicitly override DEBUG_SANITIZERS to
address,undefined and enable ASAN's leak checking with ASAN_OPTIONS=detect_leaks=1
where supported. That override does not guarantee LSAN support. Clean before
changing sanitizer selections or overriding compiler/flags within the same mode.
No fallback silently removes instrumentation.

## Components

- compiler.c retains the original local/global import resolver and cycle checks,
  and replaces the parse/execute entry points with parse/lower compilation.
  Global imports still use ~/.bytelang/lib and the legacy .bl suffix lookup.
  The CLI no longer copies Libraries into the home directory automatically;
  provision those files explicitly if using global imports.
- ir.c/ir.h own the custom IR, lower every supplied AST expression/statement,
  structurally validate it, and dump it.
- emit_c.c translates blocks into labels/branches and emits direct user calls.
- runtime.c contains the extracted builtins and dynamic value operations.
- value.c/value.h retain the original tagged/reference-counted representation.
- main.c provides compiler output modes and invokes the native toolchain.

## Runtime fixes

- Release truncated array values before shrinking the allocation.
- Release contained values when freeing array storage.
- Retain an array replacement before releasing the overwritten value.
- Respect owned payload flags when destroying objects or explicitly freeing them.
- Restore ownership flags when growing/reallocating a previously freed payload.
- Check allocation-size arithmetic for array resizing and concatenation.
- Diagnose signed arithmetic overflow, division/modulo by zero, and the
  LLONG_MIN / -1 case rather than executing undefined C arithmetic.
- Clean up frame registers and locals on every return and runtime-error path.
  Returned objects stay owned until the caller consumes/releases them; the old
  dangling public BLValueView return path is no longer used.

Reference counting still does not collect object cycles. The supplied language
has no synchronization/threading features; TSAN instrumentation does not make
its reference counts atomic or make shared runtime objects thread-safe.

## Semantic decisions inherited from the first IR bundle

Top-level procedures are collected before lowering, supporting forward calls
and recursion. Nested procedure definitions, duplicate declarations/parameters/
procedures, and builtin-name procedure redefinitions are rejected. Variables
are function-scoped; local declarations shadow globals throughout that function.
Unexecuted declarations leave slots uninitialized and accessing them reports a
runtime error. Re-executing a declaration in a loop reinitializes the same slot.
Unknown names and direct user-call arity errors are compile-time errors.

These differ from the supplied VM's dynamic registration, first-match duplicate
variable lookup, and runtime name resolution. Blocks still introduce no scope.
String declarations still convert values into C-string buffers, and indexing
still applies only to buffers. The supplied parser's syntax restrictions remain
in place; this does not add for loops, short-circuit operators, types, or syntax
that are absent from its headers and implementation.

Import expansion retains the old diagnostic limitation: locations refer to the
expanded root source, not an original-source map for imported files.

See IR-NOTES.md for the register/slot ownership contract and instruction layout.
The native backend implements that contract. Structural validation does not
perform a dominance/definite-assignment proof or a static type proof.

## Validation performed

Strict C11 compilation with Wall/Wextra/Wpedantic/Werror passed for compiler and
runtime sources. An emitted sample also compiled with those strict flags.
IR-lowering unit tests and 21 native execution/diagnostic cases passed in release
and TSAN modes. The native cases passed under ASAN/UBSAN with leak checking
disabled because LSAN's exit scan fails under this execution environment's
process-inspection restriction. A completed LSAN leak check is not claimed.
The test programs cover recursion, forward calls, imports, scopes, loops,
string conversion, buffer alias invalidation/reallocation, array shrink/grow,
self-assignment, return/error cleanup, overflow, bounds, new proc syntax, old fn
rejection, output modes, and source-overwrite protection.

Validation used GCC on this host. Apple Clang/Homebrew LLVM and native macOS
execution were not available for validation here. Run make check locally in
each supported mode before replacing your established compiler workflow.
