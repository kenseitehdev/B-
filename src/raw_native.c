#include "../include/compiler.h"
#include "../include/target.h"
#include <stdint.h>
#include <stdlib.h>
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
static void json_text(FILE *,const char *);

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
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_BIT_NOT:
    case BL_IR_BIT_AND: case BL_IR_BIT_OR: case BL_IR_BIT_XOR:
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
static int codeview_name(const char *name){
    if(!name||!((*name>='A'&&*name<='Z')||(*name>='a'&&*name<='z')||*name=='_'))return 0;
    for(++name;*name;++name)if(!((*name>='A'&&*name<='Z')||(*name>='a'&&*name<='z')||(*name>='0'&&*name<='9')||*name=='_'))return 0;
    return 1;
}
static void codeview_source_alias(FILE *out,const BLIRFunction *fn){
    if(!fn||!codeview_name(fn->name))return;
    fprintf(out,".globl bpp_source_%s\nbpp_source_%s:\n",fn->name,fn->name);
}
static void procedure_done_label(FILE *out, const Raw *raw) {
    if (raw->macho) fprintf(out, "L_bpp_raw_proc_%zu_done", raw->function);
    else { procedure_label(out, raw->function); fputs("_done", out); }
}
static void block_label(FILE *out, size_t function, size_t block) { fprintf(out, "L_raw_f%zu_b%zu", function, block); }
static void debug_pc_label(const Raw *raw, const char *edge) {
    fprintf(raw->out, "L_bpp_pc_f%zu_b%zu_i%zu_%s:\n",
            raw->function, raw->block, raw->instruction, edge);
}
static void codeview_end_label(FILE *out,size_t function){fprintf(out,"L_bpp_codeview_end_%zu",function);}

static void xcall(const Raw *raw, const char *name) {
    fprintf(raw->out, "    call %s%s\n", name, raw->windows ? "" : "@PLT");
}

/* Sanitizer calls must not destroy a previously loaded operand or live
 * arithmetic flags. Keep scratch registers on the real ABI stack, separately
 * from the checked frame. Eight pushes preserve 16-byte call alignment. */
static const char *const xscratch[]={"%rax","%rcx","%rdx","%r8","%r9","%r10","%r11"};
static void xsave(const Raw *raw){
    for(size_t s=0;s<7;++s)fprintf(raw->out,"    pushq %s\n",xscratch[s]);
    /* Preserve only arithmetic flags. PUSHFQ/POPFQ also copy debugger control
     * flags, so single-stepping can otherwise leave TF set across C calls.
     * SETO/LAHF do not change flags; restore the incoming RAX before args. */
    fputs("    seto %al\n    lahf\n    pushq %rax\n    movq 56(%rsp), %rax\n",raw->out);
    if(raw->windows)fputs("    subq $32, %rsp\n",raw->out);
}
static size_t xsaved(const Raw *raw,const char *reg){
    for(size_t s=0;s<7;++s)if(!strcmp(reg,xscratch[s]))return (7-s)*8+(raw->windows?32:0);
    return SIZE_MAX;
}
static void xrestore(const Raw *raw,const char *result){
    if(result)fprintf(raw->out,"    movq %%rax, %zu(%%rsp)\n",xsaved(raw,result));
    if(raw->windows)fputs("    addq $32, %rsp\n",raw->out);
    /* 0+127 clears OF; 1+127 sets OF. SAHF restores SF/ZF/AF/PF/CF without
     * changing OF or the debugger's live control flags. POPs preserve them. */
    fputs("    popq %rax\n    addb $127, %al\n    sahf\n",raw->out);
    for(size_t s=7;s-- >0;)fprintf(raw->out,"    popq %s\n",xscratch[s]);
}
static void xsite(const Raw *raw,const BLIRInst *i){
    if(!raw->module->instrument_native)return;
    size_t file=i->source_file==BL_IR_NONE?0:i->source_file;
    xsave(raw);
    fprintf(raw->out,"    leaq L_s_source_%zu(%%rip), %s\n    movq $%d, %s\n    movq $%d, %s\n",file,raw->windows?"%rcx":"%rdi",i->source_line,raw->windows?"%rdx":"%rsi",i->source_column,raw->windows?"%r8":"%rdx");
    xcall(raw,"bpp_s_site");xrestore(raw,NULL);
}
static void xread(const Raw *raw,const char *address,const char *dst,size_t width){
    if(!raw->module->instrument_native){fprintf(raw->out,"    %s %s, %s\n",width==8?"movq":"movzbq",address,dst);return;}
    xsave(raw);fprintf(raw->out,"    leaq %s, %s\n    movq $%zu, %s\n",address,raw->windows?"%rcx":"%rdi",width,raw->windows?"%rdx":"%rsi");
    xcall(raw,"bpp_s_read");xrestore(raw,dst);
}
static void xwrite(const Raw *raw,const char *address,const char *src,size_t width){
    xsave(raw);
    fprintf(raw->out,"    leaq %s, %s\n    movq %zu(%%rsp), %s\n    movq $%zu, %s\n",address,raw->windows?"%rcx":"%rdi",xsaved(raw,src),raw->windows?"%rdx":"%rsi",width,raw->windows?"%r8":"%rdx");
    xcall(raw,"bpp_s_write");xrestore(raw,NULL);
}

