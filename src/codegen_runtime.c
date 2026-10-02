#include "../include/codegen_abi.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

struct BPPArmFrame {
    BLRuntime *vm;
    BLValue *globals, *registers, *locals, *args;
    unsigned char *global_initialized, *local_initialized;
    size_t register_count, local_count;
    size_t reserved;
};
static void *allocate(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) { fputs("B++: frame allocation overflow\n", stderr); exit(1); }
    void *p = calloc(n ? n : 1, size); if (!p) { perror("calloc"); exit(1); } return p;
}
void bpp_a_integer(BPPArmFrame *f, size_t dst, long long v) { bl_rt_move(&f->registers[dst], bl_value_int(v)); }
void bpp_a_string(BPPArmFrame *f, size_t dst, const char *s) { bl_rt_move(&f->registers[dst], bl_value_string_copy(s)); }
void bpp_a_arg(BPPArmFrame *f, size_t arg, size_t reg) { f->args[arg] = f->registers[reg]; }
BLValue *bpp_a_args(BPPArmFrame *f) { return f->args; }
BLValue *bpp_a_destination(BPPArmFrame *f, size_t r) { bl_rt_move(&f->registers[r], bl_value_null()); return &f->registers[r]; }
void bpp_a_builtin(BPPArmFrame *f, size_t dst, const char *name, size_t argc, int line, int column) {
    BLSite site = {line, column}; bl_rt_move(&f->registers[dst], bl_rt_builtin(f->vm, &site, name, f->args, argc));
}
int bpp_a_failed(BPPArmFrame *f) { return f->vm->error != NULL; }
/* Value operations only: control flow and user calls are native instructions.
 * Inputs and destinations are compile-time constants in the emitted assembly. */
void bpp_a_op(BPPArmFrame *f, size_t op, size_t dst, size_t a, size_t b, size_t c, int line, int column) {
    BLSite site = {line, column}; BLValue v = bl_value_null();
    switch ((BLIROp)op) {
    case BL_IR_NULL: break;
    case BL_IR_LOAD_LOCAL: v = bl_rt_load(f->vm, &site, f->locals[a], f->local_initialized[a], "local"); break;
    case BL_IR_LOAD_GLOBAL: v = bl_rt_load(f->vm, &site, f->globals[a], f->global_initialized[a], "global"); break;
    case BL_IR_INIT_LOCAL: case BL_IR_STORE_LOCAL:
        bl_rt_store(f->vm, &site, &f->locals[a], &f->local_initialized[a], f->registers[b], op == BL_IR_INIT_LOCAL, "local"); return;
    case BL_IR_INIT_GLOBAL: case BL_IR_STORE_GLOBAL:
        bl_rt_store(f->vm, &site, &f->globals[a], &f->global_initialized[a], f->registers[b], op == BL_IR_INIT_GLOBAL, "global"); return;
    case BL_IR_NEG: case BL_IR_NOT: v = bl_rt_unary(f->vm, &site, (BLIROp)op, f->registers[a]); break;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
        v = bl_rt_binary(f->vm, &site, (BLIROp)op, f->registers[a], f->registers[b]); break;
    case BL_IR_INDEX_GET: v = bl_rt_index(f->vm, &site, f->registers[a], f->registers[b]); break;
    case BL_IR_INDEX_SET: bl_rt_index_set(f->vm, &site, f->registers[a], f->registers[b], f->registers[c]); return;
    case BL_IR_TO_STRING_BUFFER: v = bl_rt_to_buffer(f->registers[a]); break;
    case BL_IR_RELEASE: bl_rt_move(&f->registers[a], v); return;
    case BL_IR_FREE_LOCAL: bl_rt_free(f->vm, &site, f->locals[a], f->local_initialized[a], "local"); return;
    case BL_IR_FREE_GLOBAL: bl_rt_free(f->vm, &site, f->globals[a], f->global_initialized[a], "global"); return;
    case BL_IR_DELETE_LOCAL:
        bl_rt_store(f->vm, &site, &f->locals[a], &f->local_initialized[a], v, 0, "local"); return;
    case BL_IR_DELETE_GLOBAL:
        bl_rt_store(f->vm, &site, &f->globals[a], &f->global_initialized[a], v, 0, "global"); return;
    default: fputs("B++: invalid native value operation\n", stderr); abort();
    }
    bl_rt_move(&f->registers[dst], v);
}
void bpp_a_leave(BPPArmFrame *f, size_t reg, BLValue *out) {
    *out = reg == BL_IR_NONE ? bl_value_null() : f->registers[reg];
    if (reg != BL_IR_NONE) f->registers[reg] = bl_value_null();
    for (size_t j = 0; j < f->register_count; ++j) bl_value_release(f->registers[j]);
    for (size_t j = 0; j < f->local_count; ++j) bl_value_release(f->locals[j]);
}
int bpp_a_run(BPPArmProc entry, const char *path, size_t count, int print_result) {
    BLRuntime vm = {path, NULL}; BLValue result = bl_value_null();
    BLValue *g = allocate(count, sizeof *g); unsigned char *gi = allocate(count, 1);
    entry(&vm, g, gi, NULL, &result); int status = vm.error ? 1 : 0;
    if (vm.error) fprintf(stderr, "%s\n", vm.error); else if (print_result) bl_rt_print_result(result);
    bl_value_release(result);
    for (size_t j = 0; j < count; ++j) bl_value_release(g[j]);
    free(g); free(gi); free(vm.error); return status;
}

_Static_assert(sizeof(BPPArmFrame) == BPP_CG_HEADER_BYTES, "private frame header layout changed");
_Static_assert(sizeof(BLValue) == BPP_CG_VALUE_BYTES && offsetof(BLValue, tag) == 0 && offsetof(BLValue, as) == 8,
    "private value layout changed");
BPPArmFrame *bpp_n_enter(BLRuntime *vm, BLValue *g, unsigned char *gi,
    const BLValue *args, const BPPNativeLayout *layout, void *storage) {
    unsigned char *base = storage;
    size_t locals_offset = BPP_CG_HEADER_BYTES + layout->registers * BPP_CG_VALUE_BYTES;
    size_t flags_offset = locals_offset + layout->locals * BPP_CG_VALUE_BYTES;
    size_t args_offset = (flags_offset + layout->locals + 15) & ~(size_t)15;
    size_t total = args_offset + layout->call_args * BPP_CG_VALUE_BYTES;
    /* Sizes originate in overflow-checked emitted layout metadata. */
    for (size_t j = 0; j < total; ++j) base[j] = 0;
    BPPArmFrame *f = storage;
    f->vm = vm; f->globals = g; f->global_initialized = gi;
    f->registers = (BLValue *)(base + BPP_CG_HEADER_BYTES);
    f->locals = (BLValue *)(base + locals_offset);
    f->local_initialized = base + flags_offset;
    f->args = (BLValue *)(base + args_offset);
    f->register_count = layout->registers; f->local_count = layout->locals;
    for (size_t j = 0; j < layout->params; ++j) { f->locals[j] = bl_value_clone(args[j]); f->local_initialized[j] = 1; }
    return f;
}
