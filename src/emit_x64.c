#include "../include/target.h"
#include "../include/codegen_abi.h"
#include "../include/regalloc.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {FILE *out;const BLIRFunction *fn;BPPRegPlan plan;size_t f,b,k,locals,flags,args,bytes,maxargs,alloc,saved_count;int win,asm_comments;unsigned saved[10];unsigned char *dirty,*before,*kind,*constant;unsigned *loaded_phys;long long *value;} X;
static const char *cache(const X *e,unsigned p){static char name[16];const BPPRegisterInfo *r=bpp_target_register(e->win?BPP_WINDOWS_X64:BPP_LINUX_X64,p);if(!r)return "%rax";snprintf(name,sizeof name,"%%%s",r->name);return name;}
static size_t save_set(X *e){
    e->saved[e->saved_count++]=5; /* rbp is the stable frame base. */
    e->saved[e->saved_count++]=12; /* r12 is the B++ runtime frame. */
    unsigned allocated[BPP_MAX_ALLOCATABLE_REGS];size_t n=bpp_reg_saved_set(&e->plan,allocated,BPP_MAX_ALLOCATABLE_REGS);
    for(size_t i=0;i<n;++i){int seen=0;for(size_t j=0;j<e->saved_count;++j)if(e->saved[j]==allocated[i])seen=1;if(!seen)e->saved[e->saved_count++]=allocated[i];}
    /* rbp must be pushed first; keep the remaining save order stable. */
    for(size_t i=2;i<e->saved_count;++i)for(size_t j=i;j>2&&e->saved[j]<e->saved[j-1];--j){unsigned t=e->saved[j];e->saved[j]=e->saved[j-1];e->saved[j-1]=t;}
    return e->saved_count;
}
static size_t cell(X *e,size_t r){return BPP_CG_HEADER_BYTES+e->plan.cell[r]*16;}
static size_t position(X *e){return bpp_reg_position(&e->plan,e->b,e->k);}
static unsigned physical_at(X *e,size_t r,size_t pos){BPPRegLocation loc=bpp_reg_location_at(&e->plan,r,pos);return loc.kind==BPP_LOC_REGISTER?loc.physical:0;}
static void spill_note(X *e,size_t r,BPPSpillReason reason,unsigned physical){if(e->asm_comments)fprintf(e->out,"    # B++ spill vreg=v%zu physical=%s cell=%zu reason=%s\n",r,cache(e,physical),e->plan.cell[r],bpp_reg_spill_reason_name(reason));}
static void arg(X *e,unsigned n,const char *source,int address){
    static const char *unixregs[]={"%rdi","%rsi","%rdx","%rcx","%r8","%r9"};
    static const char *winregs[]={"%rcx","%rdx","%r8","%r9"};
    unsigned count=e->win?4:6;const char *reg=n<count?(e->win?winregs[n]:unixregs[n]):"%rax";
    fprintf(e->out,"    %s %s, %s\n",address?"leaq":"movq",source,reg);
    if(n>=count)fprintf(e->out,"    movq %%rax, %u(%%rsp)\n",e->win?32+(n-4)*8:(n-6)*8);
}
static void number(X *e,unsigned n,uint64_t v){char s[48];snprintf(s,sizeof s,"$%llu",(unsigned long long)v);arg(e,n,s,0);}
static void call(X *e,const char *name){fprintf(e->out,"    call %s%s\n",name,e->win?"":"@PLT");}
static void local(X *e,const char *name){fprintf(e->out,"L_%s_%zu_%zu_%zu",name,e->f,e->b,e->k);}
static void def(X *e,const char *name){local(e,name);
    fputs(":\n",e->out);}
