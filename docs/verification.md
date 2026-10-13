# Native verification and release gates (1.0.16)

Run `make BUILD_MODE=release test check-static-abi check-native-checks
check-differential check-verification check-fuzz check-reproducible check-stress`
for native release validation. Repeat the native, fuzz, differential, stress
and reproducibility checks in debug/TSAN, and use Linux/Clang for MSAN. Python 3
is required for bounded test execution and reproducer retention.

`check-verification` exercises 64 parameters, 300 live recursive frames, 512
locals, 256 branches, several frame sizes and intentional stack exhaustion.
Every positive fixture runs optimized and with `--no-opt`. Stack exhaustion
must terminate with a stack fault; a deadline is a failure, not evidence.
Windows uses its native stack limit; Unix tests set a 512 KiB limit.

The independent seeded evaluator covers arithmetic, comparisons, signed
quotient/remainder, branches, loops, procedure calls, recursion, aliased byte
buffers and reallocations. Raw source has no managed strings/arrays; unsupported
constructs are rejected-input probes rather than fictitious runtime features.
On differential failure the driver records seed/mode, input, expected/actual
output, stderr and a smaller valid expression-depth reproducer. Fuzz failures
retain the original input and a bounded byte-reduced input preserving the exit
class. Reduction is best effort within a fixed budget, not a global-minimum
claim. Artifacts are written to `build/verification-failures` (override with
`BPP_FAILURE_DIR`).

`check-fuzz` runs valid, mutated and rejected input through lexer/parser,
lowering, baseline/optimized IR validation and each native emitter. It also
corrupts a valid instruction destination and requires verifier rejection.
Each input runs in a separate process with wall/CPU/output limits. Large
stress compilations receive 120 CPU seconds and a 180-second wall deadline;
fuzz probes and native execution keep their smaller 20-second CPU budget. ASAN/MSAN
virtual-address space is intentionally not capped because their shadow maps
require large reservations. CI compiles the compiler/runtime with warnings as
errors separately from test-fixture warning policies.

`check-reproducible` compares repeated assembly and object bytes for all four
targets, with fixed paths and toolchain, in optimized/baseline/debug modes.
Debug info embeds source paths and compilation directories, so moving a build
can change bytes. Executable build IDs, Mach-O UUIDs, PE/PDB timestamps and
identifiers, archive headers and compiler-generated runtime debug information
are controlled by the host assembler/linker/archiver. No universal bit-for-bit
executable or cross-toolchain identity is claimed.

Linux LLVM verification targets compiler-owned DWARF in the generated native
object. GCC/compiler-rt C units can contain inlined sanitizer call-site records
rejected by LLVM, and older LLVM verifiers can stall on those units; verifying
unrelated C-toolchain debug info is not a B++ release gate. Leak checking remains a separate gate outside debugger subprocesses; ptrace
debugger runs disable leak tracing and have 60-second command deadlines.
Linked B++ DIEs,
location lists and CFI remain inspected with readelf and exercised in native
GDB, including local values and imported recursive frames. macOS verifies the
linked dSYM and uses LLDB. Windows verifies COFF/PDB records and native LLDB
values/stepping. Adjacent initialized locations with the same canonical frame
home are coalesced within each block; deletion/initialization gaps remain.

The static ABI gate builds separate archives and consumers, transitive
dependencies, arity/export checks and manifest/archive identity checks on each
native release runner. Existing opcode values are part of compiler-private
runtime compatibility; new instructions must not renumber existing operations.
The public static ABI remains versioned independently.

CI distinguishes compiler/linker availability, runtime smoke, cross-assembly
and native execution. Required host sanitizer probe failures fail the selected
profile. Unsupported probes are recorded explicitly. Commands and jobs have
wall deadlines; superseded PR runs are canceled. Retained transcripts record
exact revision, host architecture, compiler and mode. Only successful matching
runs on the final implementation close the platform release gates.

Release 1.0.16 closes these gates with all 18 jobs / 19 execution profiles in
[native CI run 47](https://github.com/kenseitehdev/B-/actions/runs/38329351396),
implementation `2c817e176897d0addfd7df624b01d4e136ac1f57`.
[verification-1.0.16.json](verification-1.0.16.json) retains exact job IDs,
toolchain identity, sanitizer probe results and transcript artifact digests.
Hosted macOS TSAN has a 35-minute overall job limit to accommodate its complete
sequential suite; native/fuzz/stress commands retain 600-second deadlines; its full 64-seed
differential suite has a 900-second deadline and their per-process resource limits. Other Unix jobs keep a
15-minute overall limit. Unsupported sanitizer flavors remain explicit results.

The 1.0.18 platform gate (`check-platform`) inspects actual cross-target object
bytes and matching-host paths/identity safeguards. See [platform-support.md](platform-support.md).
Object inspection evidence is retained separately from matching-host execution
and does not promote cross-assembled targets to native verified.
