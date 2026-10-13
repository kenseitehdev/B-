#include "../include/target.h"
#include "../include/codegen_abi.h"
#include "../include/regalloc.h"
#include "../include/value_facts.h"
#include "../include/isel.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {FILE *out;const BLIRFunction *fn;BPPRegPlan plan;BPPValueFacts facts;BPPValueFact *fact_regs,*fact_locals,*fact_globals;size_t f,b,k,near,locals,flags,args,bytes,maxargs,alloc,saved_count,emitted_spills,emitted_reloads,emitted_rematerializations;int win,asm_comments,instrument_native,native_release,truth_states;unsigned saved[10];unsigned char *dirty,*before,*kind,*constant;unsigned *loaded_phys;long long *value;unsigned *dirty_phys;} X;
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
/* Outgoing arguments are borrowed BLValue pairs.  `slow` flushes the source
 * cell before reaching here, therefore no clone/release helper is needed. */
static void copy_arg(X *e,size_t argument,size_t source){
    fprintf(e->out,"    movl %zu(%%r12), %%r10d\n    movq %zu(%%r12), %%r11\n    movq %u(%%r12), %%rax\n",cell(e,source),cell(e,source)+8,BPP_CG_FRAME_ARGS_OFFSET);
    if(argument)fprintf(e->out,"    addq $%zu, %%rax\n",argument*BPP_CG_VALUE_BYTES);
    fputs("    movl %r10d, (%rax)\n    movq %r11, 8(%rax)\n",e->out);
}
static size_t local_slot(X *e,size_t l){return e->plan.local_map?e->plan.local_map[l]:l;}
static void arg(X *e,unsigned n,const char *source,int address);
static void call(X *e,const char *name);
static void cleanup_cell(X *e,size_t offset,const char *area,size_t index){
    fprintf(e->out,"    cmpl $2, %zu(%%r12)\n    jne L_cleanup_%s_skip_%zu_%zu\n    movq %zu(%%r12), %%r10\n    testq %%r10, %%r10\n    je L_cleanup_%s_skip_%zu_%zu\n",offset,area,e->f,index,offset+8,area,e->f,index);
    arg(e,0,"%r10",0);call(e,e->native_release?"bpp_n_release_object":"bpp_a_release_object");
    fprintf(e->out,"L_cleanup_%s_skip_%zu_%zu:\n    movl $0, %zu(%%r12)\n    movq $0, %zu(%%r12)\n",area,e->f,index,offset,offset+8);
}
static void cleanup_frame(X *e){
    for(size_t root=0;root<e->plan.root_cell_count;++root)
        cleanup_cell(e,BPP_CG_HEADER_BYTES+root*BPP_CG_VALUE_BYTES,"root",root);
    for(size_t local=0;local<e->plan.local_slot_count;++local)
        cleanup_cell(e,e->locals+local*BPP_CG_VALUE_BYTES,"local",local);
}
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
/* Construct the private frame directly. Only parameter cloning remains a
 * helper call because it changes observable object reference counts. */
static void frame_init(X *e){
    const char *vm=e->win?"%rcx":"%rdi",*globals=e->win?"%rdx":"%rsi",*initialized=e->win?"%r8":"%rdx",*incoming_args=e->win?"%r9":"%rcx";
    size_t flags=layout_flags(&e->plan,e->fn),map_offset=64;
    char layout[80];snprintf(layout,sizeof layout,"L_layout_%zu",e->f);
    fprintf(e->out,"    movq %s, %u(%%r12)\n    movq %s, %u(%%r12)\n    movq %s, %u(%%r12)\n",vm,BPP_CG_FRAME_VM_OFFSET,globals,BPP_CG_FRAME_GLOBALS_OFFSET,initialized,BPP_CG_FRAME_GLOBAL_INITIALIZED_OFFSET);
    if(e->win)fprintf(e->out,"    movq %%rbp, %u(%%r12)\n",BPP_CG_FRAME_OUTPUT_OFFSET);else fprintf(e->out,"    movq -%zu(%%rbp), %%rax\n    movq %%rax, %u(%%r12)\n",e->saved_count*8,BPP_CG_FRAME_OUTPUT_OFFSET);
    fprintf(e->out,"    leaq %u(%%r12), %%rax\n    movq $%zu, %%r10\nL_frame_zero_%zu:\n    testq %%r10, %%r10\n    je L_frame_ready_%zu\n    movq $0, (%%rax)\n    addq $8, %%rax\n    decq %%r10\n    jmp L_frame_zero_%zu\nL_frame_ready_%zu:\n",BPP_CG_HEADER_BYTES,(e->bytes-BPP_CG_HEADER_BYTES)/8,e->f,e->f,e->f,e->f);
    fprintf(e->out,"    movq $0, %u(%%r12)\n    movq $0, %u(%%r12)\n    movq $0, 104(%%r12)\n    movq $0, %u(%%r12)\n    leaq %u(%%r12), %%rax\n    movq %%rax, %u(%%r12)\n    leaq %zu(%%r12), %%rax\n    movq %%rax, %u(%%r12)\n    leaq %zu(%%r12), %%rax\n    movq %%rax, %u(%%r12)\n    leaq %zu(%%r12), %%rax\n    movq %%rax, %u(%%r12)\n",BPP_CG_CALL_OUTPUT_OFFSET,BPP_CG_FRAME_REGISTER_MAP_OFFSET,BPP_CG_FRAME_LOCAL_MAP_OFFSET,BPP_CG_HEADER_BYTES,BPP_CG_FRAME_REGISTERS_OFFSET,e->locals,BPP_CG_FRAME_LOCALS_OFFSET,e->flags,BPP_CG_FRAME_LOCAL_INITIALIZED_OFFSET,e->args,BPP_CG_FRAME_ARGS_OFFSET);
    fprintf(e->out,"    movq $%zu, %u(%%r12)\n    movq $%zu, %u(%%r12)\n    movq $%zu, %u(%%r12)\n    movq $%zu, %u(%%r12)\n",e->fn->register_count,BPP_CG_FRAME_REGISTER_COUNT_OFFSET,e->plan.local_slot_count,BPP_CG_FRAME_LOCAL_COUNT_OFFSET,e->plan.cell_count,BPP_CG_FRAME_CELL_COUNT_OFFSET,e->plan.root_cell_count,BPP_CG_ROOT_COUNT_OFFSET);
    if(flags&BPP_LAYOUT_REGISTER_MAP){fprintf(e->out,"    movabsq $%s, %%rax\n",layout);if(map_offset)fprintf(e->out,"    addq $%zu, %%rax\n",map_offset);fprintf(e->out,"    movq %%rax, %u(%%r12)\n",BPP_CG_FRAME_REGISTER_MAP_OFFSET);map_offset+=e->fn->register_count*8;}
    if(flags&BPP_LAYOUT_LOCAL_MAP){fprintf(e->out,"    movabsq $%s, %%rax\n",layout);if(map_offset)fprintf(e->out,"    addq $%zu, %%rax\n",map_offset);fprintf(e->out,"    movq %%rax, %u(%%r12)\n",BPP_CG_FRAME_LOCAL_MAP_OFFSET);}
    for(size_t parameter=0;parameter<e->fn->param_count;++parameter){
        size_t physical=local_slot(e,parameter);
        size_t destination=e->locals+physical*BPP_CG_VALUE_BYTES;
        size_t initialized=e->flags+physical;
        size_t source=parameter*BPP_CG_VALUE_BYTES;
        fprintf(e->out,"    movl %zu(%s), %%r10d\n    movq %zu(%s), %%r11\n    movl %%r10d, %zu(%%r12)\n    movq %%r11, %zu(%%r12)\n    movb $1, %zu(%%r12)\n    cmpl $2, %%r10d\n    jne L_param_no_retain_%zu_%zu\n    testq %%r11, %%r11\n    je L_param_no_retain_%zu_%zu\n    addl $1, %u(%%r11)\nL_param_no_retain_%zu_%zu:\n",source,incoming_args,source+8,incoming_args,destination,destination+8,initialized,e->f,parameter,e->f,parameter,BPP_CG_OBJECT_REF_COUNT_OFFSET,e->f,parameter);
    }
}
static void local(X *e,const char *name){fprintf(e->out,"L_%s_%zu_%zu_%zu",name,e->f,e->b,e->k);}
static void def(X *e,const char *name){local(e,name);
    fputs(":\n",e->out);}
