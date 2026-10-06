#include "../include/target.h"
#include "../include/codegen_abi.h"
#include "../include/regalloc.h"
#include "../include/value_facts.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {FILE *out;const BLIRFunction *fn;BPPRegPlan plan;BPPValueFacts facts;BPPValueFact *fact_regs,*fact_locals,*fact_globals;size_t f,b,k,locals,flags,args,bytes,maxargs,alloc,saved_count,emitted_spills,emitted_reloads,emitted_rematerializations;int win,asm_comments,instrument_native;unsigned saved[10];unsigned char *dirty,*before,*kind,*constant;unsigned *loaded_phys;long long *value;unsigned *dirty_phys;} X;
static void sync_known(X *e){for(size_t r=0;r<e->fn->register_count;++r){BPPValueFact f=e->fact_regs[r];e->kind[r]=f.kind==BPP_VALUE_INT?BPP_KIND_INT:f.kind==BPP_VALUE_NULL?BPP_KIND_NULL:f.kind>=BPP_VALUE_STRING?BPP_KIND_OBJECT:BPP_KIND_UNKNOWN;e->constant[r]=f.integer_known;e->value[r]=f.integer;}}
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
static size_t local_slot(X *e,size_t l){return e->plan.local_map?e->plan.local_map[l]:l;}
static size_t layout_flags(const BPPRegPlan *p,const BLIRFunction *fn){size_t flags=0;int identity=1;for(size_t r=0;r<fn->register_count;++r)if(p->cell[r]!=r){identity=0;break;}if(!identity)flags|=BPP_LAYOUT_REGISTER_MAP;identity=1;for(size_t l=0;l<fn->local_count;++l)if(p->local_map[l]!=l){identity=0;break;}if(!identity)flags|=BPP_LAYOUT_LOCAL_MAP;return flags;}
static size_t position(X *e){return bpp_reg_position(&e->plan,e->b,e->k);}
static unsigned physical_at(X *e,size_t r,size_t pos){BPPRegLocation loc=bpp_reg_location_at(&e->plan,r,pos);return loc.kind==BPP_LOC_REGISTER?loc.physical:0;}
static const BLIRInst *frame_site(const BLIRFunction *fn){const BLIRInst *first=NULL,*largest=NULL;size_t maxargs=0;for(size_t b=0;b<fn->block_count;++b)for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *i=&fn->blocks[b].insts[k];if(!first)first=i;if(i->arg_count>maxargs){maxargs=i->arg_count;largest=i;}}return largest?largest:first;}
static void frame_error(BLIRModule *m,const BLIRFunction *fn,const char *backend){const BLIRInst *site=frame_site(fn);snprintf(m->error,sizeof m->error,"%s:%d:%d: %s frame layout overflow in function '%s'",m->path?m->path:"<input>",site?site->line:1,site?site->column:1,backend,fn->name);}
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
    BPPRegLocation location=bpp_reg_location_at(&e->plan,r,position(e));
    if(location.kind==BPP_LOC_REMATERIALIZABLE){if(e->asm_comments)fprintf(e->out,"    # B++ rematerialize vreg=v%zu immediate=%lld reason=safe-integer-expression\n",r,location.immediate);fprintf(e->out,"    movabsq $%lld, %s\n",location.immediate,dst);++e->emitted_rematerializations;return;}
    unsigned p=location.kind==BPP_LOC_REGISTER?location.physical:0;
    if(p){if(e->dirty[r]&&e->dirty_phys[r]&&e->dirty_phys[r]!=p){unsigned old=e->dirty_phys[r];if(e->asm_comments)fprintf(e->out,"    # B++ spill vreg=v%zu physical=%s cell=%zu reason=allocation-segment-transition\n",r,cache(e,old),e->plan.cell[r]);fprintf(e->out,"    movq %s, %zu(%%r12)\n",cache(e,old),cell(e,r)+8);++e->emitted_spills;e->dirty[r]=0;e->dirty_phys[r]=0;e->loaded_phys[r]=0;}if(e->loaded_phys[r]!=p){if(e->asm_comments)fprintf(e->out,"    # B++ reload vreg=v%zu physical=%s cell=%zu reason=%s\n",r,cache(e,p),e->plan.cell[r],e->loaded_phys[r]?"allocation-segment-change":"first-payload-use");fprintf(e->out,"    movq %zu(%%r12), %s\n",cell(e,r)+8,cache(e,p));++e->emitted_reloads;e->loaded_phys[r]=p;}fprintf(e->out,"    movq %s, %s\n",cache(e,p),dst);}
    else fprintf(e->out,"    movq %zu(%%r12), %s\n",cell(e,r)+8,dst);
}
static void flush(X *e,unsigned char *dirty,int clear){for(size_t r=0;r<e->fn->register_count;++r)if(dirty[r]){
    BPPSpillReason reason=bpp_reg_spill_reason_before(&e->plan,e->b,e->k,r);unsigned p=e->dirty_phys[r];if(reason!=BPP_SPILL_NONE&&p){spill_note(e,r,reason,p);fprintf(e->out,"    movq %s, %zu(%%r12)\n",cache(e,p),cell(e,r)+8);++e->emitted_spills;}
    if(clear){dirty[r]=0;e->dirty_phys[r]=0;}
}}
static void edge_flush(X *e,size_t to){for(size_t r=0;r<e->fn->register_count;++r)if(e->dirty[r]){
    unsigned p=e->dirty_phys[r];if(!e->instrument_native&&p&&bpp_reg_keep_dirty_on_edge(&e->plan,e->b,to,r,p))continue;
    BPPSpillReason reason=bpp_reg_spill_reason_on_edge(&e->plan,e->b,to,r);if(reason!=BPP_SPILL_NONE&&p){spill_note(e,r,reason,p);fprintf(e->out,"    movq %s, %zu(%%r12)\n",cache(e,p),cell(e,r)+8);++e->emitted_spills;}
}}
static void cached(X *e,size_t r,const char *source){unsigned p=physical_at(e,r,position(e));
    if(!p)return;
    for(size_t a=0;a<e->fn->register_count;++a)if(a!=r&&(physical_at(e,a,position(e))==p||e->dirty_phys[a]==p)){if(e->dirty[a]&&e->dirty_phys[a]==p){BPPSpillReason reason=bpp_reg_spill_reason_before(&e->plan,e->b,e->k,a);if(reason!=BPP_SPILL_NONE){spill_note(e,a,reason,p);fprintf(e->out,"    movq %s, %zu(%%r12)\n",cache(e,p),cell(e,a)+8);++e->emitted_spills;}e->dirty[a]=0;e->dirty_phys[a]=0;}if(e->loaded_phys[a]==p)e->loaded_phys[a]=0;}
    fprintf(e->out,"    movq %s, %s\n",source,cache(e,p));
    e->loaded_phys[r]=p;
}
static void restore_live_after(X *e){size_t old=position(e),pos=old+1;const BLIRInst *i=&e->fn->blocks[e->b].insts[e->k];size_t dst=i->op==BL_IR_RELEASE?i->a:i->dst;for(size_t r=0;r<e->fn->register_count;++r)if(bpp_reg_live_after(&e->plan,e->b,e->k,r)){unsigned before=physical_at(e,r,old),after=physical_at(e,r,pos);if(after&&(r==dst||after!=before))e->loaded_phys[r]=0;}}
static BPPSpillReason slow_edge_reason(X *e,size_t r){
    const BLIRInst *i=&e->fn->blocks[e->b].insts[e->k];size_t pos=position(e);
    if(i->dst==r)return BPP_SPILL_DESTINATION;
    if(bpp_reg_instruction_visible(&e->plan,e->b,e->k,r))return BPP_SPILL_HELPER_INPUT;
    if(e->plan.alloc_class[r]==BPP_ALLOC_TAGGED_PAIR&&e->plan.begin[r]!=SIZE_MAX&&e->plan.begin[r]<=pos&&e->plan.end[r]>=pos)return BPP_SPILL_ERROR_CLEANUP;
    if(bpp_reg_live_after(&e->plan,e->b,e->k,r))return BPP_SPILL_LIVE_ACROSS;
    return bpp_reg_spill_reason_before(&e->plan,e->b,e->k,r);
}
static void sync_slow_edge_homes(X *e){size_t pos=position(e);for(size_t r=0;r<e->fn->register_count;++r)if(e->dirty[r]){BPPSpillReason reason=slow_edge_reason(e,r);unsigned p=e->dirty_phys[r];if(reason==BPP_SPILL_LIVE_ACROSS&&p&&physical_at(e,r,pos+1)==p)continue;if(reason!=BPP_SPILL_NONE&&p){spill_note(e,r,reason,p);fprintf(e->out,"    movq %s, %zu(%%r12)\n",cache(e,p),cell(e,r)+8);++e->emitted_spills;}e->dirty[r]=0;e->dirty_phys[r]=0;}}
static void guard(X *e,size_t r){if(e->kind[r]==BPP_KIND_INT)return;fprintf(e->out,"    cmpl $1, %zu(%%r12)\n",cell(e,r));jump(e,"jne","slow");}
static void result(X *e,size_t r,int tag){
    fprintf(e->out,"    cmpl $2, %zu(%%r12)\n",cell(e,r));jump(e,"jne","write");
    flush(e,e->dirty,0);
    fputs("    movq %r10, 104(%r12)\n",e->out);
    arg(e,0,"%r12",0);number(e,1,r);call(e,"bpp_a_destination");
    fputs("    movq 104(%r12), %r10\n",e->out);
    restore_live_after(e);
    def(e,"write");fprintf(e->out,"    movl $%d, %zu(%%r12)\n",tag,cell(e,r));
    if(physical_at(e,r,position(e))){cached(e,r,"%r10");const BLIRInst *current=&e->fn->blocks[e->b].insts[e->k];int helper_join=current->op!=BL_IR_INT&&current->op!=BL_IR_NULL;if(tag==1&&helper_join){fprintf(e->out,"    movq %%r10, %zu(%%r12)\n",cell(e,r)+8);e->dirty[r]=0;e->dirty_phys[r]=0;}else{e->dirty[r]=(unsigned char)(tag==1);e->dirty_phys[r]=tag==1?physical_at(e,r,position(e)):0;}}
    else if(tag==1)fprintf(e->out,"    movq %%r10, %zu(%%r12)\n",cell(e,r)+8);
    jump(e,"jmp","done");
}
static void slot(X *e,size_t n,int g,int flag){
    if(g){fprintf(e->out,"    movq %u(%%r12), %%rax\n",flag?40:8);
    if(n)fprintf(e->out,"    addq $%zu, %%rax\n",flag?n:n*16);}
    else {size_t physical=local_slot(e,n);fprintf(e->out,"    leaq %zu(%%r12), %%rax\n",flag?e->flags+physical:e->locals+physical*16);}
}
static int fast(X *e,const BLIRInst *i){FILE *o=e->out;
    switch(i->op){
    case BL_IR_NULL:case BL_IR_INT:fprintf(o,"    movq $%lld, %%r10\n",i->op==BL_IR_INT?i->integer:0LL);result(e,i->dst,i->op==BL_IR_INT);return 1;
    case BL_IR_RELEASE:
        fprintf(o,"    cmpl $2, %zu(%%r12)\n",cell(e,i->a));jump(e,"je","slow");
        fprintf(o,"    movl $0, %zu(%%r12)\n",cell(e,i->a));e->dirty[i->a]=0;e->dirty_phys[i->a]=0;cached(e,i->a,"$0");jump(e,"jmp","done");return 1;
    case BL_IR_LOAD_LOCAL:case BL_IR_LOAD_GLOBAL:{int g=i->op==BL_IR_LOAD_GLOBAL;
        slot(e,i->a,g,1);
    fputs("    cmpb $0, (%rax)\n",o);jump(e,"je","slow");
        slot(e,i->a,g,0);
    fputs("    cmpl $1, (%rax)\n",o);jump(e,"jne","slow");
    fputs("    movq 8(%rax), %r10\n",o);result(e,i->dst,1);return 1;}
    case BL_IR_STORE_LOCAL:case BL_IR_STORE_GLOBAL:case BL_IR_INIT_LOCAL:case BL_IR_INIT_GLOBAL:{
        int g=i->op==BL_IR_STORE_GLOBAL||i->op==BL_IR_INIT_GLOBAL,init=i->op==BL_IR_INIT_GLOBAL||i->op==BL_IR_INIT_LOCAL;
        if(!g&&i->op==BL_IR_INIT_LOCAL&&e->plan.local_slot_shared[local_slot(e,i->a)])return 0;
        guard(e,i->b);
    if(!init){slot(e,i->a,g,1);
    fputs("    cmpb $0, (%rax)\n",o);jump(e,"je","slow");}
        slot(e,i->a,g,0);
    fputs("    cmpl $2, (%rax)\n",o);jump(e,"je","slow");payload(e,i->b,"%r10");
        fputs("    movl $1, (%rax)\n    movq %r10, 8(%rax)\n",o);
        if(init){slot(e,i->a,g,1);
    fputs("    movb $1, (%rax)\n",o);}jump(e,"jmp","done");return 1;}
    case BL_IR_NOT:
        if(e->fact_regs[i->a].truth!=BPP_TRUTH_UNKNOWN){fprintf(o,"    movq $%d, %%r10\n",e->fact_regs[i->a].truth==BPP_TRUTH_TRUE?0:1);if(e->asm_comments)fprintf(o,"    # B++ truthiness-elided vreg=v%zu truth=%s\n",i->a,bpp_truth_name((BPPTruth)e->fact_regs[i->a].truth));result(e,i->dst,1);return 1;}
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
    /* Runtime results already have a canonical home. The native fast arm
     * writes through its result before the shared continuation, so invalidate
     * the destination cache here and let the first later payload use reload. */
    if(i->dst!=BL_IR_NONE){e->loaded_phys[i->dst]=0;e->dirty[i->dst]=0;e->dirty_phys[i->dst]=0;}
    jump(e,"jmp","done");
}
static void literal(FILE *o,const char *name,const char *s){fprintf(o,"%s:\n    .byte ",name);for(const unsigned char *p=(const unsigned char *)s;*p;++p)fprintf(o,"%u,",(unsigned)*p);
    fputs("0\n",o);}
static void access_traffic(FILE *o,const BLIRFunction *fn){
    size_t loads=0,stores=0,branches=0,calls=0;
    for(size_t b=0;b<fn->block_count;++b)for(size_t k=0;k<fn->blocks[b].count;++k){BLIROp op=fn->blocks[b].insts[k].op;
        loads+=op==BL_IR_LOAD_LOCAL||op==BL_IR_LOAD_GLOBAL;
        stores+=op==BL_IR_INIT_LOCAL||op==BL_IR_INIT_GLOBAL||op==BL_IR_STORE_LOCAL||op==BL_IR_STORE_GLOBAL;
        branches+=op==BL_IR_JUMP||op==BL_IR_BRANCH;
        calls+=op==BL_IR_CALL;
    }
    fprintf(o,"    # B++ access-traffic function=%s slot-loads=%zu slot-stores=%zu branches=%zu helper-calls=%zu\n",fn->name,loads,stores,branches,calls);
}
int bpp_emit_x64(BLIRModule *m,FILE *o,int print,int win){
    BPPTarget target=win?BPP_WINDOWS_X64:BPP_LINUX_X64;
    if(!bl_ir_validate_pass(m,"x86-64 backend input")||!bpp_target_validate_ir(m,target,"x86-64"))return 0;
    X *all=calloc(m->function_count,sizeof *all);
    if(!all)return 0;
    int ok=1;size_t failed_fn=0;
    for(size_t f=0;f<m->function_count;++f){X *e=&all[f];e->out=o;e->win=win;e->asm_comments=m->emit_asm_comments;e->instrument_native=m->instrument_native;e->f=f;e->fn=&m->functions[f];
        if(!bpp_reg_plan_create_target(e->fn,&e->plan,target)){snprintf(m->error,sizeof m->error,"x86-64 register allocation failed in function '%s': %s",e->fn->name,bpp_reg_plan_last_error(&e->plan));ok=0;
    break;}
        if(!bpp_reg_plan_validate(e->fn,&e->plan)){snprintf(m->error,sizeof m->error,"x86-64 register-map verifier failed in function '%s'",e->fn->name);ok=0;
    break;}
        if(!bpp_value_facts_create(m,f,&e->facts)){failed_fn=f;ok=0;break;}
        size_t n=e->fn->register_count;e->dirty=calloc(n?n:1,1);e->before=calloc(n?n:1,1);e->kind=calloc(n?n:1,1);e->constant=calloc(n?n:1,1);e->loaded_phys=calloc(n?n:1,sizeof *e->loaded_phys);e->value=calloc(n?n:1,sizeof *e->value);e->dirty_phys=calloc(n?n:1,sizeof *e->dirty_phys);e->fact_regs=calloc(n?n:1,sizeof *e->fact_regs);e->fact_locals=calloc(e->fn->local_count?e->fn->local_count:1,sizeof *e->fact_locals);e->fact_globals=calloc(m->global_count?m->global_count:1,sizeof *e->fact_globals);
    if(!e->dirty||!e->before||!e->kind||!e->constant||!e->loaded_phys||!e->value||!e->dirty_phys||!e->fact_regs||!e->fact_locals||!e->fact_globals){failed_fn=f;ok=0;
    break;}
        for(size_t b=0;b<e->fn->block_count;++b)for(size_t k=0;k<e->fn->blocks[b].count;++k)if(e->fn->blocks[b].insts[k].arg_count>e->maxargs)e->maxargs=e->fn->blocks[b].insts[k].arg_count;
        if(e->plan.cell_count>INT_MAX/16||e->fn->local_count>INT_MAX/32||e->maxargs>INT_MAX/16){failed_fn=f;ok=0;
    break;}
        BPPNativeFrameLayout frame;if(!bpp_native_frame_layout(e->plan.cell_count,e->plan.local_slot_count,e->maxargs,&frame)){failed_fn=f;ok=0;break;}
        e->locals=frame.locals_offset;e->flags=frame.flags_offset;e->args=frame.outgoing_offset;e->bytes=frame.frame_bytes;
        if(e->bytes>(size_t)INT_MAX-128){failed_fn=f;ok=0;
    break;}
        e->saved_count=0;save_set(e);
        BPPNativeStackUsage stack;if(!bpp_native_stack_usage(e->bytes,e->saved_count,0,&stack)){failed_fn=f;ok=0;break;}e->alloc=stack.allocation_bytes;
    }
    if(!ok&&!m->error[0])frame_error(m,&m->functions[failed_fn],"x86-64");
    if(ok){
        bpp_asm_source(o,m);
        fputs("# B++ native x86-64, private ABI revision 2\n.text\n.globl main\nmain:\n",o);
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
            for(size_t b=0;b<e->fn->block_count;++b){e->b=b;bpp_value_facts_begin_block(&e->facts,b,e->fact_regs,e->fact_locals,e->fact_globals);sync_known(e);memset(e->dirty,0,e->fn->register_count);memset(e->dirty_phys,0,e->fn->register_count*sizeof *e->dirty_phys);memset(e->loaded_phys,0,e->fn->register_count*sizeof *e->loaded_phys);if(!m->instrument_native)for(size_t r=0;r<e->fn->register_count;++r){unsigned p=bpp_reg_dirty_entry(&e->plan,b,r);if(p){e->dirty[r]=1;e->dirty_phys[r]=p;e->loaded_phys[r]=p;}}fprintf(o,"L_f%zu_b%zu:\n",f,b);
                for(size_t k=0;k<e->fn->blocks[b].count;++k){e->k=k;const BLIRInst *i=&e->fn->blocks[b].insts[k];
                    if(i->dst!=BL_IR_NONE&&e->plan.begin[i->dst]==SIZE_MAX)continue;
                    if(m->debug_path)bpp_asm_location(o,i);
                    if(m->emit_asm_comments)bpp_asm_instruction_comment(o,e->fn,f,b,k,&e->plan,target,1);
                    if(m->instrument_native&&(i->op==BL_IR_LOAD_GLOBAL||i->op==BL_IR_STORE_GLOBAL||i->op==BL_IR_INIT_GLOBAL)){flush(e,e->dirty,1);arg(e,0,"%r12",0);number(e,1,i->op);number(e,2,i->a);call(e,"bpp_a_access");}
                    if(i->op==BL_IR_RETURN){flush(e,e->dirty,1);number(e,1,i->a);fprintf(o,"    jmp L_cleanup_%zu\n",f);continue;}
                    if(i->op==BL_IR_JUMP){edge_flush(e,i->a);fprintf(o,"    jmp L_f%zu_b%zu\n",f,i->a);memset(e->dirty,0,e->fn->register_count);memset(e->dirty_phys,0,e->fn->register_count*sizeof *e->dirty_phys);continue;}
                    if(i->op==BL_IR_BRANCH){BPPTruth truth=(BPPTruth)e->fact_regs[i->a].truth;if(truth!=BPP_TRUTH_UNKNOWN){size_t to=truth==BPP_TRUTH_TRUE?i->b:i->c;if(e->asm_comments)fprintf(o,"    # B++ truthiness-elided vreg=v%zu truth=%s\n",i->a,bpp_truth_name(truth));edge_flush(e,to);fprintf(o,"    jmp L_f%zu_b%zu\n",f,to);memset(e->dirty,0,e->fn->register_count);memset(e->dirty_phys,0,e->fn->register_count*sizeof *e->dirty_phys);continue;}fprintf(o,"    cmpl $0, %zu(%%r12)\n",cell(e,i->a));jump(e,"je","false");fprintf(o,"    cmpl $1, %zu(%%r12)\n",cell(e,i->a));jump(e,"jne","true");payload(e,i->a,"%r10");
    fputs("    testq %r10, %r10\n",o);jump(e,"jne","true");def(e,"false");edge_flush(e,i->c);fprintf(o,"    jmp L_f%zu_b%zu\n",f,i->c);def(e,"true");edge_flush(e,i->b);fprintf(o,"    jmp L_f%zu_b%zu\n",f,i->b);memset(e->dirty,0,e->fn->register_count);memset(e->dirty_phys,0,e->fn->register_count*sizeof *e->dirty_phys);continue;}
                    /* The emitted slow arm is a possible helper/error edge for
                     * every fast lowering, including checks not classified as
                     * direct clobbers by the IR opcode alone.  Synchronize the
                     * edge-live homes before the fast/slow split so a later
                     * shared continuation can reload lazily on either path. */
                    sync_slow_edge_homes(e);
                    memcpy(e->before,e->dirty,e->fn->register_count);int conditional=fast(e,i);
    if(conditional)def(e,"slow");
                    slow(e,i,conditional);def(e,"done");bpp_value_facts_transfer(&e->facts,i,e->fact_regs,e->fact_locals,e->fact_globals);sync_known(e);
                }
            }
            fprintf(o,"L_fail_%zu:\n",f);number(e,1,UINT64_MAX);fprintf(o,"L_cleanup_%zu:\n",f);arg(e,0,"%r12",0);arg(e,2,"96(%r12)",0);call(e,"bpp_a_leave");
            if(m->instrument_native)call(e,"bpp_a_trace_exit");
            fprintf(o,"    addq $%zu, %%rsp\n",alloc);for(size_t s=e->saved_count;s-- >0;){const char *name=cache(e,e->saved[s]);fprintf(o,"    popq %s\n",name);
    if(!win)fprintf(o,"    .cfi_restore %s\n",name);}
            if(!win)fputs("    .cfi_def_cfa %rsp, 8\n",o);
    fputs("    ret\n",o);
    fputs(win?"    .seh_endproc\n":"    .cfi_endproc\n",o);if(e->asm_comments){fprintf(o,"    # B++ allocation-traffic function=%s spills=%zu reloads=%zu rematerializations=%zu\n",e->fn->name,e->emitted_spills,e->emitted_reloads,e->emitted_rematerializations);access_traffic(o,e->fn);}
        }
        fputs(win?".section .rdata,\"dr\"\n":".section .rodata\n",o);literal(o,"L_path",m->path);
        for(size_t f=0;f<m->function_count;++f)for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){const BLIRInst *i=&m->functions[f].blocks[b].insts[k];
    if(i->text){char label[96];snprintf(label,sizeof label,"L_text_%zu_%zu_%zu",f,b,k);literal(o,label,i->text);}}
        fputs(".p2align 3\n",o);for(size_t f=0;f<m->function_count;++f){e=&all[f];size_t flags=layout_flags(&e->plan,e->fn);fprintf(o,"L_layout_%zu:\n    .quad %zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",f,e->fn->param_count,e->fn->register_count,e->fn->local_count,e->maxargs,e->plan.cell_count,e->plan.local_slot_count,e->plan.root_cell_count,flags);if(flags&BPP_LAYOUT_REGISTER_MAP)for(size_t r=0;r<e->fn->register_count;++r)fprintf(o,"    .quad %zu\n",e->plan.cell[r]);if(flags&BPP_LAYOUT_LOCAL_MAP)for(size_t l=0;l<e->fn->local_count;++l)fprintf(o,"    .quad %zu\n",e->plan.local_map[l]);}
        if(!win)fputs(".section .note.GNU-stack,\"\",@progbits\n",o);
    }
            for(size_t f=0;f<m->function_count;++f){bpp_reg_plan_destroy(&all[f].plan);bpp_value_facts_destroy(&all[f].facts);free(all[f].dirty);free(all[f].before);free(all[f].kind);free(all[f].constant);free(all[f].loaded_phys);free(all[f].value);free(all[f].dirty_phys);free(all[f].fact_regs);free(all[f].fact_locals);free(all[f].fact_globals);}free(all);return ok&&!ferror(o);
}
