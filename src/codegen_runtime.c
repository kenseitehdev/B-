#include "../include/codegen_abi.h"
#include "../include/native_runtime.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#ifdef BPP_BENCHMARK_METRICS
#include "../include/benchmark_metrics.h"
static BPPBenchmarkMetrics benchmark;
static int benchmarking;
void bpp_bench_begin(void){benchmark=(BPPBenchmarkMetrics){0};benchmarking=1;}
BPPBenchmarkMetrics bpp_bench_end(void){benchmarking=0;return benchmark;}
void bpp_bench_enter(const void *stack,size_t bytes){
    if(!benchmarking)return;
    uintptr_t low=(uintptr_t)stack,high=low+bytes;
    ++benchmark.native_calls;++benchmark.depth;
    if(benchmark.depth>benchmark.max_depth)benchmark.max_depth=benchmark.depth;
    if(!benchmark.lowest_stack||low<benchmark.lowest_stack)benchmark.lowest_stack=low;
    if(high>benchmark.highest_stack)benchmark.highest_stack=high;
}
void bpp_bench_exit(void){if(benchmarking&&benchmark.depth)--benchmark.depth;}
#define BPP_PROFILE_HELPER() do{if(benchmarking)++benchmark.boundary_helper_calls;}while(0)
#else
#define BPP_PROFILE_HELPER() ((void)0)
#endif