static void jump(X *e,const char *op,const char *name){fprintf(e->out,"    %s ",op);local(e,name);fputc('\n',e->out);}
/* Internal B++ control transfers use the x86-64 large code model: absolute
 * address materialization plus an indirect transfer.  The only rel32 branch
 * is the short skip immediately surrounding this sequence. */
static void long_transfer(X *e,const char *label,int call){
    if(e->asm_comments)fprintf(e->out,"    # B++ large-code-model %s target=%s relocation=x86-64-abs64\n",call?"call":"jump",label);
    fprintf(e->out,"    movabsq $%s, %%r11\n    %s *%%r11\n",label,call?"call":"jmp");
}
static void long_label(X *e,const char *label){long_transfer(e,label,0);}
static void long_block(X *e,size_t block){char label[96];snprintf(label,sizeof label,"L_f%zu_b%zu",e->f,block);long_label(e,label);}
static void long_call_proc(X *e,size_t function){char label[64];snprintf(label,sizeof label,"L_proc_%zu",function);long_transfer(e,label,1);}
static void jump_block(X *e,const char *op,size_t block){
    const char *inverse=!strcmp(op,"je")?"jne":!strcmp(op,"jne")?"je":!strcmp(op,"jg")?"jle":!strcmp(op,"jge")?"jl":!strcmp(op,"jl")?"jge":!strcmp(op,"jle")?"jg":"jne";
    size_t near=e->near++;fprintf(e->out,"    %s L_far_skip_%zu_%zu\n",inverse,e->f,near);long_block(e,block);fprintf(e->out,"L_far_skip_%zu_%zu:\n",e->f,near);
}
static void label_arg(X *e,unsigned n,const char *label){fprintf(e->out,"    movabsq $%s, %%rax\n",label);arg(e,n,"%rax",0);}
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
static void edge_flush_pair(X *e,size_t first,size_t second){edge_flush(e,first);edge_flush(e,second);}
static int branch_fallthrough(X *e,const BLIRInst *i){
    if(e->b+1>=e->fn->block_count||(i->b!=e->b+1&&i->c!=e->b+1))return 0;
    edge_flush_pair(e,i->b,i->c);fprintf(e->out,"    cmpl $0, %zu(%%r12)\n",cell(e,i->a));jump_block(e,"je",i->c);
    fprintf(e->out,"    cmpl $1, %zu(%%r12)\n",cell(e,i->a));jump_block(e,"jne",i->b);payload(e,i->a,"%r10");fputs("    testq %r10, %r10\n",e->out);
    if(i->b==e->b+1)jump_block(e,"jle",i->c);else jump_block(e,"jg",i->b);
    if(e->asm_comments)fputs("    # B++ branch-layout inverted conditional; selected successor falls through\n",e->out);
    return 1;
}
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
/* bpp_a_destination is exactly a move of null into a register cell.  Normal
 * executables perform that directly so only static-library objects retain the
 * compiler-private C bridge.  The scratch slot keeps an in-flight primitive
 * result alive across the native object destructor. */
static void native_destination(X *e,size_t r,int preserve_result){
    size_t id=e->near++;
    if(preserve_result)fputs("    movq %r10, 104(%r12)\n",e->out);
    fprintf(e->out,"    cmpl $2, %zu(%%r12)\n    jne L_native_destination_skip_%zu_%zu_%zu_%zu\n    movq %zu(%%r12), %%r10\n    testq %%r10, %%r10\n    je L_native_destination_skip_%zu_%zu_%zu_%zu\n",cell(e,r),e->f,e->b,e->k,id,cell(e,r)+8,e->f,e->b,e->k,id);
    arg(e,0,"%r10",0);call(e,"bpp_n_release_object");
    fprintf(e->out,"L_native_destination_skip_%zu_%zu_%zu_%zu:\n    movl $0, %zu(%%r12)\n    movq $0, %zu(%%r12)\n",e->f,e->b,e->k,id,cell(e,r),cell(e,r)+8);
    if(preserve_result)fputs("    movq 104(%r12), %r10\n",e->out);
}
/* Direct BLString construction for executable output. Static libraries retain
 * bpp_a_string until their public runtime boundary is native. */
