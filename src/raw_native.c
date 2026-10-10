#include "../include/compiler.h"
#include "../include/target.h"
#include <stdint.h>
#include <string.h>

/*
 * The raw backend is intentionally boring: every B++ value is one machine
 * word.  It does not use BLValue, BLRuntime, refcounts, tags, or a hidden
 * object runtime.  A procedure receives (globals, global-init flags,
 * argument-word array, result-word address).  User calls use exactly the
 * same private raw procedure convention on every target.
 */
typedef struct {
    FILE *out;
    BLIRModule *module;
    const BLIRFunction *fn;
    BPPTarget target;
    size_t function, block, instruction;
    size_t values, locals, flags, args, bytes, max_args;
    int windows, arm64, macho;
} Raw;

static size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static size_t max_arguments(const BLIRFunction *fn) {
    size_t maximum = 0;
    for (size_t b = 0; b < fn->block_count; ++b)
        for (size_t k = 0; k < fn->blocks[b].count; ++k)
            if (fn->blocks[b].insts[k].arg_count > maximum)
                maximum = fn->blocks[b].insts[k].arg_count;
    return maximum;
}

static void layout(Raw *raw) {
    raw->values = 32;
    raw->locals = raw->values + raw->fn->register_count * 8;
    raw->flags = raw->locals + raw->fn->local_count * 8;
    raw->args = align_up(raw->flags + raw->fn->local_count, 8);
    raw->max_args = max_arguments(raw->fn);
    raw->bytes = align_up(raw->args + raw->max_args * 8 + 16, 16);
    /* x86 enters with the return address already pushed.  Six callee-saved
     * pushes leave rsp 8 mod 16, so reserve an extra eight bytes before any
     * host or B++ call. */
    if (!raw->arm64) raw->bytes += 8;
}

static int raw_op_supported(BLIROp op) {
    switch (op) {
    case BL_IR_NULL: case BL_IR_INT:
    case BL_IR_LOAD_LOCAL: case BL_IR_STORE_LOCAL:
    case BL_IR_LOAD_GLOBAL: case BL_IR_STORE_GLOBAL:
    case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL:
    case BL_IR_NEG: case BL_IR_NOT:
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
    case BL_IR_MALLOC: case BL_IR_REMALLOC:
    case BL_IR_MFREE_LOCAL: case BL_IR_MFREE_GLOBAL:
    case BL_IR_BYTE_LOAD: case BL_IR_BYTE_STORE:
    case BL_IR_PRINT_BYTE: case BL_IR_PRINT_RANGE:
    case BL_IR_CALL: case BL_IR_RELEASE:
    case BL_IR_DELETE_LOCAL: case BL_IR_DELETE_GLOBAL:
    case BL_IR_JUMP: case BL_IR_BRANCH: case BL_IR_RETURN:
        return 1;
    default:
        return 0;
    }
}

int bpp_raw_native_eligible(BLIRModule *module) {
    if (module->emit_library || module->static_library_count || module->static_procedure_count) {
        snprintf(module->error, sizeof module->error,
                 "%s: raw native backend does not yet accept ABI-v1 static-library calls",
                 module->path ? module->path : "<input>");
        return 0;
    }
    for (size_t f = 0; f < module->function_count; ++f)
        for (size_t b = 0; b < module->functions[f].block_count; ++b)
            for (size_t k = 0; k < module->functions[f].blocks[b].count; ++k) {
                const BLIRInst *inst = &module->functions[f].blocks[b].insts[k];
                if (!raw_op_supported(inst->op) ||
                    (inst->op == BL_IR_CALL && inst->c != BL_IR_NONE)) {
                    snprintf(module->error, sizeof module->error,
                             "%s:%d:%d: raw native backend has no lowering for '%s'",
                             module->path ? module->path : "<input>", inst->line, inst->column,
                             bl_ir_op_name(inst->op));
                    return 0;
                }
            }
    return 1;
}

static void label(const Raw *raw, const char *kind) {
    fprintf(raw->out, "L_raw_f%zu_b%zu_i%zu_%s", raw->function, raw->block,
            raw->instruction, kind);
}

static void procedure_label(FILE *out, size_t function) { fprintf(out, "bpp_raw_proc_%zu", function); }
static void block_label(FILE *out, size_t function, size_t block) { fprintf(out, "L_raw_f%zu_b%zu", function, block); }

static void xcall(const Raw *raw, const char *name) {
    fprintf(raw->out, "    call %s%s\n", name, raw->windows ? "" : "@PLT");
}