static void xvalue(const Raw *raw, BLIRId reg, const char *target) {
    char address[80];snprintf(address,sizeof address,"%zu(%%r12)",raw->values+reg*8);xread(raw,address,target,8);
}
static void xstore(const Raw *raw, BLIRId reg, const char *source) {
    if(raw->module->instrument_native){char address[80];snprintf(address,sizeof address,"%zu(%%r12)",raw->values+reg*8);xwrite(raw,address,source,8);return;}
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
static size_t trap_category(const Raw *raw){
    BLIROp op=raw->fn->blocks[raw->block].insts[raw->instruction].op;
    return op==BL_IR_NEG||op==BL_IR_ADD||op==BL_IR_SUB||op==BL_IR_MUL?1:op==BL_IR_DIV||op==BL_IR_MOD?2:3;
}
static void xtrap(const Raw *raw) {
    if(raw->module->instrument_native){fprintf(raw->out,"    movq $%zu, %s\n",trap_category(raw),raw->windows?"%rcx":"%rdi");xcall(raw,"bpp_s_fail");}
    else xcall(raw,"exit");
}
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
    switch (inst->op) {
    case BL_IR_NULL: case BL_IR_INT:
        fprintf(out, "    movq $%lld, %%rax\n", inst->op == BL_IR_INT ? inst->integer : 0LL); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_LOAD_LOCAL: case BL_IR_LOAD_GLOBAL: {
        int global = inst->op == BL_IR_LOAD_GLOBAL; xcheck_initialized(raw, inst->a, global); xlocal_address(raw, inst->a, global, 0);
        xread(raw,"(%rax)","%rax",8); xstore(raw, inst->dst, "%rax"); break;
    }
    case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL: case BL_IR_STORE_LOCAL: case BL_IR_STORE_GLOBAL: {
        int global = inst->op == BL_IR_INIT_GLOBAL || inst->op == BL_IR_STORE_GLOBAL;
        int init = inst->op == BL_IR_INIT_LOCAL || inst->op == BL_IR_INIT_GLOBAL;
        if (!init) xcheck_initialized(raw, inst->a, global);
        xvalue(raw, inst->b, "%rdx"); xlocal_address(raw, inst->a, global, 0);
        if(raw->module->instrument_native)xwrite(raw,"(%rax)","%rdx",8);else fputs("    movq %rdx, (%rax)\n", out);
        if (init) { xlocal_address(raw, inst->a, global, 1); fputs("    movb $1, (%rax)\n", out); }
        break;
    }
    case BL_IR_DELETE_LOCAL: case BL_IR_DELETE_GLOBAL: {
        int global = inst->op == BL_IR_DELETE_GLOBAL; xlocal_address(raw, inst->a, global, 0); fputs("    movq $0, (%rax)\n", out);
        xlocal_address(raw, inst->a, global, 1); fputs("    movb $0, (%rax)\n", out); break;
    }
    case BL_IR_MFREE_LOCAL: case BL_IR_MFREE_GLOBAL: {
        int global = inst->op == BL_IR_MFREE_GLOBAL; xcheck_initialized(raw, inst->a, global); xlocal_address(raw, inst->a, global, 0);
        xread(raw,"(%rax)","%rax",8); fprintf(out, "    movq %%rax, %s\n", raw->windows ? "%rcx" : "%rdi"); xcall(raw, raw->module->instrument_native?"bpp_s_free":"free"); break;
    }
    case BL_IR_RELEASE: if(raw->module->debug_path)fputs("    nop\n",out);break;
    case BL_IR_BIT_NOT:
        xvalue(raw,inst->a,"%rax");fputs("    notq %rax\n",out);xstore(raw,inst->dst,"%rax");break;
    case BL_IR_BIT_AND:case BL_IR_BIT_OR:case BL_IR_BIT_XOR:
        xvalue(raw,inst->a,"%rax");xvalue(raw,inst->b,"%rdx");
        fprintf(out,"    %s %%rdx, %%rax\n",inst->op==BL_IR_BIT_AND?"andq":inst->op==BL_IR_BIT_OR?"orq":"xorq");xstore(raw,inst->dst,"%rax");break;
    case BL_IR_NEG:
        xvalue(raw, inst->a, "%rax"); fputs("    negq %rax\n    jno ", out); label(raw, "arith"); fputs("\n    movl $1, ", out); fputs(raw->windows ? "%ecx\n" : "%edi\n", out); xtrap(raw); label(raw, "arith"); fputs(":\n", out); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_NOT:
        xvalue(raw, inst->a, "%rax"); fputs("    testq %rax, %rax\n    setg %dl\n    setl %al\n    movzbl %dl, %edx\n    movzbl %al, %eax\n    subq %rdx, %rax\n", out); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx");
        fprintf(out, "    %s %%rdx, %%rax\n    jno ", inst->op == BL_IR_ADD ? "addq" : inst->op == BL_IR_SUB ? "subq" : "imulq");
        label(raw, "arith"); fputs("\n    movl $1, ", out); fputs(raw->windows ? "%ecx\n" : "%edi\n", out); xtrap(raw); label(raw, "arith"); fputs(":\n", out); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_DIV: case BL_IR_MOD:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rcx"); fputs("    testq %rcx, %rcx\n    jne ", out); label(raw, "divisor"); fputs("\n    movl $1, ", out); fputs(raw->windows ? "%ecx\n" : "%edi\n", out); xtrap(raw); label(raw, "divisor"); fputs(":\n", out);
        fputs("    cmpq $-1, %rcx\n    jne ", out); label(raw, "divide"); fputs("\n    movabsq $0x8000000000000000, %rdx\n    cmpq %rdx, %rax\n    jne ", out); label(raw, "divide"); fputs("\n    movl $1, ", out); fputs(raw->windows ? "%ecx\n" : "%edi\n", out); xtrap(raw); label(raw, "divide"); fputs(":\n    cqto\n    idivq %rcx\n", out); xstore(raw, inst->dst, inst->op == BL_IR_DIV ? "%rax" : "%rdx"); break;
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx"); fprintf(out, "    cmpq %%rdx, %%rax\n    movl $0, %%eax\n    set%s %%al\n    leaq -1(%%rax,%%rax), %%rax\n", xcondition(inst->op)); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_MALLOC:
        xvalue(raw, inst->a, "%rax"); fprintf(out, "    movq %%rax, %s\n", raw->windows ? "%rcx" : "%rdi"); xcall(raw, raw->module->instrument_native?"bpp_s_malloc":"malloc"); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_REMALLOC:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx");
        if (raw->windows) fputs("    movq %rax, %rcx\n", out); else fputs("    movq %rax, %rdi\n    movq %rdx, %rsi\n", out);
        xcall(raw, raw->module->instrument_native?"bpp_s_realloc":"realloc"); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_BYTE_LOAD:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx"); if(raw->module->instrument_native){if(raw->windows)fputs("    movq %rax, %rcx\n",out);else fputs("    movq %rax, %rdi\n    movq %rdx, %rsi\n",out);xcall(raw,"bpp_s_byte_load");}else fputs("    movzbq (%rax,%rdx), %rax\n",out); xstore(raw, inst->dst, "%rax"); break;
    case BL_IR_BYTE_STORE:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx"); xvalue(raw, inst->c, "%rcx"); if(raw->module->instrument_native){if(raw->windows)fputs("    movq %rcx, %r8\n    movq %rax, %rcx\n",out);else fputs("    movq %rax, %rdi\n    movq %rdx, %rsi\n    movq %rcx, %rdx\n",out);xcall(raw,"bpp_s_byte_store");}else fputs("    movb %cl, (%rax,%rdx)\n",out); break;
    case BL_IR_PRINT_BYTE:
        xvalue(raw, inst->a, "%rax"); fputs("    movb %al, 24(%r12)\n", out);
        if (raw->windows) fputs("    movl $1, %ecx\n    leaq 24(%r12), %rdx\n    movl $1, %r8d\n", out);
        else fputs("    movl $1, %edi\n    leaq 24(%r12), %rsi\n    movl $1, %edx\n", out);
        xcall(raw, raw->windows ? "_write" : "write"); break;
    case BL_IR_PRINT_RANGE:
        xvalue(raw, inst->a, "%rax"); xvalue(raw, inst->b, "%rdx");
        if(raw->module->instrument_native){if(raw->windows)fputs("    movq %rax, %rcx\n",out);else fputs("    movq %rax, %rdi\n    movq %rdx, %rsi\n",out);xcall(raw,"bpp_s_print_range");break;}
        if (raw->windows) fputs("    movq %rdx, %r8\n    movl $1, %ecx\n    movq %rax, %rdx\n", out);
        else fputs("    movl $1, %edi\n    movq %rax, %rsi\n", out);
        xcall(raw, raw->windows ? "_write" : "write"); break;
    case BL_IR_CALL: xuser_call(raw, inst); break;
    default: break;
    }
}

