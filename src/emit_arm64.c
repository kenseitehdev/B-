#include "../include/compiler.h"
#include "../include/target.h"
#include "../include/codegen_abi.h"
#include "../include/regalloc.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { size_t locals, flags, args, bytes, maxargs; } Layout;
typedef struct { FILE *out; size_t f,b,k; const BLIRFunction *fn; Layout layout; const BPPRegPlan *plan; unsigned char *dirty, *before, *kind, *constant;long long *value;size_t near; } Emitter;
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
static int layout(const BLIRFunction *fn,const BPPRegPlan *plan, Layout *l) {
    l->maxargs=0;
    for(size_t b=0;b<fn->block_count;++b) for(size_t k=0;k<fn->blocks[b].count;++k)
        if(fn->blocks[b].insts[k].arg_count>l->maxargs) l->maxargs=fn->blocks[b].insts[k].arg_count;
    if(plan->cell_count>(SIZE_MAX-BPP_CG_HEADER_BYTES)/16) return 0;
    l->locals=BPP_CG_HEADER_BYTES+plan->cell_count*16;
    if(fn->local_count>(SIZE_MAX-l->locals)/16) return 0;
    l->flags=l->locals+fn->local_count*16;
    if(fn->local_count>SIZE_MAX-l->flags || l->flags+fn->local_count>SIZE_MAX-15) return 0;
    l->args=(l->flags+fn->local_count+15)&~(size_t)15;
    if(l->maxargs>(SIZE_MAX-l->args)/16) return 0;
    l->bytes=l->args+l->maxargs*16; return 1;
}
static size_t arm_saved_registers(const BPPRegPlan *plan,unsigned *ids){
    static const unsigned fixed[]={19,20,21,22,24,26};size_t n=0;
    for(size_t i=0;i<sizeof fixed/sizeof fixed[0];++i)ids[n++]=fixed[i];
    unsigned allocated[BPP_MAX_ALLOCATABLE_REGS];size_t count=bpp_reg_saved_set(plan,allocated,BPP_MAX_ALLOCATABLE_REGS);
    for(size_t i=0;i<count;++i){int found=0;for(size_t j=0;j<n;++j)if(ids[j]==allocated[i])found=1;if(!found)ids[n++]=allocated[i];}
    for(size_t i=1;i<n;++i){unsigned x=ids[i];size_t j=i;while(j&&ids[j-1]>x){ids[j]=ids[j-1];--j;}ids[j]=x;}return n;
}
static size_t arm_save_area(const BPPRegPlan *plan){unsigned ids[BPP_MAX_ALLOCATABLE_REGS];size_t n=arm_saved_registers(plan,ids);return (n*8+15)&~(size_t)15;}
static void arm_save_prologue(FILE *o,const BPPRegPlan *plan){
    unsigned ids[BPP_MAX_ALLOCATABLE_REGS];size_t n=arm_saved_registers(plan,ids),bytes=arm_save_area(plan);
    fputs("    stp x29, x30, [sp, #-16]!\n    .cfi_def_cfa_offset 16\n    .cfi_offset x29, -16\n    .cfi_offset x30, -8\n    mov x29, sp\n    .cfi_def_cfa x29, 16\n",o);
    if(bytes)fprintf(o,"    sub sp, sp, #%zu\n",bytes);
    for(size_t i=0;i<n;++i){size_t off=(i+1)*8;fprintf(o,"    str x%u, [x29, #-%zu]\n    .cfi_offset x%u, -%zu\n",ids[i],off,ids[i],16+off);}
}
static void arm_save_epilogue(FILE *o,const BPPRegPlan *plan){
    unsigned ids[BPP_MAX_ALLOCATABLE_REGS];size_t n=arm_saved_registers(plan,ids);
    fputs("    mov sp, x29\n",o);
    for(size_t i=0;i<n;++i){size_t off=(i+1)*8;fprintf(o,"    ldr x%u, [x29, #-%zu]\n    .cfi_restore x%u\n",ids[i],off,ids[i]);}
    fputs("    ldp x29, x30, [sp], #16\n    .cfi_def_cfa sp, 0\n    .cfi_restore x29\n    .cfi_restore x30\n    ret\n    .cfi_endproc\n",o);
}
static void label(Emitter *e,const char *name) { fprintf(e->out,"L_%s_%zu_%zu_%zu",name,e->f,e->b,e->k); }
static void farjump(FILE *o,const char *target,int call){
    addr(o,16,target);fprintf(o,"    %s x16\n",call?"blr":"br");
}
static void branch(Emitter *e,const char *instruction,const char *name) {
    char target[120];snprintf(target,sizeof target,"L_%s_%zu_%zu_%zu",name,e->f,e->b,e->k);
    if(!strcmp(instruction,"b")){farjump(e->out,target,0);return;}
    char inverse[64];
    if(!strncmp(instruction,"cbz ",4))snprintf(inverse,sizeof inverse,"cbnz %s",instruction+4);
    else if(!strncmp(instruction,"cbnz ",5))snprintf(inverse,sizeof inverse,"cbz %s",instruction+5);
    else snprintf(inverse,sizeof inverse,"%s",!strcmp(instruction,"b.ne")?"b.eq":!strcmp(instruction,"b.eq")?"b.ne":!strcmp(instruction,"b.vs")?"b.vc":"b.ne");
    size_t near=e->near++;
    fprintf(e->out,"    %s L_near_%zu_%zu\n",inverse,e->f,near);
    farjump(e->out,target,0);
    fprintf(e->out,"L_near_%zu_%zu:\n",e->f,near);
}
static void define(Emitter *e,const char *name) {label(e,name);fputs(":\n",e->out);}
/* x19=stack value-frame base; globals and init bytes reside in the header. */
static void offset(FILE *out,unsigned dst,unsigned base,size_t n) {
    if(n<=4095) fprintf(out,"    add x%u, x%u, #%zu\n",dst,base,n);
    else {imm(out,16,n);fprintf(out,"    add x%u, x%u, x16\n",dst,base);}
}
static void regaddr(Emitter *e,unsigned dst,size_t r) { offset(e->out,dst,19,BPP_CG_HEADER_BYTES+e->plan->cell[r]*16); }
static size_t position(Emitter *e){return bpp_reg_position(e->plan,e->b,e->k);}
static unsigned physical_at(Emitter *e,size_t r,size_t pos){BPPRegLocation loc=bpp_reg_location_at(e->plan,r,pos);return loc.kind==BPP_LOC_REGISTER?loc.physical:0;}
static void payload(Emitter *e,unsigned dst,size_t r) {
    unsigned physical=physical_at(e,r,position(e));
    if(physical) fprintf(e->out,"    mov x%u, x%u\n",dst,physical);
    else {regaddr(e,12,r);fprintf(e->out,"    ldr x%u, [x12, #8]\n",dst);}
}
/* Dirty payloads remain in callee-saved registers until a consumer needs cells.
 * Conditional slow paths flush the pre-instruction snapshot without changing
 * the conservative state at the fast/slow merge. */