static void xvalue(const Raw *raw, BLIRId reg, const char *target) {
    fprintf(raw->out, "    movq %zu(%%r12), %s\n", raw->values + reg * 8, target);
}
static void xstore(const Raw *raw, BLIRId reg, const char *source) {
    fprintf(raw->out, "    movq %s, %zu(%%r12)\n", source, raw->values + reg * 8);
}
static void xlocal_address(const Raw *raw, size_t slot, int global, int flags) {
    if (global) {
        fprintf(raw->out, "    movq %u(%%r12), %%rax\n", flags ? 8u : 0u);
        if (slot) fprintf(raw->out, "    addq $%zu, %%rax\n", flags ? slot : slot * 8);
    } else {
        fprintf(raw->out, "    leaq %zu(%%r12), %%rax\n",
                flags ? raw->flags + slot : raw->locals + slot * 8);
    }
}
static void xtrap(const Raw *raw) { xcall(raw, "exit"); }
static void xcheck_initialized(const Raw *raw, size_t slot, int global) {
    xlocal_address(raw, slot, global, 1);
    fputs("    cmpb $0, (%rax)\n    jne ", raw->out); label(raw, "initialized");
    fputs("\n    movl $1, ", raw->out); fputs(raw->windows ? "%ecx\n" : "%edi\n", raw->out);
    xtrap(raw); label(raw, "initialized"); fputs(":\n", raw->out);
}
static void xcopy_arguments(const Raw *raw, const BLIRInst *inst) {
    for (size_t n = 0; n < inst->arg_count; ++n) {
        xvalue(raw, inst->args[n], "%rax");
        fprintf(raw->out, "    movq %%rax, %zu(%%r12)\n", raw->args + n * 8);
    }
}
static void xuser_call(const Raw *raw, const BLIRInst *inst) {
    xcopy_arguments(raw, inst);
    if (raw->windows) {
        fputs("    movq 0(%r12), %rcx\n    movq 8(%r12), %rdx\n", raw->out);
        fprintf(raw->out, "    leaq %zu(%%r12), %%r8\n    leaq %zu(%%r12), %%r9\n", raw->args, raw->values + inst->dst * 8);
    } else {
        fputs("    movq 0(%r12), %rdi\n    movq 8(%r12), %rsi\n", raw->out);
        fprintf(raw->out, "    leaq %zu(%%r12), %%rdx\n    leaq %zu(%%r12), %%rcx\n", raw->args, raw->values + inst->dst * 8);
    }
    fputs("    call ", raw->out); procedure_label(raw->out, inst->a); fputc('\n', raw->out);
}

static const char *xcondition(BLIROp op) {
    switch (op) {
    case BL_IR_EQ: return "e"; case BL_IR_NE: return "ne"; case BL_IR_LT: return "l";
    case BL_IR_LE: return "le"; case BL_IR_GT: return "g"; default: return "ge";
    }
}
static void xemit_inst(Raw *raw, const BLIRInst *inst) {
    FILE *out = raw->out;
    if (raw->module->debug_path) bpp_asm_location(out, raw->module, inst);
    switch (inst->op) {
    case BL_IR_NULL: case BL_IR_INT:
        fprintf(out, "    movq $%lld, %%rax\n", inst->op == BL_IR_INT ? inst->integer : 0LL); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_LOAD_LOCAL: case BL_IR_LOAD_GLOBAL: {
        int global = inst->op == BL_IR_LOAD_GLOBAL; xcheck_initialized(raw, inst->a, global); xlocal_address(raw, inst->a, global, 0);
        fputs("    movq (%rax), %rax\n", out); xstore(raw, inst->dst, "%rax"); break;
    }
    case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL: case BL_IR_STORE_LOCAL: case BL_IR_STORE_GLOBAL: {
        int global = inst->op == BL_IR_INIT_GLOBAL || inst->op == BL_IR_STORE_GLOBAL;
        int init = inst->op == BL_IR_INIT_LOCAL || inst->op == BL_IR_INIT_GLOBAL;
        if (!init) xcheck_initialized(raw, inst->a, global);
        xvalue(raw, inst->b, "%rdx"); xlocal_address(raw, inst->a, global, 0); fputs("    movq %rdx, (%rax)\n", out);
        if (init) { xlocal_address(raw, inst->a, global, 1); fputs("    movb $1, (%rax)\n", out); }
        break;
    }
    case BL_IR_DELETE_LOCAL: case BL_IR_DELETE_GLOBAL: {
        int global = inst->op == BL_IR_DELETE_GLOBAL; xlocal_address(raw, inst->a, global, 0); fputs("    movq $0, (%rax)\n", out);
        xlocal_address(raw, inst->a, global, 1); fputs("    movb $0, (%rax)\n", out); break;
    }
    case BL_IR_MFREE_LOCAL: case BL_IR_MFREE_GLOBAL: {
        int global = inst->op == BL_IR_MFREE_GLOBAL; xcheck_initialized(raw, inst->a, global); xlocal_address(raw, inst->a, global, 0);
        fputs("    movq (%rax), %rax\n", out); fprintf(out, "    movq %%rax, %s\n", raw->windows ? "%rcx" : "%rdi"); xcall(raw, "free"); break;
    }
    case BL_IR_RELEASE: break;
    case BL_IR_NEG:
        xvalue(raw, inst->a, "%rax"); fputs("    negq %rax\n    jno ", out); label(raw, "arith"); fputs("\n    movl $1, ", out); fputs(raw->windows ? "%ecx\n" : "%edi\n", out); xtrap(raw); label(raw, "arith"); fputs(":\n", out); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_NOT:
        xvalue(raw, inst->a, "%rax"); fputs("    testq %rax, %rax\n    movl $0, %eax\n    sete %al\n", out); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx");
        fprintf(out, "    %s %%rdx, %%rax\n    jno ", inst->op == BL_IR_ADD ? "addq" : inst->op == BL_IR_SUB ? "subq" : "imulq");
        label(raw, "arith"); fputs("\n    movl $1, ", out); fputs(raw->windows ? "%ecx\n" : "%edi\n", out); xtrap(raw); label(raw, "arith"); fputs(":\n", out); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_DIV: case BL_IR_MOD:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rcx"); fputs("    testq %rcx, %rcx\n    jne ", out); label(raw, "divisor"); fputs("\n    movl $1, ", out); fputs(raw->windows ? "%ecx\n" : "%edi\n", out); xtrap(raw); label(raw, "divisor"); fputs(":\n", out);
        fputs("    cmpq $-1, %rcx\n    jne ", out); label(raw, "divide"); fputs("\n    movabsq $0x8000000000000000, %rdx\n    cmpq %rdx, %rax\n    jne ", out); label(raw, "divide"); fputs("\n    movl $1, ", out); fputs(raw->windows ? "%ecx\n" : "%edi\n", out); xtrap(raw); label(raw, "divide"); fputs(":\n    cqto\n    idivq %rcx\n", out); xstore(raw, inst->dst, inst->op == BL_IR_DIV ? "%rax" : "%rdx"); break;
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx"); fprintf(out, "    cmpq %%rdx, %%rax\n    movl $0, %%eax\n    set%s %%al\n", xcondition(inst->op)); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_MALLOC:
        xvalue(raw, inst->a, "%rax"); fprintf(out, "    movq %%rax, %s\n", raw->windows ? "%rcx" : "%rdi"); xcall(raw, "malloc"); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_REMALLOC:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx");
        if (raw->windows) fputs("    movq %rax, %rcx\n", out); else fputs("    movq %rax, %rdi\n    movq %rdx, %rsi\n", out);
        xcall(raw, "realloc"); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_BYTE_LOAD:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx"); fputs("    movzbq (%rax,%rdx), %rax\n", out); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_BYTE_STORE:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx"); xvalue(raw, inst->c, "%rcx"); fputs("    movb %cl, (%rax,%rdx)\n", out); break;
    case BL_IR_PRINT_BYTE:
        xvalue(raw, inst->a, "%rax"); fputs("    movb %al, 24(%r12)\n", out);
        if (raw->windows) fputs("    movl $1, %ecx\n    leaq 24(%r12), %rdx\n    movl $1, %r8d\n", out);
        else fputs("    movl $1, %edi\n    leaq 24(%r12), %rsi\n    movl $1, %edx\n", out);
        xcall(raw, raw->windows ? "_write" : "write"); break;
    case BL_IR_PRINT_RANGE:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx");
        if (raw->windows) fputs("    movq %rdx, %r8\n    movl $1, %ecx\n    movq %rax, %rdx\n", out);
        else fputs("    movl $1, %edi\n    movq %rax, %rsi\n", out);
        xcall(raw, raw->windows ? "_write" : "write"); break;
    case BL_IR_CALL: xuser_call(raw, inst); break;
    default: break;
    }
}