static void jump(X *e,const char *op,const char *name){fprintf(e->out,"    %s ",op);local(e,name);fputc('\n',e->out);}
static void payload(X *e,size_t r,const char *dst){
    unsigned p=physical_at(e,r,position(e));
    if(p){if(e->loaded_phys[r]!=p){if(e->asm_comments)fprintf(e->out,"    # B++ reload vreg=v%zu physical=%s cell=%zu reason=%s\n",r,cache(e,p),e->plan.cell[r],e->loaded_phys[r]?"allocation-segment-change":"first-payload-use");fprintf(e->out,"    movq %zu(%%r12), %s\n",cell(e,r)+8,cache(e,p));e->loaded_phys[r]=p;}fprintf(e->out,"    movq %s, %s\n",cache(e,p),dst);}
    else fprintf(e->out,"    movq %zu(%%r12), %s\n",cell(e,r)+8,dst);
}
static void flush(X *e,unsigned char *dirty,int clear){size_t pos=position(e);for(size_t r=0;r<e->fn->register_count;++r)if(dirty[r]){
    BPPSpillReason reason=bpp_reg_spill_reason_before(&e->plan,e->b,e->k,r);if(reason!=BPP_SPILL_NONE){unsigned p=physical_at(e,r,pos);if(p){spill_note(e,r,reason,p);fprintf(e->out,"    movq %s, %zu(%%r12)\n",cache(e,p),cell(e,r)+8);}}
    if(clear)dirty[r]=0;
}}
static void edge_flush(X *e,size_t to){size_t pos=position(e);for(size_t r=0;r<e->fn->register_count;++r)if(e->dirty[r]){
    BPPSpillReason reason=bpp_reg_spill_reason_on_edge(&e->plan,e->b,to,r);unsigned p=physical_at(e,r,pos);if(reason!=BPP_SPILL_NONE&&p){spill_note(e,r,reason,p);fprintf(e->out,"    movq %s, %zu(%%r12)\n",cache(e,p),cell(e,r)+8);}
}}
static void cached(X *e,size_t r,const char *source){unsigned p=physical_at(e,r,position(e));
    if(!p)return;
    for(size_t a=0;a<e->fn->register_count;++a)if(a!=r&&physical_at(e,a,position(e))==p){if(e->dirty[a]){fprintf(e->out,"    movq %s, %zu(%%r12)\n",cache(e,p),cell(e,a)+8);e->dirty[a]=0;}e->loaded_phys[a]=0;}
    fprintf(e->out,"    movq %s, %s\n",source,cache(e,p));
    e->loaded_phys[r]=p;
}
static void reload_at(X *e,size_t r,size_t pos){unsigned p=physical_at(e,r,pos);if(r!=BL_IR_NONE&&p){if(e->asm_comments){const BLIRInst *i=&e->fn->blocks[e->b].insts[e->k];const char *reason=i->dst==r?"helper-defined-result":"segment-transition-after-clobber";fprintf(e->out,"    # B++ reload vreg=v%zu physical=%s cell=%zu reason=%s\n",r,cache(e,p),e->plan.cell[r],reason);}fprintf(e->out,"    movq %zu(%%r12), %s\n",cell(e,r)+8,cache(e,p));e->loaded_phys[r]=p;}}
static void restore_live_after(X *e){size_t old=position(e),pos=old+1;const BLIRInst *i=&e->fn->blocks[e->b].insts[e->k];size_t dst=i->op==BL_IR_RELEASE?i->a:i->dst;for(size_t r=0;r<e->fn->register_count;++r)if(bpp_reg_live_after(&e->plan,e->b,e->k,r)){unsigned before=physical_at(e,r,old),after=physical_at(e,r,pos);if(after&&(after!=before||r==dst))reload_at(e,r,pos);}}
static void guard(X *e,size_t r){if(e->kind[r]==BPP_KIND_INT)return;fprintf(e->out,"    cmpl $1, %zu(%%r12)\n",cell(e,r));jump(e,"jne","slow");}
static void result(X *e,size_t r,int tag){
    fprintf(e->out,"    cmpl $2, %zu(%%r12)\n",cell(e,r));jump(e,"jne","write");
    flush(e,e->dirty,0);
    fputs("    movq %r10, 104(%r12)\n",e->out);
    arg(e,0,"%r12",0);number(e,1,r);call(e,"bpp_a_destination");
    fputs("    movq 104(%r12), %r10\n",e->out);
    restore_live_after(e);
    def(e,"write");fprintf(e->out,"    movl $%d, %zu(%%r12)\n",tag,cell(e,r));
    if(physical_at(e,r,position(e))){cached(e,r,"%r10");e->dirty[r]=(unsigned char)(tag==1);}
    else if(tag==1)fprintf(e->out,"    movq %%r10, %zu(%%r12)\n",cell(e,r)+8);
    jump(e,"jmp","done");
}
static void slot(X *e,size_t n,int g,int flag){
    if(g){fprintf(e->out,"    movq %u(%%r12), %%rax\n",flag?40:8);
    if(n)fprintf(e->out,"    addq $%zu, %%rax\n",flag?n:n*16);}
    else fprintf(e->out,"    leaq %zu(%%r12), %%rax\n",flag?e->flags+n:e->locals+n*16);
}
static int fast(X *e,const BLIRInst *i){FILE *o=e->out;
    switch(i->op){
    case BL_IR_NULL:case BL_IR_INT:fprintf(o,"    movq $%lld, %%r10\n",i->op==BL_IR_INT?i->integer:0LL);result(e,i->dst,i->op==BL_IR_INT);return 1;
    case BL_IR_RELEASE:
        fprintf(o,"    cmpl $2, %zu(%%r12)\n",cell(e,i->a));jump(e,"je","slow");
        fprintf(o,"    movl $0, %zu(%%r12)\n",cell(e,i->a));e->dirty[i->a]=0;cached(e,i->a,"$0");jump(e,"jmp","done");return 1;
    case BL_IR_LOAD_LOCAL:case BL_IR_LOAD_GLOBAL:{int g=i->op==BL_IR_LOAD_GLOBAL;
        slot(e,i->a,g,1);
    fputs("    cmpb $0, (%rax)\n",o);jump(e,"je","slow");
        slot(e,i->a,g,0);
    fputs("    cmpl $1, (%rax)\n",o);jump(e,"jne","slow");
    fputs("    movq 8(%rax), %r10\n",o);result(e,i->dst,1);return 1;}
    case BL_IR_STORE_LOCAL:case BL_IR_STORE_GLOBAL:case BL_IR_INIT_LOCAL:case BL_IR_INIT_GLOBAL:{
        int g=i->op==BL_IR_STORE_GLOBAL||i->op==BL_IR_INIT_GLOBAL,init=i->op==BL_IR_INIT_GLOBAL||i->op==BL_IR_INIT_LOCAL;
        guard(e,i->b);
    if(!init){slot(e,i->a,g,1);
    fputs("    cmpb $0, (%rax)\n",o);jump(e,"je","slow");}
        slot(e,i->a,g,0);
    fputs("    cmpl $2, (%rax)\n",o);jump(e,"je","slow");payload(e,i->b,"%r10");
        fputs("    movl $1, (%rax)\n    movq %r10, 8(%rax)\n",o);
        if(init){slot(e,i->a,g,1);
    fputs("    movb $1, (%rax)\n",o);}jump(e,"jmp","done");return 1;}
    case BL_IR_NOT:
        fprintf(o,"    cmpl $0, %zu(%%r12)\n",cell(e,i->a));jump(e,"je","not_null");
        fprintf(o,"    cmpl $1, %zu(%%r12)\n",cell(e,i->a));jump(e,"jne","not_object");
        payload(e,i->a,"%r10");
    fputs("    testq %r10, %r10\n    movl $0, %r10d\n    sete %r10b\n",o);jump(e,"jmp","not_ready");
        def(e,"not_null");
    fputs("    movl $1, %r10d\n",o);jump(e,"jmp","not_ready");def(e,"not_object");
    fputs("    movl $0, %r10d\n",o);def(e,"not_ready");result(e,i->dst,1);return 1;
    case BL_IR_NEG:guard(e,i->a);payload(e,i->a,"%r10");
    fputs("    negq %r10\n",o);jump(e,"jo","slow");result(e,i->dst,1);return 1;
    case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:
    case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:
        guard(e,i->a);guard(e,i->b);payload(e,i->a,"%r10");
        if((i->op==BL_IR_ADD||i->op==BL_IR_SUB||i->op==BL_IR_MUL)&&e->constant[i->b]&&e->value[i->b]>=INT_MIN&&e->value[i->b]<=INT_MAX){
            fprintf(o,"    %s $%lld, %%r10%s\n",i->op==BL_IR_ADD?"addq":i->op==BL_IR_SUB?"subq":"imulq",e->value[i->b],i->op==BL_IR_MUL?", %r10":"");jump(e,"jo","slow");result(e,i->dst,1);return 1;
        }
        payload(e,i->b,"%r11");
        if(i->op==BL_IR_ADD||i->op==BL_IR_SUB||i->op==BL_IR_MUL){fprintf(o,"    %s %%r11, %%r10\n",i->op==BL_IR_ADD?"addq":i->op==BL_IR_SUB?"subq":"imulq");jump(e,"jo","slow");}
        else if(i->op==BL_IR_DIV||i->op==BL_IR_MOD){
            fputs("    testq %r11, %r11\n",o);jump(e,"je","slow");
    fputs("    cmpq $-1, %r11\n",o);jump(e,"jne","divide");
            fputs("    movabsq $0x8000000000000000, %rax\n    cmpq %rax, %r10\n",o);jump(e,"je","slow");def(e,"divide");
            fputs("    movq %r10, %rax\n    cqto\n    idivq %r11\n",o);fprintf(o,"    movq %s, %%r10\n",i->op==BL_IR_MOD?"%rdx":"%rax");
        }else{const char *cc=i->op==BL_IR_EQ?"e":i->op==BL_IR_NE?"ne":i->op==BL_IR_LT?"l":i->op==BL_IR_LE?"le":i->op==BL_IR_GT?"g":"ge";
            fprintf(o,"    cmpq %%r11, %%r10\n    movl $0, %%r10d\n    set%s %%r10b\n",cc);}
        result(e,i->dst,1);return 1;
    default:return 0;
    }
}
static void textref(X *e,unsigned argn){char s[96];snprintf(s,sizeof s,"L_text_%zu_%zu_%zu(%%rip)",e->f,e->b,e->k);arg(e,argn,s,1);}
static void slow(X *e,const BLIRInst *i,int conditional){flush(e,conditional?e->before:e->dirty,!conditional);arg(e,0,"%r12",0);
    if(i->op==BL_IR_STRING){number(e,1,i->dst);textref(e,2);call(e,"bpp_a_string");}
    else if(i->op==BL_IR_CALL){
        for(size_t j=0;j<i->arg_count;++j){arg(e,0,"%r12",0);number(e,1,j);number(e,2,i->args[j]);call(e,"bpp_a_arg");}
        arg(e,0,"%r12",0);
        if(i->a==BL_IR_NONE){number(e,1,i->dst);textref(e,2);number(e,3,i->arg_count);number(e,4,(uint64_t)i->line);number(e,5,(uint64_t)i->column);call(e,"bpp_a_builtin");}
        else{number(e,1,i->dst);call(e,"bpp_a_destination");
    fputs("    movq %rax, 72(%r12)\n",e->out);arg(e,0,"%r12",0);call(e,"bpp_a_args");
            arg(e,3,"%rax",0);arg(e,0,"0(%r12)",0);arg(e,1,"8(%r12)",0);arg(e,2,"40(%r12)",0);arg(e,4,"72(%r12)",0);fprintf(e->out,"    call L_proc_%zu\n",i->a);}
    }else{number(e,1,i->op);number(e,2,i->dst);number(e,3,i->a);number(e,4,i->b);number(e,5,i->c);number(e,6,(uint64_t)i->line);number(e,7,(uint64_t)i->column);call(e,"bpp_a_op");}
    arg(e,0,"%r12",0);call(e,"bpp_a_failed");
    fputs("    testl %eax, %eax\n",e->out);jump(e,"je","post_call");fprintf(e->out,"    jmp L_fail_%zu\n",e->f);def(e,"post_call");
    restore_live_after(e);
    jump(e,"jmp","done");
}
static void literal(FILE *o,const char *name,const char *s){fprintf(o,"%s:\n    .byte ",name);for(const unsigned char *p=(const unsigned char *)s;*p;++p)fprintf(o,"%u,",(unsigned)*p);
    fputs("0\n",o);}
