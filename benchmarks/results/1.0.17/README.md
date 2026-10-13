# 1.0.17 native benchmark evidence

The six CSV/JSON pairs in `ci/` are unmodified report files from
[Bake native CI run 7](https://github.com/kenseitehdev/B-/actions/runs/38430265880),
implementation `d73a77a92bea735257fcc6435efa8d2d7568141c`.
Each runner checks eight workloads in both optimization modes, with one
warmup, three samples and two work invocations per sample. All 96 case/mode
results pass the schema and semantic checks. All 288 per-process compile
memory probes returned values. ZIP artifact identities/digests are recorded in
`docs/verification-1.0.17.json`.

Hosts: macOS ARM64/Clang, Linux x86-64 and ARM64 with Clang/GCC, and Windows
x86-64/UCRT64 Clang. Windows uses a UTF-8 process manifest, LLVM archives and
LLD executable linking. Shared CI measurements are harness evidence, not
isolated hardware baselines or speed guarantees. Native frame spans exclude
C helper stack usage; emitted spill/reload sites are static counts.

Two sequential local Apple M3 Pro reports additionally passed with seven
samples, three warmups and eight work invocations per sample, matching compiler,
runtime and workload hashes. Their local files are under
`build/benchmarks/1.0.17/local-current-{1,2}.{json,csv}`; they are kept separate
from the hosted-runner reports.

See [the measurement contract](../../../docs/benchmarking.md). Revalidate any
report with `python3 tests/check_benchmark.py PATH_WITHOUT_EXTENSION`.