static void xemit_function(Raw *raw) {
    FILE *out = raw->out; layout(raw);
    procedure_label(out, raw->function); fputs(":\n", out);
    if (!raw->windows) fputs("    .cfi_startproc\n", out);
    fputs("    pushq %rbp\n    movq %rsp, %rbp\n    pushq %r12\n    pushq %r13\n    pushq %r14\n    pushq %r15\n    pushq %rbx\n", out);
    fprintf(out, "    subq $%zu, %%rsp\n    movq %%rsp, %%r12\n", raw->bytes);
    /* Keep the incoming ABI arguments in callee-saved registers while rep
     * clears every value cell and initialization flag.  Stack contents must
     * never decide whether a B++ local is initialized. */
    if (raw->windows) fputs("    movq %rcx, %r13\n    movq %rdx, %r14\n    movq %r8, %r15\n    movq %r9, %rbx\n", out);
    else fputs("    movq %rdi, %r13\n    movq %rsi, %r14\n    movq %rdx, %r15\n    movq %rcx, %rbx\n", out);
    fprintf(out, "    movq %%r12, %%rdi\n    xorl %%eax, %%eax\n    movq $%zu, %%rcx\n    rep stosq\n", (raw->bytes + 7) / 8);
    fputs("    movq %r13, 0(%r12)\n    movq %r14, 8(%r12)\n    movq %rbx, 16(%r12)\n", out);
    const char *args = "%r15";
    for (size_t p = 0; p < raw->fn->param_count; ++p) {
        fprintf(out, "    movq %zu(%s), %%rax\n    movq %%rax, %zu(%%r12)\n    movb $1, %zu(%%r12)\n", p * 8, args, raw->locals + p * 8, raw->flags + p);
    }
//    block_label(out, raw->function, 0); fputs(":\n", out);
    for (size_t b = 0; b < raw->fn->block_count; ++b) {
        raw->block = b; block_label(out, raw->function, b); fputs(":\n", out);
        for (size_t k = 0; k < raw->fn->blocks[b].count; ++k) {
            raw->instruction = k; const BLIRInst *inst = &raw->fn->blocks[b].insts[k];
            if (inst->op == BL_IR_JUMP) { fprintf(out, "    jmp L_raw_f%zu_b%zu\n", raw->function, inst->a); continue; }
            if (inst->op == BL_IR_BRANCH) { xvalue(raw, inst->a, "%rax"); fputs("    testq %rax, %rax\n    jne ", out); block_label(out, raw->function, inst->b); fputs("\n    jmp ", out); block_label(out, raw->function, inst->c); fputc('\n', out); continue; }
            if (inst->op == BL_IR_RETURN) { xvalue(raw, inst->a, "%rax"); fputs("    movq 16(%r12), %rdx\n    movq %rax, (%rdx)\n    jmp ", out); procedure_label(out, raw->function); fputs("_done\n", out); continue; }
            xemit_inst(raw, inst);
        }
    }
    procedure_label(out, raw->function); fputs("_done:\n", out);
    fprintf(out, "    addq $%zu, %%rsp\n    popq %%rbx\n    popq %%r15\n    popq %%r14\n    popq %%r13\n    popq %%r12\n    popq %%rbp\n    ret\n", raw->bytes);
    if (!raw->windows) fputs("    .cfi_endproc\n", out);
}

