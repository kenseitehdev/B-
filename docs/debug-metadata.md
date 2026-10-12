# Native debug metadata (1.0.14)

Raw native output emits original file/line locations after import expansion,
optimization and CFG layout. DWARF5 on Mach-O/ELF has procedure scopes,
parameters, locals, declaration files, types, an address table and location
lists. Frame addresses are relative to x19/r12. Definite-initialization flow
intersects predecessor states, handles loops and deletion, and emits locations
only while a source slot is valid. `--emit-map` exposes the same canonical
source-variable ranges over block/instruction positions. This backend has no
promoted source locals: cached IR-value register transitions are not presented
as source-variable locations.

The source `byte` local uses a signed 64-bit word; indexed memory access is a
single byte. `ptr` uses a 64-bit void-pointer type. Untyped parameters expose
their signed raw-word representation under a type name that explicitly says
their source type is unspecified. Type sizes match the actual frame storage.
Declaration file IDs are 32-bit. Procedure ranges include the shared epilogue.
CFI uses a stable frame-based CFA through probes and helper calls; Windows
uses SEH, including saved nonvolatile registers and `___chkstk_ms` for large
frames. The allocator-based transitional backend leaves unavailable source
locations empty rather than presenting an IR cache as a named local.

Windows COFF output emits CodeView procedure signatures, source-level names,
parameter/local symbols, source-type aliases, line tables and register-relative
defranges. The line/symbol subsections reside in `.debug$S`, types in `.debug$T`.
Debug executable linking uses LLD and writes `<output>.pdb`. macOS linking
writes `<output>.dSYM`. Keep these artifacts with their executables. See LLVM's
[CodeView symbol records](https://llvm.org/docs/PDB/CodeViewSymbols.html) and
[type records](https://llvm.org/docs/PDB/CodeViewTypes.html) for the record formats.

`make BUILD_MODE=debug check-debug-metadata` verifies imported files, local
locations, type/procedure records, CFI/SEH, and debugger stepping/backtraces.
LLVM tooling verifies the dSYM on macOS, reads COFF records across platforms,
and inspects a linked PDB when LLD/PDB tools are present. Native LLDB checks
include local/parameter values, loop stepping, recursion and imported faults;
Linux also runs GDB tests, including displayed parameter/local values. LLVM
verification on Linux uses the compiler-owned native object; linked DIEs,
locations and unwind records are checked with readelf and native GDB, avoiding
known verifier incompatibility with unrelated GCC sanitizer debug units. Windows CI installs LLDB/LLD and requires native PDB
inspection, stepping and variable-value checks. A cross-assembled COFF object
does not count as a Windows debugger run.

The macOS ARM64 LLDB and dSYM checks passed locally for this change. ELF and
COFF assemblies pass cross-assembly and COFF records decode with LLVM.
Remaining Linux GDB, Windows LLDB/PDB and final-matrix verification fixes
are assigned to 1.0.16 in `todo.md`; no matching native execution is inferred
from cross-assembly.
