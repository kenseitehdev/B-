#include "../include/compiler.h"
#include "../include/codegen_abi.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { size_t locals, flags, args, bytes, maxargs; } Layout;
typedef struct { FILE *out; size_t f,b,k; const BLIRFunction *fn; Layout layout; } Emitter;
static void imm(FILE *out, unsigned reg, uint64_t n) {
    fprintf(out,"    movz x%u, #%u\n",reg,(unsigned)(n&65535));
    for(unsigned s=16;s<64;s+=16) if((n>>s)&65535) fprintf(out,"    movk x%u, #%u, lsl #%u\n",reg,(unsigned)((n>>s)&65535),s);
}
static void addr(FILE *out,unsigned r,const char *label) {
    fprintf(out,"    adrp x%u, %s@PAGE\n    add x%u, x%u, %s@PAGEOFF\n",r,label,r,r,label);
}
static void literal(FILE *out,const char *label,const char *s) {
    fprintf(out,"%s:\n    .byte ",label); for(const unsigned char *p=(const unsigned char *)s;*p;++p) fprintf(out,"%u,",(unsigned)*p); fputs("0\n",out);
}
static int layout(const BLIRFunction *fn, Layout *l) {
    l->maxargs=0;
    for(size_t b=0;b<fn->block_count;++b) for(size_t k=0;k<fn->blocks[b].count;++k)
        if(fn->blocks[b].insts[k].arg_count>l->maxargs) l->maxargs=fn->blocks[b].insts[k].arg_count;
    if(fn->register_count>(SIZE_MAX-BPP_CG_HEADER_BYTES)/16) return 0;
    l->locals=BPP_CG_HEADER_BYTES+fn->register_count*16;
    if(fn->local_count>(SIZE_MAX-l->locals)/16) return 0;
    l->flags=l->locals+fn->local_count*16;
    if(fn->local_count>SIZE_MAX-l->flags || l->flags+fn->local_count>SIZE_MAX-15) return 0;
    l->args=(l->flags+fn->local_count+15)&~(size_t)15;
    if(l->maxargs>(SIZE_MAX-l->args)/16) return 0;
    l->bytes=l->args+l->maxargs*16; return 1;
}
static void label(Emitter *e,const char *name) { fprintf(e->out,"L_%s_%zu_%zu_%zu",name,e->f,e->b,e->k); }
static void branch(Emitter *e,const char *instruction,const char *name) { fprintf(e->out,"    %s ",instruction);label(e,name);fputc('\n',e->out); }
static void define(Emitter *e,const char *name) {label(e,name);fputs(":\n",e->out);}
/* x19=stack value-frame base; x21=globals; x22=global init bytes. */
static void offset(FILE *out,unsigned dst,unsigned base,size_t n) {
    if(n<=4095) fprintf(out,"    add x%u, x%u, #%zu\n",dst,base,n);
    else {imm(out,dst,n);fprintf(out,"    add x%u, x%u, x%u\n",dst,base,dst);}
}
static void regaddr(Emitter *e,unsigned dst,size_t r) { offset(e->out,dst,19,BPP_CG_HEADER_BYTES+r*16); }
static void payload(Emitter *e,unsigned dst,size_t r) {regaddr(e,12,r);fprintf(e->out,"    ldr x%u, [x12, #8]\n",dst);}
static void intguard(Emitter *e,size_t r) {regaddr(e,12,r);fputs("    ldr w11, [x12]\n    cmp w11, #1\n",e->out);branch(e,"b.ne","slow");}
/* x26 retains the primitive result across an optional object-release call. */
static void result(Emitter *e,size_t dst,unsigned tag) {
    regaddr(e,12,dst);fputs("    ldr w11, [x12]\n    cmp w11, #2\n",e->out);branch(e,"b.ne","write");
    fputs("    mov x0, x25\n",e->out);imm(e->out,1,dst);fputs("    bl _bpp_a_destination\n    mov x12, x0\n",e->out);
    define(e,"write");fprintf(e->out,"    mov w11, #%u\n    str w11, [x12]\n    str x26, [x12, #8]\n",tag);branch(e,"b","done");
}
static void slotaddr(Emitter *e,unsigned dst,int global,size_t slot) {offset(e->out,dst,global?21:19,(global?0:e->layout.locals)+slot*16);}
static void flagaddr(Emitter *e,unsigned dst,int global,size_t slot) {offset(e->out,dst,global?22:19,(global?0:e->layout.flags)+slot);}
static int fast(Emitter *e,const BLIRInst *i) {
    FILE *o=e->out;
    switch(i->op) {
    case BL_IR_NULL: imm(o,26,0);result(e,i->dst,0);return 1;
    case BL_IR_INT: imm(o,26,(uint64_t)i->integer);result(e,i->dst,1);return 1;
    case BL_IR_RELEASE:
        regaddr(e,12,i->a);fputs("    ldr w11, [x12]\n    cmp w11, #2\n",o);branch(e,"b.eq","slow");
        fputs("    str wzr, [x12]\n    str xzr, [x12, #8]\n",o);branch(e,"b","done");return 1;
    case BL_IR_LOAD_LOCAL: case BL_IR_LOAD_GLOBAL: {
        int g=i->op==BL_IR_LOAD_GLOBAL;flagaddr(e,12,g,i->a);fputs("    ldrb w11, [x12]\n",o);branch(e,"cbz w11,","slow");
        slotaddr(e,12,g,i->a);fputs("    ldr w11, [x12]\n    cmp w11, #1\n",o);branch(e,"b.ne","slow");fputs("    ldr x26, [x12, #8]\n",o);result(e,i->dst,1);return 1;
    }
    case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL: case BL_IR_STORE_LOCAL: case BL_IR_STORE_GLOBAL: {
        int g=i->op==BL_IR_INIT_GLOBAL||i->op==BL_IR_STORE_GLOBAL;
        int init=i->op==BL_IR_INIT_GLOBAL||i->op==BL_IR_INIT_LOCAL;
        intguard(e,i->b);
        if(!init){flagaddr(e,12,g,i->a);fputs("    ldrb w11, [x12]\n",o);branch(e,"cbz w11,","slow");}
        slotaddr(e,12,g,i->a);fputs("    ldr w11, [x12]\n    cmp w11, #2\n",o);branch(e,"b.eq","slow");
        payload(e,26,i->b);slotaddr(e,12,g,i->a);fputs("    mov w11, #1\n    str w11, [x12]\n    str x26, [x12, #8]\n",o);
        flagaddr(e,12,g,i->a);fputs("    mov w11, #1\n    strb w11, [x12]\n",o);branch(e,"b","done");return 1;
    }
    case BL_IR_NOT:
        regaddr(e,12,i->a);fputs("    ldr w9, [x12]\n",o);branch(e,"cbz w9,","not_null");
        fputs("    cmp w9, #1\n",o);branch(e,"b.ne","not_object");fputs("    ldr x9, [x12, #8]\n    cmp x9, #0\n    cset x26, eq\n",o);branch(e,"b","not_ready");
        define(e,"not_null");imm(o,26,1);branch(e,"b","not_ready");define(e,"not_object");imm(o,26,0);define(e,"not_ready");result(e,i->dst,1);return 1;
    case BL_IR_NEG:
        intguard(e,i->a);payload(e,9,i->a);fputs("    negs x26, x9\n",o);branch(e,"b.vs","slow");result(e,i->dst,1);return 1;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
        intguard(e,i->a);intguard(e,i->b);payload(e,9,i->a);payload(e,10,i->b);
        switch(i->op) {
        case BL_IR_ADD:fputs("    adds x26, x9, x10\n",o);branch(e,"b.vs","slow");break;
        case BL_IR_SUB:fputs("    subs x26, x9, x10\n",o);branch(e,"b.vs","slow");break;
        case BL_IR_MUL:fputs("    mul x26, x9, x10\n    smulh x11, x9, x10\n    cmp x11, x26, asr #63\n",o);branch(e,"b.ne","slow");break;
        case BL_IR_DIV:case BL_IR_MOD:
            branch(e,"cbz x10,","slow");imm(o,11,UINT64_MAX);fputs("    cmp x10, x11\n",o);branch(e,"b.ne","divide");
            imm(o,11,UINT64_C(1)<<63);fputs("    cmp x9, x11\n",o);branch(e,"b.eq","slow");define(e,"divide");
            fputs("    sdiv x26, x9, x10\n",o);if(i->op==BL_IR_MOD)fputs("    msub x26, x26, x10, x9\n",o);break;
        default: {
            const char *condition=i->op==BL_IR_EQ?"eq":i->op==BL_IR_NE?"ne":i->op==BL_IR_LT?"lt":i->op==BL_IR_LE?"le":i->op==BL_IR_GT?"gt":"ge";
            fprintf(o,"    cmp x9, x10\n    cset x26, %s\n",condition);break;
        }
        }
        result(e,i->dst,1);return 1;
    default:return 0;
    }
}
static void slow(Emitter *e,const BLIRInst *i) {
    FILE *o=e->out;fputs("    mov x0, x25\n",o);
    if(i->op==BL_IR_STRING){imm(o,1,i->dst);char s[96];snprintf(s,sizeof s,"L_text_%zu_%zu_%zu",e->f,e->b,e->k);addr(o,2,s);fputs("    bl _bpp_a_string\n",o);}
    else if(i->op==BL_IR_CALL) {
        for(size_t j=0;j<i->arg_count;++j){fputs("    mov x0, x25\n",o);imm(o,1,j);imm(o,2,i->args[j]);fputs("    bl _bpp_a_arg\n",o);}
        fputs("    mov x0, x25\n",o);
        if(i->a==BL_IR_NONE){imm(o,1,i->dst);char s[96];snprintf(s,sizeof s,"L_text_%zu_%zu_%zu",e->f,e->b,e->k);addr(o,2,s);imm(o,3,i->arg_count);imm(o,4,(uint64_t)i->line);imm(o,5,(uint64_t)i->column);fputs("    bl _bpp_a_builtin\n",o);}
        else{imm(o,1,i->dst);fputs("    bl _bpp_a_destination\n    mov x23, x0\n    mov x0, x25\n    bl _bpp_a_args\n    mov x3, x0\n    mov x0, x20\n    mov x1, x21\n    mov x2, x22\n    mov x4, x23\n",o);fprintf(o,"    bl L_proc_%zu\n",i->a);imm(o,23,UINT64_MAX);}
    } else{imm(o,1,i->op);imm(o,2,i->dst);imm(o,3,i->a);imm(o,4,i->b);imm(o,5,i->c);imm(o,6,(uint64_t)i->line);imm(o,7,(uint64_t)i->column);fputs("    bl _bpp_a_op\n",o);}
    fputs("    mov x0, x25\n    bl _bpp_a_failed\n",o);branch(e,"cbz w0,","done");fprintf(o,"    b L_fail_%zu\n",e->f);
}
int bl_emit_arm64(BLIRModule *m,FILE *o,int print_result) {
    if(!o||!bl_ir_validate(m))return 0;
    Layout *layouts=calloc(m->function_count,sizeof *layouts);if(!layouts)return 0;
    for(size_t f=0;f<m->function_count;++f)if(!layout(&m->functions[f],&layouts[f])){snprintf(m->error,sizeof m->error,"ARM64 frame size overflow");free(layouts);return 0;}
    fputs("// B++ native codegen; compiler-private ABI placeholder revision 0\n.section __TEXT,__text,regular,pure_instructions\n.p2align 2\n.globl _main\n_main:\n    .cfi_startproc\n    stp x29, x30, [sp, #-16]!\n    .cfi_def_cfa_offset 16\n    .cfi_offset w29, -16\n    .cfi_offset w30, -8\n    mov x29, sp\n",o);
    addr(o,0,"L_proc_0");addr(o,1,"L_path");imm(o,2,m->global_count);imm(o,3,(uint64_t)(print_result!=0));
    fputs("    bl _bpp_a_run\n    ldp x29, x30, [sp], #16\n    .cfi_def_cfa_offset 0\n    .cfi_restore w29\n    .cfi_restore w30\n    ret\n    .cfi_endproc\n",o);
    for(size_t f=0;f<m->function_count;++f){const BLIRFunction *fn=&m->functions[f];Emitter e={o,f,0,0,fn,layouts[f]};
        fprintf(o,".p2align 2\nL_proc_%zu:\n    .cfi_startproc\n",f);
        fputs("    stp x29, x30, [sp, #-80]!\n    .cfi_def_cfa_offset 80\n    .cfi_offset w29, -80\n    .cfi_offset w30, -72\n    mov x29, sp\n    .cfi_def_cfa w29, 80\n    stp x19, x20, [sp, #16]\n    stp x21, x22, [sp, #32]\n    stp x23, x24, [sp, #48]\n    stp x25, x26, [sp, #64]\n    .cfi_offset w19, -64\n    .cfi_offset w20, -56\n    .cfi_offset w21, -48\n    .cfi_offset w22, -40\n    .cfi_offset w23, -32\n    .cfi_offset w24, -24\n    .cfi_offset w25, -16\n    .cfi_offset w26, -8\n    mov x20, x0\n    mov x21, x1\n    mov x22, x2\n    mov x24, x4\n",o);
        imm(o,9,e.layout.bytes);fprintf(o,"L_probe_%zu:\n    cmp x9, #4096\n    b.lo L_probe_tail_%zu\n    sub sp, sp, #4096\n    str xzr, [sp]\n    sub x9, x9, #4096\n    b L_probe_%zu\nL_probe_tail_%zu:\n    sub sp, sp, x9\n    mov x19, sp\n",f,f,f,f);
        char name[64];snprintf(name,sizeof name,"L_layout_%zu",f);addr(o,4,name);fputs("    mov x5, x19\n    bl _bpp_n_enter\n    mov x25, x0\n",o);imm(o,23,UINT64_MAX);fprintf(o,"    b L_f%zu_b0\n",f);
        for(size_t b=0;b<fn->block_count;++b){e.b=b;fprintf(o,"L_f%zu_b%zu:\n",f,b);
            for(size_t k=0;k<fn->blocks[b].count;++k){e.k=k;const BLIRInst *i=&fn->blocks[b].insts[k];fprintf(o,"    // IR %d at %d:%d\n",(int)i->op,i->line,i->column);
                if(i->op==BL_IR_JUMP){fprintf(o,"    b L_f%zu_b%zu\n",f,i->a);continue;}
                if(i->op==BL_IR_RETURN){imm(o,23,i->a);fprintf(o,"    b L_cleanup_%zu\n",f);continue;}
                if(i->op==BL_IR_BRANCH){regaddr(&e,12,i->a);fputs("    ldr w9, [x12]\n",o);branch(&e,"cbz w9,","false");fputs("    cmp w9, #1\n",o);branch(&e,"b.ne","true");fputs("    ldr x9, [x12, #8]\n",o);branch(&e,"cbnz x9,","true");define(&e,"false");fprintf(o,"    b L_f%zu_b%zu\n",f,i->c);define(&e,"true");fprintf(o,"    b L_f%zu_b%zu\n",f,i->b);continue;}
                if(fast(&e,i)) define(&e,"slow");
                slow(&e,i); define(&e,"done");
            }
        }
        fprintf(o,"L_fail_%zu:\n",f);imm(o,23,UINT64_MAX);fprintf(o,"L_cleanup_%zu:\n    mov x0, x25\n    mov x1, x23\n    mov x2, x24\n    bl _bpp_a_leave\n    mov sp, x29\n",f);
        fputs("    ldp x19, x20, [sp, #16]\n    ldp x21, x22, [sp, #32]\n    ldp x23, x24, [sp, #48]\n    ldp x25, x26, [sp, #64]\n    ldp x29, x30, [sp], #80\n    .cfi_def_cfa sp, 0\n    .cfi_restore w19\n    .cfi_restore w20\n    .cfi_restore w21\n    .cfi_restore w22\n    .cfi_restore w23\n    .cfi_restore w24\n    .cfi_restore w25\n    .cfi_restore w26\n    .cfi_restore w29\n    .cfi_restore w30\n    ret\n    .cfi_endproc\n",o);
    }
    fputs(".section __TEXT,__cstring,cstring_literals\n",o);literal(o,"L_path",m->path);
    for(size_t f=0;f<m->function_count;++f)for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){const BLIRInst *i=&m->functions[f].blocks[b].insts[k];if(i->text){char s[96];snprintf(s,sizeof s,"L_text_%zu_%zu_%zu",f,b,k);literal(o,s,i->text);}}
    fputs(".section __TEXT,__const\n.p2align 3\n",o);
    for(size_t f=0;f<m->function_count;++f)fprintf(o,"L_layout_%zu:\n    .quad %zu,%zu,%zu,%zu\n",f,m->functions[f].param_count,m->functions[f].register_count,m->functions[f].local_count,layouts[f].maxargs);
    fputs(".subsections_via_symbols\n",o);free(layouts);return !ferror(o);
}