static void xemit_main(Raw *raw, int print_result) {
    size_t globals = raw->module->global_count ? raw->module->global_count : 1;
    FILE *out = raw->out;
    fputs(".globl main\nmain:\n", out);
    if (!raw->windows) fputs("    .cfi_startproc\n", out);
    /* 48 bytes supplies the Windows home area plus a result word and keeps
     * rsp 16-byte aligned before every call on both x86-64 ABIs. */
    fputs("    pushq %rbp\n    movq %rsp, %rbp\n    subq $48, %rsp\n", out);
    if (raw->windows) fputs("    leaq bpp_raw_globals(%rip), %rcx\n    leaq bpp_raw_global_flags(%rip), %rdx\n    xorq %r8, %r8\n    leaq 32(%rsp), %r9\n", out);
    else fputs("    leaq bpp_raw_globals(%rip), %rdi\n    leaq bpp_raw_global_flags(%rip), %rsi\n    xorq %rdx, %rdx\n    leaq 32(%rsp), %rcx\n", out);
    fputs("    call bpp_raw_proc_0", out); fputc('\n', out);
    if (print_result) {
        if (raw->windows) fputs("    movq 32(%rsp), %rdx\n    leaq L_raw_print_int(%rip), %rcx\n", out);
        else fputs("    movq 32(%rsp), %rsi\n    leaq L_raw_print_int(%rip), %rdi\n    xorl %eax, %eax\n", out);
        xcall(raw, "printf");
    }
    fputs("    xorl %eax, %eax\n    leave\n    ret\n", out);
    if (!raw->windows) fputs("    .cfi_endproc\n", out);
    fprintf(out, ".section .bss\n    .comm bpp_raw_globals,%zu,8\n    .comm bpp_raw_global_flags,%zu,1\n", globals * 8, globals);
    if (print_result) fputs(".section .rodata\nL_raw_print_int:\n    .asciz \"%lld\\n\"\n", out);
    if (!raw->windows) fputs(".section .note.GNU-stack,\"\",@progbits\n", out);
}

/* ARM64 helpers.  They use the same raw procedure ABI as x86-64, expressed
 * in x0..x3.  x19 is the frame base, x20/x21 are global value/flag bases,
 * and x22 is the result pointer; all are ABI-preserved across calls. */
static void aimmediate(FILE *out, const char *reg, uint64_t value) {
    fprintf(out, "    movz %s, #%u", reg, (unsigned)(value & 0xffffu));
    for (unsigned part = 1; part < 4; ++part) if ((value >> (part * 16)) & 0xffffu)
        fprintf(out, "\n    movk %s, #%u, lsl #%u", reg, (unsigned)((value >> (part * 16)) & 0xffffu), part * 16);
    fputc('\n', out);
}
static void aaddress(const Raw *raw, const char *dst, const char *base, size_t offset) {
    if (!offset) { fprintf(raw->out, "    mov %s, %s\n", dst, base); return; }
    if (offset <= 4095) { fprintf(raw->out, "    add %s, %s, #%zu\n", dst, base, offset); return; }
    aimmediate(raw->out, "x9", offset); fprintf(raw->out, "    add %s, %s, x9\n", dst, base);
}
static const char *asymbol(const Raw *raw, const char *name) {
    static char symbol[80]; snprintf(symbol, sizeof symbol, "%s%s", raw->macho ? "_" : "", name); return symbol;
}
static void acall(const Raw *raw, const char *name) { fprintf(raw->out, "    bl %s\n", asymbol(raw, name)); }
static void avalue(const Raw *raw, BLIRId reg, const char *dst) { aaddress(raw, "x9", "x19", raw->values + reg * 8); fprintf(raw->out, "    ldr %s, [x9]\n", dst); }
static void astore(const Raw *raw, BLIRId reg, const char *src) { aaddress(raw, "x9", "x19", raw->values + reg * 8); fprintf(raw->out, "    str %s, [x9]\n", src); }
static void alocal_address(const Raw *raw, size_t slot, int global, int flags) { aaddress(raw, "x9", global ? (flags ? "x21" : "x20") : "x19", global ? (flags ? slot : slot * 8) : (flags ? raw->flags + slot : raw->locals + slot * 8)); }
static void atrap(const Raw *raw) { aimmediate(raw->out, "x0", 1); acall(raw, "exit"); }
static void acheck_initialized(const Raw *raw, size_t slot, int global) { alocal_address(raw, slot, global, 1); fputs("    ldrb w10, [x9]\n    cbnz w10, ", raw->out); label(raw, "initialized"); fputs("\n", raw->out); atrap(raw); label(raw, "initialized"); fputs(":\n", raw->out); }
static void aemit_global_address(const Raw *raw, const char *register_name, const char *name) {
    if (raw->macho) fprintf(raw->out, "    adrp %s, _%s@PAGE\n    add %s, %s, _%s@PAGEOFF\n", register_name, name, register_name, register_name, name);
    else fprintf(raw->out, "    adrp %s, %s\n    add %s, %s, :lo12:%s\n", register_name, name, register_name, register_name, name);
}
/* Unlike external Mach-O symbols, assembler-local labels do not acquire the
 * leading underscore.  Keep this separate from aemit_global_address(): using
 * the external spelling here leaves a relocation against _L_raw_print_int
 * with no matching definition at link time. */