struct BPPNativeFrame {
    BLRuntime *vm;
    BLValue *globals, *registers, *locals, *args;
    unsigned char *global_initialized, *local_initialized;
    size_t register_count, local_count;
    void *call_output_scratch;
    const size_t *register_map;
    size_t cell_count;
    BLValue *output;
    size_t reserved;
    const size_t *local_map;
    size_t root_count;
};
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
static void native_msan_prepare(BPPNativeFrame *f){
    /* IR validation and local initialization flags establish native-defined
     * values. Assembly does not update MSAN's parameter or stack shadow. */
    __msan_unpoison(f,sizeof *f);
    __msan_unpoison(f->registers,f->cell_count*sizeof(BLValue));
    __msan_unpoison(f->locals,f->local_count*sizeof(BLValue));
    __msan_unpoison(f->local_initialized,f->local_count);
}
#else
static void native_msan_prepare(BPPNativeFrame *f){(void)f;}
#endif
static const BPPNativeRuntimeHelper native_helpers[] = {
    {"bpp_n_enter", "native emitter", "frame initialization", "none; parameter ownership uses bpp_a_clone_parameters", "cannot report language errors", "not applicable", 1},
    {"bpp_a_clone_parameters", "legacy/unit helper", "parameter ownership initialization", "clones borrowed incoming parameters into local slots", "cannot report language errors", "C runtime sanitizer instrumentation", 1},
    {"bpp_a_run", "legacy/unit helper", "startup; globals; process exit", "owns module result and globals", "prints one recorded runtime error", "C runtime sanitizer instrumentation", 1},
    {"bpp_a_op", "codegen_runtime.c + runtime.c", "dynamic values; slots; indexing; release", "moves owned result into destination", "records source-located runtime errors", "C runtime sanitizer instrumentation", 0},
    {"bpp_a_string", "codegen_runtime.c + value.c", "static-library literal fallback", "moves new owned string into destination", "allocation failure terminates host process", "C runtime sanitizer instrumentation", 0},
    {"bpp_a_destination", "codegen_runtime.c + runtime.c", "destination object cleanup", "releases previous destination object", "cannot report language errors", "C runtime sanitizer instrumentation", 0},
    {"bpp_a_builtin", "codegen_runtime.c + runtime.c", "builtin dispatch; allocation; output", "moves owned builtin result into destination", "records source-located runtime errors", "C runtime sanitizer instrumentation", 0},
    {"bpp_a_leave", "native emitter", "return-value transfer", "moves selected result to the caller slot", "cannot report language errors", "not applicable", 1},
    {"bpp_a_cleanup_frame", "legacy/unit helper", "frame root/local cleanup", "releases remaining owned roots and locals", "cannot report language errors", "C runtime sanitizer instrumentation", 1},
    {"bpp_a_release_object", "codegen_runtime.c + value.c", "object refcount/destructor", "releases one owned object reference", "cannot report language errors", "C runtime sanitizer instrumentation", 0},
    {"bpp_a_print_result", "codegen_runtime.c + runtime.c", "result formatting/output", "borrows completed result", "cannot report language errors", "C runtime sanitizer instrumentation", 0},
    {"bpp_a_report_error", "codegen_runtime.c", "diagnostic output", "borrows completed error string", "cannot report language errors", "C runtime sanitizer instrumentation", 0},
    {"bpp_a_allocate_zero", "codegen_runtime.c", "zeroed process-state allocation", "returns unowned max-align allocation", "returns null/ENOMEM; launcher terminates", "C runtime sanitizer instrumentation", 0},
    {"bpp_a_free_memory", "codegen_runtime.c", "process-state deallocation", "consumes allocation", "cannot report language errors", "C runtime sanitizer instrumentation", 0},
    {"bpp_a_allocation_failed", "codegen_runtime.c", "host allocation failure", "none", "terminates host process after perror", "C runtime sanitizer instrumentation", 0},
    {"bpp_a_access", "codegen_runtime.c", "instrumented global access", "borrows frame storage", "cannot report language errors", "ASAN/TSAN hooks", 0},
    {"bpp_a_trace_enter", "codegen_runtime.c", "procedure entry instrumentation", "none", "cannot report language errors", "TSAN hook", 0},
    {"bpp_a_trace_exit", "codegen_runtime.c", "procedure exit instrumentation", "none", "cannot report language errors", "TSAN hook", 0},
    {"bpp_a_arg", "native emitter", "outgoing argument cell copy", "borrows tag/payload into call area", "cannot report language errors", "not applicable", 1},
    {"bpp_a_args", "native emitter", "outgoing argument-base lookup", "borrows frame pointer", "cannot report language errors", "not applicable", 1},
    {"bpp_a_failed", "native emitter", "runtime-error probe", "borrows runtime pointer", "tests recorded error only", "not applicable", 1}
};
size_t bpp_native_runtime_helper_count(void) { return sizeof native_helpers / sizeof native_helpers[0]; }
const BPPNativeRuntimeHelper *bpp_native_runtime_helper_at(size_t index) {
    return index < bpp_native_runtime_helper_count() ? &native_helpers[index] : NULL;
}
static size_t slot(BPPNativeFrame *f,size_t r){return f->register_map?f->register_map[r]:r;}
static size_t local_slot(BPPNativeFrame *f,size_t l){return f->local_map?f->local_map[l]:l;}
static void *allocate(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) { fputs("B++: frame allocation overflow\n", stderr); exit(1); }
    void *p = calloc(n ? n : 1, size); if (!p) { perror("calloc"); exit(1); } return p;
}
BPP_HOSTCALL void bpp_a_truth_enable(BPPNativeFrame *f,int reset){ BPP_PROFILE_HELPER();native_msan_prepare(f);bl_rt_truth_enable(f->vm,reset);}
BPP_HOSTCALL int bpp_a_truthy(BPPNativeFrame *f,size_t reg){ BPP_PROFILE_HELPER();return bl_value_is_truthy(f->registers[slot(f,reg)]);}
BPP_HOSTCALL void bpp_a_call_begin(BPPNativeFrame *f){ BPP_PROFILE_HELPER();native_msan_prepare(f);f->reserved=(size_t)bl_rt_error_serial(f->vm);}
BPP_HOSTCALL void bpp_a_call_end(BPPNativeFrame *f,size_t dst){ BPP_PROFILE_HELPER();native_msan_prepare(f);BLValue *out=&f->registers[slot(f,dst)];if(out->tag==BLV_NULL&&f->reserved!=bl_rt_error_serial(f->vm))bl_rt_move(out,bl_rt_latest_error(f->vm));}
BPP_HOSTCALL void bpp_a_integer(BPPNativeFrame *f, size_t dst, long long v) { BPP_PROFILE_HELPER(); bl_rt_move(&f->registers[slot(f,dst)], bl_value_int(v)); }
BPP_HOSTCALL void bpp_a_string(BPPNativeFrame *f, size_t dst, const char *s) { BPP_PROFILE_HELPER(); bl_rt_move(&f->registers[slot(f,dst)], bl_value_string_copy(s)); }
BPP_HOSTCALL void bpp_a_clone_parameters(BPPNativeFrame *f, const BLValue *args, size_t count) { BPP_PROFILE_HELPER();
    native_msan_prepare(f);
#if __has_feature(memory_sanitizer)
    if(count)__msan_unpoison(args,count*sizeof(BLValue));
#endif
    for (size_t j = 0; j < count; ++j) {
        size_t local = local_slot(f, j);
        f->locals[local] = bl_value_clone(args[j]);
        f->local_initialized[local] = 1;
    }
}
BPP_HOSTCALL void bpp_a_arg(BPPNativeFrame *f, size_t arg, size_t reg) { BPP_PROFILE_HELPER(); f->args[arg] = f->registers[slot(f,reg)]; }
BPP_HOSTCALL BLValue * bpp_a_args(BPPNativeFrame *f) { BPP_PROFILE_HELPER(); return f->args; }
BPP_HOSTCALL BLValue * bpp_a_destination(BPPNativeFrame *f, size_t r) { BPP_PROFILE_HELPER(); bl_rt_move(&f->registers[slot(f,r)], bl_value_null()); return &f->registers[slot(f,r)]; }
BPP_HOSTCALL void bpp_a_builtin(BPPNativeFrame *f, size_t dst, const char *name, size_t argc, uint64_t line, int column) { BPP_PROFILE_HELPER();
    native_msan_prepare(f);
#if __has_feature(memory_sanitizer)
    if(argc)__msan_unpoison(f->args,argc*sizeof(BLValue));
#endif
    BLSite site = {(int)(uint32_t)line,column,(unsigned)(line>>32)}; bl_rt_move(&f->registers[slot(f,dst)], bl_rt_builtin(f->vm, &site, name, f->args, argc));
}
BPP_HOSTCALL int bpp_a_failed(BPPNativeFrame *f) { BPP_PROFILE_HELPER(); return f->vm->error != NULL; }
/* Value operations only: control flow and user calls are native instructions.
 * Inputs and destinations are compile-time constants in the emitted assembly. */
