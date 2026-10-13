# Target/platform contract (1.0.18)

Four OS/architecture pairs are implemented: macOS ARM64, Linux x86-64,
Linux ARM64, and Windows x86-64. `src/target.c` owns their descriptors;
`include/target.h` exposes the record. Target IDs/triples, object formats,
external symbol prefixes, section directives, relocation/encoding limits,
helper ABI, unwind model, stack alignment, shadow/red-zone rules and probe
conventions are queried through that record. Register inventories and ISA
instruction selectors remain separate implementations keyed by target ID.
`bpp --target-info TARGET` exposes the descriptor and register/range records.

`bpp_target_for_host` accepts only the four pairs. Unknown operating systems,
macOS Intel, Windows ARM64, 32-bit targets and other architectures return
`BPP_TARGET_UNSUPPORTED`; they cannot silently inherit Linux x86-64 or
macOS ARM64. A compiler built on an unsupported host requires an explicit
supported target for non-executable cross-target output. Executables require
matching host/runtime objects. No Rosetta, ARM64EC or foreign-runtime linkage
is inferred from an ISA name. All current targets use little-endian 64-bit
pointers and integers, and 16-byte ABI stack alignment.

| Target | Format / external prefix | Helper ABI / unwind | Stack policy |
|---|---|---|---|
| macos-arm64 | Mach-O / `_` | Darwin AAPCS64 / CFI and compact unwind | 16-byte alignment, no shadow space or red zone; inline page-touch probes |
| linux-x86_64 | ELF / none | System V AMD64 / DWARF CFI | 16-byte alignment; ABI permits 128-byte red zone, which generated frames do not use; inline page-touch probes |
| linux-arm64 | ELF / none | AAPCS64 / DWARF CFI | 16-byte alignment, no shadow space or red zone; inline page-touch probes |
| windows-x86_64 | COFF/PE / none | Windows x64 / SEH | 16-byte alignment, 32-byte caller shadow space; `___chkstk_ms` for large frames |

The descriptor's 4096-byte probe stride is a codegen interval, not a claim
that every host's VM page size is 4096 bytes. Generated probes touch each
interval and preserve target frame/call alignment. Public static ABI 2 uses
host argument registers/stack slots for its narrow B++ procedure bridge;
raw bootstrap procedures retain their separate private word-array convention.
Neither is a general language-facing C FFI.

## Supported release baseline

These are the minimum **supported and tested release configurations** for this
cycle. Earlier systems/toolchains may work but have no support claim until a
matching-host suite runs. Cross-assembling an older deployment triple does not
lower the native release baseline. Exact runner versions and compiler hashes
are retained in the 1.0.17 evidence and subsequent platform CI artifacts.

| Host baseline | Supported compiler/build driver | Object/link/debug tools |
|---|---|---|
| macOS 15 ARM64 | Homebrew LLVM/Clang 23; macOS SDK 15 or newer through Xcode command-line tools/`xcrun` | LLVM assembler/readers, Apple SDK/linker, `dsymutil`, LLDB |
| Ubuntu 24.04 x86-64 or ARM64, glibc 2.39 | Clang 18 or GCC 13 | LLVM 18+ cross-object tools, native GNU/LLVM linker and GDB |
| Windows Server 2022 x86-64 (build 20348) | MSYS2 UCRT64 Clang/LLVM 23 and GNU Make | LLVM `ar`, LLD, binutils resource compiler, LLVM PDB readers/LLDB, UCRT |

The macOS assembler triple currently contains macOS 13; it describes object
assembly/deployment encoding, not verified execution on macOS 13. macOS SDK
version and deployment defaults are those of the selected native driver;
platform evidence records `xcrun`'s SDK version. Linux GCC is a supported
native compiler/driver, while foreign object inspection uses Clang rather
than asking GCC to accept LLVM target triples. Windows uses UTF-8 process
metadata, UCRT64 and LLD. MSVC/clang-cl syntax, MSVCRT/MINGW64, Windows desktop
versions other than the recorded runner, and Windows ARM64 are outside this
verified toolchain contract. A new driver/SDK family requires separate native
startup, archive, unwind, debugger and sanitizer evidence.

## Verification boundaries