static void aemit_local_address(const Raw *raw, const char *register_name, const char *name) {
    if (raw->macho) fprintf(raw->out, "    adrp %s, %s@PAGE\n    add %s, %s, %s@PAGEOFF\n", register_name, name, register_name, register_name, name);
    else fprintf(raw->out, "    adrp %s, %s\n    add %s, %s, :lo12:%s\n", register_name, name, register_name, register_name, name);
}
static void auser_call(const Raw *raw, const BLIRInst *inst) {
    for (size_t n = 0; n < inst->arg_count; ++n) { avalue(raw, inst->args[n], "x10"); aaddress(raw, "x9", "x19", raw->args + n * 8); fputs("    str x10, [x9]\n", raw->out); }
    fputs("    mov x0, x20\n    mov x1, x21\n", raw->out); aaddress(raw, "x2", "x19", raw->args); aaddress(raw, "x3", "x19", raw->values + inst->dst * 8);
    fputs("    bl ", raw->out); procedure_label(raw->out, inst->a); fputc('\n', raw->out);
}
static void aemit_inst(Raw *raw, const BLIRInst *inst) {
    FILE *out = raw->out; if (raw->module->debug_path) bpp_asm_location(out, raw->module, inst);
    switch (inst->op) {
    case BL_IR_NULL: case BL_IR_INT: aimmediate(out, "x10", inst->op == BL_IR_INT ? (uint64_t)inst->integer : 0); astore(raw, inst->dst, "x10"); break;
    case BL_IR_LOAD_LOCAL: case BL_IR_LOAD_GLOBAL: { int g=inst->op==BL_IR_LOAD_GLOBAL; acheck_initialized(raw,inst->a,g); alocal_address(raw,inst->a,g,0); fputs("    ldr x10, [x9]\n",out); astore(raw,inst->dst,"x10"); break; }
    case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL: case BL_IR_STORE_LOCAL: case BL_IR_STORE_GLOBAL: { int g=inst->op==BL_IR_INIT_GLOBAL||inst->op==BL_IR_STORE_GLOBAL, init=inst->op==BL_IR_INIT_GLOBAL||inst->op==BL_IR_INIT_LOCAL; if(!init)acheck_initialized(raw,inst->a,g); avalue(raw,inst->b,"x10"); alocal_address(raw,inst->a,g,0); fputs("    str x10, [x9]\n",out); if(init){alocal_address(raw,inst->a,g,1);fputs("    mov w10, #1\n    strb w10, [x9]\n",out);}break; }
    case BL_IR_DELETE_LOCAL: case BL_IR_DELETE_GLOBAL: {int g=inst->op==BL_IR_DELETE_GLOBAL;alocal_address(raw,inst->a,g,0);fputs("    str xzr, [x9]\n",out);alocal_address(raw,inst->a,g,1);fputs("    strb wzr, [x9]\n",out);break;}
    case BL_IR_MFREE_LOCAL: case BL_IR_MFREE_GLOBAL: {int g=inst->op==BL_IR_MFREE_GLOBAL;acheck_initialized(raw,inst->a,g);alocal_address(raw,inst->a,g,0);fputs("    ldr x0, [x9]\n",out);acall(raw,"free");break;}
    case BL_IR_RELEASE: break;
    case BL_IR_NEG: avalue(raw,inst->a,"x10");fputs("    negs x10, x10\n    b.vs ",out);label(raw,"trap");fputs("\n",out);astore(raw,inst->dst,"x10");fputs("    b ",out);label(raw,"done");fputs("\n",out);label(raw,"trap");fputs(":\n",out);atrap(raw);label(raw,"done");fputs(":\n",out);break;
    case BL_IR_NOT: avalue(raw,inst->a,"x10");fputs("    cmp x10, #0\n    cset x10, eq\n",out);astore(raw,inst->dst,"x10");break;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: avalue(raw,inst->a,"x10");avalue(raw,inst->b,"x11"); if(inst->op==BL_IR_ADD)fputs("    adds x10, x10, x11\n    b.vs ",out);else if(inst->op==BL_IR_SUB)fputs("    subs x10, x10, x11\n    b.vs ",out);else fputs("    mul x12, x10, x11\n    smulh x13, x10, x11\n    asr x14, x12, #63\n    cmp x13, x14\n    b.ne ",out);label(raw,"trap");fputs("\n",out); if(inst->op==BL_IR_MUL)fputs("    mov x10, x12\n",out);astore(raw,inst->dst,"x10");fputs("    b ",out);label(raw,"done");fputs("\n",out);label(raw,"trap");fputs(":\n",out);atrap(raw);label(raw,"done");fputs(":\n",out);break;
    case BL_IR_DIV: case BL_IR_MOD: avalue(raw,inst->a,"x10");avalue(raw,inst->b,"x11");fputs("    cbnz x11, ",out);label(raw,"divisor");fputs("\n",out);atrap(raw);label(raw,"divisor");fputs(":\n",out);aimmediate(out,"x12",UINT64_MAX);fputs("    cmp x11, x12\n    b.ne ",out);label(raw,"divide");fputs("\n",out);aimmediate(out,"x12",UINT64_C(0x8000000000000000));fputs("    cmp x10, x12\n    b.ne ",out);label(raw,"divide");fputs("\n",out);atrap(raw);label(raw,"divide");fputs(":\n    sdiv x12, x10, x11\n",out);if(inst->op==BL_IR_DIV)fputs("    mov x10, x12\n",out);else fputs("    msub x10, x12, x11, x10\n",out);astore(raw,inst->dst,"x10");break;
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE: {const char *cc=inst->op==BL_IR_EQ?"eq":inst->op==BL_IR_NE?"ne":inst->op==BL_IR_LT?"lt":inst->op==BL_IR_LE?"le":inst->op==BL_IR_GT?"gt":"ge";avalue(raw,inst->a,"x10");avalue(raw,inst->b,"x11");fprintf(out,"    cmp x10, x11\n    cset x10, %s\n",cc);astore(raw,inst->dst,"x10");break;}
    case BL_IR_MALLOC: avalue(raw,inst->a,"x0");acall(raw,"malloc");astore(raw,inst->dst,"x0");break;
    case BL_IR_REMALLOC: avalue(raw,inst->a,"x0");avalue(raw,inst->b,"x1");acall(raw,"realloc");astore(raw,inst->dst,"x0");break;
    case BL_IR_BYTE_LOAD: avalue(raw,inst->a,"x10");avalue(raw,inst->b,"x11");fputs("    ldrb w10, [x10, x11]\n",out);astore(raw,inst->dst,"x10");break;
    case BL_IR_BYTE_STORE: avalue(raw,inst->a,"x10");avalue(raw,inst->b,"x11");avalue(raw,inst->c,"x12");fputs("    strb w12, [x10, x11]\n",out);break;
    case BL_IR_PRINT_BYTE: avalue(raw,inst->a,"x10");fputs("    strb w10, [x19, #24]\n    mov x0, #1\n    add x1, x19, #24\n    mov x2, #1\n",out);acall(raw,"write");break;
    case BL_IR_PRINT_RANGE: avalue(raw,inst->a,"x1");avalue(raw,inst->b,"x2");fputs("    mov x0, #1\n",out);acall(raw,"write");break;
    case BL_IR_CALL: auser_call(raw,inst);break;
    default:break;
    }
}
static void aemit_function(Raw *raw) {
    FILE *out=raw->out;layout(raw);procedure_label(out,raw->function);fputs(":\n    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n    stp x19, x20, [sp, #-16]!\n    stp x21, x22, [sp, #-16]!\n    stp x23, x24, [sp, #-16]!\n",out);aimmediate(out,"x9",raw->bytes);fputs("    sub sp, sp, x9\n    mov x19, sp\n    mov x20, x0\n    mov x21, x1\n    mov x22, x3\n    mov x23, x2\n",out);aaddress(raw,"x9","x19",0);aimmediate(out,"x10",(raw->bytes+7)/8);fprintf(out,"L_raw_zero_%zu:\n    cbz x10, L_raw_zero_done_%zu\n    str xzr, [x9], #8\n    sub x10, x10, #1\n    b L_raw_zero_%zu\nL_raw_zero_done_%zu:\n",raw->function,raw->function,raw->function,raw->function);for(size_t p=0;p<raw->fn->param_count;++p){aaddress(raw,"x9","x23",p*8);fputs("    ldr x10, [x9]\n",out);aaddress(raw,"x9","x19",raw->locals+p*8);fputs("    str x10, [x9]\n",out);aaddress(raw,"x9","x19",raw->flags+p);fputs("    mov w10, #1\n    strb w10, [x9]\n",out);}for(size_t b=0;b<raw->fn->block_count;++b){raw->block=b;block_label(out,raw->function,b);fputs(":\n",out);for(size_t k=0;k<raw->fn->blocks[b].count;++k){raw->instruction=k;const BLIRInst *i=&raw->fn->blocks[b].insts[k];if(i->op==BL_IR_JUMP){fprintf(out,"    b L_raw_f%zu_b%zu\n",raw->function,i->a);continue;}if(i->op==BL_IR_BRANCH){avalue(raw,i->a,"x10");fprintf(out,"    cbnz x10, L_raw_f%zu_b%zu\n    b L_raw_f%zu_b%zu\n",raw->function,i->b,raw->function,i->c);continue;}if(i->op==BL_IR_RETURN){avalue(raw,i->a,"x10");fputs("    str x10, [x22]\n    b ",out);procedure_label(out,raw->function);fputs("_done\n",out);continue;}aemit_inst(raw,i);}}
    procedure_label(out,raw->function);fputs("_done:\n",out);aimmediate(out,"x9",raw->bytes);fputs("    add sp, sp, x9\n    ldp x23, x24, [sp], #16\n    ldp x21, x22, [sp], #16\n    ldp x19, x20, [sp], #16\n    ldp x29, x30, [sp], #16\n    ret\n",out);
}
static void aemit_main(Raw *raw,int print_result){FILE*out=raw->out;size_t globals=raw->module->global_count?raw->module->global_count:1;if(raw->macho)fputs(".globl _main\n_main:\n",out);else fputs(".globl main\n.type main, %function\nmain:\n",out);fputs("    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n    sub sp, sp, #16\n",out);aemit_global_address(raw,"x0","bpp_raw_globals");aemit_global_address(raw,"x1","bpp_raw_global_flags");fputs("    mov x2, xzr\n    mov x3, sp\n    bl bpp_raw_proc_0\n",out);if(print_result){fputs("    ldr x1, [sp]\n",out);aemit_local_address(raw,"x0","L_raw_print_int");acall(raw,"printf");}fputs("    mov w0, #0\n    add sp, sp, #16\n    ldp x29, x30, [sp], #16\n    ret\n",out);if(raw->macho){fputs(".section __TEXT,__cstring,cstring_literals\nL_raw_print_int:\n    .asciz \"%lld\\n\"\n.zerofill __DATA,__bss,_bpp_raw_globals,",out);fprintf(out,"%zu,3\n.zerofill __DATA,__bss,_bpp_raw_global_flags,%zu,0\n",globals*8,globals);}else{if(print_result)fputs(".section .rodata\nL_raw_print_int:\n    .asciz \"%lld\\n\"\n",out);fprintf(out,".comm bpp_raw_globals,%zu,8\n.comm bpp_raw_global_flags,%zu,1\n.section .note.GNU-stack,\"\",%%progbits\n",globals*8,globals);}}