static void xemit_function(Raw *raw) {
    FILE *out = raw->out; layout(raw);
    if(raw->windows&&raw->module->debug_path)fprintf(out,".cv_func_id %zu\n",raw->function);
    if(raw->windows&&raw->module->debug_path)codeview_source_alias(out,raw->fn);
    procedure_label(out, raw->function); fputs(":\n", out);
    if(raw->windows){fputs(".seh_proc ",out);procedure_label(out,raw->function);fputc('\n',out);}
    else fputs("    .cfi_startproc\n",out);
    fputs("    pushq %rbp\n",out);
    if(raw->windows)fputs("    .seh_pushreg %rbp\n",out);else fputs("    .cfi_def_cfa_offset 16\n    .cfi_offset %rbp, -16\n",out);
    if(!raw->windows)fputs("    movq %rsp, %rbp\n    .cfi_def_cfa_register %rbp\n",out);
    static const char *const saves[]={"%r12","%r13","%r14","%r15","%rbx"};
    for(size_t s=0;s<sizeof saves/sizeof saves[0];++s){fprintf(out,"    pushq %s\n",saves[s]);if(raw->windows)fprintf(out,"    .seh_pushreg %s\n",saves[s]);else fprintf(out,"    .cfi_offset %s, -%zu\n",saves[s],24+s*8);}
    if(raw->windows)fputs("    pushq %rdi\n    .seh_pushreg %rdi\n    pushq %rsi\n    .seh_pushreg %rsi\n",out);
    if(raw->windows&&raw->bytes>=4096)fprintf(out,"    movq $%zu, %%rax\n    call ___chkstk_ms\n    subq %%rax, %%rsp\n",raw->bytes);
    else if(!raw->windows&&raw->bytes>=4096){
        fprintf(out,"    movq $%zu, %%r11\nL_raw_probe_%zu:\n    subq $4096, %%rsp\n    movb $0, (%%rsp)\n    subq $1, %%r11\n    jne L_raw_probe_%zu\n",raw->bytes/4096,raw->function,raw->function);
        if(raw->bytes%4096)fprintf(out,"    subq $%zu, %%rsp\n",raw->bytes%4096);
    }else fprintf(out,"    subq $%zu, %%rsp\n",raw->bytes);
    if(raw->windows)fprintf(out,"    .seh_stackalloc %zu\n    movq %%rsp, %%rbp\n    .seh_setframe %%rbp, 0\n    .seh_endprologue\n",raw->bytes);
    fputs("    movq %rsp, %r12\n",out);
    /* Keep the incoming ABI arguments in callee-saved registers while rep
     * clears every value cell and initialization flag.  Stack contents must
     * never decide whether a B++ local is initialized. */
    if (raw->windows) fputs("    movq %rcx, %r13\n    movq %rdx, %r14\n    movq %r8, %r15\n    movq %r9, %rbx\n", out);
    else fputs("    movq %rdi, %r13\n    movq %rsi, %r14\n    movq %rdx, %r15\n    movq %rcx, %rbx\n", out);
    if(raw->module->instrument_native){fprintf(out,"    movq $%zu, %s\n",raw->bytes,raw->windows?"%rcx":"%rdi");xcall(raw,"bpp_s_frame_enter");fputs("    movq %rax, %r12\n",out);}
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
            if(raw->module->debug_path){bpp_asm_location(out,raw->module,inst);if(raw->windows)bpp_asm_codeview_location(out,raw->module,inst,raw->function);debug_pc_label(raw,"begin");}
            xsite(raw,inst);
            if (inst->op == BL_IR_JUMP) { fprintf(out, "    jmp L_raw_f%zu_b%zu\n", raw->function, inst->a); if(raw->module->debug_path)debug_pc_label(raw,"end"); continue; }
            if (inst->op == BL_IR_BRANCH) { xvalue(raw, inst->a, "%rax"); fputs("    testq %rax, %rax\n    jg ", out); block_label(out, raw->function, inst->b); fputs("\n    jmp ", out); block_label(out, raw->function, inst->c); fputc('\n', out); if(raw->module->debug_path)debug_pc_label(raw,"end"); continue; }
            if (inst->op == BL_IR_RETURN) { xvalue(raw, inst->a, "%rax"); fputs("    movq 16(%r12), %rdx\n    movq %rax, (%rdx)\n    jmp ", out); procedure_done_label(out, raw); fputc('\n',out); if(raw->module->debug_path)debug_pc_label(raw,"end"); continue; }
            xemit_inst(raw, inst);
            if(raw->module->debug_path)debug_pc_label(raw,"end");
        }
    }
    procedure_done_label(out, raw); fputs(":\n", out);
    if(raw->module->instrument_native){fprintf(out,"    movq %%r12, %s\n",raw->windows?"%rcx":"%rdi");xcall(raw,"bpp_s_frame_leave");}
    fprintf(out,"    addq $%zu, %%rsp\n",raw->bytes);
    if(raw->windows)fputs("    popq %rsi\n    popq %rdi\n",out);
    for(size_t s=sizeof saves/sizeof saves[0];s-- >0;){fprintf(out,"    popq %s\n",saves[s]);if(!raw->windows)fprintf(out,"    .cfi_restore %s\n",saves[s]);}
    fputs("    popq %rbp\n",out);
    if(raw->windows){fputs("    ret\n",out);if(raw->module->debug_path){codeview_end_label(out,raw->function);fputs(":\n",out);}fputs("    .seh_endproc\n",out);}else fputs("    .cfi_def_cfa %rsp, 8\n    .cfi_restore %rbp\n    ret\n    .cfi_endproc\n",out);
    if(raw->macho){procedure_label(out,raw->function);fputs("_done:\n",out);}
    procedure_label(out,raw->function);fputs("_end:\n",out);
}

