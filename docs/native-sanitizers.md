# Native sanitizer contract (1.0.15)

The C output backend and CLI routes are retired at the end of 1.0.15. All
executable output uses native code generation. Seeded regressions evaluate
expected results independently of compiler IR and compare both native
optimization modes; the evaluator is test tooling, not a compiler backend.

The current source language has 64-bit raw words, byte-addressed memory,
`malloc`, `remalloc`, `free`, globals, procedures and direct calls. It has no
runtime tags, arrays, managed buffers, objects, threads, atomics or public FFI.
Checks for those retired or future representations are not claimed by the raw
backend. The transitional ABI-v1 tagged backend retains its existing hooks.

Signed overflow, invalid division and uninitialized-variable checks are language
guarantees in normal native code. Memory bounds, definedness, allocation
lifetime and leak checks are compiler debugging instrumentation. `debug`,
`tsan` and `msan` builds enable them; `release` emits no `bpp_s_*` calls and
links no native sanitizer object unless explicitly requested with
`--native-sanitizers`. This flag enables the portable checks with a release
compiler even when host ASAN/TSAN/MSAN runtimes are unavailable. The explicitly requested
`--strict-native-runtime` bootstrap profile keeps its helper-free contract and
does not emit these checks, even when the compiler itself is sanitized.

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
disabled only for this boundary object because assembly cannot populate it;
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

| Host / compiler | Local evidence for this change | Remaining gate |
|---|---|---|
| macOS ARM64 / Homebrew Clang 23 | ASAN/UBSAN and TSAN native fault suites pass | Hosted CI transcript |
| Linux x86-64 / Clang | CI includes debug, TSAN and MSAN native fault suites | Matching-runner execution |
| Linux ARM64 / Clang | Instrumented assembly assembles; CI includes debug/TSAN | Matching-runner execution |
| Windows x86-64 / Clang UCRT64 | Instrumented assembly assembles; CI runs release `check-native-checks` | Native portable checks and host sanitizer-runtime availability/execution |
| GCC-compatible toolchains | Portable boundary uses GCC-compatible ASAN/TSAN feature macros | Matching-toolchain execution |

MSAN is rejected on macOS, Windows and non-Clang toolchains. Standalone LSAN
availability is probed independently; the portable leak check does not depend
on standalone LSAN support.