int bpp_emit_raw_native(BLIRModule *module, FILE *out, int print_result, BPPTarget target) {
    const BPPBackendCapabilities *cap=bpp_target_capabilities(target);
    if(!module||!out||!cap||!bpp_raw_native_eligible(module))return 0;
    bpp_asm_source(out,module);
    Raw raw={0};raw.out=out;raw.module=module;raw.target=target;raw.windows=cap->format==BPP_FORMAT_COFF;raw.arm64=cap->isa==BPP_ISA_ARM64;raw.macho=cap->format==BPP_FORMAT_MACHO;
    if(raw.arm64) fputs(raw.macho?".section __TEXT,__text,regular,pure_instructions\n.p2align 2\n":".text\n.p2align 2\n",out);else fputs(".text\n.p2align 4\n",out);
    for(size_t f=0;f<module->function_count;++f){raw.function=f;raw.fn=&module->functions[f];if(raw.arm64)aemit_function(&raw);else xemit_function(&raw);}
    if(raw.arm64)aemit_main(&raw,print_result);else xemit_main(&raw,print_result);
    return !ferror(out);
}

static void json_text(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', out); fputc(*p, out); }
        else if (*p == '\n') fputs("\\n", out);
        else if (*p == '\r') fputs("\\r", out);
        else if (*p == '\t') fputs("\\t", out);
        else if (*p < 0x20) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}