static void xemit_main(Raw *raw, int print_result) {
    size_t globals = raw->module->global_count ? raw->module->global_count : 1;
    FILE *out = raw->out;
    fputs(".globl main\nmain:\n", out);
    if(raw->windows)fputs("    .seh_proc main\n",out);else fputs("    .cfi_startproc\n",out);
    /* 48 bytes supplies the Windows home area plus a result word and keeps
     * rsp 16-byte aligned before every call on both x86-64 ABIs. */
    fputs("    pushq %rbp\n",out);
    if(raw->windows)fputs("    .seh_pushreg %rbp\n",out);else fputs("    .cfi_def_cfa_offset 16\n    .cfi_offset %rbp, -16\n",out);
    if(raw->windows)fputs("    subq $48, %rsp\n    .seh_stackalloc 48\n    movq %rsp, %rbp\n    .seh_setframe %rbp, 0\n    .seh_endprologue\n",out);else fputs("    movq %rsp, %rbp\n    .cfi_def_cfa_register %rbp\n    subq $48, %rsp\n",out);
    if(raw->module->instrument_native){fprintf(out,"    leaq bpp_raw_globals(%%rip), %s\n    movq $%zu, %s\n",raw->windows?"%rcx":"%rdi",globals*8,raw->windows?"%rdx":"%rsi");xcall(raw,"bpp_s_globals");}
    if (raw->windows) fputs("    leaq bpp_raw_globals(%rip), %rcx\n    leaq bpp_raw_global_flags(%rip), %rdx\n    xorq %r8, %r8\n    leaq 32(%rsp), %r9\n", out);
    else fputs("    leaq bpp_raw_globals(%rip), %rdi\n    leaq bpp_raw_global_flags(%rip), %rsi\n    xorq %rdx, %rdx\n    leaq 32(%rsp), %rcx\n", out);
    fputs("    call bpp_raw_proc_0", out); fputc('\n', out);
    if (print_result) {
        if (raw->windows) fputs("    movq 32(%rsp), %rdx\n    leaq L_raw_print_int(%rip), %rcx\n", out);
        else fputs("    movq 32(%rsp), %rsi\n    leaq L_raw_print_int(%rip), %rdi\n    xorl %eax, %eax\n", out);
        xcall(raw, "printf");
    }
    if(raw->module->instrument_native)xcall(raw,"bpp_s_finish");
    fputs("    xorl %eax, %eax\n",out);
    fputs(raw->windows?"    addq $48, %rsp\n    popq %rbp\n":"    leave\n",out);
    if(raw->windows)fputs("    ret\n    .seh_endproc\n",out);else fputs("    .cfi_def_cfa %rsp, 8\n    .cfi_restore %rbp\n    ret\n    .cfi_endproc\n",out);
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
static void asave(const Raw *raw){
    fputs("    sub sp, sp, #160\n",raw->out);
    for(unsigned r=0;r<18;r+=2)fprintf(raw->out,"    stp x%u, x%u, [sp, #%u]\n",r,r+1,r*8);
    fputs("    str x30, [sp, #144]\n    mrs x9, nzcv\n    str x9, [sp, #152]\n",raw->out);
}
static void arestore(const Raw *raw,const char *dst){
    if(dst)fprintf(raw->out,"    str x0, [sp, #%u]\n",(unsigned)strtoul(dst+1,NULL,10)*8);
    fputs("    ldr x9, [sp, #152]\n    msr nzcv, x9\n    ldr x30, [sp, #144]\n",raw->out);
    for(unsigned r=0;r<18;r+=2)fprintf(raw->out,"    ldp x%u, x%u, [sp, #%u]\n",r,r+1,r*8);
    fputs("    add sp, sp, #160\n",raw->out);
}
static void aread(const Raw *raw,const char *dst,size_t width){
    if(!raw->module->instrument_native){fprintf(raw->out,"    ldr %s, [x9]\n",dst);return;}
    asave(raw);fprintf(raw->out,"    ldr x0, [sp, #72]\n    mov x1, #%zu\n",width);acall(raw,"bpp_s_read");arestore(raw,dst);
}
static void awrite(const Raw *raw,const char *src){
    if(!raw->module->instrument_native){fprintf(raw->out,"    str %s, [x9]\n",src);return;}
    asave(raw);fprintf(raw->out,"    ldr x0, [sp, #72]\n    ldr x1, [sp, #%u]\n    mov x2, #8\n",(unsigned)strtoul(src+1,NULL,10)*8);acall(raw,"bpp_s_write");arestore(raw,NULL);
}
static void avalue(const Raw *raw, BLIRId reg, const char *dst) { aaddress(raw, "x9", "x19", raw->values + reg * 8); aread(raw,dst,8); }
static void astore(const Raw *raw, BLIRId reg, const char *src) { aaddress(raw, "x9", "x19", raw->values + reg * 8); awrite(raw,src); }
static void alocal_address(const Raw *raw, size_t slot, int global, int flags) { aaddress(raw, "x9", global ? (flags ? "x21" : "x20") : "x19", global ? (flags ? slot : slot * 8) : (flags ? raw->flags + slot : raw->locals + slot * 8)); }
static void atrap(const Raw *raw) { aimmediate(raw->out, "x0",raw->module->instrument_native?trap_category(raw):1); acall(raw,raw->module->instrument_native?"bpp_s_fail":"exit"); }
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
static void asite(const Raw *raw,const BLIRInst *i){
    if(!raw->module->instrument_native)return;
    asave(raw);char name[80];snprintf(name,sizeof name,"L_s_source_%zu",i->source_file==BL_IR_NONE?0:i->source_file);
    aemit_local_address(raw,"x0",name);aimmediate(raw->out,"x1",(uint64_t)i->source_line);aimmediate(raw->out,"x2",(uint64_t)i->source_column);
    acall(raw,"bpp_s_site");arestore(raw,NULL);
}
static void auser_call(const Raw *raw, const BLIRInst *inst) {
    for (size_t n = 0; n < inst->arg_count; ++n) { avalue(raw, inst->args[n], "x10"); aaddress(raw, "x9", "x19", raw->args + n * 8); fputs("    str x10, [x9]\n", raw->out); }
    fputs("    mov x0, x20\n    mov x1, x21\n", raw->out); aaddress(raw, "x2", "x19", raw->args); aaddress(raw, "x3", "x19", raw->values + inst->dst * 8);
    fputs("    bl ", raw->out); procedure_label(raw->out, inst->a); fputc('\n', raw->out);
}
static void aemit_inst(Raw *raw, const BLIRInst *inst) {
    FILE *out = raw->out;
    switch (inst->op) {
    case BL_IR_NULL: case BL_IR_INT: aimmediate(out, "x10", inst->op == BL_IR_INT ? (uint64_t)inst->integer : 0); astore(raw, inst->dst, "x10"); break;
    case BL_IR_LOAD_LOCAL: case BL_IR_LOAD_GLOBAL: { int g=inst->op==BL_IR_LOAD_GLOBAL; acheck_initialized(raw,inst->a,g); alocal_address(raw,inst->a,g,0); aread(raw,"x10",8); astore(raw,inst->dst,"x10"); break; }
    case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL: case BL_IR_STORE_LOCAL: case BL_IR_STORE_GLOBAL: { int g=inst->op==BL_IR_INIT_GLOBAL||inst->op==BL_IR_STORE_GLOBAL, init=inst->op==BL_IR_INIT_GLOBAL||inst->op==BL_IR_INIT_LOCAL; if(!init)acheck_initialized(raw,inst->a,g); avalue(raw,inst->b,"x10"); alocal_address(raw,inst->a,g,0); awrite(raw,"x10"); if(init){alocal_address(raw,inst->a,g,1);fputs("    mov w10, #1\n    strb w10, [x9]\n",out);}break; }
    case BL_IR_DELETE_LOCAL: case BL_IR_DELETE_GLOBAL: {int g=inst->op==BL_IR_DELETE_GLOBAL;alocal_address(raw,inst->a,g,0);fputs("    str xzr, [x9]\n",out);alocal_address(raw,inst->a,g,1);fputs("    strb wzr, [x9]\n",out);break;}
    case BL_IR_MFREE_LOCAL: case BL_IR_MFREE_GLOBAL: {int g=inst->op==BL_IR_MFREE_GLOBAL;acheck_initialized(raw,inst->a,g);alocal_address(raw,inst->a,g,0);aread(raw,"x0",8);acall(raw,raw->module->instrument_native?"bpp_s_free":"free");break;}
    case BL_IR_RELEASE: if(raw->module->debug_path)fputs("    nop\n",out);break;
    case BL_IR_BIT_NOT:
        avalue(raw,inst->a,"x10");fputs("    mvn x10, x10\n",out);astore(raw,inst->dst,"x10");break;
    case BL_IR_BIT_AND:case BL_IR_BIT_OR:case BL_IR_BIT_XOR:
        avalue(raw,inst->a,"x10");avalue(raw,inst->b,"x11");
        fprintf(out,"    %s x10, x10, x11\n",inst->op==BL_IR_BIT_AND?"and":inst->op==BL_IR_BIT_OR?"orr":"eor");astore(raw,inst->dst,"x10");break;
    case BL_IR_NEG: avalue(raw,inst->a,"x10");fputs("    negs x10, x10\n    b.vs ",out);label(raw,"trap");fputs("\n",out);astore(raw,inst->dst,"x10");fputs("    b ",out);label(raw,"done");fputs("\n",out);label(raw,"trap");fputs(":\n",out);atrap(raw);label(raw,"done");fputs(":\n",out);break;
    case BL_IR_NOT: avalue(raw,inst->a,"x10");fputs("    cmp x10, #0\n    cset x11, lt\n    cset x10, gt\n    sub x10, x11, x10\n",out);astore(raw,inst->dst,"x10");break;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: avalue(raw,inst->a,"x10");avalue(raw,inst->b,"x11"); if(inst->op==BL_IR_ADD)fputs("    adds x10, x10, x11\n    b.vs ",out);else if(inst->op==BL_IR_SUB)fputs("    subs x10, x10, x11\n    b.vs ",out);else fputs("    mul x12, x10, x11\n    smulh x13, x10, x11\n    asr x14, x12, #63\n    cmp x13, x14\n    b.ne ",out);label(raw,"trap");fputs("\n",out); if(inst->op==BL_IR_MUL)fputs("    mov x10, x12\n",out);astore(raw,inst->dst,"x10");fputs("    b ",out);label(raw,"done");fputs("\n",out);label(raw,"trap");fputs(":\n",out);atrap(raw);label(raw,"done");fputs(":\n",out);break;
    case BL_IR_DIV: case BL_IR_MOD: avalue(raw,inst->a,"x10");avalue(raw,inst->b,"x11");fputs("    cbnz x11, ",out);label(raw,"divisor");fputs("\n",out);atrap(raw);label(raw,"divisor");fputs(":\n",out);aimmediate(out,"x12",UINT64_MAX);fputs("    cmp x11, x12\n    b.ne ",out);label(raw,"divide");fputs("\n",out);aimmediate(out,"x12",UINT64_C(0x8000000000000000));fputs("    cmp x10, x12\n    b.ne ",out);label(raw,"divide");fputs("\n",out);atrap(raw);label(raw,"divide");fputs(":\n    sdiv x12, x10, x11\n",out);if(inst->op==BL_IR_DIV)fputs("    mov x10, x12\n",out);else fputs("    msub x10, x12, x11, x10\n",out);astore(raw,inst->dst,"x10");break;
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE: {const char *cc=inst->op==BL_IR_EQ?"eq":inst->op==BL_IR_NE?"ne":inst->op==BL_IR_LT?"lt":inst->op==BL_IR_LE?"le":inst->op==BL_IR_GT?"gt":"ge";avalue(raw,inst->a,"x10");avalue(raw,inst->b,"x11");fprintf(out,"    cmp x10, x11\n    cset x10, %s\n    lsl x10, x10, #1\n    sub x10, x10, #1\n",cc);astore(raw,inst->dst,"x10");break;}
    case BL_IR_MALLOC: avalue(raw,inst->a,"x0");acall(raw,raw->module->instrument_native?"bpp_s_malloc":"malloc");astore(raw,inst->dst,"x0");break;
    case BL_IR_REMALLOC: avalue(raw,inst->a,"x0");avalue(raw,inst->b,"x1");acall(raw,raw->module->instrument_native?"bpp_s_realloc":"realloc");astore(raw,inst->dst,"x0");break;
    case BL_IR_BYTE_LOAD: avalue(raw,inst->a,"x10");avalue(raw,inst->b,"x11");if(raw->module->instrument_native){fputs("    mov x0, x10\n    mov x1, x11\n",out);acall(raw,"bpp_s_byte_load");fputs("    mov x10, x0\n",out);}else fputs("    ldrb w10, [x10, x11]\n",out);astore(raw,inst->dst,"x10");break;
    case BL_IR_BYTE_STORE: avalue(raw,inst->a,"x10");avalue(raw,inst->b,"x11");avalue(raw,inst->c,"x12");if(raw->module->instrument_native){fputs("    mov x0, x10\n    mov x1, x11\n    mov x2, x12\n",out);acall(raw,"bpp_s_byte_store");}else fputs("    strb w12, [x10, x11]\n",out);break;
    case BL_IR_PRINT_BYTE: avalue(raw,inst->a,"x10");fputs("    strb w10, [x19, #24]\n    mov x0, #1\n    add x1, x19, #24\n    mov x2, #1\n",out);acall(raw,"write");break;
    case BL_IR_PRINT_RANGE: if(raw->module->instrument_native){avalue(raw,inst->a,"x0");avalue(raw,inst->b,"x1");acall(raw,"bpp_s_print_range");break;} avalue(raw,inst->a,"x1");avalue(raw,inst->b,"x2");fputs("    mov x0, #1\n",out);acall(raw,"write");break;
    case BL_IR_CALL: auser_call(raw,inst);break;
    default:break;
    }
    if(raw->module->debug_path)debug_pc_label(raw,"end");
}
static void aemit_function(Raw *raw) {
    FILE *out=raw->out;
    layout(raw);
    procedure_label(out,raw->function);
    fputs(":\n    .cfi_startproc\n"
          "    stp x29, x30, [sp, #-16]!\n"
          "    .cfi_def_cfa_offset 16\n"
          "    .cfi_offset x29, -16\n"
          "    .cfi_offset x30, -8\n"
          "    mov x29, sp\n"
          "    .cfi_def_cfa x29, 16\n"
          "    stp x19, x20, [sp, #-16]!\n"
          "    .cfi_offset x19, -32\n"
          "    .cfi_offset x20, -24\n"
          "    stp x21, x22, [sp, #-16]!\n"
          "    .cfi_offset x21, -48\n"
          "    .cfi_offset x22, -40\n"
          "    stp x23, x24, [sp, #-16]!\n"
          "    .cfi_offset x23, -64\n"
          "    .cfi_offset x24, -56\n",out);
    if(raw->bytes>=4096){
        aimmediate(out,"x9",raw->bytes/4096);
        fprintf(out,"L_raw_probe_%zu:\n    sub sp, sp, #1, lsl #12\n    str xzr, [sp]\n    subs x9, x9, #1\n    b.ne L_raw_probe_%zu\n",raw->function,raw->function);
        if(raw->bytes%4096)fprintf(out,"    sub sp, sp, #%zu\n",raw->bytes%4096);
    }else {aimmediate(out,"x9",raw->bytes);fputs("    sub sp, sp, x9\n",out);}
    fputs("    mov x19, sp\n"
          "    mov x20, x0\n    mov x21, x1\n"
          "    mov x22, x3\n    mov x23, x2\n",out);
    if(raw->module->instrument_native){aimmediate(out,"x0",raw->bytes);acall(raw,"bpp_s_frame_enter");fputs("    mov x19, x0\n",out);}
    aaddress(raw,"x9","x19",0);
    aimmediate(out,"x10",(raw->bytes+7)/8);
    fprintf(out,"L_raw_zero_%zu:\n    cbz x10, L_raw_zero_done_%zu\n"
                "    str xzr, [x9], #8\n    sub x10, x10, #1\n"
                "    b L_raw_zero_%zu\nL_raw_zero_done_%zu:\n",
            raw->function,raw->function,raw->function,raw->function);
    for(size_t p=0;p<raw->fn->param_count;++p){
        if(raw->module->debug_path)fputs("    .cfi_def_cfa x29, 16\n",out);
        aaddress(raw,"x9","x23",p*8);
        fputs("    ldr x10, [x9]\n",out);
        aaddress(raw,"x9","x19",raw->locals+p*8);
        fputs("    str x10, [x9]\n",out);
        aaddress(raw,"x9","x19",raw->flags+p);
        fputs("    mov w10, #1\n    strb w10, [x9]\n",out);
    }
    for(size_t b=0;b<raw->fn->block_count;++b){
        raw->block=b;
        block_label(out,raw->function,b);
        fputs(":\n",out);
        if(raw->module->debug_path)fputs("    .cfi_def_cfa x29, 16\n",out);
        for(size_t k=0;k<raw->fn->blocks[b].count;++k){
            raw->instruction=k;
            const BLIRInst *i=&raw->fn->blocks[b].insts[k];
            if(raw->module->debug_path){
                bpp_asm_location(out,raw->module,i);
                fputs("    .cfi_def_cfa x29, 16\n",out);
                debug_pc_label(raw,"begin");
            }
            asite(raw,i);
            if(i->op==BL_IR_JUMP){
                fprintf(out,"    b L_raw_f%zu_b%zu\n",raw->function,i->a);
                if(raw->module->debug_path)debug_pc_label(raw,"end");
                continue;
            }
            if(i->op==BL_IR_BRANCH){
                avalue(raw,i->a,"x10");
                fprintf(out,"    cmp x10, #0\n    b.gt L_raw_f%zu_b%zu\n"
                            "    b L_raw_f%zu_b%zu\n",
                        raw->function,i->b,raw->function,i->c);
                if(raw->module->debug_path)debug_pc_label(raw,"end");
                continue;
            }
            if(i->op==BL_IR_RETURN){
                avalue(raw,i->a,"x10");
                fputs("    str x10, [x22]\n    b ",out);
                procedure_done_label(out,raw);
                fputc('\n',out);
                if(raw->module->debug_path)debug_pc_label(raw,"end");
                continue;
            }
            aemit_inst(raw,i);
        }
    }
    procedure_done_label(out,raw);
    fputs(":\n",out);
    if(raw->module->debug_path)fputs("    .cfi_def_cfa x29, 16\n",out);
    if(raw->module->instrument_native){fputs("    mov x0, x19\n",out);acall(raw,"bpp_s_frame_leave");}
    aimmediate(out,"x9",raw->bytes);
    fputs("    add sp, sp, x9\n"
          "    ldp x23, x24, [sp], #16\n"
          "    ldp x21, x22, [sp], #16\n"
          "    ldp x19, x20, [sp], #16\n"
          "    ldp x29, x30, [sp], #16\n"
          "    .cfi_def_cfa sp, 0\n"
          "    ret\n    .cfi_endproc\n",out);
    if(raw->macho){procedure_label(out,raw->function);fputs("_done:\n",out);}
    procedure_label(out,raw->function);fputs("_end:\n",out);
}
static void aemit_main(Raw *raw,int print_result){FILE*out=raw->out;size_t globals=raw->module->global_count?raw->module->global_count:1;if(raw->macho)fputs(".globl _main\n_main:\n",out);else fputs(".globl main\n.type main, %function\nmain:\n",out);fputs("    .cfi_startproc\n    stp x29, x30, [sp, #-16]!\n    .cfi_def_cfa_offset 16\n    .cfi_offset x29, -16\n    .cfi_offset x30, -8\n    mov x29, sp\n    .cfi_def_cfa x29, 16\n    sub sp, sp, #16\n",out);if(raw->module->instrument_native){aemit_global_address(raw,"x0","bpp_raw_globals");aimmediate(out,"x1",globals*8);acall(raw,"bpp_s_globals");}aemit_global_address(raw,"x0","bpp_raw_globals");aemit_global_address(raw,"x1","bpp_raw_global_flags");fputs("    mov x2, xzr\n    mov x3, sp\n    bl bpp_raw_proc_0\n",out);if(print_result){fputs("    ldr x1, [sp]\n",out);aemit_local_address(raw,"x0","L_raw_print_int");acall(raw,"printf");}if(raw->module->instrument_native)acall(raw,"bpp_s_finish");fputs("    mov w0, #0\n    .cfi_def_cfa x29, 16\n    add sp, sp, #16\n    ldp x29, x30, [sp], #16\n    .cfi_def_cfa sp, 0\n    ret\n    .cfi_endproc\n",out);if(raw->macho){fputs(".section __TEXT,__cstring,cstring_literals\nL_raw_print_int:\n    .asciz \"%lld\\n\"\n.zerofill __DATA,__bss,_bpp_raw_globals,",out);fprintf(out,"%zu,3\n.zerofill __DATA,__bss,_bpp_raw_global_flags,%zu,0\n",globals*8,globals);}else{if(print_result)fputs(".section .rodata\nL_raw_print_int:\n    .asciz \"%lld\\n\"\n",out);fprintf(out,".comm bpp_raw_globals,%zu,8\n.comm bpp_raw_global_flags,%zu,1\n.section .note.GNU-stack,\"\",%%progbits\n",globals*8,globals);}}

int bpp_emit_raw_native(BLIRModule *module, FILE *out, int print_result, BPPTarget target) {
    const BPPBackendCapabilities *cap=bpp_target_capabilities(target);
    if(!module||!out||!cap||!bpp_raw_native_eligible(module))return 0;
    bpp_asm_source(out,module);
    Raw raw={0};raw.out=out;raw.module=module;raw.target=target;raw.windows=cap->format==BPP_FORMAT_COFF;raw.arm64=cap->isa==BPP_ISA_ARM64;raw.macho=cap->format==BPP_FORMAT_MACHO;
    if(raw.windows)bpp_asm_codeview_source(out,module);
    if(raw.arm64) fputs(raw.macho?".section __TEXT,__text,regular,pure_instructions\n.p2align 2\n":".text\n.p2align 2\n",out);else fputs(".text\n.p2align 4\n",out);
    int dwarf=module->debug_path&&(cap->format==BPP_FORMAT_MACHO||cap->format==BPP_FORMAT_ELF);
    if(dwarf)fputs("L_bpp_debug_start:\n",out);
    for(size_t f=0;f<module->function_count;++f){raw.function=f;raw.fn=&module->functions[f];if(raw.arm64)aemit_function(&raw);else xemit_function(&raw);}
    if(raw.arm64)aemit_main(&raw,print_result);else xemit_main(&raw,print_result);
    if(module->instrument_native){fputs(raw.macho?".section __TEXT,__cstring,cstring_literals\n":raw.windows?".section .rdata,\"dr\"\n":".section .rodata\n",out);
        size_t count=module->source_file_count?module->source_file_count:1;
        for(size_t f=0;f<count;++f){fprintf(out,"L_s_source_%zu:\n    .asciz ",f);json_text(out,module->source_file_count?module->source_files[f].path:module->path);fputc('\n',out);}
    }
    if(raw.windows)bpp_asm_codeview_symbols(out,module,1);
    if(dwarf){
        fputs(raw.macho?".section __TEXT,__text,regular,pure_instructions\nL_bpp_debug_end:\n":".text\nL_bpp_debug_end:\n",out);
        bpp_asm_compile_unit(out,module,target,"L_bpp_debug_start","L_bpp_debug_end",
                             "bpp_raw_proc_","bpp_raw_proc_","_end");
    }
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
            fprintf(out, ",\"representation\":\"raw-word\",\"declared_type\":null,\"location\":{\"kind\":\"frame-slot\",\"offset\":%zu,\"initialized_flag_offset\":%zu},\"location_status\":\"exact_raw_frame_slot\"}", raw.locals + p * 8, raw.flags + p);
        }
        fputs("],\"locals\":[", out);
        for (size_t l = fn->param_count; l < fn->local_count; ++l) {
            if (l != fn->param_count) fputc(',', out);
            fputs("{\"name\":", out); json_text(out, fn->locals[l]);
            fputs(",\"representation\":\"raw-word\",\"declared_type\":",out);
            if(fn->local_types&&fn->local_types[l])json_text(out,fn->local_types[l]);else fputs("null",out);
            fprintf(out, ",\"declared_storage\":{\"kind\":\"frame-slot\",\"offset\":%zu,\"initialized_flag_offset\":%zu},\"location_status\":\"exact_raw_frame_slot_when_initialized\",\"may_be_uninitialized\":true}", raw.locals + l * 8, raw.flags + l);
        }
        fputs("],\"source_variable_ranges\":",out);bpp_raw_variable_ranges(out,fn);
        fputs(",\"instructions\":[", out);
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
                fprintf(out, ",\"value_vreg\":%zu,\"value_location_role\":\"assigned_value_at_this_instruction\",\"value_location\":{\"kind\":\"frame-slot\",\"offset\":%zu}}", inst->b, raw.values + inst->b * 8);
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