static void flush(Emitter *e,unsigned char *dirty,int clear) {
    size_t pos=position(e);for(size_t r=0;r<e->fn->register_count;++r) if(dirty[r]) {
        unsigned physical=physical_at(e,r,pos);if(physical){regaddr(e,12,r);fprintf(e->out,"    str x%u, [x12, #8]\n",physical);}
        if(clear)dirty[r]=0;
    }
}
static void cache_result(Emitter *e,size_t r,unsigned src) {
    unsigned physical=physical_at(e,r,position(e));
    if(physical) {
        /* Preserve an old dirty owner before this physical register is reused. */
        for(size_t a=0;a<e->fn->register_count;++a)
            if(a!=r && e->dirty[a] && physical_at(e,a,position(e))==physical) {
                regaddr(e,12,a);fprintf(e->out,"    str x%u, [x12, #8]\n",physical);e->dirty[a]=0;
            }
        if(src==31) fprintf(e->out,"    mov x%u, xzr\n",physical); else fprintf(e->out,"    mov x%u, x%u\n",physical,src); }
}
static void cache_reload_at(Emitter *e,size_t r,size_t pos) {
    unsigned physical=physical_at(e,r,pos);
    if(r!=BL_IR_NONE&&physical){regaddr(e,12,r);fprintf(e->out,"    ldr x%u, [x12, #8]\n",physical);}
}
static void restore_live_after(Emitter *e){
    size_t pos=position(e)+1;
    for(size_t r=0;r<e->fn->register_count;++r)if(bpp_reg_live_after(e->plan,e->b,e->k,r))cache_reload_at(e,r,pos);
}
static void intguard(Emitter *e,size_t r) {if(e->kind[r]==BPP_KIND_INT)return;regaddr(e,12,r);fputs("    ldr w11, [x12]\n    cmp w11, #1\n",e->out);branch(e,"b.ne","slow");}
/* x26 retains the primitive result across an optional object-release call. */
static void result(Emitter *e,size_t dst,unsigned tag) {
    regaddr(e,12,dst);fputs("    ldr w11, [x12]\n    cmp w11, #2\n",e->out);branch(e,"b.ne","write");
    flush(e,e->dirty,0);
    fputs("    mov x0, x19\n",e->out);imm(e->out,1,dst);fputs("    bl _bpp_a_destination\n    mov x17, x0\n",e->out);restore_live_after(e);fputs("    mov x12, x17\n",e->out);
    define(e,"write");fprintf(e->out,"    mov w11, #%u\n    str w11, [x12]\n",tag);
    if(physical_at(e,dst,position(e))) {
        cache_result(e,dst,26);e->dirty[dst]=(unsigned char)(tag==1);
    } else if(tag==1) fputs("    str x26, [x12, #8]\n",e->out);
    branch(e,"b","done");
}
static void slotaddr(Emitter *e,unsigned dst,int global,size_t slot) {if(global)fputs("    ldr x12, [x19, #8]\n",e->out);offset(e->out,dst,global?12:19,(global?0:e->layout.locals)+slot*16);}
static void flagaddr(Emitter *e,unsigned dst,int global,size_t slot) {if(global)fputs("    ldr x12, [x19, #40]\n",e->out);offset(e->out,dst,global?12:19,(global?0:e->layout.flags)+slot);}
static int fast(Emitter *e,const BLIRInst *i) {
    FILE *o=e->out;
    switch(i->op) {
    case BL_IR_NULL: imm(o,26,0);result(e,i->dst,0);return 1;
    case BL_IR_INT: imm(o,26,(uint64_t)i->integer);result(e,i->dst,1);return 1;
    case BL_IR_RELEASE:
        regaddr(e,12,i->a);fputs("    ldr w11, [x12]\n    cmp w11, #2\n",o);branch(e,"b.eq","slow");
        fputs("    str wzr, [x12]\n",o);e->dirty[i->a]=0;cache_result(e,i->a,31);branch(e,"b","done");return 1;
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
        if(init){flagaddr(e,12,g,i->a);fputs("    mov w11, #1\n    strb w11, [x12]\n",o);}
        branch(e,"b","done");return 1;
    }
    case BL_IR_NOT:
        regaddr(e,12,i->a);fputs("    ldr w9, [x12]\n",o);branch(e,"cbz w9,","not_null");
        fputs("    cmp w9, #1\n",o);branch(e,"b.ne","not_object");payload(e,9,i->a);fputs("    cmp x9, #0\n    cset x26, eq\n",o);branch(e,"b","not_ready");
        define(e,"not_null");imm(o,26,1);branch(e,"b","not_ready");define(e,"not_object");imm(o,26,0);define(e,"not_ready");result(e,i->dst,1);return 1;
    case BL_IR_NEG:
        intguard(e,i->a);payload(e,9,i->a);fputs("    negs x26, x9\n",o);branch(e,"b.vs","slow");result(e,i->dst,1);return 1;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
        intguard(e,i->a);intguard(e,i->b);payload(e,9,i->a);
        if((i->op==BL_IR_ADD||i->op==BL_IR_SUB)&&e->constant[i->b]&&e->value[i->b]>=-4095&&e->value[i->b]<=4095){
            long long v=e->value[i->b];int subtract=(i->op==BL_IR_SUB)^(v<0);
            fprintf(o,"    %s x26, x9, #%lld\n",subtract?"subs":"adds",v<0?-v:v);branch(e,"b.vs","slow");result(e,i->dst,1);return 1;
        }
        payload(e,10,i->b);
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
static void slow(Emitter *e,const BLIRInst *i,int conditional) {
    flush(e,conditional?e->before:e->dirty,!conditional);
    FILE *o=e->out;fputs("    mov x0, x19\n",o);
    if(i->op==BL_IR_STRING){imm(o,1,i->dst);char s[96];snprintf(s,sizeof s,"L_text_%zu_%zu_%zu",e->f,e->b,e->k);addr(o,2,s);fputs("    bl _bpp_a_string\n",o);}
    else if(i->op==BL_IR_CALL) {
        for(size_t j=0;j<i->arg_count;++j){fputs("    mov x0, x19\n",o);imm(o,1,j);imm(o,2,i->args[j]);fputs("    bl _bpp_a_arg\n",o);}
        fputs("    mov x0, x19\n",o);
        if(i->a==BL_IR_NONE){imm(o,1,i->dst);char s[96];snprintf(s,sizeof s,"L_text_%zu_%zu_%zu",e->f,e->b,e->k);addr(o,2,s);imm(o,3,i->arg_count);imm(o,4,(uint64_t)i->line);imm(o,5,(uint64_t)i->column);fputs("    bl _bpp_a_builtin\n",o);}
        else{imm(o,1,i->dst);fputs("    bl _bpp_a_destination\n",o);fprintf(o,"    str x0, [x19, #%u]\n",BPP_CG_CALL_OUTPUT_OFFSET);fputs("    mov x0, x19\n    bl _bpp_a_args\n    mov x3, x0\n    ldr x0, [x19]\n    ldr x1, [x19, #8]\n    ldr x2, [x19, #40]\n",o);fprintf(o,"    ldr x4, [x19, #%u]\n",BPP_CG_CALL_OUTPUT_OFFSET);char target[64];snprintf(target,sizeof target,"L_proc_%zu",i->a);farjump(o,target,1);}
    } else{imm(o,1,i->op);imm(o,2,i->dst);imm(o,3,i->a);imm(o,4,i->b);imm(o,5,i->c);imm(o,6,(uint64_t)i->line);imm(o,7,(uint64_t)i->column);fputs("    bl _bpp_a_op\n",o);}
    fputs("    mov x0, x19\n    bl _bpp_a_failed\n",o);branch(e,"cbz w0,","post_call");char failed[64];snprintf(failed,sizeof failed,"L_fail_%zu",e->f);farjump(o,failed,0);
    define(e,"post_call");
    restore_live_after(e);
    branch(e,"b","done");
}
int bl_emit_arm64(BLIRModule *m,FILE *o,int print_result,BPPTarget target) {
    if(!o||!bl_ir_validate_pass(m,"ARM64 backend input")||!bpp_target_validate_ir(m,target,"ARM64"))return 0;
    Layout *layouts=calloc(m->function_count,sizeof *layouts);if(!layouts)return 0;
    BPPRegPlan *plans=calloc(m->function_count,sizeof *plans);
    if(!plans){free(layouts);return 0;}
    for(size_t f=0;f<m->function_count;++f) {
        if(!bpp_reg_plan_create_target(&m->functions[f],&plans[f],target)) {
            snprintf(m->error,sizeof m->error,"ARM64 register allocation failed in function '%s'",m->functions[f].name);
            for(size_t j=0;j<m->function_count;++j)bpp_reg_plan_destroy(&plans[j]);
            free(plans);free(layouts);return 0;
        }
        if(!bpp_reg_plan_validate(&m->functions[f],&plans[f])) {
            snprintf(m->error,sizeof m->error,"ARM64 register-map verifier failed in function '%s'",m->functions[f].name);
            for(size_t j=0;j<m->function_count;++j)bpp_reg_plan_destroy(&plans[j]);
            free(plans);free(layouts);return 0;
        }
        if(!layout(&m->functions[f],&plans[f],&layouts[f])) {
            snprintf(m->error,sizeof m->error,"ARM64 frame layout failed in function '%s'",m->functions[f].name);
        for(size_t j=0;j<m->function_count;++j)bpp_reg_plan_destroy(&plans[j]);
        free(plans);free(layouts);return 0;
        }
    }
    size_t maxregs=0;
    for(size_t f=0;f<m->function_count;++f)if(m->functions[f].register_count>maxregs)maxregs=m->functions[f].register_count;
    unsigned char *dirty=calloc(maxregs?maxregs:1,1),*before=calloc(maxregs?maxregs:1,1),*kind=calloc(maxregs?maxregs:1,1),*constant=calloc(maxregs?maxregs:1,1);
    long long *value=calloc(maxregs?maxregs:1,sizeof *value);
    if(!dirty||!before||!kind||!constant||!value){
        for(size_t f=0;f<m->function_count;++f)bpp_reg_plan_destroy(&plans[f]);
        free(dirty);free(before);free(kind);free(constant);free(value);free(plans);free(layouts);return 0;
    }
    bpp_asm_source(o,m);
    fputs("// B++ native codegen; compiler-private ABI placeholder revision 1\n.section __TEXT,__text,regular,pure_instructions\n.p2align 2\n.globl _main\n_main:\n    .cfi_startproc\n    stp x29, x30, [sp, #-16]!\n    .cfi_def_cfa_offset 16\n    .cfi_offset w29, -16\n    .cfi_offset w30, -8\n    mov x29, sp\n",o);
    addr(o,0,"L_proc_0");addr(o,1,"L_path");imm(o,2,m->global_count);imm(o,3,(uint64_t)(print_result!=0));
    fputs("    bl _bpp_a_run\n    ldp x29, x30, [sp], #16\n    .cfi_def_cfa_offset 0\n    .cfi_restore w29\n    .cfi_restore w30\n    ret\n    .cfi_endproc\n",o);
    for(size_t f=0;f<m->function_count;++f){const BLIRFunction *fn=&m->functions[f];Emitter e={o,f,0,0,fn,layouts[f],&plans[f],dirty,before,kind,constant,value,0};
        memset(dirty,0,fn->register_count);
        fprintf(o,"// payload registers: %zu of %zu IR values cached in x20/x21/x22/x23/x24/x25/x27/x28\n.p2align 2\nL_proc_%zu:\n    .cfi_startproc\n",plans[f].cached_count,fn->register_count,f);
        arm_save_prologue(o,&plans[f]);
        fputs("    mov x20, x0\n    mov x21, x1\n    mov x22, x2\n    mov x24, x4\n",o);
        imm(o,9,e.layout.bytes);fprintf(o,"L_probe_%zu:\n    cmp x9, #4096\n    b.lo L_probe_tail_%zu\n    sub sp, sp, #4096\n    str xzr, [sp]\n    sub x9, x9, #4096\n    b L_probe_%zu\nL_probe_tail_%zu:\n    sub sp, sp, x9\n    mov x19, sp\n",f,f,f,f);
        char name[64];snprintf(name,sizeof name,"L_layout_%zu",f);addr(o,4,name);fputs("    mov x5, x19\n    bl _bpp_n_enter\n    str x24, [x19, #96]\n",o);if(m->instrument_native)fputs("    ldr x0, [x29, #8]\n    bl _bpp_a_trace_enter\n",o);fprintf(o,"    b L_f%zu_b0\n",f);
        for(size_t b=0;b<fn->block_count;++b){e.b=b;memset(kind,0,fn->register_count);memset(constant,0,fn->register_count);fprintf(o,"L_f%zu_b%zu:\n",f,b);
            for(size_t r=0;r<fn->register_count;++r)if(bpp_reg_live_in(e.plan,b,r))cache_reload_at(&e,r,bpp_reg_position(e.plan,b,0));
            for(size_t k=0;k<fn->blocks[b].count;++k){e.k=k;const BLIRInst *i=&fn->blocks[b].insts[k];if(m->debug_path)bpp_asm_location(o,i);if(m->emit_asm_comments)bpp_asm_instruction_comment(o,fn,f,b,k,e.plan,BPP_MACOS_ARM64,0);
                if(m->instrument_native&&(i->op==BL_IR_LOAD_GLOBAL||i->op==BL_IR_STORE_GLOBAL||i->op==BL_IR_INIT_GLOBAL)){flush(&e,e.dirty,1);fputs("    mov x0, x19\n",o);imm(o,1,i->op);imm(o,2,i->a);fputs("    bl _bpp_a_access\n",o);}
                if(i->op==BL_IR_JUMP){flush(&e,e.dirty,1);if(i->a!=b+1){char target[64];snprintf(target,sizeof target,"L_f%zu_b%zu",f,i->a);farjump(o,target,0);}continue;}
                if(i->op==BL_IR_RETURN){flush(&e,e.dirty,1);imm(o,1,i->a);char target[64];snprintf(target,sizeof target,"L_cleanup_%zu",f);farjump(o,target,0);continue;}
                if(i->op==BL_IR_BRANCH){flush(&e,e.dirty,1);regaddr(&e,12,i->a);fputs("    ldr w9, [x12]\n",o);branch(&e,"cbz w9,","false");fputs("    cmp w9, #1\n",o);branch(&e,"b.ne","true");payload(&e,9,i->a);branch(&e,"cbnz x9,","true");define(&e,"false");{char target[64];snprintf(target,sizeof target,"L_f%zu_b%zu",f,i->c);farjump(o,target,0);}define(&e,"true");{char target[64];snprintf(target,sizeof target,"L_f%zu_b%zu",f,i->b);farjump(o,target,0);}continue;}
                memcpy(e.before,e.dirty,fn->register_count);
                int conditional=fast(&e,i);
                if(conditional) define(&e,"slow");
                slow(&e,i,conditional); define(&e,"done");bpp_known_update(kind,constant,value,i);
            }
        }
        fprintf(o,"L_fail_%zu:\n",f);imm(o,1,UINT64_MAX);fprintf(o,"L_cleanup_%zu:\n    mov x0, x19\n    ldr x2, [x19, #96]\n    bl _bpp_a_leave\n",f);
        if(m->instrument_native)fputs("    bl _bpp_a_trace_exit\n",o);
        arm_save_epilogue(o,&plans[f]);
    }
    fputs(".section __TEXT,__cstring,cstring_literals\n",o);literal(o,"L_path",m->path);
    for(size_t f=0;f<m->function_count;++f)for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){const BLIRInst *i=&m->functions[f].blocks[b].insts[k];if(i->text){char s[96];snprintf(s,sizeof s,"L_text_%zu_%zu_%zu",f,b,k);literal(o,s,i->text);}}
    fputs(".section __TEXT,__const\n.p2align 3\n",o);
    for(size_t f=0;f<m->function_count;++f){
        fprintf(o,"L_layout_%zu:\n    .quad %zu,%zu,%zu,%zu,%zu\n",f,m->functions[f].param_count,m->functions[f].register_count,m->functions[f].local_count,layouts[f].maxargs,plans[f].cell_count);
        for(size_t r=0;r<m->functions[f].register_count;++r)fprintf(o,"    .quad %zu\n",plans[f].cell[r]);
    }
    fputs(".subsections_via_symbols\n",o);
    for(size_t f=0;f<m->function_count;++f)bpp_reg_plan_destroy(&plans[f]);
    free(dirty);free(before);free(kind);free(constant);free(value);free(plans);free(layouts);return !ferror(o);
}