static void raw_span(const BLIRModule *module, const BLIRInst *inst, size_t *file, int *line, int *column) {
    *file = inst->source_file == BL_IR_NONE ? 0 : inst->source_file;
    *line = inst->source_line ? inst->source_line : inst->line;
    *column = inst->source_column ? inst->source_column : inst->column;
    if (*file >= module->source_file_count) *file = 0;
}

static int raw_variable_write(BLIROp op) {
    return op == BL_IR_INIT_LOCAL || op == BL_IR_INIT_GLOBAL ||
           op == BL_IR_STORE_LOCAL || op == BL_IR_STORE_GLOBAL;
}
static int raw_variable_end(BLIROp op) {
    return op == BL_IR_MFREE_LOCAL || op == BL_IR_MFREE_GLOBAL ||
           op == BL_IR_DELETE_LOCAL || op == BL_IR_DELETE_GLOBAL;
}

/* --emit-map must describe the frame that is actually emitted.  The old
 * cache/spill map describes the retired tagged backend and would be actively
 * misleading for raw executables, so raw code has its own compact manifest. */
int bpp_emit_raw_locations(BLIRModule *module, FILE *out, BPPTarget target) {
    const BPPBackendCapabilities *cap = bpp_target_capabilities(target);
    if (!module || !out || !cap || !bpp_raw_native_eligible(module)) return 0;
    fprintf(out, "{\"target\":"); json_text(out, bpp_target_name(target));
    fputs(",\"backend\":\"raw-word\",\"source\":", out); json_text(out, module->path);
    fputs(",\"source_files\":[", out);
    for (size_t f = 0; f < module->source_file_count; ++f) {
        const BPPSourceFile *source = &module->source_files[f];
        if (f) fputc(',', out);
        fprintf(out, "{\"id\":%zu,\"path\":", f + 1); json_text(out, source->path);
        if (source->parent_file == BL_IR_NONE) fputs(",\"parent\":null", out);
        else fprintf(out, ",\"parent\":%zu", source->parent_file + 1);
        fprintf(out, ",\"import_line\":%d}", source->import_line);
    }
    fputs("],\"functions\":[", out);
    for (size_t f = 0; f < module->function_count; ++f) {
        const BLIRFunction *fn = &module->functions[f]; Raw raw = {0};
        raw.module = module; raw.fn = fn; raw.target = target;
        raw.arm64 = cap->isa == BPP_ISA_ARM64; layout(&raw);
        if (f) fputc(',', out);
        fprintf(out, "{\"name\":"); json_text(out, fn->name);
        fprintf(out, ",\"source\":{\"file\":%zu,\"line\":%d,\"column\":%d}",
                (fn->source_file == BL_IR_NONE ? 0 : fn->source_file) + 1,
                fn->source_line, fn->source_column);
        fprintf(out, ",\"frame_register\":"); json_text(out, raw.arm64 ? "x19" : "r12");
        fprintf(out, ",\"frame_layout\":{\"raw_word_bytes\":8,\"header_bytes\":32,\"value_cells_offset\":%zu,\"local_slots_offset\":%zu,\"local_flags_offset\":%zu,\"outgoing_arguments_offset\":%zu,\"outgoing_argument_words\":%zu,\"frame_bytes\":%zu,\"alignment\":16}", raw.values, raw.locals, raw.flags, raw.args, raw.max_args, raw.bytes);
        fputs(",\"scopes\":[{\"kind\":\"procedure\",\"name\":", out); json_text(out, fn->name);
        fprintf(out, ",\"file\":%zu,\"line\":%d,\"column\":%d}]", (fn->source_file == BL_IR_NONE ? 0 : fn->source_file) + 1, fn->source_line, fn->source_column);
        fputs(",\"parameters\":[", out);
        for (size_t p = 0; p < fn->param_count; ++p) {
            if (p) fputc(',', out);
            fputs("{\"name\":", out); json_text(out, fn->locals[p]);
            fprintf(out, ",\"representation\":\"raw-word\",\"location\":{\"kind\":\"frame-slot\",\"offset\":%zu,\"initialized_flag_offset\":%zu}}", raw.locals + p * 8, raw.flags + p);
        }
        fputs("],\"locals\":[", out);
        for (size_t l = fn->param_count; l < fn->local_count; ++l) {
            if (l != fn->param_count) fputc(',', out);
            fputs("{\"name\":", out); json_text(out, fn->locals[l]);
            fprintf(out, ",\"representation\":\"raw-word\",\"declared_storage\":{\"kind\":\"frame-slot\",\"offset\":%zu,\"initialized_flag_offset\":%zu},\"location_status\":\"exact raw frame slot\",\"may_be_uninitialized\":true}", raw.locals + l * 8, raw.flags + l);
        }
        fputs("],\"instructions\":[", out);
        int first = 1;
        for (size_t b = 0; b < fn->block_count; ++b) for (size_t k = 0; k < fn->blocks[b].count; ++k) {
            const BLIRInst *inst = &fn->blocks[b].insts[k]; size_t file; int line, column;
            raw_span(module, inst, &file, &line, &column);
            if (!first) fputc(',', out);
            first = 0;
            fprintf(out, "{\"block\":%zu,\"instruction\":%zu,\"file\":%zu,\"line\":%d,\"column\":%d,\"expanded_line\":%d,\"opcode\":", b, k, file + 1, line, column, inst->line);
            json_text(out, bl_ir_op_name(inst->op));
            if (raw_variable_write(inst->op)) {
                int global = inst->op == BL_IR_INIT_GLOBAL || inst->op == BL_IR_STORE_GLOBAL;
                const char *name = global ? module->globals[inst->a] : fn->locals[inst->a];
                fprintf(out, ",\"variable_write\":{\"scope\":\"%s\",\"slot\":%zu,\"name\":", global ? "global" : "local", inst->a); json_text(out, name);
                fprintf(out, ",\"value_vreg\":%zu,\"value_location\":{\"kind\":\"frame-slot\",\"offset\":%zu}}", inst->b, raw.values + inst->b * 8);
            } else if (raw_variable_end(inst->op)) {
                int global = inst->op == BL_IR_MFREE_GLOBAL || inst->op == BL_IR_DELETE_GLOBAL;
                const char *name = global ? module->globals[inst->a] : fn->locals[inst->a];
                fprintf(out, ",\"variable_state\":{\"scope\":\"%s\",\"slot\":%zu,\"name\":", global ? "global" : "local", inst->a); json_text(out, name);
                fprintf(out, ",\"state\":\"%s\"}", inst->op == BL_IR_MFREE_LOCAL || inst->op == BL_IR_MFREE_GLOBAL ? "freed" : "deleted");
            }
            fputc('}', out);
        }
        fputs("]}", out);
    }
    fputs("]}\n", out);
    return !ferror(out);
}