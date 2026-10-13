# Native sanitizer contract (1.0.16)

The C output backend and CLI routes are retired at the end of 1.0.15. All
executable output uses native code generation. Seeded regressions evaluate
expected results independently of compiler IR and compare both native
optimization modes; the evaluator is test tooling, not a compiler backend.

The default 1.0.16 native value runtime supports signed integers, binary64
states and owned memory handles. It checks bounds, byte definedness and lifetime
before C memory accesses, returning U=0 with source/error context on recoverable
failures. Native assembly supplies control flow, calls and integer fast paths;
C value leaves do not interpret the IR. ABI 2 exposes the same value semantics.

The explicit `--native-sanitizers` diagnostic profile uses raw words and the
portable fail-fast instrumentation described below. Its deliberate fault
fixtures validate diagnostics and exit behavior separately from the default
recoverable state tests. Strict bootstrap output retains its helper-free profile.
Debug/TSAN/MSAN host builds instrument C components; assembly is not claimed to
have arbitrary host shadow propagation. MSAN trusts validated compiler-owned
frame cells, while application buffer bytes remain checked for definedness and
C memory accesses retain host instrumentation.

The compiler-private boundary is in `include/native_sanitizer.h`. Generated
assembly does not contain a Clang/GCC shadow-memory mapping. The portable C
boundary tracks allocation ranges, one definedness byte per application byte,
allocation source sites, live/retired frames and retired heap allocations.
Retired regions are tombstones; native accesses are rejected before touching
freed memory. As with ordinary address sanitizers, address reuse can hide a
stale pointer after another allocation acquires that address.

Sanitized frames use separate heap storage with 32-byte guards. ASAN builds
poison the guards and unpoison the backing allocation before freeing it. Every
procedure return retires its frame; later checked accesses report
`stack-use-after-return`. Checked word and byte operations validate bounds and
initializedness. Compiler-owned frame clearing, parameter copies, initialization
flags, outgoing argument copies and result transfer remain trusted assembly
housekeeping; their addresses follow the validated IR/frame layout. Heap byte
loads/stores, pointer offsets, output ranges and allocation/free operations
pass through checked helpers. Reallocation preserves definedness of copied
bytes and leaves growth undefined. Invalid native integer operations report
their source site and category. There are no raw value tags to validate.

C helpers perform the actual application read/write once, so host ASAN/UBSAN,
TSAN and MSAN instrument those C accesses without a second native memory-event
callback. The portable checks run before the access, avoiding duplicate host
reports for the same native fault. Clang's MSAN parameter/return TLS protocol is
disabled for C objects called through the native value/diagnostic boundaries because assembly cannot populate it;
MSAN instrumentation of its C memory accesses remains enabled. Native byte
definedness comes from the portable map. This is not a promise of MSAN shadow
propagation through arbitrary handwritten assembly or foreign calls.

TSAN observes non-atomic C reads/writes in the checked boundary, and native
procedure entry/exit is represented by explicit tracing hooks. B++ has no
thread creation, synchronization or atomic operations today. The allocation
registry is single-threaded; concurrent entry from foreign code is unsupported.
Before adding threads, the compiler must define memory ordering and implement
thread-safe metadata and synchronization events. TSAN does not supply those
language semantics.

Leak checking runs on normal module completion. Globals are conservative roots;
initialized aligned words in reachable allocations are scanned for pointers
into other tracked regions. Unreachable allocations, including unreachable
cycles, report their allocation source site. Reachable global allocations are
released when instrumentation shuts down. Integer words resembling pointers
can conservatively keep an allocation reachable. Fatal error paths terminate
immediately and do not run the normal-completion leak scan.

Portable faults print `file:line:column: B++ sanitizer: category` and exit 86.
Host sanitizer failures in compiler/C code retain the host runtime's reporting
and exit conventions. `tests/check_native_sanitizer.sh` checks positive cases,
fault categories/sites/statuses, optimized and baseline native fault execution,
all four target assemblies, and hook-free release output. Its companion C test
also checks frame bounds/return lifetime, allocation growth, cycles and roots.

Run `make BUILD_MODE=release check-native-checks` for the portable opt-in profile,
or `make BUILD_MODE=debug check-native-sanitizer`, repeat with `tsan`, and use
`msan` only on Linux/Clang. `make check-sanitizer-availability` records actual
driver/linker/runtime availability for each host flavor. An unavailable runtime
is an explicit result, not a passed gate. Native CI retains the compiler
revision, runner/toolchain identity and test transcript.

Final implementation evidence is [native CI run 47](https://github.com/kenseitehdev/B-/actions/runs/38329351396), revision `2c817e176897d0addfd7df624b01d4e136ac1f57`. All 18 jobs / 19 execution profiles pass. Exact job IDs, compiler identities, availability probes and retained transcript artifact digests are in [verification-1.0.16.json](verification-1.0.16.json).

| Host / compiler | Matching-host execution | Availability limits |
|---|---|---|
| macOS ARM64 / Homebrew Clang 23.1.0 | Release, ASAN/UBSAN and TSAN; native fault, truth, ABI and differential suites | MSAN unsupported; standalone LSAN smoke recorded separately |
| Linux x86-64 and ARM64 / Ubuntu Clang 18.1.3 | Release, ASAN/LSAN/UBSAN, TSAN and MSAN; native fault, truth, ABI, differential and stress suites | MSAN is the checked boundary described above |
| Linux x86-64 and ARM64 / GCC 13.3.0 | Release, ASAN/LSAN/UBSAN and TSAN; native fault, truth, ABI, differential and stress suites | MSAN unsupported with GCC |
| Windows x86-64 / UCRT64 Clang 23.1.3 | Release, portable native diagnostics, UBSAN trap debug profile and live PDB/LLDB checks | ASAN runtime unavailable; LSAN/TSAN unsupported by this driver; MSAN unsupported |

Windows UBSAN smoke links with the available runtime, but recoverable handler symbols required by the full build are unavailable. Its debug execution profile therefore uses `-fsanitize-trap=undefined`; an intentional C overflow verifies the trap is active. This does not claim Windows ASAN, LSAN or TSAN execution.

MSAN is rejected on macOS, Windows and non-Clang toolchains. Standalone LSAN availability is probed independently; the portable leak check does not depend on standalone LSAN support. Native execution evidence applies to the recorded implementation and toolchains, rather than every possible toolchain version.
