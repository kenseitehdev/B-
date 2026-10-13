# Native benchmarking (1.0.17)

Build an unsanitized compiler with `make BUILD_MODE=release`, then run
`make BUILD_MODE=release benchmark-native`. The portable Python driver writes
schema-2 CSV and JSON to `build/benchmarks/1.0.17/native`. The older schema-1
instruction-selection benchmark and historical C comparisons remain available.

```sh
python3 tools/benchmark.py --samples 7 --warmup 3 --repetitions 8 \
  --iterations 2048 --machine-id my-stable-machine \
  --output build/benchmarks/1.0.17/my-run
python3 tests/check_benchmark.py build/benchmarks/1.0.17/my-run
```

Sampling bounds and command deadlines prevent accidental unbounded runs.
Commands run sequentially, with no parallel benchmark workloads. Dedicated
idle hardware gives stronger evidence than shared CI; runner results validate
the harness and capture measurements, rather than certify speed improvements.
Set a stable machine ID explicitly for repeated hardware baselines. Otherwise
the driver hashes hostname/architecture as a host fingerprint; this is not a
hardware serial or a guarantee across renamed/replaced hosts. CPU, OS, target
triple, tool identity, compiler binary/revision and dirty status, runtime-object
hashes, source, flags and exact measurement commands are retained. Revision
identity comes from GitHub Actions or an available Git checkout; outside both,
revision/dirty status are explicitly unavailable while binary hashes remain.

## Measurement boundaries

Every case runs optimized and `--no-opt`. The latter disables IR optimization;
register allocation and legal instruction selection still run. Each generated
library exports `work(n)` through static ABI 2, and a C fixture checks its
returned integer against an independently calculated Python result.

* Compile wall time: compiler startup, parse/import/lowering, IR optimization,
  allocation, native generation and assembly-text output; no assembler/linker.
* Assembly wall time: host driver startup and generation of one native object.
* Link wall time: host driver and link of already compiled fixture/runtime/native
  objects. Fixture C compilation is outside the timed link phase.
* Startup wall time: process creation, CRT initialization and immediate driver
  exit for that linked image. It does not include an invocation of `work`.
* Steady ABI work: repeated `work(n)` invocations within one process after
  warmup. This includes the static-ABI boundary, result check and ownership
  release, without process startup. No subtraction of two noisy timings is used.

Compile/assembly/link/startup phases each have warmups and repeated samples.
In-process work has its own warmups, repetitions per sample, and samples. JSON
retains raw values, median, median absolute deviation, min and max. Per-process
compile peak RSS is collected with `wait4` on Unix (units normalized to bytes)
or a Windows process-memory query. Unavailable probes are null, never zero.
Python process timing includes polling/wait overhead, especially relevant to
very short commands. Hardware cycle counters are not inferred from wall time.

Generated object text/read-only-data bytes and executable/object/assembly sizes
are recorded with the host size tool. Layout maps retain per-function frames.
Assembly allocation comments provide actual emitted spill, reload and
rematerialization **site** counts. These are static counts, not dynamic traffic.

A separate untimed profiling binary uses `--benchmark-metrics` library assembly
and a C boundary object compiled with `BPP_BENCHMARK_METRICS`. Native entry/exit
hooks record actual maximum generated-function call depth, frame-stack address
span and entry count. The separately compiled C boundary counts actual helper
invocations. Frame span excludes deeper C-helper stack frames and process-wide
stack high-water usage. Timing assembly has no entry/exit profiling hooks; the
ordinary runtime compiles its counter macros away. Profiling does not change
the public ABI or introduce a language-facing FFI/profiling feature.

## Cases and policy

Eight cases cover integers, branches, calls, recursion, owned fractional states,
byte buffers, byte-string processing and a 1024-update source unit. Recursion
checks measured depth against its known input-driven depth. All timing and
profiling invocations verify semantic results. Managed string/Array source
constructors are retired, so the report records those cases as unsupported
and measures the supported byte/state objects instead. A future custom
assembler/linker is also explicitly unavailable in current comparisons.

Native CI collects the same report on macOS ARM64, Linux x86-64/ARM64 with
Clang/GCC, and Windows x86-64. Schema/semantic checks fail on invalid results;
there are no speed-ratio pass/fail thresholds on shared runners. Establish
expectations only after repeated isolated runs with matching hardware,
toolchain and options, preserving older reports rather than replacing them.