`make BUILD_MODE=release check-platform` checks actual host identity, execution
in space/Unicode paths, temporary-directory handling, debug sidecars, and
source protection across root/imported sources and hard-link aliases. Windows
additionally checks drive letters, mixed separators, case-only aliases, and
native backslash archive-manifest/library-search paths. Win32 file identity
uses UTF-8-to-wide paths and volume/file indices, because CRT inode fields
are not a reliable hard-link identity contract. Source/debug-snapshot output
must not overwrite any source participating in the compilation.

The same gate assembles eight actual objects: all four target formats through
both ordinary value and raw bootstrap backends. LLVM reads object headers,
sections, relocations, unwind records and debug sections. Large baseline
frames force Windows chkstk/SEH coverage; ELF checks require a non-executable
GNU-stack section. Cross-object bytes do not demonstrate native OS startup,
CRT/import resolution or debugger behavior. Those remain the matching-host
native CI gates in `codegen.yml`, including source stepping/backtraces and
large procedure/stack checks. Artifacts under `build/platform-evidence` retain
reader output, object bytes, descriptors and a summary with tool identities.

Mach-O branch calls use `ARM64_RELOC_BRANCH26`, not `BR26`. ARM64 imm19 literal
and conditional displacements are assembler-local range constraints on
Mach-O; there is no invented `ARM64_RELOC_BRANCH19` object relocation. See
[LLVM's Mach-O definitions](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/BinaryFormat/MachO.h)
and the [PE/COFF format specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format).
[Branch/address limits](branch-address-model.md) describe encoding checks and
host assembler/linker responsibility for relocation layout.

## Remaining runtime components

The C *output backend* has been removed. Compiler parsing, IR, optimization,
allocation and emitters remain C implementations. Native executables link
`runtime.o`, `value.o`, `codegen_runtime.o`, `bpp_abi.o` and `truth.o`; checked
profiles additionally use `native_sanitizer.o` and available host sanitizer
runtimes. These files provide internal value/ownership/error machinery,
truth-state propagation, static ABI validation and platform I/O/allocation.
Generated startup still depends on the host CRT/system loader and libc/UCRT.
The raw strict bootstrap path directly uses its private allocation/I/O/exit
leaves. Static archives remain target-specific and carry ABI/runtime revision,
export and archive-identity manifests. This release does not remove host
assembler/linker, CRT or private C runtime dependencies.

## CG exit review

The 1.0.17 matrix contains eighteen successful native jobs and six successful
Bake/benchmark jobs. Versioned reports cover all four host profiles with exact
compiler/runtime hashes, flags, code/frame sizes and raw samples. The allocator
workspace limit and spill/frame design are documented in
[register allocation](register-allocation.md), with bounded stress and
reproducibility checks in native CI. Historical C-reference entries remain
historical; Windows conformance now uses independent native expectations.

Shared sections 1.2–1.19 have been audited: the old section-1 register/call
native-runner gate is satisfied by the retained 1.0.16/1.0.17 matrices; current
platform changes need their own final matching-host checks. The ABI v1 wording
in historical completion notes is superseded by the implemented ABI 2 contract
in [static-abi.md](static-abi.md), not an assertion of continued v1 compatibility.
No checked “selective payload cache” work is relabeled as a general machine
allocator. Liveness's checked workspace ceiling remains a bounded-size contract.

Hosted benchmark reports and the two local repeats are reproducible measurement
evidence. They do not satisfy the explicitly **isolated hardware** benchmark
requests for Linux/Windows. By user decision, those are separate hardware gates outside the 1.0.18 platform work. They remain open until controlled idle dedicated
hosts produce repeated reports with matching hashes and stable machine IDs.
The stage-2 public ABI freeze remains open; completing the platform work does
not silently freeze the public contract or implement the separate ABI roadmap.

The 1.0.18 platform gate passes all six matching-host jobs in
[run 10](https://github.com/kenseitehdev/B-/actions/runs/38437964667).
[verification-1.0.18.json](verification-1.0.18.json) records the 48 inspected
objects, artifact digests and native path checks. All eighteen broader native regression jobs in
[run 58](https://github.com/kenseitehdev/B-/actions/runs/38437964637) also pass,
including macOS TSAN. The 1.0.18 scope is verified by all 24 CI jobs; isolated
hardware measurements and the public ABI freeze remain separate gates.
