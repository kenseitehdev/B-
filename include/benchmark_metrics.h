#ifndef BPP_BENCHMARK_METRICS_H
#define BPP_BENCHMARK_METRICS_H
#include <stddef.h>
#include <stdint.h>
/* Compiler-private, single-threaded benchmark fixture API. These symbols are
 * present only in a separately compiled BPP_BENCHMARK_METRICS runtime object. */
typedef struct {
    uint64_t depth, max_depth, native_calls, boundary_helper_calls;
    uintptr_t lowest_stack, highest_stack;
} BPPBenchmarkMetrics;
void bpp_bench_begin(void);
BPPBenchmarkMetrics bpp_bench_end(void);
void bpp_bench_enter(const void *, size_t);
void bpp_bench_exit(void);
#endif
