# Native timing harness

These older C-backend measurements are historical. The C output backend was
retired in 1.0.15; current comparisons use native optimized and baseline output.

Run `make -s BUILD_MODE=release benchmark > timings.csv` after a release build.
The C clock wrapper uses a monotonic host clock and measures five process runs
following one warmup. BPP_BENCHMARK_RUNS changes that count. Each workload includes
an assertion so the measured computation cannot simply disappear. Results report
executable bytes and mean milliseconds including process startup. This is a small
comparison harness, not a statistically controlled benchmark framework.

An initial Linux x86-64 run in this session gave:

| Workload | C backend ms | Native ms | Native without IR optimization ms |
|---|---:|---:|---:|
| 200000 integer iterations | 16.74 | 3.35 | 5.17 |
| 10000 procedure calls | 2.95 | 1.50 | 1.59 |
| 10000 array workloads | 8.50 | 6.70 | 7.50 |

These measurements ran concurrently with validation and have no stable hardware
baseline. They are useful as a harness smoke check, not a performance guarantee
or evidence of those gains on the other targets. Register/cell allocation runs
in both native modes; --no-opt disables only IR optimization. Actual-host results
and isolated repeated runs remain a codegen gate.
