#include "../include/codegen_abi.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>


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
static size_t slot(BPPNativeFrame *f,size_t r){return f->register_map?f->register_map[r]:r;}
static size_t local_slot(BPPNativeFrame *f,size_t l){return f->local_map?f->local_map[l]:l;}
static void *allocate(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) { fputs("B++: frame allocation overflow\n", stderr); exit(1); }
    void *p = calloc(n ? n : 1, size); if (!p) { perror("calloc"); exit(1); } return p;
}
BPP_HOSTCALL void bpp_a_integer(BPPNativeFrame *f, size_t dst, long long v) { bl_rt_move(&f->registers[slot(f,dst)], bl_value_int(v)); }
BPP_HOSTCALL void bpp_a_string(BPPNativeFrame *f, size_t dst, const char *s) { bl_rt_move(&f->registers[slot(f,dst)], bl_value_string_copy(s)); }
BPP_HOSTCALL void bpp_a_arg(BPPNativeFrame *f, size_t arg, size_t reg) { f->args[arg] = f->registers[slot(f,reg)]; }
BPP_HOSTCALL BLValue * bpp_a_args(BPPNativeFrame *f) { return f->args; }
BPP_HOSTCALL BLValue * bpp_a_destination(BPPNativeFrame *f, size_t r) { bl_rt_move(&f->registers[slot(f,r)], bl_value_null()); return &f->registers[slot(f,r)]; }
BPP_HOSTCALL void bpp_a_builtin(BPPNativeFrame *f, size_t dst, const char *name, size_t argc, int line, int column) {
    BLSite site = {line, column}; bl_rt_move(&f->registers[slot(f,dst)], bl_rt_builtin(f->vm, &site, name, f->args, argc));
}
BPP_HOSTCALL int bpp_a_failed(BPPNativeFrame *f) { return f->vm->error != NULL; }
/* Value operations only: control flow and user calls are native instructions.
 * Inputs and destinations are compile-time constants in the emitted assembly. */
BPP_HOSTCALL void bpp_a_op(BPPNativeFrame *f, size_t op, size_t dst, size_t a, size_t b, size_t c, int line, int column) {
    BLSite site = {line, column}; BLValue v = bl_value_null();
    switch ((BLIROp)op) {
    case BL_IR_NULL: break;
    case BL_IR_LOAD_LOCAL: a=local_slot(f,a); v = bl_rt_load(f->vm, &site, f->locals[a], f->local_initialized[a], "local"); break;
    case BL_IR_LOAD_GLOBAL: v = bl_rt_load(f->vm, &site, f->globals[a], f->global_initialized[a], "global"); break;
    case BL_IR_INIT_LOCAL: case BL_IR_STORE_LOCAL:
        a=local_slot(f,a);bl_rt_store(f->vm, &site, &f->locals[a], &f->local_initialized[a], f->registers[slot(f,b)], op == BL_IR_INIT_LOCAL, "local"); return;
    case BL_IR_INIT_GLOBAL: case BL_IR_STORE_GLOBAL:
        bl_rt_store(f->vm, &site, &f->globals[a], &f->global_initialized[a], f->registers[slot(f,b)], op == BL_IR_INIT_GLOBAL, "global"); return;
    case BL_IR_NEG: case BL_IR_NOT: v = bl_rt_unary(f->vm, &site, (BLIROp)op, f->registers[slot(f,a)]); break;
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
        a=local_slot(f,a);bl_rt_store(f->vm, &site, &f->locals[a], &f->local_initialized[a], v, 0, "local"); return;
    case BL_IR_DELETE_GLOBAL:
        bl_rt_store(f->vm, &site, &f->globals[a], &f->global_initialized[a], v, 0, "global"); return;
    default: fputs("B++: invalid native value operation\n", stderr); abort();
    }
    bl_rt_move(&f->registers[slot(f,dst)], v);
}
BPP_HOSTCALL void bpp_a_leave(BPPNativeFrame *f, size_t reg, BLValue *out) {
    *out = reg == BL_IR_NONE ? bl_value_null() : f->registers[slot(f,reg)];
    if (reg != BL_IR_NONE) f->registers[slot(f,reg)] = bl_value_null();
    for (size_t j = 0; j < f->root_count; ++j) bl_value_release(f->registers[j]);
    for (size_t j = 0; j < f->local_count; ++j) bl_value_release(f->locals[j]);
}
BPP_HOSTCALL int bpp_a_run(BPPNativeProc entry, const char *path, size_t count, int print_result) {
    BLRuntime vm = {path, NULL}; BLValue result = bl_value_null();
    BLValue *g = allocate(count, sizeof *g); unsigned char *gi = allocate(count, 1);
    entry(&vm, g, gi, NULL, &result); int status = vm.error ? 1 : 0;
    if (vm.error) fprintf(stderr, "%s\n", vm.error); else if (print_result) bl_rt_print_result(result);
    bl_value_release(result);
    for (size_t j = 0; j < count; ++j) bl_value_release(g[j]);
    free(g); free(gi); free(vm.error); return status;
}

_Static_assert(sizeof(BPPNativeFrame) == BPP_CG_HEADER_BYTES, "private frame header layout changed");
_Static_assert(offsetof(BPPNativeFrame, call_output_scratch) == BPP_CG_CALL_OUTPUT_OFFSET,
    "private call output scratch offset changed");
_Static_assert(offsetof(BPPNativeFrame, local_map) == BPP_CG_LOCAL_MAP_OFFSET && offsetof(BPPNativeFrame, root_count) == BPP_CG_ROOT_COUNT_OFFSET,
    "private local/root metadata offsets changed");
_Static_assert(sizeof(BLValue) == BPP_CG_VALUE_BYTES && offsetof(BLValue, tag) == 0 && offsetof(BLValue, as) == 8,
    "private value layout changed");
BPP_HOSTCALL BPPNativeFrame * bpp_n_enter(BLRuntime *vm, BLValue *g, unsigned char *gi,
    const BLValue *args, const BPPNativeLayout *layout, void *storage) {
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
    for (size_t j = 0; j < layout->params; ++j) { size_t local=local_slot(f,j);f->locals[local] = bl_value_clone(args[j]); f->local_initialized[local] = 1; }
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
BPP_HOSTCALL void bpp_a_access(BPPNativeFrame *f,size_t op,size_t index){
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
BPP_NO_THREAD BPP_HOSTCALL void bpp_a_trace_enter(const void *caller){
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
    __tsan_func_entry(caller);
#else
    (void)caller;
#endif
}
BPP_NO_THREAD BPP_HOSTCALL void bpp_a_trace_exit(void){
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
    __tsan_func_exit();
#endif
}
