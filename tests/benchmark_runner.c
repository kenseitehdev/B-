#define _POSIX_C_SOURCE 200809L
#include "bpp_abi.h"
#include "benchmark_metrics.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BPP_BENCHMARK_METRICS
#ifdef _WIN32
#include <windows.h>
static double now(void){LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return (double)t.QuadPart/(double)f.QuadPart;}
#else
#include <time.h>
static double now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))exit(1);return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
#endif
#endif
extern const BPPStaticLibrary bpp_v2_library_bench;
static const BPPStaticProcedure *work;
static int invoke(long long n,long long expected){
    BLRuntime vm={"benchmark",NULL};BLValue a=bl_value_int(n),out=bl_value_null();
    int ok=bpp_static_invoke(work,&vm,NULL,NULL,&a,1,&out)==BPP_STATIC_OK&&out.tag==BLV_INT&&out.as.i==expected;
    bl_value_release(out);return ok;
}
static long long number(const char *s){char *end;errno=0;long long n=strtoll(s,&end,10);if(errno||*end){fputs("bad numeric argument\n",stderr);exit(2);}return n;}
int main(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"startup"))return 0;
    char error[256];if(!bpp_static_library_validate(&bpp_v2_library_bench,error,sizeof error)){fputs(error,stderr);return 3;}
    for(size_t i=0;i<bpp_v2_library_bench.procedure_count;++i)if(!strcmp(bpp_v2_library_bench.procedures[i].name,"work"))work=&bpp_v2_library_bench.procedures[i];
    if(!work||argc<4)return 2;
    long long n=number(argv[2]),expected=number(argv[3]);
#ifdef BPP_BENCHMARK_METRICS
    if(argc!=4||strcmp(argv[1],"metrics"))return 2;
    bpp_bench_begin();int ok=invoke(n,expected);BPPBenchmarkMetrics m=bpp_bench_end();
    if(!ok||m.depth||!m.max_depth)return 3;
    printf("{\"max_native_call_depth\":%llu,\"native_frame_stack_span_bytes\":%llu,\"native_calls\":%llu,\"boundary_helper_calls\":%llu}\n",
        (unsigned long long)m.max_depth,(unsigned long long)(m.highest_stack-m.lowest_stack),
        (unsigned long long)m.native_calls,(unsigned long long)m.boundary_helper_calls);
#else
    if(argc!=7||strcmp(argv[1],"timing"))return 2;
    long long warmup=number(argv[4]),samples=number(argv[5]),runs=number(argv[6]);
    if(warmup<0||warmup>1000||samples<1||samples>1000||runs<1||runs>10000)return 2;
    for(long long i=0;i<warmup;++i)if(!invoke(n,expected))return 3;
    for(long long sample=0;sample<samples;++sample){double start=now();for(long long i=0;i<runs;++i)if(!invoke(n,expected))return 3;printf("%.9f\n",(now()-start)*1000.0/(double)runs);}
#endif
    return 0;
}