BPP_HOSTCALL void bpp_a_op(BPPNativeFrame *f, size_t op, size_t dst, size_t a, size_t b, size_t c, uint64_t line, int column) { BPP_PROFILE_HELPER();
    native_msan_prepare(f);
    BLSite site = {(int)(uint32_t)line,column,(unsigned)(line>>32)}; BLValue v = bl_value_null();unsigned long long serial=bl_rt_error_serial(f->vm);
    switch ((BLIROp)op) {
    case BL_IR_MALLOC:v=bl_rt_native_memory(f->vm,&site,BL_IR_MALLOC,f->registers[slot(f,a)],bl_value_null(),bl_value_null());break;
    case BL_IR_REMALLOC:case BL_IR_BYTE_LOAD:case BL_IR_BYTE_STORE:case BL_IR_PRINT_RANGE:
        v=bl_rt_native_memory(f->vm,&site,(BLIROp)op,f->registers[slot(f,a)],f->registers[slot(f,b)],c!=BL_IR_NONE?f->registers[slot(f,c)]:bl_value_null());if(dst==BL_IR_NONE){bl_value_release(v);return;}break;
    case BL_IR_MFREE_LOCAL:a=local_slot(f,a);bl_rt_free(f->vm,&site,f->locals[a],f->local_initialized[a],"local");return;
    case BL_IR_MFREE_GLOBAL:bl_rt_free(f->vm,&site,f->globals[a],f->global_initialized[a],"global");return;
    case BL_IR_PRINT_BYTE:if(f->registers[slot(f,a)].tag==BLV_INT)putchar((unsigned char)f->registers[slot(f,a)].as.i);return;
    case BL_IR_NULL: break;
    case BL_IR_LOAD_LOCAL: a=local_slot(f,a); v = bl_rt_load(f->vm, &site, f->locals[a], f->local_initialized[a], "local"); break;
    case BL_IR_LOAD_GLOBAL: v = bl_rt_load(f->vm, &site, f->globals[a], f->global_initialized[a], "global"); break;
    case BL_IR_INIT_LOCAL: case BL_IR_STORE_LOCAL:
        a=local_slot(f,a);bl_rt_store(f->vm, &site, &f->locals[a], &f->local_initialized[a], f->registers[slot(f,b)], op == BL_IR_INIT_LOCAL, "local"); return;
    case BL_IR_INIT_GLOBAL: case BL_IR_STORE_GLOBAL:
        bl_rt_store(f->vm, &site, &f->globals[a], &f->global_initialized[a], f->registers[slot(f,b)], op == BL_IR_INIT_GLOBAL, "global"); return;
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_BIT_NOT: v = bl_rt_unary(f->vm, &site, (BLIROp)op, f->registers[slot(f,a)]); break;
    case BL_IR_BIT_AND: case BL_IR_BIT_OR: case BL_IR_BIT_XOR:
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
        v = bl_rt_binary(f->vm, &site, (BLIROp)op, f->registers[slot(f,a)], f->registers[slot(f,b)]); break;
    case BL_IR_INDEX_GET: v = bl_rt_index(f->vm, &site, f->registers[slot(f,a)], f->registers[slot(f,b)]); break;
    case BL_IR_INDEX_SET: bl_rt_index_set(f->vm, &site, f->registers[slot(f,a)], f->registers[slot(f,b)], f->registers[slot(f,c)]); return;
    case BL_IR_TO_STRING_BUFFER: v = bl_rt_to_buffer(f->registers[slot(f,a)]); break;
    case BL_IR_RELEASE: bl_rt_move(&f->registers[slot(f,a)], v); return;
    case BL_IR_FREE_LOCAL: a=local_slot(f,a);bl_rt_free(f->vm, &site, f->locals[a], f->local_initialized[a], "local"); return;
    case BL_IR_FREE_GLOBAL: bl_rt_free(f->vm, &site, f->globals[a], f->global_initialized[a], "global"); return;
    case BL_IR_DELETE_LOCAL:
        a=local_slot(f,a);bl_rt_store(f->vm, &site, &f->locals[a], &f->local_initialized[a], v, 0, "local");f->local_initialized[a]=0; return;
    case BL_IR_DELETE_GLOBAL:
        bl_rt_store(f->vm, &site, &f->globals[a], &f->global_initialized[a], v, 0, "global");f->global_initialized[a]=0; return;
    default: fputs("B++: invalid native value operation\n", stderr); abort();
    }
    if(v.tag==BLV_NULL&&bl_rt_error_serial(f->vm)!=serial)v=bl_rt_latest_error(f->vm);
    bl_rt_move(&f->registers[slot(f,dst)], v);
}
BPP_HOSTCALL void bpp_a_cleanup_frame(BPPNativeFrame *f) { BPP_PROFILE_HELPER();
    native_msan_prepare(f);
    for (size_t j = 0; j < f->root_count; ++j) bl_value_release(f->registers[j]);
    for (size_t j = 0; j < f->local_count; ++j) bl_value_release(f->locals[j]);
}
BPP_HOSTCALL void bpp_a_release_object(BLObject *object) { BPP_PROFILE_HELPER();
    if (!object) return;
    BLValue value = bl_value_null();
    value.tag = BLV_OBJECT;
    value.as.obj = object;
    bl_value_release(value);
}
BPP_HOSTCALL void bpp_a_print_result(const BLValue *value) { BPP_PROFILE_HELPER();
    if (value) bl_rt_print_result(*value);
}
BPP_HOSTCALL void bpp_a_report_error(const char *error) { BPP_PROFILE_HELPER();
    if (error) fprintf(stderr, "%s\n", error);
}
BPP_HOSTCALL void *bpp_a_allocate_zero(size_t count, size_t size) { BPP_PROFILE_HELPER();
    if (size && count > SIZE_MAX / size) { errno = ENOMEM; return NULL; }
    return calloc(count ? count : 1, size ? size : 1);
}
BPP_HOSTCALL void bpp_a_free_memory(void *memory) { BPP_PROFILE_HELPER(); free(memory); }
BPP_HOSTCALL void bpp_a_allocation_failed(void) { BPP_PROFILE_HELPER(); perror("calloc"); exit(1); }
BPP_HOSTCALL void bpp_a_leave(BPPNativeFrame *f, size_t reg, BLValue *out) { BPP_PROFILE_HELPER();
    *out = reg == BL_IR_NONE ? bl_value_null() : f->registers[slot(f,reg)];
    if (reg != BL_IR_NONE) f->registers[slot(f,reg)] = bl_value_null();
    bpp_a_cleanup_frame(f);
}
BPP_HOSTCALL int bpp_a_run(BPPNativeProc entry, const char *path, size_t count, int print_result) { BPP_PROFILE_HELPER();
    BLRuntime vm = {path, NULL}; BLValue result = bl_value_null();
    BLValue *g = allocate(count, sizeof *g); unsigned char *gi = allocate(count, 1);
    entry(&vm, g, gi, NULL, &result); int status = vm.error ? 1 : 0;
    if (vm.error) fprintf(stderr, "%s\n", vm.error); else if (print_result) bl_rt_print_result(result);
    bl_value_release(result);
    for (size_t j = 0; j < count; ++j) bl_value_release(g[j]);
    free(g); free(gi); free(vm.error); return status;
}