int bpp_emit_x64(BLIRModule *m,FILE *o,int print,int win){
    BPPTarget target=win?BPP_WINDOWS_X64:BPP_LINUX_X64;
    if(!bl_ir_validate_pass(m,"x86-64 backend input")||!bpp_target_validate_ir(m,target,"x86-64"))return 0;
    X *all=calloc(m->function_count,sizeof *all);
    if(!all)return 0;
    int ok=1;size_t failed_fn=0;
    for(size_t f=0;f<m->function_count;++f){X *e=&all[f];e->out=o;e->win=win;e->asm_comments=m->emit_asm_comments;e->f=f;e->fn=&m->functions[f];
        if(!bpp_reg_plan_create_target(e->fn,&e->plan,target)){snprintf(m->error,sizeof m->error,"x86-64 register allocation failed in function '%s'",e->fn->name);ok=0;
    break;}
        if(!bpp_reg_plan_validate(e->fn,&e->plan)){snprintf(m->error,sizeof m->error,"x86-64 register-map verifier failed in function '%s'",e->fn->name);ok=0;
    break;}
        size_t n=e->fn->register_count;e->dirty=calloc(n?n:1,1);e->before=calloc(n?n:1,1);e->kind=calloc(n?n:1,1);e->constant=calloc(n?n:1,1);e->loaded_phys=calloc(n?n:1,sizeof *e->loaded_phys);e->value=calloc(n?n:1,sizeof *e->value);
    if(!e->dirty||!e->before||!e->kind||!e->constant||!e->loaded_phys||!e->value){failed_fn=f;ok=0;
    break;}
        for(size_t b=0;b<e->fn->block_count;++b)for(size_t k=0;k<e->fn->blocks[b].count;++k)if(e->fn->blocks[b].insts[k].arg_count>e->maxargs)e->maxargs=e->fn->blocks[b].insts[k].arg_count;
        if(e->plan.cell_count>INT_MAX/16||e->fn->local_count>INT_MAX/32||e->maxargs>INT_MAX/16){failed_fn=f;ok=0;
    break;}
        e->locals=BPP_CG_HEADER_BYTES+e->plan.cell_count*16;e->flags=e->locals+e->fn->local_count*16;e->args=(e->flags+e->fn->local_count+15)&~(size_t)15;e->bytes=e->args+e->maxargs*16;
        if(e->bytes>(size_t)INT_MAX-128){failed_fn=f;ok=0;
    break;}
        e->saved_count=0;save_set(e);
        e->alloc=e->bytes+((e->saved_count&1)?80:72);
    }
    if(!ok&&!m->error[0])snprintf(m->error,sizeof m->error,"x86-64 frame planning failed or exceeded 2 GiB in function '%s'",m->functions[failed_fn].name);
    if(ok){
        bpp_asm_source(o,m);
        fputs("# B++ native x86-64, private ABI revision 1\n.text\n.globl main\nmain:\n",o);
        X *e=&all[0];
        if(win)fputs("    .seh_proc main\n    subq $40, %rsp\n    .seh_stackalloc 40\n    .seh_endprologue\n",o);
        else fputs("    .cfi_startproc\n    pushq %rbp\n    .cfi_def_cfa_offset 16\n    .cfi_offset %rbp, -16\n    movq %rsp, %rbp\n    .cfi_def_cfa_register %rbp\n",o);
        arg(e,0,"L_proc_0(%rip)",1);arg(e,1,"L_path(%rip)",1);number(e,2,m->global_count);number(e,3,(uint64_t)print);call(e,"bpp_a_run");
        if(win)fputs("    addq $40, %rsp\n    ret\n    .seh_endproc\n",o);else fputs("    popq %rbp\n    .cfi_def_cfa %rsp, 8\n    ret\n    .cfi_endproc\n",o);
        for(size_t f=0;f<m->function_count;++f){e=&all[f];size_t alloc=e->alloc;fprintf(o,".p2align 4\nL_proc_%zu:\n",f);
            if(win)fprintf(o,"    .seh_proc L_proc_%zu\n",f);else fputs("    .cfi_startproc\n",o);
            for(size_t s=0;s<e->saved_count;++s){const char *name=cache(e,e->saved[s]);fprintf(o,"    pushq %s\n",name);
    if(win)fprintf(o,"    .seh_pushreg %s\n",name);else{
                if(s==0)fputs("    .cfi_def_cfa_offset 16\n    .cfi_offset %rbp, -16\n    movq %rsp, %rbp\n    .cfi_def_cfa_register %rbp\n",o);
                else fprintf(o,"    .cfi_offset %s, -%zu\n",name,16+s*8);
            }}
            if(win){fprintf(o,"    movq $%zu, %%rax\n    call ___chkstk_ms\n    subq %%rax, %%rsp\n    .seh_stackalloc %zu\n    .seh_endprologue\n",alloc,alloc);fprintf(o,"    movq %zu(%%rsp), %%rbp\n",alloc+e->saved_count*8+40);}
            else{fprintf(o,"    movq %%r8, -%zu(%%rbp)\n",e->saved_count*8);size_t remaining=alloc;while(remaining>=4096){fputs("    subq $4096, %rsp\n    movb $0, (%rsp)\n",o);remaining-=4096;}if(remaining)fprintf(o,"    subq $%zu, %%rsp\n",remaining);}
            fputs("    leaq 64(%rsp), %r12\n",o);char layout[80];snprintf(layout,sizeof layout,"L_layout_%zu(%%rip)",f);arg(e,4,layout,1);arg(e,5,"%r12",0);call(e,"bpp_n_enter");
            if(win)fputs("    movq %rbp, 96(%r12)\n",o);else fprintf(o,"    movq -%zu(%%rbp), %%rax\n    movq %%rax, 96(%%r12)\n",e->saved_count*8);
            if(m->instrument_native){char caller[64];if(win)snprintf(caller,sizeof caller,"%zu(%%rsp)",alloc+e->saved_count*8);else snprintf(caller,sizeof caller,"8(%%rbp)");arg(e,0,caller,0);call(e,"bpp_a_trace_enter");}
            for(size_t b=0;b<e->fn->block_count;++b){e->b=b;memset(e->kind,0,e->fn->register_count);memset(e->constant,0,e->fn->register_count);memset(e->dirty,0,e->fn->register_count);memset(e->loaded_phys,0,e->fn->register_count*sizeof *e->loaded_phys);fprintf(o,"L_f%zu_b%zu:\n",f,b);
                for(size_t k=0;k<e->fn->blocks[b].count;++k){e->k=k;const BLIRInst *i=&e->fn->blocks[b].insts[k];
                    if(m->debug_path)bpp_asm_location(o,i);
                    if(m->emit_asm_comments)bpp_asm_instruction_comment(o,e->fn,f,b,k,&e->plan,target,1);
                    if(m->instrument_native&&(i->op==BL_IR_LOAD_GLOBAL||i->op==BL_IR_STORE_GLOBAL||i->op==BL_IR_INIT_GLOBAL)){flush(e,e->dirty,1);arg(e,0,"%r12",0);number(e,1,i->op);number(e,2,i->a);call(e,"bpp_a_access");}
                    if(i->op==BL_IR_RETURN){flush(e,e->dirty,1);number(e,1,i->a);fprintf(o,"    jmp L_cleanup_%zu\n",f);continue;}
                    if(i->op==BL_IR_JUMP){edge_flush(e,i->a);fprintf(o,"    jmp L_f%zu_b%zu\n",f,i->a);memset(e->dirty,0,e->fn->register_count);continue;}
                    if(i->op==BL_IR_BRANCH){fprintf(o,"    cmpl $0, %zu(%%r12)\n",cell(e,i->a));jump(e,"je","false");fprintf(o,"    cmpl $1, %zu(%%r12)\n",cell(e,i->a));jump(e,"jne","true");payload(e,i->a,"%r10");
    fputs("    testq %r10, %r10\n",o);jump(e,"jne","true");def(e,"false");edge_flush(e,i->c);fprintf(o,"    jmp L_f%zu_b%zu\n",f,i->c);def(e,"true");edge_flush(e,i->b);fprintf(o,"    jmp L_f%zu_b%zu\n",f,i->b);memset(e->dirty,0,e->fn->register_count);continue;}
                    memcpy(e->before,e->dirty,e->fn->register_count);int conditional=fast(e,i);
    if(conditional)def(e,"slow");
                    slow(e,i,conditional);def(e,"done");bpp_known_update(e->kind,e->constant,e->value,i);
                }
            }
            fprintf(o,"L_fail_%zu:\n",f);number(e,1,UINT64_MAX);fprintf(o,"L_cleanup_%zu:\n",f);arg(e,0,"%r12",0);arg(e,2,"96(%r12)",0);call(e,"bpp_a_leave");
            if(m->instrument_native)call(e,"bpp_a_trace_exit");
            fprintf(o,"    addq $%zu, %%rsp\n",alloc);for(size_t s=e->saved_count;s-- >0;){const char *name=cache(e,e->saved[s]);fprintf(o,"    popq %s\n",name);
    if(!win)fprintf(o,"    .cfi_restore %s\n",name);}
            if(!win)fputs("    .cfi_def_cfa %rsp, 8\n",o);
    fputs("    ret\n",o);
    fputs(win?"    .seh_endproc\n":"    .cfi_endproc\n",o);
        }
        fputs(win?".section .rdata,\"dr\"\n":".section .rodata\n",o);literal(o,"L_path",m->path);
        for(size_t f=0;f<m->function_count;++f)for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){const BLIRInst *i=&m->functions[f].blocks[b].insts[k];
    if(i->text){char label[96];snprintf(label,sizeof label,"L_text_%zu_%zu_%zu",f,b,k);literal(o,label,i->text);}}
        fputs(".p2align 3\n",o);for(size_t f=0;f<m->function_count;++f){e=&all[f];fprintf(o,"L_layout_%zu:\n    .quad %zu,%zu,%zu,%zu,%zu\n",f,e->fn->param_count,e->fn->register_count,e->fn->local_count,e->maxargs,e->plan.cell_count);for(size_t r=0;r<e->fn->register_count;++r)fprintf(o,"    .quad %zu\n",e->plan.cell[r]);}
        if(!win)fputs(".section .note.GNU-stack,\"\",@progbits\n",o);
    }
    for(size_t f=0;f<m->function_count;++f){bpp_reg_plan_destroy(&all[f].plan);free(all[f].dirty);free(all[f].before);free(all[f].kind);free(all[f].constant);free(all[f].loaded_phys);free(all[f].value);}free(all);return ok&&!ferror(o);
}