static void native_string(X *e,const BLIRInst *i){
    const char *text=i->text?i->text:"";size_t length=strlen(text),id=e->near++;
    char label[96];snprintf(label,sizeof label,"L_text_%zu_%zu_%zu",e->f,e->b,e->k);
    native_destination(e,i->dst,0);
    number(e,0,1);number(e,1,BPP_CG_STRING_BYTES);call(e,"bpp_n_allocate_zero");
    fputs("    testq %rax, %rax\n",e->out);fprintf(e->out,"    jne L_native_string_object_%zu_%zu_%zu_%zu\n",e->f,e->b,e->k,id);call(e,"bpp_n_allocation_failed");
    fprintf(e->out,"L_native_string_object_%zu_%zu_%zu_%zu:\n    movq %%rax, 72(%%r12)\n",e->f,e->b,e->k,id);
    number(e,0,length+1);number(e,1,1);call(e,"bpp_n_allocate_zero");
    fputs("    testq %rax, %rax\n",e->out);fprintf(e->out,"    jne L_native_string_data_%zu_%zu_%zu_%zu\n",e->f,e->b,e->k,id);call(e,"bpp_n_allocation_failed");
    fprintf(e->out,"L_native_string_data_%zu_%zu_%zu_%zu:\n",e->f,e->b,e->k,id);
    arg(e,0,"%rax",0);label_arg(e,1,label);number(e,2,length+1);call(e,"memcpy");
    fputs("    movq 72(%r12), %r10\n    movl $1, 0(%r10)\n    movl $1, 4(%r10)\n    movq %rax, 8(%r10)\n",e->out);
    fprintf(e->out,"    movq $%zu, 16(%%r10)\n    movl $2, %zu(%%r12)\n    movq %%r10, %zu(%%r12)\n",length,cell(e,i->dst),cell(e,i->dst)+8);
}
static void result(X *e,size_t r,int tag){
    fprintf(e->out,"    cmpl $2, %zu(%%r12)\n",cell(e,r));jump(e,"jne","write");
    flush(e,e->dirty,0);
    if(e->native_release)native_destination(e,r,1);
    else{fputs("    movq %r10, 104(%r12)\n",e->out);arg(e,0,"%r12",0);number(e,1,r);call(e,"bpp_a_destination");fputs("    movq 104(%r12), %r10\n",e->out);}
    restore_live_after(e);
    def(e,"write");fprintf(e->out,"    movl $%d, %zu(%%r12)\n",tag,cell(e,r));
    if(physical_at(e,r,position(e))){cached(e,r,"%r10");const BLIRInst *current=&e->fn->blocks[e->b].insts[e->k];int helper_join=current->op!=BL_IR_INT&&current->op!=BL_IR_NULL;if(tag==1&&helper_join){fprintf(e->out,"    movq %%r10, %zu(%%r12)\n",cell(e,r)+8);e->dirty[r]=0;e->dirty_phys[r]=0;}else{e->dirty[r]=(unsigned char)(tag==1);e->dirty_phys[r]=tag==1?physical_at(e,r,position(e)):0;}}
    else if(tag==1)fprintf(e->out,"    movq %%r10, %zu(%%r12)\n",cell(e,r)+8);
    jump(e,"jmp","done");
}
static void add_displacement(X *e,size_t displacement){
    BPPISelAddressRequest request={e->win?BPP_WINDOWS_X64:BPP_LINUX_X64,displacement,1};BPPISelAddressSelection selection;
    if(!bpp_isel_select_address(&request,&selection)||selection.form==BPP_ISEL_ADDRESS_MATERIALIZE){
        if(displacement){fprintf(e->out,"    movabsq $%zu, %%r11\n    addq %%r11, %%rax\n",displacement);}return;
    }
    if(selection.form==BPP_ISEL_ADDRESS_X64_DISP32)fprintf(e->out,"    addq $%zu, %%rax\n",displacement);
}
static void frame_address(X *e,size_t displacement){
    BPPISelAddressRequest request={e->win?BPP_WINDOWS_X64:BPP_LINUX_X64,displacement,1};BPPISelAddressSelection selection;
    if(bpp_isel_select_address(&request,&selection)&&selection.form==BPP_ISEL_ADDRESS_X64_DISP32){fprintf(e->out,"    leaq %zu(%%r12), %%rax\n",displacement);return;}
    fputs("    movq %r12, %rax\n",e->out);add_displacement(e,displacement);
}
static void slot(X *e,size_t n,int g,int flag){
    if(g){fprintf(e->out,"    movq %u(%%r12), %%rax\n",flag?40:8);add_displacement(e,flag?n:n*16);}
    else {size_t physical=local_slot(e,n);frame_address(e,flag?e->flags+physical:e->locals+physical*16);}
}
static int fast(X *e,const BLIRInst *i){FILE *o=e->out;
    if(e->truth_states&&(i->op==BL_IR_MALLOC||i->op==BL_IR_REMALLOC||i->op==BL_IR_MFREE_LOCAL||i->op==BL_IR_MFREE_GLOBAL||i->op==BL_IR_BYTE_LOAD||i->op==BL_IR_BYTE_STORE||i->op==BL_IR_PRINT_BYTE||i->op==BL_IR_PRINT_RANGE))return 0;
    switch(i->op){
    case BL_IR_NULL:case BL_IR_INT:fprintf(o,"    movq $%lld, %%r10\n",i->op==BL_IR_INT?i->integer:0LL);result(e,i->dst,i->op==BL_IR_INT);return 1;
    case BL_IR_MALLOC:
        guard(e,i->a);payload(e,i->a,"%r10");arg(e,0,"%r10",0);call(e,"malloc");
        fputs("    movq %rax, %r10\n",o);result(e,i->dst,1);return 1;
    case BL_IR_REMALLOC:
        guard(e,i->a);guard(e,i->b);payload(e,i->a,"%r10");payload(e,i->b,"%r11");
        arg(e,0,"%r10",0);arg(e,1,"%r11",0);call(e,"realloc");fputs("    movq %rax, %r10\n",o);result(e,i->dst,1);return 1;
    case BL_IR_BYTE_LOAD:
        guard(e,i->a);guard(e,i->b);payload(e,i->a,"%r10");payload(e,i->b,"%r11");
        fputs("    movzbq (%r10,%r11), %r10\n",o);result(e,i->dst,1);return 1;
    case BL_IR_BYTE_STORE:
        guard(e,i->a);guard(e,i->b);guard(e,i->c);payload(e,i->a,"%r10");payload(e,i->b,"%r11");payload(e,i->c,"%rax");
        fputs("    movb %al, (%r10,%r11)\n",o);jump(e,"jmp","done");return 1;
    case BL_IR_MFREE_LOCAL: case BL_IR_MFREE_GLOBAL: {
        int g=i->op==BL_IR_MFREE_GLOBAL;slot(e,i->a,g,0);fputs("    movq 8(%rax), %r10\n",o);arg(e,0,"%r10",0);call(e,"free");jump(e,"jmp","done");return 1; }
    case BL_IR_PRINT_BYTE:
        guard(e,i->a);payload(e,i->a,"%r10");fputs("    movb %r10b, 104(%r12)\n",o);
        number(e,0,1);fprintf(o,"    leaq 104(%%r12), %%rax\n");arg(e,1,"%rax",0);number(e,2,1);call(e,e->win?"_write":"write");jump(e,"jmp","done");return 1;
    case BL_IR_PRINT_RANGE:
        guard(e,i->a);guard(e,i->b);payload(e,i->a,"%r10");payload(e,i->b,"%r11");
        number(e,0,1);arg(e,1,"%r10",0);arg(e,2,"%r11",0);call(e,e->win?"_write":"write");jump(e,"jmp","done");return 1;
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
    case BL_IR_BIT_NOT:
        guard(e,i->a);payload(e,i->a,"%r10");fputs("    notq %r10\n",o);result(e,i->dst,1);return 1;
    case BL_IR_BIT_AND:case BL_IR_BIT_OR:case BL_IR_BIT_XOR:
        guard(e,i->a);guard(e,i->b);payload(e,i->a,"%r10");payload(e,i->b,"%r11");
        fprintf(o,"    %s %%r11, %%r10\n",i->op==BL_IR_BIT_AND?"andq":i->op==BL_IR_BIT_OR?"orq":"xorq");result(e,i->dst,1);return 1;
    case BL_IR_NOT:return 0;
    case BL_IR_NEG:guard(e,i->a);payload(e,i->a,"%r10");
    fputs("    negq %r10\n",o);jump(e,"jo","slow");result(e,i->dst,1);return 1;
    case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:
    case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:
        {BPPISelRequest request={e->win?BPP_WINDOWS_X64:BPP_LINUX_X64,i->op,e->constant[i->b],e->value[i->b]};BPPInstructionSelection selection;
        if(!bpp_isel_select(&request,&selection)||selection.scratch_gprs>4)return 0;
        if(e->asm_comments)fprintf(o,"    # B++ isel form=%s cost=%u scratch-gprs=%u flags=%s\n",bpp_isel_form_name(selection.form),selection.cost,selection.scratch_gprs,selection.defines_flags?"defined-consumed-locally":"none");
        guard(e,i->a);guard(e,i->b);payload(e,i->a,"%r10");
        if(selection.form==BPP_ISEL_X64_IMM32){
            if(i->op==BL_IR_ADD||i->op==BL_IR_SUB||i->op==BL_IR_MUL){
                fprintf(o,"    %s $%lld, %%r10%s\n",i->op==BL_IR_ADD?"addq":i->op==BL_IR_SUB?"subq":"imulq",selection.immediate,i->op==BL_IR_MUL?", %r10":"");jump(e,"jo","slow");result(e,i->dst,1);return 1;
            }
            {const char *cc=i->op==BL_IR_EQ?"e":i->op==BL_IR_NE?"ne":i->op==BL_IR_LT?"l":i->op==BL_IR_LE?"le":i->op==BL_IR_GT?"g":"ge";
            fprintf(o,"    cmpq $%lld, %%r10\n    movl $0, %%r10d\n    set%s %%r10b\n    leaq -1(%%r10,%%r10), %%r10\n",selection.immediate,cc);result(e,i->dst,1);return 1;}
        }
        payload(e,i->b,"%r11");
        if(i->op==BL_IR_ADD||i->op==BL_IR_SUB||i->op==BL_IR_MUL){fprintf(o,"    %s %%r11, %%r10\n",i->op==BL_IR_ADD?"addq":i->op==BL_IR_SUB?"subq":"imulq");jump(e,"jo","slow");}
        else if(i->op==BL_IR_DIV||i->op==BL_IR_MOD){
            fputs("    testq %r11, %r11\n",o);jump(e,"je","slow");
    fputs("    cmpq $-1, %r11\n",o);jump(e,"jne","divide");
            fputs("    movabsq $0x8000000000000000, %rax\n    cmpq %rax, %r10\n",o);jump(e,"je","slow");def(e,"divide");
            fputs("    movq %r10, %rax\n    cqto\n    idivq %r11\n",o);fprintf(o,"    movq %s, %%r10\n",i->op==BL_IR_MOD?"%rdx":"%rax");
        }else{const char *cc=i->op==BL_IR_EQ?"e":i->op==BL_IR_NE?"ne":i->op==BL_IR_LT?"l":i->op==BL_IR_LE?"le":i->op==BL_IR_GT?"g":"ge";
            fprintf(o,"    cmpq %%r11, %%r10\n    movl $0, %%r10d\n    set%s %%r10b\n    leaq -1(%%r10,%%r10), %%r10\n",cc);}
        result(e,i->dst,1);return 1;}
    default:return 0;
    }
}
static void textref(X *e,unsigned argn){char label[96];snprintf(label,sizeof label,"L_text_%zu_%zu_%zu",e->f,e->b,e->k);label_arg(e,argn,label);}
static void slow(X *e,const BLIRInst *i,int conditional){flush(e,conditional?e->before:e->dirty,!conditional);arg(e,0,"%r12",0);
    if(i->op==BL_IR_STRING){if(e->native_release)native_string(e,i);else{number(e,1,i->dst);textref(e,2);call(e,"bpp_a_string");}}
    else if(i->op==BL_IR_CALL){
        for(size_t j=0;j<i->arg_count;++j)copy_arg(e,j,i->args[j]);
        arg(e,0,"%r12",0);
        if(i->a==BL_IR_NONE&&i->c==BL_IR_NONE){number(e,1,i->dst);textref(e,2);number(e,3,i->arg_count);number(e,4,((uint64_t)(i->source_file==BL_IR_NONE?0:i->source_file+1)<<32)|(uint32_t)(i->source_line>0?i->source_line:i->line));number(e,5,(uint64_t)(i->source_column>0?i->source_column:i->column));call(e,"bpp_a_builtin");}
        else{if(e->truth_states){arg(e,0,"%r12",0);call(e,"bpp_a_call_begin");arg(e,0,"%r12",0);}if(e->native_release){native_destination(e,i->dst,0);fprintf(e->out,"    leaq %zu(%%r12), %%rax\n    movq %%rax, 72(%%r12)\n",cell(e,i->dst));}else{number(e,1,i->dst);call(e,"bpp_a_destination");fputs("    movq %rax, 72(%r12)\n",e->out);}fprintf(e->out,"    movq %u(%%r12), %%rax\n",BPP_CG_FRAME_ARGS_OFFSET);
            arg(e,3,"%rax",0);arg(e,0,"0(%r12)",0);arg(e,1,"8(%r12)",0);arg(e,2,"40(%r12)",0);arg(e,4,"72(%r12)",0);if(i->c!=BL_IR_NONE)long_transfer(e,i->text,1);else long_call_proc(e,i->a);if(e->truth_states){arg(e,0,"%r12",0);number(e,1,i->dst);call(e,"bpp_a_call_end");}}
    }else{number(e,1,i->op);number(e,2,i->dst);number(e,3,i->a);number(e,4,i->b);number(e,5,i->c);number(e,6,((uint64_t)(i->source_file==BL_IR_NONE?0:i->source_file+1)<<32)|(uint32_t)(i->source_line>0?i->source_line:i->line));number(e,7,(uint64_t)(i->source_column>0?i->source_column:i->column));call(e,"bpp_a_op");}
    fprintf(e->out,"    movq %u(%%r12), %%rax\n    cmpq $0, %u(%%rax)\n",BPP_CG_FRAME_VM_OFFSET,BPP_CG_RUNTIME_ERROR_OFFSET);jump(e,"je","post_call");{char failed[64];snprintf(failed,sizeof failed,"L_fail_%zu",e->f);long_label(e,failed);}def(e,"post_call");
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
/* Private platform-runtime leaves.  These are emitted assembly, not B++ C
 * runtime calls: they tail-transfer only to the host's allocator/process
 * boundary.  Keeping them in the generated image makes the eventual native
 * allocator replacement a local ABI change rather than an emitter rewrite. */
static void emit_native_platform_leaves(FILE *out,int win){
    const char *calloc_name=win?"calloc":"calloc@PLT";
    const char *free_name=win?"free":"free@PLT";
    const char *exit_name=win?"exit":"exit@PLT";
    fputs(".p2align 4\n.globl bpp_n_allocate_zero\nbpp_n_allocate_zero:\n",out);
    fprintf(out,"    jmp %s\n",calloc_name);
    fputs(".p2align 4\n.globl bpp_n_free_memory\nbpp_n_free_memory:\n",out);
    fprintf(out,"    jmp %s\n",free_name);
    fputs(".p2align 4\n.globl bpp_n_allocation_failed\nbpp_n_allocation_failed:\n    movl $1, %edi\n",out);
    if(win)fputs("    movl $1, %ecx\n",out);
    fprintf(out,"    jmp %s\n",exit_name);
    /* Object layout is private but fixed by codegen_abi.h/value.h: kind@0,
     * refcount@4, data/items@8, size@16, owned@24. This leaf mirrors
     * bl_value_release/bl_object_free without calling value.c. */
    fputs(".p2align 4\n.globl bpp_n_release_object\nbpp_n_release_object:\n",out);
    if(win)fputs("    testq %rcx, %rcx\n    je L_native_release_ret\n    pushq %rbx\n    pushq %r12\n    pushq %r13\n    subq $32, %rsp\n    movq %rcx, %rbx\n",out);
    else fputs("    testq %rdi, %rdi\n    je L_native_release_ret\n    pushq %rbx\n    pushq %r12\n    pushq %r13\n    movq %rdi, %rbx\n",out);
    fputs("    subl $1, 4(%rbx)\n    jne L_native_release_done\n    movl (%rbx), %eax\n    cmpl $1, %eax\n    je L_native_release_string\n    cmpl $2, %eax\n    je L_native_release_buffer\n    cmpl $4, %eax\n    je L_native_release_object\n    cmpl $3, %eax\n    je L_native_release_array\n    jmp L_native_release_done\nL_native_release_string:\n    movq 8(%rbx), %rax\n    testq %rax, %rax\n    je L_native_release_object\n",out);
    if(win)fputs("    movq %rax, %rcx\n",out);else fputs("    movq %rax, %rdi\n",out);
    fputs("    call bpp_n_free_memory\nL_native_release_object:\n",out);
    if(win)fputs("    movq %rbx, %rcx\n",out);else fputs("    movq %rbx, %rdi\n",out);
    fputs("    call bpp_n_free_memory\n    jmp L_native_release_done\nL_native_release_buffer:\n    cmpl $0, 24(%rbx)\n    je L_native_release_object\n    movq 8(%rbx), %rax\n    testq %rax, %rax\n    je L_native_release_object\n",out);
    if(win)fputs("    movq %rax, %rcx\n",out);else fputs("    movq %rax, %rdi\n",out);
    fputs("    call bpp_n_free_memory\n    jmp L_native_release_object\nL_native_release_array:\n    cmpl $0, 24(%rbx)\n    je L_native_release_object\n    movq 8(%rbx), %r12\n    testq %r12, %r12\n    je L_native_release_object\n    movq 16(%rbx), %r13\n    shlq $4, %r13\n    addq %r12, %r13\nL_native_release_array_loop:\n    cmpq %r13, %r12\n    je L_native_release_array_items\n    cmpl $2, (%r12)\n    jne L_native_release_array_next\n    movq 8(%r12), %rax\n    testq %rax, %rax\n    je L_native_release_array_next\n",out);
    if(win)fputs("    movq %rax, %rcx\n",out);else fputs("    movq %rax, %rdi\n",out);
    fputs("    call bpp_n_release_object\nL_native_release_array_next:\n    addq $16, %r12\n    jmp L_native_release_array_loop\nL_native_release_array_items:\n    movq 8(%rbx), %rax\n",out);
    if(win)fputs("    movq %rax, %rcx\n",out);else fputs("    movq %rax, %rdi\n",out);
    fputs("    call bpp_n_free_memory\n    jmp L_native_release_object\nL_native_release_done:\n",out);
    if(win)fputs("    addq $32, %rsp\n",out);
    fputs("    popq %r13\n    popq %r12\n    popq %rbx\nL_native_release_ret:\n    ret\n",out);
    fputs(".p2align 4\n.globl bpp_n_print_result\nbpp_n_print_result:\n",out);
    if(win)fputs("    testq %rcx, %rcx\n    je L_native_print_ret\n    movq %rcx, %rax\n    subq $40, %rsp\n",out);
    else fputs("    testq %rdi, %rdi\n    je L_native_print_ret\n    movq %rdi, %rax\n    subq $8, %rsp\n",out);
    fputs("    movl (%rax), %r11d\n    testl %r11d, %r11d\n    je L_native_print_done\n    cmpl $1, %r11d\n    je L_native_print_int\n    cmpl $2, %r11d\n    jne L_native_print_done\n    movq 8(%rax), %rax\n    testq %rax, %rax\n    je L_native_print_done\n    movl (%rax), %r11d\n    cmpl $1, %r11d\n    je L_native_print_string\n    cmpl $2, %r11d\n    je L_native_print_buffer\n    cmpl $3, %r11d\n    je L_native_print_array\n    jmp L_native_print_done\nL_native_print_int:\n",out);
    if(win)fputs("    movq 8(%rax), %rdx\n    leaq L_native_fmt_int(%rip), %rcx\n    call printf\n",out);
    else fputs("    movq 8(%rax), %rsi\n    leaq L_native_fmt_int(%rip), %rdi\n    xorl %eax, %eax\n    call printf@PLT\n",out);
    fputs("    jmp L_native_print_done\nL_native_print_string:\n",out);
    if(win)fputs("    movq 8(%rax), %rcx\n    call puts\n",out);else fputs("    movq 8(%rax), %rdi\n    call puts@PLT\n",out);
    fputs("    jmp L_native_print_done\nL_native_print_buffer:\n",out);
    if(win)fputs("    movq 16(%rax), %rdx\n    movl 24(%rax), %r8d\n    leaq L_native_fmt_buffer(%rip), %rcx\n    call printf\n",out);
    else fputs("    movq 16(%rax), %rsi\n    movl 24(%rax), %edx\n    leaq L_native_fmt_buffer(%rip), %rdi\n    xorl %eax, %eax\n    call printf@PLT\n",out);
    fputs("    jmp L_native_print_done\nL_native_print_array:\n",out);
    if(win)fputs("    movq 16(%rax), %rdx\n    movl 24(%rax), %r8d\n    leaq L_native_fmt_array(%rip), %rcx\n    call printf\n",out);
    else fputs("    movq 16(%rax), %rsi\n    movl 24(%rax), %edx\n    leaq L_native_fmt_array(%rip), %rdi\n    xorl %eax, %eax\n    call printf@PLT\n",out);
    fputs("L_native_print_done:\n",out);
    if(win)fputs("    addq $40, %rsp\n",out);else fputs("    addq $8, %rsp\n",out);
    fputs("L_native_print_ret:\n    ret\n",out);
    fputs(".p2align 4\n.globl bpp_n_report_error\nbpp_n_report_error:\n",out);
    if(win)fputs("    testq %rcx, %rcx\n    je L_native_report_ret\n    pushq %rbx\n    subq $32, %rsp\n    movq %rcx, %rbx\n    call strlen\n    movq %rax, %r8\n    movq %rbx, %rdx\n    movl $2, %ecx\n    call write\n    leaq L_native_newline(%rip), %rdx\n    movl $1, %r8d\n    movl $2, %ecx\n    call write\n    addq $32, %rsp\n    popq %rbx\n",out);
    else fputs("    testq %rdi, %rdi\n    je L_native_report_ret\n    pushq %rbx\n    movq %rdi, %rbx\n    call strlen@PLT\n    movq %rax, %rdx\n    movq %rbx, %rsi\n    movl $2, %edi\n    call write@PLT\n    leaq L_native_newline(%rip), %rsi\n    movl $1, %edx\n    movl $2, %edi\n    call write@PLT\n    popq %rbx\n",out);
    fputs("L_native_report_ret:\n    ret\n",out);
}
/* The executable launcher is native. It owns POD process state and calls
 * narrow ABI-decorated leaves only for diagnostics and objects. */
static void main_state_address(X *e,size_t state,const char *reg){fprintf(e->out,"    leaq %zu(%%rsp), %s\n",state,reg);}
static void main_release_value(X *e,size_t state,int global,size_t index){
    const char *area=global?"global":"result";
    if(global)fprintf(e->out,"    movq %zu(%%rsp), %%rax\n",state+16);
    else main_state_address(e,state+32,"%rax");
    if(global&&index)fprintf(e->out,"    addq $%zu, %%rax\n",index*BPP_CG_VALUE_BYTES);
    fprintf(e->out,"    cmpl $2, (%%rax)\n    jne L_main_%s_skip_%zu\n    movq 8(%%rax), %%r10\n    testq %%r10, %%r10\n    je L_main_%s_skip_%zu\n",area,index,area,index);
    arg(e,0,"%r10",0);call(e,e->truth_states?"bpp_a_release_object":"bpp_n_release_object");
    if(global)fprintf(e->out,"    movq %zu(%%rsp), %%rax\n",state+16);
    else main_state_address(e,state+32,"%rax");
    if(global&&index)fprintf(e->out,"    addq $%zu, %%rax\n",index*BPP_CG_VALUE_BYTES);
    fprintf(e->out,"L_main_%s_skip_%zu:\n    movl $0, (%%rax)\n    movq $0, 8(%%rax)\n",area,index);
}
static void emit_native_main(X *e,size_t global_count,int print){
    size_t state=e->win?48:0,bytes=e->win?104:64,globals=global_count?global_count:1;
    if(e->win)fputs(".globl main\nmain:\n    .seh_proc main\n    subq $104, %rsp\n    .seh_stackalloc 104\n    .seh_endprologue\n",e->out);
    else fputs(".globl main\nmain:\n    .cfi_startproc\n    pushq %rbp\n    .cfi_def_cfa_offset 16\n    .cfi_offset %rbp, -16\n    movq %rsp, %rbp\n    .cfi_def_cfa_register %rbp\n    subq $64, %rsp\n",e->out);
    label_arg(e,0,"L_path");main_state_address(e,state,"%rax");
    fprintf(e->out,"    movq %s, 0(%%rax)\n    movq $0, 8(%%rax)\n",e->win?"%rcx":"%rdi");
    number(e,0,globals);number(e,1,BPP_CG_VALUE_BYTES);call(e,"bpp_n_allocate_zero");
    fputs("    testq %rax, %rax\n    jne L_main_globals_ready\n",e->out);call(e,"bpp_n_allocation_failed");
    fputs("L_main_globals_ready:\n",e->out);fprintf(e->out,"    movq %%rax, %zu(%%rsp)\n",state+16);
    number(e,0,globals);number(e,1,1);call(e,"bpp_n_allocate_zero");
    fputs("    testq %rax, %rax\n    jne L_main_flags_ready\n",e->out);call(e,"bpp_n_allocation_failed");
    fputs("L_main_flags_ready:\n",e->out);fprintf(e->out,"    movq %%rax, %zu(%%rsp)\n    movl $0, %zu(%%rsp)\n",state+24,state+48);
    if(e->win){main_state_address(e,state+32,"%rax");fputs("    movq %rax, 32(%rsp)\n",e->out);main_state_address(e,state,"%rcx");fprintf(e->out,"    movq %zu(%%rsp), %%rdx\n    movq %zu(%%rsp), %%r8\n    xorq %%r9, %%r9\n",state+16,state+24);}
    else{main_state_address(e,state,"%rdi");fprintf(e->out,"    movq %zu(%%rsp), %%rsi\n    movq %zu(%%rsp), %%rdx\n    xorq %%rcx, %%rcx\n",state+16,state+24);main_state_address(e,state+32,"%r8");}
    long_call_proc(e,0);
    fprintf(e->out,"    movq %zu(%%rsp), %%rax\n    testq %%rax, %%rax\n    jne L_main_error\n",state+8);
    if(print){main_state_address(e,state+32,e->win?"%rcx":"%rdi");call(e,e->truth_states?"bpp_a_print_result":"bpp_n_print_result");}
    fputs("    jmp L_main_cleanup\nL_main_error:\n",e->out);fprintf(e->out,"    movq %zu(%%rsp), %%rax\n",state+8);arg(e,0,"%rax",0);call(e,"bpp_n_report_error");fprintf(e->out,"    movl $1, %zu(%%rsp)\n",state+48);
    fputs("L_main_cleanup:\n",e->out);main_release_value(e,state,0,0);
    for(size_t global=0;global<global_count;++global)main_release_value(e,state,1,global);
    fprintf(e->out,"    movq %zu(%%rsp), %%rax\n",state+16);arg(e,0,"%rax",0);call(e,"bpp_n_free_memory");
    fprintf(e->out,"    movq %zu(%%rsp), %%rax\n",state+24);arg(e,0,"%rax",0);call(e,"bpp_n_free_memory");
    fprintf(e->out,"    movq %zu(%%rsp), %%rax\n",state+8);arg(e,0,"%rax",0);call(e,"bpp_n_free_memory");
    fprintf(e->out,"    movl %zu(%%rsp), %%eax\n",state+48);
    if(e->win)fprintf(e->out,"    addq $%zu, %%rsp\n    ret\n    .seh_endproc\n",bytes);
    else fputs("    addq $64, %rsp\n    popq %rbp\n    .cfi_def_cfa %rsp, 8\n    ret\n    .cfi_endproc\n",e->out);
}
int bpp_emit_x64(BLIRModule *m,FILE *o,int print,int win){
    BPPTarget target=win?BPP_WINDOWS_X64:BPP_LINUX_X64;
    if(!bl_ir_validate_pass(m,"x86-64 backend input")||!bpp_target_validate_ir(m,target,"x86-64"))return 0;
    X *all=calloc(m->function_count,sizeof *all);
    if(!all)return 0;
    int ok=1;size_t failed_fn=0;
    for(size_t f=0;f<m->function_count;++f){X *e=&all[f];e->out=o;e->win=win;e->asm_comments=m->emit_asm_comments;e->instrument_native=m->instrument_native;e->native_release=!m->emit_library&&!m->truth_states&&!m->static_library_count;e->truth_states=m->truth_states||m->emit_library||m->static_library_count;e->f=f;e->fn=&m->functions[f];
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
        if(win&&m->debug_path)bpp_asm_codeview_source(o,m);
        fputs("# B++ native x86-64, static ABI revision 2 / private frame revision 2\n.text\n",o);
        if(!win&&m->debug_path)fputs("L_bpp_debug_start:\n",o);
        X *e=&all[0];
        if(!m->emit_library){emit_native_platform_leaves(o,win);emit_native_main(e,m->global_count,print);}
        for(size_t f=0;f<m->function_count;++f){e=&all[f];size_t alloc=e->alloc;
            if(m->emit_library&&f)fprintf(o,".globl bpp_v2_%s\nbpp_v2_%s:\n",e->fn->name,e->fn->name);
            if(!m->emit_library&&f)fprintf(o,".p2align 4\n.globl bpp_source_%s\nbpp_source_%s:\n",e->fn->name,e->fn->name);
            fprintf(o,".p2align 4\nL_proc_%zu:\n",f);
            if(win&&m->debug_path)fprintf(o,".cv_func_id %zu\n",f);
            if(win)fprintf(o,"    .seh_proc L_proc_%zu\n",f);else fputs("    .cfi_startproc\n",o);
            for(size_t s=0;s<e->saved_count;++s){const char *name=cache(e,e->saved[s]);fprintf(o,"    pushq %s\n",name);
    if(win)fprintf(o,"    .seh_pushreg %s\n",name);else{
                if(s==0)fputs("    .cfi_def_cfa_offset 16\n    .cfi_offset %rbp, -16\n    movq %rsp, %rbp\n    .cfi_def_cfa_register %rbp\n",o);
                else fprintf(o,"    .cfi_offset %s, -%zu\n",name,16+s*8);
            }}
            if(win){fprintf(o,"    movq $%zu, %%rax\n    call ___chkstk_ms\n    movq $%zu, %%rax\n    subq %%rax, %%rsp\n    .seh_stackalloc %zu\n    leaq 64(%%rsp), %%r12\n    .seh_setframe %%r12, 64\n    .seh_endprologue\n",alloc+4096,alloc,alloc);fprintf(o,"    movq %zu(%%rsp), %%rbp\n",alloc+e->saved_count*8+40);}
            else{fprintf(o,"    movq %%r8, -%zu(%%rbp)\n",e->saved_count*8);size_t remaining=alloc;while(remaining>=4096){fputs("    subq $4096, %rsp\n    movb $0, (%rsp)\n",o);remaining-=4096;}if(remaining)fprintf(o,"    subq $%zu, %%rsp\n",remaining);}
            fputs("    leaq 64(%rsp), %r12\n",o);frame_init(e);
            if(m->benchmark_metrics){arg(e,0,"%rsp",0);number(e,1,alloc+e->saved_count*8);call(e,"bpp_bench_enter");}
            if(m->truth_states){arg(e,0,"%r12",0);number(e,1,f==0?1:0);call(e,"bpp_a_truth_enable");}
            if(m->instrument_native){char caller[64];if(win)snprintf(caller,sizeof caller,"%zu(%%rsp)",alloc+e->saved_count*8);else snprintf(caller,sizeof caller,"8(%%rbp)");arg(e,0,caller,0);call(e,"bpp_a_trace_enter");}
            for(size_t b=0;b<e->fn->block_count;++b){e->b=b;bpp_value_facts_begin_block(&e->facts,b,e->fact_regs,e->fact_locals,e->fact_globals);sync_known(e);memset(e->dirty,0,e->fn->register_count);memset(e->dirty_phys,0,e->fn->register_count*sizeof *e->dirty_phys);memset(e->loaded_phys,0,e->fn->register_count*sizeof *e->loaded_phys);if(!m->instrument_native)for(size_t r=0;r<e->fn->register_count;++r){unsigned p=bpp_reg_dirty_entry(&e->plan,b,r);if(p){e->dirty[r]=1;e->dirty_phys[r]=p;e->loaded_phys[r]=p;}}fprintf(o,"L_f%zu_b%zu:\n",f,b);
                for(size_t k=0;k<e->fn->blocks[b].count;++k){e->k=k;const BLIRInst *i=&e->fn->blocks[b].insts[k];if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_begin:\n",f,b,k);
                    if(i->dst!=BL_IR_NONE&&e->plan.begin[i->dst]==SIZE_MAX){if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}
                    if(m->debug_path){bpp_asm_location(o,m,i);if(win)bpp_asm_codeview_location(o,m,i,f);}
                    if(m->emit_asm_comments)bpp_asm_instruction_comment(o,e->fn,f,b,k,&e->plan,target,1);
                    if(m->instrument_native&&(i->op==BL_IR_LOAD_GLOBAL||i->op==BL_IR_STORE_GLOBAL||i->op==BL_IR_INIT_GLOBAL)){flush(e,e->dirty,1);arg(e,0,"%r12",0);number(e,1,i->op);number(e,2,i->a);call(e,"bpp_a_access");}
                    if(i->op==BL_IR_RETURN){flush(e,e->dirty,1);fprintf(o,"    leaq %zu(%%r12), %%rax\n    movq %%rax, %u(%%r12)\n",cell(e,i->a),BPP_CG_CALL_OUTPUT_OFFSET);char cleanup[64];snprintf(cleanup,sizeof cleanup,"L_cleanup_%zu",f);long_label(e,cleanup);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}
                    if(i->op==BL_IR_JUMP){edge_flush(e,i->a);if(i->a!=b+1)long_block(e,i->a);else if(e->asm_comments)fputs("    # B++ CFG fallthrough jump omitted\n",o);memset(e->dirty,0,e->fn->register_count);memset(e->dirty_phys,0,e->fn->register_count*sizeof *e->dirty_phys);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}
                    if(i->op==BL_IR_BRANCH&&m->truth_states){
                        edge_flush_pair(e,i->b,i->c);
                        fprintf(o,"    cmpl $0, %zu(%%r12)\n    je L_state_false_%zu_%zu_%zu\n    cmpl $1, %zu(%%r12)\n    je L_state_integer_%zu_%zu_%zu\n    movq %zu(%%r12), %%r10\n    testq %%r10, %%r10\n    je L_state_false_%zu_%zu_%zu\n    cmpl $4, (%%r10)\n    jne L_state_true_%zu_%zu_%zu\n    movsd 8(%%r10), %%xmm0\n    pxor %%xmm1, %%xmm1\n    ucomisd %%xmm1, %%xmm0\n    ja L_state_true_%zu_%zu_%zu\n    jmp L_state_false_%zu_%zu_%zu\nL_state_integer_%zu_%zu_%zu:\n    cmpq $0, %zu(%%r12)\n    jle L_state_false_%zu_%zu_%zu\nL_state_true_%zu_%zu_%zu:\n",cell(e,i->a),f,b,k,cell(e,i->a),f,b,k,cell(e,i->a)+8,f,b,k,f,b,k,f,b,k,f,b,k,f,b,k,cell(e,i->a)+8,f,b,k,f,b,k);
                        long_block(e,i->b);fprintf(o,"L_state_false_%zu_%zu_%zu:\n",f,b,k);long_block(e,i->c);memset(e->dirty,0,e->fn->register_count);memset(e->dirty_phys,0,e->fn->register_count*sizeof *e->dirty_phys);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}
                    if(i->op==BL_IR_BRANCH){BPPTruth truth=(BPPTruth)e->fact_regs[i->a].truth;if(truth!=BPP_TRUTH_UNKNOWN){size_t to=truth==BPP_TRUTH_TRUE?i->b:i->c;if(e->asm_comments)fprintf(o,"    # B++ truthiness-elided vreg=v%zu truth=%s\n",i->a,bpp_truth_name(truth));edge_flush(e,to);if(to!=b+1)long_block(e,to);memset(e->dirty,0,e->fn->register_count);memset(e->dirty_phys,0,e->fn->register_count*sizeof *e->dirty_phys);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}if(branch_fallthrough(e,i)){memset(e->dirty,0,e->fn->register_count);memset(e->dirty_phys,0,e->fn->register_count*sizeof *e->dirty_phys);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}fprintf(o,"    cmpl $0, %zu(%%r12)\n",cell(e,i->a));jump(e,"je","false");fprintf(o,"    cmpl $1, %zu(%%r12)\n",cell(e,i->a));jump(e,"jne","true");payload(e,i->a,"%r10");
    fputs("    testq %r10, %r10\n",o);jump(e,"jg","true");def(e,"false");edge_flush(e,i->c);long_block(e,i->c);def(e,"true");edge_flush(e,i->b);long_block(e,i->b);memset(e->dirty,0,e->fn->register_count);memset(e->dirty_phys,0,e->fn->register_count*sizeof *e->dirty_phys);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}
                    /* The emitted slow arm is a possible helper/error edge for
                     * every fast lowering, including checks not classified as
                     * direct clobbers by the IR opcode alone.  Synchronize the
                     * edge-live homes before the fast/slow split so a later
                     * shared continuation can reload lazily on either path. */
                    sync_slow_edge_homes(e);
                    memcpy(e->before,e->dirty,e->fn->register_count);int conditional=fast(e,i);
    if(conditional)def(e,"slow");
                    slow(e,i,conditional);def(e,"done");bpp_value_facts_transfer(&e->facts,i,e->fact_regs,e->fact_locals,e->fact_globals);sync_known(e);if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);
                }
            }
            fprintf(o,"L_fail_%zu:\n    movq $0, %u(%%r12)\nL_cleanup_%zu:\n    movq %u(%%r12), %%rdx\n    movq %u(%%r12), %%rax\n    testq %%rax, %%rax\n    je L_cleanup_null_%zu\n    movl (%%rax), %%r10d\n    movq 8(%%rax), %%r11\n    movl %%r10d, (%%rdx)\n    movq %%r11, 8(%%rdx)\n    movl $0, (%%rax)\n    movq $0, 8(%%rax)\n    jmp L_cleanup_release_%zu\nL_cleanup_null_%zu:\n    movl $0, (%%rdx)\n    movq $0, 8(%%rdx)\nL_cleanup_release_%zu:\n",f,BPP_CG_CALL_OUTPUT_OFFSET,f,BPP_CG_FRAME_OUTPUT_OFFSET,BPP_CG_CALL_OUTPUT_OFFSET,f,f,f,f);cleanup_frame(e);
            if(m->instrument_native)call(e,"bpp_a_trace_exit");
            if(m->benchmark_metrics)call(e,"bpp_bench_exit");
            fprintf(o,"    addq $%zu, %%rsp\n",alloc);for(size_t s=e->saved_count;s-- >0;){const char *name=cache(e,e->saved[s]);fprintf(o,"    popq %s\n",name);
    if(!win)fprintf(o,"    .cfi_restore %s\n",name);}
            if(!win)fputs("    .cfi_def_cfa %rsp, 8\n",o);
    fputs("    ret\n",o);
    if(m->debug_path)fprintf(o,"L_proc_end_%zu:\n",f);
    fputs(win?"    .seh_endproc\n":"    .cfi_endproc\n",o);

    if(e->asm_comments){fprintf(o,"    # B++ allocation-traffic function=%s spills=%zu reloads=%zu rematerializations=%zu\n",e->fn->name,e->emitted_spills,e->emitted_reloads,e->emitted_rematerializations);access_traffic(o,e->fn);}
        }
        if(!win&&m->debug_path)fputs("L_bpp_debug_end:\n",o);
        fputs(win?".section .rdata,\"dr\"\n":".section .rodata\n",o);literal(o,"L_path",m->path);
        if(!m->emit_library){literal(o,"L_native_fmt_int","%lld\n");literal(o,"L_native_fmt_buffer","<buffer size=%zu owned=%d>\n");literal(o,"L_native_fmt_array","<array size=%zu owned=%d>\n");literal(o,"L_native_newline","\n");}
        for(size_t f=0;f<m->function_count;++f)for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){const BLIRInst *i=&m->functions[f].blocks[b].insts[k];
    if(i->text){char label[96];snprintf(label,sizeof label,"L_text_%zu_%zu_%zu",f,b,k);literal(o,label,i->text);}}
        if(m->emit_library)for(size_t f=1;f<m->function_count;++f){char label[64];snprintf(label,sizeof label,"L_abi_name_%zu",f);literal(o,label,m->functions[f].name);}
        fputs(".p2align 3\n",o);for(size_t f=0;f<m->function_count;++f){e=&all[f];size_t flags=layout_flags(&e->plan,e->fn);fprintf(o,"L_layout_%zu:\n    .quad %zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",f,e->fn->param_count,e->fn->register_count,e->fn->local_count,e->maxargs,e->plan.cell_count,e->plan.local_slot_count,e->plan.root_cell_count,flags);if(flags&BPP_LAYOUT_REGISTER_MAP)for(size_t r=0;r<e->fn->register_count;++r)fprintf(o,"    .quad %zu\n",e->plan.cell[r]);if(flags&BPP_LAYOUT_LOCAL_MAP)for(size_t l=0;l<e->fn->local_count;++l)fprintf(o,"    .quad %zu\n",e->plan.local_map[l]);}
        if(m->emit_library){
            size_t count=m->function_count?m->function_count-1:0;const char *name=m->library_name;
            fprintf(o,".globl bpp_v2_library_%s\nbpp_v2_library_%s:\n    .long 0x42505031,2,0,7\n    .long 16,0,8,0\n    .quad %zu\n    .quad bpp_v2_procedures_%s\nbpp_v2_procedures_%s:\n",name,name,count,name,name);
            for(size_t f=1;f<m->function_count;++f)fprintf(o,"    .quad L_abi_name_%zu\n    .long %zu,1\n    .quad bpp_v2_%s\n",f,m->functions[f].param_count,m->functions[f].name);
        }
        if(win)bpp_asm_codeview_symbols(o,m,0);
        else if(m->debug_path)bpp_asm_compile_unit(o,m,target,"L_bpp_debug_start","L_bpp_debug_end","L_proc_","L_proc_end_","");
        if(!win)fputs(".section .note.GNU-stack,\"\",@progbits\n",o);
    }
            for(size_t f=0;f<m->function_count;++f){bpp_reg_plan_destroy(&all[f].plan);bpp_value_facts_destroy(&all[f].facts);free(all[f].dirty);free(all[f].before);free(all[f].kind);free(all[f].constant);free(all[f].loaded_phys);free(all[f].value);free(all[f].dirty_phys);free(all[f].fact_regs);free(all[f].fact_locals);free(all[f].fact_globals);}free(all);return ok&&!ferror(o);
}