_Static_assert(sizeof(BPPNativeFrame) == BPP_CG_HEADER_BYTES, "private frame header layout changed");
_Static_assert(offsetof(BPPNativeFrame, vm) == BPP_CG_FRAME_VM_OFFSET &&
    offsetof(BPPNativeFrame, globals) == BPP_CG_FRAME_GLOBALS_OFFSET &&
    offsetof(BPPNativeFrame, registers) == BPP_CG_FRAME_REGISTERS_OFFSET &&
    offsetof(BPPNativeFrame, locals) == BPP_CG_FRAME_LOCALS_OFFSET &&
    offsetof(BPPNativeFrame, args) == BPP_CG_FRAME_ARGS_OFFSET &&
    offsetof(BPPNativeFrame, global_initialized) == BPP_CG_FRAME_GLOBAL_INITIALIZED_OFFSET &&
    offsetof(BPPNativeFrame, local_initialized) == BPP_CG_FRAME_LOCAL_INITIALIZED_OFFSET &&
    offsetof(BPPNativeFrame, register_count) == BPP_CG_FRAME_REGISTER_COUNT_OFFSET &&
    offsetof(BPPNativeFrame, local_count) == BPP_CG_FRAME_LOCAL_COUNT_OFFSET &&
    offsetof(BPPNativeFrame, register_map) == BPP_CG_FRAME_REGISTER_MAP_OFFSET &&
    offsetof(BPPNativeFrame, cell_count) == BPP_CG_FRAME_CELL_COUNT_OFFSET &&
    offsetof(BPPNativeFrame, output) == BPP_CG_FRAME_OUTPUT_OFFSET,
    "private native frame offsets changed");
_Static_assert(offsetof(BPPNativeFrame, call_output_scratch) == BPP_CG_CALL_OUTPUT_OFFSET,
    "private call output scratch offset changed");
_Static_assert(offsetof(BPPNativeFrame, local_map) == BPP_CG_LOCAL_MAP_OFFSET && offsetof(BPPNativeFrame, root_count) == BPP_CG_ROOT_COUNT_OFFSET,
    "private local/root metadata offsets changed");
_Static_assert(sizeof(BLValue) == BPP_CG_VALUE_BYTES && offsetof(BLValue, tag) == 0 && offsetof(BLValue, as) == 8,
    "private value layout changed");
_Static_assert(offsetof(BLRuntime, error) == BPP_CG_RUNTIME_ERROR_OFFSET,
    "private runtime error offset changed");
_Static_assert(offsetof(BLObject, ref_count) == BPP_CG_OBJECT_REF_COUNT_OFFSET,
    "private object reference-count offset changed");
BPP_HOSTCALL BPPNativeFrame * bpp_n_enter(BLRuntime *vm, BLValue *g, unsigned char *gi,
    const BLValue *args, const BPPNativeLayout *layout, void *storage) { BPP_PROFILE_HELPER();
    unsigned char *base = storage;
    size_t cells=layout->cells;
    size_t locals_offset = BPP_CG_HEADER_BYTES + cells * BPP_CG_VALUE_BYTES;
    size_t flags_offset = locals_offset + layout->local_slots * BPP_CG_VALUE_BYTES;
    size_t args_offset = (flags_offset + layout->local_slots + 15) & ~(size_t)15;
    size_t total = args_offset + layout->call_args * BPP_CG_VALUE_BYTES;
    /* Sizes originate in overflow-checked emitted layout metadata. */
    for (size_t j = 0; j < total; ++j) base[j] = 0;
    BPPNativeFrame *f = storage;
    f->vm = vm; f->globals = g; f->global_initialized = gi;
    f->registers = (BLValue *)(base + BPP_CG_HEADER_BYTES);
    f->locals = (BLValue *)(base + locals_offset);
    f->local_initialized = base + flags_offset;
    f->args = (BLValue *)(base + args_offset);
    f->register_count = layout->registers; f->local_count = layout->local_slots;
    f->cell_count=cells;f->root_count=layout->roots;
    const size_t *maps=(const size_t *)(layout+1);
    f->register_map=(layout->map_flags&BPP_LAYOUT_REGISTER_MAP)?maps:NULL;
    if(layout->map_flags&BPP_LAYOUT_REGISTER_MAP)maps+=layout->registers;
    f->local_map=(layout->map_flags&BPP_LAYOUT_LOCAL_MAP)?maps:NULL;
    bpp_a_clone_parameters(f, args, layout->params);
    return f;
}

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
extern void __asan_load16(const void *);extern void __asan_store16(const void *);
extern void __asan_load1(const void *);extern void __asan_store1(const void *);
#endif
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
extern void __tsan_read16(const void *);extern void __tsan_write16(const void *);
extern void __tsan_read1(const void *);extern void __tsan_write1(const void *);
extern void __tsan_func_entry(const void *);extern void __tsan_func_exit(void);
#define BPP_NO_THREAD __attribute__((no_sanitize_thread))
#else
#define BPP_NO_THREAD
#endif
BPP_HOSTCALL void bpp_a_access(BPPNativeFrame *f,size_t op,size_t index){ BPP_PROFILE_HELPER();
    int write=op!=BL_IR_LOAD_GLOBAL,init=op==BL_IR_INIT_GLOBAL;
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
    if(write)__asan_store16(&f->globals[index]);else __asan_load16(&f->globals[index]);
    if(init)__asan_store1(&f->global_initialized[index]);else __asan_load1(&f->global_initialized[index]);
#endif
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
    if(write)__tsan_write16(&f->globals[index]);else __tsan_read16(&f->globals[index]);
    if(init)__tsan_write1(&f->global_initialized[index]);else __tsan_read1(&f->global_initialized[index]);
#endif
    (void)f;(void)index;(void)write;(void)init;
}
BPP_NO_THREAD BPP_HOSTCALL void bpp_a_trace_enter(const void *caller){ BPP_PROFILE_HELPER();
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
    __tsan_func_entry(caller);
#else
    (void)caller;
#endif
}
BPP_NO_THREAD BPP_HOSTCALL void bpp_a_trace_exit(void){ BPP_PROFILE_HELPER();
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
    __tsan_func_exit();
#endif
}
